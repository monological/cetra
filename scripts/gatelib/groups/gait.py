"""The gait group: a walker's gait (stride, footfalls, the eye's lag and bob, a climb; spec
13.47).
"""

import math
import re

from gatelib.probes import _gametest_probe, _probe_verdicts


# The gait's defaults (gait.c's create_gait), restated because a gate cannot ask the binary for
# them. A change to any of them is a change to how silent walks, which is what should go red.
GAIT_STEP_BASE = 0.40      # a level step, m, at no speed
GAIT_STEP_PER_SPEED = 0.25  # and longer by this per m/s
GAIT_EYE_RATE = 20.0        # rad/s of the eye's spring on the ground
GAIT_EYE_AIR_RATE = 60.0    # and in the air
GAIT_LAG_BELOW = 0.25
GAIT_LAG_ABOVE = 0.10
GAIT_DIP_PER_SPEED = 0.03
GAIT_DIP_RATE = 12.0
GAIT_SETTLE_FROM = 0.2      # gait.c's SETTLE_FROM
GAIT_SHUFFLE_TURN = math.radians(70.0)
GAIT_SHUFFLE_GAP = 0.35
GAIT_CLIMB_UP = 0.29        # a climb's rate, as a share of the speed asked


_GAIT_PROBE = re.compile(r"^gait (\S+) (\S+) (\S+) (.*)$", re.M)


def _gait_probe(case):
    """{(label, key): [floats]} from one gametest --gait-probe run, or None. No window, no
    physics and no world: the probe feeds the gait moves it makes up itself."""
    return _gametest_probe("--gait-probe", _GAIT_PROBE, case)


def _gait_spring(x0, rate, t):
    """A critically damped spring let go from x0 at rest: where it is t seconds on."""
    return x0 * (1.0 + rate * t) * math.exp(-rate * t)


