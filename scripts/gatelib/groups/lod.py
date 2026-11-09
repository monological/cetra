"""The lod group: LOD chains (selection by projected size, spec 11.28 Phase 6)."""

import json
import os

from gatelib.harness import asset
from gatelib.profiling import LOD_FIXTURE, LOD_SWEEP, _lod_run, _profiled_run
from gatelib.scenes import SKIN_FIXTURE


def _skin_fixture_triangles():
    """Triangle count of the skinned fixture, read from the glTF it generated."""
    path = asset(SKIN_FIXTURE)
    with open(path) as f:
        gltf = json.load(f)
    prim = gltf["meshes"][0]["primitives"][0]
    return gltf["accessors"][prim["indices"]]["count"] // 3


def run_lod_gate(workdir):
    """LOD chains: that one gets built, that distance selects down it, that the
    flag reaches it, and that skinned geometry is refused one.

    Everything here is an integer out of the SUBMISSION table's `triangles`
    column, which is the ONLY counter a level change moves -- switching level
    leaves draws and instances exactly where they were, so an arm watching those
    could not tell working selection from none.
    """
    if not os.path.exists(asset(LOD_FIXTURE)):
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
