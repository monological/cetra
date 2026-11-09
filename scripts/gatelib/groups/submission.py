"""The submission group: submission (draw counts + the CPU column, spec 11.28 / E5)."""

import json
import re
import os

from gatelib.harness import ROOT, asset
from gatelib.images import compare
from gatelib.profiling import (
    NON_PASS_ROWS, SUBMIT_CASCADES, SUBMIT_COLS, SUBMIT_FIXTURE, _fixture_mesh_nodes, _profiled_run,
    _submit_sum_detail, _within,
)
from gatelib.scenes import SKIN_FIXTURE


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


def run_submission_gate(workdir):
    if not os.path.exists(asset(SUBMIT_FIXTURE)):
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
    with open(asset("instancing_fixture.gltf")) as f:
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

    # --- shadow-batch: the depth pass groups its own casters ----------------
    # inst-count above only asks that the shadow pass batches AT ALL
    # (draws < instances), which a run finder walking contiguous list entries
    # satisfies while still splitting on every culled item and every LOD change.
    # This asks how WELL, against the only ceiling there is: the chunk size.
    #
    # The depth map is order-independent, so this pass reorders its casters to
    # group them -- which is why it can be held to a bar the camera lane cannot.
    # The camera lane is depth-sorted and must stay so.
    #
    # This is the ONLY arm that can see the compaction half of 11.90 -- a culled
    # item no longer ending a run. forest-shadow-lod cannot: compaction is
    # present on both sides of its LOD-on/LOD-off pair and cancels exactly. It
    # is in turn blind to the LEVEL half, because this fixture's two meshes are
    # under LOD_MIN_TRIANGLES so every item here sits at level 0. Two arms, two
    # mechanisms; neither is redundant and neither is sufficient.
    #
    # The bar is DERIVED, the way inst-count derives its own: summing ceil over
    # k groups is at most ceil of the sum plus k-1, so that bound is exact at
    # full visibility and slackens by one draw per group as cascades cull.
    # An earlier form compared against the opaque pass at 0.9x, which was worse
    # twice over -- the "handicap" it claimed cancels in a ratio (equal sets
    # drawn three times triple both terms), so the 0.9 was really undeclared
    # slack against culling; and it tied a shadow assertion to a lane whose
    # fragmentation is a tuning knob, in the direction that makes this arm
    # weaker as that knob moves.
    s_draws, s_inst = shadow.get("draws", 0), shadow.get("instances", 0)
    groups = 2 * SUBMIT_CASCADES  # the glTF's two meshes, once per cascade
    want = -(-s_inst // inst_max) + groups - 1
    ok = 0 < s_draws <= want
    print(f"  shadow-batch {'PASS' if ok else 'FAIL'}  {s_inst} casters in {s_draws} draws "
          f"(want 0 < draws <= {want} = ceil({s_inst}/{inst_max}) + {groups} - 1)")
    if not ok:
        failures.append("shadow-batch")

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
        # docs/verification.md names as able to move every pixel in the frame.
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
            if name not in NON_PASS_ROWS and not _within(ms, frame_ms)}
    timed_ok = _within(cpu.get("TIMED", 0.0), frame_ms)
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
                 if ms == 0.0 and n not in NON_PASS_ROWS]
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
