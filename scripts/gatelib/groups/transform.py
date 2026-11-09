"""The transform group: the transform walk (once a frame, spec 11.96)."""

import os

from gatelib.harness import _bin, _run
from gatelib.probes import _probe_rows


# The only RIGID scene-graph mover in the tree that is also headless
# deterministic -- its sphere rides game->time, so frame N is position N.
SPORES = _bin("spores")


def run_transform_walk_gate(workdir):
    """A moving node keeps a distinct previous pose, and every node's is seeded.

    These exist because their failures are invisible to everything else in this
    suite. All 29 goldens are static scenes under a static camera, so a build
    with every motion vector dead renders 0 px against every one of them --
    measured, not assumed -- and origin-velocity divides by its own steady state,
    so a uniformly dead buffer scores 0 and passes.

    What they guard is the previous-pose half of the walk: scene.c latches it in
    its own pass, and both of the ways that pass can go wrong are silent.

      transform-moves     a node the app moves reads a DISTINCT previous pose,
                          and the step is one frame of its path -- measured
                          against the chord between consecutive samples, so the
                          bar calibrates itself off whatever path the fixture
                          drives. Bounded BOTH ways: no latch at all freezes prev
                          at the frame-0 pose and the ratio grows a frame per
                          sample, no walk at all drives it to 0.
      transform-stills    every node carries a SEEDED previous pose, and the ones
                          the app never moves read prev == global. The seed is
                          the half a pixel arm cannot reach: an unseeded node
                          reports travel from the world origin, which is a smear
                          for one node and most of the frame for a region's worth
                          arriving together.

    Not guarded here because it is no longer representable: walking twice in one
    frame. While the latch was fused into the walk that zeroed every motion
    vector; since the split the walk is idempotent and a second call recomputes
    the same values. Verified by calling it twice on purpose -- these arms stay
    green, where before the split the same mutation drove the ratio to 0.

    Read off a probe rather than off pixels, because the quantity IS a pair of
    CPU-side matrices -- and a velocity buffer would test the shader's upload
    path at the same time, which this spec does not touch. That leaves the
    consumer half of the chain (prev -> uPrevModel -> velocity -> TAA) still
    uninstrumented, which is booked rather than closed here.
    """
    del workdir # the probe is stdout; nothing is written
    if not os.path.exists(SPORES):
        print("  transform-moves SKIP  (spores not built)")
        return []
    arms = ["transform-moves", "transform-stills"]

    failures = []

    # EVERY frame, because `step` is a one-frame delta -- the sampling stride
    # never enters it, so a wider stride buys no distance and costs whole frames.
    # -f 4 is frames 0..3, i.e. three usable rows after frame 0 rather than the
    # two a stride of 4 gave.
    r = _run([SPORES, "-x", "-f", "4", "--transform-probe", "1"],
             capture_output=True, text=True)
    rows = _probe_rows(r.stdout + r.stderr, "transform-probe")
    if r.returncode != 0 or not rows:
        for arm in arms:
            print(f"  {arm} ERROR spores exited {r.returncode} or logged no probe")
        return arms

    # Frame 0 is excluded on purpose: the first walk SEEDS prev from global, so
    # `moved` is legitimately 0 there for everything. An arm that included it
    # would be asserting the seed is broken.
    nodes = [x for x in rows if x.get("kind") == "node" and x.get("frame") != "0"]
    if not nodes:
        for arm in arms:
            print(f"  {arm} ERROR the probe emitted no node rows past frame 0")
        return arms

    # --- moves ---------------------------------------------------------------
    # Bounded BOTH ways, and the upper bound is the half that took a review to
    # notice. `step > 0` alone passes the failure where prev freezes at the
    # frame-0 pose, because step then GROWS -- measured 2.001 and 3.004 against a
    # 0.4% spread on the true build.
    #
    # The bar is the node's own chord between consecutive samples, so it
    # calibrates itself off whatever path the fixture drives and survives
    # anyone retuning it. Measured spread on the true build is 0.4%.
    movers = [x for x in nodes if x.get("moved") == "1"]
    by_name = {}
    for x in sorted(nodes, key=lambda r: int(r["frame"])):
        by_name.setdefault(x["name"], []).append(x)
    ratios = []
    for name, rowsx in by_name.items():
        for a, b in zip(rowsx, rowsx[1:]):
            span = int(b["frame"]) - int(a["frame"])
            pa = [float(v) for v in a["pos"].split(",")]
            pb = [float(v) for v in b["pos"].split(",")]
            chord = sum((pb[i] - pa[i]) ** 2 for i in range(3)) ** 0.5
            if chord > 0.0:
                ratios.append(float(b["step"]) / (chord / span))
    ok = bool(movers) and bool(ratios) and all(0.9 <= r <= 1.1 for r in ratios)
    print(f"  transform-moves {'PASS' if ok else 'FAIL'}  {len(movers)} of {len(nodes)} node rows "
          f"carry a distinct previous pose, and each step is "
          f"{sorted(round(r, 3) for r in ratios)} of the chord it travelled (want 0.9..1.1: "
          f"dropping the latch reads 2.001 and 3.004, dropping the walk reads 0)")
    if not ok:
        failures.append("transform-moves")

    # --- stills --------------------------------------------------------------
    # `valid`, NOT `step`. The first draft compared step against 0 on rows whose
    # `moved` was already 0 -- and moved==0 IS a memcmp over the whole mat4, so
    # the translation is bit-equal and step is exactly 0 by construction. It
    # asserted nothing, and passed every mutation it named.
    #
    # prev_valid is what actually carries the seed: delete the seeding branch and
    # it reads 0 on every row forever, while a node that was never moved still
    # reports prev == global because create_node leaves both identity. That is
    # the failure this arm is for, and it is the one the pixel corpus cannot see
    # -- a node whose prev is unseeded reports travel from the world origin.
    stills = [x for x in nodes if x.get("moved") == "0"]
    unseeded = sorted({x["name"] for x in nodes if x.get("valid") != "1"})
    ok = bool(stills) and not unseeded
    print(f"  transform-stills {'PASS' if ok else 'FAIL'}  {len(stills)} pinned node rows report "
          f"prev == global and all {len(nodes)} carry a seeded previous pose"
          f"{'' if not unseeded else f', except {unseeded}'} (want every row seeded: an unseeded "
          f"node reports a trip from the world origin, which no golden in this corpus can see)")
    if not ok:
        failures.append("transform-stills")

    return failures
