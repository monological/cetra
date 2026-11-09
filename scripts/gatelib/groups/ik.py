"""The ik group: two-bone IK (reach, clamp, the singular bind pose, the pole; spec 12.4)."""

import math
import os

from gatelib.harness import GAMETEST, asset
from gatelib.probes import _IK_PROBE, _gametest_probe, _ik_probe, _ik_probe_run


def _ik_probe_on(case, model):
    """The same, against a rig OTHER than the generated puppet.

    Uncached and separate from _ik_probe rather than an argument threaded through it:
    that one is memoised on the case alone and read by seventeen call sites, and widening
    its key to buy one caller a second rig is a worse trade than a four-line runner.

    `--puppet` is how the rig is chosen because --ik-probe dispatches after the parse
    loop and the probe loads whatever `puppet_path` then holds -- so this needs no probe
    case of its own and no C at all.
    """
    return _gametest_probe("--ik-probe", _IK_PROBE, case, extra=["--puppet", asset(model)])


def _ik_knee_bend_deg(a, b, c):
    """The knee's deviation from straight for segments a, b at hip-to-ankle distance c.

    The law of cosines gives the INTERIOR angle; what the probe measures is the angle
    between the thigh and shin directions, which is its supplement.
    """
    cos_interior = (a * a + b * b - c * c) / (2.0 * a * b)
    return 180.0 - math.degrees(math.acos(max(-1.0, min(1.0, cos_interior))))


