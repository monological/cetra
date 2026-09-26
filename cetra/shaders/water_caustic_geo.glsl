#version 330 core

/*
 * Energy per landed cell, exact; shape across it, smooth.
 *
 * The corners' own concentrations (the vertex stage) give a smooth picture but not a
 * conserving one: near a fold a corner's neighbourhood lands on almost nothing, so it reads a
 * huge value, and interpolating that across the large triangles either side of the fold adds
 * light that was never there -- measured at twice the mean on a real sea. The triangle's OWN
 * ratio, beam area over landed area, is exactly what it delivers, but is one flat value per
 * cell and prints as squares.
 *
 * So both: the corners' values are scaled until their mean is the triangle's ratio. Linear
 * interpolation integrates to that mean times the area, so each triangle delivers exactly the
 * light it carries, distributed the way its corners say.
 */

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

in vec2 vLanded[];   // metres from the target corner
in vec2 vSource[];   // metres from the target corner
in float vCorner[];  // the corner's own concentration

out float gIntensity;

// WATER_CAUSTIC_MAX, the ceiling on one cell's ratio.
#include "water_caustic_constants.glsl"

float triangleArea(vec2 a, vec2 b, vec2 c) {
    vec2 u = b - a;
    vec2 v = c - a;
    return 0.5 * abs(u.x * v.y - u.y * v.x);
}

void main() {
    float landed = triangleArea(vLanded[0], vLanded[1], vLanded[2]);
    float source = triangleArea(vSource[0], vSource[1], vSource[2]);
    // The floor on the divisor IS the ceiling: a landed area below source / MAX reads as MAX.
    // The 1e-12 keeps a triangle with no source area from dividing zero by zero.
    float ratio = source / max(landed, max(source / WATER_CAUSTIC_MAX, 1.0e-12));
    float mean = (vCorner[0] + vCorner[1] + vCorner[2]) / 3.0;
    float scale = ratio / max(mean, 1.0e-12);
    for (int i = 0; i < 3; i++) {
        gl_Position = gl_in[i].gl_Position;
        gIntensity = vCorner[i] * scale;
        EmitVertex();
    }
    EndPrimitive();
}
