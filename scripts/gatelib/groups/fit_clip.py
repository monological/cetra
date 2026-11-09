"""The fit-clip group: a fitted camera's near and far planes (spec 13.57)."""

import json
import math
import os

from gatelib.harness import RENDER, _run, asset
from gatelib.scenes import BOARD_VIEWS, RENDER_CAM


# A fitted camera's planes (spec 13.57), held to the fit worked out here from the board's own
# glTF, every node of which is a unit shape at its translation and scale. The margins and the
# ratio are camera.c's and camera.h's.
FIT_FAR_MARGIN, FIT_NEAR_MARGIN, FIT_RATIO_MAX = 1.1, 0.9, 1e4
# An eye inside the table's box, looking along it.
FIT_INSIDE_VIEW = ["--cam-eye", "0.1,-0.005,0.1", "--cam-target", "0.1,-0.005,0"]
# An eye beside the table looking away from it, so no box reaches the frustum.
FIT_ASIDE_VIEW = ["--cam-eye", "0.5,0.02,0", "--cam-target", "0.5,0.02,-1"]
# How far a sea with no bounds is fitted out from under the eye, x the typical size (engine.c).
FIT_SEA_REACH = 1000.0
# How far a traced plane may sit from this file's answer, relative: the engine fits in float.
FIT_REL_TOL = 1e-4


def _board_boxes():
    """The board fixture's world boxes, one a node."""
    with open(asset("board_fixture.gltf")) as f:
        nodes = json.load(f)["nodes"]
    return [([t - s / 2 for t, s in zip(n["translation"], n["scale"])],
             [t + s / 2 for t, s in zip(n["translation"], n["scale"])]) for n in nodes]


def _fit_expected(boxes, eye, target, fov_degrees, aspect):
    """(near, far, nearest depth, nearest distance) for a camera fitting `boxes`: far past the
    farthest depth of a box in front of the eye, near short of the larger of the nearest depth
    and the nearest distance times the frustum corner's cosine -- the second dropped past the
    farthest depth, where no box reaches the frustum -- never under far / the ratio."""
    f = [t - e for t, e in zip(target, eye)]
    length = math.sqrt(sum(c * c for c in f))
    f = [c / length for c in f]
    nearest_depth = nearest_dist = math.inf
    farthest = 0.0
    for lo, hi in boxes:
        mid = sum(((a + b) / 2 - e) * d for a, b, e, d in zip(lo, hi, eye, f))
        spread = sum((b - a) / 2 * abs(d) for a, b, d in zip(lo, hi, f))
        if mid + spread <= 0.0:
            continue
        nearest_depth = min(nearest_depth, max(mid - spread, 0.0))
        gap = [max(a - e, 0.0, e - b) for a, b, e in zip(lo, hi, eye)]
        nearest_dist = min(nearest_dist, math.sqrt(sum(g * g for g in gap)))
        farthest = max(farthest, mid + spread)
    far = farthest * FIT_FAR_MARGIN
    k = math.tan(math.radians(fov_degrees) / 2)
    bound = nearest_dist / math.sqrt(1 + k * k * (1 + aspect * aspect))
    nearest = max(nearest_depth, bound) if bound <= farthest else nearest_depth
    return max(nearest * FIT_NEAR_MARGIN, far / FIT_RATIO_MAX), far, nearest_depth, nearest_dist