def run_gait_gate(workdir):
    """The gait (spec 13.47): a walker's stride, footfalls, eye and pace, with no window.

    gait.c takes displacements and a time and touches nothing else, so every case below is a
    walk the probe makes up -- a straight one, a kerb, a fall, a flight as a ramp -- and every
    arm a closed form or an equality, never a recording. None of it is silent: the app's own
    walking is walked by a person, and these hold the module it is built on.

      gait-split     three seconds at 1.6 m/s in frames of 1/60, 1/120 and an uneven run of four
                     lengths take the same number of steps and end within a fiftieth of a step
                     of each other. The stride runs on ground covered, so how the frames fall
                     must not change how many steps a path takes.
      gait-cadence   a walk of 1.6 m/s steps 120 times a minute and a jog of 3.4 163, from
                     step = 0.40 + 0.25 * speed: the cadences people walk and jog at.
      gait-rest      a walk stopped part way through a step sets the foot in the air down
                     exactly once, takes no step standing, and leaves the eye on the body: no
                     lag and no bob, to a millionth of a metre.
      gait-shuffle   standing and turning 4.5 radians at 3 rad/s moves the feet three times,
                     every 70 degrees and no closer than 0.35 s; turning while walking moves
                     them only as steps.
      gait-bob-off   at bob_scale 0 the head does not move one bit through a walk with a kerb
                     in it, and the eye's lag is the same to the bit as at 1, since the bob
                     feeds nothing back. The scale-1 run bobs 2 cm, so the arm is not green on
                     a bob that never ran.
      gait-trough    every footfall lands at the bottom of the bob, within a hundredth of its
                     depth, and the sway between one footfall and the next leans toward the foot
                     that came down. Built the other way round, the footsteps land at the top
                     of the head's rise, which is half a step out and reads as a limp.
      gait-spring    a 15 cm kerb taken in one frame is taken up as a critically damped spring
                     at 20 rad/s, to the closed form at 1, 6 and 12 frames; a 40 cm rise holds
                     the eye no more than 25 cm below and a 30 cm drop no more than 10 above;
                     and in the air the same kerb goes at 60 rad/s.
      gait-flight    a walker asking 1.6 m/s up a flight of 15 risers of 0.19 on goings of 0.25,
                     collided as a ramp, takes 15 steps up it to within one (Blondel's rule, a
                     step for every 2R + G of 0.63 m), at a pace of exactly climb / grade, with
                     the flight counted whole and the bob faded out.
      gait-land      0.3 s in the air coming down at 2 m/s lands once, on the frame it lands,
                     and dips the eye 0.03 m per m/s -- 6 cm -- at 1/12 s, the critically damped
                     spring's peak.
      gait-hitch     one frame of a quarter second across six steps' ground lets one foot
                     fall, not six, and a frame of no time moves nothing and lets none fall.
    """
    failed = []
    verdict = _probe_verdicts(_gait_probe, failed)

    def split(r):
        ends = {label: r[(label, "end")] for label in ("60", "120", "uneven")}
        phases = [v[0] for v in ends.values()]
        steps = {int(v[1]) for v in ends.values()}
        ok = max(phases) - min(phases) < 0.02 and len(steps) == 1
        return ok, (f"3 s at 1.6 m/s ends at phase "
                    f"{', '.join(f'{k} {v[0]:.4f}' for k, v in ends.items())} (want within 0.02), "
                    f"in {sorted(steps)} steps (want one count)")

    def cadence(r):
        said = []
        ok = True
        for label, speed in (("walk", 1.6), ("jog", 3.4)):
            want = 60.0 * speed / (GAIT_STEP_BASE + GAIT_STEP_PER_SPEED * speed)
            got = r[(label, "rate")][0]
            ok = ok and abs(got - want) / want < 0.005
            said.append(f"{label} {got:.2f} a minute (want {want:.2f})")
        return ok, ", ".join(said)

    def rest(r):
        through, settles, steps = r[("stop", "through")]
        lag, bob = r[("end", "offset")]
        ok = (through > GAIT_SETTLE_FROM and int(settles) == 1 and int(steps) == 0 and lag < 1e-6
              and bob < 1e-6)
        return ok, (f"stopped {through:.3f} of a step past a heel (want past {GAIT_SETTLE_FROM}), "
                    f"it set {int(settles)} foot down (want 1) and took {int(steps)} steps "
                    f"standing (want 0); the eye's lag ended at {lag:.2e} m and the bob at "
                    f"{bob:.2e} (want under 1e-6)")

    def shuffle(r):
        count, gap = r[("standing", "count")]
        walking = r[("walking", "count")][0]
        want = math.floor(4.5 / GAIT_SHUFFLE_TURN)
        ok = int(count) == want and gap >= GAIT_SHUFFLE_GAP - 1e-6 and int(walking) == 0
        return ok, (f"4.5 rad turned standing moved the feet {int(count)} times (want {want}), "
                    f"{gap:.3f} s apart at the closest (want {GAIT_SHUFFLE_GAP} or more); turning "
                    f"while walking, {int(walking)} (want 0)")

    def bob_off(r):
        moved, lag_apart = r[("walk", "off")]
        deepest = r[("walk", "on")][0]
        ok = int(moved) == 0 and lag_apart == 0.0 and deepest < -0.019
        return ok, (f"at scale 0 the head moved on {int(moved)} frames (want 0) and the eye's lag "
                    f"differed from scale 1's by {lag_apart:g} m (want 0, to the bit); at scale 1 "
                    f"it bobbed {-deepest * 100:.2f} cm")

    def trough(r):
        depth, falls = r[("walk", "depth")]
        leaned, leans = r[("walk", "lean")]
        ok = depth > 0.99 and int(falls) >= 8 and int(leaned) == int(leans) and leans >= 8
        return ok, (f"over {int(falls)} footfalls the head was at least {depth:.4f} of the way "
                    f"down to the lowest of its step (want over 0.99), and leaned toward the foot "
                    f"that came down after {int(leaned)} of {int(leans)}")

    def spring(r):
        worst = max(abs(lag - _gait_spring(-0.15, GAIT_EYE_RATE, t))
                    for t, lag in (r[("kerb", frames)] for frames in ("1", "6", "12")))
        up, down = r[("clamp", "held")]
        t_air, air = r[("air", "3")]
        air_off = abs(air - _gait_spring(-0.15, GAIT_EYE_AIR_RATE, t_air))
        clamped = abs(up + GAIT_LAG_BELOW) < 1e-5 and abs(down - GAIT_LAG_ABOVE) < 1e-5
        ok = worst < 1e-5 and clamped and air_off < 1e-5
        return ok, (f"a 15 cm kerb taken up off the closed form by at most {worst:.2e} m over 12 "
                    f"frames; a 40 cm rise held at {up} and a 30 cm drop at {down} (want "
                    f"-{GAIT_LAG_BELOW} and {GAIT_LAG_ABOVE}); in the air, {air_off:.2e} off at 3 "
                    f"frames (want both under 1e-5)")

    def flight(r):
        steps = r[("ramp", "steps")][0]
        pace, stairs, bob, grade = r[("mid", "state")]
        ok = (abs(steps - 15) <= 1 and abs(pace - GAIT_CLIMB_UP / grade) < 1e-5
              and stairs == 1.0 and bob == 0.0 and abs(grade - 2.85 / 3.75) < 0.05 * 2.85 / 3.75)
        return ok, (f"{int(steps)} steps up 15 risers (want 15 within 1); half way the grade was "
                    f"{grade:.4f} (want {2.85 / 3.75:.4f} within 5%), the pace {pace:.5f} (want "
                    f"{GAIT_CLIMB_UP} / grade = {GAIT_CLIMB_UP / grade:.5f}), the flight counted "
                    f"at {stairs:g} (want 1) and the bob at {bob:g} (want 0)")

    def land(r):
        lands, first = r[("fall", "kinds")]
        dip, when = r[("fall", "dip")]
        want = -GAIT_DIP_PER_SPEED * 2.0
        ok = (int(lands) == 1 and int(first) == 1 and abs(dip - want) < 0.01 * abs(want)
              and abs(when - 1.0 / GAIT_DIP_RATE) <= 0.5 / 60.0 + 1e-6)
        return ok, (f"{int(lands)} landing (want 1), {'on' if first else 'NOT on'} the frame it "
                    f"landed; the eye dipped {dip * 100:.3f} cm at {when:.4f} s (want "
                    f"{want * 100:.3f} at {1.0 / GAIT_DIP_RATE:.4f})")

    def hitch(r):
        fell, crossed = r[("frame", "fell")]
        zero_fell, moved = r[("zero", "fell")]
        ok = int(fell) == 1 and crossed >= 2 and int(zero_fell) == 0 and moved == 0.0
        return ok, (f"a quarter-second frame across {int(crossed)} heel strikes let {int(fell)} "
                    f"foot fall (want 1); a frame of no time let {int(zero_fell)} fall and moved "
                    f"the stride {moved:g} (want 0 and 0)")

    ok, said = verdict("gait-split", "split", split)
    print(f"  gait-split {'PASS' if ok else 'FAIL'}  {said}")
    ok, said = verdict("gait-cadence", "cadence", cadence)
    print(f"  gait-cadence {'PASS' if ok else 'FAIL'}  {said}")
    ok, said = verdict("gait-rest", "rest", rest)
    print(f"  gait-rest {'PASS' if ok else 'FAIL'}  {said}")
    ok, said = verdict("gait-shuffle", "shuffle", shuffle)
    print(f"  gait-shuffle {'PASS' if ok else 'FAIL'}  {said}")
    ok, said = verdict("gait-bob-off", "bob-off", bob_off)
    print(f"  gait-bob-off {'PASS' if ok else 'FAIL'}  {said}")
    ok, said = verdict("gait-trough", "trough", trough)
    print(f"  gait-trough {'PASS' if ok else 'FAIL'}  {said}")
    ok, said = verdict("gait-spring", "spring", spring)
    print(f"  gait-spring {'PASS' if ok else 'FAIL'}  {said}")
    ok, said = verdict("gait-flight", "flight", flight)
    print(f"  gait-flight {'PASS' if ok else 'FAIL'}  {said}")
    ok, said = verdict("gait-land", "land", land)
    print(f"  gait-land {'PASS' if ok else 'FAIL'}  {said}")
    ok, said = verdict("gait-hitch", "hitch", hitch)
    print(f"  gait-hitch {'PASS' if ok else 'FAIL'}  {said}")
    return failed
