"""The profiler group: gpu timing (per-pass queries, spec 11.27 / E4)."""

import math
import os
import time

from gatelib.harness import _run, asset
from gatelib.images import compare
from gatelib.profiling import (
    GPU_FIXTURE, NON_PASS_ROWS, _gpu_cmd, _profiled_run, _timing_delta, _within,
)


# Passes asserted BY NAME rather than by a count. A threshold drifts with the
# scope list and cannot say which pass vanished; the defect this catches -- the
# whole post chain writing through a NULL profiler -- left exactly three rows
# standing, which any count low enough to be safe would have cleared.
GPU_REQUIRED_ROWS = frozenset(
    {"shadow cascades", "opaque", "gtao sweep", "ssr", "bloom pyramid", "tonemap + finishing",
     # Unconditional in engine_build_draw_list, and required here because being
     # ABSENT is the state 11.89 found it in -- a full per-frame scene flatten
     # billed to nothing. Its GPU time is 0.000 by construction (it issues no
     # GL), so what this asserts is that the scope still REGISTERS: put a
     # profiler_suspend upstream of the call and the row stops printing.
     "draw list build"})
GPU_MIN_POSITIVE = 4          # rows that must be strictly > 0, not merely present
GPU_SUM_MIN = 0.30            # TIMED must be at least this fraction of FRAME
GPU_SCALE_DROP = 0.20         # render-res passes must shed this much at half scale
# The whole frame, which carries CPU time and everything after the TAAU seam --
# tonemap, finishing, GUI, present -- that half render scale cannot touch. Same
# value as GPU_SCALE_DROP today and deliberately not the same constant: the two
# have different achievable ranges, so tightening one to sharpen a claim about
# the opaque pass must not silently demand it of a frame that cannot deliver it.
GPU_FRAME_DROP = 0.20
# The GPU column's own bar for TIMED against FRAME, which is a MULTIPLE where
# the CPU column's is a rounding slack. That asymmetry is the profiler's, not a
# convenience: FRAME is the frame's CPU-side bracket, and profiler.h states that
# only the CPU column is inside it -- the driver runs behind, so a pass's GPU
# time can legitimately land outside the bracket that submitted it. Asserting
# the CPU identity on the GPU column would go red on correct code the first time
# a fixture is GPU-bound enough. The bar still catches what it is for: spec
# 11.97's defect read 3.4x to 5.3x here.
GPU_TIMED_CEILING = 2.0
# How far PERIOD may sit from a clock the profiler does not own. Wide on
# purpose, and the width is the HARNESS's noise rather than the profiler's:
# differencing two process times cancels startup's MEAN but not its variance, so
# whatever that varies by lands on this arm divided by the frame difference.
# Measured across two runs at a 45-frame difference: PERIOD held to 0.1% (11.955
# then 11.945 ms) while the outside measurement moved 26% (15.259 then 12.147).
# The frame counts below are what buy the margin back; this bound is then set
# against what the arm is FOR, which is a row that is saturated, clamped or
# divided by the wrong count. Spec 11.89's clamp pinned it at exactly 100.000 ms
# -- 8x, not 40% -- and every arm in this group stayed green through it.
GPU_WALL_TOLERANCE = 0.40
# The two legs of that comparison, and the gap between them is the whole design:
# startup variance is divided by the DIFFERENCE, so 120 extra frames cost about
# a second of render and shrink the harness noise above by 2.7x. The long leg
# must also outlast a second latch window, since profiler_report publishes the
# last one latched and at 45 frames that is still the first, which is warm-up.
#
# Stated here rather than leaning on _gpu_cmd's default, because the difference
# is a DIVISOR: taking one end from a default and the other from a constant is
# how the arm silently starts dividing by the wrong number of frames.
GPU_WALL_SHORT_FRAMES = "45"
GPU_WALL_FRAMES = "165"
# Short enough that the run NEVER latches, which is what makes gpu-gated
# deterministic rather than a race. profiler_report publishes the last latched
# window, and the GI volume's one-shot sweep is in the FIRST -- so at any length
# that latches more than once, whether the arm sees the gated scope at all
# depends on how fast the machine is. Measured at 45 frames: the published table
# had 17 rows and none of them was `gi capture`, so the arm was green without
# testing anything. Below one window the fallback publishes the whole run.
GPU_GATED_FRAMES = "5"


