"""The gamepad group: gamepad input (a scripted pad through the action layer, spec 11.109)."""

import math
import re
import os
import subprocess

from gatelib.harness import GAMETEST
from gatelib.probes import GAMETEST_TRACE_EVERY


# The step, its ground state, and the move_x the step acted on, every
# GAMETEST_TRACE_EVERY steps.
_GAMETEST_TRACE = re.compile(
    r"player step (\d+) t=\s*[\d.]+ pos(?:\s+-?[\d.]+){3}\s+vel(?:\s+-?[\d.]+){3}\s+"
    r"grounded (\d)\s+move\s+(-?[\d.]+)\s+-?[\d.]+ jump \d")
_GAMETEST_BINDING = re.compile(r"^([a-z_]+)\s+(.*)$", re.M)
# The same trace line's horizontal velocity (x and z, skipping y) and the follow
# camera's heading, each read on its own so _GAMETEST_TRACE's group numbering --
# which five arms index positionally -- stays where it is.
_GAMETEST_VEL = re.compile(
    r"player step (\d+) t=\s*[\d.]+ pos(?:\s+-?[\d.]+){3}\s+"
    r"vel\s+(-?[\d.]+)\s+-?[\d.]+\s+(-?[\d.]+)")
_GAMETEST_CAM = re.compile(r"player step (\d+) .* cam (-?[\d.]+)")


def _gametest_pad_run(workdir, tag, script, frames):
    """One scripted-pad gametest run: {step: (move_x, grounded)} from its trace and
    its Jump! count; None when the run failed or traced nothing.

    Headless the engine hands the loop its fixed dt, so a frame is one step and the
    script's frame numbers are the trace's step numbers. The MOVE column is what the
    arms read -- the action value the app commanded from -- and not the position:
    five boxes fall at rand() positions, which differ per platform's libc, and a leg
    that walks into one on Linux would fail a displacement there and nowhere else.
    """
    path = os.path.join(workdir, f"pad_{tag}.txt")
    with open(path, "w") as f:
        f.write(script)
    r = subprocess.run(
        [GAMETEST, "-x", "-f", str(frames), "--trace-player", "--trace-every",
         str(GAMETEST_TRACE_EVERY), "--pad-script", path],
        capture_output=True, text=True)
    text = r.stdout + r.stderr
    samples = _GAMETEST_TRACE.findall(text)
    if r.returncode != 0 or not samples:
        return None
    steps = {int(step): (float(mx), g == "1") for step, g, mx in samples}
    # The velocity and the camera heading off the SAME line, through a second pass
    # rather than by widening _GAMETEST_TRACE: five arms read that regex's three
    # groups positionally, and adding captures to it would renumber every one of
    # them to serve the one arm that wants these columns.
    vel = {int(s): (float(vx), float(vz)) for s, vx, vz in _GAMETEST_VEL.findall(text)}
    cam = {int(s): float(c) for s, c in _GAMETEST_CAM.findall(text)}
    return {"steps": steps, "vel": vel, "cam": cam, "jumps": text.count("Jump!")}


def _leg(run, first, last):
    """The move_x values a leg's samples carry, or None if one is missing."""
    wanted = range(first, last + 1, GAMETEST_TRACE_EVERY)
    if any(s not in run["steps"] for s in wanted):
        return None
    return [run["steps"][s][0] for s in wanted]


