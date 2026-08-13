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

// The baked bed, for shoaling. Absent (bedAvailable 0) is the normal case: the
// per-fragment water column comes from the resolved scene depth instead, which is
// exact and works against arbitrary geometry. This answers the one question screen
// depth cannot -- a vertex needs its own depth to know how far to move, and
// sampling screen depth would need the position that depth is meant to produce.
uniform sampler2D bedTex;
uniform int bedAvailable;
uniform float waterExtent;

// Depth over which a wave goes from fully shoaled to fully open-water. Waves
// shorten and steepen as the bed rises under them; below the floor there is not
// enough water column left to carry any displacement at all.
const float OCEAN_SHOAL_MIN = 0.14;
const float OCEAN_SHOAL_FULL = 2.7;

// 1 in open water, falling to 0 as the bed comes up. Multiplies displacement, not
// height alone: the horizontal term has to shrink with it or the surface slides
// sideways over a beach it is no longer above.
float oceanShoal(vec2 p) {
    if (bedAvailable == 0)
        return 1.0;
    vec2 uv = p / (waterExtent * 2.0) + 0.5;
    float bed = texture(bedTex, clamp(uv, vec2(0.0), vec2(1.0))).r;
    return smoothstep(OCEAN_SHOAL_MIN, OCEAN_SHOAL_FULL, waterLevel - bed);
}

struct OceanSurface {
    vec3 world;  // displaced world position
    vec3 normal; // unit normal from the analytic derivatives
    float shoal; // 1 = open water, 0 = the bed has reached the surface
    // Horizontal-map Jacobian. Below 1 the surface is COMPRESSING, which is where
    // a real sea throws whitewater, so this is the foam selector rather than a
    // height threshold. Always 1 on the Gerstner path, whose steepness is
    // clamped so the mapping cannot compress that far.
    float jacobian;
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
/*
 * Spectral surface: the long and medium bands displace the mesh, the short band
 * is left for the fragment stage's slope distribution.
 *
 * Every derivative comes out of the same transform as the height -- field0.a is a
 * cross derivative, field1.rg the two slopes, field1.ba the two horizontal
 * derivatives -- so the normal below is analytic and costs nothing extra. That
 * packing is the reason to spend two RGBA targets per cascade.
 */
OceanSurface oceanEvaluateSpectral(vec2 p) {
    vec4 long0 = texture(cascade0_0, fract(p / cascadeLength[0] + 0.5));
    vec4 long1 = texture(cascade0_1, fract(p / cascadeLength[0] + 0.5));
    vec4 med0 = texture(cascade1_0, fract(p / cascadeLength[1] + 0.5));
    vec4 med1 = texture(cascade1_1, fract(p / cascadeLength[1] + 0.5));

    float q0 = cascadeChoppiness[0];
    float q1 = cascadeChoppiness[1];

    vec2 horizontal = long0.rg * q0 + med0.rg * q1;
    float height = long0.b + med0.b;
    // A linear random sea is vertically symmetric, and a real one is not: crests
    // are sharper than troughs are deep. This low-order bound-harmonic term is the
    // cheap weakly-nonlinear correction for that, kept small deliberately -- pushed
    // harder it folds the surface, which is what the choppy-wave literature bounds.
    height += 0.14 * (long0.b * long0.b - 0.080) + 0.32 * (med0.b * med0.b - 0.030);
    // Not named `cross`: that is a builtin this function calls two lines below.
    float crossDeriv = long0.a * q0 + med0.a * q1;
    vec2 slope = long1.rg + med1.rg;
    vec2 dHoriz = long1.ba * q0 + med1.ba * q1;

    float shoal = oceanShoal(p);

    OceanSurface s;
    // No amplitude knob here, deliberately. The spectrum is already physical --
    // its height comes out of the wind speed and fetch it was seeded with -- so
    // scaling it would be authoring over a sea state rather than choosing one.
    // waterAmplitude belongs to the Gerstner path, where there is no sea state to
    // ask. A calmer spectral ocean is a lower wind speed, not a smaller number
    // here.
    s.world = vec3(p.x + horizontal.x * shoal, waterLevel + height * shoal,
                   p.y + horizontal.y * shoal);
    // The derivatives carry the same factor, so the normal stays the normal OF the
    // displaced surface rather than of the open-water one it would have been.
    vec3 dPdx = vec3(1.0 + dHoriz.x * shoal, slope.x * shoal, crossDeriv * shoal);
    vec3 dPdz = vec3(crossDeriv * shoal, slope.y * shoal, 1.0 + dHoriz.y * shoal);
    s.normal = normalize(cross(dPdz, dPdx));
    s.jacobian = (1.0 + dHoriz.x * shoal) * (1.0 + dHoriz.y * shoal) -
                 crossDeriv * crossDeriv * shoal * shoal;
    s.shoal = shoal;
    return s;
}

OceanSurface oceanEvaluate(vec2 p, float t) {
    if (waveModel == 1)
        return oceanEvaluateSpectral(p);

    float shoal = oceanShoal(p);
    vec3 displaced = vec3(p.x, waterLevel, p.y);
    // Partial derivatives of the displaced position with respect to the
    // undisplaced grid coordinates. The identity rows are the flat plane's
    // contribution, which the horizontal displacement then bends.
    vec3 dPdx = vec3(1.0, 0.0, 0.0);
    vec3 dPdz = vec3(0.0, 0.0, 1.0);

    float wavelength = waterWavelength;
    float amplitude = waterAmplitude * shoal;
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
    // Steepness is clamped so this mapping cannot compress; reporting 1 says
    // "nothing to select foam from" rather than approximating a number the model
    // does not produce.
    s.jacobian = 1.0;
    s.shoal = shoal;
    return s;
}
