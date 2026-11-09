"""The alphacov group: mip alpha coverage (a cutout keeps its area with distance, spec 11.87)."""

import os

from gatelib.harness import _import_fixture_gen, asset, render
from gatelib.images import _alphacov_mean, _read_ppm


# Mip alpha coverage preservation (spec 11.87), on assets/alphacov_fixture.
#
# A receding plane wearing a grid of small opaque dots at cutoff 0.5. The dots
# are the point: a soft edge one texel wide is SYMMETRIC about the threshold, so
# box-filtering moves as many texels up as down and the surviving fraction holds
# -- which is why neither leaf atlas drifts (0.3957 at level 0, 0.3964 six levels
# down) and why neither could test this. An isolated dot has no such symmetry;
# its coverage falls 0.113 -> 0.125 -> 0.062 -> 0.000 over four halvings.
ALPHACOV_FIXTURE = "alphacov_fixture.cscn"
# A dot is far brighter than the ground it is cut out of, so a luma threshold
# separates them without needing to know where any individual dot landed.
ALPHACOV_LUMA = 90.0
# What fraction of the near field's coverage the mid band must still carry.
# Measured 0.911 as 11.88 built it, 0.619 with the per-texel measure restored,
# and 0.591 with no preservation at all -- so the bar sat 0.081 above the
# nearest falsifying mutation. Re-measured under 11.100: 0.904 as built (level
# 3's rescale became a dither) and 0.622 with the rescale broken, so the
# margin is 0.078 and the numbers above are the pre-distribution record.
ALPHACOV_MIN_RATIO = 0.70
# And what the FAR band may NOT carry. It is bounded the opposite way from the
# ratio above and that is the whole reason it exists: since 11.100 these levels
# are DISTRIBUTED too, but their visibility budgets round to 2, 1 and 0 ON
# texels, so near-empty is still what correct looks like out here and the band
# guards SATURATION -- an inverted budget reads 1.0000, and so did 11.87's
# texel-counting cascade (the pre-distribution record: 1.0000 against 0.0000
# either side of it in the frame).
ALPHACOV_FAR_MAX = 0.25
# The DEEP signal (spec 11.100): the facing quad's mean luma above the sky
# background, in 8-bit codes. Past the uniform level, where no scale can
# reproduce a fractional coverage, alpha DISTRIBUTION keeps the quad alive at
# roughly the level-0 coverage instead of letting it vanish. MEAN luma and not
# a bright-pixel count, and on the quad rather than the plane -- both by
# measurement; run_alphacov_gate's docstring carries the grazing story.
# Two-sided because it is a fidelity band: the floor fails a build that still
# vanishes -- measured +3.21, edge taps of the rescaled level 3, not the ~0
# the backdrop argument predicts -- and the ceiling fails one whose visibility
# budget saturates, measured +180.25 under the inverted-budget mutation (past
# the a-priori "full dot-minus-sky lift" estimate because the mutation also
# saturates the plane and bloom rides the whole frame). The distributing build
# reads +20.97, and 20.97/180.25 = 0.116 of the saturated lift against the
# 0.113 target: the floor sits 4.8 above the vanishing reading and 13 under
# the true one; the ceiling sits 120 under the saturating one.
ALPHACOV_DEEP_MIN = 8.0
ALPHACOV_DEEP_MAX = 60.0


def _alphacov_frac(pix, w, h, band):
    """Fraction of a horizontal band bright enough to be a surviving dot."""
    y0, y1 = int(h * band[0]), int(h * band[1])
    n = k = 0
    for y in range(y0, y1):
        for x in range(w):
            o = (y * w + x) * 3
            if 0.2126 * pix[o] + 0.7152 * pix[o + 1] + 0.0722 * pix[o + 2] > ALPHACOV_LUMA:
                k += 1
            n += 1
    return k / n if n else 0.0


