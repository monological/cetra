#!/usr/bin/env python3
"""Generate assets/models/puppet.gltf + assets/scenes/puppet.cscn -- the animation blending instrument (spec 12.1).

A figure of twenty-two rigid boxes, one per bone, on the `cetra_rig:` bone names
the committed walk cycle (assets/models/strut_walk.fbx) carries, so that clip binds to
this rig by exact name. Every clip the blending gates read is authored HERE, in
closed form, so what a gate expects is a number this file states and not a
frame somebody looked at.

THE RIG. Y up, facing +Z, a T-pose, feet at y = 0, symmetric in x and z -- so
the render app's recentre (centre x/z to 0, min y to 0) moves it by exactly
nothing, which the generator asserts. Every joint's bind rotation is the
identity and its bind transform a pure translation from its parent; the skin's
joints are listed parent-first, which is the order the engine's skeleton takes
(import.c, process_ai_skeleton). Each box's vertices all weight their one bone
at 1.0: a limb follows its bone exactly, and no blend softens a wrong answer.

THE CLIPS, and what each is for:
  idle    2.0 s loop   a breathing sway; what plays with no flag
  walk    1.0 s loop   thighs +-30 deg about X in opposite phase, arms counter
  run     0.5 s loop   the same shape at +-50 / +-35
  jump    1.0 s once   legs tuck by 0.2 s, hold, back to bind at 1.0
  wave    1.0 s once   the right arm raised, the forearm waving; ends on bind
  hold90  4.0 s once   the left forearm to +90 deg about Z by 0.25 s, then HELD
  rest    4.0 s once   the bind pose as a clip (one channel), the other
                       endpoint of the analytic midpoint blend
  swim    1.6 s loop   a breaststroke: arms sweeping in the frontal plane
  travel_walk 1.0 s loop  walk's shape, and 1.20 m of +Z per loop
  travel_run  0.5 s loop  run's shape, and 1.60 m
  lunge   0.6 s once   one stride carrying 1.20 m, ending on the bind pose
  spin    1.0 s once   half a turn about Y on the spot, in 45-degree steps

The last four are what ROOT MOTION reads (spec 12.18): a clip states how far it
travels and the engine gives that to the character, where every other clip here
is in place and a game has to decide for itself how fast to move. walk and run
keep their own shape and stand beside them rather than being replaced, because a
golden plays run by name and half the anim arms expect the pose those two hold.

The loops are in COS phase: t = 0 and t = T/2 are the extremes, so frame 30 of
the render app's fixed 1/60 clock (t = 0.5 s) reads a full stride on walk and
run, not the crossing. hold90's last key is at 4.0 s: the render app plays
looping, so a clip that ended at the read frame would wrap to bind in exactly
the frame being measured -- the skinned_cull fixture's own lesson.

WHY EVERY ANIMATED JOINT CARRIES A TRANSLATION TRACK. An embedded clip is
imported without retargeting, and a channel with rotation keys but no position
keys reads position (0, 0, 0) -- which collapses the joint onto its parent. Each
animated joint therefore also carries a two-key translation sampler holding
its bind offset, and the generator asserts that pairing.

The bone names keep their colon through assimp's glTF path (as they do through
FBX); the `anim-clip-loads` gate arm is what notices if that ever stops.

Regenerate with: python3 assets/generators/gen_puppet_fixture.py
"""

import base64
import json
import math
import struct
from fixture_paths import asset_path, asset_ref

PREFIX = "cetra_rig:"

