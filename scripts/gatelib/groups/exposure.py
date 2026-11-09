"""The exposure group: histogram auto-exposure (the meter, its mask, its percentiles)."""

import math
import os

from gatelib.harness import _probe_render, asset
from gatelib.scenes import _scale_emitters, cscn_copy


# ---------------------------------------------------------------------------
# Auto-exposure metering (spec 11.52)
# ---------------------------------------------------------------------------

EXPOSURE_FIXTURE = "postfx_convergence_fixture.cscn"
EXPOSURE_SCALE_FIXTURE = "cornell_point.cscn"

# Frames. The meter reads a PRE-EXPOSED frame, so its own reading sits inside a
# feedback loop until the adaptation settles; anything measured before that is
# measuring the approach, not the answer.
#
# On this fixture the gap first touches the deadband around frame 18 and is
# pushed back out twice as the scene keeps brightening, settling at frame 91 --
# so 150 is margin over THAT, not over the deadband's first touch. An earlier
# note here said frame 12, measured before the percentiles changed what the meter
# reads and never re-taken.
EXPOSURE_FRAMES = 150
# Both legs of the linearity arm read identically at 60, 120, 150 and 200 frames,
# so this is the converged value plus headroom rather than a guess. Longer than
# EXPOSURE_FRAMES because the x1000 leg starts ~10 stops from its answer.
EXPOSURE_LINEAR_FRAMES = 120

# Meter linearity: emitters x1000 with EXPOSURE LEFT ALONE must move the raw
# metered value by exactly log2(1000) stops, because the meter reads absolute
# radiance with pre-exposure divided back out.
#
# NOT the SCALE_GATES shape, and that is the point. Scaling emitters by K while
# dividing the camera by K double-compensates once the meter is live -- the
# camera divides by K and the meter responds to the same K -- so pre_exposure
# lands K^2 off and no metering can satisfy it. Those arms are only meaningful
# with exposure pinned, which is how they are written.
#
# The bar is what separates the shipped meter from every alternative measured:
# floor at the key -1.6084 stops, percentiles 0.10/0.90 -6.1180, 0.50/0.95
# -0.1337, and the shipped 0.70/0.95 -0.0046. 0.05 admits the last and rejects
# the next-best by a factor of 29.
EXPOSURE_SCALE = 1000.0
EXPOSURE_LINEAR_EPS = 0.05

# Two runs of one build, every field of every line. Not a tolerance: the
# readback is deliberately blocking and the blend is per-FRAME rather than per
# second, both traded for exactly this.
EXPOSURE_STABLE_FRAMES = 60

# The snap makes adapted EXACTLY equal the target, so the true converged gap is
# 0. This is only float slack on two %.6f fields the engine prints in log2 --
# not a tolerance on the convergence itself.
EXPOSURE_CONVERGE_EPS = 1e-5
# The change the meter converges FROM: the camera turned off the model at this frame, which the
# meter reads 1.45 stops darker. A static scene meters the same every frame, so the first
# measurement, taken whole, leaves nothing to converge from; the gap this arm used to find on
# frames 1-7 was the fog volume still carrying frame 0's exposure (spec 13.20), a defect.
EXPOSURE_CONVERGE_MOVE_FRAME = 10
EXPOSURE_CONVERGE_MOVE = f"{EXPOSURE_CONVERGE_MOVE_FRAME}:0,0.35,1.6,0,0.35,3.6"

# A gain of exactly 1.0 is auto-exposure declining to act. Compared with ==
# rather than a tolerance because the cap is an fminf, so the value is the
# literal 1.0f or it is not the cap.
EXPOSURE_GAIN_CAP = 1.0
# The bright half of the same arm: a scene the meter SHOULD stop down. Measured
# 0.000827 at x1000, so this rejects a build where the cap swallowed everything.
EXPOSURE_DARKEN_MAX = 0.5

# Cutting the highlight tail must LOWER the metered value. Measured 0.3445 stops
# between high 1.0 and the shipped 0.95 on the convergence fixture.
#
# NOT measured on flare_fixture, which looks like the obvious instrument -- a
# strength-60 quad on a black backdrop -- and is the wrong one. That frame is so
# overwhelmingly black that the metered value pins at the shader's 1e-8 numeric
# guard whatever the percentiles are: -26.28 over the whole population against a
# guard at -26.58. An arm reading there is comparing two clamps, and it PASSED
# that way before this was checked. Hence the floor below, which is the real
# assertion: the arm is only meaningful while the meter is on live values.
EXPOSURE_TAIL_MIN_DROP = 0.15
EXPOSURE_TAIL_FLOOR = -20.0