def run_alphacov_gate(workdir):
    """An alpha-tested cutout keeps its coverage down the mip chain (spec 11.87).

      alphacov-mip   the mid field still carries most of the near field's dot
                     coverage. Box-filtering an isolated dot drags its alpha
                     toward the background rather than toward the mean of an
                     edge, so an unpreserved chain does not thin the grid, it
                     DELETES it -- this fixture's coverage runs 0.113 at level 0
                     and 0.000 by level 3. The per-level rescale is what holds
                     it, and the arm reads the ratio between two bands of ONE
                     frame so it needs no stored image and no second build.

      alphacov-deep  the FACING QUAD is alive, and not solid (spec 11.100).
                     It hangs in the sky at mip ~4.9, past the level where the
                     chain goes uniform and no scale can reproduce a
                     fractional coverage -- alpha distribution is what answers
                     there, so its mean-luma signal over the sky background is
                     bounded on BOTH sides: the floor fails a build that still
                     vanishes (the quad discards to backdrop), the ceiling
                     fails one whose visibility budget saturates. Read as a
                     MEAN, not a bright-pixel count, and on FACING geometry,
                     not the plane -- both by measurement: on the grazing
                     plane, anisotropic filtering averages the sparse dither
                     into a smooth low-alpha field the sharpened test then
                     deletes, so the distributing build counted LOWER there
                     (0.0027) than the vanishing one (0.0114, rescaled-cluster
                     bleed) and no floor separates them in the honest
                     direction. That grazing loss is a real ceiling of the
                     technique, recorded in spec 11.100; the quad is Yuksel's
                     own experimental geometry.

      alphacov-far   and the far field is not SOLID. Bounded the opposite way
                     from the ratio, which is the point of it.

    WHY A DOT GRID AND NOT FOLIAGE. Every alpha-tested asset in this corpus is a
    leaf atlas with a soft edge about a texel wide, and a soft edge is symmetric
    about the threshold: filtering moves as many texels above it as below, so the
    surviving fraction barely moves and preservation is inert. Measured on
    apps/tree's atlas, 0.3957 at level 0 against 0.3964 six levels down. An arm
    written on that content would pass whether the feature worked or not.

    WHY TWO ARMS POINTING OPPOSITE WAYS, and this is spec 11.88 paying for spec
    11.87. A ratio bounded only BELOW cannot fail on over-preservation: a chain
    that drives the far field solid reads mid HIGHER than near, so the ratio goes
    up and the arm passes harder the worse the bug gets. 11.87 shipped exactly
    that -- cascading let the per-level 4x cap compound to an effective 4.41,
    which lifted a uniform level over the cutoff and painted a solid white band
    across the distance -- and this gate was green for it. Neither arm is safe
    alone: alphacov-mip alone passes on a build that saturates, and alphacov-far
    alone passes on one that preserves nothing, since an empty far field is what
    correct looks like out there.

    WHAT NONE OF THEM CLAIM. The old ceiling -- past the uniform level no scale
    reproduces a fraction -- is ANSWERED since 11.100 by distribution, and the
    deep arm is its instrument. What stays unclaimed: LEVEL 0's pristineness
    (the dot grid is binary at level 0, so this fixture is structurally blind
    to a build that rewrites it -- the 11.100 ledger proved the mutation live
    at the artefact-byte level and invisible to every frame instrument in the
    corpus, and records that hole rather than papering it); the GRAZING loss
    (at ~15 degrees under anisotropic filtering the sampler averages the dither
    into a smooth low-alpha field the sharpened test deletes -- a real ceiling
    of the technique, measured in spec 11.100, which is why the deep arm reads
    the facing quad); and the tail's starvation, where budgets of 2, 1 and 0
    ON texels make near-empty correct.

    AND WHAT THEY DO NOT COVER, which spec 11.88 measured rather than assumed.
    The rescale has three parts and the mip arm sees ONE of them, the coverage
    measure: reverting it alone dropped the ratio to 0.619 on 11.88's build, a
    hair above that build's unpreserved 0.591. Saturation needs the texel count
    AND the cascade together, so fixing either quiets the far band, and NOTHING
    here covers the pristine chain or the best-error scale -- both
    correctness-by-reference (docs/papers/README.md), both defensive. The
    DISTRIBUTION's parts fare better: the deep arm sees the feature, its
    normalization and its level gating, and cook-determinism sees its
    byte-determinism (a per-process value folded into the arithmetic reads one
    artefact of 59 differing). Specs 11.88 and 11.100 carry the full matrices
    -- not repeated here, where gate-arm-docs reads a table's first column as
    arm names.
    """
    # One tuple names the group's arms for every skip and error path, so a
    # fourth arm cannot be remembered in one path and vanish from another.
    arms = ("alphacov-mip", "alphacov-deep", "alphacov-far")
    scene = asset(ALPHACOV_FIXTURE)
    if not os.path.exists(scene):
        for arm in arms:
            print(f"  {arm} SKIP  ({ALPHACOV_FIXTURE} not present)")
        return []
    # The bands come from the generator rather than being restated here: where a
    # band lands is a fact about that geometry and the camera it ships with, and
    # the generator asserts each against them. A copy here could only agree by
    # hand. _import_fixture_gen prints its own SKIP under the first arm's name;
    # name the rest too, or they vanish without a line.
    gen = _alphacov_gen()
    if gen is None:
        for arm in arms[1:]:
            print(f"  {arm}     SKIP  (gen_alphacov_fixture unavailable)")
        return []

    out = os.path.join(workdir, "alphacov.ppm")
    err = render(scene, out, [])
    if err:
        print(f"  alphacov-mip ERROR render failed: {err.strip()[-200:]}")
        return list(arms)

    w, h, pix = _read_ppm(out)
    near = _alphacov_frac(pix, w, h, gen.BAND_NEAR)
    mid = _alphacov_frac(pix, w, h, gen.BAND_MID)
    far = _alphacov_frac(pix, w, h, gen.BAND_FAR)
    sky = _alphacov_mean(pix, w, h, (0.0, gen.BAND_SKY[0], 1.0, gen.BAND_SKY[1]))
    deep = _alphacov_mean(pix, w, h, gen.BOX_DEEP) - sky
    ratio = mid / near if near > 0 else 0.0
    # The near band has to actually carry dots, or a frame that rendered nothing
    # satisfies a ratio of 0/0 and every reading below is meaningless. Shared by
    # every arm: an empty frame would otherwise SATISFY the far band's ceiling.
    lit = near > 0.05

    failures = []
    ok = lit and ratio >= ALPHACOV_MIN_RATIO
    print(f"  alphacov-mip {'PASS' if ok else 'FAIL'}  dot coverage {near:.4f} near, "
          f"{mid:.4f} mid, ratio {ratio:.3f} (want >= {ALPHACOV_MIN_RATIO}, and the near band "
          f"lit above 0.05: {lit}). Without the per-level rescale this reads 0.622 -- it read "
          f"0.591 before 11.100, whose level-3 dither now cushions the mid band a little.")
    if not ok:
        failures.append("alphacov-mip")

    ok = lit and ALPHACOV_DEEP_MIN <= deep <= ALPHACOV_DEEP_MAX
    print(f"  alphacov-deep {'PASS' if ok else 'FAIL'}  the facing quad reads {deep:+.2f} luma "
          f"over the sky's {sky:.2f} (want {ALPHACOV_DEEP_MIN} <= signal <= {ALPHACOV_DEEP_MAX}, "
          f"and the near band lit: {lit}). The quad sits at mip ~4.9, past the uniform level, "
          f"where a scale cannot reproduce a fractional coverage -- distribution keeps it alive "
          f"at roughly the level-0 coverage; a vanishing build discards it to backdrop, a "
          f"saturating one lifts the box by the full dot-minus-sky difference.")
    if not ok:
        failures.append("alphacov-deep")

    ok = lit and far <= ALPHACOV_FAR_MAX
    print(f"  alphacov-far {'PASS' if ok else 'FAIL'}  the far field reads {far:.4f} "
          f"(want <= {ALPHACOV_FAR_MAX}, and the near band lit: {lit}). Since 11.100 these "
          f"levels are distributed too, but their visibility budgets are 2, 1 and 0 ON texels, "
          f"so near-empty is still what correct looks like out here -- the band guards "
          f"SATURATION: the inverted-budget mutation reads 1.0000, and so did 11.87's "
          f"texel-counting cascade.")
    if not ok:
        failures.append("alphacov-far")

    return failures


_ALPHACOV_GEN = None


def _alphacov_gen():
    global _ALPHACOV_GEN
    if _ALPHACOV_GEN is None:
        _ALPHACOV_GEN = _import_fixture_gen("gen_alphacov_fixture.py", "alphacov-mip")
    return _ALPHACOV_GEN