def _gpu_table(workdir, tag, extra, screenshot=None):
    """The GPU block alone, in the (rows, unparsed) shape this gate's arms use."""
    tables = _profiled_run(workdir, tag, extra, screenshot)
    if tables is None:
        return None, None
    return tables["gpu"], tables["unparsed"]["gpu"]


def _scale_drop_detail(timing, label, bar):
    """(ok, detail) for "this row shed `bar` at half scale". The CALLER prints.

    Split this way for the reason _submit_sum_detail is: gate-arm-docs reads each
    group's own source for the arm names it runs, so a verdict line printed from
    in here is invisible to it.

    `separated` is unread, and not for the reason gpu-scale gives for its own
    copy. That flag is v > max(base) -- the variant being SLOWER -- so it is
    False whenever the asserted drop actually happens. It is directional, not
    merely redundant, and an arm that scores on it can never pass.
    """
    before, after, signed, noise, _ = timing
    drop = -signed  # half scale should make it SMALLER
    return (drop >= bar and noise < bar,
            f"{label} {before:.3f} -> {after:.3f} ms at half scale, {drop * 100.0:.0f}% off "
            f"(want >= {bar * 100.0:.0f}%), against a {noise * 100.0:.0f}% run-to-run floor")


def run_profiler_gate(workdir):
    """Per-pass GPU and CPU timing, and whether the numbers mean what they say.

      gpu-parse       every line between the banners parses as a row, so a shifted
                      report format fails by name instead of as missing passes.
      gpu-rows        the required passes are present, BY NAME -- a count cannot say
                      which one vanished.
      gpu-nonzero     enough passes read above zero, and all are finite. A driver that
                      accepts timer queries and measures nothing looks identical to a
                      working one until here.
      gpu-sum         TIMED covers at least a third of FRAME. Loose, and the only thing
                      standing between the table and a third of the frame going unnamed.
      gpu-off         profiled and unprofiled renders are 0 px apart. The instrument
                      must not move what it measures, and no golden runs with the flag.
      gpu-dof         a pass that is OFF has no row, and turning it on produces one that
                      costs something. Without it every arm above passes against a
                      profiler printing plausible constants.
      gpu-scale       the opaque row sheds time at half render scale -- attached to real
                      work rather than to the frame counter.
      gpu-frame       the frame row moves with load, so it is a measurement and not a
                      constant. Every ratio a perf spec quotes has it as denominator.
      gpu-period      FRAME <= PERIOD. FRAME is the frame's own bracket and PERIOD the
                      wall period that contains it, so this is an ordering the profiler
                      cannot violate without its clock being wrong.
      gpu-gated       TIMED <= FRAME with a GATED scope in the published window. See
                      below -- this is the arm the group did not have.
      gpu-wall        PERIOD agrees with a clock the profiler does not own, differenced
                      across two run lengths so startup cancels.

    THE LAST THREE COVER WHAT THE ARMS ABOVE THEM STRUCTURALLY CANNOT SEE -- an
    ordering, a gated scope, and an external clock. Each arm's own comment carries its
    provenance; only gpu-gated is about spec 11.97's defect.

    That defect: each published number divided by its own count -- GPU results the driver
    returned, frames the scope was entered, frames in the window -- so a scope gated on
    something occasional published a per-OCCURRENCE cost in a column of per-FRAME means,
    and TIMED summed it in as though it ran throughout. `dir_shadow_fixture --gi-volume`
    printed a CPU TIMED of 736.855 ms against a 35.049 ms FRAME: 21x a ceiling that
    profiler.h and gates.py both assert. Nothing was wrong with the clock.

    gpu-sum could not see it -- it is a LOWER bound, and the defect makes TIMED too big.
    cpu-bound (run_submission_gate) asserts the ceiling and was green only because its
    fixture has no gated scope. So the fixture is the arm: --gi-volume, whose first sweep
    runs every probe in ONE frame and is the largest gated scope in the tree.
    """
    if not os.path.exists(asset(GPU_FIXTURE)):
        print(f"  gpu          SKIP  (missing {GPU_FIXTURE})")
        return []

    on_ppm = os.path.join(workdir, "gpu_on.ppm")
    # The whole report rather than the GPU block alone: gpu-period reads both
    # timing columns, and re-rendering to see the second one would pay a full
    # render for a table this run already printed.
    base_tables = _profiled_run(workdir, "base", [], screenshot=on_ppm)
    if base_tables is None:
        return ["gpu"]
    base, unparsed = base_tables["gpu"], base_tables["unparsed"]["gpu"]
    failures = []

    ok = unparsed == 0
    print(f"  gpu-parse    {'PASS' if ok else 'FAIL'}  {unparsed} unreadable rows "
          f"(a shifted report format must fail here, not as missing passes below)")
    if not ok:
        failures.append("gpu-parse")

    named = {k: v for k, v in base.items() if k not in NON_PASS_ROWS}

    # By name, not by count: a count cannot say WHICH pass vanished, and the
    # real defect left three rows standing.
    missing = sorted(GPU_REQUIRED_ROWS - set(named))
    ok = not missing
    print(f"  gpu-rows     {'PASS' if ok else 'FAIL'}  {len(named)} passes; "
          f"{'none missing' if ok else 'MISSING ' + ', '.join(missing)}")
    if not ok:
        failures.append("gpu-rows")

    # Strictly positive and finite. "total > 0" passed on a single nonzero row,
    # and a driver that reports zeros is the thing this arm exists for.
    positive = [v for v in named.values() if v > 0.0]
    finite = all(math.isfinite(v) for v in named.values())
    ok = len(positive) >= GPU_MIN_POSITIVE and finite
    print(f"  gpu-nonzero  {'PASS' if ok else 'FAIL'}  {len(positive)} of {len(named)} passes "
          f"above zero, all finite: {finite} (want >= {GPU_MIN_POSITIVE} positive; a driver "
          f"that accepts queries and measures nothing looks identical until here)")
    if not ok:
        failures.append("gpu-nonzero")

    # TIMED against the wall-clock ceiling. Their difference also carries CPU
    # time and GPU idle, so this is a loose bound on how much GPU work no scope
    # covers -- loose is still the only thing standing between the table and a
    # third of the frame going unnamed.
    timed, frame = base.get("TIMED"), base.get("FRAME (wall)")
    if timed is None or frame is None or frame <= 0.0:
        ok = False
        print(f"  gpu-sum      FAIL  report carries no TIMED/FRAME pair (timed={timed}, "
              f"frame={frame})")
    else:
        ok = timed >= GPU_SUM_MIN * frame
        print(f"  gpu-sum      {'PASS' if ok else 'FAIL'}  TIMED {timed:.3f} ms is "
              f"{timed / frame * 100.0:.0f}% of the {frame:.3f} ms frame "
              f"(want >= {GPU_SUM_MIN * 100.0:.0f}%)")
    if not ok:
        failures.append("gpu-sum")

    # The claim the goldens cannot make: they run WITHOUT the flag, so they say
    # nothing about whether wrapping every pass in a query moves a pixel.
    off_ppm = os.path.join(workdir, "gpu_off.ppm")
    r = _run(_gpu_cmd(off_ppm, [], False), capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(off_ppm):
        print("  gpu-off      ERROR while rendering without the flag")
        failures.append("gpu-off")
    else:
        ae, _ = compare(on_ppm, off_ppm)
        ok = ae == 0
        print(f"  gpu-off      {'PASS' if ok else 'FAIL'}  {ae} px between profiled and "
              f"unprofiled (want exactly 0: the instrument must not move what it measures)")
        if not ok:
            failures.append("gpu-off")

    # The inverse arm. A pass that is off must have NO row, and turning it on
    # must produce one that costs something -- without this, every arm above
    # passes against a profiler printing plausible constants.
    dof_on, _ = _gpu_table(workdir, "dof", ["--dof", "--dof-focus", "6"])
    if dof_on is None:
        failures.append("gpu-dof")
    else:
        absent = "dof" not in named
        present = dof_on.get("dof", 0.0) > 0.0
        ok = absent and present
        print(f"  gpu-dof      {'PASS' if ok else 'FAIL'}  dof absent without the flag: "
              f"{absent}; {dof_on.get('dof', 0.0):.3f} ms with it (want > 0)")
        if not ok:
            failures.append("gpu-dof")

    # Attached to real work, not to the frame counter. Both runs carry --taa
    # --headless-jitter so the only difference is the pixel count: TAA jitters
    # the projection the opaque pass draws with, and a control that changes
    # three things cannot attribute the delta to one of them.
    taa = ["--taa", "--headless-jitter"]
    full, _ = _gpu_table(workdir, "full", taa + ["--render-scale", "1.0"])
    half, _ = _gpu_table(workdir, "half", taa + ["--render-scale", "0.5"])
    # A second full-scale run, to say what run-to-run variance is before
    # claiming a drop is real. Timings are noisier than pixels, and this tree
    # requires the floor be measured rather than assumed.
    full2, _ = _gpu_table(workdir, "full2", taa + ["--render-scale", "1.0"])
    if full is None or half is None or full2 is None:
        # Both arms read this trio. Name them both or the second vanishes from
        # the report without a line, and the failure list under-reports by one.
        print("  gpu-scale    ERROR while rendering the render-scale trio")
        print("  gpu-frame    ERROR while rendering the render-scale trio")
        failures += ["gpu-scale", "gpu-frame"]
    else:
        # Presence before ratio, in _timing_delta: a missing row read as 0 ms
        # scores a vanished pass as a 100% saving.
        timing = _timing_delta([full, full2], [half])
        if timing is None:
            print(f"  gpu-scale    FAIL  no usable opaque row to compare (full="
                  f"{full.get('opaque')}, half={half.get('opaque')}, "
                  f"repeat={full2.get('opaque')}); the pass was not timed, not cheap")
            failures.append("gpu-scale")
        else:
            ok, detail = _scale_drop_detail(timing, "opaque", GPU_SCALE_DROP)
            print(f"  gpu-scale    {'PASS' if ok else 'FAIL'}  {detail}")
            if not ok:
                failures.append("gpu-scale")

        # --- gpu-frame: the frame row is a measurement, not a constant --------
        # Every ratio a performance spec quotes has FRAME as its denominator, and
        # a denominator that does not move is worse than a missing one: it reads
        # as a real number. Spec 11.89 found it pinned to a constant.
        #
        # Rides gpu-scale's three runs rather than rendering its own.
        #
        # WHAT THIS ARM DOES NOT COVER, because the comment it replaced claimed
        # otherwise: it is not a regression test for that clamp. The clamp fired
        # above 100 ms/frame and this fixture runs at ~14, so the arm is green on
        # the unfixed code too. Nothing in the suite is slow enough to trip it,
        # which is why the bug survived. Catching it needs FRAME checked against
        # a clock the profiler does not own; booked, not built.
        frame_timing = _timing_delta([full, full2], [half], row="FRAME (wall)")
        if frame_timing is None:
            # FAIL, not SKIP. _timing_delta returns None for a row that is
            # missing, zero or negative -- every one of which is a MORE broken
            # frame row than the constant this arm watches for, and a SKIP
            # appends nothing so the suite would exit 0 over it.
            print(f"  gpu-frame    FAIL  no usable FRAME (wall) row (full="
                  f"{full.get('FRAME (wall)')}, half={half.get('FRAME (wall)')}, "
                  f"repeat={full2.get('FRAME (wall)')}); missing, zero or negative")
            failures.append("gpu-frame")
        else:
            ok, detail = _scale_drop_detail(frame_timing, "FRAME", GPU_FRAME_DROP)
            print(f"  gpu-frame    {'PASS' if ok else 'FAIL'}  {detail}")
            if not ok:
                failures.append("gpu-frame")

    # --- gpu-period: the bracket sits inside the period ---------------------
    # FRAME is the frame's own bracket; PERIOD is the wall time from one frame's
    # start to the next, which contains it plus the swap, the vsync wait and the
    # poll. FRAME <= PERIOD is therefore an ordering the profiler cannot violate
    # while its clock is right, and it is what connects the TIGHT ceiling the CPU
    # column is read against to a number still comparable with an fps counter.
    #
    # Read ONCE, not once per column. Both rows are whole-frame quantities and
    # print_timing_table emits them unswitched, so the two columns carry the same
    # two floats by construction -- a per-column loop here would evaluate the
    # identical comparison twice and could never report one column alone. What is
    # worth asserting instead is exactly that construction: if the columns ever
    # disagree, the printer has grown a `cpu` branch it should not have.
    frame, period = base.get("FRAME (wall)"), base.get("PERIOD")
    same = base_tables["cpu"].get("PERIOD") == period
    if frame is None or period is None:
        print(f"  gpu-period   FAIL  no FRAME/PERIOD pair (frame={frame}, period="
              f"{period}); a missing row is a more broken report than a wrong one")
        failures.append("gpu-period")
    else:
        ok = _within(frame, period) and same
        print(f"  gpu-period   {'PASS' if ok else 'FAIL'}  FRAME {frame:.3f} ms inside "
              f"PERIOD {period:.3f} ms"
              + ("" if ok else
                 "; INVERTED" if not _within(frame, period) else
                 f"; the two columns disagree on PERIOD "
                 f"({base_tables['cpu'].get('PERIOD')} vs {period})"))
        if not ok:
            failures.append("gpu-period")

    # --- gpu-gated: the ceiling holds when a scope runs on SOME frames ------
    # The arm this group did not have. Everything above runs on a fixture where
    # every scope runs every frame, which is the one case the three denominators
    # agreed in even under the defect -- so the whole group was blind to a scope
    # that runs sometimes.
    #
    # --gi-volume is the strongest available instance rather than a convenient
    # one: the volume's first sweep runs every probe in a SINGLE frame, so it is
    # a several-hundred-millisecond scope entered once in a run, which is exactly
    # the shape that published a per-occurrence cost beside per-frame means.
    #
    # GPU_GATED_FRAMES is load-bearing -- see its comment. The row has to be in
    # the published window for this arm to be testing anything, and it is
    # asserted below rather than assumed, because the failure is silent: at 45
    # frames the table came back with 17 rows, none of them the sweep, and the
    # arm passed.
    gi = _profiled_run(workdir, "gi", ["--gi-volume"], frames=GPU_GATED_FRAMES)
    if gi is None:
        failures.append("gpu-gated")
    else:
        # Both columns, at DIFFERENT bars, because the profiler makes different
        # promises about them: the CPU one is an identity and the GPU one is not
        # (GPU_TIMED_CEILING). Reading either alone would have missed half the
        # defect -- the GPU column was over by 3.4x where the CPU column was over
        # by 21x.
        bad = {}
        for col, factor in (("gpu", GPU_TIMED_CEILING), ("cpu", 1.0)):
            timed, frame = gi[col].get("TIMED"), gi[col].get("FRAME (wall)")
            if "gi capture" not in gi[col]:
                bad[col] = "no `gi capture` row: the gated scope is not in this window"
            elif timed is None or frame is None or frame <= 0.0:
                bad[col] = "no TIMED/FRAME pair"
            elif not _within(timed, frame, factor):
                bad[col] = (f"{timed:.3f} vs {frame:.3f} ms ({timed / frame:.1f}x, "
                            f"want <= {factor:.1f}x)")
        ok = not bad
        print(f"  gpu-gated    {'PASS' if ok else 'FAIL'}  TIMED within FRAME with a "
              f"gated scope in the window: "
              + ("both columns" if ok
                 else "; ".join(f"{c} {d}" for c, d in bad.items())))
        if not ok:
            failures.append("gpu-gated")

    # --- gpu-wall: checked against a clock the profiler does not own --------
    # Booked by spec 11.89 and never built, and it is the arm that would have
    # caught the clamp that pinned this row at exactly 100.000 ms: every arm in
    # this group compares the profiler against ITSELF, so a row that is
    # internally consistent and externally wrong passes all of them.
    #
    # Differenced across two run lengths so process startup -- context creation,
    # asset load, shader compile -- cancels instead of being estimated. The long
    # leg is what makes the comparison fair: profiler_report publishes the LAST
    # LATCHED window, so a 45-frame run publishes its first, which is warm-up.
    t0 = time.monotonic()
    short = _profiled_run(workdir, "wall_short", [], frames=GPU_WALL_SHORT_FRAMES)
    t_short = time.monotonic() - t0
    t0 = time.monotonic()
    long_run = _profiled_run(workdir, "wall_long", [], frames=GPU_WALL_FRAMES)
    t_long = time.monotonic() - t0
    extra_frames = int(GPU_WALL_FRAMES) - int(GPU_WALL_SHORT_FRAMES)
    if short is None or long_run is None:
        print("  gpu-wall     ERROR while rendering the two run lengths")
        failures.append("gpu-wall")
    else:
        harness_ms = (t_long - t_short) / extra_frames * 1000.0
        period = long_run["gpu"].get("PERIOD")
        if period is None or period <= 0.0 or harness_ms <= 0.0:
            print(f"  gpu-wall     FAIL  no usable pair (PERIOD={period}, harness="
                  f"{harness_ms:.3f} ms/frame over {extra_frames} extra frames)")
            failures.append("gpu-wall")
        else:
            rel = abs(period - harness_ms) / harness_ms
            ok = rel <= GPU_WALL_TOLERANCE
            print(f"  gpu-wall     {'PASS' if ok else 'FAIL'}  PERIOD {period:.3f} ms "
                  f"against {harness_ms:.3f} ms/frame measured outside the process "
                  f"({rel * 100.0:.0f}% apart, want <= {GPU_WALL_TOLERANCE * 100.0:.0f}%)")
            if not ok:
                failures.append("gpu-wall")

    return failures
