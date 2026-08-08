#!/usr/bin/env python3
"""Run the working-space contract gates (specs/10.1, 10.2).

    python3 scripts/gates.py [--keep]

Every gate here is SELF-VERIFYING: it compares two renders of the same build,
so there is no stored reference image to regenerate and no baseline to drift.
That is the whole reason these can run in CI where the pixel goldens cannot --
a golden needs a "before", a scale gate does not.

WHY THESE EXIST (spec 10.2): 10.1 shipped with nine goldens green and three
space-mixing bugs live. The goldens all pin exposure, so `preExposure == 1` and
multiplying by it is the identity -- they are structurally blind to the entire
class. The one gate that was not blind, scale invariance, ran on a single
punctual-lit scene, so it covered one of the four paths that write into the HDR
buffer.

The rule: a gate that pins exposure cannot see a space-mixing bug.
"""

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RENDER = os.path.join(ROOT, "out", "bin", "render")
SCALE = 1000.0


def _cscn_camera(name, **extra):
    """Camera read out of the fixture's own .cscn, in _projector's dict shape.

    A gate that predicts where geometry lands on screen needs the exact camera
    the render used. Transcribing it leaves two copies that agree only until
    someone edits the .cscn, and the failure is silent -- the gate keeps passing
    while measuring a different scene than it predicts. Reading it removes the
    copy. Anything passed as **extra is the gate's OWN choice (sample columns,
    thresholds), which is worth keeping visibly distinct from what the scene
    file dictates.
    """
    with open(os.path.join(ROOT, "assets", name)) as f:
        cam = json.load(f)["camera"]
    return {"eye": tuple(cam["eye"]), "target": tuple(cam["target"]),
            "fovy_deg": float(cam["fov"]), **extra}

# Scale-invariance gates. Scaling every emitter by K and the exposure by 1/K is
# a no-op on a correctly normalised pipeline, so the two frames must match.
SCALE_GATES = [
    ("punctual", "assets/cornell_point.cscn", []),
    ("area", "assets/area_scale_fixture.cscn", []),
    ("subsurface", "assets/sss_scale_fixture.cscn", []),
    # Fog's in-scatter radiance is a real emitter, so the scaler lifts
    # fog.ambient alongside the lights.
    ("fog", "assets/froxel_scale_fixture.cscn", ["--fog"]),
]

# Pass on PEAK error, not on a differing-pixel count.
#
# A per-gate pixel budget is the wrong instrument: the numbers drift whenever
# float arithmetic reassociates (moving one multiply out of a sum is enough),
# so the budget gets raised to match, and after a few rounds it is large enough
# to hide a real defect. Peak error does not drift -- 1/255 is one 8-bit
# quantization step, so anything at or below it cannot be seen in the output at
# all, and anything above it is a genuine difference no matter how few pixels
# carry it. AE is still reported, because a jump in how MANY pixels round
# differently is worth a look even when none of them moved visibly.
LSB = 1.0 / 255.0 + 1e-6


def scaled_copy(src, dst, factor):
    """Multiply every authored intensity by `factor` and divide exposure by it.

    Doing this mechanically rather than committing a hand-edited twin is the
    point: it is impossible for the two halves to drift in any way except the
    scale, which is exactly the property the gate is asserting.
    """
    with open(src) as f:
        d = json.load(f)

    for light in d.get("lights", []):
        if "intensity" in light:
            light["intensity"] *= factor

    env = d.get("environment")
    if env:
        if "intensity" in env:
            env["intensity"] *= factor
        if "ambient" in env:
            env["ambient"] = [c * factor for c in env["ambient"]]

    post = d.setdefault("post", {})
    fog = post.get("fog")
    if fog and "ambient" in fog:
        fog["ambient"] = [c * factor for c in fog["ambient"]]
    post["exposure"] = post.get("exposure", 1.0) / factor

    # Model paths resolve against the scene file's directory, so an out-of-tree
    # copy has to carry absolute ones.
    for m in d.get("models", []):
        if not os.path.isabs(m["path"]):
            m["path"] = os.path.join(os.path.dirname(os.path.abspath(src)), m["path"])

    with open(dst, "w") as f:
        json.dump(d, f, indent=1)


def render(scene, out, extra):
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "-W", "400", "-H", "300", "-S", out]
    r = subprocess.run(cmd + extra, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return r.stdout + r.stderr
    return None


def compare(a, b):
    """Return (differing pixels, peak absolute error as a 0..1 fraction)."""
    def metric(name):
        r = subprocess.run(["magick", "compare", "-metric", name, a, b, "null:"],
                           capture_output=True, text=True)
        return (r.stderr or r.stdout).strip()

    ae = int(float(metric("AE").split()[0]))
    # PAE prints "257 (0.00392157)"; the parenthesised value is the normalised one.
    pae_raw = metric("PAE")
    pae = float(pae_raw.split("(")[1].rstrip(")")) if "(" in pae_raw else 0.0
    return ae, pae


def run_scale_gates(workdir):
    failures = []
    for name, rel, extra in SCALE_GATES:
        src = os.path.join(ROOT, rel)
        if not os.path.exists(src):
            print(f"  {name:<12} SKIP  (missing {rel})")
            continue

        big = os.path.join(workdir, f"{name}_{int(SCALE)}x.cscn")
        scaled_copy(src, big, SCALE)
        a = os.path.join(workdir, f"{name}_a.ppm")
        b = os.path.join(workdir, f"{name}_b.ppm")

        for scene, out in ((src, a), (big, b)):
            err = render(scene, out, extra)
            if err:
                print(f"  {name:<12} ERROR while rendering {scene}")
                failures.append(name)
                break
        else:
            ae, pae = compare(a, b)
            ok = pae <= LSB
            detail = f"{ae} px, peak {pae:.6f}"
            if ok and ae:
                detail += " (1 LSB -- invisible)"
            print(f"  {name:<12} {'PASS' if ok else 'FAIL'}  {detail}")
            if not ok:
                failures.append(name)
    return failures


# Area-shadow penumbra gate (spec 10.4). The fixture's geometry is chosen so the
# answer is known before rendering: a panel of half-width r at height H over an
# occluder of half-width a at height h puts the umbra edge at
# r + (a - r)*H/(H - h) and the fully-lit edge at -r + (a + r)*H/(H - h), so the
# penumbra is a band of width 2*r*h/(H - h) centred between them.
#
# This is the only image gate in the repo with an analytic answer rather than a
# stored reference, which is the whole reason it exists: every area-lit golden is
# currently a reference for a bug (spec 10.3), so none of them can arbitrate a
# change to the shadow projection.
#
# The four geometry numbers mirror gen_area_shadow_fixture.py and have to match
# it or the gate predicts a penumbra the scene does not cast; the camera comes
# from the scene file.
PENUMBRA = _cscn_camera(
    "area_shadow_fixture.cscn",
    panel_half=0.3, panel_h=3.0, occluder_half=0.5, occluder_h=1.0,
)
# What the CENTRE of the transition is allowed to drift, in world units. It is a
# geometric fact, so every phase must hold it; only the WIDTH is expected to move
# (it is ~0.01 with a hard 3x3 filter and should approach the analytic band once
# a source-sized penumbra lands).
PENUMBRA_CENTRE_TOL = 0.01


def _penumbra_edges():
    p = PENUMBRA
    k = p["panel_h"] / (p["panel_h"] - p["occluder_h"])
    inner = p["panel_half"] + k * (p["occluder_half"] - p["panel_half"])
    outer = -p["panel_half"] + k * (p["occluder_half"] + p["panel_half"])
    return inner, outer


def _projector(cam, w, h):
    """World -> pixel for a fixture camera dict (eye/target/fovy_deg).

    cglm's perspective fov is VERTICAL.
    """
    def sub(a, b):
        return tuple(x - y for x, y in zip(a, b))

    def norm(v):
        m = math.sqrt(sum(c * c for c in v))
        return tuple(c / m for c in v)

    def cross(a, b):
        return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])

    def dot(a, b):
        return sum(x * y for x, y in zip(a, b))

    eye, target = cam["eye"], cam["target"]
    fwd = norm(sub(target, eye))
    right = norm(cross(fwd, (0.0, 1.0, 0.0)))
    up = cross(right, fwd)
    ty = 1.0 / math.tan(math.radians(cam["fovy_deg"]) * 0.5)
    tx = ty / (w / h)

    def project(p):
        d = sub(p, eye)
        vx, vy, vz = dot(d, right), dot(d, up), -dot(d, fwd)
        return (((vx * tx) / -vz * 0.5 + 0.5) * w, (0.5 - (vy * ty) / -vz * 0.5) * h)

    return project


