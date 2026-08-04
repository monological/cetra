#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Depth-of-field, first pass: downsample the sharp scene to half resolution
// and pack a signed circle-of-confusion (CoC) into alpha. CoC < 0 is nearer
// than the focus plane (foreground), CoC > 0 is farther (background); its
// magnitude is the blur radius in half-res texels. Runs at half res.
uniform sampler2D sceneTex; // Full-res HDR scene (post SSR)
uniform sampler2D depthTex; // Full-res resolved depth
uniform mat4 projection;
uniform float focusDistance; // View-space distance kept sharp
uniform float focusRange;    // Distance over which CoC ramps to max
uniform float maxCoC;        // Max blur radius, half-res texels

// Analytic view-space Z from an NDC depth (cglm right-handed perspective)
#define DOF_COC
#include "depth.glsl"

void main()
{
    vec3 color = texture(sceneTex, TexCoords).rgb;
    float depth = texture(depthTex, TexCoords).r;

    float coc = cocAtNdcZ(depth * 2.0 - 1.0, depth >= 1.0);

    FragColor = vec4(color, coc);
}
