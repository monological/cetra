"""The taa-ortho group: the TAA jitter under an orthographic camera (spec 11.103)."""

import os

from gatelib.harness import asset
from gatelib.lighting import _taa_churn


# The TAA jitter under an orthographic camera (spec 11.103).
#
# texcomp_fixture because the defect SCALES WITH VIEW DEPTH and this is the
# corpus's only fixture that spans one: a plane running 1 to 400 units out. On a
# shallow scene the same bug is three pixels and no statistic separates it -- the
# alpha ladder, tried first, reads 52,637 broken against 48,404 fixed and decides
# nothing.
TAA_ORTHO_FIXTURE = "texcomp_fixture.cscn"
# Chosen to frame that plane at roughly the perspective run's coverage, so the
# two legs compare pictures of the same thing.
TAA_ORTHO_HEIGHT = "60"
# THE ARM MUST RUN UPSCALED OR IT CANNOT SEE HALF THE DEFECT, and this is the
# reason the scale is here rather than left at 1. The jitter is published in
# PIXELS for the TAAU resolve to un-apply, so the offset's SIGN only matters
# once something un-applies it: at full scale the wrong sign measured 192,630
# against the right sign's 210,974 -- it reads BETTER, and an arm there would
# have preferred the bug.
TAA_ORTHO_SCALE = "0.67"
# Ortho churn as a multiple of the same fixture's perspective churn. A ratio
# rather than an absolute because the fixture is inherently churny under TAA
# (a minifying plane of high-frequency maps); what is under test is whether the
# orthographic path behaves like the perspective one, not how noisy the picture
# is. A CORRECT ortho leg still churns more -- 1.25 to 1.38 over repeat runs --
# because the uniform projection holds the far end of that plane at a spatial
# frequency perspective would have shrunk away, so the bar is calibrated on the
# measured value and not on 1.0. The spread is the perspective leg's, which
# moved 376k to 415k while the ortho leg repeated exactly; two processes cannot
# share a load order, which is the residue of the problem the frame pair below
# solves. Bar 1.7: 1.23x the worst correct reading, and the nearest mutation is
# 1.39x the bar the other way.
# Re-derived after spec 11.104, which made GTAO, the view vector and the
# cluster wedge orthographic on this leg for the first time: the ortho leg
# reads 531,282 in both of two runs, the ratio 1.28 to 1.41 over the
# perspective leg's same spread, and the bar stays at 1.21x the worst correct
# reading rather than moving.
# Since spec 14.8 the spread is gone: a headless run's textures are in before
# frame 1, so the two processes no longer differ in load order, and both legs
# repeat exactly -- 485,976 and 375,977 in each of two runs, ratio 1.29. The bar
# stays, 1.32x that reading.
TAA_ORTHO_RATIO_MAX = 1.7


def run_taa_ortho_gate(workdir):
    """The TAA jitter is a sub-pixel offset under BOTH projections (spec 11.103).

      taa-ortho  an orthographic camera's jitter is a sub-pixel offset too, so its
                 TAA churn stays near the same fixture's perspective churn

    WHAT WENT WRONG. The jitter was added to draw_projection[2][0], the element
    that multiplies view-space z into clip x. Perspective divides that back out
    (clip.w is -z_eye) and lands a constant sub-pixel shift; glm_ortho's clip.w
    is 1, so it survives SCALED BY DEPTH -- about 150 px at a 300-unit camera
    distance. Nothing could reach it: the only orthographic cameras in the tree
    were two apps with no headless capture, and an imported camera is always
    perspective, which is why --ortho exists on the render app at all.

    FALSIFIED THREE WAYS, all measured at these parameters, as a ratio against
    the perspective leg: the shipped fix reads 1.38; putting the offset back on
    [2][0] reads 3.09; keeping the translation element but not negating it reads
    2.36. The last is the one this arm's scale and frame pair exist for -- at
    full scale, or at two converged frames, an un-negated offset is invisible.
    """
    arms = ("taa-ortho",)
    scene = asset(TAA_ORTHO_FIXTURE)
    if not os.path.exists(scene):
        print(f"  taa-ortho    SKIP  ({TAA_ORTHO_FIXTURE} not present)")
        return []
    scaled = ["--render-scale", TAA_ORTHO_SCALE]
    # ADJACENT, so the sign is visible, and LATE, so the two processes are
    # comparable. At 61/62 the perspective leg alternated between 381,755 and
    # 626,734 across runs of one build -- bimodal rather than noisy, which is the
    # async texture-load timing this fixture streams through, and the reason the
    # ladder group takes both its frames from ONE process. A ratio across two
    # processes cannot do that, so it waited for loading to settle instead. Since
    # spec 14.8 the loading is done before frame 1; the pair stays where it was
    # measured.
    pair = {"frames": 120, "first": 119}
    persp = _taa_churn(workdir, scene, "ortho_persp", scaled, **pair)
    orth = _taa_churn(workdir, scene, "ortho_ortho", scaled + ["--ortho", TAA_ORTHO_HEIGHT],
                      **pair)
    if persp is None or orth is None or persp <= 0:
        print("  taa-ortho    ERROR churn run failed or read zero")
        return list(arms)
    ratio = orth / persp
    ok = ratio <= TAA_ORTHO_RATIO_MAX
    print(f"  taa-ortho    {'PASS' if ok else 'FAIL'}  orthographic TAA churns {orth} px against "
          f"the same fixture's perspective {persp}, ratio {ratio:.2f} (want <= "
          f"{TAA_ORTHO_RATIO_MAX}: the jitter on the perspective element reads 3.09 and an "
          f"un-negated translation reads 2.36)")
    return [] if ok else list(arms)