def _crossing(xs, vals, umbra, lit, frac):
    """First up-crossing of umbra + frac*(lit-umbra), lerped between samples."""
    t = umbra + frac * (lit - umbra)
    for i in range(1, len(vals)):
        if vals[i - 1] < t <= vals[i]:
            a = (t - vals[i - 1]) / (vals[i] - vals[i - 1])
            return xs[i - 1] + a * (xs[i] - xs[i - 1])
    return float("nan")


def _read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    fields, i = [], 0
    while len(fields) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    return int(fields[1]), int(fields[2]), data[i + 1:]


def _linear_luma(pix, w, h, px, py):
    """Undo the sRGB encode -- the transition is measured on a linear signal."""
    x = max(0, min(w - 1, int(round(px))))
    y = max(0, min(h - 1, int(round(py))))
    o = (y * w + x) * 3
    total = 0.0
    for k in range(3):
        c = pix[o + k] / 255.0
        total += c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    return total / 3.0


def _linear_rgb(pix, w, h, px, py):
    """Per-channel sibling of _linear_luma; the reddening assertion needs the
    channels apart, and an average would hide exactly what it is looking for."""
    x = max(0, min(w - 1, int(round(px))))
    y = max(0, min(h - 1, int(round(py))))
    o = (y * w + x) * 3
    out = []
    for k in range(3):
        c = pix[o + k] / 255.0
        out.append(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4)
    return out


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
    scene = os.path.join(ROOT, "assets", "skin_curvature_fixture.cscn")
    # --no-sss isolates the analytic falloff from the screen-space blur, which is
    # the only way to attribute what is measured. --no-shadows because the far
    # side of a convex caster is in its own shadow, so shadows multiply the whole
    # band by zero. --no-bloom because bloom smears the band being measured.
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "--no-auto-exposure", "-E", "1.0",
           "--no-sss", "--no-shadows", "--no-bloom", "-W", "800", "-H", "500",
           "-S", out] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
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
        scene = os.path.join(ROOT, "assets", scene_name)
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
    fixture = os.path.join(ROOT, "assets", "skin_curvature_fixture.cscn")
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
    scene = os.path.join(ROOT, "assets", "skin_curvature_fixture.cscn")
    out = os.path.join(workdir, f"skinm_{tag}.ppm")
    # --no-shadows so the far side is not multiplied away, --no-bloom so nothing
    # smears the falloff being measured.
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "--no-auto-exposure", "-E", "1.0",
           "--no-shadows", "--no-bloom", "-W", dims[0], "-H", dims[1], "-S", out] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
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
    scene = os.path.join(ROOT, "assets", "skin_curvature_fixture.cscn")
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
    scene = os.path.join(ROOT, "assets", "skin_curvature_fixture.cscn")
    if not os.path.exists(scene):
        print("  sss-band     SKIP  (missing skin_curvature_fixture.cscn)")
        return []

    out = os.path.join(workdir, "sssband.ppm")
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "--no-auto-exposure", "-E", "1.0",
           "--no-shadows", "--no-bloom", "-W", "1200", "-H", "750", "-S", out]
    r = subprocess.run(cmd, capture_output=True, text=True)
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
    scene = os.path.join(ROOT, "assets", "skin_curvature_fixture.cscn")
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
    scene = os.path.join(ROOT, "assets", "skin_curvature_fixture.cscn")
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


