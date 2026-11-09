"""The graph group: the animation state machine (states, transitions, binding; spec 12.20)."""

import functools
import re
import os
import subprocess

from gatelib.harness import GAMETEST
from gatelib.probes import _PROBE_TIMEOUT, _gametest_probe_text


_GRAPH_PROBE = re.compile(r"^graph ([\w-]+) ([\w-]+) (\w+)((?:\s+-?[\d.]+)+)$", re.M)
# The state column --trace-player appends LAST, after the camera and the character
# heading, by the rule that block states three times: appended, never inserted,
# because every regex reading the line counts its groups from the left.
_GRAPH_TRACE = re.compile(r"player step (\d+) .* graph (\w+) ([\d.]+)")

# The three scripts the parity arms drive, and every frame index in them was
# MEASURED rather than predicted -- the plate lip, the drop and the pool are all
# functions of an eroded height field with two noise warps, and two of the plan's
# predictions about this app were wrong.
#
# Read at every step rather than every tenth: the landing's fade is 0.08 s and the
# rise is under a second, so a ten-step cadence steps over both.
_GRAPH_GROUND = "0-59 idle\n60-179 ly=-1\n180-182 rt=1\n183-259 idle\n260-262 lt=1\n263-330 idle\n"
_GRAPH_AIR = "0-59 idle\n60-119 ly=-1\n120-122 a\n123-300 idle\n"
# Walk forward and never stop: the plate's lip is about 500 steps out and the fall
# to the water is another 260.
_GRAPH_WATER = "0-59 idle\n60-900 ly=-1\n"


def _graph_trace(workdir, tag, script, frames, extra=None):
    """The RLE state sequence from one scripted run: [(step, state), ...] at each
    change, or None if the run failed or traced nothing.

    The sequence is what ships in the arm, spelled out. A recording is how the
    sequence was LEARNED -- 12.19's Phase 0 pattern -- but a literal list is what a
    reviewer can read and disagree with, where a blob is not.
    """
    path = os.path.join(workdir, f"graph_{tag}.txt")
    with open(path, "w") as f:
        f.write(script)
    # 160x100, because what these arms read is a TEXT column and every pixel is
    # thrown away. The trace is byte-identical at the default 1280x720 and the
    # water leg costs 58.6 s there against 22.9 s here -- the state sequence is a
    # function of the physics and the pad script, not of the resolution. Stated
    # rather than inherited, which is what every render() call in the suite does.
    try:
        r = subprocess.run(
            [GAMETEST, "-x", "-f", str(frames), "-W", "160", "-H", "100", "--no-crates",
             "--no-chaser", "--trace-player", "--trace-every", "1", "--pad-script", path]
            + (extra or []),
            capture_output=True, text=True, timeout=_PROBE_TIMEOUT)
    except subprocess.TimeoutExpired:
        # A timeout reads as a failed run, since that is what a caller can act on
        # -- _gametest_probe_text's rule. Raising here would take the whole group
        # down instead of failing the four arms that depend on it.
        return None
    text = r.stdout + r.stderr
    samples = _GRAPH_TRACE.findall(text)
    if r.returncode != 0 or not samples:
        return None
    seq = []
    for step, state, _secs in samples:
        if not seq or seq[-1][1] != state:
            seq.append((int(step), state))
    return seq


@functools.cache
def _graph_probe_text(case):
    """One gametest --graph-probe run, cached by case.

    Every case here has exactly ONE reader today, so the cache never hits -- it is
    `_cam_probe_text`'s shape kept for the day a second arm reads another column
    of one case, and it is honest to say that rather than to claim a saving it is
    not making.
    """
    text, rc = _gametest_probe_text("--graph-probe", case)
    return None if rc != 0 else text


def _graph_probe(case):
    """{(label, key): [floats]} from a --graph-probe case.

    No window, no GL and for most cases no engine either: a graph is a table over
    named values, so an arm that needed a frame would be evidence the design had
    slipped -- `camera_rig`'s rule, and `ui_layout`'s before it.
    """
    text = _graph_probe_text(case)
    if not text:
        return None
    return {(label, key): [float(v) for v in nums.split()]
            for c, label, key, nums in _GRAPH_PROBE.findall(text) if c == case}


