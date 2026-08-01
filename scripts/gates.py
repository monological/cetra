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

    def crossing(frac):
        t = umbra + frac * (lit - umbra)
        for i in range(1, len(vals)):
            if vals[i - 1] < t <= vals[i]:
                a = (t - vals[i - 1]) / (vals[i] - vals[i - 1])
                return xs[i - 1] + a * (xs[i] - xs[i - 1])
        return float("nan")

    lo, mid, hi = crossing(0.10), crossing(0.50), crossing(0.90)
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
    "elev_deg": 40.0,
    "elev_low_deg": 10.0,
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


def run_dir_shadow_gate(workdir):
    fixture = os.path.join(ROOT, "assets", "dir_shadow_fixture.cscn")
    if not os.path.exists(fixture):
        print("  dir-shadow   SKIP  (missing dir_shadow_fixture.cscn)")
        return []
    failures = []

    # --- punctual grazing (cornell_leak wall base) --------------------------
    # Rendered from the leak room, not the fixture: the wedge is punctual-path
    # fallout and must be fixed in the reference implementation before the
    # cascade port copies it (spec 10.5 phase 2a).
    shadow = ref = None
    leak = os.path.join(ROOT, "assets", "cornell_leak.gltf")
    if os.path.exists(leak):
        outs = {}
        for tag, extra in (("leak", []), ("leak_ns", ["--no-shadows"])):
            out = os.path.join(workdir, f"dir_{tag}.ppm")
            cmd = [RENDER, "-m", leak, "-x", "-f", "30", "--no-auto-exposure", "-E", "1.0",
                   "-W", "800", "-H", "600", "-S", out] + LEAK_CAM + extra
            r = subprocess.run(cmd, capture_output=True, text=True)
            outs[tag] = _read_ppm(out) if r.returncode == 0 and os.path.exists(out) else None
        shadow, ref = outs["leak"], outs["leak_ns"]
    if shadow and ref:
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
        if not ok:
            failures.append("dir-grazing")
    else:
        print("  grazing      ERROR while rendering cornell_leak")
        failures.append("dir-grazing")

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
        # No silent caps: enough of the umbra must actually be measured.
        if measured < 30:
            raise ValueError(f"only {measured} umbra samples visible ({skipped} occluded)")
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
        mid = float("nan")
        thresh = umbra + 0.5 * (lit - umbra)
        for i in range(1, len(vals)):
            if vals[i - 1] < thresh <= vals[i]:
                a = (thresh - vals[i - 1]) / (vals[i] - vals[i - 1])
                mid = zs[i - 1] + a * (zs[i] - zs[i - 1])
                break
        ok = abs(mid - want_edge) <= DIR_EDGE_TOL
        print(f"  edge-{label:<7} {'PASS' if ok else 'FAIL'}  crossing {mid:.4f} "
              f"(want {want_edge:.4f} +/-{DIR_EDGE_TOL})")
        if not ok:
            failures.append(f"dir-edge-{label}")

    # --- churn: shadows must not add temporal instability under TAA ---------
    # Static camera, jitter on (windowed parity). Frames 90 and 120 of the same
    # run are compared; the --no-shadows pair is the noise floor everything else
    # in the pipeline contributes, and the shadowed pair must stay at it.
    churn = {}
    for tag, extra in (("taa", []), ("taa_ns", ["--no-shadows"])):
        base = os.path.join(workdir, f"dir_{tag}.ppm")
        cmd = [RENDER, "-m", fixture, "-x", "-f", "120", "--no-auto-exposure", "-E", "1.0",
               "--taa", "--headless-jitter", "--screenshot-every", "30",
               "-W", "800", "-H", "600", "-S", base] + hard + extra
        subprocess.run(cmd, capture_output=True, text=True)
        f90 = base[:-4] + "_000090.ppm"
        f120 = base[:-4] + "_000120.ppm"
        churn[tag] = compare(f90, f120)[0] if (os.path.exists(f90) and os.path.exists(f120)) \
            else None
    if churn["taa"] is None or churn["taa_ns"] is None:
        print("  churn        ERROR while rendering the TAA sequences")
        failures.append("dir-churn")
    else:
        floor = churn["taa_ns"]
        ok = churn["taa"] <= floor * 2 + 100
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
            got = float(line.split("range=")[1].split(",")[0])
    if got is None:
        print("  gltf-range   ERROR (no light imported)")
        return ["gltf-range"]
    ok = abs(got - 25.0) < 1e-3
    print(f"  gltf-range   {'PASS' if ok else 'FAIL'}  authored 25.0, imported {got}")
    return [] if ok else ["gltf-range"]


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
        print("cascade shadow (analytic ellipse):")
        failures += run_dir_shadow_gate(workdir)
        print("import:")
        failures += run_range_gate()
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
