"""The region group: region residency (spec 11.63)."""

import math
import os

from gatelib.harness import FOREST
from gatelib.world import ORIGIN_AUTO, REGION_CHURN, REGION_WALK, _FOREST_TRACE, _region_run


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