def run_graph_gate(workdir):
    """What PLAYS and WHEN (spec 12.20): a table of states, a table of transitions
    over a closed condition vocabulary, and a bind that resolves every state
    against the rig actually loaded.

    Every arm here binds a graph to a NULL animator over states that play
    nothing, so what is under test is the TABLE and nothing else -- no window, no
    GL, no rig, no clip. An arm that needed a frame would be evidence the design
    had slipped, which is `ui_layout`'s rule and `camera_rig`'s after it.

      graph-order       Two rows out of one state, both satisfied: the EARLIER
                        row wins, and swapping the two swaps the answer. Priority
                        is row order and nothing else.
      graph-any         from == NULL fires from any state -- and a SPECIFIC row
                        above it beats it, because there is one table and one
                        order with no separate any-state pass.
      graph-and         Two conditions on a row are ANDed. Two-sided: either
                        alone fires nothing, or an implementation that ORs passes
                        the both-true leg unnoticed.
      graph-trigger     Four legs. A trigger fires once, does not re-fire, is
                        GONE at the end of a tick that consumed nothing -- and a
                        BOOL under the identical table persists. The last leg is
                        what tells the two apart.
      graph-elapsed     Time IN STATE: zero on the entry tick, 30 ticks of 1/60
                        to cross half a second, and reset by entering elsewhere.
      graph-hysteresis  What a bool cannot say. A value bobbing either side of a
                        threshold settles in ONE state under two bands and flips
                        on nearly every tick under one -- which is the live
                        defect in the machine this replaces, as an arm.
      graph-reenter     A row into the state already current is never taken, so
                        an interrupt that STAYS true does not re-issue its own
                        play every tick and hold the clip at its first frame.
      graph-disabled    A stood-down graph freezes -- no row, no clock, nothing
                        consumed -- and coming back resumes where it was rather
                        than at the start. A ragdoll is that case.
      graph-instances   Two graphs over ONE borrowed const table hold different
                        states and different clocks. The table is data; the
                        instance is where everything that moves lives.
      graph-unreachable A state whose source never registered is named at bind
                        and every row into it is PRUNED, so the row below fires
                        instead -- two-sided against the same table with nothing
                        missing. And the path a real rig takes to get there: a
                        clip it does not carry arrives as a NULL entry, which
                        refuses the whole source rather than registering one that
                        cannot play.
      graph-authoring   Six ways a table can be wrong, each refused BY NAME and
                        refusing the graph whole: a row to nowhere, a row reading
                        a parameter that does not exist, a row from a state to
                        itself, an op a parameter's kind cannot answer, two
                        states of one name, and a finished row out of a state
                        that returns by itself. And one that is a warning
                        instead -- a state that can be entered and not left is a
                        thing a caller may mean.

    The five below are the ones where the question IS the seam, so they take a
    headless game for a real rig -- and still draw no frame.

      graph-finished    An EXIT state leaves on the animator's finished EDGE,
                        once. The identical table with the state looping never
                        leaves at all, which is the falsification: a level read
                        rather than an edge passes the first leg and fails this.
      graph-fade        The row's fade reaches playback verbatim, read off the
                        animator's own envelope at three points of a half-second
                        row. A graph that rounded one or substituted a default is
                        red here.
      graph-guard       The escape hatch is ANDed AFTER the conditions, and is
                        not called at all for a row whose conditions already
                        failed -- so a guard with a side effect is not a hidden
                        per-frame call. That third leg is the one a naive
                        implementation fails.
      graph-return      Why RETURN is a kind and not a spelling of EXIT. A
                        returning state resumes the outgoing source at the clock
                        it LEFT; an exiting one is left by a row, which re-issues
                        the space from zero. On a locomotion space that
                        difference is the walk phase, and every foot lock rides
                        it.
      graph-identity    A one-state graph is BIT-IDENTICAL to calling the
                        animator directly: every bone matrix over 120 ticks, max
                        diff 0 exactly, not to a tolerance. A machine that
                        decides what plays may add no arithmetic to what plays.

    And four that drive the app, headless, through a scripted pad -- the arm
    `docs/verification.md` booked six specs ago, when what the medium machine
    picked was read off a trace by eye and photographed.

      graph-parity-ground   Walk, lunge, spin: the state sequence the hand-rolled
                            machine produced, spelled out rather than recorded.
      graph-parity-air      The same for the full airborne path, which needs a rig
                            carrying a fall loop and a landing -- the committed
                            default has neither, so this runs on t_pose.fbx and the
                            shared clip set. It reads the jump-to-fall hand-off
                            end to end, which nothing else does.
      graph-parity-water    Walking off the plate into the water enters the stroke
                            ONCE and stays. Before the band it flipped every ~50
                            steps for as long as anybody floated.
      graph-no-swim         A rig with no stroke never enters the state it cannot
                            play, and keeps moving -- where the machine this
                            replaces labelled it as swimming, kept its walk cycle
                            playing, pinned the rate and decayed to a standstill on
                            a loop whose only fixed point was zero.
    """
    failed = []

    def note(name, ok):
        if not ok:
            failed.append(name)

    def read(case, need):
        """One probe case and the keys it must carry, or None. The verdict line is
        each arm's own, with its name as a LITERAL -- `gate-arm-docs` reads the
        source for those, so a helper that printed them would document a group
        nothing could verify."""
        d = _graph_probe(case)
        if not d or any(k not in d for k in need):
            return None
        return d

    def one(d, label, key):
        return d[(label, key)][0]

    def close(a, b, tol=1e-4):
        return abs(a - b) < tol

    # ---- graph-order
    d = read("order", [("first", "state"), ("swapped", "state")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        a, b = one(d, "first", "state"), one(d, "swapped", "state")
        ok = a == 1 and b == 2
        detail = (f"the earlier row wins ({a:.0f}, want 1) and swapping the two swaps it "
                  f"({b:.0f}, want 2)")
    print(f"  graph-order       {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-order", ok)

    # ---- graph-any
    d = read("any", [("leg0", "state"), ("leg1", "state"), ("leg2", "state")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        a, b, c = one(d, "leg0", "state"), one(d, "leg1", "state"), one(d, "leg2", "state")
        # Legs 0 and 2 start in b, where a specific row sits above the any row and
        # must win; leg 1 starts in c, which only the any row can leave. No leg
        # starts in the any row's destination, or it would read the same whether
        # the row fired or was skipped -- which is what the first draft did.
        ok = a == 2 and b == 0 and c == 2
        detail = (f"a specific row above an any row beats it ({a:.0f}, want 2) and the any row "
                  f"fires where no specific one applies ({b:.0f}, want 0)")
    print(f"  graph-any         {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-any", ok)

    # ---- graph-and
    d = read("and", [("speedonly", "state"), ("groundonly", "state"), ("both", "state")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        s1, g1 = one(d, "speedonly", "state"), one(d, "groundonly", "state")
        both = one(d, "both", "state")
        ok = s1 == 0 and g1 == 0 and both == 1
        detail = (f"either condition alone fires nothing ({s1:.0f}, {g1:.0f}, want 0 and 0) and "
                  f"both together fire ({both:.0f}, want 1)")
    print(f"  graph-and         {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-and", ok)

    # ---- graph-trigger
    d = read("trigger", [("fired", "state"), ("again", "state"), ("stale", "state"),
                         ("bool", "state")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        f1, again = one(d, "fired", "state"), one(d, "again", "state")
        stale, boolean = one(d, "stale", "state"), one(d, "bool", "state")
        ok = f1 == 1 and again == 1 and stale == 0 and boolean == 2
        detail = (f"fires once ({f1:.0f}, want 1) and not twice ({again:.0f}, want 1); a trigger "
                  f"nothing consumed is gone ({stale:.0f}, want 0) where a bool persists "
                  f"({boolean:.0f}, want 2)")
    print(f"  graph-trigger     {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-trigger", ok)

    # ---- graph-elapsed
    d = read("elapsed", [("entry", "secs"), ("cross", "ticks"), ("reset", "secs")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        entry, ticks = one(d, "entry", "secs"), one(d, "cross", "ticks")
        reset = one(d, "reset", "secs")
        ok = entry == 0.0 and ticks == 30 and reset == 0.0
        detail = (f"zero on the entry tick ({entry:.3f}), thirty ticks of 1/60 to cross half a "
                  f"second ({ticks:.0f}, want 30), and zero again in the next state ({reset:.3f})")
    print(f"  graph-elapsed     {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-elapsed", ok)

    # ---- graph-hysteresis
    d = read("hysteresis", [("banded", "flips"), ("hard", "flips")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        banded, hard = one(d, "banded", "flips"), one(d, "hard", "flips")
        ok = banded == 0 and hard > 30
        detail = (f"two bands settle in one state ({banded:.0f} flips, want 0) where one "
                  f"threshold flips on nearly every tick ({hard:.0f} of 40)")
    print(f"  graph-hysteresis  {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-hysteresis", ok)

    # ---- graph-reenter
    d = read("reenter", [("loop", "entries"), ("loop", "secs")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        entries, secs = one(d, "loop", "entries"), one(d, "loop", "secs")
        ok = entries == 1 and secs > 0.4
        detail = (f"an any row that stays true enters once ({entries:.0f}, want 1) and the "
                  f"state's clock keeps running ({secs:.3f} s, want > 0.4)")
    print(f"  graph-reenter     {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-reenter", ok)

    # ---- graph-disabled
    d = read("disabled", [("held", "state"), ("held", "secs"), ("resumed", "state")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        held, secs = one(d, "held", "state"), one(d, "held", "secs")
        resumed = one(d, "resumed", "state")
        ok = held == 0 and secs == 0.0 and resumed == 1
        detail = (f"sixty ticks stood down move nothing ({held:.0f}, {secs:.3f} s) and twenty "
                  f"after it do ({resumed:.0f}, want 1)")
    print(f"  graph-disabled    {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-disabled", ok)

    # ---- graph-instances
    d = read("instances", [("x", "state"), ("y", "state"), ("x", "secs"), ("y", "secs")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        xs, ys = one(d, "x", "state"), one(d, "y", "state")
        xt, yt = one(d, "x", "secs"), one(d, "y", "secs")
        ok = xs == 1 and ys == 0 and xt == 0.0 and yt > 0.03
        detail = (f"one table, two graphs: states {xs:.0f} and {ys:.0f} (want 1 and 0), clocks "
                  f"{xt:.4f} and {yt:.4f} s")
    print(f"  graph-instances   {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-instances", ok)

    # ---- graph-unreachable
    d = read("unreachable", [("pruned", "state"), ("live", "state"), ("noclip", "registered")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        pruned, live = one(d, "pruned", "state"), one(d, "live", "state")
        noclip = one(d, "noclip", "registered")
        ok = pruned == 2 and live == 1 and noclip == 0
        detail = (f"a row into a state this rig cannot play is pruned and the one below it fires "
                  f"({pruned:.0f}, want 2) where the same table with nothing missing takes the "
                  f"first ({live:.0f}, want 1); a source whose clip is absent does not register "
                  f"({noclip:.0f}, want 0)")
    print(f"  graph-unreachable {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-unreachable", ok)

    # ---- graph-authoring
    refusals = ["unknownstate", "unknownparam", "selfrow", "wrongkind", "duplicate",
                "returnfinish", "returnchain"]
    d = read("authoring", [(c, "bound") for c in refusals] + [("trap", "bound")])
    # The trap case must WARN, and the warning is the deliverable -- asserting only
    # that bind succeeded would stay green with the whole no-way-out analysis
    # deleted, which is what the first draft did.
    text = _graph_probe_text("authoring") or ""
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        refused = [c for c in refusals if one(d, c, "bound") == 0]
        trap = one(d, "trap", "bound")
        warned = "has no way out" in text
        ok = len(refused) == len(refusals) and trap == 1 and warned
        detail = (f"{len(refused)} of {len(refusals)} bad tables refused whole "
                  f"({', '.join(c for c in refusals if c not in refused) or 'none missed'}), and "
                  f"a state with no way out is named rather than refused "
                  f"(warned {int(warned)}, bound {trap:.0f})")
    print(f"  graph-authoring   {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-authoring", ok)

    # ---- graph-finished
    d = read("finished", [("exit", "ticks"), ("loop", "ticks")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        ex, lo = one(d, "exit", "ticks"), one(d, "loop", "ticks")
        # 240 is the probe's cap: a looping state never raising the edge is what
        # reaching it means.
        ok = 30 < ex < 200 and lo == 240
        detail = (f"a non-looping state leaves on its own end ({ex:.0f} ticks) and the same "
                  f"table looping never leaves ({lo:.0f}, the cap)")
    print(f"  graph-finished    {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-finished", ok)

    # ---- graph-fade
    d = read("fade", [("t0", "weight"), ("thalf", "weight"), ("tfull", "weight"),
                      ("tfull", "fading")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        t0, th = one(d, "t0", "weight"), one(d, "thalf", "weight")
        tf, fading = one(d, "tfull", "weight"), one(d, "tfull", "fading")
        ok = close(t0, 1.0 / 30.0) and close(th, 0.5) and tf == 1.0 and fading == 0
        detail = (f"a half-second row fades on its own envelope ({t0:.3f}/{th:.3f}/{tf:.3f}, want "
                  f"1/30, 0.5 and 1) and has settled by its end ({fading:.0f}, want 0)")
    print(f"  graph-fade        {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-fade", ok)

    # ---- graph-guard
    d = read("guard", [("blocked", "moved"), ("blocked", "calls"), ("allowed", "moved"),
                       ("allowed", "calls"), ("unarmed", "moved"), ("unarmed", "calls")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        bm, bc = one(d, "blocked", "moved"), one(d, "blocked", "calls")
        am, ac = one(d, "allowed", "moved"), one(d, "allowed", "calls")
        um, uc = one(d, "unarmed", "moved"), one(d, "unarmed", "calls")
        ok = bm == 0 and bc == 1 and am == 1 and ac == 1 and um == 0 and uc == 0
        detail = (f"a false guard blocks a row whose conditions hold ({bm:.0f}, want 0) and a "
                  f"true one does not ({am:.0f}, want 1); a row whose conditions already failed "
                  f"never reaches it ({uc:.0f} calls, want 0)")
    print(f"  graph-guard       {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-guard", ok)

    # ---- graph-return
    d = read("return", [("returned", "before"), ("returned", "after"), ("exited", "before"),
                        ("exited", "after")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        rb, ra = one(d, "returned", "before"), one(d, "returned", "after")
        eb, ea = one(d, "exited", "before"), one(d, "exited", "after")
        ok = rb == ra and ea < eb / 10.0 and rb > 0
        detail = (f"a returning state resumes the clock it left ({rb:.2f} -> {ra:.2f}, the same "
                  f"tick) where an exiting one restarts it ({eb:.2f} -> {ea:.2f})")
    print(f"  graph-return      {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-return", ok)

    # ---- graph-identity
    d = read("identity", [("pose", "maxdiff"), ("clock", "diff")])
    if not d:
        ok, detail = False, "the probe failed or measured nothing"
    else:
        pose, clock = one(d, "pose", "maxdiff"), one(d, "clock", "diff")
        ok = pose == 0.0 and clock == 0.0
        detail = (f"a one-state graph is the animator called directly, to the bit: pose {pose:.9g} "
                  f"and clock {clock:.9g} over 120 ticks (want 0 exactly)")
    print(f"  graph-identity    {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-identity", ok)

    def states_of(seq):
        return [s for _step, s in seq] if seq else []

    # ---- graph-parity-ground
    seq = _graph_trace(workdir, "ground", _GRAPH_GROUND, 331)
    want = ["ground", "lunge", "ground", "spin", "ground"]
    got = states_of(seq)
    ok = got == want
    detail = (f"{' -> '.join(got) if got else 'nothing traced'} (want {' -> '.join(want)})")
    print(f"  graph-parity-ground {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-parity-ground", ok)

    # ---- graph-parity-air
    seq = _graph_trace(workdir, "air", _GRAPH_AIR, 301,
                       ["--puppet", "assets/models/t_pose.fbx"])
    want = ["ground", "air", "land", "ground", "rise", "air", "land", "ground"]
    got = states_of(seq)
    ok = got == want
    detail = (f"{' -> '.join(got) if got else 'nothing traced'} (want {' -> '.join(want)})")
    print(f"  graph-parity-air  {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-parity-air", ok)

    # ---- graph-parity-water
    seq = _graph_trace(workdir, "water", _GRAPH_WATER, 901)
    got = states_of(seq)
    entries = got.count("water")
    ok = got == ["ground", "water"]
    detail = (f"{' -> '.join(got) if got else 'nothing traced'}: the stroke is entered "
              f"{entries} time(s) and not left (want exactly one, and no flip back)")
    print(f"  graph-parity-water {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-parity-water", ok)

    # ---- graph-no-swim
    seq = _graph_trace(workdir, "noswim", _GRAPH_WATER, 901, ["--no-swim"])
    got = states_of(seq)
    ok = got == ["ground"]
    detail = (f"{' -> '.join(got) if got else 'nothing traced'} (want ground alone: a state "
              f"whose source this rig cannot play is pruned, not entered)")
    print(f"  graph-no-swim     {'PASS' if ok else 'FAIL'}  {detail}")
    note("graph-no-swim", ok)

    return failed
