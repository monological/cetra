"""The skin-offpath, skin-curvature, skin-handoff, skin-area, sss-invariance and sss-banding groups:

  skin-offpath        pre-integrated skin (off-path byte identity)
  skin-curvature      pre-integrated skin (curvature ordering)
  skin-handoff        pre-integrated skin (handoff past the scatter ceiling)
  skin-area           subsurface under an area light (spec 11.19 / B3.2)
  sss-invariance      subsurface blur (world width vs frame size)
  sss-banding         subsurface blur (kernel not visible as rings)
"""

import math
import os

from gatelib.harness import NO_HALOS, RENDER, _run, asset, render
from gatelib.images import _linear_luma, _linear_rgb, _read_ppm, compare
from gatelib.scenes import _cscn_camera, _projector


# Pre-integrated skin (spec 11.13), measured on assets/skin_curvature_fixture.
#
# Three nodes share one unit sphere at scales 0.5 / 1.0 / 2.0, so their
# curvatures are exactly 2.0 / 1.0 / 0.5 -- and the angular width of the effect
# is proportional to curvature. The gate asserts the ORDERING that follows,
# which needs no stored reference and cannot drift: a falloff that ignores
# curvature is a wrap term, and a wrap term is two lines and cheaper.
#
SKIN_CAM = _cscn_camera("skin_curvature_fixture.cscn")
SKIN_LIGHT_TRAVEL = (0.93, -0.26, -0.26)  # direction light travels
SKIN_SPHERES = [("r050", 0.5, -3.6), ("r100", 1.0, -1.7), ("r200", 2.0, 1.7)]
SKIN_WRAP_DEG = 95.0   # just past the terminator: Lambert is zero, scatter is not
SKIN_LIT_DEG = 20.0    # well inside the lit cap, as the per-sphere reference
SKIN_MID_DEG = 45.0    # the reddening comparison point
SKIN_ORDER_RATIO = 1.15  # required step per 2x curvature
# Guard against a camera edit shrinking the spheres into sampling noise, as a
# FRACTION of frame height rather than an absolute pixel count. An absolute
# threshold silently encodes the author's display: this fixture yields 36 px at
# 800x500 on a 1x framebuffer and 72 px on a 2x one, so a 60 px floor passes on
# a retina laptop and hard-fails in CI for a reason unrelated to the feature.
SKIN_MIN_RADIUS_FRAC = 0.06


def _skin_sample_points(radius, cx):
    """Surface points at known angles from the light, on the visible side.

    A point at angle theta from L is C + R(cos(theta) L + sin(theta) T), where T
    is the component of the view direction perpendicular to L. That keeps every
    sample on the hemisphere facing the camera while pinning NdotL = cos(theta)
    analytically, so the gate never has to search an image for a feature.
    """
    def norm(v):
        m = math.sqrt(sum(c * c for c in v))
        return tuple(c / m for c in v)

    centre = (cx, 0.0, 0.0)
    light = norm(tuple(-c for c in SKIN_LIGHT_TRAVEL))  # surface -> light
    view = norm(tuple(SKIN_CAM["eye"][i] - centre[i] for i in range(3)))
    vdotl = sum(view[i] * light[i] for i in range(3))
    tang = norm(tuple(view[i] - vdotl * light[i] for i in range(3)))

    def at(deg):
        t = math.radians(deg)
        return tuple(
            centre[i] + radius * (math.cos(t) * light[i] + math.sin(t) * tang[i])
            for i in range(3)
        )

    return at


def _skin_render(workdir, tag, extra):
    out = os.path.join(workdir, f"skin_{tag}.ppm")
    scene = asset("skin_curvature_fixture.cscn")
    # --no-sss isolates the analytic falloff from the screen-space blur, which is
    # the only way to attribute what is measured. --no-shadows because the far
    # side of a convex caster is in its own shadow, so shadows multiply the whole
    # band by zero. --no-bloom because bloom smears the band being measured.
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "--no-auto-exposure", "-E", "1.0",
           "--no-sss", "--no-shadows", *NO_HALOS, "-W", "800", "-H", "500",
           "-S", out] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    return out


# Scenes that must be untouched by pre-integrated skin. The first two carry no
# subsurface at all; sss_scale_fixture DOES, with a profile assigned, and is the
# one that proves the opt-in rather than the feature merely being unreachable.
SKIN_OFFPATH_SCENES = ("cornell_point.cscn", "contact_fixture.cscn", "sss_scale_fixture.cscn")


