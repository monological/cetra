#version 330 core
out vec4 FragColor;

// Auto-exposure step 2: bin the measure target's log2 luminances.
//
// A GATHER histogram, one fragment per bin, each looping the whole source and
// counting what lands in it. That is O(bins * texels) where a scatter would be
// O(texels) -- and it is the right trade here because the source is 64x64. 64
// bins x 4096 texels is 262144 fetches, which is less work than one 512x512
// fullscreen pass. GL 4.1 has no compute, no atomics and no imageStore, so the
// alternative is additive blending of point primitives, which needs a vertex
// stream, a blend state and a float-blendable target to buy nothing at this
// size.
//
// Writes COUNT and SUM, not count alone. The sum is what lets the reduce pass
// recover the exact mean of the surviving samples rather than approximating it
// from bin centres -- so with no percentile clipping the answer is identical to
// the mip-chain average this replaced, and with clipping it is exact for every
// whole bin and interpolated only across the two partial ones at the ends.

uniform sampler2D lumTex; // LUM_MEASURE_SIZE^2, R16F, log2 luminance
uniform int srcSize;      // edge of lumTex in texels
uniform int binCount;
uniform vec2 binRange;   // log2 luminance at bin 0's floor and the last bin's ceiling
uniform int meterMode;   // MeteringMode (exposure.h)
uniform float meterRadius; // spot / falloff radius, fraction of the half-diagonal

// How much this texel's vote is worth. Weighting the POPULATION and not the
// value is deliberate: a mode has to be a framing choice, so weighting a flat
// frame any way at all must still meter the same luminance. Scaling the value
// instead would make every mode change an exposure offset as well.
float meterWeight(vec2 uv) {
    if (meterMode == 0) // METERING_UNIFORM
        return 1.0;
    // Normalised distance from centre, in UV. On a non-square frame the disc is
    // an ellipse in pixels, which is the convention a mask texture carries too.
    float r = length(uv - vec2(0.5)) / max(meterRadius, 1e-4);
    if (meterMode == 2) // METERING_SPOT: a hard edge, so nothing outside votes
        return r <= 1.0 ? 1.0 : 0.0;
    // METERING_CENTRE: full weight inside the radius, easing to a floor rather
    // than to zero. A zero would make the mode a spot with a soft edge; the
    // point of centre-weighting is that the surround still has a say.
    return mix(1.0, 0.1, smoothstep(1.0, 2.0, r));
}

void main() {
    int bin = int(gl_FragCoord.x);
    float lo = binRange.x;
    float span = max(binRange.y - binRange.x, 1e-6);

    float count = 0.0;
    float sum = 0.0;
    float invSize = 1.0 / float(srcSize);
    for (int y = 0; y < srcSize; y++) {
        for (int x = 0; x < srcSize; x++) {
            float w = meterWeight((vec2(x, y) + 0.5) * invSize);
            if (w <= 0.0)
                continue;
            float v = texelFetch(lumTex, ivec2(x, y), 0).r;
            // Out-of-range samples land in the end bins rather than being
            // dropped. They still contribute their TRUE value to the sum, so a
            // scene outside the authored range meters correctly and only its
            // percentile position is approximate -- the failure mode that costs
            // least, and the one that cannot silently lose energy.
            int b = int(floor(clamp((v - lo) / span, 0.0, 0.999999) * float(binCount)));
            if (b == bin) {
                count += w;
                sum += v * w;
            }
        }
    }
    FragColor = vec4(count, sum, 0.0, 1.0);
}
