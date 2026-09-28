#version 330 core

/*
 * Diffraction glare (spec 13.4): one radix-2 stage of a Stockham FFT along one axis of the grid,
 * Clearwater's. Each texel holds two complex values (.xy, .zw), transformed together.
 *
 * Stockham rather than Cooley-Tukey in place: every stage reads the whole previous one and
 * writes a new one, which is the only shape a fragment pass can take, and the order comes out
 * natural with no bit-reversal pass. `glareSign` -1 is the forward transform and +1 the inverse;
 * neither is normalised, which the kernel's spectrum carries instead.
 *
 * Its own pass rather than the water's: that one is fixed to the water's twiddle tables and
 * cascade layers, and this grid is not square.
 */

in vec2 TexCoords;
out vec4 Stage;

uniform sampler2D glareSrc;
uniform int glareSpan;   // the stage's butterfly span, 1, 2, 4, ...
uniform int glareHoriz;  // 1 = along x
uniform int glareHalf;   // half the axis length
uniform float glareSign;

#include "complex.glsl"

void main() {
    ivec2 id = ivec2(gl_FragCoord.xy);
    int j = glareHoriz == 1 ? id.x : id.y;
    int k = j & (glareSpan - 1);
    int i = ((j - (j & (2 * glareSpan - 1))) >> 1) + k;
    bool odd = (j & glareSpan) != 0;
    ivec2 a = glareHoriz == 1 ? ivec2(i, id.y) : ivec2(id.x, i);
    ivec2 b = glareHoriz == 1 ? ivec2(i + glareHalf, id.y) : ivec2(id.x, i + glareHalf);
    vec4 x0 = texelFetch(glareSrc, a, 0);
    vec4 x1 = texelFetch(glareSrc, b, 0);
    float angle = glareSign * 3.14159265359 * float(k) / float(glareSpan);
    vec2 w = vec2(cos(angle), sin(angle));
    vec4 wx = vec4(complexMul(w, x1.xy), complexMul(w, x1.zw));
    Stage = odd ? x0 - wx : x0 + wx;
}
