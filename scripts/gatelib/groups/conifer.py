"""The conifer group: the conifer form of the tree generator (spec 13.35)."""

from gatelib.harness import _probe_render, _probe_rows, asset


# The tree generator's fixture (spec 13.35): a `trees` block whose every tree is there for an arm
# below, written by gen_conifer_fixture.py.
CONIFER_FIXTURE = "conifer_fixture.cscn"
# The recursive form's digests as they stood before the excurrent form shared its generator:
# captured before the first edit to tree_gen.c, and taken again under the engine's own fnv1a64
# from a build the first digests still passed on. Broadleaf seed 42 is the tree viewer's own
# tree, dead seed 4049 silent's first dead-tree model. Bark first, then leaves.
CONIFER_IDENTITY = {
    ("broadleaf", 42): ("20d86c66be025d26", "9631df773244ec08"),
    ("dead", 4049): ("54082ad5be3d631c", "88201fb960ff6465"),
}
CONIFER_PRESETS = ("spruce", "fir", "snag")
CONIFER_SEEDS = (1, 2, 3)
CONIFER_TRI_BUDGET = 10000  # bark and needles together, one tree at full detail


def _conifer_probe():
    """Every tree in the conifer fixture through the render app's --tree-probe, keyed by
    (preset, seed, irregularity) -- the irregularity the file authored, None where it left the
    preset's own: its digest row, and for a conifer its shape row and its levels of detail. One run
    of one frame."""
    rows, text = _probe_render(asset(CONIFER_FIXTURE), "--tree-probe", "tree-probe", frames=1)
    trees = {}
    for r in rows:
        irr = r["irregularity"]
        trees[r["i"]] = {"preset": r["preset"], "seed": int(r["seed"]),
                         "irregularity": None if irr == "preset" else round(float(irr), 3),
                         "bark_tris": int(r["bark_tris"]), "leaf_tris": int(r["leaf_tris"]),
                         "bark": r["bark"], "leaves": r["leaves"]}
    for r in _probe_rows(text, "tree-shape"):
        if r["i"] in trees:
            trees[r["i"]]["shape"] = {k: float(v) for k, v in r.items() if k != "i"}
    for r in _probe_rows(text, "tree-lod"):
        if r["i"] in trees:
            trees[r["i"]]["lod"] = {
                "keep": float(r["keep"]),
                "sprays": [int(r[f"sprays{k}"]) for k in range(int(r["leaf_levels"]))]}
    return {(t["preset"], t["seed"], t["irregularity"]): t for t in trees.values()}


