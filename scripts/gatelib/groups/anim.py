"""The anim group: animation blending (a blend space, a crossfade, a masked layer, spec 12.1)."""

import json
import math
import re
import os
import subprocess

from gatelib.harness import GAMETEST, RENDER, asset, render
from gatelib.images import compare
from gatelib.probes import GAMETEST_TRACE_EVERY, _gametest_probe, _ik_probe


PUPPET = "assets/scenes/puppet.cscn"

# "anim <case> <label> <key> <numbers...>" from gametest --anim-probe. The events
# case also prints a non-numeric "fired <name> <tick>" line, which this
# deliberately does not match: the counts are what the arms read.
_ANIM_PROBE = re.compile(r"^anim ([\w-]+) (\w+) (\w+)((?:\s+-?[\d.]+)+)$", re.M)
# The trace's POSITION and the character's own heading, for the two arms that ask where
# the player actually went (spec 12.18). Separate passes over the same line, the rule
# this file already follows for vel and cam: five arms index _GAMETEST_TRACE's three
# groups positionally and adding captures would renumber every one of them.
_GAMETEST_POS = re.compile(r"player step (\d+) .*? pos\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)")
_GAMETEST_YAW = re.compile(r"player step (\d+) .* yaw (-?[\d.]+)")
# Land first: the player spawns above the floor and a move fired in the air is a move
# the medium machine takes the source back from. Then the lunge, settle, then the spin.
_MOVES_SCRIPT = "0-79 idle\n80-82 rt=1\n83-159 idle\n160-162 lt=1\n163-250 idle\n"
# The steps the two arms read: before and after each move, on the trace's own cadence.
_MOVES_STEPS = (80, 150, 160, 250)
# The lunge's own shape, from gametest.c's animator_play_once call and the generator's
# clip. Here so the shortfall the arm allows is derived from the fade rather than chosen.
_LUNGE_FADE = 0.08
_LUNGE_SECONDS = 0.6


_LOCO_AXIS = re.compile(r"Locomotion travels 0 to ([\d.]+) m/s")


def _gametest_moves(workdir, tag, extra):
    """{step: (x, z)}, {step: yaw} and the app's own axis top from one run of the
    authored-move script."""
    path = os.path.join(workdir, f"moves_{tag}.txt")
    with open(path, "w") as f:
        f.write(_MOVES_SCRIPT)
    # --no-crates, and it is what lets these two arms read POSITION at all. The five
    # falling boxes land at rand() positions inside a 20-unit box centred on the
    # player, and rand() differs per platform's libc -- so one that lands beside him
    # here lands on him elsewhere, which is exactly why _gametest_pad_run's own
    # docstring reads the commanded move instead. Without the crates the scene is the
    # plate, and a displacement is a portable number.
    r = subprocess.run(
        [GAMETEST, "-x", "-f", "260", "--no-crates", "--trace-player", "--trace-every",
         str(GAMETEST_TRACE_EVERY), "--pad-script", path] + extra,
        capture_output=True, text=True)
    text = r.stdout + r.stderr
    pos = {int(s): (float(x), float(z)) for s, x, _y, z in _GAMETEST_POS.findall(text)}
    yaw = {int(s): float(v) for s, v in _GAMETEST_YAW.findall(text)}
    axis = _LOCO_AXIS.findall(text)
    # Every step the arms index, checked here rather than at the read: a KeyError in an
    # arm takes the whole group down, where a None fails the two arms it belongs to.
    if r.returncode != 0 or any(s not in pos or s not in yaw for s in _MOVES_STEPS):
        return None
    return {"pos": pos, "yaw": yaw, "axis": float(axis[0]) if axis else 0.0}

# "anim-probe frame <n> bone <i> rot <x y z w> pos <x y z> name <name>" from
# render --anim-probe. The NAME is last because a rig may space its bone names,
# so everything after "name " is one.
_ANIM_BONE = re.compile(r"^anim-probe frame (\d+) bone \d+ rot "
                        r"(-?[\d.eE+-]+) (-?[\d.eE+-]+) (-?[\d.eE+-]+) (-?[\d.eE+-]+) "
                        r"pos (-?[\d.eE+-]+) (-?[\d.eE+-]+) (-?[\d.eE+-]+) name (.+)$", re.M)

# "(<n> channels, <n> matched, <n> retargeted)" from the loader's own per-clip
# summary. The third number is the one that used to be a copy of the second: it
# counted the flag that substitutes a bind-pose position rather than corrections
# actually computed, so a load that reconciled nothing claimed it had.
_ANIM_RETARGET = re.compile(r"Extracted animation '([\w-]+)':[^\n]*?"
                            r"(\d+) matched, (\d+) retargeted")

# The player trace's animation tail (spec 12.1), appended after `jump <d>`: the
# knob, the locomotion space's three weights, the crossfade weight, the override
# weight and the base source's name. APPENDED rather than inserted, so
# _GAMETEST_TRACE above still matches the same line.
_ANIM_TRACE = re.compile(r"player step (\d+) .* jump \d anim "
                         r"(-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) "
                         r"(-?[\d.]+) (\w+)")


def _anim_probe_run(case):
    """{(label, key): [floats]} from one gametest --anim-probe run, or None if it
    failed or measured nothing. The probe ticks ANIMATOR components through the
    game loop's own update_all_animators at a fixed 1/60, on a rig whose clips are
    authored in closed form -- so what it prints has a value this file can state.
    """
    return _gametest_probe("--anim-probe", _ANIM_PROBE, case)


def _anim_bone_probe(extra, frames=30):
    """({bone name: ((x, y, z, w), (x, y, z))}, log) for the LAST frame of a render
    --anim-probe run: each bone's global rotation, its global position, and the
    run's own output, which carries the loader's account of what it did.

    No screenshot and no framebuffer scaling: this reads a POSE, which the window
    size cannot reach.
    """
    cmd = [RENDER, "-m", PUPPET, "-x", "-f", str(frames), "-W", "400", "-H", "300",
           "--anim-probe"] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    log = r.stdout + r.stderr
    hits = _ANIM_BONE.findall(log)
    if r.returncode != 0 or not hits:
        return None, log
    last = max(int(h[0]) for h in hits)
    return ({name: ((float(x), float(y), float(z), float(w)),
                    (float(px), float(py), float(pz)))
             for f, x, y, z, w, px, py, pz, name in hits if int(f) == last}, log)


