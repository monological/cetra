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


def cscn_copy(src, dst, mutate):
    """Copy a .cscn through `mutate(dict)`, with model paths made absolute.

    Generating a twin mechanically rather than committing a hand-edited one is the point:
    the two halves then cannot differ in anything except what `mutate` touched, which is
    the property every arm built on one of these asserts.

    The path absolutization is the half worth having in one place. It is a quiet
    correctness requirement -- model paths resolve against the scene file's directory, so
    an out-of-tree copy must carry absolute ones -- and a copy that forgets it fails as a
    render error rather than as a mismatch.
    """
    with open(src) as f:
        d = json.load(f)
    mutate(d)
    for m in d.get("models", []):
        if not os.path.isabs(m["path"]):
            m["path"] = os.path.join(os.path.dirname(os.path.abspath(src)), m["path"])
    with open(dst, "w") as f:
        json.dump(d, f, indent=1)


def scaled_copy(src, dst, factor):
    """Multiply every authored intensity by `factor` and divide exposure by it."""
    def scale(d):
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

    cscn_copy(src, dst, scale)


def render(scene, out, extra, frames=30):
    cmd = [RENDER, "-m", scene, "-x", "-f", str(frames), "-W", "400", "-H", "300", "-S", out]
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
    r = subprocess.run(cmd, capture_output=True, text=True)
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


