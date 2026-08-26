/*
 * The lunar disc (spec 11.82). GEOMETRY only: where the face is, which side is
 * lit, where the terminator falls, and the earthshine on the dark limb.
 *
 * The SURFACE -- the crater relief and the albedo -- arrives BAKED, on the
 * texture moon_surface.c builds at startup. That is the whole reason this file
 * is short. The first version evaluated a crater lattice per pixel, which caps
 * the population at what a 3x3x3 neighbourhood can hold; a crater field is
 * billions of years of overprinting and what the eye reads is the OVERLAP, which
 * no per-pixel lattice can reach at any price.
 *
 * The BRIGHTNESS comes from the caller and already carries the
 * Krisciunas-Schaefer phase law -- so this file and the C side cannot disagree
 * about the phase, because they derive it from the same two directions rather
 * than passing a number.
 */

// Mild rim darkening. A full moon is NOT a Lambertian sphere, and that is its
// whole visual signature: lunar regolith backscatters, so a real full moon is a
// flat bright disc with almost no centre-to-limb gradient. 0.85 against the
// sun's own 0.4 -- the same functional form, the opposite intent.
const float MOON_LIMB = 0.85;
// Local, because this file is included by programs that define neither.
const float MOON_TAU = 6.28318530718;
const float MOON_PI = 3.14159265359;
// What an UNTEXTURED face reads as. Matches the bake's highland albedo, so
// turning the surface off changes the pattern and not the overall brightness.
const float MOON_ALBEDO_PLAIN = 0.82;
// How far the baked normal is allowed to tilt the surface. 1 is the relief as
// baked, which is already the real depth-to-diameter ratio.
const float MOON_RELIEF_GAIN = 1.0;
// Earthshine: the dark limb is Earth-lit, not black -- the old moon in the new
// moon's arms. Stated RELATIVE to the lit side, so it rides the phase law the
// caller applies and can never outshine the crescent beside it. Blue because it
// is sunlight bounced off an ocean planet, which is the one genuinely blue thing
// about moonlight; the blue people remember at night is the Purkinje shift, and
// that is a different feature.
const float MOON_EARTHSHINE = 0.020;
const vec3 MOON_EARTHSHINE_TINT = vec3(0.62, 0.76, 1.00);
// Near-neutral, faintly warm.
const vec3 MOON_TINT = vec3(1.00, 0.98, 0.95);
// The aureole's peak radiance, as a fraction of the disc's. Its WIDTH is a
// uniform (moonGlowAng) rather than a constant here: it has to relate to the
// drawn size, and how is a look decision that belongs with the others in sky.c.
const float MOON_GLOW = 0.020;

// edge      the sun disc's own [0,1] radial parameter, 1 at the centre
// dir       the view ray
// moonDir   toward the moon
// sunDir    toward the sun
// cosRadius cosine of the disc's angular RADIUS
// pixel     one pixel's angular footprint, taken by the caller under uniform
//           control flow (the stars' rule: a derivative inside non-uniform flow
//           is undefined in GLSL 330)
vec3 moonDisc(float edge, vec3 dir, vec3 moonDir, vec3 sunDir, float cosRadius, float pixel,
              float earthshine, float maria, sampler2D moonSurfaceTex) {
    // The disc coordinate, from the sun block's own `edge` rather than a second
    // trig call: s is the normalised radius and nz the eye-facing component, and
    // since s^2 = 1 - edge exactly, nz is just sqrt(edge).
    //
    // The approximation this rests on -- that 1 - edge stands in for
    // sin^2(theta)/sin^2(R) -- is a few parts per million at the shipping
    // half-degree disc, but the gate arms drive it to 40 degrees where it is off
    // by ~1.5% near centre. Harmless for the look at any real size, and named
    // here because that residual shows up in moon-lit's ladder rather than being
    // a defect in the phase geometry.
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
     * THE SURFACE LOOKUP. Equirectangular on the face vector: the disc centre is
     * the sub-observer point, so latitude is asin(y) and longitude atan2(x, z)
     * with the near side at zero -- which also makes the foreshortening toward
     * the limb fall out, since equal steps in the face vector are unequal steps
     * in longitude there. The wrap seam sits at longitude 180, on the far side,
     * and is never drawn.
     */
    vec2 muv = vec2(atan(face.x, face.z) / MOON_TAU + 0.5,
                    0.5 - asin(clamp(face.y, -1.0, 1.0)) / MOON_PI);
    vec4 surf = texture(moonSurfaceTex, muv);

    /*
     * THE RELIEF NORMAL, which is what turns a painted disc into a surface. A
     * crater rim catches grazing light on one side and drops its own bowl into
     * shadow on the other, and the two swap as the phase moves -- that is what
     * the eye reads as relief, and it is not a colour, so no albedo map produces
     * it.
     *
     * The baked normal is tangent-space, so it needs the local east/north pair.
     * Both come out of the face vector with no trigonometry: east is the
     * longitude derivative, which is (z, 0, -x) normalised, and north is their
     * cross product. Degenerate only at the poles, which sit at the very top and
     * bottom of the drawn disc.
     */
    vec3 tn = normalize(surf.xyz * 2.0 - 1.0);
    tn.xy *= MOON_RELIEF_GAIN;
    float cl = length(face.xz);
    vec3 east = (cl > 1e-4) ? vec3(face.z, 0.0, -face.x) / cl : vec3(1.0, 0.0, 0.0);
    vec3 north = cross(face, east);
    vec3 relief = normalize(east * tn.x + north * tn.y + face * tn.z);

    // Back to world: the face frame's axes are (du, dv, -moonDir).
    vec3 n = du * relief.x + dv * relief.y - moonDir * relief.z;

    /*
     * LOMMEL-SEELIGER, not Lambert. The regolith backscatters, so brightness
     * goes as mu0/(mu0+mu): at full phase mu0 and mu are equal everywhere and
     * the disc reads a flat 0.5, which is exactly why a real full moon has no
     * centre-to-limb shading and why cosine lighting renders a snooker ball.
     * Near the terminator the same expression falls off slowly and lets crater
     * walls throw their own shade, which is where all the relief lives.
     *
     * Normalised so a head-on full moon reads 1, since the bare ratio is 0.5
     * there.
     */
    float mu0 = dot(n, sunDir);
    float mu = dot(n, -moonDir);
    float lit = max(mu0, 0.0) / max(mu0 + mu, 1e-3) * 2.0;
    // The terminator, one pixel wide on the zero crossing -- the disc's angular
    // radius converts the screen pixel into the face units mu0 varies over.
    float px = pixel / max(sqrt(2.0 * (1.0 - cosRadius)), 1e-6);
    lit *= smoothstep(-px, px, mu0);
    lit = clamp(lit, 0.0, 1.4);

    float albedo = mix(MOON_ALBEDO_PLAIN, surf.a, maria);

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
    float rimSoft = clamp(px, 0.004, 0.25);
    float rim = 1.0 - smoothstep(1.0 - rimSoft, 1.0, s);

    vec3 tint = mix(MOON_EARTHSHINE_TINT, MOON_TINT, lit);
    return tint * (rim * albedo * limb * mix(MOON_EARTHSHINE * earthshine, 1.0, lit));
}