def _anim_bone_pose(extra, frames=30):
    """{bone name: (x, y, z, w)} -- the rotation half of _anim_bone_probe."""
    bones, _ = _anim_bone_probe(extra, frames)
    return None if bones is None else {n: rot for n, (rot, _) in bones.items()}


def _anim_render(workdir, tag, extra, frames=30):
    """One puppet capture at the shared size; its path, or None."""
    out = os.path.join(workdir, "anim_%s.ppm" % tag)
    return None if render(PUPPET, out, extra, frames=frames) else out


def _anim_frames(workdir, tag, extra, frames):
    """The same with every frame kept, for an arm that reads a transition."""
    out = os.path.join(workdir, "anim_%s.ppm" % tag)
    if render(PUPPET, out, extra + ["--screenshot-every", "1"], frames=frames):
        return None
    return out


def _anim_frame(base, n):
    return base[:-4] + "_%06d.ppm" % n


def run_anim_gate(workdir):
    """The animation blend layer (spec 12.1), on a generated puppet whose clips are
    authored in closed form -- so every bar below is a number this file states
    rather than a frame somebody once looked at. Half the arms read PIXELS through
    the render app; half read poses, weights and events through gametest's
    --anim-probe, which drives ANIMATOR components with the game loop's own
    update_all_animators.

      anim-endpoints   a blend space parked on an entry is that clip ALONE, to the
                       pixel, at both ends. Walk and run must also differ, or the
                       arm would pass on any two identical frames.
      anim-midpoint    blending the bind pose with a 90-degree rotation reads
                       2*atan2(t*sin45, (1-t)+t*cos45): 21.59/45.00/68.41 degrees at
                       t = .25/.5/.75. Slerp would read 22.5/45/67.5, so the QUARTER
                       points are what tell the two apart -- the midpoint alone
                       cannot, both being 45.
      anim-crossfade   before the switch the frame is the outgoing clip alone; after
                       the fade it is the incoming clip alone, bit for bit, as though
                       snapped at the switch frame; mid-fade it is neither. A snap, a
                       fade that never settles, and an incoming clock started at the
                       fade's END each fail a different one of those three.
      anim-layer       the override moves the frame it is masked onto.
      anim-layer-release a one-shot layer fades itself out, and the frame returns to
                       the base EXACTLY.
      anim-weights     the space's weights sum to 1, read (1,0,0)/(0,1,0)/(0,0,1) on
                       the three entries, split evenly between them, and clamp past
                       both ends.
      anim-fade        the crossfade envelope is 0 at the switch, exactly half at half
                       the duration and exactly 1 when it settles -- and the settled
                       pose equals the incoming clip played alone, to the bit.
      anim-mask        a masked bone moves; an unmasked one does not move AT ALL (0
                       exactly, not 0 to a tolerance), so a mask that leaks up the
                       parent chain fails. Released, the pose is the base's again.
      anim-two-rigs    two ANIMATOR entities ticked together hold different poses, and
                       the one playing a clip equals that clip played alone: the pose
                       is per node, not per process.
      anim-phase       walk and run under one knob share ONE phase, advancing at the
                       weighted mean of their cycle rates -- independent clocks would
                       read 1/3 and 2/3 where this reads one number.
      anim-events      a clip's events fire twice a cycle, twice as often at twice the
                       rate, once per plant when two entries are mixed (never doubled)
                       and never from a clip carrying none.
      anim-clip-loads  the committed walk clip binds every one of the puppet's bones
                       by exact name.
      anim-stride      the walk clip's own feet imply a ground speed, and the generated
                       pendulum walk's REFUSE to. The number is crossed against the ik
                       probe's independent fit, which is a different method for the same
                       quantity -- a median over every sample the ankle spent low, against
                       a least-squares fit over one contiguous window -- so agreeing is
                       evidence rather than proof that one was copied from the other.
      anim-rate        what a blend SPACE implies, which is not the weighted mean of its
                       entries' strides: the space keeps one clock, so the pose lays down
                       sum(w*stride*seconds) of ground per turn of the loop while the
                       clock turns it at sum(w/seconds). The gate recomputes both from the
                       inputs the probe printed. The mean is only wrong when the clips
                       differ in LENGTH, which is why these two are 2.0 s and 0.5 s.
      anim-trace-idle  the LOOP ticks the component: a standing player's trace reports
                       the idle weight at 1, which stays 0 if update_all_animators is
                       never called from the pre-render hook.
      anim-twin        a second rig playing its own clip changes the FRAME -- two poses
                       in one draw. This is the only arm that reaches the per-node pose
                       through the renderer: the probes compare bone matrices on the
                       CPU, where a process-global pose would still look right. With
                       one, both rigs would hold the same state and the two runs would
                       be identical.
      anim-retarget    a clip authored on one rig, played on another that rests
                       differently (spec 12.11). BOTH configurations are asserted,
                       because only the pair says the mechanism is live: with no source
                       rest pose a foot must sit ABOVE the hips and the loader must
                       report 0 channels corrected -- the failure, pinned -- and with
                       -s the feet must be below and every matched channel corrected.
                       The count is not redundant with the posture: disarming the
                       global path leaves the local-delta fallback correcting 15 of 22
                       and the rig still standing, which the posture bar alone reads as
                       a pass.
      anim-root-travel what a clip STATES about its own displacement (spec 12.18): the
                       travelling clips carry the distance the generator authored, spin
                       carries half a turn and no distance, and every in-place clip --
                       the generated pendulum walk and the committed strut_walk among
                       them -- REFUSES. The refusals are the half that could not be had
                       by inference, and strut_walk's is the measurement that replaced
                       one.
      anim-root-wrap   three loops of a travelling clip carry three loops of ground. The
                       clock jumping back at the seam is not the character going
                       backwards, and the correction that puts it right is out by a whole
                       loop in either direction if it is missing or doubled.
      anim-root-blend  what a MIXTURE lays down, which is not the mean of its clips'
                       speeds: one clock, so the ground per turn is sum(w*loop) and the
                       turns per second sum(w/seconds). The two part company only when
                       the clips differ in length, which is why these are 1.0 s and 0.5 s.
      anim-root-switch a cut to the start of a clip moves the pose by most of a loop and
                       the character by nothing at all. A PIN rather than a measurement:
                       a fresh source carries no reading, so the property is structural
                       and the arm below is what can go red.
      anim-root-fade   a crossfade between clips travelling at different speeds carries
                       the envelope's own integral and not a metre more. This is where
                       reading the POSE and reading the CLIPS part company -- the blended
                       root is a lerp of two curves metres apart, so a pose difference
                       hands the character that gap as if it had run.
      anim-root-yaw    a clip that turns the body half way round turns it half way round,
                       and carries no travel while doing it.
      anim-root-zeroed the pose the frame DRAWS stands at its bind position and heading
                       however far the character has been sent -- the other half of every
                       arm above, since a rig that kept the travel it gave away would move
                       twice and no pixel would say which half was wrong. Its height still
                       moves, that being the bob rather than travel.
      anim-root-lunge  the PLAYER travels what the clip states, at a rig scale this gate
                       DERIVES rather than knows -- and travels nothing at all under
                       --no-root-motion, where the clips that state a travel are not
                       loaded. The first arm here to drive the demo rather than the
                       engine, so a distance that never reaches the character is caught
                       here and in no arm above. What it does NOT catch is a scale
                       dropped from both the axis and the velocity, since it reads the
                       scale off that same axis; what it catches is one applied on one
                       path and not the other, which is the shape 12.9's bug had.
      anim-root-spin   and a clip that turns the player half way round while carrying him
                       nowhere. Two-sided the same way.
    """
    import math

    if not os.path.exists(GAMETEST) or not os.path.exists(RENDER):
        print("  anim         SKIP  (render or gametest not built)")
        return []
    failures = []

    # --- anim-endpoints --------------------------------------------------------
    ends = {t: _anim_render(workdir, t, e) for t, e in (
        ("blend0", ["--anim-space", "walk,run", "--anim-blend", "0"]),
        ("walk", ["--anim-clip", "walk"]),
        ("blend1", ["--anim-space", "walk,run", "--anim-blend", "1"]),
        ("run", ["--anim-clip", "run"]))}
    if not all(ends.values()):
        print("  anim-endpoints FAIL  a render failed")
        failures.append("anim-endpoints")
    else:
        lo, _ = compare(ends["blend0"], ends["walk"])
        hi, _ = compare(ends["blend1"], ends["run"])
        apart, _ = compare(ends["walk"], ends["run"])
        ok = lo == 0 and hi == 0 and apart > 0
        print(f"  anim-endpoints {'PASS' if ok else 'FAIL'}  param 0 vs walk {lo} px, param 1 "
              f"vs run {hi} px (want 0), walk vs run {apart} px (want > 0)")
        if not ok:
            failures.append("anim-endpoints")

    # --- anim-midpoint ---------------------------------------------------------
    def _nlerp_deg(t):
        half = math.radians(45.0)
        return math.degrees(2.0 * math.atan2(t * math.sin(half),
                                             (1.0 - t) + t * math.cos(half)))

    worst, seen = 0.0, True
    for t in (0.25, 0.5, 0.75):
        pose = _anim_bone_pose(["--anim-space", "rest,hold90", "--anim-blend", str(t)])
        if not pose or "cetra_rig:LeftForeArm" not in pose:
            seen = False
            break
        x, y, z, w = pose["cetra_rig:LeftForeArm"]
        deg = math.degrees(2.0 * math.atan2(math.sqrt(x * x + y * y + z * z), abs(w)))
        worst = max(worst, abs(deg - _nlerp_deg(t)))
    if not seen:
        print("  anim-midpoint FAIL  a probe failed or did not report the bone")
        failures.append("anim-midpoint")
    else:
        ok = worst < 0.05
        print(f"  anim-midpoint {'PASS' if ok else 'FAIL'}  worst error {worst:.4f} deg against "
              f"nlerp's {_nlerp_deg(0.25):.2f}/{_nlerp_deg(0.5):.2f}/{_nlerp_deg(0.75):.2f} "
              f"(want < 0.05; slerp would read 22.50/45.00/67.50)")
        if not ok:
            failures.append("anim-midpoint")

    # --- anim-crossfade --------------------------------------------------------
    switch = ["--anim-clip", "idle", "--anim-switch-to", "run", "--anim-switch-at", "20"]
    fading = _anim_frames(workdir, "xf_fade", switch + ["--anim-fade", "0.25"], 41)
    before = _anim_frames(workdir, "xf_before", ["--anim-clip", "idle"], 41)
    snapped = _anim_frames(workdir, "xf_snap", switch + ["--anim-fade", "0"], 41)
    if not (fading and before and snapped):
        print("  anim-crossfade FAIL  a render failed")
        failures.append("anim-crossfade")
    else:
        pre, _ = compare(_anim_frame(fading, 10), _anim_frame(before, 10))
        post, _ = compare(_anim_frame(fading, 40), _anim_frame(snapped, 40))
        mid_out, _ = compare(_anim_frame(fading, 27), _anim_frame(before, 27))
        mid_in, _ = compare(_anim_frame(fading, 27), _anim_frame(snapped, 27))
        ok = pre == 0 and post == 0 and mid_out > 0 and mid_in > 0
        print(f"  anim-crossfade {'PASS' if ok else 'FAIL'}  f10 vs outgoing {pre} px, f40 vs "
              f"snapped {post} px (want 0), f27 vs outgoing {mid_out} px and vs snapped "
              f"{mid_in} px (want both > 0)")
        if not ok:
            failures.append("anim-crossfade")

    # --- anim-layer / anim-layer-release ---------------------------------------
    over = ["--anim-clip", "idle", "--anim-layer", "wave", "--anim-mask", "cetra_rig:RightArm"]
    lay = _anim_render(workdir, "layer", over)
    base = _anim_render(workdir, "layer_base", ["--anim-clip", "idle"])
    if not (lay and base):
        print("  anim-layer   FAIL  a render failed")
        failures.append("anim-layer")
    else:
        moved, _ = compare(lay, base)
        ok = moved > 0
        print(f"  anim-layer   {'PASS' if ok else 'FAIL'}  the masked arm moves {moved} px "
              f"against the base alone (want > 0)")
        if not ok:
            failures.append("anim-layer")

    rel = _anim_render(workdir, "layer_rel", over + ["--anim-layer-once"], frames=91)
    base90 = _anim_render(workdir, "layer_base90", ["--anim-clip", "idle"], frames=91)
    if not (rel and base90):
        print("  anim-layer-release FAIL  a render failed")
        failures.append("anim-layer-release")
    else:
        left, _ = compare(rel, base90)
        ok = left == 0
        print(f"  anim-layer-release {'PASS' if ok else 'FAIL'}  {left} px left of the one-shot "
              f"once it released (want 0)")
        if not ok:
            failures.append("anim-layer-release")

    # --- anim-weights ----------------------------------------------------------
    d = _anim_probe_run("locomotion")
    want = {"p000": (1, 0, 0), "p025": (0.5, 0.5, 0), "p050": (0, 1, 0),
            "p075": (0, 0.5, 0.5), "p100": (0, 0, 1), "p150": (0, 0, 1), "pm050": (1, 0, 0)}
    if not d or any((k, "weights") not in d for k in want):
        print("  anim-weights FAIL  the probe failed or measured nothing")
        failures.append("anim-weights")
    else:
        worst = max(max(abs(a - b) for a, b in zip(d[(k, "weights")], v))
                    for k, v in want.items())
        sums = max(abs(sum(d[(k, "weights")]) - 1.0) for k in want)
        ok = worst < 1e-4 and sums < 1e-5
        print(f"  anim-weights {'PASS' if ok else 'FAIL'}  worst weight error {worst:.6f} "
              f"(want < 1e-4), worst sum error {sums:.6f} (want < 1e-5)")
        if not ok:
            failures.append("anim-weights")

    # --- anim-fade -------------------------------------------------------------
    d = _anim_probe_run("crossfade")
    need = [("t0", "fade"), ("thalf", "fade"), ("tF", "fade"), ("tpost", "settled"),
            ("pose", "maxdiff")]
    if not d or any(k not in d for k in need):
        print("  anim-fade    FAIL  the probe failed or measured nothing")
        failures.append("anim-fade")
    else:
        t0 = d[("t0", "fade")][0]
        half = d[("thalf", "fade")][0]
        full = d[("tF", "fade")][0]
        settled = d[("tpost", "settled")][0]
        same = d[("pose", "maxdiff")][0]
        ok = t0 == 0.0 and abs(half - 0.5) < 1e-3 and full == 1.0 and settled == 1 and same == 0.0
        print(f"  anim-fade    {'PASS' if ok else 'FAIL'}  envelope {t0:.3f}/{half:.3f}/"
              f"{full:.3f} (want 0/0.5/1), settled {int(settled)}, settled pose vs the clip "
              f"alone {same:.6f} (want 0 exactly)")
        if not ok:
            failures.append("anim-fade")

    # --- anim-mask -------------------------------------------------------------
    d = _anim_probe_run("layer")
    need = [("w15", "weight"), ("w15", "finished"), ("masked_out", "maxdiff"),
            ("masked_in", "maxdiff"), ("after", "weight"), ("after", "finished"),
            ("after", "maxdiff")]
    if not d or any(k not in d for k in need):
        print("  anim-mask    FAIL  the probe failed or measured nothing")
        failures.append("anim-mask")
    else:
        held = d[("w15", "weight")][0]
        out = d[("masked_out", "maxdiff")][0]
        inside = d[("masked_in", "maxdiff")][0]
        gone = d[("after", "weight")][0]
        done = d[("after", "finished")][0]
        back = d[("after", "maxdiff")][0]
        ok = (held == 1.0 and d[("w15", "finished")][0] == 0 and out == 0.0 and inside > 0.1
              and gone == 0.0 and done == 1 and back == 0.0)
        print(f"  anim-mask    {'PASS' if ok else 'FAIL'}  masked {inside:.4f} (want > 0.1), "
              f"unmasked {out:.6f} (want 0 exactly), released {int(done)} and {back:.6f} back "
              f"to the base")
        if not ok:
            failures.append("anim-mask")

    # --- anim-two-rigs ---------------------------------------------------------
    d = _anim_probe_run("two-rigs")
    if not d or ("ab", "maxdiff") not in d or ("bc", "maxdiff") not in d:
        print("  anim-two-rigs FAIL  the probe failed or measured nothing")
        failures.append("anim-two-rigs")
    else:
        ab = d[("ab", "maxdiff")][0]
        bc = d[("bc", "maxdiff")][0]
        ok = ab > 0.05 and bc == 0.0
        print(f"  anim-two-rigs {'PASS' if ok else 'FAIL'}  two rigs differ by {ab:.4f} "
              f"(want > 0.05) and the matching pair by {bc:.6f} (want 0 exactly)")
        if not ok:
            failures.append("anim-two-rigs")

    # --- anim-phase ------------------------------------------------------------
    d = _anim_probe_run("phase")
    need = [("walk", "frac"), ("run", "frac"), ("expected", "frac")]
    if not d or any(k not in d for k in need):
        print("  anim-phase   FAIL  the probe failed or measured nothing")
        failures.append("anim-phase")
    else:
        w = d[("walk", "frac")][0]
        r = d[("run", "frac")][0]
        want_frac = d[("expected", "frac")][0]
        ok = abs(w - r) < 1e-5 and abs(w - want_frac) < 1e-3
        print(f"  anim-phase   {'PASS' if ok else 'FAIL'}  walk {w:.6f} and run {r:.6f} share "
              f"one phase, against {want_frac:.6f} expected")
        if not ok:
            failures.append("anim-phase")

    # --- anim-events -----------------------------------------------------------
    d = _anim_probe_run("events")
    want_counts = {"walk": 4, "run": 8, "mixed": 6, "idle": 0}
    if not d or any((k, "count") not in d for k in want_counts):
        print("  anim-events  FAIL  the probe failed or measured nothing")
        failures.append("anim-events")
    else:
        got = {k: int(d[(k, "count")][0]) for k in want_counts}
        ok = got == want_counts
        print(f"  anim-events  {'PASS' if ok else 'FAIL'}  walk/run/mixed/idle "
              f"{got['walk']}/{got['run']}/{got['mixed']}/{got['idle']} (want 4/8/6/0)")
        if not ok:
            failures.append("anim-events")

    # --- anim-clip-loads -------------------------------------------------------
    d = _anim_probe_run("import")
    joints = 0
    try:
        with open(asset("puppet.gltf")) as f:
            doc = json.load(f)
        joints = sum(1 for j in doc["skins"][0]["joints"]
                     if doc["nodes"][j]["name"].startswith("cetra_rig:"))
    except (OSError, ValueError, KeyError, IndexError):
        joints = 0
    if not d or ("clip", "matched") not in d or not joints:
        print("  anim-clip-loads FAIL  the probe failed or the fixture could not be read")
        failures.append("anim-clip-loads")
    else:
        matched = int(d[("clip", "matched")][0])
        channels = int(d[("clip", "channels")][0])
        ok = matched == joints and channels > matched
        print(f"  anim-clip-loads {'PASS' if ok else 'FAIL'}  {matched} of the clip's "
              f"{channels} channels bound by name (want all {joints} of the rig's bones)")
        if not ok:
            failures.append("anim-clip-loads")

    # --- anim-stride -----------------------------------------------------------
    d = _anim_probe_run("stride")
    fit = _ik_probe("lock")
    need = [("walk", "answered"), ("walk", "speed"), ("walk", "dir"), ("swing", "answered")]
    if not d or any(k not in d for k in need) or not fit or ("clip", "stride") not in fit:
        print("  anim-stride  FAIL  a probe failed or measured nothing")
        failures.append("anim-stride")
    else:
        answered = d[("walk", "answered")][0]
        speed = d[("walk", "speed")][0]
        facing = math.sqrt(sum(v * v for v in d[("walk", "dir")]))
        swing = d[("swing", "answered")][0]
        left = fit[("clip", "stride")][0]
        # Two genuinely DIFFERENT methods for one quantity, which is why agreeing is
        # evidence: the engine takes the median horizontal velocity over every sample the
        # ankle spent low, the ik probe least-squares-fits the toe across one contiguous
        # window it had to find first. Two copies of one algorithm agreeing proves the
        # copying; these two agree to about 2 per cent.
        #
        # REFUSING is the other half and is not a lesser result. The generated walk swings
        # straight legs, so both feet are lowest at the same instant and swinging opposite
        # ways: their low samples carry both directions and median to nothing, and the two
        # feet disagree on which way the ground goes. An answer there would be worse than
        # none -- a window fit taken anyway lands on a swing and points the body backwards.
        ok = (answered == 1 and swing == 0 and abs(facing - 1.0) < 1e-3
              and 0.85 * left < speed < 1.1 * left)
        print(f"  anim-stride  {'PASS' if ok else 'FAIL'}  the walk clip implies "
              f"{speed:.6f} model units per second against the ik probe's independent "
              f"left-foot fit of {left:.6f}, {speed / left * 100.0:.1f} per cent of it (want 85 "
              f"to 110: a median over every low sample against a least-squares fit over one "
              f"window), on a unit direction of {facing:.6f}; the generated pendulum walk "
              f"{'refuses' if swing == 0 else 'ANSWERED'} (want refused: it has no stance to "
              f"measure)")
        if not ok:
            failures.append("anim-stride")

    # --- anim-rate -------------------------------------------------------------
    d = _anim_probe_run("rate")
    need = [("single", "engine"), ("single", "stride"), ("pair", "seconds"),
            ("pair", "strides"), ("pair", "param"), ("pair", "engine"), ("still", "engine"),
            ("blind", "engine")]
    if not d or any(k not in d for k in need) or len(d[("pair", "seconds")]) < 2:
        print("  anim-rate    FAIL  the probe failed or measured nothing")
        failures.append("anim-rate")
    else:
        sec = d[("pair", "seconds")]
        stride = d[("pair", "strides")]
        # The weights the space would give, recomputed here rather than read back: two
        # entries at 0 and 1 with the knob between them are a plain lerp.
        t = d[("pair", "param")][0]
        w = [1.0 - t, t]
        # The space keeps ONE clock. The blended pose lays down sum(w * stride * seconds)
        # of ground per turn of the loop, and the clock turns the loop at sum(w / seconds)
        # -- so the answer is the PRODUCT. The mean of the strides is the plausible wrong
        # expression, and it is only wrong when the clips differ in length, which is why
        # these two are 2.0 s and 0.5 s.
        def blend(strides):
            return (sum(w[i] * strides[i] * sec[i] for i in (0, 1))
                    * sum(w[i] / sec[i] for i in (0, 1)))

        want = blend(stride)
        mean = sum(w[i] * stride[i] for i in (0, 1))
        got = d[("pair", "engine")][0]
        single, one_stride = d[("single", "engine")][0], d[("single", "stride")][0]
        # A strideless entry contributes no ground and still turns the clock, so the answer
        # is the same expression with that term's stride at 0 -- lower than the pair's, and
        # emphatically not zero. That is what lets an idle sit at the bottom of a speed
        # axis. A space where NOTHING was measured is the different case and answers 0.
        still, still_want = d[("still", "engine")][0], blend([stride[0], 0.0])
        blind = d[("blind", "engine")][0]
        ok = (abs(got - want) < 1e-4 and abs(got - mean) > 1e-3
              and abs(single - one_stride) < 1e-6 and abs(still - still_want) < 1e-4
              and blind == 0.0)
        print(f"  anim-rate    {'PASS' if ok else 'FAIL'}  a {sec[0]:.1f}s clip striding "
              f"{stride[0]:.1f} blended half and half with a {sec[1]:.1f}s one striding "
              f"{stride[1]:.1f} implies {got:.6f} (want {want:.6f}, and NOT the weighted "
              f"mean's {mean:.6f}); one entry reports its own {single:.6f}, a standing "
              f"partner pulls it to {still:.6f} (want {still_want:.6f}) and a space nobody "
              f"measured answers {blind:.1f}")
        if not ok:
            failures.append("anim-rate")

    # --- anim-trace-idle -------------------------------------------------------
    r = subprocess.run([GAMETEST, "-x", "-f", "120", "--trace-player", "--trace-every", "20"],
                       capture_output=True, text=True)
    rows = {int(m[0]): m for m in _ANIM_TRACE.findall(r.stdout + r.stderr)}
    late = [m for step, m in rows.items() if step >= 40]
    if r.returncode != 0 or not late:
        print("  anim-trace-idle FAIL  the run failed or traced no animation tail")
        failures.append("anim-trace-idle")
    else:
        idle_w = min(float(m[2]) for m in late)
        srcs = {m[8] for m in late}
        ok = abs(idle_w - 1.0) < 1e-3 and srcs == {"locomotion"}
        print(f"  anim-trace-idle {'PASS' if ok else 'FAIL'}  a standing player reports idle "
              f"weight {idle_w:.3f} (want 1) from {sorted(srcs)}")
        if not ok:
            failures.append("anim-trace-idle")

    # --- anim-twin -------------------------------------------------------------
    twins = {}
    for clip in ("idle", "run"):
        out = os.path.join(workdir, "anim_twin_%s.ppm" % clip)
        r = subprocess.run([GAMETEST, "-x", "-f", "8", "--twin", clip, "-S", out],
                           capture_output=True, text=True)
        twins[clip] = out if r.returncode == 0 and os.path.exists(out) else None
    if not all(twins.values()):
        print("  anim-twin    FAIL  a run failed")
        failures.append("anim-twin")
    else:
        # Eight frames: the spawned boxes are still above the top edge from this
        # camera, so their rand() positions cannot reach the comparison.
        moved, _ = compare(twins["idle"], twins["run"])
        ok = moved >= 20
        print(f"  anim-twin    {'PASS' if ok else 'FAIL'}  a second rig on its own clip moves "
              f"{moved} px (want >= 20); on a process-global pose both rigs would hold one "
              f"state and this would be 0")
        if not ok:
            failures.append("anim-twin")

    # --- anim-retarget ---------------------------------------------------------
    # A clip authored on one rig played on another whose rest pose differs -- which
    # the committed corpus already is: strut_walk is authored on t_pose.fbx, and the
    # generated puppet shares its bone NAMES while resting in pure translation. So
    # every channel binds by exact name, and every one of them is wrong.
    #
    # Both halves are asserted, because only the pair says the mechanism is live:
    # with no source rest pose the load must be visibly broken, and with one it must
    # stand up. The measure is a foot's global height against the hips'. It is
    # rig-agnostic, it is what a person sees, and it cannot be met by a pose nobody
    # could stand in -- an uncorrected clip does not lean, it folds the legs over the
    # head. A per-bone quaternion bar would need a rig-specific expectation and would
    # pass on exactly that.
    #
    # The COUNT is the other half and is not redundant: disarming use_global_retarget
    # leaves the local-delta fallback correcting 15 of the 22 channels, and the rig
    # still stands -- the higher foot lands 0.607 m below the hips against the whole
    # path's 0.722. The posture test alone reads that regression as a pass.
    clip = ["-a", asset("strut_walk.fbx")]
    blind, blind_log = _anim_bone_probe(clip)
    fixed, fixed_log = _anim_bone_probe(clip + ["-s", asset("t_pose.fbx")])
    feet = ("cetra_rig:LeftFoot", "cetra_rig:RightFoot")
    hips = "cetra_rig:Hips"
    need = feet + (hips,)
    if not blind or not fixed or any(b not in blind or b not in fixed for b in need):
        print("  anim-retarget FAIL  a probe failed or did not pose the legs")
        failures.append("anim-retarget")
    else:
        def counted(log):
            hit = [m for m in _ANIM_RETARGET.findall(log) if m[0] == "strut_walk"]
            return (int(hit[0][1]), int(hit[0][2])) if hit else (0, 0)

        def above_hips(bones):
            """How far the HIGHER foot sits above the hips, in metres. Negative is
            a rig standing on its feet."""
            return max(bones[b][1][1] for b in feet) - bones[hips][1][1]

        blind_matched, blind_corrected = counted(blind_log)
        _, fixed_corrected = counted(fixed_log)
        blind_high = above_hips(blind)
        fixed_high = above_hips(fixed)
        ok = (blind_high > 0.0 and fixed_high < 0.0
              and blind_corrected == 0 and fixed_corrected == blind_matched > 0)
        print(f"  anim-retarget {'PASS' if ok else 'FAIL'}  with no source rest pose the "
              f"highest foot sits {blind_high:+.3f} m relative to the hips and the loader "
              f"reports {blind_corrected} of {blind_matched} channels retargeted (want a "
              f"foot ABOVE the hips and 0 -- the defect, pinned); with -s the same foot "
              f"sits {fixed_high:+.3f} m and {fixed_corrected} are retargeted (want below, "
              f"and all {blind_matched})")
        if not ok:
            failures.append("anim-retarget")

    # --- anim-root-travel ------------------------------------------------------
    # What a clip STATES about its own displacement, against what the generator
    # authored. The distances are declared in gen_puppet_fixture.py and asserted
    # there; this arm is the other end of the same statement.
    #
    # The refusals are the half that could not be had by inference. Before this arm
    # the claim that the committed FBX clips are in place rested on anim-stride
    # passing at all -- a clip carrying root motion has a near-stationary stance toe
    # and would be refused by the stride measurement -- which is a deduction from an
    # absence. strut is the same question asked directly.
    stated = _anim_probe_run("root")
    want_travel = {"travel_walk": 1.20, "travel_run": 1.60, "lunge": 1.20}
    # Every committed FBX clip the app can load, not one of them. Four documents said
    # the committed corpus is in place; asking all eight found that two are not --
    # touch_down and jump_start each carry about 9 cm of hip displacement and a small
    # turn, a crouch and a rise rather than a character going anywhere. The game never
    # sees it (they load RETARGETED, which the read refuses), but the sentence in four
    # files was wrong and one measurement could not have said so.
    refuse = ("idle", "walk", "strut_walk", "steady_run", "quiet_idle", "swim_cycle",
              "float_idle", "fall_cycle")
    posed = ("touch_down", "jump_start")
    need = [(k, "travel") for k in list(want_travel) + ["spin"] + list(refuse) + list(posed)]
    if not stated or any(k not in stated for k in need):
        print("  anim-root-travel FAIL  the probe failed or measured nothing")
        failures.append("anim-root-travel")
    else:
        # answered, dx, dy, dz, yaw
        worst = max(abs(stated[(k, "travel")][3] - v) for k, v in want_travel.items())
        skew = max(max(abs(stated[(k, "travel")][1]), abs(stated[(k, "travel")][4]))
                   for k in want_travel)
        answered = all(stated[(k, "travel")][0] == 1.0
                       for k in list(want_travel) + ["spin"])
        spun = abs(abs(stated[("spin", "travel")][4]) - math.pi)
        spin_still = max(abs(stated[("spin", "travel")][i]) for i in (1, 3))
        refused = [k for k in refuse if stated[(k, "travel")][0] != 0.0]
        # The two that do state something state a POSE difference, not a stride: a
        # bound on it is what keeps this an observation rather than a licence.
        posed_far = max(math.hypot(stated[(k, "travel")][1], stated[(k, "travel")][3])
                        for k in posed)
        ok = (answered and not refused and worst < 1e-4 and skew < 1e-4
              and spun < 1e-4 and spin_still < 1e-4 and posed_far < 0.15)
        print(f"  anim-root-travel {'PASS' if ok else 'FAIL'}  the travelling clips state "
              f"their distance to {worst:.6f} m (want < 1e-4) with {skew:.6f} m of drift "
              f"off the axis, spin states {stated[('spin', 'travel')][4]:+.4f} rad "
              f"(want +-pi) and travels {spin_still:.6f} m; {len(refuse)} committed clips "
              f"refuse{'' if not refused else ' -- except ' + ', '.join(refused)}, and the "
              f"landing and the rise carry {posed_far:.3f} m of hip between their ends "
              f"(want < 0.15: a crouch, not a stride, and retargeting hides it from the game)")
        if not ok:
            failures.append("anim-root-travel")

    # --- anim-root-wrap / blend / switch / fade / yaw / zeroed ------------------
    # What the animator hands a character, against what the clips state.
    #
    # Every expectation below is the WHOLE distance, exactly. It was one update short
    # for a while, because the extraction stored each entry's previous reading and a
    # new source had none -- until that reading turned out to be computable from the
    # clock the space already keeps. The arithmetic here is what noticed: an arm
    # carrying a "- dt" it cannot explain is an arm describing an artefact.
    d = _anim_probe_run("rootmotion")
    dt = 1.0 / 60.0
    # Seeded before the branch that fills them, because the two arms AFTER this block
    # read them: a probe failure there would otherwise raise NameError and take the
    # whole group down instead of failing the arms it belongs to.
    walk_s = run_s = walk_loop = run_loop = None
    rows = [("wrap", "travelled"), ("blend", "travelled"), ("blend", "seconds"),
            ("blend", "loops"), ("switch", "travelled"), ("fade", "travelled"),
            ("fade", "shape"), ("spin", "turned"), ("spin", "travelled"),
            ("pinned", "offset"), ("pinned", "yaw"), ("spun", "yaw")]
    if not d or any(k not in d for k in rows):
        for arm in ("anim-root-wrap", "anim-root-blend", "anim-root-switch", "anim-root-fade",
                    "anim-root-yaw", "anim-root-zeroed"):
            print(f"  {arm} FAIL  the probe failed or measured nothing")
            failures.append(arm)
    else:
        walk_s, run_s, blend_s = d[("blend", "seconds")]
        walk_loop, run_loop = d[("blend", "loops")]

        # --- anim-root-wrap ----------------------------------------------------
        # Three loops carry three loops of ground, to the millimetre. A missing wrap
        # correction loses one loop per crossing; one applied twice gains one. Either
        # is 1.2 m away from this bar, so the tolerance is tight enough to also catch
        # a single dropped update.
        want = walk_loop * 3.0
        got = d[("wrap", "travelled")]
        err = abs(got[2] - want)
        ok = err < 1e-3 and abs(got[0]) < 1e-4
        print(f"  anim-root-wrap {'PASS' if ok else 'FAIL'}  three loops carried "
              f"{got[2]:.4f} m against the {want:.4f} m stated (want within 1e-3; a dropped "
              f"seam would read {want - walk_loop:.4f} and a doubled one "
              f"{want + walk_loop:.4f})")
        if not ok:
            failures.append("anim-root-wrap")

        # --- anim-root-blend ---------------------------------------------------
        # A mixture's travel is not the mean of its clips' speeds. The space keeps
        # ONE clock, so the pose lays down sum(w*loop) of ground per turn of it while
        # the clock turns at sum(w/seconds) -- and the two part company exactly when
        # the clips differ in LENGTH, which is why these are 1.0 s and 0.5 s.
        ground = 0.5 * walk_loop + 0.5 * run_loop
        rate = 0.5 / walk_s + 0.5 / run_s
        want = ground * rate * blend_s
        mean = 0.5 * (walk_loop / walk_s) + 0.5 * (run_loop / run_s)
        naive = mean * blend_s
        got = d[("blend", "travelled")]
        ok = abs(got[2] - want) < 1e-3 and abs(got[2] - naive) > 1e-2
        print(f"  anim-root-blend {'PASS' if ok else 'FAIL'}  half way between a {walk_s:.1f}s "
              f"clip carrying {walk_loop:.2f} m and a {run_s:.1f}s one carrying {run_loop:.2f} m, "
              f"{blend_s:.1f}s of it travelled {got[2]:.4f} m (want {want:.4f}, and NOT the mean "
              f"of the two speeds at {naive:.4f})")
        if not ok:
            failures.append("anim-root-blend")

        # --- anim-root-switch --------------------------------------------------
        # A cut mid-loop back to the start of a clip moves the POSE by most of a loop
        # and the character by one update's walk. This arm reads that one update.
        #
        # It used to expect zero and could not fail: a fresh source carried no reading
        # to difference, so the property held structurally. Computing both ends from
        # the clock made it a measurement -- the update after a cut is now a real
        # quantity, and a reading that leaked the jump reads sixty times it.
        want = (walk_loop / walk_s) * dt
        got = d[("switch", "travelled")]
        ok = abs(got[2] - want) < 1e-4 and abs(got[0]) < 1e-4
        print(f"  anim-root-switch {'PASS' if ok else 'FAIL'}  the update after a cut carried "
              f"{got[2]:.6f} m against the {want:.6f} one update of this clip is worth (want "
              f"within 1e-4; a reading that differenced the poses across the cut would carry "
              f"most of a loop)")
        if not ok:
            failures.append("anim-root-switch")

        # --- anim-root-fade ----------------------------------------------------
        # A crossfade between clips that travel at different speeds, which is where
        # reading the POSE and reading the clips part company: over the fade the
        # blended root is a lerp of two curves metres apart, so a pose difference
        # hands the character that gap on top of the travel. Per-entry readings
        # cannot see it.
        #
        # The bar is the discrete sum the animator actually walks -- the envelope
        # sampled once per update, including the +1e-6 the engine settles the fade
        # with -- rather than the integral, which is 0.017 m away and would need a
        # tolerance wide enough to hide a real error.
        fade_s, win_s, tick = d[("fade", "shape")]
        v_walk, v_run = walk_loop / walk_s, run_loop / run_s
        want = 0.0
        for i in range(1, int(round(win_s / tick)) + 1):
            elapsed = i * tick
            w = 1.0 if elapsed + 1e-6 >= fade_s else elapsed / fade_s
            want += tick * ((1.0 - w) * v_walk + w * v_run)
        got = d[("fade", "travelled")]
        lo, hi = v_walk * win_s, v_run * win_s
        ok = abs(got[2] - want) < 1e-3 and lo < got[2] < hi
        print(f"  anim-root-fade {'PASS' if ok else 'FAIL'}  {win_s:.1f}s across a {fade_s:.1f}s "
              f"fade from {v_walk:.2f} to {v_run:.2f} m/s carried {got[2]:.4f} m (want "
              f"{want:.4f}, and strictly between {lo:.2f} and {hi:.2f})")
        if not ok:
            failures.append("anim-root-fade")

        # --- anim-root-yaw -----------------------------------------------------
        turned = d[("spin", "turned")][0]
        spun_travel = max(abs(v) for v in d[("spin", "travelled")])
        ok = abs(turned - math.pi) < 1e-4 and spun_travel < 1e-4
        print(f"  anim-root-yaw {'PASS' if ok else 'FAIL'}  half a turn came out "
              f"{turned:.6f} rad against pi, carrying {spun_travel:.6f} m of travel (want "
              f"none: a clip may turn without going anywhere)")
        if not ok:
            failures.append("anim-root-yaw")

        # --- anim-root-zeroed --------------------------------------------------
        # The pose the frame draws, after three loops that sent the character 3.6 m
        # and a turn that sent it half way round. A rig that kept either would move
        # twice -- once as a body and once as a mesh sliding off its own capsule --
        # and nothing in the pixels would say which half was wrong.
        off = d[("pinned", "offset")]
        flat = math.hypot(off[0], off[2])
        yaw = abs(d[("pinned", "yaw")][0])
        spun_yaw = abs(d[("spun", "yaw")][0])
        ok = flat < 1e-6 and yaw < 1e-6 and spun_yaw < 1e-6 and abs(off[1]) > 1e-6
        print(f"  anim-root-zeroed {'PASS' if ok else 'FAIL'}  the drawn root stands "
              f"{flat:.6f} m from its bind position at {yaw:.6f} rad, and the spun rig at "
              f"{spun_yaw:.6f} rad (want 0 for all three); its height still moves by "
              f"{off[1]:+.6f} m, which is the bob and not travel")
        if not ok:
            failures.append("anim-root-zeroed")

    # --- anim-root-lunge / anim-root-spin --------------------------------------
    # The two arms that ask where the PLAYER went, rather than what the animator
    # handed out. Everything above measures the engine with the app absent; these
    # drive the demo with a scripted pad and read its own trace, so a travel that
    # never reaches the character -- drained and dropped, scaled wrong, turned by
    # the wrong yaw -- is caught here and nowhere else.
    #
    # Both are two-sided against --no-root-motion, where the clips that state a
    # travel are not loaded at all: the same script then presses the same triggers
    # and the character neither moves nor turns.
    rooted = _gametest_moves(workdir, "rooted", [])
    inplace = _gametest_moves(workdir, "inplace", ["--no-root-motion"])
    lunge_row = stated.get(("lunge", "travel")) if stated else None
    if (not rooted or not inplace or not lunge_row or run_loop is None
            or rooted["axis"] <= 0.0):
        for arm in ("anim-root-lunge", "anim-root-spin"):
            print(f"  {arm} FAIL  a run failed or the probe measured nothing")
            failures.append(arm)
    else:
        def moved(run, a, b):
            (x0, z0), (x1, z1) = run["pos"][a], run["pos"][b]
            return math.hypot(x1 - x0, z1 - z0)

        # --- anim-root-lunge ---------------------------------------------------
        # The world distance the clip states, DERIVED rather than written down. The
        # app prints its axis top in world metres per second and the probe states the
        # run's travel in model units, so the ratio of the two is the rig's scale and
        # this gate never has to know PLAYER_SCALE. It also means a scale applied on
        # one path and not the other -- 12.9's live bug, and what 12.10's fourth phase
        # exists for -- comes out here as a distance that is not the clip's.
        scale = rooted["axis"] / (run_loop / run_s)
        want = lunge_row[3] * scale
        got = moved(rooted, 80, 150)
        still = moved(inplace, 80, 150)
        # The shortfall is DERIVED and has exactly two terms, both of them the engine's
        # stated behaviour rather than slack:
        #
        #   the FADE -- the one-shot fades in over _LUNGE_FADE, and across it the pose
        #   is a blend, so the clip lays down only the envelope's share of its own
        #   speed: a triangle, half the fade at the lunge's metres per second;
        #
        #   the RESUME -- a source switch drops whatever the caller has not yet taken,
        #   which is what stops a character that stopped draining being handed its
        #   whole hoard in one step on landing. The drain runs once per fixed step, so
        #   at most one update of travel is pending when the one-shot ends.
        #
        # Anything more than those two is travel that went missing, and the bar says so.
        speed = (lunge_row[3] / _LUNGE_SECONDS) * scale
        lost = (0.5 * _LUNGE_FADE + dt) * speed
        floor = want - lost
        ok = floor < got < want and still < 1e-3
        print(f"  anim-root-lunge {'PASS' if ok else 'FAIL'}  the lunge carried the player "
              f"{got:.3f} m of the {want:.2f} it states at this rig's derived scale of "
              f"{scale:.1f} (want above {floor:.3f}: the {_LUNGE_FADE:.2f}s fade-in and one "
              f"undrained update account for {lost:.3f} m and nothing else may go missing), "
              f"and {still:.3f} m under --no-root-motion (want 0)")
        if not ok:
            failures.append("anim-root-lunge")

        # --- anim-root-spin ----------------------------------------------------
        # A turn that carries no ground. The position bar is half the arm: a spin that
        # moved the character would still turn it by the right angle.
        turned = abs(rooted["yaw"][250] - rooted["yaw"][150])
        held = abs(inplace["yaw"][250] - inplace["yaw"][150])
        drifted = moved(rooted, 160, 250)
        # The same two terms in radians, at the spin's own rate of pi per second.
        spin_lost = (0.5 * _LUNGE_FADE + dt) * math.pi
        ok = math.pi - spin_lost < turned < math.pi + 1e-3 and drifted < 1e-3 and held < 1e-3
        print(f"  anim-root-spin {'PASS' if ok else 'FAIL'}  the spin turned the player "
              f"{turned:.3f} rad of the {math.pi:.3f} it states, carrying {drifted:.3f} m "
              f"(want none), and turned {held:.3f} under --no-root-motion (want 0)")
        if not ok:
            failures.append("anim-root-spin")

    return failures
