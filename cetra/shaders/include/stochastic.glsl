/*
 * By-example stochastic texturing, the sampling half (Heitz & Neyret 2018).
 *
 * procedural/stochastic_tex.h carries the argument for why this exists and what the bake does.
 * What happens here: skew the UV onto a triangle lattice, take the three vertices of the
 * triangle it lands in, sample the texture once per vertex at a random offset derived from
 * that vertex, and blend the three with barycentric weights.
 *
 * Because each lattice vertex has its own offset, neighbouring points on the ground read
 * different parts of the tile, and the texture's period stops being visible. Because the blend
 * is variance-preserving and the sampled texture was transformed to a Gaussian, the result
 * carries the ORIGINAL texture's histogram rather than a washed-out average of it.
 *
 * THE FOOTPRINT IS PASSED IN, and that is not optional. The per-vertex offsets are piecewise
 * constant, so the offset UVs step from one lattice cell to the next; an implicit lookup takes
 * its mip from those derivatives and picks a different level either side of every cell
 * boundary, printing the lattice as a mosaic of differently blurred patches. The offsets are
 * translations, and a translation does not change a footprint, so the gradients are the
 * undisplaced ones.
 */

// Must match STOCHASTIC_LUT_SIZE and STOCHASTIC_SIGMA_SPAN in procedural/stochastic_tex.h.
// The span is what maps the stored [0,1] back to sigma; disagreeing with the bake puts every
// value on the wrong part of the inverse table.
#define STOCHASTIC_LUT_SIZE 64
const float STOCHASTIC_SIGMA_SPAN = 3.0;

/*
 * The inverse CDF, three channels to a row.
 *
 * A UNIFORM ARRAY rather than a texture, which is what lets this run in a program that has
 * declared all sixteen samplers the driver allows: 64 vec3 is 768 bytes, and uniform space is
 * the escape the clustered-light and shore-film blocks already established for exactly this.
 */
uniform vec3 stochasticLut[STOCHASTIC_LUT_SIZE];
// Lattice cell size in UV. Smaller decorrelates harder, at the cost of blending over a shorter
// distance -- too small and the blend itself becomes the visible texture.
uniform float stochasticScale;

vec2 _stochasticHash(vec2 p) {
    // Only has to spread offsets over the tile; a collision here shows up as two lattice cells
    // sampling the same place, which reads as a patch of ordinary tiling.
    return fract(sin(vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)))) * 43758.5453);
}

/*
 * The triangle lattice.
 *
 * Triangles rather than squares because a point needs only three samples instead of four, and
 * the three barycentric weights sum to one by construction. The skew turns the equilateral
 * lattice into an axis-aligned one, so the cell and the weights fall out of floor() and
 * fract() with no trigonometry.
 */
void _stochasticLattice(vec2 uv, out vec3 w, out vec2 v1, out vec2 v2, out vec2 v3) {
    const mat2 toSkewed = mat2(1.0, 0.0, -0.57735027, 1.15470054);
    vec2 skewed = toSkewed * (uv * 3.464); // 2 * sqrt(3): unit cell in the skewed frame
    vec2 base = floor(skewed);
    vec3 t = vec3(fract(skewed), 0.0);
    t.z = 1.0 - t.x - t.y;
    // The sign of t.z says which of the cell's two triangles the point fell in.
    if (t.z > 0.0) {
        w = vec3(t.z, t.y, t.x);
        v1 = base;
        v2 = base + vec2(0.0, 1.0);
        v3 = base + vec2(1.0, 0.0);
    } else {
        w = vec3(-t.z, 1.0 - t.y, 1.0 - t.x);
        v1 = base + vec2(1.0, 1.0);
        v2 = base + vec2(1.0, 0.0);
        v3 = base + vec2(0.0, 1.0);
    }
}

// One channel back through the inverse table. Linearly interpolated: the table is an inverse
// CDF and therefore smooth, so this interpolates between neighbouring points on a curve.
float _stochasticInverse(float g, int channel) {
    // The blended value is Gaussian in the stored [0,1] encoding. Back to sigma, then through
    // the Gaussian CDF to get the position in the distribution the table is indexed by.
    float sigma = (g - 0.5) * 2.0 * STOCHASTIC_SIGMA_SPAN;
    // Logistic approximation to the normal CDF. Its worst error is about 0.01 in probability,
    // which lands inside one entry of a 64-wide table, and it costs one exp against an erf.
    float cdf = 1.0 / (1.0 + exp(-1.702 * sigma));
    float x = clamp(cdf * float(STOCHASTIC_LUT_SIZE) - 0.5, 0.0, float(STOCHASTIC_LUT_SIZE - 1));
    int i0 = int(floor(x));
    int i1 = min(i0 + 1, STOCHASTIC_LUT_SIZE - 1);
    return mix(stochasticLut[i0][channel], stochasticLut[i1][channel], x - float(i0));
}

/*
 * Sample a Gaussianised texture stochastically. `footprint` is the undisplaced UV, whose
 * derivatives select the mip -- see the note at the top.
 *
 * Returns the value in the ORIGINAL texture's distribution, so a caller uses it exactly where
 * it used a plain texture() result.
 */
vec3 stochasticSample(sampler2D tex, vec2 uv, vec2 footprint) {
    vec3 w;
    vec2 v1, v2, v3;
    _stochasticLattice(uv / max(stochasticScale, 1.0e-4), w, v1, v2, v3);

    vec2 dx = dFdx(footprint);
    vec2 dy = dFdy(footprint);

    vec3 g1 = textureGrad(tex, uv + _stochasticHash(v1), dx, dy).rgb;
    vec3 g2 = textureGrad(tex, uv + _stochasticHash(v2), dx, dy).rgb;
    vec3 g3 = textureGrad(tex, uv + _stochasticHash(v3), dx, dy).rgb;

    /*
     * The variance-preserving blend, and the one line the whole method turns on.
     *
     * Dividing by the sum of the weights would be an average, and averaging independent
     * samples of a field divides its variance -- contrast collapses and the ground goes flat
     * and muddy, which is what a naive three-tap blend looks like. Dividing by the square root
     * of the sum of SQUARED weights keeps the variance of a single sample, so the blend has
     * the contrast of the texture it came from. Exact for a Gaussian input, which is what the
     * bake's transform guarantees.
     */
    vec3 g = vec3(0.5) + (w.x * (g1 - 0.5) + w.y * (g2 - 0.5) + w.z * (g3 - 0.5)) /
                             max(sqrt(dot(w, w)), 1.0e-6);

    return vec3(_stochasticInverse(g.r, 0), _stochasticInverse(g.g, 1),
                _stochasticInverse(g.b, 2));
}
