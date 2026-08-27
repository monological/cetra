// Coverage from a masked material's alpha (spec 11.87).
//
// A silhouette is a property of the MATERIAL, not of the framebuffer. Both
// branches below cross at `a == cutoff`, so the shape an author asked for is
// the shape that renders at one sample and at four -- by construction, rather
// than by two constants being kept in agreement by hand.
//
// That was not true before this file existed: the A2C path compared against a
// fixed 0.02 instead of the authored cutoff, so a masked material grew a fringe
// under MSAA made of the whole alpha falloff between the two. On backlit
// foliage it read as a bright outline; on the shadow map, which is
// single-sampled and always used the authored value, it meant a leaf and its
// own shadow were different shapes.
//
// THE 0.02 WAS NOT WRONG, ITS SPACE WAS. Discarding a fragment that covers 2%
// of a pixel is right and this file's caller still does it. Thresholding raw
// alpha at 0.02 renders everything above it at full size. water_frag.glsl has
// kept the number in the correct space since 11.33 and is where the shape of
// this function comes from.
//
// DERIVATIVES. `fwidth` is undefined in fragment-varying control flow (GLSL
// 3.30), so a caller must reach this from dynamically uniform flow. Every
// branch enclosing pbr_frag's alpha fetch is on a `uniform int`, which is what
// makes it legal there. The one weaker spot is a masked material that also
// carries a height map: POM's silhouette clip discards per fragment upstream.
// No such material exists in this tree -- the only height map is on opaque bark
// -- and desktop GL keeps helper invocations alive across `discard` anyway, but
// it is the assumption to check before authoring one.

// Fractional sample coverage for an alpha-tested fragment.
//
// `a2c` is whether alpha-to-coverage is live for this draw, which is the sample
// count in disguise: with one sample there is nothing to dither into, so the
// answer must be binary or a half-covered fragment writes itself at full
// strength.
//
// The a2c branch measures the distance to the threshold in PIXELS -- dividing
// by how fast alpha moves per pixel is what converts it -- so the transition is
// one pixel wide whatever the texture's own falloff looks like, and a
// half-covered pixel reads 0.5 at any resolution or camera angle.
// Above this rate of change per pixel the alpha is ALIASING rather than
// resolving, and the remap below stops meaning anything (spec 11.87).
//
// It divides by the per-pixel rate to express distance-to-threshold in pixels,
// which assumes alpha is locally linear across the pixel. On a minified atlas it
// is not: a leaf whose whole soft edge falls inside one texel footprint reports
// a rate near 1, and then even a texel deep inside the leaf -- alpha 0.9 against
// a 0.4 cutoff -- comes back at 0.5 + 0.5/1.0, which clamps to 1 only just, and
// a slightly higher rate leaves the leaf's INTERIOR partially covered. Whatever
// is behind then shows through a surface that should be solid.
//
// Measured on apps/tree's canopy, against a 4x-supersampled ground truth: a
// pixel that should read luma 20 read 110 without this, and 24 with it.
const float ALPHA_ALIASED_RATE = 0.5;

float alphaMaskCoverage(float a, float cutoff, int a2c) {
    if (a2c == 0)
        return step(cutoff, a);
    // Rate first, because the fallback needs it and a derivative must be taken
    // before any branch that could diverge on it.
    float rate = fwidth(a);
    if (rate > ALPHA_ALIASED_RATE)
        return step(cutoff, a);
    return clamp((a - cutoff) / max(rate, 1e-4) + 0.5, 0.0, 1.0);
}
