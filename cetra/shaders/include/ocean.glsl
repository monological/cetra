// The water surface, evaluated in ONE place.
//
// Both the position the vertex stage rasterizes and the normal the fragment
// stage shades with come from here, and that is the point: a normal derived from
// a different height than the raster used prints as shading that does not match
// the silhouette, and a shadow pass evaluating its own copy casts a shadow from a
// surface that is not where the surface is. wind.glsl makes the same argument for
// the same reason.
//
// Derivatives are ANALYTIC, never finite differences. The models this seam is
// built for both hand them over for free: a Gerstner term differentiates in
// closed form, and an FFT cascade transforms its derivative fields alongside its
// height in the same pass. A finite difference would need a second full
// evaluation per axis and would still be wrong at the crest, which is the one
// place the normal matters most.

uniform float waterLevel;
uniform vec2 waterWindDir;     // unit; the longest wave's travel direction
uniform float waterAmplitude;  // half crest-to-trough of the longest wave
uniform float waterWavelength; // longest wave, world units
uniform float waterSteepness;  // 0 = round sine crests, 1 = the sharpest legal
uniform float waterSpread;     // per-octave direction fan, radians

// Four octaves. Each is a quarter the length and a bit under half the height of
// the one before, which is the classic wave-train spacing: enough separation that
// the octaves do not beat against each other into a visible repeat.
const int OCEAN_WAVES = 4;
const float OCEAN_LENGTH_FALLOFF = 0.42;
const float OCEAN_AMPLITUDE_FALLOFF = 0.44;
const float OCEAN_GRAVITY = 9.81;

// Spectral cascades (waveModel 1). Sampled in the VERTEX stage as well as the
// fragment stage, which GL 3.3 allows and which is the whole point -- the long and
// medium bands displace real geometry.
uniform int waveModel; // 0 = Gerstner octaves, 1 = spectral cascades
uniform sampler2D cascade0_0;
uniform sampler2D cascade0_1;
uniform sampler2D cascade1_0;
uniform sampler2D cascade1_1;
uniform sampler2D cascade2_0;
uniform sampler2D cascade2_1;
uniform float cascadeLength[3];
uniform float cascadeChoppiness[3];
// Last frame's target 0 for the two cascades that displace, and whether they hold a
// frame yet. 0 on the first two frames, and then the previous position falls back to
// the current one -- which reports camera motion only, the behaviour the whole
// spectral path had before these existed.
uniform sampler2D cascadePrev0;
uniform sampler2D cascadePrev1;
uniform int prevAvailable;

// The baked bed, for shoaling: height in R, its world-space gradient in G,B. Absent
// (bedAvailable 0) is the normal case: the per-fragment water column comes from the
// resolved scene depth instead, which is exact and works against arbitrary geometry.
// This answers the one question screen depth cannot -- a vertex needs its own depth to
// know how far to move, and sampling screen depth would need the position that depth
// is meant to produce.
uniform sampler2D bedTex;
uniform int bedAvailable;
uniform float waterExtent;

// Depth over which a wave goes from fully shoaled to fully open-water. Waves
// shorten and steepen as the bed rises under them; below the floor there is not
// enough water column left to carry any displacement at all.
const float OCEAN_SHOAL_MIN = 0.14;
const float OCEAN_SHOAL_FULL = 2.7;

/*
 * 1 in open water, falling to 0 as the bed comes up, with its own gradient.
 *
 * Multiplies displacement, not height alone: the horizontal term has to shrink with it
 * or the surface slides sideways over a beach it is no longer above. And because it
 * multiplies the displacement, its GRADIENT is a product-rule term in every surface
 * derivative -- the factor alone describes a surface whose normal is the open-water
 * one wherever the bed is steep, which is the surf zone specifically. Zero over a flat
 * bed and zero with no bed at all, so the term costs nothing where it says nothing.
 *
 * .x is the factor; .yz is d(factor)/d(world x, world z).
 */
