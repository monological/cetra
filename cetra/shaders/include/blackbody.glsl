// A blackbody's colour and brightness at a temperature (spec 13.14), read from the table
// fire.c builds by integrating Planck's law against the CIE 1931 observer. Each texel is the
// Rec.709 chromaticity (rgb over luminance) and log10 of the luminance, so interpolating
// between two neighbours is interpolating the exponent rather than a curve that climbs five
// decades across the table. Read by texelFetch and interpolated here, in exactly fire.c's
// arithmetic, so the light the CPU derives and the emission drawn agree.
//
// Needs fire_constants.glsl.

uniform sampler2D blackbodyLut; // FIRE_BB_LUT_SIZE x 1, RGBA32F

// Nits per channel; black below FIRE_BB_T_MIN, where a body glows too faintly to matter.
vec3 blackbodyNits(float kelvin, out float luminance) {
    luminance = 0.0;
    if (!(kelvin >= FIRE_BB_T_MIN))
        return vec3(0.0);
    float x = min((kelvin - FIRE_BB_T_MIN) / (FIRE_BB_T_MAX - FIRE_BB_T_MIN), 1.0) *
              float(FIRE_BB_LUT_SIZE - 1);
    int i0 = int(floor(x));
    int i1 = min(i0 + 1, FIRE_BB_LUT_SIZE - 1);
    vec4 a = texelFetch(blackbodyLut, ivec2(i0, 0), 0);
    vec4 b = texelFetch(blackbodyLut, ivec2(i1, 0), 0);
    vec4 texel = a + (b - a) * (x - float(i0));
    luminance = pow(10.0, texel.w);
    return texel.rgb * luminance;
}

// The reaction zone's own glow, CH* and C2* chemiluminescence: a flame's blue base. Luminance
// 1; fire.c's FIRE_BLUE.
const vec3 FIRE_BLUE = vec3(0.31, 0.68, 6.2);
