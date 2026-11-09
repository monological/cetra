"""The origin group: a world away from the origin, and one that moves under it (spec 11.62 /
D11).
"""

import json
import os

from gatelib.harness import FOREST, _run, asset
from gatelib.lighting import (
    DIR_ERODE, DIR_SHADOW, _dir_ellipse, _dir_render, _dir_visible, _term_reader,
)
from gatelib.world import ORIGIN_AUTO, _ORIGIN_TRACE, _origin_diff


# Where the shadow-offset arm places the fixture. Two requirements, and both are
# lower bounds rather than taste. It has to exceed the scene-fit map's own reach
# -- the fixture's radius is about 6 units and the viewer sets ortho_size to
# twice that -- or a map anchored at the origin still covers the scene and the
# arm measures nothing. And it has to be an exact power of two, so that
# translating the fixture is not itself a precision experiment: every authored
# coordinate here lands on a representable value at 16384, which keeps this a
# test of what the map COVERS rather than of what fp32 rounds.
ORIGIN_SHADOW_OFFSET = 16384.0
# A shadowed sample must stay this dark once the map follows the scene. Sized
# off the same umbra the dir-shadow hole arm reads at DIR_ERODE, with room for
# the map resolving a fraction of a texel differently out at the offset.
ORIGIN_UMBRA_MAX = 0.05
# ... and an unshadowed one this bright. A map that misses the scene entirely
# leaves the shadow term at exactly 1.0, so this bar is met by a wide margin or
# not at all; there is no near miss to calibrate against.
ORIGIN_LIT_MIN = 0.90


# Where the shift arms place the world. NEAR is chosen so that fp32 costs almost
# nothing there -- which is the whole point of it: a correct shift must be nearly
# free at a distance where there is nothing to recover, while anything that failed
# to follow the delta costs the same at every distance, because a wrong address is
# not a small error. Both are multiples of the app's 256-unit shift lattice, so
# the new origin lands exactly on the camera rather than near it.
ORIGIN_NEAR = 256.0
ORIGIN_FAR = 262144.0
# A shift at ORIGIN_NEAR may move this much of the frame. Measured 0.22%; a splat
# that stayed behind reads 76%, and a camera re-pinned to an absolute reads the
# same, so the gap between pass and fail here is two orders of magnitude.
ORIGIN_SHIFT_MAX = 0.02
# ... and at ORIGIN_FAR it must move a LOT, because there the shift is recovering
# real precision. Measured 39.9%. Without this the arm above is also satisfied by
# a build where --world-offset does nothing at all.
ORIGIN_DEGRADE_MIN = 0.20
# The velocity arm. The frame after a shift is compared against the steady state
# two frames later, which is the same pair of runs still differing by precision
# alone -- so the ratio asks whether the TRANSITION cost anything beyond what the
# frames on either side of it already cost. Measured 1.08; leaving prev_view_proj
# uncorrected reads 4.48, and there is nothing in between.
ORIGIN_VELOCITY_MAX_RATIO = 2.0
# World units the character's height above the terrain may drift across a shift,
# and the tolerance on the shift moving it by exactly the delta. Measured 0.000 on
# both: the capsule and the ground move together, so this is an equality, and the
# margin is for a controller that resolves a contact differently on the frame the
# broadphase is rebuilt.
ORIGIN_CLEARANCE_MAX = 0.05
ORIGIN_AUTO_THRESHOLD = 512.0


def _origin_offset_fixture(workdir, offset):
    """A translated twin of dir_shadow_fixture, and the scene file that frames it.

    The glTF's four nodes carry no `translation` -- gen_dir_shadow_fixture.py bakes
    positions into the vertex buffer -- so adding one moves the whole scene without
    touching the base64 buffer. That is the only way to translate GEOMETRY from a
    gate: a .cscn's models[] carries a path and nothing else, so cscn_copy can move
    the camera and the lights but never the meshes (spec 11.35 dropped an arm over
    exactly this).

    The camera moves with it, so the two frames are the same PICTURE from the same
    relative pose. That is what lets the sample points below stay in unshifted
    coordinates: the projector subtracts eye from point, and both took the same
    offset.
    """
    src = asset("dir_shadow_fixture.gltf")
    with open(src) as fh:
        doc = json.load(fh)
    for node in doc["nodes"]:
        node["translation"] = [offset, 0.0, offset]
    gltf = os.path.join(workdir, "origin_dir_shadow.gltf")
    with open(gltf, "w") as fh:
        json.dump(doc, fh)

    with open(asset("dir_shadow_fixture.cscn")) as fh:
        scene = json.load(fh)
    scene["models"] = [{"path": gltf}]
    for key in ("eye", "target"):
        v = scene["camera"][key]
        scene["camera"][key] = [v[0] + offset, v[1], v[2] + offset]
    cscn = os.path.join(workdir, "origin_dir_shadow.cscn")
    with open(cscn, "w") as fh:
        json.dump(scene, fh)
    return cscn


