#version 330 core

// A late-draw surface (spec 13.29's fixture): a field of noise new every frame, `noiseCells.x`
// cells across and `.y` down, each grey at up to `.z` nits, added over whatever is behind it.
// Drawn past TAA, so no history averages the frames together.

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;
in vec4 vColor;
in float vViewDepth;
out vec4 FragColor;

#include "late_surface.glsl"
#include "noise.glsl"

uniform vec4 noiseCells;

void main()
{
    uvec2 cell = uvec2(vUv * noiseCells.xy);
    float n = frameNoise(cell, uint(frame));
    // A centimetre of slack: the quad is a metre clear of the wall behind it.
    float shown = lateVisible(vViewDepth, 0.01);
    FragColor = vec4(lateEmit(vec3(n * noiseCells.z), vViewDepth) * shown, 0.0);
}
