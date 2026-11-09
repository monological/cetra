"""The camera group: the camera through a scripted pointer (drag, pan, release; spec 12.19)."""

import functools
import math
import re
import os
import subprocess

from gatelib.harness import RENDER, _bin, asset
from gatelib.probes import _PROBE_TIMEOUT, _gametest_probe_text
from gatelib.scenes import RENDER_CAM


CAM_PROBE_POSE = re.compile(
    r"^cam \S+ (\S+) eye (\S+) (\S+) (\S+) look (\S+) (\S+) (\S+) yaw (\S+) pitch (\S+)$", re.M)
CAM_SET_FILE = re.compile(r"^cam settings file fov (\S+) sens (\S+) invert (\d) reduce (\d)$", re.M)
CAM_SET_FOV = re.compile(r"^cam settings fov (\S+)$", re.M)
CAM_SET_RIG = re.compile(
    r"^cam settings (\S+) scale (\S+) invert (\d) shake (\S+) authored (\S+) (\S+)$", re.M)


def _cam_probe_settings(workdir):
    """gametest --cam-probe settings, against a per-run settings dir so a real
    player's file is never touched (settings.h's CETRA_SETTINGS_DIR)."""
    text = _cam_probe_text("settings", os.path.join(workdir, "cam_settings"))
    f = CAM_SET_FILE.findall(text) if text else []
    rows = CAM_SET_RIG.findall(text) if text else []
    if not f or not rows:
        return None
    fov = CAM_SET_FOV.findall(text)
    if not fov:
        return None
    return {"file": [float(f[0][0]), float(f[0][1]), int(f[0][2]), int(f[0][3])],
            "fov": float(fov[0]),
            "rig": {row[0]: {"scale": float(row[1]), "invert": int(row[2]),
                             "shake": float(row[3]),
                             "authored": (float(row[4]), float(row[5]))} for row in rows}}


CAM_SEAM = re.compile(r"^cam seam (\S+) eye (\S+) (\S+) (\S+) look (\S+) (\S+) (\S+)$", re.M)


def _cam_probe_seam():
    """--cam-probe seam: {label: {eye, look}} off a HEADLESS engine, since what it
    asserts is the engine reaching a rig. It still draws no frame."""
    text = _cam_probe_text("seam")
    rows = CAM_SEAM.findall(text) if text else []
    if not rows:
        return None
    return {row[0]: {"eye": [float(x) for x in row[1:4]], "look": [float(x) for x in row[4:7]]}
            for row in rows}


CAM_PROBE_DIST = re.compile(
    r"^cam \S+ (\S+) dist (\S+) arm (\S+) eye_lift (\S+) wide (\S+)$", re.M)


def _cam_probe_dist(case):
    """The distance columns of a --cam-probe case. Same run as _cam_probe's."""
    text = _cam_probe_text(case)
    rows = CAM_PROBE_DIST.findall(text) if text else []
    if not rows:
        return None
    return {row[0]: {"dist": float(row[1]), "arm": float(row[2]), "eye_lift": float(row[3]),
                     "wide": float(row[4])} for row in rows}


CAM_PROBE_BASIS = re.compile(r"^cam basis (\S+) steers (\d) yaw (\S+)$", re.M)


@functools.cache
def _cam_probe_text(case, settings_dir=None):
    """One gametest --cam-probe run, cached by case.

    Through the shared runner, which carries the timeout -- a probe that wedges
    should fail its group, not hang the suite. Cached because several arms read
    different columns of one case's output and a spawn per column is a process
    per regex."""
    env = dict(os.environ, CETRA_SETTINGS_DIR=settings_dir) if settings_dir else None
    text, rc = _gametest_probe_text("--cam-probe", case, env=env)
    return None if rc != 0 else text


def _cam_probe(case):
    """{label: pose} from a --cam-probe case. No window, no GL, no engine: the rig
    takes plain values and touches nothing, so an arm that needed a frame would be
    evidence the design had slipped."""
    text = _cam_probe_text(case)
    rows = CAM_PROBE_POSE.findall(text) if text else []
    if not rows:
        return None
    out = {}
    for row in rows:
        v = [float(x) for x in row[1:]]
        out[row[0]] = {"eye": v[0:3], "look": v[3:6], "yaw": v[6], "pitch": v[7]}
    return out


def _cam_probe_basis():
    text = _cam_probe_text("basis")
    rows = CAM_PROBE_BASIS.findall(text) if text else []
    if not rows:
        return None
    return {label: (int(steers), float(yaw)) for label, steers, yaw in rows}

# create_mouse_drag_controller's defaults, restated because a gate cannot ask the
# binary for them. If a rewrite changes either, these arms fail -- which is right:
# a changed drag rate IS a behaviour change, and the whole reason this group
# exists is that nothing could see one before.
DRAG_SENSITIVITY = 0.002   # radians of orbit per framebuffer pixel
DRAG_PAN_PER_UNIT = 0.0005 # world units per pixel, per unit of camera distance


