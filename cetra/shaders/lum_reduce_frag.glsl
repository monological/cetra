#version 330 core
out vec4 FragColor;

// Auto-exposure step 3: collapse the histogram to the one log2 luminance the
// CPU reads back, discarding a fraction of the population at each end.
//
// Percentiles are taken over POPULATION, not over the value range: "ignore the
// darkest 20% and the brightest 2% of pixels" is a statement about the scene,
// which is why it survives the scene being scaled. An absolute threshold does
// not -- the metering floor this replaces is exactly that, and it costs 1.61
// stops of scale-covariance on a dim scene (spec 11.52 P0).
//
// The tails are cut by ACCUMULATED COUNT, so a bin at the boundary is split
// rather than taken or dropped whole. That is what keeps the answer continuous
// as the distribution slides across bin edges: x1000 is ~9.97 stops, not a
// whole number of bins, so two scaled copies of one scene do not land on the
// same offsets and a bin-granular cut would report them differently.

uniform sampler2D histTex; // binCount x 1, RG32F: (count, sum of log2 values)
uniform int binCount;
uniform vec2 percentiles; // fraction of the population to keep, low and high edge

void main() {
    float total = 0.0;
    for (int i = 0; i < binCount; i++)
        total += texelFetch(histTex, ivec2(i, 0), 0).r;

    if (total <= 0.0) {
        // No samples at all. Emitting 0 would be log2(1 nit) -- a real reading,
        // and a wrong one. exposure_submit_measurement refuses non-finite input,
        // so this hands it something it will decline rather than latch.
        FragColor = vec4(0.0 / 0.0, 0.0, 0.0, 1.0);
        return;
    }

    float lo = total * clamp(percentiles.x, 0.0, 1.0);
    float hi = total * clamp(percentiles.y, 0.0, 1.0);

    float walked = 0.0;
    float keptSum = 0.0;
    float keptCount = 0.0;
    for (int i = 0; i < binCount; i++) {
        vec2 b = texelFetch(histTex, ivec2(i, 0), 0).rg;
        float c = b.x;
        float binLo = walked;
        walked += c;
        if (c <= 0.0)
            continue;
        float take = min(walked, hi) - max(binLo, lo);
        if (take <= 0.0)
            continue;
        // The bin's own mean, weighted by how much of it survives. A whole bin
        // contributes its exact sum; a split bin contributes its mean times the
        // surviving fraction, which is the only assumption in the pass and is
        // confined to the two bins the cuts fall in.
        keptSum += (b.y / c) * take;
        keptCount += take;
    }

    // Both percentiles cutting everything away is authored, not exceptional --
    // fall back to the whole population rather than to a NaN, since a user who
    // sets low >= high wants a mean, not a black frame.
    if (keptCount <= 0.0) {
        float s = 0.0;
        for (int i = 0; i < binCount; i++)
            s += texelFetch(histTex, ivec2(i, 0), 0).g;
        FragColor = vec4(s / total, 0.0, 0.0, 1.0);
        return;
    }
    FragColor = vec4(keptSum / keptCount, 0.0, 0.0, 1.0);
}
