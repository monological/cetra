"""The prepass, cluster, quadtree and forest groups:

  prepass             depth prepass (identical picture, less shading, spec 11.30 / E6)
  cluster             cluster-DAG level of detail (spec 11.63)
  quadtree            the terrain quadtree (spec 11.63)
  forest              forest (scattered content: batching, ordering, LOD, spec 11.29)
"""

import re
import os
import subprocess

from gatelib.harness import FOREST, RENDER, ROOT, _run, asset, render
from gatelib.images import compare
from gatelib.profiling import _report_tables, _shading_block
from gatelib.world import _CLUSTER_PROBE, _FOREST_TRACE


_FOREST_CHAINS = re.compile(r"Forest: (\d+) LOD chains built, (\d+) refused")
_FOREST_MESHES = re.compile(r"Forest: (\d+) distinct meshes")
# What the terrain contributed. Since spec 11.63 that is not a constant: the
# quadtree selects against the camera, so two runs at different framings draw
# different numbers of patches, and any arm comparing mesh counts across framings
# has to subtract this to be left with the props it meant to count.
_FOREST_TERRAIN = re.compile(
    r"Forest terrain: (?:quadtree \d+ levels, (\d+) patches selected|fixed grid, (\d+) tiles)")

# One framing for every arm that is not about framing. Inside the terrain, high
# enough to see past the near trees, so a good fraction of the world is culled
# and the rest is a mix of distances -- which is what makes the batching and LOD
# numbers mean something rather than measuring an empty or a fully-visible frame.
FOREST_CAM = ["--cam-eye", "0,40,120", "--cam-target", "0,10,0"]

# Off the island and above it, so the whole of it is in frame and the props run
# from a couple of hundred units out to eleven hundred. For the two arms that are
# about DISTANCE, and they need their own framing since 11.63 made the app an
# island: standing on it, everything is inside a 425-unit disc and there is no far
# field for LOD to save anything in. Measured 5% saved at FOREST_CAM against 44%
# here, on the same scene -- the first is not LOD failing, it is a camera with
# nothing distant in front of it.
FOREST_CAM_WIDE = ["--cam-eye", "0,300,700", "--cam-target", "0,-20,0"]

# Above the terrain aimed up and away, so nothing the scatter placed is in front
# of the camera and forest-cull can assert exact numbers instead of a direction.
FOREST_CAM_AWAY = ["--cam-eye", "0,300,0", "--cam-target", "600,900,600"]


_FOREST_RUN_CACHE = {}


def _forest_run(tag, extra, cam=None):
    """One profiled forest run, or None if it did not produce a readable report.

    Built in one place for the same reason _gpu_cmd is: several arms here claim
    their two runs differ in exactly one flag, and two hand-written command lists
    would let that stop being true without anything failing.

    Returning None for an unreadable report -- rather than an empty table every
    caller then has to .get() its way around -- is what lets the arms index
    columns directly. An arm reading a missing column as 0 does not fail loudly;
    it compares 0 against 0 and passes.

    MEMOIZED on (extra, cam), because two pairs of these are byte-identical
    across gate groups -- cluster-parity's baseline and lod's, and their two
    --no-lod runs -- differing only in the output filename. The arms already
    require those results to be equal, so serving one run to both is not an
    assumption, it is the assumption they are built on. `tag` is deliberately
    NOT part of the key: it names the file, and two runs that differ only in
    where the screenshot lands are the same run.
    """
    key = (tuple(extra), tuple(cam) if cam else None)
    if key in _FOREST_RUN_CACHE:
        return _FOREST_RUN_CACHE[key]

    # --no-fog because the app defaults it on: it is a froxel volume with its own
    # accumulator and it costs real time per run, while contributing nothing to
    # the submission counts every arm here reads.
    # No -S: every arm here reads the profiler's tables and the startup log, and
    # none looks at a pixel -- so a screenshot is a full readback and a file write
    # for nothing. _terrain_run reached this conclusion first; the pattern is the
    # same one.
    cmd = ([FOREST, "-x", "-f", "20", "-W", "800", "-H", "450", "--profiler", "--no-fog"]
           + (cam or FOREST_CAM) + extra)
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  forest       ERROR {tag} exited {r.returncode}: "
              f"{(r.stdout + r.stderr).strip()[-300:]}")
        return None
    text = r.stdout + r.stderr
    tables = _report_tables(text)
    m = _FOREST_CHAINS.search(text)
    mm = _FOREST_MESHES.search(text)
    shading = _shading_block(text)
    unparsed = sum(tables["unparsed"].values())
    if not m or not mm or not shading or unparsed or "opaque" not in tables["submit"]:
        print(f"  forest       ERROR {tag} report unreadable: {unparsed} unparsed rows, "
              f"chains {'yes' if m else 'no'}, meshes {'yes' if mm else 'no'}, "
              f"shading {'yes' if shading else 'no'}, "
              f"opaque {'yes' if 'opaque' in tables['submit'] else 'no'}")
        return None
    tables["chains"] = {"built": int(m.group(1)), "refused": int(m.group(2))}
    tables["meshes"] = int(mm.group(1))
    mt = _FOREST_TERRAIN.search(text)
    if not mt:
        # None, not 0, for the reason this helper's docstring gives: forest-cull
        # subtracts this, and a silent zero would take it back to comparing raw
        # mesh counts across two framings, which is what stopped working.
        print(f"  forest       ERROR {tag} printed no terrain summary")
        return None
    tables["terrain_meshes"] = int(mt.group(1) or mt.group(2))
    tables["shading"] = shading
    tables["opaque"] = tables["submit"]["opaque"]
    _FOREST_RUN_CACHE[key] = tables
    return tables


RAIDEN = os.path.join(ROOT, "my_models", "raiden", "source", "raiden_textured_rigged.glb")