def _pointer_run(workdir, binary, tag, script, frames, extra=()):
    """One scripted-pointer run of `binary`: {frame: pose} from --trace-camera.

    A pose is (eye, target, dist, theta, phi, ortho), off the line both apps
    print through app_trace_camera -- one format, so the two cannot drift apart
    under one regex."""
    path = os.path.join(workdir, f"pointer_{tag}.txt")
    with open(path, "w") as f:
        f.write(script)
    r = subprocess.run([binary, "-x", "-f", str(frames), *extra,
                        "--input-script", path, "--trace-camera"],
                       capture_output=True, text=True, timeout=_PROBE_TIMEOUT)
    rows = RENDER_CAM.findall(r.stdout + r.stderr)
    if r.returncode != 0 or not rows:
        return None
    return {int(row[0]): {"eye": [float(x) for x in row[1:4]],
                          "target": [float(x) for x in row[4:7]],
                          "dist": float(row[7]), "theta": float(row[8]),
                          "phi": float(row[9]), "ortho": float(row[10]),
                          "near": float(row[11]), "far": float(row[12])} for row in rows}


def _render_pointer_run(workdir, tag, script, frames):
    """The viewer camera. The puppet is the model because it is committed, small
    and has a fixture's stable bounds, so the framing these arms start from is a
    constant rather than whatever an asset happened to measure."""
    return _pointer_run(workdir, RENDER, tag, script, frames,
                        ("-m", asset("puppet.gltf"), "-W", "320", "-H", "200",
                         "--no-scene-file"))


def _shapes_pointer_run(workdir, tag, script, frames):
    """The plane camera, which no automated run could reach at all until shapes
    gained a headless mode: it is the only caller of PlaneDrag."""
    return _pointer_run(workdir, _bin("shapes"), f"canvas_{tag}", script, frames)