def _origin_umbra_samples():
    """Ground points inside both umbrae, in UNSHIFTED fixture coordinates."""
    pts = []
    for centre in (DIR_SHADOW["float_c"], DIR_SHADOW["rest_c"]):
        cx, cz, sx, sz = _dir_ellipse(centre, DIR_SHADOW["elev_deg"])
        for iu in range(-2, 3):
            for iv in range(-2, 3):
                u, v = iu / 2.0, iv / 2.0
                if u * u + v * v > 1.0:
                    continue
                p = (cx + DIR_ERODE * sx * u, 0.0, cz + DIR_ERODE * sz * v)
                if _dir_visible(p):
                    pts.append(p)
    return pts


def _origin_forest(workdir, tag, offset, extra, frames="40", mode="6", size=("800", "450")):
    """One forest run with the world placed `offset` units out, and its log.

    The camera moves with the world, so every arm here compares two frames of the
    same view and the only variable is how large the coordinates are. Framing is
    given explicitly rather than left to the follow camera because a follow camera
    reads the player's position, and the player is a physics body -- which is a
    different subsystem's correctness, not this one's.

    --render-mode 6 for the reason docs/verification.md gives: forest's Hillaire sky moves
    tens of thousands of pixels run to run, and the albedo view is the only mode
    with a 0 px floor. Every arm below measures its own floor anyway.

    The trail (spec 11.68) is left ON here, and only `origin-auto` passes
    --no-trail. That arm compares two runs that shifted by DIFFERENT amounts and
    demands they agree at exactly 0 px; a road makes that unreachable, because
    the dominant-layer index rides the atlas alpha and is read with an unfiltered
    texelFetch, so a road's hard discontinuity in that field flips one texel
    under an ulp of reconstruction difference and switches the whole detail tap.
    Measured 106 px of 1.44M (0.007%), and 61 px between two MANUAL shifts at
    different frames -- which is what says it is the shift AMOUNT, not the
    trigger, and not the auto path.

    Every other arm here keeps the trail deliberately. Their bars are 2% of
    frame, a 20% floor, and a ratio -- none of which 0.007% can reach -- and
    `origin-shift` is the ONE place in the suite that renders a road under an
    origin shift at all. A road that failed to follow authoredPos would land in
    the wrong place, which is a full-size difference rather than a sub-pixel one,
    and that arm is what would catch it.
    """
    path = os.path.join(workdir, f"origin_{tag}.ppm")
    cmd = [FOREST, "-x", "-f", frames, "-W", size[0], "-H", size[1], "--no-fog",
           "--render-mode", mode, "--seed", "1337", "--world-offset", repr(offset),
           "--cam-eye", f"{offset},40,{offset + 120}",
           "--cam-target", f"{offset},10,{offset}", "-S", path] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(path):
        return None, ""
    return path, r.stdout + r.stderr


def _origin_trace(offset, extra):
    """--trace-player rows for a forest run with the world placed `offset` out.

    The follow camera is deliberately NOT pinned here: this arm is about where the
    character is, and pinning the camera would leave the one subsystem under test
    unobserved.
    """
    # 70 frames, not 120: --trace-player prints every 30 steps, the shift lands at
    # 40, and the arm reads the last row before it (step 30) and the first after
    # (step 60). The rest was 50 frames of physics nobody looked at.
    cmd = [FOREST, "-x", "-f", "70", "-W", "200", "-H", "150", "--no-fog",
           "--render-mode", "6", "--seed", "1337", "--world-offset", repr(offset),
           "--trace-player"] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None
    return [{"t": float(m.group(1)), "x": float(m.group(2)), "y": float(m.group(3)),
             "z": float(m.group(4)), "grounded": int(m.group(5)), "terrain": float(m.group(6))}
            for m in _ORIGIN_TRACE.finditer(r.stdout + r.stderr)]


