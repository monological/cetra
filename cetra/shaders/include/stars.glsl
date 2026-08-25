// Procedural star field for the sky background (spec 11.79). Two octahedral
// lattices over the direction sphere: a 128x128 BRIGHT field (~9k stars at
// the shipping occupancy, the naked-eye count to magnitude 6.5) and a
// 256x256 faint WASH -- the thousands of barely-resolved stars that make a
// dark sky read as deep rather than as a scatter of dots. Evaluated only by
// the two background variants; the env/IBL path must never carry this, for
// the same firefly reason sky_env_frag refuses the sun disc.
//
// The three properties that separate a night sky from noise, in order:
//   flux    -- star counts go as 10^(0.6m), so flux is Pareto(-2.5): a few
//              dominant stars over a wash of faint ones. Sampled as u^(-2/3).
//   colour  -- a restrained slice of the Planckian locus, white-blue
//              dominated with the warm quarter rare. Oversaturating is the
//              giveaway.
//   the band -- the Milky Way is unresolved stars, so it is a density and
//              glow modulation along a fixed great circle, not more points.
//
// Each star is a core sized in PIXELS off the view-direction derivative --
// a star is a point-spread function, and its saturated core must stay ~1 px
// at any resolution or fov. A fixed ANGULAR width was built first and reads
// as flat white CIRCLES: at HDR peaks the whole region above tonemap
// saturation is a disc, and a fixed angle makes that disc grow with
// resolution. The glare wing scales with the star's own brightness, because
// tree ships bloom at 0.015 and the glow has to come from the PSF itself.
//
// The caller passes the direction already rotated into the CELESTIAL frame
// (starFrame), so the grid pole sits on the celestial pole: under a turning
// sky the field rigidly wheels and the pole cell holds still, which is what
// a real sky does about Polaris.
#include "octahedral.glsl"
#include "noise.glsl"

const float STAR_GRID = 128.0;       // bright-field cells per octahedral axis
const float STAR_OCCUPANCY = 0.42;   // base fraction of cells holding a star
const float STAR_SIGMA_PX = 0.7;     // core width in pixels (see header)
const float STAR_HALO = 0.10;        // glare-wing ceiling, reached by the bright tail
// Peak radiance of a flux-1 star. Sized so a flux-10 star (about seven per
// 40-degree frame) already saturates the tonemap: the hero population must
// be a reliable feature of every framing, not the luck of whether the rare
// deep tail landed in view -- a field whose peaks never saturate reads as
// dirty specks at any density.
const float STAR_BASE = 0.5;
const float STAR_FLUX_CAP = 60.0;    // Pareto tail clamp
const float STAR_GLOW = 0.12;        // Milky Way band peak radiance

// The band's pole in the celestial frame: the galactic pole sits ~63 deg
// from the celestial pole, which is what tips the band across the sky
// instead of tracing the equator.
const vec3 STAR_BAND_POLE = vec3(0.0, 0.4540, 0.8910);
const float STAR_BAND_WIDTH = 0.15;  // across-band sigma of dot(dir, pole)

// Per-purpose hash keys (noise.glsl's parameterised-hash contract: the FORM
// is shared, the pattern is this file's own). Cell ids are an integer
// lattice, where the sin-fract hash is known to collide heavily (roadmap
// E10), so every lookup goes through an irrational pre-scale first; the grid
// size joins the input so the two lattices decorrelate.
const vec2 STAR_LATTICE = vec2(0.618034, 0.754878);
const vec2 STAR_K_OCCUPY = vec2(157.3, 269.5);
const vec2 STAR_K_JX = vec2(419.2, 371.9);
const vec2 STAR_K_JY = vec2(269.5, 183.3);
const vec2 STAR_K_FLUX = vec2(113.5, 271.9);
const vec2 STAR_K_COLOR = vec2(345.7, 143.1);
const vec3 STAR_K_NOISE = vec3(127.1, 311.7, 74.7);

// 3D value noise for the band's lane structure. Over the DIRECTION rather
// than the octahedral square, because interpolation across the oct seam
// would print the fold as a line in the glow; the per-star taps never
// interpolate, so only the smooth term needs this.
float starNoise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i + vec3(0.0, 0.0, 0.0), STAR_K_NOISE);
    float n100 = hash13(i + vec3(1.0, 0.0, 0.0), STAR_K_NOISE);
    float n010 = hash13(i + vec3(0.0, 1.0, 0.0), STAR_K_NOISE);
    float n110 = hash13(i + vec3(1.0, 1.0, 0.0), STAR_K_NOISE);
    float n001 = hash13(i + vec3(0.0, 0.0, 1.0), STAR_K_NOISE);
    float n101 = hash13(i + vec3(1.0, 0.0, 1.0), STAR_K_NOISE);
    float n011 = hash13(i + vec3(0.0, 1.0, 1.0), STAR_K_NOISE);
    float n111 = hash13(i + vec3(1.0, 1.0, 1.0), STAR_K_NOISE);
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

