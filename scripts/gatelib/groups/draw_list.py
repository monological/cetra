"""The draw-list group: draw list (submission order, spec 11.28 Phase 3)."""

import os

from gatelib.harness import asset, render
from gatelib.images import compare


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
        path = asset(fixture)
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
    fixture = asset("oit_cards_fixture.cscn")
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