def run_penumbra_gate(workdir):
    fixture = os.path.join(ROOT, "assets", "area_shadow_fixture.gltf")
    if not os.path.exists(fixture):
        print("  penumbra     SKIP  (missing area_shadow_fixture.gltf)")
        return []

    # Rendered twice, and the shadowed frame is DIVIDED by the unshadowed one.
    # Measuring the shadowed frame alone takes its "lit" reference from the
    # brightest sample on the scan, but the panel's own falloff varies across
    # that scan, so the reference is wrong everywhere except at one point. On a
    # narrow transition that hardly matters; on a 0.3-wide band it moved the
    # apparent centre by 0.06 -- an artifact of the measurement, read as a bias
    # in the shadow. The ratio is the shadow term on its own, flat 0..1.
    frames = {}
    for tag, extra in (("shadow", []), ("nolight", ["--no-shadows"])):
        out = os.path.join(workdir, f"penumbra_{tag}.ppm")
        cmd = [RENDER, "-m", fixture, "-x", "-f", "30", "--no-auto-exposure", "-E", "1.0",
               "-W", "800", "-H", "600", "-S", out] + extra
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            print(f"  penumbra     ERROR while rendering the fixture ({tag})")
            return ["penumbra"]
        frames[tag] = _read_ppm(out)

    w, h, pix = frames["shadow"]
    _, _, ref = frames["nolight"]
    project = _projector(PENUMBRA, w, h)
    inner, outer = _penumbra_edges()

    # Scan world x along +X at z=0 on the ground, well outside the band both ways.
    xs = [inner - 0.3 + i * 0.002 for i in range(int((outer - inner + 0.6) / 0.002))]
    vals = []
    for x in xs:
        px, py = project((x, 0.0, 0.0))
        lit_here = _linear_luma(ref, w, h, px, py)
        vals.append(_linear_luma(pix, w, h, px, py) / lit_here if lit_here > 1e-4 else 1.0)
    umbra, lit = min(vals), max(vals)
    if lit - umbra < 0.5:
        print(f"  penumbra     ERROR no shadow edge found (umbra {umbra:.3f}, lit {lit:.3f})")
        return ["penumbra"]

    lo = _crossing(xs, vals, umbra, lit, 0.10)
    mid = _crossing(xs, vals, umbra, lit, 0.50)
    hi = _crossing(xs, vals, umbra, lit, 0.90)
    want_centre = 0.5 * (inner + outer)
    ok = abs(mid - want_centre) <= PENUMBRA_CENTRE_TOL
    print(f"  penumbra     {'PASS' if ok else 'FAIL'}  centre {mid:.4f} "
          f"(want {want_centre:.4f} +/-{PENUMBRA_CENTRE_TOL}), "
          f"10-90 width {hi - lo:.4f} (analytic {outer - inner:.4f})")
    return [] if ok else ["penumbra"]


# Cascade-shadow gates (spec 10.5). Same design as the penumbra gate: geometry
# whose answer is computable before rendering, measured on the shadow term alone
# (shadowed frame divided by a --no-shadows frame).
#
# Mirrors assets/dir_shadow_fixture.cscn / _lowsun.cscn / gen_dir_shadow_fixture.py
# -- these numbers and the camera have to match the fixtures or the gate measures
# a different scene than it predicts. The sun is at azimuth 0 (travel -Z), so a
# sphere of radius r centred at (cx, h, 0) shadows the ellipse
#     centre (cx, -h/tan E), semi-axes r (x) and r/sin E (z).
DIR_SHADOW = {
    "r": 0.5,
    "float_c": (0.0, 1.5, 0.0),   # clean-geometry control: edge position
    "rest_c": (1.6, 0.5, 0.0),    # ground-tangent defect subject: holes
    "elev_deg": 40.0,  # the lowsun variant's 10 degrees lives only in its .cscn:
                       # the acne strip is the sole low-sun measurement and it
                       # makes no positional prediction
    # Elevated and OFF-AXIS, and that is load-bearing: from a shallow +Z
    # camera the floating sphere occludes the far half of its own umbra, and
    # every sample there silently reads the sphere's lit front face instead
    # of ground -- an entire debugging session was spent on that phantom
    # before the guards below existed. Two guards keep it honest now: samples
    # whose sight line passes within CLEARANCE of a sphere are skipped
    # geometrically, and _term_reader raises on any sample whose reference
    # luma is not ground.
    "eye": (6.5, 8.0, 2.5), "target": (0.8, 0.0, -1.0), "fovy_deg": 55.0,
    "clearance": 0.55,  # sight-line miss distance vs a sphere centre
    # Ground clear of every shadow at BOTH suns (pillar band lives at x=-3.5,
    # the ellipses end at x=2.1) and comfortably in-frame from the off-axis
    # camera below.
    "strip_x": (2.6, 4.5), "strip_z": (-2.5, 0.0),
}
# Umbra samples stay at 75% of the semi-axes: the margin covers the coarse
# sphere's silhouette chord error (~4mm) plus a PCF footprint at map texel
# scale (~17mm), so a sample can only read lit through a genuine hole.
DIR_ERODE = 0.75
DIR_HOLE_MAX = 0.02   # umbra term; a single authored light means true umbra ~ 0
DIR_ACNE_TOL = 0.05   # |term - 1| on bare ground; acne and false-dark are 10x+
DIR_EDGE_TOL = 0.05   # world units; ~3 outermost texels
# Churn bound: TAA-jittered frames never repeat byte-for-byte, so the shadowed
# sequence is judged against the --no-shadows run of the same scene. 2x
# absorbs the shadow term legitimately participating in the jittered edges it
# darkens; the +100 px absorbs count noise on a near-zero floor.
DIR_CHURN_FACTOR = 2
DIR_CHURN_SLACK_PX = 100
# The wall-base line in cornell_leak at the framing below, as frame fractions.
# Nothing occludes this stretch of the left wall's base; the punctual near-side
# work darkens it to ~0.52 (the wedge spec 10.5 records).
LEAK_CAM = ["--cam-eye", "-0.832,0.062,1.232", "--cam-target", "-1.068,0.045,0.070"]
LEAK_LINE = ((0.08, 0.77), (0.48, 0.59))
LEAK_TOL = 0.05


def _dir_ellipse(centre, elev_deg):
    e = math.radians(elev_deg)
    cx, cz = centre[0], centre[2] - centre[1] / math.tan(e)
    return cx, cz, DIR_SHADOW["r"], DIR_SHADOW["r"] / math.sin(e)


def _dir_visible(p):
    """Can the gate camera actually see ground point p, or does a sphere sit on
    the sight line? Umbra points hugging a ground-tangent caster are optically
    inaccessible from ANY camera; they are skipped, not mismeasured."""
    eye = DIR_SHADOW["eye"]
    d = tuple(b - a for a, b in zip(eye, p))
    dlen2 = sum(c * c for c in d)
    for centre in (DIR_SHADOW["float_c"], DIR_SHADOW["rest_c"]):
        w = tuple(c - e for c, e in zip(centre, eye))
        t = max(0.0, min(1.0, sum(a * b for a, b in zip(w, d)) / dlen2))
        closest = tuple(e + t * c for e, c in zip(eye, d))
        dist2 = sum((a - b) ** 2 for a, b in zip(closest, centre))
        if dist2 < DIR_SHADOW["clearance"] ** 2:
            return False
    return True


def _dir_render(workdir, scene, tag, extra):
    out = os.path.join(workdir, f"dir_{tag}.ppm")
    cmd = [RENDER, "-m", os.path.join(ROOT, "assets", scene), "-x", "-f", "30",
           "--no-auto-exposure", "-E", "1.0", "-W", "800", "-H", "600", "-S", out] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    return _read_ppm(out)