def run_absorption_gate(workdir):
    """Volume absorption scales with path length, and only in the tinted channels.

    Three arms, none implying the others:

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
# before phase 2. Three boxes down the submerged ramp: it is opaque, and with the eye
# under the surface there is no water interface between it and the camera, so the ONLY
# thing that can absorb it is the froxel volume. Farther is further down the frame
# here (the ramp recedes downward and narrows), and R/B falls 1.35 / 1.19 / 0.85.
WATER_FOG_BED_BOXES = [(0.40, 0.390, 0.60, 0.420),
                       (0.40, 0.460, 0.60, 0.490),
                       (0.40, 0.540, 0.60, 0.570)]
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
# the five, which is what makes this the only instrument on that path -- the same argument
# water-cscn makes for absorption.
WATER_SEASTATE_REF = {"waves": "fft"}
WATER_SEASTATE_CALM = {"waves": "fft", "windSpeed": 4.0}
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
# geometry rather than an oversight: its sun sits at azimuth 135 and elevation 26, so the
# mirror direction lands about 24 degrees below the bottom of a 42 degree frame -- measured
# 0 px there with the lobe on and off. So the arm puts the sun ahead and low, which is the
# geometry a glitter path needs and the one every photograph of one was taken in.
WATER_GLITTER_SUN = {"sun_elevation": 14.0, "sun_azimuth": 180.0}
WATER_GLITTER_CAMERA = {"eye": [0.0, 1.6, 7.0], "target": [0.0, 0.55, 0.0], "fov": 45}
# Spectral, because the lobe's width is the slope the surface stopped resolving and the
# spectral path is where that is a measured quantity rather than four dropped octaves.
WATER_GLITTER_WATER = {"waves": "fft"}
# Measured 20,963 px. The lobe is ADDITIVE, so the second half asserts a direction as well
# as a magnitude: a sun that darkened the sea would be a sign error passing a pixel count.
WATER_GLITTER_MIN_PX = 5000
WATER_GLITTER_BOX = (0.55, 0.28, 0.95, 0.40)

# Persistent foam (spec 11.42). A ROUGH sea, because whitecaps are selected from the
# horizontal map folding and the default 11.5 m/s state folds rarely enough that the signal
# is a few per cent -- 20 m/s is the same instrument with the effect above its own noise,
# and it is only reachable at all because spec 11.42 made the sea state authorable.
#
# 90 frames, not 30: the accumulator has to have something to remember. At 30 the trail is
# barely longer than the crest that made it.
WATER_FOAM_SEA = {"waves": "fft", "windSpeed": 20.0, "level": 0.6, "extent": 70.0}
WATER_FOAM_FRAMES = 90
# Open water only -- above the ramp's crest and below the horizon band, so no dry geometry
# and no sky is inside it. The ramp is irrelevant to this arm: crest foam is a property of
# the wave field, where the SHORE band next door is a property of the bed, needs
# --water-bed dome, and is identically zero on this fixture without one.
#
# PERSIST rather than FOAM in the name, because WATER_FOAM_OPEN_BOX already exists for
# water-shore-foam and is a different box on a different scene. The first version of this
# reused that name, the later definition won, and the arm measured the shore box on an
# open-water framing and read 0 -- a green-looking 0/0 rather than an error.
WATER_PERSIST_OPEN_BOX = (0.05, 0.20, 0.95, 0.35)
# Measured 6,608 -> 7,490 foam pixels (+13.3%) and 85,604 px over the frame. The ratio is
# the load-bearing half: a count alone would pass on a sea that simply got brighter.
WATER_FOAM_PERSIST_RATIO = 1.07
WATER_FOAM_PERSIST_MIN_PX = 20000
# And `level` is a field both paths can set, so authoring it must land in exactly the
# same place the flag does. 0 px or one of them is lying.
WATER_CSCN_LEVEL = 0.9

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
# Measured 0.63x on the mid box; the floor is loose enough to survive a retune of the
# shoal window and still fail an inert bed.
WATER_SHOAL_MAX_RATIO = 0.80
WATER_SHOAL_OPEN_TOL = 0.05

# The shore foam band, on the GERSTNER path -- which is what isolates it. Crest foam is
# selected from Jacobian compression and the Gerstner map's steepness is clamped so it
# cannot compress, so on that path the shore band is the only foam there is. Measured
# 1,306 px in the band with a bed and 0 at both extremes, 0 everywhere with no bed.
WATER_FOAM_LUMA_MIN = 0.21
WATER_FOAM_RB_MIN = 0.55
# Windowed on BOTH sides of the shoal factor, so the band has to be a band: at shoal 0
# the surface is about to be discarded and at 1 there is no shore to foam against.
# Nearer is further down the frame here (the camera sits over the crown), so the
# fully-shoaled box is the low one and the open-water box is the high one.
WATER_FOAM_OPEN_BOX = (0.02, 0.10, 0.32, 0.24)
WATER_FOAM_BAND_BOX = (0.02, 0.26, 0.32, 0.31)
WATER_FOAM_SHOALED_BOX = (0.02, 0.55, 0.32, 0.90)
WATER_FOAM_BAND_MIN_PX = 300

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
# entirely inside these rows and this x range, so the measurement window is where the
# effect is and nowhere else.
WATER_SHORE_WINDOW = (0.30, 0.47, 0.70, 0.555)
# One flag apart at 4x MSAA. Measured 4,352 px at peak 44/255 -- small because most of
# this edge was never a shader edge: the water surface CROSSES the bed here, and the
# per-sample depth test antialiases a depth crossing on its own. Coverage carries the
# part the depth test cannot see, which is the threshold sliver the discard removes.
WATER_SHORE_MIN_PX = 1500
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
# surface draws over it.
WATER_DRY_BOX = (0.44, 0.42, 0.56, 0.46)
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
    r = subprocess.run(cmd, capture_output=True, text=True)
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


def _water_fft_probe(extra, scene=None):
    """Run --water-fft-probe and return (per-cascade rows, impulse dict).

    Empty results are a failure at every call site here, unlike _water_probe where
    declining is one of the results: the flag is only ever passed with a spectral surface
    already asked for, so nothing to measure means the surface did not survive the flags.
    """
    cmd = [RENDER, "-m", scene or os.path.join(ROOT, "assets", WATER_FIXTURE), "-x", "-f", "4",
           "-W", "200", "-H", "150", "--water-fft-probe"] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    rows = []
    impulse = {}
    for line in (r.stdout + r.stderr).splitlines():
        if not line.startswith("water-fft-probe "):
            continue
        parts = line.split()[1:]
        # Three line shapes: a header of key=value, the impulse pair, and the per-cascade
        # rows, which are the only ones leading with a bare index.
        if parts[0] == "impulse":
            impulse = {k: float(v) for k, v in (p.split("=", 1) for p in parts[1:])
                       if k != "available"}
            continue
        if "=" in parts[0]:
            continue
        row = {"cascade": int(parts[0])}
        row.update({k: float(v) for k, v in (p.split("=", 1) for p in parts[1:])})
        rows.append(row)
    return rows, impulse


def _water_glitter_variant(src, dst):
    """Copy the fixture with the sun ahead and low, and the camera facing it.

    Three blocks rather than the water one alone, which is why this is not a
    _water_cscn_variant call: the lobe is a property of where the sun IS relative to the
    eye, so the framing is the instrument and the water block only picks the wave model.
    """
    def mutate(d):
        d["environment"].update(WATER_GLITTER_SUN)
        d["camera"] = dict(WATER_GLITTER_CAMERA)
        d.setdefault("water", {}).update(WATER_GLITTER_WATER)

    cscn_copy(src, dst, mutate)


def _water_cscn_variant(src, dst, overrides):
    """Copy a .cscn with its water block overridden."""
    cscn_copy(src, dst, lambda d: d.setdefault("water", {}).update(overrides))


def _water_roughness(pix, w, h, box):
    """Per-pixel linear-luma standard deviation in a fractional box.

    A wave, to a box of pixels, is spread: the surface tilts, so the reflection it
    returns varies across the box. A shoaled surface has lost its displacement and
    returns nearly one value. The MEAN would not see this at all -- calm and choppy
    water average to much the same place.
    """
    x0, y0, x1, y1 = box
    vals = [_linear_luma(pix, w, h, px, py)
            for py in range(int(y0 * h), int(y1 * h))
            for px in range(int(x0 * w), int(x1 * w))]
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
      water-shore-soft the shoreline is antialiased rather than cut off: the frame
                      moves against --no-water-coverage AND the sharpest single-row
                      fall across the waterline gets shallower. The second half is
                      what makes this a softening claim and not just a liveness one.
      water-shore-hard the same flag at ONE sample is 0 px. Alpha-to-coverage has
                      nothing to dither into there, so the shader must fall back to
                      the cutoff -- if it kept the fractional fragment instead, the
                      sliver would be written at full strength.
      water-cscn      the scene file's water block reaches the surface, and the flags
                      override it rather than the reverse. absorption is read because
                      no flag can set it, so the frame can only have moved through the
                      authoring path.
      water-horizon   the surface reaches the horizon, asserted as REACH INVARIANCE:
                      multiplying the nominal extent by 14,000 must not move the water's
                      top edge, because an edge already at the vanishing line cannot go
                      higher. Needs no horizon row and no camera parameters. The clipmap
                      fails it by 71 px, which is what it was written against.
      water-farfield  the far field is FILTERED rather than aliased, the filtering is
                      confined to the far field, and the slope energy it removes arrives
                      as roughness rather than being dropped. Read against
                      --no-water-lod, which reports a zero footprint and so reaches the
                      unfiltered surface exactly. The direction of the roughness handover
                      is not measurable here -- see WATER_FAR_ROUGH_AUTHORED.
      water-foam-persist whitewater OUTLIVES the crest that made it. Read against
                      --no-water-foam-history, which selects foam from this frame's fold
                      alone and is the pre-11.42 behaviour exactly. On open water and on a
                      rough sea for reasons that are both in WATER_FOAM_SEA.
      water-glitter   the sea has a specular response to its own sun, and it BRIGHTENS.
                      Under the procedural sky the environment cubemap carries no disc, so
                      before spec 11.42 there was none at all -- and the lobe's width comes
                      from the slope the surface stopped resolving, so it cannot be read
                      off the fixture's own framing. See WATER_GLITTER_SUN.
      water-fft-var   the transformed field carries the variance the SEEDING predicted,
                      per band and in both height and slope. The first arm here that
                      reads the spectrum rather than a picture of it, and the only one
                      that can fail on a transform which is deterministic, differs from
                      Gerstner, and is still wrong. Blind to a missed fftshift, which
                      moves the field in space and not in variance -- that is
                      water-fft-impulse's half.
      water-fixture-roundtrip the fixture's GENERATOR still produces the fixture. No
                      renders: it regenerates into a temp directory and compares text.
                      Exists because gen_water_fixture.py stopped emitting the `water`
                      block when 11.33 made that block create the surface, and stayed that
                      way for three specs -- so the docstring's "regenerate with" line was
                      an instruction to strip the water and fail every arm below.
      water-fft-impulse the transform matches its CLOSED FORM on two single modes: a
                      centred impulse must come back constant, and its neighbour as one
                      cycle across the grid. Run through the same 14 stages and the same
                      twiddle table the sea uses, so it tests this transform rather than
                      a copy. Verified by breaking the shader -- see
                      WATER_FFT_IMPULSE_MAX for what the rest of the suite did then.
      water-seastate  the spectral sea state reaches the seeding. Wind speed, fetch,
                      depth, peak enhancement and swell are authorable since spec 11.42
                      and NO flag can set any of them, so a scene file is the only way in
                      and this is the only arm on that path. The wind DIRECTION reaches
                      both models now and is covered by water-fft-live moving with it.
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

    dry = _water_rb(pix, w, h, WATER_DRY_BOX)
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
            bed = [_water_rb(pix2, w2, h2, box) for box in WATER_FOG_BED_BOXES]
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
        lum_on = _water_box_luma(g_on_pix, gw, gh, WATER_GLITTER_BOX)
        lum_off = _water_box_luma(g_off_pix, gw, gh, WATER_GLITTER_BOX)
        ok = ae_glit >= WATER_GLITTER_MIN_PX and lum_on > lum_off
        print(f"  water-glitter {'PASS' if ok else 'FAIL'}  {ae_glit} px vs no glitter "
              f"(want >={WATER_GLITTER_MIN_PX}), sun box {lum_off:.4f} -> {lum_on:.4f} "
              f"(want brighter)")
        if not ok:
            failures.append("water-glitter")

    # Foam outlives the crest that made it. Read on open water, where the fold is the only
    # thing that can select whitewater -- the shore band is a different mechanism and is
    # identically zero here without a bed.
    foam_scene = os.path.join(workdir, "water_foam.cscn")
    _water_cscn_variant(scene, foam_scene, WATER_FOAM_SEA)
    foam_on = os.path.join(workdir, "water_foam_on.ppm")
    foam_off = os.path.join(workdir, "water_foam_off.ppm")
    err = render(foam_scene, foam_on, WATER_PIN + WATER_NO_CATCHER,
                 frames=WATER_FOAM_FRAMES)
    if not err:
        err = render(foam_scene, foam_off,
                     WATER_PIN + WATER_NO_CATCHER + ["--no-water-foam-history"],
                     frames=WATER_FOAM_FRAMES)
    if err:
        print(f"  water-foam-persist ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-foam-persist")
    else:
        ae_foam, _ = compare(foam_off, foam_on)
        fw, fh, f_on_pix = _read_ppm(foam_on)
        _, _, f_off_pix = _read_ppm(foam_off)
        px_on = _water_foam_px(f_on_pix, fw, fh, WATER_PERSIST_OPEN_BOX)
        px_off = _water_foam_px(f_off_pix, fw, fh, WATER_PERSIST_OPEN_BOX)
        ratio = px_on / max(px_off, 1)
        ok = ratio >= WATER_FOAM_PERSIST_RATIO and ae_foam >= WATER_FOAM_PERSIST_MIN_PX
        print(f"  water-foam-persist {'PASS' if ok else 'FAIL'}  open-water foam {px_off} "
              f"-> {px_on} = {ratio:.2f}x (want >={WATER_FOAM_PERSIST_RATIO}), {ae_foam} px "
              f"over the frame (want >={WATER_FOAM_PERSIST_MIN_PX})")
        if not ok:
            failures.append("water-foam-persist")

    # The transform carries the variance the seeding predicted. The first arm in this
    # suite that reads the SPECTRUM rather than a picture of it.
    var_rows, impulse = _water_fft_probe(WATER_PIN + ["--water-waves", "fft"])
    if len(var_rows) != WATER_CASCADES:
        print(f"  water-fft-var FAIL  --water-fft-probe printed {len(var_rows)} cascades, "
              f"want {WATER_CASCADES}")
        failures.append("water-fft-var")
    else:
        hr = [r["height_ratio"] for r in var_rows]
        sr = [r["slope_ratio"] for r in var_rows]
        ok = all(WATER_FFT_VAR_MIN <= v <= WATER_FFT_VAR_MAX for v in hr + sr)
        print(f"  water-fft-var {'PASS' if ok else 'FAIL'}  measured/predicted height "
              f"{'/'.join(f'{v:.2f}' for v in hr)} slope {'/'.join(f'{v:.2f}' for v in sr)} "
              f"(want {WATER_FFT_VAR_MIN}-{WATER_FFT_VAR_MAX} on all six)")
        if not ok:
            failures.append("water-fft-var")

    # The transform against its closed form. The only arm in this suite that can fail on
    # a transform which is deterministic, differs from Gerstner, and is still wrong.
    if not impulse:
        print("  water-fft-impulse FAIL  --water-fft-probe printed no impulse result")
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
    r = subprocess.run([sys.executable, gen, regen_dir], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  water-fixture-roundtrip FAIL  generator exited {r.returncode}: "
              f"{(r.stderr or r.stdout).strip()[-200:]}")
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
        ok = not drifted
        print(f"  water-fixture-roundtrip {'PASS' if ok else 'FAIL'}  regenerated 2 files, "
              f"{'all identical to the committed pair' if ok else 'DRIFTED: ' + ', '.join(drifted)}")
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
        ratio = crest / max(ref, 1e-9)
        ok = ratio >= WATER_WATERLINE_MIN_RATIO
        print(f"  water-waterline {'PASS' if ok else 'FAIL'}  crest/foreground "
              f"{crest:.4f}/{ref:.4f} = {ratio:.4f} (want "
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
    err = None if not fft_ok else render(scene, dome, WATER_FFT_FLAGS + WATER_DOME_BED)
    if not fft_ok:
        print("  water-shoal  SKIP  (the spectral render this compares against failed)")
    elif err:
        print(f"  water-shoal  ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-shoal")
    else:
        wd, hd, pixd = _read_ppm(dome)
        wn, hn, pixn = _read_ppm(fa)
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
        ok = (band >= WATER_FOAM_BAND_MIN_PX and at_open == 0 and at_shoal == 0 and
              none_band == 0)
        print(f"  water-shore-foam {'PASS' if ok else 'FAIL'}  band {band} px "
              f"(want >={WATER_FOAM_BAND_MIN_PX}), open water {at_open} and fully "
              f"shoaled {at_shoal} (want 0 at both extremes), same box with no bed "
              f"{none_band} (want 0)")
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
        soft_fall = _water_shore_fall(a, WATER_SHORE_WINDOW)
        hard_fall = _water_shore_fall(hard, WATER_SHORE_WINDOW)
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
# The prepass costs 8-18% on this fixture; the bar sits well below that effect.
# The floor is NOT reliably below it -- it has been observed at 5% -- which is
# why the separation rule below exists rather than a ceiling on the floor.
CROSSOVER_MIN = 0.05
# The effect must clear the MEASURED floor by this much -- a ratio, not a fixed
# ceiling on the floor itself. A fixed ceiling was tried twice and flaked twice:
# at ~3 ms absolute this driver's single-run timings wander 0.2-5% (probed x3),
# and one base pair per suite run samples that spread once. A run whose floor
# came up 5% under an 18% effect is a sound measurement at 3.6x separation; a
# rule that rejects it is asserting on the weather. The ratio keeps the honest
# property the ceiling was for -- a floor that swallows its own signal still
# fails -- without failing on a wide floor under a wider effect.
CROSSOVER_SEPARATION = 3.0


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
    base = _profiled_run(workdir, "od_t1", taa + ["--no-sort-opaque"],
                         fixture=OVERDRAW_TILES, size=("800", "600"))
    floor_run = _profiled_run(workdir, "od_t2", taa + ["--no-sort-opaque"],
                              fixture=OVERDRAW_TILES, size=("800", "600"))
    on = _profiled_run(workdir, "od_t3", taa + ["--no-sort-opaque", "--depth-prepass"],
                       fixture=OVERDRAW_TILES, size=("800", "600"))
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
    ok = cost >= CROSSOVER_MIN and cost >= CROSSOVER_SEPARATION * noise and flat
    print(f"  prepass-crossover {'PASS' if ok else 'FAIL'}  opaque {b:.3f} -> {o:.3f} ms with "
          f"the prepass, {cost * 100.0:+.0f}% (want >= +{CROSSOVER_MIN * 100.0:.0f}%: nothing "
          f"to reject at complexity 1.0), against a {noise * 100.0:.0f}% floor "
          f"(want {CROSSOVER_SEPARATION:.0f}x separation); complexity 1.0: {flat}")
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


def _run_import_gates(workdir):
    del workdir # both drive the importer directly rather than a render
    return run_range_gate() + run_fbx_unit_gate()


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
    r = subprocess.run(cmd, capture_output=True, text=True)
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
    r = subprocess.run(cmd, capture_output=True, text=True)
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
GATE_GROUPS = [
    ("scale", "scale invariance (lights x1000, exposure /1000):", run_scale_gates),
    ("penumbra", "area shadow (analytic penumbra):", run_penumbra_gate),
    ("grazing", "punctual grazing (leak wall base):", run_grazing_gate),
    ("dir-shadow", "cascade shadow (analytic ellipse):", run_dir_shadow_gate),
    ("catcher", "catcher over a real ground (contact fixture):", run_catcher_gate),
    ("catcher-transparency", "catcher vs transparency (panel through the plane):",
     run_catcher_transparency_gate),
    ("oit", "order-independent transparency (analytic card stack):", run_oit_gate),
    ("absorption", "volume absorption (path length and channel selectivity, spec 11.32):",
     run_absorption_gate),
    ("fog-volume", "local fog volumes (density, tint, arming; spec 11.39):",
     run_fog_volume_gate),
    ("cloud-shadow", "cloud shadows into the fog and onto the ground (specs 11.39, 11.41):",
     run_cloud_shadow_gate),
    ("water", "water surface (determinism, absorption, shoreline, reach; specs 11.32-11.35):",
     run_water_gate),
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
    ("translucent", "translucent shadows (analytic layer stack, spec 11.26 / C1):",
     run_translucent_shadow_gate),
    ("translucent-offpath", "translucent shadows (off-path identity and the inverse arm):",
     run_translucent_offpath_gate),
    ("profiler", "gpu timing (per-pass queries, spec 11.27 / E4):", run_profiler_gate),
    ("submission", "submission (draw counts + the CPU column, spec 11.28 / E5):",
     run_submission_gate),
    ("draw-list", "draw list (submission order, spec 11.28 Phase 3):", run_draw_list_gate),
    ("lod", "LOD chains (selection by projected size, spec 11.28 Phase 6):", run_lod_gate),
    ("mask", "alpha mask (binary above the cutoff, spec 11.31):", run_mask_gate),
    ("varying", "varyings under partial coverage (spec 11.38):", run_varying_gate),
    ("sss-tag", "subsurface profile tag through the MSAA resolve (spec 11.37):",
     run_sss_tag_gate),
    ("overdraw", "depth complexity (a scene whose answer is known, spec 11.31):",
     run_overdraw_gate),
    ("prepass", "depth prepass (identical picture, less shading, spec 11.30 / E6):",
     run_prepass_gate),
    ("forest", "forest (scattered content: batching, ordering, LOD, spec 11.29):",
     run_forest_gate),
    ("import", "import:", _run_import_gates),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="keep the generated scenes and frames")
    ap.add_argument("--only", metavar="SEL",
                    help="run only groups whose selector contains SEL (comma-separated)")
    ap.add_argument("--list", action="store_true", help="list group selectors and exit")
    args = ap.parse_args()

    if args.list:
        for selector, banner, _ in GATE_GROUPS:
            print(f"  {selector:<22} {banner}")
        return 0

    if not os.path.exists(RENDER):
        sys.exit(f"{RENDER} not found -- run ./build.sh first")

    groups = GATE_GROUPS
    if args.only:
        wanted = [s.strip() for s in args.only.split(",") if s.strip()]
        groups = [g for g in GATE_GROUPS if any(w in g[0] for w in wanted)]
        # A selector that matches nothing is a typo, and running zero gates while
        # reporting success is the worst outcome this script has.
        if not groups:
            sys.exit(f"--only {args.only!r} matched no group; --list to see them")

    workdir = tempfile.mkdtemp(prefix="cetra_gates_")
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
