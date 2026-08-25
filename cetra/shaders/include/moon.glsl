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
// Crater lattice frequencies. The two are not a harmonic pair on purpose --
// an integer ratio prints the lattice as a visible grid of same-size craters.
const float MOON_CRATER_BIG = 5.3;
const float MOON_CRATER_SMALL = 13.7;
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
 * CRATERS, which are the thing that makes a lunar surface read as a surface.
 *
 * Value noise cannot produce them at any octave count: what the eye is looking
 * for is CIRCLES with raised bright rims and darker floors, and noise has no
 * circular structure in it. Without these the disc reads as stains no matter
 * how the maria are tuned, which is where this file started.
 *
 * One crater per lattice cell, jittered off the cell centre, with a hashed
 * radius. Searched over the 3x3x3 neighbourhood because a jittered centre can
 * reach into its neighbours; on the FACE vector rather than the projected
 * offset, so craters foreshorten toward the limb the way real ones do -- equal
 * surface distances compress in projection for free.
 *
 * Returns a signed brightness delta, so a surface with no crater over it is
 * exactly unchanged.
 */
float moonCraterField(vec3 face, float scale, float rimGain, float floorGain) {
    vec3 sp = face * scale;
    vec3 base = floor(sp);
    float acc = 0.0;
    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) {
            for (int k = -1; k <= 1; k++) {
                vec3 cell = base + vec3(float(i), float(j), float(k));
                uvec3 c = uvec3(ivec3(cell));
                uint h0 = starHash3(c);
                uint h1 = starPcg(h0);
                uint h2 = starPcg(h1);
                uint h3 = starPcg(h2);
                // Jittered centre, and a radius that varies a lot: a real
                // field is mostly small craters with a few large ones, so the
                // radius is cubed to skew it that way.
                vec3 jitter = vec3(starUnit(h0), starUnit(h1), starUnit(h2));
                float rr = starUnit(h3);
                float rr2 = rr * rr;
                float radius = 0.07 + 0.62 * rr2 * rr2 * rr;
                float d = length(sp - (cell + jitter)) / radius;
                if (d < 1.25) {
                    // Bright raised rim just inside the edge, dark floor in the
                    // middle. Both fade out past the rim so the apron is soft.
                    float rim = smoothstep(0.55, 0.92, d) * (1.0 - smoothstep(0.92, 1.25, d));
                    float bowl = 1.0 - smoothstep(0.0, 0.80, d);
                    acc += rimGain * rim - floorGain * bowl;
                }
            }
        }
    }
    return acc;
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
              float earthshine, float maria) {
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

    // The lunar surface normal at this pixel: the eye-facing pole is -moonDir,
    // the lateral part is the disc offset. Orthographic, because the moon is
    // 220 lunar radii away.
    vec3 n = du * p.x + dv * p.y - moonDir * nz;

    // THE TERMINATOR IS dot(n, sunDir) CROSSING ZERO. Lit fraction, limb
    // orientation and the crescent's tilt on screen all fall out of this one
    // dot product -- which is why there is no phase to author here and no way
    // to draw a crescent whose horns face the wrong way.
    //
    // A soft STEP, not a cosine, for the reason MOON_LIMB carries. Its width
    // is ONE PIXEL in disc units: the disc is a few pixels across at gate
    // resolution and tens at 1080p, so a fixed constant is a hard edge at one
    // and a smear at the other. The stars' lesson and the stars' mechanism.
    float discRadius = sqrt(2.0 * (1.0 - cosRadius)); // sin(R), small-angle
    float soft = clamp(pixel / max(discRadius, 1e-6), 0.02, 0.5);
    float lit = smoothstep(-soft, soft, dot(n, sunDir));

    // Maria over the LOCKED face frame rather than the world normal: indexed
    // by n the pattern would swim across the disc as the moon moved, and
    // indexed by the sun-relative frame the terminator builds it would spin
    // with the phase.
    vec3 face = vec3(p, nz);

    // The seas. DOMAIN-WARPED, because the shapes are the tell: unwarped noise
    // through a threshold gives round blobs, and real maria are large irregular
    // lobed regions with inlets. One cheap warp turns one into the other.
    vec3 warp = vec3(starNoise3(face * 1.7 + 11.0), starNoise3(face * 1.7 + 23.0),
                     starNoise3(face * 1.7 + 37.0)) - 0.5;
    vec3 mface = face + warp * MOON_MARIA_WARP;
    float m = 0.62 * starNoise3(mface * MOON_MARIA_COARSE) +
              0.38 * starNoise3(mface * MOON_MARIA_MID);
    float seas = mix(MOON_MARIA_ALBEDO, 1.0,
                     smoothstep(MOON_MARIA_LOW, MOON_MARIA_HIGH, m));

    // Craters at two scales over everything, then the fine mottle. Rims are
    // brighter and floors darker on the HIGHLANDS than on the seas, which is
    // what the photographs show -- basalt has less to throw up.
    float craterScale = mix(0.18, 1.0, smoothstep(MOON_MARIA_ALBEDO, 1.0, seas));
    float craters = moonCraterField(face, MOON_CRATER_BIG, 0.15, 0.10) +
                    moonCraterField(face, MOON_CRATER_SMALL, 0.085, 0.05);
    float grain = starNoise3(face * MOON_MARIA_FINE);

    // A few ray systems, on the highlands only -- rays over a dark sea is the
    // one arrangement the moon does not show.
    float rays = moonRays(face, normalize(vec3(-0.15, -0.62, 0.77)), MOON_RAY_REACH) +
                 0.6 * moonRays(face, normalize(vec3(0.55, 0.35, 0.76)), MOON_RAY_REACH * 0.7);

    float surface = seas * (1.0 + MOON_MARIA_GRAIN * (grain - 0.5)) +
                    craters * craterScale + rays * MOON_RAY_GAIN * craterScale;
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