def _raiden_render(out, extra):
    """The docs/verification.md baseline recipe, which is 0 px run-to-run.

    Here rather than in a gate because it is the corpus's only ALPHA_MASK +
    ALPHA_BLEND subject: every other fixture is opaque, so it is the only thing
    that can see a change to masked geometry at all.
    """
    cmd = [RENDER, "-m", RAIDEN,
           "-t", os.path.join(ROOT, "my_models", "raiden", "textures"),
           "-e", os.path.join(ROOT, "my_models", "studio_small_03_8k.hdr"),
           "-a", os.path.join(ROOT, "my_models", "animations", "strut_walk.fbx"),
           "-s", os.path.join(ROOT, "my_models", "animations", "T-Pose.fbx"),
           "-x", "-f", "120", "--no-springs", "--no-auto-exposure", "-E", "1.0", "-S", out]
    r = _run(cmd + extra, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return r.stdout + r.stderr
    return None


def run_prepass_gate(workdir):
    """The depth prepass: identical picture, less shading (spec 11.30 / E6).

    The identity arm runs on `instancing_fixture` rather than on forest, and
    that is deliberate twice over. It is purely opaque, so every item goes
    through the prepass and none sits it out; and it is the one subject measured
    at exactly 0 px across a draw-order change, so a failure here is the prepass
    and not the scene.
    """
    failures = []

    off = os.path.join(workdir, "prepass_off.ppm")
    on = os.path.join(workdir, "prepass_on.ppm")
    err = render(asset("instancing_fixture.cscn"), off,
                 ["--no-auto-exposure", "-E", "1.0"])
    err = err or render(asset("instancing_fixture.cscn"), on,
                        ["--no-auto-exposure", "-E", "1.0", "--depth-prepass"])
    if err:
        print(f"  prepass-identity ERROR render failed: {err.strip()[-200:]}")
        return ["prepass-identity"]

    # 0 px, not a budget. The prepass writes depth from a DIFFERENT program than
    # the one that shades, and the shading pass then tests against it with
    # GL_LEQUAL -- so this arm is what proves `invariant gl_Position` and the
    # shared object-position chunk actually hold. A near miss here is fragments
    # failing the test, i.e. holes, not a slightly different picture.
    ae, _ = compare(off, on)
    ok = ae == 0
    print(f"  prepass-identity {'PASS' if ok else 'FAIL'}  {ae} px between prepass on and off "
          f"(want exactly 0)")
    if not ok:
        failures.append("prepass-identity")

    # --- prepass-masked: the arm above CANNOT see the flag's real cost -------
    # instancing_fixture is purely opaque, so nothing about it changes when
    # masked geometry does. Masked geometry IS prepassed since 11.31, but the
    # shading pass still runs GL_LEQUAL, so coincident cards that GL_LESS
    # rejected now both pass and the later one wins.
    #
    # Asserted as a BOUND rather than 0, because 0 is not the truth here and an
    # arm that demanded it would just be turned off. The number is what stops it
    # drifting quietly: raiden is the corpus's one masked subject and is 0 px
    # run-to-run, so any movement is the flag.
    rai_off = os.path.join(workdir, "prepass_rai_off.ppm")
    rai_off2 = os.path.join(workdir, "prepass_rai_floor.ppm")
    rai_on = os.path.join(workdir, "prepass_rai_on.ppm")
    if not os.path.exists(RAIDEN):
        # my_models is not in every checkout. SKIP rather than pass: this is the
        # only arm that can see the flag's cost, so a silent pass would be worse
        # than no arm at all.
        print("  prepass-masked SKIP  (my_models/raiden not present)")
        err = None
    elif (err := _raiden_render(rai_off, []) or _raiden_render(rai_off2, []) or
                 _raiden_render(rai_on, ["--depth-prepass"])):
        print(f"  prepass-masked ERROR render failed: {err.strip()[-200:]}")
        failures.append("prepass-masked")
    else:
        # The floor, rendered rather than asserted. It used to be a comment
        # saying raiden is 0 px run-to-run, which was true and was never
        # checked -- fine against 46,314, not fine against 395, where an
        # unmeasured floor is within an order of magnitude of the signal.
        floor, _ = compare(rai_off, rai_off2)
        ae, _ = compare(rai_off, rai_on)
        # 46,314 when this arm was written; 395 now, and BOTH reductions have
        # causes worth keeping apart. The opaque lane stopping blending took most
        # of it -- a coincident card that LEQUAL lets through used to composite a
        # second time and now overwrites with the value already there. Admitting
        # masked geometry to the prepass took none of it, which was a surprise.
        #
        # What is left is inherent to HAVING a prepass and will not reach 0: the
        # shading pass must test LEQUAL, so two surfaces at exactly equal depth
        # both pass and the last drawn wins, where GL_LESS rejected the second.
        # Same mechanism as cornell_box's 1 px coplanar tie.
        ok = floor < ae <= 2000
        print(f"  prepass-masked {'PASS' if ok else 'FAIL'}  raiden moves {ae} px under "
              f"--depth-prepass against a {floor} px floor (want above the floor and "
              f"<= 2000; coincident surfaces under LEQUAL, see spec 11.31)")
        if not ok:
            failures.append("prepass-masked")

    # Everything below needs the forest binary, which run_forest_gate skips on
    # when absent -- and this gate runs first, so without the guard a tree with
    # no out/bin/forest raises FileNotFoundError instead of skipping.
    if not os.path.exists(FOREST):
        print("  prepass-shading SKIP  (forest not built)")
        return failures

    # --- prepass-shading: it removes shading, and the row appears -----------
    base = _forest_run("pp_base", ["--taa", "--headless-jitter", "--no-sort-opaque"])
    pre = _forest_run("pp_on",
                      ["--taa", "--headless-jitter", "--no-sort-opaque", "--depth-prepass"])
    if base is None or pre is None:
        failures.append("prepass-shading")
    else:
        b, p = base["shading"], pre["shading"]
        # Depth complexity is the claim; draws rising is the cost, asserted so a
        # prepass that silently stopped submitting would not read as a win.
        drew_more = pre["opaque"]["draws"] > base["opaque"]["draws"]
        ok = p["complexity"] < b["complexity"] and drew_more
        print(f"  prepass-shading {'PASS' if ok else 'FAIL'}  depth complexity "
              f"{b['complexity']:.2f} -> {p['complexity']:.2f} (want lower), "
              f"{base['opaque']['draws']} -> {pre['opaque']['draws']} draws (want higher: the "
              f"prepass submits its own)")
        if not ok:
            failures.append("prepass-shading")

    return failures

# A clustered prototype must reach at least this reduction from its finest band to
# its coarsest, or the DAG did not produce a usable cut. Well under bark's measured
# 277x and well over what a leaf card manages, which is the point: the arm has to
# pass on the geometry that CAN simplify without demanding it of the geometry that
# cannot.
CLUSTER_BAND_REDUCTION_MIN = 8.0

# How far the DAG's drawn triangles may sit from the chain's at one framing. The
# two are at PARITY on this corpus (measured -2.7% near, -0.1% mid, -1.4% far), and
# this arm pins that rather than claiming a win the measurement does not support.
CLUSTER_PARITY_TOLERANCE = 0.06

# Both builders must still beat drawing every triangle, at FOREST_CAM_WIDE, which
# is the framing cluster-parity uses. Measured 46.0% for the chain and 44.5% for
# the DAG; the bar sits far below both so it reads "LOD happened", not "this
# build's tuning".
#
# Both figures were 15.2% / 13.0% before 11.63 and are not comparable across it:
# those were taken at FOREST_CAM on a flat domain, and on the island that camera
# has nothing further than 425 units in front of it, which is why the two arms
# that are about distance moved to their own framing.
CLUSTER_LOD_SAVING_MIN = 0.10


def run_cluster_gate(workdir):
    """Cluster-DAG level of detail, and what it does and does not buy.

      cluster-seal    every cluster at every level indexes the ORIGINAL vertex
                         buffer, and destructive simplification never ran. That is
                         what makes a crack between levels impossible, and it is
                         structural -- no frame can show it, so it is read off the
                         probe rather than rendered.
      cluster-alias   a band shares an earlier band's range exactly when its cut
                         is byte-identical, checked against a per-band index
                         digest so the arm asks the builder's own question rather
                         than a weaker one about triangle counts. Structural like
                         the seal: a duplicated band draws a correct picture, so
                         no frame shows it.
      cluster-bands   the cut actually coarsens with distance: band triangle
                         counts never rise, and the geometry that CAN simplify
                         reaches a large reduction. Leaf cards are expected not to
                         and are not held to it. Falsified by inverting the two
                         clauses of the cut rule in cluster_build.cpp, which emits
                         the same cluster set in every band -- reduction 1.0x.
      cluster-batch   instancing survives. A per-instance cut would have ended
                         batching; the band quantisation exists so it does not,
                         and this is the arm that says so -- identical INSTANCE
                         counts against the chain, STRICTLY fewer draws wherever
                         a band aliases, and --no-instancing degenerating to one
                         draw per instance. The draw clause was an equality until
                         11.92 made the DAG batch better; strict is what keeps
                         the half of 11.92 that wins the draws under a test.
      cluster-parity  the DAG draws about as many triangles as the chain, and
                         both beat --no-lod. Parity is the honest claim on this
                         corpus: boundary locking costs roughly what error-driven
                         selection gains, and the DAG's win is the seal above.

    The DAG is applied to PROPS ONLY, and the exclusion is measured rather than
    assumed: on a regular grid -- which is what a terrain tile is -- locking every
    group boundary costs more than it buys, and the chain reached a better surface
    at the same triangle count. Terrain gets the quadtree instead.
    """
    if not os.path.exists(FOREST):
        print("  cluster-seal SKIP  (forest not built)")
        return []
    failures = []

    probe = subprocess.run(
        [FOREST, "-x", "-f", "1", "-W", "200", "-H", "150", "--no-fog", "--seed", "1337",
         "--cluster-probe"], capture_output=True, text=True)
    rows = [m for m in _CLUSTER_PROBE.finditer(probe.stdout + probe.stderr)]
    # The return code separately from the rows: a crashed forest produces no rows
    # either, and reporting it as "the probe printed nothing" sends the reader
    # looking at the parser instead of at the process that died.
    if probe.returncode != 0:
        print(f"  cluster-seal ERROR the cluster probe exited {probe.returncode}")
        return ["cluster-seal", "cluster-alias", "cluster-bands", "cluster-batch", "cluster-parity"]
    if not rows:
        print("  cluster-seal ERROR the cluster probe produced no rows")
        return ["cluster-seal", "cluster-alias", "cluster-bands", "cluster-batch", "cluster-parity"]

    # --- seal ---------------------------------------------------------------
    # foreign_indices, NOT max_index, and the distinction is the whole arm. An
    # index merely being in range cannot move: mesh_build_cluster_lod refuses an
    # out-of-range input before building, and meshoptimizer promises its output
    # references the input buffer. What CAN move is a coarse band referencing a
    # vertex band 0 never used, which is what a remapping simplifier produces.
    #
    # This arm asserted `permissive == 0` until the reviewers caught that the
    # probe wrote that field as a literal -- both of its bars were tautologies,
    # including under the mutation the spec named for it.
    foreign = [r for r in rows if int(r.group(8)) != 0]
    banded = [r for r in rows if int(r.group(5)) >= 2]
    ok = not foreign and len(banded) >= 8
    print(f"  cluster-seal {'PASS' if ok else 'FAIL'}  {len(rows)} clustered prototypes, "
          f"{len(foreign)} referencing a vertex band 0 never used (want 0), over "
          f"{len(banded)} prototypes that actually built a coarse band (want >= 8, or "
          f"there was nothing for a coarse band to get wrong)")
    if not ok:
        failures.append("cluster-seal")

    # --- alias --------------------------------------------------------------
    # A band whose cut equals an earlier band's must SHARE that range, not store
    # a second copy (spec 11.92). Read off the probe for cluster-seal's reason:
    # a duplicated band draws a perfectly correct picture, so no frame can show
    # this and no other arm would notice it coming back.
    #
    # Against the CONTENT DIGEST, not the triangle counts, and that is the whole
    # strength of the arm. Counts make it wrong in both directions: it could not
    # catch the builder's index comparison being weakened to a count -- the
    # mutation cluster_build.cpp names as the dangerous one, since it aliases two
    # distinct cuts of equal size and draws the wrong geometry -- and it would
    # turn RED on a future prototype where that coincidence happens and the
    # builder correctly refuses to alias. The digest makes the biconditional
    # exactly the builder's own rule.
    #
    # Not paired with a "did it ever fire" clause on the corpus. That would read
    # as a mechanism failure when the honest cause is a simplifier good enough to
    # leave no duplicate band, which is an improvement.
    alias_rows, alias_bands, mismatched = 0, 0, 0
    for r in rows:
        digests = [int(v.split("=")[1]) for v in r.group(11).split()]
        mask = int(r.group(10))
        for i in range(1, len(digests)):
            aliased = bool((mask >> i) & 1)
            if aliased:
                alias_bands += 1
            if aliased != (digests[i] == digests[i - 1]):
                mismatched += 1
        if mask:
            alias_rows += 1
    ok = mismatched == 0
    print(f"  cluster-alias {'PASS' if ok else 'FAIL'}  {alias_bands} bands over {alias_rows} "
          f"prototypes share an earlier band's range, {mismatched} disagreeing with the index "
          f"digests (want 0: a band shares a range exactly when its cut is byte-identical)")
    if not ok:
        failures.append("cluster-alias")

    # --- bands --------------------------------------------------------------
    worst_rise, best_reduction = 0, 0.0
    for r in rows:
        bands = [int(v.split("=")[1]) for v in r.group(9).split()]
        for i in range(1, len(bands)):
            worst_rise = max(worst_rise, bands[i] - bands[i - 1])
        if bands[-1] > 0:
            best_reduction = max(best_reduction, bands[0] / float(bands[-1]))
    ok = worst_rise <= 0 and best_reduction >= CLUSTER_BAND_REDUCTION_MIN
    print(f"  cluster-bands {'PASS' if ok else 'FAIL'}  worst band-to-band rise "
          f"{worst_rise} triangles (want <= 0), best prototype reduction {best_reduction:.0f}x "
          f"(want >= {CLUSTER_BAND_REDUCTION_MIN:.0f}x; a leaf card manages ~2x and is not the one "
          f"this reads)")
    if not ok:
        failures.append("cluster-bands")

    # --- batching and parity ------------------------------------------------
    # FOREST_CAM_WIDE, because the parity arm is about DISTANCE: standing on the
    # island there is no far field for either builder to coarsen, and the two agree
    # trivially on a scene where neither had anything to do.
    dag = _forest_run("cluster_dag", [], cam=FOREST_CAM_WIDE)
    chain = _forest_run("cluster_chain", ["--no-clusters"], cam=FOREST_CAM_WIDE)
    nolod = _forest_run("cluster_nolod", ["--no-lod"], cam=FOREST_CAM_WIDE)
    uninst = _forest_run("cluster_uninst", ["--no-instancing"], cam=FOREST_CAM_WIDE)
    if not all((dag, chain, nolod, uninst)):
        print("  cluster-batch ERROR while rendering forest")
        return failures + ["cluster-batch", "cluster-parity"]

    # `drew` is not a formality: every comparison below is an equality, and a
    # scene that submitted nothing satisfies all three. forest-batch-off guards
    # exactly this and these arms were written without it.
    drew = dag["opaque"]["draws"] > 0 and uninst["opaque"]["draws"] > 0
    # STRICTLY FEWER where a band aliases, and the strictness is the arm.
    #
    # This read `==` while the DAG and the chain batched identically, and the
    # failure it exists to catch -- a per-instance cut, which splits every run --
    # makes the DAG's count EXPLODE, so any upper bound catches it. 11.92 relaxed
    # it to `<=` when aliasing made the DAG batch better, and that was the wrong
    # repair: the alias is only half the change, and the OTHER half -- collapsing
    # the selected level onto the range it names -- is the half that produces the
    # entire draw win. With `<=` that half can be deleted outright and every arm
    # in this suite stays green, including the alias arm above, which reads the
    # builder's output and never runs the selector. Strict `<` is what fails the
    # moment the collapse goes away.
    #
    # Conditional on the probe because the property genuinely is: a corpus with
    # no duplicate band has nothing to win, and demanding a win there would fail
    # on a better simplifier rather than on a regression.
    #
    # `same_inst` is NOT the load-bearing clause and this comment used to say it
    # was. It pins that no geometry was LOST; it is blind to geometry being
    # WRONG -- an instance drawing a different band's range at the same triangle
    # count -- which is the only thing aliasing can break. cluster-alias pins
    # that, against the index digests.
    if alias_bands:
        better_draws = dag["opaque"]["draws"] < chain["opaque"]["draws"]
    else:
        better_draws = dag["opaque"]["draws"] <= chain["opaque"]["draws"]
    same_inst = dag["opaque"]["instances"] == chain["opaque"]["instances"]
    unbatched = uninst["opaque"]["draws"] == uninst["opaque"]["instances"]
    ok = drew and better_draws and same_inst and unbatched
    print(f"  cluster-batch {'PASS' if ok else 'FAIL'}  DAG draws {dag['opaque']['draws']} vs "
          f"chain {chain['opaque']['draws']} (want {'<' if alias_bands else '<='}, from "
          f"{alias_bands} aliased bands: a per-instance cut would split every run, and without "
          f"the level collapse the two are equal) carrying {dag['opaque']['instances']} vs "
          f"{chain['opaque']['instances']} instances (want equal); --no-instancing gives "
          f"{uninst['opaque']['draws']} draws for {uninst['opaque']['instances']} instances "
          f"(want equal)")
    if not ok:
        failures.append("cluster-batch")

    d_tris = dag["opaque"]["triangles"]
    c_tris = chain["opaque"]["triangles"]
    n_tris = nolod["opaque"]["triangles"]
    drift = abs(d_tris - c_tris) / float(max(c_tris, 1))
    d_save = 1.0 - d_tris / float(max(n_tris, 1))
    c_save = 1.0 - c_tris / float(max(n_tris, 1))
    # Same reason as above, and sharper here: with all-zero counts the two
    # savings come out at 1.0 and the drift at 0, so an empty frame reads as a
    # perfect result rather than as no result.
    ok = (n_tris > 0 and d_tris > 0 and drift <= CLUSTER_PARITY_TOLERANCE
          and d_save >= CLUSTER_LOD_SAVING_MIN and c_save >= CLUSTER_LOD_SAVING_MIN)
    print(f"  cluster-parity {'PASS' if ok else 'FAIL'}  DAG {d_tris} triangles vs chain "
          f"{c_tris}, drift {100.0 * drift:.1f}% (want <= {100.0 * CLUSTER_PARITY_TOLERANCE:.0f}%); "
          f"against --no-lod's {n_tris} that is {100.0 * d_save:.1f}% and {100.0 * c_save:.1f}% "
          f"saved (want >= {100.0 * CLUSTER_LOD_SAVING_MIN:.0f}% each)")
    if not ok:
        failures.append("cluster-parity")

    return failures


# The quadtree probe's three line shapes.
#
# NAMED groups throughout, and every field captured whether or not an arm reads
# it. Positionally these were group(1..7) against a dict that used 1,2,3,4,7 --
# so `resident` and `built` were matched and thrown away, which is the exact setup
# where inserting a field silently shifts every index after it and the arms go on
# passing against the wrong numbers. The level row was worse: four of its fields
# were non-capturing, so ADDING one anywhere in that line empties by_level and the
# group fails with a message about the probe printing nothing.
_QT_SUMMARY = re.compile(
    r"terrain-quadtree-probe levels=(?P<levels>\d+) segments=(?P<segments>\d+) "
    r"split_factor=(?P<split_factor>[\d.]+) selected=(?P<selected>\d+) "
    r"resident=(?P<resident>\d+) built=(?P<built>\d+) triangles=(?P<triangles>\d+)")
_QT_SEAM = re.compile(
    r"terrain-quadtree-probe seams=(?P<seams>\d+) unbalanced=(?P<unbalanced>\d+) "
    r"fine_min=(?P<fine_min>[\d.]+) coarse_max=(?P<coarse_max>[\d.]+) "
    r"gap=(?P<gap>[\d.eE+-]+) interior_gap=(?P<interior_gap>[\d.eE+-]+)")
_QT_LEVEL = re.compile(
    r"terrain-quadtree-probe level=(?P<level>\d+) span=(?P<span>[\d.]+) "
    r"cell=(?P<cell>[\d.]+) split_at=(?P<split_at>[\d.]+) "
    r"morph_start=(?P<morph_start>[-\d.]+) morph_end=(?P<morph_end>[-\d.]+) "
    r"selected=(?P<selected>\d+) source_level=(?P<source_level>\d+) "
    r"source_cell=(?P<source_cell>[\d.]+) dropped=(?P<dropped>[\d.eE+-]+)")
_QT_FIELD_LEVELS = re.compile(r"Terrain field: (\d+) level\(s\)")

# Both gaps are float rounding on ~95-unit heights, where an ulp is 7.6e-6. The
# bar is two orders above that and four below the mutations it has to catch:
# flipping the coarse quad's diagonal reads 2.92 world units, and dropping the
# split factor to 2.0 opens a seam at coarse_max 0.173.
QUADTREE_GAP_MAX = 1.0e-3

# Patches, for a SIXTEENFOLD increase in ground area. The claim is that a
# quadtree's cost is logarithmic in world size where a fixed grid's is quadratic,
# so the bar is nowhere near tight -- 16x area really did cost 1.94x, and the
# thing this catches is the growth going quadratic, which would be 16x.
QUADTREE_GROWTH_MAX = 4.0

# How much the depth prepass may move on the quadtree, as a multiple of how much
# it moves on the fixed grid. The residual is pre-existing and belongs to masked
# foliage taking its own program; the morph is supposed to add nothing to it.
# Measured 612 px against 612 -- a ratio of 1.00 -- and starving one program of
# uMorphEye reads 9x, so the bar sits between two numbers that are far apart.
QUADTREE_PREPASS_TOLERANCE = 2.5

# Albedo pixels --no-morph must move. Measured 6,493 with the morph healthy and
# 22 with cetraMorphOffset deleted, over a floor of exactly 0 -- so the bar sits
# a factor of six below the signal and a factor of forty-five above the failure.
# An absolute count rather than a ratio to the floor, because this path's floor
# is genuinely 0 and a ratio to it is undefined.
QUADTREE_MORPH_MIN_PX = 1000

# World units of relief a coarse level must actually give up. Well above float
# noise and well under what either source drops in practice -- 4.9 for the fbm,
# 6.2 for a filtered field -- so the thing it catches is the drop being ZERO,
# which is what a level selector that always answers 0 gives, and what a
# SUBSAMPLED pyramid gives on a lattice the patches align with.
QUADTREE_MIP_DROP_MIN = 0.5


# Far enough back that the descent stops at the root: one patch, at the coarsest
# level there is, which is the only framing where the band-limit has anything to
# do. Every other arm here reads FOREST_CAM, where the whole selection is fine
# enough to want the full surface.
FOREST_CAM_HIGH = ["--cam-eye", "0,3000,3000", "--cam-target", "0,0,0"]


def _quadtree_probe(extra, cam=None):
    """The terrain quadtree probe, parsed, or None.

    Three frames and a small window: the probe reads the SELECTION, which is a
    function of the camera and not of how long the app has been running, and the
    seam measurement walks every selected patch's boundary on the CPU.
    """
    cmd = ([FOREST, "-x", "-f", "3", "-W", "400", "-H", "240", "--no-fog", "--seed", "1337",
            "--terrain-quadtree-probe"] + (cam or FOREST_CAM) + extra)
    r = _run(cmd, capture_output=True, text=True)
    text = r.stdout + r.stderr
    summary = _QT_SUMMARY.search(text)
    seam = _QT_SEAM.search(text)
    if r.returncode != 0 or not summary or not seam:
        return None
    fl = _QT_FIELD_LEVELS.search(text)
    levels = [{"level": int(m["level"]), "span": float(m["span"]), "cell": float(m["cell"]),
               "split_at": float(m["split_at"]), "morph_start": float(m["morph_start"]),
               "morph_end": float(m["morph_end"]), "selected": int(m["selected"]),
               "source_level": int(m["source_level"]), "source_cell": float(m["source_cell"]),
               "dropped": float(m["dropped"])} for m in _QT_LEVEL.finditer(text)]
    return {
        "field_levels": int(fl.group(1)) if fl else 0,
        "by_level": levels,
        "levels": int(summary["levels"]),
        "segments": int(summary["segments"]),
        "split_factor": float(summary["split_factor"]),
        "selected": int(summary["selected"]),
        "resident": int(summary["resident"]),
        "built": int(summary["built"]),
        "triangles": int(summary["triangles"]),
        "seams": int(seam["seams"]),
        "unbalanced": int(seam["unbalanced"]),
        "fine_min": float(seam["fine_min"]),
        "coarse_max": float(seam["coarse_max"]),
        "gap": float(seam["gap"]),
        "interior_gap": float(seam["interior_gap"]),
    }


def _grid_tiles(extra):
    """How many tiles the FIXED grid builds, or None.

    One frame at a postage stamp: the number is printed at startup and nothing
    here looks at a pixel. It still has to be a real run rather than arithmetic on
    the extent, or an app that quietly stopped growing its grid would be read as
    the quadtree's win.

    THE COST IS DELIBERATE AND HAS BEEN RE-FILED ONCE. At --terrain-extent 2000
    this builds 1,024 tiles, which is millions of fbm evaluations to read one
    integer, and the obvious fix -- print the count before building -- is exactly
    the arithmetic the paragraph above rejects, since on_init builds either way.
    Making it genuinely cheap needs an early-exit flag on the app, i.e. a small
    feature rather than a cleanup. Measured in release it is a fraction of a
    second; the number that made it look worth fixing came from a debug build.
    """
    # No -S -- this docstring already says nothing here looks at a pixel.
    cmd = ([FOREST, "-x", "-f", "1", "-W", "160", "-H", "120", "--no-fog", "--seed", "1337",
            "--no-quadtree"] + extra)
    r = _run(cmd, capture_output=True, text=True)
    m = _FOREST_TERRAIN.search(r.stdout + r.stderr)
    if r.returncode != 0 or not m:
        return None
    return int(m.group(1) or m.group(2))


def run_quadtree_gate(workdir):
    """The terrain quadtree: does the CDLOD selection hold together (spec 11.63).

      quadtree-seam    adjacent patches never differ by more than one level, and
                       at every seam the fine side is fully morphed while the
                       coarse side has not started -- so both are evaluating the
                       same coarse surface there and no crack is possible. Read
                       off the probe, at two world sizes.
      quadtree-morph  a fully morphed patch really is the surface its parent
                       DRAWS, measured against the parent's own triangles. This
                       is the pop, and the seam arm cannot see it: every boundary
                       vertex is even-indexed and therefore its own morph target,
                       so the interior can be wrong with every seam clean.
      quadtree-scale   the selection grows logarithmically with the world and the
                       fixed grid grows quadratically. The whole reason the
                       quadtree exists, and the only arm that varies the domain.
      quadtree-mip           a coarse patch reads a coarse SOURCE, and reading it
                       actually removes detail. Both sources: the fbm drops
                       octaves, a stored field reads a filtered level. The second
                       half is the arm -- picking a coarser level is free to be a
                       no-op, and with a subsampled pyramid on an aligned lattice
                       it is exactly that.
      quadtree-depth   the morph reaches the DEPTH PREPASS. It rasterizes the
                       same triangles and the shading pass tests against its
                       depth with LEQUAL, so a program that displaces differently
                       does not shade wrong, it deletes the surface.
      quadtree-render  the morph happens AT ALL, in a rendered frame, read
                       through --no-morph. Every arm above this one reads the
                       probe, whose morph factor is a hand C port of
                       cetraMorphFactor -- so a morph that never reached the
                       shader passes all four of them, which is precisely the
                       failure --wind-bound-probe was rebuilt to close.

    Every number here except the last is invisible in a frame, which is why the
    arms exist. A crack opens for the few frames the camera spends crossing a
    band, on ground that is by construction distant; a pop is one frame; and a
    starved prepass removes terrain only where the coarse surface happens to sit
    in front.
    """
    if not os.path.exists(FOREST):
        print("  quadtree-seam SKIP  (forest not built)")
        return []
    failures = []

    near = _quadtree_probe([])
    far = _quadtree_probe(["--terrain-extent", "2000"])
    if not near or not far:
        print("  quadtree-seam ERROR the quadtree probe produced no rows")
        return ["quadtree-seam", "quadtree-morph", "quadtree-scale", "quadtree-mip",
                "quadtree-depth", "quadtree-render"]

    # --- seam ---------------------------------------------------------------
    # fine_min == 1 exactly and coarse_max == 0 exactly, not approximately: both
    # are clamped, so the correct answer is the clamp's own endpoint and anything
    # between them is a seam that is genuinely open.
    ok = all(p["unbalanced"] == 0 and p["fine_min"] == 1.0 and p["coarse_max"] == 0.0
             and p["gap"] <= QUADTREE_GAP_MAX and p["seams"] > 0 for p in (near, far))
    print(f"  quadtree-seam {'PASS' if ok else 'FAIL'}  {near['seams']} seams at 1 km and "
          f"{far['seams']} at 4 km, {near['unbalanced'] + far['unbalanced']} more than one level "
          f"apart (want 0); fine side {near['fine_min']:.4f}/{far['fine_min']:.4f} (want 1), "
          f"coarse side {near['coarse_max']:.4f}/{far['coarse_max']:.4f} (want 0), worst gap "
          f"{max(near['gap'], far['gap']):.2e} units (want <= {QUADTREE_GAP_MAX:.0e})")
    if not ok:
        failures.append("quadtree-seam")

    # --- interior -----------------------------------------------------------
    worst = max(near["interior_gap"], far["interior_gap"])
    ok = worst <= QUADTREE_GAP_MAX
    print(f"  quadtree-morph {'PASS' if ok else 'FAIL'}  a fully morphed patch sits "
          f"{worst:.2e} world units off the surface its parent draws (want <= "
          f"{QUADTREE_GAP_MAX:.0e}; the wrong coarse-quad diagonal reads 2.92)")
    if not ok:
        failures.append("quadtree-morph")

    # --- world size ---------------------------------------------------------
    # The fixed-grid counts come from the app rather than from the ratio of the
    # extents, so an app that quietly stopped growing its tile grid is caught
    # here instead of flattering the quadtree.
    #
    # One frame at a postage stamp, not a profiled render: what is read is the
    # tile COUNT, and a 4 km grid builds 1,024 tiles before it can draw one -- a
    # 20-frame 800x450 run to learn an integer the app prints at startup.
    grid_near = _grid_tiles([])
    grid_far = _grid_tiles(["--terrain-extent", "2000"])
    if not grid_near or not grid_far:
        print("  quadtree-scale ERROR while rendering the fixed grid")
        failures.append("quadtree-scale")
    else:
        qt_growth = far["selected"] / float(max(near["selected"], 1))
        grid_growth = grid_far / float(max(grid_near, 1))
        # The floor on `selected` is load-bearing: a quadtree that stopped
        # descending and returned its root at both sizes reads 1.0x growth and
        # passes the ratio outright.
        ok = (near["selected"] > 1 and qt_growth <= QUADTREE_GROWTH_MAX
              and grid_growth > qt_growth * 2.0)
        print(f"  quadtree-scale {'PASS' if ok else 'FAIL'}  16x the ground area takes the "
              f"quadtree from {near['selected']} patches to {far['selected']} ({qt_growth:.2f}x, "
              f"want <= {QUADTREE_GROWTH_MAX:.0f}x) while the fixed grid goes "
              f"{grid_near} -> {grid_far} "
              f"({grid_growth:.1f}x, want more than twice the quadtree's)")
        if not ok:
            failures.append("quadtree-scale")

    # --- the band limit -----------------------------------------------------
    # Read from a camera far enough back that the descent stops at the root, so
    # the one selected patch is at the coarsest level there is. At FOREST_CAM the
    # whole selection is fine enough to want the full surface, and every
    # `dropped` there is legitimately 0.
    analytic = _quadtree_probe([], cam=FOREST_CAM_HIGH)
    stored = _quadtree_probe(["--erode", "--erode-iterations", "20"], cam=FOREST_CAM_HIGH)
    if not analytic or not stored:
        print("  quadtree-mip       ERROR the band-limit probe produced no rows")
        failures.append("quadtree-mip")
    else:
        def coarsest(p):
            rows = [r for r in p["by_level"] if r["selected"] > 0]
            return max(rows, key=lambda r: r["level"]) if rows else None

        a_row, s_row = coarsest(analytic), coarsest(stored)
        # Monotone in the level, since a coarser patch may never ask for a finer
        # source: that ordering is what keeps a patch and its parent one source
        # level apart, which is what the morph target depends on.
        ordered = all(p["by_level"][k]["source_level"] >= p["by_level"][k - 1]["source_level"]
                      for p in (analytic, stored) for k in range(1, len(p["by_level"])))
        ok = (a_row and s_row and ordered and stored["field_levels"] > 1
              and a_row["source_level"] > 0 and a_row["dropped"] > QUADTREE_MIP_DROP_MIN
              and s_row["source_level"] > 0 and s_row["dropped"] > QUADTREE_MIP_DROP_MIN)
        print(f"  quadtree-mip       {'PASS' if ok else 'FAIL'}  the coarsest selected patch reads "
              f"source level {a_row['source_level'] if a_row else '?'} of the fbm and drops "
              f"{a_row['dropped'] if a_row else 0:.2f} world units, and level "
              f"{s_row['source_level'] if s_row else '?'} of a "
              f"{stored['field_levels']}-level stored field dropping "
              f"{s_row['dropped'] if s_row else 0:.2f} (want > {QUADTREE_MIP_DROP_MIN} each, a field "
              f"pyramid deeper than 1, and source levels non-decreasing: {ordered})")
        if not ok:
            failures.append("quadtree-mip")

    # --- the prepass --------------------------------------------------------
    # --no-sky, because the Hillaire atmosphere is what makes forest not
    # pixel-deterministic; the arm reads a difference between two frames and
    # needs the difference to be the prepass.
    #
    # Twelve frames at 400x240, and the size is affordable because what the arm
    # reads is a RATIO of two pixel counts taken at the same size. Starving a
    # program of uMorphEye deletes surface wherever the coarse ground sits in
    # front of the fine, which is a fraction of the frame and not a speck count.
    shots = {}
    for tag, extra in (("qt", []), ("qt_pp", ["--depth-prepass"]),
                       ("grid", ["--no-quadtree"]),
                       ("grid_pp", ["--no-quadtree", "--depth-prepass"])):
        out = os.path.join(workdir, f"quadtree_{tag}.ppm")
        cmd = ([FOREST, "-x", "-f", "12", "-W", "400", "-H", "240", "--no-fog", "--no-sky",
                "--seed", "1337", "-S", out] + FOREST_CAM + extra)
        r = _run(cmd, capture_output=True, text=True)
        shots[tag] = out if r.returncode == 0 and os.path.exists(out) else None
    if not all(shots.values()):
        print("  quadtree-depth ERROR while rendering the prepass comparison")
        return failures + ["quadtree-depth", "quadtree-render"]

    qt_move, _ = compare(shots["qt"], shots["qt_pp"])
    grid_move, _ = compare(shots["grid"], shots["grid_pp"])
    # The precondition, without which the arm passes on a frame where nothing
    # morphs: some patch has to have reached factor 1 for the prepass to have
    # anything to disagree about.
    morphing = near["fine_min"] == 1.0 and near["seams"] > 0
    ok = morphing and qt_move <= grid_move * QUADTREE_PREPASS_TOLERANCE
    print(f"  quadtree-depth {'PASS' if ok else 'FAIL'}  --depth-prepass moves {qt_move} px on "
          f"the quadtree against {grid_move} on the fixed grid, whose terrain does not morph at "
          f"all (want <= {QUADTREE_PREPASS_TOLERANCE:.1f}x it; starving one program of uMorphEye reads "
          f"9x), with {near['seams']} seams morphing")
    if not ok:
        failures.append("quadtree-depth")

    # --- render -------------------------------------------------------------
    # Everything above this line reads the probe, and the probe's morph factor is
    # a hand C port of cetraMorphFactor. So a morph dead in all five geometry
    # programs -- the include never reached, the attributes never bound, the
    # uniform never uploaded -- passes every one of them. This is the arm that
    # looks at a frame, and it is the same lesson --wind-bound-probe was rebuilt
    # on transform feedback for.
    #
    # The framing is its own, and not FOREST_CAM, because the morph is only large
    # where a patch is MID-WINDOW: level 0's window here is 164 to 219 units, so
    # the camera is placed to fill the frame with ground at that range. At
    # FOREST_CAM the whole comparison reads 146 px against a 5 px floor.
    #
    # --render-mode 6 is what makes this a POSITION test rather than a "something
    # moved" test, and that distinction was measured rather than assumed. Read
    # lit, deleting cetraMorphOffset entirely still moved 3,999 px and PASSED --
    # because cetraMorphNormal is a separate displacement of the same factor and
    # --no-morph switches both off, so the arm was reading the shading. Albedo has
    # no normal in it, so what is left is where the geometry PROJECTS. It is also
    # forest's one pixel-deterministic path, which is why the floor is an exact 0
    # here and 174 on the lit frame.
    #
    # Known gap, stated rather than papered over: this does not cover
    # cetraMorphNormal. Deleting it alone moves nothing in albedo.
    mshots = {}
    for tag, extra in (("on", []), ("on2", []), ("off", ["--no-morph"])):
        out = os.path.join(workdir, f"quadtree_morph_{tag}.ppm")
        cmd = ([FOREST, "-x", "-f", "3", "-W", "800", "-H", "500", "--no-fog", "--no-sky",
                "--seed", "1337", "--render-mode", "6",
                "--cam-eye", "0,60,220", "--cam-target", "0,-10,-120", "-S", out] + extra)
        r = _run(cmd, capture_output=True, text=True)
        mshots[tag] = out if r.returncode == 0 and os.path.exists(out) else None
    if not all(mshots.values()):
        print("  quadtree-render ERROR while rendering the morph comparison")
        return failures + ["quadtree-render"]

    floor, _ = compare(mshots["on"], mshots["on2"])
    moved, _ = compare(mshots["on"], mshots["off"])
    # The floor is asserted at 0 and not merely bounded: this path IS
    # deterministic, so anything above it means the comparison stopped measuring
    # the morph and started measuring the renderer.
    ok = floor == 0 and moved >= QUADTREE_MORPH_MIN_PX
    print(f"  quadtree-render {'PASS' if ok else 'FAIL'}  --no-morph moves {moved} px of albedo "
          f"(want >= {QUADTREE_MORPH_MIN_PX}; a dead cetraMorphOffset reads 22) over a "
          f"same-config floor of {floor} (want exactly 0); this is the only arm that reaches the "
          f"SHADER -- the five above it read a C port of the same arithmetic")
    if not ok:
        failures.append("quadtree-render")

    return failures


def run_forest_gate(workdir):
    """The forest app: does scattered content actually batch, and does LOD fire.

    Every arm reads the SUBMISSION table, which is integers with no noise floor.
    The app pins its own exposure, so nothing here inherits the adaptation hazard
    that docs/verification.md names for cross-configuration comparison.
    """
    if not os.path.exists(FOREST):
        print("  forest       SKIP  (forest not built)")
        return []

    failures = []
    base = _forest_run("base", [])
    # Nothing below can mean anything if the base run did not report, so this is
    # the one arm that returns rather than accumulating.
    if base is None:
        return ["forest-parse"]
    opaque = base["opaque"]

    # --- forest-chains: every generated mesh got a chain --------------------
    # mesh_build_lod_chain has one caller in the engine, in import.c. Everything
    # this app draws is generated, so if the app stopped asking for chains every
    # LOD arm below would still pass -- against a scene where nothing had one.
    #
    # Exact, not `> 0`. Sixty-four of the meshes are terrain tiles, so a
    # regression that refused a chain to every TREE and every ROCK would still
    # leave built == 64, and forest-lod would still pass on the terrain alone.
    # Both operands are read from the app rather than mirrored here.
    built = base["chains"]["built"]
    refused = base["chains"]["refused"]
    distinct = base["meshes"]
    ok = refused == 0 and built == distinct
    print(f"  forest-chains {'PASS' if ok else 'FAIL'}  {built} chains built, {refused} refused, "
          f"against {distinct} distinct meshes (want every mesh chained)")
    if not ok:
        failures.append("forest-chains")

    # --- forest-batch-ship: batching still happens as the engine ACTUALLY runs -
    # Reads `base`, so it costs no extra render. A low floor on purpose: with LOD
    # and the depth sort both fragmenting runs the shipping ratio is ~1.77, and
    # the thing worth catching is not a drift from that but a COLLAPSE to 1.0,
    # which is the value that means batching stopped happening at all.
    inst = opaque["instances"]
    ship = inst / opaque["draws"] if opaque["draws"] else 0.0
    ok = ship >= 1.4
    print(f"  forest-batch-ship {'PASS' if ok else 'FAIL'}  shipping config: {inst} instances "
          f"in {opaque['draws']} draws (ratio {ship:.2f}, want >= 1.4; 1.0 means no batching)")
    if not ok:
        failures.append("forest-batch-ship")

    # --- forest-batch-off: one draw per instance, the baseline for the ratio -
    off = _forest_run("noinst", ["--no-instancing"])
    if off is None:
        failures.append("forest-batch-off")
        failures.append("forest-batch")
    else:
        o = off["opaque"]
        ok = o["draws"] == o["instances"] and o["draws"] > 0
        print(f"  forest-batch-off {'PASS' if ok else 'FAIL'}  --no-instancing: "
              f"{o['draws']} draws for {o['instances']} instances (want equal)")
        if not ok:
            failures.append("forest-batch-off")

    # --- forest-batch: the scatter actually batches -------------------------
    # Measured with LOD OFF and SORTING OFF, because the batcher joins only
    # consecutive items sharing (mesh, lod) and each of those is a separate
    # source of fragmentation this arm is not about:
    #
    #   LOD    splits a prototype spanning a distance range across levels.
    #   SORT   scatters identical meshes across DRAW_SORT_DEPTH_BUCKETS.
    #
    # Neither conflation is academic. With LOD on the ratio reads 2.31 at the
    # pinned seed and 1.94 at --seed 7, so no threshold with useful margin can
    # sit on it; with sorting on as well it reads 2.1. With both off the same
    # property reads 13.2 and 10.4, which is a claim about the batcher.
    #
    # This is not the threshold being relaxed to fit: the floor stays at 8, and
    # both flags remove a KNOWN confounder rather than a regression. The cost in
    # the configuration that actually ships is forest-batch-ship, below --
    # written because excluding two confounders left NO arm measuring batching
    # as the engine runs it, and a change that collapsed it to one draw per
    # instance would have passed everything here.
    nolod = _forest_run("nolod", ["--no-lod", "--no-sort-opaque"])
    if nolod is None:
        failures.append("forest-batch")
    else:
        n = nolod["opaque"]
        n_draws, n_inst = n["draws"], n["instances"]
        ratio = (n_inst / n_draws) if n_draws else 0.0
        ok = ratio >= 8.0
        print(f"  forest-batch {'PASS' if ok else 'FAIL'}  opaque {n_inst} instances in {n_draws} "
              f"draws with LOD and sorting off (ratio {ratio:.1f}, want >= 8)")
        if not ok:
            failures.append("forest-batch")

    # --- forest-lod: level selection fires on generated geometry ------------
    # A fixed camera comparing on against off, NOT a distance sweep: pulling back
    # over scattered content reveals more world, so triangles rise with distance
    # however well LOD works. Identical instances is what proves the difference is
    # level selection and not visibility.
    #
    # Its OWN framing since 11.63, and its own pair of runs with it. The app is an
    # island now: standing on it at FOREST_CAM every prop is inside a 425-unit disc
    # and LOD has almost nothing to coarsen — the same scene reads 5% saved there
    # and 44% from off the shore. Five per cent is not LOD failing, it is a camera
    # with nothing distant in front of it, and an arm about DISTANCE needs a
    # framing that has some.
    lod_on = _forest_run("lod_on", [], cam=FOREST_CAM_WIDE)
    lod_off = _forest_run("lod_off", ["--no-lod"], cam=FOREST_CAM_WIDE)
    if not lod_on or not lod_off:
        failures.append("forest-lod")
    else:
        w_on, w_off = lod_on["opaque"], lod_off["opaque"]
        same_vis = w_on["instances"] == w_off["instances"]
        saving = 1.0 - (w_on["triangles"] / w_off["triangles"]) if w_off["triangles"] else 0.0
        ok = same_vis and saving >= 0.10
        print(f"  forest-lod   {'PASS' if ok else 'FAIL'}  {w_on['triangles']} triangles with "
              f"LOD vs {w_off['triangles']} without ({saving * 100.0:.0f}% saved, want >= 10%), "
              f"both carrying {w_on['instances']} instances")
        if not ok:
            failures.append("forest-lod")

        # --- forest-shadow-lod: LOD must not cost the shadow pass draws -----
        # Rides forest-lod's pair, because the pair IS the experiment: LOD off
        # is the same casters with every ring boundary removed, so it is what
        # the shadow pass would submit if levels never split a batch.
        #
        # This is the arm that can see the change 11.90 actually made. The one
        # in the submission group cannot: instancing_fixture carries 1,042
        # triangles over two meshes, both under LOD_MIN_TRIANGLES, so every
        # item there sits at level 0 and the regroup is an identity permutation
        # -- deleting it outright would not move a number that arm reads.
        #
        # The bar is CONTENT, not mechanism, and it is worth saying so. The
        # mechanism bound is one extra partial chunk per (mesh, level, cascade)
        # group a level split creates; 25% is where that lands on this scatter,
        # diluted by the terrain quadtree's one-instance patch draws, which are
        # identical on both sides. Separation is 15x on the excess -- the same
        # comparison read +371% before 11.90 -- not the three orders of
        # magnitude an earlier draft of this comment claimed.
        #
        # Instances asserted equal for the reason forest-lod and forest-order
        # both assert it: visibility is bound-based and level-independent, so
        # the pair must see the same casters. Without it a regression that
        # OVER-culls lowers draws with LOD on and passes.
        #
        # Absent rather than defaulted, for the reason _forest_run's docstring
        # gives: a missing column read as 0 compares 0 against 0 and passes.
        s_on = lod_on["submit"].get("shadow cascades")
        s_off = lod_off["submit"].get("shadow cascades")
        if not s_on or not s_off:
            which = "LOD-on" if not s_on else "LOD-off"
            print(f"  forest-shadow-lod FAIL  no shadow cascades row in the {which} run")
            failures.append("forest-shadow-lod")
        elif not s_off["draws"]:
            print("  forest-shadow-lod FAIL  the LOD-off run drew no shadow casters at all")
            failures.append("forest-shadow-lod")
        else:
            same_cast = s_on["instances"] == s_off["instances"]
            cost = s_on["draws"] / s_off["draws"] - 1.0
            ok = cost <= 0.25 and same_cast
            print(f"  forest-shadow-lod {'PASS' if ok else 'FAIL'}  shadow draws "
                  f"{s_on['draws']} with LOD vs {s_off['draws']} without "
                  f"({cost * 100.0:+.0f}%, want <= +25%), both carrying {s_on['instances']} "
                  f"casters (want equal): levels must not fragment a pass that reorders freely")
            if not ok:
                failures.append("forest-shadow-lod")

    # --- forest-order: spatial ordering is what makes batching work ---------
    # The app's largest finding, and without this arm nothing defends it. The
    # guard is that instances and triangles must be IDENTICAL: the two runs draw
    # exactly the same geometry, and the only thing that changed is whether the
    # survivors of a frustum test ended up next to each other in the list.
    #
    # Both runs take --no-sort-opaque, because the depth sort REPLACES the
    # scatter order this arm is about: with it on the comparison reads 1684 vs
    # 1819 instead of 1287 vs 2368, which is the arm measuring the bucket count
    # rather than the Morton ordering. Sorting moves neither instances nor
    # triangles, so the base run stays usable as the sorted side elsewhere.
    sorted_run = _forest_run("morton", ["--no-sort-opaque"])
    unsorted_run = _forest_run("nosort", ["--no-sort-opaque", "--no-spatial-sort"])
    if sorted_run is None or unsorted_run is None:
        failures.append("forest-order")
    else:
        s = sorted_run["opaque"]
        u = unsorted_run["opaque"]
        m_draws, m_inst = s["draws"], s["instances"]
        same_work = u["instances"] == m_inst and u["triangles"] == s["triangles"]
        ok = same_work and m_draws < u["draws"]
        print(f"  forest-order {'PASS' if ok else 'FAIL'}  Morton {m_draws} draws vs unsorted "
              f"{u['draws']}, same {m_inst} instances and "
              f"{'same' if u['triangles'] == s['triangles'] else 'DIFFERENT'} "
              f"triangles (want fewer draws for identical work)")
        if not ok:
            failures.append("forest-order")

        # --- forest-shadow-order: the depth pass is order-INDEPENDENT --------
        # The complement of the arm above, on the same two runs, and an EXACT
        # equality rather than an inequality: the shadow pass compacts and
        # regroups, so its per-(mesh, level) counts depend on which casters a
        # layer wants and not on the order they arrive in. Scatter order is the
        # camera lane's problem alone since 11.90, and this is what says so.
        #
        # Free: both runs are already rendered and memoized for forest-order.
        so = sorted_run["submit"].get("shadow cascades")
        uo = unsorted_run["submit"].get("shadow cascades")
        if not so or not uo:
            print("  forest-shadow-order FAIL  no shadow cascades row in one of the pair")
            failures.append("forest-shadow-order")
        else:
            ok = so["draws"] == uo["draws"] and so["instances"] == uo["instances"]
            print(f"  forest-shadow-order {'PASS' if ok else 'FAIL'}  Morton {so['draws']} draws "
                  f"vs unsorted {uo['draws']} carrying {so['instances']} and {uo['instances']} "
                  f"casters (want both identical: reordering makes scatter order irrelevant here)")
            if not ok:
                failures.append("forest-shadow-order")

    # --- forest-rest: a character with no input does not travel -------------
    # Slope sliding is invisible from inside the app, because the camera follows
    # the thing that is drifting. It also survived one fix that looked right:
    # leaving ANY downward velocity on a grounded character makes ExtendedUpdate
    # resolve it along the surface, so a -2 m/s residual became 0.36 m/s downhill
    # on a 10-degree face. Asserted on the position, which cannot be argued with.
    rest = subprocess.run(
        [FOREST, "-x", "-f", "240", "-W", "320", "-H", "180", "--trace-player", "--no-fog"],
        capture_output=True, text=True)
    rest_text = rest.stdout + rest.stderr
    samples = _FOREST_TRACE.findall(rest_text)
    # Skip the first two: the capsule has to resolve contact before it can be
    # said to be standing on anything.
    settled = samples[2:]
    if rest.returncode != 0 or len(settled) < 4:
        print(f"  forest-rest  FAIL  exit {rest.returncode}, {len(samples)} trace samples: "
              f"{rest_text.strip()[-200:]}")
        failures.append("forest-rest")
    else:
        xs = [float(s[0]) for s in settled]
        zs = [float(s[2]) for s in settled]
        drift = max(abs(max(xs) - min(xs)), abs(max(zs) - min(zs)))
        # Grounded on every settled sample, or a character that fell THROUGH the
        # collider would pass: its x/z are constant too. The spec's physics
        # check asks for exactly this and nothing was asserting it.
        grounded = all(s[3] == "1" for s in settled)
        # And the ground has to be sloped, or the arm is vacuous -- a flat spawn
        # cannot express the defect, which is downhill travel.
        slope = min(float(s[4]) for s in settled)
        ok = drift < 0.05 and grounded and slope < 0.99
        print(f"  forest-rest  {'PASS' if ok else 'FAIL'}  idle drift {drift:.3f} units over "
              f"{len(settled)} samples (want < 0.05), grounded throughout: {grounded}, "
              f"ground normal y {slope:.3f} (want < 0.99, i.e. actually on a slope)")
        if not ok:
            failures.append("forest-rest")

    # --- forest-cull: the frustum still removes most of the world -----------
    away = _forest_run("away", [], cam=FOREST_CAM_AWAY)
    if away is None:
        failures.append("forest-cull")
        failures.append("overdraw-empty")
    else:
        # --- overdraw-empty: nothing shaded where nothing is drawn ----------
        # What is left here of the old overdraw-probe, which also tried to assert
        # the reading over the forest and could not: that number moves with the
        # AA mode, the draw order and the scatter, so it sat at 1.05 against a
        # > 1.0 bar. The exact half of the claim moved to overdraw-exact, where
        # the answer is an integer. This half still belongs on forest, because
        # only a real scene can be emptied by the CULLER rather than by framing.
        #
        # GL_SAMPLES_PASSED is the neighbouring primitive to GL_TIME_ELAPSED, and
        # this driver answers GL_TIMESTAMP with 0 while its scoped queries work,
        # so "the call was accepted" proves nothing: aimed at empty sky the only
        # reading that can be right is exactly 0, and over the forest it must not
        # be.
        sky = away["shading"]
        base_shading = base["shading"]
        ok = sky["shaded"] == 0 and base_shading["shaded"] > 0
        print(f"  overdraw-empty {'PASS' if ok else 'FAIL'}  {sky['shaded']} samples aimed at "
              f"sky (want exactly 0), {base_shading['shaded']} over the forest (want > 0; "
              f"complexity {base_shading['complexity']:.2f}, pinned by overdraw-exact)")
        if not ok:
            failures.append("overdraw-empty")

        a = away["opaque"]
        # Exact, not an inequality: aimed into the sky every mesh is outside the
        # frustum, so seen == culled and draws == 0.
        #
        # The second half stops a scene that failed to load from satisfying the
        # first, and it counts PROPS -- seen minus the terrain's own meshes.
        # Comparing raw `seen` across the two framings stopped working when the
        # terrain became a quadtree, because the aimed-away camera is somewhere
        # else and selects a different patch set. The scatter is what is supposed
        # to be identical between them, and now that is what is compared.
        props_away = a["meshes seen"] - away["terrain_meshes"]
        props_base = opaque["meshes seen"] - base["terrain_meshes"]
        ok = (props_away == props_base and a["meshes culled"] == a["meshes seen"]
              and a["draws"] == 0)
        print(f"  forest-cull  {'PASS' if ok else 'FAIL'}  aimed away: {a['meshes culled']} of "
              f"{a['meshes seen']} culled and {a['draws']} draws (want all culled, 0 draws), "
              f"carrying {props_away} prop meshes against the base framing's {props_base} "
              f"(want equal: the scatter does not move when the camera does)")
        if not ok:
            failures.append("forest-cull")

    return failures
