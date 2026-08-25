// The lunar disc (spec 11.82). SHAPE only: which side is lit, where the
// terminator falls, the maria, the earthshine. The caller supplies the
// BRIGHTNESS, which already carries the Krisciunas-Schaefer phase law -- so
// this file and the C side cannot disagree about the phase, because they
// derive it from the same two directions rather than passing a number.
//
// Included for starNoise3 alone: the maria want value noise on an integer
// hash, the star field already carries exactly that, and the include system
// is include-once so the second consumer costs nothing. If a third consumer
// ever wants the noise without the stars, that is when it gets hoisted.
#include "stars.glsl"

// A full moon is NOT a Lambertian sphere, and that is its whole visual
// signature. Lunar regolith backscatters (Lommel-Seeliger plus a strong
// opposition surge), so a real full moon is a flat bright disc with almost no
// centre-to-limb gradient. Shading the lit face by cos(theta) renders a
// snooker ball, which is the classic tell. 0.85 against the sun's own 0.4 --
// the same functional form, the opposite intent.
const float MOON_LIMB = 0.85;
// Local, because this file is included by programs that define neither.
const float MOON_TAU = 6.28318530718;
const float MOON_PI = 3.14159265359;
/*
 * The maria: basalt seas, roughly half the highlands' albedo.
 *
 * THREE octaves, not two, and a narrow band rather than a wide one. Two
 * low-frequency octaves through a wide smoothstep gave big soft blobs that read
 * as coffee stains -- the real seas have distinct shapes with reasonably
 * defined shores, and sit under a layer of finer structure (craters, rays) that
 * is what stops a disc looking painted. The third octave is that layer; it
 * carries a fraction of the weight, so it textures without breaking the seas up.
 */
const float MOON_MARIA_ALBEDO = 0.44;
const float MOON_MARIA_LOW = 0.46;
const float MOON_MARIA_HIGH = 0.54;
const float MOON_MARIA_COARSE = 2.1;
const float MOON_MARIA_MID = 4.7;
const float MOON_MARIA_FINE = 13.0;
// How much the finest octave mottles the whole face, seas and highlands alike.
const float MOON_MARIA_GRAIN = 0.13;
// How far the seas' own coordinate is pushed around before it is thresholded.
// This is what turns round blobs into lobed shapes with inlets.
const float MOON_MARIA_WARP = 0.55;
// Crater lattice frequencies, three octaves. Not a harmonic series on purpose:
// integer ratios print the lattice as a visible grid of same-size craters.
const float MOON_CRATER_BIG = 5.3;
const float MOON_CRATER_MID = 13.7;
const float MOON_CRATER_SMALL = 34.1;
// Relief, in units of the moon's own radius. Real lunar relief is tiny against
// the globe -- Tycho's rim stands about 0.3% of a lunar radius -- so GAIN is how
// far the normal is TILTED rather than a displacement: the usual bump-mapping
// bargain, where the silhouette stays a circle (it should) while the shading
// behaves as though it did not.
const float MOON_RELIEF_GAIN = 1.0;
const float MOON_RELIEF_EPS = 0.0035;
// Where the fine octaves fade, in face units per pixel. Past FAR the surface is
// smooth, which is correct rather than a compromise: a moon a few pixels across
// has no resolvable craters, and a gradient sampled finer than the pixel is the
// definition of aliasing.
const float MOON_LOD_NEAR = 0.004;
const float MOON_LOD_FAR = 0.03;
// The seas sit LOWER than the highlands, which is what makes their shores read
// as coastlines rather than as paint. Plus wrinkle ridges in them, and the
// broken relief of the highlands.
const float MOON_MARIA_DEPTH = 0.006;
const float MOON_RIDGE_MARIA = 0.0018;
const float MOON_RIDGE_LAND = 0.0035;
const float MOON_RIDGE_SCALE = 9.0;
// Ray systems: how far they reach across the face, and how bright.
const float MOON_RAY_REACH = 0.85;
const float MOON_RAY_GAIN = 0.16;
// Earthshine: the dark limb is Earth-lit, not black -- the old moon in the new
// moon's arms. Stated RELATIVE to the lit side, so it rides the phase law the
// caller applies and can never outshine the crescent beside it. Blue because
// it is sunlight bounced off an ocean planet, which is the one genuinely blue
// thing about moonlight; the blue people remember at night is the Purkinje
// shift, and that is a different feature.
const float MOON_EARTHSHINE = 0.020;
const vec3 MOON_EARTHSHINE_TINT = vec3(0.62, 0.76, 1.00);
// Near-neutral, faintly warm.
const vec3 MOON_TINT = vec3(1.00, 0.98, 0.95);
// The aureole's peak radiance, as a fraction of the disc's. Its WIDTH is a
// uniform (moonGlowAng) rather than a constant here: it has to relate to the
// drawn size, and how is a look decision that belongs with the others in sky.c.
const float MOON_GLOW = 0.020;

