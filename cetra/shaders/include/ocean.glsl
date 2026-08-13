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

struct OceanSurface {
    vec3 world;  // displaced world position
    vec3 normal; // unit normal from the analytic derivatives
};

/*
 * Sum of Gerstner waves.
 *
 * A Gerstner wave moves its surface points horizontally as well as vertically,
 * bunching them toward the crest, which is what sharpens a crest and broadens a
 * trough instead of leaving a symmetric sine. The horizontal term is what makes
 * it a Gerstner wave rather than a height field, and it is also what can fold the
 * surface over itself: the mapping stops being injective once Q*A*k exceeds 1.
 *
 * So steepness is NOT passed through to Q. It is divided by the octave's own
 * A*k and by the octave count, which makes waterSteepness a bounded 0..1 knob
 * that cannot fold the mesh at any amplitude or wavelength. Authoring Q directly
 * would make "sharp" mean something different at every wavelength and put a
 * self-intersecting surface one slider nudge away.
 */
OceanSurface oceanEvaluate(vec2 p, float t) {
    vec3 displaced = vec3(p.x, waterLevel, p.y);
    // Partial derivatives of the displaced position with respect to the
    // undisplaced grid coordinates. The identity rows are the flat plane's
    // contribution, which the horizontal displacement then bends.
    vec3 dPdx = vec3(1.0, 0.0, 0.0);
    vec3 dPdz = vec3(0.0, 0.0, 1.0);

    float wavelength = waterWavelength;
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

        // See the note above: Q is derived so the surface cannot fold.
        float q = waterSteepness / max(k * amplitude * float(OCEAN_WAVES), 1e-4);
        float qa = q * amplitude;

        displaced.x += qa * dir.x * cosp;
        displaced.y += amplitude * sinp;
        displaced.z += qa * dir.y * cosp;

        float dqa = qa * k * sinp;
        float dah = amplitude * k * cosp;
        dPdx.x -= dqa * dir.x * dir.x;
        dPdx.y += dah * dir.x;
        dPdx.z -= dqa * dir.y * dir.x;
        dPdz.x -= dqa * dir.x * dir.y;
        dPdz.y += dah * dir.y;
        dPdz.z -= dqa * dir.y * dir.y;

        wavelength *= OCEAN_LENGTH_FALLOFF;
        amplitude *= OCEAN_AMPLITUDE_FALLOFF;
    }

    OceanSurface s;
    s.world = displaced;
    // cross(dPdz, dPdx) and not the reverse: on a flat surface that is +Y, and
    // the flipped order would light every wave from underneath.
    s.normal = normalize(cross(dPdz, dPdx));
    return s;
}