# (name, parent name or None, bind position in the rig's space, box centre,
# box half-extents). Left is +X, the character's own left when facing +Z.
BONES = [
    ("Hips", None, (0.0, 0.95, 0.0), (0.0, 0.93, 0.0), (0.14, 0.07, 0.09)),
    ("Spine", "Hips", (0.0, 1.05, 0.0), (0.0, 1.115, 0.0), (0.12, 0.065, 0.08)),
    ("Spine1", "Spine", (0.0, 1.18, 0.0), (0.0, 1.245, 0.0), (0.13, 0.065, 0.08)),
    ("Spine2", "Spine1", (0.0, 1.31, 0.0), (0.0, 1.40, 0.0), (0.15, 0.09, 0.09)),
    ("Neck", "Spine2", (0.0, 1.50, 0.0), (0.0, 1.54, 0.0), (0.04, 0.04, 0.04)),
    ("Head", "Neck", (0.0, 1.58, 0.0), (0.0, 1.69, 0.0), (0.09, 0.11, 0.10)),
    ("LeftShoulder", "Spine2", (0.06, 1.46, 0.0), (0.13, 1.46, 0.0), (0.07, 0.05, 0.05)),
    ("LeftArm", "LeftShoulder", (0.20, 1.46, 0.0), (0.34, 1.46, 0.0), (0.14, 0.045, 0.045)),
    ("LeftForeArm", "LeftArm", (0.48, 1.46, 0.0), (0.61, 1.46, 0.0), (0.13, 0.04, 0.04)),
    ("LeftHand", "LeftForeArm", (0.74, 1.46, 0.0), (0.80, 1.46, 0.0), (0.06, 0.03, 0.04)),
    ("RightShoulder", "Spine2", (-0.06, 1.46, 0.0), (-0.13, 1.46, 0.0), (0.07, 0.05, 0.05)),
    ("RightArm", "RightShoulder", (-0.20, 1.46, 0.0), (-0.34, 1.46, 0.0), (0.14, 0.045, 0.045)),
    ("RightForeArm", "RightArm", (-0.48, 1.46, 0.0), (-0.61, 1.46, 0.0), (0.13, 0.04, 0.04)),
    ("RightHand", "RightForeArm", (-0.74, 1.46, 0.0), (-0.80, 1.46, 0.0), (0.06, 0.03, 0.04)),
    ("LeftUpLeg", "Hips", (0.10, 0.90, 0.0), (0.10, 0.69, 0.0), (0.075, 0.21, 0.075)),
    ("LeftLeg", "LeftUpLeg", (0.10, 0.48, 0.0), (0.10, 0.28, 0.0), (0.065, 0.20, 0.065)),
    ("LeftFoot", "LeftLeg", (0.10, 0.08, 0.0), (0.10, 0.04, -0.04), (0.06, 0.04, 0.08)),
    ("RightUpLeg", "Hips", (-0.10, 0.90, 0.0), (-0.10, 0.69, 0.0), (0.075, 0.21, 0.075)),
    ("RightLeg", "RightUpLeg", (-0.10, 0.48, 0.0), (-0.10, 0.28, 0.0), (0.065, 0.20, 0.065)),
    ("RightFoot", "RightLeg", (-0.10, 0.08, 0.0), (-0.10, 0.04, -0.04), (0.06, 0.04, 0.08)),
    # The toes (spec 12.9), APPENDED rather than filed beside their own feet: a bone
    # inserted mid-table renumbers every one after it, and the order is what the engine
    # accumulates globals in. At the end their parents are still earlier, which is the
    # only property the table has to have.
    #
    # They take the front of the foot rather than extending it -- the foot box gave up
    # the z it hands over -- because the mesh is asserted symmetric in z below, and a toe
    # that reached further forward than the heel reaches back would fire that assert. The
    # sole stays at y = 0 on both boxes, which is the bind pose ik_add_foot derives
    # `sole_offset` from, and the ankle bind is untouched so that offset is unchanged.
    ("LeftToeBase", "LeftFoot", (0.10, 0.04, 0.04), (0.10, 0.02, 0.08), (0.05, 0.02, 0.04)),
    ("RightToeBase", "RightFoot", (-0.10, 0.04, 0.04), (-0.10, 0.02, 0.08), (0.05, 0.02, 0.04)),
]
NAMES = [PREFIX + b[0] for b in BONES]
INDEX = {n: i for i, n in enumerate(NAMES)}
PARENT = [INDEX[PREFIX + b[1]] if b[1] else -1 for b in BONES]
BIND = [b[2] for b in BONES]