def _term_reader(shadow_frame, ref_frame):
    """Shadow term at a world GROUND point: shadowed luma / unshadowed luma.

    Occlusion guard: the reference luma at every sampled pixel must sit in one
    band -- the ground is a single flat Lambertian material, so its unshadowed
    luma is nearly constant across the frame. A pixel whose reference luma
    falls outside the band is NOT ground (a caster stands between the camera
    and the sample point; the spheres' albedo is half the ground's), and
    trusting it produces phantom measurements. Such samples raise instead.
    """
    w, h, pix = shadow_frame
    _, _, ref = ref_frame
    project = _projector(DIR_SHADOW, w, h)
    # Calibrate the ground band from a point that is bare ground by
    # construction (inside the acne strip).
    gx = 0.5 * (DIR_SHADOW["strip_x"][0] + DIR_SHADOW["strip_x"][1])
    gz = 0.5 * (DIR_SHADOW["strip_z"][0] + DIR_SHADOW["strip_z"][1])
    px, py = project((gx, 0.0, gz))
    ground_lit = _linear_luma(ref, w, h, px, py)

    def term(p):
        px, py = project(p)
        lit = _linear_luma(ref, w, h, px, py)
        if not (0.6 * ground_lit <= lit <= 1.6 * ground_lit):
            raise ValueError(
                "sample %r reads reference luma %.4f vs ground %.4f -- occluded "
                "from this camera, fixture/gate geometry disagree" % (p, lit, ground_lit))
        return _linear_luma(pix, w, h, px, py) / lit

    return term


# The punctual grazing gate: cornell_leak's wall base, which nothing occludes,
# must read fully lit. Its own gate rather than a dir-shadow sub-gate: it
# measures the PUNCTUAL path (the wedge was near-side fallout fixed in the
# reference implementation before the cascade port could copy it, spec 10.5
# phase 2a), so it must not vanish behind a missing cascade fixture.
def run_grazing_gate(workdir):
    leak = os.path.join(ROOT, "assets", "cornell_leak.gltf")
    if not os.path.exists(leak):
        print("  grazing      SKIP  (missing cornell_leak.gltf)")
        return []

    shadow = _dir_render(workdir, "cornell_leak.gltf", "leak", LEAK_CAM)
    ref = _dir_render(workdir, "cornell_leak.gltf", "leak_ns", LEAK_CAM + ["--no-shadows"])
    if not (shadow and ref):
        print("  grazing      ERROR while rendering cornell_leak")
        return ["grazing"]

    w, h, pix = shadow
    _, _, refpix = ref
    worst = 1.0
    (x0, y0), (x1, y1) = LEAK_LINE
    for i in range(41):
        t = i / 40.0
        px = (x0 + (x1 - x0) * t) * w
        py = (y0 + (y1 - y0) * t) * h
        lit = _linear_luma(refpix, w, h, px, py)
        if lit > 1e-4:
            worst = min(worst, _linear_luma(pix, w, h, px, py) / lit)
    ok = worst >= 1.0 - LEAK_TOL
    print(f"  grazing      {'PASS' if ok else 'FAIL'}  wall-base term {worst:.4f} "
          f"(want >= {1.0 - LEAK_TOL})")
    return [] if ok else ["grazing"]


def _taa_churn(workdir, fixture, tag, extra):
    """AE between frames 90 and 120 of one static-camera TAA run (None on error)."""
    base = os.path.join(workdir, f"churn_{tag}.ppm")
    cmd = [RENDER, "-m", fixture, "-x", "-f", "120", "--no-auto-exposure", "-E", "1.0",
           "--taa", "--headless-jitter", "--screenshot-every", "30",
           "-W", "800", "-H", "600", "-S", base] + extra
    subprocess.run(cmd, capture_output=True, text=True)
    f90 = base[:-4] + "_000090.ppm"
    f120 = base[:-4] + "_000120.ppm"
    if not (os.path.exists(f90) and os.path.exists(f120)):
        return None
    return compare(f90, f120)[0]


# The shadow catcher's virtual floor is auto-enabled by every sky-lit scene and
# sits at y=0 -- exactly where a scene that ships its own ground puts it. The
# catcher must never win the depth race against that real ground: when it does,
# it re-stamps its own flat-bias shadow term over already-shaded pixels in
# jitter-dependent patches (long rectangular streaks that flicker under TAA).
# contact_fixture is the suite's only sky+catcher+real-ground scene, so the
# check lives here; the authored-light fixtures have no catcher and cannot
# measure it. Default settings deliberately (PCSS on, full post): the defect
# was invisible to every gate that pins the pipeline down.
def run_catcher_gate(workdir):
    fixture = os.path.join(ROOT, "assets", "contact_fixture.cscn")
    if not os.path.exists(fixture):
        print("  catcher      SKIP  (missing contact_fixture.cscn)")
        return []
    shadowed = _taa_churn(workdir, fixture, "catch", [])
    floor = _taa_churn(workdir, fixture, "catch_ns", ["--no-shadows"])
    if shadowed is None or floor is None:
        print("  catcher      ERROR while rendering the TAA sequences")
        return ["catcher-churn"]
    ok = shadowed <= floor * DIR_CHURN_FACTOR + DIR_CHURN_SLACK_PX
    print(f"  catcher      {'PASS' if ok else 'FAIL'}  {shadowed} px frame-to-frame "
          f"(no-shadow floor {floor} px)")
    return [] if ok else ["catcher-churn"]


# Cloud-layer steady-state churn (spec 11.0). The march's rotating dither is
# resolved by its own ray-direction accumulation; what leaks past it into the
# frame must stay near the no-clouds floor or the sky shimmers under TAA.
# Report-only for its first cycle: the bound (first measurement x1.5, on a
# floor measured fresh each run) becomes blocking once a cycle of history
# shows it stable. NB _taa_churn compares frames THIRTY apart, where the
# non-repeating dither sequence touches ~6x more pixels than adjacent-frame
# churn does -- at RMSE ~3e-4 amplitude (phase 3's adjacent-frame number).
# The ERROR path blocks even while the threshold only reports: broken render
# infrastructure and a drifting churn number are different failures.
CLOUD_CHURN_FACTOR = 9.7  # 153015/23656 measured at phase 5, x1.5 headroom


def run_cloud_churn_gate(workdir):
    fixture = os.path.join(ROOT, "assets", "aerial_fixture.gltf")
    if not os.path.exists(fixture):
        print("  clouds       SKIP  (missing aerial_fixture.gltf)")
        return []
    clouded = _taa_churn(workdir, fixture, "cloud", ["--clouds"])
    floor = _taa_churn(workdir, fixture, "cloud_off", [])
    if clouded is None or floor is None:
        print("  clouds       ERROR while rendering the TAA sequences")
        return ["cloud-churn"]
    ok = clouded <= floor * CLOUD_CHURN_FACTOR + DIR_CHURN_SLACK_PX
    print(f"  clouds       {'PASS' if ok else 'REPORT'}  {clouded} px frame-to-frame "
          f"(no-cloud floor {floor} px)")
    return []  # report-only this cycle; flip to failures on the next