def run_camera_gate(workdir):
    """The camera, asserted through a SCRIPTED POINTER (spec 12.19).

    Until this group existed nothing in the suite had ever pressed a mouse, so
    the camera four apps move with a drag was covered by no arm and no golden --
    `docs/verification.md` recorded the hole for the follow camera's rotation and
    it was wider than that. --input-script replays a drag through the engine's
    own pointer path, so what these arms read is the code a hand reaches.

    Every arm below is a CLOSED FORM rather than a recorded pose, and that is the
    point: a recording says the camera does what it used to, where a closed form
    says what it should do and a person can check the arithmetic by reading it.

    Most need no window at all, the way `ui_layout`'s do: the rig takes plain
    values and touches no GL, no physics and no clock, so an arm that needed a
    frame would be evidence that design had slipped. `cam-settings` and
    `cam-engine-seam` are the two that take a headless engine, because what they
    assert is an engine reaching a rig; neither draws a frame.

      cam-orbit-closed the eye lands on anchor + look_lift + eye_lift - dir*dist
                     with every term non-zero, so a dropped one cannot hide
                     behind a default; the turn is a RATE (full input for half a
                     second equals half input for one); and an update asking for
                     nothing does not move the eye, because the pose is derived
                     from the aim rather than integrated.
      cam-pitch-clamp 12 radians of demand into a band of [-1.25, 0.2] saturates
                     at each end and HOLDS when asked again -- which is what
                     tells a clamp from a wrap that came round the other side.
      cam-first-person at distance 0 the eye sits on the anchor and the target is
                     one unit along the aim. A first-person camera has an aim
                     DIRECTION and no aim point, and eye-to-target is read
                     downstream as a focus distance, so the token unit is load
                     bearing rather than arbitrary.
      cam-pose-adopt a stated eye and target are adopted exactly, and a tick that
                     asks for nothing does not move them. Both halves: the rig
                     is given lifts of 9 first, so an adopt that failed to zero
                     them would displace the very pose it was handed.
      cam-settings   the four values a player states -- FOV, look sensitivity,
                     invert-Y and motion reduction -- survive the file and reach
                     a live rig. IDEMPOTENT is the half that bites: settings are
                     applied on every edit, so a slider held for a second reaches
                     apply sixty times, and scaling a rate in place read 9.11
                     rad/s where 4.05 was wanted two applies in. The authored
                     rates must come back untouched, which is what makes that
                     true; invert and shake are read two-sided.
      cam-engine-seam the ENGINE running the rig, the one case here that needs
                     one: no rig leaves the camera exactly alone (so an app that
                     poses its own is untouched), one rig writes its pose, and a
                     second REPLACES it -- the first is then moved and updated
                     and must not reach the camera. One slot is what makes the
                     five hand-rolled stand-down guards unnecessary rather than
                     uniform.
      cam-arm-response a 0..1 signal moves the distance between a near and a far
                     end at 1 - e^(-rate*dt), and the two rates are DIFFERENT:
                     opening out and coming back in are not the same event, so
                     an arm checking only that it moved would pass on one shared
                     rate. Held at rest it returns to the near end exactly.
      cam-probe-shorten a probe reporting half a 10-unit arm clear shortens it to
                     the hit less the skin, reporting none of it floors at the
                     minimum rather than collapsing onto what the camera
                     follows, and THE AIM POINT IS UNCHANGED THROUGHOUT -- which
                     is the whole difference between shortening an arm and
                     clamping an eye, and the reason forest's floor clamp
                     becomes this rather than a second seam.
      cam-blend      an anchor that jumps 20 units under a one-second blend reads
                     the OLD pose at t=0, half way at half a second, the new one
                     at t=1, and holds. The t=0 leg is the one that matters: a
                     blend starting at the new pose shows the cut it exists to
                     hide.
      cam-shake      a shake moves the eye, decays to zero, and at shake_scale 0
                     is BIT-IDENTICAL to no shake -- not merely small. A
                     motion-reduction setting that leaves a millimetre in is one
                     that does not work.
      cam-rail       a four-point rail starts and ends on its authored ends and
                     passes exactly THROUGH its interior control, which is
                     Catmull-Rom's defining property and what a wrong basis
                     matrix would miss; clearing it hands the eye back to the
                     arm.
      cam-basis      a rig that does not steer answers false and leaves the
                     caller's yaw at its sentinel; one that does publishes its
                     own. This is the arm that turns 12.13 and 12.17's flip from
                     a consequence of which branch wrote the pose into a field
                     with a name and a default.
      cam-drag-orbit a 200-pixel horizontal drag turns phi by exactly
                     200 x sensitivity and leaves theta, the distance and the
                     target where they were. The second half is what tells an
                     orbit from a pan: a pan moves the target and nothing else,
                     so an arm reading only the angle would pass on either.
      cam-drag-release the pose stops changing the frame after the button comes
                     up, and the drag's total is still there. Without it the arms
                     either side of it would pass on a controller that never stopped.
      cam-canvas     the 2D canvas, which no automated run could reach before
                     this: a drag pans the target in the drag's own ratio with z
                     and the ortho height unmoved, the wheel takes the ORTHO
                     HEIGHT to 0.9 squared with the distance untouched (a 2D zoom
                     is a projection field where a 3D one is a pose field), and
                     the view stays square-on throughout -- rotation being
                     forbidden under a parallel projection rather than unused --
                     and a notch at opposite corners pulls the target opposite
                     ways, which is the zoom being anchored on the CURSOR. That
                     last is the property the 2D camera keeps its own maths for,
                     and it was green over a path anchoring at the window corner
                     until engine_cursor_fb learned about the scripted pointer.
      cam-drag-zoom  three wheel notches take the distance to 0.9 cubed of what
                     it was, and the aim does not move. The viewer had NO scroll
                     handling at all until spec 12.19 -- zoom was arrow-keys-only
                     -- and nothing noticed, because nothing in the suite could
                     turn a wheel.
      cam-drag-pan   a SHIFT-drag of (200, 100) pixels moves the target by
                     hypot(200, 100) x distance x 0.0005 and leaves phi, theta
                     and the distance alone. The displacement is read as a
                     MAGNITUDE because the pan runs in the camera's own plane,
                     not the world's: the up vector is cross(forward, right) and
                     the eye sits above the target, so a pan with a vertical
                     component correctly moves the target in z as well.
    """
    failed = []

    def note(name, ok):
        if not ok:
            failed.append(name)

    # ---- the rig itself, with no window at all
    orbit = _cam_probe("orbit")
    if not orbit:
        print("  cam-orbit-closed SKIP  no probe output")
        note("cam-orbit-closed", False)
    else:
        # anchor (3, 1, -2), look_lift 0.5, eye_lift 1.5, dist 4, yaw 0, pitch 0.
        # Every term is non-zero so a dropped one cannot hide.
        s = orbit["start"]
        want_look = [3.0, 1.5, -2.0]
        want_eye = [3.0, 3.0, -6.0]
        placed = (max(abs(a - b) for a, b in zip(s["look"], want_look)) < 1e-5
                  and max(abs(a - b) for a, b in zip(s["eye"], want_eye)) < 1e-5)
        # 1.8 rad/s: full input for 0.5 s, then half input for 1.0 s, must be
        # the same turn twice.
        first = orbit["turned"]["yaw"] - s["yaw"]
        second = orbit["again"]["yaw"] - orbit["turned"]["yaw"]
        rate_law = abs(first - 0.9) < 1e-5 and abs(second - first) < 1e-5
        held = orbit["held"]
        derived = max(abs(a - b) for a, b in zip(held["eye"], orbit["again"]["eye"])) < 1e-9
        ok = placed and rate_law and derived
        print(f"  cam-orbit-closed {'PASS' if ok else 'FAIL'}  eye {s['eye']} look {s['look']} "
              f"(want {want_eye} / {want_look}); a turn of {first:.6f} rad at full input over "
              f"0.5 s and {second:.6f} at half over 1.0 s (a rate is a rate); an update asking "
              f"for nothing moved the eye by "
              f"{max(abs(a - b) for a, b in zip(held['eye'], orbit['again']['eye'])):.2e} "
              f"(the pose is derived from the aim, so it must not creep)")
        note("cam-orbit-closed", ok)

    clamp = _cam_probe("clamp")
    if not clamp:
        print("  cam-pitch-clamp SKIP  no probe output")
        note("cam-pitch-clamp", False)
    else:
        # 12 radians of demand into a band of [-1.25, 0.2], three times over.
        hi, lo, lower = clamp["high"]["pitch"], clamp["low"]["pitch"], clamp["lower"]["pitch"]
        ok = abs(hi - 0.2) < 1e-6 and abs(lo + 1.25) < 1e-6 and abs(lower - lo) < 1e-9
        print(f"  cam-pitch-clamp {'PASS' if ok else 'FAIL'}  12 rad of demand saturates at "
              f"{hi:.6f} and {lo:.6f} (want 0.2 and -1.25), and holds at {lower:.6f} when asked "
              f"again -- a clamp holds where a wrap would come round the other side")
        note("cam-pitch-clamp", ok)

    fp = _cam_probe("first-person")
    if not fp:
        print("  cam-first-person SKIP  no probe output")
        note("cam-first-person", False)
    else:
        e = fp["eye"]
        # dist 0: the eye sits ON the anchor plus its lift, and the target is one
        # unit along the aim -- there being no aim POINT at all in first person.
        want_eye = [3.0, 2.7, -2.0]
        at_anchor = max(abs(a - b) for a, b in zip(e["eye"], want_eye)) < 1e-5
        span = math.dist(e["look"], e["eye"])
        yaw, pitch = e["yaw"], e["pitch"]
        cp = math.cos(pitch)
        want_dir = [math.sin(yaw) * cp, math.sin(pitch), math.cos(yaw) * cp]
        got_dir = [(a - b) for a, b in zip(e["look"], e["eye"])]
        aimed = max(abs(a - b) for a, b in zip(got_dir, want_dir)) < 1e-5
        ok = at_anchor and abs(span - 1.0) < 1e-5 and aimed
        print(f"  cam-first-person {'PASS' if ok else 'FAIL'}  at distance 0 the eye is "
              f"{e['eye']} (want {want_eye}) and the target is {span:.6f} out along the aim "
              f"(want 1, since a first-person camera has an aim DIRECTION and no aim point, and "
              f"everything downstream reads eye-to-target as a distance)")
        note("cam-first-person", ok)

    pose = _cam_probe("pose")
    if not pose:
        print("  cam-pose-adopt SKIP  no probe output")
        note("cam-pose-adopt", False)
    else:
        # A stated eye and target, adopted onto a rig carrying lifts of 9, then
        # ticked. Both halves matter: the adopt must reproduce the pose, and the
        # tick must not move it, or a pinned camera drifts the moment it runs.
        s, t = pose["set"], pose["ticked"]
        want_eye, want_look = [1.0, 2.0, 3.0], [-4.0, 0.5, 6.0]
        adopted = (max(abs(a - b) for a, b in zip(s["eye"], want_eye)) < 1e-6
                   and max(abs(a - b) for a, b in zip(s["look"], want_look)) < 1e-6)
        # EXACT, not close. A stated pose that a tick moves at all is --cam-eye
        # and a config restore reproducing a session approximately, and the error
        # grows with the arm: the far leg below frames from 20,196 units, where
        # re-deriving the pose lost 0.002 and 885 pixels of a restored frame.
        drift = max(abs(a - b) for a, b in zip(t["eye"], s["eye"]))
        far_drift = max(abs(a - b) for a, b in zip(pose["far_ticked"]["eye"],
                                                   pose["far"]["eye"]))
        ok = adopted and drift == 0.0 and far_drift == 0.0
        print(f"  cam-pose-adopt {'PASS' if ok else 'FAIL'}  a stated pose is adopted exactly, "
              f"and a tick that asks for nothing moves the eye {drift:.2e} at a 5-unit arm and "
              f"{far_drift:.2e} at a 20,196-unit one (want 0 at both: the lifts must be zeroed "
              f"by the adopt, and the pose must be KEPT rather than re-derived through asin and "
              f"atan2, whose error grows with the arm)")
        note("cam-pose-adopt", ok)

    setg = _cam_probe_settings(workdir)
    if not setg:
        print("  cam-settings SKIP  no probe output")
        note("cam-settings", False)
    else:
        fov, sens, invert, reduce_m = setg["file"]
        survived = (abs(fov - 71.5) < 1e-4 and abs(sens - 2.25) < 1e-6
                    and invert == 1 and reduce_m == 1)
        on, twice, off = setg["rig"]["applied"], setg["rig"]["twice"], setg["rig"]["upright"]
        # That they ARRIVED. Without this the arm passes with the two lines that
        # apply them deleted, because 1.0 == 1.0 across all three rows -- which
        # is what it did.
        reached = (abs(on["scale"] - 2.25) < 1e-6 and on["invert"] == 1
                   and abs(off["scale"] - 2.25) < 1e-6
                   and abs(setg["fov"] - 71.5) < 1e-3)
        # IDEMPOTENT, which is the half that bites: settings are applied on every
        # edit, so a slider held for a second reaches this sixty times. Scaling a
        # rate in place read 9.11 rad/s where 4.05 was wanted, two applies in.
        idempotent = (on["scale"] == twice["scale"] and on["invert"] == twice["invert"]
                      and on["shake"] == twice["shake"])
        # The authored rates are never overwritten, which is what makes that true.
        authored = (abs(twice["authored"][0] - 1.8) < 1e-6
                    and abs(twice["authored"][1] - 1.2) < 1e-6)
        two_sided = off["invert"] == 0 and off["shake"] == 1.0 and on["shake"] == 0.0
        ok = survived and reached and idempotent and authored and two_sided
        print(f"  cam-settings {'PASS' if ok else 'FAIL'}  fov {fov:g}, sensitivity {sens:g}, "
              f"invert {invert} and reduce-motion {reduce_m} survive the file; applying twice "
              f"leaves scale {twice['scale']:g} (want the same as one apply, since a held slider "
              f"applies once a frame) with the authored rates {twice['authored']} untouched; the "
              f"scale ARRIVES at {on['scale']:g} and the FOV at {setg['fov']:g} deg; and invert "
              f"and shake are two-sided ({off['invert']} / {off['shake']:g} with them off)")
        note("cam-settings", ok)

    seam = _cam_probe_seam()
    if not seam:
        print("  cam-engine-seam SKIP  no probe output")
        note("cam-engine-seam", False)
    else:
        # An app that poses its own camera must be untouched by a seam it never
        # opted into, or this could not have landed before any app converted.
        untouched = max(abs(a - b) for a, b in zip(seam["none"]["eye"],
                                                   seam["before"]["eye"])) < 1e-9
        first = max(abs(a - b) for a, b in zip(seam["first"]["eye"], [5.0, 0.0, -2.0])) < 1e-5
        # The second rig replaced the first, and the FIRST was then moved to
        # (99, 99, 99) and updated. If it still reached the camera there would be
        # two live writers -- which is the condition five apps each hand-rolled a
        # guard against.
        second = max(abs(a - b) for a, b in zip(seam["second"]["eye"], [-7.0, 0.0, -3.0])) < 1e-5
        ok = untouched and first and second
        print(f"  cam-engine-seam {'PASS' if ok else 'FAIL'}  with no rig installed the camera "
              f"is {'left exactly alone' if untouched else 'MOVED'}; one rig puts it at "
              f"{seam['first']['eye']} (want [5, 0, -2]); and after a second replaces it the "
              f"camera is at {seam['second']['eye']} (want [-7, 0, -3]) with the first rig moved "
              f"to 99 and updated -- so one slot is what makes the five hand-rolled stand-down "
              f"guards unnecessary rather than uniform")
        note("cam-engine-seam", ok)

    resp = _cam_probe_dist("response")
    if not resp:
        print("  cam-arm-response SKIP  no probe output")
        note("cam-arm-response", False)
    else:
        # near 4 / far 12, widen 4/s and tighten 1.2/s, a quarter second each way.
        # 1 - e^(-rate*dt) is the fraction covered, so the two are 0.6321 and
        # 0.2592 of what was left -- the asymmetry IS the behaviour, and an arm
        # that only checked "it moved" would pass on one shared rate.
        widen = resp["widening"]["wide"]
        tight = resp["tightening"]["wide"]
        want_widen = 1.0 - math.exp(-4.0 * 0.25)
        want_tight = widen * math.exp(-1.2 * 0.25)
        lerped = abs(resp["widening"]["dist"] - (4.0 + 8.0 * widen)) < 1e-4
        settled = abs(resp["settled"]["dist"] - 4.0) < 1e-4
        ok = (abs(widen - want_widen) < 1e-5 and abs(tight - want_tight) < 1e-5
              and lerped and settled)
        print(f"  cam-arm-response {'PASS' if ok else 'FAIL'}  a quarter second of signal covers "
              f"{widen:.6f} of the way out (want {want_widen:.6f}) and a quarter second back "
              f"covers to {tight:.6f} (want {want_tight:.6f}) -- asymmetric on purpose, since a "
              f"shot that widens late reads as lag and one that tightens fast reads as a snap; "
              f"the distance is the lerp of the two ends and returns to "
              f"{resp['settled']['dist']:.6f} (want 4)")
        note("cam-arm-response", ok)

    pr = _cam_probe("probe")
    prd = _cam_probe_dist("probe")
    if not pr or not prd:
        print("  cam-probe-shorten SKIP  no probe output")
        note("cam-probe-shorten", False)
    else:
        # A probe reporting half of a 10-unit arm clear, then none of it, with a
        # skin of 0.6 and a floor of 2.
        blocked, flush = prd["blocked"]["arm"], prd["flush"]["arm"]
        # The eye stays ON the arm, which is the property a height clamp breaks
        # and an identical aim POINT does not: the look point is written from the
        # anchor either way, so comparing it is true by construction.
        def _dir(p):
            d = [a - b for a, b in zip(p["look"], p["eye"])]
            n = math.dist(p["look"], p["eye"])
            return [c / n for c in d] if n > 0 else d
        rays = [_dir(pr[k]) for k in ("clear", "blocked", "flush")]
        on_arm = all(max(abs(a - b) for a, b in zip(rays[0], r)) < 1e-5 for r in rays)
        # And the WANTED distance is untouched by the probe: it is what the
        # response asked for, and a probe writing into it would bake one frame's
        # occlusion into the arm permanently.
        wanted = all(abs(prd[k]["dist"] - 10.0) < 1e-4 for k in ("blocked", "flush"))
        ok = abs(blocked - 4.4) < 1e-4 and abs(flush - 2.0) < 1e-4 and on_arm and wanted
        print(f"  cam-probe-shorten {'PASS' if ok else 'FAIL'}  half of a 10-unit arm clear "
              f"shortens it to {blocked:.4f} (want 5 - 0.6 skin), nothing clear floors at "
              f"{flush:.4f} (want the 2 minimum, or the camera collapses into what it follows), "
              f"the eye stays {'on the arm' if on_arm else 'OFF the arm'} throughout -- which is "
              f"what a height clamp breaks and an identical aim point would not see -- and the "
              f"WANTED distance is {'untouched' if wanted else 'OVERWRITTEN'} at 10")
        note("cam-probe-shorten", ok)

    bl = _cam_probe("blend")
    if not bl:
        print("  cam-blend SKIP  no probe output")
        note("cam-blend", False)
    else:
        # The anchor jumps 20 units and the blend runs a second. Frame 0 must
        # still be the OLD pose: a blend that starts at the new one shows the
        # cut it exists to hide.
        at0 = max(abs(a - b) for a, b in zip(bl["t0"]["eye"], bl["before"]["eye"])) < 1e-6
        half_x = bl["half"]["eye"][0]
        at1 = abs(bl["t1"]["eye"][0] - 20.0) < 1e-5
        after = max(abs(a - b) for a, b in zip(bl["after"]["eye"], bl["t1"]["eye"])) < 1e-6
        ok = at0 and abs(half_x - 10.0) < 1e-5 and at1 and after
        print(f"  cam-blend {'PASS' if ok else 'FAIL'}  an anchor that jumps 20 units under a "
              f"1 s blend reads {bl['t0']['eye'][0]:.4f} at t=0 (want the OLD 0, or the cut "
              f"shows), {half_x:.4f} halfway (want 10, smoothstep being symmetric there), "
              f"{bl['t1']['eye'][0]:.4f} at t=1 and stays there afterwards")
        note("cam-blend", ok)

    sh = _cam_probe("shake")
    if not sh:
        print("  cam-shake SKIP  no probe output")
        note("cam-shake", False)
    else:
        moved = math.dist(sh["shaken"]["eye"], sh["rest"]["eye"])
        spent = math.dist(sh["spent"]["eye"], sh["rest"]["eye"])
        # Not "a small shake": the SAME pose, to the bit. Motion reduction that
        # leaves a millimetre in is a motion-reduction switch that does not work.
        reduced = sh["reduced"]["eye"] == sh["rest"]["eye"]
        ok = moved > 0.1 and spent < 1e-6 and reduced
        print(f"  cam-shake {'PASS' if ok else 'FAIL'}  a shake moves the eye {moved:.4f} and "
              f"decays to {spent:.2e} (want 0), and at shake_scale 0 the pose is "
              f"{'bit-identical' if reduced else 'NOT identical'} to no shake at all -- which is "
              f"what the motion-reduction setting has to mean")
        note("cam-shake", ok)

    rail = _cam_probe("rail")
    if not rail:
        print("  cam-rail SKIP  no probe output")
        note("cam-rail", False)
    else:
        # Four authored points. Catmull-Rom passes THROUGH its control points,
        # which a Bezier over the same four would not -- so hitting the interior
        # one exactly is what says the basis and the tangents are right.
        first = max(abs(a - b) for a, b in zip(rail["start"]["eye"], [0.0, 0.0, 0.0])) < 1e-5
        last = max(abs(a - b) for a, b in zip(rail["end"]["eye"], [30.0, 5.0, 0.0])) < 1e-5
        interior = max(abs(a - b) for a, b in zip(rail["third"]["eye"], [10.0, 5.0, 0.0])) < 1e-5
        # The MIDDLE of a segment, the only sample where the tangents matter.
        # Catmull-Rom gives z = 5.625 here; a straight polyline gives 5.0 and a
        # wrong tangent coefficient 5.3125, so this separates all three by
        # thousands of times the tolerance. Every node lands on its control point
        # whatever the basis, so the three legs above cannot tell them apart.
        mid = max(abs(a - b) for a, b in zip(rail["mid"]["eye"], [15.0, 2.5, 5.625])) < 1e-4
        off = max(abs(a - b) for a, b in zip(rail["off"]["eye"], [0.0, 0.0, -4.0])) < 1e-5
        ok = first and last and interior and mid and off
        print(f"  cam-rail {'PASS' if ok else 'FAIL'}  a four-point rail starts at its first "
              f"point and ends at its last; a third of the way along it is at "
              f"{rail['third']['eye']} (want the second authored point exactly, since "
              f"Catmull-Rom passes THROUGH its controls); the middle of a segment is at "
              f"{rail['mid']['eye']} (want [15, 2.5, 5.625] -- a polyline would give 5.0 there, "
              f"which is the only sample that evaluates the tangents at all); and clearing the "
              f"rail hands the eye back to the arm")
        note("cam-rail", ok)

    basis = _cam_probe_basis()
    if not basis:
        print("  cam-basis SKIP  no probe output")
        note("cam-basis", False)
    else:
        off_steers, off_yaw = basis["off"]
        on_steers, on_yaw = basis["on"]
        # The sentinel is what makes the false leg mean something: a rig that
        # does not steer must leave the caller's variable ALONE, not zero it,
        # or a game reading it would silently face east.
        ok = (off_steers == 0 and off_yaw == -99.0 and on_steers == 1 and abs(on_yaw - 0.75) < 1e-6)
        print(f"  cam-basis {'PASS' if ok else 'FAIL'}  a rig that does not steer answers "
              f"{off_steers} and leaves the caller's yaw at its sentinel {off_yaw:g}; one that "
              f"does answers {on_steers} with {on_yaw:g} (want 0/-99 then 1/0.75). This is the "
              f"arm that makes 12.13 and 12.17's flip a named field rather than a consequence")
        note("cam-basis", ok)

    # A press with no movement, then 20 frames of 10 pixels: the press frame is
    # separated from the motion so a stale drag offset would show as a jump.
    orbit = _render_pointer_run(workdir, "orbit", (
        "0-4    at=320,200\n"
        "5-24   at=320,200 down\n"
        "25-44  by=10,0 down\n"
        "45-60  idle\n"), 62)

    if not orbit:
        print("  cam-drag-orbit SKIP  no trace")
        note("cam-drag-orbit", False)
    else:
        before, after = orbit[24], orbit[45]
        want = 200.0 * DRAG_SENSITIVITY
        got = before["phi"] - after["phi"]
        still = (abs(after["theta"] - before["theta"]) < 1e-6
                 and abs(after["dist"] - before["dist"]) < 1e-6
                 and max(abs(a - b) for a, b in zip(after["target"], before["target"])) < 1e-6)
        ok = abs(got - want) < 1e-4 and still
        print(f"  cam-drag-orbit {'PASS' if ok else 'FAIL'}  200 px of drag turned phi "
              f"{got:.6f} rad (want {want:.6f}), with theta, distance and the target "
              f"{'unmoved' if still else 'MOVED, so this is not an orbit'}")
        note("cam-drag-orbit", ok)

        last = max(orbit)
        held = orbit[45]
        settled = all(abs(orbit[f]["phi"] - held["phi"]) < 1e-9 for f in range(46, last + 1))
        moved = abs(held["phi"] - orbit[0]["phi"]) > 0.1
        ok = settled and moved and last >= 55
        print(f"  cam-drag-release {'PASS' if ok else 'FAIL'}  phi is "
              f"{'constant' if settled else 'STILL MOVING'} over the {last - 45} frames after "
              f"the release, having turned {held['phi'] - orbit[0]['phi']:.4f} rad in total "
              f"(a controller that never stopped would pass every other arm here)")
        note("cam-drag-release", ok)

    canvas = _shapes_pointer_run(workdir, "pan", (
        "0-4    at=200,400\n"
        "5-24   at=200,400 down\n"
        "25-44  by=5,-2.5 down\n"
        "45     idle\n"
        "50     at=200,400 wheel=1\n"
        "60     at=200,400 wheel=1\n"), 70)

    if not canvas:
        print("  cam-canvas SKIP  no trace")
        note("cam-canvas", False)
    else:
        start, panned = canvas[24], canvas[45]
        dx = panned["target"][0] - start["target"][0]
        dy = panned["target"][1] - start["target"][1]
        # The drag was (100, -50) framebuffer pixels, so the pan must carry that
        # RATIO exactly. Read as a ratio and not as world units because the units
        # per pixel depend on the framebuffer scale, which differs per display.
        ratio = dx / dy if dy != 0.0 else 0.0
        flat = (abs(panned["eye"][2] - start["eye"][2]) < 1e-6
                and abs(panned["ortho"] - start["ortho"]) < 1e-6)
        # Two wheel notches at 0.9: a 2D zoom is ortho_height, where a 3D zoom is
        # a distance, and the distance must not move at all.
        zoomed = canvas[max(canvas)]
        factor = zoomed["ortho"] / panned["ortho"]
        held = abs(zoomed["dist"] - panned["dist"]) < 1e-6
        # Rotation is not merely unused in 2D -- it is forbidden, since any
        # rotation shears flat geometry under a parallel projection.
        square = all(abs(canvas[f]["phi"] - start["phi"]) < 1e-6
                     and abs(canvas[f]["theta"] - start["theta"]) < 1e-6 for f in canvas)
        # The zoom is ANCHORED on the cursor, which is the one property the 2D
        # camera keeps its own maths for. A notch at opposite corners must pull
        # the target opposite ways; before engine_cursor_fb answered for the
        # scripted pointer it read the real mouse and anchored both at the window
        # corner, with every other leg here green over it.
        # Framebuffer pixels, so genuinely either side of the centre: shapes is
        # 375x812 points on a 2x display, which is 750x1624 with its middle at
        # (375, 812).
        left = _shapes_pointer_run(workdir, "zoomL", "0-4 at=100,812\n5 at=100,812 wheel=1\n", 12)
        right = _shapes_pointer_run(workdir, "zoomR", "0-4 at=650,812\n5 at=650,812 wheel=1\n", 12)
        anchored = False
        if left and right:
            lx = left[max(left)]["target"][0] - left[4]["target"][0]
            rx = right[max(right)]["target"][0] - right[4]["target"][0]
            anchored = lx * rx < 0.0 and abs(lx) > 1e-4 and abs(rx) > 1e-4
        ok = (abs(ratio + 2.0) < 1e-3 and abs(dx) > 1.0 and flat
              and abs(factor - 0.81) < 1e-4 and held and square and anchored)
        print(f"  cam-canvas {'PASS' if ok else 'FAIL'}  a (100, -50) px drag pans the target "
              f"{dx:.4f}, {dy:.4f} -- a ratio of {ratio:.4f} (want -2 exactly, the drag's own), "
              f"with z and the ortho height unmoved; two wheel notches take the ortho height to "
              f"{factor:.6f} of it (want 0.81) with the DISTANCE untouched, a 2D zoom being a "
              f"projection field where a 3D one is a pose field; the view stays square-on "
              f"throughout ({'yes' if square else 'NO'}), rotation being forbidden here rather "
              f"than unused; and a notch at opposite corners pulls the target "
              f"{'opposite ways' if anchored else 'THE SAME WAY'}, which is the zoom being "
              f"anchored on the cursor rather than on the window")
        note("cam-canvas", ok)

    zoom = _render_pointer_run(workdir, "zoom", (
        "0-9    at=320,200\n"
        "10     at=320,200 wheel=1\n"
        "20     at=320,200 wheel=1\n"
        "30     at=320,200 wheel=1\n"), 40)

    if not zoom:
        print("  cam-drag-zoom SKIP  no trace")
        note("cam-drag-zoom", False)
    else:
        # Three notches at 0.9 each. Before spec 12.19 this app had no scroll
        # handling at all -- the wheel did nothing in a 3D viewer, and nothing
        # noticed, because nothing in the suite could turn one.
        before, after = zoom[10], max(zoom)
        got = zoom[after]["dist"] / before["dist"]
        want = 0.9 ** 3
        # The aim is untouched: a wheel zooms, it does not steer.
        still = (abs(zoom[after]["phi"] - before["phi"]) < 1e-6
                 and abs(zoom[after]["theta"] - before["theta"]) < 1e-6)
        ok = abs(got - want) < 1e-4 and still
        print(f"  cam-drag-zoom {'PASS' if ok else 'FAIL'}  three wheel notches took the distance "
              f"to {got:.6f} of what it was (want {want:.6f} = 0.9 cubed), with the aim "
              f"{'unmoved' if still else 'MOVED'} -- this app had no scroll handling at all "
              f"before the pointer seam existed to notice")
        note("cam-drag-zoom", ok)

    pan = _render_pointer_run(workdir, "pan", (
        "0-4    at=320,200\n"
        "5-24   at=320,200 down shift\n"
        "25-44  by=10,5 down shift\n"
        "45-50  idle\n"), 52)

    if not pan:
        print("  cam-drag-pan SKIP  no trace")
        note("cam-drag-pan", False)
    else:
        before, after = pan[24], pan[45]
        want = math.hypot(200.0, 100.0) * before["dist"] * DRAG_PAN_PER_UNIT
        got = math.dist(after["target"], before["target"])
        still = (abs(after["phi"] - before["phi"]) < 1e-6
                 and abs(after["theta"] - before["theta"]) < 1e-6
                 and abs(after["dist"] - before["dist"]) < 1e-6)
        ok = abs(got - want) < 1e-3 and still
        print(f"  cam-drag-pan {'PASS' if ok else 'FAIL'}  a shift-drag of (200, 100) px moved "
              f"the target {got:.6f} (want {want:.6f} = hypot x dist x {DRAG_PAN_PER_UNIT}), "
              f"with phi, theta and the distance "
              f"{'unmoved' if still else 'MOVED, so this is not a pan'}")
        note("cam-drag-pan", ok)

    return failed
