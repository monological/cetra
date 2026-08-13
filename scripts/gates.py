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
import re
import os
from itertools import groupby
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
    r = subprocess.run(cmd, capture_output=True, text=True)
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
    r = subprocess.run(cmd, capture_output=True, text=True)
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
    # --no-dither beside --no-vignette: the hidden arm differences two SINGLE
    # pixels at different fragcoords, and an independent LSB on each can reach
    # CATCHER_HIDDEN_TOL on its own, before any ordering error contributes.
    cmd = [RENDER, "-m", scene, "--no-recenter", "-x", "-f", "30", "-W", "800", "-H", "500",
           "--no-vignette", "--no-dither", "-S", out] + extra
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
    r = subprocess.run(cmd, capture_output=True, text=True)
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


def _gpu_cmd(out, extra, profile, fixture=GPU_FIXTURE, size=("800", "600")):
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
    """
    return ([RENDER, "-m", os.path.join(ROOT, "assets", fixture), "-x", "-f", "45",
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


def _profiled_run(workdir, tag, extra, screenshot=None, fixture=GPU_FIXTURE, size=("800", "600")):
    """One profiled render. Returns the parsed tables, or None on failure.

    The tables carry an "import" entry as well, because the dedup counters are
    logged rather than tabled and an arm that wanted them would otherwise pay
    for a whole extra render to see one line.
    """
    out = screenshot or os.path.join(workdir, f"gpu_{tag}.ppm")
    r = subprocess.run(_gpu_cmd(out, extra, True, fixture, size), capture_output=True, text=True)
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


def _timing_delta(base, repeat, variant, row="opaque"):
    """(before, after, signed relative delta, run-to-run floor) or None.

    One helper because two arms make the same claim in the same shape -- "this
    config moved the row by X against a floor of Y" -- and the second copy had
    already drifted: it could not tell a MISSING row from one that read 0.000 ms,
    which is the exact confusion gpu-scale's own comment was written to prevent.
    A vanished pass reading 0 scores as a 100% saving.
    """
    b, r, v = base.get(row), repeat.get(row), variant.get(row)
    if b is None or r is None or v is None or b <= 0.0:
        return None
    return b, v, (v - b) / b, abs(b - r) / b


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
    r = subprocess.run(_gpu_cmd(off_ppm, [], False), capture_output=True, text=True)
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
        timing = _timing_delta(full, full2, half)
        if timing is None:
            print(f"  gpu-scale    FAIL  no usable opaque row to compare (full="
                  f"{full.get('opaque')}, half={half.get('opaque')}, "
                  f"repeat={full2.get('opaque')}); the pass was not timed, not cheap")
            failures.append("gpu-scale")
        else:
            before, after, signed, noise = timing
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


def _submit_sum_ok(tables, label, failures, name):
    """seen == instances + culled, for every pass and for the total."""
    bad = []
    rows = dict(tables["submit"])
    for pass_name, row in rows.items():
        if row["meshes seen"] != row["instances"] + row["meshes culled"]:
            bad.append((pass_name, row["meshes seen"], row["instances"], row["meshes culled"]))
    ok = bool(rows) and not bad
    print(f"  {name:<12} {'PASS' if ok else 'FAIL'}  {label}: {len(rows)} passes, "
          f"{'identity holds in each' if ok else f'violations {bad}'}")
    if not ok:
        failures.append(name)


FOREST = os.path.join(ROOT, "out", "bin", "forest")
_FOREST_CHAINS = re.compile(r"Forest: (\d+) LOD chains built, (\d+) refused")
_FOREST_MESHES = re.compile(r"Forest: (\d+) distinct meshes")
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

# Above the terrain aimed up and away, so nothing the scatter placed is in front
# of the camera and forest-cull can assert exact numbers instead of a direction.
FOREST_CAM_AWAY = ["--cam-eye", "0,300,0", "--cam-target", "600,900,600"]


def _forest_run(workdir, tag, extra, cam=None):
    """One profiled forest run, or None if it did not produce a readable report.

    Built in one place for the same reason _gpu_cmd is: several arms here claim
    their two runs differ in exactly one flag, and two hand-written command lists
    would let that stop being true without anything failing.

    Returning None for an unreadable report -- rather than an empty table every
    caller then has to .get() its way around -- is what lets the arms index
    columns directly. An arm reading a missing column as 0 does not fail loudly;
    it compares 0 against 0 and passes.
    """
    out = os.path.join(workdir, f"forest_{tag}.ppm")
    # --no-fog because the app defaults it on: it is a froxel volume with its own
    # accumulator and it costs real time per run, while contributing nothing to
    # the submission counts every arm here reads.
    cmd = ([FOREST, "-x", "-f", "20", "-W", "800", "-H", "450", "--profiler", "--no-fog",
            "-S", out] + (cam or FOREST_CAM) + extra)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
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
    tables["shading"] = shading
    tables["opaque"] = tables["submit"]["opaque"]
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


def _mask_box_luma(pix, w, h, box):
    """Mean linear luma over a fractional box, on a MASK_GRID square grid."""
    x0, y0, x1, y1 = box
    n = MASK_GRID
    samples = [_linear_luma(pix, w, h, (x0 + (x1 - x0) * (ix + 0.5) / n) * w,
                            (y0 + (y1 - y0) * (iy + 0.5) / n) * h)
               for iy in range(n) for ix in range(n)]
    return sum(samples) / len(samples)


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
    ref = _mask_box_luma(pix, w, h, MASK_REF_BOX)
    kept = _mask_box_luma(pix, w, h, MASK_KEPT_BOX)
    gone = _mask_box_luma(pix, w, h, MASK_GONE_BOX)

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
# The prepass costs 8-14% on this fixture. The bar sits above the 1.5% floor
# these renders measure and well below the effect.
CROSSOVER_MIN = 0.05
# Separate from the bar, because one literal doing both jobs would pass a run
# whose floor (4.9%) had swallowed its own signal (5.1%).
CROSSOVER_NOISE_MAX = 0.03


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
    published from engine->msaa_samples, but asking this driver for a 1-sample
    target returns a 2-sample one, so every reading on the TAA path was exactly
    double. A single full-frame quad read 2.00.
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

    base = _profiled_run(workdir, "od_t1", taa + ["--no-sort-opaque"],
                         fixture=OVERDRAW_TILES, size=("400", "300"))
    floor_run = _profiled_run(workdir, "od_t2", taa + ["--no-sort-opaque"],
                              fixture=OVERDRAW_TILES, size=("400", "300"))
    on = _profiled_run(workdir, "od_t3", taa + ["--no-sort-opaque", "--depth-prepass"],
                       fixture=OVERDRAW_TILES, size=("400", "300"))
    if base is None or floor_run is None or on is None:
        return failures + ["prepass-crossover"]

    timing = _timing_delta(base["gpu"], floor_run["gpu"], on["gpu"])
    if timing is None:
        print("  prepass-crossover FAIL  no usable opaque row to compare")
        return failures + ["prepass-crossover"]
    b, o, cost, noise = timing
    # The premise, asserted rather than left in a comment: this fixture has
    # nothing to reject. If it ever gained overlap the arm would quietly be
    # measuring a different scene.
    flat = base["shading"] and abs(base["shading"]["complexity"] - 1.0) <= OVERDRAW_TOLERANCE
    ok = cost >= CROSSOVER_MIN and noise < CROSSOVER_NOISE_MAX and flat
    print(f"  prepass-crossover {'PASS' if ok else 'FAIL'}  opaque {b:.3f} -> {o:.3f} ms with "
          f"the prepass, {cost * 100.0:+.0f}% (want >= +{CROSSOVER_MIN * 100.0:.0f}%: nothing "
          f"to reject at complexity 1.0), against a {noise * 100.0:.0f}% floor "
          f"(want < {CROSSOVER_NOISE_MAX * 100.0:.0f}%); complexity 1.0: {flat}")
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
    r = subprocess.run(cmd + extra, capture_output=True, text=True)
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
    base = _forest_run(workdir, "pp_base", ["--taa", "--headless-jitter", "--no-sort-opaque"])
    pre = _forest_run(workdir, "pp_on",
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
    base = _forest_run(workdir, "base", [])
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
    off = _forest_run(workdir, "noinst", ["--no-instancing"])
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
    nolod = _forest_run(workdir, "nolod", ["--no-lod", "--no-sort-opaque"])
    if nolod is None:
        failures.append("forest-batch")
        failures.append("forest-lod")
    else:
        n = nolod["opaque"]
        n_draws, n_inst = n["draws"], n["instances"]
        ratio = (n_inst / n_draws) if n_draws else 0.0
        ok = ratio >= 8.0
        print(f"  forest-batch {'PASS' if ok else 'FAIL'}  opaque {n_inst} instances in {n_draws} "
              f"draws with LOD and sorting off (ratio {ratio:.1f}, want >= 8)")
        if not ok:
            failures.append("forest-batch")

        # --- forest-lod: level selection fires on generated geometry --------
        # A fixed camera comparing on against off, NOT a distance sweep: pulling
        # back over scattered content reveals more world, so triangles rise with
        # distance however well LOD works. Identical instances is what proves the
        # difference is level selection and not visibility.
        #
        # The two runs also differ in --no-sort-opaque, which the shared nolod
        # run carries for forest-batch above. That is safe HERE and only here:
        # sorting reorders draws without changing which meshes survive the
        # frustum, so it moves neither instances nor triangles -- the only two
        # columns this arm reads.
        same_vis = n_inst == inst
        saving = 1.0 - (opaque["triangles"] / n["triangles"]) if n["triangles"] else 0.0
        ok = same_vis and saving >= 0.10
        print(f"  forest-lod   {'PASS' if ok else 'FAIL'}  {opaque['triangles']} triangles with "
              f"LOD vs {n['triangles']} without ({saving * 100.0:.0f}% saved, want >= 10%), both "
              f"carrying {inst} instances")
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
    sorted_run = _forest_run(workdir, "morton", ["--no-sort-opaque"])
    unsorted_run = _forest_run(workdir, "nosort", ["--no-sort-opaque", "--no-spatial-sort"])
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
    away = _forest_run(workdir, "away", [], cam=FOREST_CAM_AWAY)
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
        # frustum, so seen == culled and draws == 0. `seen` matching the base run
        # is what stops a scene that failed to load from satisfying the rest.
        ok = (a["meshes seen"] == opaque["meshes seen"] and a["meshes culled"] == a["meshes seen"]
              and a["draws"] == 0)
        print(f"  forest-cull  {'PASS' if ok else 'FAIL'}  aimed away: {a['meshes culled']} of "
              f"{a['meshes seen']} culled and {a['draws']} draws (want all of "
              f"{opaque['meshes seen']} culled, 0 draws)")
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
    _submit_sum_ok(base, "base", failures, "submit-sum")

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
        _submit_sum_ok(far, "far camera", failures, "submit-sum-far")

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
            r = subprocess.run(cmd, capture_output=True, text=True)
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
        print("subsurface under an area light (spec 11.19 / B3.2):")
        failures += run_skin_area_gate(workdir)
        print("hair lobes driven by the strand map (spec 11.20 / B8):")
        failures += run_hair_flow_gate(workdir)
        print("lens flare and chromatic aberration (spec 11.21 / B7):")
        failures += run_flare_gate(workdir)
        print("subsurface blur (world width vs frame size):")
        failures += run_sss_invariance_gate(workdir)
        print("subsurface blur (kernel not visible as rings):")
        failures += run_sss_banding_gate(workdir)
        print("output dither (8-bit contour bands, spec 11.24 / E1):")
        failures += run_dither_gate(workdir)
        print("translucent shadows (analytic layer stack, spec 11.26 / C1):")
        failures += run_translucent_shadow_gate(workdir)
        print("translucent shadows (off-path identity and the inverse arm):")
        failures += run_translucent_offpath_gate(workdir)
        print("gpu timing (per-pass queries, spec 11.27 / E4):")
        failures += run_profiler_gate(workdir)
        print("submission (draw counts + the CPU column, spec 11.28 / E5):")
        failures += run_submission_gate(workdir)
        print("draw list (submission order, spec 11.28 Phase 3):")
        failures += run_draw_list_gate(workdir)
        print("LOD chains (selection by projected size, spec 11.28 Phase 6):")
        failures += run_lod_gate(workdir)
        print("alpha mask (binary above the cutoff, spec 11.31):")
        failures += run_mask_gate(workdir)
        print("depth complexity (a scene whose answer is known, spec 11.31):")
        failures += run_overdraw_gate(workdir)
        print("depth prepass (identical picture, less shading, spec 11.30 / E6):")
        failures += run_prepass_gate(workdir)
        print("forest (scattered content: batching, ordering, LOD, spec 11.29):")
        failures += run_forest_gate(workdir)
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
