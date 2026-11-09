"""The mask group: alpha mask (binary above the cutoff, spec 11.31)."""

import os

from gatelib.harness import asset, render
from gatelib.images import _box_luma, _read_ppm, compare
from gatelib.scenes import MASK_FIXTURE, MASK_LIT_FLOOR, MASK_REF_BOX


MASK_KEPT_BOX = (0.729, 0.42, 0.834, 0.58)
MASK_GONE_BOX = (0.448, 0.42, 0.553, 0.58)
# And the discarded quad has to be shown to be ABSENT. Nearer the backdrop than
# to a lit surface, with room for the vignette and the tonemap toe.
MASK_GONE_CEIL = 0.10
# How far the discarded quad may move BETWEEN sample counts, as a ratio (spec
# 11.87). This is the arm's real bar, because the absolute ceiling above cannot
# be it: that one was sized to separate backdrop from a lit quad at 0.47, and a
# cutoff silently replaced by 0.02 only lifts the box to 0.0915 -- under the
# ceiling, over twice the 1-sample reading. A silhouette is a property of the
# material, so the two counts have to agree, not merely both look dark.
MASK_SAMPLE_RATIO_MAX = 1.25


def run_mask_gate(workdir):
    """ALPHA_MASK is binary above the cutoff, and absent below it (spec 11.31).

    glTF's rule for MASK is that a fragment at or above alphaCutoff renders
    fully opaque and one below is discarded. The opaque lane used to inherit
    global SRC_ALPHA blending, so a masked fragment at alpha 0.6 landed at 0.6
    of itself over the CLEAR COLOUR -- the pass runs before the skybox -- and
    nothing in the corpus could see it. Every golden's masked geometry is either
    absent or, in translucent_shadow's case, a caster held above the frame, so
    all 21 goldens pass either way.

    Three quads in ONE frame rather than three renders: same light, same normal,
    same base colour, differing only in the COLOR_0 alpha they carry. No stored
    image and no cross-run comparison to hold still.

    BOTH arms are needed and neither implies the other. Without the cutoff arm a
    regression that stopped discarding renders every masked fragment, still
    opaque, and passes -- which is the "masked geometry would render as solid
    quads" failure render.c names.

    --taa is what drops the engine to one sample, and the first two arms take
    that path deliberately: under alpha-to-coverage a fractional alpha becomes
    fractional sample coverage, so the KEPT quad legitimately differs from the
    opaque reference at 4x and the equality above does not hold there.
    Exposure and tonemap are pinned by the fixture's own .cscn.

    That legitimacy claim used to be written wider than it is true, and the
    width is what made spec 11.87's defect unreachable. A coverage dither at an
    EDGE is legitimate. A quad whose every texel sits below the authored cutoff
    appearing at all is not -- and it did, because the cutoff was replaced by a
    fixed 0.02 whenever A2C was live. mask-samples below is the arm that asks.
    """
    scene = asset(MASK_FIXTURE)
    if not os.path.exists(scene):
        print(f"  mask-opaque  SKIP  ({MASK_FIXTURE} not present)")
        return []

    out = os.path.join(workdir, "mask_binary.ppm")
    err = render(scene, out, ["--taa", "--headless-jitter"])
    if err:
        print(f"  mask-opaque  ERROR render failed: {err.strip()[-200:]}")
        return ["mask-opaque"]

    failures = []
    w, h, pix = _read_ppm(out)
    ref = _box_luma(pix, w, h, MASK_REF_BOX)
    kept = _box_luma(pix, w, h, MASK_KEPT_BOX)
    gone = _box_luma(pix, w, h, MASK_GONE_BOX)

    lit = ref >= MASK_LIT_FLOOR and kept >= MASK_LIT_FLOOR
    # Relative, not absolute: the quantity is "these are the same surface", and
    # the exposure and tonemap are free to move what that surface reads as.
    delta = abs(ref - kept) / ref if ref > 0 else 1.0
    ok = lit and delta <= 0.02
    print(f"  mask-opaque  {'PASS' if ok else 'FAIL'}  masked quad {kept:.4f} vs opaque "
          f"reference {ref:.4f} linear luma, {delta * 100.0:.2f}% apart (want <= 2%, and both "
          f"lit above {MASK_LIT_FLOOR}: {lit}). Blending the masked lane reads ~36% low.")
    if not ok:
        failures.append("mask-opaque")

    ok = gone <= MASK_GONE_CEIL
    print(f"  mask-cutoff  {'PASS' if ok else 'FAIL'}  below-cutoff quad reads {gone:.4f} "
          f"(want <= {MASK_GONE_CEIL}, i.e. backdrop; a lit one reads {kept:.4f})")
    if not ok:
        failures.append("mask-cutoff")

    # --- mask-prepass: the A2C depth-only exit, which nothing else reaches ---
    # NO --taa here, and that is the whole point: TAA drops the engine to one
    # sample, which is the path the two arms above take and the path on which
    # alpha-to-coverage does not run at all. At 4x MSAA the masked prepass goes
    # through pbr_frag's second depth-only exit and has to produce the same
    # sample mask as the shading pass, or GL_LEQUAL deletes the samples they
    # disagree on.
    #
    # The fixture's BACKDROP is what makes this falsifiable, and it was added
    # because the arm could not fail without it. A prepass sample mask that
    # covers more than the shading pass writes depth where nothing shades, and
    # the symptom is the surface behind being occluded -- with nothing behind,
    # any disagreement is invisible. Measured while proving that: skipping the
    # lighting-environment binds for depth-only draws reads 0 px on a fixture
    # with no backdrop and 62,009 px on this one.
    pre_off = os.path.join(workdir, "mask_pre_off.ppm")
    pre_on = os.path.join(workdir, "mask_pre_on.ppm")
    err = render(scene, pre_off, []) or render(scene, pre_on, ["--depth-prepass"])
    if err:
        print(f"  mask-prepass ERROR render failed: {err.strip()[-200:]}")
        return failures + ["mask-prepass"]

    ae, _ = compare(pre_off, pre_on)
    ok = ae == 0
    print(f"  mask-prepass {'PASS' if ok else 'FAIL'}  {ae} px between prepass on and off at "
          f"4x MSAA (want exactly 0: the prepass and the shading pass must derive the same "
          f"alpha-to-coverage sample mask)")
    if not ok:
        failures.append("mask-prepass")

    # --- mask-samples: the silhouette must not depend on the sample count -----
    #
    # The three arms above cannot see this and none of them is at fault. Two run
    # at one sample, where the A2C branch is not taken at all; the third is a
    # self-comparison at 4x, so a cutoff that is wrong in BOTH of its renders
    # still diffs to zero. Nothing in the corpus rendered visible MASK geometry
    # at two sample counts and compared them.
    #
    # --msaa rather than --taa for the 1-sample leg: --taa drops the sample
    # count AND enables the temporal filter AND (with --headless-jitter) adds
    # jitter, which is three changes where the question needs one. apps/render
    # applies --msaa after the TAA policy for exactly this reason.
    #
    # The liveness guard is not decoration. "The quad is absent" is satisfied
    # perfectly by a frame where nothing loaded, so the reference and kept boxes
    # have to be lit at BOTH counts or the reading proves nothing.
    reads = {}
    for samples in (1, 4):
        shot = os.path.join(workdir, f"mask_s{samples}.ppm")
        err = render(scene, shot, ["--msaa", str(samples)])
        if err:
            print(f"  mask-samples ERROR render failed at {samples}x: {err.strip()[-200:]}")
            return failures + ["mask-samples"]
        sw, sh, spix = _read_ppm(shot)
        reads[samples] = (_box_luma(spix, sw, sh, MASK_REF_BOX),
                          _box_luma(spix, sw, sh, MASK_KEPT_BOX),
                          _box_luma(spix, sw, sh, MASK_GONE_BOX))

    lit = all(r >= MASK_LIT_FLOOR and k >= MASK_LIT_FLOOR for r, k, _ in reads.values())
    # A RATIO between the two counts, AND an absolute ceiling on each. The ratio
    # asks the question this arm exists for -- the silhouette is a property of
    # the material, not of the framebuffer -- and the ceilings are what stop it
    # being satisfied by two counts that agree on something wrong.
    #
    # Spec 11.88 found that hole: with a ceiling on gone4 alone, a below-cutoff
    # quad rendering FULLY VISIBLE at one sample passed, since 0.47 against a
    # suppressed 0.09 is a ratio of 0.19. That is the "masked geometry renders as
    # solid quads" failure this whole group exists to catch, and this arm is the
    # only one that renders bare --msaa 1 with no TAA.
    #
    # gone1 == 0 is the IDEAL -- perfectly absent -- so it must not become an inf
    # ratio and fail. `lit` already guarantees the frame rendered, so a zero here
    # is a real reading and the ratio is simply not needed to judge it.
    gone1, gone4 = reads[1][2], reads[4][2]
    ratio = gone4 / gone1 if gone1 > 0 else 0.0
    ok = (lit and ratio <= MASK_SAMPLE_RATIO_MAX
          and gone1 <= MASK_GONE_CEIL and gone4 <= MASK_GONE_CEIL)
    print(f"  mask-samples {'PASS' if ok else 'FAIL'}  the below-cutoff quad reads "
          f"{gone1:.4f} at 1 sample and {gone4:.4f} at 4, ratio {ratio:.2f} (want <= "
          f"{MASK_SAMPLE_RATIO_MAX}, BOTH <= {MASK_GONE_CEIL} absolute, and the lit boxes "
          f"above {MASK_LIT_FLOOR} at both: {lit}). Its alpha is 0.2 against an authored "
          f"cutoff of 0.4, so it must be absent at every sample count; a cutoff replaced "
          f"by 0.02 under alpha-to-coverage reads 2.10.")
    if not ok:
        failures.append("mask-samples")

    return failures
