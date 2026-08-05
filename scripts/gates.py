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
# Mirrors assets/area_shadow_fixture.cscn and gen_area_shadow_fixture.py -- these
# five numbers and the camera below have to match the fixture or the gate is
# measuring a different scene than it is predicting.
PENUMBRA = {
    "panel_half": 0.3, "panel_h": 3.0, "occluder_half": 0.5, "occluder_h": 1.0,
    "eye": (0.0, 2.2, 3.2), "target": (0.0, 0.0, 0.0), "fovy_deg": 40.0,
}
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
# These numbers mirror the fixture and its .cscn. Change either and the gate
# measures a different scene than it predicts.
SKIN_CAM = {"eye": (-0.2, 0.93, 9.5), "target": (-0.2, 0.93, 0.0), "fovy_deg": 40.0}
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
    # disabled. It also has to run large: pre-integration only takes up the slack
    # the blur's pixel cap creates, so at a small render the cap never binds,
    # there is no slack, and the feature correctly contributes nothing. A gate
    # that asserted "> 0" at 400x300 would be asserting a bug.
    fixture = os.path.join(ROOT, "assets", "skin_curvature_fixture.cscn")
    if os.path.exists(fixture):
        a = os.path.join(workdir, "skinlive_a.ppm")
        b = os.path.join(workdir, "skinlive_b.ppm")
        big = ["-W", "1200", "-H", "750"]
        err = render(fixture, a, big)
        err = err or render(fixture, b, big + ["--no-skin-preint"])
        if err:
            print("  skin-live    ERROR while rendering the curvature fixture")
            fails.append("skin-offpath")
        else:
            ae, _ = compare(a, b)
            ok = ae > 0
            print(f"  skin-live    {'PASS' if ok else 'FAIL'}  opted-in fixture, full path: "
                  f"{ae} px (want > 0, else the three zeros above prove nothing)")
            if not ok:
                fails.append("skin-offpath")
    return fails


def _skin_terminator_width(pix, w, h, project, at, lit):
    """Terminator 90%-to-10% falloff, measured in SURFACE DEGREES.

    Sampling by angle rather than by pixel is what makes the number comparable
    across resolutions at all: a width in pixels doubles when the frame does,
    a width in degrees does not.
    """
    prev_a = prev_v = None
    hits = {}
    # Starts at the lit reference itself: by 50 degrees Lambert is already at
    # 0.68 of its 20-degree value, so a later start misses the 90% crossing.
    a = float(SKIN_LIT_DEG)
    while a <= 150.0:
        v = _linear_luma(pix, w, h, *project(at(a)))
        for frac in (0.9, 0.1):
            if frac in hits or prev_v is None:
                continue
            level = frac * lit
            if prev_v >= level > v:
                span = prev_v - v
                t = (prev_v - level) / span if span > 1e-12 else 0.0
                hits[frac] = prev_a + t * (a - prev_a)
        prev_a, prev_v = a, v
        a += 0.5
    if 0.9 not in hits or 0.1 not in hits:
        return None
    return hits[0.1] - hits[0.9]


def run_skin_handoff_gate(workdir):
    """D5: the angular half takes over in proportion to how hard the blur is capped.

    The blur's kernel is capped in PIXELS (SSS_MAX_BLUR_PX), so the world width it
    delivers falls as 1/height once the cap binds; phase 0 measured the delivered
    scatter collapsing to 0.282 of its unclamped value across a 5x sweep. D5 has
    pbr_frag compute that shortfall per fragment and open the angular falloff by
    exactly the slack, so the harder the cap bites, the more pre-integration
    carries. That handoff is what this asserts.

    It deliberately does NOT assert the total is resolution-INVARIANT, which is
    what D5 was originally specified to achieve. Measured, it is not: across a 4x
    sweep the total drifts 13.4% with the feature on against 8.3% with it off,
    and in the opposite direction. The two halves are not interchangeable
    currencies -- at the terminator the surface is turning away from the camera,
    so the screen-space blur compresses there on top of its pixel cap while the
    angular falloff does not, and trading one for the other changes the delivered
    width. Recorded in specs/11.13, section D5.

    The bars come from the two ways D5 can be broken. Hardcoding the deficit to 0
    kills the feature wherever SSS runs and gives 0 at both heights, which the
    high bar catches; hardcoding it to 1 removes the composition entirely and
    contributes 11.9 deg at the LOW height, falling as resolution rises, which
    the low bar catches. Nothing else on the branch covers either: every other
    skin measurement runs --no-sss, which forces the deficit to 1.
    """
    scene = os.path.join(ROOT, "assets", "skin_curvature_fixture.cscn")
    if not os.path.exists(scene):
        print("  skin-handoff SKIP  (missing skin_curvature_fixture.cscn)")
        return []

    def width_at(tag, dims, extra):
        out = os.path.join(workdir, f"skinscat_{tag}.ppm")
        # SSS ON: the whole point is the two mechanisms summing. --no-shadows so
        # the far side is not multiplied away, --no-bloom so nothing smears the
        # falloff being measured.
        cmd = [RENDER, "-m", scene, "-x", "-f", "30", "--no-auto-exposure", "-E", "1.0",
               "--no-shadows", "--no-bloom", "-W", dims[0], "-H", dims[1], "-S", out] + extra
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            return None
        w, h, pix = _read_ppm(out)
        project = _projector(SKIN_CAM, w, h)
        # The mid sphere: big enough to sample finely, small enough to sit well
        # inside the frame at both resolutions.
        name, radius, cx = SKIN_SPHERES[1]
        at = _skin_sample_points(radius, cx)
        lit = _linear_luma(pix, w, h, *project(at(SKIN_LIT_DEG)))
        return _skin_terminator_width(pix, w, h, project, at, lit)

    # 800x500 leaves the mid sphere's terminator under the pixel cap; 1600x1000
    # puts it roughly 2x over.
    given = {}
    for tag, dims in (("lo", ("800", "500")), ("hi", ("1600", "1000"))):
        on = width_at(tag + "_on", dims, [])
        off = width_at(tag + "_off", dims, ["--no-skin-preint"])
        if on is None or off is None:
            print(f"  skin-handoff ERROR while rendering at {dims[0]}x{dims[1]}")
            return ["skin-handoff"]
        given[tag] = on - off

    quiet = given["lo"] <= 0.5
    carries = given["hi"] >= 3.0
    ok = quiet and carries
    print(f"  skin-handoff {'PASS' if ok else 'FAIL'}  angular half gives "
          f"{given['lo']:+.2f} deg at 800x500 (want <= +0.50, blur is unclamped) and "
          f"{given['hi']:+.2f} deg at 1600x1000 (want >= +3.00, blur is capped)")
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
        print("cloud layer (steady-state churn, report-only):")
        failures += run_cloud_churn_gate(workdir)
        print("pre-integrated skin (off-path byte identity):")
        failures += run_skin_offpath_gate(workdir)
        print("pre-integrated skin (curvature ordering):")
        failures += run_skin_curvature_gate(workdir)
        print("pre-integrated skin (handoff from the capped blur):")
        failures += run_skin_handoff_gate(workdir)
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
