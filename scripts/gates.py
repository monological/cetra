#!/usr/bin/env python3
"""Run the working-space contract gates (specs/10.1, 10.2).

    python3 scripts/gates.py [--keep]

Every gate here is SELF-VERIFYING: it compares two renders of the same build,
so there is no stored reference image to regenerate and no baseline to drift.
That is the whole reason these can run in CI where the pixel goldens cannot --
a golden needs a "before", a scale gate does not.

WHY THESE EXIST (spec 10.2): 10.1 shipped with nine goldens green and three
space-mixing bugs live. The goldens all pin exposure, so `preExposure == 1` and
multiplying by it is the identity -- they are structurally blind to the entire
class. The one gate that was not blind, scale invariance, ran on a single
punctual-lit scene, so it covered one of the four paths that write into the HDR
buffer.

The rule: a gate that pins exposure cannot see a space-mixing bug.
"""

import argparse
import os
import shutil
import sys
import tempfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="keep the generated scenes and frames")
    ap.add_argument("--only", metavar="SEL",
                    help="run only groups whose selector contains SEL (comma-separated)")
    ap.add_argument("--arms", metavar="SEL",
                    help="run only the arm named SEL, or the arms whose name contains it "
                         "(comma-separated), and only the groups holding them; with --only, "
                         "within those groups")
    ap.add_argument("--list", action="store_true", help="list group selectors and exit")
    ap.add_argument("--list-arms", action="store_true",
                    help="list each group's arms (under --only, those groups') and exit")
    ap.add_argument("--bin-dir", metavar="DIR",
                    help="where to find the app binaries (default out/bin; "
                         "out/release/bin runs forest ~5x faster)")
    args = ap.parse_args()

    # gatelib looks its binaries up as it is imported, so --bin-dir reaches it through the
    # environment, before the import. Rebinding the names here instead would reach this file
    # alone: every module that imported RENDER would keep the default it saw.
    if args.bin_dir:
        os.environ["CETRA_BIN_DIR"] = os.path.abspath(args.bin_dir)
    from gatelib.arms import arms_printed, arms_ran, choose, guards
    from gatelib.groups import GATE_GROUPS
    from gatelib.harness import BIN_DIR, CALIBRATED_FB_SCALE, RENDER, _detect_fb_scale

    if args.list:
        for selector, banner, _ in GATE_GROUPS:
            print(f"  {selector:<22} {banner}")
        return 0

    groups = GATE_GROUPS
    if args.only:
        wanted = [s.strip() for s in args.only.split(",") if s.strip()]
        groups = [g for g in GATE_GROUPS if any(w in g[0] for w in wanted)]
        # A selector that matches nothing is a typo, and running zero gates while
        # reporting success is the worst outcome this script has.
        if not groups:
            sys.exit(f"--only {args.only!r} matched no group; --list to see them")

    if args.list_arms:
        for selector, _, fn in groups:
            names = arms_printed(fn)
            how = "chosen one by one" if guards(fn) else "runs whole under --arms"
            print(f"  {selector} ({len(names)} arms, {how})")
            for name in names:
                print(f"      {name}")
        return 0

    # The arms chosen, by group. Read from the verdict lines in each group's source, so an arm
    # whose name is built at run time cannot be chosen; its group's other arms still can. A
    # selector that is an arm's whole name chooses that arm alone, or ui-layout could never be run
    # without ui-layout-resize; any other matches by substring.
    chosen = {}
    if args.arms:
        wanted = [s.strip() for s in args.arms.split(",") if s.strip()]
        names = {n for _, _, fn in groups for n in arms_printed(fn)}
        whole = {w for w in wanted if w in names}
        for selector, _, fn in groups:
            hits = [n for n in arms_printed(fn)
                    if n in whole or any(w in n for w in wanted if w not in whole)]
            if hits:
                chosen[selector] = hits
        if not chosen:
            sys.exit(f"--arms {args.arms!r} matched no arm; --list-arms to see them")
        groups = [g for g in groups if g[0] in chosen]
        choose(n for hits in chosen.values() for n in hits)

    if not os.path.exists(RENDER):
        sys.exit(f"{RENDER} not found -- run ./build.sh first")
    # Said out loud on every run: which binaries these numbers describe is part
    # of the result, not a detail of the harness.
    print(f"binaries: {BIN_DIR}")
    # And the lever, where it will actually be read, because a flag nobody
    # remembers is a flag nobody uses. Only offered when a release tree exists:
    # suggesting it otherwise is suggesting a build, which is the user's call.
    if BIN_DIR.endswith(os.path.join("out", "bin")):
        print("          (forest runs ~5x faster from a release build -- "
              "./build.sh --release and it is picked up automatically)")
    # And what --arms chose, before anything runs: a group that does not guard its arms runs
    # all of them, which is minutes more than the arms asked for and should never be a surprise.
    for selector, _, fn in groups:
        if selector in chosen:
            whole = "" if guards(fn) else " -- runs whole, its arms are not guarded"
            print(f"arms: {selector}: {', '.join(chosen[selector])}{whole}")

    workdir = tempfile.mkdtemp(prefix="cetra_gates_")
    # The suite's own cook cache (spec 11.99), never the developer's persistent
    # one: a repo-root cache may hold artefacts cooked by ANOTHER BUILD, and a
    # gate reading them would land cross-build content in triangle counts and
    # pixels silently. One dir per run means everything a gate reads was cooked
    # by the binary under test; arms that measure a LIVE bake still pass
    # --no-cook on top.
    os.environ["CETRA_COOK_DIR"] = os.path.join(workdir, "cook")
    # Said out loud for the same reason the binary directory is: the framebuffer
    # a number was measured on is part of the number. 2 is what this suite's
    # sample coordinates were authored against, so it renders unchanged; 1 asks
    # for double and lands on the same buffer.
    scale = _detect_fb_scale(workdir)
    print(f"framebuffer: {scale}x the requested size"
          + ("" if scale == CALIBRATED_FB_SCALE
             else f" -- requests scaled x{CALIBRATED_FB_SCALE // scale} to reach the "
                  f"{CALIBRATED_FB_SCALE}x buffer this suite was calibrated on"))
    failures = []
    try:
        for _, banner, fn in groups:
            print(banner)
            failures += fn(workdir)
    finally:
        if args.keep:
            print(f"\nartifacts in {workdir}")
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    # A chosen arm a guarding group never let run: its guard names another arm, or it sits in a
    # branch the run did not take. Passing it as nothing to do would report a check never made.
    unrun = [n for selector, _, fn in groups if guards(fn)
             for n in chosen.get(selector, ()) if n not in arms_ran()]
    if unrun:
        print(f"\nchosen but never run: {', '.join(unrun)}")
        failures += unrun

    scope = "" if groups is GATE_GROUPS else f" ({len(groups)} of {len(GATE_GROUPS)} groups)"
    if chosen:
        scope = (f" ({sum(len(h) for h in chosen.values())} chosen arms in {len(groups)} "
                 f"of {len(GATE_GROUPS)} groups)")
    if failures:
        print(f"\n{len(failures)} gate(s) failed{scope}: {', '.join(failures)}")
        return 1
    print(f"\nall gates passed{scope}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