# Parent-first: the engine accumulates globals in array order.
for i, p in enumerate(PARENT):
    assert p < i, "joints must be listed parent-first"

# Local bind translation = own bind position minus the parent's.
LOCAL = [tuple(BIND[i][k] - (BIND[PARENT[i]][k] if PARENT[i] >= 0 else 0.0) for k in range(3))
         for i in range(len(BONES))]


def quat_axis(axis, deg):
    """Quaternion (x, y, z, w) for `deg` degrees about a unit axis."""
    h = math.radians(deg) * 0.5
    s = math.sin(h)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h))


X, Y, Z = (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)
IDENT = (0.0, 0.0, 0.0, 1.0)

# ---------------------------------------------------------------------------
# Geometry: one box per bone, flat normals, outward winding.
# ---------------------------------------------------------------------------

FACES = [  # (normal, u, v): corners at c + n*hn +- u*hu +- v*hv, CCW seen from outside
    ((1, 0, 0), (0, 1, 0), (0, 0, 1)),
    ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
    ((0, 1, 0), (0, 0, 1), (1, 0, 0)),
    ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
    ((0, 0, 1), (1, 0, 0), (0, 1, 0)),
    ((0, 0, -1), (0, 1, 0), (1, 0, 0)),
]

positions, normals, uvs, joints, weights, indices = [], [], [], [], [], []
for bone_index, (_, _, _, centre, half) in enumerate(BONES):
    for n, u, v in FACES:
        base = len(positions)
        hn = sum(abs(n[k]) * half[k] for k in range(3))
        hu = sum(abs(u[k]) * half[k] for k in range(3))
        hv = sum(abs(v[k]) * half[k] for k in range(3))
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            positions.append(tuple(centre[k] + n[k] * hn + su * u[k] * hu + sv * v[k] * hv
                                   for k in range(3)))
            normals.append(tuple(float(c) for c in n))
            uvs.append(((su + 1) * 0.5, (sv + 1) * 0.5))
            joints.append((bone_index, 0, 0, 0))
            weights.append((1.0, 0.0, 0.0, 0.0))
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
        # The winding trap: every face's normal must point away from the box's
        # centre, or the box is inside-out and casts as nothing.
        a, b, c = positions[base], positions[base + 1], positions[base + 2]
        e1 = tuple(b[k] - a[k] for k in range(3))
        e2 = tuple(c[k] - a[k] for k in range(3))
        cross = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                 e1[0] * e2[1] - e1[1] * e2[0])
        assert sum(cross[k] * n[k] for k in range(3)) > 0, "face wound inside-out"

# Every joint drives at least one vertex, so assimp makes a bone of each.
assert set(j[0] for j in joints) == set(range(len(BONES)))

mn = [min(p[k] for p in positions) for k in range(3)]
mx = [max(p[k] for p in positions) for k in range(3)]
assert abs(mn[1]) < 1e-9, "feet must stand on y = 0"
assert abs(mn[0] + mx[0]) < 1e-9 and abs(mn[2] + mx[2]) < 1e-9, "not centred in x/z"

# Inverse bind matrices: the joints are pure translations under an identity
# root, so the inverse is a translation by -bind. Column-major.
ibms = []
for pos in BIND:
    ibms.append([1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0,
                 -pos[0], -pos[1], -pos[2], 1.0])

# ---------------------------------------------------------------------------
# Clips. A clip is {joint name: [(time, quat), ...]} plus an optional Hips
# TRANSLATION curve; every animated joint gets its bind translation as a track.
#
# That curve used to be a height alone, which is all a bob needs. It carries all
# three components since spec 12.18, because a root that moves horizontally is
# what root motion reads: the engine takes the displacement between the clip's
# first tick and its last and gives it to the character, so a clip states how
# far it travels instead of a game guessing. A clip whose curve only rises and
# falls is still in place, and the engine says so.
# ---------------------------------------------------------------------------

