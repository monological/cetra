"""The cull group: wind and skinned geometry is cullable (spec 11.53)."""

import json
import os

from gatelib.harness import asset, render
from gatelib.images import compare
from gatelib.probes import _probe_render
from gatelib.profiling import (
    SUBMIT_CASCADES, _fixture_mesh_nodes, _profiled_run, _submit_sum_detail,
)


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
        if not os.path.exists(asset(f"{name}.cscn")):
            print(f"  cull         SKIP  (missing {name}.cscn)")
            return []

    failures = []

    # Read from the fixtures rather than mirrored here, for the reason
    # _fixture_mesh_nodes states: a hand-copied count goes stale when the
    # generator changes and the arm passes against whatever it was told.
    wind_meshes = _fixture_mesh_nodes(f"{WIND}.gltf")
    with open(asset(f"{WIND}.gltf")) as f:
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
        scene = asset(f"{fixture}.cscn")
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
    rows, _ = _probe_render(asset(f"{WIND}.cscn"),
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
