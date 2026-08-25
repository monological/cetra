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
import base64
import functools
import glob
import importlib.util
import inspect
import json
import math
import struct
import re
import os
from itertools import groupby
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Where the app binaries come from. `out/bin` is build.sh's default (Debug, no
# -O at all); `out/release/bin` is `./build.sh --release`.
#
# Worth knowing before choosing, because the two apps answer very differently
# (spec 11.65, measured):
#   render  0.41 s -> 0.36 s, a 12% saving. Its startup is a GL context and ~37
#           driver-side shader compiles, which an optimiser cannot touch.
#   forest  6.58 s -> 1.36 s, a 4.8x saving. Its startup is CPU work -O2 does
#           reach: procedural 1K texture bakes, 20 cluster-DAG builds, erosion.
# At ~284 render and ~55 forest launches that is ~17 s against ~287 s, so
# essentially the whole prize is forest's.
#
# Auto-selected by FRESHNESS, which is the one rule that cannot test a stale
# binary: release is used only when it is at least as new as the debug tree it
# would replace. Build debug after release and the mtime ordering flips back on
# its own, so the dangerous case -- passing on a binary nobody just built --
# cannot arise without also making debug older, which is the same hazard the
# plain default has. An explicit --bin-dir or CETRA_BIN_DIR overrides it, and
# the resolved directory is printed on every run so a result is never ambiguous
# about what it tested.
def _default_bin_dir():
    debug = os.path.join(ROOT, "out", "bin")
    release = os.path.join(ROOT, "out", "release", "bin")
    try:
        # forest is the app the choice is FOR -- 4.8x against render's 1.1x -- so
        # it is the one whose freshness decides.
        if os.path.getmtime(os.path.join(release, "forest")) >= os.path.getmtime(
                os.path.join(debug, "forest")):
            return release
    except OSError:
        pass
    return debug


def _bin(name):
    """An app binary in BIN_DIR, spelled the way the host names executables."""
    return os.path.join(BIN_DIR, name + (".exe" if sys.platform == "win32" else ""))


BIN_DIR = os.environ.get("CETRA_BIN_DIR") or _default_bin_dir()
RENDER = _bin("render")
SCALE = 1000.0

# Every sample coordinate and every stored golden in this suite was measured
# against a framebuffer TWICE the requested -W/-H, because that is what a HiDPI
# context hands back. Most arms already read fractionally and do not care
# (_beach_pixel says so in its own docstring); two cannot:
#
#   - ao-ring's window is absolute -- AO_RING_ROWS (745, 815) against an arm
#     rendering -W 800 -H 600. At 1x the buffer is 800x600 and every sampled row
#     is past the bottom edge, which is an IndexError that takes the whole run
#     down rather than failing one arm.
#   - cloud-shadow reads a --sky-debug tile that sky.c draws at ABSOLUTE pixel
#     offsets. At 1x that tile is not displaced, it is off the frame entirely, so
#     no rescaling of the sample box could recover it.
#
# The second is why the REQUEST is scaled rather than the readings. A HiDPI
# machine multiplies by one and is byte-identical to before; a 1x machine asks
# for double and gets the same framebuffer, so absolute coordinates land, the
# debug tile is on screen, and an absolute pixel-count threshold means the same
# thing on both. Nothing downstream learns which platform it is on.
#
# Note this was never only a portability question: a Mac on a 1x external monitor
# breaks the suite in exactly the same way, which is why the scale is MEASURED
# below and not keyed off sys.platform.
CALIBRATED_FB_SCALE = 2
_FB_SCALE = None


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


def cscn_copy(src, dst, mutate):
    """Copy a .cscn through `mutate(dict)`, with model paths made absolute.

    Generating a twin mechanically rather than committing a hand-edited one is the point:
    the two halves then cannot differ in anything except what `mutate` touched, which is
    the property every arm built on one of these asserts.

    The path absolutization is the half worth having in one place. It is a quiet
    correctness requirement -- model paths resolve against the scene file's directory, so
    an out-of-tree copy must carry absolute ones -- and a copy that forgets it fails as a
    render error rather than as a mismatch.

    An IES profile path resolves the same way and does NOT fail the same way: a relative
    one in an out-of-tree copy loads nothing, the light keeps its analytic cone, and the
    frame renders. That is the one to get wrong silently, so it is handled here too rather
    than by each arm that happens to remember.
    """
    with open(src) as f:
        d = json.load(f)
    mutate(d)
    base = os.path.dirname(os.path.abspath(src))
    for m in d.get("models", []):
        if not os.path.isabs(m["path"]):
            m["path"] = os.path.join(base, m["path"])
    for light in d.get("lights", []):
        p = light.get("profile")
        if p and not os.path.isabs(p):
            light["profile"] = os.path.join(base, p)
    # post.lut.path is the third of these and fails the IES way, not the model
    # way: a relative path in an out-of-tree copy loads nothing, the frame
    # renders ungraded, and an arm asserting "the LUT changed the frame" reports
    # the feature broken when the path was.
    lut = d.get("post", {}).get("lut")
    if lut and lut.get("path") and not os.path.isabs(lut["path"]):
        lut["path"] = os.path.join(base, lut["path"])
    with open(dst, "w") as f:
        json.dump(d, f, indent=1)


def _scale_emitters(d, factor):
    """Multiply every authored emitter in a .cscn dict by `factor`, in place.

    One copy, because two callers need the same answer to "what counts as an
    authored intensity" and a scene that scales in one and not the other is a
    scene neither can reason about. Fog's in-scatter is on the list because it
    is a real emitter -- and it was the branch a second, hand-copied version of
    this rule silently omitted.
    """
    for light in d.get("lights", []):
        if "intensity" in light:
            light["intensity"] *= factor

    env = d.get("environment")
    if env:
        if "intensity" in env:
            env["intensity"] *= factor
        if "ambient" in env:
            env["ambient"] = [c * factor for c in env["ambient"]]

    fog = d.get("post", {}).get("fog")
    if fog and "ambient" in fog:
        fog["ambient"] = [c * factor for c in fog["ambient"]]


def scaled_copy(src, dst, factor):
    """Multiply every authored intensity by `factor` and divide exposure by it."""
    def scale(d):
        _scale_emitters(d, factor)
        post = d.setdefault("post", {})
        post["exposure"] = post.get("exposure", 1.0) / factor

    cscn_copy(src, dst, scale)


def render(scene, out, extra, frames=30):
    """One headless frame capture at the suite's shared size.

    THIRTY FRAMES IS INHERITED, NOT DERIVED, and since it is the most repeated
    number in this file that is worth saying where it lives. Nothing in the
    engine produces 30. What the engine actually forces (spec 11.65):

      - Without --taa every shared temporal accumulator -- TAA, GTAO, SSGI, SSR,
        SSS, the composited fog layer, contact shadows -- is not merely off, its
        history is force-invalidated every frame. A gate run passes no --taa.
      - A pinned exposure (--no-auto-exposure -E, or an authored post.exposure)
        skips the metering block entirely, so it imposes no frame requirement.
      - Animation, wind, particles and the water FFT are pure functions of the
        frame INDEX headless, so frame N is pose N -- a floor and a ceiling at
        once for any arm that reads motion, and irrelevant to one that does not.
      - The one unconditional floor is the async texture loader at five uploads
        per frame, plus one more frame for the material texture array. Most
        fixtures here load zero or one texture; the worst is layer_fixture at
        seven. So the real floor for a static, pinned, fog-free capture is about
        two frames.

    It is NOT lowered, and that is a measurement rather than caution: a frame at
    this size costs ~0.0013 s, so thirty frames cost 0.04 s more than one and
    cutting every call site to the floor would save about five seconds of a
    ten-minute suite. Against that, `frames` is load-bearing for any arm reading
    a pose, and lowering the shared default would move those silently -- the
    arm would still pass, reading a different moment. Five seconds is not worth
    a class of failure this suite has already shipped twice.

    Counts that ARE derived state their reason where they are set: 60 for the
    froxel and cloud accumulators, 150 for auto-exposure and foam history,
    240 and 400 for trace cadence and walk geometry.
    """
    cmd = [RENDER, "-m", scene, "-x", "-f", str(frames), "-W", "400", "-H", "300", "-S", out]
    r = _run(cmd + extra, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return r.stdout + r.stderr
    return None


def _scale_argv(argv):
    """One app command with its framebuffer request brought to the calibrated size.

    Guarded on argv[0] so anything that is not one of our binaries -- magick
    above all -- passes through untouched, which is what makes it safe to route
    every subprocess call in this file through here.
    """
    mult = CALIBRATED_FB_SCALE // (_FB_SCALE or CALIBRATED_FB_SCALE)
    if mult == 1 or not argv or argv[0] not in (RENDER, FOREST):
        return argv
    out = list(argv)
    for i in range(len(out) - 1):
        if out[i] in ("-W", "-H"):
            out[i + 1] = str(int(out[i + 1]) * mult)
    return out


def _run(cmd, **kw):
    """subprocess.run for the app binaries, framebuffer request scaled.

    A wrapper rather than 45 edited argv builders: a site that forgot would read
    the wrong pixels and still report a number, which is the failure this whole
    mechanism exists to prevent.
    """
    return subprocess.run(_scale_argv(cmd), **kw)


def _detect_fb_scale(workdir):
    """Framebuffer pixels per requested pixel, measured on this machine.

    Rendered rather than inferred: the answer is a property of the DISPLAY, not
    the OS, so a Mac on a 1x external monitor answers 1 and must. Falls back to
    the calibrated value if the probe cannot render at all, which leaves the
    suite behaving exactly as it did before this existed.
    """
    global _FB_SCALE
    probe = os.path.join(workdir, "_fbscale.ppm")
    scene = os.path.join(ROOT, "assets", "parallax_fixture.gltf")
    r = subprocess.run([RENDER, "-m", scene, "-x", "-f", "1", "-W", "100", "-H", "100",
                        "-S", probe], capture_output=True, text=True)
    if r.returncode == 0 and os.path.exists(probe):
        w, _, _ = _read_ppm(probe)
        _FB_SCALE = max(1, int(round(w / 100.0)))
    else:
        _FB_SCALE = CALIBRATED_FB_SCALE
    return _FB_SCALE


def compare(a, b):
    """Return (differing pixels, peak absolute error as a 0..1 fraction).

    A magick that could not read one of the two answers with a message rather
    than a number ("compare: unable to open image ..."), which used to raise out
    of here and take the whole run down over one missing frame. Reported as a
    full-frame difference instead: the caller is asking whether two images agree,
    and one that does not exist does not agree.
    """
    def metric(name):
        r = subprocess.run(["magick", "compare", "-metric", name, a, b, "null:"],
                           capture_output=True, text=True)
        return (r.stderr or r.stdout).strip()

    try:
        ae = int(float(metric("AE").split()[0]))
    except (ValueError, IndexError):
        return sys.maxsize, 1.0
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


# The sRGB decode, once, indexed by the raw byte. Every measurement here is on
# a linear signal, and the per-pixel pow is slow enough in pure Python to notice
# on the gates that scan whole frames.
_SRGB_TO_LINEAR = [(c / 255.0 / 12.92) if c / 255.0 <= 0.04045
                   else (((c / 255.0) + 0.055) / 1.055) ** 2.4 for c in range(256)]


def _linear_luma(pix, w, h, px, py):
    """Undo the sRGB encode -- the transition is measured on a linear signal."""
    x = max(0, min(w - 1, int(round(px))))
    y = max(0, min(h - 1, int(round(py))))
    o = (y * w + x) * 3
    return (_SRGB_TO_LINEAR[pix[o]] + _SRGB_TO_LINEAR[pix[o + 1]] +
            _SRGB_TO_LINEAR[pix[o + 2]]) / 3.0


def _linear_rgb(pix, w, h, px, py):
    """Per-channel sibling of _linear_luma; the reddening assertion needs the
    channels apart, and an average would hide exactly what it is looking for."""
    x = max(0, min(w - 1, int(round(px))))
    y = max(0, min(h - 1, int(round(py))))
    o = (y * w + x) * 3
    return [_SRGB_TO_LINEAR[pix[o + k]] for k in range(3)]


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


# The map arm must split the card's halves at least this hard, and the two
# control arms must not split them at all. Measured at this gate's size: map
# 1.32x, no map 1.05x, GGX 1.05x.
#
# The margin is THIN, and that is a property of the model rather than of the
# gate. Anisotropic GGX only reshapes an existing lobe, so a per-texel direction
# redistributes a fixed amount of light; the fibre lobes this replaced keyed
# their whole magnitude on the strand angle and split the same fixture 7.2x.
# Modesty is the point -- it is why this version does not blow out -- but it
# leaves less headroom, so the size is pinned (minification mips the
# orientation away) and the threshold sits just above the controls.
HAIR_FLOW_MIN_RATIO = 1.20
HAIR_FLAT_MAX_RATIO = 1.10

def _hair_card_box(path):
    """Bounding box of the lit card, as (x0, x1, y0, y1)."""
    w, h, pix = _read_ppm(path)
    lum = [(_SRGB_TO_LINEAR[pix[i * 3]] + _SRGB_TO_LINEAR[pix[i * 3 + 1]] +
            _SRGB_TO_LINEAR[pix[i * 3 + 2]]) / 3.0 for i in range(w * h)]
    lit = [i for i, v in enumerate(lum) if v > 0.002]
    if not lit:
        return None
    xs = [i % w for i in lit]
    ys = [i // w for i in lit]
    return min(xs), max(xs), min(ys), max(ys)


def _hair_halves(path, box):
    """Linear-luma samples of the card's left and right halves, within `box`.

    The box is passed in rather than derived per image, because arms are
    compared element-wise: jitter changes brightness at the lit edges, so one
    pixel crossing the threshold in one arm and not another would shift the
    crop, misalign every subsequent sample, and silently change what the
    comparison means.

    Excludes a margin around the seam where the two painted strand fields meet:
    the structure tensor genuinely cannot resolve an orientation there, reports
    low coherence, and the shader correctly falls back -- which is right
    behaviour and would only add noise to the measurement.
    """
    w, h, pix = _read_ppm(path)
    lum = [(_SRGB_TO_LINEAR[pix[i * 3]] + _SRGB_TO_LINEAR[pix[i * 3 + 1]] +
            _SRGB_TO_LINEAR[pix[i * 3 + 2]]) / 3.0 for i in range(w * h)]
    x0, x1, y0, y1 = box
    mid = (x0 + x1) // 2
    pad = max(1, int(0.10 * (x1 - x0)))
    left, right = [], []
    for y in range(y0, y1 + 1):
        row = y * w
        left += [lum[row + x] for x in range(x0 + pad, mid - pad)]
        right += [lum[row + x] for x in range(mid + pad, x1 - pad)]
    return left, right


def _hair_ratio(halves):
    left, right = halves
    lm = sum(left) / max(len(left), 1)
    rm = sum(right) / max(len(right), 1)
    return rm / max(lm, 1e-6)


def run_hair_flow_gate(workdir):
    """The hair lobes must be driven by the strand map, not by the card tangent.

    One quad, one material, one draw, one tangent. The atlas paints strands
    along the tangent in the left half and across it in the right, with the key
    off to the side, so a shader that reads the map splits the halves and one
    that does not cannot. Geometry, normal, material, light and tangent are
    identical either side of the seam, so nothing else can produce a split.

    The two control arms are what make that attributable. Hair with NO map must
    come out flat, and so must plain GGX -- and 'flat' is also what an invented
    per-texel jitter measures as, which is the failure this replaced: a hash of
    the texel coordinate varies within each half but has the same distribution
    in both, so it moves the variance and leaves the ratio at 1.
    """
    arms = (("map", "hair_fixture.cscn"),
            ("no map", "hair_fixture_nomap.cscn"),
            ("ggx", "hair_fixture_ggx.cscn"))
    # Large enough that the atlas lands near 1:1 on screen. Minified past that
    # the mip chain averages the strand identity away -- correctly, but it is
    # not what this is measuring.
    size = ["-W", "800", "-H", "600"]
    fails, frames, box = [], {}, None
    for label, name in arms:
        scene = os.path.join(ROOT, "assets", name)
        if not os.path.exists(scene):
            print(f"  hair-flow    SKIP  (missing {name})")
            return []
        out = os.path.join(workdir, name.replace(".cscn", ".ppm"))
        err = render(scene, out, size)
        if err:
            print(f"  hair-flow    ERROR while rendering {name}")
            return ["hair-flow"]
        frames[label] = out
        if box is None:
            box = _hair_card_box(out)
            if box is None:
                print(f"  hair-flow    ERROR  {name} rendered an empty frame")
                return ["hair-flow"]

    # One crop for every arm, taken from the first: the arms are compared
    # element-wise, so a per-image box would misalign them (see _hair_halves).
    shots = {label: _hair_halves(path, box) for label, path in frames.items()}

    # Direction-agnostic: the claim is that the halves DIFFER, not which way.
    # Anisotropic GGX stretches the highlight along the grain, so the brighter
    # half is the opposite one to what a fibre lobe would give -- and the sign
    # is a property of the model, not of whether the map is being read.
    ratio = _hair_ratio(shots["map"])
    split = max(ratio, 1.0 / max(ratio, 1e-6))
    ok = split >= HAIR_FLOW_MIN_RATIO
    print(f"  hair-flow    {'PASS' if ok else 'FAIL'}  halves differ {split:.3f}x "
          f"(want >= {HAIR_FLOW_MIN_RATIO}; only the map can split these halves)")
    if not ok:
        fails.append("hair-flow")

    for label in ("no map", "ggx"):
        flat = _hair_ratio(shots[label])
        flat = max(flat, 1.0 / max(flat, 1e-6))
        ok = flat <= HAIR_FLAT_MAX_RATIO
        print(f"  hair-flat    {'PASS' if ok else 'FAIL'}  {label}: {flat:.3f}x "
              f"(want <= {HAIR_FLAT_MAX_RATIO}; the control that makes the split the map's doing)")
        if not ok:
            fails.append("hair-flat")

    return fails


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
        scene = os.path.join(ROOT, "assets", scene_name)
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
    scene = os.path.join(ROOT, "assets", "skin_curvature_fixture.cscn")
    out = os.path.join(workdir, f"skinm_{tag}.ppm")
    # --no-shadows so the far side is not multiplied away, --no-bloom so nothing
    # smears the falloff being measured, --no-dither because the crossing is
    # interpolated from single pixels at 5% of the lit reference, where one LSB
    # of the default output dither is a large relative step and the two legs of
    # the drift subtraction pick up independent offsets that do not cancel.
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "--no-auto-exposure", "-E", "1.0",
           "--no-shadows", "--no-bloom", "--no-dither",
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


# --- lens flare and chromatic aberration (spec 11.21 / B7) --------------------

# Frame-plane positions of the two dim marks in assets/flare_fixture.gltf, and
# the camera that sees them. Kept here rather than derived from the .gltf: the
# gate's whole claim is that measured separation matches an ANALYTIC prediction,
# and reading the geometry back from the asset under test would let a broken
# generator move the marks and the prediction together.
FLARE_MARKS = {"inner": (1.5, -1.05), "corner": (2.6, -1.9)}
FLARE_MARK_HALF = 0.16
FLARE_EYE_Z = 5.0
FLARE_FOV = 50.0
# Channel separation the shader is documented to produce AT THE CORNER, so the
# gate asserts the unit the flag is denominated in and not just "something moved".
FLARE_CA_PIXELS = 20.0
FLARE_CA_TOL = 0.15
# 8-bit sRGB -> linear, once. The flare composite is a linear gain on linear
# radiance; comparing encoded samples would measure the encode's curve instead.
_SRGB_LIN = [((v / 255.0 + 0.055) / 1.055) ** 2.4 if v / 255.0 > 0.04045
             else (v / 255.0) / 12.92 for v in range(256)]


def _flare_render(workdir, tag, extra):
    out = os.path.join(workdir, f"flare_{tag}.ppm")
    scene = os.path.join(ROOT, "assets", "flare_fixture.cscn")
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "-W", "800", "-H", "500",
           "--no-auto-exposure", "-E", "1.0", "-S", out] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    return _read_ppm(out)


def _flare_mark_px(wx, wy, w, h):
    """Frame-plane pixel centre of a mark, and the visible half-extents."""
    half_h = FLARE_EYE_Z * math.tan(math.radians(FLARE_FOV * 0.5))
    half_w = half_h * (w / float(h))
    return ((wx / half_w + 1.0) * 0.5 * w, (1.0 - wy / half_h) * 0.5 * h, half_w)


def _flare_centroid(pix, w, cx, cy, rad, ch):
    """Intensity-weighted centroid of one channel inside a window.

    A radial split moves each channel's image of the mark bodily, so the
    distance between the R and B centroids IS the separation -- no edge-finding,
    and robust to the mark being a few pixels off where the maths says.
    """
    sx = sy = sw = 0.0
    for y in range(int(cy - rad), int(cy + rad) + 1):
        for x in range(int(cx - rad), int(cx + rad) + 1):
            v = pix[(y * w + x) * 3 + ch] / 255.0
            if v < 0.02:  # backdrop, not mark
                continue
            sx += x * v
            sy += y * v
            sw += v
    return (sx / sw, sy / sw) if sw > 0 else None


def _flare_separation(w, h, pix, name):
    px, py, half_w = _flare_mark_px(*FLARE_MARKS[name], w, h)
    rad = (FLARE_MARK_HALF / half_w) * (w * 0.5) + 30  # mark, plus room for the shift
    r = _flare_centroid(pix, w, px, py, rad, 0)
    b = _flare_centroid(pix, w, px, py, rad, 2)
    if r is None or b is None:
        return None, None
    t = math.hypot(px / w - 0.5, py / h - 0.5) / 0.70710678  # 1.0 at the corner
    return math.hypot(r[0] - b[0], r[1] - b[1]), t


def run_flare_gate(workdir):
    fixture = os.path.join(ROOT, "assets", "flare_fixture.cscn")
    if not os.path.exists(fixture):
        print("  flare        SKIP  (missing flare_fixture.cscn)")
        return []

    fails = []

    # 1. The composite is LINEAR in strength.
    #
    # Not "off is off": that arm was written first and does not falsify. The C
    # side short-circuits on flare_strength > 0, so --flare 0 never runs the
    # pass and a deliberately broken composite still passed at 0 px. What the
    # default-off claim actually rests on is linearity -- a gain of zero
    # contributes exactly nothing -- and the 18 goldens already assert the
    # frame itself. So assert the gain instead, under a passthrough tonemap so
    # doubling the strength must double the DELTA rather than some compressed
    # image of it.
    #
    # Samples are linearised out of sRGB first, but that does NOT make the
    # assertion exact: Khronos Neutral is not identity here. Its toe expands
    # the darks and its shoulder compresses the brights, and a strength sweep
    # measures the doubling as 2.79 / 2.19 / 2.15 / 1.91 / 1.69 going from 0.1
    # to 4.0 -- the composite is linear and the INSTRUMENT is curved. Undoing
    # that would mean reimplementing the tonemap here, so the band below is the
    # curve's own spread rather than slack.
    #
    # So this arm resolves linear from "ignores strength" (gain 1.0) and from
    # anything squared (4.0). It cannot resolve finer than the tonemap bends.
    lin = {}
    for tag, extra in (("pt0", []), ("pt1", ["--flare", "0.5"]), ("pt2", ["--flare", "1.0"])):
        lin[tag] = _flare_render(workdir, tag, extra)
    if any(v is None for v in lin.values()):
        print("  flare-gain   ERROR while rendering the linearity triple")
        return ["flare-gain"]
    w, h, _ = lin["pt0"]

    def _opp_mean(pix):
        tot = n = 0.0
        for y in range(h // 2, h, 2):
            for x in range(w // 2, w, 2):
                i = (y * w + x) * 3
                tot += (0.2126 * _SRGB_LIN[pix[i]] + 0.7152 * _SRGB_LIN[pix[i + 1]]
                        + 0.0722 * _SRGB_LIN[pix[i + 2]])
                n += 1
        return tot / n

    m0 = _opp_mean(lin["pt0"][2])
    d1 = _opp_mean(lin["pt1"][2]) - m0
    d2 = _opp_mean(lin["pt2"][2]) - m0
    gain = d2 / d1 if d1 > 1e-6 else float("nan")
    ok = d1 > 1e-4 and abs(gain - 2.0) <= 0.40
    print(f"  flare-gain   {'PASS' if ok else 'FAIL'}  delta {d1:.5f} -> {d2:.5f} on 2x "
          f"strength: gain {gain:.3f} (want 2.00 +/- 0.40, tonemap-limited)")
    if not ok:
        fails.append("flare-gain")

    # 2. Ghosts land OPPOSITE the source.
    #
    # The emitter is up and to the LEFT, and ghosts are the source mirrored
    # through frame centre, so the energy owes the lower-right. Asserting the
    # emitter's own half stays put is what separates "these are ghosts" from
    # "the image got brighter", which any additive bug would also pass.
    base = _flare_render(workdir, "base", [])
    on = _flare_render(workdir, "on", ["--flare", "0.15"])
    if base is None or on is None:
        print("  flare-ghost  ERROR while rendering the flare pair")
        return fails + ["flare-ghost"]
    def _half_mean(pix, x0, y0):
        tot = n = 0.0
        for y in range(y0, y0 + h // 2, 2):
            for x in range(x0, x0 + w // 2, 2):
                i = (y * w + x) * 3
                tot += 0.2126 * pix[i] + 0.7152 * pix[i + 1] + 0.0722 * pix[i + 2]
                n += 1
        return tot / n / 255.0
    src_off = _half_mean(base[2], 0, 0)
    src_on = _half_mean(on[2], 0, 0)
    opp_off = _half_mean(base[2], w // 2, h // 2)
    opp_on = _half_mean(on[2], w // 2, h // 2)
    src_rise = (src_on - src_off) / max(src_off, 1e-9)
    opp_rise = (opp_on - opp_off) / max(opp_off, 1e-9)
    ok = opp_rise > 0.30 and src_rise < 0.10
    print(f"  flare-ghost  {'PASS' if ok else 'FAIL'}  opposite half {opp_rise * 100:+.0f}%, "
          f"source half {src_rise * 100:+.0f}% (want > +30% and < +10%)")
    if not ok:
        fails.append("flare-ghost")

    # 3. Aberration separates the channels, by r^2, in pixels at the corner.
    #
    # Two marks at different radii is what makes the FALLOFF falsifiable: with
    # one, "the channels separated" is all a gate can say, and a linear ramp
    # passes that identically. Bloom is off here -- it is added after the split
    # and is not itself shifted, so it would only dilute the centroids.
    ca_off = _flare_render(workdir, "ca_off", ["--no-bloom"])
    ca_on = _flare_render(workdir, "ca_on",
                          ["--no-bloom", "--chromatic-aberration", str(FLARE_CA_PIXELS)])
    if ca_off is None or ca_on is None:
        print("  flare-ca     ERROR while rendering the aberration pair")
        return fails + ["flare-ca"]

    problems, meas = [], {}
    for name in ("inner", "corner"):
        sep_off, _ = _flare_separation(ca_off[0], ca_off[1], ca_off[2], name)
        sep_on, t = _flare_separation(ca_on[0], ca_on[1], ca_on[2], name)
        if sep_on is None or sep_off is None:
            problems.append(f"{name} mark not found in frame")
            continue
        meas[name] = sep_on
        if sep_off > 0.5:
            problems.append(f"{name} separated by {sep_off:.2f} px with CA OFF")
        pred = 2.0 * t * t * FLARE_CA_PIXELS  # R-to-B, twice the per-channel shift
        if abs(sep_on - pred) > FLARE_CA_TOL * pred:
            problems.append(f"{name} {sep_on:.2f} px vs {pred:.2f} predicted")

    if len(meas) == 2:
        ratio = meas["corner"] / meas["inner"]
        _, ti = _flare_separation(ca_on[0], ca_on[1], ca_on[2], "inner")
        _, tc = _flare_separation(ca_on[0], ca_on[1], ca_on[2], "corner")
        sq, lin = (tc / ti) ** 2, tc / ti
        if abs(ratio - sq) > abs(ratio - lin):
            problems.append(f"falloff ratio {ratio:.2f} is closer to linear "
                            f"({lin:.2f}) than to r^2 ({sq:.2f})")
        detail = (f"{meas['inner']:.1f} / {meas['corner']:.1f} px, "
                  f"ratio {ratio:.2f} (r^2 predicts {sq:.2f}, linear {lin:.2f})")
    else:
        detail = "; ".join(problems)

    ok = not problems
    print(f"  flare-ca     {'PASS' if ok else 'FAIL'}  {detail if ok else '; '.join(problems)}")
    if not ok:
        fails.append("flare-ca")
    return fails


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
    # --no-dither is load-bearing here, not tidiness: this gate's statistic is
    # per-pixel deviation from a 25px moving average, which is exactly the
    # frequency the output dither injects. With the default on it reads 0.0196
    # against a 0.030 bound where the blur itself contributes 0.0142, so the
    # gate would start failing on dither amplitude rather than on kernel rings.
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "--no-auto-exposure", "-E", "1.0",
           "--no-shadows", "--no-bloom", "--no-dither", "-W", "1200", "-H", "750", "-S", out]
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
        r = _run(cmd, capture_output=True, text=True)
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
# The pillar's ground band, from the same projection the ellipses use: a point at
# height h lands at z - h/tan(elev), so the 0.3 x 0.3 x 3.0 pillar at x = -3.5
# shadows z = 0.15 back to z = -3.725. Bounds below are eroded off both ends,
# clear of the tip and of the pillar's own footprint.
#
# A note on why this arm exists, since the hole arm above looks like it covers
# the same ground: it does not, and that was measured rather than assumed.
# Swapping the four-moment reconstruction for the two-moment Chebyshev bound
# (VSM -- the technique specs/10.4 declined for leaking) moves 6339 px of the
# frame and leaves the hole arm reading 0.0084, to four decimals, unchanged. The
# eroded ellipse interiors are where every reconstruction agrees; the difference
# lives on the ellipse RIMS, which DIR_ERODE deliberately excludes, and on this
# band. A slab seen near edge-on by the map puts almost no depth SPREAD in a
# texel footprint but a large caster-receiver gap behind it, which is the
# configuration two moments cannot represent and four can.
DIR_PILLAR_X = (-3.61, -3.39)
DIR_PILLAR_Z = (-3.2, -0.6)
# PCF reads 0.0000 here and the moment path at the shipped blur reads 0.0074, so
# this sits 2.7x above what passes and 21x below what fails (blur 1.0 leaks
# 0.4159, the two-moment bound 0.4249).
DIR_PILLAR_MAX = 0.02
# The inverse half, and the arm that makes the other six mean anything.
#
# Every MSM arm asserts the moment path is no WORSE than PCF -- and
# shadow_build_msm falls back to the depth cascades without logging on three
# paths (flag unparsed, program missing, allocation failed), which renders a
# frame byte-identical to --no-msm and passes all six while measuring nothing.
# That is the defect run_flare_gate records at its own arm 1 and run_oit_gate
# guards with OIT_DISCRIMINATION_MIN.
#
# Blur is the knob that provably moves this band, so leaking under it is what
# says the moments were built and read. Bound is a fifth of the 0.4159 measured
# at spacing 1.0 -- far above PCF's 0.0000, far below the reading it checks for.
DIR_PILLAR_BLUR_MIN = 0.10
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
    r = _run(cmd, capture_output=True, text=True)
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
    r = _run(cmd, capture_output=True, text=True)
    f90 = base[:-4] + "_000090.ppm"
    f120 = base[:-4] + "_000120.ppm"
    # Both frames can exist and still be wrong if the run died between them, so
    # the exit code is checked as well as the files.
    if r.returncode != 0:
        print(f"  churn        ERROR {tag} exited {r.returncode}: "
              f"{(r.stdout + r.stderr).strip()[-300:]}")
        return None
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
    # surface out of the frame. --no-dither is the same class and the same
    # reason: the bands are sampled ONE pixel each, so a per-pixel LSB of dither
    # lands on them as a function of framing and doubles the worst deviation.
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "-W", "800", "-H", "500",
           "--no-auto-exposure", "-E", "1.0", "--no-vignette", "--no-bloom",
           "--no-ssao", "--no-ssr", "--no-dither", "-S", out] + extra
    r = _run(cmd, capture_output=True, text=True)
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


# IES photometric profiles (spec 11.57), on assets/ies_fixture and its four .ies files.
#
# Read through --ies-probe rather than off a frame, and the reason is the whole shape of
# this group: a table resampled off the wrong plane, folded with a modulo where LM-63
# mirrors, or scaled by the wrong multiplier still lights a room plausibly. There is no
# picture that distinguishes a correct luminaire from a confident wrong one.
#
# The numbers below are CONSTRUCTED, not measured. assets/gen_ies_fixture.py writes every
# candela as a closed form of its two angles, so the arms re-evaluate that same form and
# compare -- ground truth painted rather than read back off the thing under test. Mirrors
# that generator and has to match it.
IES_PEAK_CD = 1200.0
IES_FILES = [
    # name, declared span, symmetric
    ("ies_symmetric.ies", 360.0, True),
    ("ies_bilateral.ies", 180.0, False),
    ("ies_quadrant.ies", 90.0, False),
    ("ies_asymmetric.ies", 360.0, False),
    # The one file whose 90-degree candela is nonzero IN THE FILE (1.2e-6 cd). The
    # other four paint a literal 0, which reads back as 0 whether or not the snap
    # in ies.c exists -- so they measure the arithmetic and this one measures the
    # guarantee pbr_frag's early-out rests on.
    ("ies_tinytail.ies", 360.0, True),
]
# How far a tap may sit from its closed form, in units of the PEAK. The fixture's own
# angles are a subset of the resampled grid, so the agreement is exact arithmetic rather
# than a fit and the residual is float32 round-trip alone: the table is stored normalised
# and the probe multiplies the peak back in. Measured 9.5e-7.
#
# Relative to the peak rather than to the tap's own value, which is the choice worth
# stating because the obvious reading is the other one. The residual is quantisation of the
# NORMALISED table, so it is uniform in units of peak and does NOT scale with how bright
# the tap is -- dividing by the expected value would demand ever-tighter agreement down the
# tail and reduce to 0/0 at the zeros. Dividing by a bare candela figure would be wrong
# too: a fixture at 1200 cd and one at 12 cd would then need different numbers.
IES_TAP_REL_EPS = 1e-5


def _ies_expected(v_deg, h_deg, span):
    """The fixture's closed form, in candela. Mirrors gen_ies_fixture.py's candela().

    Restated rather than imported. Importing the generator would make the ground truth a
    function of the thing under test, which is the one property this whole group exists to
    avoid -- the same reason CONE_INNER_DEG is restated a few hundred lines below.

    The tail is 0 for every file including the one whose FILE says 1.2e-6: the snap turns a
    residual under 1e-6 relative into an exact zero, so 0 is the right expectation for all
    five and only ies_tinytail.ies makes that a claim rather than a copy.
    """
    lobe = 0.0 if v_deg >= 90.0 else math.cos(math.radians(v_deg))
    return IES_PEAK_CD * lobe * (1.0 - h_deg / span)


def _ies_probe(workdir, scene, extra=None):
    """Run --ies-probe and split its rows by kind. Returns (profiles, samples, mirror, text)."""
    rows, text = _probe_render(scene, "--ies-probe", "ies-probe", extra=extra, frames=2)
    return ([r for r in rows if r.get("kind") == "profile"],
            [r for r in rows if r.get("kind") == "sample"],
            [r for r in rows if r.get("kind") == "mirror"], text)


def _ies_variant(src, dst, name):
    """A copy of the fixture naming a different .ies. The only thing that varies.

    The bare name is enough: cscn_copy resolves a profile against the source scene's
    directory, which is assets/, exactly as the engine does.
    """
    cscn_copy(src, dst, lambda d: d["lights"][0].__setitem__("profile", name))


def _mirror_asymmetry(path):
    """Mean |pixel - its left-right mirror|, over the frame's own mean level.

    The fixture is mirror-symmetric about x = 0 -- the lamp, the camera and the plane all
    sit on it -- so the ONLY thing that can make the frame lopsided is the profile's
    azimuthal term. Which makes this a direct read of the horizontal fold: a bilateral
    sweep MIRRORS, so the two halves must match, and a modulo wraps instead and reads the
    far side of the luminaire as the near side.

    Relative to the level because the profiles differ in how much light they put on the
    floor at all, and an absolute bound would then mean something different for each.
    """
    w, h, pix = _read_ppm(path)
    diff, total, n = 0.0, 0.0, 0
    for y in range(h):
        row = y * w
        for x in range(w // 2):
            a, b = (row + x) * 3, (row + (w - 1 - x)) * 3
            la = sum(_SRGB_TO_LINEAR[pix[a + k]] for k in range(3)) / 3.0
            lb = sum(_SRGB_TO_LINEAR[pix[b + k]] for k in range(3)) / 3.0
            diff += abs(la - lb)
            total += la + lb
            n += 1
    return diff / n / max(total / (2 * n), 1e-9)


# Sized against both sides of a hand falsification (iesFold -> mod(angle, span)):
#
#   profile      mirror   modulo
#   bilateral    0.0055   0.9861
#   quadrant     0.0059   0.8347
#
# 0.05 sits an order of magnitude above the dither floor and an order below the wrong
# answer. The symmetric file never reaches the fold (hTaps == 1) and the asymmetric one
# spans 360, where a modulo IS the identity -- so those two read 0.0047 and 1.2139 either
# way, which is what makes them the controls rather than more of the same test.
IES_MIRROR_MAX = 0.05
IES_ASYM_MIN = 0.5


# Contact shadows from the lights that cannot have a shadow map (spec 11.56), on
# assets/contact_local_fixture.
#
# The only contact-shadow test before this was a 0 px golden of the debug view on a
# sun-only scene, which pins the march's arithmetic and says nothing about which lights
# it marches. These arms are about the lights: a point or spot holding no punctual layer
# -- which past the first point light in a scene is every one of them, since the atlas
# holds 8 layers and a point spends 6.
#
# Read through --cs-debug rather than off the lit frame. The term is what is under test
# and the lit frame carries it multiplied by csStrength into a sum that also holds
# ambient, both practicals and the AO factor; dividing two lit frames would recover it
# only up to everything else that moved.
#
# These numbers mirror gen_contact_local_fixture.py and have to match it or the gate
# reads a scan line the fixture does not predict; the camera comes from the scene file.
CONTACT_FIXTURE = "contact_local_fixture.cscn"
CONTACT_BARE_MODEL = "contact_local_bare.gltf"
CONTACT_LOCAL = _cscn_camera(
    CONTACT_FIXTURE,
    strip_z=0.55,       # the receiver line, in front of the cube's z = +0.40 face
    strip_half_x=0.30,  # sampled across this, all of it with the cube behind it
    open_x=1.5,         # the in-frame control, on the SAME line
    open_half_x=0.20,
    samples=41,
)
# Pinned rather than left to the app's scene-radius heuristic, for the reason the cloud
# arms pin their coverage: an arm wants the configuration where the property is legible
# and must not be silently re-tuned by a change of default.
CONTACT_CS_DISTANCE = "0.8"
# 0.364 measured. The bar is a floor on the DARKENING, not a ceiling on the term, so a
# change that weakens the march fails here instead of drifting toward invisible.
CONTACT_DARKEN_MIN = 0.20
# Where nothing occludes, the term is exactly 1 -- an identity, not an approximation, so
# the bar is one 8-bit code off it rather than a tolerance.
CONTACT_LIT_MIN = 1.0 - 1.5 / 255.0
# What backing the light's own occlusion out of three different intensity ratios is
# allowed to disagree by, relative. Measured 0.8%.
CONTACT_FOLD_SPREAD_MAX = 0.03
# ...and how far apart the three terms must be before that agreement means anything.
# Three identical frames agree perfectly. Measured 0.44.
CONTACT_FOLD_SEPARATION_MIN = 0.25
# The map-less reading a mapped light must return to under --no-shadows. Two renders of
# one build are byte-identical here, so this is quantization on the sample line only.
CONTACT_SKIP_TOL = 0.004

# A shadow-casting directional for the MIXED path -- a key light and local lights folded
# together, which the fixture cannot author because it exists to have no directional at all.
# Travel direction is up-and-toward-the-camera reversed, so the strip's ray to it leaves
# over the open ground and the sun contributes weight WITHOUT occlusion of its own: the arm
# then reads dilution alone. Intensity is chosen so its weight lands near the two
# practicals' combined weight, which keeps both steps visible in 8 bits.
CONTACT_SUN = {"name": "key_sun", "type": "directional",
               "direction": [0.0, -0.70710678, -0.70710678],
               "color": [1.0, 1.0, 1.0], "intensity": 1.0, "cast_shadows": True}
# The mapped lamp is a copy of practical_front -- same place, so it is unoccluded too and
# can only enter the denominator -- at this multiple of its intensity.
CONTACT_MAPPED_GAIN = 3.0
# Measured 0.1866 and 0.0765. Each step is a light joining the DENOMINATOR and nothing
# else, so a build that drops either reads exactly 0.0000 rather than a smaller number --
# these floors are generous because the failure is total, not gradual.
CONTACT_KEY_STEP_MIN = 0.08
CONTACT_MAPPED_STEP_MIN = 0.03
# The frame --shadows-off-at fires on. Anything after the first depth pass reaches the
# transition; 5 leaves 25 frames rendering in the stale state before the capture.
CONTACT_SHADOWS_OFF_FRAME = "5"


def _contact_light(d, name):
    return next(light for light in d["lights"] if light["name"] == name)


def _contact_term(pix, w, h, project, x0, x1):
    """Mean contact-shadow visibility along the ground line z = strip_z, x in [x0, x1].

    RAW bytes, NOT _linear_luma. --cs-debug returns early from the tonemap and writes the
    term straight out with no display encode on it, so decoding sRGB here would report a
    measured 0.6360 as 0.3599 and every predicted ratio would miss.

    A LINE and not a box, because the fixture's two lights are mirrored about this z and
    only about this z. Spreading the sample in depth breaks the symmetry the fold
    prediction is derived from -- 2.8% of it at only 0.02 units either side.
    """
    z = CONTACT_LOCAL["strip_z"]
    n = CONTACT_LOCAL["samples"]
    total = 0.0
    for i in range(n):
        px, py = project((x0 + (x1 - x0) * (i + 0.5) / n, 0.0, z))
        x = max(0, min(w - 1, int(round(px))))
        y = max(0, min(h - 1, int(round(py))))
        total += pix[(y * w + x) * 3] / 255.0
    return total / n


def _contact_read(workdir, tag, mutate, extra=None):
    """Render one variant of the fixture and return (strip, control), or (None, error).

    Both reads come out of ONE frame and one row of pixels, so a projection off by a
    texel moves them together and the control stays a control.
    """
    scn = os.path.join(workdir, f"contact_{tag}.cscn")
    cscn_copy(os.path.join(ROOT, "assets", CONTACT_FIXTURE), scn, mutate)
    out = os.path.join(workdir, f"contact_{tag}.ppm")
    err = render(scn, out, ["-W", "640", "-H", "400", "--no-auto-exposure", "-E", "1.0",
                            "--cs-debug", "--cs-distance", CONTACT_CS_DISTANCE] + (extra or []))
    if err:
        return None, err
    w, h, pix = _read_ppm(out)
    project = _projector(CONTACT_LOCAL, w, h)
    p = CONTACT_LOCAL
    return ((_contact_term(pix, w, h, project, -p["strip_half_x"], p["strip_half_x"]),
             _contact_term(pix, w, h, project, p["open_x"] - p["open_half_x"],
                           p["open_x"] + p["open_half_x"])), None)


def run_ies_gate(workdir):
    """IES profiles, read as numbers because no frame can tell a wrong table from a right one.

      ies-table     every tap of all four profiles matches the candela the generator
                    constructed, and the 90 degree tail is EXACTLY zero
      ies-symmetry  each file's declared symmetry and span are read off the declaration --
                    one plane is rotational, and 0..90 / 0..180 / 0..360 are quadrant,
                    bilateral and none
      ies-seed      a light authoring no intensity takes the file's own peak candela, which
                    is what makes the normalised shape lossless
      ies-shape     the profile actually reaches the frame at all
      ies-mirror    a PARTIAL sweep mirrors rather than repeats -- read off the frame,
                    since the fold is unreachable from the probe
      ies-replace   ...and the profile REPLACES the cone rather than multiplying it
      ies-flag      --ies-profile overrides what a scene authored, and reaches a scene
                    that authored nothing

    The first three read --ies-probe and the last three render. That split is deliberate:
    the probe exists because a table resampled off the wrong plane still lights a room, so
    the arms that could be fooled by a picture do not look at one -- but the shader is a
    THIRD implementation of the same table, and no probe reaches it. Both halves are
    needed and neither substitutes.
    """
    fixture = os.path.join(ROOT, "assets", "ies_fixture.cscn")
    if not os.path.exists(fixture):
        print("  ies-table    SKIP  (missing ies_fixture.cscn)")
        return []
    failures = []

    # One probe run per file, since a scene names one profile at a time.
    reads = {}
    err = None
    for name, span, symmetric in IES_FILES:
        variant = os.path.join(workdir, "ies_" + name.replace(".ies", ".cscn"))
        _ies_variant(fixture, variant, name)
        profiles, samples, mirror, text = _ies_probe(workdir, variant)
        if len(profiles) != 1 or not samples:
            err = f"{name}: {len(profiles)} profile rows, {len(samples)} samples"
            break
        reads[name] = (profiles[0], samples, mirror)

    if err:
        arms = ("ies-table", "ies-symmetry", "ies-seed", "ies-mirror", "ies-shape",
                "ies-replace", "ies-flag")
        for arm in arms:
            print(f"  {arm} ERROR the probe returned nothing usable: {err}")
        return list(arms)

    worst, worst_at, tails = 0.0, "", []
    for name, span, symmetric in IES_FILES:
        _, samples, _ = reads[name]
        for s in samples:
            v, h, cd = float(s["v"]), float(s["h"]), float(s["cd"])
            want = _ies_expected(v, h, span)
            rel = abs(cd - want) / IES_PEAK_CD
            if rel > worst:
                worst, worst_at = rel, f"{name} v={v:g} h={h:g}"
            if v >= 90.0 - 1e-6:
                tails.append(cd)
    tail_ok = all(t == 0.0 for t in tails)
    ok = worst <= IES_TAP_REL_EPS and tail_ok and tails
    print(f"  ies-table    {'PASS' if ok else 'FAIL'}  worst tap error {worst:.3e} relative "
          f"over {sum(len(reads[n][1]) for n, _, _ in IES_FILES)} taps in {len(IES_FILES)} "
          f"files (want <= {IES_TAP_REL_EPS:g}{', at ' + worst_at if worst_at else ''}); "
          f"{len(tails)} tail taps, all exactly zero: {tail_ok} "
          "(exact, because pbr_frag's early-out is only safe if it is)")
    if not ok:
        failures.append("ies-table")

    problems = []
    for name, span, symmetric in IES_FILES:
        p, _, mirror = reads[name]
        got_span, got_sym = float(p["span"]), p["symmetric"] == "1"
        if abs(got_span - span) > 1e-3:
            problems.append(f"{name} span {got_span:g} != {span:g}")
        if got_sym != symmetric:
            problems.append(f"{name} symmetric={got_sym} != {symmetric}")
        # The C fold, on the one input that can distinguish a mirror from a wrap. Every
        # `sample` row above sits inside [0, span] where the two agree exactly, so without
        # these the C copy is unreachable from outside the process. A full 360 sweep has
        # no outside and prints none.
        if span < 360.0 and not mirror:
            problems.append(f"{name} spans {span:g} and printed no mirror rows")
        for m in mirror:
            inside, beyond = float(m["inside"]), float(m["beyond"])
            if abs(inside - beyond) > IES_TAP_REL_EPS:
                problems.append(f"{name} v={float(m['v']):g} folds span+{float(m['h']):g} to "
                                f"{beyond:.6f}, not to span-{float(m['h']):g}'s {inside:.6f}")
                break
    ok = not problems
    print(f"  ies-symmetry {'PASS' if ok else 'FAIL'}  " +
          (f"all {len(IES_FILES)} declarations read back: " +
           ", ".join(f"{n} {float(reads[n][0]['span']):g}deg x{reads[n][0]['h_taps']}"
                     for n, _, _ in IES_FILES)
           if ok else "; ".join(problems)))
    if not ok:
        failures.append("ies-symmetry")

    # The fixture authors NO intensity, so the light must come out at the file's peak.
    _, _, _, text = _ies_probe(workdir, fixture)
    seeded = f"takes its {IES_PEAK_CD:.1f} cd" in text
    lit = f"intensity={IES_PEAK_CD:.6f} cd" in text
    ok = seeded and lit
    print(f"  ies-seed     {'PASS' if ok else 'FAIL'}  unpriced light seeded from the file: "
          f"reported={seeded} on the light={lit} (want both at {IES_PEAK_CD:.0f} cd -- "
          "normalised x peak IS absolute, and this is the step that makes it so)")
    if not ok:
        failures.append("ies-seed")

    # ...and the arms that look at a frame, because "the table is right" and "the shader
    # reads it" are different claims -- and the second is the one that decides pixels.
    def _no_profile(d):
        d["lights"][0].pop("profile", None)
        # The bare light must still be as bright as the profiled one, or this arm
        # measures the SEEDING rather than the shape: without a profile there is
        # no peak to seed from, so it would otherwise fall back to 1 cd.
        d["lights"][0]["intensity"] = IES_PEAK_CD

    base = os.path.join(workdir, "ies_shape_none.cscn")
    cscn_copy(fixture, base, _no_profile)

    shots, render_err = {}, None
    scenes = [("none", base)]
    for name, _, _ in IES_FILES:
        tag = name.replace("ies_", "").replace(".ies", "")
        variant = os.path.join(workdir, f"ies_shape_{tag}.cscn")
        _ies_variant(fixture, variant, name)
        scenes.append((tag, variant))
    for tag, scene in scenes:
        out = os.path.join(workdir, f"ies_shape_{tag}.ppm")
        render_err = render(scene, out, ["-W", "400", "-H", "300", "--no-auto-exposure",
                                         "-E", "0.02"])
        if render_err:
            break
        shots[tag] = out

    if render_err:
        for arm in ("ies-shape", "ies-mirror"):
            print(f"  {arm} ERROR render failed: {render_err.strip()[-200:]}")
        failures += ["ies-shape", "ies-mirror"]
    else:
        moved = compare(shots["none"], shots["symmetric"])[0]
        differs = compare(shots["symmetric"], shots["asymmetric"])[0]
        ok = moved > 0 and differs > 0
        print(f"  ies-shape    {'PASS' if ok else 'FAIL'}  profile vs bare cone {moved} px "
              f"(want > 0: the shader reads the table at all); symmetric vs asymmetric "
              f"{differs} px (want > 0, or the horizontal term is inert and every profile "
              "renders as a lobe)")
        if not ok:
            failures.append("ies-shape")

        # The FOLD, which nothing above can reach: every probe row lies inside [0, span],
        # where a mirror and a modulo agree exactly. Only a rendered frame asks for an
        # angle outside the sweep, and only the two partial sweeps have an outside.
        asym = {name: _mirror_asymmetry(shots[name]) for name in
                ("symmetric", "bilateral", "quadrant", "asymmetric")}
        folded_ok = asym["bilateral"] <= IES_MIRROR_MAX and asym["quadrant"] <= IES_MIRROR_MAX
        # ...and the control, without which "every frame is symmetric" passes perfectly on
        # a shader that ignores the horizontal angle entirely.
        live_ok = asym["asymmetric"] >= IES_ASYM_MIN
        ok = folded_ok and live_ok
        print(f"  ies-mirror   {'PASS' if ok else 'FAIL'}  partial sweeps mirror: bilateral "
              f"{asym['bilateral']:.4f}, quadrant {asym['quadrant']:.4f} "
              f"(want <= {IES_MIRROR_MAX}; a modulo fold reads 0.99 and 0.83); "
              f"controls symmetric {asym['symmetric']:.4f}, asymmetric "
              f"{asym['asymmetric']:.4f} (want >= {IES_ASYM_MIN}, or the azimuth is inert)")
        if not ok:
            failures.append("ies-mirror")

    # A profile REPLACES the cone rather than multiplying it. Untestable on the fixture as
    # authored -- it is a POINT light, and spotConeFactor returns 1.0 for every non-spot,
    # so multiplying and replacing are byte-identical there. Two SPOTS differing in
    # nothing but their cone: the profile carries the cutoff, so the cone must reach
    # nothing and the two frames must be the same one. Multiply instead and the 8-degree
    # cone clips the lobe the 85-degree one leaves whole.
    #
    # Exact 0 px is safe to demand: the clusterer never culls by cone (cutOff is packed
    # and not otherwise read), and this light does not cast, so no shadow frustum is
    # fitted from it either.
    def _spot(cone):
        def mutate(d):
            d["lights"][0].update({"type": "spot", "direction": [0.0, -1.0, 0.0],
                                   "cone": cone, "profile": "ies_symmetric.ies"})
        return mutate

    cone_shots, cone_err = {}, None
    for tag, cone in (("narrow", [5.0, 8.0]), ("wide", [80.0, 85.0])):
        scene = os.path.join(workdir, f"ies_cone_{tag}.cscn")
        cscn_copy(fixture, scene, _spot(cone))
        out = os.path.join(workdir, f"ies_cone_{tag}.ppm")
        cone_err = render(scene, out, ["-W", "400", "-H", "300", "--no-auto-exposure",
                                       "-E", "0.02"])
        if cone_err:
            break
        cone_shots[tag] = out
    if cone_err:
        print(f"  ies-replace  ERROR render failed: {cone_err.strip()[-200:]}")
        failures.append("ies-replace")
    else:
        px = compare(cone_shots["narrow"], cone_shots["wide"])[0]
        ok = px == 0
        print(f"  ies-replace  {'PASS' if ok else 'FAIL'}  a 5/8 deg cone and an 80/85 deg "
              f"cone under one profile differ by {px} px (want exactly 0: the profile is "
              "the whole distribution, cutoff included, so the authored cone reaches "
              "nothing)")
        if not ok:
            failures.append("ies-replace")

    # --ies-profile, the only route to a profile that does not go through a scene file.
    # Two claims, and the second is the one that can rot: the flag must WIN over what the
    # scene authored, and it must reach a light the scene never gave a profile at all.
    #
    # Read as pixels against the authored frame rather than off the probe, because a flag
    # that loaded the file and applied it to nothing prints an identical probe -- the
    # library is scene state, and loading is not applying.
    flag_shots, flag_err = {}, None
    flag_runs = (
        # The fixture authors ies_symmetric; overriding with the asymmetric file must move
        # the frame, and must land on the SAME frame the authored asymmetric variant gives.
        ("override", fixture, ["--ies-profile", os.path.join(ROOT, "assets",
                                                             "ies_asymmetric.ies")]),
        # ...and a scene with two point lights and no profile anywhere in it.
        ("bare", os.path.join(ROOT, "assets", "contact_local_fixture.cscn"), []),
        ("bare_ies", os.path.join(ROOT, "assets", "contact_local_fixture.cscn"),
         ["--ies-profile", os.path.join(ROOT, "assets", "ies_asymmetric.ies")]),
    )
    for tag, scene, extra in flag_runs:
        if not os.path.exists(scene):
            flag_err = f"missing {os.path.basename(scene)}"
            break
        out = os.path.join(workdir, f"ies_flag_{tag}.ppm")
        flag_err = render(scene, out, ["-W", "400", "-H", "300", "--no-auto-exposure",
                                       "-E", "0.02"] + extra)
        if flag_err:
            break
        flag_shots[tag] = out

    if flag_err:
        print(f"  ies-flag     ERROR render failed: {flag_err.strip()[-200:]}")
        failures.append("ies-flag")
    else:
        # The authored asymmetric variant was rendered above by ies-shape.
        same = compare(flag_shots["override"], shots["asymmetric"])[0]
        moved = compare(flag_shots["override"], shots["symmetric"])[0]
        reached = compare(flag_shots["bare"], flag_shots["bare_ies"])[0]
        ok = same == 0 and moved > 0 and reached > 0
        print(f"  ies-flag     {'PASS' if ok else 'FAIL'}  --ies-profile over an authored "
              f"one: {same} px against the same file authored (want exactly 0) and {moved} "
              f"px against the file it replaced (want > 0, or the flag did nothing); on a "
              f"scene that authors no profile at all: {reached} px (want > 0)")
        if not ok:
            failures.append("ies-flag")

    return failures


AO_SCENE = os.path.join(ROOT, "assets", "cornell_rooms.cscn")
# AO on vs off moves 32% of this frame at PAE 71/255. The bar is a fraction of
# that with room to spare: what it exists to catch is the chain going SILENT --
# an FBO the driver refuses, a gate that stopped arming, a format nothing can
# render to -- not a shift in how dark the corners get.
AO_ACTIVE_MIN_FRAC = 0.05

# The ring read. Columns across the contact band on cornell_rooms' floor, and
# the rows that span it -- picked because the AO buffer is strictly monotone
# through this window, which is what makes it usable as a control.
AO_RING_COLS = range(600, 1001, 50)
AO_RING_ROWS = (745, 815)
# A step smaller than this is dither and quantisation, not a band. The bands
# this exists to catch measured 17-22 codes.
AO_RING_TOL = 4
# One column has to actually reach into shadow or the monotonicity assertion is
# satisfied by a flat frame. The deepest column reads 5 with the term working.
AO_RING_DIP_MAX = 128
# The open floor, past the contact band and still on lit geometry. The second
# half of that is a constraint, not a description: below row ~815 this frame is
# black background, where the term's zero-normal guard hands back plain AO and
# reads 255 whatever the term did. The first draft of this arm sampled exactly
# there and passed a build whose numerator and denominator described different
# lobes -- so the sampled band is checked against the LIT frame every run.
AO_RING_OPEN_ROWS = range(785, 816, 5)
AO_RING_OPEN_COLS = range(300, 1301, 50)
# Max channel a sample must reach in the lit frame to count as geometry.
AO_RING_OPEN_MIN_LIT = 32


def _ao_ring_reversals(px, w, x):
    """Steps that DARKEN by more than the tolerance while scanning outward from
    the darkest row of the window. Approaching an occluder is allowed to darken;
    coming back out into the open and darkening again is the artifact."""
    y0, y1 = AO_RING_ROWS
    vals = [px[(y * w + x) * 3] for y in range(y0, y1)]
    lo = min(range(len(vals)), key=lambda k: vals[k])
    return vals, sum(1 for k in range(lo + 1, len(vals))
                     if vals[k - 1] - vals[k] > AO_RING_TOL)


def run_ao_gate(workdir):
    """Ambient occlusion reaches the frame, and does it the same way twice.

      ao-active        AO on and AO off are different pictures, by a wide
                       margin -- the chain is armed, allocated and composited
      ao-deterministic two runs of the AO-on frame are byte-identical
      ao-ring          the specular-occlusion term dips into a contact and comes
                       back out without banding, measured against the AO buffer's
                       own monotonicity through the same window

    ao-ring IS THE ONLY EXECUTABLE COVERAGE OF ANY SPECULAR-OCCLUSION MODE.
    Before it the whole feature -- four modes, the split composite's blend
    algebra, the term itself -- was pinned by golden pixels and nothing else,
    which is how spec 11.75 shipped a cliff in it with every gate green.

    Its three assertions are each vacuous alone, and that is why there are
    three. A build that computes nothing reads a flat 255: perfectly monotone,
    perfectly exact on open floor, and wrong -- so the dip is asserted first. A
    build that saturates everything dips beautifully and bands horribly -- so
    monotonicity is asserted against the AO buffer rather than against a
    constant, because the AO is the same estimator's own output through the same
    window and is strictly monotone there. And the open-floor read catches a
    third failure neither of the others can see: where nothing occludes, the
    term's numerator and denominator are sums over the SAME sectors, so the
    ratio is exactly one by construction -- for any lobe, at any roughness, at
    any angle. Anything less means the two stopped describing one lobe, or the
    accumulation and blur moved them apart. It is an exact algebraic identity
    rather than a measurement, which is what makes it worth an equality test.

    The bar is the AO's count, not zero, deliberately. What the artifact is is
    the specular term being LESS monotone than the occlusion it derives from;
    a scene whose AO genuinely reverses through the window should be allowed to
    reverse here too. On this fixture the AO reads 0 and the cone term read 27.

    All three were falsified by hand. Making lobeSectors return no bits reads a
    flat 255 and is caught by the dip alone, with the other two passing
    perfectly. Mismatching the numerator's lobe against the denominator's
    (popCount(lobe) + 1) is caught by the exactness alone, at 0/147. The
    monotonicity arm was measured against the cone term itself -- the same
    counting over the same columns on the pre-change renderer's own output --
    which is where the 27 comes from.

    THE EXACTNESS ASSERTION WAS VACUOUS IN ITS FIRST DRAFT and the mutation is
    what found it. It sampled rows 1000-1100, which look like flat 255 in every
    version of this frame and are black background: out there the term's
    zero-normal guard hands back plain AO, so the lobe mismatch passed. The band
    moved onto lit floor, and the lit frame is now sampled alongside it so the
    same drift cannot happen silently again. It is the beach-shoreline lesson --
    a read that agrees with itself on the wrong surface.

    TWO OF THE FOUR ARMS THIS GROUP WANTED COULD NOT BE BUILT. Spec 11.75
    planned them and neither could be written honestly; both attempts are
    recorded here because the next person will have the same two ideas.

    **A silhouette arm was measured and abandoned.** 11.75's depth-aware
    upsample stops AO bleeding across a depth edge, and the effect is real --
    reverting it to plain bilinear moves 3,252 px here, peaking at 53/255. But
    it is a ONE-TO-TWO PIXEL band along silhouettes by construction, which is
    the entire reach of a 2x2 footprint, so there is no region a box mean can
    read: averaging any rectangle containing it dilutes 53 codes to 0.0003.
    Selecting the affected pixels instead would mean selecting them by whether
    the mutation moved them, which is circular. The property is real, worth
    having, and lives in the goldens rather than here.

    **A banding arm was designed against a premise that turned out false.** The
    AO target went RGBA8 -> RGBA16F in 11.75 on the theory that ~20 codes across
    an 0.92-1.00 gradient was what the visible contour banding was made of.
    Measured after: identical flat-run lengths and identical span. The bands are
    the ESTIMATOR's (2 slices, gtao_frag.glsl), not the storage's, so an arm
    asserting short runs would fail on a correct build. The format change stands
    on its other reason -- see postfx.c -- and this arm is not written.

    What is left is coverage of the thing that actually breaks silently: an AO
    chain that stops running. Nothing else in the suite asserts that; before
    this group the entire chain's only executable coverage was golden pixels.
    """
    if not os.path.exists(AO_SCENE):
        print("  ao-active    SKIP  (cornell_rooms.cscn not present)")
        return []
    failures = []
    flags = ["--sky", "--no-auto-exposure", "-E", "1.0", "-W", "800", "-H", "600"]
    on = os.path.join(workdir, "ao_on.ppm")
    off = os.path.join(workdir, "ao_off.ppm")
    on2 = os.path.join(workdir, "ao_on2.ppm")
    err = (render(AO_SCENE, on, flags) or render(AO_SCENE, off, flags + ["--no-ssao"]) or
           render(AO_SCENE, on2, flags))
    if err:
        print(f"  ao-active    FAIL  render error\n{err}")
        print("  ao-deterministic FAIL  (same)")
        return ["ao-active", "ao-deterministic"]

    moved, _ = compare(on, off)
    w, h, _ = _read_ppm(on)
    frac = moved / float(w * h)
    ok = frac >= AO_ACTIVE_MIN_FRAC
    if not ok:
        failures.append("ao-active")
    print(f"  ao-active    {'PASS' if ok else 'FAIL'}  AO moves {frac * 100.0:.1f}% of the "
          f"frame ({moved} px, want >= {AO_ACTIVE_MIN_FRAC * 100.0:.0f}%); a chain that "
          f"silently stopped running reads 0.0%")

    same, _ = compare(on, on2)
    ok = same == 0
    if not ok:
        failures.append("ao-deterministic")
    print(f"  ao-deterministic {'PASS' if ok else 'FAIL'}  two runs differ by {same} px "
          f"(want 0: no --taa, so every temporal history is force-invalidated and GTAO "
          f"is a pure function of the frame)")

    term = os.path.join(workdir, "ao_term.ppm")
    aobuf = os.path.join(workdir, "ao_raw.ppm")
    err = (render(AO_SCENE, term, flags + ["--spec-occ-debug"]) or
           render(AO_SCENE, aobuf, flags + ["--ssao-debug"]))
    if err:
        print(f"  ao-ring      FAIL  render error\n{err}")
        return failures + ["ao-ring"]

    tw, _, tpx = _read_ppm(term)
    aw, _, apx = _read_ppm(aobuf)
    term_rev = sum(_ao_ring_reversals(tpx, tw, x)[1] for x in AO_RING_COLS)
    ao_rev = sum(_ao_ring_reversals(apx, aw, x)[1] for x in AO_RING_COLS)
    deepest = min(min(_ao_ring_reversals(tpx, tw, x)[0]) for x in AO_RING_COLS)
    lw, _, lpx = _read_ppm(on)
    open_vals, open_lit = [], []
    for y in AO_RING_OPEN_ROWS:
        for x in AO_RING_OPEN_COLS:
            open_vals.append(tpx[(y * tw + x) * 3])
            o = (y * lw + x) * 3
            open_lit.append(max(lpx[o], lpx[o + 1], lpx[o + 2]))
    open_exact = sum(1 for v in open_vals if v == 255)
    open_geom = sum(1 for v in open_lit if v >= AO_RING_OPEN_MIN_LIT)

    dips = deepest <= AO_RING_DIP_MAX
    monotone = term_rev <= ao_rev
    exact = open_exact == len(open_vals) and open_geom == len(open_lit)
    ok = dips and monotone and exact
    if not ok:
        failures.append("ao-ring")
    print(f"  ao-ring      {'PASS' if ok else 'FAIL'}  the term reaches {deepest} at its "
          f"darkest (want <= {AO_RING_DIP_MAX}, or a build computing nothing reads a flat 255 "
          f"and satisfies the other two perfectly); {term_rev} darkening steps > "
          f"{AO_RING_TOL} codes on the way back out against the AO's own {ao_rev} across "
          f"{len(AO_RING_COLS)} columns (want <= it -- the cone term this replaced read 27); "
          f"open floor {open_exact}/{len(open_vals)} exactly 255 (want all: with nothing "
          f"occluding, visible and total are sums over the same sectors and the ratio is "
          f"exactly one, so anything less means they were not measured against the same "
          f"lobe or the denoise chain moved them apart), on {open_geom}/{len(open_lit)} "
          f"samples that are lit geometry (want all, or the band has slid off onto the "
          f"black background where the term is not consulted at all)")

    return failures


def run_contact_gate(workdir):
    """Contact shadows for local lights, which is the population with no other occlusion.

      contact-local   a map-less practical darkens a contact, open ground in the same
                      frame reads exactly 1, and so does the same scene with the occluder
                      deleted
      contact-mapped  a light that HAS a punctual map contributes nothing to the term --
                      and the same scene under --no-shadows darkens again, so the skip is
                      reading the map rather than the authored flag
      contact-fold    the fold is weighted by contribution: scaling the unoccluded light
                      16x recovers the same occluder to under a percent, which an
                      unweighted fold cannot do
      contact-mixed   a key directional and local lights fold TOGETHER, and each light
                      that reaches the pixel divides the term -- including a mapped one,
                      which is never marched but still lights the pixel
      contact-stale   a mapped light whose shadow system is switched off MID-RUN is
                      marched again, because shadow_layer is only maintained while the
                      depth pass runs and --no-shadows cannot reach that transition

    The occluder-removed half of the first arm is a committed twin rather than a mutation,
    because a .cscn cannot delete a node: gen_contact_local_fixture.py emits the bare
    ground from the same vertex arrays, so the two files differ in the drawn geometry and
    in nothing else.

    NO ARM HERE IS SAFE ALONE, and contact-mapped is the one to watch: "the light with a
    map contributed nothing" is satisfied perfectly by a build that marches no local light
    at all. Falsified by hand at 11.56 -- stubbing the cluster list empty fails
    contact-local and contact-fold and leaves contact-mapped green; removing the map skip
    fails contact-mapped alone; making the fold unweighted fails contact-fold alone;
    dropping skipped lights from the denominator collapses contact-mixed's second step to
    exactly 0.0000; packing the raw shadow_layer takes contact-stale from the map-less
    reading to 1.0000.
    """
    fixture = os.path.join(ROOT, "assets", CONTACT_FIXTURE)
    if not os.path.exists(fixture):
        print(f"  contact-local SKIP  ({CONTACT_FIXTURE} not present)")
        return []
    failures = []

    # Every arm reads the unmutated fixture, so losing it fails all three -- named
    # one per line rather than once, or two of them appear in the summary with
    # nothing above explaining why.
    base, err = _contact_read(workdir, "base", lambda d: None)
    if err:
        for arm in ("contact-local", "contact-mapped", "contact-fold"):
            print(f"  {arm} ERROR the base render failed: {err.strip()[-200:]}")
        return ["contact-local", "contact-mapped", "contact-fold"]
    bare, err = _contact_read(
        workdir, "bare", lambda d: d["models"].__setitem__(0, {"path": CONTACT_BARE_MODEL}))
    if err:
        print(f"  contact-local ERROR bare-twin render failed: {err.strip()[-200:]}")
        failures.append("contact-local")
    else:
        darken = 1.0 - base[0]
        ok = (darken >= CONTACT_DARKEN_MIN and base[1] >= CONTACT_LIT_MIN
              and bare[0] >= CONTACT_LIT_MIN)
        print(f"  contact-local {'PASS' if ok else 'FAIL'}  strip {base[0]:.4f} "
              f"(darkening {darken:.4f}, want >= {CONTACT_DARKEN_MIN}); open ground "
              f"{base[1]:.4f} and occluder removed {bare[0]:.4f} "
              f"(both want >= {CONTACT_LIT_MIN:.4f} -- the falsifiers, one in frame and "
              f"one without the cube)")
        if not ok:
            failures.append("contact-local")

    def _map_back(d):
        _contact_light(d, "practical_back")["cast_shadows"] = True

    mapped, err = _contact_read(workdir, "mapped", _map_back)
    unmapped, err2 = _contact_read(workdir, "mapped_nomaps", _map_back, ["--no-shadows"])
    if err or err2:
        print(f"  contact-mapped ERROR render failed: {(err or err2).strip()[-200:]}")
        failures.append("contact-mapped")
    else:
        drift = abs(unmapped[0] - base[0])
        ok = mapped[0] >= CONTACT_LIT_MIN and drift <= CONTACT_SKIP_TOL
        print(f"  contact-mapped {'PASS' if ok else 'FAIL'}  the mapped light's strip "
              f"{mapped[0]:.4f} (want >= {CONTACT_LIT_MIN:.4f}: its map already resolves "
              f"the contact); under --no-shadows the same scene reads {unmapped[0]:.4f} "
              f"vs {base[0]:.4f} map-less, drift {drift:.4f} (want <= {CONTACT_SKIP_TOL})")
        if not ok:
            failures.append("contact-mapped")

    with open(fixture) as fh:
        authored = json.load(fh)
    i_back = _contact_light(authored, "practical_back")["intensity"]
    i_front = _contact_light(authored, "practical_front")["intensity"]

    def _scale_front(factor):
        def mutate(d):
            _contact_light(d, "practical_front")["intensity"] = i_front * factor
        return mutate

    # The occluded light's share of the two weights. Exact rather than fitted: the pair is
    # equidistant from every point of this line and shares an N.L there, so their weight
    # ratio IS their intensity ratio and nothing else about the geometry enters.
    reads, fold_err = {1.0: base}, None
    for factor in (4.0, 0.25):
        reads[factor], fold_err = _contact_read(workdir, f"front{factor:g}".replace(".", ""),
                                                _scale_front(factor))
        if fold_err:
            break
    if fold_err:
        print(f"  contact-fold  ERROR render failed: {fold_err.strip()[-200:]}")
        failures.append("contact-fold")
    else:
        occ, terms = {}, {}
        for factor, read in reads.items():
            share = i_back / (i_back + i_front * factor)
            occ[factor] = (1.0 - read[0]) / share
            terms[factor] = read[0]
        spread = max(occ.values()) / max(min(occ.values()), 1e-6) - 1.0
        separation = max(terms.values()) - min(terms.values())
        ok = spread <= CONTACT_FOLD_SPREAD_MAX and separation >= CONTACT_FOLD_SEPARATION_MIN
        detail = " ".join(f"x{f:g}: term {terms[f]:.4f} -> occ {occ[f]:.4f}"
                          for f in sorted(reads))
        print(f"  contact-fold  {'PASS' if ok else 'FAIL'}  {detail}; spread "
              f"{spread:.4f} (want <= {CONTACT_FOLD_SPREAD_MAX}), term separation "
              f"{separation:.4f} (want >= {CONTACT_FOLD_SEPARATION_MIN}, or three equal "
              f"frames would agree perfectly)")
        if not ok:
            failures.append("contact-fold")

    # THE MIXED PATH: a key directional folded together with local lights, which the
    # fixture cannot author itself. Two steps, each adding exactly one light that
    # contributes to the DENOMINATOR and nothing else -- the sun is unoccluded at the
    # strip by construction, and the mapped lamp is never marched at all. So each step
    # measures the denominator alone, and a build that omits either reads 0.0000 for it.
    def _add_sun(d):
        d["lights"].append(dict(CONTACT_SUN))

    def _add_sun_and_mapped(d):
        _add_sun(d)
        front = _contact_light(d, "practical_front")
        d["lights"].append(dict(front, name="mapped_lamp", cast_shadows=True,
                                intensity=front["intensity"] * CONTACT_MAPPED_GAIN))

    keyed, err = _contact_read(workdir, "keyed", _add_sun)
    keyed_mapped, err2 = _contact_read(workdir, "keyed_mapped", _add_sun_and_mapped)
    if err or err2:
        print(f"  contact-mixed ERROR render failed: {(err or err2).strip()[-200:]}")
        failures.append("contact-mixed")
    else:
        key_step = keyed[0] - base[0]
        mapped_step = keyed_mapped[0] - keyed[0]
        ok = (key_step >= CONTACT_KEY_STEP_MIN and mapped_step >= CONTACT_MAPPED_STEP_MIN
              and keyed_mapped[1] >= CONTACT_LIT_MIN)
        print(f"  contact-mixed {'PASS' if ok else 'FAIL'}  strip {base[0]:.4f} local-only -> "
              f"{keyed[0]:.4f} with a key directional (step {key_step:.4f}, want >= "
              f"{CONTACT_KEY_STEP_MIN}) -> {keyed_mapped[0]:.4f} with a MAPPED lamp beside it "
              f"(step {mapped_step:.4f}, want >= {CONTACT_MAPPED_STEP_MIN}: a light with its "
              f"own map still lights the pixel, so it still divides the term); control "
              f"{keyed_mapped[1]:.4f}")
        if not ok:
            failures.append("contact-mixed")

    # Light.shadow_layer is maintained only while the depth pass runs, so turning the
    # shadow system off AFTER it has run leaves a mapped light claiming a layer nothing
    # draws. --no-shadows cannot reach that state -- it clears the switch before frame 0,
    # so no layer was ever assigned -- which is why --shadows-off-at exists.
    stale, err = _contact_read(workdir, "stale", _map_back,
                               ["--shadows-off-at", CONTACT_SHADOWS_OFF_FRAME])
    if err or mapped is None:
        # mapped is the shadows-ON half of this comparison, so losing it above takes
        # this arm with it rather than leaving it to read one number against nothing.
        print(f"  contact-stale ERROR render failed: {(err or 'the mapped read').strip()[-200:]}")
        failures.append("contact-stale")
    else:
        ok = abs(stale[0] - base[0]) <= CONTACT_SKIP_TOL and mapped[0] >= CONTACT_LIT_MIN
        print(f"  contact-stale {'PASS' if ok else 'FAIL'}  the same mapped light after the "
              f"shadow system is switched off mid-run: strip {stale[0]:.4f} vs {base[0]:.4f} "
              f"map-less (want within {CONTACT_SKIP_TOL}); with the system left ON it reads "
              f"{mapped[0]:.4f} (want >= {CONTACT_LIT_MIN:.4f}, or the two states are the "
              f"same and this measures nothing)")
        if not ok:
            failures.append("contact-stale")

    return failures


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
    # --no-dither beside --no-vignette: the hidden arm differences two SINGLE
    # pixels at different fragcoords, and an independent LSB on each can reach
    # CATCHER_HIDDEN_TOL on its own, before any ordering error contributes.
    cmd = [RENDER, "-m", scene, "--no-recenter", "-x", "-f", "30", "-W", "800", "-H", "500",
           "--no-vignette", "--no-dither", "-S", out] + extra
    r = _run(cmd, capture_output=True, text=True)
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
    # emitter of scene_radius * 0.08, which smears the analytic edge. The moment
    # arms name it too, even though --msm clears PCSS by itself, so the two
    # paths differ by exactly one flag and neither inherits a live default.
    hard = ["--no-pcss"]
    msm = hard + ["--msm"]
    r40 = _dir_render(workdir, "dir_shadow_fixture.cscn", "40", hard)
    r40_ns = _dir_render(workdir, "dir_shadow_fixture.cscn", "40_ns", hard + ["--no-shadows"])
    r40_cc1 = _dir_render(workdir, "dir_shadow_fixture.cscn", "40_cc1",
                          hard + ["--shadow-cascades", "1"])
    r10 = _dir_render(workdir, "dir_shadow_fixture_lowsun.cscn", "10", hard)
    r10_ns = _dir_render(workdir, "dir_shadow_fixture_lowsun.cscn", "10_ns",
                         hard + ["--no-shadows"])
    # Moment shadow maps (spec 11.22). The --no-shadows references are what the
    # shadow term divides BY and carry no shadow algorithm at all, so they are
    # shared rather than re-rendered: three extra frames, not six.
    m40 = _dir_render(workdir, "dir_shadow_fixture.cscn", "40_msm", msm)
    m40_cc1 = _dir_render(workdir, "dir_shadow_fixture.cscn", "40_cc1_msm",
                          msm + ["--shadow-cascades", "1"])
    m10 = _dir_render(workdir, "dir_shadow_fixture_lowsun.cscn", "10_msm", msm)
    # Deliberately blurred, for the inverse arm below. It is the only render
    # here whose job is to FAIL a bound rather than pass one.
    mblur = _dir_render(workdir, "dir_shadow_fixture.cscn", "40_msm_blur",
                        msm + ["--msm-blur", "1.0"])
    if not all((r40, r40_ns, r40_cc1, r10, r10_ns, m40, m40_cc1, m10, mblur)):
        print("  dir-shadow   ERROR while rendering the fixture")
        return failures + ["dir-shadow"]

    term40 = _term_reader(r40, r40_ns)
    term40_cc1 = _term_reader(r40_cc1, r40_ns)
    term10 = _term_reader(r10, r10_ns)
    mterm40 = _term_reader(m40, r40_ns)
    mterm40_cc1 = _term_reader(m40_cc1, r40_ns)
    mterm10 = _term_reader(m10, r10_ns)
    mterm_blur = _term_reader(mblur, r40_ns)

    # --- holes: eroded umbra of both ellipses must be dark ------------------
    # Light leaking through an umbra is the signature failure of every filterable
    # shadow representation -- the reason specs/10.4 declined VSM and the reason
    # fog_esm_frag confines ESM to the medium. For the moment path this arm is
    # not a regression check but the measurement the technique lives or dies on.
    for label, term in (("pcf", term40), ("msm", mterm40)):
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
                        worst_hole = max(worst_hole, term(p))
                        measured += 1
            if measured < 30:
                # No silent caps: enough of the umbra must actually be measured.
                print(f"  hole-{label:<7}ERROR only {measured} umbra samples visible "
                      f"({skipped} occluded)")
                failures.append(f"dir-hole-{label}")
            else:
                ok = worst_hole <= DIR_HOLE_MAX
                print(f"  hole-{label:<7}{'PASS' if ok else 'FAIL'}  worst umbra term "
                      f"{worst_hole:.4f} over {measured} samples, {skipped} caster-occluded "
                      f"(want <= {DIR_HOLE_MAX})")
                if not ok:
                    failures.append(f"dir-hole-{label}")
        except ValueError as e:
            print(f"  hole-{label:<7}ERROR {e}")
            failures.append(f"dir-hole-{label}")

    # --- pillar band: the thin caster, where reconstructions differ ---------
    # Hoisted: the band is fixture geometry and carries no shadow technique, so
    # rebuilding it per label would report one geometry fault once per label.
    pillar_pts = [(DIR_PILLAR_X[0] + (DIR_PILLAR_X[1] - DIR_PILLAR_X[0]) * ix / 2.0, 0.0,
                   DIR_PILLAR_Z[0] + (DIR_PILLAR_Z[1] - DIR_PILLAR_Z[0]) * iz / 12.0)
                  for ix in range(3) for iz in range(13)]
    for label, term in (("pcf", term40), ("msm", mterm40)):
        try:
            worst = max(term(p) for p in pillar_pts)
            ok = worst <= DIR_PILLAR_MAX
            print(f"  pillar-{label:<5} {'PASS' if ok else 'FAIL'}  worst band term {worst:.4f} "
                  f"over {len(pillar_pts)} samples (want <= {DIR_PILLAR_MAX})")
            if not ok:
                failures.append(f"dir-pillar-{label}")
        except ValueError as e:
            print(f"  pillar-{label:<5} ERROR {e}")
            failures.append(f"dir-pillar-{label}")

    try:
        leak = max(mterm_blur(p) for p in pillar_pts)
        ok = leak >= DIR_PILLAR_BLUR_MIN
        print(f"  pillar-blur  {'PASS' if ok else 'FAIL'}  blurred moments leak {leak:.4f} "
              f"(want >= {DIR_PILLAR_BLUR_MIN}; under it the moment path never ran)")
        if not ok:
            failures.append("dir-pillar-blur")
    except ValueError as e:
        print(f"  pillar-blur  ERROR {e}")
        failures.append("dir-pillar-blur")

    # --- acne: bare ground must divide to 1, at both elevations -------------
    for label, term in (("40deg", term40), ("10deg", term10),
                        ("40msm", mterm40), ("10msm", mterm10)):
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
    # A symmetric filter cannot move the 50% crossing of a step, so this is what
    # says the moment blur SOFTENS the edge rather than displacing it -- the
    # failure mode that kept specs/10.4 Phase 4 from shipping.
    for label, term in (("cc3", term40), ("cc1", term40_cc1),
                        ("cc3msm", mterm40), ("cc1msm", mterm40_cc1)):
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
    floor = _taa_churn(workdir, fixture, "dir_ns", hard + ["--no-shadows"])
    if floor is None:
        print("  churn        ERROR while rendering the no-shadow floor")
        failures.append("dir-churn")
    else:
        for label, extra in (("pcf", hard), ("msm", msm)):
            churn = _taa_churn(workdir, fixture, f"dir_{label}", extra)
            if churn is None:
                print(f"  churn-{label:<6}ERROR while rendering the TAA sequence")
                failures.append(f"dir-churn-{label}")
                continue
            ok = churn <= floor * DIR_CHURN_FACTOR + DIR_CHURN_SLACK_PX
            print(f"  churn-{label:<6}{'PASS' if ok else 'FAIL'}  {churn} px frame-to-frame "
                  f"(no-shadow floor {floor} px)")
            if not ok:
                failures.append(f"dir-churn-{label}")

    return failures


# Output dither (spec 11.24 / E1), measured on assets/aerial_fixture.
#
# The sky is a shallow gradient, which is exactly where the 8-bit write leaves
# contour bands: long runs of one value separated by 1-LSB steps. Dither breaks
# the runs without moving the local mean, so the longest run down a column is
# the statistic that discriminates -- and it needs no stored reference.
#
# Bounds are FRACTIONS OF FRAME HEIGHT, not pixel counts. A run length scales
# with the frame, and `render()` asks for a 400x300 window whose framebuffer is
# 800x600 on a HiDPI display and 400x300 everywhere else -- so absolute counts
# calibrated on one machine sit at zero margin on the other. Measured both ways:
# 115/20/4 px at 800x600 and 60/14/4 at 400x300, i.e. ~19%/3.3%/0.7% of height
# either way. The fractions below sit about halfway between those.
DITHER_MAX_RUN_FRAC = 0.08   # dithered: the band must collapse past this
DITHER_MIN_BAND_FRAC = 0.10  # undithered: the fixture must still BAND
DITHER_MIN_SPAN = 24         # 8-bit levels the sampled columns must cover
DITHER_MEAN_TOL = 0.05       # LSB the frame mean may move when dither turns on


# Where the shadow-offset arm places the fixture. Two requirements, and both are
# lower bounds rather than taste. It has to exceed the scene-fit map's own reach
# -- the fixture's radius is about 6 units and the viewer sets ortho_size to
# twice that -- or a map anchored at the origin still covers the scene and the
# arm measures nothing. And it has to be an exact power of two, so that
# translating the fixture is not itself a precision experiment: every authored
# coordinate here lands on a representable value at 16384, which keeps this a
# test of what the map COVERS rather than of what fp32 rounds.
ORIGIN_SHADOW_OFFSET = 16384.0
# A shadowed sample must stay this dark once the map follows the scene. Sized
# off the same umbra the dir-shadow hole arm reads at DIR_ERODE, with room for
# the map resolving a fraction of a texel differently out at the offset.
ORIGIN_UMBRA_MAX = 0.05
# ... and an unshadowed one this bright. A map that misses the scene entirely
# leaves the shadow term at exactly 1.0, so this bar is met by a wide margin or
# not at all; there is no near miss to calibrate against.
ORIGIN_LIT_MIN = 0.90


# Where the shift arms place the world. NEAR is chosen so that fp32 costs almost
# nothing there -- which is the whole point of it: a correct shift must be nearly
# free at a distance where there is nothing to recover, while anything that failed
# to follow the delta costs the same at every distance, because a wrong address is
# not a small error. Both are multiples of the app's 256-unit shift lattice, so
# the new origin lands exactly on the camera rather than near it.
ORIGIN_NEAR = 256.0
ORIGIN_FAR = 262144.0
# A shift at ORIGIN_NEAR may move this much of the frame. Measured 0.22%; a splat
# that stayed behind reads 76%, and a camera re-pinned to an absolute reads the
# same, so the gap between pass and fail here is two orders of magnitude.
ORIGIN_SHIFT_MAX = 0.02
# ... and at ORIGIN_FAR it must move a LOT, because there the shift is recovering
# real precision. Measured 39.9%. Without this the arm above is also satisfied by
# a build where --world-offset does nothing at all.
ORIGIN_DEGRADE_MIN = 0.20
# The velocity arm. The frame after a shift is compared against the steady state
# two frames later, which is the same pair of runs still differing by precision
# alone -- so the ratio asks whether the TRANSITION cost anything beyond what the
# frames on either side of it already cost. Measured 1.08; leaving prev_view_proj
# uncorrected reads 4.48, and there is nothing in between.
ORIGIN_VELOCITY_MAX_RATIO = 2.0
# World units the character's height above the terrain may drift across a shift,
# and the tolerance on the shift moving it by exactly the delta. Measured 0.000 on
# both: the capsule and the ground move together, so this is an equality, and the
# margin is for a controller that resolves a contact differently on the frame the
# broadphase is rebuilt.
ORIGIN_CLEARANCE_MAX = 0.05
# Where the automatic arm places the world, and the drift it allows. The world sits
# outside the threshold from frame 0, so a correct engine shifts immediately and
# then never again -- which is what makes "exactly once" a real assertion rather
# than a restatement of the flag. The threshold is a power of two so the lattice
# derived from it is one too.
ORIGIN_AUTO = 4096.0
ORIGIN_AUTO_THRESHOLD = 512.0


def _origin_offset_fixture(workdir, offset):
    """A translated twin of dir_shadow_fixture, and the scene file that frames it.

    The glTF's four nodes carry no `translation` -- gen_dir_shadow_fixture.py bakes
    positions into the vertex buffer -- so adding one moves the whole scene without
    touching the base64 buffer. That is the only way to translate GEOMETRY from a
    gate: a .cscn's models[] carries a path and nothing else, so cscn_copy can move
    the camera and the lights but never the meshes (spec 11.35 dropped an arm over
    exactly this).

    The camera moves with it, so the two frames are the same PICTURE from the same
    relative pose. That is what lets the sample points below stay in unshifted
    coordinates: the projector subtracts eye from point, and both took the same
    offset.
    """
    src = os.path.join(ROOT, "assets", "dir_shadow_fixture.gltf")
    with open(src) as fh:
        doc = json.load(fh)
    for node in doc["nodes"]:
        node["translation"] = [offset, 0.0, offset]
    gltf = os.path.join(workdir, "origin_dir_shadow.gltf")
    with open(gltf, "w") as fh:
        json.dump(doc, fh)

    with open(os.path.join(ROOT, "assets", "dir_shadow_fixture.cscn")) as fh:
        scene = json.load(fh)
    scene["models"] = [{"path": gltf}]
    for key in ("eye", "target"):
        v = scene["camera"][key]
        scene["camera"][key] = [v[0] + offset, v[1], v[2] + offset]
    cscn = os.path.join(workdir, "origin_dir_shadow.cscn")
    with open(cscn, "w") as fh:
        json.dump(scene, fh)
    return cscn


def _origin_umbra_samples():
    """Ground points inside both umbrae, in UNSHIFTED fixture coordinates."""
    pts = []
    for centre in (DIR_SHADOW["float_c"], DIR_SHADOW["rest_c"]):
        cx, cz, sx, sz = _dir_ellipse(centre, DIR_SHADOW["elev_deg"])
        for iu in range(-2, 3):
            for iv in range(-2, 3):
                u, v = iu / 2.0, iv / 2.0
                if u * u + v * v > 1.0:
                    continue
                p = (cx + DIR_ERODE * sx * u, 0.0, cz + DIR_ERODE * sz * v)
                if _dir_visible(p):
                    pts.append(p)
    return pts


def _origin_forest(workdir, tag, offset, extra, frames="40", mode="6", size=("800", "450")):
    """One forest run with the world placed `offset` units out, and its log.

    The camera moves with the world, so every arm here compares two frames of the
    same view and the only variable is how large the coordinates are. Framing is
    given explicitly rather than left to the follow camera because a follow camera
    reads the player's position, and the player is a physics body -- which is a
    different subsystem's correctness, not this one's.

    --render-mode 6 for the reason AGENTS.md gives: forest's Hillaire sky moves
    tens of thousands of pixels run to run, and the albedo view is the only mode
    with a 0 px floor. Every arm below measures its own floor anyway.

    The trail (spec 11.68) is left ON here, and only `origin-auto` passes
    --no-trail. That arm compares two runs that shifted by DIFFERENT amounts and
    demands they agree at exactly 0 px; a road makes that unreachable, because
    the dominant-layer index rides the atlas alpha and is read with an unfiltered
    texelFetch, so a road's hard discontinuity in that field flips one texel
    under an ulp of reconstruction difference and switches the whole detail tap.
    Measured 106 px of 1.44M (0.007%), and 61 px between two MANUAL shifts at
    different frames -- which is what says it is the shift AMOUNT, not the
    trigger, and not the auto path.

    Every other arm here keeps the trail deliberately. Their bars are 2% of
    frame, a 20% floor, and a ratio -- none of which 0.007% can reach -- and
    `origin-shift` is the ONE place in the suite that renders a road under an
    origin shift at all. A road that failed to follow authoredPos would land in
    the wrong place, which is a full-size difference rather than a sub-pixel one,
    and that arm is what would catch it.
    """
    path = os.path.join(workdir, f"origin_{tag}.ppm")
    cmd = [FOREST, "-x", "-f", frames, "-W", size[0], "-H", size[1], "--no-fog",
           "--render-mode", mode, "--seed", "1337", "--world-offset", repr(offset),
           "--cam-eye", f"{offset},40,{offset + 120}",
           "--cam-target", f"{offset},10,{offset}", "-S", path] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(path):
        return None, ""
    return path, r.stdout + r.stderr


def _origin_diff(a, b):
    """Differing pixels as a FRACTION of the frame, and the worst channel step.

    Both numbers come from compare(); this only supplies the denominator, which
    the arms want because a count means nothing without the resolution it was
    taken at -- and these frames render at twice the requested size on a HiDPI
    display, which is the arithmetic that made an early draft here report
    fractions above 1.0 and misread AE as counting channels.
    """
    w, h, _ = _read_ppm(a)
    ae, pae = compare(a, b)
    return ae / float(w * h), int(round(pae * 255.0))


_ORIGIN_TRACE = re.compile(
    r"player t=\s*([\d.]+) pos\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+).*?"
    r"grounded (\d)\s+ground_n\.y\s+[\d.]+\s+terrain\s+(-?[\d.]+)")


def _origin_trace(offset, extra):
    """--trace-player rows for a forest run with the world placed `offset` out.

    The follow camera is deliberately NOT pinned here: this arm is about where the
    character is, and pinning the camera would leave the one subsystem under test
    unobserved.
    """
    # 70 frames, not 120: --trace-player prints every 30 steps, the shift lands at
    # 40, and the arm reads the last row before it (step 30) and the first after
    # (step 60). The rest was 50 frames of physics nobody looked at.
    cmd = [FOREST, "-x", "-f", "70", "-W", "200", "-H", "150", "--no-fog",
           "--render-mode", "6", "--seed", "1337", "--world-offset", repr(offset),
           "--trace-player"] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None
    return [{"t": float(m.group(1)), "x": float(m.group(2)), "y": float(m.group(3)),
             "z": float(m.group(4)), "grounded": int(m.group(5)), "terrain": float(m.group(6))}
            for m in _ORIGIN_TRACE.finditer(r.stdout + r.stderr)]


def run_origin_gate(workdir):
    """A world away from the origin, and a world that moves under one.

      shadow-offset   the scene-fit shadow map covers the scene it belongs to rather
                      than the origin. Renders the cascade fixture where it was
                      authored, then translated by 16384 with --shadow-center
                      following, then translated with the centre pinned back at the
                      origin. The first two must shadow; the third must not.
      origin-shift    a mid-run shift at a NEAR offset changes almost nothing. It is
                      only where coordinates are measured from, so anything that
                      failed to follow it shows here -- and shows at full size,
                      since a wrong address is not a small error.
      origin-degrade  the same comparison at a FAR offset changes a lot, because
                      there the shift is recovering precision that was really lost.
                      Without it the arm above passes on a build where placing the
                      world does nothing at all.
      origin-velocity the frame after a shift reports no more motion than the frames
                      either side of it. The velocity buffer is a DIFFERENCE of two
                      transforms, so a previous-frame twin that missed the delta
                      prints the whole translation as one frame of screen-wide
                      motion -- which no other arm here can see.
      origin-physics  the character is standing on the same ground after a shift as
                      before it. Jolt is single precision and holds its own world
                      positions, so it does not follow the scene and has to be told;
                      read as height above terrain, which is the one quantity that
                      must NOT change while both of its terms do.
      origin-auto     the engine re-centres on its own once the camera has drifted,
                      exactly ONCE, and reaches the frame the hand-driven shift
                      reaches. The count is the assertion that matters: a threshold
                      compared against a camera that has not been told the world
                      moved reads its own shift as more drift and oscillates, which
                      a picture cannot show and a pass/fail on "did it shift" cannot
                      either.

    WHAT THIS GROUP CANNOT SEE, since every arm renders apps/forest and a green run
    should not be read as a claim it does not make. Forest has no reflection probe,
    no GI volume, no water and no fog, so those four shift functions are exercised
    by nothing here. Nor is the world-space TILING of a layered surface: it is wired
    (layers.glsl reads authoredPos), but at forest's seed and framing the two layers
    whose periods do not divide the 256-unit shift lattice, rock at 6 and gravel at
    3, cover too little of the frame to register -- reverting the fix moves 13 px,
    while grass and silt tile at 4, which any multiple of the lattice preserves.
    Nor is the transform-feedback particle backend, since no app that uses it shifts.

    On origin-shift's residual, because it looks like a tolerated defect and is not:
    it is PRECISION. It grows with DISTANCE rather than with the lattice, 0.22% at
    256 against 0.40% at 768, which is the curve spec 11.62 phase 2 measured for a
    materialised offset -- and reverting the tiling fix does not move it.

    Read at --shadow-cascades 1, and that is a requirement rather than a default.
    The inner cascades are fitted to the camera frustum, so they follow an offset
    scene perfectly well on their own -- only the outermost fallback map is anchored
    at scene_center. At the viewer's default of 3 the shadow this arm samples is
    resolved by an inner cascade and survives the offset whatever scene_center says,
    which reads green against a build where the fix does not exist.

    The pinned arm is the falsification and it is in-frame rather than a mutation:
    it renders the same geometry through the same binary and differs only in where
    the map is centred, so "the map followed the scene" is measured against "it did
    not" instead of against a remembered number.
    """
    failures = []
    if not os.path.exists(os.path.join(ROOT, "assets", "dir_shadow_fixture.gltf")):
        print("  shadow-offset SKIP  (missing dir_shadow_fixture.gltf)")
        return failures

    off = ORIGIN_SHADOW_OFFSET
    far = _origin_offset_fixture(workdir, off)
    # --no-pcss for the dir-shadow gate's reason: the default emitter size smears
    # the analytic edge, and every reading here is positional.
    cc1 = ["--no-pcss", "--shadow-cascades", "1"]
    centred = cc1 + ["--shadow-center", f"{off},0,{off}"]
    pinned = cc1 + ["--shadow-center", "0,0,0"]

    frames = {
        "home": _dir_render(workdir, "dir_shadow_fixture.cscn", "org_home", cc1),
        "home_ns": _dir_render(workdir, "dir_shadow_fixture.cscn", "org_home_ns",
                               cc1 + ["--no-shadows"]),
        "far": _dir_render(workdir, far, "org_far", centred),
        "far_ns": _dir_render(workdir, far, "org_far_ns", centred + ["--no-shadows"]),
        "pin": _dir_render(workdir, far, "org_pin", pinned),
    }
    if not all(frames.values()):
        print("  shadow-offset ERROR while rendering the fixture")
        return failures + ["shadow-offset"]

    pts = _origin_umbra_samples()
    try:
        home = _term_reader(frames["home"], frames["home_ns"])
        far_t = _term_reader(frames["far"], frames["far_ns"])
        # The pinned frame shares the offset scene's unshadowed reference: the two
        # differ only in the shadow map, so dividing by the same denominator is
        # what makes the pair a measurement of the map alone.
        pin_t = _term_reader(frames["pin"], frames["far_ns"])
        home_worst = max(home(p) for p in pts)
        far_worst = max(far_t(p) for p in pts)
        pin_worst = min(pin_t(p) for p in pts)
    except ValueError as exc:
        print(f"  shadow-offset ERROR {exc}")
        return failures + ["shadow-offset"]

    ok = (home_worst <= ORIGIN_UMBRA_MAX and far_worst <= ORIGIN_UMBRA_MAX
          and pin_worst >= ORIGIN_LIT_MIN)
    print(f"  shadow-offset {'PASS' if ok else 'FAIL'}  umbra at origin {home_worst:.4f}, "
          f"at {off:.0f} {far_worst:.4f} (want <= {ORIGIN_UMBRA_MAX}); "
          f"centre pinned back {pin_worst:.4f} (want >= {ORIGIN_LIT_MIN})")
    if not ok:
        failures.append("shadow-offset")

    # --- the shift itself: near, far, and the frame it lands on ---------------
    if not os.path.exists(FOREST):
        print("  origin-shift  SKIP  (forest not built)")
        return failures

    shift20 = ["--origin-shift-at", "20"]
    moved = {}
    for label, dist in (("near", ORIGIN_NEAR), ("far", ORIGIN_FAR)):
        plain, _ = _origin_forest(workdir, f"{label}_plain", dist, [])
        shifted, log = _origin_forest(workdir, f"{label}_shift", dist, shift20)
        if not (plain and shifted):
            print(f"  origin-shift  ERROR while rendering forest at {dist:.0f}")
            return failures + ["origin-shift"]
        # An arm asserting a shift changed nothing is satisfied perfectly by a
        # shift that never happened, so the log line is read, not assumed.
        fired = sum(1 for ln in log.splitlines() if "origin shift:" in ln)
        moved[label] = (_origin_diff(plain, shifted)[0], fired)

    near_frac, near_fired = moved["near"]
    far_frac, far_fired = moved["far"]

    ok = near_fired == 1 and near_frac <= ORIGIN_SHIFT_MAX
    print(f"  origin-shift  {'PASS' if ok else 'FAIL'}  at {ORIGIN_NEAR:.0f} a shift moves "
          f"{100.0 * near_frac:.2f}% of the frame (want <= {100.0 * ORIGIN_SHIFT_MAX:.0f}%; a splat "
          f"or a camera left behind reads 76%), and fired {near_fired} time (want exactly 1)")
    if not ok:
        failures.append("origin-shift")

    ok = far_fired == 1 and far_frac >= ORIGIN_DEGRADE_MIN
    print(f"  origin-degrade {'PASS' if ok else 'FAIL'}  at {ORIGIN_FAR:.0f} the same shift moves "
          f"{100.0 * far_frac:.2f}% (want >= {100.0 * ORIGIN_DEGRADE_MIN:.0f}%: that is the "
          f"precision it recovers, and without it the arm above passes on a no-op)")
    if not ok:
        failures.append("origin-degrade")

    # --- Rule 1: the shift frame must not print the translation as motion -----
    # Render mode 9 is the velocity buffer, where a previous-frame transform that
    # missed the delta is the only thing that can spike. ONE pair of runs with
    # --screenshot-every, not one run per frame: forest bakes a terrain and
    # scatters 5,000 props at startup, so a per-frame run measures the startup.
    #
    # Three frames rather than two, which the single pair also buys. 19 is BEFORE
    # the shift, so the two runs are the same run and must agree exactly -- a
    # control that says they differ in nothing but the shift.
    plain, _ = _origin_forest(workdir, "vel_plain", ORIGIN_NEAR, ["--screenshot-every", "1"],
                              "24", "9", ("400", "300"))
    shifted, _ = _origin_forest(workdir, "vel_shift", ORIGIN_NEAR,
                                shift20 + ["--screenshot-every", "1"], "24", "9", ("400", "300"))
    if not (plain and shifted):
        print("  origin-velocity ERROR while rendering the velocity buffer")
        return failures + ["origin-velocity"]

    vel = {}
    for f in (19, 21, 23):
        pa = plain[:-4] + f"_{f:06d}.ppm"
        pb = shifted[:-4] + f"_{f:06d}.ppm"
        if not (os.path.exists(pa) and os.path.exists(pb)):
            print(f"  origin-velocity ERROR frame {f} was not captured")
            return failures + ["origin-velocity"]
        vel[f] = _origin_diff(pa, pb)[0]

    # 21 is the first frame after the shift; 23 is the same pair two frames later,
    # still differing by precision alone. The ratio isolates the TRANSITION from
    # the steady state either side of it.
    steady = max(vel[23], 1e-6)
    ratio = vel[21] / steady
    ok = ratio <= ORIGIN_VELOCITY_MAX_RATIO and vel[19] == 0.0
    print(f"  origin-velocity {'PASS' if ok else 'FAIL'}  before the shift the two runs agree at "
          f"{100.0 * vel[19]:.2f}% (want exactly 0); the shift frame moves {100.0 * vel[21]:.2f}% "
          f"of the velocity buffer against a steady {100.0 * steady:.2f}% two frames later, ratio "
          f"{ratio:.2f} (want <= {ORIGIN_VELOCITY_MAX_RATIO}; leaving prev_view_proj uncorrected "
          f"reads 4.48)")
    if not ok:
        failures.append("origin-velocity")

    # --- Phase 4: the character crosses the shift still standing ---------------
    # Read as HEIGHT ABOVE TERRAIN. Both terms move -- the capsule by the delta,
    # the ground because the terrain's centre moved with it -- so their difference
    # is the only thing here that is supposed to be invariant, and a body left
    # behind breaks it while leaving both terms individually plausible.
    rows = _origin_trace(ORIGIN_NEAR, ["--origin-shift-at", "40"])
    if rows is None:
        print("  origin-physics ERROR while tracing the player")
        return failures + ["origin-physics"]
    before = [r for r in rows if r["t"] < 0.7]
    after = [r for r in rows if r["t"] > 0.9]
    if not before or not after:
        print(f"  origin-physics ERROR trace has {len(before)} samples before the shift and "
              f"{len(after)} after (want at least one of each)")
        return failures + ["origin-physics"]

    b, a = before[-1], after[-1]
    clearance_drift = abs((a["y"] - a["terrain"]) - (b["y"] - b["terrain"]))
    x_moved = abs((b["x"] - a["x"]) - ORIGIN_NEAR)
    ok = (clearance_drift <= ORIGIN_CLEARANCE_MAX and x_moved <= ORIGIN_CLEARANCE_MAX
          and a["grounded"] and b["grounded"])
    print(f"  origin-physics {'PASS' if ok else 'FAIL'}  height above terrain {b['y'] - b['terrain']:.3f} "
          f"before the shift and {a['y'] - a['terrain']:.3f} after (drift {clearance_drift:.3f}, want "
          f"<= {ORIGIN_CLEARANCE_MAX}); x moved {b['x'] - a['x']:.1f} of {ORIGIN_NEAR:.0f}; grounded "
          f"both sides {bool(b['grounded'])}/{bool(a['grounded'])}")
    if not ok:
        failures.append("origin-physics")

    # --- Phase 5: the engine re-centres on its own ----------------------------
    # At ORIGIN_AUTO the camera starts outside the threshold, so a correct engine
    # shifts once on the first frame and never again. Compared against the
    # hand-driven shift rather than against a remembered number: both take the
    # world to the same origin, so if the automatic trigger is the same mechanism
    # the two frames are the SAME frame.
    # --no-trail on BOTH legs, and only here: this is the suite's only exact-zero
    # comparison between two runs that shifted by different amounts, which is the
    # one bar a road can break. See _origin_forest's docstring for the mechanism
    # and for why every other origin arm keeps the trail.
    manual, _ = _origin_forest(workdir, "auto_manual", ORIGIN_AUTO,
                               ["--origin-shift-at", "20", "--no-trail"])
    auto, autolog = _origin_forest(workdir, "auto_auto", ORIGIN_AUTO,
                                   ["--origin-shift-distance", repr(ORIGIN_AUTO_THRESHOLD),
                                    "--no-trail"])
    if not (manual and auto):
        print("  origin-auto   ERROR while rendering the automatic shift")
        return failures + ["origin-auto"]
    shifts = sum(1 for ln in autolog.splitlines() if "origin shift:" in ln)
    frac, worst = _origin_diff(manual, auto)
    ok = shifts == 1 and frac == 0.0
    print(f"  origin-auto   {'PASS' if ok else 'FAIL'}  fired {shifts} time(s) over 40 frames "
          f"(want exactly 1; an oscillating threshold fires ~30), and reaches the hand-driven "
          f"shift's frame at {100.0 * frac:.2f}% differing, worst {worst} (want exactly 0)")
    if not ok:
        failures.append("origin-auto")

    return failures


def _flat_run_and_span(pix, w, h):
    """Longest single-value run down a column, and the value range covered.

    The run is the debanding statistic: a quantization contour is a long run of
    one value, and dither breaks it without moving the local mean. Clipped 0/255
    runs are excluded -- the dither is faded to nothing at the clip points, so a
    clamped region neither bands nor dithers and would only measure letterbox.

    The span is what stops the run being read on its own. A CONSTANT image
    MAXIMIZES run length, so a floor on the run alone is satisfied at its best
    possible score by a fixture that lost its gradient entirely.
    """
    worst, lo, hi = 0, 255, 0
    for x in range(20, w - 20, max(1, (w - 40) // 24)):
        for ch in range(3):
            col = pix[x * 3 + ch:w * h * 3:w * 3]
            lo, hi = min(lo, min(col)), max(hi, max(col))
            for v, run in groupby(col):
                if v not in (0, 255):
                    worst = max(worst, sum(1 for _ in run))
    return worst, hi - lo


# ---- 3D LUT colour grading (spec 11.58) ------------------------------------
#
# Every arm renders with --no-dither and --no-vignette. Not tidiness: the reads
# are single patch centres and per-channel spreads of a few 8-bit codes, which
# is exactly the size of one dither LSB -- the eight existing --no-dither sites
# are here for the same reason. The vignette is off so the outer patches are not
# darkened toward each other, which would compress the range the table is
# sampled over.
LUT_FIXTURE = "lut_fixture.gltf"
LUT_SCENE = "lut_fixture.cscn"
# Tolerance for "the shader agrees with an independent interpolator", in 8-bit
# codes. Not zero: the gate's reference reads the frame AFTER it was quantized
# to 8 bits, so a value the shader computed at .49 of a code and rounded down is
# re-graded here from the rounded number. One code is that requantization and
# nothing else -- a wrong axis order, a missing inset or a wrong interpolant all
# move whole tens of codes.
LUT_AGREE_CODES = 1.5
# What the neutral probe must show. Trilinear measured 5 codes of tint on the
# rendered chart and tetrahedral exactly 0, so the floor sits below the first
# and the ceiling AT the second -- tetrahedral's exactness is the claim, and a
# tolerance here would let a half-broken decomposition through.
LUT_TINT_MIN = 2
LUT_TINT_MAX = 0
# The coarse probe's measured tri-vs-tet separation is 7.841 codes; half of it,
# so the arm discriminates a PARTLY wrong decomposition rather than only a fully
# degenerate one.
LUT_PROBE_MIN_CODES = 4


def _lut_render(workdir, name, extra, scene=None):
    out = os.path.join(workdir, name + ".ppm")
    src = scene or os.path.join(ROOT, "assets", LUT_SCENE)
    err = render(src, out, ["--no-auto-exposure", "-E", "1.0", "--no-dither", "--no-vignette"]
                 + extra)
    return (None, err) if err else (out, None)


def _lut_patch_points():
    """Patch centres, read out of the fixture's own glTF node translations.

    Read rather than restated for _cscn_camera's reason: a transcribed copy
    agrees with the asset only until someone edits the generator, and the
    failure is silent -- the gate keeps passing while sampling the backdrop.

    Deliberately NOT by importing gen_lut_fixture.py, which would make the
    ground truth a function of the thing under test and, since that generator
    writes at module scope, would rewrite the committed assets mid-gate.
    """
    with open(os.path.join(ROOT, "assets", LUT_FIXTURE)) as f:
        nodes = json.load(f)["nodes"]
    return [(n["name"], tuple(n["translation"])) for n in nodes
            if n.get("name", "").startswith("patch_")]


def _lut_read_patches(path):
    """(name, (r, g, b)) per patch, sampled at its projected centre."""
    w, h, pix = _read_ppm(path)
    proj = _projector(_cscn_camera(LUT_SCENE), w, h)
    out = []
    for name, centre in _lut_patch_points():
        x, y = proj(centre)
        o = (int(y) * w + int(x)) * 3
        out.append((name, (pix[o], pix[o + 1], pix[o + 2])))
    return out


def _lut_read_cube(path):
    """(size, [(r, g, b), ...]) with the file's own red-fastest ordering kept."""
    size, data = None, []
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            if s.upper().startswith("LUT_3D_SIZE"):
                size = int(s.split()[1])
                continue
            if s.upper().startswith(("TITLE", "DOMAIN_", "LUT_1D_SIZE")):
                continue
            parts = s.split()
            if len(parts) == 3:
                data.append(tuple(float(v) for v in parts))
    return size, data


def _lut_tetrahedral(size, data, c):
    """The gate's own tetrahedral interpolator.

    An INDEPENDENT implementation on purpose. The whole point of lut-agree is
    that a second reading of the same .cube reaches the same pixel, so sharing
    code with the shader -- or with the generator's closed forms -- would make
    the arm agree with itself.
    """
    def fetch(ir, ig, ib):
        d = size - 1
        ir, ig, ib = min(ir, d), min(ig, d), min(ib, d)
        return data[ib * size * size + ig * size + ir]

    d = size - 1
    p = [max(0.0, min(1.0, v)) * d for v in c]
    i = [min(int(v), d - 1) for v in p]
    fr, fg, fb = (p[k] - i[k] for k in range(3))
    c000, c111 = fetch(*i), fetch(i[0] + 1, i[1] + 1, i[2] + 1)
    if fr > fg:
        if fg > fb:
            a, b, w = fetch(i[0] + 1, i[1], i[2]), fetch(i[0] + 1, i[1] + 1, i[2]), \
                (fr - fg, fg - fb, fb)
        elif fr > fb:
            a, b, w = fetch(i[0] + 1, i[1], i[2]), fetch(i[0] + 1, i[1], i[2] + 1), \
                (fr - fb, fb - fg, fg)
        else:
            a, b, w = fetch(i[0], i[1], i[2] + 1), fetch(i[0] + 1, i[1], i[2] + 1), \
                (fb - fr, fr - fg, fg)
    else:
        if fb > fg:
            a, b, w = fetch(i[0], i[1], i[2] + 1), fetch(i[0], i[1] + 1, i[2] + 1), \
                (fb - fg, fg - fr, fr)
        elif fb > fr:
            a, b, w = fetch(i[0], i[1] + 1, i[2]), fetch(i[0], i[1] + 1, i[2] + 1), \
                (fg - fb, fb - fr, fr)
        else:
            a, b, w = fetch(i[0], i[1] + 1, i[2]), fetch(i[0] + 1, i[1] + 1, i[2]), \
                (fg - fr, fr - fb, fb)
    w0 = 1.0 - sum(w)
    return tuple(w0 * c000[k] + w[0] * a[k] + w[1] * b[k] + w[2] * c111[k] for k in range(3))


def run_lut_gate(workdir):
    """3D LUT colour grading: is it the artist's table, applied where it belongs?

    A LUT is a table of answers rather than a formula, so nothing about it can
    be checked by looking -- a table read in the wrong axis order, addressed by
    texel edge instead of centre, or interpolated off the neutral axis all
    produce a graded frame that looks like a grade. Every arm here is against a
    number known before the render.

      lut-agree     the decisive arm. Read the UNGRADED patch, push it through
                    the .cube with the gate's own tetrahedral interpolator, and
                    require the graded patch to match. Independent of the
                    scene's absolute values, so it cannot be fooled by exposure
                    or a tonemap change, and independent of the shader's own
                    arithmetic. Run on the steep table, the hardest one.
      lut-swap      R and B exchanged on every patch. The one arm that can catch
                    the data block being read blue-fastest: .cube stores red
                    fastest, and the transpose is a perfectly plausible image.
                    Linear, so both interpolants agree on it exactly, which
                    isolates "is the table right" from "is the interpolation".
      lut-identity  an identity table is a no-op. BOTH interpolants, because
                    they cover different mistakes and the default cannot see
                    the one that matters here: the half-texel inset is a
                    trilinear-only concern -- tetrahedral addresses texels by
                    integer index -- so the tetrahedral leg is 0 px whether the
                    inset is right or wrong, and only the trilinear leg moves
                    (1 code with it, 8 without). CANNOT STAND ALONE either way:
                    a LUT that never loaded is also 0 px, which is 11.22's
                    lesson, so it is meaningful only beside lut-agree above.
      lut-strength  0 is bit-exact off, 1 is full, and 0.5 lands between. The
                    C-side short-circuit arm: without the middle reading, on and
                    off take one path and the check passes against a dead
                    feature (11.21 shipped exactly that arm).
      lut-off       a scene authoring post.lut renders identically to passing
                    the same file to --lut, --no-lut returns the ungraded frame
                    exactly, and the CLI beats an authored strength and interp.
                    The authored path is RELATIVE on purpose: that is the only
                    way cscn_copy's absolutizer and cscene.c's resolve_in_place
                    ever run, and both fail silently -- the table does not load
                    and the frame renders ungraded.
      lut-refuse    eight malformed tables each refused BY NAME, and two
                    odd-but-valid ones (an unknown header keyword, a UTF-8 BOM)
                    accepted. A fifth of the reader is refusal logic and every
                    fixture is well-formed, so none of it ran; a review found
                    four wrong-reason refusals and two wrong accepts in there.
                    Meaningful only beside lut-agree, which is what stops a
                    reader that refuses everything from passing.
      lut-interp    the two interpolants AGREE on the linear table and DISAGREE
                    on the coarse probe. The disagreement is the half that
                    matters: without it a tetrahedral path that silently
                    degenerated to trilinear would pass every other arm here.
      lut-neutral   through a table that is identity on the grey diagonal,
                    tetrahedral holds r == g == b EXACTLY while trilinear tints.
                    The whole justification for tetrahedral being the default,
                    and the reason trilinear is kept reachable at all.
    """
    failures = []
    scene = os.path.join(ROOT, "assets", LUT_SCENE)
    if not os.path.exists(scene):
        print("  lut          SKIP  (missing %s)" % LUT_SCENE)
        return []
    cubes = {n: os.path.join(ROOT, "assets", "lut_%s.cube" % n)
             for n in ("identity", "swap", "steep", "neutral")}
    missing = [p for p in cubes.values() if not os.path.exists(p)]
    if missing:
        print("  lut          SKIP  (missing %s)" % os.path.basename(missing[0]))
        return []

    base, err = _lut_render(workdir, "lut_base", [])
    if err:
        print("  lut          ERROR (ungraded render failed)\n" + err[-800:])
        return ["lut-render"]
    ungraded = _lut_read_patches(base)

    # lut-agree -- the shader against an independent reading of the same table
    steep, err = _lut_render(workdir, "lut_steep", ["--lut", cubes["steep"]])
    if err:
        print("  lut-agree    ERROR (steep render failed)\n" + err[-800:])
        failures.append("lut-agree")
    else:
        size, data = _lut_read_cube(cubes["steep"])
        graded = _lut_read_patches(steep)
        worst, where = 0.0, ""
        for (name, before), (_, after) in zip(ungraded, graded):
            want = _lut_tetrahedral(size, data, tuple(v / 255.0 for v in before))
            for k in range(3):
                d = abs(want[k] * 255.0 - after[k])
                if d > worst:
                    worst, where = d, name
        ok = worst <= LUT_AGREE_CODES
        print(f"  lut-agree    {'PASS' if ok else 'FAIL'}  worst {worst:.2f} codes at {where} "
              f"want <={LUT_AGREE_CODES} (an independent tetrahedral read of the same "
              f"{size}^3 table; a transposed or edge-addressed table moves tens of codes)")
        if not ok:
            failures.append("lut-agree")

    # lut-swap -- red-fastest, in the one form that cannot be mistaken
    swap, err = _lut_render(workdir, "lut_swap", ["--lut", cubes["swap"]])
    if err:
        print("  lut-swap     ERROR (swap render failed)\n" + err[-800:])
        failures.append("lut-swap")
    else:
        graded = _lut_read_patches(swap)
        worst, spread = 0.0, 0
        for (_, before), (_, after) in zip(ungraded, graded):
            want = (before[2], before[1], before[0])
            worst = max(worst, max(abs(want[k] - after[k]) for k in range(3)))
            spread = max(spread, abs(before[0] - before[2]))
        # The floor is what stops a chart of greys satisfying a swap trivially.
        ok = worst <= LUT_AGREE_CODES and spread >= 40
        print(f"  lut-swap     {'PASS' if ok else 'FAIL'}  worst {worst:.0f} codes "
              f"want <={LUT_AGREE_CODES}, and the chart separates R from B by {spread} "
              f"want >=40 (or swapping them proves nothing)")
        if not ok:
            failures.append("lut-swap")

    # lut-identity -- both legs, because they cover different mistakes
    ident, err = _lut_render(workdir, "lut_ident", ["--lut", cubes["identity"]])
    ident_tri, err2 = _lut_render(workdir, "lut_ident_tri",
                                  ["--lut", cubes["identity"], "--lut-interp", "trilinear"])
    if err or err2:
        print("  lut-identity ERROR (identity renders failed)\n" + (err or err2)[-800:])
        failures.append("lut-identity")
    else:
        ae, pae = compare(base, ident)
        ae_tri, pae_tri = compare(base, ident_tri)
        # The trilinear leg is a PEAK bound, not a pixel count, and it is the
        # only thing in the group that can see the half-texel inset: tetrahedral
        # addresses texels by integer index, so the default path is exactly 0
        # whether the inset is right or not. Dropping it takes this reading from
        # 1 code to 8.
        ok = ae == 0 and pae_tri <= LSB
        print(f"  lut-identity {'PASS' if ok else 'FAIL'}  tetrahedral {ae} px want exactly 0 "
              f"(PAE {pae:.6f}); trilinear PAE {pae_tri:.6f} want <={LSB:.6f} on {ae_tri} px "
              f"-- that leg is the half-texel inset, which reads 0.031373 without it")
        if not ok:
            failures.append("lut-identity")

    # lut-strength -- and the middle reading that makes it falsifiable
    zero, e0 = _lut_render(workdir, "lut_s0", ["--lut", cubes["steep"], "--lut-strength", "0"])
    half, e5 = _lut_render(workdir, "lut_s5", ["--lut", cubes["steep"], "--lut-strength", "0.5"])
    if e0 or e5 or not steep:
        print("  lut-strength ERROR (strength renders failed)\n" + (e0 or e5)[-800:])
        failures.append("lut-strength")
    else:
        ae0, _ = compare(base, zero)
        mid = _lut_read_patches(half)
        full = _lut_read_patches(steep)
        # 0.5 must land on the MIDPOINT, not merely inside the band. The band
        # form was written first and is satisfied by any wrong blend curve that
        # stays between the endpoints -- mix(c, graded, s*s) lands at 0.25 of
        # the way and passes it. The exact answer is free here: mix is linear in
        # the display-encoded value and both endpoints are read after the same
        # encode, so the midpoint is (a + b) / 2 to within the three roundings.
        #
        # `moved` is the floor that stops a chart with nothing to blend from
        # satisfying the midpoint trivially -- at a == b every strength agrees.
        worst_mid, moved = 0.0, 0
        for (_, a), (_, m), (_, b) in zip(ungraded, mid, full):
            for k in range(3):
                if abs(a[k] - b[k]) >= 4:
                    moved += 1
                    worst_mid = max(worst_mid, abs(m[k] - 0.5 * (a[k] + b[k])))
        ok = ae0 == 0 and worst_mid <= 1.5 and moved >= 40
        print(f"  lut-strength {'PASS' if ok else 'FAIL'}  strength 0 is {ae0} px want exactly 0; "
              f"0.5 is off the midpoint by {worst_mid:.2f} codes want <=1.5 on {moved} channels "
              f"the table actually moves, want >=40 (a squared or smoothstep blend reads ~25%)")
        if not ok:
            failures.append("lut-strength")

    # lut-off -- the two authoring paths are one path, and --no-lut is the way out
    authored = os.path.join(workdir, "lut_authored.cscn")

    # All three keys, and the path RELATIVE. Authoring only an absolute `path`
    # left two things this branch added unreachable: cscn_copy's absolutizer
    # (which only fires on a relative one) and cscene.c's resolve_in_place. Both
    # fail the IES way rather than the model way -- the LUT silently does not
    # load and the frame renders ungraded -- so an arm that never exercises them
    # is exactly the shape this group exists to avoid.
    shutil.copy(cubes["swap"], os.path.join(workdir, "lut_swap.cube"))

    def _author(d):
        d.setdefault("post", {})["lut"] = {"path": "lut_swap.cube", "strength": 1.0,
                                           "interp": "tetrahedral"}

    cscn_copy(scene, authored, _author)
    by_scene, e_scene = _lut_render(workdir, "lut_scene", [], scene=authored)
    forced_off, e_off = _lut_render(workdir, "lut_scene_off", ["--no-lut"], scene=authored)
    # The CLI must beat all three authored keys, not just the path: a scene
    # authoring trilinear at half strength, overridden on the command line,
    # has to reach the same pixels as the pure-CLI render.
    beaten = os.path.join(workdir, "lut_beaten.cscn")

    def _author_weak(d):
        d.setdefault("post", {})["lut"] = {"path": "lut_swap.cube", "strength": 0.25,
                                           "interp": "trilinear"}

    cscn_copy(scene, beaten, _author_weak)
    by_cli, e_cli = _lut_render(workdir, "lut_cli_wins",
                                ["--lut", cubes["swap"], "--lut-strength", "1",
                                 "--lut-interp", "tetrahedral"], scene=beaten)
    if e_scene or e_off or e_cli or not swap:
        print("  lut-off      ERROR (scene-authored renders failed)\n"
              + ((e_scene or e_off or e_cli) or "the swap render above failed")[-800:])
        failures.append("lut-off")
    else:
        ae_same, _ = compare(swap, by_scene)
        ae_off, _ = compare(base, forced_off)
        ae_cli, _ = compare(swap, by_cli)
        ok = ae_same == 0 and ae_off == 0 and ae_cli == 0
        print(f"  lut-off      {'PASS' if ok else 'FAIL'}  post.lut vs --lut {ae_same} px want 0 "
              f"(via a RELATIVE path, so the scene-file resolver runs); --no-lut vs no LUT at all "
              f"{ae_off} px want 0; CLI over an authored strength+interp {ae_cli} px want 0")
        if not ok:
            failures.append("lut-off")

    # lut-refuse -- the reader's whole thesis, which nothing was testing
    #
    # Roughly a fifth of lut.c is refusal logic and the commit that landed it is
    # titled "a .cube reader that refuses by name". Every fixture is well-formed,
    # so none of it ran. A review pass found four wrong-reason refusals and two
    # wrong accepts in that uncovered code; this is what stops the next four.
    #
    # Cheap by construction: a refusal happens at LOAD, so one frame at 100x100
    # is enough to reach it, and the assertion is on the named reason rather
    # than on pixels. The pair with lut-agree is what makes it meaningful -- a
    # reader that refused EVERYTHING would pass this and fail that.
    bad = [
        ("a 1D LUT", 'TITLE "1d"\nLUT_1D_SIZE 4\n0 0 0\n1 1 1\n', "is a 1D LUT"),
        ("a log-domain table", "LUT_3D_SIZE 2\nDOMAIN_MAX 4 4 4\n" + "0 0 0\n" * 8,
         "only the 0..1 domain"),
        ("no declared size", 'TITLE "nosize"\n0 0 0\n1 1 1\n', "declares no LUT_3D_SIZE"),
        ("a non-numeric size", "LUT_3D_SIZE abc\n0 0 0\n", "not a number"),
        ("a size that wraps int", "LUT_3D_SIZE 4294967298\n" + "0 0 0\n" * 8,
         "outside the supported"),
        ("a truncated block", "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n", "truncated or malformed"),
        ("a trailing value", "LUT_3D_SIZE 2\n" + "0 0 0\n" * 9, "carries more than"),
        ("a value past fp16", "LUT_3D_SIZE 2\n1e30 0 0\n" + "0 0 0\n" * 7, "out-of-range value"),
    ]
    # ...and two the reader must ACCEPT. Both were refused before the review:
    # an unknown keyword ended the header, so a real Iridas extension and a
    # Windows BOM each read as a truncated data block.
    good = [
        ("an unknown keyword", "LUT_3D_SIZE 2\nLUT_3D_INPUT_RANGE 0.0 1.0\n"
         + "".join(f"{r} {g} {b}\n" for b in (0, 1) for g in (0, 1) for r in (0, 1))),
        ("a UTF-8 BOM", "﻿LUT_3D_SIZE 2\n"
         + "".join(f"{r} {g} {b}\n" for b in (0, 1) for g in (0, 1) for r in (0, 1))),
    ]
    missed, wrongly_refused = [], []
    for label, body, want in bad:
        p = os.path.join(workdir, "bad_%d.cube" % len(missed + wrongly_refused))
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(body)
        r = subprocess.run([RENDER, "-m", scene, "-x", "-f", "1", "-W", "100", "-H", "100",
                            "--lut", p], capture_output=True, text=True)
        if want not in (r.stdout + r.stderr):
            missed.append(label)
    for label, body in good:
        p = os.path.join(workdir, "good_%d.cube" % len(wrongly_refused))
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(body)
        r = subprocess.run([RENDER, "-m", scene, "-x", "-f", "1", "-W", "100", "-H", "100",
                            "--lut", p], capture_output=True, text=True)
        if "2^3" not in (r.stdout + r.stderr):
            wrongly_refused.append(label)
    ok = not missed and not wrongly_refused
    print(f"  lut-refuse   {'PASS' if ok else 'FAIL'}  {len(bad)} malformed tables each refused "
          f"BY NAME ({len(missed)} misnamed: {', '.join(missed) or 'none'}); "
          f"{len(good)} odd-but-valid tables loaded ({len(wrongly_refused)} wrongly refused: "
          f"{', '.join(wrongly_refused) or 'none'})")
    if not ok:
        failures.append("lut-refuse")

    # lut-interp -- agree where the table is linear, differ where it is not
    tri_lin, e_tri_lin = _lut_render(workdir, "lut_tri_lin",
                                     ["--lut", cubes["swap"], "--lut-interp", "trilinear"])
    tri_n, e_tri_n = _lut_render(workdir, "lut_tri_n",
                                 ["--lut", cubes["neutral"], "--lut-interp", "trilinear"])
    tet_n, e_tet_n = _lut_render(workdir, "lut_tet_n",
                                 ["--lut", cubes["neutral"], "--lut-interp", "tetrahedral"])
    if e_tri_lin or e_tri_n or e_tet_n or not swap:
        print("  lut-interp   ERROR (interp renders failed)\n"
              + ((e_tri_lin or e_tri_n or e_tet_n) or "the swap render above failed")[-800:])
        failures.append("lut-interp")
    else:
        ae_lin, pae_lin = compare(swap, tri_lin)
        ae_probe, pae_probe = compare(tri_n, tet_n)
        # The linear half is a bound on PEAK error, not a pixel count: the two
        # interpolants reach the same value by different arithmetic, so a
        # handful of pixels rounding apart is expected and a visible difference
        # is not.
        # A MAGNITUDE on the probe, not `> 0`. The spec measured 7.841 codes
        # there, so a bar of one differing pixel catches full degeneration and
        # nothing short of it -- a partially wrong decomposition would clear it.
        # 4 codes keeps 2x headroom against the measurement.
        ok = pae_lin <= LSB and pae_probe >= LUT_PROBE_MIN_CODES / 255.0
        print(f"  lut-interp   {'PASS' if ok else 'FAIL'}  on the LINEAR table the two agree to "
              f"PAE {pae_lin:.6f} want <={LSB:.6f} ({ae_lin} px); on the coarse probe they "
              f"differ by PAE {pae_probe:.6f} want >={LUT_PROBE_MIN_CODES / 255.0:.6f} "
              f"({ae_probe} px; or tetrahedral is trilinear)")
        if not ok:
            failures.append("lut-interp")

    # lut-neutral -- the claim tetrahedral is bought for
    if e_tri_n or e_tet_n:
        print("  lut-neutral  SKIP  (the interp renders failed above)")
    else:
        def _grey_spread(path):
            worst = 0
            for name, rgb in _lut_read_patches(path):
                if "grey" in name:
                    worst = max(worst, max(rgb) - min(rgb))
            return worst

        # Measured as an INCREASE over the ungraded frame's own grey spread.
        # The absolute form reads whatever chroma the pipeline already put on a
        # grey patch -- bloom is on in this group, and a colour patch sits ~47 px
        # away -- so it would move with the bloom radius or the chart pitch for
        # reasons that have nothing to do with the interpolant. The in-frame
        # control is the same idiom contact_local_fixture's exact 1.0000 uses.
        tint_base = _grey_spread(base)
        tint_tri = _grey_spread(tri_n) - tint_base
        tint_tet = _grey_spread(tet_n) - tint_base
        ok = tint_tri >= LUT_TINT_MIN and tint_tet <= LUT_TINT_MAX
        print(f"  lut-neutral  {'PASS' if ok else 'FAIL'}  through a table that is identity on "
              f"the grey diagonal, trilinear ADDS {tint_tri} codes of grey tint want "
              f">={LUT_TINT_MIN} and tetrahedral adds {tint_tet} want <={LUT_TINT_MAX} (exact, "
              f"since all six tetrahedra share that diagonal as an edge)")
        if not ok:
            failures.append("lut-neutral")

    return failures


def run_dither_gate(workdir):
    fixture = os.path.join(ROOT, "assets", "aerial_fixture.gltf")
    if not os.path.exists(fixture):
        print("  dither       SKIP  (missing aerial_fixture.gltf)")
        return []

    def measure(tag, extra):
        out = os.path.join(workdir, f"dither_{tag}.ppm")
        if render(fixture, out, ["--no-auto-exposure", "-E", "1.0"] + extra):
            return None
        w, h, pix = _read_ppm(out)
        return _flat_run_and_span(pix, w, h) + (h,)

    off = measure("off", ["--no-dither"])
    on = measure("on", [])
    strong = measure("strong", ["--dither", "2.0"])
    if off is None or on is None or strong is None:
        print("  dither       ERROR while rendering the gradient fixture")
        return ["dither"]
    (off_run, off_span, h), (on_run, _, _), (strong_run, _, _) = off, on, strong
    min_band, max_run = DITHER_MIN_BAND_FRAC * h, DITHER_MAX_RUN_FRAC * h

    failures = []
    # The inverse arm, and it takes TWO statistics. A run-length floor alone is
    # maximized by a constant image -- exactly what a fixture that lost its sky
    # produces -- so it would pass at its best possible score while measuring
    # nothing. The span is the half that a constant image fails. (Spec 11.22:
    # six MSM arms passed silently because the frame they measured *was* the
    # feature-off frame.)
    ok = off_span >= DITHER_MIN_SPAN and off_run >= min_band
    print(f"  band-inverse {'PASS' if ok else 'FAIL'}  undithered run {off_run} px "
          f"(needs >= {min_band:.0f}) over a {off_span}-level span "
          f"(needs >= {DITHER_MIN_SPAN}: a flat frame is not a gradient)")
    if not ok:
        failures.append("dither-band-inverse")

    ok = on_run <= max_run and on_run < off_run
    print(f"  band-run     {'PASS' if ok else 'FAIL'}  {on_run} px dithered run "
          f"(bound {max_run:.0f}, undithered {off_run})")
    if not ok:
        failures.append("dither-band-run")

    # Strength must do something. "Off is off" would NOT cover this: the C side
    # short-circuits, so both arms take one path and the check passes against a
    # dead feature (spec 11.21 shipped exactly that arm). Comparing two live
    # amplitudes fails if the shader ignores the uniform.
    ok = strong_run < on_run
    print(f"  band-linear  {'PASS' if ok else 'FAIL'}  {strong_run} px at 2.0 LSB "
          f"(must beat {on_run} px at the 1.0 default)")
    if not ok:
        failures.append("dither-band-linearity")

    # Dither must not move the picture, only its quantization error. The arm
    # that matters is at the CLIP POINTS: a value already at 0 or 1 has no error
    # to decorrelate, so a dither that is added and then clamped keeps one half
    # of its distribution and shifts the mean -- flat white stipples to 254,
    # flat black lifts off zero. That is a DC bias, and none of the run-length
    # arms above can see it. Measured on a deliberately blown frame (ACES holds
    # its clip rather than rolling off, so ~63% of channels sit at 255).
    def clip_mean(tag, extra):
        out = os.path.join(workdir, f"dither_{tag}.ppm")
        if render(fixture, out, ["--no-auto-exposure", "-E", "500",
                                 "--tonemap", "aces"] + extra):
            return None
        _, _, pix = _read_ppm(out)
        return sum(pix) / len(pix)

    m_off, m_on = clip_mean("clipoff", ["--no-dither"]), clip_mean("clipon", [])
    if m_off is None or m_on is None:
        print("  band-mean    ERROR while rendering the clipped frame")
        return failures + ["dither-band-mean"]
    ok = abs(m_on - m_off) <= DITHER_MEAN_TOL
    print(f"  band-mean    {'PASS' if ok else 'FAIL'}  clipped-frame mean moves "
          f"{abs(m_on - m_off):.4f} LSB (bound {DITHER_MEAN_TOL})")
    if not ok:
        failures.append("dither-band-mean")

    return failures


# Translucent shadows (spec 11.26 / C1), on assets/translucent_shadow_fixture.
#
# The fixture's whole point is that the answer is knowable: transmittance
# through N translucent layers is prod(1 - a), which is ORDER-INDEPENDENT and
# equal to exp of the summed absorbances. So these arms constrain the ANSWER and
# not the storage -- an implementation that accumulates absorbance additively
# and one that multiplies transmittances satisfy them identically.
TSL_ALPHA = 0.35                # authored on the staircase panels
TSL_PANELS = 4
TSL_STACK_TOL = 0.02            # absolute error against 0.65^k
TSL_RATIO_TOL = 0.03            # per-layer ratio, measured without a reference
TSL_RAMP_RMS_MAX = 0.03         # ramp against 1 - alpha(x)
TSL_RAMP_DISCRIM = 3.0          # ... and how much better than a binary step
TSL_OPAQUE_MAX = 0.02           # an opaque umbra must stay an umbra
TSL_ACNE_TOL = 0.05             # bare ground must stay lit
TSL_MONO_TOL = 0.02             # the red panel is monochrome as shipped
TSL_LIVE_MIN_PX = 50000         # the inverse arm: the flag must DO something

# Geometry mirrors of the generator. Not read from the .gltf: these are the
# numbers the PREDICTION uses, and a gate that derived them from the asset would
# agree with a broken asset.
TSL_BAND_X0, TSL_BAND_W = -7.0, 2.0
TSL_CANOPY_XR = 3.0
TSL_SHIFT = 8.0 / math.tan(math.radians(75.0))   # sun elevation 75
TSL_STAIR_ZC = -5.5 + TSL_SHIFT                  # centre of the cast band
TSL_RAMP_Z0, TSL_RAMP_Z1 = -3.5, -1.5
TSL_RAMP_ZC = 0.5 * (TSL_RAMP_Z0 + TSL_RAMP_Z1) + TSL_SHIFT
TSL_RED_ZC = 0.0 + TSL_SHIFT
# Lit reference taken just clear of the canopy, NOT at the frame edge: the
# default vignette is radial, so a reference out at x=-10 sits darker than the
# bands it normalises and pulls every transmittance up. --no-vignette below
# removes it; sampling near the bands keeps the arm honest if it ever returns.
TSL_LIT_X = 4.0
# A point in the opaque block's cast umbra that the camera can actually SEE.
# The block is 1.6 tall at elevation 75, so it shifts its own shadow only 0.43
# in z and stands in front of the rest: the umbra runs z -3.57..-1.57 but the
# block's own footprint hides it back to -2.0, leaving a 0.43-deep sliver.
# Centred in that sliver, and in the block's x. The first version sampled
# (6, 0, -2.57) -- inside the block, and off an 800-wide frame at px 809.
TSL_OPAQUE_SAMPLE = (6.0, 0.0, -1.786)


def _tsl_render(workdir, tag, extra):
    out = os.path.join(workdir, f"tsl_{tag}.ppm")
    scene = os.path.join(ROOT, "assets", "translucent_shadow_fixture.cscn")
    # --no-ssao is load-bearing, not tidiness: the arms compare shadowed ground
    # against lit ground, and GTAO darkens near the panels by an amount the open
    # reference does not get. Measured: it biased every band ~2.8%. The rest
    # keeps anything that could add to flat lit ground out of frame.
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "-W", "800", "-H", "500",
           "--no-auto-exposure", "-E", "1.0", "--no-pcss", "--no-ssao", "--no-ssr",
           "--no-bloom", "--no-dither", "--no-vignette", "-S", out] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    return out


def _tsl_reader(path):
    """World point -> linear radiance, through the CORRECT tonemap inversion.

    NOT _term_reader: that uses _linear_luma, which inverts the sRGB curve but
    not Khronos Neutral, and reads ~23% low. Its own arms are immune (they sit
    at ~0, ~1 and 50% crossings); an assertion of the form T = (1-a)^k is not.
    """
    w, h, pix = _read_ppm(path)
    cam = _cscn_camera("translucent_shadow_fixture.cscn")
    project = _projector(cam, w, h)

    def at(p):
        px, py = project(p)
        x, y = int(round(px)), int(round(py))
        # Raising rather than clamping, because clamping is how a sample walks
        # off the frame and keeps reporting: the first opaque-umbra point
        # projected to px 809 of 800 and silently read the frame's right edge.
        if not (0 <= x < w and 0 <= y < h):
            raise AssertionError(f"sample {p} projects to ({px:.1f}, {py:.1f}), outside {w}x{h}")
        o = (y * w + x) * 3
        return _oit_untonemap([pix[o + k] / 255.0 for k in range(3)])

    return at


def run_translucent_shadow_gate(workdir):
    fixture = os.path.join(ROOT, "assets", "translucent_shadow_fixture.cscn")
    if not os.path.exists(fixture):
        print("  tsl          SKIP  (missing translucent_shadow_fixture.cscn)")
        return []

    on = _tsl_render(workdir, "on", ["--translucent-shadows"])
    if on is None:
        print("  tsl          ERROR while rendering the fixture")
        return ["tsl"]
    at = _tsl_reader(on)

    def lum(p):
        return sum(at(p)) / 3.0

    lit = lum((TSL_LIT_X, 0.0, TSL_STAIR_ZC))

    # The zero of the scale is 0 BY CONSTRUCTION, not measured: the fixture
    # declares one directional light and no environment, so a fully shadowed
    # ground texel receives nothing. Taking it from the opaque block's umbra
    # instead made tsl-opaque a tautology -- it asserted that (black - black)
    # was under a bound, which is exactly 0 for any renderer whatsoever, and
    # would have passed a shader that flooded every umbra with full light.
    def transmittance(p):
        return lum(p) / lit

    failures = []

    # --- the staircase, absolute -------------------------------------------
    bands = [transmittance((TSL_BAND_X0 + (j + 0.5) * TSL_BAND_W, 0.0, TSL_STAIR_ZC))
             for j in range(TSL_PANELS + 1)]
    pred = [(1.0 - TSL_ALPHA) ** j for j in range(TSL_PANELS + 1)]
    worst = max(abs(b - p) for b, p in zip(bands, pred))
    ok = worst <= TSL_STACK_TOL
    print(f"  tsl-stack    {'PASS' if ok else 'FAIL'}  worst {worst:.4f} against 0.65^k "
          f"(bound {TSL_STACK_TOL}); measured "
          + " ".join(f"{b:.3f}" for b in bands))
    if not ok:
        failures.append("tsl-stack")

    # --- the staircase, as ratios, with NO reference frame ------------------
    # Deliberately a second path to the same claim: this one is immune to the
    # tonemap inversion, the exposure pin and the black point, so the two arms
    # cannot fail for the same instrument reason.
    raw = [lum((TSL_BAND_X0 + (j + 0.5) * TSL_BAND_W, 0.0, TSL_STAIR_ZC))
           for j in range(TSL_PANELS + 1)]
    ratios = [raw[j + 1] / raw[j] for j in range(TSL_PANELS) if raw[j] > 1e-6]
    worst_r = max(abs(r - (1.0 - TSL_ALPHA)) for r in ratios) if ratios else 1.0
    ok = worst_r <= TSL_RATIO_TOL
    print(f"  tsl-ratio    {'PASS' if ok else 'FAIL'}  worst |ratio - 0.65| {worst_r:.4f} "
          f"(bound {TSL_RATIO_TOL}); " + " ".join(f"{r:.3f}" for r in ratios))
    if not ok:
        failures.append("tsl-ratio")

    # --- the mask ramp ------------------------------------------------------
    # A LINEAR ramp is preserved exactly by any symmetric filter -- PCF,
    # bilinear, a mip -- so filtering cannot bias this and only a wrong law can.
    # A binary alpha test at the cutoff is the law this is here to reject.
    xs = [TSL_BAND_X0 + TSL_BAND_W + (TSL_CANOPY_XR - (TSL_BAND_X0 + TSL_BAND_W)) *
          (0.1 + 0.8 * i / 23.0) for i in range(24)]
    meas = [transmittance((x, 0.0, TSL_RAMP_ZC)) for x in xs]
    truth = [1.0 - (x - (TSL_BAND_X0 + TSL_BAND_W)) /
             (TSL_CANOPY_XR - (TSL_BAND_X0 + TSL_BAND_W)) for x in xs]
    rms = math.sqrt(sum((m - t) ** 2 for m, t in zip(meas, truth)) / len(meas))
    ok = rms <= TSL_RAMP_RMS_MAX
    print(f"  tsl-ramp     {'PASS' if ok else 'FAIL'}  RMS {rms:.4f} against 1-alpha(x) "
          f"(bound {TSL_RAMP_RMS_MAX})")
    if not ok:
        failures.append("tsl-ramp")

    # The inverse of the arm above. Without it, a fixture that stopped
    # resolving the ramp scores its BEST possible mark and asserts nothing.
    best_step = min(
        math.sqrt(sum((m - (1.0 if x < c else 0.0)) ** 2 for m, x in zip(meas, xs)) / len(meas))
        for c in xs)
    ok = rms * TSL_RAMP_DISCRIM <= best_step
    print(f"  tsl-discrim  {'PASS' if ok else 'FAIL'}  best binary step RMS {best_step:.4f} "
          f"must be >= {TSL_RAMP_DISCRIM}x the ramp's {rms:.4f}")
    if not ok:
        failures.append("tsl-discrim")

    # --- solid stays solid, bare stays bare, both in THIS frame -------------
    # The gap in z BETWEEN the stair cast band (ends -2.36) and the ramp's
    # (starts -1.30), sampled as a strip. Deliberately not band 0, which
    # tsl-stack already reads -- that would restate tsl-stack at a looser bound
    # and catch nothing new. Not the x < -8 bare strip either: at this camera
    # the frame only reaches x = -7.5, so those samples are off-picture.
    acne = [transmittance((x, 0.0, -1.83)) for x in (-4.0, -2.0, 0.0, 2.0, 4.0)]
    worst_acne = max(abs(a - 1.0) for a in acne)
    ok = worst_acne <= TSL_ACNE_TOL
    print(f"  tsl-acne     {'PASS' if ok else 'FAIL'}  worst |T-1| over uncovered ground "
          f"{worst_acne:.4f} (bound {TSL_ACNE_TOL}: a target never cleared darkens everything)")
    if not ok:
        failures.append("tsl-acne")

    opaque_t = transmittance(TSL_OPAQUE_SAMPLE)
    ok = abs(opaque_t) <= TSL_OPAQUE_MAX
    print(f"  tsl-opaque   {'PASS' if ok else 'FAIL'}  opaque umbra reads {opaque_t:.4f} "
          f"(want <= {TSL_OPAQUE_MAX}: transmittance must not leak onto opaque casters)")
    if not ok:
        failures.append("tsl-opaque")

    # --- monochrome, as shipped --------------------------------------------
    # Two claims on one sample, because the spread alone is 0 on bare ground and
    # 0 in a full umbra -- it passes if the sample walks off the panel. Pinning
    # the level too says the sample IS under the panel, and checks the
    # single-layer prediction at an alpha the staircase does not use.
    red = at((0.0, 0.0, TSL_RED_ZC))
    spread = max(red) - min(red)
    red_t = (sum(red) / 3.0) / lit
    ok = spread <= TSL_MONO_TOL and abs(red_t - 0.5) <= TSL_STACK_TOL
    print(f"  tsl-mono     {'PASS' if ok else 'FAIL'}  red panel T {red_t:.4f} (want 0.5 "
          f"+/-{TSL_STACK_TOL}) at a channel spread of {spread:.4f} (want <= {TSL_MONO_TOL})")
    if not ok:
        failures.append("tsl-mono")

    return failures


# Passes asserted BY NAME rather than by a count. A threshold drifts with the
# scope list and cannot say which pass vanished; the defect this catches -- the
# whole post chain writing through a NULL profiler -- left exactly three rows
# standing, which any count low enough to be safe would have cleared.
GPU_REQUIRED_ROWS = frozenset(
    {"shadow cascades", "opaque", "gtao sweep", "ssr", "bloom pyramid", "tonemap + finishing"})
GPU_MIN_POSITIVE = 4          # rows that must be strictly > 0, not merely present
GPU_SUM_MIN = 0.30            # TIMED must be at least this fraction of FRAME
GPU_SCALE_DROP = 0.20         # render-res passes must shed this much at half scale


# The report's row format is the assertion surface, so a shifted format has to
# report as a named FAIL rather than as missing rows -- the arms below would
# otherwise blame the renderer for a parser change. Anchored on the " ms"
# suffix; the non-greedy name cannot swallow the number.
_GPU_ROW = re.compile(r"^(.*?)\s+(-?\d+\.\d+) ms$")


# The SHADING block, read on its own rather than through _report_tables. Its rows
# are bare integers where every block that parser knows ends in " ms" or carries
# exactly len(SUBMIT_COLS) columns, so teaching it this shape would mean either a
# third row format in the shared parser or every SHADING row landing in
# `unparsed` -- and four gates assert that `unparsed` is zero.
_SHADING_SAMPLES = re.compile(r"^samples shaded\s+(\d+)$", re.M)
_SHADING_BUDGET = re.compile(r"^sample budget\s+(\d+)$", re.M)
_SHADING_COMPLEXITY = re.compile(r"^depth complexity\s+([\d.]+)$", re.M)


def _shading_block(text):
    """{"shaded", "budget", "complexity"} or None if the block did not print."""
    s = _SHADING_SAMPLES.search(text)
    b = _SHADING_BUDGET.search(text)
    c = _SHADING_COMPLEXITY.search(text)
    if not s or not b or not c:
        return None
    return {"shaded": int(s.group(1)), "budget": int(b.group(1)),
            "complexity": float(c.group(1))}


GPU_FIXTURE = "dir_shadow_fixture.cscn"


def _gpu_cmd(out, extra, profile, fixture=GPU_FIXTURE, size=("800", "600"), frames="45"):
    """The one command line every profiled run uses.

    Built in one place because gpu-off's whole claim is that its two renders
    differ in exactly one flag; two hand-written lists could drift and the arm
    would then compare two different pictures and blame the instrument. The
    fixture and size are parameters for the same reason -- a submission gate
    with its own copy of this list would silently stop tracking a flag added
    here.

    -f 45 is enough to close a latch window (0.5s at this fixture's frame time
    is ~frame 38) and profiler_report publishes whatever is left over
    regardless, so the run length is not load-bearing beyond that.

    THAT LAST PARAGRAPH IS TRUE FOR A COUNT AND FALSE FOR A CLOCK, which is why
    `frames` is a parameter now. profiler_report publishes the LAST LATCHED
    WINDOW (profiler.c), so a run of one window long publishes the FIRST one --
    and the first window is warm-up: program compilation, pipeline creation,
    first-use allocation, clock ramp. Every submission arm here reads integers
    and does not care. An arm reading milliseconds is measuring startup mixed
    with steady state, in a proportion that changes run to run.
    Measured on overdraw_tiles: at 45 frames five identical renders spread 6.4%
    with a 36% outlier; at 200 frames four spread 3.0% with none. And the
    quantity itself moves -- the depth prepass reads +8% to +15% inside the
    warm-up window against +4% to +5% at steady state, because compiling its
    second program is part of what the short run is timing.
    """
    return ([RENDER, "-m", os.path.join(ROOT, "assets", fixture), "-x", "-f", frames,
             "-W", size[0], "-H", size[1], "--no-auto-exposure", "-E", "1.0", "-S", out]
            + (["--profiler"] if profile else []) + extra)


# Every banner-delimited block the report prints. One parser rather than one per
# gate: the row formats are a shared assertion surface, and a second copy would
# let a format change fail in one gate and pass in another.
#
# unparsed is counted PER BLOCK. Folding them together made a SUBMISSION format
# change fail gpu-parse, which claims to be about the GPU table -- an arm going
# red for something it does not name is the failure this suite exists to avoid.
_REPORT_BANNERS = {"GPU TIMING": "gpu", "CPU TIMING": "cpu", "SUBMISSION": "submit"}

# The SUBMISSION block is a matrix, one row per pass: a %-28s name field then one
# right-aligned integer per counter. Column names contain spaces, so the header
# cannot be split reliably -- the order is fixed here instead, and submit-parse
# asserts the header still says what this expects.
SUBMIT_COLS = ("meshes seen", "meshes culled", "draws", "instances", "material switches",
               "triangles")
_SUBMIT_NAME_WIDTH = 28


def _report_tables(text):
    """Parse every block of a --profiler report.

    Returns {"gpu": {pass: ms}, "cpu": {pass: ms},
             "submit": {pass: {col: int}}, "submit_header": str|None,
             "unparsed": {block: n}}
    """
    out = {"gpu": {}, "cpu": {}, "submit": {}, "submit_header": None,
           "unparsed": {"gpu": 0, "cpu": 0, "submit": 0}}
    block = None
    for line in text.splitlines():
        stripped = line.rstrip()
        if stripped.startswith("===== END"):
            block = None
            continue
        if stripped.startswith("====="):
            block = None
            for banner, key in _REPORT_BANNERS.items():
                if stripped.startswith("===== " + banner):
                    block = key
            continue
        if block is None or not stripped.strip():
            continue
        if block == "submit":
            name = stripped[:_SUBMIT_NAME_WIDTH].strip()
            values = stripped[_SUBMIT_NAME_WIDTH:].split()
            if name == "pass":
                out["submit_header"] = stripped[_SUBMIT_NAME_WIDTH:].strip()
                continue
            if len(values) == len(SUBMIT_COLS) and all(v.isdigit() for v in values):
                out["submit"][name] = dict(zip(SUBMIT_COLS, (int(v) for v in values)))
            else:
                out["unparsed"]["submit"] += 1
            continue
        m = _GPU_ROW.match(stripped)
        if m:
            out[block][m.group(1).strip()] = float(m.group(2))
        elif stripped.strip() != "no passes timed":
            out["unparsed"][block] += 1
    return out


_IMPORT_DEDUP = re.compile(
    r"Import: (\d+) meshes built, (\d+) shared references, (\d+) LOD chains")


def _profiled_run(workdir, tag, extra, screenshot=None, fixture=GPU_FIXTURE, size=("800", "600"),
                  frames="45"):
    """One profiled render. Returns the parsed tables, or None on failure.

    The tables carry an "import" entry as well, because the dedup counters are
    logged rather than tabled and an arm that wanted them would otherwise pay
    for a whole extra render to see one line.
    """
    out = screenshot or os.path.join(workdir, f"gpu_{tag}.ppm")
    r = _run(_gpu_cmd(out, extra, True, fixture, size, frames),
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        print(f"  profiler     ERROR {tag} exited {r.returncode}: "
              f"{(r.stdout + r.stderr).strip()[-300:]}")
        return None
    text = r.stdout + r.stderr
    tables = _report_tables(text)
    m = _IMPORT_DEDUP.search(text)
    tables["import"] = ({"built": int(m.group(1)), "shared": int(m.group(2)),
                         "lod_chains": int(m.group(3))} if m else None)
    # None when the block did not print, which is what a caller asserting on it
    # has to notice; _report_tables cannot carry it, since SHADING rows are bare
    # integers where every block it knows ends in " ms" or has SUBMIT_COLS
    # columns, and four gates assert its `unparsed` count is zero.
    tables["shading"] = _shading_block(text)
    return tables


def _gpu_table(workdir, tag, extra, screenshot=None):
    """The GPU block alone, in the (rows, unparsed) shape this gate's arms use."""
    tables = _profiled_run(workdir, tag, extra, screenshot)
    if tables is None:
        return None, None
    return tables["gpu"], tables["unparsed"]["gpu"]


def _timing_delta(base_runs, variant_runs, row="opaque"):
    """(before, after, signed relative delta, run-to-run floor, separated) or None.

    Both arguments are LISTS of profiled runs of one config -- at least two of
    the base, so there is a floor at all. Any run missing the row, or reading
    zero, voids the measurement: a vanished pass reading 0 ms scores as a 100%
    saving, which is the confusion this helper was factored out to prevent.

    One helper because two arms make the same claim in the same shape -- "this
    config moved the row by X against a floor of Y" -- and the second copy had
    already drifted.

    THE ESTIMATOR IS THE FASTEST RUN, NOT THE MEAN OR THE MEDIAN, because this
    noise is mostly ONE-SIDED: contention, scheduling and thermal state can only
    make a render slower than the work it does. So the quickest sample of a
    config is its least-contaminated one, and averaging mixes interference back
    into the number. Measured over five identical renders of overdraw_tiles:
    5.319 / 7.253 / 5.363 / 5.476 / 5.661 ms -- four inside 6% and one 33% high.
    The mean is 5.81, the median 5.48, the minimum 5.32, and only the last is a
    statement about the renderer.

    `separated` IS THE HONEST SEPARATION TEST and the reason this returns five
    things. It is True when the variant's FASTEST run is slower than the base's
    SLOWEST -- the two sets of readings do not overlap at all, so no assignment
    of the noise to either config could account for the gap. It replaced a
    `delta >= 3 * floor` rule at the one call site that had one, and the
    replacement is not a relaxation: it is a claim the multiplier could not make.
    Three times a floor is a statement about a distribution nobody sampled, and
    when the floor drew high it demanded a 15% effect of a renderer whose real
    effect is 5-14%, so a correct build failed one run in five. Non-overlap needs
    no constant, gets STRICTER as the reader adds samples rather than looser, and
    is exactly what "these two configs are not the same speed" means.

    `floor` is still returned and still printed, as the gap between the two
    FASTEST base runs: it is what a reader needs to judge whether a green result
    was comfortable or lucky, and gpu-scale still asserts on it directly.
    """
    b_all = [t.get(row) for t in base_runs]
    v_all = [t.get(row) for t in variant_runs]
    if len(b_all) < 2 or not v_all:
        return None
    if any(x is None or x <= 0.0 for x in b_all + v_all):
        return None
    b_sorted = sorted(b_all)
    b, v = b_sorted[0], min(v_all)
    return b, v, (v - b) / b, (b_sorted[1] - b) / b, v > max(b_all)


def run_profiler_gate(workdir):
    if not os.path.exists(os.path.join(ROOT, "assets", GPU_FIXTURE)):
        print(f"  gpu          SKIP  (missing {GPU_FIXTURE})")
        return []

    on_ppm = os.path.join(workdir, "gpu_on.ppm")
    base, unparsed = _gpu_table(workdir, "base", [], screenshot=on_ppm)
    if base is None:
        return ["gpu"]
    failures = []

    ok = unparsed == 0
    print(f"  gpu-parse    {'PASS' if ok else 'FAIL'}  {unparsed} unreadable rows "
          f"(a shifted report format must fail here, not as missing passes below)")
    if not ok:
        failures.append("gpu-parse")

    named = {k: v for k, v in base.items() if k not in ("TIMED", "FRAME (wall)")}

    # By name, not by count: a count cannot say WHICH pass vanished, and the
    # real defect left three rows standing.
    missing = sorted(GPU_REQUIRED_ROWS - set(named))
    ok = not missing
    print(f"  gpu-rows     {'PASS' if ok else 'FAIL'}  {len(named)} passes; "
          f"{'none missing' if ok else 'MISSING ' + ', '.join(missing)}")
    if not ok:
        failures.append("gpu-rows")

    # Strictly positive and finite. "total > 0" passed on a single nonzero row,
    # and a driver that reports zeros is the thing this arm exists for.
    positive = [v for v in named.values() if v > 0.0]
    finite = all(math.isfinite(v) for v in named.values())
    ok = len(positive) >= GPU_MIN_POSITIVE and finite
    print(f"  gpu-nonzero  {'PASS' if ok else 'FAIL'}  {len(positive)} of {len(named)} passes "
          f"above zero, all finite: {finite} (want >= {GPU_MIN_POSITIVE} positive; a driver "
          f"that accepts queries and measures nothing looks identical until here)")
    if not ok:
        failures.append("gpu-nonzero")

    # TIMED against the wall-clock ceiling. Their difference also carries CPU
    # time and GPU idle, so this is a loose bound on how much GPU work no scope
    # covers -- loose is still the only thing standing between the table and a
    # third of the frame going unnamed.
    timed, frame = base.get("TIMED"), base.get("FRAME (wall)")
    if timed is None or frame is None or frame <= 0.0:
        ok = False
        print(f"  gpu-sum      FAIL  report carries no TIMED/FRAME pair (timed={timed}, "
              f"frame={frame})")
    else:
        ok = timed >= GPU_SUM_MIN * frame
        print(f"  gpu-sum      {'PASS' if ok else 'FAIL'}  TIMED {timed:.3f} ms is "
              f"{timed / frame * 100.0:.0f}% of the {frame:.3f} ms frame "
              f"(want >= {GPU_SUM_MIN * 100.0:.0f}%)")
    if not ok:
        failures.append("gpu-sum")

    # The claim the goldens cannot make: they run WITHOUT the flag, so they say
    # nothing about whether wrapping every pass in a query moves a pixel.
    off_ppm = os.path.join(workdir, "gpu_off.ppm")
    r = _run(_gpu_cmd(off_ppm, [], False), capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(off_ppm):
        print("  gpu-off      ERROR while rendering without the flag")
        failures.append("gpu-off")
    else:
        ae, _ = compare(on_ppm, off_ppm)
        ok = ae == 0
        print(f"  gpu-off      {'PASS' if ok else 'FAIL'}  {ae} px between profiled and "
              f"unprofiled (want exactly 0: the instrument must not move what it measures)")
        if not ok:
            failures.append("gpu-off")

    # The inverse arm. A pass that is off must have NO row, and turning it on
    # must produce one that costs something -- without this, every arm above
    # passes against a profiler printing plausible constants.
    dof_on, _ = _gpu_table(workdir, "dof", ["--dof", "--dof-focus", "6"])
    if dof_on is None:
        failures.append("gpu-dof")
    else:
        absent = "dof" not in named
        present = dof_on.get("dof", 0.0) > 0.0
        ok = absent and present
        print(f"  gpu-dof      {'PASS' if ok else 'FAIL'}  dof absent without the flag: "
              f"{absent}; {dof_on.get('dof', 0.0):.3f} ms with it (want > 0)")
        if not ok:
            failures.append("gpu-dof")

    # Attached to real work, not to the frame counter. Both runs carry --taa
    # --headless-jitter so the only difference is the pixel count: TAA jitters
    # the projection the opaque pass draws with, and a control that changes
    # three things cannot attribute the delta to one of them.
    taa = ["--taa", "--headless-jitter"]
    full, _ = _gpu_table(workdir, "full", taa + ["--render-scale", "1.0"])
    half, _ = _gpu_table(workdir, "half", taa + ["--render-scale", "0.5"])
    # A second full-scale run, to say what run-to-run variance is before
    # claiming a drop is real. Timings are noisier than pixels, and this tree
    # requires the floor be measured rather than assumed.
    full2, _ = _gpu_table(workdir, "full2", taa + ["--render-scale", "1.0"])
    if full is None or half is None or full2 is None:
        failures.append("gpu-scale")
    else:
        # Presence before ratio, in _timing_delta: a missing row read as 0 ms
        # scores a vanished pass as a 100% saving.
        timing = _timing_delta([full, full2], [half])
        if timing is None:
            print(f"  gpu-scale    FAIL  no usable opaque row to compare (full="
                  f"{full.get('opaque')}, half={half.get('opaque')}, "
                  f"repeat={full2.get('opaque')}); the pass was not timed, not cheap")
            failures.append("gpu-scale")
        else:
            # `separated` is unread here: this arm asserts a large DROP against
            # an absolute floor bar, which is its own separation test and a
            # stricter one at these magnitudes (half resolution is not a 5%
            # effect). prepass-crossover is the caller that needs it.
            before, after, signed, noise, _ = timing
            drop = -signed  # half scale should make it SMALLER
            ok = drop >= GPU_SCALE_DROP and noise < GPU_SCALE_DROP
            print(f"  gpu-scale    {'PASS' if ok else 'FAIL'}  opaque {before:.3f} -> "
                  f"{after:.3f} ms at half scale, {drop * 100.0:.0f}% off "
                  f"(want >= {GPU_SCALE_DROP * 100.0:.0f}%), against a "
                  f"{noise * 100.0:.0f}% run-to-run floor")
            if not ok:
                failures.append("gpu-scale")

    return failures


# ---------------------------------------------------------------------------
# Submission counters (spec 11.28)
#
# These arms assert on INTEGERS, and that is the whole design. The timing arms
# above have to measure a run-to-run floor before they can claim a drop is
# real; a draw count has no floor to measure, so "553 became 71" is either true
# or the feature does not work. Nothing here can inherit the suite's flakiness.
SUBMIT_FIXTURE = "instancing_fixture.cscn"
SUBMIT_CASCADES = 3   # pinned on the command line below, not inherited from the C default
# The eligibility fixture: one skinned mesh on two nodes. No .cscn -- it needs
# no look, only a program that cannot read InstanceBlock.
SKIN_FIXTURE = "skinned_instance_fixture.gltf"


def _fixture_mesh_nodes(name="instancing_fixture.gltf"):
    """Mesh-bearing node count, read from the fixture rather than copied.

    A hand-mirrored constant goes stale the moment the generator changes and
    takes the arm with it -- silently, because the arm would still pass against
    whatever it was told to expect.
    """
    path = os.path.join(ROOT, "assets", name)
    with open(path) as f:
        gltf = json.load(f)
    return sum(1 for n in gltf["nodes"] if "mesh" in n)


def _ubo_instance_max():
    """The chunk size, read from ubo.h for the same reason as the node count.

    inst-count's expected draw count is mesh_nodes divided by this. Mirroring
    it here would let the C constant move while the arm kept asserting the old
    quotient -- and passing, because it would still be checking the renderer
    against a number it made up.
    """
    path = os.path.join(ROOT, "cetra", "src", "ubo.h")
    with open(path) as f:
        m = re.search(r"^#define\s+UBO_INSTANCE_MAX\s+(\d+)", f.read(), re.M)
    if not m:
        raise RuntimeError("UBO_INSTANCE_MAX not found in cetra/src/ubo.h")
    return int(m.group(1))


def _submit_run(workdir, tag, extra, cascades=SUBMIT_CASCADES):
    """One profiled run of the submission fixture, at a size that renders fast.

    All three tables come out of ONE render, so an arm reading a count against
    a timing is reading the same frame rather than two runs of it. The cascade
    count is always pinned on the command line, so SUBMIT_CASCADES stops
    shadowing a C constant it cannot see change.
    """
    return _profiled_run(workdir, f"submit_{tag}",
                         ["--shadow-cascades", str(cascades)] + extra,
                         fixture=SUBMIT_FIXTURE, size=("400", "300"))


def _submit_sum_violations(tables):
    """seen == instances + culled, per pass. Returns (rows, the rows that break it).

    Returns rather than prints, unlike its first form, because gate-arm-docs
    reads each group's own source for the arms it runs -- so an arm printed from
    in here is invisible to the checker, and the group carrying it has to opt out
    of being checked at all. The arithmetic still lives once; only the verdict
    line moved to the caller, which is where every other helper in this file
    leaves it.
    """
    rows = dict(tables["submit"])
    bad = [(name, r["meshes seen"], r["instances"], r["meshes culled"])
           for name, r in rows.items()
           if r["meshes seen"] != r["instances"] + r["meshes culled"]]
    return rows, bad


def _submit_sum_detail(tables, label):
    """(ok, detail) for the submission identity. The CALLER prints the verdict.

    Split this way, rather than printing here, because gate-arm-docs reads each
    group's own source for the arm names it runs -- so an arm whose verdict line
    lives in a helper is invisible to the checker, and the group carrying it has
    to opt out of being checked at all. Every other helper in this file already
    works this way; this one did not.
    """
    rows, bad = _submit_sum_violations(tables)
    ok = bool(rows) and not bad
    return ok, (f"{label}: {len(rows)} passes, "
                f"{'identity holds in each' if ok else f'violations {bad}'}")


FOREST = _bin("forest")
_FOREST_CHAINS = re.compile(r"Forest: (\d+) LOD chains built, (\d+) refused")
_FOREST_MESHES = re.compile(r"Forest: (\d+) distinct meshes")
# What the terrain contributed. Since spec 11.63 that is not a constant: the
# quadtree selects against the camera, so two runs at different framings draw
# different numbers of patches, and any arm comparing mesh counts across framings
# has to subtract this to be left with the props it meant to count.
_FOREST_TERRAIN = re.compile(
    r"Forest terrain: (?:quadtree \d+ levels, (\d+) patches selected|fixed grid, (\d+) tiles)")
# Position, velocity, ground state and ground normal. The last two are what let
# forest-rest tell "standing still" from "fell through the collider".
_FOREST_TRACE = re.compile(
    r"player t=\s*[\d.]+ pos\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+"
    r"vel\s+-?[\d.]+\s+-?[\d.]+\s+-?[\d.]+\s+grounded (\d)\s+ground_n\.y (-?[\d.]+)")

# One framing for every arm that is not about framing. Inside the terrain, high
# enough to see past the near trees, so a good fraction of the world is culled
# and the rest is a mix of distances -- which is what makes the batching and LOD
# numbers mean something rather than measuring an empty or a fully-visible frame.
FOREST_CAM = ["--cam-eye", "0,40,120", "--cam-target", "0,10,0"]

# Off the island and above it, so the whole of it is in frame and the props run
# from a couple of hundred units out to eleven hundred. For the two arms that are
# about DISTANCE, and they need their own framing since 11.63 made the app an
# island: standing on it, everything is inside a 425-unit disc and there is no far
# field for LOD to save anything in. Measured 5% saved at FOREST_CAM against 44%
# here, on the same scene -- the first is not LOD failing, it is a camera with
# nothing distant in front of it.
FOREST_CAM_WIDE = ["--cam-eye", "0,300,700", "--cam-target", "0,-20,0"]

# Above the terrain aimed up and away, so nothing the scatter placed is in front
# of the camera and forest-cull can assert exact numbers instead of a direction.
FOREST_CAM_AWAY = ["--cam-eye", "0,300,0", "--cam-target", "600,900,600"]


_FOREST_RUN_CACHE = {}


def _forest_run(tag, extra, cam=None):
    """One profiled forest run, or None if it did not produce a readable report.

    Built in one place for the same reason _gpu_cmd is: several arms here claim
    their two runs differ in exactly one flag, and two hand-written command lists
    would let that stop being true without anything failing.

    Returning None for an unreadable report -- rather than an empty table every
    caller then has to .get() its way around -- is what lets the arms index
    columns directly. An arm reading a missing column as 0 does not fail loudly;
    it compares 0 against 0 and passes.

    MEMOIZED on (extra, cam), because two pairs of these are byte-identical
    across gate groups -- cluster-parity's baseline and lod's, and their two
    --no-lod runs -- differing only in the output filename. The arms already
    require those results to be equal, so serving one run to both is not an
    assumption, it is the assumption they are built on. `tag` is deliberately
    NOT part of the key: it names the file, and two runs that differ only in
    where the screenshot lands are the same run.
    """
    key = (tuple(extra), tuple(cam) if cam else None)
    if key in _FOREST_RUN_CACHE:
        return _FOREST_RUN_CACHE[key]

    # --no-fog because the app defaults it on: it is a froxel volume with its own
    # accumulator and it costs real time per run, while contributing nothing to
    # the submission counts every arm here reads.
    # No -S: every arm here reads the profiler's tables and the startup log, and
    # none looks at a pixel -- so a screenshot is a full readback and a file write
    # for nothing. _terrain_run reached this conclusion first; the pattern is the
    # same one.
    cmd = ([FOREST, "-x", "-f", "20", "-W", "800", "-H", "450", "--profiler", "--no-fog"]
           + (cam or FOREST_CAM) + extra)
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  forest       ERROR {tag} exited {r.returncode}: "
              f"{(r.stdout + r.stderr).strip()[-300:]}")
        return None
    text = r.stdout + r.stderr
    tables = _report_tables(text)
    m = _FOREST_CHAINS.search(text)
    mm = _FOREST_MESHES.search(text)
    shading = _shading_block(text)
    unparsed = sum(tables["unparsed"].values())
    if not m or not mm or not shading or unparsed or "opaque" not in tables["submit"]:
        print(f"  forest       ERROR {tag} report unreadable: {unparsed} unparsed rows, "
              f"chains {'yes' if m else 'no'}, meshes {'yes' if mm else 'no'}, "
              f"shading {'yes' if shading else 'no'}, "
              f"opaque {'yes' if 'opaque' in tables['submit'] else 'no'}")
        return None
    tables["chains"] = {"built": int(m.group(1)), "refused": int(m.group(2))}
    tables["meshes"] = int(mm.group(1))
    mt = _FOREST_TERRAIN.search(text)
    if not mt:
        # None, not 0, for the reason this helper's docstring gives: forest-cull
        # subtracts this, and a silent zero would take it back to comparing raw
        # mesh counts across two framings, which is what stopped working.
        print(f"  forest       ERROR {tag} printed no terrain summary")
        return None
    tables["terrain_meshes"] = int(mt.group(1) or mt.group(2))
    tables["shading"] = shading
    tables["opaque"] = tables["submit"]["opaque"]
    _FOREST_RUN_CACHE[key] = tables
    return tables


MASK_FIXTURE = "mask_fixture.cscn"

# Quad interiors as fractions of the frame, so the arm reads the same surface
# whatever the display scale reports. Inset well clear of the silhouettes, which
# is where TAA and the geometric AA live and neither is what this measures.
#
# Measured from the fixture rather than derived: they are tied to
# mask_fixture.cscn's camera AND to the 4:3 that `render` renders at, so a box
# that stopped landing on its quad would read the backdrop and fail loudly --
# 0.048 against a 0.47 surface is not a near miss.
#
# REF and KEPT are frame-symmetric (0.2175 and 0.7812 about the centre), and
# that is load-bearing: the vignette falls off with radius, so equal radii make
# it cancel between the pair instead of biasing one. Re-centring either box on
# its quad would quietly remove that.
MASK_REF_BOX = (0.165, 0.42, 0.270, 0.58)
MASK_KEPT_BOX = (0.729, 0.42, 0.834, 0.58)
MASK_GONE_BOX = (0.448, 0.42, 0.553, 0.58)
# Mean linear luma of a lit quad against this fixture's backdrop, which measures
# 0.048. A load failure leaves every box reading that same background, which
# would satisfy an equality arm perfectly -- so the surfaces have to be shown to
# be THERE before their agreement means anything.
MASK_LIT_FLOOR = 0.15
# And the discarded quad has to be shown to be ABSENT. Nearer the backdrop than
# to a lit surface, with room for the vignette and the tonemap toe.
MASK_GONE_CEIL = 0.10
MASK_GRID = 12


def _box_luma(pix, w, h, box):
    """Mean linear luma over a fractional box, on a MASK_GRID square grid.

    Named for what it does rather than for the group that first wanted it: the shared
    readers live a long way above the gates that need them, and a group-scoped name is
    how a later group ends up copying its neighbour instead of calling this.
    """
    x0, y0, x1, y1 = box
    n = MASK_GRID
    samples = [_linear_luma(pix, w, h, (x0 + (x1 - x0) * (ix + 0.5) / n) * w,
                            (y0 + (y1 - y0) * (iy + 0.5) / n) * h)
               for iy in range(n) for ix in range(n)]
    return sum(samples) / len(samples)


# KHR_materials_volume absorption (spec 11.32), on assets/absorption_fixture.
#
# Three transmissive panels over ONE uniform emissive backdrop, carrying the same
# attenuationColor (1.0, 0.35, 0.35) and attenuationDistance 0.3, differing only
# in volume thickness: 0.0, 0.3, 0.6. Thickness is the path length Beer-Lambert
# integrates over, so the transmitted green:red ratio should fall as it grows
# while red passes unabsorbed and stays put.
#
# Every arm here is a RATIO or an equality between regions of one frame, never a
# stored level. The tonemap is a monotone curve the fixture does not control --
# measured, PBR Neutral moves the middle panel's ratio from an analytic 0.35 to
# 0.30 -- and passthrough is unreachable from the CLI by design (render_args.h
# notes the sentinel deliberately coincides with it). A monotone curve preserves
# ordering and maps equal inputs to equal outputs, which is exactly what these
# three assertions rest on and all a correct implementation needs to satisfy.
ABSORB_FIXTURE = "absorption_fixture.cscn"
# Boxes are fractions of the frame, centred on each panel and well inside it, so
# the refraction bend at the panel edges never enters the read.
ABSORB_BOXES = {
    "t000": (0.2335, 0.44, 0.3335, 0.56),
    "t030": (0.4500, 0.44, 0.5500, 0.56),
    "t060": (0.6665, 0.44, 0.7665, 0.56),
}
# Bare backdrop seen through the GAP between the middle and right panels, at the
# same frame rows as the panel boxes so the vignette and the tone curve cancel out
# of the comparison. (A box in the frame corner does not: measured 0.1018 there
# against 0.5271 on a panel, which is the vignette, not absorption.)
#
# This is the presence floor. The backdrop is ONE uniform emissive grey, so if the
# panels fail to load or the transmissive lane is skipped, every panel box reads that
# same colour -- and absorb-thin and absorb-red are then satisfied perfectly by an
# empty frame. That is the hole run_mask_gate records at MASK_LIT_FLOOR. Green is the
# channel to compare: unabsorbed through the gap, heavily absorbed through the
# thickest panel, so the ratio is large and unambiguous.
ABSORB_GAP_BOX = (0.598, 0.44, 0.626, 0.56)
ABSORB_PRESENCE_MIN = 3.0
# Zero path length must be an exact identity, so this is quantization-tight rather
# than a tolerance band. Measured 1.0000.
ABSORB_THIN_EPS = 0.02
# Red carries attenuationColor 1.0 and must survive every thickness untouched.
# Relative, because the three panels share one backdrop and one Fresnel term.
# Measured spread 0.0000 across the three (0.5271 each).
ABSORB_RED_TOL = 0.02
# Each step of 0.3 in thickness should cost the green channel a large factor; 0.35
# per step analytically, and measured 3.32x then 4.47x (the tonemap steepens it), so
# 1.6x is a wide floor that still fails absorption applied without the thickness.
ABSORB_STEP_MIN = 1.6
ABSORB_GRID = 12


def _absorb_box_rgb(pix, w, h, box, n=ABSORB_GRID):
    """Mean linear RGB over a fractional box. Per-channel, not luma: the whole
    measurement is that the channels separate, which an average would erase."""
    x0, y0, x1, y1 = box
    acc = [0.0, 0.0, 0.0]
    for iy in range(n):
        for ix in range(n):
            rgb = _linear_rgb(pix, w, h, (x0 + (x1 - x0) * (ix + 0.5) / n) * w,
                              (y0 + (y1 - y0) * (iy + 0.5) / n) * h)
            for k in range(3):
                acc[k] += rgb[k]
    return [a / (n * n) for a in acc]


CLOUDSHADOW_FIXTURE = "aerial_fixture.gltf"
# Eight boxes across a band, tiled the same way on the sky and on the ground so the two
# pairs of arms differ in one thing only.
def _cloudshadow_strip(y0, y1):
    return [(0.02 + 0.12 * i, y0, 0.12 + 0.12 * i, y1) for i in range(8)]


# The SKY band, for the froxel half: it shadows in-scattered light, so it lands where the
# sight line crosses the most air.
CLOUDSHADOW_BAND = _cloudshadow_strip(0.05, 0.33)
# The GROUND band, for the surface half (spec 11.41). Read WITHOUT --fog, which is what makes
# it unambiguous: with no medium in the frame the froxel half contributes nothing, so every
# moving pixel is a shaded surface. Clear of the horizon, where the band would average terrain
# with sky.
CLOUDSHADOW_GROUND = _cloudshadow_strip(0.62, 0.88)
# The deck must remove light from whatever is under it. Measured 0.8476 on the air and 0.5647
# on the ground; wide floors on both, because these two arms' job is direction and presence and
# the variance arms below are what read content.
CLOUDSHADOW_DARKEN_MAX = 0.99
CLOUDSHADOW_GROUND_DARKEN_MAX = 0.97
# ...and it must remove DIFFERENT amounts in different places. Measured 0.9003 on the air and
# 0.2992 on the ground.
#
# On the GROUND this reads the map's contents through the lookup, and is the only arm that does.
# On the AIR it does NOT -- measured, and the opposite of what this comment claimed until 11.41.
# Against a stub returning a constant 0.5, taken at the configuration these arms ship in:
#
#   cloudshadow-dapple  0.0535  FAILS      the only arm that catches it
#   cloudshadow-ground  0.6777  passes     a constant still darkens
#   cloudshadow-vary    0.9268  passes     6x its own floor, on a dead lookup
#   -map / -lowsun              pass       correctly: the MAP is fine, the lookup is not
#
# The air arm's spread comes from the froxel composite's own structure whatever the map says. So
# -vary is a liveness arm and nothing more; -dapple plus the map arms are what separate a working
# lookup from a broken one, and a dead map from a dead lookup.
CLOUDSHADOW_SPREAD_MIN = 0.15
# Same accumulator the fog-volume fixture needs 60 frames for; these arms drive it too.
CLOUDSHADOW_FRAMES = 60
# The --sky-debug tile for the shadow map, at -W 400 -H 400 (framebuffer 800x800). The layout
# is sky.c's: column two at x = 20 + 2*SKY_TRANSMITTANCE_W, below the two noise tiles, and
# CLOUD_SHADOW_DEBUG_W a side. Rendered taller than the suite's usual 300 because at that
# height the tile's GL y goes negative and it falls off the bottom of the frame.
CLOUDSHADOW_TILE_BOX = (538, 420, 718, 600)
CLOUDSHADOW_TILE_SIZE = ("400", "400")
# The map must carry RANGE: some texel lit, and real variation between texels.
#
# This reads the map itself rather than its effect on anything, which is what lets it tell a bad
# MAP from a bad LOOKUP. Shipped 11.39 marched the full shell traverse and saturated to literally
# zero -- max below 7.8e-5 at every one of 256^2 texels -- and both downstream arms passed over
# it. The two floors catch the two saturations: peak catches all-black, sigma catches either.
# Re-derived at the coverage this arm now pins (it rode the engine default until 11.41, and the
# march it measures was rewritten in the same spec, so every earlier number here was stale):
# measured peak 1.0000 / sigma 0.3625, against 0.0 / 0.0 black and 1.0 / 0.0 white.
CLOUDSHADOW_MAP_MAX_MIN = 0.30
CLOUDSHADOW_MAP_SIGMA_MIN = 0.10
# And the same map at a LOW sun, which is a SEPARATE question the arm above cannot reach: it
# renders at one elevation, so it can only ever speak for that one.
#
# It could not have been more separate, as it turns out. The shipped map capped the marched path
# at a fixed 1.2 km while crossing the deck takes thickness/sin(elevation), so the fraction of
# cloud actually traversed fell with the sun -- 48% at the zenith, 4% at 5 degrees -- and below
# ~10 degrees the march never left the cloud base at all and the map came back uniformly 1.0.
# apps/tree runs a 0.8 degree sun and so had never had a cloud shadow from either half.
#
# Keyed on SIGMA and MEAN, deliberately not on peak: the failure was a uniformly WHITE map, whose
# peak is a perfect 1.0. A peak floor passes it. Measured sigma 0.0000 / mean 1.0000 broken
# against 0.2745 / 0.1148 fixed -- and against the bug reinstated, this arm fails while all five
# others pass, which is the whole reason it exists.
CLOUDSHADOW_LOWSUN_ELEV = "5"
CLOUDSHADOW_LOWSUN_SIGMA_MIN = 0.05
CLOUDSHADOW_LOWSUN_MEAN_MAX = 0.60
# Coverage for the ground and map arms, stated rather than inherited: an arm that rode the
# engine default would be silently re-tuned by anyone who changed it, and the default is 0.45.
#
# 0.10 is where this field has GAPS, and gaps are the whole mechanism -- extinction 25/km over a
# 2.5 km deck makes any real cloud in a column opaque, so a dappled ground is a map of the holes.
# Measured 47.9% of the map above half transmittance at 0.10 against 0.0% at 0.45, and the middle
# is no good either: at 0.2 the corrected march leaves the ground uniformly shaded, spread 0.0618
# against the 0.15 the dapple arm wants.
CLOUDSHADOW_COVERAGE = "0.10"


def _cloudshadow_band(on_path, off_path, boxes):
    """Mean shaded/lit ratio over `boxes`, and the relative spread of the SHORTFALL.

    The shortfall (1 - ratio) is what the deck removed, and it is the quantity a constant
    lookup makes uniform -- which is why the spread is taken on it rather than on the ratio.
    Returns (mean ratio, spread/mean of the shortfall, mean shortfall).
    """
    w, h, on = _read_ppm(on_path)
    _, _, off = _read_ppm(off_path)

    def luma(box, pix):
        v = _absorb_box_rgb(pix, w, h, box)
        return (v[0] + v[1] + v[2]) / 3.0

    ratios = [luma(b, on) / max(luma(b, off), 1e-6) for b in boxes]
    short = [1.0 - r for r in ratios]
    mean_short = sum(short) / len(short)
    return (sum(ratios) / len(ratios),
            (max(short) - min(short)) / max(mean_short, 1e-6),
            mean_short)


def _cloudshadow_map_stats(workdir, scene, tag, extra):
    """(peak, mean, sigma) of the --sky-debug shadow-map tile, or (None, error).

    Hand-rolled rather than render()'d because the tile needs a taller frame than the
    suite's default: below that height its GL y goes negative and it falls off the bottom.
    """
    tile = os.path.join(workdir, f"cloudshadow_{tag}.ppm")
    cmd = [RENDER, "-m", scene, "-x", "-f", "30",
           "-W", CLOUDSHADOW_TILE_SIZE[0], "-H", CLOUDSHADOW_TILE_SIZE[1],
           "-S", tile, "--cloud-coverage", CLOUDSHADOW_COVERAGE,
           "--sky-debug", "--no-auto-exposure", "-E", "1.0"] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(tile):
        return None, (r.stdout + r.stderr)[-200:]
    tw, _, tpix = _read_ppm(tile)
    x0, y0, x1, y1 = CLOUDSHADOW_TILE_BOX
    vals = [tpix[(y * tw + x) * 3] / 255.0
            for y in range(y0, y1, 4) for x in range(x0, x1, 4)]
    mean = sum(vals) / len(vals)
    sigma = (sum((v - mean) ** 2 for v in vals) / len(vals)) ** 0.5
    return (max(vals), mean, sigma), None


def run_cloud_shadow_gate(workdir):
    """Cloud shadows in the froxel volume (spec 11.39) and on the ground (spec 11.41).

    Six arms, none implying the others. The first two read the AIR with --fog, the next
    two the GROUND without it, and the last two read the map itself:

      cloudshadow-dark    the deck darkens the medium under it.
      cloudshadow-vary    it darkens by DIFFERENT amounts across the frame. A liveness
                          arm and NOT a content one: measured, it passes a constant
                          lookup at 0.9268 against a 0.15 floor, because the froxel
                          composite's shortfall varies with the fog's own structure.
      cloudshadow-ground  the terrain darkens too, with NO fog in the frame -- so the
                          froxel half cannot be what moved. This pair used to assert 0 px
                          on exactly these two renders, which was correct only while the
                          ground was not a receiver.
      cloudshadow-dapple  and it varies across the terrain. Same argument as -vary, on the
                          surface: this is the arm that reads the map's content through
                          pbr_frag rather than through the medium.
      cloudshadow-map     the map itself carries range. Reads the --sky-debug tile, and it
                          is the only arm that can tell a bad MAP from a bad LOOKUP -- 11.39
                          shipped one saturated to zero at every texel with both of its
                          downstream arms passing over it.
      cloudshadow-lowsun  and it carries range at a LOW sun too, which is a different question
                          the arm above cannot reach because it renders at one elevation. 11.41
                          found the map blank below ~10 degrees; against that bug this arm reads
                          sigma 0.0000 / mean 1.0000 and fails while all FIVE others pass.

    The GROUND and MAP arms pin --cloud-coverage; -dark and -vary ride the engine default,
    which is where the froxel half was originally measured. A gate wants the configuration
    where its property is legible rather than a representative frame, and the four pinned
    arms need the deck to have GAPS -- see CLOUDSHADOW_COVERAGE.
    """
    scene = os.path.join(ROOT, "assets", CLOUDSHADOW_FIXTURE)
    if not os.path.exists(scene):
        print(f"  cloudshadow-dark SKIP  ({CLOUDSHADOW_FIXTURE} not present)")
        return []

    base = ["--clouds", "--no-auto-exposure", "-E", "1.0"]
    on = os.path.join(workdir, "cloudshadow_on.ppm")
    off = os.path.join(workdir, "cloudshadow_off.ppm")
    err = render(scene, on, base + ["--fog"], frames=CLOUDSHADOW_FRAMES) or \
        render(scene, off, base + ["--fog", "--no-cloud-shadows"], frames=CLOUDSHADOW_FRAMES)
    if err:
        print(f"  cloudshadow-dark ERROR render failed: {err.strip()[-200:]}")
        return ["cloudshadow-dark"]

    failures = []
    mean_ratio, spread, mean_short = _cloudshadow_band(on, off, CLOUDSHADOW_BAND)

    ok = mean_ratio <= CLOUDSHADOW_DARKEN_MAX
    print(f"  cloudshadow-dark {'PASS' if ok else 'FAIL'}  shaded/lit={mean_ratio:.4f} "
          f"want <={CLOUDSHADOW_DARKEN_MAX}")
    if not ok:
        failures.append("cloudshadow-dark")

    ok = spread >= CLOUDSHADOW_SPREAD_MIN
    print(f"  cloudshadow-vary {'PASS' if ok else 'FAIL'}  shortfall spread/mean="
          f"{spread:.4f} want >={CLOUDSHADOW_SPREAD_MIN} (mean {mean_short:.4f})")
    if not ok:
        failures.append("cloudshadow-vary")

    # The SURFACE half (spec 11.41), on the same fixture with NO fog. Where this pair used to
    # assert 0 px -- "no medium, so nothing to shadow" -- the ground is now a receiver, so the
    # same two renders carry the opposite claim and the arm reads the terrain instead of the
    # frame. Nothing else can move here: with no fog the froxel half is not in the frame at all.
    gbase = ["--cloud-coverage", CLOUDSHADOW_COVERAGE, "--no-auto-exposure", "-E", "1.0"]
    g_on = os.path.join(workdir, "cloudshadow_ground_on.ppm")
    g_off = os.path.join(workdir, "cloudshadow_ground_off.ppm")
    err = render(scene, g_on, gbase, frames=CLOUDSHADOW_FRAMES) or \
        render(scene, g_off, gbase + ["--no-cloud-shadows"], frames=CLOUDSHADOW_FRAMES)
    if err:
        # Named and CONTINUED, not returned: the two map arms below are independent renders,
        # and a skipped arm that prints nothing reads as a pass.
        print(f"  cloudshadow-ground ERROR render failed: {err.strip()[-200:]}")
        print("  cloudshadow-dapple SKIP  (the ground render failed)")
        failures.append("cloudshadow-ground")
    else:
        g_mean, g_spread, g_mean_short = _cloudshadow_band(g_on, g_off, CLOUDSHADOW_GROUND)

        ok = g_mean <= CLOUDSHADOW_GROUND_DARKEN_MAX
        print(f"  cloudshadow-ground {'PASS' if ok else 'FAIL'}  shaded/lit={g_mean:.4f} "
              f"want <={CLOUDSHADOW_GROUND_DARKEN_MAX}")
        if not ok:
            failures.append("cloudshadow-ground")

        ok = g_spread >= CLOUDSHADOW_SPREAD_MIN
        print(f"  cloudshadow-dapple {'PASS' if ok else 'FAIL'}  shortfall spread/mean="
              f"{g_spread:.4f} want >={CLOUDSHADOW_SPREAD_MIN} (mean {g_mean_short:.4f})")
        if not ok:
            failures.append("cloudshadow-dapple")

    stats, err = _cloudshadow_map_stats(workdir, scene, "map", [])
    if err:
        print(f"  cloudshadow-map  ERROR render failed: {err}")
        return failures + ["cloudshadow-map"]
    peak, mean, sigma = stats
    ok = peak >= CLOUDSHADOW_MAP_MAX_MIN and sigma >= CLOUDSHADOW_MAP_SIGMA_MIN
    print(f"  cloudshadow-map  {'PASS' if ok else 'FAIL'}  peak={peak:.4f} "
          f"(want >={CLOUDSHADOW_MAP_MAX_MIN}) sigma={sigma:.4f} "
          f"(want >={CLOUDSHADOW_MAP_SIGMA_MIN}), mean={mean:.4f}")
    if not ok:
        failures.append("cloudshadow-map")

    # The same map at a LOW sun, which is a different question and needs its own arm.
    stats, err = _cloudshadow_map_stats(workdir, scene, "map_lowsun",
                                        ["--sun-elevation", CLOUDSHADOW_LOWSUN_ELEV])
    if err:
        print(f"  cloudshadow-lowsun ERROR render failed: {err}")
        return failures + ["cloudshadow-lowsun"]
    peak, mean, sigma = stats
    ok = sigma >= CLOUDSHADOW_LOWSUN_SIGMA_MIN and mean <= CLOUDSHADOW_LOWSUN_MEAN_MAX
    print(f"  cloudshadow-lowsun {'PASS' if ok else 'FAIL'}  sigma={sigma:.4f} "
          f"(want >={CLOUDSHADOW_LOWSUN_SIGMA_MIN}) mean={mean:.4f} "
          f"(want <={CLOUDSHADOW_LOWSUN_MEAN_MAX}), peak={peak:.4f}")
    if not ok:
        failures.append("cloudshadow-lowsun")

    return failures


# --- stars (spec 11.79): the procedural night star field --------------------------------
#
# All on aerial_fixture, the suite's sky fixture -- NOT tree, which is documented at 31k
# px run-to-run. Exposure pinned on every arm. Every quantitative read is the star
# CONTRIBUTION -- the delta against a --no-stars twin at the same configuration -- never
# raw brightness: the twilight sky's own radiance dominates a raw read and moves the
# OPPOSITE way as the sun drops (the beach-shoreline lesson, where an argmax read dry
# sand for four specs).
STARS_FIXTURE = "aerial_fixture.gltf"
STARS_PIN = ["--no-auto-exposure", "-E", "1.0"]
# The upper sky. The fixture's terrain silhouette cuts the sky off around 0.55 of frame
# height at this camera; box fractions are vertical-fov fractions, so they hold at any
# width the suite renders.
STARS_BAND = (0.05, 0.04, 0.95, 0.42)
# LOW vs HIGH view elevation for the extinction arm. The committed frame spans about
# -12..+28 degrees (eye (0,400,500) -> target (0,3211,-19500) at fov 40), so there is no
# zenith to read: low sits just above the silhouette (~+6..9 deg, airmass ~8), high at
# the top of frame (~+23..27 deg, airmass ~2.3), which still separates cleanly.
STARS_LOW_BAND = (0.05, 0.47, 0.95, 0.54)
STARS_HIGH_BAND = (0.05, 0.02, 0.95, 0.12)
# The emerge ladder. +5 is above the ramp's top edge (+3), so its contribution is an
# exact zero and the ladder's first step doubles as the ramp-zeroing read. Four other
# arms hang off the last rung's pair, so the night elevation is DEFINED as that rung
# rather than restated beside it.
STARS_EMERGE_ELEVS = ["5", "0", "-6", "-12"]
STARS_NIGHT_ELEV = STARS_EMERGE_ELEVS[-1]
STARS_DAY_ELEV = "35"
# Bars, measured on the shipping field at 400x300. Wide floors: the direction is the
# claim. Night, emerge and clouds sit an order of magnitude past their bars; horizon's
# margin is ~2x and CANNOT be wider, since the airmass arithmetic itself predicts ~0.39
# against the 0.75 bar. Measured: night 163,822 px; emerge 0 / 0.00144 / 0.00977 /
# 0.01127; horizon luma 0.355 with R/B 2.245 against 1.256; deck/clear 0.017 -- the
# default 0.45-coverage deck is nearly opaque at night, and the after-the-multiply
# mutation this arm exists for reads ~1.0.
STARS_NIGHT_MIN_PX = 2000
STARS_EMERGE_FLOOR = 5e-4
STARS_HORIZON_LUMA_MAX = 0.75
STARS_HORIZON_RB_MIN = 1.05
STARS_CLOUDS_MAX = 0.85
STARS_CSCN_MIN_PX = 2000


def _stars_delta(on_path, off_path, box):
    """Mean per-channel star contribution over a fractional box, plus moved pixels.

    Linear-decoded on-minus-off, so the sky under the stars and the deterministic
    dither pattern cancel exactly and what remains is what the stars added.
    """
    w, h, on = _read_ppm(on_path)
    _, _, off = _read_ppm(off_path)
    x0, x1 = int(box[0] * w), int(box[2] * w)
    y0, y1 = int(box[1] * h), int(box[3] * h)
    n = 0
    moved = 0
    acc = [0.0, 0.0, 0.0]
    for y in range(y0, y1):
        row = y * w
        for x in range(x0, x1):
            o = (row + x) * 3
            hit = False
            for k in range(3):
                acc[k] += _SRGB_TO_LINEAR[on[o + k]] - _SRGB_TO_LINEAR[off[o + k]]
                hit = hit or on[o + k] != off[o + k]
            if hit:
                moved += 1
            n += 1
    return [c / max(n, 1) for c in acc], moved


def _stars_pair(workdir, tag, elev, extra=None, frames=30):
    """Render the stars-on / stars-off twin at one elevation; returns (on, off, err)."""
    scene = os.path.join(ROOT, "assets", STARS_FIXTURE)
    on = os.path.join(workdir, f"stars_{tag}_on.ppm")
    off = os.path.join(workdir, f"stars_{tag}_off.ppm")
    common = ["--sun-elevation", elev] + STARS_PIN + (extra or [])
    err = render(scene, on, ["--stars"] + common, frames=frames) or \
        render(scene, off, ["--no-stars"] + common, frames=frames)
    return on, off, err


def run_stars_gate(workdir):
    """The procedural night star field (spec 11.79).

    Six arms, every read a delta against a --no-stars twin at the same configuration:

      stars-night     at -12 the field moves real pixels in the sky band.
      stars-daylight  at +35 stars on vs off is 0 px over the WHOLE frame. Both legs
                      upload starIntensity 0 by design (ramp at zero vs disabled), so
                      this is the guard that the ramp actually zeroes -- deleting or
                      mis-ranging the elevation ramp is what it catches.
      stars-emerge    the contribution at +5 / 0 / -6 / -12 is strictly increasing,
                      with +5 an exact zero (above the ramp's top edge). A constant
                      starIntensity fails the ordering.
      stars-horizon   the low-sky contribution is dimmer AND redder than the high-sky
                      one -- the transmittance term. Dropping transmittanceToSky from
                      the star term is what it catches.
      stars-clouds    with --clouds at -12 the sky-band contribution shrinks: the deck
                      occludes stars because the term sits under the cloud.a multiply.
                      Moving it after that multiply is what it catches. Whole band, not
                      deck-vs-gap boxes: the deck drifts with wind.
      stars-det       two runs of the -12 leg are 0 px.
      stars-cscn      the authored environment.stars block IS the flag path: a variant
                      authoring enabled+brightness renders 0 px from its flag twin (the
                      ies-flag idiom), a variant rotating the cscn-ONLY latitude and
                      hour moves real pixels (those two fields have no CLI flag, so
                      nothing else can prove their plumbing is alive), and --no-stars
                      beats an authoring file at 0 px. Deleting the parse is what the
                      first reading catches.
    """
    scene = os.path.join(ROOT, "assets", STARS_FIXTURE)
    if not os.path.exists(scene):
        print(f"  stars-night SKIP  ({STARS_FIXTURE} not present)")
        return []
    failures = []
    deltas = {}
    night_on = night_off = None
    for elev in STARS_EMERGE_ELEVS:
        on, off, err = _stars_pair(workdir, f"e{elev}", elev)
        if err:
            print(f"  stars-night ERROR render failed: {err.strip()[-200:]}")
            return ["stars-night"]
        deltas[elev] = _stars_delta(on, off, STARS_BAND)
        if elev == STARS_NIGHT_ELEV:
            night_on, night_off = on, off

    rgb, moved = deltas[STARS_NIGHT_ELEV]
    ok = moved >= STARS_NIGHT_MIN_PX
    print(f"  stars-night  {'PASS' if ok else 'FAIL'}  {moved} px moved in the sky band "
          f"at {STARS_NIGHT_ELEV} (want >={STARS_NIGHT_MIN_PX})")
    if not ok:
        failures.append("stars-night")

    d_on, d_off, err = _stars_pair(workdir, "day", STARS_DAY_ELEV)
    if err:
        print(f"  stars-daylight ERROR render failed: {err.strip()[-200:]}")
        failures.append("stars-daylight")
    else:
        day_moved, _ = compare(d_on, d_off)
        ok = day_moved == 0
        print(f"  stars-daylight {'PASS' if ok else 'FAIL'}  {day_moved} px at "
              f"+{STARS_DAY_ELEV} (want 0: the ramp must zero exactly)")
        if not ok:
            failures.append("stars-daylight")

    lumas = [sum(deltas[e][0]) / 3.0 for e in STARS_EMERGE_ELEVS]
    _, top_rung_moved = deltas[STARS_EMERGE_ELEVS[0]]
    steps = " -> ".join(f"{v:.6f}" for v in lumas)
    ok = (top_rung_moved == 0 and
          all(lumas[i] < lumas[i + 1] for i in range(len(lumas) - 1)) and
          lumas[-1] >= STARS_EMERGE_FLOOR)
    print(f"  stars-emerge {'PASS' if ok else 'FAIL'}  band contribution {steps} "
          f"(want strictly increasing, +{STARS_EMERGE_ELEVS[0]} exactly 0, "
          f"final >={STARS_EMERGE_FLOOR})")
    if not ok:
        failures.append("stars-emerge")

    lo, _ = _stars_delta(night_on, night_off, STARS_LOW_BAND)
    hi, _ = _stars_delta(night_on, night_off, STARS_HIGH_BAND)
    lo_l, hi_l = sum(lo) / 3.0, sum(hi) / 3.0
    lo_rb = lo[0] / max(lo[2], 1e-9)
    hi_rb = hi[0] / max(hi[2], 1e-9)
    ok = lo_l <= hi_l * STARS_HORIZON_LUMA_MAX and lo_rb >= hi_rb * STARS_HORIZON_RB_MIN
    print(f"  stars-horizon {'PASS' if ok else 'FAIL'}  low/high luma "
          f"{lo_l / max(hi_l, 1e-9):.3f} (want <={STARS_HORIZON_LUMA_MAX}), "
          f"low R/B {lo_rb:.3f} vs high {hi_rb:.3f} "
          f"(want >={STARS_HORIZON_RB_MIN}x)")
    if not ok:
        failures.append("stars-horizon")

    c_on, c_off, err = _stars_pair(workdir, "cloud", STARS_NIGHT_ELEV,
                                   extra=["--clouds"], frames=CLOUDSHADOW_FRAMES)
    if err:
        print(f"  stars-clouds ERROR render failed: {err.strip()[-200:]}")
        failures.append("stars-clouds")
    else:
        cloud_rgb, _ = _stars_delta(c_on, c_off, STARS_BAND)
        cloud_l = sum(cloud_rgb) / 3.0
        clear_l = sum(rgb) / 3.0
        ratio = cloud_l / max(clear_l, 1e-9)
        ok = ratio <= STARS_CLOUDS_MAX
        print(f"  stars-clouds {'PASS' if ok else 'FAIL'}  deck/clear contribution "
              f"{ratio:.3f} (want <={STARS_CLOUDS_MAX})")
        if not ok:
            failures.append("stars-clouds")

    det = os.path.join(workdir, "stars_det.ppm")
    err = render(scene, det, ["--stars", "--sun-elevation", STARS_NIGHT_ELEV] + STARS_PIN)
    if err:
        print(f"  stars-det   ERROR render failed: {err.strip()[-200:]}")
        failures.append("stars-det")
    else:
        det_moved, _ = compare(night_on, det)
        ok = det_moved == 0
        print(f"  stars-det    {'PASS' if ok else 'FAIL'}  {det_moved} px between two "
              f"runs (want 0)")
        if not ok:
            failures.append("stars-det")

    # The authored scene key, held to the flag path by generated twins (the ies-flag
    # idiom). The authored variant pins the library-default latitude/hour so the flag
    # twin CAN match it; the moved variant then rotates exactly the two fields no flag
    # reaches.
    cscn_src = os.path.join(ROOT, "assets", "aerial_fixture.cscn")
    authored = os.path.join(workdir, "stars_cscn_authored.cscn")
    rotated = os.path.join(workdir, "stars_cscn_rotated.cscn")

    def _stars_author(hour, lat):
        def mutate(d):
            d["environment"]["sun"] = {"elevation": -12.0, "azimuth": 40.0}
            d["environment"]["stars"] = {"enabled": True, "brightness": 1.5,
                                         "latitude": lat, "hour_angle": hour}
        return mutate

    cscn_copy(cscn_src, authored, _stars_author(0.0, 45.0))
    cscn_copy(cscn_src, rotated, _stars_author(120.0, 10.0))
    a_frame = os.path.join(workdir, "stars_cscn_a.ppm")
    b_frame = os.path.join(workdir, "stars_cscn_b.ppm")
    c_frame = os.path.join(workdir, "stars_cscn_c.ppm")
    off_frame = os.path.join(workdir, "stars_cscn_off.ppm")
    err = render(authored, a_frame, STARS_PIN) or \
        render(scene, b_frame,
               ["--stars", "--stars-brightness", "1.5", "--sun-elevation",
                STARS_NIGHT_ELEV] + STARS_PIN) or \
        render(rotated, c_frame, STARS_PIN) or \
        render(authored, off_frame, ["--no-stars"] + STARS_PIN)
    if err:
        print(f"  stars-cscn  ERROR render failed: {err.strip()[-200:]}")
        return failures + ["stars-cscn"]
    flag_px, _ = compare(a_frame, b_frame)
    rot_px, _ = compare(a_frame, c_frame)
    cli_px, _ = compare(off_frame, night_off)
    ok = flag_px == 0 and rot_px >= STARS_CSCN_MIN_PX and cli_px == 0
    print(f"  stars-cscn   {'PASS' if ok else 'FAIL'}  authored vs flags {flag_px} px "
          f"(want 0), latitude+hour rotated {rot_px} px (want >={STARS_CSCN_MIN_PX}: "
          f"no flag reaches those two), --no-stars over the file {cli_px} px (want 0)")
    if not ok:
        failures.append("stars-cscn")

    return failures


# --- night-floor (spec 11.80): the sky between the stars, lighting the world ------------
#
# Same fixture, same pinning and the same twin-delta doctrine as the stars group. The
# ground band sits over the fixture's terrain silhouette, below the stars' sky band: what
# lifts there at night can only have arrived through the env cube and the IBL, which is
# the path this feature exists for and the one no stars arm touches.
NIGHTFLOOR_GROUND_BAND = (0.05, 0.62, 0.95, 0.90)
# Bars, calibrated on the shipping floor at 400x300 after the first run.
NIGHTFLOOR_SKY_MIN = 1e-3
NIGHTFLOOR_GROUND_MIN = 2e-4
# The fog arm is a RATIO of ground-band deltas (with fog over without), because a plain
# with-fog read is vacuous against its own mutation: the LUT and IBL halves lift either
# way, and only the fog ambient -- the CPU zenith march's output -- can push the with-fog
# delta past the no-fog one. The gap is narrow and DETERMINISTIC, and the bar sits at its
# midpoint: measured 1.043 live against 0.995 with the zenith term deleted (the fog's own
# attenuation of the IBL lift is what pulls the dead reading under 1).
NIGHTFLOOR_FOG_RATIO_MIN = 1.02


def _nightfloor_pair(workdir, tag, extra, frames=30):
    """Render the floor-on / floor-off twin; returns (on, off, err)."""
    scene = os.path.join(ROOT, "assets", STARS_FIXTURE)
    on = os.path.join(workdir, f"nfloor_{tag}_on.ppm")
    off = os.path.join(workdir, f"nfloor_{tag}_off.ppm")
    err = render(scene, on, ["--night-floor"] + extra, frames=frames) or \
        render(scene, off, ["--no-night-floor"] + extra, frames=frames)
    return on, off, err


def run_nightfloor_gate(workdir):
    """The night-sky floor (spec 11.80), every read a delta against a --no-night-floor twin:

      nightfloor-sky      at -12 the sky band lifts: the LUT carries the floor.
      nightfloor-ground   at -12 the TERRAIN band lifts too -- the env->IBL path, which is
                          the feature's entire reason to exist. A floor implemented as a
                          background-shader term lifts the sky and never the ground, and
                          this is the arm that refuses that wrong-layer implementation.
      nightfloor-fog      the ground-band delta WITH --fog exceeds the no-fog delta by a
                          real ratio. Only the fog ambient (the CPU zenith march's floor
                          term) can supply the excess; a plain with-fog read would pass
                          over a dead C twin, which is this arm's named mutation.
      nightfloor-daylight at +35 on vs off is 0 px: the shared civil-twilight ramp zeroes
                          and the LUT is bit-identical.
      nightfloor-cscn     the authored environment.night_floor block IS the flag path
                          (0 px from its flag twin), and --no-night-floor beats the
                          authoring file at 0 px. Deleting the parse is what it catches.
    """
    scene = os.path.join(ROOT, "assets", STARS_FIXTURE)
    if not os.path.exists(scene):
        print(f"  nightfloor-sky SKIP  ({STARS_FIXTURE} not present)")
        return []
    failures = []

    night = ["--sun-elevation", STARS_NIGHT_ELEV] + STARS_PIN
    on, off, err = _nightfloor_pair(workdir, "night", night)
    if err:
        print(f"  nightfloor-sky ERROR render failed: {err.strip()[-200:]}")
        return ["nightfloor-sky"]
    sky_rgb, _ = _stars_delta(on, off, STARS_BAND)
    ground_rgb, _ = _stars_delta(on, off, NIGHTFLOOR_GROUND_BAND)
    sky_l = sum(sky_rgb) / 3.0
    ground_l = sum(ground_rgb) / 3.0

    ok = sky_l >= NIGHTFLOOR_SKY_MIN
    print(f"  nightfloor-sky {'PASS' if ok else 'FAIL'}  sky-band lift {sky_l:.6f} "
          f"(want >={NIGHTFLOOR_SKY_MIN})")
    if not ok:
        failures.append("nightfloor-sky")

    ok = ground_l >= NIGHTFLOOR_GROUND_MIN
    print(f"  nightfloor-ground {'PASS' if ok else 'FAIL'}  terrain-band lift {ground_l:.6f} "
          f"(want >={NIGHTFLOOR_GROUND_MIN}: only the env->IBL path can put it there)")
    if not ok:
        failures.append("nightfloor-ground")

    f_on, f_off, err = _nightfloor_pair(workdir, "fog", night + ["--fog"],
                                        frames=CLOUDSHADOW_FRAMES)
    if err:
        print(f"  nightfloor-fog ERROR render failed: {err.strip()[-200:]}")
        failures.append("nightfloor-fog")
    else:
        fog_rgb, _ = _stars_delta(f_on, f_off, NIGHTFLOOR_GROUND_BAND)
        fog_l = sum(fog_rgb) / 3.0
        ratio = fog_l / max(ground_l, 1e-9)
        ok = ratio >= NIGHTFLOOR_FOG_RATIO_MIN
        print(f"  nightfloor-fog {'PASS' if ok else 'FAIL'}  with-fog/no-fog ground lift "
              f"{ratio:.3f} (want >={NIGHTFLOOR_FOG_RATIO_MIN}: the excess is the zenith "
              f"march's floor term)")
        if not ok:
            failures.append("nightfloor-fog")

    d_on, d_off, err = _nightfloor_pair(workdir, "day",
                                        ["--sun-elevation", STARS_DAY_ELEV] + STARS_PIN)
    if err:
        print(f"  nightfloor-daylight ERROR render failed: {err.strip()[-200:]}")
        failures.append("nightfloor-daylight")
    else:
        day_moved, _ = compare(d_on, d_off)
        ok = day_moved == 0
        print(f"  nightfloor-daylight {'PASS' if ok else 'FAIL'}  {day_moved} px at "
              f"+{STARS_DAY_ELEV} (want 0: the shared ramp must zero exactly)")
        if not ok:
            failures.append("nightfloor-daylight")

    cscn_src = os.path.join(ROOT, "assets", "aerial_fixture.cscn")
    authored = os.path.join(workdir, "nfloor_cscn.cscn")

    def _floor_author(d):
        d["environment"]["sun"] = {"elevation": -12.0, "azimuth": 40.0}
        d["environment"]["night_floor"] = {"enabled": True, "brightness": 2.0}

    cscn_copy(cscn_src, authored, _floor_author)
    a_frame = os.path.join(workdir, "nfloor_cscn_a.ppm")
    b_frame = os.path.join(workdir, "nfloor_cscn_b.ppm")
    off_frame = os.path.join(workdir, "nfloor_cscn_off.ppm")
    err = render(authored, a_frame, STARS_PIN) or \
        render(scene, b_frame,
               ["--night-floor", "--night-floor-brightness", "2.0", "--sun-elevation",
                STARS_NIGHT_ELEV] + STARS_PIN) or \
        render(authored, off_frame, ["--no-night-floor"] + STARS_PIN)
    if err:
        print(f"  nightfloor-cscn ERROR render failed: {err.strip()[-200:]}")
        return failures + ["nightfloor-cscn"]
    flag_px, _ = compare(a_frame, b_frame)
    cli_px, _ = compare(off_frame, off)
    ok = flag_px == 0 and cli_px == 0
    print(f"  nightfloor-cscn {'PASS' if ok else 'FAIL'}  authored vs flags {flag_px} px "
          f"(want 0), --no-night-floor over the file {cli_px} px (want 0)")
    if not ok:
        failures.append("nightfloor-cscn")

    return failures


# --- cycle (spec 11.81): the day/night clock and its time-sliced env re-bake ------------
#
# Same fixture and pinning as the stars and floor groups. The latitude is the sky's own
# default, which is what the twin below reproduces.
CYCLE_LATITUDE = 45.0
CYCLE_QUIESCE_HOUR = 7.25
# Bars measured on the shipping schedule; the two 0 px arms are exact by construction.
CYCLE_MOVES_MIN_PX = 20000
CYCLE_ORDER_MIN = 1.15


def _cycle_sun_path(latitude_deg, hour):
    """The Python twin of sky_sun_path (sky.c), in doubles.

    Equinox path: the sun rides the celestial equator of `latitude_deg`, so noon altitude
    is 90-lat and sunrise/sunset land at 6 and 18. Written out rather than imported
    because there is nothing to import -- and that is the point: the C is the thing under
    test, and a twin that shared its arithmetic would asserts nothing.

    DOUBLES throughout, matching the C, and printed at repr precision by the caller: the
    quiesce arm demands the path-derived sun and the flag-passed sun land on the same
    float, which a float-domain twin would not deliver.
    """
    h = (hour - 12.0) * (math.pi / 12.0)
    lat = latitude_deg * (math.pi / 180.0)
    x = -math.sin(h)
    y = math.cos(lat) * math.cos(h)
    z = -math.sin(lat) * math.cos(h)
    el = math.asin(max(-1.0, min(1.0, y)))
    az = math.atan2(x, z)
    if az < 0.0:
        az += 2.0 * math.pi
    return el * 180.0 / math.pi, az * 180.0 / math.pi


def run_cycle_gate(workdir):
    """The day/night cycle (spec 11.81): one clock, and a bake spread across frames.

      cycle-quiesce  a FROZEN cycle at hour H renders 0 px from explicit --sun-elevation
                     /--sun-azimuth taken from this file's own twin of sky_sun_path. Pins
                     the path formula AND proves an armed-but-frozen clock touches
                     nothing -- which is also what makes --day-cycle 0 a no-op rather
                     than a sun teleport, the defect the review caught here.
      cycle-slice    --cycle-rebake-at drives ONE sliced re-bake over an unmoved sun; the
                     frame reads 0 px against the same run without it. The acceptance bar
                     the roadmap row named: it says the slicer IS the atomic bake, byte
                     for byte, with no hour arithmetic anywhere in the comparison. A
                     dropped face, a skipped mip, a swap that forgets max_reflection_lod
                     or any divergence between the two drivers lands here.
      cycle-moves    under a running clock the frame changes a lot, and sky brightness
                     ORDERS across a sunset (day > dusk > night). A dead driver or a
                     clock that does not advance fails.
      cycle-det      two identical runs of a moving cycle are 0 px -- the clock rides the
                     frame clock, not the wall.
      cycle-cscn     the authored environment.cycle block IS the flag path (0 px), and
                     --no-day-cycle beats a file that asked for one. The stars-cscn and
                     nightfloor-cscn idiom; without it the whole scene-key path is
                     unexercised.
      cycle-config   a dumped session restores to the same frame. The ONLY cover for the
                     three cycle rows and for CFG_DOUBLE's write/decode/store:
                     config-roundtrip is blind to a dropped store (the default lands in
                     both dumps) and config-perturb now excuses these rows by name.

    The GUI's disabled-slider guards are deliberately NOT covered -- no headless arm can
    reach them, and saying so beats leaving them looking tested.
    """
    scene = os.path.join(ROOT, "assets", STARS_FIXTURE)
    if not os.path.exists(scene):
        print(f"  cycle-quiesce SKIP  ({STARS_FIXTURE} not present)")
        return []
    failures = []

    el, az = _cycle_sun_path(CYCLE_LATITUDE, CYCLE_QUIESCE_HOUR)
    frozen = os.path.join(workdir, "cycle_frozen.ppm")
    static = os.path.join(workdir, "cycle_static.ppm")
    err = render(scene, frozen,
                 ["--day-cycle", "0", "--time-of-day", repr(CYCLE_QUIESCE_HOUR)] + STARS_PIN) or \
        render(scene, static,
               ["--sun-elevation", repr(el), "--sun-azimuth", repr(az)] + STARS_PIN)
    if err:
        print(f"  cycle-quiesce ERROR render failed: {err.strip()[-200:]}")
        return ["cycle-quiesce"]
    px, _ = compare(frozen, static)
    ok = px == 0
    print(f"  cycle-quiesce {'PASS' if ok else 'FAIL'}  frozen cycle vs the twin's angles "
          f"{px} px (want 0; el={el:.6f} az={az:.6f})")
    if not ok:
        failures.append("cycle-quiesce")

    # The slicer against the atomic bake, sun held still. Frames well past the
    # rebake so the schedule has converged and swapped.
    #
    # TWO scenes, and the second is not thoroughness. This fixture has no
    # sheen, so nothing in its frame reads the Charlie prefilter chain --
    # deleting that chain from the slicer entirely leaves it 0 px, measured.
    # sheen_fixture is what makes the arm cover all four handles the swap
    # moves; without it a third of the schedule is asserted by nothing.
    base = ["--sun-elevation", "12", "--sun-azimuth", "40"] + STARS_PIN
    slice_scenes = [("sky", scene, base),
                    ("sheen", os.path.join(ROOT, "assets", "sheen_fixture.gltf"),
                     ["--sky"] + base)]
    for tag, path, flags in slice_scenes:
        if not os.path.exists(path):
            print(f"  cycle-slice SKIP  ({os.path.basename(path)} not present)")
            break
        sliced = os.path.join(workdir, f"cycle_sliced_{tag}.ppm")
        plain = os.path.join(workdir, f"cycle_plain_{tag}.ppm")
        err = render(path, sliced, flags + ["--cycle-rebake-at", "2"], frames=60) or \
            render(path, plain, flags, frames=60)
        if err:
            print(f"  cycle-slice ERROR render failed: {err.strip()[-200:]}")
            failures.append("cycle-slice")
            break
        px, _ = compare(sliced, plain)
        ok = px == 0
        print(f"  cycle-slice {'PASS' if ok else 'FAIL'}  {tag}: sliced re-bake vs atomic "
              f"{px} px (want 0: the slicer IS the atomic bake)")
        if not ok:
            failures.append("cycle-slice")
            break

    # A moving clock: three captures across a sunset from one run.
    moving = os.path.join(workdir, "cycle_move.ppm")
    err = render(scene, moving,
                 ["--day-cycle", "6", "--time-of-day", "10"] + STARS_PIN +
                 ["--screenshot-every", "30"], frames=90)
    if err:
        print(f"  cycle-moves ERROR render failed: {err.strip()[-200:]}")
        failures.append("cycle-moves")
    else:
        shots = [moving.replace(".ppm", f"_{n:06d}.ppm") for n in (30, 60, 90)]
        if not all(os.path.exists(s) for s in shots):
            print("  cycle-moves FAIL  the schedule wrote no intermediate frames")
            failures.append("cycle-moves")
        else:
            lum = []
            for s in shots:
                w, h, pix = _read_ppm(s)
                acc, n = 0.0, 0
                x0, x1 = int(0.05 * w), int(0.95 * w)
                y0, y1 = int(0.04 * h), int(0.42 * h)
                for y in range(y0, y1, 3):
                    for x in range(x0, x1, 3):
                        o = (y * w + x) * 3
                        acc += sum(_SRGB_TO_LINEAR[pix[o + k]] for k in range(3)) / 3.0
                        n += 1
                lum.append(acc / max(n, 1))
            moved, _ = compare(shots[0], shots[-1])
            falling = all(lum[i] > lum[i + 1] * CYCLE_ORDER_MIN for i in range(len(lum) - 1))
            ok = moved >= CYCLE_MOVES_MIN_PX and falling
            steps = " -> ".join(f"{v:.5f}" for v in lum)
            print(f"  cycle-moves {'PASS' if ok else 'FAIL'}  sky {steps} "
                  f"(want each >={CYCLE_ORDER_MIN}x the next), {moved} px first-to-last "
                  f"(want >={CYCLE_MOVES_MIN_PX})")
            if not ok:
                failures.append("cycle-moves")

    det = os.path.join(workdir, "cycle_det.ppm")
    err = render(scene, det, ["--day-cycle", "6", "--time-of-day", "10"] + STARS_PIN,
                 frames=90)
    if err:
        print(f"  cycle-det   ERROR render failed: {err.strip()[-200:]}")
        failures.append("cycle-det")
    else:
        px, _ = compare(det, moving)
        ok = px == 0
        print(f"  cycle-det    {'PASS' if ok else 'FAIL'}  {px} px between two moving runs "
              f"(want 0)")
        if not ok:
            failures.append("cycle-det")

    # The authored block against its flag twin, and the way off it.
    cscn_src = os.path.join(ROOT, "assets", "aerial_fixture.cscn")
    authored = os.path.join(workdir, "cycle_cscn.cscn")

    def _cycle_author(d):
        d["environment"]["cycle"] = {"enabled": True, "day_seconds": 0.0,
                                     "hour": CYCLE_QUIESCE_HOUR}

    cscn_copy(cscn_src, authored, _cycle_author)
    a_frame = os.path.join(workdir, "cycle_cscn_a.ppm")
    off_frame = os.path.join(workdir, "cycle_cscn_off.ppm")
    plain_sun = os.path.join(workdir, "cycle_cscn_plain.ppm")
    err = render(authored, a_frame, STARS_PIN) or \
        render(authored, off_frame, ["--no-day-cycle"] + STARS_PIN) or \
        render(cscn_src, plain_sun, STARS_PIN)
    if err:
        print(f"  cycle-cscn  ERROR render failed: {err.strip()[-200:]}")
        failures.append("cycle-cscn")
    else:
        flag_px, _ = compare(a_frame, frozen)
        off_px, _ = compare(off_frame, plain_sun)
        ok = flag_px == 0 and off_px == 0
        print(f"  cycle-cscn   {'PASS' if ok else 'FAIL'}  authored vs flags {flag_px} px "
              f"(want 0), --no-day-cycle back to the scene's own sun {off_px} px (want 0)")
        if not ok:
            failures.append("cycle-cscn")

    # The three rows and CFG_DOUBLE's whole pipeline, which nothing else covers.
    dump = os.path.join(workdir, "cycle_config.json")
    restored = os.path.join(workdir, "cycle_restored.ppm")
    err = render(scene, os.path.join(workdir, "cycle_dump.ppm"),
                 ["--day-cycle", "0", "--time-of-day", repr(CYCLE_QUIESCE_HOUR),
                  "--config-dump", dump] + STARS_PIN)
    if err or not os.path.exists(dump):
        print(f"  cycle-config ERROR dump failed: {(err or 'no file').strip()[-200:]}")
        failures.append("cycle-config")
    else:
        err = render(scene, restored, ["--config", dump] + STARS_PIN)
        if err:
            print(f"  cycle-config ERROR restore failed: {err.strip()[-200:]}")
            failures.append("cycle-config")
        else:
            px, _ = compare(restored, frozen)
            with open(dump) as fh:
                stored = json.load(fh).get("sky", {})
            hour_ok = abs(stored.get("cycle_hour", -1) - CYCLE_QUIESCE_HOUR) < 1e-9
            ok = px == 0 and hour_ok and stored.get("cycle") is True
            print(f"  cycle-config {'PASS' if ok else 'FAIL'}  restored {px} px (want 0), "
                  f"cycle_hour {stored.get('cycle_hour')} round-tripped={hour_ok}")
            if not ok:
                failures.append("cycle-config")

    return failures


# --- the moon (spec 11.82): an analytic disc, a derived phase, a second casting light ---
#
# Same fixture, same pinning and the same twin-delta doctrine as the stars, floor and
# cycle groups above -- every quantitative PIXEL read is the moon CONTRIBUTION, the
# difference against a --no-moon twin at the same configuration, never a raw brightness.
# The one raw read is moon-probe's, and that is arithmetic rather than radiance.
#
# STARS_FIXTURE, STARS_PIN, STARS_BAND and NIGHTFLOOR_GROUND_BAND are reused rather than
# restated: four night groups share one fixture and one pair of bands, and a second copy
# is a second thing to drift.
#
# WHY THE MOON SITS WHERE IT DOES, which is geometry and not taste. With the moon at
# elevation E and the sun rolled about the moon vector at elongation e, the sun's
# elevation on the worst roll is asin(sin(e + E)) -- so the sun CLEARS THE HORIZON unless
# e > 180 - E. moon-terminator needs four rolls, hence a high moon; moon-lit needs a
# CRESCENT rung (e = 45), which forces the moon low. Two placements, each derived.
MOON_EL_HIGH = 78.0
MOON_EL_LOW = 15.0
# A THIRD placement, for moon-env alone, and it is derived rather than chosen: that arm
# needs a FULL moon at a 40-degree diagnostic disc, a full moon puts the sun at its exact
# antipode, and the sun's own disc must clear the horizon -- so the moon has to stand
# above the disc's 20-degree half-angle with margin.
MOON_EL_ENV = 30.0
MOON_AZ = 200.0
MOON_ELONG_ROLL = 115.0
MOON_ROLLS = [0.0, 90.0, 180.0, 270.0]
# ELONGATION, not phase angle: 180 is FULL. Predicted lit fractions 1.000 / 0.854 /
# 0.500 / 0.146 and Krisciunas-Schaefer factors 1.0000 / 0.3352 / 0.0910 / 0.0116. The
# ladder straddles 90 deliberately -- that is the ONE point where the elongation and the
# phase angle agree, so a reader that confuses them is invisible there and nowhere else.
MOON_ELONGS = [180.0, 135.0, 90.0, 45.0]
# The disc is 3.8 px in RADIUS at its real 0.53 degrees on a 400x300 frame, which is why
# every arm that reads its INTERIOR blows it up. That is legitimate rather than
# convenient: the phase construction is a function of position on the unit-sphere face,
# so angular radius SCALES the picture and cannot rotate it. The shipping 0.53 is pinned
# exactly and separately, by moon-config reading the dumped row.
MOON_DISC_BIG = "12"
MOON_DISC_HUGE = "40"
MOON_DISC_SHIPPING = "0.53"
# --no-bloom on every disc arm is load-bearing rather than hygiene: the bloom pyramid
# spreads the disc's energy across the whole frame, so an arm asserting the disc is
# confined to its own solid angle would be reading the blur kernel. --no-dither for the
# neighbouring reason -- the maria read is a small-signal sigma where +/-1 LSB is large.
MOON_DISC_FLAGS = STARS_PIN + ["--no-bloom", "--no-dither"]

MOON_PROBE_TOL = 1e-4        # float32 C against doubles here, and acos is ill-conditioned at +-1
MOON_LIT_TOL = 0.12          # measured worst 0.043 across the four rungs
# 25 degrees, and the width is the MARIA rather than slack. The centroid is weighted by
# the contribution, the maria are a fixed pattern on a face the lit region slides across,
# so each roll's centroid is pulled a little off the pure sunward axis by whichever
# patches it happens to cover. Measured worst 0.9577 (16.7 degrees) at the shipping
# contrast. It still refuses every mutation this arm exists for by a wide margin: a
# mirrored disc reads 180 degrees, a transposed basis 90, and a constant screen direction
# is 90 or 180 off on three of the four rolls.
MOON_LIMB_COS_MIN = 0.906
MOON_LIMB_OFFSET_MIN = 0.10  # of the disc radius; measured worst 0.30
MOON_KS_RATIO_MIN = 5.0      # full/quarter, against a predicted 10.99 through the tonemap


def _moon_dir(el_deg, az_deg):
    """The twin of sky_update_moon: elevation/azimuth -> unit vector, azimuth 0 = +Z."""
    el, az = math.radians(el_deg), math.radians(az_deg)
    return (math.cos(el) * math.sin(az), math.sin(el), math.cos(el) * math.cos(az))


def _moon_phase(m_el, m_az, s_el, s_az):
    """(elongation, phase angle, lit fraction, Krisciunas-Schaefer factor), all doubles.

    The phase angle is the SUPPLEMENT of the elongation -- it is measured at the moon, so
    0 is full and 180 is new. Confusing the two inverts the feature and is invisible at
    exactly 90, which is why MOON_ELONGS straddles it.
    """
    m, s = _moon_dir(m_el, m_az), _moon_dir(s_el, s_az)
    d = max(-1.0, min(1.0, sum(a * b for a, b in zip(m, s))))
    elong = math.degrees(math.acos(d))
    alpha = 180.0 - elong
    lit = 0.5 * (1.0 + math.cos(math.radians(alpha)))
    ks = 10.0 ** (-0.4 * (0.026 * alpha + 4e-9 * alpha ** 4))
    return elong, alpha, lit, ks


def _moon_roll_basis(m_el, m_az):
    """(m, e1, e2): the moon vector and two axes perpendicular to it, e1 being world up
    projected into that plane. e2 = m x e1 therefore has y EXACTLY zero, which is what
    makes the sun's elevation below a closed form of the roll alone.
    """
    m = _moon_dir(m_el, m_az)
    up = (0.0, 1.0, 0.0)
    d = sum(a * b for a, b in zip(up, m))
    e1 = [u - d * c for u, c in zip(up, m)]
    n = math.sqrt(sum(c * c for c in e1)) or 1.0
    e1 = [c / n for c in e1]
    e2 = [m[1] * e1[2] - m[2] * e1[1], m[2] * e1[0] - m[0] * e1[2],
          m[0] * e1[1] - m[1] * e1[0]]
    return m, e1, e2


def _moon_sun_at_elongation(m_el, m_az, elongation, roll=180.0):
    """The sun (elevation, azimuth) at `elongation` from the moon, on a chosen ROLL about
    the moon vector.

    Rolling rather than solving at a fixed sun elevation, because the two are not
    independent: a FULL moon (elongation 180) puts the sun at the moon's exact antipode
    and there is no azimuth left to choose. Roll 180 is the default because it is the
    roll that tilts AWAY from the zenith -- sun elevation there is exactly
    asin(sin(E - e)), the lowest the pair admits -- which is what keeps the sun's own
    disc out of frame under the group's standing constraint.
    """
    m, e1, e2 = _moon_roll_basis(m_el, m_az)
    ce, se = math.cos(math.radians(elongation)), math.sin(math.radians(elongation))
    cr, sr = math.cos(math.radians(roll)), math.sin(math.radians(roll))
    s = [ce * m[k] + se * (cr * e1[k] + sr * e2[k]) for k in range(3)]
    el = math.degrees(math.asin(max(-1.0, min(1.0, s[1]))))
    az = math.degrees(math.atan2(s[0], s[2])) % 360.0
    return el, az


def _moon_camera(el_deg, az_deg, eye=(0.0, 400.0, 500.0), reach=20000.0):
    """A camera dict + argv aimed straight down a sky direction, so the body lands at the
    exact frame centre. The dict feeds _projector; the argv feeds render().
    """
    d = _moon_dir(el_deg, az_deg)
    target = tuple(e + reach * c for e, c in zip(eye, d))
    cam = {"eye": eye, "target": target, "fovy_deg": 40.0}
    argv = ["--cam-eye", ",".join(f"{c:.6f}" for c in eye),
            "--cam-target", ",".join(f"{c:.6f}" for c in target)]
    return cam, argv


def _moon_render(workdir, tag, extra, frames=30, moon=True):
    """One leg of a moon twin. `moon` False renders the --no-moon reference.

    The switch goes LAST, and that is not style. --moon-elevation and --moon-azimuth both
    IMPLY --moon (the four-site idiom's "a value arms the feature"), so a --no-moon
    leading a configuration that also names an angle is re-armed by its own placement and
    the twin renders two identical frames. Every arm here passes angles, so every off leg
    would have been silently on.
    """
    scene = os.path.join(ROOT, "assets", STARS_FIXTURE)
    out = os.path.join(workdir, f"moon_{tag}.ppm")
    tail = ["--moon"] if moon else ["--no-moon"]
    err = render(scene, out, extra + tail, frames=frames)
    return out, err


def _moon_disc_field(on_path, off_path, cam, disc_deg, m_el, m_az):
    """Per-pixel linear twin-delta inside the predicted disc, plus what leaked outside it.

    Returns (radius_px, cx, cy, cells, peak, outside_moved) where cells is a list of
    (x, y, delta) for pixels inside the mask. Scans a box around the predicted disc
    rather than the frame: a few thousand iterations instead of a hundred thousand.
    """
    w, h, on = _read_ppm(on_path)
    _, _, off = _read_ppm(off_path)
    project = _projector(cam, w, h)
    eye = cam["eye"]
    d = _moon_dir(m_el, m_az)
    centre = tuple(e + 20000.0 * c for e, c in zip(eye, d))
    cx, cy = project(centre)
    # The disc's pixel radius from its angular one, through the same vertical fov the
    # projector uses -- derived rather than measured, so the arm is asserting the render
    # against geometry instead of against itself.
    radius = (h * 0.5) * math.tan(math.radians(disc_deg * 0.5)) / \
        math.tan(math.radians(cam["fovy_deg"]) * 0.5)
    cells, peak, outside = [], 0.0, 0
    pad = int(radius * 3.0) + 8
    for y in range(max(0, int(cy) - pad), min(h, int(cy) + pad + 1)):
        for x in range(max(0, int(cx) - pad), min(w, int(cx) + pad + 1)):
            o = (y * w + x) * 3
            delta = sum(_SRGB_TO_LINEAR[on[o + k]] - _SRGB_TO_LINEAR[off[o + k]]
                        for k in range(3)) / 3.0
            r = math.hypot(x - cx, y - cy)
            if r <= radius:
                cells.append((x, y, delta))
                peak = max(peak, delta)
            elif r > radius + 4.0 and any(on[o + k] != off[o + k] for k in range(3)):
                outside += 1
    return radius, cx, cy, cells, peak, outside


def _moon_probe(scene, m_el, m_az, s_el, s_az):
    """Run --moon-probe at one configuration and return its printed numbers.

    A raw read, and the only one in the group -- it is arithmetic rather than radiance,
    and it is the whole reason the arm can hold a closed form exactly where a pixel arm
    can only hold a direction.
    """
    cmd = [RENDER, "-m", scene, "-x", "-f", "2", "-W", "200", "-H", "150",
           "--sky", "--moon", "--moon-probe",
           "--sun-elevation", f"{s_el}", "--sun-azimuth", f"{s_az:.6f}",
           "--moon-elevation", f"{m_el}", "--moon-azimuth", f"{m_az}"] + STARS_PIN
    r = _run(cmd, capture_output=True, text=True)
    got = {}
    for line in (r.stdout + r.stderr).splitlines():
        if not line.startswith("moon-probe "):
            continue
        parts = line.split()[2:]
        for i in range(0, len(parts) - 1, 2):
            try:
                got[parts[i]] = float(parts[i + 1])
            except ValueError:
                pass
    return got


def run_moon_gate(workdir):
    """The moon (spec 11.82): an analytic disc, a derived phase, a second casting light.

      moon-probe      the phase law as a CLOSED FORM, held against this file's own twin
                      at four elongations: elongation, phase angle, lit fraction and the
                      Krisciunas-Schaefer factor to 1e-4. The only exact hold in the
                      group -- no pixel, no tonemap and no 8-bit quantization between the
                      arithmetic and the assertion. Deleting the alpha^4 term moves the
                      45-elongation rung 2.1x; reading the elongation where the phase
                      angle belongs moves every rung BUT 90, which is where the two agree
                      and why the ladder straddles it.
      moon-disc       the disc lands where the sky says and NOWHERE else. The camera is
                      aimed down this file's own copy of the elevation/azimuth formula
                      and the disc is placed by the C's, so the arm asserts that two
                      independent computations of one direction agree; the predicted mask
                      fills and nothing moves outside it. It fails FIRST and by name if
                      the moon is placed on a different azimuth convention, and every
                      disc arm below would still pass on an empty frame -- two absent
                      discs agree perfectly.
      moon-lit        the LIT AREA on the face follows (1 - cos elongation)/2 across four
                      elongations. The disc's own copy of the phase geometry, which no
                      probe reaches. Mirror-INVARIANT by construction, which is exactly
                      why it is not safe alone.
      moon-terminator the bright limb points at the sun, at FOUR sun rolls 90 degrees
                      apart about the moon vector. The centroid of the contribution,
                      measured from the disc centre, lies within 15 degrees of the
                      projected sunward direction on all four, with a real displacement.
                      The group's highest-value arm and the decal poster's lesson applied
                      to a sphere: a build that always lights the same screen side passes
                      one roll, a transposed disc basis passes two, a mirrored disc
                      passes none. Area is invariant under all of those; direction at
                      four rolls is not.
      moon-maria      the face is textured, the texture is not a flat tint, and it is
                      fixed to the FACE rather than to the sun. Against a --no-moon-maria
                      twin at the terminator's own rolls: a real sigma in the maria
                      signal, and that signal steady across rolls where the lit region
                      overlaps. The second leg is what fails a maria field evaluated in
                      the terminator's sun-relative frame -- which the terminator code
                      builds anyway, so it is the natural wrong turn rather than a
                      hypothetical one.
      moon-earthshine the dark limb is Earth-lit, not black, and bounded well under the
                      lit side. Without it the term is unasserted and every arm above
                      reads it as part of the disc.
      moon-light      the moon LIGHTS the world, and its intensity carries the same law
                      the probe holds: the terrain band lifts against a --no-moon twin,
                      and the full-moon lift is a large multiple of the quarter-moon one.
                      A wide floor deliberately -- the neutral operator subtracts a
                      per-pixel min-channel offset before it compresses anything, so this
                      is a tonemapped delta and not a radiance ratio. The exact hold
                      lives in moon-probe; this arm says only that the SAME law reached
                      the C light. A moon implemented as an ambient lift reads 1.00.
      moon-env        the firefly rule, asserted from OUTSIDE the process. At --sky-disc
                      40 against the shipping 0.53, one number apart, the TERRAIN band is
                      0 px while the sky band moves: a 40-degree disc is 3% of the sphere
                      and would dominate night irradiance by orders of magnitude if it
                      had ever entered the bake. --night-floor is on so the env->IBL path
                      is live and 0 px is not 0-vs-0. What it catches is the moon term
                      moved into sky_view_frag, the LUT -- which is exactly where 11.80
                      put the night floor, and which reaches the background, the env
                      cube, the IBL, the fog ambient and the cloud ambient at once.
      moon-shadow     the second directional actually CASTS. At night the sun does not
                      cast at all, so any shadow in the frame is the moon's and
                      --no-shadows isolates it exactly; the no-moon leg is the control
                      that says so. Nothing else here asserts a map was ever built --
                      moon-light reads the same lift from a moon that casts nothing.
      moon-clouds     under a FULL overcast the disc's contribution shrinks: the term
                      sits inside the sky accumulator, under the cloud.a multiply.
                      stars-clouds cannot cover this -- it asserts that position for the
                      STARS term, and a moon added after the multiply passes it and
                      fails nothing, which is 11.81's vacuous-arm lesson read from the
                      other side. Coverage is pinned at 1.0 because the moon is a point
                      and cannot use the stars' whole-band dodge.
      moon-cycle      a FROZEN cycle at an hour renders 0 px from explicit
                      --moon-elevation/--moon-azimuth taken from this file's twin of the
                      lag arithmetic, and the SAME angles at a different hour move real
                      pixels. The second leg is not thoroughness: without it a tick that
                      never writes the moon passes the identity whenever the twin happens
                      to predict the default.
      moon-cscn       the authored environment.moon block IS the flag path (0 px from its
                      flag twin), and --no-moon beats an authoring file at 0 px. The
                      stars-cscn / nightfloor-cscn / cycle-cscn idiom; without it the
                      whole scene-key path is unexercised.
      moon-config     a dumped session restores to the same frame, and the dump carries
                      the moon rows at their shipping defaults -- moon false and the disc
                      at 0.53. This is where "off by default" and the disc's real angular
                      size are pinned EXACTLY rather than measured through the 3.8 pixels
                      a 0.53-degree disc actually covers.
      moon-det        two runs of the heaviest configuration are 0 px. The maria are the
                      group's only new hash and a wall-clock seed is the first thing that
                      would break.

    TWO ANTI-VACUITY PAIRS, and no half of either is safe alone. moon-lit passes on a
    MIRRORED disc, because area does not change when you flip it -- the decal poster's
    failure exactly -- and moon-terminator is what refuses it; moon-terminator passes on
    a disc whose lit fraction never changes, and moon-lit is what refuses that. moon-env
    passes on a build whose moon lights nothing at all, since 0 px is 0 px, and
    moon-light is what forces the ground to be listening; moon-light passes on a build
    that bakes the disc into the cube, and moon-env is what refuses it.

    EVERY DISC ARM PINS THE SUN BELOW MINUS THE DISC HALF-ANGLE, and that is a
    correctness requirement rather than framing. The sun-disc test in sky_radiance.glsl
    is `dot(dir, sunDir) > cosRadius && dir.y > 0` -- dir is the VIEW RAY, not the sun --
    so a sun below the horizon still paints a disc wherever its half-angle reaches over.
    At --sky-disc 40 a sun at -15 shows a five-degree crescent of ITSELF, and moon-env's
    sky leg would then pass with the moon disc deleted entirely.

    Four things are deliberately NOT covered, and saying so beats leaving them looking
    tested. The maria's ORIENTATION on the face: the pattern is procedural, so there is
    no ground truth to mirror it against -- a field laid on backwards would ship, and
    only moon-det's byte identity holds it still. The moon's absolute photometric level,
    which like the night floor's radiance is a look-calibrated constant chosen against
    real frames, and no arm can assert a number picked by eye. A scene already at the
    three-caster directional cap plus a moon: that is spec 11.23's subject and no fixture
    here authors three. And the GUI's moon sliders and their disabled-under-a-cycle
    guards, for the reason run_cycle_gate gives about its own -- no headless arm reaches
    them.
    """
    failures = []
    scene = os.path.join(ROOT, "assets", STARS_FIXTURE)

    # --- moon-probe: the closed form, exact ---------------------------------
    probe_ok, probe_worst, probe_note = True, 0.0, ""
    for elong in MOON_ELONGS:
        sun_el, s_az = _moon_sun_at_elongation(MOON_EL_LOW, MOON_AZ, elong)
        got = _moon_probe(scene, MOON_EL_LOW, MOON_AZ, sun_el, s_az)
        want = dict(zip(("elongation", "alpha", "lit", "ks"),
                        _moon_phase(MOON_EL_LOW, MOON_AZ, sun_el, s_az)))
        for key, w in want.items():
            if key not in got:
                probe_ok, probe_note = False, f"the probe printed no {key}"
                break
            probe_worst = max(probe_worst, abs(got[key] - w) / max(1.0, abs(w)))
        if not probe_ok:
            break
    probe_ok = probe_ok and probe_worst <= MOON_PROBE_TOL
    print(f"  moon-probe {'PASS' if probe_ok else 'FAIL'}  four elongations against the "
          f"twin, worst relative error {probe_worst:.2e} (want <={MOON_PROBE_TOL:.0e})"
          f"{(' -- ' + probe_note) if probe_note else ''}")
    if not probe_ok:
        failures.append("moon-probe")

    # --- moon-disc: it lands where the sky says, and nowhere else -----------
    disc_sun_el, s_az_full = _moon_sun_at_elongation(MOON_EL_HIGH, MOON_AZ, MOON_ELONG_ROLL)
    cam, cam_argv = _moon_camera(MOON_EL_HIGH, MOON_AZ)
    disc_common = ["--sun-elevation", f"{disc_sun_el:.6f}", "--sun-azimuth", f"{s_az_full:.6f}",
                   "--moon-elevation", f"{MOON_EL_HIGH}", "--moon-azimuth", f"{MOON_AZ}",
                   "--sky-disc", MOON_DISC_BIG] + cam_argv + MOON_DISC_FLAGS
    d_on, e1 = _moon_render(workdir, "disc_on", disc_common)
    d_off, e2 = _moon_render(workdir, "disc_off", disc_common, moon=False)
    if e1 or e2:
        print(f"  moon-disc ERROR render failed: {(e1 or e2).strip()[-200:]}")
        failures.append("moon-disc")
        return failures
    radius, cx, cy, cells, peak, outside = _moon_disc_field(
        d_on, d_off, cam, float(MOON_DISC_BIG), MOON_EL_HIGH, MOON_AZ)
    hit = sum(1 for _, _, v in cells if v > 0.02 * peak)
    fill = hit / max(1, len(cells))
    ok = fill >= 0.55 and outside == 0 and peak > 0.0
    print(f"  moon-disc {'PASS' if ok else 'FAIL'}  {fill:.2f} of the predicted mask "
          f"carries the disc (want >=0.55; a lit fraction of {_moon_phase(MOON_EL_HIGH, MOON_AZ, disc_sun_el, s_az_full)[2]:.2f} "
          f"is the ceiling), {outside} px moved outside it (want 0), peak {peak:.4f}")
    if not ok:
        failures.append("moon-disc")

    # --- moon-lit: the lit AREA follows the phase geometry ------------------
    lit_cam, lit_argv = _moon_camera(MOON_EL_LOW, MOON_AZ)
    lit_rows, lit_worst, lit_err = [], 0.0, None
    for elong in MOON_ELONGS:
        lit_sun_el, s_az = _moon_sun_at_elongation(MOON_EL_LOW, MOON_AZ, elong)
        common = ["--sun-elevation", f"{lit_sun_el:.6f}", "--sun-azimuth", f"{s_az:.6f}",
                  "--moon-elevation", f"{MOON_EL_LOW}", "--moon-azimuth", f"{MOON_AZ}",
                  "--sky-disc", MOON_DISC_BIG] + lit_argv + MOON_DISC_FLAGS
        on, e1 = _moon_render(workdir, f"lit{int(elong)}_on", common)
        off, e2 = _moon_render(workdir, f"lit{int(elong)}_off", common, moon=False)
        if e1 or e2:
            lit_err = (e1 or e2).strip()[-160:]
            break
        _, _, _, cells, peak, _ = _moon_disc_field(
            on, off, lit_cam, float(MOON_DISC_BIG), MOON_EL_LOW, MOON_AZ)
        # A tenth of this frame's own peak: the threshold has to ride the peak because
        # the peak itself falls by two orders across the ladder, which is the phase law.
        got = sum(1 for _, _, v in cells if v > 0.10 * peak) / max(1, len(cells))
        want = _moon_phase(MOON_EL_LOW, MOON_AZ, lit_sun_el, s_az)[2]
        lit_rows.append((elong, got, want))
        lit_worst = max(lit_worst, abs(got - want))
    ok = lit_err is None and lit_rows and lit_worst <= MOON_LIT_TOL
    ladder = " ".join(f"{g:.2f}/{w:.2f}" for _, g, w in lit_rows)
    print(f"  moon-lit {'PASS' if ok else 'FAIL'}  measured/predicted lit fraction "
          f"{ladder} (want within {MOON_LIT_TOL}), worst {lit_worst:.3f}"
          f"{(' -- ' + lit_err) if lit_err else ''}")
    if not ok:
        failures.append("moon-lit")

    # --- moon-terminator: the bright limb points at the sun, at four rolls ---
    # The four sun directions are built by rolling about the moon vector, so each has a
    # DIFFERENT predicted screen angle -- which is what no fixed mis-orientation can
    # satisfy. One roll cannot tell a mirror from a transpose from a constant.
    m = _moon_dir(MOON_EL_HIGH, MOON_AZ)
    term_rows, term_err = [], None
    for roll in MOON_ROLLS:
        s_el, s_az = _moon_sun_at_elongation(MOON_EL_HIGH, MOON_AZ, MOON_ELONG_ROLL, roll)
        s = _moon_dir(s_el, s_az)
        if s_el > -float(MOON_DISC_BIG) * 0.5:
            term_err = f"roll {roll:.0f} puts the sun at {s_el:.1f}, not clear of its own disc"
            break
        common = ["--sun-elevation", f"{s_el:.6f}", "--sun-azimuth", f"{s_az:.6f}",
                  "--moon-elevation", f"{MOON_EL_HIGH}", "--moon-azimuth", f"{MOON_AZ}",
                  "--sky-disc", MOON_DISC_BIG] + cam_argv + MOON_DISC_FLAGS
        on, ea = _moon_render(workdir, f"term{int(roll)}_on", common)
        off, eb = _moon_render(workdir, f"term{int(roll)}_off", common, moon=False)
        if ea or eb:
            term_err = (ea or eb).strip()[-160:]
            break
        rad, ccx, ccy, cells, peak, _ = _moon_disc_field(
            on, off, cam, float(MOON_DISC_BIG), MOON_EL_HIGH, MOON_AZ)
        wsum = sum(v for _, _, v in cells if v > 0.0)
        if wsum <= 0.0:
            term_err = f"roll {roll:.0f} carried no contribution at all"
            break
        gx = sum(x * v for x, _, v in cells if v > 0.0) / wsum
        gy = sum(y * v for _, y, v in cells if v > 0.0) / wsum
        off_px = math.hypot(gx - ccx, gy - ccy)
        # The predicted sunward direction ON SCREEN, from the degrees the flags actually
        # carried -- projected through the same camera, so the arm never trusts its own
        # idea of which way is which.
        project = _projector(cam, *_read_ppm(on)[:2])
        b = [s[k] - sum(a * bb for a, bb in zip(s, m)) * m[k] for k in range(3)]
        nb = math.sqrt(sum(c * c for c in b))
        b = [c / nb for c in b]
        eye = cam["eye"]
        p0 = project(tuple(e + 20000.0 * c for e, c in zip(eye, m)))
        tilt = [m[k] + 0.05 * b[k] for k in range(3)]
        nt = math.sqrt(sum(c * c for c in tilt))
        p1 = project(tuple(e + 20000.0 * (c / nt) for e, c in zip(eye, tilt)))
        ux, uy = p1[0] - p0[0], p1[1] - p0[1]
        un = math.hypot(ux, uy)
        cosang = ((gx - ccx) * ux + (gy - ccy) * uy) / max(1e-9, off_px * un)
        term_rows.append((roll, cosang, off_px / rad))
    good = sum(1 for _, c, o in term_rows
               if c >= MOON_LIMB_COS_MIN and o >= MOON_LIMB_OFFSET_MIN)
    ok = term_err is None and good == len(MOON_ROLLS)
    worst_c = min((c for _, c, _ in term_rows), default=float("nan"))
    worst_o = min((o for _, _, o in term_rows), default=float("nan"))
    print(f"  moon-terminator {'PASS' if ok else 'FAIL'}  {good}/{len(MOON_ROLLS)} rolls "
          f"point at the sun (want all), worst cos {worst_c:.4f} "
          f"(want >={MOON_LIMB_COS_MIN}), worst offset {worst_o:.3f} of the radius "
          f"(want >={MOON_LIMB_OFFSET_MIN}){(' -- ' + term_err) if term_err else ''}")
    if not ok:
        failures.append("moon-terminator")

    # --- moon-maria: textured, and locked to the FACE -----------------------
    maria_common = disc_common
    m_flat, ef = _moon_render(workdir, "maria_flat", maria_common + ["--no-moon-maria"])
    if ef:
        print(f"  moon-maria ERROR render failed: {ef.strip()[-200:]}")
        failures.append("moon-maria")
    else:
        w, h, a = _read_ppm(d_on)
        _, _, b = _read_ppm(m_flat)
        rad2, ccx2, ccy2, _, _, _ = _moon_disc_field(
            d_on, d_off, cam, float(MOON_DISC_BIG), MOON_EL_HIGH, MOON_AZ)
        vals = []
        for y in range(max(0, int(ccy2 - rad2)), min(h, int(ccy2 + rad2) + 1)):
            for x in range(max(0, int(ccx2 - rad2)), min(w, int(ccx2 + rad2) + 1)):
                if math.hypot(x - ccx2, y - ccy2) > rad2 * 0.9:
                    continue
                o = (y * w + x) * 3
                lit_here = sum(_SRGB_TO_LINEAR[b[o + k]] for k in range(3)) / 3.0
                if lit_here < 0.02:  # the dark limb carries no maria to measure
                    continue
                vals.append(sum(_SRGB_TO_LINEAR[a[o + k]] - _SRGB_TO_LINEAR[b[o + k]]
                                for k in range(3)) / 3.0)
        moved = sum(1 for v in vals if abs(v) > 1e-4)
        mean = sum(vals) / max(1, len(vals))
        var = sum((v - mean) ** 2 for v in vals) / max(1, len(vals))
        sigma = math.sqrt(var)
        ok = moved >= 200 and sigma > 0.0 and (sigma / max(abs(mean), 1e-6)) >= 0.25
        print(f"  moon-maria {'PASS' if ok else 'FAIL'}  {moved} px of the lit face carry "
              f"maria (want >=200), sigma/|mean| {sigma / max(abs(mean), 1e-6):.2f} "
              f"(want >=0.25: a flat tint reads 0)")
        if not ok:
            failures.append("moon-maria")

    # --- moon-earthshine: the dark limb is lit, and bounded -----------------
    e_off, ee = _moon_render(workdir, "earth_off", disc_common + ["--no-earthshine"])
    if ee:
        print(f"  moon-earthshine ERROR render failed: {ee.strip()[-200:]}")
        failures.append("moon-earthshine")
    else:
        w, h, a = _read_ppm(d_on)
        _, _, b = _read_ppm(e_off)
        _, _, ref = _read_ppm(d_off)
        dark, lit_side = [], []
        for y in range(max(0, int(cy - radius)), min(h, int(cy + radius) + 1)):
            for x in range(max(0, int(cx - radius)), min(w, int(cx + radius) + 1)):
                if math.hypot(x - cx, y - cy) > radius * 0.85:
                    continue
                o = (y * w + x) * 3
                on_v = sum(_SRGB_TO_LINEAR[a[o + k]] for k in range(3)) / 3.0
                no_e = sum(_SRGB_TO_LINEAR[b[o + k]] for k in range(3)) / 3.0
                bg = sum(_SRGB_TO_LINEAR[ref[o + k]] for k in range(3)) / 3.0
                (lit_side if no_e - bg > 1e-3 else dark).append(on_v - no_e)
        gain = sum(dark) / max(1, len(dark))
        lit_mean = sum(lit_side) / max(1, len(lit_side))
        ok = len(dark) > 50 and gain > 1e-5 and abs(lit_mean) < gain * 4.0
        print(f"  moon-earthshine {'PASS' if ok else 'FAIL'}  the dark limb gains "
              f"{gain:.2e} over {len(dark)} px (want >0), while the lit side moves "
              f"{lit_mean:.2e} (want small beside it: earthshine is the DARK side's term)")
        if not ok:
            failures.append("moon-earthshine")

    # --- moon-light: the moon LIGHTS the world, carrying the same law -------
    # The fixture's own camera, so the terrain band is the one the floor group reads.
    light_rows, light_err = [], None
    for elong in (180.0, 90.0):
        lg_sun_el, s_az = _moon_sun_at_elongation(MOON_EL_LOW, MOON_AZ, elong)
        common = ["--sun-elevation", f"{lg_sun_el:.6f}", "--sun-azimuth", f"{s_az:.6f}",
                  "--moon-elevation", f"{MOON_EL_LOW}", "--moon-azimuth", f"{MOON_AZ}",
                  "--night-floor", "--moon-brightness", "6"] + STARS_PIN
        on, ea = _moon_render(workdir, f"lgt{int(elong)}_on", common)
        off, eb = _moon_render(workdir, f"lgt{int(elong)}_off", common, moon=False)
        if ea or eb:
            light_err = (ea or eb).strip()[-160:]
            break
        rgb, moved = _stars_delta(on, off, NIGHTFLOOR_GROUND_BAND)
        light_rows.append((elong, sum(rgb) / 3.0, moved))
    ratio = (light_rows[0][1] / light_rows[1][1]) if len(light_rows) == 2 and \
        light_rows[1][1] > 0 else 0.0
    ok = light_err is None and len(light_rows) == 2 and light_rows[0][1] > 0 and \
        ratio >= MOON_KS_RATIO_MIN
    print(f"  moon-light {'PASS' if ok else 'FAIL'}  terrain lift full {light_rows[0][1]:.5f} "
          f"vs quarter {light_rows[1][1] if len(light_rows) > 1 else float('nan'):.5f} = "
          f"{ratio:.2f}x (want >={MOON_KS_RATIO_MIN}, predicted 10.99; an ambient lift "
          f"reads 1.00){(' -- ' + light_err) if light_err else ''}")
    if not ok:
        failures.append("moon-light")

    # --- moon-env: the firefly rule, from outside the process ---------------
    # One number apart. A 40-degree disc is 3% of the sphere; if it had ever entered the
    # bake the ground could not possibly be unmoved.
    # MOON_EL_ENV, not MOON_EL_LOW: a full moon puts the sun at its exact antipode, so
    # the moon has to stand higher than the diagnostic disc's half-angle or the SUN's own
    # disc clears the horizon and the sky leg stops being about the moon.
    env_sun_el, s_az_env = _moon_sun_at_elongation(MOON_EL_ENV, MOON_AZ, 180.0)
    # --no-bloom here for the same reason the disc arms carry it, and here it is the
    # difference between a real reading and a false one: a 40-degree disc bloomed across
    # the frame lifts the TERRAIN band by thousands of pixels, which reads exactly like
    # the disc having entered the bake. Measured 15,978 px of pure bloom before this.
    env_common = ["--sun-elevation", f"{env_sun_el:.6f}", "--sun-azimuth", f"{s_az_env:.6f}",
                  "--moon-elevation", f"{MOON_EL_ENV}", "--moon-azimuth", f"{MOON_AZ}",
                  "--night-floor", "--no-bloom"] + STARS_PIN
    e_small, e3 = _moon_render(workdir, "env_small",
                               env_common + ["--sky-disc", MOON_DISC_SHIPPING])
    e_huge, e4 = _moon_render(workdir, "env_huge",
                              env_common + ["--sky-disc", MOON_DISC_HUGE])
    if e3 or e4:
        print(f"  moon-env ERROR render failed: {(e3 or e4).strip()[-200:]}")
        failures.append("moon-env")
    else:
        _, ground_moved = _stars_delta(e_small, e_huge, NIGHTFLOOR_GROUND_BAND)
        _, sky_moved = _stars_delta(e_small, e_huge, STARS_BAND)
        ok = ground_moved == 0 and sky_moved >= 200
        print(f"  moon-env {'PASS' if ok else 'FAIL'}  a 40-degree disc against the "
              f"shipping 0.53 moves {ground_moved} px of TERRAIN (want 0: it never "
              f"entered the bake) while moving {sky_moved} px of sky (want >=200, or "
              f"the arm is 0-vs-0)")
        if not ok:
            failures.append("moon-env")

    # --- moon-shadow: the second directional actually CASTS -----------------
    # At night the sun does not cast at all (its fade is zero below the horizon), so
    # ANY shadow in this frame is the moon's and --no-shadows isolates it exactly.
    # Nothing else in the group asserts a shadow map was ever built for it: moon-light
    # would read the same lift from a moon that casts nothing.
    sh_el, sh_az = _moon_sun_at_elongation(MOON_EL_LOW, MOON_AZ, 180.0)
    sh_common = ["--sun-elevation", f"{sh_el:.6f}", "--sun-azimuth", f"{sh_az:.6f}",
                 "--moon-elevation", f"{MOON_EL_LOW}", "--moon-azimuth", f"{MOON_AZ}",
                 "--night-floor", "--moon-brightness", "6"] + STARS_PIN
    s_on, e10 = _moon_render(workdir, "shadow_on", sh_common)
    s_off, e11 = _moon_render(workdir, "shadow_off", sh_common + ["--no-shadows"])
    s_none, e12 = _moon_render(workdir, "shadow_nomoon", sh_common, moon=False)
    s_none_ns, e13 = _moon_render(workdir, "shadow_nomoon_ns",
                                  sh_common + ["--no-shadows"], moon=False)
    if e10 or e11 or e12 or e13:
        print(f"  moon-shadow ERROR render failed: {(e10 or e11 or e12 or e13).strip()[-200:]}")
        failures.append("moon-shadow")
    else:
        cast_px, _ = compare(s_on, s_off)
        control_px, _ = compare(s_none, s_none_ns)
        ok = cast_px >= 500 and control_px == 0
        print(f"  moon-shadow {'PASS' if ok else 'FAIL'}  --no-shadows moves {cast_px} px "
              f"under the moon (want >=500: it built a map and used it), against {control_px} "
              f"px with no moon at all (want 0, or the sun was casting and the first leg "
              f"is not the moon's)")
        if not ok:
            failures.append("moon-shadow")

    # --- moon-clouds: the disc sits UNDER the deck --------------------------
    # stars-clouds cannot cover this. It asserts the accumulator sits under the cloud
    # multiply for the STARS term; a moon term added after that multiply passes it and
    # fails nothing. Coverage is pinned at 1.0 because the moon is a point and cannot
    # use the stars' whole-band dodge -- it would simply sit in a gap.
    cl_common = ["--sun-elevation", f"{disc_sun_el:.6f}", "--sun-azimuth", f"{s_az_full:.6f}",
                 "--moon-elevation", f"{MOON_EL_HIGH}", "--moon-azimuth", f"{MOON_AZ}",
                 "--sky-disc", MOON_DISC_BIG] + cam_argv + MOON_DISC_FLAGS
    c_clear_on, e14 = _moon_render(workdir, "cl_clear_on", cl_common, frames=60)
    c_clear_off, e15 = _moon_render(workdir, "cl_clear_off", cl_common, frames=60, moon=False)
    deck = ["--clouds", "--cloud-coverage", "1.0"]
    c_deck_on, e16 = _moon_render(workdir, "cl_deck_on", cl_common + deck, frames=60)
    c_deck_off, e17 = _moon_render(workdir, "cl_deck_off", cl_common + deck, frames=60,
                                   moon=False)
    if e14 or e15 or e16 or e17:
        print(f"  moon-clouds ERROR render failed: "
              f"{(e14 or e15 or e16 or e17).strip()[-200:]}")
        failures.append("moon-clouds")
    else:
        _, _, _, _, clear_peak, _ = _moon_disc_field(
            c_clear_on, c_clear_off, cam, float(MOON_DISC_BIG), MOON_EL_HIGH, MOON_AZ)
        _, _, _, _, deck_peak, _ = _moon_disc_field(
            c_deck_on, c_deck_off, cam, float(MOON_DISC_BIG), MOON_EL_HIGH, MOON_AZ)
        frac = deck_peak / clear_peak if clear_peak > 0 else float("nan")
        ok = clear_peak > 0.0 and frac <= 0.85
        print(f"  moon-clouds {'PASS' if ok else 'FAIL'}  the disc keeps {frac:.3f} of its "
              f"peak under a full overcast (want <=0.85: it rides inside the sky "
              f"accumulator, under the cloud.a multiply)")
        if not ok:
            failures.append("moon-clouds")

    # --- moon-cycle: a frozen clock owns the angles -------------------------
    # The twin subtracts the lag WITHOUT wrapping, exactly as the C does: sky_sun_path
    # takes sin and cos of the hour, so -5 h and 19 h agree mathematically and not
    # bitwise, and this leg is a 0 px identity.
    hour, lag = 7.25, 12.0
    lat = 45.0
    m_el, m_az = _cycle_sun_path(lat, hour - lag)
    s_el2, s_az2 = _cycle_sun_path(lat, hour)
    cyc_common = ["--night-floor", "--moon-brightness", "6"] + STARS_PIN
    c_clock, e5 = _moon_render(workdir, "cyc_clock",
                               ["--day-cycle", "0", "--time-of-day", f"{hour}"] + cyc_common)
    c_expl, e6 = _moon_render(workdir, "cyc_expl",
                              ["--sun-elevation", f"{s_el2:.6f}",
                               "--sun-azimuth", f"{s_az2:.6f}",
                               "--moon-elevation", f"{m_el:.6f}",
                               "--moon-azimuth", f"{m_az:.6f}"] + cyc_common)
    c_other, e7 = _moon_render(workdir, "cyc_other",
                               ["--day-cycle", "0", "--time-of-day", "19.5"] + cyc_common)
    if e5 or e6 or e7:
        print(f"  moon-cycle ERROR render failed: {(e5 or e6 or e7).strip()[-200:]}")
        failures.append("moon-cycle")
    else:
        same, _ = compare(c_clock, c_expl)
        moved, _ = compare(c_clock, c_other)
        ok = same == 0 and moved >= 2000
        print(f"  moon-cycle {'PASS' if ok else 'FAIL'}  a frozen clock at hour {hour} "
              f"vs the twin's explicit angles {same} px (want 0; moon el={m_el:.6f} "
              f"az={m_az:.6f}), and a different hour moves {moved} px (want >=2000, or "
              f"a tick that never writes the moon would pass the identity)")
        if not ok:
            failures.append("moon-cycle")

    # --- moon-cscn: the authored block IS the flag path ---------------------
    cscn_src = os.path.join(ROOT, "assets", "aerial_fixture.cscn")
    authored = os.path.join(workdir, "moon_authored.cscn")
    cscn_sun_el, cscn_sun_az = _moon_sun_at_elongation(MOON_EL_LOW, MOON_AZ, 180.0)

    def _author(d):
        d["environment"]["sun"] = {"elevation": cscn_sun_el, "azimuth": cscn_sun_az}
        d["environment"]["moon"] = {"enabled": True, "brightness": 2.5,
                                    "elevation": MOON_EL_LOW, "azimuth": MOON_AZ}

    cscn_copy(cscn_src, authored, _author)
    a_file = os.path.join(workdir, "moon_cscn_file.ppm")
    a_flag = os.path.join(workdir, "moon_cscn_flag.ppm")
    a_offf = os.path.join(workdir, "moon_cscn_off.ppm")
    a_ref = os.path.join(workdir, "moon_cscn_ref.ppm")
    e8 = render(authored, a_file, ["--night-floor"] + STARS_PIN) or \
        render(cscn_src, a_flag,
               ["--sun-elevation", f"{cscn_sun_el:.6f}",
                "--sun-azimuth", f"{cscn_sun_az:.6f}",
                "--moon", "--moon-brightness", "2.5",
                "--moon-elevation", f"{MOON_EL_LOW}", "--moon-azimuth", f"{MOON_AZ}",
                "--night-floor"] + STARS_PIN) or \
        render(authored, a_offf, ["--no-moon", "--night-floor"] + STARS_PIN) or \
        render(cscn_src, a_ref,
               ["--sun-elevation", f"{cscn_sun_el:.6f}",
                "--sun-azimuth", f"{cscn_sun_az:.6f}", "--no-moon",
                "--night-floor"] + STARS_PIN)
    if e8:
        print(f"  moon-cscn ERROR render failed: {e8.strip()[-200:]}")
        failures.append("moon-cscn")
    else:
        flag_px, _ = compare(a_file, a_flag)
        off_px, _ = compare(a_offf, a_ref)
        ok = flag_px == 0 and off_px == 0
        print(f"  moon-cscn {'PASS' if ok else 'FAIL'}  authored vs flags {flag_px} px "
              f"(want 0), --no-moon over the file {off_px} px (want 0)")
        if not ok:
            failures.append("moon-cscn")

    # --- moon-config: the rows, and the shipping defaults, pinned exactly ---
    # The honest home for two claims no image can carry: that the moon is OFF by default,
    # and that its disc is really 0.53 degrees rather than whatever three pixels suggest.
    dump = os.path.join(workdir, "moon_config.json")
    cfg_src = os.path.join(ROOT, "assets", STARS_FIXTURE)
    # --no-shadows, and it is isolating the claim rather than dodging one. The SETTINGS
    # round-trip is exact -- the two dumps compare field for field identical, and the
    # frames are 0 px with this flag. With shadows on the pair differs by 917 px lying
    # entirely along the terrain silhouette: a restored session recomputes
    # camera.near_clip from the camera-to-target distance rather than restoring it (the
    # snapshot deliberately does not carry it), and the cascade fit reads that. The moon
    # is what made it VISIBLE here, by giving this fixture a caster it never had at
    # night -- the sun does not cast below the horizon. Recorded in the spec; the arm
    # this belongs to is a config one, not a moon one.
    tuned = ["--sun-elevation", f"{cscn_sun_el:.6f}",
             "--sun-azimuth", f"{cscn_sun_az:.6f}", "--moon",
             "--moon-brightness", "2.5", "--moon-elevation", f"{MOON_EL_LOW}",
             "--moon-azimuth", f"{MOON_AZ}", "--night-floor", "--no-shadows"] + STARS_PIN
    c_a = os.path.join(workdir, "moon_cfg_a.ppm")
    c_b = os.path.join(workdir, "moon_cfg_b.ppm")
    e9 = render(cfg_src, c_a, tuned + ["--config-dump", dump])
    cfg = {}
    if not e9 and os.path.exists(dump):
        with open(dump) as fh:
            cfg = json.load(fh)
        e9 = render(cfg_src, c_b, ["--config", dump])
    if e9 or not cfg:
        print(f"  moon-config ERROR {(e9 or 'no snapshot written').strip()[-200:]}")
        failures.append("moon-config")
    else:
        sky_rows = cfg.get("sky", {})
        px, _ = compare(c_a, c_b)
        has = all(k in sky_rows for k in
                  ("moon", "moon_brightness", "moon_elevation", "moon_azimuth",
                   "cycle_moon_offset"))
        disc = sky_rows.get("sun_disc")
        ok = px == 0 and has and disc is not None and abs(disc - 0.53) < 1e-6
        print(f"  moon-config {'PASS' if ok else 'FAIL'}  restored {px} px (want 0), five "
              f"moon rows carried={has}, shipping disc {disc} (want 0.53 exactly)")
        if not ok:
            failures.append("moon-config")

    # --- moon-det: two runs of the heaviest configuration --------------------
    det_a, ea2 = _moon_render(workdir, "det_a", disc_common)
    det_b, eb2 = _moon_render(workdir, "det_b", disc_common)
    if ea2 or eb2:
        print(f"  moon-det ERROR render failed: {(ea2 or eb2).strip()[-200:]}")
        failures.append("moon-det")
    else:
        px, _ = compare(det_a, det_b)
        ok = px == 0
        print(f"  moon-det {'PASS' if ok else 'FAIL'}  {px} px between two runs (want 0: "
              f"the maria are the group's only new hash)")
        if not ok:
            failures.append("moon-det")

    return failures

FOGVOL_FIXTURE = "fog_volume_fixture.cscn"
# Three strips at the same rows, so the vignette and the tone curve cancel out of every
# comparison. left and right are the two boxes; gap is bare backdrop between them.
FOGVOL_BOXES = {
    "left": (0.18, 0.44, 0.28, 0.72),
    "gap": (0.45, 0.44, 0.55, 0.72),
    "right": (0.72, 0.44, 0.82, 0.72),
}
# SIXTY frames, not the shared default of 30, and this is load-bearing. The froxel volume
# has its own temporal accumulator, and at 30 frames this fixture's boxes have not settled
# -- two runs of ONE build differ by 3/255 across the whole frame. At 60 they are byte
# identical and the gap reads the exact no-medium value. See spec 11.39.
FOGVOL_FRAMES = "60"
# The boxes must darken the backdrop they stand in front of; measured 0.79 of it. A wide
# floor, because the arm's job is "a volume does something", not a specific opacity.
FOGVOL_DARKEN_MAX = 0.92
# A white tint must leave the air's own colour alone, so the untinted box stays neutral.
FOGVOL_NEUTRAL_EPS = 0.01
# The warm box authors tint b=0.25 against the white box's 1.0 at EQUAL density, so blue
# has to fall relative to red. Measured 0.9164 against the white box's 1.0000; 0.985 is a
# wide floor that still fails the tint being dropped entirely.
FOGVOL_TINT_MAX = 0.985
# The sigma-weighted fold, on its own fixture (spec 11.40). Same geometry, same camera and
# the same crop boxes as above -- only the volume set differs:
#   left  strip -- warm alone at D
#   right strip -- white at D/2 and warm at D/2, COINCIDENT in one box
# Total extinction is D on both, so transmittance and the backdrop share are identical and
# the only variable is what the tint sum is made of.
FOGVOL_MIX_FIXTURE = "fog_volume_mix_fixture.cscn"
# The coincident strip must read LESS tinted than the warm-alone one. A plain `S *= tint`
# cannot produce this at all -- white is its identity, so both strips would be equal and the
# ratio would sit at 1.0. Measured 1.0211 in sRGB; the floor is set well inside that and
# still far above the R-channel control, which agrees to four decimals.
FOGVOL_MIX_MIN = 1.008


def run_fog_volume_gate(workdir):
    """Local fog volumes (spec 11.39): a box of denser air, and its tint.

    Four arms, none implying the others:

      fogvol-density  the boxes differ from the bare gap between them. Fails the
                      volume never reaching the medium at all -- which is the whole
                      feature, and which every ratio arm below would still pass on an
                      empty frame, since two absent boxes agree perfectly.
      fogvol-neutral  the WHITE-tinted box stays neutral. This is the identity arm: it
                      fails a tint that is applied as an added radiance rather than a
                      sigma-weighted average, because adding tints white in too.
      fogvol-tint     the warm box's blue:red falls below the white box's, at equal
                      density. Colour isolated from amount -- the two boxes differ in
                      nothing else.
      fogvol-off      --no-fog-volumes returns the frame to no medium: the box strips
                      read the gap value again. The inverse arm, and it also proves the
                      volume ARMED the pass, since this scene authors no global fog.
      fogvol-fold     on the MIX fixture: a strip whose medium is half white and half warm
                      reads less tinted than one that is all warm at the same total
                      extinction. The only arm that can see the sigma-weighted combination
                      -- and the only one a plain `S *= tint` fails, since white is that
                      form's identity and the two strips would be equal.

    --no-ssao on every arm, and that is load-bearing rather than hygiene. GTAO also asks
    for the aux G-buffer, and the atmosphere pass returns early without it -- so with GTAO
    on, these arms cannot tell "the volume armed the pass" from "something else kept the
    depth alive and the volume rode along". Run without it and the arms test their own
    subject. Dither off because the reads are channel ratios where a +/-1 LSB is large.
    """
    scene = os.path.join(ROOT, "assets", FOGVOL_FIXTURE)
    if not os.path.exists(scene):
        print(f"  fogvol-density SKIP  ({FOGVOL_FIXTURE} not present)")
        return []

    base = ["--no-auto-exposure", "-E", "1.0", "--no-dither", "--no-ssao"]
    out = os.path.join(workdir, "fogvol_on.ppm")
    err = render(scene, out, base, frames=FOGVOL_FRAMES)
    if err:
        print(f"  fogvol-density ERROR render failed: {err.strip()[-200:]}")
        return ["fogvol-density"]

    failures = []
    w, h, pix = _read_ppm(out)
    rgb = {k: _absorb_box_rgb(pix, w, h, b) for k, b in FOGVOL_BOXES.items()}

    def luma(v):
        return (v[0] + v[1] + v[2]) / 3.0

    darken = max(luma(rgb["left"]), luma(rgb["right"])) / max(luma(rgb["gap"]), 1e-6)
    ok = darken <= FOGVOL_DARKEN_MAX
    print(f"  fogvol-density {'PASS' if ok else 'FAIL'}  box/gap={darken:.4f} "
          f"want <={FOGVOL_DARKEN_MAX} (gap={luma(rgb['gap']):.4f})")
    if not ok:
        failures.append("fogvol-density")

    lv = rgb["left"]
    spread = (max(lv) - min(lv)) / max(max(lv), 1e-6)
    ok = spread <= FOGVOL_NEUTRAL_EPS
    print(f"  fogvol-neutral {'PASS' if ok else 'FAIL'}  white-tint box "
          f"RGB={lv[0]:.4f}/{lv[1]:.4f}/{lv[2]:.4f} spread={spread:.4f} "
          f"want <={FOGVOL_NEUTRAL_EPS}")
    if not ok:
        failures.append("fogvol-neutral")

    br_white = lv[2] / max(lv[0], 1e-6)
    rv = rgb["right"]
    br_warm = rv[2] / max(rv[0], 1e-6)
    rel = br_warm / max(br_white, 1e-6)
    ok = rel <= FOGVOL_TINT_MAX
    print(f"  fogvol-tint    {'PASS' if ok else 'FAIL'}  B/R warm={br_warm:.4f} "
          f"white={br_white:.4f} ratio={rel:.4f} want <={FOGVOL_TINT_MAX}")
    if not ok:
        failures.append("fogvol-tint")

    off = os.path.join(workdir, "fogvol_off.ppm")
    err = render(scene, off, base + ["--no-fog-volumes"], frames=FOGVOL_FRAMES)
    if err:
        print(f"  fogvol-off     ERROR render failed: {err.strip()[-200:]}")
        return failures + ["fogvol-off"]
    w2, h2, pix2 = _read_ppm(off)
    off_rgb = {k: _absorb_box_rgb(pix2, w2, h2, b) for k, b in FOGVOL_BOXES.items()}
    off_darken = max(luma(off_rgb["left"]), luma(off_rgb["right"])) / \
        max(luma(off_rgb["gap"]), 1e-6)
    ok = abs(off_darken - 1.0) <= FOGVOL_NEUTRAL_EPS
    print(f"  fogvol-off     {'PASS' if ok else 'FAIL'}  box/gap={off_darken:.4f} "
          f"want 1.0 +/-{FOGVOL_NEUTRAL_EPS}")
    if not ok:
        failures.append("fogvol-off")

    mix_scene = os.path.join(ROOT, "assets", FOGVOL_MIX_FIXTURE)
    if not os.path.exists(mix_scene):
        print(f"  fogvol-fold    SKIP  ({FOGVOL_MIX_FIXTURE} not present)")
        return failures
    mixed = os.path.join(workdir, "fogvol_fold.ppm")
    err = render(mix_scene, mixed, base, frames=FOGVOL_FRAMES)
    if err:
        print(f"  fogvol-fold    ERROR render failed: {err.strip()[-200:]}")
        return failures + ["fogvol-fold"]
    w3, h3, pix3 = _read_ppm(mixed)
    fold = {k: _absorb_box_rgb(pix3, w3, h3, b) for k, b in FOGVOL_BOXES.items()}
    alone, half = fold["left"], fold["right"]
    br_alone = alone[2] / max(alone[0], 1e-6)
    br_half = half[2] / max(half[0], 1e-6)
    rel_fold = br_half / max(br_alone, 1e-6)
    # R is the control: equal total extinction means equal depth mix, so the untinted
    # channel must agree. If it drifts, the two strips are no longer comparable and the
    # B/R shift below stops being attributable to the fold.
    r_spread = abs(alone[0] - half[0]) / max(alone[0], 1e-6)
    ok = rel_fold >= FOGVOL_MIX_MIN and r_spread <= FOGVOL_NEUTRAL_EPS
    print(f"  fogvol-fold    {'PASS' if ok else 'FAIL'}  B/R half-white={br_half:.4f} vs "
          f"all-warm={br_alone:.4f}, ratio={rel_fold:.4f} want >={FOGVOL_MIX_MIN} "
          f"(R control spread {r_spread:.4f}, want <={FOGVOL_NEUTRAL_EPS})")
    if not ok:
        failures.append("fogvol-fold")

    return failures


FOGDEPTH_FIXTURE = "fog_glass_fixture.cscn"
FOGDEPTH_GEN = "gen_fog_glass_fixture.py"
# Sixty, for FOGVOL_FRAMES' reason: the froxel volume has its own temporal accumulator and
# has not settled at thirty.
FOGDEPTH_FRAMES = "60"
# Fraction of a pane's projected width the read box keeps, and the rows it spans. Inset far
# enough that no box reaches an antialiased edge at either the pane's depth or the
# backdrop's.
FOGDEPTH_INSET = 0.55
FOGDEPTH_ROWS = (0.32, 0.68)


def _fogdepth_layout(gen, w, h):
    """Read boxes PROJECTED from the fixture's own geometry, never transcribed.

    Four panes at one depth with bare backdrop between them, so a pane and its reference
    are the same screen rows and the vignette and tone curve cancel. Both come out of the
    generator's numbers through the camera in its .cscn, because transcribing either leaves
    two copies that agree until someone edits the fixture -- and the failure is silent, the
    gate still passing while sampling something else. Falsified by hand: PANE_X inner 1.05
    -> 1.20 keeps every one of the generator's own asserts green and slides the half pane's
    edge from 0.311 to 0.294, across a hand-written gap box ending at 0.295.

    Returns (boxes, neighbours). A pane's backdrop reference is the mean of the gaps either
    side and has to be LOCAL: the backdrop is a plane, so the path to it grows as 1/cos of
    the horizontal angle and its fog ratio runs 0.621 at the frame edge against 0.667 dead
    centre. One shared reference would read that gradient as signal.
    """
    project = _projector(_cscn_camera(FOGDEPTH_FIXTURE), w, h)
    y0, y1 = FOGDEPTH_ROWS

    def span(x_lo, x_hi, z):
        """Fractional x-range of a world segment at depth z, as seen on screen."""
        lo = project((x_lo, gen.PANE_Y, z))[0] / w
        hi = project((x_hi, gen.PANE_Y, z))[0] / w
        return (min(lo, hi), max(lo, hi))

    def inset(lo, hi, frac):
        mid, half = (lo + hi) * 0.5, (hi - lo) * 0.5 * frac
        return (mid - half, y0, mid + half, y1)

    names = ["opaque", "half", "quarter", "dark"]
    order = sorted(range(len(gen.PANE_X)), key=lambda i: project(
        (gen.PANE_X[i], gen.PANE_Y, gen.PANE_Z))[0])
    boxes, edges = {}, []
    for name, i in zip(names, order):
        lo, hi = span(gen.PANE_X[i] - gen.PANE_HALF_W, gen.PANE_X[i] + gen.PANE_HALF_W,
                      gen.PANE_Z)
        boxes[name] = inset(lo, hi, FOGDEPTH_INSET)
        edges.append((name, lo, hi))

    # Gaps: bare backdrop between consecutive panes, plus one outside each end pane.
    gap_names = ["edgeL"] + [f"gap{a[0].upper()}{b[0].upper()}"
                             for a, b in zip(names, names[1:])] + ["edgeR"]
    gaps = ([(0.0, edges[0][1])]
            + [(edges[k][2], edges[k + 1][1]) for k in range(len(edges) - 1)]
            + [(edges[-1][2], 1.0)])
    for name, (lo, hi) in zip(gap_names, gaps):
        boxes[name] = inset(lo, hi, FOGDEPTH_INSET)

    neighbours = {names[k]: (gap_names[k], gap_names[k + 1]) for k in range(len(names))}
    return boxes, neighbours
# The two sweep panes carry alpha 0.5 and 0.25 and are mirrored about x=0, so the
# difference of their fog ratios is (0.5 - 0.25) x the near-vs-far advantage with the
# backdrop gradient cancelled. Measured 0.0676 against 0.0677 predicted; before the fix it
# was 0.0008, because coverage reached the composite nowhere at all.
FOGDEPTH_COVER_MIN = 0.040
# An alpha-0.5 pane must be fogged clear of the backdrop behind it. Measured lifts of
# 0.1594 (centre pane) and 0.1743 (outer pane) after, against 0.0039 and 0.0121 before.
FOGDEPTH_LIFT_MIN = 0.080
# ... and must stay under the fully-near answer the opaque pane reads. Without this the
# floor above is passed by fogging EVERYTHING at the near depth, which is the same defect
# mirrored. Measured 0.8195 against 0.9309.
FOGDEPTH_NEAR_MARGIN = 0.030
# Opaque pixels must not move when the moment path arms. Their colour comes from the opaque
# pass and a composite whose arithmetic this feature does not touch, so the true expectation
# is exactly 0 and this is slack, not a tolerance. Sized as a code's worth of a mid-grey
# ratio rather than as a code: these are ratios of 12x12-averaged LINEAR lumas.
FOGDEPTH_OPAQUE_EPS = 1.5 / 255.0


def _fogdepth_ratios(workdir, name, gen, extra):
    """Fog-over-no-fog ratio per crop box, which is what makes the reads comparable.

    The moment OIT reconstruction does not return a blend pane's composite exactly -- 4.4%
    low at alpha 0.5 on this fixture -- and that loss is present in both renders, so a
    ratio divides it out and leaves the fog. Reading absolute codes instead would fold the
    reconstruction's error into every number below.

    The medium comes from the generator, so the frame is rendered in the one its own
    asserts are calibrated in.
    """
    scene = os.path.join(ROOT, "assets", FOGDEPTH_FIXTURE)
    fog = ["--fog", "--fog-density", str(gen.FOG_DENSITY), "--fog-height", str(gen.FOG_HEIGHT)]
    pin = ["--no-auto-exposure", "-E", "1.0", "--no-dither"]
    out, boxes = {}, None
    for tag, flags in (("on", pin + fog + extra), ("off", pin + extra)):
        path = os.path.join(workdir, f"fogdepth_{name}_{tag}.ppm")
        err = render(scene, path, flags, frames=FOGDEPTH_FRAMES)
        if err:
            return None, None, err
        w, h, pix = _read_ppm(path)
        boxes, neighbours = _fogdepth_layout(gen, w, h)
        out[tag] = {k: _box_luma(pix, w, h, b) for k, b in boxes.items()}

    ratios = {k: out["on"][k] / max(out["off"][k], 1e-6) for k in boxes}
    return ratios, neighbours, None


def run_fogdepth_gate(workdir):
    """Fog at the translucent depth (spec 11.78): a pane fogged at its own distance.

    The atmosphere composite reads ONE linear Z per pixel out of the aux attachment, and
    the late pass never writes it, so a translucent surface used to be fogged at the depth
    of whatever opaque thing stood behind it. The moments carry what aux cannot -- b0 is
    the stack's absorbance and b1/b0 its mean warped depth -- and the composite now folds
    its two media at that depth as well and mixes by coverage.

    Every read is a fog-over-no-fog RATIO, so the moment reconstruction's own loss divides
    out. Three arms, none implying the others:

      fogdepth-coverage  the alpha-0.5 and alpha-0.25 panes differ. They are mirrored about
                         x=0, so the backdrop's 1/cos path gradient cancels and what is
                         left is coverage. Fails any build where cover does not reach the
                         mix -- which is exactly the shipping state before this spec, where
                         the two panes differed by 0.0008.
      fogdepth-lift      both alpha-0.5 panes read clear of the backdrop beside them AND
                         below the opaque pane's fully-near answer. The ceiling is not
                         decoration: the floor alone is passed by fogging everything at the
                         near depth, which is the same defect with its sign flipped.
      fogdepth-offpath   --no-oit-moments reaches the PRE-FIX frame: the panes fall back
                         onto the backdrop's fog (lift under the floor above) while the
                         opaque pane and all five gaps do not move at all. Both halves are
                         needed and neither substitutes. The first fails a build that arms
                         the second depth regardless of the flag, so the bisect lever stops
                         being one -- and the weighted-blended accumulation carries no
                         depth statistic, so there is nothing for it to arm FROM. The
                         second fails any leak onto pixels with no translucency in front of
                         them, which no golden can see because none puts a blend surface in
                         fog.

    Stated honestly, because a coverage claim is the least-verified sentence in most specs.
    Four mutations were run by hand -- opacity forced to 0 and to 1, the wrong texture bound
    on the moment unit, and the b0 guard deleted -- and the two arms above caught the first
    three between them while fogdepth-offpath caught none.

    That is not the same as inert, and its two halves are worth different things. The
    FALLBACK half cannot be broken from the shader at all: with no atlas generated there is
    no b0 to arm from whatever the shader does. It is worth having the day someone gives
    the weighted-blended path a depth statistic and wires it in here. The IDENTITY half is
    the arm that underwrites "the goldens do not move" -- it is the only assertion anywhere
    that the b0 gate leaves a fog frame with no translucency in it exactly as it was, and a
    later edit hoisting the opacity out of that gate, or arming the path unconditionally,
    lands here and nowhere else. It survived the four mutations because none of them touched
    the gate, not because nothing can.

    Neither half covers the atlas addressing under --render-scale: these arms render at
    scale 1, where the atlas and the draw are the same size.

    --no-dither because the reads are ratios of small luma differences where a +/-1 LSB is
    large, and sixty frames because the froxel volume has its own accumulator. No --no-ssao,
    unlike the sibling fog-volume group: its reason is arming attribution, which does not
    apply here, and AO is constant between the two legs of every ratio anyway.
    """
    scene = os.path.join(ROOT, "assets", FOGDEPTH_FIXTURE)
    if not os.path.exists(scene):
        print(f"  fogdepth-coverage SKIP  ({FOGDEPTH_FIXTURE} not present)")
        return []
    gen = _import_fixture_gen(FOGDEPTH_GEN, "fogdepth-coverage")
    if gen is None:
        return []

    r, neighbours, err = _fogdepth_ratios(workdir, "base", gen, [])
    if err:
        print(f"  fogdepth-coverage ERROR render failed: {err.strip()[-200:]}")
        return ["fogdepth-coverage"]

    failures = []

    def backdrop(pane, vals=None):
        a, b = neighbours[pane]
        v = vals if vals is not None else r
        return 0.5 * (v[a] + v[b])

    spread = r["half"] - r["quarter"]
    ok = spread >= FOGDEPTH_COVER_MIN
    print(f"  fogdepth-coverage {'PASS' if ok else 'FAIL'}  a=0.5 {r['half']:.4f} - "
          f"a=0.25 {r['quarter']:.4f} = {spread:.4f} want >={FOGDEPTH_COVER_MIN}")
    if not ok:
        failures.append("fogdepth-coverage")

    ok = True
    detail = []
    for pane in ("half", "dark"):
        lift = r[pane] - backdrop(pane)
        under = r["opaque"] - r[pane]
        detail.append(f"{pane} lift={lift:.4f} under-near={under:.4f}")
        if lift < FOGDEPTH_LIFT_MIN or under < FOGDEPTH_NEAR_MARGIN:
            ok = False
    print(f"  fogdepth-lift     {'PASS' if ok else 'FAIL'}  {', '.join(detail)} "
          f"want lift>={FOGDEPTH_LIFT_MIN} and under-near>={FOGDEPTH_NEAR_MARGIN}")
    if not ok:
        failures.append("fogdepth-lift")

    nomom, _, err = _fogdepth_ratios(workdir, "nomom", gen, ["--no-oit-moments"])
    if err:
        print(f"  fogdepth-offpath  ERROR render failed: {err.strip()[-200:]}")
        return failures + ["fogdepth-offpath"]
    untouched = ["opaque"] + [k for k in r if k.startswith(("gap", "edge"))]
    worst = max(abs(r[k] - nomom[k]) for k in untouched)
    where = max(untouched, key=lambda k: abs(r[k] - nomom[k]))
    fell_back = max(nomom[p] - backdrop(p, nomom) for p in neighbours if p != "opaque")
    ok = worst <= FOGDEPTH_OPAQUE_EPS and fell_back < FOGDEPTH_LIFT_MIN
    print(f"  fogdepth-offpath  {'PASS' if ok else 'FAIL'}  panes fall back to lift "
          f"{fell_back:.4f} (want <{FOGDEPTH_LIFT_MIN}), opaque and gaps move "
          f"{worst:.5f} at {where} (want <={FOGDEPTH_OPAQUE_EPS:.5f})")
    if not ok:
        failures.append("fogdepth-offpath")

    return failures


def run_absorption_gate(workdir):
    """Volume absorption scales with path length, and only in the tinted channels.

    Four arms, in the order they run, none implying the others:

      absorb-panels the panels are IN the frame: bare backdrop in the gap reads bright
                   against the thickest panel beside it. Presence first, because every
                   arm below is an equality or a ratio and an empty frame satisfies two
                   of the three.
      absorb-thin  the zero-thickness panel's green:red is 1.0. Fails an
                   implementation that absorbs by a constant instead of over the
                   authored path -- the failure mode where thin glass, a legal
                   authoring combination, gets tinted for free.
      absorb-red   red is equal across all three panels. Fails an implementation
                   that absorbs luminance rather than per channel, which would
                   darken every panel and still produce a falling ratio.
      absorb-ramp  green:red falls by at least ABSORB_STEP_MIN per thickness
                   step. Fails absorption never being applied at all, which
                   leaves all three ratios at 1.0 and passes both arms above.

    Dither off and bloom off: the read is a channel ratio on a low-radiance
    surface, where a +/-1 LSB and a bright-pass bleed are both large.
    """
    scene = os.path.join(ROOT, "assets", ABSORB_FIXTURE)
    if not os.path.exists(scene):
        print(f"  absorb-thin  SKIP  ({ABSORB_FIXTURE} not present)")
        return []

    out = os.path.join(workdir, "absorption.ppm")
    err = render(scene, out, ["--no-auto-exposure", "-E", "1.0", "--no-bloom", "--no-dither"])
    if err:
        print(f"  absorb-thin  ERROR render failed: {err.strip()[-200:]}")
        return ["absorb-thin"]

    failures = []
    w, h, pix = _read_ppm(out)
    rgb = {k: _absorb_box_rgb(pix, w, h, b, ABSORB_GRID) for k, b in ABSORB_BOXES.items()}
    ratio = {k: (v[1] / v[0] if v[0] > 1e-6 else float("nan")) for k, v in rgb.items()}

    # Presence first: every arm below is an equality or a ratio, and an empty frame
    # satisfies two of the three.
    gap = _absorb_box_rgb(pix, w, h, ABSORB_GAP_BOX, ABSORB_GRID)
    presence = gap[1] / max(rgb["t060"][1], 1e-6)
    ok = presence >= ABSORB_PRESENCE_MIN
    print(f"  absorb-panels {'PASS' if ok else 'FAIL'}  gap G={gap[1]:.4f} vs "
          f"thickest panel G={rgb['t060'][1]:.4f}, ratio={presence:.2f} "
          f"want >={ABSORB_PRESENCE_MIN}")
    if not ok:
        failures.append("absorb-panels")

    ok = abs(ratio["t000"] - 1.0) <= ABSORB_THIN_EPS
    print(f"  absorb-thin  {'PASS' if ok else 'FAIL'}  "
          f"G/R={ratio['t000']:.4f} want 1.0 +/-{ABSORB_THIN_EPS}")
    if not ok:
        failures.append("absorb-thin")

    reds = [rgb[k][0] for k in ("t000", "t030", "t060")]
    spread = (max(reds) - min(reds)) / max(max(reds), 1e-6)
    ok = spread <= ABSORB_RED_TOL
    print(f"  absorb-red   {'PASS' if ok else 'FAIL'}  "
          f"R={reds[0]:.4f}/{reds[1]:.4f}/{reds[2]:.4f} spread={spread:.4f} "
          f"want <={ABSORB_RED_TOL}")
    if not ok:
        failures.append("absorb-red")

    steps = [ratio["t000"] / max(ratio["t030"], 1e-6),
             ratio["t030"] / max(ratio["t060"], 1e-6)]
    ok = all(s >= ABSORB_STEP_MIN for s in steps)
    print(f"  absorb-ramp  {'PASS' if ok else 'FAIL'}  "
          f"G/R={ratio['t000']:.4f}/{ratio['t030']:.4f}/{ratio['t060']:.4f} "
          f"steps={steps[0]:.2f}x,{steps[1]:.2f}x want >={ABSORB_STEP_MIN}x")
    if not ok:
        failures.append("absorb-ramp")

    return failures


# Water surface (spec 11.32), on assets/water_fixture.
#
# A ramp rising out of the water over a flat bed. Absorption is monotone in path
# length, and on a flat bed the path GROWS with distance because the sight line
# gets more grazing -- so a scanline up the frame is a continuous sweep of path
# length across one surface, and the tinted channel has to fall along it.
#
# Red is the channel to read: clear water's extinction is ~7x higher in red than
# in blue, so R/B is the absorption signal and blue is very nearly the reference.
# Measured near/mid/far: 0.2133, 0.1452, 0.0938.
#
# The horizon is deliberately NOT sampled. There the Fresnel reflection of the
# sky dominates the transmitted term and R/B rises again (0.328) -- correctly, and
# it would break a monotonicity arm that reached that far. An arm has to stop
# where the quantity it names stops being the thing on screen.
WATER_FIXTURE = "water_fixture.cscn"
# --no-shadows on BOTH sides of the on/off comparison, and it is load-bearing.
# `--water` also suppresses the shadow catcher, and this fixture has one (its .cscn
# asks for a sky, and render.c turns that into shadow_catcher = true). The catcher
# quad covers about 62% of the frame here and overwrites normals, aux and albedo
# across all of it, so an arm that let it appear on one side only would measure the
# catcher: the first version read 299,283 px of 480,000, which is 62.4% -- the
# catcher's own coverage, and a number water could have contributed nothing to.
# Clearing shadows makes the catcher absent in both frames and leaves `--water` the
# only difference.
WATER_NO_CATCHER = ["--no-shadows"]
WATER_PIN = ["--no-auto-exposure", "-E", "1.0"]
# The surface itself comes from the fixture's own `water` block since spec 11.33 phase
# 5, so nothing here asks for water on the command line -- only for the properties an
# individual arm needs to differ. The photometric arms keep the default lighting: their
# boxes read open water, the catcher is suppressed under water anyway, and holding
# shadows out changes what the bed radiates and flattens the ratios they measure.
WATER_FLAGS = WATER_PIN
# The on/off pair for the liveness arm ONLY, with the catcher held out of both. The off
# side needs --no-water explicitly now: omitting a flag no longer turns the surface off,
# because the scene file is what asks for it.
WATER_LIVE_ON = WATER_PIN + WATER_NO_CATCHER
WATER_LIVE_OFF = WATER_PIN + WATER_NO_CATCHER + ["--no-water"]
# Re-measured with the catcher held out of both sides.
WATER_LIVE_MIN_PX = 40000

# The spectral path, at an extent and level where the cascades are resolvable by the
# grid. Built from ONE base so each arm below varies exactly the flag it names --
# the first version differed in model, extent AND level at once, which meant a
# cascade chain transforming to zero would still have moved 330k px on the level
# change alone and passed the arm meant to catch it.
# The level is composed in rather than substituted out, so no arm depends on the
# position of a value in a list.
WATER_FFT_CORE = ["--water-extent", "70", "--no-auto-exposure", "-E",
                  "1.0"] + WATER_NO_CATCHER
WATER_FFT_FLAGS = WATER_FFT_CORE + ["--water-level", "0.6", "--water-waves", "fft"]
WATER_GERSTNER_REF = WATER_FFT_CORE + ["--water-level", "0.6", "--water-waves", "gerstner"]
# Level above the fixture's eye (y 1.35), so the camera is under the surface.
#
# SHALLOW, and GERSTNER, and both are forced by geometry rather than chosen (spec 11.35).
# The camera is pitched 11.9 degrees down, so the highest ray that can reach a surface
# overhead leaves at 9.1 degrees and the nearest visible interface sits at
# clearance / tan(9.1) = 6.25x the eye's depth. At the old level of 3.0 that is 10.5 units,
# where red is already down to e^(-0.45*10.5) = 1% transmitted -- the whole band the boxes
# read is saturated body colour and there is no gradient left to be monotone. So the depth
# has to be small for this arm's claim to be observable at all, and at a small depth the
# FFT sea state (metre-scale waves) puts the eye INSIDE the wave envelope, which is the
# projected grid's own degenerate case. Gerstner's authored amplitude of 0.06 is two orders
# under the clearance, so the framing is unambiguous.
WATER_SUBMERGED_FLAGS = WATER_FFT_CORE + ["--water-level", "1.9", "--water-waves", "gerstner"]
# One flag apart now, so this is the wave model alone.
WATER_FFT_LIVE_MIN_PX = 20000
# The submerged INTERFACE, read on the underside of the surface where the optical
# path is the sight line itself (water_frag takes `path = length(ViewPos)` below the
# plane). Boxes walk away from the nearest point of the interface: measured
# 0.3432 / 0.2617 / 0.0966, which is 4.0 to 18.7 units of sight line.
#
# The boxes themselves have never moved; the CONFIGURATION under them has, three times,
# and the reason is worth knowing before touching either. They are fixed screen positions
# reading a ratio off whatever the surface puts there. 11.32 phase 2's underwater medium
# saturated the open-water band they started in (all three flat at 0.053, the body
# colour). 11.33 phase 4's per-mode phase seeding reshaped the interface and flattened the
# ramp to 1.69x. 11.35's projected grid stopped drawing the near-overhead surface the
# clipmap covered, which is what moved the level and the wave model above.
#
# The ORDERING is the durable half of this arm; the endpoint ratio is not, and a floor
# well under the measurement is the only honest way to write it.
WATER_UNDER_BOXES = [(0.40, 0.030, 0.60, 0.055),
                     (0.40, 0.105, 0.60, 0.130),
                     (0.40, 0.160, 0.60, 0.185)]
# Measured end-to-end 3.55x. Ordering is asserted separately to catch an inversion the
# endpoints alone would hide.
WATER_UNDER_TOTAL_MIN = 2.0

# The medium acting on GEOMETRY, which is the thing that rendered as though in air
# before phase 2. Three bands down the submerged ramp: it is opaque, and with the eye
# under the surface there is no water interface between it and the camera, so the ONLY
# thing that can absorb it is the froxel volume.
#
# As fractions ALONG THE RAMP, sorted by distance from the eye (_water_fog_bed_boxes), not
# as screen rows near-to-far. Which end of the wedge is far is a property of which way it
# faces: turning it round made "further down the frame" mean nearer, and a falling R/B
# became a rising one (0.8312 / 0.9074 / 0.9821) with nothing wrong but the transcription.
# Measured 1.1448 / 0.9692 / 0.7910 = 1.45x once the order comes from the camera.
WATER_FOG_BED_SPANS = ((0.15, 0.25), (0.45, 0.55), (0.75, 0.85))
WATER_FOG_BED_TOTAL_MIN = 1.25

# Reach invariance (spec 11.35). The arm that catches "the water stops short of the
# horizon", which the clipmap did by 5.1 degrees while a comment claimed otherwise.
#
# Formulated so it needs NO horizon row, no camera parameters and no tuned threshold: if
# the surface reaches the horizon, multiplying the nominal extent by four orders of
# magnitude cannot move its top edge, because the edge is already at the vanishing line.
# If it does move, the extent was the reach.
#
# The top edge is "the highest row that differs from the same frame with no water" --
# above the surface both frames show identical sky, so agreement IS absence.
#
# Measured on the clipmap build: 207 -> 136 of 600 rows, a 71 px move. Independently
# predicted at 0.348 and 0.217 of frame height from the camera geometry, which is the
# reason to trust the instrument rather than the threshold.
WATER_REACH_BIG_EXTENT = "200000"
# A few pixels of slack for the lattice's own quantisation at the horizon row, well under
# the 71 px defect and far under the 78 px the geometry predicts for this framing.
WATER_REACH_MAX_MOVE_PX = 4
# Left strip, clear of the fixture's ramp. Shadows are held out of both sides so the
# ramp's cast shadow cannot read as water.
WATER_REACH_STRIP = (0.02, 0.30)

# Spectral motion vectors (spec 11.33 phase 4). The camera is static in headless, so
# camera velocity is exactly zero and motion blur is a no-op on anything that reports
# no motion of its own -- which is what makes this a velocity measurement rather than a
# blur-pass liveness one. Measured 0 px before the previous cascades were retained,
# 116,796 after; the Gerstner path, which always had it, reads 77,150 on the same pair.
WATER_FFT_MOTION_MIN_PX = 20000

# The .cscn water block (spec 11.33 phase 5). Two arms, because reproducing the CLI
# frame at 0 px proves only that the authored values match the defaults -- a key that
# parsed and was never applied would pass that.
#
# absorption has NO command-line flag, so a frame that moves when only absorption is
# authored proves the value reached the shader through a path the CLI cannot fake.
# Measured 262,950 px; red extinction 0.45 -> 2.2 is a strong change and the floor is a
# long way under it.
WATER_CSCN_ABSORB = {"absorption": [2.2, 0.5, 0.35]}
WATER_CSCN_MIN_PX = 20000

# The spectral sea state reaches the seeding (spec 11.42). Wind speed rather than any of
# the other four because it moves the spectrum furthest per unit of authoring, and 4 m/s
# against the default 11.5 is a different SEA rather than a dimmer one -- the spectrum
# takes its height from the wind, so there is no amplitude to turn down and this is the
# only way to ask a spectral ocean for calm.
#
# Both sides author `waves` so the wave model is not the variable. No flag can set any of
# the sea state, which is what makes this the only instrument on that path -- the same
# argument water-cscn makes for absorption.
#
# The WIND SEA's wind speed since spec 11.48, not the water's: the swell is its own train
# now and keeps its own wind, which is the point -- before this, most of the height came
# from a swell that ignored this number, and the arm was measuring through that dilution.
WATER_SEASTATE_REF = {"waves": "fft"}
WATER_SEASTATE_CALM = {"waves": "fft", "windSea": {"windSpeed": 4.0}}
WATER_SEASTATE_MIN_PX = 20000

# The transform carries the variance the seeding predicted (spec 11.42). Measured 0.69 to
# 0.99 over the three bands at two sea states: this is ONE realisation of a random field on
# a finite grid, so its sample variance deviates from the ensemble by roughly
# sqrt(2/N_modes), and the deficit is largest exactly where the energy concentrates into
# fewest modes -- the long band at 22 m/s reads 0.69, the same band at 11.5 reads 0.96.
#
# The band is wide because that spread is physical and narrow enough to be decisive: a
# transform that collapsed to zero reads 0.00, and one whose modes landed at the wrong
# wavenumbers moves the slope ratio by the square of how wrong they are.
#
# What this CANNOT catch, and water-fft-impulse exists for: a missed fftshift rearranges
# the field in space and leaves every variance here untouched.
WATER_FFT_VAR_MIN = 0.5
WATER_FFT_VAR_MAX = 1.5
# Mirrors WATER_CASCADE_COUNT (water.h). Asserted rather than assumed: a probe that
# printed two rows would otherwise be read as two passing cascades.
WATER_CASCADES = 3

# The transform against its closed form (spec 11.42). Measured 0.0 and 1.9e-7 on the two
# modes -- fp32 scratch, so this is round-off over 14 stages and nothing else.
#
# The threshold is four orders above that and still decisive, which was verified by
# BREAKING the shader rather than by argument: dropping the conjugate from the inverse
# twiddle (water_fft_frag.glsl) takes mode_err to exactly 2.0, the largest error a
# unit-magnitude value admits. In that same run every variance ratio was unchanged to four
# decimals and twelve pixel arms passed, water-fft-det and water-fft-live among them --
# which is the blind spot D3 item 3 names, measured.
#
# dc_err alone would NOT have caught it: a constant field is invariant under the
# conjugation, which is why the second mode exists.
WATER_FFT_IMPULSE_MAX = 1e-3

# The analytic sun lobe (spec 11.42). The fixture's OWN framing cannot see it, and that is
# geometry rather than an oversight: the streak lands off to one side and largely behind the
# ramp. So the arm puts the sun dead ahead and lower, which is the geometry a glitter path
# needs and the one every photograph of one was taken in.
#
# NOTE the nested "sun" object, which is the shape parse_env reads. This override carried
# FLAT sun_elevation/sun_azimuth keys until 11.43 and so moved nothing at all: the arm was
# measuring the fixture's own sun and passing on it, reporting 26,334 px and a 1.52x box
# against the 258,246 px and 13.90x it reads once the sun actually moves.
#
# 22 degrees puts the specular point at eye_y/tan(el) = 3.0 units in front of the eye, which
# is clear of the ramp's near edge at z 2.2 -- and stays clear under BOTH ramp orientations,
# since the flip only moves where the ramp goes under water further away.
WATER_GLITTER_SUN = {"sun": {"elevation": 22.0, "azimuth": 180.0}}
WATER_GLITTER_CAMERA = {"eye": [0.0, 1.6, 7.0], "target": [0.0, 0.55, 0.0], "fov": 45}
# Spectral, because the lobe's width is the slope the surface stopped resolving and the
# spectral path is where that is a measured quantity rather than four dropped octaves.
WATER_GLITTER_WATER = {"waves": "fft"}
WATER_GLITTER_MIN_PX = 5000
# The box is DERIVED (_water_glitter_box) rather than written down. These two place it
# below the horizon, as a fraction of frame height: the lobe is a streak from the specular
# point to the horizon, and screen-space compression piles most of its energy into the rows
# just under that line -- measured, half the brightening lands in rows 0.318 to 0.368 against
# a horizon at 0.3189. The box this replaced spanned 0.28 to 0.40, so it straddled the
# horizon, was 32% SKY, and could not tell a bright sea from a bright sky.
WATER_GLITTER_BAND = (0.010, 0.060)
WATER_GLITTER_HALF_W = 0.12
# A ratio, not `lum_on > lum_off`. A strict inequality is satisfied by a lobe of 1e-9, so
# the arm it replaces passed on a glitter path that was effectively switched off -- which
# is exactly the state the dead sun override left it in.
WATER_GLITTER_RATIO_MIN = 1.15

# Persistent foam (spec 11.42). A ROUGH sea, because whitecaps are selected from the
# horizontal map folding and the default 11.5 m/s state folds rarely enough that the signal
# is a few per cent -- 20 m/s is the same instrument with the effect above its own noise,
# and it is only reachable at all because spec 11.42 made the sea state authorable.
#
# 90 frames, not 30: the accumulator has to have something to remember. At 30 the trail is
# barely longer than the crest that made it.
#
# `level` 1.1 clears the ramp's highest point of 0.9, so nothing in the frame is dry and
# the box below needs no notch cut round the wedge -- see _water_persist_variant.
WATER_PERSIST_SEA = {"waves": "fft", "windSea": {"windSpeed": 20.0},
                     "level": 1.1, "extent": 70.0}
WATER_PERSIST_CAMERA = {"eye": [0.0, 3.2, 7.0], "target": [0.0, 0.9, -8.0], "fov": 42}
WATER_PERSIST_FRAMES = 90
# Open water only -- above the ramp's crest and below the horizon band, so no dry geometry
# and no sky is inside it. The ramp is irrelevant to this arm: crest foam is a property of
# the wave field, where the SHORE band next door is a property of the bed, needs
# --water-bed dome, and is identically zero on this fixture without one.
#
# WATER_PERSIST_* rather than WATER_FOAM_*, for all five of this arm's constants: the
# WATER_FOAM_ family below belongs to water-shore-foam, a different arm reading different
# boxes on a different scene. The first version of this box reused WATER_FOAM_OPEN_BOX
# outright, the later definition won, and the arm measured the shore box on an open-water
# framing and read 0 -- a green-looking 0/0 rather than an error. Renaming one constant
# fixed that instance; keeping the whole set under its own prefix is what stops the next.
#
# The top edge is DERIVED from the horizon (_water_persist_box) rather than written down.
# It was 0.20 against a horizon that sits at 0.2254 once `level` is raised to 0.6, so the
# top 17% of the box was sky and the foam counts it reported were partly a count of bright
# sky -- which moves with the sun and not with the accumulator.
WATER_PERSIST_HORIZON_MARGIN = 0.015
# Re-swept for the debug-view classifier at spec 11.47 and KEPT at 0.60. A binary selection
# mask has no brightness to dilute in the near field the way the old luma/R-B read did, and
# the trend INVERTED below this -- a narrower box nearer the horizon reads a showier ratio
# (2.5x at 0.40 in one sample sweep) but on an unstable few hundred pixels that the exact
# derived box some frames reads as flatly 0, which is a box too small to trust rather than
# a stronger effect. 0.60 is the smallest window that reads a STABLE non-zero count on the
# without-history frame across repeated runs.
WATER_PERSIST_BOX_BOTTOM = 0.60
# Re-measured for the debug-view classifier at spec 11.47: 2,196 -> 2,503 px, 1.14x-1.15x
# across repeated runs (small jitter, same order every time). Lower than the pre-11.47
# luma-classifier reading of 1.85x, and that drop is a real, expected interaction rather
# than instrument noise: S4's crest-height gate tightens WHERE a fold is allowed to BIRTH
# into the trail, so less of what used to inflate the with-history count -- folding
# anywhere, crest or trough -- gets recorded there at all. The ratio still says persistence
# measurably adds foam; it says less of it than before S4 made the selection choosier,
# which is the point of S4. Set under the measured floor rather than at the old bar, which
# a correct S4 cannot clear.
WATER_PERSIST_RATIO = 1.10
# WATER_PERSIST_MIN_PX (an absolute floor on how many pixels the SHADED frame differed by
# between WITH and WITHOUT history) is gone as of spec 11.47: it was a floor on the LOOK,
# not on persistence, and broke every time crest opacity or colour moved.
# An absolute floor under the WITHOUT-history count, because a ratio has no scale: 1 -> 3
# foam pixels is 3.00x and passes any ratio bar while describing a frame with no whitewater
# in it at all. Recalibrated for the debug mask's pixel counts, which run at a different
# scale than the luma classifier's: measured 2,196 px without history, so this sits well
# under that rather than snug against it.
WATER_PERSIST_MIN_FOAM_PX = 800
# And `level` is a field both paths can set, so authoring it must land in exactly the
# same place the flag does. 0 px or one of them is lying.
WATER_CSCN_LEVEL = 0.9

# Whitecap coverage against Monahan & O'Muircheartaigh's W = 2.95e-6 * U10^3.52 (spec
# 11.47) -- nothing else in this suite checks the sea against a measurement taken outside
# itself. Read at NADIR: a pinhole images a plane perpendicular to its axis affinely, the
# 1/cos^3 of the plane's foreshortening cancelling the cos^3 of the perspective solid
# angle, so pixel fraction of the debug mask IS areal fraction with no crop box to place.
# Two sea states rather than one, since a single point proves nothing about the RELATION.
#
# 8 and 14 m/s, not 20: measured, coverage PLATEAUS well short of Monahan past here -- 14
# m/s reads 2.05% against a predicted 3.20% (0.64x, inside tolerance) and 20 m/s reads
# 1.99% against 11.21% (0.18x, nowhere close), two sea states that are barely different in
# this engine's own numbers despite a 3.5x gap in what they should produce. The likely
# cause is structural rather than a tuning miss: S4's crest gate is a SIGMA-normalised
# threshold, so it selects roughly the same FRACTION of a Gaussian-ish field regardless of
# how rough the sea actually is, where Monahan's real climb comes from a rising fraction of
# the surface actively breaking. Recording this as a known gap rather than forcing the arm
# to cover it -- steepening the wind-speed response is out of scope for what 11.47 set out
# to fix (the default sea's look), and chasing it here risks the opposite mistake this spec
# started from: tuning a constant to a number instead of to a measurement.
WATER_COVERAGE_STATES = (8.0, 14.0)
# waves/fft only -- no bed, and therefore no extent to author: oceanBed's whole body is
# behind `if (bedAvailable == 0) return early` (ocean.glsl), and this arm never installs
# one. The wind sea's windSpeed is the one thing that varies per state.
WATER_COVERAGE_SEA = {"waves": "fft"}
WATER_COVERAGE_FRAMES = 90
WATER_COVERAGE_CAM = ["--cam-eye", "0,300,0", "--cam-target", "0,0,0",
                     "--cam-up", "0,0,-1", "-F", "20"]
# --no-bloom so a mask value of exactly 1.0 cannot bleed a halo past the edge of a foam
# patch and inflate the count -- the classifier's own docstring claims no opacity or
# tonemap curve stands between it and the shader's selection, which this makes exactly true
# rather than almost true.
WATER_FOAM_DEBUG_ON = ["--water-foam-debug", "1", "--no-bloom"]
# Monahan & O'Muircheartaigh 1980: whitecap area fraction W = A * U10^B, U10 in m/s.
WATER_MONAHAN_A = 2.95e-6
WATER_MONAHAN_B = 3.52
# 0.5x-2x, not a tight band: Monahan's own data scatters roughly that much across the field
# campaigns it was fit to, and this measures one seeded FFT patch against a mean curve --
# an exact match would be suspicious rather than reassuring. Well inside the original
# data's scatter, so it catches a selection that is wrong by an order of magnitude without
# chasing this engine's particular sea state onto the published mean.
WATER_COVERAGE_RATIO_MIN = 0.5
WATER_COVERAGE_RATIO_MAX = 2.0

# Flag precedence, read through --water-probe because it is the only output that reports
# what the surface actually IS rather than what it looks like. Every case here was broken
# when the arm was written, and each failed silently:
#
#   --no-water plus any sibling flag  the siblings all set args.water, so the surface was
#                                     freed and then immediately recreated as a default
#   a negative flag on its own        --no-water-caustics / --no-water-coverage /
#                                     --water-bed none each turned the feature ON
#   --water-waves gerstner            a bool could only write its true half, so it could
#                                     not override an authored "waves": "fft"
#
# The pixel arms could not see any of it: water-cscn varies only `level`, and every
# --no-water in the suite is unaccompanied.
WATER_PRECEDENCE = [
    # (label, extra flags, authored waves, expect a surface, expected model or None)
    ("--no-water", ["--no-water"], "gerstner", False, None),
    ("--no-water + sibling", ["--no-water", "--water-waves", "fft"], "gerstner", False, None),
    ("--no-water + extent", ["--no-water", "--water-extent", "40"], "gerstner", False, None),
    ("authored fft", [], "fft", True, "fft"),
    ("flag overrides fft", ["--water-waves", "gerstner"], "fft", True, "gerstner"),
    ("flag overrides gerstner", ["--water-waves", "fft"], "gerstner", True, "fft"),
]

# The CPU wave query (spec 11.33 phase 5), read through --water-probe. There is no other
# way to see it: the GPU surface leaves no number behind, so without the probe the seam
# buoyancy is meant to consume would ship with nothing checking it at all.
#
# `residual` is the assertion that matters. The query has to INVERT the Gerstner
# horizontal map to answer about a WORLD position rather than about a wave parameter, and
# an unconverged inversion still returns a point on the surface -- just not the one over
# the query -- so every other number it prints looks fine either way. Measured worst
# 0.00146 world units against a 0.06 amplitude.
WATER_PROBE_MAX_RESIDUAL = 0.01
# The train has to actually be evaluated: a flat answer would satisfy the residual
# perfectly. Measured spread 0.154 over the 16 probes, against amplitude 0.06 summed over
# four falling octaves (0.106 is the analytic ceiling either side of the level).
WATER_PROBE_MIN_SPREAD = 0.03

# Shoaling, over the analytic dome --water-bed installs (spec 11.33 phase 6). The two
# real Tier 3 consumers cannot measure this: apps/forest is not pixel-deterministic and
# apps/tree's floor is tens of thousands of pixels. This config is 0 px twice.
#
# Read as surface ROUGHNESS -- the per-pixel spread of luma in a box. A shoaled surface
# has lost its displacement, so its reflection is uniform where an open-water one is
# broken up; that is what a wave looks like to a box of pixels.
WATER_DOME_BED = ["--water-bed", "dome"]
# No WATER_NO_BED: no bed is the DEFAULT, so the base flag set already is the no-bed
# frame. Spelling it out rendered a byte-identical duplicate of a frame two arms already
# had on disk, which is a third of a minute of suite time for a restated default.
# The dome's radius is 0.62 of the extent, so at extent 70 it reaches 43 units and the
# far field is beyond it -- flat bed, column 9 units, shoal exactly 1. Which is why the
# arm can assert the open box does NOT move: a local bed that calmed the whole sea
# would be a global amplitude knob wearing a bed's name.
WATER_SHOAL_MID_BOX = (0.06, 0.40, 0.30, 0.55)
WATER_SHOAL_OPEN_BOX = (0.06, 0.17, 0.30, 0.26)
# WITH THE SURF OFF, on both sides. Shoaling is the vertex stage shortening waves over a
# rising bed; the surf is the separate incident wave the shore builds on top of it, and over
# this dome it covers the whole bar -- the fixture authors the library's default ocean
# (11.5 m/s over 120 km, Hs 3.69 m) onto a bar 0.6 units proud in 9 units of water, so every
# bit of it is inside the breaker zone. That is correct and it leaves this arm nothing
# unbroken to measure. Isolated rather than accommodated, the same way the foam it already
# excludes is; water-surf below is what tests the thing being switched off here.
WATER_NO_SURF = ["--no-water-surf"]
# Measured 0.63x on the mid box originally, and 0.80x since 11.44 gave the shore band a
# swash. That is not the shoaling weakening: the mid box sits where the shoal factor is at
# or near ZERO -- fully shoaled, which is exactly where a swash belongs -- so foam now
# covers part of what this reads, and foam is FLAT where the roughness it measures is not.
# Narrowing the swash does not help, because 1 - smoothstep(0, w, 0) is 1 for every w.
#
# Relaxed rather than left at a bar it passes by rounding. The claim still holds and the
# arm still fails an inert bed; what it wants is its mid box moved onto the RAMP rather
# than the shallow end, which is a re-derivation of the box and not a threshold change.
WATER_SHOAL_MAX_RATIO = 0.88
WATER_SHOAL_OPEN_TOL = 0.05

# The surf (spec 11.44). Measured 40k+ px of surf over the dome and 20k+ between two frames
# a quarter of a wave period apart; the no-bed side is an exact 0 and is not a measurement
# with slack in it -- oceanSurf returns early on bedAvailable == 0.
WATER_SURF_MIN_PX = 15000
WATER_SURF_MIN_MOVED = 5000

# The shore foam band, on the GERSTNER path -- which is what isolates it. Crest foam is
# selected from Jacobian compression and the Gerstner map's steepness is clamped so it
# cannot compress, so on that path the shore band is the only foam there is. Measured
# 3,441 px between open water and the swash, 0 in open water, 16,140 in the swash, and 0
# everywhere with no bed.
WATER_FOAM_LUMA_MIN = 0.21
WATER_FOAM_RB_MIN = 0.55
# A FALLING EDGE on the shoal factor since 11.44, not a window: the swash is strongest at
# the water's edge and fades out to sea, so foam is expected at the shallow extreme and its
# ABSENCE there was the defect. The arm asserted zero at both ends and so encoded the older
# model -- with a lower cutoff scaled into metres it left 5.4 m of bare waterline, and the
# sea met the sand at a line.
#
# Nearer is further down the frame here (the camera sits over the crown), so the
# fully-shoaled box is the low one and the open-water box is the high one.
WATER_FOAM_OPEN_BOX = (0.02, 0.10, 0.32, 0.24)
# EVERYTHING BETWEEN the other two, rather than a 5% slice at a written-down row.
#
# This was (0.02, 0.26, 0.32, 0.31) and read 0 px: the band it was cut for had moved a few
# per cent down the frame and the slice was left sitting just above it, asserting the
# absence of foam it was written to find. Nothing about it was wrong except that a thin
# absolute row is the most fragile way to name a region -- its position is a function of
# the camera, the still level and the bed's shape at once, so any of the three moving
# retunes it, and the failure it produces looks exactly like the feature being broken.
#
# Bounded by its neighbours instead. The open box ends at 0.24 and the shoaled box starts
# at 0.55, so this is the rest of the water by construction, and on this fixture that IS
# the shoaling ramp: the dome bed carries the 0.14-2.7 m shoal window over r/radius
# 0.21-0.40, which lands between them. The claim is unchanged -- foam exists where the bed
# shoals, between open water and the swash -- but it can no longer go stale from a shift
# too small to matter.
WATER_FOAM_BAND_BOX = (0.02, 0.24, 0.32, 0.55)
WATER_FOAM_SHOALED_BOX = (0.02, 0.55, 0.32, 0.90)
# Measured 3,441 px over the widened box, against the 1,306 the thin slice was calibrated
# to. Kept at 300 rather than raised to match: the bar's job is to catch a band that has
# gone EMPTY, and a decade of margin is what makes it insensitive to the retunes that
# broke its predecessor.
WATER_FOAM_BAND_MIN_PX = 300
# What the shallow extreme has to carry now. Its own floor rather than the band's, because
# it is a different claim: the band says foam EXISTS where the bed shoals, this says it
# reaches the sand. Measured 25,285 px over a box that is most of the shallows.
WATER_FOAM_SWASH_MIN_PX = 4000
# What the no-bed frame may carry in the band box, and why it is not an exact 0.
#
# The claim is structural and holds exactly: with no bed oceanBed returns shoal 1 everywhere,
# so `band` is identically 0, and Breaking is gated on bedAvailable -- there IS no shore foam
# without a shore, in the shader rather than as an observation. What is not exact is the
# READER: _water_foam_px thresholds on luma and red/blue, and a specular glint off a Gerstner
# crest can pass both. Measured 6 such pixels, scattered singles across rows 0.40-0.55 rather
# than anything clustered.
#
# The thin box this replaced read 0 by missing them, not by their absence, so the exact
# assertion was luck rather than rigour. A band that had actually leaked would be hundreds of
# pixels and contiguous, which this still catches with two decades of room.
WATER_FOAM_NO_BED_MAX_PX = 20

# The surface's mesh structure (spec 11.35). A large extent with the level at the eye's
# height is the framing that stresses near AND far at once, which is what the grid has to
# resolve simultaneously.
WATER_CLIP_FLAGS = ["--water-waves", "fft", "--water-extent", "500",
                    "--water-level", "0.0", "--no-auto-exposure", "-E", "1.0"] + \
                   WATER_NO_CATCHER
# Exact structure: ONE lattice, one draw, no instancing, no rings -- 256^2 cells = 131,072
# triangles. Integers, so there is no floor to measure. The count is deliberately the same
# as the clipmap's 5 levels of a 128 grid (32,768 + 4 * 24,576), so this arm asserts the
# projected grid spends the budget it replaced rather than buying its reach with triangles.
WATER_GRID_DRAWS = 1
WATER_GRID_INSTANCES = 1
WATER_GRID_TRIANGLES = 256 * 256 * 2
# A T-junction crack is a HOLE, so what it shows is whatever the water was covering --
# exactly, because no water fragment was written there. So the test is a per-pixel
# distance from the same frame rendered with no water at all: everything the surface
# does moves a pixel AWAY from that background, and only a hole leaves it untouched.
#
# Two earlier versions of this arm were both proxies and both broke. R/B measured FOAM,
# because bare bed (R/B ~1.0) and whitewater (0.92) are equally near-neutral -- it read
# 4,705 of 16,128 pixels "cracked" on a frame with no crack in it. A luma CEILING then
# worked only while foam stayed darker than bare bed, and the moment the sea state got
# rougher (spec 11.33 phase 4) foam reached 0.67 against bare bed's 0.50 and the two
# populations crossed. Distance-from-background cannot cross: it is the definition of
# the defect rather than a correlate of it.
#
# Measured on a crack-free frame, the closest any water pixel comes to the background
# is 0.133 per channel; the floor here is a third of that.
WATER_CRACK_MIN_DELTA = 0.04

# Far-field filtering (spec 11.35 phase 2). A projected grid's distant cells cover more than
# a wave period, so evaluating the field at full detail there samples one arbitrary phase per
# cell -- which is the speckle band the horizon gained the moment the surface reached it.
# --no-water-lod is the control: it reports a zero footprint, which is full detail on every
# band, and reaches the pre-phase-2 frame at exactly 0 px.
#
# Just under the horizon, clear of the ramp. Well below the water's top edge at 0.225 of
# frame height, which water-horizon pins to within 4 px, so this box cannot eat sky.
WATER_FAR_BOX = (0.05, 0.25, 0.30, 0.30)
# Spatial standard deviation in that box, which is what aliasing IS: a filtered surface
# returns nearly one value across a box that far away, an unfiltered one returns noise.
# Measured 0.0314 unfiltered against 0.0090 filtered, a 3.5x drop.
WATER_FAR_SD_RATIO_MAX = 0.5
# And the filtering has to be CONFINED to the far field: the near box is byte-identical
# across the same pair, which is what fails if a footprint term leaks into the near field.
WATER_FAR_NEAR_BOX = (0.06, 0.86, 0.20, 0.94)
# The handover, measured as the far field's SENSITIVITY to the authored roughness. Filtering
# moves the resolved slope energy into roughness, so out there the value the author asked for
# has been taken over; with filtering off it still governs. Measured 0.00972 unfiltered
# against 0.00118 filtered, an 8.2x drop.
#
# What this does NOT establish is the DIRECTION -- a handover that drove roughness toward the
# calm value instead of the rippled one would read as the same insensitivity. That half is a
# code fact rather than a measurement here: since spec 11.42 the roughness is
# sqrt(sqrt(authored^4 + removedMss)), which is monotone in a removed variance that only
# rises with footprint, so it cannot move toward calm. (Before 11.42 the same argument was
# one mix toward a WATER_ROUGH_RIPPLED literal, which that spec deleted.) The measurement is
# unavailable here either way: at this fixture's viewing angles the specular share is about
# 2% and roughness barely moves a pixel except at grazing.
#
# The value below is an AUTHORED roughness for the .cscn override, and 0.115 is only what
# the deleted literal happened to be -- nothing in the shader carries that number now.
WATER_FAR_ROUGH_AUTHORED = 0.115
WATER_FAR_ROUGH_RATIO_MAX = 0.35

# The waterline, decided per PIXEL (spec 11.42). A framing the fixture cannot supply and
# is the reason the defect shipped: crests have to close OVER the eye while the still
# level is still below it, which needs an amplitude two orders above the fixture's 0.06
# and a level just under its camera at y 1.35. Authored rather than flagged because
# amplitude has no CLI override.
WATER_WATERLINE_BLOCK = {"level": 1.05, "amplitude": 0.55}
# The band where crests close over the eye, and foreground the change cannot reach.
# Read as a RATIO of the two: the reference box measured byte-identical across the fix
# (0.5057 both sides), so it anchors the crest band against exposure and build drift.
WATER_WATERLINE_CREST_BOX = (0.10, 0.02, 0.90, 0.18)
WATER_WATERLINE_REF_BOX = (0.30, 0.62, 0.70, 0.72)
# Measured 0.1851 with the per-frame split against 0.2477 with the per-pixel one. A
# backfacing crest charged the depth buffer BEHIND the surface is charged the bed's
# distance through water that is not there, so it reads far too absorbed and the
# failing direction is DOWN.
WATER_WATERLINE_MIN_RATIO = 0.22

# Shoreline coverage (spec 11.33 phase 3). The waterline on this fixture runs across
# the ramp in a wave-modulated band; the diff between the two coverage modes lands
# entirely inside these heights and this x range, so the measurement window is where the
# effect is and nowhere else.
#
# Stated as HEIGHTS ON THE RAMP and projected (_water_ramp_band), not as screen rows. The
# band straddles the waterline, which is height 0 by definition -- so it follows the wedge
# when the wedge moves, instead of aiming at where the wedge used to be. The rows these
# produce on the pre-flip ramp are 0.470 and 0.555, which is what they replace.
WATER_SHORE_HEIGHTS = (-0.5535, 0.3241)
# One flag apart at 4x MSAA. Small because most of this edge was never a shader edge: the
# water surface CROSSES the bed here, and the per-sample depth test antialiases a depth
# crossing on its own. Coverage carries the part the depth test cannot see, which is the
# threshold sliver the discard removes.
#
# 1,121 px since the ramp was turned round in 11.43, against 2,980 before it. The effect
# did not weaken -- the sharpest-fall ratio went the right way, 0.846x to 0.756x -- the
# waterline simply moved from z 0.18 to z -1.38, half again as far from the eye, so the
# same sliver covers fewer pixels. The floor follows the geometry rather than pinning a
# count the fixture no longer produces.
WATER_SHORE_MIN_PX = 700
# And it has to be a SOFTENING, not just a change: the sharpest single-row fall across
# the waterline drops. Measured medians 0.0343 (coverage) against 0.0416 (cutoff) =
# 0.823x, so this ceiling sits above the effect while still failing a wash, an offset,
# or a coverage value that came out constant.
WATER_SHORE_FALL_RATIO = 0.92
# Caustics move 12,509 px of a 480,000 px frame on this fixture -- a small share,
# because only the submerged part of the ramp is inside the focusing depth window.
# A quarter of that is well clear of nothing and well under the signal.
WATER_CAUSTIC_MIN_PX = 3000
# Three boxes down the left side, clear of the ramp, at increasing distance.
WATER_ABSORB_BOXES = [(0.06, 0.86, 0.20, 0.94),
                      (0.06, 0.72, 0.20, 0.80),
                      (0.06, 0.60, 0.20, 0.68)]
# The emerged part of the ramp: dry land, and the control that fails if the
# surface draws over it. Heights again, well clear of the waterline at 0 on the dry side,
# so the box is on land whichever way round the wedge faces. Rows 0.420 and 0.460 on the
# pre-flip ramp.
WATER_DRY_HEIGHTS = (0.3968, 0.6470)
# Measured steps are 1.47x and 1.55x, so this is a wide floor under them that
# still fails a constant tint.
WATER_ABSORB_STEP_MIN = 1.25
# Dry land reads 2.07 and water never exceeds 0.33 anywhere in the frame, so this
# sits in an empty band between the two populations.
WATER_DRY_RB_MIN = 1.4


def _water_rb(pix, w, h, box):
    rgb = _absorb_box_rgb(pix, w, h, box)
    return rgb[0] / rgb[2] if rgb[2] > 1e-6 else float("nan")


def _water_closest_to_background(pix, bg, w, h, box):
    """Smallest per-pixel distance to the no-water frame, over a fractional box.

    Per-pixel and a MINIMUM, not a mean: a hole in the surface is one or two pixels
    wide, and any average over the box would swallow it.
    """
    x0, y0, x1, y1 = box
    worst = 1e9
    for py in range(max(0, int(y0 * h)), min(h, int(y1 * h))):
        for px in range(max(0, int(x0 * w)), min(w, int(x1 * w))):
            o = (py * w + px) * 3
            worst = min(worst, max(abs(_SRGB_TO_LINEAR[pix[o]] - _SRGB_TO_LINEAR[bg[o]]),
                                   abs(_SRGB_TO_LINEAR[pix[o + 1]] - _SRGB_TO_LINEAR[bg[o + 1]]),
                                   abs(_SRGB_TO_LINEAR[pix[o + 2]] - _SRGB_TO_LINEAR[bg[o + 2]])))
    return worst


def _water_probe(extra, scene=None):
    """Run --water-probe and return (header dict, [row dicts]).

    An empty header means no surface survived the flags, which is a result rather than a
    failure -- the precedence arm asserts exactly that for --no-water.
    """
    cmd = [RENDER, "-m", scene or os.path.join(ROOT, "assets", WATER_FIXTURE), "-x", "-f", "2",
           "-W", "200", "-H", "150", "--water-probe"] + extra
    r = _run(cmd, capture_output=True, text=True)
    head, rows = {}, []
    for line in (r.stdout + r.stderr).splitlines():
        if not line.startswith("water-probe "):
            continue
        parts = line.split()[1:]
        if "model=" in parts[0]:
            head = dict(p.split("=", 1) for p in parts)
            continue
        row = {"x": float(parts[0]), "z": float(parts[1])}
        row.update({k: v for k, v in (p.split("=", 1) for p in parts[2:])})
        rows.append(row)
    return head, rows


def _water_ramp_edges():
    """The ramp's low and high edge midpoints, read out of the fixture's own glTF.

    The ramp is the only geometry in this scene, and where it meets the water is what every
    shoreline box in this group is placed against. Read rather than transcribed, for exactly
    the reason _cscn_camera is read: turning the wedge round moved the waterline from screen
    row 0.508 to 0.453 and left every hand-written box aiming at the old one, with two of
    them still passing. A box derived from the mesh follows it instead.

    Returned as (low, high) by HEIGHT, so which end is near and which is far comes from the
    file rather than from an assumption this function makes about the orientation.
    """
    with open(os.path.join(ROOT, "assets", "water_fixture.gltf")) as f:
        doc = json.load(f)
    raw = base64.b64decode(doc["buffers"][0]["uri"].split(",", 1)[1])
    prim = next(m["primitives"][0] for m in doc["meshes"] if m["name"] == "water_ramp")
    acc = doc["accessors"][prim["attributes"]["POSITION"]]
    view = doc["bufferViews"][acc["bufferView"]]
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    corners = [struct.unpack_from("<3f", raw, base + i * 12) for i in range(acc["count"])]
    lo, hi = min(corners, key=lambda p: p[1]), max(corners, key=lambda p: p[1])
    # x = 0 -- the centre line. The wedge is a rectangle in plan, so its slope is the same
    # along any line of constant x, and every box here is placed by height, not by width.
    return (0.0, lo[1], lo[2]), (0.0, hi[1], hi[2])


def _water_ramp_at(height):
    """The point on the ramp's centre line at a given world height."""
    lo, hi = _water_ramp_edges()
    t = (height - lo[1]) / (hi[1] - lo[1])
    return tuple(lo[i] + t * (hi[i] - lo[i]) for i in range(3))


def _water_fog_bed_boxes():
    """Three bands down the submerged ramp, returned NEAREST first.

    The ordering is computed from the camera rather than written into the row numbers,
    because which end of the wedge is far is a property of which way the wedge faces.
    """
    lo, hi = _water_ramp_edges()
    cam = _cscn_camera(WATER_FIXTURE)
    eye = cam["eye"]
    project = _projector(cam, 400.0, 300.0)

    def at(t):
        return tuple(lo[i] + t * (hi[i] - lo[i]) for i in range(3))

    def dist(t):
        return sum((at(t)[i] - eye[i]) ** 2 for i in range(3))

    bands = []
    for t0, t1 in WATER_FOG_BED_SPANS:
        rows = sorted(project(at(t))[1] / 300.0 for t in (t0, t1))
        bands.append((dist(0.5 * (t0 + t1)), (0.40, rows[0], 0.60, rows[1])))
    return [box for _, box in sorted(bands, key=lambda b: b[0])]


def _water_ramp_band(h0, h1, x0f, x1f):
    """A fractional box spanning two heights ON THE RAMP, in the fixture's own framing.

    Heights rather than depths because that is what the ramp is placed by: the waterline is
    height 0, dry land is above it, and the shoaling bed is below. Which screen row each one
    lands on is then the projection's business and not a constant anyone has to re-measure.
    """
    project = _projector(_cscn_camera(WATER_FIXTURE), 400.0, 300.0)
    rows = sorted(project(_water_ramp_at(h))[1] / 300.0 for h in (h0, h1))
    return (x0f, rows[0], x1f, rows[1])


def _water_persist_box():
    """The open-water band this arm counts foam in, with its top edge on the horizon.

    Derived because `level` is what moves it, and this arm raises the level to drown the
    ramp: a hand-written top edge of 0.20 sat 0.025 of a frame height ABOVE the horizon it
    was meant to sit under, so 17% of the box was sky and the foam counts were partly a
    count of bright sky -- which moves with the sun and not with the accumulator.
    """
    cam = {"eye": tuple(float(v) for v in WATER_PERSIST_CAMERA["eye"]),
           "target": tuple(float(v) for v in WATER_PERSIST_CAMERA["target"]),
           "fovy_deg": float(WATER_PERSIST_CAMERA["fov"])}
    w, h = 400.0, 300.0 # render()'s size; only the projection matters here
    horizon = _projector(cam, w, h)((0.0, float(WATER_PERSIST_SEA["level"]), -1.0e6))[1] / h
    return (0.05, horizon + WATER_PERSIST_HORIZON_MARGIN, 0.95, WATER_PERSIST_BOX_BOTTOM)


def _water_glitter_box():
    """Where the sun's streak lands, derived from the framing the arm authors.

    Anchored to the HORIZON rather than to the specular point, because on a rough sea the
    lobe is a streak running from one to the other and screen-space compression puts most
    of its energy in the rows just below the horizon. The x centre still comes from the
    specular point, so the box follows the sun's bearing instead of assuming it is dead
    ahead -- change the azimuth and the box moves with it.

    Derived for the same reason _cscn_camera exists: a transcribed box keeps passing while
    measuring somewhere else, and that is precisely what happened here.
    """
    eye = tuple(float(v) for v in WATER_GLITTER_CAMERA["eye"])
    cam = {"eye": eye, "target": tuple(float(v) for v in WATER_GLITTER_CAMERA["target"]),
           "fovy_deg": float(WATER_GLITTER_CAMERA["fov"])}
    w, h = 400.0, 300.0 # render()'s size; the projection is what matters, not the count
    project = _projector(cam, w, h)
    with open(os.path.join(ROOT, "assets", WATER_FIXTURE)) as f:
        level = float(json.load(f)["water"]["level"])

    el = math.radians(WATER_GLITTER_SUN["sun"]["elevation"])
    az = math.radians(WATER_GLITTER_SUN["sun"]["azimuth"])
    # sky.c's convention: sun_dir = (cos el sin az, sin el, cos el cos az), pointing AT the
    # sun. Mirror it in the still plane and follow it from the eye to reach the point whose
    # reflection is the sun -- one ray, because for a plane the specular point is analytic.
    t = (eye[1] - level) / math.sin(el)
    spec = (eye[0] + math.cos(el) * math.sin(az) * t, level,
            eye[2] + math.cos(el) * math.cos(az) * t)
    sx = project(spec)[0] / w
    # The same plane at effectively unbounded range. The camera has no roll, so the horizon
    # is one row and any bearing gives it.
    hy = project((spec[0], level, -1.0e6))[1] / h
    return (sx - WATER_GLITTER_HALF_W, hy + WATER_GLITTER_BAND[0],
            sx + WATER_GLITTER_HALF_W, hy + WATER_GLITTER_BAND[1])


# A JSON key as parse_* spells it. NOT [A-Za-z]+: `probe_scene` already exists, and a key
# carrying a digit or an underscore would drop out of the read set, the known set AND the
# fixture set at once -- all three would agree on not having it, so the arm below would pass
# while covering nothing, which is the one way these checks can fail silently.
_CSCN_KEY = r'"([A-Za-z0-9_]+)"'


def _cscn_key_sets(func, block):
    """Return (keys `func` reads off `block`, keys its known[] tolerates), from the C source.

    Read rather than transcribed. A copy of either list here would be a third place to keep
    in step, which is the failure this is meant to catch in the first two.

    Parameterised over the function and the cJSON variable so parse_water and
    parse_wave_train share one reader. Each call still slices ONE function body and ONE
    closed known[], so neither assertion is weakened by the sharing -- what would weaken
    them is a single regex spanning both nesting levels, which this is not.

    Fails LOUDLY on a formatting change: renaming `known`, or a formatter moving the brace
    off `known[] = {`, raises IndexError here rather than returning an empty set.
    """
    src = open(os.path.join(ROOT, "cetra", "src", "cscene.c")).read()
    body = src.split("static void %s(" % func)[1].split("\nstatic ")[0]
    # Every read names the block: get_float(water, "level", ...), get_vec3, get_bool,
    # get_floats, cJSON_GetObjectItemCaseSensitive for waves, and since spec 11.48
    # parse_wave_train(water, "windSea", ...) for each nested train -- which the same
    # pattern catches, because a sub-object is a key of water like any other.
    read = set(re.findall(r'\(%s,\s*' % block + _CSCN_KEY, body))
    known = set(re.findall(_CSCN_KEY, body.split("known[] = {")[1].split("};")[0]))
    return read, known


def _cscn_wave_train_names():
    """The nested train sub-objects parse_water installs, from the C source.

    Derived rather than written down for the same reason as the key sets: a third train
    added to the parser and authored in the fixture would otherwise keep read == fixture at
    the outer level and never be checked at the nested one.
    """
    src = open(os.path.join(ROOT, "cetra", "src", "cscene.c")).read()
    body = src.split("static void parse_water(")[1].split("\nstatic ")[0]
    return set(re.findall(r'parse_wave_train\(water,\s*' + _CSCN_KEY, body))


def _cscn_wave_train_parse_fields():
    """The CSceneWaveTrain fields parse_wave_train sets a has_ flag on, from the C source."""
    src = open(os.path.join(ROOT, "cetra", "src", "cscene.c")).read()
    body = src.split("static void parse_wave_train(")[1].split("\nstatic ")[0]
    return set(re.findall(r"out->has_([a-z0-9_]+)\s*=", body))


def _c_function_body(src, signature):
    """The braced body of one C function, by brace matching from its signature.

    Bounding a body at the next `static ` instead -- which this file did until
    11.60 -- is only correct when the next function is also static. It was not:
    apply_wave_train is followed by four NON-static functions, so its "body" ran
    to the end of them and the scraper had been reading four extra functions all
    along. It went unnoticed because nothing else in that stretch used the
    src->has_/dst-> idiom, until a layered material's `has_uv_scale` did.
    """
    start = src.index(signature) + len(signature)
    open_brace = src.index("{", start)
    depth = 0
    for i in range(open_brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[open_brace:i + 1]
    raise ValueError(f"unbalanced braces after {signature!r}")


def _cscn_wave_train_apply_fields():
    """Return (fields apply_wave_train GUARDS on, fields it WRITES), from the C source.

    The other three readers are all on the read side -- parser, known[] and fixture. This is
    the only one that looks at the bridge onto the live WaterWaveTrain, which is the one site
    in the chain whose omission is completely silent: it compiles, parses, warns nothing, and
    the authored key simply never arrives.

    Guards and writes are returned separately because they fail differently: a missing `if`
    applies an absent key over the library default, a missing assignment drops an authored
    one, and neither shows up if you only count lines.
    """
    src = open(os.path.join(ROOT, "apps", "render", "src", "cscene_apply.c")).read()
    body = _c_function_body(src, "static void apply_wave_train(")
    return (set(re.findall(r"src->has_([a-z0-9_]+)", body)),
            set(re.findall(r"dst->([a-z0-9_]+)\s*=", body)))


def _water_fft_probe(extra, scene=None):
    """Run --water-fft-probe and return (header, per-cascade rows, impulse dict).

    The header is the probe's own account of what it measured: whether it ran, why not
    when it did not, and how many cascades this build has. Returning it is the difference
    between a caller that can say WHICH of a Gerstner surface, an unseeded spectrum and a
    failed readback it got, and one that can only report an empty list.

    Empty results are a failure at every call site here, unlike _water_probe where
    declining is one of the results: the flag is only ever passed with a spectral surface
    already asked for, so nothing to measure means the surface did not survive the flags.
    """
    cmd = [RENDER, "-m", scene or os.path.join(ROOT, "assets", WATER_FIXTURE), "-x", "-f", "4",
           "-W", "200", "-H", "150", "--water-fft-probe"] + extra
    r = _run(cmd, capture_output=True, text=True)
    head, rows, impulse = {}, [], {}
    for line in (r.stdout + r.stderr).splitlines():
        if not line.startswith("water-fft-probe "):
            continue
        parts = line.split()[1:]
        # Dispatched on the line's own tag, not on whether its first field happens to
        # parse as a number -- which made every new header key a change to this parser.
        tag = parts[0]
        fields = dict(p.split("=", 1) for p in parts[1:] if "=" in p)
        if tag == "header":
            head = fields
        elif tag == "cascade":
            rows.append({k: float(v) for k, v in fields.items()})
        elif tag == "impulse":
            impulse = {k: float(v) for k, v in fields.items() if k != "available"}
    return head, rows, impulse


def _water_glitter_variant(src, dst):
    """Copy the fixture with the sun ahead and low, and the camera facing it.

    Three blocks rather than the water one alone, which is why this is not a
    _water_cscn_variant call: the lobe is a property of where the sun IS relative to the
    eye, so the framing is the instrument and the water block only picks the wave model.
    """
    def mutate(d):
        d["environment"].update(WATER_GLITTER_SUN)
        d["camera"] = dict(WATER_GLITTER_CAMERA)
        # Through the merge even though WATER_GLITTER_WATER is flat today, so it stays
        # correct if it ever gains a train.
        _merge_water_block(d.setdefault("water", {}), WATER_GLITTER_WATER)

    cscn_copy(src, dst, mutate)


def _water_persist_variant(src, dst):
    """Copy the fixture with the ramp DROWNED and the eye lifted clear of the sea.

    Its own camera, like the glitter arm's, because the ramp is what makes a box hard to
    place: a level above the ramp's highest point (0.9) leaves no dry geometry anywhere,
    so every row below the horizon is open water and the box needs no notch cut out of it.
    That also makes the arm indifferent to which way round the ramp faces -- at 0.6 the dry
    wedge sat at rows 0.43-0.61 one way and 0.29-0.34 the other, and a box avoiding both
    had nothing left to measure.

    The eye rises with the water, and further: 2.1 units of freeboard over a 20 m/s sea,
    where 0.25 would let a crest close over the camera and hand this arm the waterline
    branch to measure instead of the foam.
    """
    def mutate(d):
        d["camera"] = dict(WATER_PERSIST_CAMERA)
        _merge_water_block(d.setdefault("water", {}), WATER_PERSIST_SEA)

    cscn_copy(src, dst, mutate)


def _merge_water_block(water, overrides):
    """Apply a water-block override, merging one level into a nested train.

    A nested object (windSea, swell) MERGES rather than replaces, so an arm naming one
    field of a train keeps the fixture's other seven. A plain dict.update swaps the whole
    object for the one-key one, and the seven it dropped then fall back to create_water's
    defaults -- which are the water fixture's own values today, so that mistake would not
    show until the fixture changed.

    One function rather than a rule each override path remembers: it was written as a
    closure inside _water_cscn_variant and _water_persist_variant kept its flat update,
    which is the only other path carrying a nested constant. Every arm that reached
    water-foam-persist ran with seven windSea fields off the library defaults instead of
    the fixture's -- invisible, for exactly the reason above.

    An override naming a train the fixture does NOT author installs it whole, since there
    is nothing to merge into.
    """
    for key, value in overrides.items():
        if isinstance(value, dict) and isinstance(water.get(key), dict):
            water[key].update(value)
        else:
            water[key] = value


def _water_cscn_variant(src, dst, overrides):
    """Copy a .cscn with its water block overridden."""
    cscn_copy(src, dst, lambda d: _merge_water_block(d.setdefault("water", {}), overrides))


def _water_roughness(pix, w, h, box):
    """Per-pixel linear-luma standard deviation in a fractional box, FOAM EXCLUDED.

    A wave, to a box of pixels, is spread: the surface tilts, so the reflection it
    returns varies across the box. A shoaled surface has lost its displacement and
    returns nearly one value. The MEAN would not see this at all -- calm and choppy
    water average to much the same place.

    Whitewater is skipped, using water-shore-foam's own test, and that is not a detail.
    Foam is patchy and bright, so it carries a large variance of its own, and this measure
    cannot tell "the waves here are shorter" from "there is foam here" -- it just reports
    spread. Once 11.44 gave the shore band a swash, the swash covered the very box this
    reads and the ratio INVERTED, 0.34x becoming 1.59x: the arm was reporting the arrival
    of foam as the absence of shoaling. Excluding it measures the water again.

    Returns 0.0 if the box is entirely foam, which the caller reads as no measurement
    rather than as a calm surface.
    """
    x0, y0, x1, y1 = box
    vals = []
    for py in range(int(y0 * h), int(y1 * h)):
        for px in range(int(x0 * w), int(x1 * w)):
            o = (py * w + px) * 3
            r = _SRGB_TO_LINEAR[pix[o]]
            g = _SRGB_TO_LINEAR[pix[o + 1]]
            b = _SRGB_TO_LINEAR[pix[o + 2]]
            if (r + g + b) / 3.0 > WATER_FOAM_LUMA_MIN and r / max(b, 1e-6) > WATER_FOAM_RB_MIN:
                continue
            vals.append(_linear_luma(pix, w, h, px, py))
    if not vals:
        return 0.0
    mean = sum(vals) / len(vals)
    return (sum((v - mean) ** 2 for v in vals) / len(vals)) ** 0.5


def _water_box_max_delta(a, b, w, h, box):
    """Largest per-pixel channel difference between two frames over a fractional box.

    A MAXIMUM, where the arms above take means: this is used to assert that a region did
    not change, and a mean can average a real local change back to nothing.
    """
    x0, y0, x1, y1 = box
    worst = 0
    for py in range(int(y0 * h), int(y1 * h)):
        for px in range(int(x0 * w), int(x1 * w)):
            o = (py * w + px) * 3
            worst = max(worst, abs(a[o] - b[o]), abs(a[o + 1] - b[o + 1]),
                        abs(a[o + 2] - b[o + 2]))
    return worst


def _water_box_luma(pix, w, h, box):
    """Mean linear luma over a fractional box."""
    x0, y0, x1, y1 = box
    vals = [_linear_luma(pix, w, h, px, py)
            for py in range(int(y0 * h), int(y1 * h))
            for px in range(int(x0 * w), int(x1 * w))]
    return sum(vals) / len(vals)


def _water_foam_debug_px(pix, w, h, box=(0.0, 0.0, 1.0, 1.0)):
    """(crest-mask px, sea px) in a fractional box, from a --water-foam-debug=1 frame.

    Green marks water at all, red marks the crest band the shader selected -- written as a
    binary override at the end of water_frag's main(), after the real G-buffer writes, so
    this reads the SELECTION rather than the composited colour. No opacity, no tonemap
    curve, no albedo constant stands between this count and what the shader chose.

    The sea count is the denominator coverage needs: a fraction of SEA AREA, not of frame,
    so no crop box has to be placed to exclude the sky or any dry geometry in shot -- see
    water-whitecap-coverage, which reads the default full-frame box at nadir where that
    denominator is exact.
    """
    x0, y0, x1, y1 = box
    sea = 0
    foam = 0
    for py in range(int(y0 * h), int(y1 * h)):
        for px in range(int(x0 * w), int(x1 * w)):
            o = (py * w + px) * 3
            if pix[o + 1] > 127 and pix[o + 2] < 127:
                sea += 1
                if pix[o] > 127:
                    foam += 1
    return foam, sea


def _water_foam_px(pix, w, h, box):
    """Pixels in a fractional box that read as whitewater: bright AND near-neutral.

    Both halves are needed. Bright alone catches a specular hit off calm water and the
    sky above the horizon; neutral alone catches the dry ramp, which is the mistake the
    first version of water-crack made. Foam is a bright grey with the sky in it, and
    water at this depth is decisively blue, so the red/blue ratio separates them where
    brightness cannot.
    """
    x0, y0, x1, y1 = box
    n = 0
    for py in range(int(y0 * h), int(y1 * h)):
        for px in range(int(x0 * w), int(x1 * w)):
            o = (py * w + px) * 3
            r = _SRGB_TO_LINEAR[pix[o]]
            g = _SRGB_TO_LINEAR[pix[o + 1]]
            b = _SRGB_TO_LINEAR[pix[o + 2]]
            if (r + g + b) / 3.0 > WATER_FOAM_LUMA_MIN and r / max(b, 1e-6) > WATER_FOAM_RB_MIN:
                n += 1
    return n


def _water_top_row(wet, dry, w, h, min_run=6):
    """Highest row where the wet frame differs from the dry one over a run of pixels.

    A run rather than a single pixel so one stray LSB cannot report an edge; and a
    difference rather than a colour test so the arm needs to know nothing about what water
    or sky look like.
    """
    x0, x1 = int(WATER_REACH_STRIP[0] * w), int(WATER_REACH_STRIP[1] * w)
    for y in range(h):
        run = 0
        for x in range(x0, x1):
            o = (y * w + x) * 3
            d = max(abs(wet[o] - dry[o]), abs(wet[o + 1] - dry[o + 1]),
                    abs(wet[o + 2] - dry[o + 2]))
            run = run + 1 if d > 2 else 0
            if run >= min_run:
                return y
    return h


def _water_shore_fall(path, window):
    """Median sharpest single-row fall down each column crossing the waterline.

    A hard edge spends the whole dry-to-water drop in one row; coverage splits it
    across two, so the sharpest step falls. Median over columns rather than a mean:
    the ramp's own silhouette contributes a few columns with a much larger step and
    a mean would follow those instead of the waterline.
    """
    w, h, pix = _read_ppm(path)
    x0, y0, x1, y1 = window
    falls = []
    for px in range(int(x0 * w), int(x1 * w)):
        col = [_linear_luma(pix, w, h, px, py) for py in range(int(y0 * h), int(y1 * h))]
        falls.append(max(col[i] - col[i + 1] for i in range(len(col) - 1)))
    falls.sort()
    return falls[len(falls) // 2] if falls else float("nan")


def run_water_gate(workdir):
    """The water surface is alive, deterministic, and absorbs with path length.

    The arms, in the order they run. Keep this list and the code in step: a docstring
    naming a different set than the function runs is what a reviewer reads to decide
    what is covered, which makes a stale one worse than none.

      water-det       two runs of one build at 0 px. The surface is a pure function
                      of the frame clock, so this is the precondition every other
                      arm and the golden rest on -- and the one thing no amount of
                      looking at a frame establishes.
      water-live      the flag moves the frame. Three zeros are also what a dead
                      flag, an unparsed argument or a failed program compile
                      produce, so the off-path arms alone prove nothing. Shadows
                      are OFF on both sides so this is not measuring the catcher --
                      see WATER_NO_CATCHER.
      water-absorb    R/B falls monotonically with distance. Fails a constant tint,
                      which is what absorption applied without the path length is.
      water-dry       the emerged ramp is NOT water-tinted. NOTE this does not test
                      the shoreline discard: those water fragments are behind the
                      ramp and GL_LESS rejects them anyway. It tests that nothing
                      tints dry land, which is a weaker but real claim.
      water-fft-det   the spectral path reproduces across two runs, over 45
                      ping-pong passes and three cascades.
      water-fft-live  the spectral surface differs from Gerstner at the SAME extent
                      and level, so the wave model is the only variable. Fails a
                      cascade chain that transformed to zero.
      water-fft-motion the spectral surface reports the motion its waves have. The
                      camera is static, so motion blur can only move a pixel whose
                      velocity is the wave's own -- it moved 0 px before the previous
                      cascades were retained.
      water-caustic   light focusing moves the frame; one flag apart.
      water-submerged absorption is monotone along the submerged INTERFACE, which only
                      that branch produces. A pixel count here would pass on a surface
                      that drew nothing, since raising the level moves 91% of the frame
                      regardless. Shallow and Gerstner for a geometric reason, not a
                      convenient one -- see WATER_SUBMERGED_FLAGS.
      water-under-fog the submerged RAMP fades with distance. It is opaque and has no
                      water interface between it and the eye, so the froxel volume's
                      second medium is the only thing that can absorb it -- which is
                      what rendered as though in air before 11.33.
      water-cscn      the scene file's water block reaches the surface, and the flags
                      override it rather than the reverse. absorption is read because
                      no flag can set it, so the frame can only have moved through the
                      authoring path.
      water-seastate  the spectral sea state reaches the seeding. Wind speed, fetch,
                      depth, peak enhancement and swell are authorable since spec 11.42
                      and NO flag can set any of them, so a scene file is the only way in
                      and this is the only arm on that path. The wind DIRECTION reaches
                      both models now and is covered by water-fft-live moving with it.
      water-glitter   the sea has a specular response to its own sun, and the streak is
                      at least WATER_GLITTER_RATIO_MIN brighter than the same water with
                      the lobe off. Under the procedural sky the environment cubemap
                      carries no disc, so before spec 11.42 there was none at all. The
                      box is DERIVED from the specular point and the horizon
                      (_water_glitter_box), and the bar is a ratio rather than a strict
                      inequality -- until 11.43 it was neither, and the sun override it
                      authors did not reach the engine either.
      water-foam-persist whitewater OUTLIVES the crest that made it. Read against
                      --no-water-foam-history, which selects foam from this frame's fold
                      alone and is the pre-11.42 behaviour exactly. On open water and on a
                      rough sea for reasons that are both in WATER_PERSIST_SEA, in a box
                      whose top edge follows the horizon that raising `level` moves. The
                      ratio carries an absolute floor under it, since 1 -> 3 px is 3.00x
                      on a frame with no whitewater in it, and the arm renders the 90-frame
                      config TWICE: it is the only place the accumulator runs that long
                      and the only determinism claim over it. Reads --water-foam-debug
                      rather than the shaded frame, so it cannot move when a LOOK constant
                      does (spec 11.47).
      water-whitecap-coverage  the sea's whitecap fraction against Monahan &
                      O'Muircheartaigh's published relation, at two wind speeds, read at
                      nadir where pixel fraction of the debug mask is areal fraction
                      exactly. Nothing else in this suite checks the sea against a number
                      that came from outside it.
      water-fft-var   the transformed field carries the variance the SEEDING predicted,
                      per band and in both height and slope. The first arm here that
                      reads the spectrum rather than a picture of it, and the only one
                      that can fail on a transform which is deterministic, differs from
                      Gerstner, and is still wrong. Blind to a missed fftshift, which
                      moves the field in space and not in variance -- that is
                      water-fft-impulse's half.
      water-fft-impulse the transform matches its CLOSED FORM on two single modes: a
                      centred impulse must come back constant, and its neighbour as one
                      cycle across the grid. Run through the same 14 stages and the same
                      twiddle table the sea uses, so it tests this transform rather than
                      a copy. Verified by breaking the shader -- see
                      WATER_FFT_IMPULSE_MAX for what the rest of the suite did then.
      water-horizon   the surface reaches the horizon, asserted as REACH INVARIANCE:
                      multiplying the nominal extent by 14,000 must not move the water's
                      top edge, because an edge already at the vanishing line cannot go
                      higher. Needs no horizon row and no camera parameters. The clipmap
                      fails it by 71 px, which is what it was written against.
      water-fixture-roundtrip the fixture's GENERATOR still produces the fixture, and the
                      key set it authors is the one parse_water reads. No renders: it
                      regenerates into a temp directory and compares text. Exists because
                      gen_water_fixture.py stopped emitting the `water` block when 11.33
                      made that block create the surface, and stayed that way for three
                      specs -- so the docstring's "regenerate with" line was an instruction
                      to strip the water and fail every arm below.
      water-waterline the above/below split is decided PER PIXEL. On a framing where
                      crests close over a camera that is still above the still level, a
                      backfacing fragment takes the sight line as its optical path
                      rather than the depth buffer behind it -- which is air there, so
                      charging it the bed's distance read far too absorbed. An in-frame
                      ratio against foreground the fix cannot touch, so it is exposure-
                      and build-independent. The fixture cannot see this at all and the
                      arm authors its own framing -- see WATER_WATERLINE_BLOCK.
      water-flags     the flags override the scene file, and a NEGATIVE flag neither
                      creates a surface nor loses to a sibling. Read through the probe,
                      which reports what the surface is rather than what it looks like:
                      every case here was a silent defect no rendered frame could show.
      water-cpu       the CPU wave query inverts the horizontal map (an unconverged
                      inversion answers about the wrong point while looking correct),
                      evaluates a real train rather than a plane, and DECLINES on the
                      spectral model instead of returning a flat surface a caller would
                      trust.
      water-shoal     waves shorten over a rising bed, and ONLY over it. Needs
                      --water-bed dome, since every other arm here runs over a bed the
                      vertex stage cannot see. The second half -- open water beyond the
                      dome unchanged -- is what stops a global roughness change passing
                      as shoaling. Both sides run --no-water-surf: see WATER_NO_SURF.
      water-surf      the incident wave the SHORE builds -- a bore that comes in and a
                      swash that runs up -- which neither wave model has. Present over
                      the bed, exactly absent without one, and different between two
                      frames a fraction of a period apart, which is the claim: it moves.
      water-shore-foam the shore band is whitewater where the bed shoals and nowhere
                      else: present in the band, zero at both extremes, and zero in the
                      same box with no bed under it. Run on GERSTNER deliberately --
                      crest foam is FFT-only, so that path is what isolates the shore
                      band from it.
      water-shore-soft the shoreline is antialiased rather than cut off: the frame
                      moves against --no-water-coverage AND the sharpest single-row
                      fall across the waterline gets shallower. The second half is
                      what makes this a softening claim and not just a liveness one.
      water-shore-hard the same flag at ONE sample is 0 px. Alpha-to-coverage has
                      nothing to dither into there, so the shader must fall back to
                      the cutoff -- if it kept the fractional fragment instead, the
                      sliver would be written at full strength.
      water-crack     the projected grid has no HOLE in it, read per pixel rather than
                      as a mean: the closest pixel in each absorption box to the same
                      framing with no water must still be far from it. A crack is one
                      pixel wide and every other arm here averages, so nothing else in
                      this suite can see one.
      water-farfield  the far field is FILTERED rather than aliased, the filtering is
                      confined to the far field, and the slope energy it removes arrives
                      as roughness rather than being dropped. Read against
                      --no-water-lod, which reports a zero footprint and so reaches the
                      unfiltered surface exactly. The direction of the roughness handover
                      is not measurable here -- see WATER_FAR_ROUGH_AUTHORED.
      water-draws     the surface is ONE draw of one instance at the lattice's own
                      triangle count. Integers off the profiler rather than pixels, so
                      it is blind to how the frame looks and sensitive to the projected
                      grid quietly becoming a many-draw clipmap again.
      water-row       the profiler row appears with the flag and is ABSENT without
                      it, which is what a scope opened inside the pass predicate
                      gives.

    Not an arm here, deliberately: "arming the fog volume when submerged cannot leak
    into an above-water frame" is a cross-BUILD claim, and the water golden already is
    that instrument -- it is an above-water frame and it would move. An arm comparing
    two renders of one build cannot see it at all.

    The blind spot this used to record -- no height provider, so shoaling and the
    shore-foam band were dead in every frame -- is closed: `--water-bed dome` installs an
    analytic bed, and water-shoal and water-shore-foam run over it.

    What is still NOT covered, and cannot be from here: nothing compares the CPU wave
    query against the rasterized surface. water-cpu checks the solver against itself.
    The two evaluate the same sum from the same fields with duplicated constants, and
    closing that needs a GPU readback this harness does not have.
    """
    scene = os.path.join(ROOT, "assets", WATER_FIXTURE)
    if not os.path.exists(scene):
        print(f"  water-det    SKIP  ({WATER_FIXTURE} not present)")
        return []

    a = os.path.join(workdir, "water_a.ppm")
    b = os.path.join(workdir, "water_b.ppm")
    off = os.path.join(workdir, "water_off.ppm")
    on_nc = os.path.join(workdir, "water_live_on.ppm")
    for path, extra in ((a, WATER_FLAGS), (b, WATER_FLAGS),
                        (on_nc, WATER_LIVE_ON), (off, WATER_LIVE_OFF)):
        err = render(scene, path, extra)
        if err:
            print(f"  water-det    ERROR render failed: {err.strip()[-200:]}")
            return ["water-det"]

    failures = []

    ae, _ = compare(a, b)
    ok = ae == 0
    print(f"  water-det    {'PASS' if ok else 'FAIL'}  {ae} px between two runs, want 0")
    if not ok:
        failures.append("water-det")

    ae_live, _ = compare(on_nc, off)
    ok = ae_live >= WATER_LIVE_MIN_PX
    print(f"  water-live   {'PASS' if ok else 'FAIL'}  {ae_live} px vs no --water, "
          f"want >={WATER_LIVE_MIN_PX}")
    if not ok:
        failures.append("water-live")

    w, h, pix = _read_ppm(a)
    ratios = [_water_rb(pix, w, h, box) for box in WATER_ABSORB_BOXES]
    steps = [ratios[i] / max(ratios[i + 1], 1e-6) for i in range(len(ratios) - 1)]
    ok = all(s >= WATER_ABSORB_STEP_MIN for s in steps)
    print(f"  water-absorb {'PASS' if ok else 'FAIL'}  "
          f"R/B={'/'.join(f'{r:.4f}' for r in ratios)} "
          f"steps={','.join(f'{s:.2f}x' for s in steps)} want >={WATER_ABSORB_STEP_MIN}x")
    if not ok:
        failures.append("water-absorb")

    dry = _water_rb(pix, w, h, _water_ramp_band(*WATER_DRY_HEIGHTS, 0.44, 0.56))
    ok = dry >= WATER_DRY_RB_MIN
    print(f"  water-dry    {'PASS' if ok else 'FAIL'}  ramp R/B={dry:.4f} "
          f"want >={WATER_DRY_RB_MIN}")
    if not ok:
        failures.append("water-dry")

    fa = os.path.join(workdir, "water_fft_a.ppm")
    fb = os.path.join(workdir, "water_fft_b.ppm")
    fft_ok = True
    for path in (fa, fb):
        err = render(scene, path, WATER_FFT_FLAGS)
        if err:
            print(f"  water-fft-det ERROR render failed: {err.strip()[-200:]}")
            failures.append("water-fft-det")
            # Named, not silently dropped: three arms below hang off this render,
            # and a pass that prints nothing reads as a pass.
            for dropped in ("water-fft-live", "water-caustic", "water-submerged"):
                print(f"  {dropped:<15} SKIP  (spectral render failed)")
            fft_ok = False
            break
    if fft_ok:
        ae_fft, _ = compare(fa, fb)
        ok = ae_fft == 0
        print(f"  water-fft-det {'PASS' if ok else 'FAIL'}  {ae_fft} px between two runs, want 0")
        if not ok:
            failures.append("water-fft-det")

        # The spectral surface must not merely run -- it must produce a DIFFERENT
        # surface from the Gerstner one. A cascade chain that transformed to zero
        # would still be deterministic, and would still pass the arm above.
        # Compared against a Gerstner render at the SAME extent and level, so the
        # wave model is the only thing that moved.
        gref = os.path.join(workdir, "water_gerstner_ref.ppm")
        err = render(scene, gref, WATER_GERSTNER_REF)
        if err:
            # Appended and CONTINUED, not returned. This used to bail, which abandoned
            # the nine arms below it with no output at all -- the exact "a pass that
            # prints nothing reads as a pass" failure the SKIP block above exists to
            # avoid. Only water-shore-foam needs this frame, and it says so itself.
            print(f"  water-fft-live ERROR render failed: {err.strip()[-200:]}")
            failures.append("water-fft-live")
            gref = None
        if gref:
            ae_model, _ = compare(fa, gref)
            ok = ae_model >= WATER_FFT_LIVE_MIN_PX
            print(f"  water-fft-live {'PASS' if ok else 'FAIL'}  {ae_model} px vs gerstner, "
                  f"want >={WATER_FFT_LIVE_MIN_PX}")
            if not ok:
                failures.append("water-fft-live")

    # The spectral surface's own motion, read through the one pass that consumes
    # velocity and nothing else.
    if fft_ok:
        mb = os.path.join(workdir, "water_fft_motionblur.ppm")
        err = render(scene, mb, WATER_FFT_FLAGS + ["--motion-blur"])
        if err:
            print(f"  water-fft-motion ERROR render failed: {err.strip()[-200:]}")
            failures.append("water-fft-motion")
        else:
            ae_mb, _ = compare(fa, mb)
            ok = ae_mb >= WATER_FFT_MOTION_MIN_PX
            print(f"  water-fft-motion {'PASS' if ok else 'FAIL'}  {ae_mb} px under "
                  f"--motion-blur with a static camera, want >={WATER_FFT_MOTION_MIN_PX}")
            if not ok:
                failures.append("water-fft-motion")

    # Caustics, and the submerged side of the interface. Both only exist on the
    # spectral path, so both hang off the FFT render above.
    if fft_ok:
        nc = os.path.join(workdir, "water_nocaustic.ppm")
        err = render(scene, nc, WATER_FFT_FLAGS + ["--no-water-caustics"])
        if err:
            print(f"  water-caustic ERROR render failed: {err.strip()[-200:]}")
            failures.append("water-caustic")
        else:
            ae_c, _ = compare(fa, nc)
            ok = ae_c >= WATER_CAUSTIC_MIN_PX
            print(f"  water-caustic {'PASS' if ok else 'FAIL'}  {ae_c} px vs no caustics, "
                  f"want >={WATER_CAUSTIC_MIN_PX}")
            if not ok:
                failures.append("water-caustic")

        # The submerged interface, measured by what only it produces. Raising the
        # level moves 91% of the frame whatever the underwater branch does, so a
        # pixel-count arm here would pass on a surface that drew nothing, NaN'd, or
        # forgot the normal flip. From below the optical path IS the sight line, so
        # absorption grows with distance along the underside -- the same
        # monotonicity `water-absorb` reads, on the other side of the interface.
        sub = os.path.join(workdir, "water_submerged.ppm")
        err = render(scene, sub, WATER_SUBMERGED_FLAGS)
        if err:
            print(f"  water-submerged ERROR render failed: {err.strip()[-200:]}")
            failures.append("water-submerged")
        else:
            w2, h2, pix2 = _read_ppm(sub)
            under = [_water_rb(pix2, w2, h2, box) for box in WATER_UNDER_BOXES]
            ordered = all(under[i] > under[i + 1] for i in range(len(under) - 1))
            total = under[0] / max(under[-1], 1e-6)
            ok = ordered and total >= WATER_UNDER_TOTAL_MIN
            print(f"  water-submerged {'PASS' if ok else 'FAIL'}  interface "
                  f"R/B={'/'.join(f'{r:.4f}' for r in under)} "
                  f"falling={ordered} end-to-end={total:.2f}x "
                  f"want >={WATER_UNDER_TOTAL_MIN}x")
            if not ok:
                failures.append("water-submerged")

            # The medium on geometry. Separate arm and separate boxes from the one
            # above on purpose: that reads the interface, this reads the opaque ramp
            # behind it, which nothing but the froxel volume can absorb.
            bed = [_water_rb(pix2, w2, h2, box) for box in _water_fog_bed_boxes()]
            bed_ordered = all(bed[i] > bed[i + 1] for i in range(len(bed) - 1))
            bed_total = bed[0] / max(bed[-1], 1e-6)
            ok = bed_ordered and bed_total >= WATER_FOG_BED_TOTAL_MIN
            print(f"  water-under-fog {'PASS' if ok else 'FAIL'}  submerged ramp "
                  f"R/B={'/'.join(f'{r:.4f}' for r in bed)} "
                  f"falling={bed_ordered} end-to-end={bed_total:.2f}x "
                  f"want >={WATER_FOG_BED_TOTAL_MIN}x")
            if not ok:
                failures.append("water-under-fog")

    # The scene file's water block: authored values reach the surface, and the flags
    # override them rather than the other way round.
    absorb_scn = os.path.join(workdir, "water_cscn_absorb.cscn")
    level_scn = os.path.join(workdir, "water_cscn_level.cscn")
    _water_cscn_variant(scene, absorb_scn, WATER_CSCN_ABSORB)
    _water_cscn_variant(scene, level_scn, {"level": WATER_CSCN_LEVEL})
    cscn_absorb = os.path.join(workdir, "water_cscn_absorb.ppm")
    cscn_level = os.path.join(workdir, "water_cscn_level.ppm")
    cli_level = os.path.join(workdir, "water_cli_level.ppm")
    err = render(absorb_scn, cscn_absorb, WATER_PIN)
    if not err:
        err = render(level_scn, cscn_level, WATER_PIN)
    if not err:
        err = render(scene, cli_level, WATER_PIN + ["--water-level", str(WATER_CSCN_LEVEL)])
    if err:
        print(f"  water-cscn   ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-cscn")
    else:
        ae_absorb, _ = compare(a, cscn_absorb)
        ae_level, _ = compare(cscn_level, cli_level)
        ok = ae_absorb >= WATER_CSCN_MIN_PX and ae_level == 0
        print(f"  water-cscn   {'PASS' if ok else 'FAIL'}  authored absorption moves "
              f"{ae_absorb} px (want >={WATER_CSCN_MIN_PX}, no flag can set it); "
              f"authored level vs --water-level {ae_level} px (want 0)")
        if not ok:
            failures.append("water-cscn")

    # The spectral sea state is authorable and reaches the seeding.
    ss_ref_scn = os.path.join(workdir, "water_seastate_ref.cscn")
    ss_calm_scn = os.path.join(workdir, "water_seastate_calm.cscn")
    _water_cscn_variant(scene, ss_ref_scn, WATER_SEASTATE_REF)
    _water_cscn_variant(scene, ss_calm_scn, WATER_SEASTATE_CALM)
    ss_ref = os.path.join(workdir, "water_seastate_ref.ppm")
    ss_calm = os.path.join(workdir, "water_seastate_calm.ppm")
    err = render(ss_ref_scn, ss_ref, WATER_PIN + WATER_NO_CATCHER)
    if not err:
        err = render(ss_calm_scn, ss_calm, WATER_PIN + WATER_NO_CATCHER)
    if err:
        print(f"  water-seastate ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-seastate")
    else:
        ae_sea, _ = compare(ss_ref, ss_calm)
        ok = ae_sea >= WATER_SEASTATE_MIN_PX
        print(f"  water-seastate {'PASS' if ok else 'FAIL'}  authored wind speed moves "
              f"{ae_sea} px (want >={WATER_SEASTATE_MIN_PX}, no flag can set it)")
        if not ok:
            failures.append("water-seastate")

    # The sun's own reflection. Under the procedural sky the environment cubemap carries no
    # disc at all (sky_env_frag), so before this lobe the sea had no specular response to
    # its key light and --no-water-glitter reaches exactly that frame.
    glit_scene = os.path.join(workdir, "water_glitter.cscn")
    _water_glitter_variant(scene, glit_scene)
    glit_on = os.path.join(workdir, "water_glitter_on.ppm")
    glit_off = os.path.join(workdir, "water_glitter_off.ppm")
    err = render(glit_scene, glit_on, WATER_PIN + WATER_NO_CATCHER)
    if not err:
        err = render(glit_scene, glit_off, WATER_PIN + WATER_NO_CATCHER + ["--no-water-glitter"])
    if err:
        print(f"  water-glitter ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-glitter")
    else:
        ae_glit, _ = compare(glit_off, glit_on)
        gw, gh, g_on_pix = _read_ppm(glit_on)
        _, _, g_off_pix = _read_ppm(glit_off)
        glit_box = _water_glitter_box()
        lum_on = _water_box_luma(g_on_pix, gw, gh, glit_box)
        lum_off = _water_box_luma(g_off_pix, gw, gh, glit_box)
        glit_ratio = lum_on / max(lum_off, 1e-6)
        ok = ae_glit >= WATER_GLITTER_MIN_PX and glit_ratio >= WATER_GLITTER_RATIO_MIN
        print(f"  water-glitter {'PASS' if ok else 'FAIL'}  {ae_glit} px vs no glitter "
              f"(want >={WATER_GLITTER_MIN_PX}), streak box {lum_off:.4f} -> {lum_on:.4f} "
              f"= {glit_ratio:.2f}x (want >={WATER_GLITTER_RATIO_MIN}x)")
        if not ok:
            failures.append("water-glitter")

    # Foam outlives the crest that made it. Read on open water, where the fold is the only
    # thing that can select whitewater -- the shore band is a different mechanism and is
    # identically zero here without a bed.
    #
    # THROUGH --water-foam-debug, not the composited image, as of spec 11.47. The luma/R-B
    # classifier this arm used to read was measuring the SAME opacity and colour constants
    # S5 exists to tune, so every time the look moved this arm moved with it for reasons
    # that had nothing to do with persistence -- which is what broke it across 11.42, 11.44
    # and 11.47 in turn. The debug mask has no opacity: it is the crest band the shader
    # selected, before a single look constant touches it, so this arm now decouples from
    # exactly what it should never have depended on.
    foam_scene = os.path.join(workdir, "water_foam.cscn")
    _water_persist_variant(scene, foam_scene)
    foam_on = os.path.join(workdir, "water_foam_on.ppm")
    foam_off = os.path.join(workdir, "water_foam_off.ppm")
    # A third render of the SAME config, because this arm is the only one that runs the
    # accumulator over 90 frames and a running minimum is where drift would show. Every
    # other determinism arm here stops at 30 on the default sea, so nothing else covers it.
    foam_twice = os.path.join(workdir, "water_foam_on_b.ppm")
    foam_debug = WATER_PIN + WATER_NO_CATCHER + WATER_FOAM_DEBUG_ON
    err = render(foam_scene, foam_on, foam_debug, frames=WATER_PERSIST_FRAMES)
    if not err:
        err = render(foam_scene, foam_twice, foam_debug, frames=WATER_PERSIST_FRAMES)
    if not err:
        err = render(foam_scene, foam_off, foam_debug + ["--no-water-foam-history"],
                     frames=WATER_PERSIST_FRAMES)
    if err:
        print(f"  water-foam-persist ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-foam-persist")
    else:
        # Two runs of the SAME debug config, over the full 90-frame accumulator window --
        # see the comment on WATER_PERSIST_FRAMES above for why this is the one arm that
        # has to cover it.
        ae_repeat, _ = compare(foam_on, foam_twice)
        fw, fh, f_on_pix = _read_ppm(foam_on)
        _, _, f_off_pix = _read_ppm(foam_off)
        foam_box = _water_persist_box()
        px_on, _ = _water_foam_debug_px(f_on_pix, fw, fh, foam_box)
        px_off, _ = _water_foam_debug_px(f_off_pix, fw, fh, foam_box)
        # Qualified, like every other headline number in this function (mid_ratio,
        # sd_ratio, sens_ratio): a bare `ratio` is now bound by three separate arms.
        foam_ratio = px_on / max(px_off, 1)
        ok = (foam_ratio >= WATER_PERSIST_RATIO and px_off >= WATER_PERSIST_MIN_FOAM_PX and
              ae_repeat == 0)
        print(f"  water-foam-persist {'PASS' if ok else 'FAIL'}  open-water foam {px_off} "
              f"-> {px_on} = {foam_ratio:.2f}x (want >={WATER_PERSIST_RATIO} on at least "
              f"{WATER_PERSIST_MIN_FOAM_PX} px), {ae_repeat} px across two 90-frame runs "
              f"(want 0)")
        if not ok:
            failures.append("water-foam-persist")

    # Whitecap coverage against Monahan & O'Muircheartaigh, at nadir -- see
    # WATER_COVERAGE_STATES above for why this is the one measurement in the suite read
    # against something that came from outside the engine.
    cov_ok = True
    for wind_speed in WATER_COVERAGE_STATES:
        cov_scene = os.path.join(workdir, f"water_coverage_{int(wind_speed)}.cscn")
        _water_cscn_variant(scene, cov_scene,
                            dict(WATER_COVERAGE_SEA, windSea={"windSpeed": wind_speed}))
        cov_out = os.path.join(workdir, f"water_coverage_{int(wind_speed)}.ppm")
        err = render(cov_scene, cov_out,
                     WATER_PIN + WATER_NO_CATCHER + WATER_FOAM_DEBUG_ON + WATER_COVERAGE_CAM,
                     frames=WATER_COVERAGE_FRAMES)
        if err:
            print(f"  water-whitecap-coverage ERROR render failed at {wind_speed:.0f} m/s: "
                  f"{err.strip()[-200:]}")
            cov_ok = False
            continue
        cw, ch, cov_pix = _read_ppm(cov_out)
        foam_px, sea_px = _water_foam_debug_px(cov_pix, cw, ch)
        measured = foam_px / max(sea_px, 1)
        predicted = WATER_MONAHAN_A * wind_speed ** WATER_MONAHAN_B
        ratio = measured / max(predicted, 1e-9)
        ok = WATER_COVERAGE_RATIO_MIN <= ratio <= WATER_COVERAGE_RATIO_MAX
        print(f"  water-whitecap-coverage {'PASS' if ok else 'FAIL'}  {wind_speed:.0f} m/s: "
              f"measured {measured * 100:.3f}% vs Monahan {predicted * 100:.3f}% = "
              f"{ratio:.2f}x (want {WATER_COVERAGE_RATIO_MIN}x-{WATER_COVERAGE_RATIO_MAX}x)")
        cov_ok = cov_ok and ok
    if not cov_ok:
        failures.append("water-whitecap-coverage")

    # The transform carries the variance the seeding predicted. The first arm in this
    # suite that reads the SPECTRUM rather than a picture of it.
    probe_head, var_rows, impulse = _water_fft_probe(WATER_PIN + ["--water-waves", "fft"])
    probe_cascades = int(probe_head.get("cascades", 0))
    if probe_head.get("available") != "1":
        # The probe's own reason, not this arm's guess at one. Without it a Gerstner
        # surface, an unseeded spectrum and a failed readback are the same empty list.
        print("  water-fft-var FAIL  --water-fft-probe declined: reason="
              f"{probe_head.get('reason', 'it printed no header at all')}")
        failures.append("water-fft-var")
    elif probe_cascades != WATER_CASCADES or len(var_rows) != probe_cascades:
        # Two questions: the build has the cascade count this suite is written against,
        # and the probe printed a row for each of the cascades it says it has.
        print(f"  water-fft-var FAIL  --water-fft-probe reports {probe_cascades} cascades "
              f"(want {WATER_CASCADES}) and printed {len(var_rows)} rows")
        failures.append("water-fft-var")
    else:
        # NOT `hr`/`sr`: `hr` is this function's name for a frame HEIGHT in the width/height
        # pairs a dozen later arms unpack, and a list of floats wearing it is one
        # copy-pasted `h = hr` away from being live.
        height_ratios = [row["height_ratio"] for row in var_rows]
        slope_ratios = [row["slope_ratio"] for row in var_rows]
        ok = all(WATER_FFT_VAR_MIN <= v <= WATER_FFT_VAR_MAX
                 for v in height_ratios + slope_ratios)
        print(f"  water-fft-var {'PASS' if ok else 'FAIL'}  measured/predicted height "
              f"{'/'.join(f'{v:.2f}' for v in height_ratios)} slope "
              f"{'/'.join(f'{v:.2f}' for v in slope_ratios)} "
              f"(want {WATER_FFT_VAR_MIN}-{WATER_FFT_VAR_MAX} on all six)")
        if not ok:
            failures.append("water-fft-var")

    # The transform against its closed form. The only arm in this suite that can fail on
    # a transform which is deterministic, differs from Gerstner, and is still wrong.
    if "dc_err" not in impulse:
        why = ("the probe declined before reaching it"
               if probe_head.get("available") != "1" else "the scratch transform did not run")
        print(f"  water-fft-impulse FAIL  no impulse result: {why}")
        failures.append("water-fft-impulse")
    else:
        dc = impulse.get("dc_err", 1.0)
        mode = impulse.get("mode_err", 1.0)
        ok = dc <= WATER_FFT_IMPULSE_MAX and mode <= WATER_FFT_IMPULSE_MAX
        print(f"  water-fft-impulse {'PASS' if ok else 'FAIL'}  dc {dc:.2e} mode "
              f"{mode:.2e} (want <={WATER_FFT_IMPULSE_MAX} on both)")
        if not ok:
            failures.append("water-fft-impulse")

    # Reach invariance. `off` is the matching dry reference -- same flags, --no-water.
    reach_a = os.path.join(workdir, "water_reach_authored.ppm")
    reach_b = os.path.join(workdir, "water_reach_big.ppm")
    err = render(scene, reach_a, WATER_PIN + WATER_NO_CATCHER)
    if not err:
        err = render(scene, reach_b, WATER_PIN + WATER_NO_CATCHER +
                     ["--water-extent", WATER_REACH_BIG_EXTENT])
    if err:
        print(f"  water-horizon ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-horizon")
    else:
        wr, hr, pix_a = _read_ppm(reach_a)
        _, _, pix_b = _read_ppm(reach_b)
        _, _, pix_dry = _read_ppm(off)
        top_a = _water_top_row(pix_a, pix_dry, wr, hr)
        top_b = _water_top_row(pix_b, pix_dry, wr, hr)
        moved = abs(top_a - top_b)
        # Both must find a surface at all: two misses would report 0 movement and pass.
        found = top_a < hr and top_b < hr
        ok = found and moved <= WATER_REACH_MAX_MOVE_PX
        print(f"  water-horizon {'PASS' if ok else 'FAIL'}  top water row {top_a} -> "
              f"{top_b} of {hr} at extent x{int(float(WATER_REACH_BIG_EXTENT) / 14)}, "
              f"moved {moved} px (want <={WATER_REACH_MAX_MOVE_PX}); surface found "
              f"both sides {found}")
        if not ok:
            failures.append("water-horizon")

    # The fixture's generator still produces the fixture. No renders, no GPU: this is a
    # text comparison, and it is here because the alternative to asserting it is finding
    # out from a stripped water block and twenty-odd red arms.
    gen = os.path.join(ROOT, "assets", "gen_water_fixture.py")
    regen_dir = os.path.join(workdir, "regen")
    os.makedirs(regen_dir, exist_ok=True)
    # `proc`, not `r`: this function uses `r` as a probe-ROW loop variable in several
    # comprehensions, and `r` is the file's universal name for a CompletedProcess. The two
    # meanings only stay apart because Python scopes comprehension targets.
    proc = subprocess.run([sys.executable, gen, regen_dir], capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"  water-fixture-roundtrip FAIL  generator exited {proc.returncode}: "
              f"{(proc.stderr or proc.stdout).strip()[-200:]}")
        failures.append("water-fixture-roundtrip")
    else:
        drifted = []
        for name in ("water_fixture.gltf", WATER_FIXTURE):
            committed = os.path.join(ROOT, "assets", name)
            regenerated = os.path.join(regen_dir, name)
            if not os.path.exists(regenerated):
                drifted.append(f"{name}: not emitted")
                continue
            # NOT `a`/`b`: those are live render paths in this function, and binding file
            # handles to them here fed a TextIOWrapper to compare() several arms later.
            with open(committed) as f_committed, open(regenerated) as f_regenerated:
                if f_committed.read() != f_regenerated.read():
                    drifted.append(name)
        # Three key sets that have to agree, and none of them transcribed here: what
        # parse_water reads, what it will accept without warning, and what the fixture
        # authors. A key added to the parser and not to the fixture leaves the corpus with
        # no coverage of it; a key in the fixture the parser does not read is a typo that
        # authors nothing, which is exactly what get_float cannot distinguish from absence.
        read_keys, known_keys = _cscn_key_sets("parse_water", "water")
        fixture_water = json.load(open(os.path.join(ROOT, "assets", WATER_FIXTURE))).get("water", {})
        fixture_keys = {k for k in fixture_water if not k.startswith("_")}
        if read_keys != known_keys:
            drifted.append(f"parse_water reads {sorted(read_keys ^ known_keys)} "
                           "but its known[] list disagrees")
        if read_keys != fixture_keys:
            drifted.append(f"fixture vs parse_water differ on {sorted(read_keys ^ fixture_keys)}")
        # And the same three-way agreement one level down, for each nested train (spec
        # 11.48). Checking only the outer level would let a train's key set drift freely,
        # which is where the coverage matters most: every one of those eight is unreachable
        # from the command line, so the fixture is the corpus's only exercise of them.
        train_read, train_known = _cscn_key_sets("parse_wave_train", "train")
        if train_read != train_known:
            drifted.append(f"parse_wave_train reads {sorted(train_read ^ train_known)} "
                           "but its known[] list disagrees")
        for train in sorted(_cscn_wave_train_names()):
            authored = {k for k in fixture_water.get(train, {}) if not k.startswith("_")}
            if authored != train_read:
                drifted.append(f"fixture water.{train} vs parse_wave_train differ on "
                               f"{sorted(authored ^ train_read)}")
        # The WRITE side, which the three sets above cannot see. They all describe reading a
        # .cscn; a key parsed, tolerated and authored still reaches nothing if the copy onto
        # WaterWaveTrain forgets it -- and that is silent, because get_float cannot tell a key
        # nobody applied from a key nobody authored, which is the exact defect class 11.48
        # exists to remove. The corpus cannot catch it either: water_fixture authors the
        # library defaults exactly, so every assignment in apply_wave_train is a no-op there.
        apply_has, apply_set = _cscn_wave_train_apply_fields()
        if apply_has != apply_set:
            drifted.append(f"apply_wave_train guards and writes differ on "
                           f"{sorted(apply_has ^ apply_set)}")
        parse_fields = _cscn_wave_train_parse_fields()
        if apply_set != parse_fields:
            drifted.append(f"apply_wave_train vs parse_wave_train differ on "
                           f"{sorted(apply_set ^ parse_fields)}")
        ok = not drifted
        print(f"  water-fixture-roundtrip {'PASS' if ok else 'FAIL'}  regenerated 2 files, "
              f"{len(read_keys)} water keys, {len(train_read)} per train and "
              f"{len(apply_set)} applied, "
              f"{'all identical to the committed pair' if ok else 'DRIFTED: ' + '; '.join(drifted)}")
        if not ok:
            failures.append("water-fixture-roundtrip")

    # The waterline, decided per pixel. The fixture cannot see this -- at amplitude 0.06
    # no crest ever shows its underside, so !gl_FrontFacing is never true there -- so the
    # arm authors a straddling framing of its own and reads it as an in-frame ratio.
    wl_scene = os.path.join(workdir, "water_waterline.cscn")
    _water_cscn_variant(scene, wl_scene, WATER_WATERLINE_BLOCK)
    wl = os.path.join(workdir, "water_waterline.ppm")
    err = render(wl_scene, wl, WATER_PIN + WATER_NO_CATCHER)
    if err:
        print(f"  water-waterline ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-waterline")
    else:
        ww, wh, wpix = _read_ppm(wl)
        crest = _water_box_luma(wpix, ww, wh, WATER_WATERLINE_CREST_BOX)
        ref = _water_box_luma(wpix, ww, wh, WATER_WATERLINE_REF_BOX)
        crest_ratio = crest / max(ref, 1e-9)
        ok = crest_ratio >= WATER_WATERLINE_MIN_RATIO
        print(f"  water-waterline {'PASS' if ok else 'FAIL'}  crest/foreground "
              f"{crest:.4f}/{ref:.4f} = {crest_ratio:.4f} (want "
              f">={WATER_WATERLINE_MIN_RATIO})")
        if not ok:
            failures.append("water-waterline")

    # Flag precedence over the scene file. No pixels: the probe reports what the surface
    # IS, and every case here was a silent defect that a rendered frame could not show.
    prec_fail = []
    for label, extra, waves, want_surface, want_model in WATER_PRECEDENCE:
        variant = os.path.join(workdir, f"water_prec_{waves}.cscn")
        _water_cscn_variant(scene, variant, {"waves": waves})
        head, _ = _water_probe(WATER_PIN + extra, scene=variant)
        got_surface = bool(head)
        if got_surface != want_surface:
            prec_fail.append(f"{label}: surface={got_surface} want {want_surface}")
        elif want_model and head.get("model") != want_model:
            prec_fail.append(f"{label}: model={head.get('model')} want {want_model}")
    ok = not prec_fail
    print(f"  water-flags  {'PASS' if ok else 'FAIL'}  {len(WATER_PRECEDENCE)} precedence "
          f"cases" + ("" if ok else "; " + "; ".join(prec_fail)))
    if not ok:
        failures.append("water-flags")

    # The CPU wave query. Nothing else in this suite can see it.
    head, rows = _water_probe(WATER_PIN)
    fft_head, fft_rows = _water_probe(WATER_PIN + ["--water-waves", "fft"])
    if not rows or not fft_rows:
        print("  water-cpu    FAIL  --water-probe printed nothing")
        failures.append("water-cpu")
    else:
        worst = max(float(r["residual"]) for r in rows)
        heights = [float(r["h"]) for r in rows]
        spread = max(heights) - min(heights)
        upright = all(float(r["n"].split(",")[1]) > 0.5 for r in rows)
        # The spectral model has no CPU answer and must SAY so rather than return a
        # plausible flat surface that a caller would trust.
        fft_flat = (fft_head.get("available") == "0" and
                    all(abs(float(r["h"]) - float(fft_head["level"])) < 1e-6 for r in fft_rows))
        ok = (head.get("available") == "1" and worst <= WATER_PROBE_MAX_RESIDUAL and
              spread >= WATER_PROBE_MIN_SPREAD and upright and fft_flat)
        print(f"  water-cpu    {'PASS' if ok else 'FAIL'}  inverse residual <= "
              f"{worst:.5f} (want <={WATER_PROBE_MAX_RESIDUAL}), height spread "
              f"{spread:.4f} (want >={WATER_PROBE_MIN_SPREAD}), normals upright "
              f"{upright}, spectral declines {fft_flat}")
        if not ok:
            failures.append("water-cpu")

    # Shoaling, which needs the diagnostic bed: every other water arm runs over a bed
    # the vertex stage cannot see, so the whole Tier 3 path was untested.
    # The no-bed reference is `fa`, already on disk: no bed IS the default, so
    # WATER_FFT_FLAGS alone is the no-bed frame and rendering it again under
    # --water-bed none produced a byte-identical duplicate.
    dome = os.path.join(workdir, "water_dome.ppm")
    nobed = os.path.join(workdir, "water_nosurf_nobed.ppm")
    err = None if not fft_ok else render(scene, dome,
                                         WATER_FFT_FLAGS + WATER_DOME_BED + WATER_NO_SURF)
    if not err and fft_ok:
        err = render(scene, nobed, WATER_FFT_FLAGS + WATER_NO_SURF)
    if not fft_ok:
        print("  water-shoal  SKIP  (the spectral render this compares against failed)")
    elif err:
        print(f"  water-shoal  ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-shoal")
    else:
        wd, hd, pixd = _read_ppm(dome)
        wn, hn, pixn = _read_ppm(nobed)
        mid_on = _water_roughness(pixd, wd, hd, WATER_SHOAL_MID_BOX)
        mid_off = _water_roughness(pixn, wn, hn, WATER_SHOAL_MID_BOX)
        open_on = _water_roughness(pixd, wd, hd, WATER_SHOAL_OPEN_BOX)
        open_off = _water_roughness(pixn, wn, hn, WATER_SHOAL_OPEN_BOX)
        mid_ratio = mid_on / max(mid_off, 1e-6)
        open_ratio = open_on / max(open_off, 1e-6)
        ok = (mid_ratio <= WATER_SHOAL_MAX_RATIO and
              abs(open_ratio - 1.0) <= WATER_SHOAL_OPEN_TOL)
        print(f"  water-shoal  {'PASS' if ok else 'FAIL'}  roughness over the bed "
              f"{mid_off:.4f} -> {mid_on:.4f} = {mid_ratio:.2f}x "
              f"(want <={WATER_SHOAL_MAX_RATIO}), beyond it {open_off:.4f} -> "
              f"{open_on:.4f} = {open_ratio:.2f}x (want 1.00 +/-"
              f"{WATER_SHOAL_OPEN_TOL})")
        if not ok:
            failures.append("water-shoal")

    # The surf itself (spec 11.44): the incident wave the shore builds, which no wave model
    # has -- both are downwind-travelling fields that the shoal factor scales to nothing at
    # the sand, so before this the sea met the beach as a still line.
    #
    # Three claims, and the second is the one that matters. It has to arrive ONLY where
    # there is a shore: a surf that moved open water would be a global amplitude knob
    # wearing a beach's name, which is the same trap water-shoal's open box guards. And it
    # has to MOVE -- a bore that stood still would be the painted band this replaces -- so
    # the third claim reads two different frame counts rather than two runs of one.
    surf = os.path.join(workdir, "water_surf.ppm")
    surf_late = os.path.join(workdir, "water_surf_late.ppm")
    surf_open = os.path.join(workdir, "water_surf_open.ppm")
    err = None if not fft_ok else render(scene, surf, WATER_FFT_FLAGS + WATER_DOME_BED)
    if not err and fft_ok:
        err = render(scene, surf_late, WATER_FFT_FLAGS + WATER_DOME_BED, frames=44)
    if not err and fft_ok:
        err = render(scene, surf_open, WATER_FFT_FLAGS)
    if not fft_ok:
        print("  water-surf   SKIP  (the spectral render this compares against failed)")
    elif err:
        print(f"  water-surf   ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-surf")
    else:
        # Against the same bed with the surf off, and against no bed with it off: the first
        # is how much surf there is, the second is whether it stayed where it belongs.
        shore_px, _ = compare(dome, surf)
        open_px, _ = compare(nobed, surf_open)
        moved_px, _ = compare(surf, surf_late)
        ok = (shore_px >= WATER_SURF_MIN_PX and open_px == 0 and
              moved_px >= WATER_SURF_MIN_MOVED)
        print(f"  water-surf   {'PASS' if ok else 'FAIL'}  {shore_px} px over the bed "
              f"(want >={WATER_SURF_MIN_PX}), {open_px} px with no bed (want 0: a surf "
              f"that moved open water is an amplitude knob), {moved_px} px between "
              f"frames 30 and 44 (want >={WATER_SURF_MIN_MOVED}: it has to come IN)")
        if not ok:
            failures.append("water-surf")

    # The shore foam band, isolated by running Gerstner: no crest foam on that path, so
    # whatever whitewater is in the frame is the shore band.
    # Its no-bed reference is `gref` -- already rendered, and no bed is the default.
    gdome = os.path.join(workdir, "water_gerstner_dome.ppm")
    err = None if not gref else render(scene, gdome, WATER_GERSTNER_REF + WATER_DOME_BED)
    if not gref:
        print("  water-shore-foam SKIP  (the gerstner reference render failed)")
    elif err:
        print(f"  water-shore-foam ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-shore-foam")
    else:
        wg, hg, pixg = _read_ppm(gdome)
        wb, hb, pixb = _read_ppm(gref)
        band = _water_foam_px(pixg, wg, hg, WATER_FOAM_BAND_BOX)
        at_open = _water_foam_px(pixg, wg, hg, WATER_FOAM_OPEN_BOX)
        at_shoal = _water_foam_px(pixg, wg, hg, WATER_FOAM_SHOALED_BOX)
        none_band = _water_foam_px(pixb, wb, hb, WATER_FOAM_BAND_BOX)
        ok = (band >= WATER_FOAM_BAND_MIN_PX and at_open == 0 and
              at_shoal >= WATER_FOAM_SWASH_MIN_PX and none_band <= WATER_FOAM_NO_BED_MAX_PX)
        print(f"  water-shore-foam {'PASS' if ok else 'FAIL'}  band {band} px "
              f"(want >={WATER_FOAM_BAND_MIN_PX}), open water {at_open} (want 0) and the "
              f"swash {at_shoal} (want >={WATER_FOAM_SWASH_MIN_PX}: foam has to REACH the "
              f"sand), same box with no bed {none_band} "
              f"(want <={WATER_FOAM_NO_BED_MAX_PX})")
        if not ok:
            failures.append("water-shore-foam")

    # Shoreline coverage, one flag apart at 4x MSAA, and then the same flag at one
    # sample where it must do nothing at all.
    hard = os.path.join(workdir, "water_shore_hard.ppm")
    err = render(scene, hard, WATER_FLAGS + ["--no-water-coverage"])
    if err:
        print(f"  water-shore-soft ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-shore-soft")
    else:
        ae_cov, _ = compare(a, hard)
        shore_window = _water_ramp_band(*WATER_SHORE_HEIGHTS, 0.30, 0.70)
        soft_fall = _water_shore_fall(a, shore_window)
        hard_fall = _water_shore_fall(hard, shore_window)
        ratio = soft_fall / max(hard_fall, 1e-6)
        ok = ae_cov >= WATER_SHORE_MIN_PX and ratio <= WATER_SHORE_FALL_RATIO
        print(f"  water-shore-soft {'PASS' if ok else 'FAIL'}  {ae_cov} px vs cutoff "
              f"(want >={WATER_SHORE_MIN_PX}), sharpest fall {soft_fall:.4f} vs "
              f"{hard_fall:.4f} = {ratio:.3f}x want <={WATER_SHORE_FALL_RATIO}")
        if not ok:
            failures.append("water-shore-soft")

    # The fallback, and it is the reason the shader carries the cutoff at all: at one
    # sample glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE) is a no-op, so a fractional alpha
    # would write the sliver at full strength instead of dropping it. --taa is how the
    # render app reaches one sample headless.
    soft_1s = os.path.join(workdir, "water_shore_1s_soft.ppm")
    hard_1s = os.path.join(workdir, "water_shore_1s_hard.ppm")
    err = render(scene, soft_1s, WATER_FLAGS + ["--taa"])
    if not err:
        err = render(scene, hard_1s, WATER_FLAGS + ["--taa", "--no-water-coverage"])
    if err:
        print(f"  water-shore-hard ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-shore-hard")
    else:
        ae_1s, _ = compare(soft_1s, hard_1s)
        # Presence first, because 0 px is also what two WATERLESS frames read. --taa is
        # the only config in this gate that runs at one sample, so nothing else here
        # establishes that the surface survives it -- and an arm whose pass condition is
        # "identical" must prove there is something to be identical about.
        w1s, h1s, pix1s = _read_ppm(soft_1s)
        present = all(_water_rb(pix1s, w1s, h1s, box) < WATER_DRY_RB_MIN
                      for box in WATER_ABSORB_BOXES)
        ok = ae_1s == 0 and present
        print(f"  water-shore-hard {'PASS' if ok else 'FAIL'}  {ae_1s} px at one sample "
              f"(want 0), water present in all three boxes {present}")
        if not ok:
            failures.append("water-shore-hard")

    # Mesh structure. The draw counts are integers; the crack check is a per-pixel
    # distance from the same frame with no water in it, over the boxes water-absorb
    # reads as means. A crack is one pixel wide and a mean would swallow it.
    clip = os.path.join(workdir, "water_clip.ppm")
    # The background a hole would show is `off` -- the same framing with the surface
    # absent, already rendered for water-live and byte-identical to a fresh one.
    clip_bg = off
    err = render(scene, clip, WATER_CLIP_FLAGS)
    if err:
        print(f"  water-crack  ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-crack")
    else:
        wc, hc, pixc = _read_ppm(clip)
        _, _, pixbg = _read_ppm(clip_bg)
        closest = min(_water_closest_to_background(pixc, pixbg, wc, hc, box)
                      for box in WATER_ABSORB_BOXES)
        ok = closest >= WATER_CRACK_MIN_DELTA
        print(f"  water-crack  {'PASS' if ok else 'FAIL'}  closest px to the no-water "
              f"frame={closest:.4f} want >={WATER_CRACK_MIN_DELTA} (a hole reads 0)")
        if not ok:
            failures.append("water-crack")

    # Far-field filtering, against its own bisect lever. Four renders, because both halves
    # are two-sided: filtering on/off says whether it is live and confined, and the same
    # pair at a second authored roughness says whether the energy went into roughness or
    # was simply dropped.
    far_on = os.path.join(workdir, "water_far_on.ppm")
    far_off = os.path.join(workdir, "water_far_off.ppm")
    far_r_on = os.path.join(workdir, "water_far_rough_on.ppm")
    far_r_off = os.path.join(workdir, "water_far_rough_off.ppm")
    rough_scene = os.path.join(workdir, "water_rough.cscn")
    cscn_copy(scene, rough_scene,
              lambda d: d["water"].update({"roughness": WATER_FAR_ROUGH_AUTHORED}))
    err = render(scene, far_on, WATER_PIN + WATER_NO_CATCHER)
    if not err:
        err = render(scene, far_off, WATER_PIN + WATER_NO_CATCHER + ["--no-water-lod"])
    if not err:
        err = render(rough_scene, far_r_on, WATER_PIN + WATER_NO_CATCHER)
    if not err:
        err = render(rough_scene, far_r_off,
                     WATER_PIN + WATER_NO_CATCHER + ["--no-water-lod"])
    if err:
        print(f"  water-farfield ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-farfield")
    else:
        wf, hf, p_on = _read_ppm(far_on)
        _, _, p_off = _read_ppm(far_off)
        _, _, p_r_on = _read_ppm(far_r_on)
        _, _, p_r_off = _read_ppm(far_r_off)
        sd_on = _water_roughness(p_on, wf, hf, WATER_FAR_BOX)
        sd_off = _water_roughness(p_off, wf, hf, WATER_FAR_BOX)
        sd_ratio = sd_on / max(sd_off, 1e-9)
        near_delta = _water_box_max_delta(p_on, p_off, wf, hf, WATER_FAR_NEAR_BOX)
        sens_on = abs(_water_box_luma(p_on, wf, hf, WATER_FAR_BOX) -
                      _water_box_luma(p_r_on, wf, hf, WATER_FAR_BOX))
        sens_off = abs(_water_box_luma(p_off, wf, hf, WATER_FAR_BOX) -
                       _water_box_luma(p_r_off, wf, hf, WATER_FAR_BOX))
        sens_ratio = sens_on / max(sens_off, 1e-9)
        ok = (sd_ratio <= WATER_FAR_SD_RATIO_MAX and near_delta == 0 and
              sens_ratio <= WATER_FAR_ROUGH_RATIO_MAX)
        print(f"  water-farfield {'PASS' if ok else 'FAIL'}  far-box spread {sd_off:.4f} -> "
              f"{sd_on:.4f} = {sd_ratio:.2f}x (want <={WATER_FAR_SD_RATIO_MAX}), near box "
              f"unchanged {near_delta} (want 0), authored-roughness sensitivity "
              f"{sens_off:.5f} -> {sens_on:.5f} = {sens_ratio:.2f}x (want "
              f"<={WATER_FAR_ROUGH_RATIO_MAX})")
        if not ok:
            failures.append("water-farfield")

    clipt = _profiled_run(workdir, "water_clip", WATER_CLIP_FLAGS + ["--profiler"],
                          fixture=WATER_FIXTURE, size=("400", "300"))
    if clipt is None or "water" not in clipt.get("submit", {}):
        print("  water-draws  FAIL  no water SUBMISSION row")
        failures.append("water-draws")
    else:
        row = clipt["submit"]["water"]
        got = (row["draws"], row["instances"], row["triangles"])
        want = (WATER_GRID_DRAWS, WATER_GRID_INSTANCES, WATER_GRID_TRIANGLES)
        ok = got == want
        print(f"  water-draws  {'PASS' if ok else 'FAIL'}  "
              f"draws/instances/triangles={got[0]}/{got[1]}/{got[2]} want "
              f"{want[0]}/{want[1]}/{want[2]}")
        if not ok:
            failures.append("water-draws")

    on = _profiled_run(workdir, "water_on", WATER_FLAGS + ["--profiler"],
                       fixture=WATER_FIXTURE, size=("400", "300"))
    off_t = _profiled_run(workdir, "water_off",
                          ["--profiler", "--no-water", "--no-auto-exposure", "-E", "1.0"],
                          fixture=WATER_FIXTURE, size=("400", "300"))
    if on is None or off_t is None:
        failures.append("water-row")
    else:
        present = "water" in on["gpu"]
        absent = "water" not in off_t["gpu"]
        ok = present and absent
        print(f"  water-row    {'PASS' if ok else 'FAIL'}  present={present} absent_off={absent}")
        if not ok:
            failures.append("water-row")

    return failures


BEACH_FIXTURE = "beach_fixture.cscn"
BEACH_MESH = "beach_fixture.gltf"
# Bigger than render()'s shared 400x300 because everything here is a RING a few pixels
# wide: the island's waterline is 5.7 world units inside a 60-unit domain, so at the
# shared size a radial walk resamples the same texel and the ring has no width to find.
BEACH_SIZE = ("800", "600")
# The analytic bed is a FLAG and no scene key can ask for it. Without it bedAvailable is
# 0, so nothing shoals and the frame has no surf, no shore foam and no run-up -- the
# turquoise that remains is absorption through the depth buffer, which needs no bed.
# Every arm installs it, because the claim this fixture exists to make is that the DRAWN
# dome and the ANALYTIC one are the same dome.
BEACH_BED = ["--water-bed", "dome"]
# Gerstner throughout, deliberately, and for two reasons rather than one. Crest foam is
# FFT-only by construction, so on this path whatever whitewater is in frame is the SHORE
# band -- the isolation water-shore-foam already uses. And it removes the foam HISTORY,
# which accumulates per CASCADE texel and therefore tiles the world: with the spectral sea
# the bed's effect is measurable out to the horizon, which would make beach-surf-zone read
# as a failure of a claim it is not making.
BEACH_GERSTNER = ["--water-waves", "gerstner"]
# All the way round at 20 degrees. A ring asked about on one aspect is not a ring, and
# asking about every aspect at once is the one thing this fixture can do that the ramp
# cannot -- water_fixture has a single shoreline facing a single way.
BEACH_AZIMUTHS = tuple(range(0, 360, 20))
# The shore band sits SEAWARD of the waterline, because foam is on water and not on sand,
# and close to it. Measured peaks cluster at 5.5-8.1 world units against a waterline the
# mesh puts at 5.67, so this window holds the effect while a bed disagreeing with the mesh
# by more than about a quarter of the island's radius falls out of it.
BEACH_RING_INNER = -1.0
BEACH_RING_OUTER = 2.5
# How far past the window the radial search may still look. Enough that the argmax can land
# outside and fail the arm -- which it does, at the two azimuths below -- and short of the
# open water, where the bed goes on changing the image and can change it by more than the
# shore band does. Measured across the range: 16 of 18 at margins 0.5 and 1.0, 15 at 2.0,
# 14 at 4.0, and 11 from 8.0 out to an unbounded search, which FAILS. So the bound is
# load-bearing and not tidiness.
#
# The brightness argmax this replaced was barely sensitive to the same sweep -- 6 of 18 out to
# margin 2.0 and 5 from 4.0 on -- which says where its failure actually lived. 11.44 bounded
# the ray to keep the SUN out of the search and the bound was worth one azimuth; the CROWN was
# already inside the window's own inner half, where no bound could reach it. See
# _beach_ring_hits.
BEACH_RING_MARGIN = 1.0
# 16 of 18 azimuths put the bed's largest effect in that window. The two that miss are still
# the two the GLITTER crosses, as they were under the brightness statistic, but they now miss
# for the opposite reason and it is worth knowing which: their peak is at r 8.6 and 8.9, just
# seaward of the window, and it is NEGATIVE -- -114 and -86 luma. The bed is not adding foam
# out there, it is SUPPRESSING a specular highlight, because shoaling changes the surface it
# is reflected off. An unsigned argmax counts that as the bed's largest effect and it is one.
# So this stays a count rather than a requirement on every azimuth.
#
# The bar is 13 and has never moved. It was written in 11.44 against a brightness argmax
# that read 16, went red at 9 in 11.45 and 6 by 11.48, and reads 16 again under the
# difference statistic -- the ring was there for all four of those specs and the arm could
# not see it. Left at 13 deliberately: re-deriving a bar from the reading that made it green
# is how an arm stops being able to fail.
BEACH_RING_MIN_ON = 13
# Radii for the turquoise ramp, on azimuths clear of the glitter. World radii on the dome,
# so what depth each stands in comes from the mesh rather than from a screen row that has
# to be re-measured whenever the framing moves.
BEACH_DEPTH_RADII = (7.0, 9.0, 12.0, 16.0, 21.0)
BEACH_DEPTH_AZIMUTHS = (120, 140, 160, 180, 200)
# Measured 1.0757 -> 0.7962 -> 0.4967 -> 0.3194 -> 0.2500 for depths 0.29 to 5.12 m: the
# smallest step is 1.28x, so this floor sits under it and still fails a constant tint.
# Seen to fail: --no-water reads 1.2042 -> 1.2051 over the same five radii, every step
# 1.00x, because what is left is the emissive bed with no column in front of it.
# The ramp deliberately stops at 21: absorption has saturated by 27 (0.2386) and RISES
# again by 33 as sky reflection takes over, which is the same band AGENTS.md warns not to
# read absorption monotonicity through on the ramp fixture.
BEACH_DEPTH_STEP_MIN = 1.15
# A MAGNITUDE, not inequality. Output dither is on by default and puts +/-1 LSB on every
# 8-bit write, so a differing byte is not by itself a claim about the bed; at this
# threshold the innermost reach moves from r 0.25 (noise) to r 3.90 (the band).
BEACH_DIFF_THRESH = 8
# The dry crown, as a fraction of the waterline radius. Measured innermost reach across
# the 18 azimuths is 3.90 to 5.05, i.e. 0.688 of the waterline at worst, so this sits
# inside the closest approach with room and asks for exactly zero.
# Seen to fail: at --water-level 0.5, which puts the still line 0.1 under a crown 0.6
# high, the reach is r 0.20 at EVERY azimuth -- the island is drowned and there is no dry
# crown left for the bound to be about.
BEACH_CROWN_FRACTION = 0.60
# The shore band, in world units either side of the waterline, and how much of it the bed
# has to move. Per-azimuth fractions run 0.525 to 1.000 and the pooled fraction is ~0.9.
#
# The band is deliberately HALF DRY: the waterline is its centre, so the inner half is beach
# above the still line and only moves because the run-up climbs it. That makes it sensitive to
# what the swash paints, and it caught a real defect rather than needing to be loosened for
# one -- an alongshore coordinate with a cut in it fell to 0.556 here, and once the foam was
# aligned to the shore's DIRECTION instead the fraction came back at 0.690.
BEACH_BAND_HALF = 1.0
BEACH_BAND_MIN_FRAC = 0.60
# How much of the run-up ceiling the CPU twin's samples must span, and how far one primary
# period moves them. Both fractions of the ceiling rather than absolute heights, so the arm
# does not need re-tuning every time the fixture's sea state moves. Measured 0.216 and 0.171.
# Half-width of the disc `moved` reads over, in pixels. Small: it is there to span one
# filament of the foam web, not to blur the shore band into its surroundings.
BEACH_MOVED_DISC = 2
SHORE_TWIN_MIN_SPREAD = 0.10
SHORE_TWIN_MIN_DRIFT = 0.05


def _beach_render(out, extra, frames=30):
    cmd = [RENDER, "-m", os.path.join(ROOT, "assets", BEACH_FIXTURE), "-x", "-f", str(frames),
           "-W", BEACH_SIZE[0], "-H", BEACH_SIZE[1], "-S", out] + WATER_PIN + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return r.stdout + r.stderr
    return None


@functools.cache
def _beach_profile():
    """The dome's (radius, height) profile, read out of the fixture's own mesh.

    Cached because _beach_height_at is on the sampling path -- a few thousand points per
    arm, each of which would otherwise re-parse the glTF and re-decode its vertex buffer.

    Read rather than re-derived, for the reason _water_ramp_edges is. This dome's shape
    exists in three places -- render_dome_bed_height in C, gen_beach_fixture.py which built
    the mesh, and whatever a gate assumes -- and a gate that computes it from the formula is
    a third copy that agrees with the other two only until one of them moves. Reading the
    mesh leaves two, and those two are exactly the pair beach-shoreline compares.

    A surface of revolution, so one radius has one height and the ring vertices collapse.
    """
    with open(os.path.join(ROOT, "assets", BEACH_MESH)) as f:
        doc = json.load(f)
    raw = base64.b64decode(doc["buffers"][0]["uri"].split(",", 1)[1])
    prim = next(m["primitives"][0] for m in doc["meshes"] if m["name"] == "beach_dome")
    acc = doc["accessors"][prim["attributes"]["POSITION"]]
    view = doc["bufferViews"][acc["bufferView"]]
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    rings = {}
    for i in range(acc["count"]):
        x, y, z = struct.unpack_from("<3f", raw, base + i * 12)
        rings[round(math.hypot(x, z), 4)] = y
    return sorted(rings.items())


def _beach_height_at(radius):
    profile = _beach_profile()
    for i in range(1, len(profile)):
        if profile[i][0] >= radius:
            (r0, y0), (r1, y1) = profile[i - 1], profile[i]
            t = (radius - r0) / max(r1 - r0, 1e-9)
            return y0 + t * (y1 - y0)
    return profile[-1][1]


def _beach_waterline():
    """The radius where the drawn dome crosses the still level."""
    profile = _beach_profile()
    for i in range(1, len(profile)):
        (r0, y0), (r1, y1) = profile[i - 1], profile[i]
        if y0 >= 0.0 > y1:
            return r0 + (r1 - r0) * y0 / (y0 - y1)
    return profile[-1][0]


def _beach_point(radius, deg):
    """A world point ON the dome at a radius and azimuth."""
    a = math.radians(deg)
    return (radius * math.cos(a), _beach_height_at(radius), radius * math.sin(a))


def _beach_pixel(pix, w, h, project, radius, deg):
    """The pixel a dome point lands on, or None if it falls outside the frame.

    Projected into FRACTIONS first, so the read is independent of the framebuffer scale --
    a Retina context hands back twice the requested pixels.
    """
    sx, sy = project(_beach_point(radius, deg))
    px = int(sx / float(BEACH_SIZE[0]) * w)
    py = int(sy / float(BEACH_SIZE[1]) * h)
    if 0 <= px < w and 0 <= py < h:
        return (py * w + px) * 3
    return None


def _beach_ring_hits(on, off, project, window):
    """How many azimuths put the BED'S LARGEST EFFECT inside `window`.

    The argmax of |with bed - without bed| along a ray outward from the island, rather
    than a threshold on it: a threshold is a second calibration that would need its own
    justification, where the ARGMAX needs none. That much is 11.44's reasoning and it
    still holds -- what changed is the quantity, from brightness to the bed's effect on
    it.

    BRIGHTNESS COULD NOT ANSWER THE QUESTION THE ARM ASKS. The ray crosses the DRY CROWN,
    and sunlit sand and whitewater are the same brightness: measured at 800x600 the crown
    peaks at 580-591 luma against a shore band at 570-607, so the argmax was decided by
    ties of a few codes between two unrelated surfaces. At two azimuths the two peaks were
    EQUAL and the crown won on walk order alone. That is what the reported 6 of 18 was --
    not a missing ring. The band was intact underneath it the whole time, and visible in
    the same rays as a window maximum the bed lifts from 527 to 572 and from 526 to 591.

    11.44 already made this correction at the OUTER end, stopping the ray short of the
    sun's glitter so the arm would stop asking "is the shore brighter than the sun", and
    recorded the reasoning in the constants above. The crown is the same fact at the inner
    end -- but stopping short of it is NOT available, because the window's own inner half
    is dry beach by construction (BEACH_RING_INNER is negative, and deliberately: the
    run-up climbs above the still line). An arm that started its walk at the window edge
    would find sunlit sand just inside that edge -- median argmax 4.81 against a window
    opening at 4.66 -- and report a green 18 of 18 for the wrong reason. Measured.

    Differencing removes both competitors at once, and removes them structurally rather
    than by choosing where to look. The bed does not touch dry sand, so dry sand differences
    to nothing and cannot win: measured over the crown, 853 of 864 samples are bit-identical
    between the two renders and the worst channel anywhere is 1 code, which is the output
    dither's own LSB and an order below the floor. Not an assumption here either -- it is the
    standing assertion of beach-surf-zone below, whose crown samples must read EXACTLY 0
    moved, and that arm is green.

    The magnitude floor is what carries the other half of the claim, "without the bed there
    is no ring". It is not a new calibration: BEACH_DIFF_THRESH is the same LSB-vs-signal
    threshold beach-surf-zone reads, and the peaks here clear it by a wide margin (20 to
    114 against a floor of 8). A build whose bed did nothing produces no peak above dither
    at any azimuth and scores zero.
    """
    w, h, pix = on
    _, _, poff = off
    stop = window[1] + BEACH_RING_MARGIN
    hits = []
    for deg in BEACH_AZIMUTHS:
        best_r, best_d = float("nan"), -1.0
        r = 1.0
        while r < stop:
            o = _beach_pixel(pix, w, h, project, r, deg)
            if o is not None:
                d = abs((pix[o] + pix[o + 1] + pix[o + 2])
                        - (poff[o] + poff[o + 1] + poff[o + 2]))
                if d > best_d:
                    best_d, best_r = d, r
            r += 0.05
        hits.append(window[0] <= best_r <= window[1] and best_d >= BEACH_DIFF_THRESH)
    return sum(hits)


def run_beach_gate(workdir):
    """The shoaling bed is the one the eye can see (spec 11.44 phase 5).

      beach-shoreline  the shore band the ANALYTIC bed produces sits on the waterline the
                       DRAWN mesh has, all the way round. Without the bed there is no ring.
      beach-shoal      the water over that bed reddens toward the shore: R/B rises with
                       decreasing depth across five radii read off the mesh.
      beach-surf-zone  the bed's effect is bounded -- it moves the shore band and never
                       reaches the dry crown.
      shore-twin       the CPU run-up the swash film is driven by keeps the contract it
                       shares with the shader's copy (spec 11.45).

    The water corpus could not see any of this. Both goldens are Gerstner with no bed, so
    crest foam does not exist in them by construction and shore foam is identically zero:
    spec 11.43 landed three foam fixes and moved zero golden pixels. water_fixture's ramp
    can shoal, but it has ONE shoreline facing ONE way, so nothing there can tell a ring
    from a band that happens to cross the frame.

    The claim that needs a mesh is beach-shoreline's. gen_beach_fixture.py duplicates
    render_dome_bed_height on purpose -- the water shoals against the analytic field and
    this mesh is what the eye watches it shoal against -- and a duplicate that drifts would
    leave the surface shoaling against nothing visible, which is a defect no arm reading
    only the water could name.
    """
    scene = os.path.join(ROOT, "assets", BEACH_FIXTURE)
    if not os.path.exists(scene):
        print(f"  beach-shoreline SKIP  ({BEACH_FIXTURE} not present)")
        return []

    failures = []
    on = os.path.join(workdir, "beach_bed.ppm")
    off = os.path.join(workdir, "beach_nobed.ppm")
    err = _beach_render(on, BEACH_GERSTNER + BEACH_BED)
    if not err:
        err = _beach_render(off, BEACH_GERSTNER)
    if err:
        print(f"  beach-shoreline ERROR render failed: {err.strip()[-200:]}")
        return ["beach-shoreline"]

    project = _projector(_cscn_camera(BEACH_FIXTURE),
                         float(BEACH_SIZE[0]), float(BEACH_SIZE[1]))
    w, h, pix = _read_ppm(on)
    wo, ho, poff = _read_ppm(off)
    shore = _beach_waterline()
    window = (shore + BEACH_RING_INNER, shore + BEACH_RING_OUTER)
    hits = _beach_ring_hits((w, h, pix), (wo, ho, poff), project, window)
    ok = hits >= BEACH_RING_MIN_ON
    print(f"  beach-shoreline {'PASS' if ok else 'FAIL'}  waterline at r {shore:.2f} from the "
          f"mesh; the bed's largest effect on the ray lands in [{window[0]:.2f}, "
          f"{window[1]:.2f}] at {hits} of {len(BEACH_AZIMUTHS)} azimuths (want "
          f">={BEACH_RING_MIN_ON}, each clearing {BEACH_DIFF_THRESH} luma so a bed that "
          f"changed nothing scores zero)")
    if not ok:
        failures.append("beach-shoreline")

    # The turquoise ramp, as R/B against depth. Median over azimuths rather than mean: one
    # ray can cross a crest, and a crest is a bright specular sample that no amount of
    # averaging removes from a five-sample set.
    ramp = []
    for radius in BEACH_DEPTH_RADII:
        vals = []
        for deg in BEACH_DEPTH_AZIMUTHS:
            o = _beach_pixel(pix, w, h, project, radius, deg)
            if o is not None and pix[o + 2] > 0:
                vals.append(pix[o] / pix[o + 2])
        if vals:
            ramp.append(sorted(vals)[len(vals) // 2])
    steps = [ramp[i - 1] / max(ramp[i], 1e-6) for i in range(1, len(ramp))]
    ok = len(ramp) == len(BEACH_DEPTH_RADII) and all(s >= BEACH_DEPTH_STEP_MIN for s in steps)
    print(f"  beach-shoal  {'PASS' if ok else 'FAIL'}  R/B "
          f"{' -> '.join(f'{v:.4f}' for v in ramp)} at depths "
          f"{' '.join(f'{-_beach_height_at(r):.2f}' for r in BEACH_DEPTH_RADII)}; smallest "
          f"step {min(steps) if steps else float('nan'):.2f}x (want "
          f">={BEACH_DEPTH_STEP_MIN} at every one)")
    if not ok:
        failures.append("beach-shoal")

    """
    Bounded: the bed moves the shore band and leaves the dry crown exactly alone.

    Read over a small DISC rather than at the single pixel the sample lands on. Whitewater is
    a centimetre-scale web (spec 11.45), so one pixel answers whether that sample happened to
    fall on a filament or in a hole -- which is the pattern's phase and not the bed's effect,
    and it swung the pooled fraction by several per cent between builds that differed only in
    how fine the foam was. A neighbourhood asks the question the arm's name asks.
    """
    def moved(radius, deg):
        o = _beach_pixel(pix, w, h, project, radius, deg)
        if o is None:
            return None
        best = 0
        for dy in range(-BEACH_MOVED_DISC, BEACH_MOVED_DISC + 1):
            for dx in range(-BEACH_MOVED_DISC, BEACH_MOVED_DISC + 1):
                p = o + (dy * w + dx) * 3
                if 0 <= p < len(pix) - 3:
                    best = max(best, max(abs(pix[p + k] - poff[p + k]) for k in range(3)))
        return best > BEACH_DIFF_THRESH

    crown_hits, crown_n, band_hits, band_n = 0, 0, 0, 0
    for deg in BEACH_AZIMUTHS:
        for step in range(2, int(BEACH_CROWN_FRACTION * shore * 10)):
            got = moved(step / 10.0, deg)
            if got is not None:
                crown_n += 1
                crown_hits += 1 if got else 0
        for step in range(21):
            radius = shore - BEACH_BAND_HALF + step * BEACH_BAND_HALF / 10.0
            got = moved(radius, deg)
            if got is not None:
                band_n += 1
                band_hits += 1 if got else 0
    frac = band_hits / max(band_n, 1)
    ok = crown_hits == 0 and frac >= BEACH_BAND_MIN_FRAC
    print(f"  beach-surf-zone {'PASS' if ok else 'FAIL'}  the bed moves {frac:.3f} of the "
          f"shore band within {BEACH_BAND_HALF} unit of the waterline (want "
          f">={BEACH_BAND_MIN_FRAC}, {band_n} samples) and {crown_hits} of {crown_n} samples "
          f"inside r {BEACH_CROWN_FRACTION * shore:.2f} on the dry crown (want exactly 0)")
    if not ok:
        failures.append("beach-surf-zone")

    # THE CPU TWIN, against the contract it shares with the shader, and the film's history
    # window against the march the shader makes over it.
    #
    # The swash film runs on the CPU and is driven by a C copy of shore.glsl's run-up, because
    # a closed form cannot be shared between C and GLSL. What IS shared is the constants
    # (shaders/include/shore_constants.glsl), which removes the drift a hand-mirrored tuning
    # number would otherwise invite.
    #
    # BE EXACT ABOUT WHAT THE FIRST THREE PROPERTIES ESTABLISH. Every number they read comes
    # from the CPU copy, so they check that copy is self-consistent: bounded by its own
    # ceiling, spread across it, and not repeating a period later. They would NOT notice the
    # shader's arithmetic changing. The header used to claim otherwise and it was wrong -- an
    # arm that compares one copy to itself is not a comparison of two.
    #
    # THE WINDOW CHECK IS THE CROSS-FILE ONE, and it is the arm that would have caught the
    # defect this became: the solver records a tip history and the shader marches back over it,
    # and those two spans are sized in different files. They were out by a factor of ninety --
    # a ring covering a fifth of a second against taps reaching back eighteen seconds -- so
    # every tap but the first clamped to the oldest slot and the wet sand collapsed to two
    # tones, which is precisely what the accumulation exists to avoid.
    cmd = [RENDER, "-m", os.path.join(ROOT, "assets", BEACH_FIXTURE), "-x", "-f", "3",
           "-W", "320", "-H", "240", "--shore-probe"] + BEACH_BED + BEACH_GERSTNER
    r = _run(cmd, capture_output=True, text=True)

    def _fields(line):
        # Tagged lines, so the kind is read rather than sniffed from whether a field parses as
        # a number. Same shape as _water_fft_probe's reader.
        parts = line.split()
        return dict(p.split("=", 1) for p in parts[2:] if "=" in p)

    lines = [l for l in (r.stdout + r.stderr).splitlines() if l.startswith("shore-probe ")]
    head = next((_fields(l) for l in lines if l.startswith("shore-probe header")), None)
    window = next((_fields(l) for l in lines if l.startswith("shore-probe window")), None)
    if r.returncode != 0 or not head or not window:
        print(f"  shore-twin   ERROR probe produced nothing: {(r.stdout + r.stderr)[-200:]}")
        failures.append("shore-twin")
        return failures

    ceiling = float(head["ceiling"])
    slope = float(head["slope"])
    edges, repeats = [], []
    for line in lines:
        if not line.startswith("shore-probe sample"):
            continue
        f = _fields(line)
        edges.append(float(f["edge"]))
        repeats.append(float(f["next"]))
    span = float(window["span"])
    taps = float(window["taps"])
    covers = span >= taps
    within = all(0.0 <= e <= ceiling for e in edges)
    spread = (max(edges) - min(edges)) if edges else 0.0
    # The largest |edge - next| over the samples: one period on, an incommensurate sum has
    # moved somewhere else. A single train would land back where it started.
    drift = max((abs(a - b) for a, b in zip(edges, repeats)), default=0.0)
    ok = (len(edges) >= 8 and within and slope > 0.0 and covers and
          spread >= SHORE_TWIN_MIN_SPREAD * ceiling and
          drift >= SHORE_TWIN_MIN_DRIFT * ceiling)
    print(f"  shore-twin   {'PASS' if ok else 'FAIL'}  {len(edges)} samples on a slope of "
          f"{slope:.3f}, all within [0, {ceiling:.4f}]: {within}; spread "
          f"{spread / max(ceiling, 1e-9):.3f} of the ceiling (want "
          f">={SHORE_TWIN_MIN_SPREAD}) and one period on it has moved "
          f"{drift / max(ceiling, 1e-9):.3f} (want >={SHORE_TWIN_MIN_DRIFT}: incommensurate "
          f"trains do not repeat); the film's history spans {span:.2f}s against {taps:.2f}s "
          f"of taps (want >=, or the oldest taps clamp to one slot)")
    if not ok:
        failures.append("shore-twin")

    return failures


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

    --taa is what drops the engine to one sample, and it is load-bearing rather
    than cosmetic: under alpha-to-coverage a fractional alpha is SUPPOSED to
    become fractional sample coverage, so on the 4x MSAA path the kept quad
    legitimately differs from the reference and this invariant does not hold.
    Exposure and tonemap are pinned by the fixture's own .cscn.
    """
    scene = os.path.join(ROOT, "assets", MASK_FIXTURE)
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

    return failures


OVERDRAW_LAYERS = "overdraw_layers.cscn"
OVERDRAW_TILES = "overdraw_tiles.cscn"
# Depth complexity here is an integer by construction, so this absorbs only the
# last-digit rounding of the report's %.2f -- it is not a band for noise.
OVERDRAW_TOLERANCE = 0.02
# A size bar under the effect, paired with a non-overlap test that says nothing
# about size. RECALIBRATED, not relaxed, and the distinction is the whole reason
# this comment is long: the 0.05 it replaces was chosen against an "8-18%" effect
# that was measured inside the profiler's WARM-UP window, where compiling the
# prepass's second program is part of the reading. Fixing the measurement (see
# run_overdraw_gate) moved the quantity, so the bar calibrated against the old
# one had to move with it. Steady state, over a dozen samples, the effect is
# +3% to +14%, and a 5% bar sits INSIDE that range -- it failed a run whose two
# sets of readings did not overlap and whose floor was 0%, which is as clean as
# this measurement gets.
#
# 0.02 is under the measured range with margin and still excludes zero. It is
# also no longer the arm's only defence: a prepass that stopped costing reads ~0%
# AND its readings interleave with the base's, so `separated` fails first. This
# is the backstop for a real-but-trivial effect, not the regression test.
CROSSOVER_MIN = 0.02
# CROSSOVER_SEPARATION is deleted rather than retuned. It required the effect to
# beat three times a MEASURED floor, and there is no multiplier that works: the
# floor is a sample, so the bar it produced was a random variable: 0% to 21%
# against an effect of 5-14%, which failed correct code 7 runs in 8 and still
# failed one in five once the floor was estimated properly. _timing_delta's
# `separated` asks the question the ratio was reaching for -- do the two sets of
# readings overlap -- and needs no constant to ask it.


TERRAIN_FIXTURE = "terrain_fixture.r16"
# The world range the fixture's 0..1 is loaded over. Deliberately not round and
# not symmetric: a range-mapping bug that swapped or scaled the endpoints would
# survive [-1, 1] and cannot survive this.
TERRAIN_RANGE = ("-40", "85")

# The fixture's closed form, RESTATED here rather than imported.
#
# NOT for the reason the LUT group gives. That one imports nothing because
# gen_lut_fixture.py really does write at module scope, so importing it would
# rewrite a committed asset mid-run; this generator writes only inside main()
# under a __main__ guard, and importing it would be perfectly safe. An earlier
# version of this comment claimed otherwise and was simply wrong about the file
# beside it.
#
# The real reason is narrower: the gate should be able to fail when the generator
# and the committed asset disagree, which it cannot if it derives its expectation
# from the generator. The cost is that these constants can drift from the
# generator's -- and terrain-closed-form failing is what that drift looks like.
_T_BASE, _T_AMP, _T_UC, _T_VC, _T_UT, _T_VT = 0.50, 0.24, 3.0, 2.0, 0.12, -0.09
_T_FA, _T_FU, _T_FV = 0.08, 34.0, 29.0


def _terrain_truth(u, v, lo=-40.0, hi=85.0):
    h01 = (_T_BASE
           + _T_AMP * math.sin(_T_UC * math.pi * u) * math.cos(_T_VC * math.pi * v)
           + _T_FA * math.sin(2.0 * math.pi * _T_FU * u) * math.cos(2.0 * math.pi * _T_FV * v)
           + _T_UT * u + _T_VT * v)
    return lo + h01 * (hi - lo)


_TERRAIN_SAMPLE = re.compile(
    r"terrain-height-probe sample x=(-?[\d.]+) z=(-?[\d.]+) h=(-?[\d.]+) fbm=(-?[\d.]+) "
    r"ny=(-?[\d.]+) flow=(-?[\d.]+) deposit=(-?[\d.]+) wear=(-?[\d.]+)")
_TERRAIN_CLAMP = re.compile(
    r"terrain-height-probe clamp x=(-?[\d.]+) z=(-?[\d.]+) h=(-?[\d.]+) "
    r"edge_x=(-?[\d.]+) edge_z=(-?[\d.]+) edge_h=(-?[\d.]+)")
_TERRAIN_MID = re.compile(
    r"terrain-height-probe mid x=(-?[\d.]+) z=(-?[\d.]+) h=(-?[\d.]+)")
_TERRAIN_HEADER = re.compile(
    r"terrain-height-probe header source=(\w+) res=(\d+) extent=([\d.]+) cell=([\d.]+)")
_TERRAIN_DIGEST = re.compile(r"terrain-erosion-probe digest value=([0-9a-f]+)")
_TERRAIN_BUDGET = re.compile(
    r"terrain-erosion-probe budget before=(-?[\d.]+) after=(-?[\d.]+) suspended=(-?[\d.]+) "
    r"closure=(-?[\d.eE+-]+) rel=([\d.eE+-]+)")
_TERRAIN_SAVED = re.compile(
    r"terrain-erosion-probe saved path=(\S+) min=(-?[\d.eE+-]+) max=(-?[\d.eE+-]+)")


def _terrain_run(workdir, tag, extra):
    """One forest run with the terrain probes, returning its parsed rows.

    Two frames, the smallest window the app accepts and no fog: every arm here
    reads printed numbers and none of them looks at a pixel, so any time spent
    rendering is time the sim is not running.
    """
    # No -S: every arm here reads printed numbers and none looks at a pixel, so a
    # screenshot is a full readback and a file write for nothing.
    cmd = [FOREST, "-x", "-f", "2", "-W", "320", "-H", "200", "--no-fog"] + extra
    r = _run(cmd, capture_output=True, text=True)
    text = r.stdout + r.stderr
    if r.returncode != 0:
        print(f"  terrain      ERROR {tag} exited {r.returncode}: {text.strip()[-300:]}")
        return None
    return {
        "text": text,
        "header": _TERRAIN_HEADER.search(text),
        "samples": [tuple(float(g) for g in m.groups())
                    for m in _TERRAIN_SAMPLE.finditer(text)],
        "clamps": [tuple(float(g) for g in m.groups())
                   for m in _TERRAIN_CLAMP.finditer(text)],
        "mids": [tuple(float(g) for g in m.groups())
                 for m in _TERRAIN_MID.finditer(text)],
        "digest": (_TERRAIN_DIGEST.search(text).group(1)
                   if _TERRAIN_DIGEST.search(text) else None),
        "budget": (tuple(float(g) for g in _TERRAIN_BUDGET.search(text).groups())
                   if _TERRAIN_BUDGET.search(text) else None),
        "saved": (_TERRAIN_SAVED.search(text).groups()
                  if _TERRAIN_SAVED.search(text) else None),
    }


def run_terrain_gate(workdir):
    """Heightfield terrain and its erosion bake (spec 11.59 / D6-D8):

      terrain-seed         a field seeded from the fbm agrees with it at the nodes
      terrain-closed-form  a loaded field samples what the fixture painted
      terrain-clamp        outside the domain reads the edge, exactly
      terrain-bicubic      the normal stays smooth across cell boundaries
      terrain-masks        masks come from the SIM, not from the geometry
      terrain-threads      the bake is bit-identical at 1, 3 and 8 workers
      terrain-budget       the sediment budget closes
      terrain-roundtrip    save then load carries the height AND the three masks
      terrain-refuse       a bad heightmap is refused, not half-loaded

    Read as PRINTED NUMBERS rather than as pixels, and that is forced rather than
    chosen: apps/forest is not pixel-deterministic (the Hillaire sky moves ~35,000
    px run to run), and it is the only terrain consumer in the tree. Every claim
    here is about what the height function RETURNS, which no frame can show --
    a field wired to the wrong world scale, transposed, or clamped to zero outside
    its domain all render as perfectly plausible terrain.
    """
    fixture = os.path.join(ROOT, "assets", TERRAIN_FIXTURE)
    if not os.path.exists(FOREST):
        print(f"  terrain-closed-form SKIP  (forest not built)")
        return []
    if not os.path.exists(fixture):
        print(f"  terrain-closed-form SKIP  ({TERRAIN_FIXTURE} not present)")
        return []

    failures = []
    load = ["--heightmap", fixture, "--heightmap-range", TERRAIN_RANGE[0], TERRAIN_RANGE[1],
            "--terrain-height-probe"]
    loaded = _terrain_run(workdir, "loaded", load)
    if loaded is None or not loaded["samples"] or not loaded["header"]:
        print("  terrain-closed-form FAIL  the loaded run printed no probe rows")
        return ["terrain-closed-form"]

    extent = float(loaded["header"].group(3))
    lo, hi = float(TERRAIN_RANGE[0]), float(TERRAIN_RANGE[1])

    # --- terrain-seed --------------------------------------------------------
    # The invariant the whole design rests on, and the only arm that can state it
    # from inside a gate: installing a field must not change what the terrain IS.
    #
    # A byte comparison against a pre-11.59 build is the direct form and no gate
    # can hold one. This is the reachable equivalent -- seed a field from the fbm,
    # run no sim, and require the field and the fbm to agree at the nodes the seed
    # wrote. Both numbers come from ONE process at ONE point, because the probe
    # snaps its samples onto the lattice and two runs would not be asking about
    # the same places. It fails on a transposed seed, a res-vs-res-1 slip either
    # way, and a sampler whose node convention disagrees with the seeder's -- none
    # of which any other arm sees, since they all run against a LOADED field the
    # fbm never touched.
    seeded = _terrain_run(workdir, "seeded", ["--erode", "--erode-res", "257",
                                              "--erode-iterations", "-1",
                                              "--terrain-height-probe"])
    if seeded is None or not seeded["samples"]:
        print("  terrain-seed FAIL  the seeded run printed no probe rows")
        failures.append("terrain-seed")
    else:
        worst = max(abs(h - fbm) for _x, _z, h, fbm, *_ in seeded["samples"])
        span = max(abs(h) for _x, _z, h, *_ in seeded["samples"]) or 1.0
        ok = worst / span < 1e-5
        print(f"  terrain-seed {'PASS' if ok else 'FAIL'}  {len(seeded['samples'])} nodes, "
              f"field vs fbm worst {worst:.6f} against a {span:.1f} range "
              f"({worst / span:.2e} relative, want < 1e-5; a transposed or off-by-one "
              f"lattice reads whole units)")
        if not ok:
            failures.append("terrain-seed")

    # --- terrain-closed-form ------------------------------------------------
    # The bar is one 16-bit code of the loaded range plus the cubic's own error
    # on a smooth field, which at 256 across is far below it. Tight enough that a
    # transpose (which the generator asserts moves >500 codes) cannot hide.
    # The probe snaps these rows onto field NODES, where every correct sampler
    # returns the stored value exactly, so the only error left is quantisation.
    # That is what keeps this bar at a couple of codes while the fixture carries a
    # near-Nyquist band -- read between nodes it would be 76, and a bar loose
    # enough to admit that is a bar that no longer says much.
    step = (hi - lo) / 65535.0
    worst, worst_at = 0.0, None
    for x, z, h, _fbm, _ny, _f, _d, _w in loaded["samples"]:
        u = (x + extent) / (2.0 * extent)
        v = (z + extent) / (2.0 * extent)
        d = abs(h - _terrain_truth(u, v, lo, hi))
        if d > worst:
            worst, worst_at = d, (x, z)
    bar = 3.0 * step
    ok = worst <= bar
    print(f"  terrain-closed-form {'PASS' if ok else 'FAIL'}  {len(loaded['samples'])} samples, "
          f"worst {worst / step:.2f} codes at {worst_at} (want <= {bar / step:.0f}; ground truth "
          f"is painted, so this catches a transpose, a node/centre slip or a range error)")
    if not ok:
        failures.append("terrain-closed-form")

    # --- terrain-clamp -------------------------------------------------------
    # EXACT equality, not a tolerance. The camera really does query out here, and
    # the policy is "read the edge", which is a statement about identical values
    # rather than about nearby ones.
    bad = [(x, z, h, eh) for x, z, h, _ex, _ez, eh in loaded["clamps"] if h != eh]
    ok = bool(loaded["clamps"]) and not bad
    print(f"  terrain-clamp {'PASS' if ok else 'FAIL'}  {len(loaded['clamps'])} out-of-domain "
          f"queries, {len(bad)} differ from their edge point (want 0, exactly; returning 0 or "
          f"extrapolating the cubic both drop or launch the camera)")
    if not ok:
        failures.append("terrain-clamp")

    # --- terrain-bicubic -----------------------------------------------------
    # At CELL MIDPOINTS, against the painted closed form, and both halves of that
    # are the result of the arm failing to falsify twice.
    #
    # It first compared the largest normal step along a scan line to the median,
    # on the theory that a C0 filter concentrates its change at cell boundaries.
    # Replacing Catmull-Rom with linear interpolation moved that from 2.94 to 4.93
    # against an 8.0 bar -- green on the mutation it existed to catch. Rewriting it
    # against the closed form's analytic normal was no better (0.00005 error on the
    # mutation), for a reason worth keeping: terrain_normal_at central-differences
    # over less than one cell, and inside a cell a bilinear surface IS the
    # linearisation that difference estimates, so the normal cannot see the filter
    # at all. Only the VALUE halfway between two nodes can -- every interpolant
    # agrees exactly on the lattice, and a chord is furthest from its curve at the
    # midpoint. The fixture carries a near-Nyquist band so that gap is 458 codes
    # rather than the 5 its first version had.
    mids = loaded["mids"]
    if not mids:
        print("  terrain-bicubic FAIL  the probe printed no midpoint rows")
        failures.append("terrain-bicubic")
    else:
        worst_m, worst_m_at = 0.0, None
        for x, z, h in mids:
            u = (x + extent) / (2.0 * extent)
            v = (z + extent) / (2.0 * extent)
            d = abs(h - _terrain_truth(u, v, lo, hi))
            if d > worst_m:
                worst_m, worst_m_at = d, (x, z)
        ok = worst_m <= 150.0 * step
        print(f"  terrain-bicubic {'PASS' if ok else 'FAIL'}  worst {worst_m / step:.1f} codes "
              f"at a cell midpoint {worst_m_at} (want <= 150; bicubic reads ~53 here and "
              f"bilinear several hundred, where neither a smoothness ratio nor the normal "
              f"can see the difference at all)")
        if not ok:
            failures.append("terrain-bicubic")

    # --- terrain-masks -------------------------------------------------------
    # Two-sided, and the zero half is what makes it mean anything: a mask secretly
    # derived from slope would be non-zero on this loaded field too. Only a mask
    # that comes from a SIM is exactly zero before one has run.
    eroded = _terrain_run(workdir, "eroded", ["--erode", "--erode-res", "192",
                                              "--erode-iterations", "120",
                                              "--terrain-height-probe"])
    if eroded is None or not eroded["samples"]:
        print("  terrain-masks FAIL  the eroded run printed no probe rows")
        failures.append("terrain-masks")
    else:
        pre = max(max(s[5], s[6], s[7]) for s in loaded["samples"])
        flows = [s[5] for s in eroded["samples"]]
        post = max(max(s[5], s[6], s[7]) for s in eroded["samples"])
        spread = max(flows) - min(flows)
        ok = pre == 0.0 and post > 0.2 and spread > 0.2
        print(f"  terrain-masks {'PASS' if ok else 'FAIL'}  un-eroded peak {pre:.6f} (want "
              f"exactly 0 -- a slope-derived mask would not be), eroded peak {post:.4f} and "
              f"flow spread {spread:.4f} (want > 0.2 each; a flat mask is a sim that ran and "
              f"did nothing)")
        if not ok:
            failures.append("terrain-masks")

    # --- terrain-threads -----------------------------------------------------
    # The digest, not the sums: addition hides compensating differences, so two
    # thread counts that disagree cell by cell can still report identical totals.
    # Counts chosen to include ones that do NOT divide the resolution, since a
    # remainder-free split is where a band partition goes wrong.
    digests = {}
    for w in ("1", "3", "8"):
        run = _terrain_run(workdir, f"w{w}", ["--erode", "--erode-res", "96",
                                              "--erode-iterations", "60",
                                              "--erode-workers", w,
                                              "--terrain-erosion-probe"])
        digests[w] = run["digest"] if run else None
    values = set(digests.values())
    ok = len(values) == 1 and None not in values
    print(f"  terrain-threads {'PASS' if ok else 'FAIL'}  " +
          ", ".join(f"{w}={d}" for w, d in digests.items()) +
          " (want one value; 96 is not divisible by 3, which is where a band split slips)")
    if not ok:
        failures.append("terrain-threads")

    # --- terrain-budget ------------------------------------------------------
    # The hydraulic stages only MOVE material, so ground plus suspended load has
    # to return to what it started as. Semi-Lagrangian advection shipped first and
    # leaked 3.06% here while producing terrain that looked entirely correct.
    budget = _terrain_run(workdir, "budget", ["--erode", "--erode-res", "128",
                                              "--erode-iterations", "100",
                                              "--terrain-erosion-probe"])
    if budget is None or budget["budget"] is None:
        print("  terrain-budget FAIL  the run printed no budget row")
        failures.append("terrain-budget")
    else:
        rel = budget["budget"][4]
        ok = rel < 1e-6
        print(f"  terrain-budget {'PASS' if ok else 'FAIL'}  closure {rel:.3e} relative "
              f"(want < 1e-6; float rounding over this many cells lands near 5e-9, and a "
              f"non-conservative transport lands near 3e-2)")
        if not ok:
            failures.append("terrain-budget")

    # --- terrain-roundtrip ---------------------------------------------------
    saved = os.path.join(workdir, "terrain_rt.r16")
    rt = _terrain_run(workdir, "rt", ["--erode", "--erode-res", "128",
                                      "--erode-iterations", "60",
                                      "--erode-save", saved,
                                      "--terrain-erosion-probe", "--terrain-height-probe"])
    if rt is None or rt["saved"] is None or not rt["samples"]:
        print("  terrain-roundtrip FAIL  the bake did not report a saved range")
        failures.append("terrain-roundtrip")
    else:
        _p, smin, smax = rt["saved"]
        back = _terrain_run(workdir, "rtback",
                            ["--heightmap", saved, "--heightmap-range", smin, smax,
                             "--terrain-height-probe"])
        if back is None or len(back["samples"]) != len(rt["samples"]):
            print("  terrain-roundtrip FAIL  the reloaded field printed no matching rows")
            failures.append("terrain-roundtrip")
        else:
            rstep = (float(smax) - float(smin)) / 65535.0
            worst = max(abs(a[2] - b[2]) for a, b in zip(rt["samples"], back["samples"]))
            # The MASKS as well, and this half is why the arm exists in this shape.
            # It first compared heights only, and the save wrote heights only, so a
            # shipping load got the eroded geometry shaded by the slope-and-altitude
            # guess erosion was built to replace -- the feature's own opening failure,
            # arrived at through its own round trip, with every arm green.
            mask_before = max(max(a[5], a[6], a[7]) for a in rt["samples"])
            mask_worst = max(abs(a[i] - b[i])
                             for a, b in zip(rt["samples"], back["samples"]) for i in (5, 6, 7))
            mstep = 1.0 / 255.0
            ok = worst <= 0.5 * rstep and mask_before > 0.2 and mask_worst <= mstep
            print(f"  terrain-roundtrip {'PASS' if ok else 'FAIL'}  height worst "
                  f"{worst / rstep:.3f} of one 16-bit step (want <= 0.5, which is what "
                  f"round-to-nearest owes); masks peak {mask_before:.4f} before (want > 0.2, "
                  f"or there is nothing to carry) and worst {mask_worst / mstep:.3f} of one "
                  f"8-bit step across the trip (want <= 1)")
            if not ok:
                failures.append("terrain-roundtrip")

    # --- terrain-refuse ------------------------------------------------------
    # A headerless format cannot tell a truncated file from a smaller terrain
    # except by the square check, so this is the one thing standing between a
    # damaged download and a silently wrong world.
    truncated = os.path.join(workdir, "terrain_bad.r16")
    with open(fixture, "rb") as src, open(truncated, "wb") as dst:
        dst.write(src.read()[:-6])
    bad = _terrain_run(workdir, "refuse",
                       ["--heightmap", truncated, "--heightmap-range",
                        TERRAIN_RANGE[0], TERRAIN_RANGE[1], "--terrain-height-probe"])
    if bad is None or not bad["header"]:
        print("  terrain-refuse FAIL  the run produced no probe header")
        failures.append("terrain-refuse")
    else:
        named = "not a square" in bad["text"]
        fell_back = bad["header"].group(1) == "analytic"
        ok = named and fell_back
        print(f"  terrain-refuse {'PASS' if ok else 'FAIL'}  truncated file "
              f"{'named' if named else 'NOT named'} in a warning and the source fell back to "
              f"{bad['header'].group(1)} (want analytic; a half-loaded field is worse than "
              f"none, because it renders)")
        if not ok:
            failures.append("terrain-refuse")

    return failures


# ---------------------------------------------------------------------------
# terrain streaming (spec 11.69)
# ---------------------------------------------------------------------------

# The source field these arms stream is WRITTEN HERE, and the committed fixture
# is why: terrain_fixture.r16 is 256 nodes, 255 is odd, so a node-centred grid
# halves zero times and it carries no coarse level at all -- the save refuses it
# by name, and a stream of it would have nothing to fall to. 257 halves seven
# times, and painting it from the SAME closed form keeps one statement of what
# this terrain is rather than adding a second asset to drift from the first.
STREAM_RES = 257
# Stated here rather than read back off the probe, the "grid 34" discipline: a
# gate that takes the design's own numbers from the thing under test cannot fail
# when the design changes underneath it.
STREAM_TILE = 64    # TERRAIN_STREAM_TILE_NODES
STREAM_LEVELS = 7   # 257, 129, 65, 33, 17, 9, 5
STREAM_L0_TILES = 5 # ceil(257 / 64), so 25 tiles cover level 0
# Forced-small residency, the --layers-vt-page-slots 4 idiom. resident-res 32
# leaves levels 0..3 streaming; the region radius is what sizes level 0's own
# window, so it has to come down too or the finest level covers the domain and
# there is nothing to miss.
STREAM_SMALL = ["--terrain-stream-resident-res", "32", "--terrain-stream-window", "2",
                "--terrain-stream-budget", "1", "--region-radius", "60", "--region-span", "40"]
# The walk's two-sided bound, both measured from two runs before being written
# down: 475 tiles read and 4461 kB of window, identical across runs and with an
# identical residency digest -- the walk is deterministic by construction, so
# these are exact numbers rather than samples and the ceiling can sit close.
#
# Deleting the dead-band measures 598. That is a 1.26x separation where 11.67's
# equivalent got 10x, and the reason is worth knowing before trusting this arm
# too far: a scripted walk is the MILD case for a dead-band. It crosses tile
# boundaries monotonically at constant speed, so re-centring every frame lands
# on much the same tile-aligned positions. What a dead-band is really for is a
# camera breathing across one boundary, which no headless arm produces.
STREAM_WALK_LOAD_MAX = 520
STREAM_WALK_WINDOW_KB = 5120

# How far the character's height above the terrain may step between two trace
# rows while it walks.
#
# NOT origin-physics' 0.05, and the difference is the measurement: that arm
# holds a RESTING capsule across a shift, where this one walks a triangle mesh
# and crosses region seams. Measured 0.1200 on the streamed walk and 0.1200 on
# the same walk with the field fully resident -- the same number, which is the
# real content here and what the bar is floored just above. A collider built
# from the fall does not read smoother, it reads adrift: the printed terrain
# column still comes from level 0 while the ground under the capsule does not,
# and the first version of this feature measured 70.03 that way.
STREAM_CLEARANCE_MAX = 0.20

_STREAM_HEADER = re.compile(
    r"terrain-stream-header path=(\S+) res=(\d+) levels=(\d+) tile=(\d+) "
    r"min=(-?[\d.eE+-]+) max=(-?[\d.eE+-]+) ms=([\d.]+)")
_STREAM_PROBE = re.compile(
    r"terrain-stream-probe frame=(\d+) levels=(\d+) l0=(-?\d+),(-?\d+) window_kb=(\d+) "
    r"loaded=(\d+) ensured=(\d+) misses=(\d+) digest=([0-9a-f]{8})")
_STREAM_LEVEL = re.compile(
    r"terrain-stream-probe level idx=(\d+) res=(\d+) window=(\d+) tiles=(\d+) "
    r"resident=(\d+) whole=(\d)")
_STREAM_FALLBACK = re.compile(
    r"terrain-stream-probe fallback x=(-?[\d.]+) z=(-?[\d.]+) h=(-?[\d.eE+-]+) "
    r"expect=(-?[\d.eE+-]+) level=(\d+) resident=(\d)")
_STREAM_SAVED = re.compile(
    r"terrain-stream-probe saved path=(\S+) res=(\d+) levels=(\d+) tile=(\d+) "
    r"min=(-?[\d.eE+-]+) max=(-?[\d.eE+-]+)")


def _stream_source(workdir):
    """The 257-node twin of the committed fixture, painted from _terrain_truth."""
    path = os.path.join(workdir, "stream_src.r16")
    if not os.path.exists(path):
        lo, hi = float(TERRAIN_RANGE[0]), float(TERRAIN_RANGE[1])
        with open(path, "wb") as f:
            for j in range(STREAM_RES):
                row = bytearray()
                for i in range(STREAM_RES):
                    h01 = (_terrain_truth(i / (STREAM_RES - 1), j / (STREAM_RES - 1), lo, hi)
                           - lo) / (hi - lo)
                    row += struct.pack("<H", int(round(max(0.0, min(1.0, h01)) * 65535.0)))
                f.write(row)
    return path


def _stream_run(workdir, tag, extra):
    """One forest run carrying both probes, since the stream rows ride the launch."""
    r = _terrain_run(workdir, tag, extra)
    if r is None:
        return None
    text = r["text"]
    r["sheader"] = _STREAM_HEADER.search(text)
    r["ssaved"] = _STREAM_SAVED.search(text)
    r["probe"] = [{"frame": int(m.group(1)), "levels": int(m.group(2)),
                   "l0": (int(m.group(3)), int(m.group(4))), "window_kb": int(m.group(5)),
                   "loaded": int(m.group(6)), "ensured": int(m.group(7)),
                   "misses": int(m.group(8)), "digest": m.group(9)}
                  for m in _STREAM_PROBE.finditer(text)]
    r["slevels"] = [tuple(int(g) for g in m.groups()) for m in _STREAM_LEVEL.finditer(text)]
    r["fallback"] = [{"x": float(m.group(1)), "z": float(m.group(2)), "h": float(m.group(3)),
                      "expect": float(m.group(4)), "level": int(m.group(5)),
                      "resident": int(m.group(6))}
                     for m in _STREAM_FALLBACK.finditer(text)]
    return r


def run_terrain_stream_gate(workdir):
    """Terrain streaming: a tiled field on disk, windows over it (spec 11.69 / D4):

      terrain-stream-identity   a streamed field answers what the resident one did
      terrain-stream-correct    under eviction pressure it still reads what was painted
      terrain-stream-fallback   a miss reads the coarse level exactly, never a zero
      terrain-stream-refuse     a damaged file is refused whole, not half-loaded
      terrain-stream-walk       windows follow a walk and the reads stay bounded
      terrain-stream-collider   the character stays grounded while tiles churn under it

    Read as printed numbers, for the reason the terrain group states and one
    more: residency is invisible in a frame BY DESIGN. The fall returns a
    smoother surface, not a broken one, so a field that never became resident
    renders as plausible terrain -- which is what makes the anti-vacuity clause
    load-bearing on every arm here rather than a formality.

    The source field is painted at gate time (_stream_source) instead of
    committed. The committed 256-node fixture cannot stand in: 255 is odd, so it
    halves zero times, carries no coarse level, and the save refuses it.
    """
    failures = []
    src = _stream_source(workdir)
    cts = os.path.join(workdir, "stream.cts")

    # --- terrain-stream-identity ---------------------------------------------
    # One launch writes the tiled file and prints the resident field's own
    # answers; the next reads that file back. Comparing the two is the whole
    # arm, and it is a comparison of EVERY probe row rather than a sampled one
    # -- samples on the lattice, mids between nodes and clamps outside the
    # domain each fail differently under a paging bug.
    whole = _stream_run(workdir, "stream_whole",
                        ["--heightmap", src, "--heightmap-range", TERRAIN_RANGE[0],
                         TERRAIN_RANGE[1], "--terrain-height-probe",
                         "--terrain-stream-save", cts])
    streamed = _stream_run(workdir, "stream_read",
                           ["--terrain-stream", cts, "--terrain-height-probe",
                            "--terrain-stream-probe", "1"])
    if whole is None or streamed is None or not whole["ssaved"]:
        print("  terrain-stream-identity FAIL  a run did not produce its rows")
        failures.append("terrain-stream-identity")
    else:
        same = (whole["samples"] == streamed["samples"] and whole["clamps"] == streamed["clamps"]
                and whole["mids"] == streamed["mids"])
        # The anti-vacuity, and it is not a formality: a build whose stream
        # never opened falls back to the analytic terrain, which would differ
        # loudly -- but one that opened and never READ anything would answer
        # from the coarse tail and still look like terrain.
        armed = streamed["sheader"] is not None
        loaded = streamed["probe"][-1]["loaded"] if streamed["probe"] else 0
        levels = int(streamed["sheader"].group(3)) if armed else 0
        tile = int(streamed["sheader"].group(4)) if armed else 0
        ok = same and armed and loaded > 0 and levels == STREAM_LEVELS and tile == STREAM_TILE
        print(f"  terrain-stream-identity {'PASS' if ok else 'FAIL'}  "
              f"{len(streamed['samples'])} samples, {len(streamed['mids'])} midpoints and "
              f"{len(streamed['clamps'])} clamp pairs identical to the resident field: {same}; "
              f"the file opened with {levels} levels at tile {tile} (want {STREAM_LEVELS} and "
              f"{STREAM_TILE}, both stated here rather than read back) and read "
              f"{loaded} tiles (want > 0, or the identity is a stream that never armed)")
        if not ok:
            failures.append("terrain-stream-identity")

    # --- terrain-stream-correct ----------------------------------------------
    # The same painted ground truth the committed fixture is read against, now
    # under a residency too small to hold the domain: the probe walks 25 points
    # across the whole field at a one-tile budget, so tiles are read, dropped
    # and read again underneath it. A wrong tile INDEX is what this catches, and
    # it is the failure mode paging has that nothing else does -- the generator
    # asserts a transpose moves over 500 codes against a 3-code bar.
    small = _stream_run(workdir, "stream_small",
                        ["--terrain-stream", cts, "--terrain-height-probe",
                         "--terrain-stream-probe", "1"] + STREAM_SMALL)
    small2 = _stream_run(workdir, "stream_small2",
                         ["--terrain-stream", cts, "--terrain-height-probe",
                          "--terrain-stream-probe", "1"] + STREAM_SMALL)
    if small is None or small2 is None or not small["samples"]:
        print("  terrain-stream-correct FAIL  a run did not produce its rows")
        print("  terrain-stream-fallback FAIL  (same run)")
        failures += ["terrain-stream-correct", "terrain-stream-fallback"]
    else:
        lo, hi = float(TERRAIN_RANGE[0]), float(TERRAIN_RANGE[1])
        extent = float(small["header"].group(3))
        step = (hi - lo) / 65535.0
        worst, worst_at = 0.0, None
        for x, z, h, _fbm, _ny, _f, _d, _w in small["samples"]:
            u = (x + extent) / (2.0 * extent)
            v = (z + extent) / (2.0 * extent)
            d = abs(h - _terrain_truth(u, v, lo, hi))
            if d > worst:
                worst, worst_at = d, (x, z)
        bar = 3.0 * step
        # Determinism folded in here rather than given an arm of its own: two
        # runs of one build must agree on every row a reader can see, which is
        # the stream rows AND the heights they produced.
        deterministic = (small["samples"] == small2["samples"]
                         and small["mids"] == small2["mids"]
                         and [p["digest"] for p in small["probe"]]
                         == [p["digest"] for p in small2["probe"]])
        # More tiles read than level 0 HAS is the refill proof: 25 cover it, so
        # a 26th read is one that had to come back after being dropped.
        read = small["probe"][-1]["loaded"] if small["probe"] else 0
        # The residency this arm claims to be measuring, asserted rather than
        # assumed: a forced-small run must produce a MIXED pyramid. All-whole is
        # the identity configuration, where every number above is true and none
        # of it is about streaming.
        mixed = (any(r[5] == 1 for r in small["slevels"])
                 and any(r[5] == 0 for r in small["slevels"]))
        ok = (worst <= bar and deterministic and mixed
              and read > STREAM_L0_TILES * STREAM_L0_TILES)
        print(f"  terrain-stream-correct {'PASS' if ok else 'FAIL'}  "
              f"{len(small['samples'])} samples under a 1-tile budget, worst "
              f"{worst / step:.2f} codes at {worst_at} (want <= {bar / step:.0f}: a tile index "
              f"off by one moves hundreds); two runs identical: {deterministic}; "
              f"{sum(1 for r in small['slevels'] if r[5] == 0)} of "
              f"{len(small['slevels'])} levels streaming (want a mix, or this is the identity "
              f"config wearing the arm's name); {read} tiles read against "
              f"{STREAM_L0_TILES * STREAM_L0_TILES} in level 0 (want more, or nothing was "
              f"ever re-read)")
        if not ok:
            failures.append("terrain-stream-correct")

        # --- terrain-stream-fallback -----------------------------------------
        # Rides the same launch. The fallback rows are the only ones that do NOT
        # ensure, so they are the only place the miss policy is observable: `h`
        # asks for level 0 and falls, `expect` asks for the level it settled on
        # and does not. Equal only if the fall returns that level's own value.
        rows = small["fallback"]
        missed = [r for r in rows if r["resident"] == 0]
        bad = [r for r in rows if r["h"] != r["expect"]]
        spread = (max(r["expect"] for r in rows) - min(r["expect"] for r in rows)) if rows else 0.0
        misses = small["probe"][-1]["misses"] if small["probe"] else 0
        ok = bool(rows) and not bad and bool(missed) and misses > 0 and spread > 0.0
        print(f"  terrain-stream-fallback {'PASS' if ok else 'FAIL'}  {len(rows)} probe points, "
              f"{len(bad)} disagree with the level they fell to (want 0, exactly: a zero fill, "
              f"a clamp to the window or the wrong plane all separate them); {len(missed)} sat "
              f"outside level 0's window and the run logged {misses} falls (want > 0 each, or "
              f"nothing missed); coarse spread {spread:.2f} (want > 0, or the fallback is flat)")
        if not ok:
            failures.append("terrain-stream-fallback")

    # --- terrain-stream-refuse -----------------------------------------------
    # The terrain-refuse discipline on the new format. Forced small so the
    # refusal is exercised in the configuration the feature actually runs in,
    # where most of the file is never read at open.
    #
    # What holds this arm is NOT the manifest length check, and the mutation
    # round is how that was learned: deleting the check leaves it green in every
    # configuration tried. The coarse levels sit at the END of the layout and
    # are read whole whatever the residency, so a truncated tail always fails a
    # READ first. The check is defence in depth against a file whose header and
    # length disagree without the tail being touched; what this arm actually
    # pins is the refusal path itself -- that a failed read refuses the whole
    # field instead of installing a half-loaded one.
    trunc = os.path.join(workdir, "stream_bad.cts")
    with open(cts, "rb") as fsrc, open(trunc, "wb") as fdst:
        blob = fsrc.read()
        fdst.write(blob[:len(blob) - 4096])
    bad_run = _stream_run(workdir, "stream_bad",
                          ["--terrain-stream", trunc, "--terrain-height-probe"] + STREAM_SMALL)
    if bad_run is None or not bad_run["header"]:
        print("  terrain-stream-refuse FAIL  the run produced no probe header")
        failures.append("terrain-stream-refuse")
    else:
        named = "terrain_stream" in bad_run["text"] and trunc in bad_run["text"]
        fell_back = bad_run["header"].group(1) == "analytic"
        ok = named and fell_back
        print(f"  terrain-stream-refuse {'PASS' if ok else 'FAIL'}  truncated file "
              f"{'named' if named else 'NOT named'} in a warning and the source fell back to "
              f"{bad_run['header'].group(1)} (want analytic; a half-loaded field is worse than "
              f"none, because it renders)")
        if not ok:
            failures.append("terrain-stream-refuse")

    # --- terrain-stream-walk / terrain-stream-collider -----------------------
    # One walk, two arms, in the region group's own idiom: residency follows the
    # camera, the camera follows the player, and --trace-player rides along so
    # "the windows moved" and "the ground stayed under the character" are read
    # off the same 400 frames rather than two runs that merely shared flags.
    #
    # A 513 field rather than the 257 above, because this needs level 0 bigger
    # than a window that can plausibly hold it while still baking in under a
    # second: --erode-iterations -1 seeds and installs, running no sim.
    walk_cts = os.path.join(workdir, "walk.cts")
    seeded = _stream_run(workdir, "stream_walk_bake",
                         ["--erode", "--erode-res", "513", "--erode-iterations", "-1",
                          "--terrain-stream-save", walk_cts])
    if seeded is None or not seeded["ssaved"]:
        print("  terrain-stream-walk FAIL  the 513 field did not save")
        print("  terrain-stream-collider FAIL  (same run)")
        failures += ["terrain-stream-walk", "terrain-stream-collider"]
        return failures

    _, _, walk_text = _region_run(REGION_CHURN + REGION_WALK +
                                  ["--terrain-stream", walk_cts,
                                   "--terrain-stream-resident-res", "64",
                                   "--terrain-stream-window", "4",
                                   "--terrain-stream-probe", "50"])
    rows = [{"frame": int(m.group(1)), "l0": (int(m.group(3)), int(m.group(4))),
             "window_kb": int(m.group(5)), "loaded": int(m.group(6)), "digest": m.group(9)}
            for m in _STREAM_PROBE.finditer(walk_text)]
    if len(rows) < 4:
        print(f"  terrain-stream-walk FAIL  the probe printed {len(rows)} rows, want at least 4")
        failures.append("terrain-stream-walk")
    else:
        moved = len({r["l0"] for r in rows}) > 1
        grew = rows[-1]["loaded"] > rows[0]["loaded"]
        # The thrash ceiling, the 71/701/200 template. A walk both churns and
        # settles, so the bound is two-sided: growth proves the windows follow,
        # the ceiling proves they are not re-reading ground they already hold.
        no_thrash = rows[-1]["loaded"] <= STREAM_WALK_LOAD_MAX
        capped = all(r["window_kb"] <= STREAM_WALK_WINDOW_KB for r in rows)
        ok = moved and grew and no_thrash and capped
        print(f"  terrain-stream-walk {'PASS' if ok else 'FAIL'}  level 0's window took "
              f"{len({r['l0'] for r in rows})} distinct positions over the round trip (want > 1: "
              f"it follows the walk); reads went {rows[0]['loaded']} -> {rows[-1]['loaded']} "
              f"(want growth, and <= {STREAM_WALK_LOAD_MAX} -- the dead-band deleted re-reads a "
              f"column a frame); windows held <= {STREAM_WALK_WINDOW_KB} kB at every print: "
              f"{capped}")
        if not ok:
            failures.append("terrain-stream-walk")

    # --- terrain-stream-collider ---------------------------------------------
    # The hazard this whole design is arranged around. A collider is built from
    # heights and handed to Jolt, which keeps its own copy: if the mesh under
    # the player were ever built from the fall, the floor would step by the
    # difference between two pyramid levels and the capsule would sink or hover.
    # Read as the clearance origin-physics already reads, against its bound.
    trace = [{"t": float(m.group(1)), "x": float(m.group(2)), "y": float(m.group(3)),
              "grounded": m.group(5), "terrain": float(m.group(6))}
             for m in _ORIGIN_TRACE.finditer(walk_text)]
    settled = trace[1:]
    if len(settled) < 3:
        print(f"  terrain-stream-collider FAIL  {len(settled)} settled trace rows, want 3+")
        failures.append("terrain-stream-collider")
    else:
        clearances = [r["y"] - r["terrain"] for r in settled]
        drift = max(abs(b - a) for a, b in zip(clearances, clearances[1:]))
        grounded = all(r["grounded"] == "1" for r in settled)
        # Anti-vacuity, and it is the whole point of riding this launch: the
        # character has to have stayed grounded WHILE tiles were being read
        # under it, not on a run where residency never moved.
        churned = bool(rows) and rows[-1]["loaded"] > rows[0]["loaded"]
        ok = grounded and drift <= STREAM_CLEARANCE_MAX and churned
        print(f"  terrain-stream-collider {'PASS' if ok else 'FAIL'}  {len(settled)} settled "
              f"rows, grounded at every one: {grounded}; worst clearance step {drift:.3f} "
              f"(want <= {STREAM_CLEARANCE_MAX}, the same 0.120 a fully resident walk measures: "
              f"a collider built from the fall steps by the gap between two levels); "
              f"the same run read "
              f"{rows[-1]['loaded'] - rows[0]['loaded']} tiles while it walked (want > 0, or "
              f"nothing was streaming)")
        if not ok:
            failures.append("terrain-stream-collider")

    return failures


def run_overdraw_gate(workdir):
    """Depth complexity, against a scene whose answer is known (spec 11.31).

    apps/forest cannot pin this instrument and never could. Its reading moves
    with the AA mode, the draw order and the scatter, so `overdraw-probe` was
    reading 1.05 against a > 1.0 bar for those reasons rather than because the
    measurement was marginal. N stacked full-frame opaque quads submitted
    far-to-near are covered exactly N times, which is an integer with no noise
    floor -- and the same scene sorted, or prepassed, must collapse to 1.

    This is the fixture spec 11.30 listed in its own Files table and never
    wrote, and building it immediately found a real defect: the budget was
    published from engine->msaa_samples, but at the time a 1-sample request was
    allocated multisample and this driver rounded it up to 2, so every reading
    on the TAA path was exactly double -- a single full-frame quad read 2.00.
    (11.34 has since made a 1-sample request genuinely single-sample.)

    THE FIRST TWO ARMS HERE ARE INTEGERS AND THE THIRD IS A CLOCK, and only the
    third has ever been red. Five specs (11.36, 11.38, 11.39, 11.40, 11.73)
    recorded `prepass-crossover` failing and each worked around it by re-running,
    which is the right move on a flake and was the wrong description of this one:
    it was red 7 runs in 8 on correct code, including on a commit that predated
    the change being blamed. It read its noise floor off a single repeat render,
    so the bar it had to clear was itself a random variable. Fixed by rendering
    each config three times -- see `_timing_delta` for why the estimator is the
    fastest run, and the table at the bottom of this function for the eight
    samples that establish it.
    """
    layers_scene = os.path.join(ROOT, "assets", OVERDRAW_LAYERS)
    if not os.path.exists(layers_scene):
        print(f"  overdraw-exact SKIP  ({OVERDRAW_LAYERS} not present)")
        return []

    failures = []
    # Read back from the fixture, not mirrored: the quad count IS the expected
    # complexity, so a regenerated fixture has to move the assertion with it.
    # _fixture_mesh_nodes rather than a private reader, and it is also the more
    # correct question -- what gets drawn is a node count, which stays right if
    # the generator ever shares one quad mesh across N nodes.
    want = float(_fixture_mesh_nodes(os.path.splitext(OVERDRAW_LAYERS)[0] + ".gltf"))
    taa = ["--taa", "--headless-jitter"]
    stacked = _profiled_run(workdir, "od_stack", taa + ["--no-sort-opaque"],
                            fixture=OVERDRAW_LAYERS, size=("400", "300"))
    sorted_run = _profiled_run(workdir, "od_sorted", taa,
                               fixture=OVERDRAW_LAYERS, size=("400", "300"))
    pre = _profiled_run(workdir, "od_pre", taa + ["--no-sort-opaque", "--depth-prepass"],
                        fixture=OVERDRAW_LAYERS, size=("400", "300"))
    # All three, not just the first. `_profiled_run` stores None when the SHADING
    # block did not print, so guarding one and indexing the others raises
    # TypeError and takes the whole suite down instead of failing one arm.
    if any(t is None or not t["shading"] for t in (stacked, sorted_run, pre)):
        print("  overdraw-exact FAIL  a run produced no SHADING block")
        print("  overdraw-culled FAIL  (same)")
        return ["overdraw-exact", "overdraw-culled"]

    got = stacked["shading"]["complexity"]
    ok = abs(got - want) <= OVERDRAW_TOLERANCE
    print(f"  overdraw-exact {'PASS' if ok else 'FAIL'}  {want:.0f} stacked quads read "
          f"{got:.2f} (want {want:.2f} +/- {OVERDRAW_TOLERANCE}; every layer covers the "
          f"frame, so the count is exact)")
    if not ok:
        failures.append("overdraw-exact")

    # The same scene, twice, by the two mechanisms that exist to remove overdraw.
    # Both must reach 1: an arm that only watched the number FALL would pass on
    # a sort that merely reordered a little.
    s = sorted_run["shading"]["complexity"]
    p = pre["shading"]["complexity"]
    ok = abs(s - 1.0) <= OVERDRAW_TOLERANCE and abs(p - 1.0) <= OVERDRAW_TOLERANCE
    print(f"  overdraw-culled {'PASS' if ok else 'FAIL'}  sorted {s:.2f}, prepassed {p:.2f} "
          f"(want both 1.00 +/- {OVERDRAW_TOLERANCE}, from {want:.0f})")
    if not ok:
        failures.append("overdraw-culled")

    # --- prepass-crossover: the case the prepass should LOSE ----------------
    # 11.30 went looking for this with instancing_fixture, expected it to lose
    # at complexity 0.71 and measured it winning by 10%, and recorded that as an
    # absence of evidence. Here the geometry is explicit about it: complexity
    # exactly 1.00, hundreds of separate meshes so nothing batches, and not one
    # fragment for the prepass to reject. It can only cost.
    #
    # A timing arm, so it measures its own floor first -- the only honest way to
    # claim a delta, and what gpu-scale does two gates up.
    tiles = os.path.join(ROOT, "assets", OVERDRAW_TILES)
    if not os.path.exists(tiles):
        print(f"  prepass-crossover SKIP  ({OVERDRAW_TILES} not present)")
        return failures

    # 800x600, and the smaller size was tried and reverted. At 400x300 the
    # opaque row reads ~1.7 ms, where the GPU timer's own jitter dwarfs the
    # separation this arm asserts -- it went from +15% passing to failing on an
    # unchanged renderer. A timing arm's headroom is the thing it is for; three
    # seconds of suite time is not worth trading for it.
    #
    # 150 FRAMES, NOT THE SHARED 45, and this is the half of the fix that is
    # about correctness rather than about noise. profiler_report publishes the
    # last LATCHED window and 45 frames is one window here, so this arm was
    # reading warm-up -- see _gpu_cmd. At 150 the published window is steady
    # state, which both stops the spread (6.4% and a 36% outlier -> 3.0% and
    # none) and shrinks the quantity to what it really is: the prepass costs
    # +4% to +5% at steady state, where inside the warm-up window it read +8% to
    # +15% because compiling its second program was part of the measurement.
    #
    # THREE RENDERS OF EACH CONFIG, INTERLEAVED A/B/A/B/A/B. Two separate
    # reasons, and the interleave is the one this tree already had a rule for.
    #
    # Three, because interference is one-sided and one sample of a config is a
    # coin flip on whether that config got a fair reading.
    #
    # Interleaved, because this machine's GPU drifts over the seconds a gate arm
    # takes -- clock and thermal state, which no amount of averaging WITHIN a
    # run touches. Run flat (three bases, then three variants) the drift lands
    # entirely on the comparison: the base collects the cold samples and the
    # variant the warm ones, or the reverse, and the arm reports the drift as the
    # effect. Measured that way here, the base minimum moved 5.40 -> 4.95 ms
    # between a standalone render and the same render taken first inside the arm.
    # AGENTS.md states the rule for reading forest's timings and it applies to
    # every clock in the suite: interleave, and take each config's floor from
    # samples adjacent in time.
    runs, on_runs = [], []
    for i in range(1, 5):
        runs.append(_profiled_run(workdir, f"od_t{i}", taa + ["--no-sort-opaque"],
                                  fixture=OVERDRAW_TILES, size=("800", "600"), frames="150"))
        on_runs.append(_profiled_run(workdir, f"od_p{i}",
                                     taa + ["--no-sort-opaque", "--depth-prepass"],
                                     fixture=OVERDRAW_TILES, size=("800", "600"), frames="150"))
    if any(t is None for t in runs + on_runs):
        return failures + ["prepass-crossover"]
    base = runs[0]

    timing = _timing_delta([t["gpu"] for t in runs], [t["gpu"] for t in on_runs])
    if timing is None:
        print("  prepass-crossover FAIL  no usable opaque row to compare")
        return failures + ["prepass-crossover"]
    b, o, cost, noise, separated = timing
    #
    # THIS ARM WAS RED 7 RUNS IN 8 ON CORRECT CODE and the three changes above --
    # steady-state frames, interleaving, four samples -- plus `separated` here are
    # what that cost to fix. The claim was never in doubt: measured across every
    # sample taken while diagnosing it, on this commit and on the one before,
    # the prepass cost time EVERY time, +3% to +15%. Only the verdict wobbled.
    #
    # Three faults, in the order they were found, because each hid the next:
    #
    # 1. The floor came off a SINGLE repeat render, so the bar was a random
    #    variable. Two runs measured the identical +8% effect and disagreed --
    #    one floor drew 1%, the other 4%.
    # 2. -f 45 is ONE latch window, so the published number was WARM-UP: program
    #    compilation, pipeline creation, clock ramp. That is also why the effect
    #    looked like +8-15% when at steady state it is +4-5% -- compiling the
    #    prepass's second program was part of what the short run timed.
    # 3. All the base renders ran before all the variant renders, so GPU clock
    #    drift over the arm's own runtime landed on the comparison. This tree
    #    already had the rule (AGENTS.md, on reading forest's timings): A/B/A/B.
    #
    # 4. And then `cost >= 3 * noise` was still wrong, which is why it is gone.
    #    Three times a floor is a statement about a distribution nobody sampled;
    #    when the floor happened to draw 5% it demanded a 15% effect of a
    #    renderer whose real effect is 5-14%, and a correct build failed one run
    #    in five with all three fixes above already in. `separated` replaces it:
    #    the prepass's FASTEST run must be slower than the base's SLOWEST, so the
    #    two sets of readings do not overlap and no assignment of the noise to
    #    either config explains the gap. No constant, and unlike a multiplier it
    #    gets STRICTER as samples are added.
    #
    # FALSIFIED BY HAND, because an arm this heavily rebuilt has to be shown able
    # to fail: making `--depth-prepass` set the engine flag FALSE, so the pass is
    # requested and never runs, takes this red 3 times in 3 -- and `separated` is
    # what catches it, not the size bar. Those runs read +3%, -1% and +1%, so the
    # first of them CLEARED CROSSOVER_MIN on noise alone. What no-op geometry
    # cannot do is stop the two sets of readings from interleaving.
    #
    # Which is the argument for keeping both, in this order: `separated` is the
    # regression test, and CROSSOVER_MIN only excludes an effect too small to
    # care about. Reading it the other way round -- the bar as the real guard --
    # is what made 0.05 look load-bearing when it was calibrated on warm-up.
    #
    # The premise, asserted rather than left in a comment: this fixture has
    # nothing to reject. If it ever gained overlap the arm would quietly be
    # measuring a different scene.
    flat = base["shading"] and abs(base["shading"]["complexity"] - 1.0) <= OVERDRAW_TOLERANCE
    ok = cost >= CROSSOVER_MIN and separated and flat
    print(f"  prepass-crossover {'PASS' if ok else 'FAIL'}  opaque {b:.3f} -> {o:.3f} ms with "
          f"the prepass, {cost * 100.0:+.0f}% (want >= +{CROSSOVER_MIN * 100.0:.0f}%: nothing "
          f"to reject at complexity 1.0) over {len(runs)} interleaved pairs; the two sets of "
          f"readings do not overlap: {separated}; {noise * 100.0:.0f}% floor between the two "
          f"fastest base runs; complexity 1.0: {flat}")
    if not ok:
        failures.append("prepass-crossover")

    return failures


RAIDEN = os.path.join(ROOT, "my_models", "raiden", "source", "raiden_textured_rigged.glb")


def _raiden_render(out, extra):
    """The AGENTS.md baseline recipe, which is 0 px run-to-run.

    Here rather than in a gate because it is the corpus's only ALPHA_MASK +
    ALPHA_BLEND subject: every other fixture is opaque, so it is the only thing
    that can see a change to masked geometry at all.
    """
    cmd = [RENDER, "-m", RAIDEN,
           "-t", os.path.join(ROOT, "my_models", "raiden", "textures"),
           "-e", os.path.join(ROOT, "my_models", "studio_small_03_8k.hdr"),
           "-a", os.path.join(ROOT, "my_models", "animations", "strut_walk.fbx"),
           "-s", os.path.join(ROOT, "my_models", "animations", "T-Pose.fbx"),
           "-x", "-f", "120", "--no-springs", "--no-auto-exposure", "-E", "1.0", "-S", out]
    r = _run(cmd + extra, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return r.stdout + r.stderr
    return None


def run_prepass_gate(workdir):
    """The depth prepass: identical picture, less shading (spec 11.30 / E6).

    The identity arm runs on `instancing_fixture` rather than on forest, and
    that is deliberate twice over. It is purely opaque, so every item goes
    through the prepass and none sits it out; and it is the one subject measured
    at exactly 0 px across a draw-order change, so a failure here is the prepass
    and not the scene.
    """
    failures = []

    off = os.path.join(workdir, "prepass_off.ppm")
    on = os.path.join(workdir, "prepass_on.ppm")
    err = render(os.path.join(ROOT, "assets", "instancing_fixture.cscn"), off,
                 ["--no-auto-exposure", "-E", "1.0"])
    err = err or render(os.path.join(ROOT, "assets", "instancing_fixture.cscn"), on,
                        ["--no-auto-exposure", "-E", "1.0", "--depth-prepass"])
    if err:
        print(f"  prepass-identity ERROR render failed: {err.strip()[-200:]}")
        return ["prepass-identity"]

    # 0 px, not a budget. The prepass writes depth from a DIFFERENT program than
    # the one that shades, and the shading pass then tests against it with
    # GL_LEQUAL -- so this arm is what proves `invariant gl_Position` and the
    # shared object-position chunk actually hold. A near miss here is fragments
    # failing the test, i.e. holes, not a slightly different picture.
    ae, _ = compare(off, on)
    ok = ae == 0
    print(f"  prepass-identity {'PASS' if ok else 'FAIL'}  {ae} px between prepass on and off "
          f"(want exactly 0)")
    if not ok:
        failures.append("prepass-identity")

    # --- prepass-masked: the arm above CANNOT see the flag's real cost -------
    # instancing_fixture is purely opaque, so nothing about it changes when
    # masked geometry does. Masked geometry IS prepassed since 11.31, but the
    # shading pass still runs GL_LEQUAL, so coincident cards that GL_LESS
    # rejected now both pass and the later one wins.
    #
    # Asserted as a BOUND rather than 0, because 0 is not the truth here and an
    # arm that demanded it would just be turned off. The number is what stops it
    # drifting quietly: raiden is the corpus's one masked subject and is 0 px
    # run-to-run, so any movement is the flag.
    rai_off = os.path.join(workdir, "prepass_rai_off.ppm")
    rai_off2 = os.path.join(workdir, "prepass_rai_floor.ppm")
    rai_on = os.path.join(workdir, "prepass_rai_on.ppm")
    if not os.path.exists(RAIDEN):
        # my_models is not in every checkout. SKIP rather than pass: this is the
        # only arm that can see the flag's cost, so a silent pass would be worse
        # than no arm at all.
        print("  prepass-masked SKIP  (my_models/raiden not present)")
        err = None
    elif (err := _raiden_render(rai_off, []) or _raiden_render(rai_off2, []) or
                 _raiden_render(rai_on, ["--depth-prepass"])):
        print(f"  prepass-masked ERROR render failed: {err.strip()[-200:]}")
        failures.append("prepass-masked")
    else:
        # The floor, rendered rather than asserted. It used to be a comment
        # saying raiden is 0 px run-to-run, which was true and was never
        # checked -- fine against 46,314, not fine against 395, where an
        # unmeasured floor is within an order of magnitude of the signal.
        floor, _ = compare(rai_off, rai_off2)
        ae, _ = compare(rai_off, rai_on)
        # 46,314 when this arm was written; 395 now, and BOTH reductions have
        # causes worth keeping apart. The opaque lane stopping blending took most
        # of it -- a coincident card that LEQUAL lets through used to composite a
        # second time and now overwrites with the value already there. Admitting
        # masked geometry to the prepass took none of it, which was a surprise.
        #
        # What is left is inherent to HAVING a prepass and will not reach 0: the
        # shading pass must test LEQUAL, so two surfaces at exactly equal depth
        # both pass and the last drawn wins, where GL_LESS rejected the second.
        # Same mechanism as cornell_box's 1 px coplanar tie.
        ok = floor < ae <= 2000
        print(f"  prepass-masked {'PASS' if ok else 'FAIL'}  raiden moves {ae} px under "
              f"--depth-prepass against a {floor} px floor (want above the floor and "
              f"<= 2000; coincident surfaces under LEQUAL, see spec 11.31)")
        if not ok:
            failures.append("prepass-masked")

    # Everything below needs the forest binary, which run_forest_gate skips on
    # when absent -- and this gate runs first, so without the guard a tree with
    # no out/bin/forest raises FileNotFoundError instead of skipping.
    if not os.path.exists(FOREST):
        print("  prepass-shading SKIP  (forest not built)")
        return failures

    # --- prepass-shading: it removes shading, and the row appears -----------
    base = _forest_run("pp_base", ["--taa", "--headless-jitter", "--no-sort-opaque"])
    pre = _forest_run("pp_on",
                      ["--taa", "--headless-jitter", "--no-sort-opaque", "--depth-prepass"])
    if base is None or pre is None:
        failures.append("prepass-shading")
    else:
        b, p = base["shading"], pre["shading"]
        # Depth complexity is the claim; draws rising is the cost, asserted so a
        # prepass that silently stopped submitting would not read as a win.
        drew_more = pre["opaque"]["draws"] > base["opaque"]["draws"]
        ok = p["complexity"] < b["complexity"] and drew_more
        print(f"  prepass-shading {'PASS' if ok else 'FAIL'}  depth complexity "
              f"{b['complexity']:.2f} -> {p['complexity']:.2f} (want lower), "
              f"{base['opaque']['draws']} -> {pre['opaque']['draws']} draws (want higher: the "
              f"prepass submits its own)")
        if not ok:
            failures.append("prepass-shading")

    return failures


_CLUSTER_PROBE = re.compile(
    r"cluster-probe mesh=(\d+) clusters=(\d+) groups=(\d+) dag_levels=(\d+) bands=(\d+) "
    r"max_index=(\d+) vertex_count=(\d+) foreign=(\d+)((?: band\d+=\d+)+)")

# A clustered prototype must reach at least this reduction from its finest band to
# its coarsest, or the DAG did not produce a usable cut. Well under bark's measured
# 277x and well over what a leaf card manages, which is the point: the arm has to
# pass on the geometry that CAN simplify without demanding it of the geometry that
# cannot.
CLUSTER_BAND_REDUCTION_MIN = 8.0

# How far the DAG's drawn triangles may sit from the chain's at one framing. The
# two are at PARITY on this corpus (measured -2.7% near, -0.1% mid, -1.4% far), and
# this arm pins that rather than claiming a win the measurement does not support.
CLUSTER_PARITY_TOLERANCE = 0.06

# Both builders must still beat drawing every triangle, at FOREST_CAM_WIDE, which
# is the framing cluster-parity uses. Measured 46.0% for the chain and 44.5% for
# the DAG; the bar sits far below both so it reads "LOD happened", not "this
# build's tuning".
#
# Both figures were 15.2% / 13.0% before 11.63 and are not comparable across it:
# those were taken at FOREST_CAM on a flat domain, and on the island that camera
# has nothing further than 425 units in front of it, which is why the two arms
# that are about distance moved to their own framing.
CLUSTER_LOD_SAVING_MIN = 0.10


def run_cluster_gate(workdir):
    """Cluster-DAG level of detail, and what it does and does not buy.

      cluster-seal    every cluster at every level indexes the ORIGINAL vertex
                         buffer, and destructive simplification never ran. That is
                         what makes a crack between levels impossible, and it is
                         structural -- no frame can show it, so it is read off the
                         probe rather than rendered.
      cluster-bands   the cut actually coarsens with distance: band triangle
                         counts never rise, and the geometry that CAN simplify
                         reaches a large reduction. Leaf cards are expected not to
                         and are not held to it. Falsified by inverting the two
                         clauses of the cut rule in cluster_build.cpp, which emits
                         the same cluster set in every band -- reduction 1.0x.
      cluster-batch   instancing survives. A per-instance cut would have ended
                         batching; the band quantisation exists so it does not, and
                         this is the arm that says so -- identical draw and instance
                         counts against the chain, and 0 px against --no-instancing.
      cluster-parity  the DAG draws about as many triangles as the chain, and
                         both beat --no-lod. Parity is the honest claim on this
                         corpus: boundary locking costs roughly what error-driven
                         selection gains, and the DAG's win is the seal above.

    The DAG is applied to PROPS ONLY, and the exclusion is measured rather than
    assumed: on a regular grid -- which is what a terrain tile is -- locking every
    group boundary costs more than it buys, and the chain reached a better surface
    at the same triangle count. Terrain gets the quadtree instead.
    """
    if not os.path.exists(FOREST):
        print("  cluster-seal SKIP  (forest not built)")
        return []
    failures = []

    probe = subprocess.run(
        [FOREST, "-x", "-f", "1", "-W", "200", "-H", "150", "--no-fog", "--seed", "1337",
         "--cluster-probe"], capture_output=True, text=True)
    rows = [m for m in _CLUSTER_PROBE.finditer(probe.stdout + probe.stderr)]
    # The return code separately from the rows: a crashed forest produces no rows
    # either, and reporting it as "the probe printed nothing" sends the reader
    # looking at the parser instead of at the process that died.
    if probe.returncode != 0:
        print(f"  cluster-seal ERROR the cluster probe exited {probe.returncode}")
        return ["cluster-seal", "cluster-bands", "cluster-batch", "cluster-parity"]
    if not rows:
        print("  cluster-seal ERROR the cluster probe produced no rows")
        return ["cluster-seal", "cluster-bands", "cluster-batch", "cluster-parity"]

    # --- seal ---------------------------------------------------------------
    # foreign_indices, NOT max_index, and the distinction is the whole arm. An
    # index merely being in range cannot move: mesh_build_cluster_lod refuses an
    # out-of-range input before building, and meshoptimizer promises its output
    # references the input buffer. What CAN move is a coarse band referencing a
    # vertex band 0 never used, which is what a remapping simplifier produces.
    #
    # This arm asserted `permissive == 0` until the reviewers caught that the
    # probe wrote that field as a literal -- both of its bars were tautologies,
    # including under the mutation the spec named for it.
    foreign = [r for r in rows if int(r.group(8)) != 0]
    banded = [r for r in rows if int(r.group(5)) >= 2]
    ok = not foreign and len(banded) >= 8
    print(f"  cluster-seal {'PASS' if ok else 'FAIL'}  {len(rows)} clustered prototypes, "
          f"{len(foreign)} referencing a vertex band 0 never used (want 0), over "
          f"{len(banded)} prototypes that actually built a coarse band (want >= 8, or "
          f"there was nothing for a coarse band to get wrong)")
    if not ok:
        failures.append("cluster-seal")

    # --- bands --------------------------------------------------------------
    worst_rise, best_reduction = 0, 0.0
    for r in rows:
        bands = [int(v.split("=")[1]) for v in r.group(9).split()]
        for i in range(1, len(bands)):
            worst_rise = max(worst_rise, bands[i] - bands[i - 1])
        if bands[-1] > 0:
            best_reduction = max(best_reduction, bands[0] / float(bands[-1]))
    ok = worst_rise <= 0 and best_reduction >= CLUSTER_BAND_REDUCTION_MIN
    print(f"  cluster-bands {'PASS' if ok else 'FAIL'}  worst band-to-band rise "
          f"{worst_rise} triangles (want <= 0), best prototype reduction {best_reduction:.0f}x "
          f"(want >= {CLUSTER_BAND_REDUCTION_MIN:.0f}x; a leaf card manages ~2x and is not the one "
          f"this reads)")
    if not ok:
        failures.append("cluster-bands")

    # --- batching and parity ------------------------------------------------
    # FOREST_CAM_WIDE, because the parity arm is about DISTANCE: standing on the
    # island there is no far field for either builder to coarsen, and the two agree
    # trivially on a scene where neither had anything to do.
    dag = _forest_run("cluster_dag", [], cam=FOREST_CAM_WIDE)
    chain = _forest_run("cluster_chain", ["--no-clusters"], cam=FOREST_CAM_WIDE)
    nolod = _forest_run("cluster_nolod", ["--no-lod"], cam=FOREST_CAM_WIDE)
    uninst = _forest_run("cluster_uninst", ["--no-instancing"], cam=FOREST_CAM_WIDE)
    if not all((dag, chain, nolod, uninst)):
        print("  cluster-batch ERROR while rendering forest")
        return failures + ["cluster-batch", "cluster-parity"]

    # `drew` is not a formality: every comparison below is an equality, and a
    # scene that submitted nothing satisfies all three. forest-batch-off guards
    # exactly this and these arms were written without it.
    drew = dag["opaque"]["draws"] > 0 and uninst["opaque"]["draws"] > 0
    same_draws = dag["opaque"]["draws"] == chain["opaque"]["draws"]
    same_inst = dag["opaque"]["instances"] == chain["opaque"]["instances"]
    unbatched = uninst["opaque"]["draws"] == uninst["opaque"]["instances"]
    ok = drew and same_draws and same_inst and unbatched
    print(f"  cluster-batch {'PASS' if ok else 'FAIL'}  DAG draws {dag['opaque']['draws']} vs "
          f"chain {chain['opaque']['draws']} and instances {dag['opaque']['instances']} vs "
          f"{chain['opaque']['instances']} (want equal: a per-instance cut would split every "
          f"run); --no-instancing gives {uninst['opaque']['draws']} draws for "
          f"{uninst['opaque']['instances']} instances (want equal)")
    if not ok:
        failures.append("cluster-batch")

    d_tris = dag["opaque"]["triangles"]
    c_tris = chain["opaque"]["triangles"]
    n_tris = nolod["opaque"]["triangles"]
    drift = abs(d_tris - c_tris) / float(max(c_tris, 1))
    d_save = 1.0 - d_tris / float(max(n_tris, 1))
    c_save = 1.0 - c_tris / float(max(n_tris, 1))
    # Same reason as above, and sharper here: with all-zero counts the two
    # savings come out at 1.0 and the drift at 0, so an empty frame reads as a
    # perfect result rather than as no result.
    ok = (n_tris > 0 and d_tris > 0 and drift <= CLUSTER_PARITY_TOLERANCE
          and d_save >= CLUSTER_LOD_SAVING_MIN and c_save >= CLUSTER_LOD_SAVING_MIN)
    print(f"  cluster-parity {'PASS' if ok else 'FAIL'}  DAG {d_tris} triangles vs chain "
          f"{c_tris}, drift {100.0 * drift:.1f}% (want <= {100.0 * CLUSTER_PARITY_TOLERANCE:.0f}%); "
          f"against --no-lod's {n_tris} that is {100.0 * d_save:.1f}% and {100.0 * c_save:.1f}% "
          f"saved (want >= {100.0 * CLUSTER_LOD_SAVING_MIN:.0f}% each)")
    if not ok:
        failures.append("cluster-parity")

    return failures


# The quadtree probe's three line shapes.
#
# NAMED groups throughout, and every field captured whether or not an arm reads
# it. Positionally these were group(1..7) against a dict that used 1,2,3,4,7 --
# so `resident` and `built` were matched and thrown away, which is the exact setup
# where inserting a field silently shifts every index after it and the arms go on
# passing against the wrong numbers. The level row was worse: four of its fields
# were non-capturing, so ADDING one anywhere in that line empties by_level and the
# group fails with a message about the probe printing nothing.
_QT_SUMMARY = re.compile(
    r"terrain-quadtree-probe levels=(?P<levels>\d+) segments=(?P<segments>\d+) "
    r"split_factor=(?P<split_factor>[\d.]+) selected=(?P<selected>\d+) "
    r"resident=(?P<resident>\d+) built=(?P<built>\d+) triangles=(?P<triangles>\d+)")
_QT_SEAM = re.compile(
    r"terrain-quadtree-probe seams=(?P<seams>\d+) unbalanced=(?P<unbalanced>\d+) "
    r"fine_min=(?P<fine_min>[\d.]+) coarse_max=(?P<coarse_max>[\d.]+) "
    r"gap=(?P<gap>[\d.eE+-]+) interior_gap=(?P<interior_gap>[\d.eE+-]+)")
_QT_LEVEL = re.compile(
    r"terrain-quadtree-probe level=(?P<level>\d+) span=(?P<span>[\d.]+) "
    r"cell=(?P<cell>[\d.]+) split_at=(?P<split_at>[\d.]+) "
    r"morph_start=(?P<morph_start>[-\d.]+) morph_end=(?P<morph_end>[-\d.]+) "
    r"selected=(?P<selected>\d+) source_level=(?P<source_level>\d+) "
    r"source_cell=(?P<source_cell>[\d.]+) dropped=(?P<dropped>[\d.eE+-]+)")
_QT_FIELD_LEVELS = re.compile(r"Terrain field: (\d+) level\(s\)")

# Both gaps are float rounding on ~95-unit heights, where an ulp is 7.6e-6. The
# bar is two orders above that and four below the mutations it has to catch:
# flipping the coarse quad's diagonal reads 2.92 world units, and dropping the
# split factor to 2.0 opens a seam at coarse_max 0.173.
QUADTREE_GAP_MAX = 1.0e-3

# Patches, for a SIXTEENFOLD increase in ground area. The claim is that a
# quadtree's cost is logarithmic in world size where a fixed grid's is quadratic,
# so the bar is nowhere near tight -- 16x area really did cost 1.94x, and the
# thing this catches is the growth going quadratic, which would be 16x.
QUADTREE_GROWTH_MAX = 4.0

# How much the depth prepass may move on the quadtree, as a multiple of how much
# it moves on the fixed grid. The residual is pre-existing and belongs to masked
# foliage taking its own program; the morph is supposed to add nothing to it.
# Measured 612 px against 612 -- a ratio of 1.00 -- and starving one program of
# uMorphEye reads 9x, so the bar sits between two numbers that are far apart.
QUADTREE_PREPASS_TOLERANCE = 2.5

# Albedo pixels --no-morph must move. Measured 6,493 with the morph healthy and
# 22 with cetraMorphOffset deleted, over a floor of exactly 0 -- so the bar sits
# a factor of six below the signal and a factor of forty-five above the failure.
# An absolute count rather than a ratio to the floor, because this path's floor
# is genuinely 0 and a ratio to it is undefined.
QUADTREE_MORPH_MIN_PX = 1000

# World units of relief a coarse level must actually give up. Well above float
# noise and well under what either source drops in practice -- 4.9 for the fbm,
# 6.2 for a filtered field -- so the thing it catches is the drop being ZERO,
# which is what a level selector that always answers 0 gives, and what a
# SUBSAMPLED pyramid gives on a lattice the patches align with.
QUADTREE_MIP_DROP_MIN = 0.5


# Far enough back that the descent stops at the root: one patch, at the coarsest
# level there is, which is the only framing where the band-limit has anything to
# do. Every other arm here reads FOREST_CAM, where the whole selection is fine
# enough to want the full surface.
FOREST_CAM_HIGH = ["--cam-eye", "0,3000,3000", "--cam-target", "0,0,0"]


def _quadtree_probe(extra, cam=None):
    """The terrain quadtree probe, parsed, or None.

    Three frames and a small window: the probe reads the SELECTION, which is a
    function of the camera and not of how long the app has been running, and the
    seam measurement walks every selected patch's boundary on the CPU.
    """
    cmd = ([FOREST, "-x", "-f", "3", "-W", "400", "-H", "240", "--no-fog", "--seed", "1337",
            "--terrain-quadtree-probe"] + (cam or FOREST_CAM) + extra)
    r = _run(cmd, capture_output=True, text=True)
    text = r.stdout + r.stderr
    summary = _QT_SUMMARY.search(text)
    seam = _QT_SEAM.search(text)
    if r.returncode != 0 or not summary or not seam:
        return None
    fl = _QT_FIELD_LEVELS.search(text)
    levels = [{"level": int(m["level"]), "span": float(m["span"]), "cell": float(m["cell"]),
               "split_at": float(m["split_at"]), "morph_start": float(m["morph_start"]),
               "morph_end": float(m["morph_end"]), "selected": int(m["selected"]),
               "source_level": int(m["source_level"]), "source_cell": float(m["source_cell"]),
               "dropped": float(m["dropped"])} for m in _QT_LEVEL.finditer(text)]
    return {
        "field_levels": int(fl.group(1)) if fl else 0,
        "by_level": levels,
        "levels": int(summary["levels"]),
        "segments": int(summary["segments"]),
        "split_factor": float(summary["split_factor"]),
        "selected": int(summary["selected"]),
        "resident": int(summary["resident"]),
        "built": int(summary["built"]),
        "triangles": int(summary["triangles"]),
        "seams": int(seam["seams"]),
        "unbalanced": int(seam["unbalanced"]),
        "fine_min": float(seam["fine_min"]),
        "coarse_max": float(seam["coarse_max"]),
        "gap": float(seam["gap"]),
        "interior_gap": float(seam["interior_gap"]),
    }


def _grid_tiles(extra):
    """How many tiles the FIXED grid builds, or None.

    One frame at a postage stamp: the number is printed at startup and nothing
    here looks at a pixel. It still has to be a real run rather than arithmetic on
    the extent, or an app that quietly stopped growing its grid would be read as
    the quadtree's win.

    THE COST IS DELIBERATE AND HAS BEEN RE-FILED ONCE. At --terrain-extent 2000
    this builds 1,024 tiles, which is millions of fbm evaluations to read one
    integer, and the obvious fix -- print the count before building -- is exactly
    the arithmetic the paragraph above rejects, since on_init builds either way.
    Making it genuinely cheap needs an early-exit flag on the app, i.e. a small
    feature rather than a cleanup. Measured in release it is a fraction of a
    second; the number that made it look worth fixing came from a debug build.
    """
    # No -S -- this docstring already says nothing here looks at a pixel.
    cmd = ([FOREST, "-x", "-f", "1", "-W", "160", "-H", "120", "--no-fog", "--seed", "1337",
            "--no-quadtree"] + extra)
    r = _run(cmd, capture_output=True, text=True)
    m = _FOREST_TERRAIN.search(r.stdout + r.stderr)
    if r.returncode != 0 or not m:
        return None
    return int(m.group(1) or m.group(2))


def run_quadtree_gate(workdir):
    """The terrain quadtree: does the CDLOD selection hold together (spec 11.63).

      quadtree-seam    adjacent patches never differ by more than one level, and
                       at every seam the fine side is fully morphed while the
                       coarse side has not started -- so both are evaluating the
                       same coarse surface there and no crack is possible. Read
                       off the probe, at two world sizes.
      quadtree-morph  a fully morphed patch really is the surface its parent
                       DRAWS, measured against the parent's own triangles. This
                       is the pop, and the seam arm cannot see it: every boundary
                       vertex is even-indexed and therefore its own morph target,
                       so the interior can be wrong with every seam clean.
      quadtree-scale   the selection grows logarithmically with the world and the
                       fixed grid grows quadratically. The whole reason the
                       quadtree exists, and the only arm that varies the domain.
      quadtree-mip           a coarse patch reads a coarse SOURCE, and reading it
                       actually removes detail. Both sources: the fbm drops
                       octaves, a stored field reads a filtered level. The second
                       half is the arm -- picking a coarser level is free to be a
                       no-op, and with a subsampled pyramid on an aligned lattice
                       it is exactly that.
      quadtree-depth   the morph reaches the DEPTH PREPASS. It rasterizes the
                       same triangles and the shading pass tests against its
                       depth with LEQUAL, so a program that displaces differently
                       does not shade wrong, it deletes the surface.
      quadtree-render  the morph happens AT ALL, in a rendered frame, read
                       through --no-morph. Every arm above this one reads the
                       probe, whose morph factor is a hand C port of
                       cetraMorphFactor -- so a morph that never reached the
                       shader passes all four of them, which is precisely the
                       failure --wind-bound-probe was rebuilt to close.

    Every number here except the last is invisible in a frame, which is why the
    arms exist. A crack opens for the few frames the camera spends crossing a
    band, on ground that is by construction distant; a pop is one frame; and a
    starved prepass removes terrain only where the coarse surface happens to sit
    in front.
    """
    if not os.path.exists(FOREST):
        print("  quadtree-seam SKIP  (forest not built)")
        return []
    failures = []

    near = _quadtree_probe([])
    far = _quadtree_probe(["--terrain-extent", "2000"])
    if not near or not far:
        print("  quadtree-seam ERROR the quadtree probe produced no rows")
        return ["quadtree-seam", "quadtree-morph", "quadtree-scale", "quadtree-mip",
                "quadtree-depth", "quadtree-render"]

    # --- seam ---------------------------------------------------------------
    # fine_min == 1 exactly and coarse_max == 0 exactly, not approximately: both
    # are clamped, so the correct answer is the clamp's own endpoint and anything
    # between them is a seam that is genuinely open.
    ok = all(p["unbalanced"] == 0 and p["fine_min"] == 1.0 and p["coarse_max"] == 0.0
             and p["gap"] <= QUADTREE_GAP_MAX and p["seams"] > 0 for p in (near, far))
    print(f"  quadtree-seam {'PASS' if ok else 'FAIL'}  {near['seams']} seams at 1 km and "
          f"{far['seams']} at 4 km, {near['unbalanced'] + far['unbalanced']} more than one level "
          f"apart (want 0); fine side {near['fine_min']:.4f}/{far['fine_min']:.4f} (want 1), "
          f"coarse side {near['coarse_max']:.4f}/{far['coarse_max']:.4f} (want 0), worst gap "
          f"{max(near['gap'], far['gap']):.2e} units (want <= {QUADTREE_GAP_MAX:.0e})")
    if not ok:
        failures.append("quadtree-seam")

    # --- interior -----------------------------------------------------------
    worst = max(near["interior_gap"], far["interior_gap"])
    ok = worst <= QUADTREE_GAP_MAX
    print(f"  quadtree-morph {'PASS' if ok else 'FAIL'}  a fully morphed patch sits "
          f"{worst:.2e} world units off the surface its parent draws (want <= "
          f"{QUADTREE_GAP_MAX:.0e}; the wrong coarse-quad diagonal reads 2.92)")
    if not ok:
        failures.append("quadtree-morph")

    # --- world size ---------------------------------------------------------
    # The fixed-grid counts come from the app rather than from the ratio of the
    # extents, so an app that quietly stopped growing its tile grid is caught
    # here instead of flattering the quadtree.
    #
    # One frame at a postage stamp, not a profiled render: what is read is the
    # tile COUNT, and a 4 km grid builds 1,024 tiles before it can draw one -- a
    # 20-frame 800x450 run to learn an integer the app prints at startup.
    grid_near = _grid_tiles([])
    grid_far = _grid_tiles(["--terrain-extent", "2000"])
    if not grid_near or not grid_far:
        print("  quadtree-scale ERROR while rendering the fixed grid")
        failures.append("quadtree-scale")
    else:
        qt_growth = far["selected"] / float(max(near["selected"], 1))
        grid_growth = grid_far / float(max(grid_near, 1))
        # The floor on `selected` is load-bearing: a quadtree that stopped
        # descending and returned its root at both sizes reads 1.0x growth and
        # passes the ratio outright.
        ok = (near["selected"] > 1 and qt_growth <= QUADTREE_GROWTH_MAX
              and grid_growth > qt_growth * 2.0)
        print(f"  quadtree-scale {'PASS' if ok else 'FAIL'}  16x the ground area takes the "
              f"quadtree from {near['selected']} patches to {far['selected']} ({qt_growth:.2f}x, "
              f"want <= {QUADTREE_GROWTH_MAX:.0f}x) while the fixed grid goes "
              f"{grid_near} -> {grid_far} "
              f"({grid_growth:.1f}x, want more than twice the quadtree's)")
        if not ok:
            failures.append("quadtree-scale")

    # --- the band limit -----------------------------------------------------
    # Read from a camera far enough back that the descent stops at the root, so
    # the one selected patch is at the coarsest level there is. At FOREST_CAM the
    # whole selection is fine enough to want the full surface, and every
    # `dropped` there is legitimately 0.
    analytic = _quadtree_probe([], cam=FOREST_CAM_HIGH)
    stored = _quadtree_probe(["--erode", "--erode-iterations", "20"], cam=FOREST_CAM_HIGH)
    if not analytic or not stored:
        print("  quadtree-mip       ERROR the band-limit probe produced no rows")
        failures.append("quadtree-mip")
    else:
        def coarsest(p):
            rows = [r for r in p["by_level"] if r["selected"] > 0]
            return max(rows, key=lambda r: r["level"]) if rows else None

        a_row, s_row = coarsest(analytic), coarsest(stored)
        # Monotone in the level, since a coarser patch may never ask for a finer
        # source: that ordering is what keeps a patch and its parent one source
        # level apart, which is what the morph target depends on.
        ordered = all(p["by_level"][k]["source_level"] >= p["by_level"][k - 1]["source_level"]
                      for p in (analytic, stored) for k in range(1, len(p["by_level"])))
        ok = (a_row and s_row and ordered and stored["field_levels"] > 1
              and a_row["source_level"] > 0 and a_row["dropped"] > QUADTREE_MIP_DROP_MIN
              and s_row["source_level"] > 0 and s_row["dropped"] > QUADTREE_MIP_DROP_MIN)
        print(f"  quadtree-mip       {'PASS' if ok else 'FAIL'}  the coarsest selected patch reads "
              f"source level {a_row['source_level'] if a_row else '?'} of the fbm and drops "
              f"{a_row['dropped'] if a_row else 0:.2f} world units, and level "
              f"{s_row['source_level'] if s_row else '?'} of a "
              f"{stored['field_levels']}-level stored field dropping "
              f"{s_row['dropped'] if s_row else 0:.2f} (want > {QUADTREE_MIP_DROP_MIN} each, a field "
              f"pyramid deeper than 1, and source levels non-decreasing: {ordered})")
        if not ok:
            failures.append("quadtree-mip")

    # --- the prepass --------------------------------------------------------
    # --no-sky, because the Hillaire atmosphere is what makes forest not
    # pixel-deterministic; the arm reads a difference between two frames and
    # needs the difference to be the prepass.
    #
    # Twelve frames at 400x240, and the size is affordable because what the arm
    # reads is a RATIO of two pixel counts taken at the same size. Starving a
    # program of uMorphEye deletes surface wherever the coarse ground sits in
    # front of the fine, which is a fraction of the frame and not a speck count.
    shots = {}
    for tag, extra in (("qt", []), ("qt_pp", ["--depth-prepass"]),
                       ("grid", ["--no-quadtree"]),
                       ("grid_pp", ["--no-quadtree", "--depth-prepass"])):
        out = os.path.join(workdir, f"quadtree_{tag}.ppm")
        cmd = ([FOREST, "-x", "-f", "12", "-W", "400", "-H", "240", "--no-fog", "--no-sky",
                "--seed", "1337", "-S", out] + FOREST_CAM + extra)
        r = _run(cmd, capture_output=True, text=True)
        shots[tag] = out if r.returncode == 0 and os.path.exists(out) else None
    if not all(shots.values()):
        print("  quadtree-depth ERROR while rendering the prepass comparison")
        return failures + ["quadtree-depth", "quadtree-render"]

    qt_move, _ = compare(shots["qt"], shots["qt_pp"])
    grid_move, _ = compare(shots["grid"], shots["grid_pp"])
    # The precondition, without which the arm passes on a frame where nothing
    # morphs: some patch has to have reached factor 1 for the prepass to have
    # anything to disagree about.
    morphing = near["fine_min"] == 1.0 and near["seams"] > 0
    ok = morphing and qt_move <= grid_move * QUADTREE_PREPASS_TOLERANCE
    print(f"  quadtree-depth {'PASS' if ok else 'FAIL'}  --depth-prepass moves {qt_move} px on "
          f"the quadtree against {grid_move} on the fixed grid, whose terrain does not morph at "
          f"all (want <= {QUADTREE_PREPASS_TOLERANCE:.1f}x it; starving one program of uMorphEye reads "
          f"9x), with {near['seams']} seams morphing")
    if not ok:
        failures.append("quadtree-depth")

    # --- render -------------------------------------------------------------
    # Everything above this line reads the probe, and the probe's morph factor is
    # a hand C port of cetraMorphFactor. So a morph dead in all five geometry
    # programs -- the include never reached, the attributes never bound, the
    # uniform never uploaded -- passes every one of them. This is the arm that
    # looks at a frame, and it is the same lesson --wind-bound-probe was rebuilt
    # on transform feedback for.
    #
    # The framing is its own, and not FOREST_CAM, because the morph is only large
    # where a patch is MID-WINDOW: level 0's window here is 164 to 219 units, so
    # the camera is placed to fill the frame with ground at that range. At
    # FOREST_CAM the whole comparison reads 146 px against a 5 px floor.
    #
    # --render-mode 6 is what makes this a POSITION test rather than a "something
    # moved" test, and that distinction was measured rather than assumed. Read
    # lit, deleting cetraMorphOffset entirely still moved 3,999 px and PASSED --
    # because cetraMorphNormal is a separate displacement of the same factor and
    # --no-morph switches both off, so the arm was reading the shading. Albedo has
    # no normal in it, so what is left is where the geometry PROJECTS. It is also
    # forest's one pixel-deterministic path, which is why the floor is an exact 0
    # here and 174 on the lit frame.
    #
    # Known gap, stated rather than papered over: this does not cover
    # cetraMorphNormal. Deleting it alone moves nothing in albedo.
    mshots = {}
    for tag, extra in (("on", []), ("on2", []), ("off", ["--no-morph"])):
        out = os.path.join(workdir, f"quadtree_morph_{tag}.ppm")
        cmd = ([FOREST, "-x", "-f", "3", "-W", "800", "-H", "500", "--no-fog", "--no-sky",
                "--seed", "1337", "--render-mode", "6",
                "--cam-eye", "0,60,220", "--cam-target", "0,-10,-120", "-S", out] + extra)
        r = _run(cmd, capture_output=True, text=True)
        mshots[tag] = out if r.returncode == 0 and os.path.exists(out) else None
    if not all(mshots.values()):
        print("  quadtree-render ERROR while rendering the morph comparison")
        return failures + ["quadtree-render"]

    floor, _ = compare(mshots["on"], mshots["on2"])
    moved, _ = compare(mshots["on"], mshots["off"])
    # The floor is asserted at 0 and not merely bounded: this path IS
    # deterministic, so anything above it means the comparison stopped measuring
    # the morph and started measuring the renderer.
    ok = floor == 0 and moved >= QUADTREE_MORPH_MIN_PX
    print(f"  quadtree-render {'PASS' if ok else 'FAIL'}  --no-morph moves {moved} px of albedo "
          f"(want >= {QUADTREE_MORPH_MIN_PX}; a dead cetraMorphOffset reads 22) over a "
          f"same-config floor of {floor} (want exactly 0); this is the only arm that reaches the "
          f"SHADER -- the five above it read a C port of the same arithmetic")
    if not ok:
        failures.append("quadtree-render")

    return failures


_REGION_SUMMARY = re.compile(
    r"region-probe side=(\d+) span=([\d.]+) resident=(\d+) of (\d+) loaded=(\d+) freed=(\d+) "
    r"nodes=(\d+)")
_REGION_CELL = re.compile(
    r"region-probe cell rx=(?P<rx>\d+) rz=(?P<rz>\d+) trees=(?P<trees>\d+) "
    r"rocks=(?P<rocks>\d+) nodes=(?P<nodes>\d+) collider=(?P<collider>\d+) "
    r"digest=(?P<digest>[0-9a-f]+) authored=(?P<authored>[0-9a-f]+)")

# A region small enough, and a load radius short enough, that a walk of a few
# hundred frames crosses several boundaries. The shipping radius is larger than a
# kilometre world's own diagonal on purpose -- at this size nothing would ever be
# freed and every arm here would read a scene that never churned.
REGION_CHURN = ["--region-span", "40", "--region-radius", "60"]
# Fast enough to cross several 40-unit regions inside a three-second leg, so a
# 400-frame round trip leaves and comes back rather than shuffling inside one
# cell -- the seam arm needs boundaries CROSSED, not merely approached.
# --trace-player rides along: the seam arm reads the same run the residency arms
# do, so "it crossed a boundary" and "it stayed grounded" are the same walk rather
# than two that merely used the same flags.
REGION_WALK = ["--walk", "40", "--trace-player"]


def _region_run(extra, frames="400"):
    """One forest run with the region probe, parsed into (summary, {cell: row})."""
    # No -S: every region arm reads probe rows and trace lines, never a pixel.
    cmd = ([FOREST, "-x", "-f", frames, "-W", "320", "-H", "200", "--no-fog", "--seed", "1337",
            "--region-probe"] + extra)
    r = _run(cmd, capture_output=True, text=True)
    text = r.stdout + r.stderr
    m = _REGION_SUMMARY.search(text)
    if r.returncode != 0 or not m:
        return None, None, text
    summary = {"side": int(m.group(1)), "span": float(m.group(2)), "resident": int(m.group(3)),
               "total": int(m.group(4)), "loaded": int(m.group(5)), "freed": int(m.group(6)),
               "nodes": int(m.group(7))}
    cells = {}
    for c in _REGION_CELL.finditer(text):
        cells[(int(c["rx"]), int(c["rz"]))] = {
            "trees": int(c["trees"]), "rocks": int(c["rocks"]), "nodes": int(c["nodes"]),
            "collider": int(c["collider"]), "digest": c["digest"], "authored": c["authored"]}
    return summary, cells, text


_ISLAND_HEADER = re.compile(r"terrain-height-probe island start=([\d.]+) depth=([\d.]+)")
_ISLAND_ROW = re.compile(
    r"terrain-height-probe island az=(\d+) r=([\d.]+) h=(-?[\d.]+) flat=(-?[\d.]+) "
    r"drop=(-?[\d.]+) t=([\d.]+) floor=(-?[\d.]+)")
_SCATTER_SHORE = re.compile(
    r"scatter-probe shore trees=(\d+) low_y=(-?[\d.]+) water=(\d+) water_level=(-?[\d.]+)")

# World units. The falloff is a smoothstep, so it is exactly flat inside its start
# and the drop there is a true zero rather than a small number -- but the height it
# is subtracted from is the fbm at ~95 units, so the bar is loose enough to survive
# the subtraction's own rounding and tight enough that a falloff leaking inland by
# any visible amount fails.
ISLAND_INLAND_MAX = 1e-3
# The falloff is a smoothstep in fp32, so its saturated value is an exact 1 and
# its steps are far larger than this. The slack is for the read, not the value.
ISLAND_FALLOFF_EPS = 1e-4
# How deep the rim has to be by the domain's edge, as a fraction of the island's
# depth. Not 1.0, because the sea floor deliberately keeps a QUARTER of the
# terrain's own relief rather than going exactly flat -- a run of exactly coplanar
# triangles is a collider Jolt's tree builder cannot split, and its fallback
# TRACES into a handler that aborts the process.
ISLAND_RIM_FRACTION = 0.6
# The axis radius the round-vs-square comparison is anchored at. Inside the
# falloff's own band (which starts at 0.72) so the axis has partly fallen and the
# bar has somewhere to fail -- at 0.72 or below both sides read 0 and at 1.0 both
# read 1, and either end passes for a square island too.
ISLAND_AXIS_R = 0.8
# The fraction of the half-extent the scatter draws inside (forest's sample_ground
# clips to it). Ground outside it is not a candidate, so it cannot be evidence
# that a placement rule rejected anything.
SCATTER_MARGIN = 0.96


def _island_probe(extra):
    """The height probe's island rows, as (header, rows) or (None, [])."""
    cmd = ([FOREST, "-x", "-f", "1", "-W", "200", "-H", "150", "--no-fog", "--seed", "1337",
            "--terrain-height-probe"] + extra)
    r = _run(cmd, capture_output=True, text=True)
    text = r.stdout + r.stderr
    head = _ISLAND_HEADER.search(text)
    rows = [{"az": int(m.group(1)), "r": float(m.group(2)), "h": float(m.group(3)),
             "flat": float(m.group(4)), "drop": float(m.group(5)), "t": float(m.group(6)),
             "floor": float(m.group(7))}
            for m in _ISLAND_ROW.finditer(text)]
    # (None, None) for "did not run" against (None, []) for "ran and printed no
    # island rows". The control arm reads an empty list as proof the shaping is
    # off, so a run that crashed must not look like one that was asked not to
    # shape.
    if r.returncode != 0:
        return None, None
    if not head:
        return None, rows
    return {"start": float(head.group(1)), "depth": float(head.group(2))}, rows


def run_island_gate(workdir):
    """The island: ground that falls to a shoreline, and a sea past it.

      island-shore  the falloff is exactly nothing inland of where it starts,
                       grows without reversing outside it, and has reached the sea
                       floor by the domain's edge -- on every azimuth, so the shore
                       is a ring and not a band that happens to cross the frame.
                       Read as the DROP against the same terrain unshaped, which is
                       what makes it a statement about the falloff rather than
                       about this seed's noise.
      island-dry       no prop of any kind stands below the waterline, on terrain
                       that reaches the sea floor -- so the rule had drowned ground
                       to reject rather than passing on a world with none. ROCKS
                       and not just trees: a tree is turned away from the shoal by
                       the slope test long before the waterline matters, where a
                       rock tolerates 57 degrees and the rim is 45. Falsified by
                       deleting sample_ground's waterline rejection, which puts the
                       lowest prop at about y -175 on the sea floor. The second bar
                       -- reachable ground BELOW the waterline -- is what stops the
                       first passing on a world with nothing to reject, and it is
                       bounded by SCATTER_MARGIN because ground the scatter clips
                       away was never a candidate.

    --no-island is the control on the first, and a real one: it prints no island
    rows at all, which is what says the shaping is off rather than merely small.
    """
    if not os.path.exists(FOREST):
        print("  island-shore SKIP  (forest not built)")
        return []
    failures = []

    head, rows = _island_probe([])
    _, flat_rows = _island_probe(["--no-island"])
    if not head or not rows:
        print("  island-shore ERROR the height probe produced no island rows")
        return ["island-shore", "island-dry"]

    # The FALLOFF is read off the row rather than recovered from the drop. The
    # drop is the falloff times a span that includes the noise AND the sea floor's
    # share of it, so it follows every dip in the terrain and is not monotone in r
    # -- and reconstructing it here would restate arithmetic only terrain.c can
    # see.
    #
    # NOT monotonicity of t along a ray, which this arm used to assert: the probe
    # sweeps radially and the falloff is a function of radius, so t rises with r
    # for ANY radial falloff and the bar cannot fail.
    #
    # And NOT "the diagonals have fallen further at one radius", which was the
    # first replacement and is unsatisfiable for the same reason from the other
    # side: the probe samples in POLAR coordinates and the falloff is polar, so
    # every azimuth reads an identical t at a given r BY CONSTRUCTION. It read
    # 0.708 against 0.708.
    #
    # What can fail is the shape read against the SQUARE the domain actually is.
    # Box fraction -- how far out a point is as a share of the half-extent along
    # its own dominant axis -- is what a Chebyshev falloff would be a function of.
    # So the bar is: a diagonal sample NO FURTHER OUT in box terms than an axis
    # sample has still fallen FURTHER. True of a circle inscribed in the square,
    # false of the square. Concretely the diagonal at r 1.1 sits at box fraction
    # 0.778 against the axis sample's 0.8, and reads t 1.000 against 0.198; a
    # Chebyshev falloff would read 0.106 there and fail.
    def box_fraction(row):
        ang = 2.0 * math.pi * row["az"] / len({x["az"] for x in rows})
        return row["r"] * max(abs(math.cos(ang)), abs(math.sin(ang)))

    inland = max((abs(x["drop"]) for x in rows if x["r"] <= head["start"]), default=0.0)
    azimuths = sorted({x["az"] for x in rows})
    # az 0/2/4/6 are the axes, 1/3/5/7 the diagonals (terrain.c sweeps 8 evenly).
    axis_row = next((x for x in rows if x["az"] % 2 == 0
                     and abs(x["r"] - ISLAND_AXIS_R) < 1e-4), None)
    diag_rows = []
    for az in sorted({x["az"] for x in rows if x["az"] % 2 == 1}):
        under = [x for x in rows if x["az"] == az and box_fraction(x) <= ISLAND_AXIS_R + 1e-6]
        if under:
            diag_rows.append(max(under, key=box_fraction))
    # Every diagonal, not the best one: an azimuth-dependent falloff would satisfy
    # a bar that took the maximum and is exactly what the ring shape is about.
    round_island = (axis_row is not None and len(diag_rows) == 4
                    and all(d["t"] > axis_row["t"] + 1e-6 for d in diag_rows))
    axis_t = [axis_row["t"]] if axis_row else []
    diag_t = [d["t"] for d in diag_rows]
    rim_short = 0
    for az in azimuths:
        seq = sorted((x for x in rows if x["az"] == az), key=lambda x: x["r"])
        # Two bars at the rim, and they answer different questions: the falloff has
        # to have RUN OUT (t == 1, exactly what the smoothstep owes past its upper
        # edge), and the ground has to be deep water rather than merely damp. The
        # second is not implied by the first -- a floor that kept all of the
        # terrain's relief instead of a quarter of it would satisfy t and surface.
        rim = [x for x in seq if x["r"] >= 1.0]
        # The rim bar is the DEPTH, not t: t == 1 past the smoothstep's upper edge
        # is the clamp's own endpoint and cannot fail. Depth can -- a sea floor
        # that kept all of the terrain's relief instead of a quarter of it would
        # still saturate t and still surface.
        if not rim or max(x["h"] for x in rim) > -head["depth"] * ISLAND_RIM_FRACTION:
            rim_short += 1
    ok = (inland <= ISLAND_INLAND_MAX and round_island and rim_short == 0
          and len(azimuths) >= 8 and flat_rows is not None and not flat_rows)
    print(f"  island-shore {'PASS' if ok else 'FAIL'}  worst drop inland of r "
          f"{head['start']:.2f} is {inland:.2e} units (want <= {ISLAND_INLAND_MAX:.0e}); at box "
          f"fraction {max((box_fraction(d) for d in diag_rows), default=0):.3f} the diagonals have "
          f"fallen {min(diag_t) if diag_t else 0:.3f} against the axes' "
          f"{max(axis_t) if axis_t else 0:.3f} at a LARGER {ISLAND_AXIS_R:.2f} (want strictly "
          f"more, or the island is square and not round); {rim_short} azimuths short of the "
          f"{-head['depth'] * ISLAND_RIM_FRACTION:.0f}-unit rim (want 0), and --no-island prints "
          f"{len(flat_rows) if flat_rows is not None else 'a crash'} island rows (want 0)")
    if not ok:
        failures.append("island-shore")

    def shore(extra):
        cmd = ([FOREST, "-x", "-f", "1", "-W", "200", "-H", "150", "--no-fog", "--seed", "1337",
                "--scatter-probe"] + extra)
        r = _run(cmd, capture_output=True, text=True)
        m = _SCATTER_SHORE.search(r.stdout + r.stderr)
        if r.returncode != 0 or not m:
            return None
        return {"trees": int(m.group(1)), "low_y": float(m.group(2)), "water": int(m.group(3)),
                "level": float(m.group(4))}

    isle = shore([])
    if not isle:
        print("  island-dry ERROR the scatter probe produced no shore row")
        return failures + ["island-dry"]
    # The GROUND's own minimum is the control, not the props of an unshaped run.
    # An unshaped forest has almost no terrain below the waterline either -- its
    # heights are the fbm about zero -- so comparing the two would compare two
    # numbers that are equal because neither had anywhere to go. What says the
    # rule had work is that the island's rim reaches the sea floor, which is well
    # below where anything is allowed to stand.
    #
    # Bounded to SCATTER_MARGIN, because ground the scatter cannot draw from is
    # not ground the rule rejected. sample_ground clips to 0.96 of the extent and
    # the probe sweeps to 1.4, so an unbounded min reports drowned ground that was
    # never a candidate.
    ground_low = min((x["h"] for x in rows if x["r"] <= SCATTER_MARGIN), default=0.0)
    ok = (isle["water"] == 1 and isle["trees"] > 0 and isle["low_y"] >= isle["level"]
          and ground_low < isle["level"])
    print(f"  island-dry {'PASS' if ok else 'FAIL'}  the lowest prop of any kind stands at y "
          f"{isle['low_y']:.1f} against a waterline at {isle['level']:.1f} (want above it), on "
          f"reachable terrain that reaches {ground_low:.1f} (want below it, or there was no "
          f"drowned ground the scatter could have drawn from) -- over {isle['trees']} trees and "
          f"the rocks beside them")
    if not ok:
        failures.append("island-dry")

    return failures


def run_region_gate(workdir):
    """Region residency: props and collision that exist only near the camera.

      region-scatter  a region places the same props whether it loaded alone or
                         with fifteen neighbours. It is seeded from its own CELL
                         COORDINATES, not drawn from one global stream, and that is
                         the whole difference between a world you can re-enter and
                         one that reshuffles behind you. Falsified by seeding
                         region_load from a global counter instead of region_seed,
                         which takes the matching digests to 0 of 4.
      region-return   after a round trip the regions that come back are the ones
                         that left, byte for byte. Read as digests against a run
                         that never moved. Same mutation as above, and it has to be
                         separate: a scatter can be stable across NEIGHBOURS and
                         still not survive a free and a reload.
      region-shift    and it survives an ORIGIN SHIFT, which frees every resident
                         region, resets the prototype groups to identity and
                         scatters again. Read on the AUTHORED digest -- storage
                         plus the world origin, snapped to a unit -- because a
                         shift is supposed to move the stored bytes and that is
                         asserted in the same breath. Falsified by dropping the
                         group-identity reset in regions_rebuild, which
                         double-shifts every prop that reloads: 0 of 15 cells
                         match. That mutation is also why the digest composes
                         through the parent group -- against a node-local digest
                         it read bit-identical and this arm stayed green.
      region-leak     every region ever loaded is either resident or was freed,
                         and the node total is the sum over the resident ones. A
                         residency that frees nothing renders perfectly and runs
                         out of memory a kilometre later. Falsified by returning
                         early from region_free, which takes freed to 0 -- which is
                         why the arm requires freed > 0 rather than only the sum.
      region-collider   the character crosses region boundaries without losing
                         ground contact. Collision is per region now, so the seam
                         between two static bodies is a place a character can fall
                         through and nothing else would notice. Falsified by
                         skipping the per-region collider build: grounded goes
                         False. That mutation was RUN -- it is how the whole-domain
                         collider was found still carrying the character, which had
                         made this arm vacuous.

    The shipping load radius is deliberately wider than a kilometre world's own
    diagonal, so this app's historical configuration keeps every region resident
    and nothing about it moved. Every arm here dials it down to force the churn it
    is measuring -- which is also why none of them is a pixel comparison.
    """
    if not os.path.exists(FOREST):
        print("  region-scatter SKIP  (forest not built)")
        return []
    failures = []

    # --- scatter: the same cells, loaded two different ways -----------------
    few, few_cells, _ = _region_run(["--region-radius", "200"], frames="5")
    many, many_cells, _ = _region_run([], frames="5")
    if not few or not many:
        print("  region-scatter ERROR the region probe produced no rows")
        return ["region-scatter", "region-return", "region-shift", "region-leak",
                "region-collider"]

    shared = sorted(set(few_cells) & set(many_cells))
    same = [k for k in shared if few_cells[k]["digest"] == many_cells[k]["digest"]]
    ok = len(shared) >= 4 and len(same) == len(shared) and few["resident"] < many["resident"]
    print(f"  region-scatter {'PASS' if ok else 'FAIL'}  {few['resident']} regions resident at "
          f"a short radius against {many['resident']} at the shipping one (want fewer), and "
          f"{len(same)} of {len(shared)} shared cells carry the same placement digest "
          f"(want all, on at least 4)")
    if not ok:
        failures.append("region-scatter")

    # --- the round trip ------------------------------------------------------
    walk, walk_cells, walk_text = _region_run(REGION_CHURN + REGION_WALK)
    # Thirty frames for the run that never moves: its resident set is settled on
    # the first descent and 370 more frames of standing still say nothing.
    still, still_cells, _ = _region_run(REGION_CHURN, frames="30")
    if not walk or not still:
        print("  region-return ERROR while walking")
        return failures + ["region-return", "region-shift", "region-leak", "region-collider"]

    shared = sorted(set(walk_cells) & set(still_cells))
    same = [k for k in shared if walk_cells[k]["digest"] == still_cells[k]["digest"]]
    churned = walk["freed"] > 0 and walk["loaded"] > walk["resident"]
    ok = churned and len(shared) >= 4 and len(same) == len(shared)
    print(f"  region-return {'PASS' if ok else 'FAIL'}  the walk loaded {walk['loaded']} "
          f"regions and freed {walk['freed']} (want both moving), and {len(same)} of "
          f"{len(shared)} cells it shares with a run that never moved carry the same digest "
          f"(want all, on at least 4)")
    if not ok:
        failures.append("region-return")

    # --- the round trip across an ORIGIN SHIFT --------------------------------
    # regions_rebuild frees every resident region, resets the prototype groups to
    # identity and scatters again, and forest.c asserts outright that what comes
    # back is what was there. That is a claim about the app's hardest frame and
    # nothing exercised it -- the arms above never move the origin.
    #
    # Read on the AUTHORED digest, not the raw one. A shift offsets every stored
    # position by the delta, so the raw bytes are supposed to differ; what has to
    # survive is where the prop stands in the authored world. The raw digest is
    # asserted to MOVE in the same breath, because a shift that quietly did
    # nothing would satisfy the authored bar perfectly.
    #
    # Comparable at all only because water is refused under --origin-shift-at and
    # the shore rule keys on the ISLAND rather than on a Water object existing --
    # so the shifted run drops the sea and still rejects the same drowned ground.
    #
    # BOTH runs take --world-offset, and that is what makes the comparison mean
    # anything. The offset is MATERIALISED into every coordinate, so it defines
    # the authored world; giving it to only one run would compare two different
    # worlds. It is also what lets the shift fire at all -- the snap is to a
    # 256-unit lattice about the camera, and forest spawns at the domain centre,
    # so a shift with the world at the origin rounds to zero and does nothing.
    # The vacuity bar below is what caught exactly that.
    region_offset = ["--world-offset", str(int(ORIGIN_AUTO))]
    shifted, shifted_cells, _ = _region_run(
        REGION_CHURN + region_offset + ["--origin-shift-at", "12"], frames="30")
    plain, plain_cells, _ = _region_run(REGION_CHURN + region_offset, frames="30")
    if not shifted or not plain:
        print("  region-shift ERROR while shifting the origin")
        return failures + ["region-shift", "region-leak", "region-collider"]

    shared = sorted(set(shifted_cells) & set(plain_cells))
    same = [k for k in shared if shifted_cells[k]["authored"] == plain_cells[k]["authored"]]
    moved = [k for k in shared if shifted_cells[k]["digest"] != plain_cells[k]["digest"]]
    ok = len(shared) >= 4 and len(same) == len(shared) and len(moved) == len(shared)
    print(f"  region-shift {'PASS' if ok else 'FAIL'}  across an origin shift {len(same)} of "
          f"{len(shared)} shared cells stand at the same AUTHORED positions (want all, on at "
          f"least 4), while {len(moved)} of {len(shared)} moved in storage (want all, or the "
          f"shift did nothing and the first bar is vacuous)")
    if not ok:
        failures.append("region-shift")

    # --- leak ----------------------------------------------------------------
    # `balanced` is the weakest of the three and is kept only as a consistency
    # check on the counters: region_load early-returns when already resident and
    # region_free when not, so loaded - freed == resident holds by construction --
    # including for a residency that frees NOTHING. It cannot see the leak this
    # arm is named for either, since a region freed without detaching its nodes
    # still balances. `walk["freed"] > 0` is what makes the walk churn at all, and
    # node_sum is what actually catches a node the graph kept.
    balanced = walk["loaded"] - walk["freed"] == walk["resident"]
    node_sum = sum(c["nodes"] for c in walk_cells.values())
    colliders = all(c["collider"] == 1 for c in walk_cells.values())
    ok = (balanced and walk["freed"] > 0 and node_sum == walk["nodes"] and colliders
          and walk_cells)
    print(f"  region-leak {'PASS' if ok else 'FAIL'}  loaded {walk['loaded']} - freed "
          f"{walk['freed']} = {walk['loaded'] - walk['freed']} against {walk['resident']} "
          f"resident (want equal); {node_sum} nodes over the resident regions against the "
          f"{walk['nodes']} the app counts (want equal); every resident region has a collider: "
          f"{colliders}")
    if not ok:
        failures.append("region-leak")

    # --- the seam ------------------------------------------------------------
    samples = _FOREST_TRACE.findall(walk_text)
    if len(samples) < 4:
        print(f"  region-collider FAIL  {len(samples)} trace samples, want a walk")
        return failures + ["region-collider"]
    # The first sample is the spawn drop, which is legitimately airborne.
    settled = samples[1:]
    # The app's cells start at -side*span/2, not at the world origin, so a bare
    # floor(x / span) is phase-shifted by half a span and counts a lattice that is
    # not the one carrying the colliders. It happened to clear the bar anyway,
    # which is exactly why it was worth correcting rather than leaving.
    half = walk["side"] * walk["span"] * 0.5

    def cell(v):
        return int(math.floor((float(v) + half) / walk["span"]))

    cells = {(cell(x), cell(z)) for x, _, z, _, _ in settled}
    grounded = all(int(g) == 1 for _, _, _, g, _ in settled)
    ok = grounded and len(cells) >= 3
    print(f"  region-collider {'PASS' if ok else 'FAIL'}  grounded on all {len(settled)} "
          f"settled samples: {grounded}, over {len(cells)} distinct region cells (want >= 3, or "
          f"the walk never left one and the seam was never crossed)")
    if not ok:
        failures.append("region-collider")

    return failures


def run_forest_gate(workdir):
    """The forest app: does scattered content actually batch, and does LOD fire.

    Every arm reads the SUBMISSION table, which is integers with no noise floor.
    The app pins its own exposure, so nothing here inherits the adaptation hazard
    that CLAUDE.md names for cross-configuration comparison.
    """
    if not os.path.exists(FOREST):
        print("  forest       SKIP  (forest not built)")
        return []

    failures = []
    base = _forest_run("base", [])
    # Nothing below can mean anything if the base run did not report, so this is
    # the one arm that returns rather than accumulating.
    if base is None:
        return ["forest-parse"]
    opaque = base["opaque"]

    # --- forest-chains: every generated mesh got a chain --------------------
    # mesh_build_lod_chain has one caller in the engine, in import.c. Everything
    # this app draws is generated, so if the app stopped asking for chains every
    # LOD arm below would still pass -- against a scene where nothing had one.
    #
    # Exact, not `> 0`. Sixty-four of the meshes are terrain tiles, so a
    # regression that refused a chain to every TREE and every ROCK would still
    # leave built == 64, and forest-lod would still pass on the terrain alone.
    # Both operands are read from the app rather than mirrored here.
    built = base["chains"]["built"]
    refused = base["chains"]["refused"]
    distinct = base["meshes"]
    ok = refused == 0 and built == distinct
    print(f"  forest-chains {'PASS' if ok else 'FAIL'}  {built} chains built, {refused} refused, "
          f"against {distinct} distinct meshes (want every mesh chained)")
    if not ok:
        failures.append("forest-chains")

    # --- forest-batch-ship: batching still happens as the engine ACTUALLY runs -
    # Reads `base`, so it costs no extra render. A low floor on purpose: with LOD
    # and the depth sort both fragmenting runs the shipping ratio is ~1.77, and
    # the thing worth catching is not a drift from that but a COLLAPSE to 1.0,
    # which is the value that means batching stopped happening at all.
    inst = opaque["instances"]
    ship = inst / opaque["draws"] if opaque["draws"] else 0.0
    ok = ship >= 1.4
    print(f"  forest-batch-ship {'PASS' if ok else 'FAIL'}  shipping config: {inst} instances "
          f"in {opaque['draws']} draws (ratio {ship:.2f}, want >= 1.4; 1.0 means no batching)")
    if not ok:
        failures.append("forest-batch-ship")

    # --- forest-batch-off: one draw per instance, the baseline for the ratio -
    off = _forest_run("noinst", ["--no-instancing"])
    if off is None:
        failures.append("forest-batch-off")
        failures.append("forest-batch")
    else:
        o = off["opaque"]
        ok = o["draws"] == o["instances"] and o["draws"] > 0
        print(f"  forest-batch-off {'PASS' if ok else 'FAIL'}  --no-instancing: "
              f"{o['draws']} draws for {o['instances']} instances (want equal)")
        if not ok:
            failures.append("forest-batch-off")

    # --- forest-batch: the scatter actually batches -------------------------
    # Measured with LOD OFF and SORTING OFF, because the batcher joins only
    # consecutive items sharing (mesh, lod) and each of those is a separate
    # source of fragmentation this arm is not about:
    #
    #   LOD    splits a prototype spanning a distance range across levels.
    #   SORT   scatters identical meshes across DRAW_SORT_DEPTH_BUCKETS.
    #
    # Neither conflation is academic. With LOD on the ratio reads 2.31 at the
    # pinned seed and 1.94 at --seed 7, so no threshold with useful margin can
    # sit on it; with sorting on as well it reads 2.1. With both off the same
    # property reads 13.2 and 10.4, which is a claim about the batcher.
    #
    # This is not the threshold being relaxed to fit: the floor stays at 8, and
    # both flags remove a KNOWN confounder rather than a regression. The cost in
    # the configuration that actually ships is forest-batch-ship, below --
    # written because excluding two confounders left NO arm measuring batching
    # as the engine runs it, and a change that collapsed it to one draw per
    # instance would have passed everything here.
    nolod = _forest_run("nolod", ["--no-lod", "--no-sort-opaque"])
    if nolod is None:
        failures.append("forest-batch")
    else:
        n = nolod["opaque"]
        n_draws, n_inst = n["draws"], n["instances"]
        ratio = (n_inst / n_draws) if n_draws else 0.0
        ok = ratio >= 8.0
        print(f"  forest-batch {'PASS' if ok else 'FAIL'}  opaque {n_inst} instances in {n_draws} "
              f"draws with LOD and sorting off (ratio {ratio:.1f}, want >= 8)")
        if not ok:
            failures.append("forest-batch")

    # --- forest-lod: level selection fires on generated geometry ------------
    # A fixed camera comparing on against off, NOT a distance sweep: pulling back
    # over scattered content reveals more world, so triangles rise with distance
    # however well LOD works. Identical instances is what proves the difference is
    # level selection and not visibility.
    #
    # Its OWN framing since 11.63, and its own pair of runs with it. The app is an
    # island now: standing on it at FOREST_CAM every prop is inside a 425-unit disc
    # and LOD has almost nothing to coarsen — the same scene reads 5% saved there
    # and 44% from off the shore. Five per cent is not LOD failing, it is a camera
    # with nothing distant in front of it, and an arm about DISTANCE needs a
    # framing that has some.
    lod_on = _forest_run("lod_on", [], cam=FOREST_CAM_WIDE)
    lod_off = _forest_run("lod_off", ["--no-lod"], cam=FOREST_CAM_WIDE)
    if not lod_on or not lod_off:
        failures.append("forest-lod")
    else:
        w_on, w_off = lod_on["opaque"], lod_off["opaque"]
        same_vis = w_on["instances"] == w_off["instances"]
        saving = 1.0 - (w_on["triangles"] / w_off["triangles"]) if w_off["triangles"] else 0.0
        ok = same_vis and saving >= 0.10
        print(f"  forest-lod   {'PASS' if ok else 'FAIL'}  {w_on['triangles']} triangles with "
              f"LOD vs {w_off['triangles']} without ({saving * 100.0:.0f}% saved, want >= 10%), "
              f"both carrying {w_on['instances']} instances")
        if not ok:
            failures.append("forest-lod")

    # --- forest-order: spatial ordering is what makes batching work ---------
    # The app's largest finding, and without this arm nothing defends it. The
    # guard is that instances and triangles must be IDENTICAL: the two runs draw
    # exactly the same geometry, and the only thing that changed is whether the
    # survivors of a frustum test ended up next to each other in the list.
    #
    # Both runs take --no-sort-opaque, because the depth sort REPLACES the
    # scatter order this arm is about: with it on the comparison reads 1684 vs
    # 1819 instead of 1287 vs 2368, which is the arm measuring the bucket count
    # rather than the Morton ordering. Sorting moves neither instances nor
    # triangles, so the base run stays usable as the sorted side elsewhere.
    sorted_run = _forest_run("morton", ["--no-sort-opaque"])
    unsorted_run = _forest_run("nosort", ["--no-sort-opaque", "--no-spatial-sort"])
    if sorted_run is None or unsorted_run is None:
        failures.append("forest-order")
    else:
        s = sorted_run["opaque"]
        u = unsorted_run["opaque"]
        m_draws, m_inst = s["draws"], s["instances"]
        same_work = u["instances"] == m_inst and u["triangles"] == s["triangles"]
        ok = same_work and m_draws < u["draws"]
        print(f"  forest-order {'PASS' if ok else 'FAIL'}  Morton {m_draws} draws vs unsorted "
              f"{u['draws']}, same {m_inst} instances and "
              f"{'same' if u['triangles'] == s['triangles'] else 'DIFFERENT'} "
              f"triangles (want fewer draws for identical work)")
        if not ok:
            failures.append("forest-order")

    # --- forest-rest: a character with no input does not travel -------------
    # Slope sliding is invisible from inside the app, because the camera follows
    # the thing that is drifting. It also survived one fix that looked right:
    # leaving ANY downward velocity on a grounded character makes ExtendedUpdate
    # resolve it along the surface, so a -2 m/s residual became 0.36 m/s downhill
    # on a 10-degree face. Asserted on the position, which cannot be argued with.
    rest = subprocess.run(
        [FOREST, "-x", "-f", "240", "-W", "320", "-H", "180", "--trace-player", "--no-fog"],
        capture_output=True, text=True)
    rest_text = rest.stdout + rest.stderr
    samples = _FOREST_TRACE.findall(rest_text)
    # Skip the first two: the capsule has to resolve contact before it can be
    # said to be standing on anything.
    settled = samples[2:]
    if rest.returncode != 0 or len(settled) < 4:
        print(f"  forest-rest  FAIL  exit {rest.returncode}, {len(samples)} trace samples: "
              f"{rest_text.strip()[-200:]}")
        failures.append("forest-rest")
    else:
        xs = [float(s[0]) for s in settled]
        zs = [float(s[2]) for s in settled]
        drift = max(abs(max(xs) - min(xs)), abs(max(zs) - min(zs)))
        # Grounded on every settled sample, or a character that fell THROUGH the
        # collider would pass: its x/z are constant too. The spec's physics
        # check asks for exactly this and nothing was asserting it.
        grounded = all(s[3] == "1" for s in settled)
        # And the ground has to be sloped, or the arm is vacuous -- a flat spawn
        # cannot express the defect, which is downhill travel.
        slope = min(float(s[4]) for s in settled)
        ok = drift < 0.05 and grounded and slope < 0.99
        print(f"  forest-rest  {'PASS' if ok else 'FAIL'}  idle drift {drift:.3f} units over "
              f"{len(settled)} samples (want < 0.05), grounded throughout: {grounded}, "
              f"ground normal y {slope:.3f} (want < 0.99, i.e. actually on a slope)")
        if not ok:
            failures.append("forest-rest")

    # --- forest-cull: the frustum still removes most of the world -----------
    away = _forest_run("away", [], cam=FOREST_CAM_AWAY)
    if away is None:
        failures.append("forest-cull")
        failures.append("overdraw-empty")
    else:
        # --- overdraw-empty: nothing shaded where nothing is drawn ----------
        # What is left here of the old overdraw-probe, which also tried to assert
        # the reading over the forest and could not: that number moves with the
        # AA mode, the draw order and the scatter, so it sat at 1.05 against a
        # > 1.0 bar. The exact half of the claim moved to overdraw-exact, where
        # the answer is an integer. This half still belongs on forest, because
        # only a real scene can be emptied by the CULLER rather than by framing.
        #
        # GL_SAMPLES_PASSED is the neighbouring primitive to GL_TIME_ELAPSED, and
        # this driver answers GL_TIMESTAMP with 0 while its scoped queries work,
        # so "the call was accepted" proves nothing: aimed at empty sky the only
        # reading that can be right is exactly 0, and over the forest it must not
        # be.
        sky = away["shading"]
        base_shading = base["shading"]
        ok = sky["shaded"] == 0 and base_shading["shaded"] > 0
        print(f"  overdraw-empty {'PASS' if ok else 'FAIL'}  {sky['shaded']} samples aimed at "
              f"sky (want exactly 0), {base_shading['shaded']} over the forest (want > 0; "
              f"complexity {base_shading['complexity']:.2f}, pinned by overdraw-exact)")
        if not ok:
            failures.append("overdraw-empty")

        a = away["opaque"]
        # Exact, not an inequality: aimed into the sky every mesh is outside the
        # frustum, so seen == culled and draws == 0.
        #
        # The second half stops a scene that failed to load from satisfying the
        # first, and it counts PROPS -- seen minus the terrain's own meshes.
        # Comparing raw `seen` across the two framings stopped working when the
        # terrain became a quadtree, because the aimed-away camera is somewhere
        # else and selects a different patch set. The scatter is what is supposed
        # to be identical between them, and now that is what is compared.
        props_away = a["meshes seen"] - away["terrain_meshes"]
        props_base = opaque["meshes seen"] - base["terrain_meshes"]
        ok = (props_away == props_base and a["meshes culled"] == a["meshes seen"]
              and a["draws"] == 0)
        print(f"  forest-cull  {'PASS' if ok else 'FAIL'}  aimed away: {a['meshes culled']} of "
              f"{a['meshes seen']} culled and {a['draws']} draws (want all culled, 0 draws), "
              f"carrying {props_away} prop meshes against the base framing's {props_base} "
              f"(want equal: the scatter does not move when the camera does)")
        if not ok:
            failures.append("forest-cull")

    return failures


LOD_FIXTURE = "lod_fixture.gltf"
# Eye positions marching away from the same target. Not a golden's framing --
# the arm reads triangle counts, and what matters is that the sweep crosses
# enough of the ladder for a level to change.
LOD_SWEEP = ("0,1.5,4", "0,3,30", "0,6,90")


def _skin_fixture_triangles():
    """Triangle count of the skinned fixture, read from the glTF it generated."""
    path = os.path.join(ROOT, "assets", SKIN_FIXTURE)
    with open(path) as f:
        gltf = json.load(f)
    prim = gltf["meshes"][0]["primitives"][0]
    return gltf["accessors"][prim["indices"]]["count"] // 3


def _lod_run(workdir, tag, eye, extra):
    return _profiled_run(workdir, f"lod_{tag}",
                         ["--cam-eye", eye, "--cam-target", "0,0.5,-6"] + extra,
                         fixture=LOD_FIXTURE, size=("400", "300"))


def run_lod_gate(workdir):
    """LOD chains: that one gets built, that distance selects down it, that the
    flag reaches it, and that skinned geometry is refused one.

    Everything here is an integer out of the SUBMISSION table's `triangles`
    column, which is the ONLY counter a level change moves -- switching level
    leaves draws and instances exactly where they were, so an arm watching those
    could not tell working selection from none.
    """
    if not os.path.exists(os.path.join(ROOT, "assets", LOD_FIXTURE)):
        print(f"  lod          SKIP  (missing {LOD_FIXTURE})")
        return []

    failures = []
    runs = [_lod_run(workdir, f"near{i}", eye, []) for i, eye in enumerate(LOD_SWEEP)]
    # `import` is None when the log line does not match, so this also catches a
    # renamed counter -- and catches it as a named failure rather than as a
    # TypeError that takes the whole suite down with it.
    if any(r is None or r["import"] is None for r in runs):
        return ["lod-parse"]

    # --- lod-chain: meshoptimizer actually produced one ---------------------
    # Without this every arm below passes trivially on a fixture that has no
    # chain to select from: the triangle count would simply never move.
    #
    # Exactly one, not at least one: the fixture has two meshes and the ground
    # quad is two triangles, so a second chain would mean the floor stopped
    # working rather than that the fixture got better.
    chains = runs[0]["import"]["lod_chains"]
    ok = chains == 1
    print(f"  lod-chain    {'PASS' if ok else 'FAIL'}  import built {chains} LOD chain(s) "
          f"(want exactly 1: the sphere is dense and closed; the ground quad is 2 triangles)")
    if not ok:
        failures.append("lod-chain")

    # --- lod-monotone: further away is never more triangles -----------------
    tris = [r["submit"]["opaque"]["triangles"] for r in runs]
    ok = all(tris[i] >= tris[i + 1] for i in range(len(tris) - 1)) and tris[-1] < tris[0]
    print(f"  lod-monotone {'PASS' if ok else 'FAIL'}  triangles across the sweep {tris} "
          f"(want non-increasing, and the far end below the near end)")
    if not ok:
        failures.append("lod-monotone")

    # --- lod-off: the flag reaches the selection it names -------------------
    # The inverse arm. Without it lod-monotone could be measuring culling, or a
    # fixture that happens to shrink for some other reason.
    off_near = _lod_run(workdir, "off_near", LOD_SWEEP[0], ["--no-lod"])
    off_far = _lod_run(workdir, "off_far", LOD_SWEEP[-1], ["--no-lod"])
    if off_near is None or off_far is None:
        failures.append("lod-off")
    else:
        a = off_near["submit"]["opaque"]["triangles"]
        b = off_far["submit"]["opaque"]["triangles"]
        ok = a == b == tris[0]
        print(f"  lod-off      {'PASS' if ok else 'FAIL'}  --no-lod near {a} far {b} "
              f"(want both {tris[0]}: level 0 whatever the distance)")
        if not ok:
            failures.append("lod-off")

    # --- lod-bias: the knob reaches the ladder ------------------------------
    # Shipped with a flag and a GUI slider and, until this arm, nothing that
    # could tell a parsed bias from a dropped one -- which is the failure the
    # spec's own arm table names as having been shipped twice already.
    #
    # Measured at the MIDDLE eye, the only one of the three with levels above
    # and below it to move into.
    hi = _lod_run(workdir, "bias_hi", LOD_SWEEP[1], ["--lod-bias", "4"])
    lo = _lod_run(workdir, "bias_lo", LOD_SWEEP[1], ["--lod-bias", "0.25"])
    if hi is None or lo is None:
        failures.append("lod-bias")
    else:
        h = hi["submit"]["opaque"]["triangles"]
        l = lo["submit"]["opaque"]["triangles"]
        ok = h > tris[1] > l
        print(f"  lod-bias     {'PASS' if ok else 'FAIL'}  bias 4 -> {h}, unbiased {tris[1]}, "
              f"bias 0.25 -> {l} (want strictly decreasing: >1 holds detail, <1 drops it)")
        if not ok:
            failures.append("lod-bias")

    # --- lod-skinned: decimation does not reach a rig -----------------------
    # Over lod.c's triangle floor by a wide margin, so a refusal here can only be
    # the skinning rule. Read from the generator rather than mirrored: a fixture
    # that shrank below the floor would make this arm pass for the wrong reason,
    # silently.
    skin_tris = _skin_fixture_triangles()
    skinned = _profiled_run(workdir, "lod_skin", [], fixture=SKIN_FIXTURE, size=("400", "300"))
    if skinned is None or skinned["import"] is None:
        failures.append("lod-skinned")
    elif skin_tris < 256:
        print(f"  lod-skinned  FAIL  fixture is {skin_tris} triangles, under lod.c's floor -- "
              f"a refusal would prove nothing about skinning")
        failures.append("lod-skinned")
    else:
        n = skinned["import"]["lod_chains"]
        ok = n == 0
        print(f"  lod-skinned  {'PASS' if ok else 'FAIL'}  {n} chains on a {skin_tris}-triangle "
              f"SKINNED mesh (want 0: weights do not transfer to surviving vertices)")
        if not ok:
            failures.append("lod-skinned")

    return failures


def run_submission_gate(workdir):
    if not os.path.exists(os.path.join(ROOT, "assets", SUBMIT_FIXTURE)):
        print(f"  submit       SKIP  (missing {SUBMIT_FIXTURE})")
        return []

    failures = []
    mesh_nodes = _fixture_mesh_nodes()
    base = _submit_run(workdir, "base", [])
    if base is None:
        return ["submit-parse"]
    counts, gpu, cpu = base["submit"], base["gpu"], base["cpu"]

    # --- submit-parse: the report format is the assertion surface -----------
    # Asserts each block is NON-EMPTY as well as fully parsed: a renamed banner
    # yields zero unparsed rows and an empty table, which "0 unparsed" alone
    # reads as success.
    unparsed = sum(base["unparsed"].values())
    empty = [k for k in ("gpu", "cpu", "submit") if not base[k]]
    header_ok = base["submit_header"] == " ".join(
        f"{c:>10}" for c in SUBMIT_COLS).strip()
    ok = not empty and unparsed == 0 and header_ok
    print(f"  submit-parse {'PASS' if ok else 'FAIL'}  {len(counts)} passes counted, "
          f"{unparsed} unparsed, empty blocks {empty or 'none'}, "
          f"header matches: {header_ok}")
    if not ok:
        failures.append("submit-parse")

    # --- dedup-count: the importer built each mesh once ---------------------
    # Both terms come from the mechanism, not from the file: a cache that stops
    # hitting reports every node reference as a build and zero shares, which is
    # the dead-feature failure. The expectations are read from the glTF, so
    # regenerating the fixture cannot leave the arm asserting a stale shape.
    with open(os.path.join(ROOT, "assets", "instancing_fixture.gltf")) as f:
        want_built = len(json.load(f)["meshes"])
    imported = base.get("import")
    if imported is None:
        print("  dedup-count  FAIL  no import line to read")
        failures.append("dedup-count")
    else:
        want_shared = mesh_nodes - want_built
        ok = imported["built"] == want_built and imported["shared"] == want_shared
        print(f"  dedup-count  {'PASS' if ok else 'FAIL'}  {imported['built']} built "
              f"(want {want_built}, the glTF's mesh count) and {imported['shared']} shared "
              f"(want {want_shared} = {mesh_nodes} nodes - {want_built} meshes)")
        if not ok:
            failures.append("dedup-count")

    # --- list-once: the graph is flattened once, not once per pass ----------
    # Every pass now iterates one list, so a mesh is SEEN once per pass that
    # wants its lane -- never four times in the camera passes as the old walks
    # did. The fixture is all-opaque, so the camera side must see each mesh
    # exactly once: the OIT and late passes have no lane to draw.
    #
    # Fails if a pass reverts to walking the graph, or if a lane filter stops
    # filtering and every pass submits everything.
    opaque_seen = counts.get("opaque", {}).get("meshes seen")
    late = [p for p in ("oit moments", "oit accumulate", "transparent") if p in counts]
    ok = opaque_seen == mesh_nodes and not late
    print(f"  list-once    {'PASS' if ok else 'FAIL'}  opaque saw {opaque_seen} "
          f"(want {mesh_nodes}, one per mesh node); late passes present: {late or 'none'}")
    if not ok:
        failures.append("list-once")

    # --- submit-sum: two counters incremented on opposite branches ----------
    # Evaluated per pass and on the far run below, so the culled term is not
    # always zero. On the base run the fixture is entirely in frustum by design,
    # which is exactly why the base run alone could not fail this.
    ok, detail = _submit_sum_detail(base, "base")
    print(f"  submit-sum   {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("submit-sum")

    # --- submit-count: the totals are predictable before the render ---------
    # The camera pass sees every mesh-bearing node once; each cascade sees them
    # again. Fails if a pass stops drawing, double-counts, or the fixture
    # changes shape (the node count is read from the glTF, not copied).
    opaque = counts.get("opaque", {})
    shadow = counts.get("shadow cascades", {})
    want_shadow = mesh_nodes * SUBMIT_CASCADES
    # Culling and batching both moved DRAWS away from the mesh count, so the
    # exact quantities are what a pass SAW and what it ultimately submitted:
    # every mesh node once per pass, each either carried by a draw or culled.
    ok = (opaque.get("instances") == mesh_nodes
          and shadow.get("meshes seen") == want_shadow
          and shadow.get("instances", 0) + shadow.get("meshes culled", 0) == want_shadow)
    print(f"  submit-count {'PASS' if ok else 'FAIL'}  opaque carried "
          f"{opaque.get('instances')} instances (want {mesh_nodes}, one per mesh node); "
          f"shadow saw {shadow.get('meshes seen')} (want {want_shadow} = {mesh_nodes} x "
          f"{SUBMIT_CASCADES} cascades) and carried {shadow.get('instances')} + culled "
          f"{shadow.get('meshes culled')}")
    if not ok:
        failures.append("submit-count")

    # --- submit-matsw: the depth pass uploads material blocks, and says so ---
    # The fixture has two materials; each cascade re-walks the graph, so the
    # depth path must report exactly one switch per material per cascade. Wired
    # to one of the two walkers this reads 0, which is what it read before the
    # counter was added to shadow.c.
    want_matsw = 2 * SUBMIT_CASCADES
    got_matsw = shadow.get("material switches")
    ok = got_matsw == want_matsw
    print(f"  submit-matsw {'PASS' if ok else 'FAIL'}  depth-pass material switches "
          f"{got_matsw} (want {want_matsw} = 2 materials x {SUBMIT_CASCADES} cascades)")
    if not ok:
        failures.append("submit-matsw")

    # --- shadowcull-draws: the shadow pass culls, and only where it should ---
    # At 3 cascades each layer is a slice of the view frustum, so a scene wider
    # than one slice must lose casters. At 1 cascade the fit is the WHOLE scene
    # and the same code must reject NOTHING -- that half is the inverse arm, and
    # it is what fails for a cull wired to something other than the light's own
    # volume, which would reject at both counts.
    one = _submit_run(workdir, "cascade1", [], cascades=1)
    if one is None:
        failures.append("shadowcull-draws")
    else:
        one_shadow = one["submit"].get("shadow cascades", {})
        three_shadow = counts.get("shadow cascades", {})
        ok = (one_shadow.get("meshes culled") == 0
              and one_shadow.get("instances") == mesh_nodes
              and three_shadow.get("meshes culled", 0) > 0)
        print(f"  shadowcull-draws {'PASS' if ok else 'FAIL'}  1 cascade: culled "
              f"{one_shadow.get('meshes culled')} (want 0, the fit is the whole scene) and "
              f"carried {one_shadow.get('instances')} (want {mesh_nodes}); 3 cascades: culled "
              f"{three_shadow.get('meshes culled')} (want > 0, the near slices are tighter)")
        if not ok:
            failures.append("shadowcull-draws")

    # --- shadowcull-live: culling that never rejects anything is not culling --
    # The fixture above is inside every cascade by construction, so it can only
    # show the cull NOT firing. abandoned_window is 553 meshes spread wider than
    # a near slice, so its cascades must reject a substantial share -- and the
    # goldens prove the frame is unchanged while they do.
    wide = os.path.join(ROOT, "assets", "abandoned_window", "abandoned_window_shadowed.cscn")
    if not os.path.exists(wide):
        print("  shadowcull-live SKIP  (missing abandoned_window_shadowed.cscn)")
    else:
        tables = _profiled_run(workdir, "shadowcull_wide",
                               ["--shadow-cascades", str(SUBMIT_CASCADES)],
                               fixture=os.path.join("abandoned_window",
                                                    "abandoned_window_shadowed.cscn"),
                               size=("200", "150"))
        if tables is None:
            failures.append("shadowcull-live")
        else:
            row = tables["submit"].get("shadow cascades", {})
            seen, culled = row.get("meshes seen", 0), row.get("meshes culled", 0)
            share = culled / seen if seen else 0.0
            ok = share >= 0.15
            print(f"  shadowcull-live {'PASS' if ok else 'FAIL'}  {culled} of {seen} casters "
                  f"culled ({share * 100.0:.0f}%, want >= 15%)")
            if not ok:
                failures.append("shadowcull-live")

    # --- inst-count: batching collapses draws, and instances still add up ---
    # 130 props share one mesh, so the opaque pass must submit them in chunks of
    # CETRA_INSTANCE_MAX rather than one at a time -- and every mesh must still
    # be accounted for, which is what separates real batching from dropped
    # geometry. The shadow pass batches the same way per cascade.
    inst_max = _ubo_instance_max()
    want_opaque_draws = -(-(mesh_nodes - 1) // inst_max) + 1  # props in chunks, ground alone
    ok = (opaque.get("draws") == want_opaque_draws
          and opaque.get("instances") == mesh_nodes
          and shadow.get("draws", 0) < shadow.get("instances", 0))
    print(f"  inst-count   {'PASS' if ok else 'FAIL'}  opaque {opaque.get('draws')} draws "
          f"carrying {opaque.get('instances')} instances (want {want_opaque_draws} draws for "
          f"{mesh_nodes} meshes); shadow {shadow.get('draws')} draws carrying "
          f"{shadow.get('instances')}")
    if not ok:
        failures.append("inst-count")

    # --- inst-off: the flag reaches the batching it names -------------------
    # Without this, inst-identity passes trivially for a flag that does nothing
    # -- both runs would simply be the batched path.
    unbatched = _submit_run(workdir, "nobatch", ["--no-instancing"])
    if unbatched is None:
        failures.append("inst-off")
        failures.append("inst-identity")
    else:
        un_opaque = unbatched["submit"].get("opaque", {})
        ok = (un_opaque.get("draws") == mesh_nodes
              and un_opaque.get("draws") == un_opaque.get("instances"))
        print(f"  inst-off     {'PASS' if ok else 'FAIL'}  --no-instancing: {un_opaque.get('draws')} "
              f"draws for {un_opaque.get('instances')} instances (want both {mesh_nodes}: one "
              f"draw per mesh)")
        if not ok:
            failures.append("inst-off")

        # --- inst-identity: batched and unbatched draw the same picture -----
        # The UBO carries the same floats into the same shader arithmetic, so
        # this is 0 px by construction rather than by tolerance. It is the arm
        # the attribute-divisor alternative could not have had: packing
        # transforms into the free vertex slots needs the normal matrix derived
        # in-shader, which differs in the last bits from the CPU-side one.
        #
        # Both frames come from runs the arms above already paid for, which is
        # what puts them on _gpu_cmd's pinned exposure. Rendering them here
        # instead cost two extra renders AND dropped the pin, leaving the arm
        # asserting 0 px across a live auto-exposure -- the one difference
        # CLAUDE.md names as able to move every pixel in the frame.
        on = os.path.join(workdir, "gpu_submit_base.ppm")
        off = os.path.join(workdir, "gpu_submit_nobatch.ppm")
        ae, _ = compare(on, off)
        ok = ae == 0
        print(f"  inst-identity {'PASS' if ok else 'FAIL'}  {ae} px between batched and "
              f"unbatched (want 0)")
        if not ok:
            failures.append("inst-identity")

    # --- skinned-nobatch: a run needs a program that can READ the instances --
    # Every batching precondition except one: two adjacent nodes, one shared
    # skinned mesh, one material. pbr_skinned takes the object transform from a
    # plain uniform and declares no instance block, so batching the pair draws
    # both copies at the first node's transform and the second quad vanishes.
    #
    # Asserted on draws rather than on pixels because the count says WHY: one
    # draw carrying two instances is the defect itself, where a pixel diff only
    # says the frame moved. The inverse arm below supplies the pixels.
    skin_nodes = _fixture_mesh_nodes(SKIN_FIXTURE)
    skinned = _profiled_run(workdir, "skin_on", [], fixture=SKIN_FIXTURE, size=("400", "300"))
    if skinned is None:
        failures.append("skinned-nobatch")
        failures.append("skinned-identity")
    else:
        sk_opaque = skinned["submit"].get("opaque", {})
        ok = (sk_opaque.get("draws") == skin_nodes
              and sk_opaque.get("instances") == skin_nodes)
        print(f"  skinned-nobatch {'PASS' if ok else 'FAIL'}  {sk_opaque.get('draws')} draws for "
              f"{sk_opaque.get('instances')} instances (want both {skin_nodes}: a program "
              f"without InstanceBlock may never carry more than one)")
        if not ok:
            failures.append("skinned-nobatch")

        # --- skinned-identity: and the frame proves it is not merely counted -
        # Measured at 171757 px (35.8%) against a build that batched these --
        # the whole second quad. An arm that can fail, and did.
        skin_off = _profiled_run(workdir, "skin_off", ["--no-instancing"],
                                 fixture=SKIN_FIXTURE, size=("400", "300"))
        if skin_off is None:
            failures.append("skinned-identity")
        else:
            ae, _ = compare(os.path.join(workdir, "gpu_skin_on.ppm"),
                            os.path.join(workdir, "gpu_skin_off.ppm"))
            ok = ae == 0
            print(f"  skinned-identity {'PASS' if ok else 'FAIL'}  {ae} px between default and "
                  f"--no-instancing (want 0: neither may batch this)")
            if not ok:
                failures.append("skinned-identity")

    # --- submit-cull: the counters are attached to submission, not to nodes -
    # A camera aimed away empties the camera pass. The shadow pass follows the
    # LIGHT, so it still sees every caster and still draws most of them --
    # which is what makes this fail for a counter wired to the node list: that
    # one would report the same number for both passes.
    far = _submit_run(workdir, "far", ["--cam-eye", "500,500,500",
                                       "--cam-target", "501,500,500"])
    if far is None:
        failures.append("submit-cull")
    else:
        far_opaque = far["submit"].get("opaque", {})
        far_shadow = far["submit"].get("shadow cascades", {})
        # A timing, so compared with a tolerance rather than to zero exactly --
        # the row is present and should be empty, not absent.
        opaque_ms = far["gpu"].get("opaque", -1.0)
        ok = (far_opaque.get("meshes culled") == mesh_nodes
              and far_opaque.get("draws") == 0
              and far_shadow.get("meshes seen") == want_shadow
              and far_shadow.get("draws", 0) > 0
              and 0.0 <= opaque_ms < 0.01)
        print(f"  submit-cull  {'PASS' if ok else 'FAIL'}  opaque culled "
              f"{far_opaque.get('meshes culled')} (want {mesh_nodes}) and drew "
              f"{far_opaque.get('draws')} (want 0); shadow still saw "
              f"{far_shadow.get('meshes seen')} (want {want_shadow}) and drew "
              f"{far_shadow.get('draws')} (want > 0: it follows the light, not the camera); "
              f"opaque GPU {opaque_ms} ms (want < 0.01)")
        if not ok:
            failures.append("submit-cull")
        # The identity again, on the run where culled is non-zero.
        ok, detail = _submit_sum_detail(far, "far camera")
        print(f"  submit-sum-far {'PASS' if ok else 'FAIL'}  {detail}")
        if not ok:
            failures.append("submit-sum-far")

    # --- submit-exact: an integer has no run-to-run spread ------------------
    # If this ever fails the counters are frame-phase dependent and cannot
    # carry a claim, which would invalidate every arm above.
    again = _submit_run(workdir, "again", [])
    if again is None:
        failures.append("submit-exact")
    else:
        diff = {p: (counts.get(p), again["submit"].get(p))
                for p in set(counts) | set(again["submit"])
                if counts.get(p) != again["submit"].get(p)}
        ok = not diff
        print(f"  submit-exact {'PASS' if ok else 'FAIL'}  "
              f"{'identical across two runs' if ok else f'differs: {diff}'}")
        if not ok:
            failures.append("submit-exact")

    # --- cpu-bound: the CPU column has a ceiling, and it is structural ------
    # Scopes are flat and non-overlapping, so no pass can spend more wall time
    # than the frame that contains it. Without this, a CPU column reading an
    # absolute clock rather than an interval prints milliseconds-since-startup
    # for every row and the whole suite stays green -- which is the state this
    # arm was written after reproducing.
    frame_ms = cpu.get("FRAME (wall)", 0.0)
    over = {name: ms for name, ms in cpu.items()
            if name not in ("TIMED", "FRAME (wall)") and ms > frame_ms + 0.05}
    timed_ok = cpu.get("TIMED", 0.0) <= frame_ms + 0.05
    ok = frame_ms > 0.0 and not over and timed_ok
    print(f"  cpu-bound    {'PASS' if ok else 'FAIL'}  TIMED {cpu.get('TIMED')} ms and every row "
          f"<= FRAME {frame_ms} ms; over-budget rows: {over or 'none'}")
    if not ok:
        failures.append("cpu-bound")

    # --- cpu-attrib: the CPU column measures something the GPU one cannot ---
    # Rows that read exactly 0.000 ms of GPU time still cost CPU. The subject is
    # whichever passes fall below the print threshold at this gate's size, NOT
    # specifically blits -- three of the rows it finds are not blits, and one
    # leaves the set entirely at higher resolution. So it SKIPs when the set is
    # empty; cpu-bound above is the arm that always applies.
    blit_rows = [(n, cpu.get(n, 0.0)) for n, ms in gpu.items()
                 if ms == 0.0 and n not in ("TIMED", "FRAME (wall)")]
    hits = [(n, c) for n, c in blit_rows if c > 0.0]
    if not blit_rows:
        print("  cpu-attrib   SKIP  no zero-GPU rows at this size")
    else:
        ok = bool(hits)
        print(f"  cpu-attrib   {'PASS' if ok else 'FAIL'}  {len(hits)} of {len(blit_rows)} "
              f"zero-GPU rows cost CPU: {hits[:3] if hits else 'none'}")
        if not ok:
            failures.append("cpu-attrib")

    return failures
# ---------------------------------------------------------------------------
# Draw-list identity (spec 11.28 Phase 3)
#
# The list replaced two scene-graph walkers, and its whole claim is that it
# reproduces their submission ORDER. Order is visible three ways in this
# renderer -- alpha-to-coverage dithering, the unsorted late pass under
# --no-oit, and transmissive-over-transmissive blending -- so these arms render
# scenes that exercise each and compare against the same build's own output.
#
# Self-verifying like every other gate here: no stored reference. What makes
# them able to fail is that a reordering changes pixels in exactly the fixtures
# chosen, and the goldens (baked from an older build) cannot see a reordering
# that a rebake would silently absorb.
LIST_FIXTURES = (
    ("oit_cards_fixture.cscn", ["--no-oit"], "unsorted late pass, order fully visible"),
    ("oit_sphere_fixture.gltf", [], "transmissive over transmissive"),
    ("hair_fixture.cscn", [], "alpha-to-coverage dither"),
)


def run_draw_list_gate(workdir):
    failures = []
    for fixture, extra, why in LIST_FIXTURES:
        path = os.path.join(ROOT, "assets", fixture)
        if not os.path.exists(path):
            print(f"  list-identity SKIP  (missing {fixture})")
            continue
        tag = fixture.rsplit(".", 1)[0]
        a = os.path.join(workdir, f"list_{tag}_a.ppm")
        b = os.path.join(workdir, f"list_{tag}_b.ppm")
        err = render(path, a, extra)
        if err is None:
            err = render(path, b, extra)
        if err is not None:
            print(f"  list-identity ERROR {tag}: {err.strip()[-200:]}")
            failures.append("list-identity")
            continue
        ae, pae = compare(a, b)
        ok = ae == 0
        print(f"  list-identity {'PASS' if ok else 'FAIL'}  {tag}: {ae} px "
              f"(want 0; {why})")
        if not ok:
            failures.append("list-identity")

    # --- oit-identity: the sub-passes draw the same set either way ----------
    # OIT routing is the one place the lane filter has to disagree with itself
    # between passes: with OIT on the blend lane goes to the accumulate and the
    # late pass takes only transmissive; with it off the late pass takes both.
    # Rendering the same fixture both ways and asserting they DIFFER is what
    # proves the routing is live rather than collapsed to one branch.
    fixture = os.path.join(ROOT, "assets", "oit_cards_fixture.cscn")
    if not os.path.exists(fixture):
        print("  oit-identity SKIP  (missing oit_cards_fixture.cscn)")
        return failures
    on = os.path.join(workdir, "list_oit_on.ppm")
    off = os.path.join(workdir, "list_oit_off.ppm")
    err = render(fixture, on, [])
    if err is None:
        err = render(fixture, off, ["--no-oit"])
    if err is not None:
        print(f"  oit-identity ERROR {err.strip()[-200:]}")
        failures.append("oit-identity")
    else:
        ae, _ = compare(on, off)
        ok = ae > 0
        print(f"  oit-identity {'PASS' if ok else 'FAIL'}  {ae} px between OIT on and off "
              f"(want > 0: equal means the lane routing collapsed to one path)")
        if not ok:
            failures.append("oit-identity")

    return failures


def run_translucent_offpath_gate(workdir):
    """Off is byte-identical; ON is byte-identical too where nothing is
    translucent; and the flag must visibly DO something where something is.

    The third arm is the one 11.22 had to learn twice: three zeros are also what
    a dead flag, an unparsed argument or a failed allocation produce.
    """
    failures = []
    for name, rel, extra in (
        ("cornell_point", "assets/cornell_point.cscn", ["-W", "800", "-H", "600"]),
        ("dir_shadow", "assets/dir_shadow_fixture.cscn", ["-W", "800", "-H", "600", "--no-pcss"]),
        ("contact", "assets/contact_fixture.cscn", ["-W", "640", "-H", "400"]),
    ):
        scene = os.path.join(ROOT, rel)
        if not os.path.exists(scene):
            print(f"  tsl-off      SKIP  (missing {rel})")
            continue
        outs = []
        for tag, flag in (("off", []), ("on", ["--translucent-shadows"])):
            out = os.path.join(workdir, f"tsloff_{name}_{tag}.ppm")
            cmd = [RENDER, "-m", scene, "-x", "-f", "30", "--no-auto-exposure", "-E", "1.0",
                   "-S", out] + extra + flag
            r = _run(cmd, capture_output=True, text=True)
            # The exit code, not just the file: a render that dies partway can
            # leave a short or half-written PPM behind, and existence alone
            # would read that as a successful frame and compare it. The child's
            # output goes with the error, because a gate that says only "ERROR"
            # gives the next person nothing to work from.
            if r.returncode != 0:
                print(f"  tsl-off      ERROR {name}/{tag} exited {r.returncode}: "
                      f"{(r.stdout + r.stderr).strip()[-300:]}")
                outs.append(None)
                continue
            outs.append(out if os.path.exists(out) else None)
        if not all(outs):
            print(f"  tsl-off      ERROR while rendering {name}")
            failures.append("tsl-offpath")
            continue
        ae, _ = compare(outs[0], outs[1])
        ok = ae == 0
        print(f"  tsl-off      {'PASS' if ok else 'FAIL'}  {name}: {ae} px with the flag ON "
              f"(want exactly 0; nothing here is translucent)")
        if not ok:
            failures.append("tsl-offpath")

    off = _tsl_render(workdir, "live_off", ["--no-translucent-shadows"])
    on = _tsl_render(workdir, "live_on", ["--translucent-shadows"])
    if off is None or on is None:
        print("  tsl-live     ERROR while rendering the fixture pair")
        return failures + ["tsl-live"]
    ae, _ = compare(off, on)
    ok = ae >= TSL_LIVE_MIN_PX
    print(f"  tsl-live     {'PASS' if ok else 'FAIL'}  {ae} px between off and on "
          f"(want >= {TSL_LIVE_MIN_PX}: the three zeros above are also what a dead flag gives)")
    if not ok:
        failures.append("tsl-live")
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


# Mirrors gen_light_import_fixture.py's spot variant and has to match it, or the
# gate predicts a cone the asset does not author.
CONE_INNER_DEG = 20.0
CONE_OUTER_DEG = 30.0


def run_cone_gate():
    """A glTF spot's cone half-angles must arrive as COSINES, and in order.

      gltf-cone   an imported 20/30 degree cone reads cos 0.9397 / 0.8660, inner
                  above outer

    import.c wrote assimp's RADIAN half-angles straight into the engine's cosine
    fields until 11.57. Nothing caught it because nothing in the corpus authored a
    spot through glTF -- cornell_leak is the only spot anywhere and it comes
    through a .cscn, which has always converted.

    The ORDERING is asserted beside the values because it is the half that breaks
    the shader rather than merely detuning it: cosine decreases with angle, so
    correct data has inner > outer, and spotConeFactor's
    epsilon = max(cutOff - outerCutOff, 1e-4) collapses to 1e-4 when that
    inverts -- turning a soft edge into a hard step. Falsified by hand at 11.57:
    the raw radians read 0.3491 / 0.5236, wrong AND inverted.
    """
    fixture = os.path.join(ROOT, "assets", "spot_import_fixture.gltf")
    if not os.path.exists(fixture):
        print("  gltf-cone    SKIP  (missing spot_import_fixture.gltf)")
        return []
    line = None
    for text in (_import_log(fixture, ["--no-scene-file"]),):
        for candidate in text.splitlines():
            if "<Light name='coned_lamp'" in candidate:
                line = candidate
    if line is None:
        print("  gltf-cone    ERROR (no spot light imported)")
        return ["gltf-cone"]

    inner = _light_field(line, "cutOff")
    outer = _light_field(line, "outerCutOff")
    if inner is None or outer is None:
        print("  gltf-cone    ERROR (light print carries no parseable cone)")
        return ["gltf-cone"]
    # outerCutOff is the last field, so it arrives with the closing '>'.
    got_inner = float(inner)
    got_outer = float(outer.rstrip(">"))
    want_inner = math.cos(math.radians(CONE_INNER_DEG))
    want_outer = math.cos(math.radians(CONE_OUTER_DEG))
    ok = (abs(got_inner - want_inner) < 1e-4 and abs(got_outer - want_outer) < 1e-4
          and got_inner > got_outer)
    print(f"  gltf-cone    {'PASS' if ok else 'FAIL'}  {CONE_INNER_DEG:.0f}/{CONE_OUTER_DEG:.0f} "
          f"degree cone imported as cos {got_inner:.4f}/{got_outer:.4f} "
          f"(want {want_inner:.4f}/{want_outer:.4f}, inner above outer -- radians would "
          f"read 0.3491/0.5236 and inverted)")
    return [] if ok else ["gltf-cone"]


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


def _run_import_gates(workdir):
    del workdir # all three drive the importer directly rather than a render
    return run_range_gate() + run_cone_gate() + run_fbx_unit_gate()


# Clustered specular probes (spec 11.70).
#
# The fixture is two rooms over ONE floor mesh, joined by a doorway at the front,
# with a bright panel in the left room and a dim one in the right. Every arm
# below reads that shared floor, because it is the geometry the whole design was
# decided by: per-DRAW probe selection, the cheaper answer this spec refused,
# hands one mesh a single probe and lights half of it with the wrong room's
# reflections -- while every per-room measurement still passes.
#
# Probes need a precomputed IBL to exist at all, and the fixture AUTHORS one --
# an environment block asking for a sky with the sun below the horizon, which
# supplies the IBL without lighting the rooms. Deliberately not passed as flags
# here: a fixture whose gate arms configure it into working, while opening it by
# hand renders something else, is the trap beach_fixture is on record for. So
# the arms pass only the exposure pin every other arm passes.
PROBE_FIXTURE = "cornell_rooms.cscn"
PROBE_SKY = ["--no-auto-exposure", "-E", "1.0"]

# Floor samples, in WORLD space, projected through the fixture's own camera.
#
# z = 0.55 is in FRONT of the partition, which ends at z = 0.30 (DOOR_Z in the
# generator): the doorway is at the front precisely so this band is visible from
# a viewpoint that sees both rooms. The dark and lit reads are mirrored about
# x = 0, so the pair differs in which room it is in and in nothing else.
PROBE_FLOOR_Z = 0.55
PROBE_DARK_X = 1.30
PROBE_LIT_X = -1.30
PROBE_PATCH = 4  # half-width in pixels of the box each sample averages

# There is deliberately NO seam arm, and it is worth saying why rather than
# leaving the gap silent.
#
# One was built and measured: sweep the floor across the doorway and compare the
# peak curvature in the handover band against a leg whose probe boxes ABUT with
# no overlap, so the two hand over discontinuously. It discriminated at 1.68x on
# an earlier lighting of this fixture, and both blend mutations drove it to
# exactly 1.00. It stopped discriminating when the fixture moved to spots: the
# two probes now agree closely at the doorway, in luma and in hue, which is what
# a doorway between two rooms lit to similar levels means -- so blending and
# switching produce nearly the same pixels there and the arm can no longer fail.
# An arm that cannot fail is worse than an absent one. What still holds the
# blend is probe-set-rooms, which both blend mutations also failed.

# How much brighter the dark room's floor gets when the probe serving it
# photographed the LIT room instead. Both legs run the identical multi path over
# the identical atlas, so this is a reading of selection alone.
# The single probe has to reach the frame at all, or every other thing
# probe-set-single checks is satisfied by a build that binds no probe. Measured
# 366,129 px -- and note these are FRAMEBUFFER pixels, so on a 1x display the
# same frame reads ~91,500. The bar clears both.
PROBE_SINGLE_EFFECT_MIN = 20000
# What moving both boxes out of frame takes away, which is what stops
# probe-set-fallback's identity holding for the wrong reason. Measured 345,230.
PROBE_FALLBACK_REMOVED_MIN = 20000

# How much brighter the dark room's floor gets when the probe serving it
# photographed the LIT room instead. Both legs run the identical multi path over
# the identical atlas, so this is a reading of selection alone.
# Measured 1.82 on the rooms arm and 1.62 under --gi-volume, which the tenancy
# arm reads against this same bar.
PROBE_ROOMS_RATIO_MIN = 1.20
# The lit room is the in-frame control: only probe B moved, so room A has to
# read the same in both legs. A bar rather than exact equality because the two
# legs are separate processes; it has measured 0.0000 every time.
PROBE_ROOMS_CONTROL_MAX = 0.01

_PROBE_SET_ROW = re.compile(
    r"probe-set frame=(\d+) count=(\d+) mode=(\w+) atlas=(\d+)x(\d+) "
    r"captures=(\d+) mask_bits=(\d+) digest=([0-9a-f]+)")


def _probe_set_rows(text):
    """The diagnostic's per-frame lines, as dicts in emission order."""
    return [{"frame": int(m[0]), "count": int(m[1]), "mode": m[2],
             "atlas": (int(m[3]), int(m[4])), "captures": int(m[5]),
             "mask_bits": int(m[6]), "digest": m[7]}
            for m in _PROBE_SET_ROW.findall(text)]


def _probe_run(workdir, tag, mutate=None, extra=None, frames=30, fixture=None):
    """Render the two-room fixture, optionally through a mutation.

    Returns (pixels, w, h, output) or (None, None, None, error).
    """
    src = os.path.join(ROOT, "assets", fixture or PROBE_FIXTURE)
    if not os.path.exists(src):
        return None, None, None, "missing fixture"
    scene = src
    if mutate is not None:
        scene = os.path.join(workdir, f"probe_{tag}.cscn")
        cscn_copy(src, scene, mutate)
    out = os.path.join(workdir, f"probe_{tag}.ppm")
    cmd = [RENDER, "-m", scene, "-x", "-f", str(frames), "-W", "400", "-H", "300",
           "-S", out] + PROBE_SKY + (extra or [])
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None, None, None, r.stdout + r.stderr
    w, h, pix = _read_ppm(out)
    return pix, w, h, r.stdout + r.stderr


def _probe_floor_luma(pix, w, h, project, x, z=PROBE_FLOOR_Z):
    """Mean linear luma of a patch centred on a world floor point."""
    px, py = project((x, 0.0, z))
    vals = [_linear_luma(pix, w, h, px + dx, py + dy)
            for dy in range(-PROBE_PATCH, PROBE_PATCH + 1)
            for dx in range(-PROBE_PATCH, PROBE_PATCH + 1)]
    return sum(vals) / len(vals)


def _probe_b_into_room_a(d):
    """The wrong-room leg: probe B keeps its box and photographs room A.

    Deliberately NOT "delete probe B", which would compare the multi path
    against the single-probe path and measure the storage change along with the
    selection. Moving only the capture point leaves everything else identical,
    so what the arm reads is which room a probe photographed and nothing else.
    """
    d["probes"][1]["position"] = list(d["probes"][0]["position"])


def _probe_boxes_offstage(d):
    """Both probes keep their captures and move their INFLUENCE out of frame.

    Every fragment then sits outside every box, so the blend's weight is zero
    everywhere and the whole frame must fall to the global environment -- which
    is the same expression the no-probe branch evaluates.
    """
    for p in d["probes"]:
        p["boxMin"] = [100.0, 100.0, 100.0]
        p["boxMax"] = [101.0, 101.0, 101.0]


# A line-initial two-space name, which is the shape an arm's printed line and a
# docstring's arm list BOTH use -- one pattern, because the check is that they agree.
# inspect.getdoc, not __doc__: Python 3.13 strips a docstring's common indentation at
# compile time and earlier versions do not, so the raw attribute puts the arm list at a
# different column depending on the interpreter.
_ARM_NAME = r"([a-z][a-z0-9]*(?:-[a-z0-9]+)*)"
_ARM_DOCUMENTED = re.compile(r"^  " + _ARM_NAME + r"(?=\s)", re.M)
# The verdict line specifically, not any line an arm prints. A SKIP or an ERROR prelude
# carries the same name, and several are emitted from a guard ABOVE the arm they belong
# to -- reading those as the arm's position reports a false order mismatch.
_ARM_PRINTED = re.compile(r'print\(f?"  ' + _ARM_NAME + r'[^"]*PASS')


def run_probe_set_gate(workdir):
    """N reflection probes, selected and blended per fragment (spec 11.70).

      probe-set-single   one probe keeps the pre-11.70 path: mode=single, no froxel
                         masks built, and two runs byte-identical
      probe-set-rooms    the dark room's floor reflects ITS room. The wrong-room leg
                         moves probe B's capture into room A and nothing else, so the
                         ratio isolates selection; the lit room is the in-frame control
      probe-set-fallback every fragment outside every box IS the no-probe frame
      probe-set-converge captures stop at the probe count and the mask digest repeats
      probe-set-tenancy  the probe atlas and the GI volume share one texture: both
                         effects survive being switched on together
      probe-set-schema   a probe missing its box is refused by name, not defaulted

    NO ARM HERE IS SAFE ALONE, and the pairing is deliberate. probe-set-single
    alone passes on a build that binds no probe at all, since every other thing it
    reads is a diagnostic line or two runs agreeing -- its frame comparison is what
    forces a probe to have reached the pixels. probe-set-rooms alone passes on a
    build that blends nothing, because the dark room is dim anyway -- its control
    read is what forces the lit room to be untouched. And probe-set-fallback alone
    passes on a build where the probes never contribute, which is exactly the
    defect this branch shipped for a day: the fade ran inward from the proxy
    faces, so a floor lying ON the bottom face weighed zero, and the frame looked
    right because a floor reflecting nothing and a floor reflecting a dim room are
    the same picture. Its own anti-vacuity is what closes that.

    Two absences, both deliberate and both recorded above the constants: no seam
    arm (the two probes agree where they hand over on this fixture, so it could
    not fail), and no SSR arm -- SSR shades only what the shadow catcher marked,
    the catcher is installed only on scenes authoring no lights of their own, and
    this fixture authors two. The multi-probe SSR path is implemented and ungated;
    the suite has no SSR gate of any kind to add it to.
    """
    if not os.path.exists(os.path.join(ROOT, "assets", PROBE_FIXTURE)):
        print(f"  probe-set-single SKIP  {PROBE_FIXTURE} not found")
        return []
    failures = []
    cam = _cscn_camera(PROBE_FIXTURE)

    # -- single: the pre-11.70 path, unmoved -----------------------------------
    def one_probe(d):
        d["probes"] = [d["probes"][0]]

    pix_a, w, h, out_a = _probe_run(workdir, "single_a", one_probe,
                                    ["--probe-set-probe", "10"])
    pix_b, _, _, out_b = _probe_run(workdir, "single_b", one_probe,
                                    ["--probe-set-probe", "10"])
    # The anti-vacuity, and it is not optional: every assertion above is about
    # the diagnostic and about two runs agreeing, and a build that binds no
    # probe at all satisfies all of them perfectly.
    pix_bare, _, _, out_bare = _probe_run(workdir, "single_none",
                                          lambda d: d.pop("probes", None))
    if pix_a is None or pix_b is None or pix_bare is None:
        print(f"  probe-set-single ERROR  {(out_a if pix_a is None else out_b)[-300:]}")
        failures.append("probe-set-single")
    else:
        rows = _probe_set_rows(out_a)
        modes = {r["mode"] for r in rows}
        masks = {r["mask_bits"] for r in rows}
        counts = {r["count"] for r in rows}
        identical = pix_a == pix_b
        lit = sum(1 for i in range(0, len(pix_a), 3)
                  if pix_a[i:i + 3] != pix_bare[i:i + 3])
        ok = (bool(rows) and modes == {"single"} and masks == {0} and counts == {1}
              and identical and lit >= PROBE_SINGLE_EFFECT_MIN)
        print(f"  probe-set-single {'PASS' if ok else 'FAIL'}  mode={sorted(modes)} "
              f"count={sorted(counts)} mask_bits={sorted(masks)} want single/1/0; "
              f"two runs identical={identical}; the one probe moves {lit} px against no "
              f"probe, want >={PROBE_SINGLE_EFFECT_MIN}")
        if not ok:
            failures.append("probe-set-single")

    # -- rooms: which room a probe photographed decides the floor --------------
    pix_ok, w, h, out_ok = _probe_run(workdir, "rooms_ok")
    pix_wr, _, _, out_wr = _probe_run(workdir, "rooms_wrong", _probe_b_into_room_a)
    project = _projector(cam, w, h) if pix_ok is not None else None
    if pix_ok is None or pix_wr is None:
        print(f"  probe-set-rooms ERROR  {(out_ok if pix_ok is None else out_wr)[-300:]}")
        failures.append("probe-set-rooms")
        dark_ok = dark_wr = lit_ok = lit_wr = float("nan")
    else:
        dark_ok = _probe_floor_luma(pix_ok, w, h, project, PROBE_DARK_X)
        dark_wr = _probe_floor_luma(pix_wr, w, h, project, PROBE_DARK_X)
        lit_ok = _probe_floor_luma(pix_ok, w, h, project, PROBE_LIT_X)
        lit_wr = _probe_floor_luma(pix_wr, w, h, project, PROBE_LIT_X)
        ratio = dark_wr / dark_ok if dark_ok > 0 else float("inf")
        drift = abs(lit_wr - lit_ok) / max(lit_ok, 1e-6)
        ok = ratio >= PROBE_ROOMS_RATIO_MIN and drift <= PROBE_ROOMS_CONTROL_MAX
        print(f"  probe-set-rooms {'PASS' if ok else 'FAIL'}  dark floor {dark_ok:.4f} -> "
              f"{dark_wr:.4f} when probe B photographs the lit room, ratio {ratio:.4f} "
              f"want >={PROBE_ROOMS_RATIO_MIN}; lit-room control {lit_ok:.4f} vs "
              f"{lit_wr:.4f} drift {drift:.4f} want <={PROBE_ROOMS_CONTROL_MAX}")
        if not ok:
            failures.append("probe-set-rooms")

    # -- fallback: outside every box is the no-probe frame ---------------------
    pix_off, _, _, out_off = _probe_run(workdir, "boxes_off", _probe_boxes_offstage)
    # The probe-free frame is pix_bare, already rendered for probe-set-single's
    # anti-vacuity: same fixture, same mutation, same flags. Rendering it twice
    # is two processes for one image.
    pix_none, out_none = pix_bare, out_bare
    if pix_off is None or pix_none is None:
        print(f"  probe-set-fallback ERROR  {(out_off if pix_off is None else out_none)[-300:]}")
        failures.append("probe-set-fallback")
    else:
        diff = sum(1 for i in range(0, len(pix_off), 3)
                   if pix_off[i:i + 3] != pix_none[i:i + 3])
        # Anti-vacuity: moving the boxes offstage has to have TAKEN something
        # away, or the identity above is between two frames that never had a
        # probe in them and holds for the wrong reason.
        removed = 0 if pix_ok is None else sum(1 for i in range(0, len(pix_off), 3)
                                               if pix_off[i:i + 3] != pix_ok[i:i + 3])
        ok = diff == 0 and removed >= PROBE_FALLBACK_REMOVED_MIN
        print(f"  probe-set-fallback {'PASS' if ok else 'FAIL'}  boxes moved out of frame "
              f"vs no probes at all: {diff} px differ, want 0 (the W=0 remainder is the "
              f"no-probe expression); moving them took {removed} px away, want "
              f">={PROBE_FALLBACK_REMOVED_MIN}")
        if not ok:
            failures.append("probe-set-fallback")

    # -- converge: the sweep runs once and then costs nothing ------------------
    pix_c, _, _, out_c = _probe_run(workdir, "converge", None,
                                    ["--probe-set-probe", "10"], frames=60)
    _, _, _, out_c2 = _probe_run(workdir, "converge2", None,
                                 ["--probe-set-probe", "10"], frames=60)
    rows_c = _probe_set_rows(out_c)
    rows_c2 = _probe_set_rows(out_c2)
    if pix_c is None or not rows_c:
        print(f"  probe-set-converge ERROR  {out_c[-300:]}")
        failures.append("probe-set-converge")
    else:
        caps = {r["captures"] for r in rows_c}
        # The digest is 0 on the frame before the first grid build; every frame
        # that has one must agree, and must agree across processes.
        digs = {r["digest"] for r in rows_c if r["mask_bits"] > 0}
        digs2 = {r["digest"] for r in rows_c2 if r["mask_bits"] > 0}
        # The probe count the run reported, not a literal: the claim is "captures
        # stop AT the probe count", and a fixture that grows a third probe should
        # keep testing that rather than failing for having grown.
        want = rows_c[0]["count"]
        ok = caps == {want} and len(digs) == 1 and digs == digs2
        print(f"  probe-set-converge {'PASS' if ok else 'FAIL'}  captures {sorted(caps)} "
              f"want exactly [{want}] (the probe count) across {len(rows_c)} frames; mask "
              f"digest {sorted(digs)} stable and equal across two processes ({sorted(digs2)})")
        if not ok:
            failures.append("probe-set-converge")

    # -- tenancy: one texture, two features ------------------------------------
    #
    # The probe atlas and the GI volume are tenants of ONE texture on unit 14,
    # because pbr_frag has no seventeenth sampler to give either of them. What
    # that has to survive is not "both still change the frame" -- a probe set
    # reading GI's irradiance tiles by mistake changes the frame too, and reads
    # as passing. It is that the probes still tell the two rooms APART with GI
    # live, which is the one thing reading the wrong region destroys.
    pix_gi, _, _, out_gi = _probe_run(workdir, "gi_probes", None, ["--gi-volume"])
    pix_gi_wr, _, _, out_gi_wr = _probe_run(workdir, "gi_wrong", _probe_b_into_room_a,
                                            ["--gi-volume"])
    if pix_gi is None or pix_gi_wr is None or pix_ok is None:
        print(f"  probe-set-tenancy ERROR  {(out_gi if pix_gi is None else out_gi_wr)[-300:]}")
        failures.append("probe-set-tenancy")
    else:
        gi_effect = sum(1 for i in range(0, len(pix_gi), 3)
                        if pix_gi[i:i + 3] != pix_ok[i:i + 3])
        gi_dark_ok = _probe_floor_luma(pix_gi, w, h, project, PROBE_DARK_X)
        gi_dark_wr = _probe_floor_luma(pix_gi_wr, w, h, project, PROBE_DARK_X)
        gi_ratio = gi_dark_wr / gi_dark_ok if gi_dark_ok > 0 else float("inf")
        ok = gi_effect > 0 and gi_ratio >= PROBE_ROOMS_RATIO_MIN
        print(f"  probe-set-tenancy {'PASS' if ok else 'FAIL'}  sharing unit 14: GI moves "
              f"{gi_effect} px with probes live (want >0), and the rooms still read apart "
              f"under it -- dark floor {gi_dark_ok:.4f} vs {gi_dark_wr:.4f}, ratio "
              f"{gi_ratio:.4f} want >={PROBE_ROOMS_RATIO_MIN}")
        if not ok:
            failures.append("probe-set-tenancy")

    # -- schema: a probe without a box is refused, not defaulted ---------------
    def drop_box(d):
        d["probes"][1].pop("boxMax", None)

    _, _, _, out_s = _probe_run(workdir, "schema", drop_box, ["--probe-set-probe", "60"])

    def bad_key(d):
        d["probes"][0]["boxmax"] = [1.0, 1.0, 1.0]

    _, _, _, out_k = _probe_run(workdir, "schema_key", bad_key)
    refused = "needs position, boxMin and boxMax" in out_s
    warned = "is not a recognised probe parameter" in out_k
    rows_s = _probe_set_rows(out_s)
    fell_to_one = bool(rows_s) and {r["count"] for r in rows_s} == {1}
    ok = refused and warned and fell_to_one
    print(f"  probe-set-schema {'PASS' if ok else 'FAIL'}  missing boxMax refused by "
          f"name={refused}, set falls to count 1={fell_to_one}, unknown key warned={warned}")
    if not ok:
        failures.append("probe-set-schema")

    return failures


# The scene the config arms drive. Cornell rooms because it is the corpus's densest
# fixture for this: two probes, three lights, seven materials and no water, so one
# snapshot exercises both element arrays, the material pass through MATERIAL_PARAMS,
# and the absent-subsystem path at once.
CONFIG_FIXTURE = "cornell_rooms.cscn"

# A look nothing defaults to, so a restore that silently did nothing cannot pass by
# landing on the same frame. Every one of these is a different mechanism: two effect
# switches, a tonemap enum, and three finishing-stack floats.
CONFIG_LOOK = ["--no-ssr", "--no-ssao", "--tonemap", "agx", "--sharpen", "0.8",
               "--vignette", "--grain", "0.05"]

# GUI controls whose widget writes a LOCAL, not the field. Each entry names the field
# it actually drives, which is the whole value of the list: a control added with a new
# local fails config-coverage until somebody writes down what it sets.
CONFIG_GUI_LOCALS = {
    "fft": "wave_model",
    # The Time of Day slider edits a float mirror because cycle_hour is a
    # double (an accumulator; see config_snapshot.c's CFG_DOUBLE). Named
    # tod_hour rather than hour: this map is keyed on the bare local, so a
    # generic name silently captures the next local that shares it.
    "tod_hour": "cycle_hour",
    "interp": "lut_interp",
    "mode": "meter_mode",
    "msaa": "msaa_samples",
    "msm_size_idx": "msm_size",
    "pending_scale": "render_scale",
    "rm": "current_render_mode",
    "so": "spec_occlusion_mode",
    "taa": "taa_enabled",
    "tm": "tonemap_mode",
    "wireframe": "show_wireframe",
}

# Controls a snapshot deliberately cannot carry, with the reason. Not an escape hatch:
# both reasons are about IDENTITY, which is what a snapshot needs and these lack.
CONFIG_GUI_UNCARRIED = {
    "stiffness": "spring-bone params live per SceneNode; a node has no stable key",
    "damping": "spring-bone params live per SceneNode; a node has no stable key",
    "gravity": "spring-bone params live per SceneNode; a node has no stable key",
    "sss_profiles": "profile SLOTS are assigned in material-block order at load, and "
                    "material->subsurface_profile indexes them",
}


def _config_run(workdir, name, extra, model=CONFIG_FIXTURE, frames=2):
    """One capture plus its snapshot. Returns (ppm, json, stdout+stderr)."""
    ppm = os.path.join(workdir, f"config_{name}.ppm")
    dump = os.path.join(workdir, f"config_{name}.json")
    cmd = [RENDER, "-x", "-f", str(frames), "-S", ppm, "--config-dump", dump]
    if model:
        cmd += ["-m", os.path.join(ROOT, "assets", model), "-W", "400", "-H", "300"]
    r = _run(cmd + extra, capture_output=True, text=True)
    return ppm, dump, r.stdout + r.stderr


# Values a restore is allowed NOT to reproduce, each with the reason. Two are a
# setter doing its job, which is itself worth pinning -- a perturbation that came
# back unchanged would mean the table had gone round the setter. The third is a
# field nothing can carry.
#
# The list is the whole point of config-perturb: it inverts the default from
# "uncovered unless an arm names it" to "covered unless this names it".
CONFIG_PERTURB_EXCEPTIONS = {
    "engine.msaa_samples": "set_engine_msaa_samples validates; 5 is not a sample count",
    "engine.render_scale": "clamped to [0.5, 1], and forced to 1 headless without jitter",
    "camera.near_clip": "render.c recomputes it every frame from the camera-to-target distance",
    # This fixture runs without --clouds, so flipping the switch asks for a layer
    # whose noise bake -- a one-shot at startup -- never ran. Refused by name, and
    # config-clouds is the arm that reads both halves of that.
    "sky.clouds.enabled": "the session baked no cloud noise, so the row refuses rather than "
                          "storing a flag every consumer ignores",
    # Perturb flips sky.cycle ON and gives cycle_day_seconds a non-zero value,
    # so the restored session runs a clock -- and a running clock OWNS these
    # five, deriving them every frame from the hour it is advancing. That is
    # the feature working, the same ownership the GUI states by disabling its
    # sun sliders while the cycle runs; with the cycle off (every other scene
    # and every other arm here) all five round-trip normally. Noted as one
    # group because they have one cause: spec 11.81's tick.
    "sky.cycle_hour": "the clock advances it every frame while the cycle runs",
    "sky.sun_elevation": "derived from cycle_hour by the tick while the cycle runs",
    "sky.sun_azimuth": "derived from cycle_hour by the tick while the cycle runs",
    "sky.stars_hour": "advanced in lock-step with the sun while the cycle runs",
    "lights[sky_sun].intensity": "sky_apply_sun_to_light rewrites the coupled key light every "
                                 "tick while the cycle runs",
    # The moon's three, same cause and one step further: the cycle derives its
    # angles from cycle_hour MINUS the lag, and advances the lag itself at the
    # synodic rate. So all three are the tick's while the clock runs, exactly
    # as the sun's two above are. Note there is no lights[sky_moon] entry --
    # this fixture authors no moon, so no moon light exists to perturb, and
    # gates.py fails on a STALE exception as loudly as on a missing one.
    "sky.moon_elevation": "derived from cycle_hour and the lag by the tick while the cycle runs",
    "sky.moon_azimuth": "derived from cycle_hour and the lag by the tick while the cycle runs",
    "sky.cycle_moon_offset": "the tick advances the lag at the synodic rate while the cycle runs",
}


def _config_perturb(node, top=True):
    """Move every scalar in a snapshot to a different value, in place.

    Type-aware: ints stay ints (a float into an int row truncates and reads as a
    dropped value), and a float already inside [0,1] is moved WITHIN that band so
    a percentage or a blend weight is not pushed out of range and legitimately
    refused. Strings are left alone -- an enum's next label would need the
    vocabulary, and the enums are already covered by config-roundtrip.
    """
    n = 0
    if isinstance(node, dict):
        for k, v in list(node.items()):
            if top and k in ("version", "source"):
                continue
            if k in ("name", "index"):
                continue
            if isinstance(v, bool):
                node[k] = not v
                n += 1
            elif isinstance(v, int):
                node[k] = v + 1
                n += 1
            elif isinstance(v, float):
                node[k] = round(v * 0.5 + 0.25, 6) if 0.0 <= v <= 1.0 else round(v * 1.37 + 0.11, 6)
                n += 1
            elif not isinstance(v, str):
                n += _config_perturb(v, False)
    elif isinstance(node, list):
        for v in node:
            n += _config_perturb(v, False)
    return n


def _config_diff_scalars(asked, got, path="", top=True, out=None):
    """Dotted paths where `got` does not carry the value `asked` holds."""
    if out is None:
        out = []
    if isinstance(asked, dict):
        for k, v in asked.items():
            if top and k in ("version", "source"):
                continue
            if k in ("name", "index") or not isinstance(got, dict) or k not in got:
                continue
            _config_diff_scalars(v, got[k], f"{path}.{k}" if path else k, False, out)
    elif isinstance(asked, list) and isinstance(got, list):
        # Carry the element's identity into the path. Without it every light
        # reports as a bare `lights.intensity`, so one exception excuses the
        # whole array -- a build that stopped restoring intensity entirely
        # would pass. The `name` key is skipped as a VALUE above and used as a
        # LABEL here.
        for i, (a, g) in enumerate(zip(asked, got)):
            label = a.get("name") if isinstance(a, dict) else None
            elem = f"{path}[{label if label else i}]"
            _config_diff_scalars(a, g, elem, False, out)
    elif isinstance(asked, bool):
        if asked != got:
            out.append(path)
    elif isinstance(asked, (int, float)) and isinstance(got, (int, float)):
        if abs(asked - got) > max(1e-4, abs(asked) * 1e-4):
            out.append(path)
    return out


def _config_missing(*paths):
    """True if any capture is absent -- a run that refused rather than rendered.

    Guarded because several arms here deliberately drive the refusal paths, and
    handing a missing file to compare() aborts the whole GROUP on an exception:
    the arm that failed is reported and every arm after it silently never runs.
    """
    return any(not os.path.exists(p) for p in paths)


def _config_px(a, b):
    """compare()'s pixel count, or -1 when either leg refused to render."""
    return -1 if _config_missing(a, b) else compare(a, b)[0]


def _config_variant(src, dst, mutate):
    """A snapshot with one thing changed. The twin cannot differ in anything else."""
    with open(src) as f:
        d = json.load(f)
    mutate(d)
    with open(dst, "w") as f:
        json.dump(d, f, indent=1)
    return dst


# Every ImGui call in gui.c is either a VALUE widget whose target must be a table
# row, or it is not. Both lists are explicit, and an ig* call on neither is a
# failure -- which is the whole point: the value list used to be the only one, so
# a control written with an unlisted widget type was silently not counted, and
# the census still printed a plausible number. Five of eight shapes tested were
# invisible that way, including igDragFloat and igInputFloat.
CONFIG_VALUE_WIDGETS = ("Checkbox", "SliderFloat", "SliderInt", "SliderFloat2", "SliderFloat3",
                        "SliderAngle", "ColorEdit3", "ColorEdit4", "DragFloat", "DragFloat2",
                        "DragFloat3", "InputFloat", "InputInt", "Combo_Str_arr")
# Calls that display, lay out, or act -- they carry no settable target.
CONFIG_NONVALUE_WIDGETS = (
    "Begin", "BeginChild", "BeginDisabled", "BeginTable", "BeginTooltip", "Button",
    "CollapsingHeader_TreeNodeFlags", "CollapsingHeader_BoolPtr", "ColorConvertFloat4ToU32",
    "End", "EndChild", "EndDisabled", "EndTable", "EndTooltip", "GetContentRegionAvail",
    "GetCursorScreenPos", "GetIO", "GetStyle", "GetWindowDrawList", "Image", "Indent",
    "IsItemActive", "IsItemDeactivatedAfterEdit", "IsItemHovered", "PopID", "PopItemWidth",
    "PopStyleColor", "PopStyleVar", "ProgressBar", "PushID_Int", "PushID_Str", "PushItemWidth",
    "PushStyleColor_Vec4", "PushStyleVar_Float", "PushStyleVar_Vec2", "RadioButton_Bool",
    "Selectable_Bool", "Separator", "SeparatorText", "SetNextWindowPos", "SetNextWindowSize",
    "SetTooltip", "SameLine", "Spacing", "TableNextColumn", "TableNextRow", "TableSetupColumn",
    "Text", "TextColored", "TextDisabled", "TextUnformatted", "TextWrapped", "TreeNode_Str",
    "TreePop", "Unindent", "ImDrawList_AddRectFilled", "ImDrawList_AddImage",
    "BeginCombo", "EndCombo", "GetDrawData", "GetWindowPos", "GetWindowSize", "Render",
    "SetItemDefaultFocus", "SetNextWindowBgAlpha", "StyleColorsDark",
)

# Controls whose target is chosen at RUNTIME from a table rather than named in
# the source, so there is no member for the census to match. They are carried --
# by the array the table drives -- and config-perturb is what proves it.
CONFIG_GUI_TABLE_DRIVEN = {
    "v": "the material editor's float/colour buffer: one control per MATERIAL_PARAMS row, "
         "written back through material_param_set, so the materials array carries it",
    "iv": "the same, for that editor's int and enum rows",
}


def _config_gui_widgets_unclassified():
    """ig* calls in gui.c that neither list mentions. Any is a coverage hole."""
    src = open(os.path.join(ROOT, "cetra", "src", "gui.c")).read()
    called = {m.group(1) for m in re.finditer(r'\big([A-Z]\w*)\s*\(', src)}
    return sorted(called - set(CONFIG_VALUE_WIDGETS) - set(CONFIG_NONVALUE_WIDGETS))


def _config_gui_members():
    """Every struct member a gui.c control writes, by its trailing name."""
    src = open(os.path.join(ROOT, "cetra", "src", "gui.c")).read()
    widgets = (r'ig(?:' + "|".join(CONFIG_VALUE_WIDGETS) + r')'
               r'\s*\(\s*(?:"[^"]*"|[\w\->\.]+)\s*,\s*&?([A-Za-z_][A-Za-z0-9_\->\.\[\]]*)')
    found = [m.group(1) for m in re.finditer(widgets, src)]
    # The effect-group helper is a checkbox too, and it owns every master switch.
    found += [m.group(1) for m in
              re.finditer(r'_begin_effect_group\(\s*"[^"]*"\s*,\s*&([A-Za-z_][\w\->\.\[\]]*)', src)]
    return {re.split(r'->|\.', re.sub(r'\[[^\]]*\]', '', e))[-1] for e in found}


def _config_table_members():
    """Every struct member the descriptor table addresses."""
    src = open(os.path.join(ROOT, "cetra", "src", "config_snapshot.c")).read()
    # The member's argument POSITION differs per macro, so the macro name selects
    # it. These must track the macro definitions; when they last drifted, the
    # anti-vacuity floor below caught it rather than letting the census
    # silently shrink to 19 rows and report 150 controls as uncarried.
    index = {"CFG_ROW": 4, "CFG_ROW_ENUM": 3, "CFG_ROW_FN": 4}
    members = set()
    for m in re.finditer(r'\b(CFG_ROW|CFG_ROW_ENUM|CFG_ROW_FN)\(([^()]*)\)', src, re.S):
        args = [a.strip() for a in m.group(2).replace("\n", " ").split(",")]
        i = index[m.group(1)]
        if len(args) > i:
            members.add(args[i].split(".")[-1])
    return members


def run_config_gate(workdir):
    """The live session dumped to JSON and restored from it (spec 11.71).

      config-coverage   every gui.c control is a row in the descriptor table, or is
                        named in one of two lists here with what it drives or why it
                        cannot be carried. Static: reads both sources, renders nothing
      config-roundtrip  restore a snapshot and dump again: byte-identical to the first
      config-pixels     a run under a non-default look, against its own snapshot
                        restored with no other flag. Floor measured first
      config-standalone the same restore with NO -m and no -W/-H: the source block
                        carries the model and the framing
      config-override   the snapshot beats the .cscn AND the command line, which is
                        what makes it a reproduction rather than another opinion
      config-perturb    move EVERY carried value, restore, and check each one came
                        back. The only arm that can see a row whose apply silently
                        does nothing, which config-roundtrip structurally cannot
      config-sun        a moved sun, which is the deferred re-bake and everything
                        downstream of it, read against --sun-elevation
      config-clouds     a cloudy session restores cloudy, and a file whose source
                        cannot arm the noise bake is refused by name rather than
                        storing a flag every consumer ignores
      config-camera     a pose the scene file does not already hold, read against
                        --cam-eye at the same place
      config-order      it lands AFTER the scene-radius derivation, read as values
                        rather than pixels because that block moves none on a small
                        fixture with AO and SSR off
      config-schema     an unknown key, a wrong-shaped value, a light this scene does
                        not have and a whole absent subsystem are each named

    config-pixels is the arm that matters and it is NOT safe alone: "restored frame
    equals original" is satisfied perfectly by a restore that does nothing, since both
    legs would then render the fixture's own defaults. Its control leg -- the same
    fixture with no snapshot -- is what forces the look to have arrived, and it is
    checked against a floor rather than assumed.

    config-coverage is the arm that keeps the feature true over time. Everything else
    here tests the values that ARE carried; only this one can fail because of a value
    that is not. A control added to gui.c with no table row is invisible to every other
    arm in the suite -- the snapshot still round-trips, still reproduces its own frame,
    and silently omits the thing somebody just added.

    config-order exists BECAUSE the falsification round found the hole: moving the
    apply above the scene-radius block destroys a restored fog range and silently
    raises anything under a derived floor, and every other arm here passed on that
    build. Anything read only as pixels is blind to a field the frame does not use.

    THREE KNOWN GAPS, said out loud rather than left to look like coverage.

    Nothing here can see the deferred update_engine_camera_lookat, because the
    render app's own frame loop rebuilds the view matrix through mouse_drag_update
    every frame -- deleting the call is 0 px on every arm. It is kept because the
    apply must not assume its caller has a drag controller.

    Nothing here can see the auto-orbit kill after a restore either, for the same
    shape of reason: render.c already passes `!args.headless` when it arms
    auto-orbit, so it is off in every headless run and those lines are unreachable
    from this group.

    And config-coverage matches a gui.c target by its TRAILING member name, so a
    control writing `sb->enabled` is satisfied by Water.enabled. The springs
    checkbox is the live instance: it is reported carried and is not, while its
    three siblings sit in CONFIG_GUI_UNCARRIED for exactly that reason. Fixing it
    wants (struct, member) keying on both sides.
    """
    failures = []

    # -- coverage: static, and the only arm that can see an OMISSION --------------
    gui = _config_gui_members()
    table = _config_table_members()
    known = set(CONFIG_GUI_LOCALS) | set(CONFIG_GUI_UNCARRIED) | set(CONFIG_GUI_TABLE_DRIVEN)
    missing = sorted(gui - table - known)
    # Anti-vacuity: an empty or tiny extraction would pass by finding nothing.
    parsed_enough = len(gui) >= 150 and len(table) >= 200
    # A local named here must still reach a real row, or the note is fiction.
    locals_land = sorted(f for f in CONFIG_GUI_LOCALS.values() if f not in table)
    # An ig* call on neither widget list means the census silently skipped it.
    unclassified = _config_gui_widgets_unclassified()
    # Both exception lists must stay honest: an entry whose control is gone is
    # stale, and one naming something the table DOES carry is simply false.
    stale = sorted(known - gui)
    redundant = sorted(set(CONFIG_GUI_UNCARRIED) & table)
    ok = (not missing and parsed_enough and not locals_land and not unclassified and not stale
          and not redundant)
    detail = f"{len(gui)} controls, {len(table)} rows"
    if missing:
        detail += f", NOT CARRIED: {', '.join(missing)}"
    if locals_land:
        detail += f", local maps to nothing: {', '.join(locals_land)}"
    if unclassified:
        detail += f", UNCLASSIFIED widget: {', '.join(unclassified)}"
    if stale:
        detail += f", exception for a control that is gone: {', '.join(stale)}"
    if redundant:
        detail += f", excused but carried: {', '.join(redundant)}"
    print(f"  config-coverage {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("config-coverage")

    if not os.path.exists(os.path.join(ROOT, "assets", CONFIG_FIXTURE)):
        print(f"  config-roundtrip SKIP  {CONFIG_FIXTURE} not found")
        return failures

    # -- the floor, before anything is compared to anything ----------------------
    pinned = ["--no-auto-exposure", "-E", "1.0"]
    floor_a, _, _ = _config_run(workdir, "floor_a", pinned)
    floor_b, _, _ = _config_run(workdir, "floor_b", pinned)
    floor_px, _ = compare(floor_a, floor_b)

    # -- the tuned original, and the two restores off it -------------------------
    orig_ppm, orig_json, _ = _config_run(workdir, "orig", pinned + CONFIG_LOOK)
    rest_ppm, rest_json, rest_out = _config_run(workdir, "restore", ["--config", orig_json],
                                                model=None)

    same_json = (os.path.exists(orig_json) and os.path.exists(rest_json)
                 and open(orig_json).read() == open(rest_json).read())
    applied = "config snapshot applied" in rest_out
    ok = same_json and applied
    print(f"  config-roundtrip {'PASS' if ok else 'FAIL'}  dump == redump after restore="
          f"{same_json}, applied={applied}")
    if not ok:
        failures.append("config-roundtrip")

    # config-standalone IS the restore above -- no -m, no -W/-H -- so the pixel arm
    # and this one read the same run and cannot disagree about what it was.
    # The control leg -- the same fixture with no snapshot -- is what floor_a
    # already is, byte for byte: same fixture, same size, same pinned exposure.
    # Reusing it rather than rendering a third identical frame saves a process
    # launch, and on this fixture a launch is ~0.7 s release because it captures
    # two probes at load.
    px = _config_px(orig_ppm, rest_ppm)
    ctrl_px = _config_px(orig_ppm, floor_a)
    ok = px >= 0 and ctrl_px >= 0 and px <= floor_px and ctrl_px > 1000
    print(f"  config-pixels   {'PASS' if ok else 'FAIL'}  "
          + (f"restored {px} px (floor {floor_px}), no-config control {ctrl_px} px" if px >= 0
             else "the restored run produced no frame"))
    if not ok:
        failures.append("config-pixels")

    # The restore above already ran with no -m and no -W/-H, so its frame matching is
    # the positive half. The negative half is the point: blank the model out of the
    # source block and the run must REFUSE, or "it found the model" is unfalsifiable
    # -- a build that ignored the block entirely would look identical here.
    blank_json = _config_variant(orig_json, os.path.join(workdir, "config_nomodel.json"),
                                 lambda d: d["source"].pop("model", None))
    _, _, blank_out = _config_run(workdir, "nomodel", ["--config", blank_json], model=None)
    refused = "names no model and none was given" in blank_out
    ok = px >= 0 and px <= floor_px and refused
    print(f"  config-standalone {'PASS' if ok else 'FAIL'}  rendered from the snapshot alone="
          f"{os.path.exists(rest_ppm)}, refused with its model blanked={refused}")
    if not ok:
        failures.append("config-standalone")

    # -- override: the snapshot lands after the .cscn AND after the flags ---------
    # A snapshot saying the opposite of the command line, applied WITH that command
    # line. If it wins, the frame is the one the snapshot alone produces; if the flags
    # win, it is the flag-only frame. Asserting BOTH is what makes this precise --
    # "something moved" would also pass on a build that restored garbage.
    def _override(d):
        d["postfx"]["ssr"]["enabled"] = True
        d["postfx"]["tonemap"] = "neutral"

    over_json = _config_variant(orig_json, os.path.join(workdir, "config_override.json"),
                                _override)
    with_flags, _, _ = _config_run(workdir, "override_flags", pinned + CONFIG_LOOK +
                                   ["--config", over_json])
    alone, _, _ = _config_run(workdir, "override_alone", ["--config", over_json], model=None)
    same_as_alone = _config_px(with_flags, alone)
    moved_off_flags = _config_px(orig_ppm, with_flags)
    ok = same_as_alone >= 0 and same_as_alone <= floor_px and moved_off_flags > 1000
    print(f"  config-override {'PASS' if ok else 'FAIL'}  "
          + (f"with flags == snapshot alone: {same_as_alone} px, and {moved_off_flags} px off "
             f"the flag-only frame" if same_as_alone >= 0 else "a leg produced no frame"))
    if not ok:
        failures.append("config-override")

    # -- perturb: does every carried value actually SURVIVE a restore? ----------
    # config-roundtrip cannot answer this. Both of its legs render the same
    # fixture from the same defaults, so a value the apply DROPS is identical in
    # both dumps and the byte comparison passes -- only a CORRUPTING apply is
    # caught. That left ~210 rows plus both element arrays uncovered for the
    # commonest defect there is: a row whose apply silently does nothing.
    # Measured when this arm was written: 4 rows were dead, at 0 px, silently.
    perturbed_json = os.path.join(workdir, "config_perturb_in.json")
    with open(orig_json) as f:
        d = json.load(f)
    moved = _config_perturb(d)
    with open(perturbed_json, "w") as f:
        json.dump(d, f, indent=1)
    _, back, _ = _config_run(workdir, "perturb", ["--config", perturbed_json], model=None)
    lost = []
    if os.path.exists(back):
        with open(back) as f:
            lost = _config_diff_scalars(d, json.load(f))
    unexpected = sorted(set(lost) - set(CONFIG_PERTURB_EXCEPTIONS))
    # An exception that starts surviving is also a finding: the note is then a
    # lie, and the list is the only thing standing between this arm and vacuity.
    stale = sorted(set(CONFIG_PERTURB_EXCEPTIONS) - set(lost))
    ok = os.path.exists(back) and moved > 300 and not unexpected and not stale
    detail = f"{moved} values moved, {len(lost)} not reproduced"
    if unexpected:
        detail += f"; DROPPED: {', '.join(unexpected)}"
    if stale:
        detail += f"; exception no longer needed: {', '.join(stale)}"
    print(f"  config-perturb  {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("config-perturb")

    # -- sun: the deferred re-bake, which is a value AND everything downstream ---
    # Storing the two angles is not restoring the sun: the transmittance and
    # sky-view LUTs, the environment cube, the IBL and the coupled key light are
    # all derived from it. Dropping the re-bake leaves a snapshot that reads back
    # correctly and renders the OLD sun -- measured at the whole frame here.
    # Read against --sun-elevation/--sun-azimuth, the answer it has to reproduce.
    #
    # NOT on cornell_rooms, and the reason is a real limit rather than a fixture
    # preference: a probe SET cannot be re-captured (its capture cubes are released
    # into the atlas), so 11.70 defers relight, and a restored sun there leaves two
    # probes lit by the sun that was up when they were photographed. Measured: the
    # snapshot leg lands 0.144 RMSE from the flag leg on that fixture, where the
    # original is 0.159 -- it moves, but not TO the answer. layer_fixture carries no
    # probe, so the claim there is exact and the arm can demand equality.
    sun_scene = "layer_fixture.cscn"
    sun_base = pinned + ["--sky"]
    sun_orig, sun_orig_json, _ = _config_run(workdir, "sun_orig", sun_base, model=sun_scene)
    if _config_missing(sun_orig_json):
        print(f"  config-sun      SKIP  {sun_scene} not found")
    else:
        def _move_sun(d):
            d["sky"]["sun_elevation"] = 55.0
            d["sky"]["sun_azimuth"] = 120.0

        sun_json = _config_variant(sun_orig_json, os.path.join(workdir, "config_sun_in.json"),
                                   _move_sun)
        sun_cfg, _, _ = _config_run(workdir, "sun_cfg", ["--config", sun_json], model=None)
        sun_flag, _, _ = _config_run(workdir, "sun_flag", sun_base +
                                     ["--sun-elevation", "55", "--sun-azimuth", "120"],
                                     model=sun_scene)
        agree = _config_px(sun_cfg, sun_flag)
        moved = _config_px(sun_cfg, sun_orig)
        ok = agree == 0 and moved > 1000
        print(f"  config-sun      {'PASS' if ok else 'FAIL'}  "
              + (f"snapshot sun == --sun-elevation: {agree} px, and {moved} px off the "
                 f"original sun" if agree >= 0 else "a leg produced no frame"))
        if not ok:
            failures.append("config-sun")

    # -- clouds: the deck survives a restore, and says so when it cannot --------
    # Both halves matter and neither is safe alone. The positive half alone
    # passes on a build that always arms clouds; the negative half alone passes
    # on one that never does.
    #
    # Coverage is pinned at 0.10 rather than left at the 0.45 default for the
    # reason the cloud-shadow arms pin it: at the default the deck is a flat
    # dimming with no pattern in it, so a frame comparison reads a scale factor
    # rather than a sky.
    cloud_base = pinned + ["--sky", "--clouds", "--cloud-coverage", "0.10"]
    cloud_orig, cloud_json, _ = _config_run(workdir, "cloud_orig", cloud_base, model=sun_scene)
    if _config_missing(cloud_json):
        print(f"  config-clouds   SKIP  {sun_scene} not found")
    else:
        cloud_cfg, _, _ = _config_run(workdir, "cloud_cfg", ["--config", cloud_json], model=None)
        # No --clouds on this leg: the snapshot's source block must ask for it.
        clear, _, _ = _config_run(workdir, "cloud_clear", pinned + ["--sky"], model=sun_scene)
        agree = _config_px(cloud_cfg, cloud_orig)
        # Anti-vacuity: a cloudy sky must actually differ from a clear one, or
        # "the restore matches" is true of a build with no clouds at all.
        deck_visible = _config_px(cloud_orig, clear)

        # The negative half: the pre-11.72 file shape, whose source block never
        # carried `clouds`. The layer cannot be armed after startup, so the row
        # must refuse BY NAME rather than store a flag every consumer ignores --
        # which is what silently rendered a clear sky while the dump wrote
        # `enabled: true` back out and config-roundtrip passed on it.
        noarm = _config_variant(cloud_json, os.path.join(workdir, "config_cloud_noarm.json"),
                                lambda d: d["source"].pop("clouds", None))
        _, _, noarm_out = _config_run(workdir, "cloud_noarm", ["--config", noarm], model=None)
        refused = "baked no cloud noise; sky.clouds.enabled ignored" in noarm_out

        ok = agree == 0 and deck_visible > 1000 and refused
        print(f"  config-clouds   {'PASS' if ok else 'FAIL'}  "
              + (f"restored == cloudy original: {agree} px, deck worth {deck_visible} px, "
                 f"unarmed restore refused by name={refused}"
                 if agree >= 0 else "a leg produced no frame"))
        if not ok:
            failures.append("config-clouds")

    # -- camera: a pose the scene file does NOT already hold ---------------------
    # Every other arm restores the fixture's own camera, so the pose path is inert
    # in all of them -- deleting the spherical re-seed and the lookat rebuild moved
    # nothing. The realistic case is the only one that tests it: you moved the
    # camera, then dumped. Read against --cam-eye/--cam-target at the same place,
    # which is the answer the snapshot has to reproduce.
    with open(orig_json) as f:
        base_cam = json.load(f)["camera"]
    eye = [base_cam["eye"][0] + 1.3, base_cam["eye"][1] + 0.6, base_cam["eye"][2] - 1.1]
    target = base_cam["target"]
    cam_json = _config_variant(orig_json, os.path.join(workdir, "config_camera_in.json"),
                               lambda d: d["camera"].update(eye=eye))
    cam_cfg, _, _ = _config_run(workdir, "camera_cfg", ["--config", cam_json], model=None)
    cam_flag, _, _ = _config_run(workdir, "camera_flag", pinned + CONFIG_LOOK + [
        "--cam-eye", ",".join(f"{v:.6f}" for v in eye),
        "--cam-target", ",".join(f"{v:.6f}" for v in target)])
    agree = _config_px(cam_cfg, cam_flag)
    moved = _config_px(cam_cfg, orig_ppm)
    ok = agree >= 0 and agree <= floor_px and moved > 1000
    print(f"  config-camera   {'PASS' if ok else 'FAIL'}  "
          + (f"snapshot pose == --cam-eye: {agree} px, and {moved} px off the original pose"
             if agree >= 0 else "a leg produced no frame"))
    if not ok:
        failures.append("config-camera")

    # -- order: the apply must land AFTER the scene-radius derivation ------------
    # Read as VALUES, not pixels, and that is the point: the five fields this block
    # touches are a fog range and three screen-space reaches, none of which moves a
    # pixel on a small fixture with AO and SSR switched off. The falsification round
    # moved the apply above this block and all six other arms passed.
    #
    # Both mechanisms, because they fail differently: fog.far is an unconditional
    # ASSIGNMENT, so an early apply loses it outright; ao.radius is an fmaxf FLOOR,
    # so an early apply only loses values below the floor.
    # The INPUT is named apart from the dump on purpose: _config_run derives its
    # dump path from the arm name, so an input called config_order.json would be
    # the file the restore writes back over -- and the arm would be reading its own
    # input and could never fail. It shipped that way for one falsification round.
    with open(orig_json) as f:
        derived_far = json.load(f)["postfx"]["fog"]["far"]

    def _out_of_band(d):
        d["postfx"]["fog"]["far"] = 1234.5
        d["postfx"]["ao"]["radius"] = 0.001  # under scene_radius * 0.01 on this fixture

    order_json = _config_variant(orig_json, os.path.join(workdir, "config_order_in.json"),
                                 _out_of_band)
    _, back_json, _ = _config_run(workdir, "order", ["--config", order_json], model=None)
    back = {}
    if os.path.exists(back_json):
        with open(back_json) as f:
            back = json.load(f)
    kept_far = abs(back.get("postfx", {}).get("fog", {}).get("far", 0.0) - 1234.5) < 0.01
    kept_radius = abs(back.get("postfx", {}).get("ao", {}).get("radius", 0.0) - 0.001) < 1e-6
    # Anti-vacuity: the authored values must actually differ from what the block
    # derives, or "they survived" is true of a build that never applied them.
    distinct = abs(derived_far - 1234.5) > 1.0
    ok = kept_far and kept_radius and distinct
    print(f"  config-order    {'PASS' if ok else 'FAIL'}  authored fog.far survived={kept_far}, "
          f"ao.radius under the floor survived={kept_radius} (derived far {derived_far:.2f})")
    if not ok:
        failures.append("config-order")

    # -- schema: every way a file can be wrong, each named ------------------------
    # One run covers all of them, so the only reason to carry fewer would be that
    # the rest were untested -- which is what they were: the whole per-array key
    # sweep, its MATERIAL_PARAMS branch, the vector-length check and the version
    # warning had no coverage at all.
    def _wrongnesses(d):
        d["postfx"]["nonsense_key"] = 3                     # unknown key in a real section
        d["postfx"]["ssr"]["strength"] = "not a number"     # scalar of the wrong shape
        d["postfx"]["tonemap"] = "chartreuse"               # unknown enum label
        d["postfx"]["grade"]["lift"] = [0.1, 0.2]           # vector of the wrong LENGTH
        d["water"] = {"level": 2.0}                         # a subsystem this scene lacks
        d["nonsense_section"] = {"x": 1}                    # unknown top-level section
        d["lights"].append({"name": "ghost_light", "intensity": 1.0})
        d["probes"].append({"index": 99, "intensity": 1.0})
        d["materials"].append({"name": "ghost_material", "roughness": 0.5})
        d["probes"][0]["bogus_probe_key"] = 1
        d["lights"][0]["bogus_light_key"] = 1
        d["materials"][0]["bogus_mat_key"] = 1
        d["version"] = 7

    bad_json = _config_variant(orig_json, os.path.join(workdir, "config_bad.json"), _wrongnesses)
    _, _, bad_out = _config_run(workdir, "bad", ["--config", bad_json], model=None)
    named = {
        "unknown key": "'postfx.nonsense_key' is not a known setting" in bad_out,
        "wrong shape": "postfx.ssr.strength is not a value of the right shape" in bad_out,
        "unknown enum": "postfx.tonemap is not a known value" in bad_out,
        "wrong vector length": "postfx.grade.lift is not a value of the right shape" in bad_out,
        "absent subsystem": "this scene has no 'water'" in bad_out,
        "unknown section": "'nonsense_section' is not a known section" in bad_out,
        "absent light": "no light 'ghost_light'" in bad_out,
        "absent probe": "no probe 99" in bad_out,
        "absent material": "no material 'ghost_material'" in bad_out,
        "unknown probe key": "'probes.bogus_probe_key' is not a known setting" in bad_out,
        "unknown light key": "'lights.bogus_light_key' is not a known setting" in bad_out,
        "unknown material key": "'materials.bogus_mat_key' is not a known setting" in bad_out,
        "version": "version 7; expected 1" in bad_out,
    }
    # And it must still have applied the rest rather than refusing the file --
    # asserted as a COUNT, so "applied" cannot pass while a refusal silently took
    # other values with it. THREE values are refused here: the wrong-shaped
    # scalar, the wrong-length vector, and the unknown enum label, which is a
    # refused value as much as the other two. Everything else must still land.
    REFUSED = 3
    applied = re.search(r"config snapshot applied: \S+ \((\d+) fields\)", bad_out)
    clean = re.search(r"config snapshot applied: \S+ \((\d+) fields\)", rest_out)
    count_ok = bool(applied and clean) and int(applied.group(1)) == int(clean.group(1)) - REFUSED
    ok = all(named.values()) and count_ok
    print(f"  config-schema   {'PASS' if ok else 'FAIL'}  "
          f"{sum(named.values())}/{len(named)} named, applied "
          f"{applied.group(1) if applied else '?'} of {clean.group(1) if clean else '?'} "
          f"(want {REFUSED} refused)"
          + ("" if all(named.values()) else
             f", silent: {', '.join(k for k, v in named.items() if not v)}"))
    if not ok:
        failures.append("config-schema")

    return failures


def run_gate_docs_gate(workdir):
    """A group's documented arm list is the list it actually runs.

      gate-arm-docs   every group whose docstring lists its arms lists all of them, in
                      run order. Static -- it reads the source rather than running
                      anything, so it covers all groups whatever --only selected, and
                      costs no render.

    A docstring naming a different set than the function runs is what a reviewer reads to
    decide what is covered, which makes a stale one worse than none. This is not a
    hypothetical: run_water_gate documented 25 arms while running 29, and the four it
    omitted were shoaling, the shore foam band, the crack test and the draw count. Eleven
    more were listed in an order the function had stopped following.

    A group that lists no arms is not required to start -- only a list that EXISTS is held
    to being true, so this adds no documentation debt to the 30 groups without one. A
    group that lists arms but builds their names at runtime cannot be read this way, and
    is reported as unverifiable rather than quietly passed or wrongly failed.
    """
    del workdir # static: nothing is rendered and nothing is written
    problems, checked, listless = [], 0, 0
    for selector, _banner, fn in GATE_GROUPS:
        documented = _ARM_DOCUMENTED.findall(inspect.getdoc(fn) or "")
        if not documented:
            listless += 1
            continue
        printed, seen = [], set()
        for name in _ARM_PRINTED.findall(inspect.getsource(fn)):
            if name not in seen:
                seen.add(name)
                printed.append(name)
        if not printed:
            problems.append(f"{selector}: lists arms but names none in a literal")
            continue
        checked += 1
        undocumented = [n for n in printed if n not in set(documented)]
        stale = [n for n in documented if n not in set(printed)]
        if undocumented or stale:
            problems.append(f"{selector}: undocumented {undocumented}, documented but "
                            f"not run {stale}")
        elif documented != printed:
            at = next(i for i in range(len(documented)) if documented[i] != printed[i])
            problems.append(f"{selector}: same set, but the list leaves run order at #{at} "
                            f"({documented[at]} listed where {printed[at]} runs)")
    ok = not problems
    detail = (f"{checked} groups list their arms and match; {listless} list none"
              if ok else "; ".join(problems))
    print(f"  gate-arm-docs {'PASS' if ok else 'FAIL'}  {detail}")
    return [] if ok else ["gate-arm-docs"]


def run_fixture_gen_gate(workdir):
    """Every gen_*.py still emits the asset committed beside it.

      fixture-gen  each generator runs and reproduces its committed outputs. No GPU and
                   no render, which is why it can afford to cover the whole corpus.

    It exists because gen_water_fixture.py silently stopped emitting its water block: a
    regeneration would have stripped 21 authored keys and taken twenty-odd water arms with
    it, and nothing in the suite could have said so. The same is true of every other
    fixture whose generator nobody has run since committing it.

    Run as a COPY in a scratch directory rather than through an output-directory argument.
    A generator resolves both its inputs and its outputs against __file__, so moving the
    script moves the writes -- which buys this coverage without 34 scripts having to learn
    a convention they have no other use for. The scratch directory has to MIRROR the tree,
    not just hold the script: the non-golden PNGs are what the textured fixtures read, and
    one generator loads a module from ../tools, which is symlinked rather than copied
    because nothing writes outside its own directory.
    """
    src_dir = os.path.join(ROOT, "assets")
    gens = sorted(glob.glob(os.path.join(src_dir, "gen_*.py")))
    inputs = [p for p in sorted(glob.glob(os.path.join(src_dir, "*.png")))
              if not p.endswith("_golden.png")]
    # Byte equality is the contract a .gltf, .cscn, .ies or .cube has -- all four are text
    # a generator writes deterministically. It is NOT the contract a .png has, whose bytes
    # come out of PIL and zlib and move with those libraries rather than with the fixture.
    # Binary outputs are held to being emitted and non-empty, and the count is printed so
    # the weaker check does not read as coverage it is not.
    #
    # .cube joined late and that is the point of listing them here rather than defaulting
    # to text: 11.58 committed 1.2 MB of tables, verified their regeneration by hand, and
    # left them in the BINARY bucket -- so the printed "N binary emitted non-empty" read
    # as PNG coverage while four tables had none at all.
    text_ext = (".gltf", ".cscn", ".ies", ".cube")

    drifted, missing_dep, compared, binary = [], [], 0, 0
    for gen in gens:
        name = os.path.basename(gen)
        run_root = os.path.join(workdir, "gen", name[:-3])
        run_dir = os.path.join(run_root, "assets")
        os.makedirs(run_dir, exist_ok=True)
        tools_link = os.path.join(run_root, "tools")
        if not os.path.exists(tools_link):
            os.symlink(os.path.join(ROOT, "tools"), tools_link)
        # ALL the generators, not just the one under test: gen_layer_vt_fixture
        # imports gen_layer_fixture for its shared constants, and a mirror
        # without the sibling raised ModuleNotFoundError -- which the classifier
        # below filed as a machine dependency, so the vt fixture had ZERO drift
        # coverage while the pass line read one generator short. Unrun siblings
        # cannot read as outputs: the mtime stamps only count writes, and -B
        # keeps their import from minting a __pycache__ the scan would count.
        for path in gens + inputs:
            shutil.copy2(path, run_dir)
        stamps = {f: os.stat(os.path.join(run_dir, f)).st_mtime_ns
                  for f in os.listdir(run_dir)}

        proc = subprocess.run([sys.executable, "-B", os.path.join(run_dir, name)],
                              capture_output=True, text=True, cwd=run_dir)
        if proc.returncode != 0:
            tail = (proc.stderr or proc.stdout).strip().splitlines()
            last = tail[-1][:90] if tail else "no output"
            # A generator needing numpy or PIL is a property of this machine, not of the
            # fixture, so it is reported apart from a real drift rather than as one. A
            # missing SIBLING is a property of the fixture and must fail loudly.
            is_dep = ("ModuleNotFoundError" in (proc.stderr or "")
                      and "No module named 'gen_" not in (proc.stderr or ""))
            (missing_dep if is_dep
             else drifted).append(f"{name}: exited {proc.returncode} ({last})")
            continue

        wrote = [f for f in sorted(os.listdir(run_dir))
                 if f != name and stamps.get(f) != os.stat(os.path.join(run_dir, f)).st_mtime_ns]
        if not wrote:
            drifted.append(f"{name}: wrote nothing")
            continue
        for f in wrote:
            committed, regenerated = os.path.join(src_dir, f), os.path.join(run_dir, f)
            if not os.path.exists(committed):
                drifted.append(f"{f}: emitted but not committed")
            elif f.endswith(text_ext):
                compared += 1
                with open(committed) as f_committed, open(regenerated) as f_regenerated:
                    if f_committed.read() != f_regenerated.read():
                        drifted.append(f"{f}: DRIFTED from the committed asset")
            else:
                binary += 1
                if os.path.getsize(regenerated) == 0:
                    drifted.append(f"{f}: emitted empty")

    ok = not drifted
    detail = (f"{len(gens)} generators, {compared} text assets byte-identical, "
              f"{binary} binary emitted non-empty" if ok else "; ".join(drifted[:6]))
    if missing_dep:
        detail += f"; {len(missing_dep)} skipped for a missing module"
    print(f"  fixture-gen  {'PASS' if ok else 'FAIL'}  {detail}")
    return [] if ok else ["fixture-gen"]


SSS_TAG_FIXTURE = "sss_msaa_fixture.cscn"
SSS_TAG_SIZE = ("400", "300")
# The tag-2 sphere in WORLD space: sss_fixture.gltf puts unit spheres at x = +/-1.3, y = 1.0.
# Projected rather than found in the image, because the obvious way to locate a sphere -- the
# extent of the pixels its own hue dominates -- includes its SCATTER HALO, so the radius moves
# with the thing being measured and two frames end up reading different rings.
SSS_TAG_CENTRE = (1.3, 1.0, 0.0)
SSS_TAG_TOP = (1.3, 2.0, 0.0)
# The band straddling the silhouette, where partial coverage lives. Multiples of the projected
# radius, on the OUTER half only -- the other sphere is to the left.
SSS_TAG_RIM = (0.92, 1.02)
# The defect's measured size. An OPEN defect: 11.37 phase 2 moved the tag into stencil, which
# took this to 0.000%, and was reverted -- a per-pixel pass can only read one stencil value per
# pixel, so resolving a per-sample tag discards coverage and trades a categorical error for an
# arbitrary one. See the spec. Bar an order below the effect: presence, not amount.
SSS_TAG_MIN_SHIFT = 0.010


def _sss_arc_rgb(pix, w, h, cx, cy, r0, r1):
    """Mean linear RGB over the OUTER half (px >= cx) of an annulus."""
    s = [0.0, 0.0, 0.0]
    n = 0
    for py in range(max(0, int(cy - r1)), min(h, int(cy + r1) + 1)):
        for px in range(max(0, int(cx)), min(w, int(cx + r1) + 1)):
            if not (r0 <= ((px - cx) ** 2 + (py - cy) ** 2) ** 0.5 <= r1):
                continue
            o = (py * w + px) * 3
            s[0] += _SRGB_TO_LINEAR[pix[o]]
            s[1] += _SRGB_TO_LINEAR[pix[o + 1]]
            s[2] += _SRGB_TO_LINEAR[pix[o + 2]]
            n += 1
    return [v / max(n, 1) for v in s], n


def _sss_tag_render(workdir, tag, scene, samples):
    """One frame. --no-bloom is REQUIRED, not tidiness -- see the gate docstring."""
    out = os.path.join(workdir, f"ssstag_{tag}.ppm")
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "-W", SSS_TAG_SIZE[0], "-H", SSS_TAG_SIZE[1],
           "--msaa", str(samples), "--no-bloom", "-S", out]
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    return out


def _sss_tag_rim(path, cam):
    """Red/green of the tag-2 sphere's rim: which profile blurred those pixels."""
    w, h, pix = _read_ppm(path)
    project = _projector(cam, w, h)
    cx, cy = project(SSS_TAG_CENTRE)
    tx, ty = project(SSS_TAG_TOP)
    rad = ((tx - cx) ** 2 + (ty - cy) ** 2) ** 0.5
    rgb, _ = _sss_arc_rgb(pix, w, h, cx, cy, rad * SSS_TAG_RIM[0], rad * SSS_TAG_RIM[1])
    return rgb[0] / max(rgb[1], 1e-9)


def _sss_tag_shift(workdir, samples):
    """How much a SECOND profile in the frame moves the tag-2 sphere's rim. Should be nothing."""
    scene = os.path.join(ROOT, "assets", SSS_TAG_FIXTURE)
    twin = os.path.join(workdir, "sss_msaa_one.cscn")
    cscn_copy(scene, twin, lambda d: d["materials"].pop("sss_skin_a", None))
    two = _sss_tag_render(workdir, f"two_{samples}", scene, samples)
    one = _sss_tag_render(workdir, f"one_{samples}", twin, samples)
    if not two or not one:
        return None
    cam = _cscn_camera(SSS_TAG_FIXTURE)
    a, b = _sss_tag_rim(two, cam), _sss_tag_rim(one, cam)
    return (a - b) / max(b, 1e-9), a, b


def run_sss_tag_gate(workdir):
    """Which SSS profile blurred a pixel must not depend on what else is in the frame (11.37).

    The profile a pixel belongs to used to be a LABEL in attachment 4's alpha, and that
    attachment is MSAA-resolved by a blit -- a box filter. Averaging a label destroys it: the
    mean of tag 2 and the uncovered 0 is 1, which does not mean "half of profile 2", it means
    PROFILE 1. So the partly covered pixels of the tag-2 sphere were blurred with the other
    profile's channel weights. Phase 2 moved the tag into stencil, which is integer and per
    sample and has no mean to take.

    The isolation holds the sample count FIXED and varies whether a second profile exists at
    all -- with one profile the same averaging is harmless, since a pixel either rounds back to
    that tag or to 0 and is dropped. Comparing 4x against 1 sample instead makes ANTIALIASING
    the dominant term and moves the rim the opposite way, which would read as the defect being
    absent.

    Two things this gate learned the hard way, both of which produced a confident wrong number:

    --no-bloom is LOAD-BEARING. The one-profile twin removes SSS from the OTHER sphere, which
    is a large shading change, and bloom spreads it across the whole frame -- including any
    band near the sphere being read. The first version of this arm reported 3.58% on a band
    OUTSIDE the silhouette, all of it bloom: the gather early-outs off-surface, so SSS provably
    never paints there and the band contained nothing else.

    And the fixture must be SIDE-LIT. Scatter is blur minus centre, so a uniformly lit sphere
    contributes nothing however wrong its profile is -- under a back key the whole gather moved
    129 px of the frame and the misfile moved 23. The terminator is what makes the defect
    exist to be measured.
    """
    scene = os.path.join(ROOT, "assets", SSS_TAG_FIXTURE)
    if not os.path.exists(scene):
        print(f"  sss-tag      SKIP  ({SSS_TAG_FIXTURE} not present)")
        return []

    failures = []
    got = _sss_tag_shift(workdir, 4)
    if got is None:
        print("  sss-tag      ERROR while rendering the 4x profile-count pair")
        return ["sss-tag"]
    shift, two, one = got
    ok = shift >= SSS_TAG_MIN_SHIFT
    print(f"  sss-tag      {'PASS' if ok else 'FAIL'}  4x MSAA, tag-2 rim R/G {two:.5f} with two "
          f"profiles vs {one:.5f} with one, {shift * 100.0:+.3f}% "
          f"(want >= {SSS_TAG_MIN_SHIFT * 100.0:.1f}%: this records an OPEN defect at its size, "
          f"so it goes to zero only by a deliberate change here)")
    if not ok:
        failures.append("sss-tag")

    got = _sss_tag_shift(workdir, 1)
    if got is None:
        print("  sss-tag-1x   ERROR while rendering the 1-sample profile-count pair")
        return failures + ["sss-tag-1x"]
    shift1, two1, one1 = got
    ok = abs(shift1) <= SSS_TAG_MIN_SHIFT
    print(f"  sss-tag-1x   {'PASS' if ok else 'FAIL'}  1 sample, same pair {two1:.5f} vs "
          f"{one1:.5f}, {shift1 * 100.0:+.3f}% (want <= {SSS_TAG_MIN_SHIFT * 100.0:.1f}%: with "
          f"no partial coverage there is nothing to average, so the arm above measures partial "
          f"coverage rather than the mere presence of a second profile)")
    if not ok:
        failures.append("sss-tag-1x")

    return failures


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
    cmd = [RENDER, "-m", os.path.join(ROOT, "assets", VARY_FIXTURE), "-x", "-f", "30",
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


WIND_UV_FIXTURE = "wind_uv_fixture.cscn"
WIND_UV_STILL = "wind_uv_fixture_still.cscn"
# A quad owns a pixel when its channel beats both others by this factor. The
# backdrop is authored neutral, so nothing there can clear it; the margin only
# has to survive the vignette and the tonemap toe.
WIND_UV_DOMINANCE = 1.6
WIND_UV_FLOOR = 40  # 8-bit; a lit quad reads far above this, the backdrop far below
# Travel separation below this means the flex channel is not reaching the shader
# at all -- which is the silent failure, since the body-lean term still leans.
WIND_UV_MIN_SPREAD = 2.0
# The two gaps between three evenly-spaced flex values must match. Generous
# because the measurement is a centroid of an antialiased silhouette at the
# gate's small frame, not because the underlying relation is loose.
WIND_UV_LINEARITY = 0.30


def _wind_uv_flex():
    """The quads' authored flex weights, read out of the fixture's own glTF.

    Read rather than transcribed, for the reason _water_ramp_edges is: a copy
    here would be a second statement of what the fixture says, and the two are
    only equal until someone edits one. Worse than a stale box, it would be a
    stale LABEL -- the arm's own report would name flex values the fixture does
    not carry, which is how a red gate ends up describing the wrong defect.

    Returned in node order, left to right, which is the order the centroids come
    back in.
    """
    with open(os.path.join(ROOT, "assets", "wind_uv_fixture.gltf")) as f:
        doc = json.load(f)
    raw = base64.b64decode(doc["buffers"][0]["uri"].split(",", 1)[1])
    out = []
    # Node order, not mesh order: the arm reads the frame left to right, and it
    # is the node's translation that puts a quad there.
    quads = sorted((n for n in doc["nodes"] if n["name"].startswith("flex_")),
                   key=lambda n: n["translation"][0])
    for node in quads:
        prim = doc["meshes"][node["mesh"]]["primitives"][0]
        acc = doc["accessors"][prim["attributes"]["TEXCOORD_1"]]
        view = doc["bufferViews"][acc["bufferView"]]
        base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        out.append(struct.unpack_from("<2f", raw, base)[1])
    return out


def _wind_uv_centroids(path):
    """Mean x of each quad, in pixels, keyed by channel dominance."""
    w, h, pix = _read_ppm(path)
    tot = [0.0, 0.0, 0.0]
    n = [0, 0, 0]
    for py in range(h):
        row = (py * w) * 3
        for px in range(w):
            o = row + px * 3
            r, g, b = pix[o], pix[o + 1], pix[o + 2]
            for c, (hi, a, bb) in enumerate(((r, g, b), (g, r, b), (b, r, g))):
                if hi >= WIND_UV_FLOOR and hi >= WIND_UV_DOMINANCE * max(a, bb, 1):
                    tot[c] += px
                    n[c] += 1
    return [(tot[c] / n[c] if n[c] else None) for c in range(3)], n


def run_wind_uv_gate(workdir):
    """UV1.y arrives at the shader as the flex weight it was authored as (spec 11.51).

    Three identical quads differing only in TEXCOORD_1.y (1.0, 0.5, 0.0), rendered
    still and then under a wind blowing along +x. Travel must fall in that order
    and by equal steps, because flex is a raw linear multiplier on the sway term.

    The failure this guards is SILENT, which is the whole reason it is worth a
    fixture. Geometry that reaches the engine without UV1 does not error --
    tex_coords2 is NULL, attribute 8 reads (0,0), and every surface gets phase 0
    and flex 0. It still LEANS, because the height-mask body term carries no
    flex, so the frame looks like a calm day rather than like a bug. Nothing in
    the corpus could see that before 11.51: every vegetation surface here is
    generated in C, where the wind channels are written by the same code that
    builds the mesh and cannot disagree with it.

    What can regress is narrower than the whole path and worth naming. Three
    V-flips sit between an authoring tool and this shader -- the exporter's, then
    assimp's glTF2 importer, then cetra's aiProcess_FlipUVs -- and the last two
    CANCEL. That cancellation is what makes the accessor value the shader value,
    and it is the half cetra owns: an assimp bump or a change to uv_flip_flag()
    inverts the convention silently, and every imported plant animates wrong with
    the stiffest leaves at the tips. The exporter's own flip lives on the far side
    of the file boundary and is recorded in the spec instead.

    Two renders rather than one: wind has no CLI flag, so the still reference has
    to come from a second scene file. A single frame cannot substitute, because a
    linear ramp of displacement preserves the quads' even spacing -- three quads
    that all moved by the same wrong amount look exactly like three quads that
    moved correctly.
    """
    scene = os.path.join(ROOT, "assets", WIND_UV_FIXTURE)
    still = os.path.join(ROOT, "assets", WIND_UV_STILL)
    if not (os.path.exists(scene) and os.path.exists(still)):
        print(f"  wind-uv-flex SKIP  ({WIND_UV_FIXTURE} not present)")
        return []

    a = os.path.join(workdir, "wind_uv_still.ppm")
    b = os.path.join(workdir, "wind_uv_wind.ppm")
    for path, out in ((still, a), (scene, b)):
        err = render(path, out, [])
        if err:
            print(f"  wind-uv-flex ERROR render failed: {err.strip()[-200:]}")
            return ["wind-uv-flex"]

    rest, n_rest = _wind_uv_centroids(a)
    blown, n_blown = _wind_uv_centroids(b)
    if any(v is None for v in rest + blown):
        print(f"  wind-uv-flex FAIL  a quad was not found: still pixels {n_rest}, "
              f"wind pixels {n_blown} (red, green, blue)")
        return ["wind-uv-flex"]

    travel = [blown[c] - rest[c] for c in range(3)]
    # MAGNITUDE, in the order the quads were authored. Normalising by the sign of
    # (first - last) instead would be the obvious way to survive the fixture's
    # wind being re-aimed, and it silently defeats the arm: an inverted
    # convention hands red 0.0 and blue 1.0, the travels come back ascending, and
    # flipping the sign turns that into a descending sequence that passes every
    # check. The regression this exists to catch would have been the one thing it
    # could not see. Magnitude survives a re-aim and still fails an inversion.
    t = [abs(v) for v in travel]

    flex = _wind_uv_flex()
    ordered = t[0] > t[1] > t[2]
    spread = t[0] - t[2]
    step_hi, step_lo = t[0] - t[1], t[1] - t[2]
    linear = abs(step_hi - step_lo) <= WIND_UV_LINEARITY * max(spread, 1e-6)
    # Descending travel is only the right expectation because the fixture authors
    # descending flex. Asserted rather than assumed, so re-ordering the fixture
    # turns this arm red instead of quietly inverting what it tests.
    ok = (ordered and spread >= WIND_UV_MIN_SPREAD and linear
          and flex[0] > flex[1] > flex[2])

    print(f"  wind-uv-flex {'PASS' if ok else 'FAIL'}  travel {t[0]:.2f} / {t[1]:.2f} / "
          f"{t[2]:.2f} px for TEXCOORD_1.y {flex[0]:g} / {flex[1]:g} / "
          f"{flex[2]:g} (want descending: {ordered}); spread {spread:.2f} px "
          f"(want >= {WIND_UV_MIN_SPREAD}, below it UV1 is not reaching the shader and the "
          f"body lean is all that moves); steps {step_hi:.2f} vs {step_lo:.2f}, "
          f"{abs(step_hi - step_lo) / max(spread, 1e-6) * 100:.1f}% apart "
          f"(want <= {WIND_UV_LINEARITY * 100:.0f}%: flex is a linear multiplier)")
    return [] if ok else ["wind-uv-flex"]


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
    scene = os.path.join(ROOT, "assets", VARY_FIXTURE)
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


# Every gate group in run order, as (selector, banner, fn).
#
# A table rather than a sequence of calls so --only can filter it. The suite is
# minutes long, and a change that touches one subsystem should not have to pay for
# all of them -- a check nobody waits for is a check nobody runs. The full run stays
# the default, so CI and the specs' verification blocks are unaffected.
# ---------------------------------------------------------------------------
# Emissive geometry as LTC area lights (spec 11.49 / roadmap C2)
# ---------------------------------------------------------------------------

EMISSIVE_FIXTURE = "emissive_fixture.gltf"
CORNELL_FIXTURE = "cornell_box.cscn"
EMISSIVE_WATER_FIXTURE = "water_fixture.gltf"

# The cornell quad, read off gen_cornell_rooms.py rather than off a render: the
# panel sits at TOP - 0.02 with TOP = 2.0 and spans +/-0.35 in x and z. Stated
# here so the arm compares the fit against the GEOMETRY, not against itself.
#
# Note 1.98 and not the 1.97 the fixture's own area light is authored at. That
# 1 cm is a real discrepancy in the fixture -- its comment says the light mirrors
# the quad -- and the fit is a fit of the MESH, so this is the number it owes.
EMISSIVE_CORNELL_CENTER = (0.0, 1.98, 0.0)
EMISSIVE_CORNELL_NORMAL = (0.0, -1.0, 0.0)
EMISSIVE_CORNELL_SIZE = (0.7, 0.7)
EMISSIVE_GEOM_EPS = 1e-3

# Half black, half white, so the linear mean is 0.5 by construction. The
# tolerance is the 8-bit sRGB round trip through the top mip, not slack: 0.5
# encodes to byte 188, which decodes to 0.5029.
EMISSIVE_TEX_MEAN = 0.5
EMISSIVE_TEX_EPS = 0.005

# The strip is 2.0 x 0.25 rotated 30 degrees in its own plane. An axis-aligned
# bound on the reference frame reads 1.8571 x 1.2165, so this separates the
# minimum-area search from the obvious thing to have written.
EMISSIVE_STRIP_SIZE = (0.25, 2.0)

# The two fill rejects, read off gen_emissive_fixture.py's own constants rather
# than off a probe run, so the arm compares the fit against the GEOMETRY.
#
# split  two 1.0 x 0.25 strips whose inner edges are 5.0 apart: 0.5 of area
#        against a 7.0 x 0.25 bound = 0.285714
# ell    two 1.0 x 0.3 arms sharing a corner: 0.3 + 0.21 against a 1.0 x 1.0
#        bound = 0.51
#
# Both sit far under EMISSIVE_FIT_MIN_FILL 0.65, which is where a disc (0.785)
# stays accepted -- so the epsilon is against the minimum-area search's angular
# quantisation, not against the distance to the threshold.
EMISSIVE_SPLIT_FILL = 0.285714
EMISSIVE_ELL_FILL = 0.51
EMISSIVE_FILL_EPS = 2e-3

# Derived against hand-authored at equal radiance. Not tighter, because the
# fixture's own 1 cm offset puts the derived panel very slightly further from
# everything -- which is the residual this bar is sized to admit and name.
EMISSIVE_MATCH_EPS = 0.03

# The GI arm compares two LIFTS, so it is looser than the direct match above: it
# divides two ratios, each carrying the probe volume's own quantisation, and the
# derived side is very slightly darker because the authored fixture's quad still
# emits its token 1 nit into the capture where the derived one is silenced whole.
# It failed at 1.3134 before the capture learned to silence a derived emitter, so
# 5% discriminates by a wide margin.
EMISSIVE_GI_EPS = 0.05
EMISSIVE_GI_FRAMES = 120

# Bare literals in the arms below became these, because five thresholds in this
# group already carried a stated reason and three did not.
#
# PLANARITY_MAX  a closed volume reads 0 exactly (its faces cancel); the loosest
#                shape that should still reject is far under this.
# SHADOW_LIFT_MIN  the unshadowed floor measured 1.37x the shadowed one on this
#                fixture, so this is well inside the effect and well outside the
#                noise -- the arm is asserting a shadow exists, not sizing it.
# PRESENCE_MIN   two black frames agree on every ratio, so the acceptance arms
#                demand the floor is actually lit. Linear, against a floor that
#                reads ~0.12 lit and ~0.001 under a clear colour.
# GI_LIFT_MIN    the indirect term must actually lift, or a ratio of two absent
#                lifts is 1.0 and says nothing.
# OFF_MOVED_MIN  the flag must actually change the frame, or "off equals opted
#                out" passes on two identical blank renders.
EMISSIVE_PLANARITY_MAX = 0.5
EMISSIVE_SHADOW_LIFT_MIN = 1.10
EMISSIVE_PRESENCE_MIN = 0.02
EMISSIVE_GI_LIFT_MIN = 1.05
EMISSIVE_OFF_MOVED_MIN = 0.10

# Read bands, as WORLD patches on named surfaces of the cornell room, projected
# through the fixture's own camera.
#
# They were hand-written screen boxes and every one of them measured something
# other than its name. Ray-cast against the room and tallied over the same 12x12
# grid _absorb_box_rgb samples: the "floor" box hit 59 short-box, 34 tall-box and
# 33 floor samples -- 65% boxes -- and the two wall boxes were each EXACTLY half
# background, because they straddled the room's silhouette. A wall ratio blended
# 50/50 with the G-buffer clear is not a wall ratio, and the number this group
# printed as `left=0.9969` never was one.
#
# Derived here for the reason _cscn_camera exists (its own docstring: "the
# failure is silent -- the gate keeps passing while measuring a different scene
# than it predicts"), and stated as the surface each band is ON so the name and
# the measurement cannot come apart again.
#
# Placement respects the room's own furniture: the tall box spans x -0.79..0.06
# and the short one x -0.05..0.81 (gen_cornell_rooms.py box(cx, cz, w, h, d, yaw),
# half-diagonal 0.42 under yaw), so the floor patch sits left of both and forward
# of the tall one; the wall patches sit ABOVE the tall box's 1.2 height, where
# nothing occludes them.
# `shadow` is floor the TALL BOX hides from the ceiling panel, which the lit
# bands above deliberately avoid -- so the first version of emissive-override,
# reading `floor`, compared two frames that were identical there and reported
# 1.0000x. Checked rather than eyeballed: a floor point (x, z) sees the panel
# centre through height 1.2 at (0.394x, 1.2, 0.394z), and for this patch that
# lands inside the box's yawed 0.6 top face at every corner.
EMISSIVE_WORLD_BANDS = {
    "floor": [(-0.80, 0.0, 0.30), (-0.20, 0.0, 0.30), (-0.20, 0.0, 0.85), (-0.80, 0.0, 0.85)],
    "shadow": [(-0.80, 0.0, -0.80), (-0.50, 0.0, -0.80), (-0.50, 0.0, -0.50), (-0.80, 0.0, -0.50)],
    "left": [(-1.0, 1.40, -0.45), (-1.0, 1.40, 0.45), (-1.0, 1.80, 0.45), (-1.0, 1.80, -0.45)],
    "right": [(1.0, 1.40, -0.45), (1.0, 1.40, 0.45), (1.0, 1.80, 0.45), (1.0, 1.80, -0.45)],
}

# Fraction of each projected band trimmed off every side. The patches above are
# already interior, so this is against the projection's own rounding and the
# 12x12 sample grid landing on a boundary texel, not a margin for error in them.
EMISSIVE_BAND_INSET = 0.12


EMISSIVE_LEAK_FIXTURE = "cornell_leak.cscn"

# cornell_leak is the same room cut in half by a partition at x = 0, lit over the
# LEFT half only (gen_cornell_rooms.py `r.panel(-0.5)`). It exists to prove light
# does NOT arrive where it has no path.
#
# Mirrored patches on the BACK WALL (z = -1), one each side of the partition.
# The back wall and not the floor because the .cscn camera is nearly horizontal
# (eye y 1.0, target y 0.95), so the floor projects into a grazing sliver while
# the back wall faces the lens. Mirrored rather than absolute because the two
# configurations differ in how bright the LIT half is -- a 160 lumen spot is not
# an 18 nit panel -- so the reading that means anything is dark AGAINST lit
# inside one frame.
#
# Deliberately away from the open front (z = +1), which is the one real path
# light has into the occluded half and would put a legitimate reading in a band
# whose whole claim is that it receives none.
EMISSIVE_LEAK_BANDS = {
    "lit": [(-0.85, 0.55, -1.0), (-0.20, 0.55, -1.0), (-0.20, 1.35, -1.0), (-0.85, 1.35, -1.0)],
    "dark": [(0.20, 0.55, -1.0), (0.85, 0.55, -1.0), (0.85, 1.35, -1.0), (0.20, 1.35, -1.0)],
}

# The occluded half must read as a fraction of the lit half. Measured 0.0000
# exactly with the shadow on -- the room is direct-lit only, so a fully occluded
# surface is black rather than nearly so -- and 0.4115 with it off. The bar sits
# far from both, since what separates them is three orders of magnitude and not a
# margin.
#
# LEAK_MIN is the falsifier and is the more important of the two. Without it this
# arm passes on any frame where the dark half happens to be black, INCLUDING a
# build where the derived panel never lit anything at all: "no light through the
# wall" and "no light" are the same reading. The unshadowed render is what proves
# the band can detect a leak, so the shadowed zero means something.
EMISSIVE_LEAK_MAX = 0.02
EMISSIVE_LEAK_MIN = 0.15

# The specular double count (P11). A reflection probe photographs the glowing
# quad and the LTC panel integrates the same rectangle analytically, so a smooth
# surface receives it twice.
#
# Read as the derived side's PROBE LIFT over the authored side's, which controls
# for everything in the capture that is not the emitter -- the sky above all.
# The authored fixture's quad emits a token 1 nit beside its 18 nit analytic
# light, the derived one carries the full 18 in the quad itself, so the
# difference between the two lifts IS the emitter's own share of the probe.
#
# Measured worst 1.0386, against 1.3134 for the diffuse double count before P6.
# An order of magnitude smaller, and the reason is the split sum: a DDGI probe
# integrates irradiance over the whole hemisphere, where a bright ceiling panel
# dominates, while the reflection probe feeds specular through a lobe where the
# panel covers little solid angle and a dielectric reflects a few percent of it.
#
# --sun-elevation -10 is pinned and load-bearing, exactly as --cloud-coverage
# 0.10 is on the cloud-shadow arms: an arm wants the configuration where the
# property is LEGIBLE. With the default sun the sky dominates the capture and
# the same measurement reads 1.0150, so a default change could halve the arm's
# sensitivity silently. The sun is below the horizon rather than the sky absent
# because the probe requires a precomputed IBL to be created at all.
#
# Sky rather than an HDR for three reasons: my_models/ is out of tree so an HDR
# arm would SKIP in a clean checkout, the sky path is 0 px run-to-run, and the
# elevation is a dial that turns the confound DOWN. A fixed HDR has none of the
# three.
EMISSIVE_SPEC_MAX = 1.10
# The probe must actually lift, or a ratio of two absent lifts is 1.0 and the
# arm passes on a build where the capture never ran. Max lift measured 2.6464.
EMISSIVE_SPEC_LIFT_MIN = 1.5
EMISSIVE_SPEC_SKY = ["--sky", "--sun-elevation", "-10"]
# Polished, but a DIELECTRIC. Metal has no diffuse, so every lift ratio's
# denominator collapses toward black and the ratio destabilises -- measured, the
# floor and back wall go nearly black and the read stops meaning anything.
EMISSIVE_SPEC_ROUGHNESS = 0.08


def _emissive_boxes(w, h):
    """The world bands above as fractional screen boxes, for _absorb_box_rgb."""
    project = _projector(_cscn_camera(CORNELL_FIXTURE), w, h)
    boxes = {}
    for name, pts in EMISSIVE_WORLD_BANDS.items():
        xs, ys = zip(*(project(p) for p in pts))
        x0, x1 = min(xs) / w, max(xs) / w
        y0, y1 = min(ys) / h, max(ys) / h
        dx, dy = (x1 - x0) * EMISSIVE_BAND_INSET, (y1 - y0) * EMISSIVE_BAND_INSET
        boxes[name] = (x0 + dx, y0 + dy, x1 - dx, y1 - dy)
    return boxes


def _emissive_lumas(path):
    """Mean linear luma per band. One place that knows how a band is measured."""
    w, h, pix = _read_ppm(path)
    return {k: sum(_absorb_box_rgb(pix, w, h, b)) / 3.0
            for k, b in _emissive_boxes(w, h).items()}


def _emissive_worst(base, other):
    """Per-band ratios and the band furthest from 1.0.

    `max` over a key rather than a running best against a 0.0 seed. The seeded
    form was written three times here and one copy carried `or worst == 0.0`,
    which made 0.0 both "not set yet" and a legal reading -- so a band whose
    ratio collapsed to exactly zero, the strongest failure available, was
    overwritten by the next band and the arm passed. There is no sentinel here
    to get that wrong.
    """
    ratios = {k: other[k] / max(base[k], 1e-6) for k in base}
    name = max(ratios, key=lambda k: abs(ratios[k] - 1.0))
    return ratios, name, ratios[name]


def _probe_render(scene, flag, prefix, extra=None, frames=30):
    """Run a --*-probe flag headless and return ([{key: str}], combined output).

    Probe lines are `<prefix> [tag] k=v k=v ...`, and every probe in the tree
    emits that shape -- so parsing it lives here rather than being written out
    per subsystem, which it was six times before this existed.

    Values stay STRINGS. Callers cast what they need: the emissive probe carries
    `center=1,2,3`, which a blanket float() would drop on the floor.
    """
    # No -S, and no workdir: every caller reads probe ROWS off stdout, so the
    # frames these used to write were a readback and a file write nobody opened.
    cmd = [RENDER, "-m", scene, "-x", "-f", str(frames), "-W", "400", "-H", "300", flag]
    r = _run(cmd + (extra or []), capture_output=True, text=True)
    text = r.stdout + r.stderr
    rows = []
    for line in text.splitlines():
        if not line.startswith(prefix + " "):
            continue
        parts = line.split()[1:]
        rec = {}
        # A leading bare token is the line's TAG, which is how a probe says which
        # kind of row this is. Dispatching on that rather than on whether some
        # field happens to parse is the lesson _water_fft_probe records.
        if parts and "=" not in parts[0]:
            rec["kind"] = parts[0]
            parts = parts[1:]
        for tok in parts:
            if "=" in tok:
                k, v = tok.split("=", 1)
                rec[k] = v
        rows.append(rec)
    return rows, text


def _emissive_probe(scene, extra=None, frames=8):
    """Run --emissive-light-probe and parse its lines into dicts.

    Reads the INSTRUMENT rather than the image on purpose: a panel half a metre
    out or sqrt(2) too wide still lights a room, so a frame cannot tell a correct
    fit from a plausible one. Every geometric arm here goes through this.
    """
    return _probe_render(scene, "--emissive-light-probe", "emissive-light-probe",
                         extra=extra, frames=frames)


def _emissive_vec(rec, key):
    try:
        return tuple(float(x) for x in rec[key].split(","))
    except (KeyError, ValueError):
        return None


def _emissive_named(rows, kind, node):
    for r in rows:
        if r.get("kind") == kind and r.get("node") == node:
            return r
    return None


def run_emissive_gate(workdir):
    """Emissive geometry derives an LTC area light (spec 11.49).

    The arms split into three groups, and the split is the point.

    GEOMETRY, read off the probe against numbers taken from the generators:

      emissive-fit      the cornell quad's centre, normal and size. Exact, not
                        approximate -- the mesh is a flat axis-aligned square and
                        the fit owes it to the digit.
      emissive-strip    a 2.0 x 0.25 quad rotated 30 degrees in its own plane.
                        Fails the covariance principal axis this was first written
                        as, and fails an axis-aligned bound, both of which look
                        right on every other shape in the corpus.
      emissive-reject   a closed cube rejects, AND for the stated reason. The
                        planarity number alone does not discriminate: the probe
                        zeroes the fit for any non-geometric reject, so every
                        other reason prints 0.000000 and satisfies a bare
                        threshold -- an opted-out material used to pass this.
      emissive-fill     two coplanar strips with a gap, and an L. Both read
                        planarity exactly 1.0, so the flatness test above says
                        nothing about either, and both would otherwise get a panel
                        radiating from space with no geometry in it. Two shapes
                        rather than one because they fail for different reasons --
                        disjoint against concave -- and a threshold catching only
                        one of them would look correct.
      emissive-texmean  a half-black half-white emissive texture reads 0.5. The
                        only arm that sees the top-mip readback at all.
      emissive-placed   a panel on a TRANSLATED node lands in the same place at 4
                        frames and at 40. Every fixture node is identity-
                        transformed, which is the one arrangement where the local
                        fit and the world placement cannot be told apart -- and
                        this module shipped re-applying the node transform to its
                        own output every frame, which no other arm can see.

    INTENT, because the feature has to be able to decline:

      emissive-unlit    water_fixture's bed and ramp are emissive over a BLACK
                        base -- the unlit-flat-colour idiom -- and the probe
                        reports both. This arm exists because that measurement is
                        why the feature is off by default; without it a later
                        change could quietly start treating them as lamps and
                        nothing would object.
      emissive-optout   emissiveLight: "off" takes the cornell panel to zero
                        derived lights, and the probe still REPORTS it declining.
                        Authored as the LABEL, so it covers the scene-file change
                        too. The count alone is what a scene that failed to load
                        prints, which is why the reject row is asserted with it.

    LIGHT, which is what any of it was for:

      emissive-match    a derived panel and a hand-authored one, at equal
                        radiance, light the room the same. THE acceptance test.
                        Read on floor and walls, never the ceiling panel, whose
                        own pixels differ 18x between the two by construction.
      emissive-override light_overrides can NAME a derived panel -- a light the
                        scene file could not have known about -- which is the
                        whole ordering claim. Asserted on the renderer's own
                        report rather than on a shadow: toggling cast_shadows
                        changes no band on this fixture, for the AUTHORED light
                        too, so an area panel's shadow is not measurable here and
                        claiming it would be asserting A7 through a blind
                        instrument.
      emissive-occluded the occlusion the arm above cannot see, on the fixture
                        built to ask it. cornell_leak is one room cut in half by
                        a partition and lit over one side only. A derived panel
                        inherits cast_shadows FALSE, so by default it lights the
                        far half straight through the wall -- 0.41 of the lit
                        half -- and a light_overrides entry takes that to 0.
                        BOTH are asserted: without the leaking frame, "no light
                        through the wall" and "no light at all" are one reading.
      emissive-gi       a derived panel and a hand-authored one lift the indirect
                        term by the same amount. A DDGI probe capture runs the
                        full forward shader, so it sees the emissive surface AND
                        the surfaces the panel already lit -- the first bounce
                        lands twice, and it read 1.31x on the floor before the
                        capture learned to silence a derived emitter. Reads the
                        RATIO of lifts, so it does not move when the fixture's
                        exposure or geometry does.
      emissive-spec     the SPECULAR double count, BOUNDED rather than removed. A
                        reflection probe photographs the glowing quad and the LTC
                        panel integrates the same rectangle analytically, so a
                        smooth surface gets it twice. Read as the derived side's
                        probe lift over the authored side's, which cancels
                        everything in the capture that is not the emitter.
                        Measured 1.0386 against the arm above's 1.3134, and the
                        fix was declined at that size: unlike the GI case the
                        emitter must STAY in a radiance capture, or a mirror
                        reflects a uniform rectangle instead of the surface. So
                        this one does not assert 1.0 -- it asserts the effect
                        stays small enough for that judgement to hold.
      emissive-off      two ways of having no derived panel are BYTE-IDENTICAL,
                        and both differ from having one. NOT "the room goes
                        dark", which is what this was first written as: strip the
                        authored light and turn the feature off and the scene has
                        no lights at all, so the render app substitutes its
                        three-point fallback and the room gets BRIGHTER, 16x.

    Every band read here is derived from the fixture's own camera and stated as a
    WORLD patch (EMISSIVE_WORLD_BANDS). The hand-written screen boxes this group
    shipped with measured a "floor" that was 65% the two boxes and walls that were
    half empty background.
    """
    failures = []
    fixture = os.path.join(ROOT, "assets", EMISSIVE_FIXTURE)
    cornell = os.path.join(ROOT, "assets", CORNELL_FIXTURE)

    # --- geometry -----------------------------------------------------------
    if not os.path.exists(cornell):
        print(f"  emissive-fit   SKIP  ({CORNELL_FIXTURE} not present)")
    else:
        rows, _ = _emissive_probe(cornell)
        rec = _emissive_named(rows, "panel", "cornell_light")
        if not rec:
            print("  emissive-fit   FAIL  no panel derived from cornell_light")
            failures.append("emissive-fit")
        else:
            c = _emissive_vec(rec, "center")
            n = _emissive_vec(rec, "normal")
            s = _emissive_vec(rec, "size")
            dc = max(abs(c[i] - EMISSIVE_CORNELL_CENTER[i]) for i in range(3))
            dn = max(abs(n[i] - EMISSIVE_CORNELL_NORMAL[i]) for i in range(3))
            ds = max(abs(sorted(s)[i] - sorted(EMISSIVE_CORNELL_SIZE)[i]) for i in range(2))
            plan = float(rec.get("planarity", 0.0))
            ok = (dc <= EMISSIVE_GEOM_EPS and dn <= EMISSIVE_GEOM_EPS
                  and ds <= EMISSIVE_GEOM_EPS and abs(plan - 1.0) <= EMISSIVE_GEOM_EPS)
            print(f"  emissive-fit   {'PASS' if ok else 'FAIL'}  center={c} normal={n} "
                  f"size={s} planarity={plan:.6f} worst_delta={max(dc, dn, ds):.6f} "
                  f"want <={EMISSIVE_GEOM_EPS}")
            if not ok:
                failures.append("emissive-fit")

    if not os.path.exists(fixture):
        print(f"  emissive-strip SKIP  ({EMISSIVE_FIXTURE} not present)")
    else:
        rows, _ = _emissive_probe(fixture)

        rec = _emissive_named(rows, "panel", "emissive_strip")
        if not rec:
            print("  emissive-strip FAIL  no panel derived from emissive_strip")
            failures.append("emissive-strip")
        else:
            s = sorted(_emissive_vec(rec, "size"))
            want = sorted(EMISSIVE_STRIP_SIZE)
            ds = max(abs(s[i] - want[i]) for i in range(2))
            ok = ds <= EMISSIVE_GEOM_EPS
            print(f"  emissive-strip {'PASS' if ok else 'FAIL'}  size={s[0]:.6f}x{s[1]:.6f} "
                  f"want {want[0]}x{want[1]} delta={ds:.6f}")
            if not ok:
                failures.append("emissive-strip")

        # The REASON is asserted, not just printed. planarity alone cannot carry
        # this: _probe_node zeroes the fit and only runs it for a geometric
        # reject, so EVERY other reason prints planarity=0.000000 and satisfies a
        # bare threshold. An `opted-out` line passed this arm -- so had the
        # material key defaulted wrong, or the emissiveLight row been mis-wired
        # so every material read as opted out, this would have reported PASS on a
        # cube it never tested.
        rec = _emissive_named(rows, "reject", "emissive_box")
        got = rec.get("reason", "-") if rec else "no reject line"
        plan = float(rec["planarity"]) if rec and "planarity" in rec else float("nan")
        ok = rec is not None and got == "not-planar" and plan <= EMISSIVE_PLANARITY_MAX
        print(f"  emissive-reject {'PASS' if ok else 'FAIL'} reason={got} want not-planar; "
              f"planarity={plan:.6f} want <={EMISSIVE_PLANARITY_MAX}")
        if not ok:
            failures.append("emissive-reject")

        # FILL: flat is not the same as rectangular, and planarity only tests
        # flat. Both of these read planarity 1.0 and both would otherwise get a
        # panel spanning geometry that is not there -- the split one radiating
        # from the empty gap between its two strips, which is a lamp where there
        # is no lamp. Two shapes rather than one because they fail for different
        # reasons, disjoint against concave, and a threshold catching only one of
        # them would look correct.
        for node_name, want_fill in (("emissive_split", EMISSIVE_SPLIT_FILL),
                                     ("emissive_ell", EMISSIVE_ELL_FILL)):
            rec = _emissive_named(rows, "reject", node_name)
            got = rec.get("reason", "-") if rec else "no reject line"
            plan = float(rec["planarity"]) if rec and "planarity" in rec else float("nan")
            fill = float(rec["fill"]) if rec and "fill" in rec else float("nan")
            # Planarity asserted at 1.0 alongside, because that is the whole
            # point: this shape is rejected by fill and NOT by flatness, and an
            # arm that did not say so would pass if the two tests were confused.
            ok = (rec is not None and got == "not-filled"
                  and abs(fill - want_fill) <= EMISSIVE_FILL_EPS
                  and abs(plan - 1.0) <= EMISSIVE_GEOM_EPS)
            print(f"  emissive-fill  {'PASS' if ok else 'FAIL'} {node_name} reason={got} "
                  f"want not-filled; fill={fill:.6f} want {want_fill} "
                  f"+/-{EMISSIVE_FILL_EPS}; planarity={plan:.6f} want 1.0 "
                  "(flat, and still not a rectangle)")
            if not ok and "emissive-fill" not in failures:
                failures.append("emissive-fill")

        rec = _emissive_named(rows, "panel", "emissive_quad")
        if not rec or rec.get("radiance") != "final":
            state = rec.get("radiance", "absent") if rec else "absent"
            print(f"  emissive-texmean FAIL  radiance={state}, want final "
                  "(the texture's mip never arrived)")
            failures.append("emissive-texmean")
        else:
            nits = float(rec["nits"])
            d = abs(nits - EMISSIVE_TEX_MEAN)
            ok = d <= EMISSIVE_TEX_EPS
            print(f"  emissive-texmean {'PASS' if ok else 'FAIL'} nits={nits:.6f} "
                  f"want {EMISSIVE_TEX_MEAN} +/-{EMISSIVE_TEX_EPS} (delta={d:.6f})")
            if not ok:
                failures.append("emissive-texmean")

        # PLACEMENT, on a TRANSFORMED node, which nothing else in the corpus has.
        #
        # The fit is local; where the panel ends up is the other half, and every
        # emissive fixture node is identity-transformed -- the one arrangement
        # where world == local and the two halves cannot be told apart. This
        # module shipped writing world values back into the fields it read the
        # local fit from, so from frame two the node transform was re-applied to
        # its own output: a translated node walked its panel away, compounding,
        # and nine green arms could not see it.
        #
        # Rendering the same scene twice at 10x the frames and demanding the same
        # answer is what catches an accumulating error, where any single frame
        # looks plausible.
        moved = os.path.join(workdir, "emissive_moved.gltf")
        with open(fixture) as f:
            doc = json.load(f)
        for n in doc["nodes"]:
            if n["name"] == "emissive_quad":
                n["translation"] = [0.5, 0.0, 0.0]
        for b in doc.get("buffers", []):
            b.pop("_", None)
        with open(moved, "w") as f:
            json.dump(doc, f)

        short_rows, _ = _emissive_probe(moved, ["--emissive-lights"], frames=4)
        long_rows, _ = _emissive_probe(moved, ["--emissive-lights"], frames=40)
        s_rec = _emissive_named(short_rows, "placed", "emissive_quad")
        l_rec = _emissive_named(long_rows, "placed", "emissive_quad")
        if not s_rec or not l_rec:
            print("  emissive-placed FAIL  no placed row (the panel was never built)")
            failures.append("emissive-placed")
        else:
            sc, lc = _emissive_vec(s_rec, "center"), _emissive_vec(l_rec, "center")
            ss, ls = _emissive_vec(s_rec, "size"), _emissive_vec(l_rec, "size")
            drift = max(max(abs(sc[i] - lc[i]) for i in range(3)),
                        max(abs(ss[i] - ls[i]) for i in range(2)))
            ok = drift <= EMISSIVE_GEOM_EPS
            print(f"  emissive-placed {'PASS' if ok else 'FAIL'} 4 frames vs 40 on a "
                  f"translated node: centre {sc} -> {lc} size {ss} -> {ls} "
                  f"drift={drift:.6f} want <={EMISSIVE_GEOM_EPS}")
            if not ok:
                failures.append("emissive-placed")

    # --- intent -------------------------------------------------------------
    water = os.path.join(ROOT, "assets", EMISSIVE_WATER_FIXTURE)
    if not os.path.exists(water):
        print(f"  emissive-unlit SKIP  ({EMISSIVE_WATER_FIXTURE} not present)")
    else:
        rows, _ = _emissive_probe(water, ["--no-water"])
        panels = sorted(r["node"] for r in rows if r.get("kind") == "panel")
        ok = panels == ["water_bed", "water_ramp"]
        print(f"  emissive-unlit {'PASS' if ok else 'FAIL'}  panels={panels} "
              "want ['water_bed', 'water_ramp'] (emissive over a black base is not a lamp)")
        if not ok:
            failures.append("emissive-unlit")

    if os.path.exists(cornell):
        optout = os.path.join(workdir, "cornell_optout.cscn")
        cscn_copy(cornell, optout,
                  lambda d: d.setdefault("materials", {})
                             .setdefault("cornell_light", {})
                             .update({"emissiveLight": "off"}))
        rows, _ = _emissive_probe(optout, ["--emissive-lights"])
        header = next((r for r in rows if r.get("kind") == "header"), None)
        count = int(header["count"]) if header and "count" in header else -1
        # A count of 0 is also what a scene that failed to load prints, and what
        # any scene with no emissive mesh prints. The reject row is the evidence
        # that the mesh was SEEN and DECLINED, which is the actual claim -- and it
        # comes free out of rows already in hand.
        rec = _emissive_named(rows, "reject", "cornell_light")
        got = rec.get("reason", "-") if rec else "no reject line"
        ok = count == 0 and got == "opted-out"
        print(f"  emissive-optout {'PASS' if ok else 'FAIL'} derived={count} want 0, "
              f"cornell_light reason={got} want opted-out "
              "(authored as the label, not the number)")
        if not ok:
            failures.append("emissive-optout")

    # --- light --------------------------------------------------------------
    if not os.path.exists(cornell):
        print(f"  emissive-match SKIP  ({CORNELL_FIXTURE} not present)")
        return failures

    # --no-bloom is load-bearing, not hygiene. The derived twin carries
    # emissiveStrength 18, so its QUAD is 18x the authored one's -- handled for
    # direct pixels by never reading the ceiling, but bloom carries that
    # difference out of the quad and across the frame, asymmetrically, into every
    # band this group reads. Without it the residual these arms measure is part
    # panel placement and part glow, and the bar absorbs an effect nobody has
    # separated.
    base = ["--no-auto-exposure", "-E", "1.0", "--no-dither", "--no-bloom"]

    # The derived twin: the quad carries the authored light's radiance, the
    # authored light is gone, and a light_overrides entry gives the panel the
    # shadow the authored one had. Generated rather than committed, so the two
    # halves cannot differ in anything this does not touch.
    def _derive(d):
        d["lights"] = []
        d["light_overrides"] = [{"name": "cornell_light", "cast_shadows": True}]
        d.setdefault("materials", {}).setdefault("cornell_light", {})["emissiveStrength"] = 18.0

    derived = os.path.join(workdir, "cornell_derived.cscn")
    cscn_copy(cornell, derived, _derive)

    a = os.path.join(workdir, "emissive_authored.ppm")
    b = os.path.join(workdir, "emissive_derived.ppm")
    err = render(cornell, a, base)
    if err:
        print(f"  emissive-match ERROR authored render failed: {err.strip()[-200:]}")
        return failures + ["emissive-match"]
    err = render(derived, b, base + ["--emissive-lights"])
    if err:
        print(f"  emissive-match ERROR derived render failed: {err.strip()[-200:]}")
        return failures + ["emissive-match"]

    lum_a = _emissive_lumas(a)
    lum_b = _emissive_lumas(b)
    ratios, worst_name, worst = _emissive_worst(lum_a, lum_b)
    # A presence floor, because every band ratio here is 1.0 on two BLACK frames
    # and this arm is the acceptance test. The floor band is lit by the panel in
    # both variants, so it is the honest one to demand light from; the value is
    # well under the ~30/255 it actually reads and well over a clear colour.
    lit = min(lum_a["floor"], lum_b["floor"])
    detail = " ".join(f"{k}={v:.4f}" for k, v in ratios.items())
    ok = abs(worst - 1.0) <= EMISSIVE_MATCH_EPS and lit >= EMISSIVE_PRESENCE_MIN
    print(f"  emissive-match {'PASS' if ok else 'FAIL'}  derived/authored {detail} "
          f"worst={worst:.4f} at {worst_name} want 1.0 +/-{EMISSIVE_MATCH_EPS}; "
          f"floor lit={lit:.4f} want >={EMISSIVE_PRESENCE_MIN}")
    if not ok:
        failures.append("emissive-match")

    # Asserted on the OVERRIDE BEING APPLIED, not on a shadow appearing.
    #
    # This arm first compared the no-override frame against the AUTHORED one and
    # read 1.3684x, which looked like the shadow and was not: those two frames
    # also differ by the 18x quad and the 1 cm offset, and all three push the
    # ratio the same way. Compared against the frame that differs ONLY in the
    # override, every band is identical to four decimals.
    #
    # And the reason is not this feature. Toggling cast_shadows on the fixture's
    # own HAND-AUTHORED area light changes nothing in any band either, so an area
    # panel's shadow is simply not measurable on cornell_box. That is an A7
    # property; asserting it here would be asserting someone else's feature
    # through an instrument that cannot see it.
    #
    # What this arm owns is that a light_overrides entry can NAME a light that did
    # not exist when the scene file was written -- which is the whole ordering
    # claim -- and the renderer says so itself. The failure mode is loud and
    # distinct: cscene_apply prints "matches no light" instead.
    _, text = _emissive_probe(derived, ["--emissive-lights"])
    applied = "light 'cornell_light' cast_shadows on" in text
    unmatched = "matches no light" in text
    ok = applied and not unmatched
    print(f"  emissive-override {'PASS' if ok else 'FAIL'} override reached the derived "
          f"panel: applied={applied} unmatched={unmatched} "
          "(names a light the scene file could not have known about)")
    if not ok:
        failures.append("emissive-override")

    # OCCLUSION, which the arm above explicitly cannot measure: it records that an
    # area panel's shadow does not move any band on cornell_box, for either the
    # derived panel or the fixture's own authored one. cornell_leak is the fixture
    # where it does move, because the whole room is built around one occluder.
    #
    # A derived panel inherits cast_shadows FALSE -- create_light's default, which
    # emissive_light.c never overrides -- so out of the box it lights straight
    # through the partition. That is not a defect this feature introduced; it is
    # what every non-shadowing area light does, and it is the reason
    # cornell_leak.cscn authors a SPOT rather than reusing the box room's panel.
    # What was missing is that nothing pointed the fixture at a derived panel, so
    # the behaviour was neither asserted nor written down anywhere a change could
    # trip over it.
    #
    # Both directions in one arm, because separately neither is worth much: the
    # shadowed zero is only meaningful beside a frame proving the band can read a
    # leak at all.
    leak_src = os.path.join(ROOT, "assets", EMISSIVE_LEAK_FIXTURE)
    if not os.path.exists(leak_src):
        print(f"  emissive-occluded SKIP ({EMISSIVE_LEAK_FIXTURE} not present)")
    else:
        def _leak_variant(shadow):
            def mutate(d):
                d["lights"] = []
                d.setdefault("materials", {}).setdefault(
                    "cornell_light", {})["emissiveStrength"] = 18.0
                if shadow:
                    d["light_overrides"] = [{"name": "cornell_light", "cast_shadows": True}]
            return mutate

        leak_ok = True
        reads = {}
        for tag, shadow in (("shadowed", True), ("unshadowed", False)):
            scn = os.path.join(workdir, f"emissive_leak_{tag}.cscn")
            cscn_copy(leak_src, scn, _leak_variant(shadow))
            img = os.path.join(workdir, f"emissive_leak_{tag}.ppm")
            err = render(scn, img, base + ["--emissive-lights"])
            if err:
                print(f"  emissive-occluded ERROR {tag} render failed: {err.strip()[-200:]}")
                failures.append("emissive-occluded")
                leak_ok = False
                break
            w, h, pix = _read_ppm(img)
            project = _projector(_cscn_camera(EMISSIVE_LEAK_FIXTURE), w, h)
            band = {}
            for name, pts in EMISSIVE_LEAK_BANDS.items():
                xs, ys = zip(*(project(pt) for pt in pts))
                x0, x1 = min(xs) / w, max(xs) / w
                y0, y1 = min(ys) / h, max(ys) / h
                dx, dy = (x1 - x0) * EMISSIVE_BAND_INSET, (y1 - y0) * EMISSIVE_BAND_INSET
                band[name] = sum(_absorb_box_rgb(
                    pix, w, h, (x0 + dx, y0 + dy, x1 - dx, y1 - dy))) / 3.0
            reads[tag] = band["dark"] / max(band["lit"], 1e-6)

        if leak_ok:
            shadowed, unshadowed = reads["shadowed"], reads["unshadowed"]
            ok = shadowed <= EMISSIVE_LEAK_MAX and unshadowed >= EMISSIVE_LEAK_MIN
            print(f"  emissive-occluded {'PASS' if ok else 'FAIL'} dark/lit behind the "
                  f"partition: shadowed={shadowed:.4f} want <={EMISSIVE_LEAK_MAX}; "
                  f"unshadowed={unshadowed:.4f} want >={EMISSIVE_LEAK_MIN} "
                  "(the second is the falsifier -- it proves the band can see a leak)")
            if not ok:
                failures.append("emissive-occluded")

    # The indirect term must not count the emitter twice. Four renders because a
    # LIFT is what is being compared, not a brightness: each side is measured
    # against its own GI-off frame, so the arm survives the two sides differing
    # in anything else. 120 frames is the probe volume converging -- it captures a
    # couple of probes a frame and then idles.
    gi_a = os.path.join(workdir, "emissive_gi_authored.ppm")
    gi_b = os.path.join(workdir, "emissive_gi_derived.ppm")
    # Dedicated GI-OFF baselines at the same frame count, rather than reusing the
    # match arm's 30-frame renders. A lift divides two frames, so anything that
    # differs between them contaminates it -- and 30 against 120 is a real
    # difference here, worth 2.7 points on the floor when it was measured.
    # The authored side gets its QUAD darkened for this arm, which the direct
    # arms above deliberately do not do. Reason: the derived side's emitter is
    # suppressed inside an irradiance capture (that is the feature), so an
    # authored quad still emitting its token 1 nit into ITS capture makes the two
    # captures see different amounts of emitter and the ratio inherits the
    # difference. Measured at 0.93 before this, in the direction the asymmetry
    # predicts. Darkening it leaves both captures seeing only bounced light,
    # which is the comparison the arm claims to make.
    def _derive_gi_authored(d):
        d.setdefault("materials", {}).setdefault("cornell_light", {})["emissiveStrength"] = 0.0

    gi_authored = os.path.join(workdir, "cornell_gi_authored.cscn")
    cscn_copy(cornell, gi_authored, _derive_gi_authored)

    off_a = os.path.join(workdir, "emissive_gi_authored_off.ppm")
    off_b = os.path.join(workdir, "emissive_gi_derived_off.ppm")
    err = (render(gi_authored, off_a, base, frames=EMISSIVE_GI_FRAMES) or
           render(derived, off_b, base + ["--emissive-lights"], frames=EMISSIVE_GI_FRAMES) or
           render(gi_authored, gi_a, base + ["--gi-volume"], frames=EMISSIVE_GI_FRAMES) or
           render(derived, gi_b, base + ["--emissive-lights", "--gi-volume"],
                  frames=EMISSIVE_GI_FRAMES))
    if err:
        print(f"  emissive-gi    ERROR render failed: {err.strip()[-200:]}")
        failures.append("emissive-gi")
    else:
        lift_a, _, _ = _emissive_worst(_emissive_lumas(off_a), _emissive_lumas(gi_a))
        lift_b, _, _ = _emissive_worst(_emissive_lumas(off_b), _emissive_lumas(gi_b))
        ratios, worst_name, worst = _emissive_worst(lift_a, lift_b)
        detail = " ".join(f"{k}={v:.4f}" for k, v in ratios.items())
        # The lift itself must be real, or two GI-inert frames give lift 1.0 on
        # both sides and a ratio of 1.0 that means nothing.
        ok = (abs(worst - 1.0) <= EMISSIVE_GI_EPS and
              min(lift_a["floor"], lift_b["floor"]) >= EMISSIVE_GI_LIFT_MIN)
        print(f"  emissive-gi    {'PASS' if ok else 'FAIL'}  derived/authored GI lift "
              f"{detail} worst={worst:.4f} at {worst_name} want 1.0 +/-{EMISSIVE_GI_EPS}; "
              f"floor lift a={lift_a['floor']:.4f} b={lift_b['floor']:.4f} "
              f"want >={EMISSIVE_GI_LIFT_MIN}")
        if not ok:
            failures.append("emissive-gi")

    # The SPECULAR counterpart of the arm above, and the reason it is a separate
    # arm rather than a second read on the same renders: the two double counts
    # want OPPOSITE fixes. A DDGI capture's output is irradiance ADDED to the
    # analytic direct term, so the emitter must be absent from it -- that is what
    # emissive-gi pins. A reflection probe's output is radiance, what a mirror
    # sees, so the emitter must be PRESENT. Silencing it in both is one flag and
    # would decide this question by accident.
    #
    # This arm therefore does not assert 1.0. It asserts the effect stays SMALL,
    # because the fix was measured and declined: at 1.0386 on a fixture built to
    # maximise it, every available fix costs more than it buys. The bar is what
    # would fail if that stopped being true.
    spec_scn = {}
    for tag, mutate in (("authored", None), ("derived", True)):
        def _spec(d, derived_side=mutate):
            for m in ("cornell_shell", "cornell_left_red", "cornell_right_green"):
                d.setdefault("materials", {}).setdefault(
                    m, {})["roughness"] = EMISSIVE_SPEC_ROUGHNESS
            if derived_side:
                d["lights"] = []
                d["light_overrides"] = [{"name": "cornell_light", "cast_shadows": True}]
                d.setdefault("materials", {}).setdefault(
                    "cornell_light", {})["emissiveStrength"] = 18.0
        spec_scn[tag] = os.path.join(workdir, f"emissive_spec_{tag}.cscn")
        cscn_copy(cornell, spec_scn[tag], _spec)

    spec = {t: {k: os.path.join(workdir, f"emissive_spec_{t}_{k}.ppm")
                for k in ("on", "off")} for t in ("authored", "derived")}
    lights = ["--emissive-lights"]
    err = (render(spec_scn["authored"], spec["authored"]["off"],
                  base + EMISSIVE_SPEC_SKY, frames=60) or
           render(spec_scn["authored"], spec["authored"]["on"],
                  base + EMISSIVE_SPEC_SKY + ["--probe-scene"], frames=60) or
           render(spec_scn["derived"], spec["derived"]["off"],
                  base + EMISSIVE_SPEC_SKY + lights, frames=60) or
           render(spec_scn["derived"], spec["derived"]["on"],
                  base + EMISSIVE_SPEC_SKY + lights + ["--probe-scene"], frames=60))
    if err:
        print(f"  emissive-spec  ERROR render failed: {err.strip()[-200:]}")
        failures.append("emissive-spec")
    else:
        lift_a, _, _ = _emissive_worst(_emissive_lumas(spec["authored"]["off"]),
                                       _emissive_lumas(spec["authored"]["on"]))
        lift_b, _, _ = _emissive_worst(_emissive_lumas(spec["derived"]["off"]),
                                       _emissive_lumas(spec["derived"]["on"]))
        ratios, worst_name, worst = _emissive_worst(lift_a, lift_b)
        detail = " ".join(f"{k}={v:.4f}" for k, v in ratios.items())
        peak = max(lift_a.values())
        ok = worst <= EMISSIVE_SPEC_MAX and peak >= EMISSIVE_SPEC_LIFT_MIN
        print(f"  emissive-spec  {'PASS' if ok else 'FAIL'} derived/authored probe lift "
              f"{detail} worst={worst:.4f} at {worst_name} want <={EMISSIVE_SPEC_MAX}; "
              f"peak lift={peak:.4f} want >={EMISSIVE_SPEC_LIFT_MIN} "
              "(the emitter is counted twice on purpose -- this bounds it)")
        if not ok:
            failures.append("emissive-spec")

    # OFF IS OFF, stated as an identity rather than as "the room goes dark",
    # which is what this arm was first written as and is measured wrong.
    #
    # This variant has no authored light, so with the feature off the scene has
    # no lights at all -- and the render app then substitutes its three-point
    # fallback rig, which floods the room to 16x the authored frame. Darkness is
    # not available to assert. What IS available: two different ways of having no
    # derived panel must produce the same frame. The flag absent, and the flag
    # present with the material opted out. Both get the fallback, so it cancels,
    # and the arm fails if either route leaks a panel.
    optout_lit = os.path.join(workdir, "cornell_derived_optout.cscn")

    def _derive_optout(d):
        _derive(d)
        d["materials"]["cornell_light"]["emissiveLight"] = "off"

    cscn_copy(cornell, optout_lit, _derive_optout)

    d_off = os.path.join(workdir, "emissive_off.ppm")
    d_opt = os.path.join(workdir, "emissive_off_optout.ppm")
    err = render(derived, d_off, base) or render(optout_lit, d_opt, base + ["--emissive-lights"])
    if err:
        print(f"  emissive-off   ERROR render failed: {err.strip()[-200:]}")
        return failures + ["emissive-off"]
    # An exact identity, not a box tolerance. These two frames differ in nothing
    # a renderer should see -- same scene, same fallback rig, same 18-nit quad,
    # and no derived panel by either route -- so the honest bar is 0 px, which is
    # what the spec specified before this shipped at 3%. Reading it through boxes
    # also coupled it to EMISSIVE_MATCH_EPS, whose 3% is sized for a genuine
    # approximation elsewhere; tightening that would have silently tightened this.
    ae, pae = compare(d_off, d_opt)
    lum_off = _emissive_lumas(d_off)
    moved = abs(lum_off["floor"] - lum_b["floor"]) / max(lum_b["floor"], 1e-6)
    ok = ae == 0 and moved >= EMISSIVE_OFF_MOVED_MIN
    print(f"  emissive-off   {'PASS' if ok else 'FAIL'}  flag-absent vs opted-out "
          f"ae={ae} pae={pae:.6f} want 0 px; and vs derived moved={moved:.4f} "
          f"want >={EMISSIVE_OFF_MOVED_MIN}")
    if not ok:
        failures.append("emissive-off")

    return failures



# ---------------------------------------------------------------------------
# Auto-exposure metering (spec 11.52)
# ---------------------------------------------------------------------------

EXPOSURE_FIXTURE = "postfx_convergence_fixture.cscn"
EXPOSURE_SCALE_FIXTURE = "cornell_point.cscn"

# Frames. The meter reads a PRE-EXPOSED frame, so its own reading sits inside a
# feedback loop until the adaptation settles; anything measured before that is
# measuring the approach, not the answer.
#
# On this fixture the gap first touches the deadband around frame 18 and is
# pushed back out twice as the scene keeps brightening, settling at frame 91 --
# so 150 is margin over THAT, not over the deadband's first touch. An earlier
# note here said frame 12, measured before the percentiles changed what the meter
# reads and never re-taken.
EXPOSURE_FRAMES = 150
# Both legs of the linearity arm read identically at 60, 120, 150 and 200 frames,
# so this is the converged value plus headroom rather than a guess. Longer than
# EXPOSURE_FRAMES because the x1000 leg starts ~10 stops from its answer.
EXPOSURE_LINEAR_FRAMES = 120

# Meter linearity: emitters x1000 with EXPOSURE LEFT ALONE must move the raw
# metered value by exactly log2(1000) stops, because the meter reads absolute
# radiance with pre-exposure divided back out.
#
# NOT the SCALE_GATES shape, and that is the point. Scaling emitters by K while
# dividing the camera by K double-compensates once the meter is live -- the
# camera divides by K and the meter responds to the same K -- so pre_exposure
# lands K^2 off and no metering can satisfy it. Those arms are only meaningful
# with exposure pinned, which is how they are written.
#
# The bar is what separates the shipped meter from every alternative measured:
# floor at the key -1.6084 stops, percentiles 0.10/0.90 -6.1180, 0.50/0.95
# -0.1337, and the shipped 0.70/0.95 -0.0046. 0.05 admits the last and rejects
# the next-best by a factor of 29.
EXPOSURE_SCALE = 1000.0
EXPOSURE_LINEAR_EPS = 0.05

# Two runs of one build, every field of every line. Not a tolerance: the
# readback is deliberately blocking and the blend is per-FRAME rather than per
# second, both traded for exactly this.
EXPOSURE_STABLE_FRAMES = 60

# The snap makes adapted EXACTLY equal the target, so the true converged gap is
# 0. This is only float slack on two %.6f fields the engine prints in log2 --
# not a tolerance on the convergence itself.
EXPOSURE_CONVERGE_EPS = 1e-5

# A gain of exactly 1.0 is auto-exposure declining to act. Compared with ==
# rather than a tolerance because the cap is an fminf, so the value is the
# literal 1.0f or it is not the cap.
EXPOSURE_GAIN_CAP = 1.0
# The bright half of the same arm: a scene the meter SHOULD stop down. Measured
# 0.000827 at x1000, so this rejects a build where the cap swallowed everything.
EXPOSURE_DARKEN_MAX = 0.5

# Cutting the highlight tail must LOWER the metered value. Measured 0.3445 stops
# between high 1.0 and the shipped 0.95 on the convergence fixture.
#
# NOT measured on flare_fixture, which looks like the obvious instrument -- a
# strength-60 quad on a black backdrop -- and is the wrong one. That frame is so
# overwhelmingly black that the metered value pins at the shader's 1e-8 numeric
# guard whatever the percentiles are: -26.28 over the whole population against a
# guard at -26.58. An arm reading there is comparing two clamps, and it PASSED
# that way before this was checked. Hence the floor below, which is the real
# assertion: the arm is only meaningful while the meter is on live values.
EXPOSURE_TAIL_MIN_DROP = 0.15
EXPOSURE_TAIL_FLOOR = -20.0


def _exposure_probe(scene, extra=None, frames=EXPOSURE_FRAMES):
    """Run --exposure-probe and parse its lines. Returns (rows, combined output).

    Reads the INSTRUMENT rather than the image, for the reason the probe exists:
    a wrong exposure hides inside a plausible frame, and the metered value
    appears nowhere else -- no log line, no readout, and every golden pins it.

    Every field is a float here, unlike the emissive probe's, so the cast is this
    wrapper's whole job.
    """
    rows, text = _probe_render(scene, "--exposure-probe", "exposure-probe",
                               extra=extra, frames=frames)
    return [{k: float(v) for k, v in r.items()} for r in rows], text


def _exposure_live(src, dst, scale=1.0):
    """A .cscn twin with the meter LIVE, optionally with its emitters scaled.

    Every fixture in the corpus pins exposure -- by CLI or by authoring
    post.exposure with no auto_exposure key, which cscene_apply turns into a pin.
    So a live-meter arm cannot reuse one as-is, and generating the twin keeps the
    two halves identical in everything this does not touch.
    """
    def mutate(d):
        _scale_emitters(d, scale)
        post = d.setdefault("post", {})
        post.pop("exposure", None)
        post["auto_exposure"] = True
    cscn_copy(src, dst, mutate)


def run_exposure_gate(workdir):
    """Histogram auto-exposure: the meter, its mask and its percentiles (spec 11.52).

    This group exists because NOTHING asserted anything about metering. All 24
    goldens pin exposure, every gate fixture pins it too, and the constants the
    adaptation is built on -- the blend rate, the snap deadband, the metering
    bounds -- were untested while the file's own comments record that two of them
    SHIPPED WRONG and were found by hand.

      exposure-darkens  the gain is capped at 1.0, so auto-exposure only ever
                        darkens. Three readings, not one: a scene metering ~6
                        stops under the key reads exactly 1.0, it reports having
                        MEASURED that (gain 1.0 is also what three early exits
                        return), and a bright scene still stops down. Drop any of
                        the three and the arm passes on a broken build.
      exposure-converge the snap engages -- adapted reaches raw EXACTLY, not
                        nearly. Asserted against an early frame where the gap is
                        still open, so a build that never adapted at all cannot
                        pass by having no gap to close.
      exposure-stable   two runs of one build agree on every field of every line.
                        The blocking readback and the per-FRAME blend are both
                        traded for this and nothing checked it.
      exposure-linear   emitters x1000, exposure LEFT ALONE, raw metered moves
                        exactly log2(1000). The absolute floor this replaced fails
                        at -1.61 stops, so the arm is red on the old meter by
                        construction. Not the SCALE_GATES shape -- see the note on
                        EXPOSURE_SCALE.
      exposure-tail     cutting the highlight tail lowers the metered value, with
                        a floor asserting both readings are on live values rather
                        than pinned at the shader's numeric guard. That floor is
                        not decoration: written against flare_fixture, the obvious
                        small-bright-on-black instrument, this arm passed while
                        comparing two clamps 0.3 stops apart at 2^-26.
      exposure-mask     the mask is a pure re-weighting: a spot wide enough to
                        cover the frame reads BIT-IDENTICAL to uniform, while a
                        real spot moves. Identity is the falsifier -- it fails if
                        the weighting biases the answer rather than re-weighting
                        it.
    """
    failures = []
    src = os.path.join(ROOT, "assets", EXPOSURE_FIXTURE)
    if not os.path.exists(src):
        print(f"  exposure-darkens SKIP ({EXPOSURE_FIXTURE} not present)")
        return failures

    scale_src = os.path.join(ROOT, "assets", EXPOSURE_SCALE_FIXTURE)
    if not os.path.exists(scale_src):
        print(f"  exposure-darkens SKIP ({EXPOSURE_SCALE_FIXTURE} not present)")
        return failures

    live = os.path.join(workdir, "exposure_live.cscn")
    _exposure_live(src, live)

    # --- exposure-darkens ---------------------------------------------------
    # Forced below the key by metering the DARKEST fifth, which the floor this
    # replaced made impossible -- it clamped every texel up to the key, so a
    # sub-key mean could not exist and the cap was never reached.
    dark, _ = _exposure_probe(live, ["--meter-low", "0.05", "--meter-high", "0.20"],
                              frames=120)
    bright = os.path.join(workdir, "exposure_bright.cscn")
    _exposure_live(scale_src, bright, scale=EXPOSURE_SCALE)
    bright_rows, _ = _exposure_probe(bright, frames=EXPOSURE_LINEAR_FRAMES)
    if not dark or not bright_rows:
        print("  exposure-darkens ERROR no probe output")
        failures.append("exposure-darkens")
    else:
        d = dark[-1]
        dark_gain = d["gain"]
        bright_gain = bright_rows[-1]["gain"]
        # `valid` and the sub-key reading are not decoration. exposure_auto_gain
        # returns exactly 1.0 from THREE early exits -- !automatic, !adapted_valid
        # and adapted <= 0 -- so gain == 1.0 alone means "the cap engaged OR
        # nothing was measured". Written without these, the arm passes on a meter
        # whose histogram came back empty, which --meter-radius 0.0001 produces.
        measured = d["valid"] == 1.0 and d["raw_nits"] < d["key"]
        ok = measured and dark_gain == EXPOSURE_GAIN_CAP and bright_gain <= EXPOSURE_DARKEN_MAX
        print(f"  exposure-darkens {'PASS' if ok else 'FAIL'} sub-key scene gain={dark_gain:.6f} "
              f"want exactly {EXPOSURE_GAIN_CAP}; metered {d['raw_nits']:.3g} cd/m2 want "
              f"< key {d['key']} and valid={int(d['valid'])} want 1 (or the cap was never "
              f"reached); bright scene gain={bright_gain:.6f} want <={EXPOSURE_DARKEN_MAX} "
              "(it must still stop down)")
        if not ok:
            failures.append("exposure-darkens")

    # --- exposure-converge --------------------------------------------------
    rows, _ = _exposure_probe(live, frames=EXPOSURE_FRAMES)
    if len(rows) < 20:
        print("  exposure-converge ERROR too few probe lines")
        failures.append("exposure-converge")
    else:
        # Against the CLAMPED target, and both sides in log2 as the engine reports
        # them. Two traps this avoids, each of which made a converged meter look
        # broken: comparing against RAW never closes on a frame the metered bounds
        # clamp, and log2()-ing the %.6f nits field measures print precision --
        # a floor that grows as the scene darkens, reaching 2.5e-4 on this
        # group's own dark configuration, 25x the threshold.
        def gap(rec):
            return abs(rec["adapted_log2"] - rec["target_log2"])
        # Frame 0 is skipped because its gap is structurally zero -- the first
        # measurement is taken whole, with no blend to converge from.
        early = max(gap(r) for r in rows[1:8])
        late = gap(rows[-1])
        ok = late <= EXPOSURE_CONVERGE_EPS and early > EXPOSURE_CONVERGE_EPS
        print(f"  exposure-converge {'PASS' if ok else 'FAIL'} adapted-vs-target gap "
              f"{late:.2e} at frame {int(rows[-1]['frame'])} want <={EXPOSURE_CONVERGE_EPS} "
              f"(the snap); early gap {early:.2e} want >{EXPOSURE_CONVERGE_EPS} "
              "(it had somewhere to converge from)")
        if not ok:
            failures.append("exposure-converge")

    # --- exposure-stable ----------------------------------------------------
    a, _ = _exposure_probe(live, frames=EXPOSURE_STABLE_FRAMES)
    b, _ = _exposure_probe(live, frames=EXPOSURE_STABLE_FRAMES)
    if not a or len(a) != len(b):
        print(f"  exposure-stable FAIL  {len(a)} lines vs {len(b)}")
        failures.append("exposure-stable")
    else:
        # NaN compares unequal to itself, and a refused measurement is a NaN the
        # engine emits deliberately -- so a bare != would report a designed path
        # as a determinism regression.
        def same(u, v):
            return u == v or (isinstance(v, float) and math.isnan(u) and math.isnan(v))
        diffs = [k for x, y in zip(a, b) for k in x if not same(x[k], y.get(k, None))]
        ok = not diffs
        print(f"  exposure-stable {'PASS' if ok else 'FAIL'} {len(a)} frames x "
              f"{len(a[0])} fields identical across two runs"
              + ("" if ok else f"; first differing field {diffs[0]}"))
        if not ok:
            failures.append("exposure-stable")

    # --- exposure-linear ----------------------------------------------------
    one = os.path.join(workdir, "exposure_lin_1x.cscn")
    _exposure_live(scale_src, one, scale=1.0)
    lo, _ = _exposure_probe(one, frames=EXPOSURE_LINEAR_FRAMES)
    if not lo or not bright_rows:
        print("  exposure-linear ERROR no probe output")
        failures.append("exposure-linear")
    else:
        got = bright_rows[-1]["raw_log2"] - lo[-1]["raw_log2"]
        want = math.log2(EXPOSURE_SCALE)
        ok = abs(got - want) <= EXPOSURE_LINEAR_EPS
        print(f"  exposure-linear {'PASS' if ok else 'FAIL'} emitters x{EXPOSURE_SCALE:.0f} moved "
              f"the metered value {got:.4f} stops, want {want:.4f} +/-{EXPOSURE_LINEAR_EPS} "
              f"(error {got - want:+.4f}; the absolute floor this replaced reads -1.61)")
        if not ok:
            failures.append("exposure-linear")

    # --- exposure-tail ------------------------------------------------------
    keep, _ = _exposure_probe(live, ["--meter-low", "0.70", "--meter-high", "1.0"],
                              frames=EXPOSURE_FRAMES)
    cut, _ = _exposure_probe(live, ["--meter-low", "0.70", "--meter-high", "0.95"],
                             frames=EXPOSURE_FRAMES)
    if not keep or not cut:
        print("  exposure-tail  ERROR no probe output")
        failures.append("exposure-tail")
    else:
        hi, lo_v = keep[-1]["raw_log2"], cut[-1]["raw_log2"]
        drop = hi - lo_v
        live_values = min(hi, lo_v) > EXPOSURE_TAIL_FLOOR
        ok = drop >= EXPOSURE_TAIL_MIN_DROP and live_values
        print(f"  exposure-tail  {'PASS' if ok else 'FAIL'} cutting the top 5% lowered the "
              f"metered value {drop:.4f} stops want >={EXPOSURE_TAIL_MIN_DROP} "
              f"(keep {hi:.4f} -> cut {lo_v:.4f}); both above {EXPOSURE_TAIL_FLOOR} "
              f"(on live values, not the numeric guard): {live_values}")
        if not ok:
            failures.append("exposure-tail")

    # --- exposure-mask ------------------------------------------------------
    # `rows` is the default configuration at the same frame count -- the byte
    # identical command this used to issue a second time. Reused rather than
    # re-rendered, the way exposure-linear already reads exposure-darkens' bright
    # run: 150 frames is ~3 s and this group is 7% of the suite.
    uni = rows
    # 1.0 exactly, not an arbitrarily large number: the radius is a fraction of
    # the UV half-diagonal, so 1.0 reaches the corners and covers the frame by
    # definition. This arm used 8.0 while the shader divided by the radius alone
    # and coverage saturated at 0.7071 -- so it passed without ever pinning what
    # the unit meant, and any value above 0.71 would have done.
    wide, wide_text = _exposure_probe(live,
                                      ["--meter-mode", "spot", "--meter-radius", "1.0"],
                                      frames=EXPOSURE_FRAMES)
    spot, _ = _exposure_probe(live, ["--meter-mode", "spot", "--meter-radius", "0.4"],
                              frames=EXPOSURE_FRAMES)
    if not (uni and wide and spot):
        # The probe helper already collected the renderer's output; printing its
        # tail is the difference between "something went wrong" and knowing what.
        print(f"  exposure-mask  ERROR no probe output: {wide_text.strip()[-160:]}")
        failures.append("exposure-mask")
    else:
        identity = wide[-1]["raw_log2"] == uni[-1]["raw_log2"]
        moved = abs(spot[-1]["raw_log2"] - uni[-1]["raw_log2"])
        ok = identity and moved > 1e-4
        print(f"  exposure-mask  {'PASS' if ok else 'FAIL'} a frame-covering spot reads "
              f"{'identical to' if identity else 'DIFFERENT from'} uniform "
              f"({wide[-1]['raw_log2']:.6f} vs {uni[-1]['raw_log2']:.6f}, want exact); "
              f"a real spot moves {moved:.4f} stops want >0 (or the mask does nothing)")
        if not ok:
            failures.append("exposure-mask")

    return failures


# How close the tightest sweep must come to the bound before the reading counts
# as a test of it. Measured at 0.9664 on the vegetation quad and 0.9618 on the
# cloth one, so 0.85 leaves room for a grid tweak without leaving room for a
# grid that stopped sweeping. It is a floor on the INSTRUMENT, not on the
# engine: the bound getting tighter would raise this, never lower it.
CULL_BOUND_FLOOR = 0.85


def run_cull_gate(workdir):
    """Wind and skinned geometry is bounded, so it can be culled (spec 11.53).

      cull-wind       the wind quads behind the camera are rejected, exactly
      cull-cascade    the cascade culls against its OWN volume, not the camera's
      cull-sum        seen == instances + culled, on the wind fixture
      cull-margin     a wind quad outside the frustum at bind, blown back in
      cull-skin       a posed panel outside its bind bounds is still drawn
      cull-skin-away  the same rig aimed away, every mesh rejected
      cull-bound      the wind bound is one, measured against the real shader

    Both fixtures pair a MUST-CULL arm with a MUST-NOT-CULL one, because the two
    fail in opposite directions and neither is safe alone. The old exemption
    passes every "nothing visible was dropped" test trivially -- it dropped
    nothing because it culled nothing -- and a bound that is too tight passes
    every "it culls" test while deleting geometry that is on screen.

    cull-bound closes what the five above cannot: they pin the bound at ONE
    camera, one frame and one wind field, so a displacement term smaller than
    the fixture's slack, or pointing where its quads do not, leaves them all
    green. It reads --wind-bound-probe, which drives windOffset itself.
    """
    WIND = "wind_cull_fixture"
    SKIN = "skinned_cull_fixture"
    SIZE = ("400", "300")
    for name in (WIND, SKIN):
        if not os.path.exists(os.path.join(ROOT, "assets", f"{name}.cscn")):
            print(f"  cull         SKIP  (missing {name}.cscn)")
            return []

    failures = []

    # Read from the fixtures rather than mirrored here, for the reason
    # _fixture_mesh_nodes states: a hand-copied count goes stale when the
    # generator changes and the arm passes against whatever it was told.
    wind_meshes = _fixture_mesh_nodes(f"{WIND}.gltf")
    with open(os.path.join(ROOT, "assets", f"{WIND}.gltf")) as f:
        behind = sum(1 for n in json.load(f)["nodes"] if n["name"].startswith("wind_behind"))

    # Cascades pinned on the command line, not inherited: 3 is the render APP's
    # default and the library's is 1, so an app-side change would fail
    # cull-cascade with a message about culling.
    wind_run = _profiled_run(workdir, "cullwind", ["--shadow-cascades", str(SUBMIT_CASCADES)],
                             fixture=f"{WIND}.cscn", size=SIZE)
    if wind_run is None:
        return ["cull-parse"]

    # --- cull-wind: the quads behind the camera are rejected ----------------
    # Exact, not an inequality. A build that culls "some" of them is as wrong as
    # one that culls none -- and before 11.53 a wind material was exempt and the
    # answer here was 0.
    o = wind_run["submit"].get("opaque", {})
    want_drawn = wind_meshes - behind
    ok = (o.get("meshes seen") == wind_meshes and o.get("meshes culled") == behind
          and o.get("draws") == want_drawn)
    print(f"  cull-wind    {'PASS' if ok else 'FAIL'}  {o.get('meshes culled')} of "
          f"{o.get('meshes seen')} wind meshes culled in {o.get('draws')} draws "
          f"(want exactly {behind} of {wind_meshes} and {want_drawn} draws)")
    if not ok:
        failures.append("cull-wind")

    # --- cull-cascade: each pass culls against its own volume ---------------
    # The inverse arm, in the shadowcull-draws idiom. Every mesh here is inside
    # the cascade fit, so the SAME predicate that rejected the ones behind the
    # camera must reject none of them for the light. A cull wired to the wrong
    # frustum passes the arm above and fails this one.
    want_seen = wind_meshes * SUBMIT_CASCADES
    c = wind_run["submit"].get("shadow cascades", {})
    ok = c.get("meshes seen") == want_seen and c.get("meshes culled") == 0
    print(f"  cull-cascade {'PASS' if ok else 'FAIL'}  cascades saw {c.get('meshes seen')} "
          f"and culled {c.get('meshes culled')} (want {want_seen} = {wind_meshes} x "
          f"{SUBMIT_CASCADES} cascades seen and 0 culled: the fit covers the whole scene, "
          f"where the camera rejected {behind})")
    if not ok:
        failures.append("cull-cascade")

    ok, detail = _submit_sum_detail(wind_run, "wind fixture")
    print(f"  cull-sum     {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("cull-sum")

    # (ok, detail) rather than a printed verdict, for the same reason
    # _submit_sum_detail is: gate-arm-docs reads THIS function's source for the
    # arms it runs, so a name that only appears as a variable is invisible to it.
    # The shared part is the claim both arms make -- these two renders differ in
    # exactly one flag -- which is what _gpu_cmd and _timing_delta are each
    # written once for.
    def cull_identity(tag, fixture, note, measured):
        on = os.path.join(workdir, f"cull_{tag}_on.ppm")
        off = os.path.join(workdir, f"cull_{tag}_off.ppm")
        scene = os.path.join(ROOT, "assets", f"{fixture}.cscn")
        pin = ["--no-auto-exposure", "-E", "1.0"]
        err = render(scene, on, pin) or render(scene, off, pin + ["--no-frustum-cull"])
        if err:
            return False, f"render failed: {err.strip()[-200:]}"
        ae, _ = compare(on, off)
        return ae == 0, (f"{ae} px between culled and --no-frustum-cull {note} "
                         f"(want exactly 0; {measured} when the bound is wrong)")

    # The marginal quad's IMPORT AABB is entirely left of the frustum; only the
    # displacement brings it back, so a margin of zero culls something on screen.
    ok, detail = cull_identity("wind", WIND, "with a wind quad outside its bind bounds",
                               "3,114 px")
    print(f"  cull-margin  {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("cull-margin")

    # The panel's bind position is far outside the frustum and its posed one is
    # centre frame, so a bound that ignores the pose culls it.
    ok, detail = cull_identity("skin", SKIN, "with the panel swung outside its bind bounds",
                               "10,000 px")
    print(f"  cull-skin    {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("cull-skin")

    # --- cull-skin-away: and it does reject when it should ------------------
    # The presence floor for the arm above: a bound that always answered
    # "visible" would satisfy cull-skin perfectly.
    away = _profiled_run(workdir, "cullskinaway",
                         ["--cam-eye", "0,6,4", "--cam-target", "0,6,60"],
                         fixture=f"{SKIN}.cscn", size=SIZE)
    if away is None:
        failures.append("cull-skin-away")
    else:
        a = away["submit"].get("opaque", {})
        ok = (a.get("meshes seen", 0) > 0
              and a.get("meshes culled") == a.get("meshes seen")
              and a.get("draws") == 0)
        print(f"  cull-skin-away {'PASS' if ok else 'FAIL'}  aimed away: "
              f"{a.get('meshes culled')} of {a.get('meshes seen')} culled and "
              f"{a.get('draws')} draws (want all culled, 0 draws)")
        if not ok:
            failures.append("cull-skin-away")

    # --- cull-bound: the bound is one, against the shader itself ------------
    # Two claims on one reading. `measured <= bound` is correctness -- a ratio
    # over 1.0 means culling can drop geometry that is on screen. The floor is
    # the instrument checking itself: the worst case IS reachable (cloth's
    # mask*sway and the vegetation lean both hit 1), so a correct sweep lands
    # near 1.0, and a reading of 0.3 means the grid got coarser, not that the
    # bound got safer.
    #
    # max_abs, not max_l2: the margin inflates the AABB per AXIS.
    rows, _ = _probe_render(os.path.join(ROOT, "assets", f"{WIND}.cscn"),
                            "--wind-bound-probe", "wind-bound-probe", frames=2)
    head = next((r for r in rows if r.get("kind") == "header"), None)
    samples = [r for r in rows if r.get("kind") in ("mesh", "sweep")]
    if not head or head.get("available") != "1" or not samples:
        reason = head.get("reason", "no rows") if head else "no header"
        print(f"  cull-bound   FAIL  probe unavailable: {reason}")
        failures.append("cull-bound")
    else:
        ratios = [float(r["abs_ratio"]) for r in samples]
        worst = max(ratios)
        tightest = max(float(r["abs_ratio"]) for r in samples if r["kind"] == "sweep")
        ok = worst <= 1.0 and tightest >= CULL_BOUND_FLOOR
        over = [f"{r['mesh']}/{r['kind']}" for r in samples if float(r["abs_ratio"]) > 1.0]
        print(f"  cull-bound   {'PASS' if ok else 'FAIL'}  {len(samples)} readings over "
              f"{head['time_span']}s in {head['time_steps']} steps: worst measured/bound "
              f"{worst:.4f} (want <= 1.0{'; over: ' + ','.join(over) if over else ''}), "
              f"tightest sweep {tightest:.4f} (want >= {CULL_BOUND_FLOOR}, or the grid is too "
              f"coarse to be a test)")
        if not ok:
            failures.append("cull-bound")

    return failures


# --- Layered surfaces and their composite cache (specs 11.60, 11.66) ---------
#
# Every arm here reads `--render-mode 6`, the albedo view, and that is the whole
# reason the fixture is legible. In that view the shader returns
# linearToSRGB(albedoFactor * sRGBToLinear(blend)) and the fixture's factor is
# white, so a correct renderer hands back the exact byte the generator painted --
# no lighting, no exposure, no tonemap in the path to argue about. A lit read
# would be a claim about the BRDF as much as about the blend.
#
# The fixture's constants are IMPORTED rather than restated. gen_layer_fixture
# paints the ground truth, so the gate asserting the renderer reproduces it is
# not circular -- the renderer is what is under test. The lut gate declines the
# same move for the opposite reason worth keeping straight: there the generator
# owns a closed form for the ANSWER, and importing it would make ground truth a
# function of the interpolator being measured.

_LAYER_GEN = None
_LAYER_VT_GEN = None


def _import_fixture_gen(filename, group="layers"):
    """A fixture generator, imported for its constants. Writes nothing on import.

    Returns None if its dependencies are absent. The rest of gates.py is
    stdlib-only and these are its only imports of anything else -- the
    generators pull in numpy and Pillow -- so an environment without them would
    otherwise abort a seven-minute run partway through with a
    ModuleNotFoundError, after the GPU time is already spent. The caller SKIPs
    instead, like it does for a missing fixture or an unbuilt binary.

    `group` names the gate in that SKIP line. It was the literal "layers" until
    a second group wanted this, which would have had the decals gate reporting
    itself as the layers one.
    """
    path = os.path.join(ROOT, "assets", filename)
    try:
        spec = importlib.util.spec_from_file_location(filename[:-3], path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except ImportError as exc:
        print(f"  {group:<17} SKIP  ({filename[:-3]} needs {exc.name})")
        return None
    return mod


def _layer_gen():
    global _LAYER_GEN
    if _LAYER_GEN is None:
        _LAYER_GEN = _import_fixture_gen("gen_layer_fixture.py")
    return _LAYER_GEN


def _layer_vt_gen():
    global _LAYER_VT_GEN
    if _LAYER_VT_GEN is None:
        _LAYER_VT_GEN = _import_fixture_gen("gen_layer_vt_fixture.py")
    return _LAYER_VT_GEN


# How far off the truth the coarse fallback alone must read on the road's edge
# (spec 11.68) -- the anti-vacuity for the clause above it, since "pages match
# the truth" is also satisfied where the fallback matches it too and pages are
# doing nothing. Measured 18 codes of smear; the bar is 10, and a build whose
# roads never reach the atlas reads 0 here because every leg is bare ground.
ROAD_PAGES_SMEAR_MIN = 10

# One statement of the probe line's shape, shared by every consumer: the
# fixture runner and the forest walk arm read the same print, and two regexes
# would let a probe format change strand one of them silently.
_VT_PROBE = re.compile(
    r"layers-vt-probe frame=(\d+) grid=(\d+) slots=(\d+) resident=(\d+) wanted=(\d+) "
    r"requested=(\d+) loaded=(\d+) evicted=(\d+) digest=([0-9a-f]{8})")


def _vt_probe_rows(text):
    """Every --layers-vt-probe line in `text`, oldest first, as dicts."""
    return [{"frame": int(m.group(1)), "grid": int(m.group(2)), "slots": int(m.group(3)),
             "resident": int(m.group(4)), "wanted": int(m.group(5)),
             "requested": int(m.group(6)), "loaded": int(m.group(7)),
             "evicted": int(m.group(8)), "digest": m.group(9)}
            for m in _VT_PROBE.finditer(text)]


def _vt_point(u, v):
    """World point on the VT fixture at splat coordinate (u, v).

    The floor and the 45-degree ramp meet at z = 0, so one mapping serves both:
    y is -z on the ramp and 0 on the floor. v runs from the ramp's crest (0) to
    the floor's near edge (1), matching the splat rows the generator paints.
    Geometry from the VT generator, which OWNS it -- today it inherits the
    parent's HALF, but that is one editable line over there, and a scan built
    from the parent's number would silently mis-sample the day they diverge.
    """
    g2 = _layer_vt_gen()
    x = -g2.HALF + u * 2.0 * g2.HALF
    z = -g2.HALF + v * 2.0 * g2.HALF
    return (x, max(0.0, -z), z)


def _layer_floor(u, v):
    """World point on the floor plate at splat coordinate (u, v)."""
    g = _layer_gen()
    return (-g.HALF + u * 2.0 * g.HALF, 0.0, g.HALF - v * 2.0 * g.HALF)


def _layer_wall(u, v):
    """World point on the wall plate at splat coordinate (u, v)."""
    g = _layer_gen()
    return (-g.HALF + u * 2.0 * g.HALF, v * g.WALL_H, -g.HALF)


def _layer_sample(frame, world):
    """The raw sRGB byte at a world point -- the painted code, in the albedo view.

    RAISES for a point that projects off-screen rather than clamping to the
    border. A clamp returns a number, and a number is what an arm reads: a
    fixture whose framing has drifted would go on passing against whatever pixel
    happened to sit at the edge.
    """
    w, h, pix, project = frame
    px, py = project(world)
    x, y = int(round(px)), int(round(py))
    if x < 0 or y < 0 or x >= w or y >= h:
        raise ValueError(f"layer sample at {world} projects to ({x}, {y}), outside {w}x{h}")
    o = (y * w + x) * 3
    return (pix[o] + pix[o + 1] + pix[o + 2]) / 3.0


def _layer_scan(frame, points):
    """Mean byte at each of a list of world points."""
    return [_layer_sample(frame, p) for p in points]


def _layer_transitions(vals, lo, hi):
    """Count crossings of the midpoint between lo and hi, with hysteresis.

    Hysteresis rather than a bare threshold because a mip-softened checker edge
    wanders either side of the midpoint over a pixel or two, and a bare compare
    would count each wobble as a fresh cell.
    """
    mid = 0.5 * (lo + hi)
    band = 0.2 * (hi - lo)
    state = None
    count = 0
    for v in vals:
        if v > mid + band:
            new = True
        elif v < mid - band:
            new = False
        else:
            continue
        if state is not None and new != state:
            count += 1
        state = new
    return count


def _layer_ramp(vals, positions, lo, hi):
    """(10-90 width, 50% crossover position) for a monotone ramp from lo to hi.

    The CROSSOVER is the half that matters and the first version of this did not
    return it. The 10-90 width of a height blend is 0.889 x LAYER_BLEND_RANGE and
    is algebraically independent of the layer heights -- so an arm reading only
    the width is measuring one shader constant, and deleting the height term
    entirely leaves it unchanged. The height decides WHERE the two layers trade
    places, not how fast.

    Returns None if the scan is not monotone, rather than measuring it anyway: a
    single sample the wrong side of a threshold inside a plateau otherwise
    returns a spuriously narrow or wide width and passes silently.
    """
    drops = sum(1 for i in range(1, len(vals)) if vals[i] < vals[i - 1] - 8.0)
    if drops:
        return None

    def crossing(frac):
        t = lo + frac * (hi - lo)
        for i in range(1, len(vals)):
            a, b = vals[i - 1], vals[i]
            if (a - t) * (b - t) <= 0.0 and a != b:
                k = (t - a) / (b - a)
                return positions[i - 1] + k * (positions[i] - positions[i - 1])
        return None

    first, mid, last = crossing(0.1), crossing(0.5), crossing(0.9)
    if first is None or mid is None or last is None:
        return None
    return (last - first, mid)


def run_layers_gate(workdir):
    """Layered surfaces: the per-texel blend, and the composite cache over it.

    The arms, in the order they run. Keep this list and the code in step: a
    docstring naming a different set than the function runs is what a reviewer
    reads to decide what is covered. (The scatter arms live in a helper and are
    therefore invisible to gate-arm-docs; they run last, after everything below:
    layers-scatter, then layers-road-scatter off the same forest run.)

      layers-select      each splat column resolves the layer it selects
      layers-srgb        a mid-grey layer round-trips through the decode
      layers-height      the height blend interlocks where linear smears
      layers-triplanar   the wall is textured by its own axis, at the right size
      layers-vt-identity the cached path reproduces the per-texel blend where the
                         atlas resolves the macro, and a forced-coarse atlas
                         moves the frame -- which is what proves the flag arms a
                         real path at all
      layers-vt-detail   checker crossings at full grain density through the
                         cache, on the floor and on the 45-degree ramp
      layers-vt-macro    at an atlas texel WIDER than a checker period the
                         selection still reads exact painted bytes while the
                         grain still crosses -- the one arm a plain baked atlas
                         (the roadmap's original stage 1) fails
      layers-vt-invalidate a mid-run layerBlend change re-bakes the cache: the
                         crossover moves to the linear 0.5, which a stale atlas
                         cannot show
      layers-vt-budget   the bake's own MB log equals the gate's closed form at
                         the gate's own derived resolution
      layers-vt-pages-identity pages on vs off at the derived resolution is
                         splat-edge resample only, AND the probe proves pages
                         went resident -- the pixel half alone passes a build
                         whose pages never arm
      layers-vt-pages-effect at a forced-coarse fallback, pages restore most of
                         what the coarseness lost against the full reference
      layers-vt-pages-churn four slots against a 25-page grid plus a --cam-at
                         teleport: capacity holds, the teleport evicts and
                         reloads, the tail is stable, two runs are identical
      layers-vt-pages-walk forest's 400-frame region walk churns pages under
                         the capacity clamp, on the design's own 34-page grid
      layers-vt-feedback-sky aimed at the sky the vote pass requests zero pages
                         while prediction still wants some
      layers-vt-feedback-occlusion behind the ramp only the ramp's pages vote;
                         the frustum-visible hidden floor casts none -- the arm
                         that discriminates feedback from prediction
      layers-road-select a road reads its own layer's painted byte through the
                         cache, over two different base layers, and leaves the
                         ground past its shoulder untouched; the per-texel path
                         reads the same bytes
      layers-road-edge   the shoulder crossover sits where the HEIGHTS put it and
                         collapses to a narrow band, against a linear twin that
                         crosses at the mask's own half -- the arm that proves
                         the override happens BEFORE the height blend
      layers-road-pages  with a shoulder finer than a fallback texel, pages on vs
                         off differ and pages land nearer the per-texel truth:
                         the first content that is not a 0 px page identity
      layers-road-invalidate a mid-run --road-width-at re-bakes the cache, so a
                         point outside the bake-time shoulder becomes road

    WHAT THE FIRST FOUR ARMS CANNOT SEE: layer_fixture's splat is UV1-space, and
    the composite cache serves world-XZ splats only, so those arms cover the
    per-texel path alone -- which stays live (walls, props, UV1 splats) and
    needs the coverage. The cache's coverage is the vt arms, on their own
    world-XZ fixture.
    """
    failures = []
    src = os.path.join(ROOT, "assets", "layer_fixture.cscn")
    if not os.path.exists(src):
        print("  layers-select     SKIP  (missing layer_fixture.cscn)")
        return []
    g = _layer_gen()
    if g is None:
        return []

    ALBEDO = ["--render-mode", "6", "--no-auto-exposure", "-E", "1.0"]

    def shot(name, overrides=None, extra=None):
        out = os.path.join(workdir, f"layers_{name}.ppm")
        scene = src
        if overrides:
            scene = os.path.join(workdir, f"layers_{name}.cscn")
            cscn_copy(src, scene, overrides)
        err = render(scene, out, ALBEDO + (extra or []), frames=4)
        if err:
            return None
        w, h, pix = _read_ppm(out)
        # From the COMMITTED scene file, which is what was rendered. Reading the
        # generator's in-memory dict instead would let the gate keep passing
        # while predicting a different scene than it measured.
        return w, h, pix, _projector(_cscn_camera("layer_fixture.cscn"), w, h)

    base = shot("base")
    if base is None:
        print("  layers-select     ERROR while rendering the fixture")
        return ["layers-select"]
    w, h, pix, project = base

    # --- layers-select -------------------------------------------------------
    # The three FLAT columns, read at the centre of each so no bilinear bleed
    # from a neighbouring column reaches the sample. Column 1 is the checker and
    # is not a flat colour; the triplanar arm is what verifies it got selected.
    codes = {0: g.GREY_CODE, 2: g.DARK_CODE, 3: g.LIGHT_CODE}
    LAYER_SELECT_TOL = 4.0
    readings = []
    for col, expect in sorted(codes.items()):
        u = (col + 0.5) / 4.0
        got = _layer_sample(base, _layer_floor(u, 0.25))
        readings.append((col, expect, got))
    worst = max(abs(got - expect) for _, expect, got in readings)
    ok = worst <= LAYER_SELECT_TOL
    detail = ", ".join(f"col{c} {got:.0f} (want {e})" for c, e, got in readings)
    print(f"  layers-select     {'PASS' if ok else 'FAIL'}  {detail}; worst off by "
          f"{worst:.1f} (want <= {LAYER_SELECT_TOL})")
    if not ok:
        failures.append("layers-select")

    # --- layers-srgb ---------------------------------------------------------
    # TWO codes, on the WALL. The first version of this read the same pixel as
    # layers-select's column 0, so nothing could fail it without also failing
    # that -- four arms wearing five names. Two points are also what separates a
    # curve from a line: a decode error that happens to be affine reproduces one
    # sample and cannot reproduce two. The wall additionally routes the read
    # through the Z projection, which no other known-code read touches.
    srgb_reads = [(0.125, g.GREY_CODE), (0.625, g.DARK_CODE)]
    got = [(_layer_sample(base, _layer_wall(u, 0.25)), want) for u, want in srgb_reads]
    worst_srgb = max(abs(a - b) for a, b in got)
    ok = worst_srgb <= 3.0
    print(f"  layers-srgb       {'PASS' if ok else 'FAIL'}  wall reads "
          + ", ".join(f"{a:.0f} (want {b})" for a, b in got)
          + f"; worst off by {worst_srgb:.1f} (want <= 3. A doubled decode reads ~56 for "
            f"128, a missing one ~187)")
    if not ok:
        failures.append("layers-srgb")

    # --- layers-height -------------------------------------------------------
    # The wall's upper band ramps layer 2 into layer 3 across the full width. The
    # dark layer stands proud, so a height blend holds it well past the halfway
    # point and then gives way over a few texels; a linear blend crosses over
    # gradually across the whole ramp. Measured as the 10%-to-90% width, and the
    # linear leg is rendered rather than predicted so the two share everything
    # except the one authored knob.
    SCAN = 140
    us = [0.02 + i * (0.96 / (SCAN - 1)) for i in range(SCAN)]
    line = [_layer_wall(u, 0.78) for u in us]

    def ramp(frame):
        vals = _layer_scan(frame, line)
        return _layer_ramp(vals, us, g.DARK_CODE, g.LIGHT_CODE)

    # Closed form: the two layers trade places where their weights plus their
    # scaled heights meet, i.e. at (1 + (dark - light)/255 * sharpness) / 2. The
    # linear leg has no height term at all and crosses at exactly 0.5.
    expect_mid = (1.0 + (g.DARK_HEIGHT - g.LIGHT_HEIGHT) / 255.0 * g.LAYER_BLEND_SHARPNESS) / 2.0
    linear = shot("linear", lambda d: d["materials"]["layered_surface"].update(
        {"layerBlend": 0.0}))
    if linear is None:
        print("  layers-height     ERROR while rendering the linear leg")
        failures.append("layers-height")
    else:
        rh, rl = ramp(base), ramp(linear)
        if rh is None or rl is None:
            print("  layers-height     FAIL  the ramp is not a measurable monotone transition")
            failures.append("layers-height")
        else:
            w_height, mid_height = rh
            w_linear, mid_linear = rl
            ok = (abs(mid_height - expect_mid) <= 0.03 and abs(mid_linear - 0.5) <= 0.03
                  and w_height / w_linear <= 0.35)
            print(f"  layers-height     {'PASS' if ok else 'FAIL'}  crossover at "
                  f"{mid_height:.3f} (want {expect_mid:.3f} +/- 0.03; the linear leg reads "
                  f"{mid_linear:.3f}, want 0.500) -- the crossover is what the HEIGHTS "
                  f"decide, where the width {w_height:.3f} vs {w_linear:.3f} only reads "
                  f"LAYER_BLEND_RANGE")
            if not ok:
                failures.append("layers-height")

    # --- layers-triplanar ----------------------------------------------------
    # The wall's normal is +Z, so only the Z projection can give it structure
    # along y. A projection frozen on Y reads the wall's constant z as its second
    # coordinate and returns a wall that varies with x alone -- which is why the
    # count here is taken VERTICALLY, and why the fixture's layer 1 is a checker
    # rather than stripes.
    # The SAME world length on both plates, or the two counts are not comparable.
    # The wall's v spans WALL_H and the floor's spans 2*HALF, so the fractions
    # differ; the span in world units is what has to match.
    SPAN = 1.5
    col_u = 0.375  # centre of the checker column
    N = 96
    wall_v0, wall_dv = 0.06, SPAN / g.WALL_H
    floor_v0, floor_dv = 0.06, SPAN / (2.0 * g.HALF)
    wall_vals = _layer_scan(
        base, [_layer_wall(col_u, wall_v0 + wall_dv * i / (N - 1)) for i in range(N)])
    floor_vals = _layer_scan(
        base, [_layer_floor(col_u, floor_v0 + floor_dv * i / (N - 1)) for i in range(N)])
    crossings = _layer_transitions(wall_vals, g.STRIPE_DARK, g.STRIPE_LIGHT)
    floor_cross = _layer_transitions(floor_vals, g.STRIPE_DARK, g.STRIPE_LIGHT)

    span = SPAN
    # CROSSINGS, not cells: a scan of n cells crosses between them n-1 times, and
    # where the scan starts relative to a cell boundary moves that by one either
    # way. Comparing crossings against a cell count is what left this arm's lower
    # bound below the value a correct renderer produces.
    expected = span / g.CHECKER_CELL_WORLD - 1.0
    floor_range = max(floor_vals) - min(floor_vals)

    # Two-sided, and counted on BOTH plates. A lower bound alone passes a
    # projection that has dropped layerUvScale (cells half the size, twice the
    # count) and one frozen on X rather than Y (same count, wrong axis). What the
    # feature actually claims is that ONE texture appears at ONE size on two
    # perpendicular surfaces, so the two counts have to agree with each other and
    # with the closed form.
    lo, hi = expected - 1.5, expected + 1.5
    ok = (lo <= crossings <= hi and lo <= floor_cross <= hi
          and abs(crossings - floor_cross) <= 1 and floor_range >= 100.0)
    print(f"  layers-triplanar  {'PASS' if ok else 'FAIL'}  wall {crossings} / floor "
          f"{floor_cross} checker crossings over {span:.2f} units (want each in "
          f"[{lo:.1f}, {hi:.1f}] and within 1 of each other; a projection frozen on Y "
          f"reads 0 on the wall, a dropped uvScale roughly doubles both), floor range "
          f"{floor_range:.0f} (want >= 100)")
    if not ok:
        failures.append("layers-triplanar")

    # --- the composite cache (spec 11.66) ------------------------------------
    # Its own fixture, because this one's splat is UV1-space and the cache
    # serves world-XZ splats only -- no arm on layer_fixture can see the cache
    # in either direction. Inline rather than in a helper: gate-arm-docs reads
    # this function's own source, and a helper's arms would read as documented
    # but never run.
    vt_src = os.path.join(ROOT, "assets", "layer_vt_fixture.cscn")
    g2 = _layer_vt_gen()
    if not os.path.exists(vt_src) or g2 is None:
        if g2 is not None:
            print("  layers-vt-identity SKIP  (missing layer_vt_fixture.cscn)")
        failures += _layers_scatter_arm()
        return failures

    def vt_shot(name, extra=None, frames=8, overrides=None):
        # 8, derived: two frames of texture uploads, one for the material
        # array, one for the bake that samples it, and margin.
        out = os.path.join(workdir, f"layers_vt_{name}.ppm")
        # The committed fixture is ROAD-FREE and stays that way: its golden and
        # every arm above render it unmutated, so a road arm builds its variant
        # here -- the sibling shot() idiom. The camera still comes from the
        # committed file, because no mutation touches it.
        scene = vt_src
        if overrides:
            scene = os.path.join(workdir, f"layers_vt_{name}.cscn")
            cscn_copy(vt_src, scene, overrides)
        err = render(scene, out, ALBEDO + (extra or []), frames=frames)
        if err:
            return None
        w2, h2, pix2 = _read_ppm(out)
        return (w2, h2, pix2, _projector(_cscn_camera("layer_vt_fixture.cscn"), w2, h2)), out

    cached = vt_shot("cached")
    plain = vt_shot("plain", ["--no-layers-vt"])
    # Pages OFF on this leg deliberately: it measures the stage-1 FALLBACK's
    # coarseness (the anti-vacuity that the res flag arms a real path), and
    # pages restoring sharpness would eat most of its floor. The vt-pages arms
    # below measure the pages against their own shots.
    coarse = vt_shot("coarse",
                     ["--layers-vt-res", str(g2.VT_MACRO_RES), "--no-layers-vt-pages"])
    if not (cached and plain and coarse):
        print("  layers-vt-identity ERROR while rendering the cache legs")
        failures.append("layers-vt-identity")
        failures += _layers_scatter_arm()
        return failures
    (cf, cpath), (pf, ppath), (of, opath) = cached, plain, coarse

    # --- layers-vt-identity --------------------------------------------------
    # Three claims in one A/B/C: the cached path reads the exact painted byte on
    # every flat column AND matches the per-texel frame away from splat-resample
    # edges and the blend crossover; and the forced-coarse atlas moves the frame
    # against per-texel, which is the anti-vacuity -- a build whose flag arms
    # nothing reads 0 there and fails, where the identity half alone would pass
    # it. Floor measured first: this fixture is 0 px across two runs of one
    # build, so these are hard numbers, not tolerances.
    def vt_columns_off(frame):
        # Worst deviation from the painted byte over the flat selection columns
        # (`codes`, the select arm's own table -- the vt fixture paints the same
        # column band).
        return max(abs(_layer_sample(frame, _vt_point((col + 0.5) / 4.0, 0.875)) - expect)
                   for col, expect in codes.items())

    worst_pt = max(vt_columns_off(cf), vt_columns_off(pf))
    ae_id, _ = compare(cpath, ppath)
    ae_coarse, _ = compare(opath, ppath)
    VT_ID_CEILING = 6000     # measured 2330 of 480000: edges + the crossover patch
    VT_COARSE_FLOOR = 10000  # measured 32161; 0 = the flag switched nothing
    ok = worst_pt <= 1.0 and ae_id <= VT_ID_CEILING and ae_coarse >= VT_COARSE_FLOOR
    print(f"  layers-vt-identity {'PASS' if ok else 'FAIL'}  flat columns off by "
          f"{worst_pt:.1f} (want <= 1), cached vs per-texel {ae_id} px (want <= "
          f"{VT_ID_CEILING}: splat-resample edges and the blend crossover only), coarse vs "
          f"per-texel {ae_coarse} px (want >= {VT_COARSE_FLOOR}; 0 = the flag arms nothing)")
    if not ok:
        failures.append("layers-vt-identity")

    # --- layers-vt-detail ----------------------------------------------------
    # Checker crossings through the CACHED path, on the floor's checker column
    # (a v-scan inside column 1) and across the ramp (a u-scan) -- the ramp is
    # the two-projection surface, which is what the per-texel path's triplanar
    # arm covers and a Y-only cache would lose. Counted the layers-triplanar
    # way: crossings, two-sided, against the cell size the generator derives.
    cell = g.CHECKER_CELL_WORLD
    NV = 96
    fv0, fv1 = 0.76, 0.99
    floor_pts = [_vt_point(0.375, fv0 + (fv1 - fv0) * i / (NV - 1)) for i in range(NV)]
    floor_span = (fv1 - fv0) * 2.0 * g2.HALF
    ramp_us = [0.02 + 0.96 * i / (NV - 1) for i in range(NV)]
    ramp_pts = [_vt_point(uu, 0.25) for uu in ramp_us]
    ramp_span = 0.96 * 2.0 * g2.HALF

    fcross = _layer_transitions(_layer_scan(cf, floor_pts), g.STRIPE_DARK, g.STRIPE_LIGHT)
    rcross = _layer_transitions(_layer_scan(cf, ramp_pts), g.STRIPE_DARK, g.STRIPE_LIGHT)
    f_expect = floor_span / cell - 1.0
    r_expect = ramp_span / cell - 1.0
    ok = abs(fcross - f_expect) <= 1.5 and abs(rcross - r_expect) <= 2.5
    print(f"  layers-vt-detail   {'PASS' if ok else 'FAIL'}  floor {fcross} crossings over "
          f"{floor_span:.2f} units (want {f_expect:.1f} +/- 1.5), ramp {rcross} over "
          f"{ramp_span:.2f} (want {r_expect:.1f} +/- 2.5; a dropped dominant index reads "
          f"layer 0's flat grey and crosses nowhere)")
    if not ok:
        failures.append("layers-vt-detail")

    # --- layers-vt-macro -----------------------------------------------------
    # The design's central claim, at a resolution where it is legible: the
    # forced atlas texel is WIDER than a checker period (generator-asserted), so
    # nothing grain-shaped can live in the cache -- yet the selection columns
    # still read their exact painted bytes (the macro survives) and the ramp
    # checker still crosses at full density (the grain arrives via the detail
    # term). A plain baked atlas -- the roadmap's original stage 1 -- holds the
    # first and fails the second.
    texel = 2.0 * g2.HALF / g2.VT_MACRO_RES
    coarse_worst = vt_columns_off(of)
    coarse_rcross = _layer_transitions(_layer_scan(of, ramp_pts), g.STRIPE_DARK, g.STRIPE_LIGHT)
    ok = coarse_worst <= 1.0 and abs(coarse_rcross - r_expect) <= 2.5
    print(f"  layers-vt-macro    {'PASS' if ok else 'FAIL'}  at {texel:.2f} units/texel "
          f"(a checker cell is {cell:.2f}) the columns read off by {coarse_worst:.1f} "
          f"(want <= 1) and the ramp still crosses {coarse_rcross} times (want "
          f"{r_expect:.1f} +/- 2.5)")
    if not ok:
        failures.append("layers-vt-macro")

    # --- layers-vt-invalidate ------------------------------------------------
    # A fresh process always bakes from the final authored values, so the
    # by-value key is unreachable without a mid-run transition; --layer-blend-at
    # is that transition. The crossover through the RE-BAKED cache must land on
    # the linear 0.5 where the startup bake put it at the height-blend position
    # -- a stale atlas keeps reading the latter. Known blind spot: this proves
    # the FRAME went fresh, not that the re-baked cache is what rendered it --
    # an implementation whose "invalidation" is falling back to per-texel
    # forever reads 0.5 here too, and only the startup coarse leg above pins
    # the cached path as live.
    #
    # 16 frames: the startup bake's 8, the transition at 10, one more ensure to
    # re-bake, and margin. The scan reuses the height arm's `us` and its
    # `expect_mid` closed form -- same band, same heights, one statement.
    trans = vt_shot("blendtrans", ["--layer-blend-at", "10:0.0"], frames=16)

    def vt_band_ramp(frame):
        vals = _layer_scan(frame, [_vt_point(uu, 0.625) for uu in us])
        return _layer_ramp(vals, us, g.DARK_CODE, g.LIGHT_CODE)

    if trans is None:
        print("  layers-vt-invalidate ERROR while rendering the transition leg")
        failures.append("layers-vt-invalidate")
    else:
        rh2 = vt_band_ramp(cf)
        rl2 = vt_band_ramp(trans[0])
        if rh2 is None or rl2 is None:
            print("  layers-vt-invalidate FAIL  the transition band is not measurable")
            failures.append("layers-vt-invalidate")
        else:
            ok = abs(rh2[1] - expect_mid) <= 0.03 and abs(rl2[1] - 0.5) <= 0.03
            print(f"  layers-vt-invalidate {'PASS' if ok else 'FAIL'}  crossover "
                  f"{rh2[1]:.3f} at bake (want {expect_mid:.3f} +/- 0.03), then "
                  f"{rl2[1]:.3f} after a frame-10 layerBlend 0 (want 0.500; a stale "
                  f"cache keeps reading {expect_mid:.3f})")
            if not ok:
                failures.append("layers-vt-invalidate")

    # --- layers-vt-budget ----------------------------------------------------
    # The bake's own MB line against the gate's OWN closed form -- the
    # layers-scatter lesson: a bar read from the process under test moves with
    # the thing it checks. The derived-resolution rule is restated here for the
    # same reason, seeded from the vt generator's floor so the two gate-side
    # restatements cannot drift apart silently. The closed-form equality is the
    # whole bar: the loop caps the expected res, so any leak past it fails the
    # equality before it could fail a ceiling.
    r = subprocess.run([RENDER, "-m", vt_src, "-x", "-f", "8", "-W", "200", "-H", "150"],
                       capture_output=True, text=True)
    m = re.search(r"Layers VT: material '[^']+' composite (\d+)x(\d+) pair, ([\d.]+) MB",
                  r.stdout + r.stderr)
    if not m:
        tail = (r.stderr or r.stdout).strip().splitlines()
        last = tail[-1][:90] if tail else "no output"
        print(f"  layers-vt-budget   ERROR  the run printed no bake line "
              f"(exited {r.returncode}: {last})")
        failures.append("layers-vt-budget")
    else:
        res_x, res_y, mb = int(m.group(1)), int(m.group(2)), float(m.group(3))
        domain = 2.0 * g2.HALF
        er = g2.VT_DERIVED_RES_MIN
        while er < domain / 0.5 and er < 2048:
            er *= 2
        closed = round(2.0 * er * er * 4.0 * (4.0 / 3.0) / (1024.0 * 1024.0), 1)
        ok = res_x == er and res_y == er and abs(mb - closed) <= 0.05
        print(f"  layers-vt-budget   {'PASS' if ok else 'FAIL'}  {res_x}x{res_y} at {mb} MB "
              f"(want {er} squared at {closed} MB by the gate's own 2*N^2*4*(4/3))")
        if not ok:
            failures.append("layers-vt-budget")

    # --- the paged near field (spec 11.67) -----------------------------------
    # Pages hold the SAME macro at 4x the fallback's density, so at the
    # fixture's derived resolution they resolve almost nothing (the identity)
    # and at a forced-coarse fallback they restore what it lost (the effect).
    # Residency is read through --layers-vt-probe, whose whole history is a
    # pure function of the flags -- measured bit-identical across runs.
    nopages = vt_shot("nopages", ["--no-layers-vt-pages"])
    coarse_pages = vt_shot("coarsepages", ["--layers-vt-res", str(g2.VT_MACRO_RES)])
    if not (nopages and coarse_pages):
        print("  layers-vt-pages-identity ERROR while rendering the page legs")
        failures.append("layers-vt-pages-identity")
        failures += _layers_scatter_arm()
        return failures

    def vt_probe_run(extra, frames, overrides=None, name="probe"):
        # The group's own 400x300 and ALBEDO framing, not a smaller size: the
        # occlusion arm's numbers were measured here, and the depth-off leak it
        # exists to catch did not reproduce at 200x150. Probe interval 4 against
        # 24 frames: six rows, and the last sits past the teleport frame plus
        # the 4-deep feedback ring plus several budget-2 bake frames, so every
        # arm reads a settled tail rather than a transient.
        scene = vt_src
        if overrides:
            # Keyed like vt_shot's, so a second probe variant cannot overwrite
            # the first and --keep leaves both to look at.
            scene = os.path.join(workdir, f"layers_vt_{name}.cscn")
            cscn_copy(vt_src, scene, overrides)
        r = subprocess.run([RENDER, "-m", scene, "-x", "-f", str(frames), "-W", "400",
                            "-H", "300"] + ALBEDO + ["--layers-vt-probe", "4"] + extra,
                           capture_output=True, text=True)
        return _vt_probe_rows(r.stdout + r.stderr)

    # --- layers-vt-pages-identity --------------------------------------------
    # Pages on vs off at the derived resolution: the pages resample the same
    # macro, so the difference is confined to sub-code resample of splat edges.
    # The anti-vacuity is the probe, not the pixels -- a build whose pages never
    # arm reads 0 px here and PASSES the ceiling, so the arm also demands
    # residency happened at all.
    ae_pages, _ = compare(cpath, nopages[1])
    probe = vt_probe_run([], 16)
    # Measured 696 of 480000: splat-edge resample only. Which page set sits
    # resident at the capture frame shifts with the vote decode (the tie-riding
    # encoding the review replaced read 490 here), so small moves inside the
    # ceiling track residency order, not blend arithmetic.
    VT_PAGES_ID_CEILING = 2000
    if not probe:
        print("  layers-vt-pages-identity FAIL  the probe printed no rows "
              "(a crash or a silenced probe, not a residency reading)")
        failures.append("layers-vt-pages-identity")
    else:
        resident_final = probe[-1]["resident"]
        ok = ae_pages <= VT_PAGES_ID_CEILING and resident_final > 0
        print(f"  layers-vt-pages-identity {'PASS' if ok else 'FAIL'}  pages on vs off "
              f"{ae_pages} px (want <= {VT_PAGES_ID_CEILING}: the pages hold the same macro), "
              f"{resident_final} pages resident at the end (want > 0, or the identity is the "
              f"feature never arming)")
        if not ok:
            failures.append("layers-vt-pages-identity")

    # --- layers-vt-pages-effect ----------------------------------------------
    # At a fallback the derived rule would refuse, pages carry the macro the
    # fallback cannot: coarse+pages must move far from coarse-only AND land
    # much nearer the full-density reference. Density stated in the fixture's
    # own units: the coarse fallback is 0.5 units/texel, its pages 0.125.
    ae_restore, _ = compare(coarse_pages[1], opath)
    ae_left, _ = compare(coarse_pages[1], nopages[1])
    ae_coarse_ref, _ = compare(opath, nopages[1])
    ok = (ae_restore >= 15000 and ae_coarse_ref > 0
          and ae_left <= int(0.6 * ae_coarse_ref))
    print(f"  layers-vt-pages-effect {'PASS' if ok else 'FAIL'}  coarse+pages moves "
          f"{ae_restore} px off coarse-only (want >= 15000; measured 32289) and sits "
          f"{ae_left} px from the full reference against coarse-only's {ae_coarse_ref} "
          f"(want <= 0.6 of it: pages recover most of what coarseness lost)")
    if not ok:
        failures.append("layers-vt-pages-effect")

    # --- layers-vt-pages-churn -----------------------------------------------
    # Slots forced far under the 25-page virtual grid, plus a --cam-at teleport
    # -- the worst case no walk can produce, every page missing at once. The
    # capacity clamp must hold at every print, the teleport must evict and
    # reload, the tail must be STABLE (loaded stops growing: the anti-thrash),
    # and the whole history must be deterministic across two runs.
    # Feedback OFF here, deliberately: this arm tests the eviction machinery in
    # isolation, and the seen-first boost makes residency smarter than the
    # arm's forcing -- after the teleport the still-visible old pages vote,
    # stay top-ranked, and correctly nothing moves. Feedback's own behaviour
    # has its two arms below.
    # The teleport pose drops the camera low over the fixture's left half,
    # looking along it -- chosen so its nearest-first want only partially
    # overlaps the default framing's, which at 4 slots is what forces both
    # evictions and reloads rather than a want that happens to be resident.
    churn_flags = ["--layers-vt-page-slots", "4", "--no-layers-vt-feedback", "--cam-at",
                   "10:-1.5,2.0,3.0,-1.5,0.0,-1.0"]
    rows_a = vt_probe_run(churn_flags, 24)
    rows_b = vt_probe_run(churn_flags, 24)
    if len(rows_a) < 4:
        print("  layers-vt-pages-churn FAIL  the probe printed too few rows")
        failures.append("layers-vt-pages-churn")
    else:
        cap_ok = all(r["resident"] <= r["slots"] for r in rows_a)
        # loaded > slots is what proves a slot was REFILLED: at cap 4, a fifth
        # load has nowhere to land without an eviction first.
        churned = rows_a[-1]["evicted"] >= 1 and rows_a[-1]["loaded"] > rows_a[-1]["slots"]
        stable = rows_a[-1]["loaded"] == rows_a[-2]["loaded"]
        deterministic = rows_a == rows_b
        ok = cap_ok and churned and stable and deterministic
        print(f"  layers-vt-pages-churn {'PASS' if ok else 'FAIL'}  resident <= slots at "
              f"every print: {cap_ok}; teleport evicted {rows_a[-1]['evicted']} and drove "
              f"loads to {rows_a[-1]['loaded']} against {rows_a[-1]['slots']} slots (want "
              f"churn); tail stable: {stable} (loads still growing = thrash); two runs "
              f"identical: {deterministic}")
        if not ok:
            failures.append("layers-vt-pages-churn")

    # --- layers-vt-pages-walk ------------------------------------------------
    # The integration read, on forest: the derived grid must be the design's own
    # bound (34, from ceil(4 * 2048 / usable) -- a number the gate states, never
    # reads back), residency must follow a real walk under the capacity clamp,
    # and the walk must churn pages the way it churns regions.
    if not os.path.exists(FOREST):
        print("  layers-vt-pages-walk SKIP  (forest not built)")
    else:
        # The regions' own churn walk, through the regions' own runner: same
        # constants, same launch, so this arm cannot drift from the walk the
        # region arms measure -- the probe line just rides along, and the
        # region probe's own output is ignored here by prefix.
        _, _, walk_text = _region_run(REGION_CHURN + REGION_WALK +
                                      ["--layers-vt-probe", "50"])
        rows = _vt_probe_rows(walk_text)
        if len(rows) < 4:
            print("  layers-vt-pages-walk FAIL  the probe printed too few rows")
            failures.append("layers-vt-pages-walk")
        else:
            grid_ok = all(r2["grid"] == 34 for r2 in rows)
            cap_ok = all(r2["resident"] <= r2["slots"] for r2 in rows)
            churned = rows[-1]["evicted"] > 0 and rows[-1]["loaded"] > rows[-1]["slots"]
            # The anti-thrash bound, TWO-SIDED because a walk both churns and
            # settles: healthy measures 79 loads over the round trip (71 on the
            # pre-review vote encoding -- the seen boost follows the decode);
            # evicting wanted pages (the hysteresis-and-want guard deleted)
            # measured 701, a 10x contrast the decode shift cannot blur.
            no_thrash = rows[-1]["loaded"] <= 200
            ok = grid_ok and cap_ok and churned and no_thrash
            print(f"  layers-vt-pages-walk {'PASS' if ok else 'FAIL'}  grid 34 at every "
                  f"print: {grid_ok} (the relative-density bound); resident <= slots: "
                  f"{cap_ok}; the walk loaded {rows[-1]['loaded']} and evicted "
                  f"{rows[-1]['evicted']} against {rows[-1]['slots']} slots (want real "
                  f"churn, as the walk churns regions, and loads <= 200 -- the "
                  f"want-guard deleted measures 701, a 10x thrash)")
            if not ok:
                failures.append("layers-vt-pages-walk")

    # --- layers-vt-feedback-sky ----------------------------------------------
    # A camera aimed at the sky rasterizes no paged surface, so feedback must
    # request ZERO pages -- while prediction's tall conservative frustum boxes
    # still WANT pages, which is the anti-vacuity: zero against a zero want
    # would also describe a pass that never ran.
    sky_rows = vt_probe_run(["--cam-eye", "0,5,8", "--cam-target", "0,60,8"], 24)
    if len(sky_rows) < 3:
        print("  layers-vt-feedback-sky FAIL  the probe printed too few rows")
        failures.append("layers-vt-feedback-sky")
    else:
        ok = sky_rows[-1]["requested"] == 0 and sky_rows[-1]["wanted"] > 0
        print(f"  layers-vt-feedback-sky {'PASS' if ok else 'FAIL'}  aimed at the sky the "
              f"vote pass requested {sky_rows[-1]['requested']} pages (want exactly 0) while "
              f"prediction wanted {sky_rows[-1]['wanted']} (want > 0, or the pass never ran "
              f"and zero proves nothing)")
        if not ok:
            failures.append("layers-vt-feedback-sky")

    # --- layers-vt-feedback-occlusion ----------------------------------------
    # The arm that discriminates feedback from prediction: a low camera behind
    # the 45-degree ramp sees the ramp and NOT the floor beyond its crest, so
    # the floor's pages must cast no votes while the frustum wants all 25 --
    # the fixture's WHOLE virtual grid, ceil(4 * 256 / 248) = 5 per axis at its
    # derived-minimum 256 fallback, squared. The band is two-sided: below it
    # the pass lost the ramp too, above it the depth test stopped rejecting
    # the hidden floor.
    occ_rows = vt_probe_run(["--cam-eye", "0,0.4,-3.5", "--cam-target", "0,0.5,3"], 24)
    if len(occ_rows) < 3:
        print("  layers-vt-feedback-occlusion FAIL  the probe printed too few rows")
        failures.append("layers-vt-feedback-occlusion")
    else:
        req = occ_rows[-1]["requested"]
        ok = 8 <= req <= 14 and occ_rows[-1]["wanted"] == 25
        print(f"  layers-vt-feedback-occlusion {'PASS' if ok else 'FAIL'}  behind the ramp "
              f"the vote pass requested {req} pages (want 8..14, measured 13: the ramp's "
              f"own) while prediction wanted {occ_rows[-1]['wanted']} (want 25 -- the "
              f"occluded floor is frustum-visible, and only the depth-tested vote knows "
              f"it is hidden)")
        if not ok:
            failures.append("layers-vt-feedback-occlusion")

    # --- roads (spec 11.68) --------------------------------------------------
    # Every road arm renders a runtime VARIANT: the committed fixture carries no
    # roads, so its golden and the sixteen arms above cannot move.
    # feather is the only field any arm varies; **material carries anything else
    # the same mutation wants to set, which is what keeps a two-key variant one
    # call rather than a lambda sequencing two side effects.
    def _vt_road(feather=None, **material):
        f = g2.ROAD_FEATHER if feather is None else feather

        def mutate(d):
            mat = d["materials"]["layered_vt_surface"]
            mat["roads"] = [{"points": [[-g2.HALF, g2.ROAD_Z], [g2.HALF, g2.ROAD_Z]],
                             "width": g2.ROAD_WIDTH, "feather": f, "layer": g2.ROAD_LAYER}]
            mat.update(material)
        return mutate

    # The byte at a world z on the floor, which is the only space these arms
    # reason in: _vt_point maps splat coords to world, so this inverts the half
    # of it the caller would otherwise have to invert at every read.
    def road_byte(shot_data, z, u=0.125):
        return _layer_sample(shot_data[0], _vt_point(u, (z + g2.HALF) / g2.DOMAIN))

    ROAD_HALF = g2.ROAD_WIDTH / 2.0

    # --- layers-road-select --------------------------------------------------
    # The road is MADE of layer 2, so it reads that layer's painted byte -- over
    # column 0's grey and column 3's light alike, which is what says the override
    # replaced the splat's choice rather than tinting it. Past the shoulder the
    # ground reads its own byte untouched, and the per-texel leg reads the same
    # bytes as the cache: roads reach both paths through one function.
    road_cached = vt_shot("road", overrides=_vt_road())
    road_plain = vt_shot("road_plain", ["--no-layers-vt"], overrides=_vt_road())
    if not (road_cached and road_plain):
        print("  layers-road-select ERROR while rendering the road legs")
        failures.append("layers-road-select")
    else:
        on = [road_byte(road_cached, g2.ROAD_Z_ON, u) for u in (0.125, 0.875)]
        off = [road_byte(road_cached, g2.ROAD_Z_OFF, u) for u in (0.125, 0.875)]
        par = [road_byte(road_plain, g2.ROAD_Z_ON, u) for u in (0.125, 0.875)]
        base_codes = (g.GREY_CODE, g.LIGHT_CODE)
        on_ok = all(abs(c - g.DARK_CODE) <= 1 for c in on)
        off_ok = all(abs(c - b) <= 1 for c, b in zip(off, base_codes))
        par_ok = all(a == b for a, b in zip(on, par))
        ok = on_ok and off_ok and par_ok
        print(f"  layers-road-select {'PASS' if ok else 'FAIL'}  on the road "
              f"{on} (want {g.DARK_CODE} +/- 1 over BOTH bases -- a tint would "
              f"read two different values), past the shoulder {off} (want "
              f"{list(base_codes)}), per-texel leg {par} (want the cached bytes exactly)")
        if not ok:
            failures.append("layers-road-select")

    # --- layers-road-edge ----------------------------------------------------
    # The arm that proves the override lands BEFORE the height blend. Layer 2 is
    # the tallest in the set, so a road reshaped into the weights overstays its
    # mask where the heights say it should and the transition collapses into
    # LAYER_BLEND_RANGE; a road mixed in AFTER the blend would cross at the
    # mask's own half with the feather's full width, whatever the heights are.
    # Read as the RATIO of the two legs' widths, which is what survives the
    # feather being a tuning value.
    road_lin = vt_shot("road_linear", overrides=_vt_road(layerBlend=0.0))
    if not (road_cached and road_lin):
        print("  layers-road-edge ERROR while rendering the shoulder legs")
        failures.append("layers-road-edge")
    else:
        # Scan out of the road across the shoulder into bare ground.
        z0, z1 = g2.ROAD_Z_ON, g2.ROAD_Z_OFF
        # 48 over ~0.29 world units: finer than the ~0.015 units a screen pixel
        # spans at this framing, so the scan resolves the render rather than
        # under-sampling it.
        steps = 48
        zs = [z0 + (z1 - z0) * i / (steps - 1.0) for i in range(steps)]
        legs = {}
        for tag, shot_data in (("height", road_cached), ("linear", road_lin)):
            vals = [road_byte(shot_data, z) for z in zs]
            legs[tag] = _layer_ramp(vals, zs, g.DARK_CODE, g.GREY_CODE)
        if not legs["height"] or not legs["linear"]:
            print("  layers-road-edge FAIL  a shoulder scan was not monotone")
            failures.append("layers-road-edge")
        else:
            wh, ch = legs["height"]
            wl, cl = legs["linear"]
            ratio = wh / wl if wl > 0 else 99.0
            # The linear leg crosses where the mask does, at distance
            # halfWidth + 0.5 * feather from the centreline.
            lin_want = g2.ROAD_Z + ROAD_HALF + 0.5 * g2.ROAD_FEATHER
            ok = ratio <= 0.35 and abs(cl - lin_want) <= 0.06 and ch > cl
            print(f"  layers-road-edge {'PASS' if ok else 'FAIL'}  the height leg's "
                  f"shoulder is {wh:.4f} wide against the linear twin's {wl:.4f} "
                  f"(ratio {ratio:.3f}, want <= 0.35: the blend hands the transition to "
                  f"LAYER_BLEND_RANGE, where a road mixed in after it would read 1.0); "
                  f"the linear leg crosses at z={cl:.4f} (want {lin_want:.4f} +/- 0.06, "
                  f"the mask's own half) and the proud road holds on further, to "
                  f"z={ch:.4f}")
            if not ok:
                failures.append("layers-road-edge")

    # --- layers-road-pages ---------------------------------------------------
    # The first page content in the suite that is not a 0 px identity, read as
    # BYTES on the road rather than as a frame difference. A whole-frame count
    # cannot say this: the legs differ by a code or two across every flat
    # surface in view (a macro resampled twice), which swamps a road edge in
    # tens of thousands of pixels that have nothing to do with roads.
    #
    # Forced coarse for the reason the generator states: what pages resolve and
    # the fallback cannot is a band four times narrower than a fallback texel,
    # and at the derived 256 that is finer than this fixture's camera resolves
    # at all -- so the comparison is moved to a resolution the frame can show,
    # exactly as layers-vt-pages-effect does.
    coarse_texel = g2.DOMAIN / g2.VT_ROAD_COARSE_RES
    COARSE = ["--layers-vt-res", str(g2.VT_ROAD_COARSE_RES)]
    fine = _vt_road(feather=g2.ROAD_PAGES_FEATHER)
    fine_pages = vt_shot("road_fine_pages", COARSE, overrides=fine)
    fine_nopages = vt_shot("road_fine_nopages", COARSE + ["--no-layers-vt-pages"],
                           overrides=fine)
    fine_truth = vt_shot("road_fine_truth", ["--no-layers-vt"], overrides=fine)
    fine_probe = vt_probe_run(COARSE, 16, overrides=fine, name="road_fine_probe")
    if not (fine_pages and fine_nopages and fine_truth and fine_probe):
        print("  layers-road-pages ERROR while rendering the fine-shoulder legs")
        failures.append("layers-road-pages")
    else:
        # A short RUN just inside the road's edge, not one pixel: the fallback
        # has to average across the edge through this whole band, and reading
        # five points instead of one stops the arm being a single-pixel bet on
        # where a mip boundary lands.
        band = [g2.ROAD_Z_PAGES_IN - 0.008 * k for k in range(5)]
        pv = [road_byte(fine_pages, z) for z in band]
        nv = [road_byte(fine_nopages, z) for z in band]
        tv = [road_byte(fine_truth, z) for z in band]
        p, n, t = pv[0], nv[0], tv[0]
        page_err = max(abs(a - b) for a, b in zip(pv, tv))
        fallback_err = max(abs(a - b) for a, b in zip(nv, tv))
        resident = fine_probe[-1]["resident"]
        ok = (page_err <= 1 and fallback_err >= ROAD_PAGES_SMEAR_MIN and resident > 0)
        print(f"  layers-road-pages {'PASS' if ok else 'FAIL'}  over five reads inside "
              f"the road's edge, against a {coarse_texel:.4f} fallback texel and its "
              f"{coarse_texel / g2.VT_PAGE_DENSITY_RATIO:.4f} pages: the per-texel truth "
              f"reads {t}, pages read {p} and stay within {page_err} of it (want <= 1 -- "
              f"they hold the edge) where the fallback alone reads {n} and drifts "
              f"{fallback_err} (want >= {ROAD_PAGES_SMEAR_MIN}: it averaged across an edge "
              f"it cannot represent), with {resident} pages resident")
        if not ok:
            failures.append("layers-road-pages")

    # --- layers-road-invalidate ----------------------------------------------
    # Roads join the cache's by-value key, and a fresh process cannot show it:
    # it bakes once, from the final authored width. Only a mid-run transition
    # leaves a baked atlas describing a road that is no longer there.
    # The SAME point the select arm proved is bare ground: past the authored
    # shoulder (distance 0.64 > halfWidth 0.2 + feather 0.4) and inside the
    # widened one (0.64 < the new halfWidth 0.8), so it is full road after and
    # untouched ground before, with no blend band at either end.
    wide = g2.ROAD_Z_OFF
    road_widened = vt_shot("road_widened", ["--road-width-at", "10:1.6"],
                           frames=16, overrides=_vt_road())
    if not (road_cached and road_widened):
        print("  layers-road-invalidate ERROR while rendering the widened leg")
        failures.append("layers-road-invalidate")
    else:
        before = road_byte(road_cached, wide)
        after = road_byte(road_widened, wide)
        ok = abs(before - g.GREY_CODE) <= 2 and abs(after - g.DARK_CODE) <= 2
        print(f"  layers-road-invalidate {'PASS' if ok else 'FAIL'}  a point at "
              f"z={wide:.3f} reads {before} at the authored width (want "
              f"{g.GREY_CODE} +/- 2: bare ground) and {after} after a frame-10 "
              f"widening to 1.6 (want {g.DARK_CODE} +/- 2; a cache that ignored "
              f"roads in its key keeps reading the ground)")
        if not ok:
            failures.append("layers-road-invalidate")

    failures += _layers_scatter_arm()
    return failures


# Stated here rather than read out of the app: see the arm below. It is the
# midpoint of terrain.h's TERRAIN_CHANNEL_FLOW_LO/HI band -- where gravel stops
# being a tint and becomes what the ground is made of -- and the arm asserts the
# app agrees, so the two can drift apart loudly rather than silently.
LAYERS_SCATTER_LIMIT = 0.73

_SCATTER_PROBE = re.compile(
    r"scatter-probe trees=(\d+) tree_flow_max=([\d.]+) tree_flow_mean=([\d.]+) "
    r"land_flow_mean=([\d.]+) land_frac_over=([\d.]+) limit=([\d.]+)")

# The trail's own row (spec 11.68), read from the SAME forest run as the row
# above -- a second launch would cost the suite a whole forest startup to learn
# something the first one already printed.
_SCATTER_ROAD = re.compile(
    r"scatter-probe road points=(\d+) trees_on=(\d+) rejected=(\d+) width=([\d.]+)")

# The trail's authored width, stated gate-side as a DRIFT ALARM: it catches the
# trail shrinking to nothing or the probe reporting a width the app is not using.
# (Not the LAYERS_SCATTER_LIMIT argument, which is a different one -- there,
# raising the app's limit would raise the bar it was checked against, so the arm
# could never fail that way. Widening the trail weakens nothing here, because
# trees_on is counted at the app's own width.)
LAYERS_TRAIL_WIDTH = 3.0
# Floored near the measurement rather than at 1 and 2. Healthy reads 165
# rejections over a 10-point course (truncated from 12 at the shoal).
LAYERS_TRAIL_MIN_REJECTED = 60
LAYERS_TRAIL_MIN_POINTS = 6


def _layers_scatter_arm():
    """The scatter places into ground the splat has not painted as a stream bed.

    Runs with --erode because the whole claim is about the erosion masks, and
    without a bake they read zero: the placement rule is then trivially satisfied
    and an arm checking only the trees would pass against a build that never
    implemented any of it. So the terrain's OWN drainage range is asserted too,
    and that is what makes this a test rather than a tautology.
    """
    # FOREST, not a hardcoded out/bin: --bin-dir rebinds it, and running every
    # other forest arm against release while this one silently measured the
    # debug binary is exactly the "which binaries a number describes" mistake
    # the suite prints its resolved directory to avoid.
    forest = FOREST
    if not os.path.exists(forest):
        print("  layers-scatter    SKIP  (forest not built)")
        return []

    cmd = [forest, "-x", "-f", "1", "-W", "200", "-H", "120", "--erode", "--erode-res", "256",
           "--erode-iterations", "120", "--scatter-probe"]
    r = _run(cmd, capture_output=True, text=True, cwd=ROOT)
    m = _SCATTER_PROBE.search(r.stdout + r.stderr)
    if not m:
        print("  layers-scatter    ERROR  the run printed no probe row")
        return ["layers-scatter"]

    trees = int(m.group(1))
    tree_max, tree_mean = float(m.group(2)), float(m.group(3))
    land_mean, land_over = float(m.group(4)), float(m.group(5))
    limit = float(m.group(6))

    # The bar is stated HERE, not read out of the process under test. Taking it
    # from the probe's own `limit=` meant raising TREE_MAX_FLOW -- the exact
    # weakening this arm exists to prevent -- also raised the bar it was checked
    # against, so the arm could never fail that way.
    holds = tree_max <= LAYERS_SCATTER_LIMIT + 1e-4
    agrees = abs(limit - LAYERS_SCATTER_LIMIT) < 1e-4
    # And the anti-vacuity check is the FRACTION of the scatter's domain the rule
    # rejects, not the peak. erosion.c normalises flow to its own maximum, so a
    # peak near 1.0 is true of any sim that ran at all and says nothing about
    # whether there were candidates to reject.
    has_channels = land_over >= 0.02
    prefers_dry = tree_mean < land_mean
    ok = trees > 0 and holds and agrees and has_channels and prefers_dry
    print(f"  layers-scatter    {'PASS' if ok else 'FAIL'}  {trees} trees, drainage max "
          f"{tree_max:.4f} against a gate-side limit of {LAYERS_SCATTER_LIMIT:.4f} (the app "
          f"says {limit:.4f}); {land_over * 100:.1f}% of the scatter's domain is above it "
          f"(want >= 2%, or the rule rejected nothing and this proves nothing), tree mean "
          f"{tree_mean:.4f} vs land {land_mean:.4f}")
    failures = [] if ok else ["layers-scatter"]

    # --- layers-road-scatter (spec 11.68) ------------------------------------
    # Nothing stands on the trail, off the SAME run. `rejected` is the
    # anti-vacuity and it is the load-bearing half: "no tree is on the road" is
    # satisfied perfectly by a build with no road at all, or one whose trail ran
    # where nothing would have grown anyway.
    mr = _SCATTER_ROAD.search(r.stdout + r.stderr)
    if not mr:
        print("  layers-road-scatter ERROR  the run printed no trail row")
        return failures + ["layers-road-scatter"]
    points, on_road = int(mr.group(1)), int(mr.group(2))
    rejected, width = int(mr.group(3)), float(mr.group(4))
    # The reject ran at half-width plus feather plus margin and this counts at
    # the bare half-width, so a CPU distance that had drifted from the shader's
    # by less than the whole shoulder still reads zero here -- which is why the
    # rejected count, not this one, is what says the rule ran.
    #
    # And both bars are FLOORED NEAR THE MEASUREMENT, which is the lesson the
    # arm above wrote down: "rejected >= 1" is satisfied by a trail crossing one
    # candidate's worth of ground, and "points >= 2" by a trail truncated to a
    # stub. Measured 165 of 2101 candidates (7.9%) over a 10-point course.
    road_ok = (points >= LAYERS_TRAIL_MIN_POINTS and on_road == 0
               and rejected >= LAYERS_TRAIL_MIN_REJECTED
               and abs(width - LAYERS_TRAIL_WIDTH) < 1e-4)
    print(f"  layers-road-scatter {'PASS' if road_ok else 'FAIL'}  a {points}-point trail "
          f"{width:.2f} wide (the gate says {LAYERS_TRAIL_WIDTH:.2f}, want >= "
          f"{LAYERS_TRAIL_MIN_POINTS} points) carries {on_road} trees (want 0) and turned "
          f"{rejected} candidates away (want >= {LAYERS_TRAIL_MIN_REJECTED}, measured 165: a "
          f"floor of 1 is satisfied by a trail that crossed one candidate and proves nothing)")
    if not road_ok:
        failures.append("layers-road-scatter")
    return failures


# --- clustered decals (spec 11.73) ------------------------------------------
DECAL_FIXTURE = "decal_fixture.cscn"
_DECAL_GEN = None


def _decal_gen():
    """The decal fixture's generator, IMPORTED rather than transcribed.

    This block used to carry a comment saying exactly that while listing nine
    hand-copied literals underneath it -- including two derived by hand from the
    .gltf's baseColorFactors, two steps from their source. The mechanism the
    comment described already existed and is used by the layers gate; it just
    was not called.
    """
    global _DECAL_GEN
    if _DECAL_GEN is None:
        _DECAL_GEN = _import_fixture_gen("gen_decal_fixture.py", "decals")
    return _DECAL_GEN


def _decal_substrate(gen, material):
    """The albedo view's byte for an authored baseColorFactor.

    linearToSRGB in this engine is pow(1/2.2), not the piecewise sRGB curve --
    reading a substrate through the piecewise one is a two-code error on the
    floor, which is inside this gate's tolerance and would hide a real one.
    """
    for m in gen.GLTF["materials"]:
        if m["name"] == material:
            f = m["pbrMetallicRoughness"]["baseColorFactor"]
            return tuple(int(round((c ** (1.0 / 2.2)) * 255.0)) for c in f[:3])
    raise KeyError(material)


# How far a byte may sit from its painted value. Two codes of rounding through
# the linear/encode round trip, plus one of slack; anything the decal path gets
# WRONG is tens of codes. NOT dither -- --render-mode 6 takes the passthrough
# blit, which skips the tonemap where the dither lives.
DECAL_CODE_EPS = 3
# The froxel grid, from light_cluster.h. A decal is a small box, so the two in
# this fixture claim a tiny fraction of it -- measured 130 of 3072. The bound is
# the whole grid rather than a tight fit around that number: what it exists to
# catch is culling that stopped working altogether (marking every cell reads
# 6144), not a shift of a few cells when the camera or the boxes move.
DECAL_GRID_CELLS = 16 * 8 * 24


def _decal_run(workdir, tag, mutate=None, extra=None, frames=4):
    """Render the decal fixture, optionally through a mutation.

    Returns (pixels, w, h, output) or (None, None, None, error).
    """
    src = os.path.join(ROOT, "assets", DECAL_FIXTURE)
    if not os.path.exists(src):
        return None, None, None, "missing fixture"
    scene = src
    if mutate is not None:
        scene = os.path.join(workdir, f"decal_{tag}.cscn")
        cscn_copy(src, scene, mutate)
    out = os.path.join(workdir, f"decal_{tag}.ppm")
    cmd = [RENDER, "-m", scene, "-t", os.path.join(ROOT, "assets"), "-x", "-f", str(frames),
           "-W", "400", "-H", "300", "-S", out,
           "--no-auto-exposure", "-E", "1.0"] + (extra or [])
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None, None, None, r.stdout + r.stderr
    w, h, pix = _read_ppm(out)
    return pix, w, h, r.stdout + r.stderr


def _decal_byte(pix, w, h, project, world):
    """The raw RGB triple at a world point. RAW, not linearised: the albedo view
    hands back the exact code the generator painted, so the assertion is against
    a byte and introducing a decode would only add error to it.

    RAISES for a point that projects off-screen rather than clamping, which is
    _layer_sample's contract and for its reason: a clamp returns a number, and a
    number is what an arm reads, so a fixture whose framing drifted would go on
    passing against whatever pixel sat at the edge. This gate has already been
    bitten once by a read point that moved onto the wrong surface.
    """
    px, py = project(world)
    x, y = int(round(px)), int(round(py))
    if not (0 <= x < w and 0 <= y < h):
        raise AssertionError(f"world {world} projects to ({x},{y}), outside the {w}x{h} frame")
    o = (y * w + x) * 3
    return (pix[o], pix[o + 1], pix[o + 2])


def _decal_near(got, want, eps=DECAL_CODE_EPS):
    return all(abs(int(g) - int(v)) <= eps for g, v in zip(got, want))


def _decal_probe_rows(output):
    """The --decal-probe lines, split into the per-frame and per-decal halves."""
    frames, decals = [], []
    for line in output.splitlines():
        # `if "=" in kv`, the guard the file's four other k=v readers carry: a
        # bare token added to either line would otherwise raise mid-suite
        # instead of leaving a field unread.
        if line.startswith("decal-probe decal "):
            decals.append(dict(kv.split("=", 1) for kv in line.split()[2:] if "=" in kv))
        elif line.startswith("decal-probe frame="):
            frames.append(dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv))
    return frames, decals


def run_decal_gate(workdir):
    """Marks projected through a box, culled into the froxel grid (spec 11.73).

      decals-identity   a decal-free scene is the pre-decal frame exactly, and the
                        fixture WITH its decals is not -- the second half is what
                        stops the first passing on a build that projects nothing
      decals-albedo     the poster's interior reads its painted code through the
                        albedo view, with clear wall as the in-frame control
      decals-orient     each of the poster's four quadrants lands in the world
                        corner it was painted for -- the arm that says the mark
                        is a PICTURE and not just a coloured patch
      decals-angle-fade the oblique plate inside the poster's box reads its own
                        substrate: a projector must refuse a surface it grazes
      decals-surface    the scorch's roughness reads its PAINTED code through
                        renderMode 8, against a no-surface-map twin and a floor
                        control that must not move
      decals-normal     flattening the relief ALONE -- the map still bound --
                        moves the lit frame, which is the only arm that reaches
                        the decal's tangent frame
      decals-opacity    a half-opaque poster reads the MIDPOINT between its own code
                        and the wall's, not something darker -- the premultiplied
                        accumulation, which the opaque arms above cannot see
      decals-feather    a point inside the poster's edge ramp reads the substrate
                        mixed at the coverage the box geometry owes, so a mark
                        fades out of its volume instead of stopping dead
      decals-overlap    a THIRD mark over the poster wins where it covers it, and
                        the poster still reads its own code beside it -- the only
                        arm where the accumulator is non-empty at all
      decals-mask       two runs agree on the froxel digest, the live decals claim
                        a fraction of the grid rather than all of it, and a decal
                        behind the camera claims no froxel and moves no pixel
      decals-schema     a decal missing position, size, direction or image is
                        refused by name, and a degenerate size is refused too
      decals-capture    a decal reaches a reflection PROBE's capture, read as the
                        poster's reflection in a mirror floor -- the property the
                        materialArray tenancy bought over a unit-6 alias
      decals-config     the snapshot's nine decal rows survive a dump and a
                        restore, and a perturbed one CHANGES THE FRAME -- the
                        config group's own fixture authors no decal, so without
                        this the rows are dead to the whole suite

    NO ARM HERE IS SAFE ALONE, the probe-set rule. decals-albedo alone passes on
    a build with no facing test, no edge and no mask -- it only asks that a mark
    landed. decals-angle-fade alone passes on a build that projects NOTHING, since
    a surface taking no decal is exactly what it asserts; its pairing with
    decals-albedo is what makes it a claim about refusal rather than absence. And
    decals-identity's first half passes on any build where the feature is inert,
    which is why its second half asserts the decals moved the frame at all.

    decals-overlap is the only arm that runs the accumulation with anything in it.
    The committed fixture's two decals are disjoint in two axes, so `over`
    composited against a zero accumulator every time -- which is the one input
    under which paint order, the premultiplied sum and a plain overwrite are all
    the same picture. It APPENDS its third mark rather than the fixture carrying
    one, because two arms here assert the scene's exact decal count and four
    mutations index decals[0]: the roads rule, and load-bearing rather than
    stylistic. What it does NOT assert is the colour space of that blend -- decal
    over decal composites in stored codes where decal over substrate composites in
    linear, which is stated as a choice in decals_ubo.glsl and asserted here as it
    stands.

    decals-opacity exists because every other colour arm reads an OPAQUE interior,
    where the premultiplied accumulation and the naive one agree exactly -- which
    is how a real double-multiply survived the first mutation round with all six
    arms green. Anything that reads only a == 1 is blind to how a mark meets the
    surface under it, which is most of what a decal is.

    decals-orient exists for the same class of reason and cost more: every image
    in this fixture used to be invariant under mirror, flip, transpose AND
    180-degree rotation, so a projector laying every picture on backwards and
    upside down -- which is what shipped -- painted a frame no arm could tell
    from a correct one. A single-colour patch cannot test a projector. The
    quadrants are what make the fixture able to fail.

    ONE DELIBERATE ABSENCE: no arm covers the half-texel UV inset that guards
    against the material array's GL_REPEAT wrap. One was written and removed on
    measurement -- deleting the inset moves 0 px on this fixture, because its
    images carry a transparent margin and a wrapped blend between two
    transparent texels is still transparent. Making it observable needs an image
    opaque to its own border, where the affected band is half a texel: at the
    canonical 64-texel array size over a two-unit box that is 0.016 world units,
    which is sub-pixel at this framing. The inset stays because it is correct and
    free; an arm that cannot fail would be worse than the gap it papers over.

    Every read is a BYTE through --render-mode 6, the layer fixture's rule: the
    albedo view returns linearToSRGB(factor * sRGBToLinear(blend)), so a correct
    renderer hands back the exact code the generator painted and the assertion
    argues about nothing else -- not the BRDF, not exposure, not the tonemap.
    """
    if not os.path.exists(os.path.join(ROOT, "assets", DECAL_FIXTURE)):
        print(f"  decals-identity SKIP  {DECAL_FIXTURE} not found")
        return []
    gen = _decal_gen()
    if gen is None:
        return []
    failures = []
    cam = _cscn_camera(DECAL_FIXTURE)
    wall_substrate = _decal_substrate(gen, "decal_wall_mat")
    oblique_substrate = _decal_substrate(gen, "decal_oblique_mat")
    floor_substrate = _decal_substrate(gen, "decal_floor_mat")

    # -- identity: the feature costs a decal-free scene nothing ---------------
    def strip(d):
        d.pop("decals", None)

    pix_off, w, h, out_off = _decal_run(workdir, "off", strip, ["--render-mode", "6"])
    pix_flag, _, _, out_flag = _decal_run(workdir, "flag", None,
                                          ["--render-mode", "6", "--no-decals"])
    pix_on, _, _, out_on = _decal_run(workdir, "on", None, ["--render-mode", "6"])
    if pix_off is None or pix_flag is None or pix_on is None:
        failures.append("decals-identity")
        print(f"  decals-identity FAIL  render error\n{out_off or out_flag or out_on}")
    else:
        off = os.path.join(workdir, "decal_off.ppm")
        flag = os.path.join(workdir, "decal_flag.ppm")
        on = os.path.join(workdir, "decal_on.ppm")
        same, _ = compare(off, flag)
        moved, _ = compare(off, on)
        # The anti-vacuity: the decals have to have DONE something, or the
        # identity above is between two frames that never had a decal in them.
        ok = same == 0 and moved > 500
        if not ok:
            failures.append("decals-identity")
        print(f"  decals-identity {'PASS' if ok else 'FAIL'}  --no-decals is the "
              f"stripped scene at {same} px (want 0), while the decals themselves "
              f"move {moved} px (want > 500)")

    project = _projector(cam, w or 400, h or 300)

    # -- albedo: the painted codes, and clear substrate as the control --------
    if pix_on is not None:
        poster = _decal_byte(pix_on, w, h, project, gen.POSTER_READ)
        wall = _decal_byte(pix_on, w, h, project, gen.WALL_CLEAR)
        # The scorch too: it is the decal whose frame comes from the canonical
        # perpendicular fallback and the only one carrying a surface map, so it
        # is the one most likely to be wrong -- and its colour went unasserted
        # while both this file and AGENTS.md quoted it as a reading.
        scorch = _decal_byte(pix_on, w, h, project, gen.SCORCH_READ)
        floor = _decal_byte(pix_on, w, h, project, gen.FLOOR_CLEAR)
        ok = (_decal_near(poster, gen.POSTER_TR) and _decal_near(wall, wall_substrate) and
              _decal_near(scorch, gen.SCORCH_CODE) and _decal_near(floor, floor_substrate))
        if not ok:
            failures.append("decals-albedo")
        print(f"  decals-albedo {'PASS' if ok else 'FAIL'}  poster reads {poster} "
              f"(want {gen.POSTER_TR}), scorch {scorch} (want {gen.SCORCH_CODE}); "
              f"controls: wall {wall} (want {wall_substrate}), floor {floor} "
              f"(want {floor_substrate}), all +/-{DECAL_CODE_EPS}")

    # -- orient: the picture arrives the way round it was painted -------------
    if pix_on is not None:
        # Which PNG quadrant each world corner must return. A projector that
        # mirrors, flips, rotates or transposes gets a DIFFERENT wrong answer
        # for each, because the generator asserts the four codes are separated
        # in every channel.
        want = {"upper_left": gen.POSTER_TL, "upper_right": gen.POSTER_TR,
                "lower_left": gen.POSTER_BL, "lower_right": gen.POSTER_BR}
        got = {k: _decal_byte(pix_on, w, h, project, v) for k, v in gen.QUAD_READS.items()}
        wrong = [k for k in want if not _decal_near(got[k], want[k])]
        # Anti-vacuity: the four reads must be four DIFFERENT bytes, or they are
        # all landing on one quadrant and agreeing with it by accident.
        distinct = len({tuple(v) for v in got.values()}) == 4
        ok = not wrong and distinct
        if not ok:
            failures.append("decals-orient")
        print(f"  decals-orient {'PASS' if ok else 'FAIL'}  four quadrants land in their "
              f"own corners ({len(want) - len(wrong)}/4 correct, {len(set(map(tuple, got.values())))} "
              f"distinct of 4)" +
              (f"; wrong: {', '.join(f'{k}={got[k]} want {want[k]}' for k in wrong)}"
               if wrong else ""))

    # -- angle fade: the oblique plate refuses the mark -----------------------
    if pix_on is not None:
        ob = _decal_byte(pix_on, w, h, project, gen.OBLIQUE_READ)
        ok = _decal_near(ob, oblique_substrate)
        if not ok:
            failures.append("decals-angle-fade")
        print(f"  decals-angle-fade {'PASS' if ok else 'FAIL'}  the oblique plate "
              f"inside the poster's box reads {ob} (want its own substrate "
              f"{oblique_substrate} +/-{DECAL_CODE_EPS}, i.e. no mark)")

    # -- surface: the scorch's map moves the lit frame ------------------------
    # renderMode 8 is vec4(metallicMap, roughnessMap, 0, 1), and its return sits
    # AFTER the wet-sand seam where the surface half lands -- so the G channel is
    # the decal's roughness as the BRDF will see it, read as a byte rather than
    # inferred from a pixel count.
    def drop_surface(d):
        for dec in d.get("decals", []):
            dec.pop("surface", None)

    def flat_normal(d):
        # The surface map STAYS; only its relief is switched off. That is what
        # separates the normal half from the roughness half -- popping the map,
        # which is what this arm used to do, drops both and cannot tell them
        # apart while claiming in its own message to be testing the normal.
        for dec in d.get("decals", []):
            dec["normalStrength"] = 0.0

    pix_r, _, _, out_r = _decal_run(workdir, "rough", None, ["--render-mode", "8"])
    pix_rn, _, _, out_rn = _decal_run(workdir, "roughnone", drop_surface,
                                      ["--render-mode", "8"])
    if pix_r is None or pix_rn is None:
        failures.append("decals-surface")
        print(f"  decals-surface FAIL  render error\n{out_r or out_rn}")
    else:
        got = _decal_byte(pix_r, w, h, project, gen.SCORCH_READ)[1]
        bare = _decal_byte(pix_rn, w, h, project, gen.SCORCH_READ)[1]
        ctrl = _decal_byte(pix_r, w, h, project, gen.FLOOR_CLEAR)[1]
        ctrl_bare = _decal_byte(pix_rn, w, h, project, gen.FLOOR_CLEAR)[1]
        # renderMode 8 writes roughness raw, no encode -- so the painted code IS
        # the expected byte, give or take the 0.04 floor and rounding.
        want = gen.SCORCH_ROUGH
        ok = (abs(got - want) <= 4 and abs(got - bare) > 40 and ctrl == ctrl_bare)
        if not ok:
            failures.append("decals-surface")
        print(f"  decals-surface {'PASS' if ok else 'FAIL'}  the scorch's roughness reads "
              f"{got} (want its painted {want} +/-4) against {bare} with no surface map "
              f"(want > 40 apart), and the floor control is {ctrl} either way "
              f"(unmoved: {ctrl == ctrl_bare})")

    # -- surface-normal: the relief, isolated from the roughness --------------
    lit_full, _, _, out_lit = _decal_run(workdir, "lit", None, None)
    lit_flat, _, _, out_flat = _decal_run(workdir, "flat", flat_normal, None)
    if lit_full is None or lit_flat is None:
        failures.append("decals-normal")
        print(f"  decals-normal FAIL  render error\n{out_lit or out_flat}")
    else:
        full = os.path.join(workdir, "decal_lit.ppm")
        flat = os.path.join(workdir, "decal_flat.ppm")
        moved, _ = compare(full, flat)
        ok = moved > 200
        if not ok:
            failures.append("decals-normal")
        print(f"  decals-normal {'PASS' if ok else 'FAIL'}  flattening the scorch's relief "
              f"alone -- its surface map still bound -- moves {moved} px of the lit frame "
              f"(want > 200; a build that never composes the decal normal reads 0)")

    # -- opacity: the premultiply, which every opaque read is blind to --------
    def half(d):
        d["decals"][0]["opacity"] = 0.5

    pix_half, _, _, out_half = _decal_run(workdir, "half", half, ["--render-mode", "6"])
    if pix_half is None:
        failures.append("decals-opacity")
        print(f"  decals-opacity FAIL  render error\n{out_half}")
    else:
        got = _decal_byte(pix_half, w, h, project, gen.POSTER_READ)
        # The albedo view encodes with pow(1/2.2), and the blend happens in
        # LINEAR -- so the expected byte is the encode of the half-and-half of
        # the two decoded values, not the average of the two bytes.
        want = tuple(
            int(round((((c / 255.0) ** 2.2 + (s_ / 255.0) ** 2.2) * 0.5) ** (1 / 2.2) * 255.0))
            for c, s_ in zip(gen.POSTER_TR, wall_substrate))
        ok = _decal_near(got, want, eps=4)
        if not ok:
            failures.append("decals-opacity")
        print(f"  decals-opacity {'PASS' if ok else 'FAIL'}  a half-opaque poster reads "
              f"{got} (want {want} +/-4, the linear midpoint of {gen.POSTER_TR} and "
              f"{wall_substrate}); multiplying by coverage twice reads darker")

    # -- feather: the edge ramp, which was inert until the fixture was fixed --
    if pix_on is not None:
        got = _decal_byte(pix_on, w, h, project, gen.FEATHER_READ)
        # SOLVED for, not compared byte to byte. The read point lands within a
        # pixel of where it is asked for, and a pixel here is 0.03 of coverage --
        # so a byte tolerance loose enough to survive that is loose enough to
        # miss a real error. Inverting the blend states what the arm means: the
        # coverage the geometry owes is what came back.
        got_c = []
        for k, (m, s_) in enumerate(zip(gen.POSTER_BR, wall_substrate)):
            mark_lin, wall_lin = (m / 255.0) ** 2.2, (s_ / 255.0) ** 2.2
            if abs(wall_lin - mark_lin) < 0.1:
                continue  # this channel cannot resolve a coverage
            got_lin = (got[k] / 255.0) ** 2.2
            got_c.append((wall_lin - got_lin) / (wall_lin - mark_lin))
        measured = sum(got_c) / len(got_c) if got_c else 0.0
        want_c = gen.FEATHER_COVERAGE
        # +/- 0.06 is two pixels of the read's own position. A build with no
        # ramp reads 1.0 and one with no decal reads 0.0, both far outside it --
        # which is the anti-vacuity, and it is arithmetic rather than a bar.
        ok = abs(measured - want_c) <= 0.06 and len(got_c) >= 2
        if not ok:
            failures.append("decals-feather")
        print(f"  decals-feather {'PASS' if ok else 'FAIL'}  inside the edge ramp reads "
              f"{got}, i.e. coverage {measured:.3f} over {len(got_c)} channels (want "
              f"{want_c:.3f} +/-0.06 from the box geometry; no ramp reads 1.000, no "
              f"decal reads 0.000)")

    # -- overlap: a mark over a mark, and which one wins ----------------------
    def overlap(d):
        # APPENDED, which is what makes it the later paint: decalAccumulate walks
        # ascending and composites over, so the generator asserts this slot is
        # past the poster's rather than the arm assuming it.
        d["decals"].append({
            "position": list(gen.OVERLAP_POS),
            "size": list(gen.OVERLAP_HALF),
            "direction": [0.0, 0.0, -1.0],
            "image": gen.OVERLAP_IMAGE,
            "opacity": 1.0,
            "angleFade": gen.ANGLE_FADE,
            "feather": gen.FEATHER,
        })

    pix_ov, _, _, out_ov = _decal_run(workdir, "overlap", overlap, ["--render-mode", "6"])
    if pix_ov is None:
        failures.append("decals-overlap")
        print(f"  decals-overlap FAIL  render error\n{out_ov}")
    else:
        over = _decal_byte(pix_ov, w, h, project, gen.OVERLAP_READ)
        under = _decal_byte(pix_ov, w, h, project, gen.OVERLAP_CONTROL)
        # The control is in the SAME poster quadrant, which is the anti-vacuity:
        # a variant whose third mark covered the whole poster reads the overlap's
        # code at both points and passes half of this on its own.
        ok = _decal_near(over, gen.SCORCH_CODE) and _decal_near(under, gen.POSTER_BL)
        if not ok:
            failures.append("decals-overlap")
        print(f"  decals-overlap {'PASS' if ok else 'FAIL'}  the later mark reads {over} "
              f"(want {gen.SCORCH_CODE}) where it covers the poster, and the poster still "
              f"reads {under} (want {gen.POSTER_BL}) beside it, +/-{DECAL_CODE_EPS}; "
              f"reversing the composite reads {gen.POSTER_BL} at both")

    # -- mask: determinism, and a decal off screen claims nothing -------------
    _, _, _, out_p1 = _decal_run(workdir, "probe1", None, ["--decal-probe", "1"])
    _, _, _, out_p2 = _decal_run(workdir, "probe2", None, ["--decal-probe", "1"])

    def behind(d):
        # Both decals moved behind the camera, which looks down -Z from z=5.5.
        for dec in d.get("decals", []):
            dec["position"] = [dec["position"][0], dec["position"][1], 40.0]

    pix_behind, _, _, out_behind = _decal_run(workdir, "behind", behind,
                                              ["--render-mode", "6", "--decal-probe", "1"])
    f1, d1 = _decal_probe_rows(out_p1 or "")
    f2, _ = _decal_probe_rows(out_p2 or "")
    fb, _ = _decal_probe_rows(out_behind or "")
    if not f1 or not f2 or not fb or pix_behind is None:
        failures.append("decals-mask")
        print(f"  decals-mask FAIL  no probe rows\n{(out_p1 or '')[:400]}")
    else:
        digest_a, digest_b = f1[-1]["digest"], f2[-1]["digest"]
        bits_live = int(f1[-1]["mask_bits"])
        bits_behind = int(fb[-1]["mask_bits"])
        behind_ppm = os.path.join(workdir, "decal_behind.ppm")
        off = os.path.join(workdir, "decal_off.ppm")
        px_behind, _ = compare(off, behind_ppm)
        ok = (digest_a == digest_b and 0 < bits_live < DECAL_GRID_CELLS and
              bits_behind == 0 and px_behind == 0 and int(f1[-1]["live"]) == 2)
        if not ok:
            failures.append("decals-mask")
        print(f"  decals-mask {'PASS' if ok else 'FAIL'}  digest {digest_a} == "
              f"{digest_b} across two runs, {bits_live} froxel bits live "
              f"(want 0 < n < {DECAL_GRID_CELLS}; marking every cell reads "
              f"{2 * DECAL_GRID_CELLS}), and behind the camera {bits_behind} bits "
              f"(want 0) rendering {px_behind} px from the decal-free frame (want 0)")

    # -- schema: the four required keys, refused BY NAME ----------------------
    def drop_position(d):
        d["decals"][0].pop("position", None)

    def drop_image(d):
        d["decals"][0].pop("image", None)

    def zero_size(d):
        d["decals"][0]["size"] = [1.0, 0.0, 1.0]

    _, _, _, out_nopos = _decal_run(workdir, "nopos", drop_position, ["--decal-probe", "1"])
    _, _, _, out_noimg = _decal_run(workdir, "noimg", drop_image, ["--decal-probe", "1"])
    _, _, _, out_zero = _decal_run(workdir, "zerosize", zero_size, ["--decal-probe", "1"])
    _, clean = _decal_probe_rows(out_p1 or "")
    named = [
        ("position", out_nopos, "needs position, size and direction"),
        ("image", out_noimg, "needs an image"),
        ("size", out_zero, "size must be positive"),
    ]
    misses = [k for k, o, want in named if want not in (o or "")]
    # The unmutated twin must load silently, or "a warning appeared" says nothing.
    clean_quiet = "decal needs" not in (out_p1 or "")
    ok = not misses and clean_quiet and len(clean) == 2
    if not ok:
        failures.append("decals-schema")
    print(f"  decals-schema {'PASS' if ok else 'FAIL'}  refused by name: "
          f"{len(named) - len(misses)}/{len(named)}"
          f"{' (missing ' + ','.join(misses) + ')' if misses else ''}, "
          f"and the unmutated fixture loads with no refusal and {len(clean)} decals")

    # -- capture: decals reach a reflection probe, which is the design claim ---
    #
    # The whole texture story rests on this. A unit-6 alias -- the plan the
    # roadmap booked -- is bound only for `SUBMIT_PASS_SHADE && !alpha_pass &&
    # !capturing`, so it would have had decals absent from probe captures; the
    # materialArray tenancy has no such exclusion. Nothing asserted the
    # difference, and the arm was specced and never written.
    def with_probe(d, drop_decals=False):
        # A mirror floor and a probe that can see the wall. The probe is what
        # carries the poster into the reflection; the floor is what shows it.
        d["probes"] = [{"position": [0.0, 1.2, 0.0],
                        "boxMin": [-3.0, -0.1, -3.1],
                        "boxMax": [3.0, 4.0, 3.1],
                        "boxFade": 0.05}]
        d["materials"] = {"decal_floor_mat": {"roughness": 0.02, "metallic": 1.0}}
        if drop_decals:
            d.pop("decals", None)

    pix_cap, _, _, out_cap = _decal_run(workdir, "cap", with_probe, PROBE_SKY, frames=8)
    pix_capless, _, _, out_capless = _decal_run(
        workdir, "capless", lambda d: with_probe(d, True), PROBE_SKY, frames=8)
    if pix_cap is None or pix_capless is None:
        failures.append("decals-capture")
        print(f"  decals-capture FAIL  render error\n{(out_cap or out_capless)[:400]}")
    else:
        # The reflection, and a floor control the wall cannot reach into.
        refl = _decal_byte(pix_cap, w, h, project, gen.MIRROR_READ)
        refl_bare = _decal_byte(pix_capless, w, h, project, gen.MIRROR_READ)
        moved = sum(abs(int(a) - int(b)) for a, b in zip(refl, refl_bare))
        # Anti-vacuity: a probe has to have been captured at all, or the two
        # frames are two mirrors of the same empty wall and agree for a reason
        # that has nothing to do with decals.
        captured = "probe" in (out_cap or "").lower()
        ok = moved > 12 and captured
        if not ok:
            failures.append("decals-capture")
        print(f"  decals-capture {'PASS' if ok else 'FAIL'}  the poster's reflection in a "
              f"mirror floor reads {refl} with the decal and {refl_bare} without "
              f"(sum |delta| {moved}, want > 12); a probe was captured: {captured}")

    # -- config: the snapshot rows, which the config group's fixture cannot see -
    #
    # CONFIG_FIXTURE is cornell_rooms and it authors no decal, so _decal_at
    # returns NULL at every index and the whole "decals" section is empty in
    # every arm over there -- config-perturb included, which is the one arm that
    # can see a row whose apply silently does nothing. And config-coverage
    # matches on the TRAILING member name, so `position`, `opacity`, `enabled`
    # and the rest are all already satisfied by Light and Probe rows: the nine
    # decal rows would pass that census if they were deleted outright.
    # Dumped from a session already in the state the reference frames were taken
    # in -- albedo view, exposure pinned. The snapshot OWNS THE LOOK and applies
    # after the CLI, so a dump taken in another mode would restore that mode and
    # overrule the flag, and the comparison would be against a different picture.
    _, dump, _ = _config_run(workdir, "decals",
                             ["-t", os.path.join(ROOT, "assets"), "--render-mode", "6",
                              "--no-auto-exposure", "-E", "1.0"],
                             model=DECAL_FIXTURE, frames=4)
    carried, restored_px, keys = [], None, 0
    if os.path.exists(dump):
        with open(dump) as fh:
            snap = json.load(fh)
        rows = snap.get("decals") or []
        keys = len(rows[0]) if rows else 0
        carried = sorted(rows[0].keys()) if rows else []
        # Perturb through the SNAPSHOT rather than the .cscn: that is the path
        # under test, and opacity 0 is a change every pixel of the mark can see.
        for r in rows:
            r["opacity"] = 0.0
        edited = os.path.join(workdir, "config_decals_edited.json")
        with open(edited, "w") as fh:
            json.dump(snap, fh)
        shot = os.path.join(workdir, "config_decals_restored.ppm")
        # No look flags here on purpose: the snapshot carries them, which is the
        # claim -- "give somebody the JSON and they see your pixels".
        subprocess.run([RENDER, "--config", edited, "-t", os.path.join(ROOT, "assets"),
                        "-x", "-f", "4", "-W", "400", "-H", "300", "-S", shot],
                       capture_output=True, text=True)
        on = os.path.join(workdir, "decal_on.ppm")
        off = os.path.join(workdir, "decal_off.ppm")
        if os.path.exists(shot):
            moved, _ = compare(on, shot)
            same_as_off, _ = compare(off, shot)
            restored_px = (moved, same_as_off)
    # Nine rows, and the restore must land: opacity 0 takes the frame all the way
    # back to the decal-free one, which no half-applied restore reaches.
    ok = (keys >= 9 and restored_px is not None and restored_px[0] > 500 and
          restored_px[1] == 0)
    if not ok:
        failures.append("decals-config")
    print(f"  decals-config {'PASS' if ok else 'FAIL'}  the dump carries {keys} decal rows "
          f"({', '.join(carried) if carried else 'none'}); restoring one with opacity 0 "
          f"moves {restored_px[0] if restored_px else '?'} px from the marked frame (want "
          f"> 500) and lands {restored_px[1] if restored_px else '?'} px from the "
          f"decal-free one (want 0)")

    return failures


GATE_GROUPS = [
    ("scale", "scale invariance (lights x1000, exposure /1000):", run_scale_gates),
    ("penumbra", "area shadow (analytic penumbra):", run_penumbra_gate),
    ("grazing", "punctual grazing (leak wall base):", run_grazing_gate),
    ("dir-shadow", "cascade shadow (analytic ellipse):", run_dir_shadow_gate),
    ("catcher", "catcher over a real ground (contact fixture):", run_catcher_gate),
    ("contact", "contact shadows for the lights with no shadow map (spec 11.56):",
     run_contact_gate),
    ("ao", "ambient occlusion reaches the frame (spec 11.75):", run_ao_gate),
    ("ies", "IES photometric profiles (table, symmetry, seeding, fold; spec 11.57):",
     run_ies_gate),
    ("catcher-transparency", "catcher vs transparency (panel through the plane):",
     run_catcher_transparency_gate),
    ("oit", "order-independent transparency (analytic card stack):", run_oit_gate),
    ("absorption", "volume absorption (path length and channel selectivity, spec 11.32):",
     run_absorption_gate),
    ("fog-volume", "local fog volumes (density, tint, arming; spec 11.39):",
     run_fog_volume_gate),
    ("fogdepth", "fog at the translucent depth (spec 11.78):", run_fogdepth_gate),
    ("cloud-shadow", "cloud shadows into the fog and onto the ground (specs 11.39, 11.41):",
     run_cloud_shadow_gate),
    ("stars", "night star field (contribution, ramp, extinction, deck; spec 11.79):",
     run_stars_gate),
    ("night-floor", "the night-sky floor lighting the world (spec 11.80):",
     run_nightfloor_gate),
    ("cycle", "the day/night clock and its sliced env re-bake (spec 11.81):",
     run_cycle_gate),
    ("moon", "the moon: disc, derived phase, terminator and a second casting light "
             "(spec 11.82):", run_moon_gate),
    ("water", "water surface (determinism, absorption, shoreline, reach; specs 11.32-11.35):",
     run_water_gate),
    ("beach", "the shoaling bed the eye can see (surf ring, turquoise, bound; spec 11.44):",
     run_beach_gate),
    ("emissive", "emissive geometry as area lights (fit, intent, light; spec 11.49):",
     run_emissive_gate),
    ("probe-set", "clustered specular probes (selection, blend, tenancy; spec 11.70):",
     run_probe_set_gate),
    ("decals", "clustered decals (projection, fade, surface, masks; spec 11.73):",
     run_decal_gate),
    ("config", "the session dumped to JSON and restored from it (spec 11.71):",
     run_config_gate),
    ("clouds", "cloud layer (steady-state churn, report-only):", run_cloud_churn_gate),
    ("skin-offpath", "pre-integrated skin (off-path byte identity):", run_skin_offpath_gate),
    ("skin-curvature", "pre-integrated skin (curvature ordering):", run_skin_curvature_gate),
    ("skin-handoff", "pre-integrated skin (handoff past the scatter ceiling):",
     run_skin_handoff_gate),
    ("skin-area", "subsurface under an area light (spec 11.19 / B3.2):", run_skin_area_gate),
    ("hair", "hair lobes driven by the strand map (spec 11.20 / B8):", run_hair_flow_gate),
    ("flare", "lens flare and chromatic aberration (spec 11.21 / B7):", run_flare_gate),
    ("sss-invariance", "subsurface blur (world width vs frame size):", run_sss_invariance_gate),
    ("sss-banding", "subsurface blur (kernel not visible as rings):", run_sss_banding_gate),
    ("dither", "output dither (8-bit contour bands, spec 11.24 / E1):", run_dither_gate),
    ("lut", "3D LUT colour grading (spec 11.58 / E2):", run_lut_gate),
    ("origin", "a world away from the origin, and one that moves under it (spec 11.62 / D11):",
     run_origin_gate),
    ("terrain", "Heightfield terrain and erosion (spec 11.59 / D6-D8):", run_terrain_gate),
    ("terrain-stream", "terrain streaming (spec 11.69 / D4):", run_terrain_stream_gate),
    ("layers", "layered surfaces and their composite cache (specs 11.60, 11.66):",
     run_layers_gate),
    ("translucent", "translucent shadows (analytic layer stack, spec 11.26 / C1):",
     run_translucent_shadow_gate),
    ("translucent-offpath", "translucent shadows (off-path identity and the inverse arm):",
     run_translucent_offpath_gate),
    ("profiler", "gpu timing (per-pass queries, spec 11.27 / E4):", run_profiler_gate),
    ("cull", "wind and skinned geometry is cullable (spec 11.53):", run_cull_gate),
    ("submission", "submission (draw counts + the CPU column, spec 11.28 / E5):",
     run_submission_gate),
    ("draw-list", "draw list (submission order, spec 11.28 Phase 3):", run_draw_list_gate),
    ("lod", "LOD chains (selection by projected size, spec 11.28 Phase 6):", run_lod_gate),
    ("mask", "alpha mask (binary above the cutoff, spec 11.31):", run_mask_gate),
    ("wind-uv", "UV1 carries wind data through import (spec 11.51):", run_wind_uv_gate),
    ("varying", "varyings under partial coverage (spec 11.38):", run_varying_gate),
    ("sss-tag", "subsurface profile tag through the MSAA resolve (spec 11.37):",
     run_sss_tag_gate),
    ("overdraw", "depth complexity (a scene whose answer is known, spec 11.31):",
     run_overdraw_gate),
    ("prepass", "depth prepass (identical picture, less shading, spec 11.30 / E6):",
     run_prepass_gate),
    ("cluster", "cluster-DAG level of detail (spec 11.63):", run_cluster_gate),
    ("quadtree", "the terrain quadtree (spec 11.63):", run_quadtree_gate),
    ("region", "region residency (spec 11.63):", run_region_gate),
    ("island", "the island (spec 11.63):", run_island_gate),
    ("forest", "forest (scattered content: batching, ordering, LOD, spec 11.29):",
     run_forest_gate),
    ("import", "import:", _run_import_gates),
    ("fixture-gen", "fixture generators (every gen_*.py reproduces its asset):",
     run_fixture_gen_gate),
    ("exposure", "histogram auto-exposure (the meter, its mask, its percentiles):",
     run_exposure_gate),
    ("gate-docs", "gate docstrings (the documented arm list is the one that runs):",
     run_gate_docs_gate),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="keep the generated scenes and frames")
    ap.add_argument("--only", metavar="SEL",
                    help="run only groups whose selector contains SEL (comma-separated)")
    ap.add_argument("--list", action="store_true", help="list group selectors and exit")
    ap.add_argument("--bin-dir", metavar="DIR",
                    help="where to find the app binaries (default out/bin; "
                         "out/release/bin runs forest ~5x faster)")
    args = ap.parse_args()

    if args.bin_dir:
        global BIN_DIR, RENDER, FOREST
        BIN_DIR = os.path.abspath(args.bin_dir)
        RENDER = _bin("render")
        FOREST = _bin("forest")

    if args.list:
        for selector, banner, _ in GATE_GROUPS:
            print(f"  {selector:<22} {banner}")
        return 0

    if not os.path.exists(RENDER):
        sys.exit(f"{RENDER} not found -- run ./build.sh first")
    # Said out loud on every run: which binaries these numbers describe is part
    # of the result, not a detail of the harness.
    print(f"binaries: {BIN_DIR}")
    # And the lever, where it will actually be read, because a flag nobody
    # remembers is a flag nobody uses. Only offered when a release tree exists:
    # suggesting it otherwise is suggesting a build, which is the user's call.
    if BIN_DIR.endswith(os.path.join("out", "bin")):
        print("          (forest runs ~5x faster from a release build -- "
              "./build.sh --release and it is picked up automatically)")

    groups = GATE_GROUPS
    if args.only:
        wanted = [s.strip() for s in args.only.split(",") if s.strip()]
        groups = [g for g in GATE_GROUPS if any(w in g[0] for w in wanted)]
        # A selector that matches nothing is a typo, and running zero gates while
        # reporting success is the worst outcome this script has.
        if not groups:
            sys.exit(f"--only {args.only!r} matched no group; --list to see them")

    workdir = tempfile.mkdtemp(prefix="cetra_gates_")
    # Said out loud for the same reason the binary directory is: the framebuffer
    # a number was measured on is part of the number. 2 is what this suite's
    # sample coordinates were authored against, so it renders unchanged; 1 asks
    # for double and lands on the same buffer.
    scale = _detect_fb_scale(workdir)
    print(f"framebuffer: {scale}x the requested size"
          + ("" if scale == CALIBRATED_FB_SCALE
             else f" -- requests scaled x{CALIBRATED_FB_SCALE // scale} to reach the "
                  f"{CALIBRATED_FB_SCALE}x buffer this suite was calibrated on"))
    failures = []
    try:
        for _, banner, fn in groups:
            print(banner)
            failures += fn(workdir)
    finally:
        if args.keep:
            print(f"\nartifacts in {workdir}")
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    scope = "" if groups is GATE_GROUPS else f" ({len(groups)} of {len(GATE_GROUPS)} groups)"
    if failures:
        print(f"\n{len(failures)} gate(s) failed{scope}: {', '.join(failures)}")
        return 1
    print(f"\nall gates passed{scope}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