# Order-independent transparency (spec 11.17), measured on assets/oit_cards_fixture.
#
# Twelve emissive cards of one alpha over an opaque backdrop, arranged so a
# horizontal scan crosses regions of exactly 0, 1, ... 12 layers with each step
# adding one card IN FRONT. The correct composite of band b is then the front-to-
# back recursion S_b = C*a + (1-a)*S_(b-1) with no free parameters -- the card
# colours are the authored emissive constants, the backdrop is its own, and the
# tonemap and display gamma are inverted exactly (the fixture keeps every value
# inside the Neutral curve's affine stretch to make that possible).
#
# So this gate does not compare OIT against a stored image or against another
# OIT. It compares it against the arithmetic, and the sorted render is checked
# against the same arithmetic first, which is what earns it the right to be
# called ground truth.
#
# Why a new fixture at all: weighted-blended OIT is EXACT whenever every layer
# carries the same colour, at any weights, because the accumulator divides by its
# own alpha sum. oit_fixture is three near-identical quads, so it cannot separate
# any two OIT schemes and never could. Colour varying WITH depth is what
# discriminates -- see the header of assets/gen_oit_cards_fixture.py.
#
# Mirrors that generator's band layout; the colours and alpha are read from the
# .gltf so the one thing that cannot be checked by inspection cannot drift.
OIT_BAND_LEFT = 0.06
OIT_BAND_RIGHT = 0.94
# Fresnel-boosted opacity: pbr_frag mixes the authored alpha toward 1 by the
# Fresnel term, so the alpha that actually composites is not the authored one.
# The cards face the camera and the bands are sampled on the horizon line, which
# holds (1 - NdotV)^5 below 1e-5 -- so only the normal-incidence F0 survives, and
# the effective alpha is a constant rather than a per-pixel function.
OIT_IOR = 1.5

# What the SORTED render is allowed to deviate from the arithmetic. It measures
# 0.0026, and one 8-bit step is 0.0044 at the brightest band, so this is under
# two quantization steps -- and 25x below the error weighted-blended OIT commits
# on the same frame, which is the number it has to be small against.
OIT_TRUTH_TOL = 0.008
# The inverse half. If the fixture did not separate a real OIT approximation from
# the truth by a wide margin, then a later scheme matching the truth would prove
# nothing about the scheme. Weighted-blended measures 0.077 RMS here.
OIT_DISCRIMINATION_MIN = 0.02
# How much of weighted-blended's error moment weighting has to remove. It removes
# 4.9x of it (0.0773 -> 0.0157 RMS), which is the whole justification for the
# feature, so the bar is a ratio rather than an absolute: an absolute one would
# have to be re-derived every time the fixture's alpha or layer count changed.
#
# 2.5x, comfortably under the measurement and comfortably over "indistinguishable".
# The residual that survives is the method's own conservatism -- the
# reconstruction returns a LOWER bound on the absorbance in front, so twelve
# layers over a 25:1 depth range come out slightly under-occluded no matter how
# the moments are stored.
OIT_MOMENT_GAIN_MIN = 2.5


def _oit_fixture_materials():
    """(background rgb, [(card rgb, alpha)]) straight out of the fixture."""
    path = os.path.join(ROOT, "assets", "oit_cards_fixture.gltf")
    with open(path) as f:
        mats = json.load(f)["materials"]
    bg = mats[0]["emissiveFactor"]
    cards = [(m["emissiveFactor"], m["pbrMetallicRoughness"]["baseColorFactor"][3])
             for m in mats[1:]]
    return bg, cards


def _oit_truth(bg, cards):
    """Correct front-to-back composite of every band, in linear HDR."""
    f0 = ((OIT_IOR - 1.0) / (OIT_IOR + 1.0)) ** 2
    out = [list(bg)]
    for color, alpha in cards:
        a = alpha + (1.0 - alpha) * f0
        out.append([color[c] * a + (1.0 - a) * out[-1][c] for c in range(3)])
    return out


def _oit_untonemap(rgb):
    """Display sample -> linear HDR, inverting displayEncode and Neutral.

    Neutral subtracts an offset driven by the sample's MINIMUM channel: a flat
    0.04 above a 0.08 toe, and 6.25x^2 below it. Both branches invert in closed
    form, and the fixture's colours are chosen to stay in the first one -- the
    second is here so a future colour edit fails loudly rather than quietly
    reading 4% low.

    pow(2.2), NOT the sRGB piecewise curve _linear_rgb uses: tonemap_frag's
    displayEncode is a plain 1/2.2 gamma, so this is the one that matches the
    engine. The two differ by ~0.01 in the shadows, which matters here because
    this gate compares absolute radiance against arithmetic rather than
    comparing two samples of one render. Reconciling _linear_rgb would re-tune
    every threshold fitted through it, so the divergence stands and is recorded
    rather than silently inherited.
    """
    ldr = [max(0.0, min(1.0, c)) ** 2.2 for c in rgb]
    m = min(ldr)
    offset = 0.4 * math.sqrt(m) - m if m < 0.04 else 0.04
    return [c + offset for c in ldr]


def _oit_bands(path, count):
    w, h, pix = _read_ppm(path)
    width = (OIT_BAND_RIGHT - OIT_BAND_LEFT) / (count + 1)
    out = []
    for b in range(count + 1):
        u = OIT_BAND_LEFT + (b + 0.5) * width
        x = max(0, min(w - 1, int(round(u * w))))
        y = h // 2
        o = (y * w + x) * 3
        out.append(_oit_untonemap([pix[o + k] / 255.0 for k in range(3)]))
    return out


def _oit_error(path, truth):
    """(worst channel deviation, RMS) of a render against the arithmetic."""
    meas = _oit_bands(path, len(truth) - 1)
    diffs = [abs(meas[b][c] - truth[b][c]) for b in range(len(truth)) for c in range(3)]
    return max(diffs), math.sqrt(sum(d * d for d in diffs) / len(diffs))


