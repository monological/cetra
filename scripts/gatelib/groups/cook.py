"""The cook group: the derived-data cook (transparent cache + --cook pre-warm; spec 11.99)."""

import re
import os
import time

from gatelib.harness import FOREST, RENDER, ROOT, _run, asset
from gatelib.images import compare
from gatelib.probes import _COOK_ROW
from gatelib.scenes import TEXCOMP_FIXTURE
from gatelib.world import _CLUSTER_PROBE, _REGION_CELL


_COOK_SUMMARY = re.compile(r"^cook-summary \S+ cooked=(\d+) hit=(\d+) miss=(\d+) refused=(\d+)",
                           re.M)


def _cook_counts(text):
    """The cook-summary's counters, zero when the row is absent."""
    summary = _COOK_SUMMARY.search(text)
    return {
        "cooked": int(summary.group(1)) if summary else 0,
        "hit": int(summary.group(2)) if summary else 0,
        "refused": int(summary.group(4)) if summary else 0,
    }


def _cook_run(tag, extra, shot=None):
    """One forest run for the cook group: 320x200, two frames, render mode 6
    (the albedo debug view -- forest's one 0 px path, and the one every cooked
    texture lands in). Returns {text, rows, cooked, hit, refused, digests,
    dag, seconds}."""
    # Eroded on purpose: the field feeds the splat, the scatter and the terrain
    # geometry, so a wrong restored plane moves the region digests AND the
    # frame -- the erosion leg of every identity claim rides the fixture.
    cmd = [FOREST, "-x", "-f", "2", "-W", "320", "-H", "200", "--render-mode", "6",
           "--region-probe", "--cluster-probe", "--erode", "--erode-res", "257",
           "--erode-iterations", "60"] + extra
    if shot:
        cmd += ["-S", shot]
    t0 = time.perf_counter()
    r = _run(cmd, capture_output=True, text=True)
    seconds = time.perf_counter() - t0
    if r.returncode != 0:
        print(f"  cook         ERROR {tag} exited {r.returncode}")
        return None
    text = r.stdout + r.stderr
    out = _cook_counts(text)
    out["text"] = text
    out["rows"] = _COOK_ROW.findall(text)
    out["seconds"] = seconds # cook-speed's reading; free, since the runs exist anyway
    # The region digests carry the collider flag and the scatter -- the
    # scatter is never cooked, so it is the CONTROL; the collider flag is the
    # restored-Jolt-shape leg. The cluster rows are the DAG leg: band digests
    # read from whatever index bytes the run drew with. Both through the
    # groups' own compiled parsers, so a probe-format change moves every
    # consumer together instead of leaving this one silently matching less.
    out["digests"] = sorted(_REGION_CELL.findall(text))
    out["dag"] = sorted(_CLUSTER_PROBE.findall(text))
    return out