vec3 oceanShoal(vec2 p) {
    if (bedAvailable == 0)
        return vec3(1.0, 0.0, 0.0);
    vec2 uv = p / (waterExtent * 2.0) + 0.5;
    vec3 bed = texture(bedTex, clamp(uv, vec2(0.0), vec2(1.0))).rgb;
    float column = waterLevel - bed.r;
    float span = OCEAN_SHOAL_FULL - OCEAN_SHOAL_MIN;
    float u = clamp((column - OCEAN_SHOAL_MIN) / span, 0.0, 1.0);
    // d/dp smoothstep(u(p)) = 6u(1-u) * du/dp, and du/dp = -d(bed)/dp / span. Flat
    // outside the window, where smoothstep has clamped and the bed can move without
    // the factor moving.
    float dFactor = 6.0 * u * (1.0 - u) / span;
    return vec3(u * u * (3.0 - 2.0 * u), -dFactor * bed.g, -dFactor * bed.b);
}

struct OceanSurface {
    vec3 world;  // displaced world position
    vec3 normal; // unit normal from the analytic derivatives
    float shoal; // 1 = open water, 0 = the bed has reached the surface
    // Horizontal-map Jacobian; below 1 the map is compressing. 1 means "not
    // computed", which is the Gerstner path.
    float jacobian;
};

/*
 * Spectral surface: the long and medium bands displace the mesh, the short band
 * is left for the fragment stage's slope distribution.
 *
 * Every derivative comes out of the same transform as the height -- field0.a is a
 * cross derivative, field1.rg the two slopes, field1.ba the two horizontal
 * derivatives -- so the normal below is analytic and costs nothing extra. That
 * packing is the reason to spend two RGBA targets per cascade.
 */
// A linear random sea is vertically symmetric, and a real one is not: crests are
// sharper than troughs are deep. These are the low-order bound-harmonic correction
// for that, kept small deliberately -- pushed harder the surface folds, which is what
// the choppy-wave literature bounds. The subtracted constants are each band's mean
// square, so the correction reshapes the surface without raising its mean level.
const float OCEAN_BOUND_LONG = 0.14;
const float OCEAN_BOUND_MED = 0.32;
const float OCEAN_BOUND_LONG_VAR = 0.080;
const float OCEAN_BOUND_MED_VAR = 0.030;

/*
 * The displaced position, from the two displacing bands' target 0 alone.
 *
 * Split out because the PREVIOUS frame's position is computed from retained copies of
 * exactly these two samples, and it has to be the same arithmetic. Two copies of the
 * bound-harmonic term would be two places for it to drift, and a velocity computed
 * from a different surface than the raster drew is the failure this file exists to
 * prevent -- pointing at the previous frame rather than at the normal.
 */
vec3 oceanSpectralPosition(vec2 p, vec4 long0, vec4 med0, float shoal) {
    vec2 horizontal = long0.rg * cascadeChoppiness[0] + med0.rg * cascadeChoppiness[1];
    float height = long0.b + med0.b;
    height += OCEAN_BOUND_LONG * (long0.b * long0.b - OCEAN_BOUND_LONG_VAR) +
              OCEAN_BOUND_MED * (med0.b * med0.b - OCEAN_BOUND_MED_VAR);
    return vec3(p.x + horizontal.x * shoal, waterLevel + height * shoal,
                p.y + horizontal.y * shoal);
}