def run_skin_offpath_gate(workdir):
    """A material that has not opted in must render identically either way.

    Exact zero, not one LSB: nothing in these scenes sets curvature_scale, so the
    shader takes the same branch and the arithmetic is the same arithmetic. A
    single differing bit here means the guard leaks.

    The last assertion is the one that stops this passing for the wrong reason.
    Three zeros are also what a dead flag, a broken parser or a fixture that
    stopped opting in would produce, so the curvature fixture must differ.
    """
    fails = []
    for scene_name in SKIN_OFFPATH_SCENES:
        scene = asset(scene_name)
        if not os.path.exists(scene):
            print(f"  skin-off     SKIP  (missing {scene_name})")
            continue
        base = os.path.join(workdir, f"skinoff_{scene_name}")
        err = render(scene, base + "_a.ppm", [])
        err = err or render(scene, base + "_b.ppm", ["--no-skin-preint"])
        if err:
            print(f"  skin-off     ERROR while rendering {scene_name}")
            fails.append("skin-offpath")
            continue
        ae, _ = compare(base + "_a.ppm", base + "_b.ppm")
        ok = ae == 0
        print(f"  skin-off     {'PASS' if ok else 'FAIL'}  {scene_name}: {ae} px "
              f"(want exactly 0)")
        if not ok:
            fails.append("skin-offpath")

    # The live half, and deliberately through the FULL path -- blur on, nothing
    # disabled. It has to run at a radius the blur CANNOT deliver, because
    # pre-integration only takes up slack and there is none otherwise.
    #
    # Until spec 11.14 the slack came from the blur's pixel cap, so this ran at a
    # large resolution to make the cap bind. That is no longer a lever: the cap is
    # now a scatter ceiling in world units per unit depth, so resolution does not
    # create slack at any size and this gate read 0 px. The lever is the authored
    # radius against the ceiling -- The pyramid's ceiling is about
    # 0.42 at the fixture's framing, so 0.28 is delivered in full and 1.5 is not.
    fixture = asset("skin_curvature_fixture.cscn")
    if os.path.exists(fixture):
        a = os.path.join(workdir, "skinlive_a.ppm")
        b = os.path.join(workdir, "skinlive_b.ppm")
        big = ["-W", "1200", "-H", "750", "--sss-radius", "1.5"]
        err = render(fixture, a, big)
        err = err or render(fixture, b, big + ["--no-skin-preint"])
        if err:
            print("  skin-live    ERROR while rendering the curvature fixture")
            fails.append("skin-offpath")
        else:
            ae, _ = compare(a, b)
            ok = ae > 0
            print(f"  skin-live    {'PASS' if ok else 'FAIL'}  opted-in fixture, full path "
                  f"past the ceiling: {ae} px "
                  f"(want > 0, else the three zeros above prove nothing)")
            if not ok:
                fails.append("skin-offpath")
    return fails


# Skin under an LTC panel must actually scatter. The bug this pins was total
# rather than partial: the area branch added its diffuse to Lo and skipped the
# SSS tap, because LTC integrates the whole rectangle analytically and has no
# single L to hand a diffusion profile. So a softbox -- the canonical portrait
# setup, and the one place skin matters most -- produced no subsurface response
# at all, and SSS on measured byte-identical to --no-sss.
#
# Stated as on-vs-off rather than against a reference image, so it needs no
# golden and cannot drift: the assertion is "the feature does something", and
# the failure it guards is a hard zero. Measured 551295 px after the fix, 0
# before; the floor is set an order of magnitude below that because what is
# being detected is presence, not amount.
SKIN_AREA_MIN_PX = 50000