KEYS_PER_LOOP = 8


def loop_keys(period, fn):
    """(time, quat) at KEYS_PER_LOOP + 1 points, the last equal to the first."""
    out = []
    for k in range(KEYS_PER_LOOP):
        t = period * k / KEYS_PER_LOOP
        out.append((t, fn(2.0 * math.pi * k / KEYS_PER_LOOP)))
    # The last key IS the first, not a sin(2 pi) that is 1e-16 off it.
    out.append((period, out[0][1]))
    return out


def hips_curve(period, bob, travel=0.0, keys=KEYS_PER_LOOP, phase=2.0):
    """The Hips' own translation track: a vertical bob, and `travel` metres of +Z.

    The forward part is LINEAR in time, which is not what a real walk's root does
    -- a foot pushing off drives the hips in surges. It is what makes the number
    exact: the engine samples the clip's ends and subtracts, and a linear ramp
    between them travels its stated distance under any interpolation, so a clip
    says 1.20 m and the character moves 1.20 m with nothing to round.
    """
    out = []
    for k in range(keys + 1):
        f = k / keys
        out.append((period * f,
                    (LOCAL[0][0],
                     BIND[0][1] + bob * math.cos(phase * 2.0 * math.pi * f),
                     LOCAL[0][2] + travel * f)))
    return out


def gait(period, thigh_deg, arm_deg, bob, travel=0.0):
    rot = {
        "LeftUpLeg": loop_keys(period, lambda p: quat_axis(X, thigh_deg * math.cos(p))),
        "RightUpLeg": loop_keys(period, lambda p: quat_axis(X, -thigh_deg * math.cos(p))),
        "LeftArm": loop_keys(period, lambda p: quat_axis(Y, -arm_deg * math.cos(p))),
        "RightArm": loop_keys(period, lambda p: quat_axis(Y, arm_deg * math.cos(p))),
    }
    return {"loop": True, "length": period, "rot": rot,
            "hips": hips_curve(period, bob, travel)}


def idle():
    period = 2.0
    rot = {
        "Spine2": loop_keys(period, lambda p: quat_axis(X, 3.0 * math.sin(p))),
        "Head": loop_keys(period, lambda p: quat_axis(X, 2.0 * math.sin(p + math.pi * 0.5))),
    }
    # A quarter turn of phase, so the breath rises and falls once rather than twice,
    # and the curve is a sine where a gait's is a cosine.
    hips = [(period * k / KEYS_PER_LOOP,
             (LOCAL[0][0], BIND[0][1] + 0.01 * math.sin(2.0 * math.pi * k / KEYS_PER_LOOP),
              LOCAL[0][2]))
            for k in range(KEYS_PER_LOOP + 1)]
    return {"loop": True, "length": period, "rot": rot, "hips": hips}


def jump():
    tuck = [(0.0, IDENT), (0.2, quat_axis(X, -60.0)), (0.7, quat_axis(X, -60.0)), (1.0, IDENT)]
    knee = [(0.0, IDENT), (0.2, quat_axis(X, 70.0)), (0.7, quat_axis(X, 70.0)), (1.0, IDENT)]
    rot = {
        "LeftUpLeg": tuck, "RightUpLeg": tuck,
        "LeftLeg": knee, "RightLeg": knee,
        "LeftArm": [(0.0, IDENT), (0.2, quat_axis(Z, 60.0)), (0.7, quat_axis(Z, 60.0)),
                    (1.0, IDENT)],
        "RightArm": [(0.0, IDENT), (0.2, quat_axis(Z, -60.0)), (0.7, quat_axis(Z, -60.0)),
                     (1.0, IDENT)],
    }
    return {"loop": False, "length": 1.0, "rot": rot, "hips": None}


