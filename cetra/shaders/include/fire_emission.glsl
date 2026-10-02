// What a fire's gas emits (spec 13.14): the one statement of it, read by the march that draws a
// fire and the reduction that sums its light, so the light a fire casts is the luminance of what
// is drawn. fire.c's fire_emission is the same arithmetic on the CPU, for a FLAME's light and the
// probe.

#include "fire_grid.glsl"
#include "blackbody.glsl"

uniform float ambient;        // K
uniform float sootAbsorption; // 1/m per ppm
uniform float blueCore;       // nits per metre at a core weight of 1
uniform vec3 blueColor;       // fire_blue_color: unclamped, luminance 1
uniform float brightness;
uniform mat3 adaptation; // fire_adaptation: the eye's adaptation to the fire, on Rec.709

float fireLuminance(vec3 rgb) {
    return dot(rgb, vec3(0.2126, 0.7152, 0.0722));
}

// Nits per metre a length of gas emits, as the adapted eye sees it: soot glowing as a blackbody
// at its temperature (Pegoraro and Parker 2006), plus the core's blue, adapted and then clamped
// into the gamut -- here, sample by sample, and nowhere after. The soot's absorption, 1/m, in
// `absorption`.
vec3 fireEmission(FireGas g, out float absorption) {
    absorption = max(g.soot, 0.0) * sootAbsorption;
    float glow = blueCore * max(g.core, 0.0);
    if (absorption <= 0.0 && glow <= 0.0)
        return vec3(0.0);
    float lum;
    vec3 bb = blackbodyNits(ambient + max(g.rise, 0.0), lum);
    return max(adaptation * (absorption * bb + glow * blueColor), vec3(0.0)) * brightness;
}