def run_skin_area_gate(workdir):
    """Skin lit only by an area panel must respond to SSS being on.

    The directional arm is what stops this passing for the wrong reason. Zero is
    also what a dead --no-sss flag, a broken .cscn parse or a fixture that lost
    its subsurface would produce, so a scene the feature already worked in has to
    differ by the same measurement.
    """
    fails = []
    for label, scene_name in (("area", "skin_area_fixture.cscn"),
                              ("directional", "skin_curvature_fixture.cscn")):
        scene = asset(scene_name)
        if not os.path.exists(scene):
            print(f"  skin-area    SKIP  (missing {scene_name})")
            continue
        base = os.path.join(workdir, f"skinarea_{label}")
        size = ["-W", "800", "-H", "500"]
        err = render(scene, base + "_on.ppm", size)
        err = err or render(scene, base + "_off.ppm", size + ["--no-sss"])
        if err:
            print(f"  skin-area    ERROR while rendering {scene_name}")
            fails.append("skin-area")
            continue
        ae, _ = compare(base + "_on.ppm", base + "_off.ppm")
        ok = ae >= SKIN_AREA_MIN_PX
        note = ("a panel used to give skin nothing" if label == "area"
                else "the control: this one always worked")
        print(f"  skin-area    {'PASS' if ok else 'FAIL'}  {label} light, sss on vs off: "
              f"{ae} px (want >= {SKIN_AREA_MIN_PX}; {note})")
        if not ok:
            fails.append("skin-area")
    return fails


def _skin_falloff_crossing(pix, w, h, project, at, lit, frac=0.05):
    """Angle where the falloff drops through `frac` of the lit reference.

    Preferred over the 90-to-10 width for anything measuring how much the blur
    delivers. Scatter's whole job is pushing light PAST the terminator, so a low
    crossing tracks delivered width almost directly, while the 90% end barely
    moves. Measured on the same renders, the two disagree badly: across an 8x
    framebuffer sweep the crossing holds to 0.7% while the width's ON-minus-OFF
    difference swings 13%, because that difference subtracts two ~36 degree
    numbers and amplifies whatever they do by about 12x (spec 11.14 phase 2).
    """
    prev_a = prev_v = None
    level = frac * lit
    a = float(SKIN_LIT_DEG)
    while a <= 175.0:
        v = _linear_luma(pix, w, h, *project(at(a)))
        if prev_v is not None and prev_v >= level > v:
            span = prev_v - v
            t = (prev_v - level) / span if span > 1e-12 else 0.0
            return prev_a + t * (a - prev_a)
        prev_a, prev_v = a, v
        a += 0.25
    return None


def _skin_sample(workdir, tag, dims, extra):
    """Render the curvature fixture and sample the mid sphere.

    Returns (crossing angle, wrap-over-lit ratio). The two answer different
    questions and neither covers the other's range: the crossing is the sensitive
    instrument for how far scatter reaches, but it CEASES TO EXIST once the
    scatter is wide enough that no point on the visible hemisphere falls to 5% of
    lit -- measured, anything past radius ~0.4 on this fixture. The ratio at a
    fixed angle is coarser but always defined, which is what a gate spanning both
    regimes needs.
    """
    scene = asset("skin_curvature_fixture.cscn")
    out = os.path.join(workdir, f"skinm_{tag}.ppm")
    # --no-shadows so the far side is not multiplied away, --no-bloom so nothing
    # smears the falloff being measured, --no-dither because the crossing is
    # interpolated from single pixels at 5% of the lit reference, where one LSB
    # of the default output dither is a large relative step and the two legs of
    # the drift subtraction pick up independent offsets that do not cancel.
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "--no-auto-exposure", "-E", "1.0",
           "--no-shadows", *NO_HALOS, "--no-dither",
           "-W", dims[0], "-H", dims[1], "-S", out] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    w, h, pix = _read_ppm(out)
    project = _projector(SKIN_CAM, w, h)
    # The mid sphere: big enough to sample finely, small enough to sit well
    # inside the frame at every resolution used here.
    _name, radius, cx = SKIN_SPHERES[1]
    at = _skin_sample_points(radius, cx)
    lit = _linear_luma(pix, w, h, *project(at(SKIN_LIT_DEG)))
    wrap = _linear_luma(pix, w, h, *project(at(SKIN_WRAP_DEG)))
    return (_skin_falloff_crossing(pix, w, h, project, at, lit),
            wrap / max(lit, 1e-9))


