"""The varying group: varyings under partial coverage (spec 11.38)."""

import os

from gatelib.harness import RENDER, _run, asset
from gatelib.images import _read_ppm, compare


VARY_FIXTURE = "varying_fixture.cscn"
# The UV control quad, as fractions of the frame. It is the only surface in the scene with UVs
# authored outside [0,1], so it is how the frame separates "authored out of range" from
# "extrapolated out of range" -- and the whole-frame UV count has it subtracted off.
#
# CONTAINS the quad rather than sitting inside it, which is the opposite of the usual convention
# here and is why: an inset box leaves a margin of the quad's own authored excursion outside
# itself, and the subtraction then charges it to the fins. Measured, that inflated the fin UV
# figure from 12,685 to 25,296 -- the quad projects to x 0.087-0.193, y 0.605-0.747, so this
# clears it on every side. Nothing is lost by being generous: the backdrop reads zero.
VARY_CTRL_BOX = (0.07, 0.58, 0.21, 0.77)
# 8-bit code above which a channel counts. Not a tuned bar: the fixture's backdrop covers the
# frame, so an uncovered pixel is 0 rather than the 0.1 grey the scene clear would give, and
# every count below is either zero or in the tens of thousands. Nothing sits near this.
VARY_FLOOR = 16
# The control quad measures 28,900 px and the fins about 12,600 in each channel. Floors an order
# down, because what is being detected is presence, not amount.
VARY_CTRL_MIN = 8000
VARY_FIN_MIN = 3000
VARY_COVERAGE_MIN = 8000  # 26,183 px differ between the sample counts in albedo mode


def _vary_render(workdir, tag, mode, samples):
    """The fixture at one render mode and one sample count. Own harness: render() is 400x300."""
    out = os.path.join(workdir, f"vary_{tag}.ppm")
    cmd = [RENDER, "-m", asset(VARY_FIXTURE), "-x", "-f", "30",
           "-W", "800", "-H", "600", "--render-mode", str(mode), "--msaa", str(samples),
           "-S", out]
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    return out


def _vary_counts(path, box=None):
    """Pixels at or above VARY_FLOOR in each channel, whole frame or inside a fractional box."""
    w, h, pix = _read_ppm(path)
    x0, y0, x1, y1 = box if box else (0.0, 0.0, 1.0, 1.0)
    counts = [0, 0, 0]
    for py in range(int(y0 * h), int(y1 * h)):
        row = (py * w) * 3
        for px in range(int(x0 * w), int(x1 * w)):
            o = row + px * 3
            for c in range(3):
                if pix[o + c] >= VARY_FLOOR:
                    counts[c] += 1
    return counts