def _exposure_probe(scene, extra=None, frames=EXPOSURE_FRAMES):
    """Run --exposure-probe and parse its lines. Returns (rows, combined output).

    Reads the INSTRUMENT rather than the image, for the reason the probe exists:
    a wrong exposure hides inside a plausible frame, and the metered value
    appears nowhere else -- no log line, no readout, and every golden pins it.

    Every field is a float here, unlike the emissive probe's, so the cast is this
    wrapper's whole job.
    """
    rows, text = _probe_render(scene, "--exposure-probe", "exposure-probe",
                               extra=extra, frames=frames)
    return [{k: float(v) for k, v in r.items()} for r in rows], text


def _exposure_live(src, dst, scale=1.0):
    """A .cscn twin with the meter LIVE, optionally with its emitters scaled.

    Every fixture in the corpus pins exposure -- by CLI or by authoring
    post.exposure with no auto_exposure key, which cscene_apply turns into a pin.
    So a live-meter arm cannot reuse one as-is, and generating the twin keeps the
    two halves identical in everything this does not touch.
    """
    def mutate(d):
        _scale_emitters(d, scale)
        post = d.setdefault("post", {})
        post.pop("exposure", None)
        post["auto_exposure"] = True
    cscn_copy(src, dst, mutate)


def run_exposure_gate(workdir):
    """Histogram auto-exposure: the meter, its mask and its percentiles (spec 11.52).

    This group exists because NOTHING asserted anything about metering. All 24
    goldens pin exposure, every gate fixture pins it too, and the constants the
    adaptation is built on -- the blend rate, the snap deadband, the metering
    bounds -- were untested while the file's own comments record that two of them
    SHIPPED WRONG and were found by hand.

      exposure-darkens  the gain is capped at 1.0, so auto-exposure only ever
                        darkens. Three readings, not one: a scene metering ~6
                        stops under the key reads exactly 1.0, it reports having
                        MEASURED that (gain 1.0 is also what three early exits
                        return), and a bright scene still stops down. Drop any of
                        the three and the arm passes on a broken build.
      exposure-converge the snap engages -- adapted reaches raw EXACTLY, not
                        nearly. Asserted against an early frame where the gap is
                        still open, so a build that never adapted at all cannot
                        pass by having no gap to close. The gap comes from the
                        camera turning off the model at frame 10: a static scene
                        meters the same every frame since spec 13.20, and the gap
                        this found on frames 1-7 before it was a defect.
      exposure-stable   two runs of one build agree on every field of every line.
                        The blocking readback and the per-FRAME blend are both
                        traded for this and nothing checked it.
      exposure-linear   emitters x1000, exposure LEFT ALONE, raw metered moves
                        exactly log2(1000). The absolute floor this replaced fails
                        at -1.61 stops, so the arm is red on the old meter by
                        construction. Not the SCALE_GATES shape -- see the note on
                        EXPOSURE_SCALE.
      exposure-tail     cutting the highlight tail lowers the metered value, with
                        a floor asserting both readings are on live values rather
                        than pinned at the shader's numeric guard. That floor is
                        not decoration: written against flare_fixture, the obvious
                        small-bright-on-black instrument, this arm passed while
                        comparing two clamps 0.3 stops apart at 2^-26.
      exposure-mask     the mask is a pure re-weighting: a spot wide enough to
                        cover the frame reads BIT-IDENTICAL to uniform, while a
                        real spot moves. Identity is the falsifier -- it fails if
                        the weighting biases the answer rather than re-weighting
                        it.
    """
    failures = []
    src = asset(EXPOSURE_FIXTURE)
    if not os.path.exists(src):
        print(f"  exposure-darkens SKIP ({EXPOSURE_FIXTURE} not present)")
        return failures

    scale_src = asset(EXPOSURE_SCALE_FIXTURE)
    if not os.path.exists(scale_src):
        print(f"  exposure-darkens SKIP ({EXPOSURE_SCALE_FIXTURE} not present)")
        return failures

    live = os.path.join(workdir, "exposure_live.cscn")
    _exposure_live(src, live)

    # --- exposure-darkens ---------------------------------------------------
    # Forced below the key by metering the DARKEST fifth, which the floor this
    # replaced made impossible -- it clamped every texel up to the key, so a
    # sub-key mean could not exist and the cap was never reached.
    dark, _ = _exposure_probe(live, ["--meter-low", "0.05", "--meter-high", "0.20"],
                              frames=120)
    bright = os.path.join(workdir, "exposure_bright.cscn")
    _exposure_live(scale_src, bright, scale=EXPOSURE_SCALE)
    bright_rows, _ = _exposure_probe(bright, frames=EXPOSURE_LINEAR_FRAMES)
    if not dark or not bright_rows:
        print("  exposure-darkens ERROR no probe output")
        failures.append("exposure-darkens")
    else:
        d = dark[-1]
        dark_gain = d["gain"]
        bright_gain = bright_rows[-1]["gain"]
        # `valid` and the sub-key reading are not decoration. exposure_auto_gain
        # returns exactly 1.0 from THREE early exits -- !automatic, !adapted_valid
        # and adapted <= 0 -- so gain == 1.0 alone means "the cap engaged OR
        # nothing was measured". Written without these, the arm passes on a meter
        # whose histogram came back empty, which --meter-radius 0.0001 produces.
        measured = d["valid"] == 1.0 and d["raw_nits"] < d["key"]
        ok = measured and dark_gain == EXPOSURE_GAIN_CAP and bright_gain <= EXPOSURE_DARKEN_MAX
        print(f"  exposure-darkens {'PASS' if ok else 'FAIL'} sub-key scene gain={dark_gain:.6f} "
              f"want exactly {EXPOSURE_GAIN_CAP}; metered {d['raw_nits']:.3g} cd/m2 want "
              f"< key {d['key']} and valid={int(d['valid'])} want 1 (or the cap was never "
              f"reached); bright scene gain={bright_gain:.6f} want <={EXPOSURE_DARKEN_MAX} "
              "(it must still stop down)")
        if not ok:
            failures.append("exposure-darkens")

    # --- exposure-converge --------------------------------------------------
    rows, _ = _exposure_probe(live, ["--cam-at", EXPOSURE_CONVERGE_MOVE], frames=EXPOSURE_FRAMES)
    if len(rows) < EXPOSURE_CONVERGE_MOVE_FRAME + 20:
        print("  exposure-converge ERROR too few probe lines")
        failures.append("exposure-converge")
    else:
        # Against the CLAMPED target, and both sides in log2 as the engine reports
        # them. Two traps this avoids, each of which made a converged meter look
        # broken: comparing against RAW never closes on a frame the metered bounds
        # clamp, and log2()-ing the %.6f nits field measures print precision --
        # a floor that grows as the scene darkens, reaching 2.5e-4 on this
        # group's own dark configuration, 25x the threshold.
        def gap(rec):
            return abs(rec["adapted_log2"] - rec["target_log2"])
        # The seven frames after the camera moves, where the meter is still on its way there.
        after = [r for r in rows if int(r["frame"]) >= EXPOSURE_CONVERGE_MOVE_FRAME]
        early = max(gap(r) for r in after[:7])
        late = gap(rows[-1])
        ok = late <= EXPOSURE_CONVERGE_EPS and early > EXPOSURE_CONVERGE_EPS
        print(f"  exposure-converge {'PASS' if ok else 'FAIL'} adapted-vs-target gap "
              f"{late:.2e} at frame {int(rows[-1]['frame'])} want <={EXPOSURE_CONVERGE_EPS} "
              f"(the snap); early gap {early:.2e} want >{EXPOSURE_CONVERGE_EPS} "
              "(it had somewhere to converge from)")
        if not ok:
            failures.append("exposure-converge")

    # --- exposure-stable ----------------------------------------------------
    a, _ = _exposure_probe(live, frames=EXPOSURE_STABLE_FRAMES)
    b, _ = _exposure_probe(live, frames=EXPOSURE_STABLE_FRAMES)
    if not a or len(a) != len(b):
        print(f"  exposure-stable FAIL  {len(a)} lines vs {len(b)}")
        failures.append("exposure-stable")
    else:
        # NaN compares unequal to itself, and a refused measurement is a NaN the
        # engine emits deliberately -- so a bare != would report a designed path
        # as a determinism regression.
        def same(u, v):
            return u == v or (isinstance(v, float) and math.isnan(u) and math.isnan(v))
        diffs = [k for x, y in zip(a, b) for k in x if not same(x[k], y.get(k, None))]
        ok = not diffs
        print(f"  exposure-stable {'PASS' if ok else 'FAIL'} {len(a)} frames x "
              f"{len(a[0])} fields identical across two runs"
              + ("" if ok else f"; first differing field {diffs[0]}"))
        if not ok:
            failures.append("exposure-stable")

    # --- exposure-linear ----------------------------------------------------
    one = os.path.join(workdir, "exposure_lin_1x.cscn")
    _exposure_live(scale_src, one, scale=1.0)
    lo, _ = _exposure_probe(one, frames=EXPOSURE_LINEAR_FRAMES)
    if not lo or not bright_rows:
        print("  exposure-linear ERROR no probe output")
        failures.append("exposure-linear")
    else:
        got = bright_rows[-1]["raw_log2"] - lo[-1]["raw_log2"]
        want = math.log2(EXPOSURE_SCALE)
        ok = abs(got - want) <= EXPOSURE_LINEAR_EPS
        print(f"  exposure-linear {'PASS' if ok else 'FAIL'} emitters x{EXPOSURE_SCALE:.0f} moved "
              f"the metered value {got:.4f} stops, want {want:.4f} +/-{EXPOSURE_LINEAR_EPS} "
              f"(error {got - want:+.4f}; the absolute floor this replaced reads -1.61)")
        if not ok:
            failures.append("exposure-linear")

    # --- exposure-tail ------------------------------------------------------
    keep, _ = _exposure_probe(live, ["--meter-low", "0.70", "--meter-high", "1.0"],
                              frames=EXPOSURE_FRAMES)
    cut, _ = _exposure_probe(live, ["--meter-low", "0.70", "--meter-high", "0.95"],
                             frames=EXPOSURE_FRAMES)
    if not keep or not cut:
        print("  exposure-tail  ERROR no probe output")
        failures.append("exposure-tail")
    else:
        hi, lo_v = keep[-1]["raw_log2"], cut[-1]["raw_log2"]
        drop = hi - lo_v
        live_values = min(hi, lo_v) > EXPOSURE_TAIL_FLOOR
        ok = drop >= EXPOSURE_TAIL_MIN_DROP and live_values
        print(f"  exposure-tail  {'PASS' if ok else 'FAIL'} cutting the top 5% lowered the "
              f"metered value {drop:.4f} stops want >={EXPOSURE_TAIL_MIN_DROP} "
              f"(keep {hi:.4f} -> cut {lo_v:.4f}); both above {EXPOSURE_TAIL_FLOOR} "
              f"(on live values, not the numeric guard): {live_values}")
        if not ok:
            failures.append("exposure-tail")

    # --- exposure-mask ------------------------------------------------------
    # `rows` is the default configuration at the same frame count -- the byte
    # identical command this used to issue a second time. Reused rather than
    # re-rendered, the way exposure-linear already reads exposure-darkens' bright
    # run: 150 frames is ~3 s and this group is 7% of the suite. It carries
    # exposure-converge's camera move, so the two spots below carry it too and
    # the three runs differ by the mask alone.
    uni = rows
    moved_cam = ["--cam-at", EXPOSURE_CONVERGE_MOVE]
    # 1.0 exactly, not an arbitrarily large number: the radius is a fraction of
    # the UV half-diagonal, so 1.0 reaches the corners and covers the frame by
    # definition. This arm used 8.0 while the shader divided by the radius alone
    # and coverage saturated at 0.7071 -- so it passed without ever pinning what
    # the unit meant, and any value above 0.71 would have done.
    wide, wide_text = _exposure_probe(live,
                                      moved_cam + ["--meter-mode", "spot", "--meter-radius", "1.0"],
                                      frames=EXPOSURE_FRAMES)
    spot, _ = _exposure_probe(live, moved_cam + ["--meter-mode", "spot", "--meter-radius", "0.4"],
                              frames=EXPOSURE_FRAMES)
    if not (uni and wide and spot):
        # The probe helper already collected the renderer's output; printing its
        # tail is the difference between "something went wrong" and knowing what.
        print(f"  exposure-mask  ERROR no probe output: {wide_text.strip()[-160:]}")
        failures.append("exposure-mask")
    else:
        identity = wide[-1]["raw_log2"] == uni[-1]["raw_log2"]
        moved = abs(spot[-1]["raw_log2"] - uni[-1]["raw_log2"])
        ok = identity and moved > 1e-4
        print(f"  exposure-mask  {'PASS' if ok else 'FAIL'} a frame-covering spot reads "
              f"{'identical to' if identity else 'DIFFERENT from'} uniform "
              f"({wide[-1]['raw_log2']:.6f} vs {uni[-1]['raw_log2']:.6f}, want exact); "
              f"a real spot moves {moved:.4f} stops want >0 (or the mask does nothing)")
        if not ok:
            failures.append("exposure-mask")

    return failures