def wave():
    arm = [(0.0, IDENT), (0.15, quat_axis(Z, -80.0)), (0.85, quat_axis(Z, -80.0)), (1.0, IDENT)]
    fore = [(0.0, IDENT), (0.25, quat_axis(Z, 25.0))]
    sign = -1.0
    t = 0.375
    while t <= 0.75 + 1e-9:
        fore.append((t, quat_axis(Z, 25.0 * sign)))
        sign = -sign
        t += 0.125
    fore.append((1.0, IDENT))
    return {"loop": False, "length": 1.0, "rot": {"RightArm": arm, "RightForeArm": fore},
            "hips": None}


def swim(period=1.6, arm_deg=55.0, knee_deg=40.0, thigh_deg=18.0, bob=0.015):
    """A breaststroke, and a new function rather than another gait() call.

    gait swings the arms about Y -- across the body, which is what walking does. A
    stroke sweeps them out and back in the frontal plane, which is Z, and the legs
    fold rather than swing, which is X at the knee. Nothing in gait's shape survives
    that, so sharing it would cost more parameters than writing this.

    Mirrored left to right rather than counter-phased: both arms pull together and
    both knees fold together, which is what makes it read as a stroke and not a walk
    performed underwater.
    """
    rot = {
        # Out on the catch, in on the recovery. Opposite signs so the pair sweeps
        # symmetrically away from the midline.
        "LeftArm": loop_keys(period, lambda p: quat_axis(Z, arm_deg * math.sin(p))),
        "RightArm": loop_keys(period, lambda p: quat_axis(Z, -arm_deg * math.sin(p))),
        # The kick trails the pull by half a cycle, which is the whole rhythm of a
        # breaststroke: arms pull, then legs drive.
        "LeftLeg": loop_keys(period, lambda p: quat_axis(X, knee_deg * (1.0 - math.cos(p)))),
        "RightLeg": loop_keys(period, lambda p: quat_axis(X, knee_deg * (1.0 - math.cos(p)))),
        "LeftUpLeg": loop_keys(period, lambda p: quat_axis(X, -thigh_deg * math.sin(p))),
        "RightUpLeg": loop_keys(period, lambda p: quat_axis(X, -thigh_deg * math.sin(p))),
    }
    hips = [(period * k / KEYS_PER_LOOP,
             (LOCAL[0][0], BIND[0][1] + bob * math.sin(2.0 * math.pi * k / KEYS_PER_LOOP),
              LOCAL[0][2]))
            for k in range(KEYS_PER_LOOP + 1)]
    return {"loop": True, "length": period, "rot": rot, "hips": hips}


# What the travelling clips state, in metres of +Z per play. The rig faces +Z, so
# these are forward. They are the whole of what spec 12.18 asserts: the engine reads
# them off the clip and the character moves exactly this far, so a gate compares a
# distance walked against a number declared here.
#
# The pair is chosen so a blend between them is not their mean at either end and the
# run is not a multiple of the walk: 1.20 m per 1.0 s and 1.60 m per 0.5 s, which is
# 1.20 and 3.20 m/s. A run that merely doubled the walk would let a weighted-mean
# blend pass by coincidence.
TRAVEL_WALK = 1.20
TRAVEL_RUN = 1.60
TRAVEL_LUNGE = 1.20
SPIN_DEG = 180.0


def lunge():
    """A one-shot that travels a fixed distance -- what stride matching cannot do.

    Its legs are a single stride rather than a loop, and it ends on the bind pose so
    the animator's return to the locomotion space has nothing to cover.
    """
    step = [(0.0, IDENT), (0.25, quat_axis(X, 45.0)), (0.6, IDENT)]
    back = [(0.0, IDENT), (0.25, quat_axis(X, -30.0)), (0.6, IDENT)]
    rot = {
        "LeftUpLeg": step, "RightUpLeg": back,
        "LeftArm": [(0.0, IDENT), (0.25, quat_axis(Y, -35.0)), (0.6, IDENT)],
        "RightArm": [(0.0, IDENT), (0.25, quat_axis(Y, 35.0)), (0.6, IDENT)],
    }
    return {"loop": False, "length": 0.6, "rot": rot,
            "hips": hips_curve(0.6, 0.03, TRAVEL_LUNGE, keys=6, phase=1.0)}


