#version 330 core

// A filled polygon's triangles (spec 14.0), three corners each from the layer's fill buffer
// (canvas_record.h). Each corner carries the nine outline points its triangle's inner ramp is
// measured against, which reach the fragment stage in pixels from the view's centre.

#include "canvas_constants.glsl"
#include "canvas_paint.glsl"

layout(location = 0) in ivec2 aP;
layout(location = 1) in ivec4 aNear0; // corner 0, corner 1
layout(location = 2) in ivec4 aNear1; // corner 2, corner 0's neighbour before it
layout(location = 3) in ivec4 aNear2; // corner 0's after, corner 1's before
layout(location = 4) in ivec4 aNear3; // corner 1's after, corner 2's before
layout(location = 5) in ivec2 aNear4; // corner 2's after
layout(location = 6) in uvec4 aU;

flat out vec4 vColor;
flat out float vRaised;
flat out vec2 vNear[9];
noperspective out vec2 vPixel; // this fragment, in pixels from the view's centre

void main() {
    vColor = vec4(0.0);
    vRaised = 0.0;
    vPixel = vec2(0.0);
    for (int i = 0; i < 9; i++)
        vNear[i] = vec2(0.0);
    // Outside every clip plane: a triangle of three such corners draws nothing, and the three
    // corners of one triangle share their tile and their tags.
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    // A polygon's outline ring is drawn by its edge records, outside it.
    ivec4 tile = texelFetch(uTiles, int(aU.w));
    vec4 outline;
    if ((aU.x & 63u) != CANVAS_KIND_FILL || tile.z == 0 ||
        !canvasColour(aU, vColor, outline, vRaised))
        return;
    vNear[0] = canvasPixels(aNear0.xy, tile.xy);
    vNear[1] = canvasPixels(aNear0.zw, tile.xy);
    vNear[2] = canvasPixels(aNear1.xy, tile.xy);
    vNear[3] = canvasPixels(aNear1.zw, tile.xy);
    vNear[4] = canvasPixels(aNear2.xy, tile.xy);
    vNear[5] = canvasPixels(aNear2.zw, tile.xy);
    vNear[6] = canvasPixels(aNear3.xy, tile.xy);
    vNear[7] = canvasPixels(aNear3.zw, tile.xy);
    vNear[8] = canvasPixels(aNear4, tile.xy);
    vPixel = canvasPixels(aP, tile.xy);
    gl_Position = canvasClip(vPixel);
}