def _oit_render(workdir, tag, extra):
    out = os.path.join(workdir, f"oit_{tag}.ppm")
    scene = os.path.join(ROOT, "assets", "oit_cards_fixture.cscn")
    # --no-vignette is load-bearing, not tidiness: the default vignette is on and
    # radial, so it would darken the outer bands by a fraction of the very error
    # being measured. The rest keeps anything that could add to a flat emissive
    # surface out of the frame.
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "-W", "800", "-H", "500",
           "--no-auto-exposure", "-E", "1.0", "--no-vignette", "--no-bloom",
           "--no-ssao", "--no-ssr", "-S", out] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    return out


def run_oit_gate(workdir):
    fixture = os.path.join(ROOT, "assets", "oit_cards_fixture.cscn")
    if not os.path.exists(fixture):
        print("  oit-cards    SKIP  (missing oit_cards_fixture.cscn)")
        return []
    bg, cards = _oit_fixture_materials()
    truth = _oit_truth(bg, cards)

    # Every arm names its weighting explicitly. --oit alone would inherit
    # whichever one is default, so the weighted arm has to say --no-oit-moments
    # or it silently measures the moment path against itself.
    sorted_frame = _oit_render(workdir, "sorted", ["--no-oit"])
    weighted = _oit_render(workdir, "weighted", ["--oit", "--no-oit-moments"])
    moments = _oit_render(workdir, "moments", ["--oit-moments"])
    if sorted_frame is None or weighted is None or moments is None:
        print("  oit-cards    ERROR while rendering the card stack")
        return ["oit-cards"]

    fails = []
    worst, rms = _oit_error(sorted_frame, truth)
    ok = worst <= OIT_TRUTH_TOL
    print(f"  oit-sorted   {'PASS' if ok else 'FAIL'}  back-to-front vs arithmetic: "
          f"worst {worst:.5f} rms {rms:.5f} (want worst <= {OIT_TRUTH_TOL})")
    if not ok:
        fails.append("oit-cards")

    w_worst, w_rms = _oit_error(weighted, truth)
    ok = w_rms >= OIT_DISCRIMINATION_MIN
    print(f"  oit-weighted {'PASS' if ok else 'FAIL'}  weighted-blended vs arithmetic: "
          f"worst {w_worst:.5f} rms {w_rms:.5f} "
          f"(want rms >= {OIT_DISCRIMINATION_MIN}, else the fixture cannot discriminate)")
    if not ok:
        fails.append("oit-cards")

    m_worst, m_rms = _oit_error(moments, truth)
    gain = w_rms / m_rms if m_rms > 0 else float("inf")
    ok = gain >= OIT_MOMENT_GAIN_MIN
    print(f"  oit-moments  {'PASS' if ok else 'FAIL'}  moment-weighted vs arithmetic: "
          f"worst {m_worst:.5f} rms {m_rms:.5f}, {gain:.2f}x closer than weighted-blended "
          f"(want >= {OIT_MOMENT_GAIN_MIN}x)")
    if not ok:
        fails.append("oit-cards")
    return fails


# Shadow catcher vs transparency (spec 11.18), on assets/catcher_transparency_fixture.
#
# The catcher is a shadow decal at y=0 that also writes depth. Until 11.18 it
# drew last, so nothing was ever ordered against that depth and translucent
# geometry BEHIND the plane drew over the shadow instead of being hidden by the
# floor. Moving it ahead of the transparent pass fixes the OIT accumulate, the
# unsorted late pass and the particle depth resolve together, because all three
# read the same buffer.
#
# The fixture puts a vertical translucent panel through the plane, so one column
# of pixels crosses from in-front to behind. Measured against a second column
# beside it, which sees the same flat backdrop at both heights -- that is what
# the fixture's opaque wall is for, and it is why this needs no second render
# and no stored reference.
#
# The sample columns are this gate's own; the camera comes from the scene file.
CATCHER_TRANSPARENCY = _cscn_camera(
    "catcher_transparency_fixture.cscn",
    panel_x=1.2,   # inside the panel, which spans x 0.2 .. 2.2
    beside_x=3.0,  # clear of the panel, its cast shadow, and the caster's
    sample_y=0.7,  # sampled at +/- this, both well clear of the plane
)
# What "the floor hid it" is allowed to measure. Both samples read the same
# backdrop texel once the panel is occluded, so the honest bar is quantization:
# one 8-bit step is about 0.005 at this brightness, so this is two of them.
CATCHER_HIDDEN_TOL = 0.010
# The inverse half. Without it the gate passes on any frame where the panel
# failed to render, or where the camera stopped framing it. Measured 0.131.
CATCHER_VISIBLE_MIN = 0.05


def _catcher_samples(path):
    """((above on-panel, above beside), (below on-panel, below beside)) in linear RGB."""
    c = CATCHER_TRANSPARENCY
    w, h, pix = _read_ppm(path)
    project = _projector(c, w, h)

    def at(x, y):
        return _linear_rgb(pix, w, h, *project((x, y, 0.0)))

    return [(at(c["panel_x"], sy), at(c["beside_x"], sy))
            for sy in (c["sample_y"], -c["sample_y"])]


def _catcher_render(workdir, tag, extra):
    out = os.path.join(workdir, f"catchertr_{tag}.ppm")
    scene = os.path.join(ROOT, "assets", "catcher_transparency_fixture.cscn")
    # --no-recenter is the fixture's whole premise: the app lifts a model's
    # bounding-box base onto y=0, and the geometry under test is the half of the
    # panel BELOW y=0. Recentred, there is nothing left to measure.
    cmd = [RENDER, "-m", scene, "--no-recenter", "-x", "-f", "30", "-W", "800", "-H", "500",
           "--no-vignette", "-S", out] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    return out


def run_catcher_transparency_gate(workdir):
    fixture = os.path.join(ROOT, "assets", "catcher_transparency_fixture.cscn")
    if not os.path.exists(fixture):
        print("  catcher-tr   SKIP  (missing catcher_transparency_fixture.cscn)")
        return []
    fails = []
    # Both transparency paths, because the fix is one ordering change that
    # serves both and a regression could easily reach only one of them.
    for tag, label, extra in (("oit", "moment OIT", []),
                              ("late", "unsorted late pass", ["--no-oit"])):
        frame = _catcher_render(workdir, tag, extra)
        if frame is None:
            print(f"  catcher-tr   ERROR while rendering the {label} frame")
            fails.append("catcher-transparency")
            continue
        (above_on, above_off), (below_on, below_off) = _catcher_samples(frame)
        visible = max(abs(a - b) for a, b in zip(above_on, above_off))
        hidden = max(abs(a - b) for a, b in zip(below_on, below_off))
        ok_v = visible >= CATCHER_VISIBLE_MIN
        ok_h = hidden <= CATCHER_HIDDEN_TOL
        print(f"  catcher-tr   {'PASS' if ok_v and ok_h else 'FAIL'}  {label}: "
              f"above the plane {visible:.4f} (want >= {CATCHER_VISIBLE_MIN}, the panel is there), "
              f"below it {hidden:.4f} (want <= {CATCHER_HIDDEN_TOL}, the floor hides it)")
        if not (ok_v and ok_h):
            fails.append("catcher-transparency")
    return fails