def spin():
    """Half a turn on the spot: the yaw half of root motion with no travel at all.

    Authored in 45-degree steps rather than one key to 180. A single hop of exactly
    half a turn has no shorter arc -- the two directions are the same length -- so
    which way it goes is whatever the interpolator decides, and a clip that states a
    turn should state its direction too.
    """
    keys = [(0.0, IDENT)]
    for k in range(1, 5):
        keys.append((k * 0.25, quat_axis(Y, SPIN_DEG * k / 4.0)))
    return {"loop": False, "length": 1.0, "rot": {"Hips": keys},
            "hips": hips_curve(1.0, 0.0, 0.0, keys=4, phase=1.0)}


def hold90():
    keys = [(0.0, IDENT), (0.25, quat_axis(Z, 90.0)), (4.0, quat_axis(Z, 90.0))]
    return {"loop": False, "length": 4.0, "rot": {"LeftForeArm": keys}, "hips": None}


def rest():
    return {"loop": False, "length": 4.0, "rot": {"Hips": [(0.0, IDENT), (4.0, IDENT)]},
            "hips": None}


CLIPS = [
    ("idle", idle()),
    ("walk", gait(1.0, 30.0, 20.0, 0.02)),
    ("run", gait(0.5, 50.0, 35.0, 0.04)),
    ("jump", jump()),
    ("wave", wave()),
    ("hold90", hold90()),
    ("rest", rest()),
    # Appended, and that is the cheap place: the packer mints chunks in this order with
    # cumulative offsets, so a clip added here leaves every existing byteOffset, accessor
    # and animation index untouched. Every consumer binds by NAME through accessor
    # indirection, so no existing clip's sampled values move either.
    ("swim", swim()),
    # Spec 12.18, appended for the same reason. walk and run are left exactly as they
    # are and these stand beside them rather than replacing them: a golden plays run by
    # name, and half the anim arms expect the pose those two hold.
    ("travel_walk", gait(1.0, 30.0, 20.0, 0.02, TRAVEL_WALK)),
    ("travel_run", gait(0.5, 50.0, 35.0, 0.04, TRAVEL_RUN)),
    ("lunge", lunge()),
    ("spin", spin()),
]

READ_FRAME_T = 0.5  # frame 30 at 1/60
for name, clip in CLIPS:
    for joint, keys in clip["rot"].items():
        assert keys[0][0] == 0.0 and abs(keys[-1][0] - clip["length"]) < 1e-9, (name, joint)
        if clip["loop"]:
            assert keys[0][1] == keys[-1][1], "a loop must end where it starts: " + name
    if not clip["loop"]:
        assert READ_FRAME_T < clip["length"], "a one-shot must still be going at the read frame"
assert CLIPS[0][0] == "idle", "the render app plays index 0"
assert abs(hold90()["rot"]["LeftForeArm"][-1][0] - 4.0) < 1e-9

# What each clip STATES about its own displacement, which is what the engine reads off
# it: the hips' last key minus its first. Asserted here so the distance a gate expects
# and the distance the file carries are the same statement rather than two.
#
# Everything not listed travels 0, and that is the load-bearing half -- an in-place clip
# has to stay in place, or a character playing it would drift with nothing to see.
TRAVELS = {"travel_walk": TRAVEL_WALK, "travel_run": TRAVEL_RUN, "lunge": TRAVEL_LUNGE}
for name, clip in CLIPS:
    curve = clip["hips"]
    moved = (0.0, 0.0, 0.0) if curve is None else tuple(
        curve[-1][1][k] - curve[0][1][k] for k in range(3))
    want = TRAVELS.get(name, 0.0)
    assert abs(moved[2] - want) < 1e-9, (name, moved[2], want)
    assert abs(moved[0]) < 1e-9, ("a clip may only travel forward", name, moved[0])
    # The vertical returns to where it started even on a one-shot: a bob is not travel,
    # and a clip that ended higher than it began would state a climb nothing supports.
    assert abs(moved[1]) < 1e-9, ("a hips curve may not end at another height", name)
