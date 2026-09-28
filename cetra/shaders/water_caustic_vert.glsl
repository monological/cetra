#version 330 core

/*
 * Refracted-grid caustics (spec 13.2), second half: draw the lattice where its light landed.
 *
 * No attributes: the vertex is its index into the (G+1)^2 lattice, which the index buffer
 * stitches into triangles, and where it landed is a texel of the first half's output.
 *
 * The concentration is measured HERE, per corner, from the neighbouring corners -- the same
 * ratio, beam area over landed area, taken over the corner's own neighbourhood -- and handed to
 * the geometry stage as the SHAPE of the light across each cell. Measured per triangle alone it
 * is one flat value per cell, and a focused line reads as a staircase of squares wherever a
 * cell is larger than a few pixels.
 */

#include "water_caustic_constants.glsl"

uniform sampler2D causticLanded; // .xy landed, .zw source, metres from the target corner
uniform sampler2D causticLandedPerIndex; // .xy how the landing moves per unit index, metres
uniform float causticTargetM;    // this level's target side, metres

out vec2 vLanded;
out vec2 vSource;
out float vCorner;
out vec2 vPerIndex;

vec4 corner(ivec2 ij) {
    return texelFetch(causticLanded, clamp(ij, ivec2(0), ivec2(WATER_CAUSTIC_GRID_N)), 0);
}

void main() {
    int side = WATER_CAUSTIC_GRID_N + 1;
    ivec2 ij = ivec2(gl_VertexID % side, gl_VertexID / side);
    vec4 here = corner(ij);

    // Central differences, one-sided at the lattice's own edge. The edge lies well outside the
    // target, so how it is closed there does not reach anything that is read.
    ivec2 lo = max(ij - 1, ivec2(0));
    ivec2 hi = min(ij + 1, ivec2(WATER_CAUSTIC_GRID_N));
    vec4 di = (corner(ivec2(hi.x, ij.y)) - corner(ivec2(lo.x, ij.y))) / float(hi.x - lo.x);
    vec4 dj = (corner(ivec2(ij.x, hi.y)) - corner(ivec2(ij.x, lo.y))) / float(hi.y - lo.y);
    float landedArea = abs(di.x * dj.y - di.y * dj.x);
    float sourceArea = abs(di.z * dj.w - di.w * dj.z);
    // The floor on the divisor IS the ceiling: a landed area below source / MAX reads as MAX.
    // Only the SHAPE is taken from the corners -- the geometry stage rescales them to the
    // triangle's exact ratio -- so here the ceiling bounds how sharp a peak can be within a
    // cell, not how much light it holds. The 1e-12 keeps a degenerate corner with no source area
    // from dividing zero by zero.
    vCorner = sourceArea / max(landedArea, max(sourceArea / WATER_CAUSTIC_MAX, 1.0e-12));
    vLanded = here.xy;
    vSource = here.zw;
    vPerIndex = texelFetch(causticLandedPerIndex, ij, 0).xy;

    vec2 uv = here.xy / causticTargetM;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