def run_dir_shadow_gate(workdir):
    fixture = os.path.join(ROOT, "assets", "dir_shadow_fixture.cscn")
    if not os.path.exists(fixture):
        print("  dir-shadow   SKIP  (missing dir_shadow_fixture.cscn)")
        return []
    failures = []

    # --- fixture renders ----------------------------------------------------
    # --no-pcss for every positional measure: PCSS is on by default with an
    # emitter of scene_radius * 0.08, which smears the analytic edge.
    hard = ["--no-pcss"]
    r40 = _dir_render(workdir, "dir_shadow_fixture.cscn", "40", hard)
    r40_ns = _dir_render(workdir, "dir_shadow_fixture.cscn", "40_ns", hard + ["--no-shadows"])
    r40_cc1 = _dir_render(workdir, "dir_shadow_fixture.cscn", "40_cc1",
                          hard + ["--shadow-cascades", "1"])
    r10 = _dir_render(workdir, "dir_shadow_fixture_lowsun.cscn", "10", hard)
    r10_ns = _dir_render(workdir, "dir_shadow_fixture_lowsun.cscn", "10_ns",
                         hard + ["--no-shadows"])
    if not all((r40, r40_ns, r40_cc1, r10, r10_ns)):
        print("  dir-shadow   ERROR while rendering the fixture")
        return failures + ["dir-shadow"]

    term40 = _term_reader(r40, r40_ns)
    term40_cc1 = _term_reader(r40_cc1, r40_ns)
    term10 = _term_reader(r10, r10_ns)

    # --- holes: eroded umbra of both ellipses must be dark ------------------
    try:
        worst_hole = 0.0
        measured = skipped = 0
        for centre in (DIR_SHADOW["float_c"], DIR_SHADOW["rest_c"]):
            cx, cz, sx, sz = _dir_ellipse(centre, DIR_SHADOW["elev_deg"])
            for iu in range(-4, 5):
                for iv in range(-4, 5):
                    u, v = iu / 4.0, iv / 4.0
                    if u * u + v * v > 1.0:
                        continue
                    p = (cx + DIR_ERODE * sx * u, 0.0, cz + DIR_ERODE * sz * v)
                    if not _dir_visible(p):
                        skipped += 1
                        continue
                    worst_hole = max(worst_hole, term40(p))
                    measured += 1
        if measured < 30:
            # No silent caps: enough of the umbra must actually be measured.
            print(f"  hole         ERROR only {measured} umbra samples visible "
                  f"({skipped} occluded)")
            failures.append("dir-hole")
        else:
            ok = worst_hole <= DIR_HOLE_MAX
            print(f"  hole         {'PASS' if ok else 'FAIL'}  worst umbra term {worst_hole:.4f} "
                  f"over {measured} samples, {skipped} caster-occluded (want <= {DIR_HOLE_MAX})")
            if not ok:
                failures.append("dir-hole")
    except ValueError as e:
        print(f"  hole         ERROR {e}")
        failures.append("dir-hole")

    # --- acne: bare ground must divide to 1, at both elevations -------------
    for label, term in (("40deg", term40), ("10deg", term10)):
        try:
            worst = 0.0
            (xa, xb), (za, zb) = DIR_SHADOW["strip_x"], DIR_SHADOW["strip_z"]
            steps = 10
            for ix in range(steps + 1):
                for iz in range(steps + 1):
                    p = (xa + (xb - xa) * ix / steps, 0.0, za + (zb - za) * iz / steps)
                    worst = max(worst, abs(term(p) - 1.0))
            ok = worst <= DIR_ACNE_TOL
            print(f"  acne-{label:<7} {'PASS' if ok else 'FAIL'}  worst |term-1| {worst:.4f} "
                  f"(want <= {DIR_ACNE_TOL})")
            if not ok:
                failures.append(f"dir-acne-{label}")
        except ValueError as e:
            print(f"  acne-{label:<7} ERROR {e}")
            failures.append(f"dir-acne-{label}")

    # --- edge position: 50% crossing of the floating sphere's near edge -----
    cx, cz, sx, sz = _dir_ellipse(DIR_SHADOW["float_c"], DIR_SHADOW["elev_deg"])
    want_edge = cz + sz
    for label, term in (("cc3", term40), ("cc1", term40_cc1)):
        zs = [want_edge - 0.5 + i * 0.002 for i in range(int(1.0 / 0.002))]
        try:
            vals = [term((cx, 0.0, z)) for z in zs]  # umbra -> lit as z rises
        except ValueError as e:
            print(f"  edge-{label:<7} ERROR {e}")
            failures.append(f"dir-edge-{label}")
            continue
        umbra, lit = min(vals), max(vals)
        if lit - umbra < 0.5:
            print(f"  edge-{label:<7} ERROR no shadow edge on the scan")
            failures.append(f"dir-edge-{label}")
            continue
        mid = _crossing(zs, vals, umbra, lit, 0.50)
        ok = abs(mid - want_edge) <= DIR_EDGE_TOL
        print(f"  edge-{label:<7} {'PASS' if ok else 'FAIL'}  crossing {mid:.4f} "
              f"(want {want_edge:.4f} +/-{DIR_EDGE_TOL})")
        if not ok:
            failures.append(f"dir-edge-{label}")

    # --- churn: shadows must not add temporal instability under TAA ---------
    # Static camera, jitter on (windowed parity). Frames 90 and 120 of the same
    # run are compared; the --no-shadows pair is the noise floor everything else
    # in the pipeline contributes, and the shadowed pair must stay at it.
    churn = {"taa": _taa_churn(workdir, fixture, "dir", hard),
             "taa_ns": _taa_churn(workdir, fixture, "dir_ns", hard + ["--no-shadows"])}
    if churn["taa"] is None or churn["taa_ns"] is None:
        print("  churn        ERROR while rendering the TAA sequences")
        failures.append("dir-churn")
    else:
        floor = churn["taa_ns"]
        ok = churn["taa"] <= floor * DIR_CHURN_FACTOR + DIR_CHURN_SLACK_PX
        print(f"  churn        {'PASS' if ok else 'FAIL'}  {churn['taa']} px frame-to-frame "
              f"(no-shadow floor {floor} px)")
        if not ok:
            failures.append("dir-churn")

    return failures