/*
 * TOPOGRAPHY. This is a HEIGHT field, not an albedo one, and the distinction is
 * the whole reason the surface reads as a surface.
 *
 * A painted crater is a ring of light and dark that looks identical from every
 * sun angle. A crater with a SHAPE has a rim that catches grazing light on one
 * side and drops its own bowl into shadow on the other, and the two swap as the
 * phase moves. That is what the eye reads as relief, and no albedo map produces
 * it, because it is not a colour.
 *
 * The profile is the real one: a depressed floor, a rim crest standing ABOVE the
 * surrounding plain, and an ejecta apron falling back outside it. Polynomials
 * rather than exp/pow -- this runs 27 cells per octave per height sample, and
 * the normal costs three height samples.
 */
float moonCraterHeight(vec3 face, float scale, float amp, float fade) {
    if (fade <= 0.0)
        return 0.0;
    vec3 sp = face * scale;
    vec3 base = floor(sp);
    float h = 0.0;
    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) {
            for (int k = -1; k <= 1; k++) {
                vec3 cell = base + vec3(float(i), float(j), float(k));
                uvec3 c = uvec3(ivec3(cell));
                uint h0 = starHash3(c);
                uint h1 = starPcg(h0);
                uint h2 = starPcg(h1);
                uint h3 = starPcg(h2);
                vec3 jitter = vec3(starUnit(h0), starUnit(h1), starUnit(h2));
                // Heavy-tailed: a crater field is overwhelmingly small ones with
                // a few large. A linear radius reads as a golf ball.
                float rr = starUnit(h3);
                float rr2 = rr * rr;
                float radius = 0.10 + 0.55 * rr2 * rr2 * rr;
                float d = length(sp - (cell + jitter)) / radius;
                if (d >= 1.55)
                    continue;
                float bowl = -max(0.0, 1.0 - d * d / 0.62);
                float rt = 1.0 - min(1.0, abs(d - 1.02) / 0.34);
                float rim = rt * rt * (3.0 - 2.0 * rt);
                float at = 1.0 - min(1.0, abs(d - 1.32) / 0.26);
                float apron = at * at * (3.0 - 2.0 * at) * 0.22;
                // radius/scale, NOT radius: the lattice works in face*scale
                // space, so an unconverted height carries the octave's own
                // frequency into the slope. Left uncorrected the fine octaves
                // were an order of magnitude out and the normal was noise.
                // Depth tracks radius, as real craters do -- a basin is deep, a
                // pit is shallow.
                h += amp * (radius / scale) * (bowl * 0.75 + rim * 0.85 + apron) * fade;
            }
        }
    }
    return h;
}

// Ridged noise: wrinkle ridges in the seas, broken relief in the highlands. The
// fold at the peak is what makes a CREST rather than a smooth swell.
float moonRidges(vec3 face, float scale) {
    float n = starNoise3(face * scale);
    float r = 1.0 - abs(2.0 * n - 1.0);
    return r * r;
}

/*
 * Elevation at a point on the face, in units of the moon's own radius.
 *
 * `cover` damps everything inside the maria: they are basalt floods, so they
 * really are smooth and low beside the highlands, and whatever cratering was
 * there before the flood is under it. `lod` fades the finest octaves as a pixel
 * grows past them -- without it the normal aliases into a boiling mess at any
 * small disc, which is most framings.
 */
float moonHeight(vec3 face, float cover, float lod) {
    float land = 1.0 - cover;
    float h = moonCraterHeight(face, MOON_CRATER_BIG, 0.14, mix(0.25, 1.0, land));
    h += moonCraterHeight(face, MOON_CRATER_MID, 0.11, mix(0.10, 1.0, land) * lod);
    h += moonCraterHeight(face, MOON_CRATER_SMALL, 0.08,
                          mix(0.05, 1.0, land) * lod * lod);
    h -= cover * MOON_MARIA_DEPTH;
    h += cover * MOON_RIDGE_MARIA * moonRidges(face, MOON_RIDGE_SCALE);
    h += land * MOON_RIDGE_LAND * lod * (moonRidges(face, MOON_RIDGE_SCALE * 2.3) - 0.35);
    return h;
}

