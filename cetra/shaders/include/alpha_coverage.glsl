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
//
// A cutoff of ZERO takes the binary branch, and that is the file's own claim
// being honoured rather than a special case bolted on: glTF says a fragment is
// opaque where `alpha >= cutoff`, so at cutoff 0 EVERY fragment is opaque and
// `step` says so. The ramp cannot -- a fully transparent texel sits exactly on
// the threshold there, and a ramp centred on the threshold returns 0.5, which
// would make a material that discards nothing half-cover its whole surface.
float alphaMaskCoverage(float a, float cutoff, int a2c) {
    if (a2c == 0 || cutoff <= 0.0)
        return step(cutoff, a);
    return clamp((a - cutoff) / max(fwidth(a), 1e-4) + 0.5, 0.0, 1.0);
}
