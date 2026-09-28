#version 330 core

/*
 * Diffraction glare (spec 13.4): the bright image's spectrum times the aperture's, which is the
 * convolution of the two once transformed back -- every glint starred at the cost of one
 * multiply per texel, however many glints there are.
 *
 * .xy is the spectrum Z of red + i green. Both are real, so each spectrum is Hermitian and the
 * two come apart from Z at k and at -k: R = (Z(k) + conj Z(-k)) / 2, G = (Z(k) - conj Z(-k)) / 2i.
 * Each is multiplied by its own channel's kernel and packed back as R' + i G', which is
 *     Z(k) (Kr + Kg) / 2 + conj Z(-k) (Kr - Kg) / 2,
 * so the inverse transform returns red in the real part and green in the imaginary. .zw is blue,
 * alone, times its kernel.
 *
 * The kernels are REAL: the aperture's pattern is a power spectrum, symmetric about its centre,
 * so its transform has no imaginary part.
 */

in vec2 TexCoords;
out vec4 Product;

uniform sampler2D glareSrc;
uniform sampler2D glareKernel; // .rgb each channel's kernel spectrum

#include "complex.glsl"

void main() {
    ivec2 size = textureSize(glareSrc, 0);
    ivec2 id = ivec2(gl_FragCoord.xy);
    ivec2 mirror = (size - id) % size;
    vec4 a = texelFetch(glareSrc, id, 0);
    vec2 zm = complexConj(texelFetch(glareSrc, mirror, 0).xy);
    vec3 k = texelFetch(glareKernel, id, 0).rgb;
    Product = vec4(a.xy * (0.5 * (k.r + k.g)) + zm * (0.5 * (k.r - k.g)), a.zw * k.b);
}