def run_sss_invariance_gate(workdir):
    """The blur delivers the SAME world scatter however large the frame is.

    This is spec 11.14's headline, and the defect it replaced was live in shipped
    code from the day SSS landed -- specs/4.12-sss.md:115 names "blur-width
    scaling ... must scale with render resolution" as a risk with nothing behind
    it. Capping the kernel in PIXELS made the delivered world width fall as
    1/height once the cap engaged: measured rho = 0.025 across a 500->4000 px
    sweep, i.e. 2.5% of the low-resolution effect survived at 4K.

    Runs --no-skin-preint so the angular falloff contributes nothing and every
    degree measured is the screen-space pass. Each height renders twice, with and
    without SSS, and the gate compares how far the blur pushes the falloff past
    where Lambert leaves it -- see _skin_falloff_crossing for why not the width.

    A 4x sweep: the old cap engaged part-way up, so a short sweep could sit
    entirely inside the clamped regime and read flat while being wrong.
    """
    scene = asset("skin_curvature_fixture.cscn")
    if not os.path.exists(scene):
        print("  sss-scale    SKIP  (missing skin_curvature_fixture.cscn)")
        return []

    # 500p, not 250p, as the low leg. At 250 the mid sphere is about 50 px across
    # and the terminator measurement runs out of pixels: measured pushes are
    # +4.83 / +5.22 / +5.16 / +5.12 deg at 250 / 500 / 1000 / 2000, so the three
    # larger agree to 1.9% and only the smallest disagrees. A gate leg that
    # cannot resolve its own subject reports the instrument, not the renderer.
    pushes = []
    for tag, dims in (("lo", ("500", "312")), ("hi", ("2000", "1250"))):
        off = _skin_sample(workdir, tag + "_lam", dims, ["--no-sss", "--no-skin-preint"])
        on = _skin_sample(workdir, tag + "_sss", dims, ["--no-skin-preint"])
        if off is None or on is None or off[0] is None or on[0] is None:
            print(f"  sss-scale    ERROR while rendering at {dims[0]}x{dims[1]}")
            return ["sss-scale"]
        pushes.append(on[0] - off[0])

    lo, hi = pushes
    drift = abs(hi / lo - 1.0) if abs(lo) > 1e-9 else float("inf")
    ok = drift <= SSS_DRIFT_MAX and lo > 1.0
    print(f"  sss-scale    {'PASS' if ok else 'FAIL'}  blur pushes the falloff "
          f"{lo:+.2f} deg at 500p and {hi:+.2f} deg at 2000p, drift {drift:.1%} "
          f"(want <= {SSS_DRIFT_MAX:.0%}, and a real push at all)")
    return [] if ok else ["sss-scale"]


# Ripple bar for run_sss_banding_gate, and the window it is measured over.
#
# Calibrated against four builds rather than chosen, because a bar with one
# control is a guess -- the metric this replaced was exactly that, reading 0.9999
# on a smooth frame and 0.9999 on a banded one, which is how the banding shipped.
#
#   separable blur, unclamped   0.012   no artifact
#   pyramid gather (shipped)    0.014   no artifact (verified at 1:1)
#   cap removed, uniform taps   0.074   visibly banded
#   cap removed, quadratic      0.088   visibly banded
#
# The bar sits at 0.030: 2.1x above the shipped build and 2.5x below the best
# dirty one. It was briefly loosened to 0.045 mid-branch, when an interim pyramid
# measured 0.030 -- that build was superseded three commits later and the bar was
# never re-tightened, leaving the shipped code sitting 3.2x under its own gate.
# Loosening a bar to pass one's own code is the obvious abuse, so: it is back
# where it started, and the row above records what the shipped build actually
# measures rather than what the interim one did.
SSS_RIPPLE_MAX = 0.030

# Drift bar for run_sss_invariance_gate.
#
# Spec 11.14 set the exit criterion at 2%; the gate first shipped at 5% with no
# justification recorded anywhere, which is the one bar on this branch that was
# moved rather than argued. 3% is the honest number: the shipped build measures
# 2.5%, so this is 20% of headroom over a real measurement, against the 8.3%
# the separable blur drifted and the rho = 0.025 collapse before that.
SSS_DRIFT_MAX = 0.03
SSS_RIPPLE_WINDOW = 12