// A two-anchor slice of the Planckian locus through white. Skewed cool on
// purpose: the warm quarter is the rare one, because a night sky people
// recognise is white-blue with occasional amber, not the reverse.
vec3 starColor(float h) {
    vec3 warm = vec3(1.0, 0.74, 0.50);
    vec3 cool = vec3(0.62, 0.76, 1.0);
    vec3 c = (h < 0.25) ? mix(warm, vec3(1.0), h * 4.0)
                        : mix(vec3(1.0), cool, (h - 0.25) / 0.75);
    return mix(vec3(1.0), c, 0.5);
}

// One star lattice: at most one star per cell, gathered over the 3x3
// neighbourhood of the cell containing `dir`. The gather is correctness,
// not thoroughness: the footprint is sized in PIXELS while the cells are
// fixed in angle, so no bake-time jitter margin can keep a star inside its
// own cell at every resolution -- a single-cell tap shipped, and truncated
// edge-adjacent stars along their cell boundaries into hard diagonal
// wedges. Neighbours across the octahedral fold are adjacent on the sphere
// too (the fold is continuous), and the square's outer border maps to the
// nadir, where the caller's transmittance is already zero.
//
// `scale` is the flux-1 peak radiance; the glare wing grows with the star's
// own brightness (bright sources flare, faint ones stay points).
vec3 starLayer(vec3 dir, float grid, float occupancy, float fluxCap, float scale,
               float sigma) {
    vec2 g = (octEncode(dir) * 0.5 + 0.5) * grid;
    vec2 base = floor(g);
    float s2 = sigma * sigma;
    vec3 sum = vec3(0.0);
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            vec2 cell = base + vec2(float(dx), float(dy));
            vec2 p = (cell + 0.5 + grid) * STAR_LATTICE;
            if (hash21(p, STAR_K_OCCUPY) >= occupancy)
                continue;
            vec2 jitter = vec2(hash21(p, STAR_K_JX), hash21(p, STAR_K_JY)) - 0.5;
            vec3 starDir = octDecode((cell + 0.5 + jitter) / grid * 2.0 - 1.0);
            // Chord length ~ angle at these widths.
            float d2 = dot(dir - starDir, dir - starDir);
            float flux = min(pow(hash21(p, STAR_K_FLUX) + 1e-3, -0.6667), fluxCap);
            float wing = STAR_HALO * clamp(flux * scale * 0.5, 0.0, 1.0);
            float fall = exp(-d2 / (2.0 * s2)) + wing * exp(-d2 / (2.0 * 16.0 * s2));
            sum += starColor(hash21(p, STAR_K_COLOR)) * (flux * scale * fall);
        }
    }
    return sum;
}

// Star radiance along a celestial-frame direction, in the sky's absolute
// units. The caller applies atmospheric transmittance and the night ramp.
vec3 starRadiance(vec3 dir) {
    // One pixel's angular footprint, taken here (uniform flow -- the caller
    // gates on a uniform) rather than inside a per-cell branch, where a
    // derivative is undefined in GLSL 330.
    float sigma = max(STAR_SIGMA_PX * length(fwidth(dir)), 1e-5);

    // The Milky Way's across-band coordinate, shared by the density boosts
    // and the glow.
    float s = dot(dir, STAR_BAND_POLE);
    float band = exp(-s * s / (2.0 * STAR_BAND_WIDTH * STAR_BAND_WIDTH));

    // The bright field, then the faint wash on a twice-fine lattice.
    vec3 star = starLayer(dir, STAR_GRID, STAR_OCCUPANCY * (1.0 + 1.1 * band),
                          STAR_FLUX_CAP, STAR_BASE, sigma) +
                starLayer(dir, STAR_GRID * 2.0, 0.5 * (1.0 + 1.0 * band), 6.0,
                          STAR_BASE * 0.35, sigma);

    // Unresolved glow with dark-lane structure, faintly blue like the real
    // integrated light.
    float n = 0.6 * starNoise3(dir * 6.0) + 0.4 * starNoise3(dir * 14.0);
    float lanes = smoothstep(0.15, 0.75, n);
    vec3 glow = vec3(0.85, 0.90, 1.0) * (band * mix(0.10, 1.0, lanes) * STAR_GLOW);

    return star + glow;
}
