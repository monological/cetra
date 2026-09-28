#version 330 core

/*
 * Diffraction glare (spec 13.4): the bright image's spectrum times the aperture's, which is the
 * convolution of the two once transformed back -- every glint starred at the cost of one
 * multiply per texel, however many glints there are. Two complex values per texel, each with its
 * own channel's kernel.
 */

in vec2 TexCoords;
out vec4 Product;

uniform sampler2D glareSrc;
uniform sampler2D glareKernel;

vec2 cmul(vec2 a, vec2 b) {
    return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

void main() {
    ivec2 id = ivec2(gl_FragCoord.xy);
    vec4 a = texelFetch(glareSrc, id, 0);
    vec4 k = texelFetch(glareKernel, id, 0);
    Product = vec4(cmul(a.xy, k.xy), cmul(a.zw, k.zw));
}
