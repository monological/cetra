#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Depth-of-field composite at full resolution: far field UNDER the sharp
// scene, near field alpha-OVER both. The far blend comes from a full-res CoC
// recompute (cheap, and a full-res-crisp classification edge -- the half-res
// alpha would shimmer at silhouettes); its narrow smoothstep means the far
// field fully replaces the sharp scene once defocus exceeds ~1 half-res
// texel, which is what lets aperture shapes actually read instead of staying
// mixed with sharp content until maximum defocus. The near field composites
// over the result, so a near bokeh disc correctly occludes a far one. Pixels
// in focus and free of near spill return the sharp texel bit-for-bit.
uniform sampler2D sceneTex; // Full-res sharp HDR scene
uniform sampler2D nearTex;  // Half-res near field: premultiplied + coverage
uniform sampler2D farTex;   // Half-res far field (linear upsample)
uniform sampler2D depthTex; // Full-res resolved depth
uniform mat4 projection;
uniform float focusDistance;
uniform float focusRange;
uniform float maxCoC;

#define DOF_COC
#include "depth.glsl"

void main()
{
    vec3 sharp = texture(sceneTex, TexCoords).rgb;

    float depth = texture(depthTex, TexCoords).r;
    float coc = cocAtNdcZ(depth * 2.0 - 1.0, depth >= 1.0);

    // Band in half-res texels. The 0.5 start must stay ABOVE the gather's
    // CLASS_EPS (0.25): every pixel this blends must have been classified
    // into the far field by its own centre tap, or the field it reads here
    // never saw its colour.
    float farBlend = smoothstep(0.5, 1.5, coc);
    vec4 nearF = texture(nearTex, TexCoords);
    if (farBlend <= 0.0 && nearF.a <= 0.0) {
        FragColor = vec4(sharp, 1.0); // in focus, no spill: untouched
        return;
    }
    vec3 base = mix(sharp, texture(farTex, TexCoords).rgb, farBlend);
    FragColor = vec4(nearF.rgb + base * (1.0 - nearF.a), 1.0);
}