def run_fit_clip_gate(workdir):
    """A fitted camera's near and far planes (spec 13.57), read off --trace-camera.

    The answer is worked out here from the board's glTF in double precision, so a plane that
    moves is held to the geometry rather than to a recording of itself.

      fit-bracket  at each of the board's three views the traced planes are the fit's: far 1.1
                   times the farthest depth of a box in front of the eye, near 0.9 times the
                   larger of the nearest depth and the nearest distance times the frustum
                   corner's cosine. Planes that were not fitted, or a fit that missed a box,
                   are not within FIT_REL_TOL of these.
      fit-inside   an eye inside the table's box, where the nearest depth and distance are both
                   0, takes the floor: near is far / 1e4.
      fit-aside    an eye beside the table looking away from it: the table is in front of the
                   eye but no part of it is in the frustum, so its distance says nothing and near
                   is the floor. Taken as a bound, it put near past far.
      fit-sea      water_fixture's sea, which has no bounds: far reaches SEA_REACH, where a sea
                   with no box once left the camera the planes it had.
    """
    failures = []
    boxes = _board_boxes()
    with open(asset("board_fixture.cscn")) as f:
        fov = json.load(f)["camera"]["fov"]
    width, height = 400, 300

    def trace(view, flags, scene="board_fixture.cscn"):
        """The last traced camera row for `scene` under `flags`, or None."""
        out = os.path.join(workdir, f"fit_{view}.ppm")
        r = _run([RENDER, "-m", asset(scene), "-x", "-f", "2", "-W", str(width),
                  "-H", str(height), "-S", out, "--trace-camera", *flags],
                 capture_output=True, text=True)
        rows = RENDER_CAM.findall(r.stdout + r.stderr)
        return rows[-1] if r.returncode == 0 and rows else None

    def fitted(view, flags):
        """(traced near, traced far, expected) for the board under `flags`, or None."""
        row = trace(view, flags)
        if row is None:
            return None
        eye, target = [float(x) for x in row[1:4]], [float(x) for x in row[4:7]]
        return (float(row[11]), float(row[12]),
                _fit_expected(boxes, eye, target, fov, width / height))

    def close(got, want):
        return abs(got - want) <= FIT_REL_TOL * want

    ok, parts = True, []
    for view, flags in BOARD_VIEWS.items():
        got = fitted(view, flags)
        if got is None:
            ok = False
            parts.append(f"{view}: no trace")
            continue
        near, far, (want_near, want_far, _, _) = got
        ok &= close(near, want_near) and close(far, want_far)
        parts.append(f"{view} near {near * 1e3:.3g} mm far {far * 1e3:.3g} mm "
                     f"(want {want_near * 1e3:.3g}, {want_far * 1e3:.3g})")
    print(f"  fit-bracket  {'PASS' if ok else 'FAIL'}  {'; '.join(parts)}")
    if not ok:
        failures.append("fit-bracket")

    got = fitted("inside", FIT_INSIDE_VIEW)
    if got is None:
        ok, detail = False, "no trace"
    else:
        near, far, (want_near, want_far, depth, dist) = got
        ok = (depth == 0.0 and dist == 0.0 and close(near, far / FIT_RATIO_MAX)
              and close(near, want_near) and close(far, want_far))
        detail = (f"near {near * 1e3:.4g} mm against far / 1e4 {far / FIT_RATIO_MAX * 1e3:.4g} mm, "
                  f"the eye's nearest depth {depth} and distance {dist} (want 0 and 0)")
    print(f"  fit-inside   {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("fit-inside")

    got = fitted("aside", FIT_ASIDE_VIEW)
    if got is None:
        ok, detail = False, "no trace"
    else:
        near, far, (want_near, want_far, _, _) = got
        ok = near < far and close(near, want_near) and close(far, want_far)
        detail = (f"near {near * 1e3:.4g} mm, far {far * 1e3:.4g} mm "
                  f"(want {want_near * 1e3:.4g} and {want_far * 1e3:.4g}, near under far)")
    print(f"  fit-aside    {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("fit-aside")

    row = trace("sea", ["--fit-clip"], scene="water_fixture.cscn")
    if row is None:
        ok, detail = False, "no trace"
    else:
        near, far = float(row[11]), float(row[12])
        ok = far >= FIT_SEA_REACH and near < far
        detail = f"near {near:.4g} m, far {far:.4g} m (want far >= {FIT_SEA_REACH:g}, near under it)"
    print(f"  fit-sea      {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("fit-sea")
    return failures
