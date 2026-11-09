"""The shadow and lighting-stream helpers several groups share."""

import json
import math
import os

from gatelib.harness import RENDER, _run, asset
from gatelib.images import _linear_luma, _read_ppm, compare
from gatelib.scenes import _projector


def _crossing(xs, vals, umbra, lit, frac):
    """First up-crossing of umbra + frac*(lit-umbra), lerped between samples."""
    t = umbra + frac * (lit - umbra)
    for i in range(1, len(vals)):
        if vals[i - 1] < t <= vals[i]:
            a = (t - vals[i - 1]) / (vals[i] - vals[i - 1])
            return xs[i - 1] + a * (xs[i] - xs[i - 1])
    return float("nan")


# Cascade-shadow gates (spec 10.5). Same design as the penumbra gate: geometry
# whose answer is computable before rendering, measured on the shadow term alone
# (shadowed frame divided by a --no-shadows frame).
#
# Mirrors assets/scenes/dir_shadow_fixture.cscn / _lowsun.cscn and its generator
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
# Churn bound: TAA-jittered frames never repeat byte-for-byte, so the shadowed
# sequence is judged against the --no-shadows run of the same scene. 2x
# absorbs the shadow term legitimately participating in the jittered edges it
# darkens; the +100 px absorbs count noise on a near-zero floor.
DIR_CHURN_FACTOR = 2
DIR_CHURN_SLACK_PX = 100


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
    cmd = [RENDER, "-m", asset(scene), "-x", "-f", "30",
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


def _taa_churn(workdir, fixture, tag, extra, frames=120, first=90):
    """AE between frame `first` and the final frame of one static-camera TAA run.

    The defaults are the original pair, 90 against 120 -- two fully CONVERGED
    frames, which is what a caller asking "does this feature keep churning after
    the history fills" wants.

    ADJACENT FRAMES ARE A DIFFERENT QUESTION and the parameters exist for it: two
    converged frames differ by whatever the accumulator has failed to settle,
    while consecutive ones also carry the per-frame jitter error. Spec 11.103
    needed the second -- a TAA jitter with the right magnitude and the wrong SIGN
    settles to a stable image and is invisible at 90-vs-120, reading 0.86 of the
    perspective leg where the correct sign reads 0.88.

    The second frame is always the one `-S` writes, so a caller changes the pair
    by moving `first` and `frames` together. The screenshot INTERVAL is derived
    rather than passed: it has to divide `first` or that file is never written
    and the run reports an error instead of a reading, and a caller with two
    numbers to keep consistent is a caller that will one day not. `first` must
    also be below `frames` -- equal, the two operands are the same frame and the
    AE is 0, which is a silently passing arm rather than a failure. None on
    error.
    """
    if first >= frames or first < 1:
        print(f"  churn        ERROR {tag} wants 1 <= first < frames, got {first}/{frames}")
        return None
    base = os.path.join(workdir, f"churn_{tag}.ppm")
    cmd = [RENDER, "-m", fixture, "-x", "-f", str(frames), "--no-auto-exposure", "-E", "1.0",
           "--taa", "--headless-jitter", "--screenshot-every", str(first),
           "-W", "800", "-H", "600", "-S", base] + extra
    r = _run(cmd, capture_output=True, text=True)
    f_first = base[:-4] + f"_{first:06d}.ppm"
    # Both frames can exist and still be wrong if the run died between them, so
    # the exit code is checked as well as the files.
    if r.returncode != 0:
        print(f"  churn        ERROR {tag} exited {r.returncode}: "
              f"{(r.stdout + r.stderr).strip()[-300:]}")
        return None
    if not (os.path.exists(f_first) and os.path.exists(base)):
        return None
    return compare(f_first, base)[0]


# Lighting data that streams (spec 13.24): ten rooms in a row, each with its own GI volume, two
# reflection probes and two bodied cached lights -- more of each than the resident caps hold.
LSTREAM_FIXTURE = "stream_rooms.cscn"
LSTREAM_PROP_ROOM = 3
LSTREAM_TILE_LIGHTS = 16  # bodied lights the cached tiles hold: 768 cells of 48


def _lstream_rooms():
    """Each room's centre x, read from the fixture's own GI volumes."""
    with open(asset(LSTREAM_FIXTURE)) as f:
        d = json.load(f)
    return [0.5 * (v["boxMin"][0] + v["boxMax"][0]) for v in d["giVolumes"]], d


def _lstream_pose(x):
    return (x, 1.6, 1.8), (x, 1.0, -2.0)


def _lstream_cam_eye(x):
    eye, target = _lstream_pose(x)
    return ["--cam-eye", ",".join(f"{c:g}" for c in eye),
            "--cam-target", ",".join(f"{c:g}" for c in target)]