def run_gamepad_gate(workdir):
    """The game layer's gamepad path above the reader seam, driven by a scripted pad
    (spec 11.109). No controller is involved: the scripted reader fills the same
    struct GLFW would, and the layer, the action table and the app are what is
    verified.

      pad-edge       A held for 90 frames is ONE jump. Level-triggered would jump
                     again on re-landing with the button still down; a per-step edge
                     would see none or two.
      pad-deadzone   the stick inside the radial zone commands no move, and at 0.6
                     commands half of what full deflection does -- the rescaled zone,
                     (0.6 - 0.2) / (1 - 0.2). A ratio, so a wrong scaling fails.
      pad-dpad       the dpad commands what a full stick does, within 1%.
      pad-off        the pad disconnecting releases the stick within a step, and a
                     button already down when it reconnects is not a press.
      pad-keys-alias every action in the app's table has a key AND a pad source, read
                     from --print-bindings -- keys cannot be scripted headless, so the
                     table is where a binding that lost one half is caught.
      pad-camera-relative
                     forward means away from the LENS: turn the camera with the right
                     stick and the commanded velocity turns with it, by the angle the
                     arrows asked for and not merely by something.

    The player spawns two units up and lands by step 20; every script waits until
    frame 60 before it does anything, since a jump needs the ground.

    pad-camera-relative is the arm none of the five above could stand in for, and the
    reason is in _gametest_pad_run's own docstring: they read the MOVE column -- the action
    value the app commanded from -- and never displacement, because five boxes fall at
    rand() positions that differ per platform's libc. The action value is identical
    under both movement schemes. So when spec 12.13 changed movement from camera-
    relative to world-aligned, and 12.17 changed it back, this group was green through
    both and neither flip was visible to anything but a person playing it. Twice the
    documentation went on describing the scheme that was no longer live.
    """
    if not os.path.exists(GAMETEST):
        print("  gamepad      SKIP  (gametest not built)")
        return []
    failures = []

    # --- pad-edge ------------------------------------------------------------
    edge = _gametest_pad_run(workdir, "edge", "0-59 idle\n60-149 a\n", 240)
    if edge is None or 60 not in edge["steps"]:
        print("  pad-edge     FAIL  run failed or traced nothing")
        failures.append("pad-edge")
    else:
        grounded = edge["steps"][60][1]
        ok = edge["jumps"] == 1 and grounded
        print(f"  pad-edge     {'PASS' if ok else 'FAIL'}  {edge['jumps']} jump(s) from A held "
              f"90 frames (want exactly 1), grounded at the press: {grounded}")
        if not ok:
            failures.append("pad-edge")

    # --- pad-deadzone, pad-dpad: four 30-step legs, each read at three samples
    legs = _gametest_pad_run(workdir, "legs",
                             "0-59 idle\n60-89 lx=-0.1\n90-119 idle\n120-149 lx=-0.6\n"
                             "150-179 idle\n180-209 lx=-1.0\n210-239 idle\n240-269 dleft\n"
                             "270-299 idle\n", 300)
    inside = legs and _leg(legs, 60, 80)
    half = legs and _leg(legs, 120, 140)
    full = legs and _leg(legs, 180, 200)
    dpad = legs and _leg(legs, 240, 260)
    if not (inside and half and full and dpad):
        print("  pad-deadzone FAIL  run failed or the trace is missing a leg's samples")
        print("  pad-dpad     FAIL  (same run)")
        failures += ["pad-deadzone", "pad-dpad"]
    else:
        # Every sample of a leg, not one: a leg is thirty frames of the same input
        # and the value has to hold across them.
        ratios = [h / f for h, f in zip(half, full) if f] if all(full) else []
        ok = (max(abs(v) for v in inside) < 0.05 and all(f < -0.99 for f in full)
              and len(ratios) == len(full) and all(abs(r - 0.5) < 0.05 for r in ratios))
        print(f"  pad-deadzone {'PASS' if ok else 'FAIL'}  stick at 0.1 commands "
              f"{max(inside, key=abs):.2f} (want 0), at 0.6 commands {half[0]:.2f} = "
              f"{ratios[0] if ratios else float('nan'):.3f} of full deflection's {full[0]:.2f} "
              f"(want 0.5 +- 0.05), across {len(full)} samples a leg")
        if not ok:
            failures.append("pad-deadzone")
        ok = all(f < -0.99 for f in full) and all(abs(d - f) <= 0.01 for d, f in zip(dpad, full))
        print(f"  pad-dpad     {'PASS' if ok else 'FAIL'}  dpad left commands {dpad[0]:.2f} "
              f"against the stick's {full[0]:.2f} (want within 1%)")
        if not ok:
            failures.append("pad-dpad")

    # --- pad-off ---------------------------------------------------------------
    off = _gametest_pad_run(workdir, "off", "0-59 idle\n60-99 lx=-1.0\n100-119 off\n120-179 a\n",
                            200)
    before = off and _leg(off, 60, 90)
    after = off and _leg(off, 100, 110)
    if not (before and after):
        print("  pad-off      FAIL  run failed or the trace is missing a sample")
        failures.append("pad-off")
    else:
        ok = all(b < -0.99 for b in before) and all(a == 0.0 for a in after) and off["jumps"] == 0
        print(f"  pad-off      {'PASS' if ok else 'FAIL'}  commanded {before[0]:.2f} before the "
              f"disconnect and {max(after, key=abs):.2f} from the step after it (want 0); "
              f"reconnected with A down: {off['jumps']} jump(s) (want 0)")
        if not ok:
            failures.append("pad-off")

    # --- pad-keys-alias ---------------------------------------------------------
    r = subprocess.run([GAMETEST, "--print-bindings"], capture_output=True, text=True)
    table = {m.group(1): m.group(2) for m in _GAMETEST_BINDING.finditer(r.stdout)
             if ":" in m.group(2)}
    want = {"move_x", "move_y", "jump", "spawn", "pause", "raycast", "ground"}
    missing = sorted(want - set(table))
    halved = sorted(n for n, srcs in table.items()
                    if "key:" not in srcs or not ("pad:" in srcs or "axis:" in srcs))
    ok = r.returncode == 0 and not missing and not halved
    print(f"  pad-keys-alias {'PASS' if ok else 'FAIL'}  {len(table)} actions listed"
          + (f", missing {missing}" if missing else "")
          + (f", without both a key and a pad source: {halved}" if halved else ""))
    if not ok:
        failures.append("pad-keys-alias")

    # --- pad-camera-relative -----------------------------------------------------
    # Two runs holding the same forward. One turns the camera first with the right
    # stick, which is what `look_x` binds beside the arrow keys -- a key cannot be
    # scripted headless, so the pad axis is the only way in.
    straight = _gametest_pad_run(workdir, "cam_straight", "0-60 idle\n61-200 ly=-1\n", 210)
    turned = _gametest_pad_run(workdir, "cam_turned", "0-60 rx=1\n61-200 ly=-1\n", 210)
    if not straight or not turned:
        print("  pad-camera-relative SKIP  no trace")
        failures.append("pad-camera-relative")
    else:
        # Sampled late, after the character has settled into a steady heading.
        def heading(run):
            step = max(s for s in run["vel"])
            vx, vz = run["vel"][step]
            return math.atan2(vx, vz), run["cam"][step]

        h0, c0 = heading(straight)
        h1, c1 = heading(turned)
        # Wrapped into (-pi, pi] before comparing: a heading near the branch cut
        # differs by 2pi from the same direction written the other way.
        def wrap(a):
            return (a + math.pi) % (2.0 * math.pi) - math.pi

        turn = wrap(c1 - c0)
        got = wrap(h1 - h0)
        err = abs(wrap(got - turn))
        # The camera moved at all, or the arm is asserting nothing: a script that
        # failed to reach look_x would give turn == 0, under which world-aligned and
        # camera-relative are the same answer and this would pass on either.
        ok = abs(turn) > 0.5 and err < 0.05
        print(f"  pad-camera-relative {'PASS' if ok else 'FAIL'}  the camera turned "
              f"{math.degrees(turn):.1f} deg and the commanded heading turned "
              f"{math.degrees(got):.1f} deg, out by {math.degrees(err):.2f} (want under 3 "
              f"deg, and the camera to have actually moved: forward is away from the LENS, "
              f"so world-aligned movement reads 0 deg here while every other arm in this "
              f"group stays green)")
        if not ok:
            failures.append("pad-camera-relative")

    return failures