def run_cook_gate(workdir):
    """The derived-data cook (spec 11.99): a transparent content-addressed
    cache over the deterministic startup bakes, plus the --cook pre-warm verb.

      cook-recipe   every cook_key literal in the tree fits COOK_RECIPE_MAX,
                    which counts the NUL. Static, and first because an
                    over-long name is refused where the key is BUILT: the site
                    then reports no miss, no refusal and no store failure, so
                    every counter below reads as though it had never been
                    written at all.
      cook-identity a warm cooked run equals a live --no-cook run: the
                    render-mode-6 frame at 0 px (every cooked texture lands in
                    it) and the region digests equal (the uncooked control).
      cook-hit      the second run of one command against one dir bakes
                    nothing: cooked=0, and its hits equal the cold run's cooks.
      cook-lever    --no-cook against a warm dir runs live: zero cook rows,
                    and the frame still equals the live run's.
      cook-miss     one artefact corrupted byte-wise is refused BY NAME,
                    re-cooked in the same run, and the frame still equals the
                    live run's -- the fallback that makes a torn cache cost a
                    bake, never a pixel.
      cook-determinism two cold cooks into two dirs are byte-identical,
                    artefact by artefact, within one build.
      cook-key      a dir warmed at seed 1337 serves seed 4242 exactly the
                    seed-blind sites (veg, the DAGs -- their seeds are
                    hard-coded) and re-cooks the seeded ones (layers, splat,
                    Jolt regions, the field), every listed recipe is SEEN (the
                    tripwire for a version bump silently unhooking a site;
                    texture-mips is exempt, classified by content not name),
                    and 4242's frame and digests equal its own live run -- the
                    key IS the identity, in both directions.
      cook-texture  the render app's cooked mip chains: the texcomp fixture --
                    the one whose camera actually MINIFIES, so the chain is
                    sampled -- at 0 px warm vs live, with the warm run
                    reporting texture hits and no cooks.
      cook-speed    cold, warm and --no-cook wall clocks read off the
                    identity arms' own launches (zero extra processes, single
                    samples, said so), REPORT-ONLY and deliberately
                    unasserted: startup wall clock in a suite run has no
                    honest noise floor (11.65 measured startups externally),
                    and the ASSERTED form of the speed claim is cook-hit's
                    cooked=0 -- a count with no floor at all.

    Every run uses its OWN cook dir under the workdir, never the suite's
    shared one: these arms measure the cache itself, so what is in the dir
    must be exactly what this group put there.
    """
    failures = []

    # --- cook-recipe --------------------------------------------------------
    # Static and FIRST, because what it catches is invisible to every counter
    # the arms below read. cook_key refuses a recipe that does not fit and
    # returns an invalid key; fetch and store then return before they touch
    # hits, misses, refusals or store failures, so an over-long name subtracts
    # a site from the summary rather than appearing anywhere in it -- which
    # reads exactly like a site nobody ever wrote. Running it ahead of the
    # renders also keeps it alive when one of them fails and the group returns
    # early.
    #
    # The limit is read from the header rather than restated here: it is the
    # size of the container's recipe field, and a second copy of a number is a
    # second thing to keep in step.
    with open(os.path.join(ROOT, "cetra", "src", "cook.h"), encoding="utf-8") as fh:
        found = re.search(r"#define\s+COOK_RECIPE_MAX\s+(\d+)", fh.read())
    recipe_max = int(found.group(1)) if found else 0
    sites = []
    for base in ("cetra/src", "apps"):
        for dirpath, _dirs, names in os.walk(os.path.join(ROOT, base)):
            for name in sorted(names):
                if not name.endswith((".c", ".cpp", ".h")):
                    continue
                path = os.path.join(dirpath, name)
                with open(path, encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
                for recipe in re.findall(r'cook_key\("([^"]*)"\)', text):
                    sites.append((os.path.relpath(path, ROOT), recipe))
    over = [f"{p}: '{r}' is {len(r)}" for p, r in sites if len(r) >= recipe_max]
    ok = recipe_max > 0 and bool(sites) and not over
    if not recipe_max:
        detail = "COOK_RECIPE_MAX not found in cetra/src/cook.h"
    elif not sites:
        detail = "no cook_key literals found -- the call shape or this scan has moved"
    elif over:
        detail = (f"{len(over)} of {len(sites)} recipes exceed the {recipe_max - 1} usable "
                  f"characters: {'; '.join(over)}")
    else:
        detail = (f"{len(sites)} recipe literals, longest {max(len(r) for _p, r in sites)} "
                  f"of {recipe_max - 1} usable ({recipe_max} counts the NUL)")
    print(f"  cook-recipe  {'PASS' if ok else 'FAIL'}  {detail}")
    if not ok:
        failures.append("cook-recipe")

    d1 = os.path.join(workdir, "cook_d1")
    d2 = os.path.join(workdir, "cook_d2")
    live_ppm = os.path.join(workdir, "cook_live.ppm")
    warm_ppm = os.path.join(workdir, "cook_warm.ppm")
    lever_ppm = os.path.join(workdir, "cook_lever.ppm")
    miss_ppm = os.path.join(workdir, "cook_miss.ppm")

    live = _cook_run("live", ["--no-cook"], shot=live_ppm)
    cold = _cook_run("cold", ["--cook-dir", d1])
    warm = _cook_run("warm", ["--cook-dir", d1], shot=warm_ppm)
    if live is None or cold is None or warm is None:
        return ["cook"]

    # --- cook-identity ------------------------------------------------------
    ae, _ = compare(live_ppm, warm_ppm)
    digests_eq = bool(live["digests"]) and live["digests"] == warm["digests"]
    dag_eq = bool(live["dag"]) and live["dag"] == warm["dag"]
    ok = ae == 0 and digests_eq and dag_eq
    print(f"  cook-identity {'PASS' if ok else 'FAIL'}  warm vs live: {ae} px, "
          f"{len(live['digests'])} region digests equal={digests_eq} (collider flags ride "
          f"them), {len(live['dag'])} cluster rows equal={dag_eq} (want 0 px and equality "
          f"throughout: a cook hit must be the bake it replaced, byte for byte where bytes "
          f"are compared)")
    if not ok:
        failures.append("cook-identity")

    # --- cook-hit -----------------------------------------------------------
    ok = cold["cooked"] > 0 and warm["cooked"] == 0 and warm["hit"] == cold["cooked"]
    print(f"  cook-hit     {'PASS' if ok else 'FAIL'}  cold cooked {cold['cooked']}, warm "
          f"cooked {warm['cooked']} with {warm['hit']} hits (want >0, 0, and equality: the "
          f"second run of one command must bake nothing)")
    if not ok:
        failures.append("cook-hit")

    # --- cook-lever ---------------------------------------------------------
    lever = _cook_run("lever", ["--cook-dir", d1, "--no-cook"], shot=lever_ppm)
    if lever is None:
        failures.append("cook-lever")
    else:
        ae, _ = compare(live_ppm, lever_ppm)
        # live's row count rides along free: it is the same claim on the run
        # already in hand, and a row-regex rot would zero both.
        ok = len(lever["rows"]) == 0 and len(live["rows"]) == 0 and ae == 0
        print(f"  cook-lever   {'PASS' if ok else 'FAIL'}  --no-cook against the warm dir: "
              f"{len(lever['rows'])} cook rows ({len(live['rows'])} on the live run) and "
              f"{ae} px vs live (want 0 and 0: the lever must run the bakes and touch "
              f"nothing)")
        if not ok:
            failures.append("cook-lever")

    # --- cook-miss ----------------------------------------------------------
    # Corrupt ONE artefact byte-wise, mid-payload -- the terrain-stream-refuse
    # idiom: the size stays right, so only the payload hash can catch it.
    victims = sorted(f for f in os.listdir(d1) if f.endswith(".cca"))
    if not victims:
        print("  cook-miss    FAIL  the cold cook left no artefacts to corrupt")
        failures.append("cook-miss")
    else:
        victim = os.path.join(d1, victims[0])
        with open(victim, "r+b") as f:
            f.seek(os.path.getsize(victim) // 2)
            byte = f.read(1)
            f.seek(-1, 1)
            f.write(bytes([byte[0] ^ 0x40]))
        miss = _cook_run("miss", ["--cook-dir", d1], shot=miss_ppm)
        if miss is None:
            failures.append("cook-miss")
        else:
            ae, _ = compare(live_ppm, miss_ppm)
            # The COUNTER, not a substring: "refused" appears in every
            # cook-summary row (refused=0), so a text search is vacuously
            # true. The filename search stays -- it is what makes the refusal
            # NAMED.
            refused_named = miss["refused"] >= 1 and victims[0] in miss["text"]
            recooked = miss["cooked"] >= 1
            ok = refused_named and recooked and ae == 0
            print(f"  cook-miss    {'PASS' if ok else 'FAIL'}  corrupt {victims[0]}: "
                  f"named={refused_named} recooked={recooked} {ae} px vs live (want "
                  f"named, re-cooked and 0 px: a torn cache costs a bake, never a pixel)")
            if not ok:
                failures.append("cook-miss")

    # --- cook-determinism ---------------------------------------------------
    second = _cook_run("cold2", ["--cook-dir", d2])
    if second is None:
        failures.append("cook-determinism")
    else:
        a = sorted(f for f in os.listdir(d1) if f.endswith(".cca"))
        b = sorted(f for f in os.listdir(d2) if f.endswith(".cca"))
        same_names = a == b and len(a) > 0
        diffs = []
        if same_names:
            for name in a:
                with open(os.path.join(d1, name), "rb") as fa:
                    da = fa.read()
                with open(os.path.join(d2, name), "rb") as fb:
                    db = fb.read()
                if da != db:
                    diffs.append(name)
        ok = same_names and not diffs
        print(f"  cook-determinism {'PASS' if ok else 'FAIL'}  {len(a)} artefacts in both "
              f"dirs, {len(diffs)} differ (want same names and zero diffs: two cold cooks "
              f"of one build are one cook)")
        if not ok:
            failures.append("cook-determinism")

    # --- cook-key: the identity cuts both ways ------------------------------
    live42_ppm = os.path.join(workdir, "cook_live42.ppm")
    key42_ppm = os.path.join(workdir, "cook_key42.ppm")
    live42 = _cook_run("live42", ["--no-cook", "--seed", "4242"], shot=live42_ppm)
    key42 = _cook_run("key42", ["--cook-dir", d1, "--seed", "4242"], shot=key42_ppm)
    if live42 is None or key42 is None:
        failures.append("cook-key")
    else:
        # texture-mips is deliberately in NEITHER set: its rows land on both
        # sides of the key by CONTENT (bark chains hit, layer/splat chains
        # re-cook), so it cannot be classified by site name.
        seed_blind = {"veg-bark/1", "veg-leaf/2", "cluster-dag/1"}
        seeded = {"terrain-layer/1", "terrain-splat/1", "jolt-region/1", "erosion-field/1"}
        seen = {site for site, _, _, _, _ in key42["rows"]}
        # The tripwire for the rot that WILL fire: a recipe version bump
        # ("terrain-layer/1" -> "/2") silently unhooks its site from these
        # sets, and the arm would stay green on pixels alone.
        missing = (seed_blind | seeded) - seen
        wrong = [(site, result) for site, _, result, _, _ in key42["rows"]
                 if (site in seed_blind and result != "hit") or
                    (site in seeded and result != "cooked")]
        ae, _ = compare(live42_ppm, key42_ppm)
        ok = (not missing and not wrong and ae == 0 and
              live42["digests"] == key42["digests"] and live42["dag"] == key42["dag"])
        print(f"  cook-key     {'PASS' if ok else 'FAIL'}  seed 4242 against the 1337 dir: "
              f"{len(wrong)} sites on the wrong side of the key, {len(missing)} listed sites "
              f"unseen, and {ae} px vs its own live run (want 0, 0 and 0: seed-blind sites "
              f"hit, seeded sites re-cook, nothing cross-serves, and every listed recipe "
              f"still exists under the name this arm knows it by)")
        if not ok:
            failures.append("cook-key")

    # --- cook-texture: the render app's cooked mip chains -------------------
    dt = os.path.join(workdir, "cook_dt")
    tex_live_ppm = os.path.join(workdir, "cook_tex_live.ppm")
    tex_warm_ppm = os.path.join(workdir, "cook_tex_warm.ppm")
    scene = asset(TEXCOMP_FIXTURE)

    def texture_run(tag, extra, shot=None):
        cmd = [RENDER, "-m", scene, "-x", "-f", "2", "-W", "400", "-H", "300",
               "--no-auto-exposure", "-E", "1.0"] + extra
        if shot:
            cmd += ["-S", shot]
        r = _run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  cook         ERROR {tag} exited {r.returncode}")
            return None
        text = r.stdout + r.stderr
        out = _cook_counts(text)
        out["tex_rows"] = len([s for s, _, _, _, _ in _COOK_ROW.findall(text)
                               if s == "texture-mips/4"])
        return out

    tex_live = texture_run("tex_live", ["--no-cook"], shot=tex_live_ppm)
    tex_cold = texture_run("tex_cold", ["--cook-dir", dt])
    tex_warm = texture_run("tex_warm", ["--cook-dir", dt], shot=tex_warm_ppm)
    if tex_live is None or tex_cold is None or tex_warm is None:
        failures.append("cook-texture")
    else:
        ae, _ = compare(tex_live_ppm, tex_warm_ppm)
        ok = (ae == 0 and tex_cold["cooked"] > 0 and tex_warm["cooked"] == 0 and
              tex_warm["tex_rows"] > 0)
        print(f"  cook-texture {'PASS' if ok else 'FAIL'}  {ae} px warm vs live on the "
              f"minifying fixture, {tex_cold['cooked']} chains cooked then {tex_warm['tex_rows']} "
              f"hit with {tex_warm['cooked']} re-cooks (want 0 px, >0, >0, 0: the cooked chain "
              f"is the derived chain, byte for byte)")
        if not ok:
            failures.append("cook-texture")

    # --- cook-speed: the clocks, for the ledger -----------------------------
    # Read off the group's OWN cold/warm/live launches -- zero extra
    # processes, and the config the numbers describe is exactly the eroded
    # fixture every other arm uses. Single samples, said so.
    print(f"  cook-speed   PASS  cold {cold['seconds']:.2f} s, warm {warm['seconds']:.2f} s, "
          f"live {live['seconds']:.2f} s (single samples off the identity runs) -- reported, "
          f"deliberately unasserted: the asserted form of this claim is cook-hit's cooked=0")

    return failures
