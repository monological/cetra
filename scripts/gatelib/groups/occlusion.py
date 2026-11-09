"""The occlusion group: occlusion culling (authored occluders, masked software depth, spec
11.98).
"""

import json
import os

from gatelib.harness import asset
from gatelib.images import compare
from gatelib.probes import _probe_render
from gatelib.profiling import (
    OCCLUSION_MAT, _fixture_mesh_nodes, _profiled_run, _submit_sum_detail, _timing_delta,
)


OCCLUSION_FIXTURE = "occlusion_fixture.cscn"
OCCLUSION_BARE = "occlusion_fixture_bare.cscn"
# The inside-the-room camera is a FOURTH .cscn variant rather than --cam-eye
# flags, because a hardcoded coordinate here cannot be kept honest: the
# generator's asserts constrain its own inside_eye, which tracks the room's
# authored height automatically, so nothing would ever compare a literal in
# this file against the fixture. A variant makes the drift unrepresentable.
OCCLUSION_INSIDE = "occlusion_fixture_inside.cscn"
# The camera almost touching a wall slab, so the derived near plane clips the
# wall open and the renderer shows the room through the shell. The regression
# it pins: a raster standing any face the eye is not in front of -- the
# pre-classification back face -- deletes 118,794 px of what the near clip
# made visible.
OCCLUSION_NEAR = "occlusion_fixture_near.cscn"
# The probe's two instrument floors, the CULL_BOUND_FLOOR pattern: floors on
# the INSTRUMENT, not the engine. catch measured 0.9413 on the portal sweep --
# the hierarchy giving ground would lower it, never raise it -- and 1534 of
# 2048 sweep boxes landed reference-hidden; a sweep starved of hidden boxes is
# a safety property tested against nothing (the first sweep shipped at 13 of
# 2048 and would have proven anything).
OCC_CATCH_FLOOR = 0.80
OCC_SWEEP_MIN_HIDDEN = 800
# The crossover's exclusion bar: effects under this are too small to close a
# roadmap row on, whatever `separated` says. The regression test is `separated`
# itself, non-overlap over four interleaved pairs -- the reading order the
# prepass crossover settled.
OCC_WIN_MIN = 0.05