def _scanline_ripple(pix, w, h, project, radius, cx, half=SSS_RIPPLE_WINDOW):
    """RMS ripple along the horizontal line through a sphere's centre.

    Measured in SCREEN space, not in surface angle, because that is where the
    artifact is periodic: a discrete-tap kernel puts its rings at the tap
    spacing, which is a pixel quantity. Sampling by angle smears them and gets
    barely 2x separation; sampling by pixel gets 6x.

    Compares the scanline against its own moving average, so it needs no
    reference image and cannot drift -- the property every gate in this file
    has.
    """
    c = project((cx, 0.0, 0.0))
    edge = project((cx, radius, 0.0))
    r_px = abs(edge[1] - c[1])
    y = int(round(c[1]))
    # 0.92 of the radius: the silhouette itself is a real discontinuity and
    # would read as ripple in any build.
    x0 = max(0, int(round(c[0] - 0.92 * r_px)))
    x1 = min(w - 1, int(round(c[0] + 0.92 * r_px)))
    vals = [_linear_luma(pix, w, h, x, y) for x in range(x0, x1 + 1)]
    if len(vals) < 4 * half:
        return None
    res = []
    for i in range(half, len(vals) - half):
        sm = sum(vals[i - half:i + half + 1]) / (2 * half + 1)
        if sm < 1e-4:
            continue
        res.append((vals[i] - sm) / sm)
    if len(res) < 8:
        return None
    return (sum(r * r for r in res) / len(res)) ** 0.5


def run_sss_banding_gate(workdir):
    """The blur's kernel must not be visible as rings.

    A screen-space blur samples its profile at discrete taps. While the kernel
    is small the taps land within a pixel or two of each other and the result
    reads as smooth; widen the kernel without adding taps and the taps become
    individually resolvable, as concentric rings through the terminator.

    That is not hypothetical. Spec 11.14 removed the blur's 48 px cap so the
    delivered world width would stop depending on resolution -- correct, and it
    took radPx from 48 to 68 px at this framing, which was enough to make the
    taps visible. Nothing in the battery caught it; a human looked at the image.
    This gate exists so that cannot happen twice, and it is why any change to the
    kernel's width or tap budget has to come past a measurement.

    Runs with SSS ON, which is the whole point -- every other skin gate runs
    --no-sss and is structurally blind to this.
    """
    scene = asset("skin_curvature_fixture.cscn")
    if not os.path.exists(scene):
        print("  sss-band     SKIP  (missing skin_curvature_fixture.cscn)")
        return []

    out = os.path.join(workdir, "sssband.ppm")
    # --no-dither is load-bearing here, not tidiness: this gate's statistic is
    # per-pixel deviation from a 25px moving average, which is exactly the
    # frequency the output dither injects. With the default on it reads 0.0196
    # against a 0.030 bound where the blur itself contributes 0.0142, so the
    # gate would start failing on dither amplitude rather than on kernel rings.
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "--no-auto-exposure", "-E", "1.0",
           "--no-shadows", *NO_HALOS, "--no-dither", "-W", "1200", "-H", "750", "-S", out]
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        print("  sss-band     ERROR while rendering the curvature fixture")
        return ["sss-band"]

    w, h, pix = _read_ppm(out)
    project = _projector(SKIN_CAM, w, h)
    # The mid sphere only. The radius-2.0 one runs off the frame at this
    # framing, so a scanline through its centre leaves the silhouette and reads
    # background -- which saturates any relative measure at 1.0 and would make
    # this gate pass or fail on framing rather than on shading.
    _name, radius, cx = SKIN_SPHERES[1]
    ripple = _scanline_ripple(pix, w, h, project, radius, cx)
    if ripple is None:
        print("  sss-band     ERROR sphere too small to scan at 1200x750")
        return ["sss-band"]

    ok = ripple <= SSS_RIPPLE_MAX
    print(f"  sss-band     {'PASS' if ok else 'FAIL'}  kernel ripple {ripple:.4f} "
          f"(want <= {SSS_RIPPLE_MAX:.3f}; smooth reference measures 0.012, "
          f"visibly banded 0.074)")
    return [] if ok else ["sss-band"]