def run_origin_gate(workdir):
    """A world away from the origin, and a world that moves under one.

      shadow-offset   the scene-fit shadow map covers the scene it belongs to rather
                      than the origin. Renders the cascade fixture where it was
                      authored, then translated by 16384 with --shadow-center
                      following, then translated with the centre pinned back at the
                      origin. The first two must shadow; the third must not.
      origin-shift    a mid-run shift at a NEAR offset changes almost nothing. It is
                      only where coordinates are measured from, so anything that
                      failed to follow it shows here -- and shows at full size,
                      since a wrong address is not a small error.
      origin-degrade  the same comparison at a FAR offset changes a lot, because
                      there the shift is recovering precision that was really lost.
                      Without it the arm above passes on a build where placing the
                      world does nothing at all.
      origin-velocity the frame after a shift reports no more motion than the frames
                      either side of it. The velocity buffer is a DIFFERENCE of two
                      transforms, so a previous-frame twin that missed the delta
                      prints the whole translation as one frame of screen-wide
                      motion -- which no other arm here can see.
      origin-physics  the character is standing on the same ground after a shift as
                      before it. Jolt is single precision and holds its own world
                      positions, so it does not follow the scene and has to be told;
                      read as height above terrain, which is the one quantity that
                      must NOT change while both of its terms do.
      origin-auto     the engine re-centres on its own once the camera has drifted,
                      exactly ONCE, and reaches the frame the hand-driven shift
                      reaches. The count is the assertion that matters: a threshold
                      compared against a camera that has not been told the world
                      moved reads its own shift as more drift and oscillates, which
                      a picture cannot show and a pass/fail on "did it shift" cannot
                      either.

    WHAT THIS GROUP CANNOT SEE, since every arm renders apps/forest and a green run
    should not be read as a claim it does not make. Forest has no reflection probe,
    no GI volume, no water and no fog, so those four shift functions are exercised
    by nothing here. Nor is the world-space TILING of a layered surface: it is wired
    (layers.glsl reads authoredPos), but at forest's seed and framing the two layers
    whose periods do not divide the 256-unit shift lattice, rock at 6 and gravel at
    3, cover too little of the frame to register -- reverting the fix moves 13 px,
    while grass and silt tile at 4, which any multiple of the lattice preserves.
    Nor is the transform-feedback particle backend, since no app that uses it shifts.

    On origin-shift's residual, because it looks like a tolerated defect and is not:
    it is PRECISION. It grows with DISTANCE rather than with the lattice, 0.22% at
    256 against 0.40% at 768, which is the curve spec 11.62 phase 2 measured for a
    materialised offset -- and reverting the tiling fix does not move it.

    Read at --shadow-cascades 1, and that is a requirement rather than a default.
    The inner cascades are fitted to the camera frustum, so they follow an offset
    scene perfectly well on their own -- only the outermost fallback map is anchored
    at scene_center. At the viewer's default of 3 the shadow this arm samples is
    resolved by an inner cascade and survives the offset whatever scene_center says,
    which reads green against a build where the fix does not exist.

    The pinned arm is the falsification and it is in-frame rather than a mutation:
    it renders the same geometry through the same binary and differs only in where
    the map is centred, so "the map followed the scene" is measured against "it did
    not" instead of against a remembered number.
    """
    failures = []
    if not os.path.exists(asset("dir_shadow_fixture.gltf")):
        print("  shadow-offset SKIP  (missing dir_shadow_fixture.gltf)")
        return failures

    off = ORIGIN_SHADOW_OFFSET
    far = _origin_offset_fixture(workdir, off)
    # --no-pcss for the dir-shadow gate's reason: the default emitter size smears
    # the analytic edge, and every reading here is positional.
    cc1 = ["--no-pcss", "--shadow-cascades", "1"]
    centred = cc1 + ["--shadow-center", f"{off},0,{off}"]
    pinned = cc1 + ["--shadow-center", "0,0,0"]

    frames = {
        "home": _dir_render(workdir, "dir_shadow_fixture.cscn", "org_home", cc1),
        "home_ns": _dir_render(workdir, "dir_shadow_fixture.cscn", "org_home_ns",
                               cc1 + ["--no-shadows"]),
        "far": _dir_render(workdir, far, "org_far", centred),
        "far_ns": _dir_render(workdir, far, "org_far_ns", centred + ["--no-shadows"]),
        "pin": _dir_render(workdir, far, "org_pin", pinned),
    }
    if not all(frames.values()):
        print("  shadow-offset ERROR while rendering the fixture")
        return failures + ["shadow-offset"]

    pts = _origin_umbra_samples()
    try:
        home = _term_reader(frames["home"], frames["home_ns"])
        far_t = _term_reader(frames["far"], frames["far_ns"])
        # The pinned frame shares the offset scene's unshadowed reference: the two
        # differ only in the shadow map, so dividing by the same denominator is
        # what makes the pair a measurement of the map alone.
        pin_t = _term_reader(frames["pin"], frames["far_ns"])
        home_worst = max(home(p) for p in pts)
        far_worst = max(far_t(p) for p in pts)
        pin_worst = min(pin_t(p) for p in pts)
    except ValueError as exc:
        print(f"  shadow-offset ERROR {exc}")
        return failures + ["shadow-offset"]

    ok = (home_worst <= ORIGIN_UMBRA_MAX and far_worst <= ORIGIN_UMBRA_MAX
          and pin_worst >= ORIGIN_LIT_MIN)
    print(f"  shadow-offset {'PASS' if ok else 'FAIL'}  umbra at origin {home_worst:.4f}, "
          f"at {off:.0f} {far_worst:.4f} (want <= {ORIGIN_UMBRA_MAX}); "
          f"centre pinned back {pin_worst:.4f} (want >= {ORIGIN_LIT_MIN})")
    if not ok:
        failures.append("shadow-offset")

    # --- the shift itself: near, far, and the frame it lands on ---------------
    if not os.path.exists(FOREST):
        print("  origin-shift  SKIP  (forest not built)")
        return failures

    shift20 = ["--origin-shift-at", "20"]
    moved = {}
    for label, dist in (("near", ORIGIN_NEAR), ("far", ORIGIN_FAR)):
        plain, _ = _origin_forest(workdir, f"{label}_plain", dist, [])
        shifted, log = _origin_forest(workdir, f"{label}_shift", dist, shift20)
        if not (plain and shifted):
            print(f"  origin-shift  ERROR while rendering forest at {dist:.0f}")
            return failures + ["origin-shift"]
        # An arm asserting a shift changed nothing is satisfied perfectly by a
        # shift that never happened, so the log line is read, not assumed.
        fired = sum(1 for ln in log.splitlines() if "origin shift:" in ln)
        moved[label] = (_origin_diff(plain, shifted)[0], fired)

    near_frac, near_fired = moved["near"]
    far_frac, far_fired = moved["far"]

    ok = near_fired == 1 and near_frac <= ORIGIN_SHIFT_MAX
    print(f"  origin-shift  {'PASS' if ok else 'FAIL'}  at {ORIGIN_NEAR:.0f} a shift moves "
          f"{100.0 * near_frac:.2f}% of the frame (want <= {100.0 * ORIGIN_SHIFT_MAX:.0f}%; a splat "
          f"or a camera left behind reads 76%), and fired {near_fired} time (want exactly 1)")
    if not ok:
        failures.append("origin-shift")

    ok = far_fired == 1 and far_frac >= ORIGIN_DEGRADE_MIN
    print(f"  origin-degrade {'PASS' if ok else 'FAIL'}  at {ORIGIN_FAR:.0f} the same shift moves "
          f"{100.0 * far_frac:.2f}% (want >= {100.0 * ORIGIN_DEGRADE_MIN:.0f}%: that is the "
          f"precision it recovers, and without it the arm above passes on a no-op)")
    if not ok:
        failures.append("origin-degrade")

    # --- Rule 1: the shift frame must not print the translation as motion -----
    # Render mode 9 is the velocity buffer, where a previous-frame transform that
    # missed the delta is the only thing that can spike. ONE pair of runs with
    # --screenshot-every, not one run per frame: forest bakes a terrain and
    # scatters 5,000 props at startup, so a per-frame run measures the startup.
    #
    # Three frames rather than two, which the single pair also buys. 19 is BEFORE
    # the shift, so the two runs are the same run and must agree exactly -- a
    # control that says they differ in nothing but the shift.
    plain, _ = _origin_forest(workdir, "vel_plain", ORIGIN_NEAR, ["--screenshot-every", "1"],
                              "24", "9", ("400", "300"))
    shifted, _ = _origin_forest(workdir, "vel_shift", ORIGIN_NEAR,
                                shift20 + ["--screenshot-every", "1"], "24", "9", ("400", "300"))
    if not (plain and shifted):
        print("  origin-velocity ERROR while rendering the velocity buffer")
        return failures + ["origin-velocity"]

    vel = {}
    for f in (19, 21, 23):
        pa = plain[:-4] + f"_{f:06d}.ppm"
        pb = shifted[:-4] + f"_{f:06d}.ppm"
        if not (os.path.exists(pa) and os.path.exists(pb)):
            print(f"  origin-velocity ERROR frame {f} was not captured")
            return failures + ["origin-velocity"]
        vel[f] = _origin_diff(pa, pb)[0]

    # 21 is the first frame after the shift; 23 is the same pair two frames later,
    # still differing by precision alone. The ratio isolates the TRANSITION from
    # the steady state either side of it.
    steady = max(vel[23], 1e-6)
    ratio = vel[21] / steady
    ok = ratio <= ORIGIN_VELOCITY_MAX_RATIO and vel[19] == 0.0
    print(f"  origin-velocity {'PASS' if ok else 'FAIL'}  before the shift the two runs agree at "
          f"{100.0 * vel[19]:.2f}% (want exactly 0); the shift frame moves {100.0 * vel[21]:.2f}% "
          f"of the velocity buffer against a steady {100.0 * steady:.2f}% two frames later, ratio "
          f"{ratio:.2f} (want <= {ORIGIN_VELOCITY_MAX_RATIO}; leaving prev_view_proj uncorrected "
          f"reads 4.48)")
    if not ok:
        failures.append("origin-velocity")

    # --- Phase 4: the character crosses the shift still standing ---------------
    # Read as HEIGHT ABOVE TERRAIN. Both terms move -- the capsule by the delta,
    # the ground because the terrain's centre moved with it -- so their difference
    # is the only thing here that is supposed to be invariant, and a body left
    # behind breaks it while leaving both terms individually plausible.
    rows = _origin_trace(ORIGIN_NEAR, ["--origin-shift-at", "40"])
    if rows is None:
        print("  origin-physics ERROR while tracing the player")
        return failures + ["origin-physics"]
    before = [r for r in rows if r["t"] < 0.7]
    after = [r for r in rows if r["t"] > 0.9]
    if not before or not after:
        print(f"  origin-physics ERROR trace has {len(before)} samples before the shift and "
              f"{len(after)} after (want at least one of each)")
        return failures + ["origin-physics"]

    b, a = before[-1], after[-1]
    clearance_drift = abs((a["y"] - a["terrain"]) - (b["y"] - b["terrain"]))
    x_moved = abs((b["x"] - a["x"]) - ORIGIN_NEAR)
    ok = (clearance_drift <= ORIGIN_CLEARANCE_MAX and x_moved <= ORIGIN_CLEARANCE_MAX
          and a["grounded"] and b["grounded"])
    print(f"  origin-physics {'PASS' if ok else 'FAIL'}  height above terrain {b['y'] - b['terrain']:.3f} "
          f"before the shift and {a['y'] - a['terrain']:.3f} after (drift {clearance_drift:.3f}, want "
          f"<= {ORIGIN_CLEARANCE_MAX}); x moved {b['x'] - a['x']:.1f} of {ORIGIN_NEAR:.0f}; grounded "
          f"both sides {bool(b['grounded'])}/{bool(a['grounded'])}")
    if not ok:
        failures.append("origin-physics")

    # --- Phase 5: the engine re-centres on its own ----------------------------
    # At ORIGIN_AUTO the camera starts outside the threshold, so a correct engine
    # shifts once on the first frame and never again. Compared against the
    # hand-driven shift rather than against a remembered number: both take the
    # world to the same origin, so if the automatic trigger is the same mechanism
    # the two frames are the SAME frame.
    # --no-trail on BOTH legs, and only here: this is the suite's only exact-zero
    # comparison between two runs that shifted by different amounts, which is the
    # one bar a road can break. See _origin_forest's docstring for the mechanism
    # and for why every other origin arm keeps the trail.
    manual, _ = _origin_forest(workdir, "auto_manual", ORIGIN_AUTO,
                               ["--origin-shift-at", "20", "--no-trail"])
    auto, autolog = _origin_forest(workdir, "auto_auto", ORIGIN_AUTO,
                                   ["--origin-shift-distance", repr(ORIGIN_AUTO_THRESHOLD),
                                    "--no-trail"])
    if not (manual and auto):
        print("  origin-auto   ERROR while rendering the automatic shift")
        return failures + ["origin-auto"]
    shifts = sum(1 for ln in autolog.splitlines() if "origin shift:" in ln)
    frac, worst = _origin_diff(manual, auto)
    ok = shifts == 1 and frac == 0.0
    print(f"  origin-auto   {'PASS' if ok else 'FAIL'}  fired {shifts} time(s) over 40 frames "
          f"(want exactly 1; an oscillating threshold fires ~30), and reaches the hand-driven "
          f"shift's frame at {100.0 * frac:.2f}% differing, worst {worst} (want exactly 0)")
    if not ok:
        failures.append("origin-auto")

    return failures