def run_occlusion_gate(workdir):
    """Occlusion culling: authored occluders, masked software depth (spec 11.98).

      occl-hidden    the portal room's opaque pass culls EXACTLY the occl_hidden_*
                     population -- as wrong when it culls some as when it culls none.
      occl-material  the material row alone reaches the same exact count.
      occl-doorway   0 px against --no-occlusion-cull, with the marginal mesh's
                     sliver in frame -- conservatism at the jamb, where an
                     inward-rounded footprint deletes a visible sliver.
      occl-inside    the camera inside the room: 0 px, and the culled count equals
                     the no-occlusion run's (frustum work, nothing added).
      occl-near      the camera almost touching a slab, near plane clipping the
                     wall open: 0 px and equal counts -- the one framing where a
                     non-front face standing in for the shell culls VISIBLE
                     geometry, invisible to every other arm's camera.
      occl-bare      the variant with NO occluders key: 0 px and equal counts --
                     the empty-context path no flag can reach.
      occl-sum       seen == instances + culled on the profiled run: occlusion
                     rejections fold into the ONE counter every existing gate reads.
      occl-shadow    the shadow-cascade submit rows are IDENTICAL on vs off --
                     occlusion is camera-only, and a caster hidden from the camera
                     still casts.
      occl-probe     the brute-force twin, two-sided: hierarchical-hidden implies
                     reference-hidden with ZERO exceptions over the scene and a
                     seeded sweep, AND the catch rate clears a floor so the probe
                     cannot pass by hiding nothing. Plus the interior contract on
                     every flagged proxy, box against its own triangles.
      occl-fragment  the measured LOSS: on the scatter twin, culling EXACTLY the
                     pillared columns' instances SPLITS one instanced run --
                     draws up while triangles down, as integers off the .gltf's
                     own name classes. The clock is printed as context only.
      occl-crossover the measured WIN: the portal room's opaque row, steady
                     state, interleaved pairs, non-overlap required. What closes
                     roadmap row 37 with a number instead of an opinion.

    The corpus's first fixtures with real occlusion in them: everything else is
    either fully visible or frustum-culled, so a culler that hides on-screen
    geometry -- or hides nothing -- is green everywhere but here. The counted
    claims are integers off the submission table, on the cull group's reasoning:
    a pixel identity alone passes a culler that culls nothing.
    """
    if not os.path.exists(asset(OCCLUSION_FIXTURE)):
        print("  occlusion    SKIP  (missing fixture)")
        return []
    failures = []
    size = ("400", "300")
    hidden = _fixture_mesh_nodes("occlusion_fixture.gltf", prefix="occl_hidden_")

    def identity_pair(tag, fixture):
        """An on/off run pair with screenshots: (ae, culled_on, culled_off), or
        None on a failed render. The CALLER prints -- gate-arm-docs reads this
        group's own source for the arm names it runs."""
        p_on = os.path.join(workdir, f"{tag}_on.ppm")
        p_off = os.path.join(workdir, f"{tag}_off.ppm")
        r_on = _profiled_run(workdir, f"{tag}_on", [], screenshot=p_on, fixture=fixture,
                             size=size, frames="30")
        r_off = _profiled_run(workdir, f"{tag}_off", ["--no-occlusion-cull"],
                              screenshot=p_off, fixture=fixture, size=size, frames="30")
        if r_on is None or r_off is None:
            return None
        ae, _ = compare(p_on, p_off)
        return (ae, r_on["submit"].get("opaque", {}).get("meshes culled"),
                r_off["submit"].get("opaque", {}).get("meshes culled"))

    on_ppm = os.path.join(workdir, "occl_on.ppm")
    off_ppm = os.path.join(workdir, "occl_off.ppm")
    on = _profiled_run(workdir, "occl_on", [], screenshot=on_ppm,
                       fixture=OCCLUSION_FIXTURE, size=size, frames="30")
    off = _profiled_run(workdir, "occl_off", ["--no-occlusion-cull"], screenshot=off_ppm,
                        fixture=OCCLUSION_FIXTURE, size=size, frames="30")
    if on is None or off is None:
        return ["occlusion"]

    # --- occl-hidden: the exact count, and the exact remainder --------------
    opaque = on["submit"].get("opaque")
    if opaque is None:
        print("  occl-hidden  FAIL  no opaque submission row")
        failures.append("occl-hidden")
    else:
        seen = opaque["meshes seen"]
        culled = opaque["meshes culled"]
        instances = opaque["instances"]
        ok = culled == hidden and instances == seen - hidden and seen > hidden
        print(f"  occl-hidden  {'PASS' if ok else 'FAIL'}  culled {culled} of {seen} "
              f"(want exactly {hidden}, the occl_hidden_* population; "
              f"{instances} drawn, want {seen - hidden})")
        if not ok:
            failures.append("occl-hidden")

    # --- occl-material: the row is a real second channel --------------------
    mat = _profiled_run(workdir, "occl_mat", [], fixture=OCCLUSION_MAT,
                        size=("400", "300"), frames="30")
    if mat is None or mat["submit"].get("opaque") is None:
        print("  occl-material FAIL  no opaque submission row")
        failures.append("occl-material")
    else:
        culled = mat["submit"]["opaque"]["meshes culled"]
        ok = culled == hidden
        print(f"  occl-material {'PASS' if ok else 'FAIL'}  the material-row variant "
              f"culled {culled} (want {hidden}: the wall slabs' own AABBs must reach "
              f"the buffer through the row alone)")
        if not ok:
            failures.append("occl-material")

    # --- occl-doorway: the conservative identity ----------------------------
    ae, _ = compare(on_ppm, off_ppm)
    ok = ae == 0
    print(f"  occl-doorway {'PASS' if ok else 'FAIL'}  {ae} px between culled and "
          f"--no-occlusion-cull (want exactly 0: the marginal sliver at the jamb is "
          f"the first thing a rounding mistake deletes)")
    if not ok:
        failures.append("occl-doorway")

    # --- occl-inside: from inside the room, occlusion adds nothing ----------
    # Culled counts are compared BETWEEN runs rather than against zero: the
    # inside camera legitimately frustum-culls the room behind it, and equality
    # is the claim that occlusion added nothing on top.
    inside = identity_pair("occl_in", OCCLUSION_INSIDE)
    if inside is None:
        failures.append("occl-inside")
    else:
        ae, c_on, c_off = inside
        ok = ae == 0 and c_on is not None and c_on == c_off
        print(f"  occl-inside  {'PASS' if ok else 'FAIL'}  {ae} px and culled "
              f"{c_on} == {c_off} (want 0 px and equality: the wall is behind the "
              f"camera, so its boxes must contribute nothing)")
        if not ok:
            failures.append("occl-inside")

    # --- occl-near: the near plane opens the wall; the raster must agree ----
    near = identity_pair("occl_near", OCCLUSION_NEAR)
    if near is None:
        failures.append("occl-near")
    else:
        ae, c_on, c_off = near
        ok = ae == 0 and c_on is not None and c_on == c_off
        print(f"  occl-near    {'PASS' if ok else 'FAIL'}  {ae} px and culled "
              f"{c_on} == {c_off} (want 0 px and equality: the near clip shows the "
              f"room through the slab, so no face may stand across the frame)")
        if not ok:
            failures.append("occl-near")

    # --- occl-bare: the path no flag reaches --------------------------------
    bare = identity_pair("occl_bare", OCCLUSION_BARE)
    if bare is None:
        failures.append("occl-bare")
    else:
        ae, c_on, c_off = bare
        ok = ae == 0 and c_on is not None and c_on == c_off
        print(f"  occl-bare    {'PASS' if ok else 'FAIL'}  {ae} px and culled "
              f"{c_on} == {c_off} on the no-occluders variant (the empty-context "
              f"early-out, which no flag can select and no other arm reads)")
        if not ok:
            failures.append("occl-bare")

    # --- occl-sum: rejections land in the one denominator -------------------
    ok, detail = _submit_sum_detail(on, "portal")
    print(f"  occl-sum     {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("occl-sum")

    # --- occl-shadow: camera-only, structurally -----------------------------
    s_on = on["submit"].get("shadow cascades")
    s_off = off["submit"].get("shadow cascades")
    if s_on is None or s_off is None:
        print(f"  occl-shadow  FAIL  no shadow-cascades row (on={s_on is not None}, "
              f"off={s_off is not None}); the fixture's sun casts")
        failures.append("occl-shadow")
    else:
        ok = s_on == s_off
        print(f"  occl-shadow  {'PASS' if ok else 'FAIL'}  cascade rows "
              f"{'identical' if ok else f'differ: {s_on} vs {s_off}'} between on and "
              f"off (a caster hidden from the camera still casts)")
        if not ok:
            failures.append("occl-shadow")

    # --- occl-probe: the brute-force twin, both jaws -------------------------
    # The raster path is SHARED between the buffer and the reference by design
    # (three consumers that cannot round differently), so what this verifies is
    # the HIERARCHY -- the tile fold, the footprint rounding, the mask --
    # against a per-pixel test at 4x resolution over the identical box set.
    # The identity arms above carry the raster half.
    rows, _ = _probe_render(asset(OCCLUSION_FIXTURE),
                            "--occlusion-probe", "occlusion-probe")
    mat_rows, _ = _probe_render(asset(OCCLUSION_MAT),
                                "--occlusion-probe", "occlusion-probe")
    bare_rows, _ = _probe_render(asset(OCCLUSION_BARE),
                                 "--occlusion-probe", "occlusion-probe")
    summary = next((r for r in rows if r.get("kind") == "summary"), None)
    sweep = next((r for r in rows if r.get("kind") == "sweep"), None)
    items = [r for r in rows if r.get("kind") == "item"]
    bare_head = next((r for r in bare_rows if r.get("kind") == "header"), None)
    proxies = [r for r in mat_rows if r.get("kind") == "proxy"]
    if summary is None or sweep is None or not items or bare_head is None:
        print("  occl-probe   FAIL  probe rows missing (summary/sweep/items/bare header)")
        failures.append("occl-probe")
    else:
        problems = []
        # Jaw one: safety, zero exceptions, scene and sweep alike.
        if int(summary["violations"]) != 0 or int(sweep["violations"]) != 0:
            problems.append(f"violations scene={summary['violations']} "
                            f"sweep={sweep['violations']} (want 0: hierarchical-hidden "
                            f"must imply reference-hidden)")
        # Jaw two: efficacy with the vacuity guard.
        if int(sweep["ref_hidden"]) < OCC_SWEEP_MIN_HIDDEN:
            problems.append(f"sweep ref_hidden {sweep['ref_hidden']} < "
                            f"{OCC_SWEEP_MIN_HIDDEN}: the safety jaw is tested "
                            f"against nothing")
        elif float(sweep["catch"]) < OCC_CATCH_FLOOR:
            problems.append(f"catch {sweep['catch']} < {OCC_CATCH_FLOOR}")
        # The fixture's own classes, cross-checked by the exact raster.
        bad_hidden = [r["name"] for r in items if r["name"].startswith("occl_hidden_")
                      and (r["hier"] != "1" or r["ref"] != "1")]
        bad_door = [r["name"] for r in items if r["name"].startswith("occl_door")
                    and r["hier"] != "0"]
        if bad_hidden:
            problems.append(f"{len(bad_hidden)} occl_hidden_* not hidden by both tests")
        if bad_door:
            problems.append(f"door meshes culled: {bad_door}")
        # The empty path answers by name.
        if bare_head.get("available") != "0" or bare_head.get("reason") != "nooccluders":
            problems.append(f"bare header {bare_head} (want available=0 reason=nooccluders)")
        # The interior contract on the material-flagged walls.
        bad_proxy = [r["name"] for r in proxies
                     if int(r["poke"]) != 0 or int(r["near"]) != 0]
        if not proxies:
            problems.append("no proxy rows on the material variant")
        elif bad_proxy:
            problems.append(f"interior contract violated: {bad_proxy}")
        ok = not problems
        print(f"  occl-probe   {'PASS' if ok else 'FAIL'}  "
              + (f"0 violations over {len(items)} items + {sweep['boxes']} sweep boxes, "
                 f"catch {sweep['catch']} (floor {OCC_CATCH_FLOOR}), "
                 f"{len(proxies)} proxies clean" if ok else "; ".join(problems)))
        if not ok:
            failures.append("occl-probe")

    # --- occl-fragment: where the feature loses, as integers ----------------
    # One shared mesh in one instanced run, a pillar fence hiding every third
    # column: culling removes exactly the pillared columns' instances and
    # SPLITS the run -- draws up while triangles down. The culled count is
    # EXACT, derived from the .gltf's own name classes, on cull-wind's rule: a
    # build that culls "some" is as wrong as one that culls none. The clock is
    # printed as context only; at this size the GPU timer's own jitter dwarfs
    # it, which is the crossover's whole reason for existing.
    with open(asset("occlusion_scatter.gltf")) as f:
        sc_names = [n.get("name", "") for n in json.load(f).get("nodes", [])]
    pillar_cols = {n.split("_")[-1] for n in sc_names if n.startswith("occl_pillar_")}
    sc_hidden = sum(1 for n in sc_names
                    if n.startswith("occl_inst_") and n.split("_")[2] in pillar_cols)
    sc_on = _profiled_run(workdir, "occl_sc_on", [], fixture="occlusion_scatter.cscn",
                          size=size, frames="30")
    sc_off = _profiled_run(workdir, "occl_sc_off", ["--no-occlusion-cull"],
                           fixture="occlusion_scatter.cscn", size=size, frames="30")
    if sc_on is None or sc_off is None or sc_on["submit"].get("opaque") is None or \
            sc_off["submit"].get("opaque") is None:
        print("  occl-fragment FAIL  no opaque submission rows on the scatter twin")
        failures.append("occl-fragment")
    else:
        d_on = sc_on["submit"]["opaque"]["draws"]
        d_off = sc_off["submit"]["opaque"]["draws"]
        t_on = sc_on["submit"]["opaque"]["triangles"]
        t_off = sc_off["submit"]["opaque"]["triangles"]
        c_on = sc_on["submit"]["opaque"]["meshes culled"]
        c_off = sc_off["submit"]["opaque"]["meshes culled"]
        g_on = sc_on["gpu"].get("opaque", 0.0)
        g_off = sc_off["gpu"].get("opaque", 0.0)
        ok = c_on == sc_hidden and c_off == 0 and d_on > d_off and t_on < t_off
        print(f"  occl-fragment {'PASS' if ok else 'FAIL'}  culled {c_on} of want exactly "
              f"{sc_hidden} (off run {c_off}, want 0); draws {d_off} -> {d_on} while "
              f"triangles {t_off} -> {t_on} (want draws UP, triangles DOWN); opaque "
              f"clock {g_off:.3f} -> {g_on:.3f} ms as context")
        if not ok:
            failures.append("occl-fragment")

    # --- occl-crossover: the measured win, roadmap row 37's number ----------
    # The prepass crossover's whole design, copied: 800x600 because the GPU
    # timer's own jitter dwarfs the effect at 400x300; -f 150 because a
    # 45-frame run publishes the warm-up window; INTERLEAVED pairs so clock and
    # thermal drift land on both configs; `separated` as the regression test
    # with OCC_WIN_MIN only excluding an effect too small to care about.
    runs_on, runs_off = [], []
    for pair in range(4):
        a = _profiled_run(workdir, f"occl_x_on{pair}", [], fixture=OCCLUSION_FIXTURE,
                          size=("800", "600"), frames="150")
        b = _profiled_run(workdir, f"occl_x_off{pair}", ["--no-occlusion-cull"],
                          fixture=OCCLUSION_FIXTURE, size=("800", "600"), frames="150")
        if a is not None:
            runs_on.append(a["gpu"])
        if b is not None:
            runs_off.append(b["gpu"])
    timing = _timing_delta(runs_on, runs_off)
    if timing is None:
        print("  occl-crossover FAIL  no usable opaque rows across the interleave")
        failures.append("occl-crossover")
    else:
        on_ms, off_ms, delta, floor, separated = timing
        ok = delta >= OCC_WIN_MIN and separated
        print(f"  occl-crossover {'PASS' if ok else 'FAIL'}  opaque {on_ms:.3f} ms culled vs "
              f"{off_ms:.3f} ms not ({delta * 100.0:+.0f}%, want >= {OCC_WIN_MIN * 100.0:.0f}% "
              f"and separated={separated}), floor {floor * 100.0:.0f}% over 4 interleaved "
              f"pairs")
        if not ok:
            failures.append("occl-crossover")

    return failures