def run_skin_handoff_gate(workdir):
    """D5: the angular half carries exactly what the blur cannot, and no more.

    Both legs run at ONE resolution and differ only in the authored radius. That
    is the whole re-pointing: until spec 11.14 the blur's shortfall came from a
    cap measured in PIXELS, so resolution was the lever and this gate swept two
    heights. The cap is now a scatter ceiling in world units per unit depth, so
    resolution creates no shortfall at any size -- swept legs converge and the
    gate asserted nothing. What creates shortfall now is an authored radius
    exceeding the ceiling, which is a scene property, as it should be.

    The pyramid's ceiling is about 0.42 at this framing, so 0.28 is
    delivered whole and 1.5 is delivered at roughly a quarter.

    The bars come from the two ways D5 can be broken. Pinning the deficit to 0
    kills the feature and makes the wide leg quiet too, which the second bar
    catches. Pinning it to 1 removes the composition, so the narrow leg starts
    contributing on top of a blur that already delivers in full, which the first
    bar catches. Nothing else covers either: every other skin measurement runs
    --no-sss, which forces the deficit to 1 by construction.

    Phase 3 confirmed the composition is right rather than merely self-consistent.
    Penner's integral, evaluated by tools/gen_skin_preint_fit.py, says the
    fixture's terminator should move +8.96 deg; the blur alone now moves it
    +11.54. It is already at the reference, so pre-integration standing down where
    the blur is unclamped is correct, not a feature going missing.
    """
    scene = asset("skin_curvature_fixture.cscn")
    if not os.path.exists(scene):
        print("  skin-handoff SKIP  (missing skin_curvature_fixture.cscn)")
        return []

    # Read through the wrap-over-lit ratio, not the crossing angle: at a radius
    # far enough past the ceiling to make the deficit bite, no point on the
    # visible hemisphere falls to 5% of lit and the crossing does not exist.
    dims = ("1200", "750")
    given = {}
    for tag, radius in (("within", "0.05"), ("past", "1.5")):
        extra = ["--sss-radius", radius]
        on = _skin_sample(workdir, "hand_" + tag + "_on", dims, extra)
        off = _skin_sample(workdir, "hand_" + tag + "_off", dims,
                           extra + ["--no-skin-preint"])
        if on is None or off is None:
            print(f"  skin-handoff ERROR while rendering at radius {radius}")
            return ["skin-handoff"]
        given[tag] = on[1] / max(off[1], 1e-9)

    quiet = given["within"] <= 1.02
    carries = given["past"] >= 1.15
    ok = quiet and carries
    print(f"  skin-handoff {'PASS' if ok else 'FAIL'}  angular half lifts the wrap "
          f"{given['within']:.3f}x at radius 0.05 (want <= 1.02, blur delivers it) "
          f"and {given['past']:.3f}x at radius 1.5 (want >= 1.15, past the ceiling)")
    return [] if ok else ["skin-handoff"]