OceanSurface oceanEvaluateSpectral(vec2 p) {
    vec4 long0 = texture(cascade0_0, fract(p / cascadeLength[0] + 0.5));
    vec4 long1 = texture(cascade0_1, fract(p / cascadeLength[0] + 0.5));
    vec4 med0 = texture(cascade1_0, fract(p / cascadeLength[1] + 0.5));
    vec4 med1 = texture(cascade1_1, fract(p / cascadeLength[1] + 0.5));

    float q0 = cascadeChoppiness[0];
    float q1 = cascadeChoppiness[1];

    // Not named `cross`: that is a builtin this function calls below.
    float crossDeriv = long0.a * q0 + med0.a * q1;
    // The crest-sharpening term changes the height, so its derivative has to be in
    // the slope or the normal is the normal of a DIFFERENT surface than the one
    // rasterized. d/dp of b*(h*h - c) is 2*b*h*dh, per band.
    vec2 slope = long1.rg * (1.0 + 2.0 * OCEAN_BOUND_LONG * long0.b) +
                 med1.rg * (1.0 + 2.0 * OCEAN_BOUND_MED * med0.b);
    vec2 dHoriz = long1.ba * q0 + med1.ba * q1;
    // The unshoaled displacement, which the product-rule terms below multiply. Same
    // expressions oceanSpectralPosition uses; the height carries its crest term
    // because that is part of what the shoal factor scales.
    vec2 horizontal = long0.rg * q0 + med0.rg * q1;
    float height = long0.b + med0.b +
                   OCEAN_BOUND_LONG * (long0.b * long0.b - OCEAN_BOUND_LONG_VAR) +
                   OCEAN_BOUND_MED * (med0.b * med0.b - OCEAN_BOUND_MED_VAR);

    vec3 sh = oceanShoal(p);
    float shoal = sh.x;

    OceanSurface s;
    // No amplitude knob here, deliberately. The spectrum is already physical --
    // its height comes out of the wind speed and fetch it was seeded with -- so
    // scaling it would be authoring over a sea state rather than choosing one.
    // waterAmplitude belongs to the Gerstner path, where there is no sea state to
    // ask. A calmer spectral ocean is a lower wind speed, not a smaller number
    // here.
    s.world = oceanSpectralPosition(p, long0, med0, shoal);
    // Scaled by the same factor, so the normal tracks the displacement rather than
    // the open-water surface it would otherwise describe -- and the factor's own
    // gradient enters as a product-rule term, since the displacement it scales varies
    // with position too. Over a flat bed sh.yz is zero and these reduce to the plain
    // scaled derivatives.
    vec3 dPdx = vec3(1.0 + dHoriz.x * shoal + horizontal.x * sh.y,
                     slope.x * shoal + height * sh.y,
                     crossDeriv * shoal + horizontal.y * sh.y);
    vec3 dPdz = vec3(crossDeriv * shoal + horizontal.x * sh.z,
                     slope.y * shoal + height * sh.z,
                     1.0 + dHoriz.y * shoal + horizontal.y * sh.z);
    s.normal = normalize(cross(dPdz, dPdx));
    // The horizontal map's determinant, from those same rows. The two off-diagonals
    // are no longer equal once the shoal gradient is in, so this cannot be shortened
    // back to a square -- and the difference between them IS the shoaling compression
    // that selects surf-zone foam.
    s.jacobian = dPdx.x * dPdz.z - dPdz.x * dPdx.z;
    s.shoal = shoal;
    return s;
}

/*
 * Sum of Gerstner waves.
 *
 * A Gerstner wave moves its surface points horizontally as well as vertically,
 * bunching them toward the crest, which is what sharpens a crest and broadens a
 * trough instead of leaving a symmetric sine. The horizontal term is what makes
 * it a Gerstner wave rather than a height field, and it is also what can fold the
 * surface over itself: the mapping stops being injective once Q*A*k exceeds 1.
 *
 * So steepness is NOT passed through to Q. It is divided by the octave's own A*k
 * and by the octave count, so the set sums to the authored value: at or below 1
 * the map stays injective at any amplitude or wavelength. Above 1 it folds, which
 * is why the C side clamps the uniform rather than trusting the caller. Authoring
 * Q directly would instead make "sharp" mean something different at every
 * wavelength.
 *
 * `t` is used here and ignored by the spectral path, which reads cascades that
 * already hold one instant -- see the previous-position note in water_vert.
 */