def run_ik_gate(workdir):
    """Two-bone IK, foot locking and stride matching (specs 12.4, 12.9, 12.10 and 12.14),
    on the puppet whose limb lengths the fixture generator states -- except ik-pole-derived,
    which needs a rig that binds BENT and takes the committed t_pose.fbx, the only arm here
    whose input is imported rather than stated in a table. Nineteen arms drive the solver
    with SYNTHETIC targets through gametest's --ik-probe and need no physics at all; three
    stand the rig on real ground and raycast. Every expected angle is recomputed here in
    Python from the segment lengths the probe reports, so nothing is a magic number
    copied from a run. The bind pose is the SINGULAR configuration -- hip to ankle is
    thigh plus shin exactly -- which is why the singular and identity arms are the two
    this group could least afford to omit.

      ik-reach     a target inside the reach is HIT: the ankle lands on it over a sweep
                   of nine directions, and the leg actually bends to do it. A solver
                   that merely points at a target passes nothing here.
      ik-clamp     a target beyond the reach leaves the leg straight and AIMED -- the
                   ankle at full extension, the bend 0, and the leg direction agreeing
                   with the target direction. A target ON the hip is the other
                   unreachable end and must not make a direction from a zero vector.
      ik-singular  at full extension the bend is 0 and at the inner fold it is 180: the
                   two places acos's argument lands on -1 or +1, where one ulp of float
                   error is a NaN. A NaN fails by ABSENCE, the probe line no longer
                   matching the numeric grammar. Straightness is held to 0.05 degrees
                   and not to 0, because acos's derivative is unbounded exactly there
                   and float epsilon comes back as about 0.02 -- asserting 0 would be
                   asserting the arithmetic is better than it can be.
      ik-identity  weight 0 changes the pose by 0 EXACTLY, because the solver skips the
                   chain rather than solving it with a null weight. Re-solving a bent
                   leg onto the ankle it already has is idempotence rather than that
                   identity -- the second pass reads a hip the first one rotated -- so
                   it is held to a tolerance and the two are asserted apart.
      ik-pole      the knee goes the way the pole says, and reversing the pole reverses
                   it. The SPAN between the two is what proves the pole is read at all
                   rather than defaulted, which a sign test alone would not catch. On
                   this rig the CALLER's pole is what is read, the bind knee sitting
                   exactly on the hip-ankle line -- which is what the next arm exists
                   for.
      ik-pole-derived the same case on t_pose.fbx, whose knee binds 4.4 degrees forward
                   and whose hip binds ROTATED, and the answer inverts: both stated
                   poles land the knee the SAME way and the span collapses, because the
                   rig's own bind overrode the argument in each. Without it nothing here
                   sees the derivation at all -- deleting it leaves every other arm in
                   this group green, since the generated puppet can only take the
                   fallback. The rotated hip is what also reaches the conversion into
                   the hip's bind frame, which a rig binding upright cannot.
      ik-analytic  the closed form, the arm this group is anchored on: the bend at five
                   distances, against acos((a^2 + b^2 - c^2) / 2ab) computed here from
                   the segments and the distance the probe reports. The sweep stops
                   short of full extension deliberately, because the derivative there
                   is unbounded and a nearer value would assert float noise.
      ik-ray-self  the foot ray finds the WORLD and not the body it was cast from. The
                   same ray unfiltered is asserted to HIT the character, so the arm
                   cannot quietly go vacuous the day the capsule stops being there.
      ik-sole      sole_offset IS the ankle's bind height, which is the one thing
                   ik-plant cannot say. That arm compares the solved ankle against
                   IkFoot.target, and ik_foot_set_ground WROTE that target as ground plus
                   sole_offset -- so both sides of it descend from the same field and a
                   doubled or zeroed offset moves them together. Here the probe's own
                   read of the bind pose is the independent half.
      ik-plant     six feet, over flat ground and both fixtures, land ON the ground the
                   ray found rather than near it. Without the pelvis drop the uphill leg
                   cannot reach and this is the arm that would say so. These three cases
                   run with RELEASE OFF, deliberately: they ask whether the solve reaches
                   the ground it was handed, and whether a foot should be planted at all
                   is ik-swing's question. Mixed together, a foot whose target sits below
                   the clip's fades to partial weight and lands short, which reads as a
                   solver that misses when it is a release behaving as specified.
      ik-slope     a ramp rising 1 in 4: the two feet find ground one slope times one
                   stance apart, and BOTH of those come from the probe, so neither side
                   carries a constant the other has to match. The uphill knee bends and
                   the downhill one is left where the clip had it.
      ik-step      the same over one riser, with the uphill foot on the OTHER side. A
                   solver that hardcoded which leg bends passes the ramp and fails here.
      ik-repeat    two runs of the slope case print identical digits. The raycast is the
                   one input this feature has that nothing else in the suite would
                   notice going order-dependent.
      ik-swing     a walk cycle actually TICKED, with the ankle's vertical travel
                   measured across it. Every other arm here poses the rig ONCE, which is
                   exactly why all of them passed while the player's feet were welded to
                   the floor and the legs had stopped moving -- a solve that overwrites a
                   stride looks perfect in a single pose. A weld reads 0. The bar is 60
                   per cent of the clip's own travel rather than 100, because release is
                   a weight FADE: a foot halfway up its swing is still part planted, so
                   the stride is damped by design and parity would be asserting a design
                   this does not have.
      ik-slide     the SLIDE, in metres: how far a foot already in contact travels
                   across the ground while the body moves over it, locked against
                   planted, over the SAME ticks. The body's speed is not chosen by the
                   fixture, it is read off the clip -- the stance foot's own mean
                   backward velocity -- so it is by construction the speed at which the
                   animation's feet are stationary in the world, and every millimetre
                   reported is the solver's. Both windows are asserted to be real
                   first: a label that said "always in contact" would report a small
                   drift for the wrong reason, one that said "never" would leave
                   nothing to measure, and a lock that claimed four ticks would look
                   perfect over them.
      ik-hysteresis one pin per walk cycle. Two thresholds instead of one is the whole
                   of the hysteresis, and this is the only arm that can see it: with a
                   single threshold the decision is re-made every frame and flips, and
                   the pin count comes out several times the cycle count while every
                   other number here barely moves.
      ik-unlock    walked at three times the speed its own feet imply, a lock releases
                   rather than straining to hold a point the animation has left: the
                   mean hold collapses against the same clip at its own speed. That it
                   still pins at all is asserted too, or the arm would pass on a
                   feature that had stopped working.
      ik-drop      the pelvis drop's CAP, and the refusal underneath it. A foot half a
                   metre past full extension drops the hips by the cap exactly and is
                   left short of the rest -- dragging them the whole way is the "T-Rex"
                   posture the technique warns against, and a little sliding is better
                   than breaking the source animation. The arm asserts the deficit
                   EXCEEDS the cap first: spec 12.5 found nothing in the tree ever asked
                   for more than it, so min(deficit, cap) returned the deficit and a
                   wrong cap passed.
      ik-settle    the largest single TICK of travel while a contact is held, which
                   ik-slide's total cannot see: a foot that jumps once and then holds
                   still reports the same drift as one that eases across a whole stance.
                   Spec 12.9 planned this arm and shipped without it; written afterwards
                   it immediately found that the held target was derived through the
                   PRE-solve ankle-to-toe vector, so the toe landed off the contact by
                   that vector times the rotation the solve was about to apply -- one
                   tick carrying the WHOLE of the residual drift, 0.010256 of 0.010256.
      ik-contact   the solver's contact label finds ONE contact per walk cycle. That
                   count is the arm: a label that flaps splits a stance into several
                   runs while the fraction and the agreement barely move, and a lock
                   built on it releases and re-grabs mid stride. The fraction and the
                   agreement with the probe's own independent height window are the
                   other two halves -- the label is the ANKLE's lift and speed, the
                   window is the ankle's height against its own travel, so they are
                   different quantities and agreeing is evidence.
      ik-slide-speeds  the same walk at both edges of the band a clip can be stretched
                   over, each one walked TWICE -- once at rate 1, the way a game with no
                   stride matching plays it, and once at the rate the engine works out.
                   The A/B is the arm: a bar on the matched number alone encodes whatever
                   else the solver is doing, and on this clip that includes a standing
                   correction of 0.45 of a leg on the right foot which no playback rate
                   moves. At the band's MIDDLE the two must be identical, which is the
                   self-check -- at the clip's own speed the rate is 1 and matching has
                   to be a no-op.
      ik-scale     the same walk on a rig node scaled 1 and 2 comes out identical in
                   model units. Every threshold the solver owns is in model space and a
                   fraction of the leg, so a scale reaches the world matrix and nothing
                   it decides. This is the gap spec 12.9 shipped a live bug through, and
                   re-introducing that comparison moves the scale-2 slide 3.3x.
    """
    if not os.path.exists(GAMETEST):
        print("  ik           SKIP  (gametest not built)")
        return []
    del workdir # nothing is rendered and nothing is written; three cases do raycast
    failures = []

    # --- ik-reach --------------------------------------------------------------
    d = _ik_probe("reach")
    if not d:
        print("  ik-reach     FAIL  the probe failed or measured nothing")
        failures.append("ik-reach")
    else:
        residuals = [d[(f"t{i}", "residual")][0] for i in range(9) if (f"t{i}", "residual") in d]
        bends = [d[(f"t{i}", "bend")][0] for i in range(9) if (f"t{i}", "bend") in d]
        worst = max(residuals) if residuals else 1.0
        least_bend = min(bends) if bends else 0.0
        ok = len(residuals) == 9 and worst < 1e-5 and least_bend > 1.0
        print(f"  ik-reach     {'PASS' if ok else 'FAIL'}  {len(residuals)} targets, worst "
              f"residual {worst:.7f} m (want < 1e-5), least bend {least_bend:.3f} deg "
              f"(want > 1: a straight leg did not solve)")
        if not ok:
            failures.append("ik-reach")

    # --- ik-clamp --------------------------------------------------------------
    d = _ik_probe("clamp")
    need = [("far", "dist"), ("far", "bend"), ("far", "align"), ("atHip", "dist"),
            ("atHip", "bend"), ("rig", "segments")]
    if not d or any(k not in d for k in need):
        print("  ik-clamp     FAIL  the probe failed or measured nothing")
        failures.append("ik-clamp")
    else:
        seg = d[("rig", "segments")]
        span = seg[0] + seg[1]
        far_dist = d[("far", "dist")][0]
        far_bend = d[("far", "bend")][0]
        align = d[("far", "align")][0]
        hip_dist = d[("atHip", "dist")][0]
        hip_bend = d[("atHip", "bend")][0]
        # A clamped leg is straight to within what acos can resolve AT its own
        # singularity, not to zero: the distance lands exactly on thigh + shin, so the
        # cosine lands on -1, where acos's derivative is unbounded and float epsilon
        # comes back as ~0.02 degrees. Asserting 0 here would be asserting that the
        # arithmetic is better than it can be.
        #
        # The at-hip half asserts the REFUSAL: a target on the hip gives no direction to
        # aim along, the solve returns without touching the pose, and from bind that
        # leaves the leg straight at full extension. An isfinite() check stood here and
        # could never fail -- the probe's grammar admits no letters, so a nan never
        # matches, the key never arrives, and the `need` guard above fails first. It was
        # the only assertion this end of the arm had.
        ok = (abs(far_dist - span) < 1e-5 and far_bend < 0.05 and align > 1.0 - 1e-6
              and abs(hip_dist - span) < 1e-5 and hip_bend < 0.05)
        print(f"  ik-clamp     {'PASS' if ok else 'FAIL'}  beyond reach the ankle sits "
              f"{far_dist:.6f} from the hip against a span of {span:.6f}, bend "
              f"{far_bend:.6f} deg, aim {align:.8f} (want 1); a target ON the hip is "
              f"refused and leaves the leg straight at {hip_dist:.6f} with bend "
              f"{hip_bend:.6f}")
        if not ok:
            failures.append("ik-clamp")

    # --- ik-singular -----------------------------------------------------------
    d = _ik_probe("singular")
    need = [("full", "bend"), ("full", "dist"), ("folded", "bend"), ("folded", "dist"),
            ("rig", "segments")]
    if not d or any(k not in d for k in need):
        print("  ik-singular  FAIL  the probe failed, or a NaN kept a line from matching")
        failures.append("ik-singular")
    else:
        seg = d[("rig", "segments")]
        span, fold = seg[0] + seg[1], abs(seg[0] - seg[1])
        full_bend = d[("full", "bend")][0]
        folded_bend = d[("folded", "bend")][0]
        # 0.05 rather than 0 for the same reason ik-clamp carries it: this IS the
        # singularity, and acos cannot resolve the last few ulps of its own argument.
        ok = (full_bend < 0.05 and folded_bend > 180.0 - 0.5
              and abs(d[("full", "dist")][0] - span) < 1e-5
              and abs(d[("folded", "dist")][0] - fold) < 1e-3)
        print(f"  ik-singular  {'PASS' if ok else 'FAIL'}  at full extension bend "
              f"{full_bend:.6f} deg (want 0) and reach {d[('full', 'dist')][0]:.6f} of "
              f"{span:.6f}; at the inner fold bend {folded_bend:.4f} deg (want 180) and "
              f"reach {d[('folded', 'dist')][0]:.6f} of {fold:.6f}")
        if not ok:
            failures.append("ik-singular")

    # --- ik-identity -----------------------------------------------------------
    d = _ik_probe("identity")
    need = [("bent", "maxdiff"), ("zeroweight", "maxdiff")]
    if not d or any(k not in d for k in need):
        print("  ik-identity  FAIL  the probe failed or measured nothing")
        failures.append("ik-identity")
    else:
        bent = d[("bent", "maxdiff")][0]
        zero = d[("zeroweight", "maxdiff")][0]
        ok = zero == 0.0 and bent < 1e-5
        print(f"  ik-identity  {'PASS' if ok else 'FAIL'}  weight 0 moves the pose by "
              f"{zero:.6f} (want 0 exactly, the chain is skipped); re-solving a bent leg "
              f"onto its own ankle moves it {bent:.7f} (want < 1e-5, idempotence rather "
              f"than identity)")
        if not ok:
            failures.append("ik-identity")

    # --- ik-pole ---------------------------------------------------------------
    d = _ik_probe("pole")
    need = [("fwd", "kneez"), ("back", "kneez"), ("span", "delta")]
    if not d or any(k not in d for k in need):
        print("  ik-pole      FAIL  the probe failed or measured nothing")
        failures.append("ik-pole")
    else:
        fwd = d[("fwd", "kneez")][0]
        back = d[("back", "kneez")][0]
        span = d[("span", "delta")][0]
        ok = fwd > 0.05 and back < -0.05 and span > 0.10
        print(f"  ik-pole      {'PASS' if ok else 'FAIL'}  knee at z {fwd:+.4f} forward "
              f"and {back:+.4f} reversed (want opposite signs past 0.05), {span:.4f} apart "
              f"(want > 0.10, or the pole was never read)")
        if not ok:
            failures.append("ik-pole")

    # --- ik-pole-derived -------------------------------------------------------
    # The SAME case on a rig that binds BENT, where the answer inverts (spec 12.14).
    #
    # ik-pole above runs on the generated puppet, whose three leg joints share one x and
    # one z to the digit -- so its bind knee sits exactly on the hip-ankle line, the
    # derivation finds no direction to take and falls back to the caller's vector. Every
    # number it prints is the behaviour that existed before the pole came from the rig,
    # and deleting that derivation leaves it green.
    #
    # t_pose.fbx binds its knee 4.4 degrees forward and its hip ROTATED, so it reaches
    # both halves: the offset is read, AND it is rotated through a hip frame that is not
    # the identity. No fixture was built for this -- the corpus already had one, the same
    # way 12.11's Z-up fixture turned out unnecessary.
    # Same case, so the same three keys ik-pole above already named.
    pole_need = [("fwd", "kneez"), ("back", "kneez"), ("span", "delta")]
    d = _ik_probe_on("pole", "t_pose.fbx")
    if not d or any(k not in d for k in pole_need):
        print("  ik-pole-derived FAIL  the probe failed or measured nothing on t_pose.fbx")
        failures.append("ik-pole-derived")
    else:
        b_fwd = d[("fwd", "kneez")][0]
        b_back = d[("back", "kneez")][0]
        b_span = d[("span", "delta")][0]
        # BOTH legs must land the same way and the span must collapse: the rig's own bind
        # overrode the argument in each. `back` is the cell that carries it -- it is
        # negative whenever the caller's vector wins, so a run against the old code reads
        # two opposite knees and a wide span here.
        ok = b_span < 0.02 and b_fwd > 0.05 and b_back > 0.05
        print(f"  ik-pole-derived {'PASS' if ok else 'FAIL'}  on a rig that binds bent the "
              f"knee lands at z {b_fwd:+.4f} and {b_back:+.4f} for OPPOSITE stated poles, "
              f"{b_span:.4f} apart (want under 0.02 and both past +0.05: the bind beat the "
              f"argument both times, where a caller-driven pole reads them opposite and "
              f"about 0.36 apart, as ik-pole does above)")
        if not ok:
            failures.append("ik-pole-derived")

    # --- ik-analytic -----------------------------------------------------------
    d = _ik_probe("analytic")
    if not d or ("rig", "segments") not in d:
        print("  ik-analytic  FAIL  the probe failed or measured nothing")
        failures.append("ik-analytic")
    else:
        seg = d[("rig", "segments")]
        worst, seen, expected, reach_miss = 0.0, 0, [], 0.0
        for i in range(5):
            keys = [(f"c{i}", k) for k in ("bend", "dist", "want")]
            if any(k not in d for k in keys):
                continue
            got = d[(f"c{i}", "bend")][0]
            reached = d[(f"c{i}", "dist")][0]
            asked = d[(f"c{i}", "want")][0]
            # Against the distance ASKED for, not the one reached. Deriving the expected
            # bend from `reached` alone is self-consistent for a solver that put the
            # ankle anywhere it liked and reported the matching angle -- the arm this
            # group is anchored on would have scored 0.0000 on it.
            reach_miss = max(reach_miss, abs(reached - asked))
            want = _ik_knee_bend_deg(seg[0], seg[1], asked)
            expected.append(want)
            worst = max(worst, abs(got - want))
            seen += 1
        ok = seen == 5 and worst < 0.05 and reach_miss < 1e-5
        shown = "/".join(f"{v:.2f}" for v in expected) if expected else "none"
        print(f"  ik-analytic  {'PASS' if ok else 'FAIL'}  {seen} of 5 distances, worst "
              f"error {worst:.4f} deg against the closed form's {shown} (want < 0.05)")
        if not ok:
            failures.append("ik-analytic")

    # --- ik-ray-self -----------------------------------------------------------
    d = _ik_probe("ground")
    need = [("self", "filtered"), ("self", "unfiltered"), ("self", "onfloor")]
    if not d or any(k not in d for k in need):
        print("  ik-ray-self  FAIL  the probe failed or measured nothing")
        failures.append("ik-ray-self")
    else:
        filt = d[("self", "filtered")][0]
        unfilt = d[("self", "unfiltered")][0]
        onfloor = d[("self", "onfloor")][0]
        # The unfiltered leg is asserted to HIT. Without it this arm would keep passing
        # on a world where the capsule had quietly stopped existing, which is a test
        # that proves the filter works by never exercising it.
        ok = filt == 0 and onfloor == 1 and unfilt == 1
        print(f"  ik-ray-self  {'PASS' if ok else 'FAIL'}  filtered, the ray lands on the floor "
              f"({int(onfloor)}) and not the character ({int(filt)}); unfiltered from the same "
              f"origin it does hit the character ({int(unfilt)}, want 1, or this arm is "
              f"vacuous)")
        if not ok:
            failures.append("ik-ray-self")

    # --- ik-sole ---------------------------------------------------------------
    d = _ik_probe("ground")
    if not d or ("rig", "soleoffset") not in d:
        print("  ik-sole      FAIL  the probe failed or measured nothing")
        failures.append("ik-sole")
    else:
        got, bind = d[("rig", "soleoffset")][0], d[("rig", "soleoffset")][1]
        # Exact, not a tolerance: both are the same bind ankle Y read twice, once by
        # ik_add_foot through skeleton_compute_bind_globals and once by the probe off the
        # posed state. Anything but equality means the engine derived it from something
        # else, which is the whole failure this arm exists for.
        ok = got == bind and bind > 0.0
        print(f"  ik-sole      {'PASS' if ok else 'FAIL'}  the engine's sole_offset is "
              f"{got:.6f} against the rig's bind ankle height {bind:.6f} (want equal, and "
              f"non-zero or a planted foot would sink by its own thickness)")
        if not ok:
            failures.append("ik-sole")

    # --- ik-plant --------------------------------------------------------------
    soles, probed = [], True
    for case in ("ground", "slope", "step"):
        dd = _ik_probe(case)
        if not dd or ("pose", "soleleft") not in dd or ("pose", "soleright") not in dd:
            probed = False
            break
        soles.append(dd[("pose", "soleleft")][0])
        soles.append(dd[("pose", "soleright")][0])
    if not probed or len(soles) != 6:
        print("  ik-plant     FAIL  a probe failed or measured nothing")
        failures.append("ik-plant")
    else:
        worst = max(soles)
        ok = worst < 1e-6
        print(f"  ik-plant     {'PASS' if ok else 'FAIL'}  over flat ground, the ramp and the "
              f"steps, six feet land on the ground their ray found; worst miss {worst:.8f} m "
              f"(want < 1e-6)")
        if not ok:
            failures.append("ik-plant")

    # --- ik-slope --------------------------------------------------------------
    d = _ik_probe("slope")
    need = [("ray", "left"), ("ray", "right"), ("ray", "delta"), ("rig", "stance"),
            ("fixture", "slope"), ("pose", "bendleft"), ("pose", "bendright")]
    if not d or any(k not in d for k in need):
        print("  ik-slope     FAIL  the probe failed or measured nothing")
        failures.append("ik-slope")
    else:
        stance = d[("rig", "stance")][0]
        slope = d[("fixture", "slope")][0]
        want = slope * stance
        got = d[("ray", "delta")][0]
        up, down = d[("pose", "bendleft")][0], d[("pose", "bendright")][0]
        ok = abs(got - want) < 1e-4 and up > down + 10.0
        print(f"  ik-slope     {'PASS' if ok else 'FAIL'}  the feet find ground "
              f"{d[('ray', 'left')][0]:.4f} and {d[('ray', 'right')][0]:.4f}, {got:.6f} apart "
              f"against slope {slope:g} times stance {stance:g} = {want:.6f}; the uphill knee "
              f"bends {up:.2f} deg against the downhill {down:.2f}")
        if not ok:
            failures.append("ik-slope")

    # --- ik-step ---------------------------------------------------------------
    d = _ik_probe("step")
    need = [("ray", "delta"), ("fixture", "riser"), ("pose", "bendleft"), ("pose", "bendright")]
    if not d or any(k not in d for k in need):
        print("  ik-step      FAIL  the probe failed or measured nothing")
        failures.append("ik-step")
    else:
        riser = d[("fixture", "riser")][0]
        got = d[("ray", "delta")][0]
        left, right = d[("pose", "bendleft")][0], d[("pose", "bendright")][0]
        ok = abs(got - riser) < 1e-4 and right > left + 10.0
        print(f"  ik-step      {'PASS' if ok else 'FAIL'}  one riser apart, {got:.6f} against "
              f"{riser:.6f}; the uphill knee here is the RIGHT one ({right:.2f} deg against the "
              f"left's {left:.2f}) -- the mirror of the ramp, so a solver that hardcoded a leg "
              f"passes one and fails the other")
        if not ok:
            failures.append("ik-step")

    # --- ik-repeat -------------------------------------------------------------
    first, second = _ik_probe_run("slope"), _ik_probe_run("slope")
    lock_a, lock_b = _ik_probe_run("lock"), _ik_probe_run("lock")
    if not first or not second or not lock_a or not lock_b:
        print("  ik-repeat    FAIL  a probe failed or measured nothing")
        failures.append("ik-repeat")
    else:
        # `is not` carries the whole arm. This must be TWO process launches: served from
        # the memo both names would be one object, first == second would be trivially
        # true, and the one arm asserting cross-process determinism would pass forever
        # without anything noticing. gate-arm-docs reads print sites, not probe calls, so
        # nothing else would catch it. The identity test fails loudly instead.
        ok = (first is not second and first == second and
              lock_a is not lock_b and lock_a == lock_b)
        print(f"  ik-repeat    {'PASS' if ok else 'FAIL'}  two runs each of the slope and lock "
              f"cases print {'identical' if ok else 'DIFFERENT'} digits across "
              f"{len(first)} and {len(lock_a)} measurements"
              f"{'' if first is not second else ' -- VACUOUS: both came from the cache'}. The "
              f"lock case is the noise floor for ik-slide, which quotes a number without one "
              f"otherwise")
        if not ok:
            failures.append("ik-repeat")

    # --- ik-swing --------------------------------------------------------------
    d = _ik_probe("swing")
    need = [("off", "travel"), ("on", "travel")]
    if not d or any(k not in d for k in need):
        print("  ik-swing     FAIL  the probe failed or measured nothing")
        failures.append("ik-swing")
    else:
        off = d[("off", "travel")][0]
        on = d[("on", "travel")][0]
        ratio = (on / off) if off > 1e-6 else 0.0
        # A BAND, not a floor, and the upper bound is the half this arm was missing. A
        # weld reads 0 -- but an IK that never ran at all reads 1.0, and that is the
        # other way this feature breaks. One-sided, the arm written to catch a stride
        # being overwritten would have passed a solve that was never applied.
        ok = off > 0.05 and 0.6 < ratio < 0.95
        print(f"  ik-swing     {'PASS' if ok else 'FAIL'}  the ankle travels {off:.6f} over a "
              f"walk cycle with IK off and {on:.6f} with it on, {ratio * 100.0:.0f} per cent "
              f"(want > 60; a foot welded to the ground reads 0). Damped and not equal because "
              f"release is a weight fade, so a foot mid-swing is still part planted")
        if not ok:
            failures.append("ik-swing")

    # --- ik-slide --------------------------------------------------------------
    d = _ik_probe("lock")
    need = [("clip", "stride"), ("contact", "fraction"), ("hold", "fraction"),
            ("plant", "slide"), ("plant", "slidefrac"), ("hold", "slide"),
            ("hold", "slidefrac")]
    if not d or any(k not in d for k in need):
        print("  ik-slide     FAIL  the probe failed or measured nothing")
        failures.append("ik-slide")
    else:
        stride = d[("clip", "stride")][0]
        frac = d[("contact", "fraction")][0]
        hold = d[("hold", "fraction")][0]
        plant_m, plant = d[("plant", "slide")][0], d[("plant", "slidefrac")][0]
        hold_m, held = d[("hold", "slide")][0], d[("hold", "slidefrac")][0]
        # The window has to BE a stance before the travel across it means anything, and a
        # walk's is a little over half its cycle. Both halves of this guard have been wrong
        # once: a contact band stated as a fraction of LEG length rather than of the foot's
        # own lift read 77 per cent of the cycle as contact. The hold has to be a real
        # share of the cycle too, since both numbers are read over the interval the lock
        # claims and a lock that claimed four ticks would look perfect over them.
        window = 0.35 < frac < 0.75 and stride > 0.05 and 0.3 < hold < 0.75
        ok = window and held < 0.01 and held < plant
        print(f"  ik-slide     {'PASS' if ok else 'FAIL'}  over the {hold * 100.0:.0f} per "
              f"cent of the cycle the lock holds, the toe travels {hold_m:.6f} m across the "
              f"ground against planting's {plant_m:.6f} m over the same ticks -- "
              f"{held * 100.0:.1f} against {plant * 100.0:.1f} per cent of a leg (want under "
              f"1, and under planting's). The body moves at the clip's own "
              f"{stride:.6f} m/s, so every millimetre is the solver's")
        if not ok:
            failures.append("ik-slide")

    # --- ik-hysteresis ---------------------------------------------------------
    d = _ik_probe("lock")
    if not d or any(k not in d for k in [("hold", "pins"), ("label", "cycles")]):
        print("  ik-hysteresis FAIL  the probe failed or measured nothing")
        failures.append("ik-hysteresis")
    else:
        pins = d[("hold", "pins")][0]
        cycles = d[("label", "cycles")][0]
        # One pin per walk cycle. The argument for two thresholds rather than one is the
        # technique author's -- a single threshold is re-decided every frame and can flip,
        # the "scissoring in and out very quickly" he describes -- and a flip would show
        # here, as a pin count several times the cycle count, and in no other arm. Stated
        # as the reasoning it is: nobody has run this build with the thresholds equal, so
        # this is not a number that was measured and then asserted.
        ok = pins == cycles
        print(f"  ik-hysteresis {'PASS' if ok else 'FAIL'}  the lock pins {pins:.0f} times "
              f"over {cycles:.0f} cycles (want one each; one threshold instead of two flips "
              f"the state and reads several)")
        if not ok:
            failures.append("ik-hysteresis")

    # --- ik-unlock -------------------------------------------------------------
    d = _ik_probe("lock")
    if not d or any(k not in d for k in [("hold", "total"), ("hold", "pins"),
                                         ("fast", "ticks"), ("fast", "pins")]):
        print("  ik-unlock    FAIL  the probe failed or measured nothing")
        failures.append("ik-unlock")
    else:
        pins = d[("hold", "pins")][0]
        total = d[("hold", "total")][0]
        fast_pins = d[("fast", "pins")][0]
        fast = d[("fast", "ticks")][0]
        # Walked at three times the speed its own feet imply, every lock breaks its unlock
        # distance almost at once. The feature must LET GO rather than strain the chain to
        # hold a point the animation has left, so the mean hold collapses. fast_pins > 0 is
        # what stops this passing because nothing ever locked at all.
        mean = total / pins if pins > 0 else 0.0
        fast_mean = fast / fast_pins if fast_pins > 0 else 0.0
        ok = fast_pins > 0 and mean > 0 and fast_mean < 0.6 * mean
        print(f"  ik-unlock    {'PASS' if ok else 'FAIL'}  at three times the clip's own "
              f"speed a lock lasts {fast_mean:.0f} ticks against {mean:.0f} at its own "
              f"(want under 60 per cent; an unreachable lock releases rather than straining, "
              f"and {fast_pins:.0f} pins says it still locks at all)")
        if not ok:
            failures.append("ik-unlock")

    # --- ik-drop ---------------------------------------------------------------
    p = _ik_probe("drop")
    need = [("pelvis", "moved"), ("pelvis", "cap"), ("reach", "deficit"),
            ("reach", "short")]
    if not p or any(k not in p for k in need):
        print("  ik-drop      FAIL  the probe failed or measured nothing")
        failures.append("ik-drop")
    else:
        moved = p[("pelvis", "moved")][0]
        cap = p[("pelvis", "cap")][0]
        deficit = p[("reach", "deficit")][0]
        short = p[("reach", "short")][0]
        # The cap has to BIND for this to be a test of it at all -- spec 12.5 found that
        # every fixture in the tree asks for less than the cap, so min(deficit, cap)
        # returns the deficit and a wrong cap passes. Hence deficit > cap asserted first.
        # The second half is the refusal: whatever the drop does not cover, the foot is
        # left short of. Dragging the hips the rest of the way is the "T-Rex" posture the
        # technique warns against, and a solver that did it would fail here.
        ok = (deficit > cap > 0.0 and abs(moved - cap) < 1e-6 and
              abs(short - (deficit - cap)) < 1e-6)
        print(f"  ik-drop      {'PASS' if ok else 'FAIL'}  a foot {deficit:.6f} m past full "
              f"extension drops the pelvis {moved:.6f} m and no further (cap {cap:.6f}), "
              f"leaving the foot {short:.6f} m short (want the remainder exactly; the hips "
              f"covering it is the posture the method warns against)")
        if not ok:
            failures.append("ik-drop")

    # --- ik-settle -------------------------------------------------------------
    d = _ik_probe("lock")
    need = [("hold", "step"), ("plant", "step"), ("rig", "segments")]
    if not d or any(k not in d for k in need):
        print("  ik-settle    FAIL  the probe failed or measured nothing")
        failures.append("ik-settle")
    else:
        step = d[("hold", "step")][0]
        plant_step = d[("plant", "step")][0]
        leg = sum(d[("rig", "segments")])
        # The largest single TICK of travel while a contact is held. ik-slide's total
        # cannot see a transition that pops: a foot that jumps once and then holds still
        # reports the same drift as one that eases across the whole stance. It is a
        # distinct failure and it was a real one -- the held target was derived through the
        # PRE-solve ankle-to-toe vector, so the toe landed off the contact by that vector
        # times the rotation the solve was about to apply, once, on the frame after every
        # pin. Step and slide read 0.010256 and 0.010256: the entire drift was one tick.
        # Bounded in ABSOLUTE terms and against planting, NOT as a share of the total
        # drift. "The drift is spread rather than spent in one tick" was the first
        # formulation and it is exactly backwards: the better the lock, the smaller the
        # total, and the more of what remains is necessarily its own largest tick. It
        # would have failed the fix that motivated it.
        ok = step < 0.5 * plant_step and step < 0.01 * leg
        print(f"  ik-settle    {'PASS' if ok else 'FAIL'}  the largest single tick of held "
              f"travel is {step:.6f} m -- {step / leg * 100.0:.1f} per cent of a leg (want "
              f"under 1) and under half planting's {plant_step:.6f} over the same window. A "
              f"transition that pops shows here and in no other arm")
        if not ok:
            failures.append("ik-settle")

    # --- ik-contact ------------------------------------------------------------
    d = _ik_probe("lock")
    if not d or any(k not in d for k in [("label", "runs"), ("label", "cycles"),
                                         ("label", "fraction"), ("label", "agree")]):
        print("  ik-contact   FAIL  the probe failed or measured nothing")
        failures.append("ik-contact")
    else:
        runs = d[("label", "runs")][0]
        cycles = d[("label", "cycles")][0]
        share = d[("label", "fraction")][0]
        agree = d[("label", "agree")][0]
        # ONE contact per cycle is the whole arm. A label that flaps splits a stance into
        # several runs and nothing else here would notice -- the fraction barely moves and
        # the agreement barely moves, but a lock built on it releases and re-grabs mid
        # stride. This caught exactly that: judged at the toe the count was 6 over 3
        # cycles, because a retargeted clip's toe joint passes back through its own bind
        # clearance in mid-swing, slowly enough that the speed test admits it too.
        ok = (runs == cycles and 0.3 < share < 0.7 and agree > 0.6)
        print(f"  ik-contact   {'PASS' if ok else 'FAIL'}  {runs:.0f} contacts over "
              f"{cycles:.0f} cycles (want one each, or the label is flapping), the foot "
              f"down {share * 100.0:.0f} per cent of the time (want 30 to 70) and agreeing "
              f"{agree * 100.0:.0f} per cent with the probe's own height window (want > 60; "
              f"two different quantities, so agreement is evidence rather than a tautology)")
        if not ok:
            failures.append("ik-contact")

    # --- ik-slide-speeds -------------------------------------------------------
    d = _ik_probe("rate")
    legs = ("slow", "one", "fast")
    need = [(leg, k) for leg in legs
            for k in ("hold", "slidefrac", "fixfrac", "freehold", "freefixfrac")]
    if not d or ("one", "play") not in d or any(k not in d or len(d[k]) < 2 for k in need):
        print("  ik-slide-speeds FAIL  the probe failed or measured nothing")
        failures.append("ik-slide-speeds")
    else:
        # A DIRECT A/B: the same body speed walked twice, once with the clip played at rate
        # 1 the way a game with no stride matching plays it, and once at the rate the
        # engine works out. A bar on the matched number alone would encode everything else
        # the solver is doing -- including a standing correction of 0.45 of a leg on the
        # right foot that no playback rate moves, a spec 12.9 contact label opening during
        # late swing, which is filed rather than fixed here.
        #
        # The MEAN over both feet, because they do not fail alike and singling out the one
        # that behaves is how an arm gets written to pass. And the matched and free rows at
        # the band's MIDDLE must be identical, which is the self-check: at the clip's own
        # speed the rate is 1 and matching has to be a no-op.
        #
        # SLIDE is reported and not asserted. At the matched speed the left foot's is 0.004
        # of a leg, the measurement floor, so a ratio against it is noise; an absolute bar
        # cannot pass the right foot's transition pop, which is the same filed defect.
        # ik-slide and ik-settle already hold that ground at the matched speed; what this
        # arm adds is the BAND.
        def mean(leg, key):
            return 0.5 * sum(d[(leg, key)][:2])

        bad = []
        if abs(mean("one", "fixfrac") - mean("one", "freefixfrac")) > 1e-6:
            bad.append("matching is not a no-op at the clip's own speed")
        for leg in ("slow", "fast"):
            if mean(leg, "fixfrac") > 0.75 * mean(leg, "freefixfrac"):
                bad.append(f"{leg} corrects {mean(leg, 'fixfrac') / mean(leg, 'freefixfrac'):.2f} "
                           f"of what it does unmatched")
        for leg in legs:
            for k, foot in enumerate(("left", "right")):
                if d[(leg, "hold")][k] <= 0.3:
                    bad.append(f"the {foot} foot lets go at {leg}")
        ok = not bad
        shape = ", ".join(
            f"{leg} plays {d[(leg, 'play')][0]:.2f}x and corrects "
            f"{mean(leg, 'fixfrac') * 100.0:.1f} per cent of a leg against "
            f"{mean(leg, 'freefixfrac') * 100.0:.1f} unmatched, sliding "
            f"{d[(leg, 'slidefrac')][0] * 100.0:.1f}/{d[(leg, 'slidefrac')][1] * 100.0:.1f} "
            f"left/right over a hold of {d[(leg, 'hold')][0] * 100.0:.0f}/"
            f"{d[(leg, 'hold')][1] * 100.0:.0f} per cent of the cycle" for leg in legs)
        print(f"  ik-slide-speeds {'PASS' if ok else 'FAIL'}  across the whole band the clip can "
              f"carry: {shape} (want a quarter off the correction at each edge, no change at the "
              f"middle, and every foot still holding 30 per cent of the cycle)" +
              ("" if ok else "; " + ", ".join(bad)))
        if not ok:
            failures.append("ik-slide-speeds")

    # --- ik-scale --------------------------------------------------------------
    d = _ik_probe("scale")
    need = [(s, k) for s in ("one", "two") for k in ("hold", "slidefrac")]
    if not d or any(k not in d for k in need):
        print("  ik-scale     FAIL  the probe failed or measured nothing")
        failures.append("ik-scale")
    else:
        h1, h2 = d[("one", "hold")][0], d[("two", "hold")][0]
        s1, s2 = d[("one", "slidefrac")][0], d[("two", "slidefrac")][0]
        # Every threshold the solver owns is in MODEL space and a fraction of the leg, so
        # a scale on the rig's node must change nothing it decides. This is the blind spot
        # spec 12.9 shipped a live bug through -- a WORLD distance compared against a model
        # threshold, which at gametest's PLAYER_SCALE 2 made the documented unlock distance
        # behave as half of itself -- and no arm could see it, because every other case in
        # this probe runs the rig at scale 1. Re-introducing that exact bug takes the slide
        # at scale two from 0.0037 to 0.0121 and leaves scale one untouched.
        ok = abs(h1 - h2) < 1e-4 and abs(s1 - s2) < 1e-5
        print(f"  ik-scale     {'PASS' if ok else 'FAIL'}  on a node scaled 1 and 2 the same "
              f"walk holds {h1 * 100.0:.0f} and {h2 * 100.0:.0f} per cent of the cycle and "
              f"slides {s1:.6f} and {s2:.6f} of a leg (want equal: a scale reaches the world "
              f"matrix and nothing the solver decides)")
        if not ok:
            failures.append("ik-scale")

    return failures
