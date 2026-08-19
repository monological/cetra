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

uniform sampler2D histTex; // binCount x rowCount, RG32F: (count, sum of log2)
uniform int binCount;
uniform int rowCount; // rows the bin pass split the source across
uniform vec2 percentiles; // fraction of the population to keep, low and high edge

// A bin's true (count, sum): the bin pass splits the source across rowCount
// rows, and both accumulators are associative, so a bin is the sum down its
// column.
//
// Re-fetched rather than cached in an array. An array would have to be sized by
// a constant while binCount is a uniform, which is exactly the C-to-GLSL size
// agreement this pipeline deleted when it stopped reading a named mip level --
// and the cost of refetching is 512 texels in a ONE-fragment pass.
vec2 binColumn(int bin) {
    vec2 acc = vec2(0.0);
    for (int r = 0; r < rowCount; r++)
        acc += texelFetch(histTex, ivec2(bin, r), 0).rg;
    return acc;
}

void main() {
    // One walk, both accumulators. The fallback at the bottom needs the total
    // sum, and taking it here rather than re-walking costs nothing.
    float total = 0.0;
    float totalSum = 0.0;
    for (int i = 0; i < binCount; i++) {
        vec2 b = binColumn(i);
        total += b.x;
        totalSum += b.y;
    }

    if (total <= 0.0) {
        // No samples at all -- reachable, since a spot smaller than one source
        // texel excludes every one of them. Emitting 0 would be log2(1 nit), a
        // real reading and a wrong one, so this hands the CPU something its
        // isfinite check refuses.
        //
        // Constructed rather than divided. `0.0 / 0.0` is UNSPECIFIED in GLSL,
        // not guaranteed NaN -- and it is exactly the expression a compiler is
        // most likely to fold, to 0.0, which is the outcome this is avoiding.
        FragColor = vec4(uintBitsToFloat(0x7FC00000u), 0.0, 0.0, 1.0);
        return;
    }

    float lo = total * clamp(percentiles.x, 0.0, 1.0);
    float hi = total * clamp(percentiles.y, 0.0, 1.0);

    float walked = 0.0;
    float keptSum = 0.0;
    float keptCount = 0.0;
    for (int i = 0; i < binCount; i++) {
        vec2 b = binColumn(i);
        float c = b.x;
        float binLo = walked;
        walked += c;
        // An empty bin needs no guard of its own: c == 0 means walked == binLo,
        // so `take` below is <= 0 in every ordering and the next line catches it
        // before the divide.
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
        FragColor = vec4(totalSum / total, 0.0, 0.0, 1.0);
        return;
    }
    FragColor = vec4(keptSum / keptCount, 0.0, 0.0, 1.0);
}