def run_conifer_gate(workdir):
    """The conifer form of the tree generator (spec 13.35), read from the render app's
    --tree-probe over the conifer fixture: each tree grown and meshed as the scene grows it, and
    digested.

      conifer-identity    the recursive form's broadleaf and dead trees hash to the digests
                          they had before the conifer form shared the generator
      conifer-determinism a conifer grown twice is the same to the bit, and another seed is
                          another tree
      conifer-budget      every conifer preset over three seeds stays inside the triangle
                          budget, bark and needles together
      conifer-shape       a tidy spruce's crown narrows -- each quarter's longest branch beats
                          the quarter two above it, and the top quarter's is the shortest --
                          and a snag's top is broken off with no spray above the break
      conifer-thin        a spruce's sprays thin by the keep factor it reports, level by level,
                          over at least three levels
      conifer-wind        the bark and the needles lean over one height range, and it is
                          STATED rather than each mesh's own: the needles' box, whose sprays
                          hang below the lowest branch and reach past the leader, is not it

    What none of these can see is whether the trees look right; that is the user's eye on the
    tree viewer or this fixture rendered, and the spec's as-built says which renders it was.
    """
    del workdir  # the probe's rows are read off stdout; nothing is written
    first, second = _conifer_probe(), _conifer_probe()
    failures = []

    # A tree at its preset's own irregularity unless one is named: the fixture holds spruce seed
    # 1 twice, at its own and at 0.
    def tree(preset, seed, irregularity=None, run=None):
        return (run if run is not None else first).get((preset, seed, irregularity))

    got = {key: tree(*key) for key in CONIFER_IDENTITY}
    ok = all(got[key] and (got[key]["bark"], got[key]["leaves"]) == want
             for key, want in CONIFER_IDENTITY.items())
    detail = ", ".join(f"{p} {s}: {(got[(p, s)] or {}).get('bark')}/"
                       f"{(got[(p, s)] or {}).get('leaves')}" for p, s in CONIFER_IDENTITY)
    print(f"  conifer-identity {'PASS' if ok else 'FAIL'}  {detail} (want the digests from "
          f"before the conifer form)")
    if not ok:
        failures.append("conifer-identity")

    a, b, other = tree("spruce", 7), tree("spruce", 7, run=second), tree("spruce", 8)
    ok = bool(a and b and other) and (a["bark"], a["leaves"]) == (b["bark"], b["leaves"]) and \
        (a["bark"], a["leaves"]) != (other["bark"], other["leaves"])
    print(f"  conifer-determinism {'PASS' if ok else 'FAIL'}  spruce seed 7 in two runs: "
          f"{a and a['bark']}/{b and b['bark']}; seed 8: {other and other['bark']} (want the two "
          f"7s equal and the 8 different)")
    if not ok:
        failures.append("conifer-determinism")

    totals = {}
    for preset in CONIFER_PRESETS:
        for seed in CONIFER_SEEDS:
            d = tree(preset, seed)
            totals[(preset, seed)] = d["bark_tris"] + d["leaf_tris"] if d else None
    ok = all(t is not None and t <= CONIFER_TRI_BUDGET for t in totals.values())
    worst = max((t for t in totals.values() if t is not None), default=None)
    print(f"  conifer-budget {'PASS' if ok else 'FAIL'}  the largest of {len(totals)} trees is "
          f"{worst} triangles (budget {CONIFER_TRI_BUDGET})")
    if not ok:
        failures.append("conifer-budget")

    tidy = tree("spruce", 1, 0.0)
    snag = tree("snag", 1)
    q = [tidy["shape"][f"q{i}"] for i in range(4)] if tidy and "shape" in tidy else None
    s = snag.get("shape") if snag else None
    narrows = q is not None and q[0] > q[2] and q[1] > q[3] and q[3] == min(q)
    broken = s is not None and s["trunk_top"] < 0.8 * s["trunk_length"] and \
        s["card_top"] < s["trunk_top"] + 1.2 * s["leaf_size"]
    ok = narrows and broken
    print(f"  conifer-shape {'PASS' if ok else 'FAIL'}  tidy spruce longest per crown quarter "
          f"{[round(v, 1) for v in q] if q else None} (want narrowing up the crown); snag trunk "
          f"top {s and round(s['trunk_top'], 1)} of {s and round(s['trunk_length'])}, highest "
          f"spray {s and round(s['card_top'], 1)}")
    if not ok:
        failures.append("conifer-shape")

    lod = (tree("spruce", 1) or {}).get("lod") or {"keep": 0.0, "sprays": []}
    sprays, keep = lod["sprays"], lod["keep"]
    thins = all(abs(sprays[i] - sprays[i - 1] * keep) < 1.0 for i in range(1, len(sprays)))
    ok = len(sprays) >= 3 and thins
    print(f"  conifer-thin {'PASS' if ok else 'FAIL'}  sprays per level {sprays} (want each "
          f"{keep} of the last, to within a spray, over three levels or more)")
    if not ok:
        failures.append("conifer-thin")

    # The needles' own box (its foot, and card_top for its top), against the range they lean
    # over: a range each mesh measured for itself would put both ends of the box ON the range's,
    # and equal ranges alone would be true of two meshes that happened to share a box.
    ys = [(d["shape"]["bark_y0"], d["shape"]["leaf_y0"], d["shape"]["bark_y1"],
           d["shape"]["leaf_y1"], d["shape"]["leaf_box_y0"], d["shape"]["card_top"])
          for d in (tidy, snag, tree("fir", 2)) if d and "shape" in d]
    ok = len(ys) == 3 and all(
        abs(b0 - l0) < 1e-3 and abs(b1 - l1) < 1e-3 and
        max(abs(box0 - l0), abs(box1 - l1)) > 0.01 * (l1 - l0)
        for b0, l0, b1, l1, box0, box1 in ys)
    print(f"  conifer-wind {'PASS' if ok else 'FAIL'}  bark and needle wind ranges "
          f"{[(round(b0, 2), round(b1, 2), round(l0, 2), round(l1, 2)) for b0, l0, b1, l1, *_ in ys]}"
          f", needles' own boxes {[(round(x0, 2), round(x1, 2)) for *_, x0, x1 in ys]} (want each "
          f"tree's two ranges equal, and not its needles' own box)")
    if not ok:
        failures.append("conifer-wind")

    return failures