assert abs(spin()["rot"]["Hips"][-1][1][1] - math.sin(math.radians(SPIN_DEG) * 0.5)) < 1e-9

# ---------------------------------------------------------------------------
# Pack.
# ---------------------------------------------------------------------------

_chunks = []  # (bytes, target or None)


def _chunk(data, target=None):
    _chunks.append((data, target))
    return len(_chunks) - 1


pos_view = _chunk(b"".join(struct.pack("<3f", *p) for p in positions), 34962)
nrm_view = _chunk(b"".join(struct.pack("<3f", *n) for n in normals), 34962)
uv_view = _chunk(b"".join(struct.pack("<2f", *t) for t in uvs), 34962)
joint_view = _chunk(b"".join(struct.pack("<4H", *j) for j in joints), 34962)
weight_view = _chunk(b"".join(struct.pack("<4f", *w) for w in weights), 34962)
idx_view = _chunk(b"".join(struct.pack("<H", i) for i in indices), 34963)
ibm_view = _chunk(b"".join(struct.pack("<16f", *m) for m in ibms))

accessors = [
    {"bufferView": pos_view, "componentType": 5126, "count": len(positions), "type": "VEC3",
     "min": mn, "max": mx},
    {"bufferView": nrm_view, "componentType": 5126, "count": len(normals), "type": "VEC3"},
    {"bufferView": uv_view, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
    {"bufferView": joint_view, "componentType": 5123, "count": len(joints), "type": "VEC4"},
    {"bufferView": weight_view, "componentType": 5126, "count": len(weights), "type": "VEC4"},
    {"bufferView": idx_view, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
    {"bufferView": ibm_view, "componentType": 5126, "count": len(ibms), "type": "MAT4"},
]
ACC_POS, ACC_NRM, ACC_UV, ACC_JOINT, ACC_WEIGHT, ACC_IDX, ACC_IBM = range(7)

# Nodes: 0 the rig root, 1 the mesh, 2.. the joints in BONES order.
MESH_NODE = 1
JOINT_NODE0 = 2
nodes = [
    {"name": "puppet", "children": [MESH_NODE, JOINT_NODE0]},
    {"name": "puppet_mesh", "mesh": 0, "skin": 0},
]
for i, name in enumerate(NAMES):
    node = {"name": name, "translation": list(LOCAL[i])}
    kids = [JOINT_NODE0 + j for j, p in enumerate(PARENT) if p == i]
    if kids:
        node["children"] = kids
    nodes.append(node)


def _accessor(view, count, kind, mn_=None, mx_=None):
    acc = {"bufferView": view, "componentType": 5126, "count": count, "type": kind}
    if mn_ is not None:
        acc["min"] = mn_
        acc["max"] = mx_
    accessors.append(acc)
    return len(accessors) - 1


animations = []
for name, clip in CLIPS:
    samplers, channels = [], []
    animated = set(clip["rot"].keys())
    if clip["hips"] is not None:
        animated.add("Hips")
    for joint in sorted(animated, key=lambda j: INDEX[PREFIX + j]):
        node = JOINT_NODE0 + INDEX[PREFIX + joint]
        if joint in clip["rot"]:
            keys = clip["rot"][joint]
            times = [k[0] for k in keys]
            t_acc = _accessor(_chunk(b"".join(struct.pack("<f", t) for t in times)), len(times),
                              "SCALAR", [min(times)], [max(times)])
            r_acc = _accessor(_chunk(b"".join(struct.pack("<4f", *k[1]) for k in keys)),
                              len(keys), "VEC4")
            samplers.append({"input": t_acc, "output": r_acc, "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1,
                             "target": {"node": node, "path": "rotation"}})
        # The translation track: the bind offset held, or the hips' own curve.
        if joint == "Hips" and clip["hips"] is not None:
            curve = clip["hips"]
            times = [k[0] for k in curve]
            local_pos = [p for _, p in curve]
        else:
            times = [0.0, clip["length"]]
            local_pos = [LOCAL[INDEX[PREFIX + joint]]] * 2
        t_acc = _accessor(_chunk(b"".join(struct.pack("<f", t) for t in times)), len(times),
                          "SCALAR", [min(times)], [max(times)])
        p_acc = _accessor(_chunk(b"".join(struct.pack("<3f", *p) for p in local_pos)),
                          len(local_pos), "VEC3")
        samplers.append({"input": t_acc, "output": p_acc, "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1,
                         "target": {"node": node, "path": "translation"}})
    # Every rotation channel has a translation channel on the same node.
    rot_nodes = {c["target"]["node"] for c in channels if c["target"]["path"] == "rotation"}
    pos_nodes = {c["target"]["node"] for c in channels if c["target"]["path"] == "translation"}
    assert rot_nodes <= pos_nodes, name
    animations.append({"name": name, "samplers": samplers, "channels": channels})

buffer_bytes = b"".join(c for c, _ in _chunks)


def _views(chunks):
    views, offset = [], 0
    for data, target in chunks:
        v = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target is not None:
            v["target"] = target
        views.append(v)
        offset += len(data)
    return views


gltf = {
    "asset": {"version": "2.0", "generator": "gen_puppet_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": nodes,
    "skins": [{"inverseBindMatrices": ACC_IBM,
               "joints": [JOINT_NODE0 + i for i in range(len(BONES))],
               "skeleton": JOINT_NODE0}],
    "meshes": [
        {"name": "puppet_mesh",
         "primitives": [{"attributes": {"POSITION": ACC_POS, "NORMAL": ACC_NRM,
                                        "TEXCOORD_0": ACC_UV, "JOINTS_0": ACC_JOINT,
                                        "WEIGHTS_0": ACC_WEIGHT},
                         "indices": ACC_IDX, "material": 0}]},
    ],
    "animations": animations,
    "materials": [
        {"name": "puppet_wood",
         "pbrMetallicRoughness": {"baseColorFactor": [0.75, 0.55, 0.32, 1.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.7}},
    ],
    "accessors": accessors,
    "bufferViews": _views(_chunks),
    "buffers": [
        {"uri": "data:application/octet-stream;base64," +
                base64.b64encode(buffer_bytes).decode("ascii"),
         "byteLength": len(buffer_bytes)},
    ],
}

# The figure is 1.8 m; at 4.2 m a 45-degree view sees 3.5 m, so it fills half
# the frame. The light comes over the camera's shoulder onto the +Z faces.
LIGHT = {"name": "PuppetSun", "type": "directional", "direction": [-0.35, -0.6, -0.72],
         "color": [1.0, 1.0, 1.0], "intensity": 3.0, "cast_shadows": False}
CAMERA = {"eye": [0.0, 0.95, 4.2], "target": [0.0, 0.95, 0.0], "fov": 45.0}
POST = {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False,
        "bloom": {"enabled": False}}

cscn = {
    "version": 1,
    "models": [{"path": asset_ref("puppet.gltf")}],
    "lights": [LIGHT],
    "camera": CAMERA,
    "post": POST,
}

with open(asset_path("puppet.gltf"), "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
with open(asset_path("puppet.cscn"), "w") as f:
    json.dump(cscn, f, indent=1)
    f.write("\n")
