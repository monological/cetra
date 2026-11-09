"""The display group: display modes (placement, monitors, what a settings file may do; spec
12.15).
"""

import re
import os

from gatelib.harness import ROOT
from gatelib.probes import _gametest_probe


# "display <case> <label> <key> <numbers...>" from gametest --display-probe, read
# through the shared four-field reader. Same no-letters rule as the others: a nan
# prints a line this cannot match, the key vanishes, and the arm fails on absence.
_DISPLAY_PROBE = re.compile(r"^display ([\w-]+) (\w+) (\w+)((?:\s+-?[\d.]+)+)$", re.M)

_GAME_SRC_DIR = os.path.join(ROOT, "cetra", "src", "game")


def _display_probe(case):
    """{(label, key): [floats]} from one gametest --display-probe run, or None."""
    return _gametest_probe("--display-probe", _DISPLAY_PROBE, case)


def run_display_gate(workdir):
    """Display modes: where a window goes, which screen it goes to, and what a
    settings file is allowed to do to a headless run (spec 12.15).

      display-placement    windowed replays the geometry it left with, exactly; fullscreen
                           and borderless fill the monitor; only borderless is undecorated
      display-monitors     a display's own name resolves to its own index, and an unknown
                           name, an empty one and NULL all resolve to the primary
      display-apply        a settings file's vsync reaches live engine state
      display-headless     ... and its window mode does NOT, on a run with no visible window
      display-vsync-writer settings.c sets no swap interval of its own

    The EFFECT of a mode change is not asserted here and cannot be. engine_set_window_mode
    refuses under headless, because a suite that seized a display would be intolerable and
    a golden whose frame size came from whatever monitor the machine has would not be a
    golden. So the decision is split from the effect -- engine_window_placement is a pure
    function of the mode, the monitor rectangle and the saved geometry -- and it is the
    decision these arms read. That the refusal HOLDS is display-headless, which is the arm
    protecting every other golden in the corpus.

    What is left over for a human is in docs/verification.md: a window actually moving, a
    second display, and the two platform settings paths.

    The probe's monitor rectangle is 2560x1440 at 1600,300 -- an origin that is not 0,0,
    because a placement which ignored the monitor's POSITION would land correctly on a
    single-display machine and put a borderless window off the edge of the second one.
    """
    del workdir # the probe needs no scratch: nothing is rendered and nothing is written
    failed = []

    def note(name, ok):
        # Bookkeeping only. Each verdict line prints its arm name as a literal,
        # because that source text is what gate-arm-docs reads to tell the
        # documented list from the one that actually runs.
        if not ok:
            failed.append(name)

    p = _display_probe("placement")
    if not p:
        print("  display-placement SKIP  no probe output")
        return ["display-placement"]
    win = p[("windowed", "rect")]
    full = p[("fullscreen", "rect")]
    bord = p[("borderless", "rect")]
    # fullscreen, decorated, x, y, w, h
    ok = (win == [0, 1, 40, 50, 640, 480] and full[0] == 1 and full[1] == 1 and
          full[2:] == [0, 0, 2560, 1440] and bord[0] == 0 and bord[1] == 0 and
          bord[2:] == [1600, 300, 2560, 1440])
    detail = (f"windowed {win[2:] !r} (want the saved 40,50 640x480 back exactly), "
              f"fullscreen takes the monitor ({full[0]:.0f}) at {full[4]:.0f}x{full[5]:.0f}, "
              f"borderless stays a window ({bord[0]:.0f}), undecorated ({bord[1]:.0f}), "
              f"at the monitor's own origin {bord[2]:.0f},{bord[3]:.0f}")
    print(f"  display-placement {'PASS' if ok else 'FAIL'}  {detail}")
    note("display-placement", ok)

    m = _display_probe("monitors")
    if not m:
        print("  display-monitors SKIP  no probe output")
        note("display-monitors", False)
    elif m[("list", "count")][0] < 1:
        # A VM or a CI box can genuinely enumerate none, and a red suite there
        # would be noise about the machine rather than about the code.
        print("  display-monitors SKIP  no display attached")
    else:
        count = m[("list", "count")][0]
        ok = (m[("resolve", "misresolved")][0] == 0 and m[("resolve", "unknown")][0] == 0 and
              m[("resolve", "empty")][0] == 0 and m[("resolve", "null")][0] == 0)
        detail = (f"{count:.0f} display(s); every name resolves to its own index "
                  f"({m[('resolve', 'misresolved')][0]:.0f} wrong, want 0) and an unknown "
                  f"name, an empty one and NULL all give the primary "
                  f"({m[('resolve', 'unknown')][0]:.0f}/{m[('resolve', 'empty')][0]:.0f}/"
                  f"{m[('resolve', 'null')][0]:.0f}, want 0/0/0)")
        print(f"  display-monitors {'PASS' if ok else 'FAIL'}  {detail}")
        note("display-monitors", ok)

    a = _display_probe("apply")
    if not a:
        print("  display-apply SKIP  no probe output")
        note("display-apply", False)
        print("  display-headless SKIP  no probe output")
        note("display-headless", False)
    else:
        vsync = a[("engine", "vsync")][0]
        ok = vsync == 0
        print(f"  display-apply {'PASS' if ok else 'FAIL'}  a file asking for vsync off "
              f"reaches the engine ({vsync:.0f}, want 0); before 12.15 the value was "
              f"consumed at init and unrecoverable")
        note("display-apply", ok)

        mode = a[("engine", "mode")][0]
        ok = mode == 0
        print(f"  display-headless {'PASS' if ok else 'FAIL'}  a file asking for borderless "
              f"leaves a headless run windowed ({mode:.0f}, want 0) -- every golden in the "
              f"corpus depends on this refusal")
        note("display-headless", ok)

    # Static, like ui-elements-closed: no process, and it holds a single-writer
    # rule that no runtime assertion can see. Two writers of the swap interval is
    # how the value became unrecoverable in the first place.
    #
    # The whole game layer rather than settings.c alone: the rule is that this
    # layer does not set the interval, and engine.h publicly includes GLFW, so
    # any file here can become the second writer. Scoping it to the one file
    # that broke it once would cover the fix and not the rule.
    try:
        offenders = []
        for name in sorted(os.listdir(_GAME_SRC_DIR)):
            if not name.endswith(".c"):
                continue
            with open(os.path.join(_GAME_SRC_DIR, name), "r", encoding="utf-8") as fh:
                # The open paren is what makes this a CALL and not prose: the
                # comment at the apply site names the function to say it is
                # deliberately not used, and an arm that failed on its own
                # explanation would be read as noise and deleted, not believed.
                hits = fh.read().count("glfwSwapInterval(")
            if hits:
                offenders.append(f"{name} x{hits}")
        ok = not offenders
        detail = (f"no file in cetra/src/game calls glfwSwapInterval (want none: it goes "
                  f"through engine_set_vsync, so the engine can put the value back after "
                  f"a mode change drops it)"
                  if ok else "called by " + ", ".join(offenders))
    except OSError as exc:
        ok, detail = False, f"could not read cetra/src/game: {exc}"
    print(f"  display-vsync-writer {'PASS' if ok else 'FAIL'}  {detail}")
    note("display-vsync-writer", ok)

    return failed
