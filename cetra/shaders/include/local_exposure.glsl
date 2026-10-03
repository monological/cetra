// Local exposure (spec 13.19; Durand & Dorsey 2002, as Unreal 5 does it), the tonemap's half:
// a per-pixel exposure on top of the camera's. The pixel's log2 luminance is split into a BASE --
// the bilateral grid sliced at its own luminance, mixed with a heavily blurred luminance -- and the
// DETAIL above it. The base's distance from middle grey is scaled by the highlight or shadow
// contrast and the detail by its own strength, and the factor is what turns the pixel's
// luminance into that. Every scale at 1 is the identity.
//
// Declares localExposureTex, the tonemap's 15th sampler.

#include "le_grid.glsl"

uniform sampler2D localExposureTex;
uniform int localExposureEnabled;
uniform float leMiddleGrey; // log2, in the exposed space the buffer arrives in
uniform float leHighlights; // contrast of the base above middle grey; 1 = unchanged
uniform float leShadows;    // and below it
uniform float leDetail;     // contrast of the detail; 1 = unchanged
uniform float leBlend;      // share of the base taken from the blurred luminance

// The grid sliced at `uv` and log2 luminance `y`: a bilinear tap in the two bins either side,
// mixed, each kept half a texel inside its tile so it never blends the bin beside it (Chen et
// al.'s two lookups). Homogeneous, so the caller divides.
vec2 leGridAt(vec2 uv, float y) {
    vec2 xy = clamp(uv * leCellScale, vec2(0.5), vec2(leGrid.xy) - 0.5);
    float z = clamp(leBinCoord(y), 0.0, float(leGrid.z - 1));
    int z0 = int(floor(z));
    int z1 = min(z0 + 1, leGrid.z - 1);
    vec2 atlas = vec2(textureSize(localExposureTex, 0));
    vec2 s0 = textureLod(localExposureTex, (vec2(leTileOrigin(z0)) + xy) / atlas, 0.0).rg;
    vec2 s1 = textureLod(localExposureTex, (vec2(leTileOrigin(z1)) + xy) / atlas, 0.0).rg;
    return mix(s0, s1, z - float(z0));
}

float leBlurredAt(vec2 uv) {
    vec2 xy = clamp(uv * vec2(leBlurRect.zw), vec2(0.5), vec2(leBlurRect.zw) - 0.5);
    vec2 atlas = vec2(textureSize(localExposureTex, 0));
    return textureLod(localExposureTex, (vec2(leBlurRect.xy) + xy) / atlas, 0.0).r;
}

// The exposure this pixel gets on top of the camera's, for its composited light `c`.
float localExposureFactor(vec2 uv, vec3 c) {
    float y = log2(max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1.0e-8));
    float blurred = leBlurredAt(uv);
    vec2 g = leGridAt(uv, y);
    // A pixel with nothing near its own luminance in the grid around it -- a thin bright line the
    // half-res grid never resolved -- has no bilateral answer, and takes the blurred one.
    float bilateral = g.y > 1.0e-7 ? g.x / g.y : blurred;
    float base = mix(bilateral, blurred, leBlend);
    float contrast = base > leMiddleGrey ? leHighlights : leShadows;
    float target = leMiddleGrey + (base - leMiddleGrey) * contrast + (y - base) * leDetail;
    return exp2(target - y);
}