/*
 * RAY SYSTEMS: the bright streaks thrown out by the few youngest craters, and
 * the most recognisable thing on a full moon after the seas themselves.
 *
 * Not derived from the crater field, because only a handful of craters have
 * them and picking those out would cost a second pass over it. A few fixed
 * centres with an angular streak function is what the eye reads, and it is the
 * one place here that is frankly a painting rather than a simulation.
 */
float moonRays(vec3 face, vec3 centre, float reach) {
    vec3 d = face - centre;
    float r = length(d);
    if (r > reach)
        return 0.0;
    // Streaks in ANGLE about the centre, high-frequency and uneven, fading
    // with distance. The noise is sampled on a ring so the streaks stay
    // radial rather than turning into blobs.
    vec3 dir = d / max(r, 1e-5);
    float streak = starNoise3(dir * 9.0 + centre * 4.0);
    streak = smoothstep(0.55, 0.95, streak);
    return streak * (1.0 - r / reach) * (1.0 - smoothstep(0.0, 0.18, r));
}

// edge      the sun disc's own [0,1] radial parameter, 1 at the centre
// dir       the view ray
// moonDir   toward the moon
// sunDir    toward the sun
// cosRadius cosine of the disc's angular RADIUS
// pixel     one pixel's angular footprint, taken by the caller under uniform
//           control flow (the stars' rule: a derivative inside non-uniform
//           flow is undefined in GLSL 330)
vec3 moonDisc(float edge, vec3 dir, vec3 moonDir, vec3 sunDir, float cosRadius, float pixel,
              float earthshine, float maria, sampler2D moonMapTex) {
    // The disc coordinate, from the sun block's own `edge` rather than a
    // second trig call: s is the normalised radius and nz the eye-facing
    // component, and since s^2 = 1 - edge exactly, nz is just sqrt(edge).
    //
    // The approximation this rests on -- that 1 - edge stands in for
    // sin^2(theta)/sin^2(R) -- is a few parts per million at the shipping
    // half-degree disc, but the gate arms drive it to 40 degrees where it is
    // off by ~1.5% near centre. Harmless for the look at any real size, and
    // named here because that residual shows up in moon-lit's ladder rather
    // than being a defect in the phase geometry.
    float s = sqrt(max(1.0 - edge, 0.0));
    float nz = sqrt(max(edge, 0.0));

    // Disc-local axes. "Up" is world up projected into the disc plane, so the
    // face keeps a fixed orientation as the moon crosses the sky -- what a
    // tidally locked body does, and one cross product cheaper than a libration
    // frame nobody could see.
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 du = cross(moonDir, up);
    du = (length(du) < 1e-4) ? vec3(1.0, 0.0, 0.0) : normalize(du);
    vec3 dv = cross(du, moonDir);

    vec2 q = vec2(dot(dir, du), dot(dir, dv));
    vec2 p = q * (s / max(length(q), 1e-8)); // inside the unit disc

    // The face vector: the surface point in the disc's own frame, +z toward the
    // viewer. Everything below is a function of it, so the whole surface is
    // locked to the face and cannot swim as the moon crosses the sky.
    vec3 face = vec3(p, nz);

    /*
     * THE SEAS COME FROM DATA, and everything else here stays procedural.
     *
     * Where the maria are is a historical accident -- Imbrium is a basin that
     * happened to be flooded -- so no noise reproduces it, and three octaves of
     * warped value noise gave a plausible cratered world that was not the Moon.
     * The map is 256x128 of coverage compiled into the binary (moon_map.h), the
     * ltc_lut.h shape, built from the published centres and extents.
     *
     * Equirectangular lookup on the face vector: the disc centre is the
     * sub-observer point, so latitude is asin(y) and longitude atan2(x, z)
     * with the near side at zero -- which also makes the foreshortening toward
     * the limb fall out, since equal steps in the face vector are unequal steps
     * in longitude there.
     */
    vec2 muv = vec2(atan(face.x, face.z) / MOON_TAU + 0.5, 0.5 - asin(clamp(face.y, -1.0, 1.0)) / MOON_PI);
    float cover = texture(moonMapTex, muv).r;
    float seas = mix(1.0, MOON_MARIA_ALBEDO, cover);

    /*
     * THE RELIEF NORMAL, which is what turns a painted disc into a surface.
     *
     * Finite-difference the height field along two tangents and tilt the sphere
     * normal by the gradient. Three height samples, each walking the crater
     * lattice, so this is the expensive part of the moon by a wide margin -- and
     * it runs for the few thousand pixels inside the disc and nowhere else.
     */
    float discAng = sqrt(2.0 * (1.0 - cosRadius));
    float px = pixel / max(discAng, 1e-6); // one pixel, in face units
    float lod = 1.0 - smoothstep(MOON_LOD_NEAR, MOON_LOD_FAR, px);
    // A pixel or the finest feature, whichever is larger: sampling the gradient
    // finer than the pixel is the definition of aliasing.
    float eps = max(px, MOON_RELIEF_EPS);

    vec3 axis = abs(face.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tanU = normalize(cross(axis, face));
    vec3 tanV = cross(face, tanU);
    float h0 = moonHeight(face, cover, lod);
    float hU = moonHeight(normalize(face + tanU * eps), cover, lod);
    float hV = moonHeight(normalize(face + tanV * eps), cover, lod);
    vec3 relief = normalize(face - (tanU * (hU - h0) + tanV * (hV - h0)) *
                                       (MOON_RELIEF_GAIN / eps));

    // Back to world: the face frame's axes are (du, dv, -moonDir).
    vec3 n = du * relief.x + dv * relief.y - moonDir * relief.z;

    /*
     * LOMMEL-SEELIGER, not Lambert, and it is the honest version of the hack it
     * replaces. The regolith backscatters, so brightness goes as mu0/(mu0+mu):
     * at full phase mu0 and mu are equal everywhere and the disc reads a flat
     * 0.5, which is exactly why a real full moon has no centre-to-limb shading
     * and why cosine lighting renders a snooker ball. Near the terminator the
     * same expression falls off slowly and lets crater walls throw their own
     * shade, which is where all the relief lives.
     *
     * The previous version faked the flat full moon with a hard smoothstep on
     * dot(n, sunDir) and had no relief at all. This gives both from one term.
     *
     * Normalised so a head-on full moon reads 1, since the bare ratio is 0.5
     * there.
     */
    float mu0 = dot(n, sunDir);
    float mu = dot(n, -moonDir);
    float lit = max(mu0, 0.0) / max(mu0 + mu, 1e-3) * 2.0;
    // One pixel of softening on the zero crossing, so the terminator is not a
    // stair at large disc sizes.
    lit *= smoothstep(-px, px, mu0);
    lit = clamp(lit, 0.0, 1.4);

    // The albedo is the map, a fine mottle, and the ray systems. The craters
    // are NOT here any more -- they are height, and the light does their
    // shading, which is the whole change.
    float grain = starNoise3(face * MOON_MARIA_FINE);
    float rays = moonRays(face, normalize(vec3(-0.15, -0.62, 0.77)), MOON_RAY_REACH) +
                 0.6 * moonRays(face, normalize(vec3(0.55, 0.35, 0.76)), MOON_RAY_REACH * 0.7);
    float surface = seas * (1.0 + MOON_MARIA_GRAIN * (grain - 0.5)) +
                    rays * MOON_RAY_GAIN * (1.0 - cover);
    float albedo = mix(1.0, clamp(surface, 0.25, 1.35), maria);

    // Mild rim darkening, on the eye-facing component already in hand.
    float limb = MOON_LIMB + (1.0 - MOON_LIMB) * nz;

    /*
     * THE LIMB IS ANTIALIASED, and it needs to be for the terminator's reason
     * one screen up -- the caller's `cosVM > moonCosRadius` is a hard binary
     * test, so without this the disc's outer boundary is a stamped-out circle
     * with jagged pixels on it at any size worth looking at.
     *
     * Same one-pixel width and the same mechanism, expressed on the disc
     * coordinate: s reaches 1 at the rim, so the ramp is the last pixel of
     * radius. A real limb is slightly soft anyway -- atmospheric seeing and the
     * lens both blur it -- so this is antialiasing that happens to be physical
     * rather than a cheat that happens to look right.
     */
    float rimSoft = clamp(pixel / max(sqrt(2.0 * (1.0 - cosRadius)), 1e-6), 0.004, 0.25);
    float rim = 1.0 - smoothstep(1.0 - rimSoft, 1.0, s);

    vec3 tint = mix(MOON_EARTHSHINE_TINT, MOON_TINT, lit);
    return tint * (rim * albedo * limb * mix(MOON_EARTHSHINE * earthshine, 1.0, lit));
}
