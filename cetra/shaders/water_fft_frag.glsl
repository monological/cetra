#version 330 core

/*
 * One Stockham inverse-FFT stage (spec 11.32).
 *
 * Reads exactly two texels plus one precomputed twiddle row and writes one texel
 * to two targets. That shape is the reason this ocean runs at all under the GL 4.1
 * ceiling: there is no shared memory, no atomic and no scatter anywhere in a
 * Stockham butterfly, so the "compute shader" it is usually written as is a
 * fragment pass with MRT and nothing is lost but the dispatch.
 *
 * Stockham rather than Cooley-Tukey because it is self-sorting: the input indices
 * per stage are precomputed into the twiddle table, so there is no bit-reversal
 * permutation pass and no in-place addressing to get wrong.
 *
 * Two targets per pass because the spectrum stage packed four complex fields into
 * two RGBA textures; the butterfly is linear, so transforming the packed pair
 * transforms all four at once.
 */

layout(location = 0) out vec4 Out0;
layout(location = 1) out vec4 Out1;

uniform sampler2D twiddleTex; // per (index, stage): rotation .xy, input indices .zw
/*
 * The band being transformed, as two LAYERS of the cascade array rather than two textures.
 *
 * The cascades were consolidated into one array so water_frag could stop sitting at its
 * sixteen-sampler ceiling (see ocean.glsl); this pass writes them, so it reads them the same
 * way. `inLayer` is the first of the pair -- the second is always the next one up, because
 * that is what a cascade's two MRT targets are.
 */
uniform sampler2DArray inFields;
uniform int inLayer;
uniform int axis;     // 0 = transform along x, 1 = along y
uniform int stage;    // 0 .. log2(size) - 1
uniform int size;
uniform int finalize; // 1 on the last stage only; folds in the fftshift

vec2 complexMul(vec2 a, vec2 b) {
    return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// Both halves of the RGBA carry a complex number, and both take the same rotation.
vec4 butterfly(vec4 a, vec4 b, vec2 twiddle) {
    return vec4(a.xy + complexMul(twiddle, b.xy), a.zw + complexMul(twiddle, b.zw));
}

void main() {
    ivec2 id = ivec2(gl_FragCoord.xy);
    int transformIndex = (axis == 0) ? id.x : id.y;

    vec4 data = texelFetch(twiddleTex, ivec2(transformIndex, stage), 0);
    // The indices were stored as floats in an fp32 texture; round rather than
    // truncate, because a value stored as 63.999997 truncates to the wrong
    // butterfly partner and the error is a plausible-looking ripple, not a crash.
    int first = int(round(data.z));
    int second = int(round(data.w));

    ivec2 coord0 = id;
    ivec2 coord1 = id;
    if (axis == 0) {
        coord0.x = first;
        coord1.x = second;
    } else {
        coord0.y = first;
        coord1.y = second;
    }

    // Conjugate rotation: the table holds the forward twiddle and this is the
    // INVERSE transform.
    vec2 inverseTwiddle = vec2(data.x, -data.y);
    vec4 v0 = butterfly(texelFetch(inFields, ivec3(coord0, inLayer), 0),
                        texelFetch(inFields, ivec3(coord1, inLayer), 0), inverseTwiddle);
    vec4 v1 = butterfly(texelFetch(inFields, ivec3(coord0, inLayer + 1), 0),
                        texelFetch(inFields, ivec3(coord1, inLayer + 1), 0), inverseTwiddle);

    if (finalize == 1) {
        // (-1)^(x+y) is the spatial-domain equivalent of shifting the spectrum's
        // origin from the corner to the centre, which is where the seeding put it.
        // Folded into this stage rather than a fourth pass per cascade.
        float checker = 1.0 - 2.0 * float((id.x + id.y) % 2);
        v0 *= checker;
        v1 *= checker;
    }

    Out0 = v0;
    Out1 = v1;
}
