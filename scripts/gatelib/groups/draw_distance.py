"""The draw-distance group: a node's draw distance, the camera's alone (spec 13.38)."""

import os

from gatelib.harness import asset
from gatelib.images import compare
from gatelib.profiling import OCCLUSION_MAT, SUBMIT_CASCADES, SUBMIT_FIXTURE, _profiled_run


# A draw distance on the submission fixture's root that cuts most of what the camera sees and
# leaves some, and one past everything (spec 13.38).
DRAW_DISTANCE_CUT = "8"
DRAW_DISTANCE_PAST = "100000"
# The occlusion fixture's four walls, its occluders under the material variant.
DRAW_DISTANCE_WALLS = ("occl_wall_left", "occl_wall_right", "occl_wall_top", "occl_wall_bottom")


def run_draw_distance_gate(workdir):
    """A node's draw distance: what the camera draws of it past that distance (spec 13.38).

      dd-past        a distance past everything on the submission fixture's root: every count
                     of the base run and 0 px
      dd-cut         a distance that cuts most of it: the camera's opaque pass culls more than
                     the base run, and the shadow cascades' row is the base run's to the integer
                     -- a light still takes what the camera does not draw
      dd-occluder    the occlusion fixture's walls, its occluders, past their distance: the
                     camera culls the four walls and nothing behind them, the same count with
                     occlusion culling on as off -- an occluder the camera does not draw hides
                     nothing

    Counted claims, off the submission table, on the occlusion group's reasoning: a pixel
    identity alone passes a draw distance that cuts nothing. Falsified at 13.38: with an occluder
    past its distance still rasterised, dd-occluder culls 304 with occlusion on -- the walls and
    the 300 behind them -- against 4 off.
    """
    if not os.path.exists(asset(SUBMIT_FIXTURE)) or not os.path.exists(asset(OCCLUSION_MAT)):
        print("  dd-past      SKIP  (missing a fixture)")
        return []
    failures = []
    size = ("400", "300")

    def run(tag, extra, fixture=SUBMIT_FIXTURE):
        shot = os.path.join(workdir, f"dd_{tag}.ppm")
        tables = _profiled_run(workdir, f"dd_{tag}", extra, screenshot=shot, fixture=fixture,
                               size=size, frames="2")
        return (tables, shot) if tables else (None, None)

    cascades = ["--shadow-cascades", str(SUBMIT_CASCADES)]
    base, base_shot = run("base", cascades)
    past, past_shot = run("past", cascades + ["--draw-distance", "root", DRAW_DISTANCE_PAST])
    cut, _ = run("cut", cascades + ["--draw-distance", "root", DRAW_DISTANCE_CUT])
    if base is None or past is None or cut is None:
        return ["draw-distance"]

    def rows(tables):
        return dict(tables["submit"])

    differ = compare(past_shot, base_shot)[0]
    ok = rows(past) == rows(base) and differ == 0
    print(f"  dd-past      {'PASS' if ok else 'FAIL'}  a draw distance of {DRAW_DISTANCE_PAST} m: "
          f"submission table {'the base run' if rows(past) == rows(base) else 'NOT the base run'}"
          f", {differ} px from it (want the same table, 0 px)")
    if not ok:
        failures.append("dd-past")

    opaque_b = rows(base).get("opaque", {}).get("meshes culled")
    opaque_c = rows(cut).get("opaque", {}).get("meshes culled")
    shadow_b = rows(base).get("shadow cascades")
    shadow_c = rows(cut).get("shadow cascades")
    ok = (opaque_b is not None and opaque_c is not None and opaque_c > opaque_b
          and shadow_b is not None and shadow_c == shadow_b)
    print(f"  dd-cut       {'PASS' if ok else 'FAIL'}  a draw distance of {DRAW_DISTANCE_CUT} m: "
          f"the camera's opaque pass culls {opaque_c} against {opaque_b} (want more), the "
          f"shadow cascades {'unchanged' if shadow_c == shadow_b else f'{shadow_c} against {shadow_b}'}"
          f" (want unchanged)")
    if not ok:
        failures.append("dd-cut")

    walls = [a for w in DRAW_DISTANCE_WALLS for a in ("--draw-distance", w, "0.01")]
    on, _ = run("walls_on", walls, fixture=OCCLUSION_MAT)
    off, _ = run("walls_off", walls + ["--no-occlusion-cull"], fixture=OCCLUSION_MAT)
    if on is None or off is None:
        failures.append("dd-occluder")
    else:
        culled_on = rows(on).get("opaque", {}).get("meshes culled")
        culled_off = rows(off).get("opaque", {}).get("meshes culled")
        ok = culled_on == culled_off == len(DRAW_DISTANCE_WALLS)
        print(f"  dd-occluder  {'PASS' if ok else 'FAIL'}  the walls past their draw distance: "
              f"the camera culls {culled_on} with occlusion on and {culled_off} off (want "
              f"{len(DRAW_DISTANCE_WALLS)} both, the walls alone)")
        if not ok:
            failures.append("dd-occluder")
    return failures