OceanSurface oceanEvaluate(vec2 p, float t) {
    if (waveModel == 1)
        return oceanEvaluateSpectral(p);

    vec3 sh = oceanShoal(p);
    float shoal = sh.x;
    // Accumulated UNSHOALED, then scaled once at the end. The shoal factor multiplies
    // the whole displacement, so its gradient multiplies the whole displacement too --
    // which is one line out here and would be six inside the loop.
    vec3 disp = vec3(0.0);
    // Partial derivatives of the displacement with respect to the undisplaced grid
    // coordinates. The flat plane's own identity rows are added at the end, since they
    // are not part of what shoals.
    vec3 dDispDx = vec3(0.0);
    vec3 dDispDz = vec3(0.0);

    float wavelength = waterWavelength;
    // Note what the shoal factor must NOT be folded into: q is amplitude's reciprocal,
    // so scaling amplitude here leaves qa = steepness/(k*N) unchanged and the lateral
    // shuffle would run at full strength over a beach whose vertical motion had
    // already faded to nothing.
    float amplitude = waterAmplitude;
    vec2 base = normalize(waterWindDir + vec2(1e-6, 0.0));

    for (int i = 0; i < OCEAN_WAVES; i++) {
        // Fan the octaves off the wind, alternating sides so the set stays
        // centred on it. A single direction reads as corduroy however many
        // octaves are stacked on it.
        float fan = waterSpread * float(i) * ((i % 2 == 0) ? 1.0 : -1.0);
        vec2 dir = vec2(base.x * cos(fan) - base.y * sin(fan),
                        base.x * sin(fan) + base.y * cos(fan));

        float k = 6.28318530718 / max(wavelength, 0.01);
        // Deep-water dispersion: long waves travel faster, which is what keeps
        // the octaves sliding past each other instead of marching in lockstep.
        float omega = sqrt(OCEAN_GRAVITY * k);
        float phase = k * dot(dir, p) - omega * t;
        float sinp = sin(phase);
        float cosp = cos(phase);

        // Q normalised by this octave's own A*k and by the octave count, so the
        // steepness uniform sums to itself across the set: at or below 1 the map
        // stays injective. See the header note.
        float q = waterSteepness / max(k * amplitude * float(OCEAN_WAVES), 1e-4);
        float qa = q * amplitude;

        disp.x += qa * dir.x * cosp;
        disp.y += amplitude * sinp;
        disp.z += qa * dir.y * cosp;

        float dqa = qa * k * sinp;
        float dah = amplitude * k * cosp;
        dDispDx.x -= dqa * dir.x * dir.x;
        dDispDx.y += dah * dir.x;
        dDispDx.z -= dqa * dir.y * dir.x;
        dDispDz.x -= dqa * dir.x * dir.y;
        dDispDz.y += dah * dir.y;
        dDispDz.z -= dqa * dir.y * dir.y;

        wavelength *= OCEAN_LENGTH_FALLOFF;
        amplitude *= OCEAN_AMPLITUDE_FALLOFF;
    }

    OceanSurface s;
    s.world = vec3(p.x, waterLevel, p.y) + disp * shoal;
    // The flat plane's identity rows, plus the scaled wave derivatives, plus the shoal
    // factor's own gradient acting on the displacement it scales. sh.yz is zero over a
    // flat bed and with no bed at all, so this is the previous expression there.
    vec3 dPdx = vec3(1.0, 0.0, 0.0) + dDispDx * shoal + disp * sh.y;
    vec3 dPdz = vec3(0.0, 0.0, 1.0) + dDispDz * shoal + disp * sh.z;
    // cross(dPdz, dPdx) and not the reverse: on a flat surface that is +Y, and
    // the flipped order would light every wave from underneath.
    s.normal = normalize(cross(dPdz, dPdx));
    // A Gerstner map DOES compress -- that bunching is what sharpens its crests --
    // but its Jacobian is not reported here. 1 means "no selector on this path", which
    // is what the foam and caustics gates read it as; deriving it would turn the
    // clamped-steepness argument for why they are FFT-only into a lie.
    s.jacobian = 1.0;
    s.shoal = shoal;
    return s;
}

/*
 * Where this point was one frame ago, which is a motion vector's other half.
 *
 * Gerstner re-evaluates at t - dt and gets it in closed form. The spectral path
 * cannot: its cascades hold ONE instant, the current one, so reading them at t - dt
 * returns the same surface and the waves report no motion of their own. That is what
 * the retained copies answer -- the same position arithmetic against last frame's
 * fields.
 *
 * Only the two displacing bands are retained, which is all this needs: the short band
 * never reaches the mesh, so it has no position to have moved.
 */
vec3 oceanPreviousWorld(vec2 p, float tPrev) {
    if (waveModel == 0)
        return oceanEvaluate(p, tPrev).world;
    // Two branches rather than a ternary on the samplers themselves: GLSL 3.30
    // samplers are opaque and may not appear in an expression.
    vec2 uvLong = fract(p / cascadeLength[0] + 0.5);
    vec2 uvMed = fract(p / cascadeLength[1] + 0.5);
    vec4 long0 = texture(cascade0_0, uvLong);
    vec4 med0 = texture(cascade1_0, uvMed);
    if (prevAvailable == 1) {
        long0 = texture(cascadePrev0, uvLong);
        med0 = texture(cascadePrev1, uvMed);
    }
    return oceanSpectralPosition(p, long0, med0, oceanShoal(p).x);
}