def run_varying_gate(workdir):
    """What MSAA does to a varying on a pixel it only partly covers (spec 11.38).

    The fragment stage runs once per PIXEL while coverage is per SAMPLE, so a varying is
    evaluated at one point -- the pixel centre unless it is qualified `centroid`. On a partly
    covered pixel that centre can lie outside the triangle, and interpolating outside a triangle
    is extrapolation: the weights go negative and the value leaves the range its three vertices
    bound. RENDER_MODE_EXTRAPOLATION reports that per channel, and for the normal and the
    tangent the test is exact -- both are unit at every vertex, and no convex combination of
    unit vectors is longer than one, so a length above one cannot arise any other way.

    These arms CHARACTERIZE rather than assert a fix, and that is deliberate. `centroid` fixes
    it and cannot be used here: it makes derivatives inexact, and the remaining varyings all
    feed one -- dFdx(Normal) the specular AA, dFdx(WorldPos) the curvature, and UV gradients the
    hardware's texture LOD. Measured, every variant moves 9 to 22 goldens with the diff
    following the TESSELLATION across whole surfaces rather than the silhouettes, which is a
    worse error than the one being removed. VertexColor took the qualifier in 11.38's
    predecessor precisely because it is the one varying nothing differentiates.

    So the invariant that is permanent is the SINGLE-SAMPLE one: at one sample there is no
    partial coverage, so extrapolation is not merely absent but impossible, and any nonzero
    reading there is the instrument lying. The 4x arm records the defect at its measured size;
    it is expected to go to zero the day the varyings are qualified or the fixture is rendered
    on an immune path, and that should arrive as a deliberate edit here rather than silently.
    """
    scene = asset(VARY_FIXTURE)
    if not os.path.exists(scene):
        print(f"  vary-ground  SKIP  ({VARY_FIXTURE} not present)")
        return []

    failures = []

    # Albedo rather than flat colour: RENDER_MODE_FLAT_COLOR paints every fragment one constant,
    # and with the backdrop covering the frame there is no edge left in it to antialias -- it
    # compares at exactly 0 between the sample counts and proves nothing.
    cov4 = _vary_render(workdir, "cov4", 6, 4)
    cov1 = _vary_render(workdir, "cov1", 6, 1)
    if not cov4 or not cov1:
        print("  vary-cover   ERROR while rendering the albedo pair")
        return ["vary-cover"]
    cover, _ = compare(cov4, cov1)
    ok = cover >= VARY_COVERAGE_MIN
    print(f"  vary-cover   {'PASS' if ok else 'FAIL'}  {cover} px differ between 4x and 1 "
          f"sample (want >= {VARY_COVERAGE_MIN}: partly covered pixels are what the rest of "
          f"this gate measures, so they have to be shown to exist)")
    if not ok:
        failures.append("vary-cover")

    x4 = _vary_render(workdir, "x4", 12, 4)
    x1 = _vary_render(workdir, "x1", 12, 1)
    if not x4 or not x1:
        print("  vary-ground  ERROR while rendering the extrapolation pair")
        return failures + ["vary-ground"]

    all4 = _vary_counts(x4)
    all1 = _vary_counts(x1)
    ctrl4 = _vary_counts(x4, VARY_CTRL_BOX)
    ctrl1 = _vary_counts(x1, VARY_CTRL_BOX)

    # Ground truth. Not "small": impossible, because one sample has no partial coverage. The UV
    # channel is exempt because the control quad's UVs are AUTHORED outside [0,1], which is a
    # property of the mesh rather than of interpolation and so survives any sample count.
    ok = all1[0] == 0 and all1[1] == 0
    print(f"  vary-ground  {'PASS' if ok else 'FAIL'}  at 1 sample: normal {all1[0]}, "
          f"tangent {all1[1]} (want exactly 0 -- with coverage a single centre test there is "
          f"nothing outside the triangle to sample)")
    if not ok:
        failures.append("vary-ground")

    # The authored excursion has to read, or the channel could be dead and the zero above would
    # mean nothing. It is uniform over the quad and identical at both sample counts.
    ok = ctrl1[2] >= VARY_CTRL_MIN and ctrl4[2] >= VARY_CTRL_MIN
    print(f"  vary-control {'PASS' if ok else 'FAIL'}  UV control quad reads {ctrl1[2]} px at 1 "
          f"sample and {ctrl4[2]} at 4x (want >= {VARY_CTRL_MIN} both; its UVs are authored "
          f"outside [0,1], so a zero here is the instrument failing, not the renderer)")
    if not ok:
        failures.append("vary-control")

    # The defect, at its size. Outside the control box, so every UV pixel counted here left
    # [0,1] by interpolation rather than by authoring.
    fins = [all4[c] - ctrl4[c] for c in range(3)]
    ok = fins[0] >= VARY_FIN_MIN and fins[1] >= VARY_FIN_MIN and fins[2] >= VARY_FIN_MIN
    print(f"  vary-msaa    {'PASS' if ok else 'FAIL'}  at 4x, off the control quad: normal "
          f"{fins[0]}, tangent {fins[1]}, uv {fins[2]} px (want >= {VARY_FIN_MIN} each -- this "
          f"records an OPEN defect, so it goes to zero only by a deliberate change)")
    if not ok:
        failures.append("vary-msaa")

    return failures