def run_skin_curvature_gate(workdir):
    scene = asset("skin_curvature_fixture.cscn")
    if not os.path.exists(scene):
        print("  skin-curve   SKIP  (missing skin_curvature_fixture.cscn)")
        return []

    on_path = _skin_render(workdir, "on", [])
    off_path = _skin_render(workdir, "off", ["--no-skin-preint"])
    if not (on_path and off_path):
        print("  skin-curve   ERROR while rendering the curvature fixture")
        return ["skin-curvature"]

    w, h, on = _read_ppm(on_path)
    _, _, off = _read_ppm(off_path)
    project = _projector(SKIN_CAM, w, h)

    wrap_on, lit_on, lit_off, wrap_off, red = {}, {}, {}, {}, {}
    for name, radius, cx in SKIN_SPHERES:
        at = _skin_sample_points(radius, cx)
        # Screen radius, as a guard: a camera edit that shrinks the spheres
        # would quietly reduce this gate to sampling noise.
        c0 = project((cx, 0.0, 0.0))
        c1 = project((cx, radius, 0.0))
        px_radius = math.hypot(c1[0] - c0[0], c1[1] - c0[1])
        if px_radius < SKIN_MIN_RADIUS_FRAC * h:
            print(f"  skin-curve   ERROR sphere {name} is {px_radius:.0f} px of {h} "
                  f"(want >= {SKIN_MIN_RADIUS_FRAC:.0%} of frame height)")
            return ["skin-curvature"]
        wrap_on[name] = _linear_luma(on, w, h, *project(at(SKIN_WRAP_DEG)))
        wrap_off[name] = _linear_luma(off, w, h, *project(at(SKIN_WRAP_DEG)))
        lit_on[name] = _linear_luma(on, w, h, *project(at(SKIN_LIT_DEG)))
        lit_off[name] = _linear_luma(off, w, h, *project(at(SKIN_LIT_DEG)))
        red[name] = (_linear_rgb(on, w, h, *project(at(SKIN_WRAP_DEG))),
                     _linear_rgb(on, w, h, *project(at(SKIN_MID_DEG))))

    fails = []

    # Controls, on the OFF frame. Without these a broken rig looks like a pass.
    worst_dark = max(wrap_off[n] / max(lit_off[n], 1e-6) for n, _, _ in SKIN_SPHERES)
    ok = worst_dark <= 0.02
    print(f"  skin-dark    {'PASS' if ok else 'FAIL'}  off-frame wrap band "
          f"{worst_dark:.4f} of lit (want <= 0.02: Lambert is black past the terminator)")
    if not ok:
        fails.append("skin-dark")

    # The key is directional, so NdotL at a given angle is identical on all
    # three; the residual spread is SPECULAR, which depends on view angle and so
    # varies along a row spanning 8 units of x. Measured at 0.05, and the bar
    # sits above it rather than at it -- the control only has to be much smaller
    # than the ordering step it is protecting, which is 15%.
    lits = [lit_off[n] for n, _, _ in SKIN_SPHERES]
    spread = (max(lits) - min(lits)) / max(max(lits), 1e-6)
    ok = spread <= 0.08
    print(f"  skin-even    {'PASS' if ok else 'FAIL'}  off-frame lit spread "
          f"{spread:.4f} (want <= 0.08: any ON spread is curvature, not framing)")
    if not ok:
        fails.append("skin-even")

    # A1: the wrap must follow curvature. This is the assertion the whole
    # fixture exists for.
    small, mid, big = (wrap_on[n] for n, _, _ in SKIN_SPHERES)
    step1 = small / max(mid, 1e-6)
    step2 = mid / max(big, 1e-6)
    ok = step1 >= SKIN_ORDER_RATIO and step2 >= SKIN_ORDER_RATIO
    print(f"  skin-order   {'PASS' if ok else 'FAIL'}  wrap steps {step1:.2f}x, {step2:.2f}x "
          f"per 2x curvature (want >= {SKIN_ORDER_RATIO}x)")
    if not ok:
        fails.append("skin-order")

    # A2: and it must actually deliver light, not merely order correctly.
    floor = small / max(lit_on["r050"], 1e-6)
    ok = floor >= 0.05
    print(f"  skin-floor   {'PASS' if ok else 'FAIL'}  sharpest wrap {floor:.4f} of lit "
          f"(want >= 0.05)")
    if not ok:
        fails.append("skin-floor")

    # A3: reddening. Red scatters furthest through flesh, so the wrap band must
    # be warmer than the mid-lit surface; a grey wrap is the wrong model.
    wrap_rgb, mid_rgb = red["r050"]
    wrap_ratio = wrap_rgb[0] / max(wrap_rgb[1], 1e-6)
    mid_ratio = mid_rgb[0] / max(mid_rgb[1], 1e-6)
    ok = wrap_ratio >= mid_ratio * 1.10
    print(f"  skin-red     {'PASS' if ok else 'FAIL'}  R/G {wrap_ratio:.3f} at the wrap vs "
          f"{mid_ratio:.3f} at 45 deg (want >= 1.10x)")
    if not ok:
        fails.append("skin-red")

    # A4: no free lunch, and it is the assertion that separates a real
    # pre-integration from a glow hack.
    #
    # Convolution with a normalised kernel conserves energy, so the light the
    # wrap band gained has to come from the lit cap -- it MUST dim. Requiring it
    # to be unchanged, as an earlier version of this gate did, asks for
    # conservation to be violated. What it must not do is brighten, because
    # anything that adds energy is inventing light rather than moving it.
    #
    # The dimming also has to ORDER by curvature, for the same reason the wrap
    # does: sharper curvature scatters over a wider angle and gives up more.
    # Ground truth for this fixture predicts 14.1% / 4.8% / 1.2%; the rendered
    # numbers differ because the frame is tonemapped, so the ordering is
    # asserted and the magnitude only reported.
    ratios = [lit_on[n] / max(lit_off[n], 1e-6) for n, _, _ in SKIN_SPHERES]
    brightest = max(ratios)
    ok = brightest <= 1.01
    print(f"  skin-energy  {'PASS' if ok else 'FAIL'}  lit cap {ratios[0]:.3f} / "
          f"{ratios[1]:.3f} / {ratios[2]:.3f} of Lambert by curvature "
          f"(want <= 1.01: it may dim, never brighten)")
    if not ok:
        fails.append("skin-energy")

    ok = ratios[0] < ratios[1] < ratios[2]
    print(f"  skin-give    {'PASS' if ok else 'FAIL'}  dimming orders with curvature "
          f"(want sharpest gives up most)")
    if not ok:
        fails.append("skin-give")

    return fails