def run_range_gate():
    """glTF KHR_lights_punctual `range` must survive import unchanged."""
    fixture = os.path.join(ROOT, "assets", "point_import_fixture.gltf")
    if not os.path.exists(fixture):
        print("  gltf-range   SKIP  (missing point_import_fixture.gltf)")
        return []
    r = subprocess.run([RENDER, "-m", fixture, "-x", "-f", "2"],
                       capture_output=True, text=True)
    got = None
    for line in (r.stdout + r.stderr).splitlines():
        if "<Light name=" in line and "range=" in line:
            field = _light_field(line, "range")
            got = float(field) if field is not None else None
    if got is None:
        print("  gltf-range   ERROR (no light imported)")
        return ["gltf-range"]
    ok = abs(got - 25.0) < 1e-3
    print(f"  gltf-range   {'PASS' if ok else 'FAIL'}  authored 25.0, imported {got}")
    return [] if ok else ["gltf-range"]


def _import_log(fixture, extra=()):
    """Two-frame headless render, combined stdout+stderr (log_* goes to stderr)."""
    r = subprocess.run([RENDER, "-m", fixture, *extra, "-x", "-f", "2"],
                       capture_output=True, text=True)
    return r.stdout + r.stderr


def _fbx_light_line(output):
    """The unit_lamp <Light ...> print, or None."""
    for line in output.splitlines():
        if "<Light name='unit_lamp'" in line:
            return line
    return None


# The print_light format is the assertion surface, so a shifted format must
# report as a named FAIL, not a parser traceback: both extractors return None
# on any mismatch.
def _light_field_vec3(line, key):
    try:
        inner = line.split(key + "=(")[1].split(")")[0]
        return [float(v) for v in inner.split(",")]
    except (IndexError, ValueError):
        return None


def _light_field(line, key):
    try:
        return line.split(key + "=")[1].split(",")[0]
    except IndexError:
        return None


def run_fbx_unit_gate():
    """FBX unit scale must bake cm to metres, and the light must survive.

    Pins two things at once (spec 11.2, paying 11.1's recorded debt): the
    engine's mirror of assimp's UnitScaleFactor * 0.01 conversion (a
    vendored-assimp change that moved it would land here first), and the FBX
    light-import path itself -- the fixture's light must arrive at its exact
    metre position, at the intensity the conversion chain produces, and
    actually reach the clusterer (a culled light is the exact silently-black
    symptom of spec 11.1's c64). A second render with --no-unit-scale is the
    mechanical twin: the same file read raw must place the light at 200 cm.
    """
    fixture = os.path.join(ROOT, "assets", "fbx_unit_fixture.fbx")
    if not os.path.exists(fixture):
        print("  fbx-unit     SKIP  (missing fbx_unit_fixture.fbx)")
        return []

    out = _import_log(fixture)
    light = _fbx_light_line(out)
    if light is None:
        print("  fbx-unit     ERROR (no FBX light imported)")
        return ["fbx-unit"]

    problems = []
    if "file declares 0.01x, applied 0.01x" not in out:
        problems.append("unit-scale log line missing or wrong")
    pos = _light_field_vec3(light, "global_position")
    if pos is None:
        problems.append("light print carries no parseable position")
    elif abs(pos[0]) > 1e-3 or abs(pos[1] - 2.0) > 1e-3 or abs(pos[2]) > 1e-3:
        problems.append(f"position {pos} != (0, 2, 0) m")
    intensity = _light_field(light, "intensity")
    if intensity is None or not intensity.endswith(" cd"):
        problems.append("light print carries no candela intensity")
    elif abs(float(intensity[:-3]) - 50.0) > 1e-3:
        problems.append(f"intensity {intensity} != 50 cd")
    clusterable = None
    for line in out.splitlines():
        if "clustered:" in line and "directional +" in line:
            clusterable = int(line.split("directional +")[1].split("clusterable")[0])
            break  # logged once per build; the first match is the record
    if clusterable != 1:
        problems.append(f"clusterable count {clusterable} != 1 "
                        "(0 is the silently-black symptom, 2+ a double import)")

    raw_light = _fbx_light_line(_import_log(fixture, ["--no-unit-scale"]))
    raw_pos = _light_field_vec3(raw_light, "global_position") if raw_light else None
    if raw_pos is None:
        problems.append("--no-unit-scale render imported no parseable light")
    elif abs(raw_pos[1] - 200.0) > 1e-3:
        problems.append(f"--no-unit-scale position y {raw_pos[1]} != 200 cm")

    ok = not problems
    detail = "cm baked to (0, 2, 0) m at 50 cd, clustered" if ok else "; ".join(problems)
    print(f"  fbx-unit     {'PASS' if ok else 'FAIL'}  {detail}")
    return [] if ok else ["fbx-unit"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="keep the generated scenes and frames")
    args = ap.parse_args()

    if not os.path.exists(RENDER):
        sys.exit(f"{RENDER} not found -- run ./build.sh first")

    workdir = tempfile.mkdtemp(prefix="cetra_gates_")
    try:
        print("scale invariance (lights x1000, exposure /1000):")
        failures = run_scale_gates(workdir)
        print("area shadow (analytic penumbra):")
        failures += run_penumbra_gate(workdir)
        print("punctual grazing (leak wall base):")
        failures += run_grazing_gate(workdir)
        print("cascade shadow (analytic ellipse):")
        failures += run_dir_shadow_gate(workdir)
        print("catcher over a real ground (contact fixture):")
        failures += run_catcher_gate(workdir)
        print("catcher vs transparency (panel through the plane):")
        failures += run_catcher_transparency_gate(workdir)
        print("order-independent transparency (analytic card stack):")
        failures += run_oit_gate(workdir)
        print("cloud layer (steady-state churn, report-only):")
        failures += run_cloud_churn_gate(workdir)
        print("pre-integrated skin (off-path byte identity):")
        failures += run_skin_offpath_gate(workdir)
        print("pre-integrated skin (curvature ordering):")
        failures += run_skin_curvature_gate(workdir)
        print("pre-integrated skin (handoff past the scatter ceiling):")
        failures += run_skin_handoff_gate(workdir)
        print("subsurface blur (world width vs frame size):")
        failures += run_sss_invariance_gate(workdir)
        print("subsurface blur (kernel not visible as rings):")
        failures += run_sss_banding_gate(workdir)
        print("import:")
        failures += run_range_gate()
        failures += run_fbx_unit_gate()
    finally:
        if args.keep:
            print(f"\nartifacts in {workdir}")
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print(f"\n{len(failures)} gate(s) failed: {', '.join(failures)}")
        return 1
    print("\nall gates passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
