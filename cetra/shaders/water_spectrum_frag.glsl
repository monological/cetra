#version 330 core

/*
 * Time-dependent ocean spectrum (spec 11.32).
 *
 * Advances every Fourier mode by the gravity-wave dispersion relation and emits
 * the four complex fields the inverse FFT will transform together. Per-texel with
 * no neighbourhood, which is why it is a fragment pass and not a compute kernel.
 *
 * The four fields are PACKED IN PAIRS so one transform produces displacement and
 * every derivative at once. Two complex numbers ride in each RGBA target as
 * (a.x - b.y, a.y + b.x): transforming that recovers a in the real part and b in
 * the imaginary part, because the surface is real. Doing it any other way costs
 * three more full transforms for the derivatives, which is the whole reason the
 * normals here are analytic and free.
 */

layout(location = 0) out vec4 Field0;
layout(location = 1) out vec4 Field1;

uniform sampler2D initialSpectrum; // h0(k) in .xy, conj(h0(-k)) in .zw
uniform sampler2D waveData;        // kx, 1/|k|, kz, omega
uniform float time;

vec2 complexMul(vec2 a, vec2 b) {
    return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

void main() {
    ivec2 coord = ivec2(gl_FragCoord.xy);
    vec4 initial = texelFetch(initialSpectrum, coord, 0);
    vec4 wave = texelFetch(waveData, coord, 0);

    float phase = wave.w * time;
    vec2 rot = vec2(cos(phase), sin(phase));
    // h(k,t) = h0(k) e^{i w t} + conj(h0(-k)) e^{-i w t}. The pairing is what
    // makes the inverse transform real without a symmetry pass.
    vec2 h = complexMul(initial.xy, rot) + complexMul(initial.zw, vec2(rot.x, -rot.y));
    vec2 ih = vec2(-h.y, h.x); // multiply by i: a derivative in k becomes this

    float kx = wave.x;
    float kz = wave.z;
    float invK = wave.y;

    vec2 dx = ih * kx * invK;  // horizontal displacement, x
    vec2 dy = h;               // height
    vec2 dz = ih * kz * invK;  // horizontal displacement, z
    vec2 dxdx = -h * kx * kx * invK;
    vec2 dydx = ih * kx;       // slope along x
    vec2 dzdx = -h * kx * kz * invK;
    vec2 dydz = ih * kz;       // slope along z
    vec2 dzdz = -h * kz * kz * invK;

    Field0 = vec4(dx.x - dz.y, dx.y + dz.x, dy.x - dzdx.y, dy.y + dzdx.x);
    Field1 = vec4(dydx.x - dydz.y, dydx.y + dydz.x, dxdx.x - dzdz.y, dxdx.y + dzdz.x);
}
