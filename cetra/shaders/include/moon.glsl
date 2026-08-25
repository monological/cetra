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
// The maria are basalt, roughly half the highlands' albedo. Two octaves over
// the tidally locked face.
const float MOON_MARIA_ALBEDO = 0.52;
const float MOON_MARIA_LOW = 0.40;
const float MOON_MARIA_HIGH = 0.58;
const float MOON_MARIA_COARSE = 2.3;
const float MOON_MARIA_FINE = 5.7;
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
    float m = 0.6 * starNoise3(face * MOON_MARIA_COARSE) +
              0.4 * starNoise3(face * MOON_MARIA_FINE);
    float albedo =
        mix(1.0, mix(MOON_MARIA_ALBEDO, 1.0, smoothstep(MOON_MARIA_LOW, MOON_MARIA_HIGH, m)),
            maria);

    // Mild rim darkening, on the eye-facing component already in hand.
    float limb = MOON_LIMB + (1.0 - MOON_LIMB) * nz;

    vec3 tint = mix(MOON_EARTHSHINE_TINT, MOON_TINT, lit);
    return tint * (albedo * limb * mix(MOON_EARTHSHINE * earthshine, 1.0, lit));
}
