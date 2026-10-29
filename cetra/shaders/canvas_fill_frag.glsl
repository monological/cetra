#version 330 core

// A filled polygon's triangles (spec 14.0): whole inside, and the inner half of the edge ramp,
// coverage 0.5 + d with d the distance in from the nearest of the six outline edges that meet the
// triangle's corners. The outer half is EDGE records in the shape pass, so a pixel by the outline
// has one coverage from each side and they meet at 0.5 on the edge.

#include "canvas_constants.glsl"
#include "canvas_pass.glsl"

flat in vec4 vColor;
flat in float vRaised;
flat in vec2 vNear[9];
noperspective in vec2 vPixel;

out vec4 FragColor;

float segmentDistance(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-12), 0.0, 1.0);
    return length(p - a - ab * t);
}

void main() {
    float d = 1e30;
    for (int k = 0; k < 3; k++) {
        d = min(d, segmentDistance(vPixel, vNear[k], vNear[3 + 2 * k]));
        d = min(d, segmentDistance(vPixel, vNear[k], vNear[4 + 2 * k]));
    }
    FragColor = canvasOutput(vColor, vRaised > 0.5 ? CANVAS_RANK_RAISED : CANVAS_RANK_SHAPE,
                             clamp(0.5 + d, 0.0, 1.0));
}
