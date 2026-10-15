"""silent's player's arms, in the sleeves of an olive field jacket, modelled, rigged and animated
in Blender for the birdcage's pickup (spec 13.55).

    /Applications/Blender.app/Contents/MacOS/Blender -b --factory-startup \
        -P apps/silent/tools/make_arms_blender.py -- [--preview raise@0.6,hold@0] \
        [--movie raise,hold,open,take,lower] [--no-export]

Builds everything from nothing, so nothing in a user's Blender reaches it, and writes:

  - assets/models/arms.glb: the arms, skinned, and their clips;
  - apps/silent/src/arms_clips.h, GENERATED: each clip's length and events, the door's swing in
    `open`, the pickup's framing and where the key rides in the right hand -- rerun this, never
    edit the header;
  - out/arms_blender/arms.blend, to open and look at; with --preview, the player's own view of
    each pose, lit by the lamp on the chest, tiled into out/arms_blender/preview/sheet.png; and
    with --movie, the clips played in turn into out/arms_blender/movie.mp4, the side view beside
    the eye's.

MODEL space is the player's BODY: the standing eye at the origin, x the player's left, y up, z the
way they face -- turned with their yaw, never pitched. Blender is z up; `bv` (from the cage's
script) is the one place the two meet.

THE RIG. `Root` at the eye. Under it, three bones that deform nothing: `View`, at the eye, its +Y
where the eye looks and its +Z up -- the game puts the camera on it while the arms are up, so a
clip's lean and look are authored with its hands -- `Cage`, the held cage's own frame at its
base's middle (its +Y up, its +Z out of its face), and `Lamp`, the flashlight's beam from its lens
on the chest, its +Y on the middle of the cage's face all through every clip. And each arm:
UpperArm, ForeArm, its
ForeTwist, Hand, and four bones a finger and the thumb. The hands are posed by reaching: a two-bone
solve from the shoulder to where the clip puts the wrist, its elbow turned away from the thumb, the
hand turned toward its forearm as far as the pose allows, each finger fanned and bent.

THE HANDS are a sculpted hand (assets/models/rigged_hand.glb, whose author and licence arms.glb
carries on) with its capped stub of wrist cut off and the forearm grown on from the cut as one
surface. The skin up the forearm follows ForeTwist, which takes the hand's turn about the forearm,
so the wrist only bends and the sleeve, on the plain ForeArm, never wrings.

THE PICKUP is authored for one framing: the eye standing PICKUP_DISTANCE from the cage's axis,
square to it, the cage's base PICKUP_DROP below the eye. The game settles the view there before
`raise`, and after `lower` the cage stands exactly where it stood. The cage's dimensions come from
make_birdcage_blender.py, which this imports; birdcage.glb is imported for the stills.

THE CLIPS are functions of time sampled at 30 fps: keyed hand, cage, lean and look poses run
through on monotone cubics, the eye leading and the fingers trailing. Every bone gets a
translation track, as the cat's do. Each run prints how far each wrist bends and turns, which is
the check that no pose asks a wrist for what a wrist cannot do, and --movie plays clips in turn.
"""

import argparse
import bisect
import math
import os
import re
import sys
from typing import NamedTuple

import bpy
import bmesh
from mathutils import Matrix, Quaternion, Vector
from mathutils.bvhtree import BVHTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_birdcage_blender as cage_model  # noqa: E402 -- the tools' own modules, found beside this one
from glb import read_glb  # noqa: E402
from make_birdcage_blender import BV, Parts, bv, finish, lathe, placed, smooth, srgb  # noqa: E402
from polyhaven import cached_path, fetch, polyhaven_urls  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT = os.path.join(ROOT, "out", "arms_blender")
GLB = os.path.join(ROOT, "assets", "models", "arms.glb")
HEADER = os.path.join(ROOT, "apps", "silent", "src", "arms_clips.h")
FPS = 30
BV4 = BV.to_4x4()


def mv(v):
    """A Blender-space vector in model space."""
    return Vector((v.x, v.z, -v.y))


# ---------------------------------------------------------------------------------------------
# The body, in metres from the standing eye

SHOULDER = Vector((0.185, -0.235, -0.07))  # the left shoulder joint; the right's is its mirror
HIP = Vector((0.0, -0.78, -0.06))          # what the upper body leans forward about
KNEE_DIP = 0.0045                          # metres the knees lower the hip per degree of lean
UPPER = 0.29                               # shoulder to elbow
FORE = 0.255                               # elbow to wrist
SIDES = (("L", 1.0), ("R", -1.0))          # the player's left is +x

# The pickup's framing: the eye PICKUP_DISTANCE from the cage's axis, the cage's base PICKUP_DROP
# below it -- the bench's top under a standing eye. Close, as somebody stands to pick a thing up:
# from 0.62 m the arms had to reach out straight to it.
PICKUP_DISTANCE = 0.5
PICKUP_DROP = 0.72
BENCH = Vector((0.0, -PICKUP_DROP, PICKUP_DISTANCE))
HOLD = Vector((0.0, -0.275, 0.36))         # where the held cage's base is, in front of the chest
CAGE_R = cage_model.CAGE_R + cage_model.BAR_WIRE  # how far its sides stand from its axis
CAGE_MID = 0.11                            # where the hands take it by its sides, up from its base
# The middle of the cage's face, in its frame: what the lamp is aimed at all through the clips.
FACE = Vector((0.0, cage_model.DOOR_Y, cage_model.DOOR_FRAME_R + cage_model.RING_WIRE))

# The flashlight is clipped to the jacket's left chest, out of the right hand's way: its lens here
# from the standing eye. It rides the body's lean, never the head's look.
LAMP_AT = Vector((0.10, -0.25, 0.05))
# Where it is aimed, from the middle of the cage's face, in the body's frame: low and to the left,
# so the light sits on the cage rather than being thrown up past it onto the wall behind.
LAMP_AIM = Vector((0.03, -0.05, 0.0))
FLASHLIGHT_GLB = os.path.join(ROOT, "assets", "models", "flashlight.glb")
# The lamp model's axes onto the Lamp bone's: its lens's -x along the bone's +Y, its up its +Z.
LAMP_TURN = Matrix(((0.0, 0.0, -1.0), (-1.0, 0.0, 0.0), (0.0, 1.0, 0.0))).to_4x4()

# The hand: a sculpted right hand and its rig, which `load_hand` brings in at HAND_LENGTH from the
# wrist to the middle finger's tip and measures in its own frame -- a along it from the wrist to
# the fingers, c across it toward the thumb, d out of its back, metres. The left is its mirror.
HAND_GLB = os.path.join(ROOT, "assets", "models", "rigged_hand.glb")
HAND_LENGTH = 0.185
DIGITS = ("Thumb", "Index", "Middle", "Ring", "Pinky")
FINGER_NAMES = DIGITS[1:]
HAND = {}  # what load_hand measured

# The sculpted hand ends in a capped stub a centimetre up its wrist. It is cut off just behind the
# wrist's joint, and the arm goes on from the cut as more of the same surface: rings widening from
# the cut's own outline into a forearm's oval, as far up as the sleeve hides it.
WRIST_CUT = -0.002
FOREARM_SKIN = 0.12              # how far up the forearm past the cut the skin goes
FOREARM_GROW = (0.0075, 0.008)   # how much wider and thicker than the cut it is there, metres
FOREARM_RINGS = (0.003, 0.007, 0.012, 0.018, 0.026, 0.036, 0.05, 0.068, 0.092, FOREARM_SKIN)


def hand_bone(source):
    """Which of this rig's bones a bone of the sculpted hand's rig is, by its name, or None."""
    # The wrist's short stub goes with the hand, under the cuff, so it rolls as the hand does.
    if re.match(r"^(pulse|hand)\.R", source):
        return "Hand"
    m = re.match(r"^(thumb|index|middle|ring|pinky)_(base|01|02|03)\.R_\d+$", source)
    if m:
        return m.group(1).capitalize() + {"base": "0", "01": "1", "02": "2", "03": "3"}[m.group(2)]
    return None


def load_hand():
    """The sculpted hand into HAND, its imported objects gone again: its mesh, its UVs, its skin
    weights by this rig's bone names and each digit's joints, in the hand's frame at real size;
    its material; and the credit its file carries, which arms.glb carries on."""
    before, before_actions = set(bpy.data.objects), set(bpy.data.actions)
    # Its file's bind pose is right as it stands; Blender's guess at another one wrecks it.
    bpy.ops.import_scene.gltf(filepath=HAND_GLB, guess_original_bind_pose=False)
    new = [o for o in bpy.data.objects if o not in before]
    rig = next(o for o in new if o.type == "ARMATURE")
    skin = max((o for o in new if o.type == "MESH"), key=lambda o: len(o.data.polygons))

    def head(pattern):
        pb = next(p for p in rig.pose.bones if re.match(pattern, p.name))
        return rig.matrix_world @ pb.head

    wrist = head(r"^hand\.R_\d+$")
    along = (head(r"^middle_01\.R_\d+$") - wrist).normalized()
    across = head(r"^index_01\.R_\d+$") - head(r"^pinky_01\.R_\d+$")
    across = (across - along * along.dot(across)).normalized()
    back = (-across).cross(along)  # a right hand's thumb is on its X's far side
    scale = HAND_LENGTH / (head(r"^middle_03\.R_end_\d+$") - wrist).length

    def local(p):
        q = p - wrist
        return Vector((q.dot(along), q.dot(across), q.dot(back))) * scale

    digits = {}
    for d in DIGITS:
        low = d.lower()
        joints = [local(head(rf"^{low}_{k}\.R_\d+$")) for k in ("base", "01", "02", "03")]
        joints.append(local(head(rf"^{low}_03\.R_end_\d+$")))
        dirs = [(joints[k + 1] - joints[k]).normalized() for k in range(4)]
        lengths = [(joints[k + 1] - joints[k]).length for k in range(4)]
        # Which way each bone bends: a finger's toward the palm, the thumb's across toward the
        # palm's middle.
        toward = []
        for k in range(4):
            want = (Vector((0.055, -0.01, -0.02)) - joints[k]) if d == "Thumb" else \
                Vector((0.0, 0.0, -1.0))
            toward.append((want - dirs[k] * want.dot(dirs[k])).normalized())
        digits[d] = {"joints": joints, "dirs": dirs, "lengths": lengths, "toward": toward}

    dg = bpy.context.evaluated_depsgraph_get()
    shown = skin.evaluated_get(dg)
    me = shown.to_mesh()
    posed = [local(skin.matrix_world @ v.co) for v in me.vertices]
    shown.to_mesh_clear()
    bm = bmesh.new()
    bm.from_mesh(skin.data)
    for v in bm.verts:
        v.co = posed[v.index]
    names = {g.index: hand_bone(g.name) for g in skin.vertex_groups}
    verts, faces, uvs, weights, cut = continue_arm(bm, names)
    bm.free()
    palm_len = digits["Middle"]["joints"][1].x
    grip = 0.55 * palm_len
    near = [v for v in verts if abs(v.x - grip) < 0.01 and abs(v.y) < 0.012]
    js, _ = read_glb(HAND_GLB)
    notes = js.get("asset", {}).get("extras", {})
    mat = skin.data.materials[0]
    mat.name = "arm_skin"
    for o in new:
        bpy.data.objects.remove(o)
    # Its own clip goes too, or the export carries it beside this rig's.
    for a in set(bpy.data.actions) - before_actions:
        bpy.data.actions.remove(a)
    HAND.update(verts=verts, faces=faces, uvs=uvs, weights=weights, digits=digits, cut=cut,
                material=mat, palm_len=palm_len, grip=grip,
                palm_side=min(v.z for v in near), back_side=max(v.z for v in near),
                credit=(f"Hands: \"{notes.get('title', '')}\" by {notes.get('author', '')}, "
                        f"{notes.get('source', '')}, {notes.get('license', '')}; modified. "
                        "This model is CC-BY-SA 4.0."))
    print(f"hand: {len(verts)} vertices, palm {palm_len:.3f} m, back of the hand "
          f"{HAND['back_side']:.4f} and palm {HAND['palm_side']:.4f} off its axis, the wrist cut "
          f"{2.0 * cut[0]:.3f} by {2.0 * cut[1]:.3f} m", flush=True)


def arm_weights(x, own):
    """A vertex's weights by where it is along the hand, `x` from the wrist's joint toward the
    fingers. Toward the fingers its own; across the wrist the hand's giving way to the forearm's
    twist, which turns the wrist as the forearm turns it so the wrist only bends; and up the arm
    the twist giving way to the forearm itself."""
    plain = smooth((0.015 - x) / 0.025)  # the wrist goes with the hand, not the thumb or pinky
    hand = smooth((x + 0.012) / 0.03)
    up = smooth((-x - 0.02) / (FOREARM_SKIN - 0.02))
    out = {n: w * (1.0 - plain) * hand for n, w in own.items()}
    out["Hand"] = out.get("Hand", 0.0) + plain * hand
    out["ForeTwist"] = (1.0 - hand) * (1.0 - up)
    out["ForeArm"] = (1.0 - hand) * up
    top = sorted(((n, w) for n, w in out.items() if w > 1e-4), key=lambda kv: -kv[1])[:4]
    total = sum(w for _, w in top)
    return {n: w / total for n, w in top}


def continue_arm(bm, names):
    """The sculpted hand with its stub of wrist cut off and the forearm carried on from the cut, in
    the hand's frame: (vertices, faces, each face's UVs, each vertex's weights by this rig's bone
    names, the cut's half-width and half-thickness). `names` maps the file's vertex groups to this
    rig's bones."""
    deform = bm.verts.layers.deform.active
    uv_layer = bm.loops.layers.uv.active
    # One vertex where the file has several along a seam, so the cut leaves one edge.
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-6)
    bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], dist=1e-7,
                           plane_co=(WRIST_CUT, 0.0, 0.0), plane_no=(1.0, 0.0, 0.0),
                           clear_inner=True)
    bm.verts.index_update()
    bm.verts.ensure_lookup_table()
    # The cut's edge, each vertex to the next in the order its face runs, with the face's UVs.
    nxt, uv_of = {}, {}
    for e in bm.edges:
        if len(e.link_faces) != 1:
            continue
        loop = next(lp for lp in e.link_faces[0].loops if lp.edge == e)
        a, b = loop.vert.index, loop.link_loop_next.vert.index
        nxt[a] = b
        uv_of[a] = (loop[uv_layer].uv.copy(), loop.link_loop_next[uv_layer].uv.copy())
    edge = [next(iter(nxt))]
    while nxt[edge[-1]] != edge[0]:
        edge.append(nxt[edge[-1]])
    if len(edge) != len(nxt):
        sys.exit(f"hand: the wrist's cut left {len(nxt)} open edges, only {len(edge)} in one loop")
    pts = [bm.verts[i].co.copy() for i in edge]
    centre = sum(pts, Vector()) / len(pts)
    half = (max(abs(p.y - centre.y) for p in pts), max(abs(p.z - centre.z) for p in pts))
    # Each vertex keeps its angle round the cut while the outline rounds out into an oval over
    # the first few centimetres; a column's UVs are its edge's, so the skin runs on unbroken.
    rings = [[bm.verts[i] for i in edge]]
    for d in FOREARM_RINGS:
        g = smooth(d / FOREARM_SKIN)
        a, b = half[0] + FOREARM_GROW[0] * g, half[1] + FOREARM_GROW[1] * g
        m = smooth(d / 0.04)
        ring = []
        for p in pts:
            q = p - centre
            th = math.atan2(q.z / half[1], q.y / half[0])
            r = Vector((0.0, q.y * a / half[0], q.z * b / half[1])).lerp(
                Vector((0.0, a * math.cos(th), b * math.sin(th))), m)
            ring.append(bm.verts.new((WRIST_CUT - d, centre.y + r.y, centre.z + r.z)))
        rings.append(ring)
    n = len(edge)
    for lo, hi in zip(rings, rings[1:]):
        for k in range(n):
            k2 = (k + 1) % n
            f = bm.faces.new((lo[k2], lo[k], hi[k], hi[k2]))
            ua, ub = uv_of[edge[k]]
            for lp, uv in zip(f.loops, (ub, ua, ua, ub)):
                lp[uv_layer].uv = uv
    end = bm.verts.new((WRIST_CUT - FOREARM_SKIN - 0.004, centre.y, centre.z))
    last = rings[-1]
    for k in range(n):
        f = bm.faces.new((last[(k + 1) % n], last[k], end))
        ua, _ = uv_of[edge[k]]
        for lp in f.loops:
            lp[uv_layer].uv = ua
    bm.verts.index_update()
    verts = [v.co.copy() for v in bm.verts]
    faces = [tuple(v.index for v in f.verts) for f in bm.faces]
    uvs = [tuple(tuple(lp[uv_layer].uv) for lp in f.loops) for f in bm.faces]
    weights = []
    for v in bm.verts:
        own = {}
        for g, w in v[deform].items():
            if names.get(g) and w > 0.0:
                own[names[g]] = own.get(names[g], 0.0) + w
        weights.append(arm_weights(v.co.x, own or {"Hand": 1.0}))
    return verts, faces, uvs, weights, half


def forearm_section(d):
    """The forearm's skin's half-width and half-thickness `d` metres up the arm from the wrist's
    cut."""
    g = smooth(d / FOREARM_SKIN)
    return HAND["cut"][0] + FOREARM_GROW[0] * g, HAND["cut"][1] + FOREARM_GROW[1] * g


def rot_x(a):
    return Matrix.Rotation(a, 3, "X")


def rot_y(a):
    return Matrix.Rotation(a, 3, "Y")


def frame(head, forward, up):
    """A bone frame at `head`: +Y along `forward`, +Z as near `up` as it can be, +X = Y x Z."""
    y = forward.normalized()
    z = up - y * up.dot(y)
    z = z.normalized() if z.length > 1e-8 else (Vector((0.0, 1.0, 0.0)) if abs(y.y) < 0.9
                                                 else Vector((0.0, 0.0, 1.0)))
    x = y.cross(z)
    m = Matrix((x, y, z)).transposed().to_4x4()
    m.translation = head
    return m


def two_bone(a, target, l1, l2, pole):
    v = target - a
    d = max(min(v.length, l1 + l2 - 1e-4), abs(l1 - l2) + 1e-4)
    u = v.normalized()
    w = pole - u * pole.dot(u)
    w = w.normalized() if w.length > 1e-6 else Vector((0.0, -1.0, 0.0))
    along = (l1 * l1 - l2 * l2 + d * d) / (2.0 * d)
    h = math.sqrt(max(l1 * l1 - along * along, 0.0))
    return a + u * along + w * h, a + u * d


# ---------------------------------------------------------------------------------------------
# Poses


class Hand(NamedTuple):
    """A hand: its wrist, where it points and which way its back faces, in BODY space; each
    finger's fan and three bends (`fingers`, by name); and the thumb's opposition, 0..1 rolling it
    across in front of the palm, and its two bends. A fan of 1 is the sculpted hand's splay and 0
    the fingers together; a bend is degrees toward the palm past the sculpted hand's own, which is
    flat."""
    wrist: Vector
    along: Vector
    back: Vector
    fingers: dict
    thumb: tuple


def shape(fan, index, middle, ring, pinky):
    """The four fingers: one fan for the hand and each finger's three bends."""
    return {n: (fan, *b) for n, b in zip(FINGER_NAMES, (index, middle, ring, pinky))}


# How far each finger turns about the back of the hand from the sculpted splay to lie beside the
# middle finger, degrees toward the thumb.
CLOSED_FAN = {"Index": -2.0, "Middle": 0.0, "Ring": 6.0, "Pinky": 11.0}

# A hand's shapes. Never flat: a relaxed hand curls more toward the little finger, its far joints
# following its near ones, and even opened to take something its fingers bend a little.
SCULPTED = shape(1.0, (0, 0, 0), (0, 0, 0), (0, 0, 0), (0, 0, 0))
RELAXED = shape(0.3, (16, 24, 14), (22, 30, 17), (28, 36, 20), (34, 40, 22))
RELAXED_THUMB = (0.45, 8.0, 12.0)
OPEN = shape(1.1, (6, 12, 6), (8, 12, 6), (11, 14, 7), (15, 16, 8))
OPEN_THUMB = (0.0, 2.0, 4.0)
CONTACT = shape(0.6, (16, 22, 11), (18, 24, 12), (20, 26, 13), (24, 28, 14))
CONTACT_THUMB = (0.25, 6.0, 5.0)
GRIP = shape(0.4, (28, 34, 18), (30, 36, 20), (32, 38, 21), (36, 40, 22))
GRIP_THUMB = (0.35, 12.0, 8.0)
CRADLE = shape(0.55, (8, 16, 9), (10, 18, 10), (13, 20, 11), (17, 22, 12))
CRADLE_THUMB = (-0.3, 8.0, 6.0)  # rolled down from under the base, clear of its rim
STEADY = shape(0.45, (18, 24, 13), (21, 27, 15), (25, 31, 17), (30, 34, 19))
STEADY_THUMB = (0.3, 8.0, 6.0)


class Frame(NamedTuple):
    """One instant of a clip: the lean forward, degrees; where the eye looks, its pitch in
    degrees; the cage's frame in body space; and the two hands."""
    lean: float
    pitch: float
    cage: Matrix
    hands: dict


def cage_frame(pos, yaw=0.0, tilt=0.0):
    """The cage at `pos`, its face turned to the eye and then `yaw` round its axis and `tilt`
    toward the eye, radians."""
    m = (rot_x(-tilt) @ rot_y(math.pi + yaw)).to_4x4()
    m.translation = pos
    return m


def hand_axes(hand, side):
    """A hand's frame: +Y along it, +Z out of its back, and the across axis toward its thumb."""
    y = hand.along.normalized()
    z = hand.back - y * hand.back.dot(y)
    z = z.normalized()
    x = y.cross(z)
    return y, z, x * side


def hand_point(hand, side, a, c, d):
    y, z, c_ax = hand_axes(hand, side)
    return hand.wrist + y * a + c_ax * c + z * d


THUMB_OPPOSE = 70.0  # degrees the thumb rolls under the palm at an opposition of 1


def digit_bends(hand, name):
    """A digit's four bones' bends, degrees: a finger's knuckle bone stays as the hand was made
    and its three joints bend; the thumb's two far joints bend, the nearer by some of the first's."""
    if name == "Thumb":
        _, a, b = hand.thumb
        return (0.0, 0.3 * a, a, b)
    return (0.0,) + tuple(hand.fingers[name][1:])


def finger_chain(hand, side, name):
    """A digit's joints from its base to its tip and each bone's (direction, back), bent from the
    sculpted hand's own pose: each bone turned about its joint's axis, in the hand's frame, the
    turns adding up down the digit, and a finger fanned at its knuckle about the back of the
    hand."""
    y, z, c_ax = hand_axes(hand, side)
    digit = HAND["digits"][name]

    def body(v):
        return y * v.x + c_ax * v.y + z * v.z

    # The thumb opposes by rolling at its base about the hand's length, across in front of the
    # palm toward the fingers; bending its joints toward the palm alone only folds it forward.
    turn = Matrix.Identity(3) if name != "Thumb" else Matrix.Rotation(
        math.radians(-THUMB_OPPOSE * hand.thumb[0]), 3, Vector((1.0, 0.0, 0.0)))
    p = digit["joints"][0].copy()
    joints, frames = [hand.wrist + body(p)], []
    for k, degrees in enumerate(digit_bends(hand, name)):
        u, w = digit["dirs"][k], digit["toward"][k]
        if k == 1 and name != "Thumb":
            fan = CLOSED_FAN[name] * (1.0 - hand.fingers[name][0])
            turn = turn @ Matrix.Rotation(math.radians(fan), 3, Vector((0.0, 0.0, 1.0)))
        turn = turn @ Matrix.Rotation(math.radians(degrees), 3, u.cross(w))
        d, b = turn @ u, turn @ -w
        p = p + d * digit["lengths"][k]
        joints.append(hand.wrist + body(p))
        frames.append((body(d).normalized(), body(b).normalized()))
    return joints, frames


_IRON = []
FINGER_R = (0.0095, 0.0095, 0.0085, 0.0075)  # a digit's half-thickness, bone by bone from its base
CLOSE_STEPS = 20


def iron():
    """The birdcage's solid parts in its model frame, as one BVH, from birdcage.glb, built once:
    what fingers close on. The door is left out: nothing grips it, and the forefinger that opens
    it is placed on it exactly."""
    if not _IRON:
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=cage_model.CAGE_GLB)
        new = set(bpy.data.objects) - before
        dg = bpy.context.evaluated_depsgraph_get()
        verts, polys = [], []
        for ob in new:
            if ob.type != "MESH" or re.sub(r"\.\d+$", "", ob.name) == "cage_door":
                continue
            ev = ob.evaluated_get(dg)
            me = ev.to_mesh()
            first = len(verts)
            verts += [mv(ob.matrix_world @ v.co) for v in me.vertices]
            polys += [[first + i for i in p.vertices] for p in me.polygons]
            ev.to_mesh_clear()
        for ob in new:
            bpy.data.objects.remove(ob)
        _IRON.append(BVHTree.FromPolygons(verts, polys))
    return _IRON[0]


def touches(joints, to_cage):
    """Whether a digit, the chain of its joints from its base, meets the iron: a capsule of
    FINGER_R round each bone from the knuckle out, the bone in the palm left to the palm."""
    tree = iron()
    for k in range(1, len(joints) - 1):
        a, b = to_cage @ joints[k], to_cage @ joints[k + 1]
        for s in (0.25, 0.5, 0.75, 1.0):
            p = a.lerp(b, s)
            loc, _, _, dist = tree.find_nearest(p)
            if loc is not None and (dist < FINGER_R[k] or (dist < 0.03 and inside(tree, p))):
                return True
    return False


def closed(touching, target):
    """How far toward `target` (a tuple, from all zeros) a digit goes: as fingers close on a
    thing, until it meets the iron and no further; or, starting in it, until it is clear of it."""
    at = lambda u: tuple(x * u for x in target)  # noqa: E731
    steps = [i / CLOSE_STEPS for i in range(CLOSE_STEPS + 1)]
    if touching(at(0.0)):
        return next((at(u) for u in steps if not touching(at(u))), at(1.0))
    reached = at(0.0)
    for u in steps[1:]:
        if touching(at(u)):
            break
        reached = at(u)
    return reached


def close_on(hand, side, cage, skip=()):
    """The hand's digits closed toward the shapes it asks for until each meets the cage, and no
    further. A shape authored by eye curls a finger straight into the iron, and nothing else
    stops it."""
    to_cage = cage.inverted()
    fingers = dict(hand.fingers)
    for n in FINGER_NAMES:
        if n in skip:
            continue
        fan, *bends = hand.fingers[n]

        def touching(b, n=n, fan=fan):
            trial = hand._replace(fingers={**fingers, n: (fan, *b)})
            return touches(finger_chain(trial, side, n)[0], to_cage)

        fingers[n] = (fan, *closed(touching, tuple(bends)))
    thumb = hand.thumb
    if "Thumb" not in skip:
        thumb = closed(lambda t: touches(finger_chain(hand._replace(thumb=t), side, "Thumb")[0],
                                         to_cage), hand.thumb)
    return hand._replace(fingers=fingers, thumb=thumb)


def wrist_for_tip(hand, side, name, tip):
    """The hand moved so the tip of `name` is at `tip`, turned and bent as it is."""
    joints, _ = finger_chain(hand, side, name)
    return hand._replace(wrist=hand.wrist + (tip - joints[-1]))


def wrist_for_grip(hand, side, at):
    """The hand moved so the middle of its palm is at `at`."""
    palm = hand_point(hand, side, HAND["grip"], 0.0, HAND["palm_side"])
    return hand._replace(wrist=hand.wrist + (at - palm))


def leaned(p, lean):
    """A point of the upper body after it leans forward `lean` degrees about the hip, the knees
    bending with it: a body bending to the bench, never one tipping over stiff legs."""
    return HIP - Vector((0.0, KNEE_DIP * lean, 0.0)) + rot_x(math.radians(lean)) @ (p - HIP)


SHOULDER_REACH = 0.045  # how far a shoulder comes forward for a far reach, metres


def pole(side, lean, hand):
    """Where an elbow goes: always somewhat down, and away from the thumb -- out for a palm turned
    down, in toward the ribs for one turned up -- so the forearm takes the hand's turn with no
    more twist than an arm has."""
    _, _, thumb = hand_axes(hand, side)
    return rot_x(math.radians(lean)) @ Vector((0.25 * side, -1.0, -0.2)) - 0.7 * thumb


def arm_of(hand, side, lean):
    """An arm's shoulder, elbow and wrist for a hand: the wrist where the hand asks, as far as the
    arm reaches."""
    shoulder = shoulder_at(side, lean, hand.wrist)
    elbow, wrist = two_bone(shoulder, hand.wrist, UPPER, FORE, pole(side, lean, hand))
    return shoulder, elbow, wrist


def settle(hand, side, lean, place, give):
    """A hand turned `give` of the way from how it was posed toward the line of its forearm, and
    put back where it has to be by `place`, which moves only its wrist: the wrist bends no further
    than the pose needs. The forearm moves with the hand, so this is found a few times over."""
    y0, z0, _ = hand_axes(hand, side)
    settled = place(hand)
    for _ in range(5):
        _, elbow, wrist = arm_of(settled, side, lean)
        turn = Quaternion().slerp(y0.rotation_difference((wrist - elbow).normalized()), give)
        settled = place(hand._replace(along=turn @ y0, back=turn @ z0))
    return settled


def shoulder_at(side, lean, wrist):
    """A shoulder joint, drawn toward a far reach as the shoulder blade slides forward round the
    ribs, so a reaching arm does not lock straight."""
    rest = leaned(Vector((SHOULDER.x * side, SHOULDER.y, SHOULDER.z)), lean)
    to = wrist - rest
    span = UPPER + FORE
    return rest + to.normalized() * (SHOULDER_REACH * smooth((to.length - 0.72 * span) /
                                                              (0.28 * span)))


def twist_frame(fore, hand, at):
    """The forearm's frame turned about its length as far as the hand is turned about it: the turn
    a forearm takes along itself, which leaves the wrist only its bend."""
    qf = fore.to_quaternion()
    q = qf.inverted() @ hand.to_quaternion()
    tw = Quaternion((q.w, 0.0, q.y, 0.0))
    tw = tw.normalized() if tw.magnitude > 1e-6 else Quaternion()
    m = (qf @ tw).to_matrix().to_4x4()
    m.translation = at
    return m


def solve(f):
    """Every bone's frame in BODY space for one instant."""
    M = {"Root": frame(Vector(), Vector((0.0, 0.0, 1.0)), Vector((0.0, 1.0, 0.0)))}
    eye = leaned(Vector(), f.lean)
    p = math.radians(f.pitch)
    M["View"] = frame(eye, Vector((0.0, math.sin(p), math.cos(p))),
                      Vector((0.0, math.cos(p), -math.sin(p))))
    M["Cage"] = f.cage.copy()
    lamp = leaned(LAMP_AT, f.lean)
    M["Lamp"] = frame(lamp, f.cage @ FACE + LAMP_AIM - lamp, Vector((0.0, 1.0, 0.0)))
    for s, side in SIDES:
        hand = f.hands[s]
        shoulder, elbow, wrist = arm_of(hand, side, f.lean)
        y, z, _ = hand_axes(hand, side)
        bend_n = (elbow - shoulder).cross(wrist - elbow)
        bend_n = bend_n * side if bend_n.length > 1e-6 else z
        # The sleeve does not wring as the wrist turns: the forearm keeps the elbow's plane, and
        # its twist bone, which only the skin follows, takes the hand's turn.
        M[f"UpperArm.{s}"] = frame(shoulder, elbow - shoulder, bend_n)
        M[f"ForeArm.{s}"] = frame(elbow, wrist - elbow, bend_n)
        M[f"Hand.{s}"] = frame(wrist, y, z)
        M[f"ForeTwist.{s}"] = twist_frame(M[f"ForeArm.{s}"], M[f"Hand.{s}"],
                                          elbow.lerp(wrist, 0.5))
        for name in DIGITS:
            joints, frames = finger_chain(hand._replace(wrist=wrist), side, name)
            for k in range(4):
                d, b = frames[k]
                M[f"{name}{k}.{s}"] = frame(joints[k], d, b)
    return M


# The bind pose: the arms forward and a little bent, the hands as they were sculpted, palms down
# and in, and the wrists straight -- the forearm's skin is built along the hand, so in the bind
# pose the hand has to lie along the forearm.
def rest_hand(side):
    hand = Hand(Vector((0.15 * side, -0.43, 0.36)), Vector((0.0, 0.0, 1.0)),
                Vector((0.2 * side, 1.0, 0.0)), SCULPTED, (0.0, 0.0, 0.0))
    return settle(hand, side, 0.0, lambda h: h, 1.0)


REST = Frame(0.0, -20.0, cage_frame(HOLD), {s: rest_hand(side) for s, side in SIDES})


FRAMES = ("Root", "View", "Cage", "Lamp")  # the bones that deform nothing, Root first


def bones_list():
    """(name, parent) in the order they are built and solved."""
    out = [("Root", None)] + [(b, "Root") for b in FRAMES[1:]]
    for s, _ in SIDES:
        out += [(f"UpperArm.{s}", "Root"), (f"ForeArm.{s}", f"UpperArm.{s}"),
                (f"ForeTwist.{s}", f"ForeArm.{s}"), (f"Hand.{s}", f"ForeArm.{s}")]
        for name in DIGITS:
            parent = f"Hand.{s}"
            for k in range(4):
                out.append((f"{name}{k}.{s}", parent))
                parent = f"{name}{k}.{s}"
    return out


BONES = bones_list()
DEFORMING = [n for n, _ in BONES if n not in FRAMES]


def bone_length(name):
    if name in FRAMES:
        return 0.1
    side, part = name[-1], name[:-2]
    if part == "UpperArm":
        return UPPER
    if part == "ForeArm":
        return FORE
    if part == "ForeTwist":
        return 0.5 * FORE
    if part == "Hand":
        return HAND["palm_len"] * 0.9
    return HAND["digits"][part[:-1]]["lengths"][int(part[-1])]


# ---------------------------------------------------------------------------------------------
# The clips. Each is a list of keys, (seconds, Frame), where a Frame's look (its lean and pitch),
# its cage and either hand may be None for no key there: a part runs through its own keys, so the
# two hands need not keep step. A hand gripping the cage is keyed in the CAGE's frame and rides it.
#
# Every part runs through its keys on monotone cubics: the motion carries its speed through a key
# and comes to rest only where it turns back or two keys hold it still. Easing into and out of
# every key, as these clips first did, stopped the arms dead at each one. And the parts do not
# keep step: the eye goes ahead of the hands to what they are about to do, and the fingers close
# after their hand arrives, the index first and the little finger last.

VIEW_LEAD = 0.1    # seconds the look goes ahead of its keys
FINGER_LAG = 0.06  # seconds the fingers come after their hand's
FINGER_STAGGER = {"Thumb": -0.02, "Index": 0.0, "Middle": 0.015, "Ring": 0.03, "Pinky": 0.045}


class Grip(NamedTuple):
    """A hand held in the cage's own frame: what it is, there."""
    hand: Hand


def in_body(hand, cage):
    if isinstance(hand, Grip):
        h = hand.hand
        r = cage.to_3x3()
        return h._replace(wrist=cage @ h.wrist, along=r @ h.along, back=r @ h.back)
    return hand


def in_cage(hand, cage):
    inv = cage.inverted()
    r = inv.to_3x3()
    return Grip(hand._replace(wrist=inv @ hand.wrist, along=r @ hand.along, back=r @ hand.back))


def pchip_slopes(ts, ys, loop):
    """Each key's slope for a monotone cubic through (ts, ys), Fritsch and Carlson's: the harmonic
    mean of the slopes either side, weighted by their spans, and zero where they disagree in sign.
    The ends are at rest unless the curve loops."""
    n = len(ts)
    h = [ts[i + 1] - ts[i] for i in range(n - 1)]
    d = [(ys[i + 1] - ys[i]) / h[i] for i in range(n - 1)]

    def between(h0, h1, d0, d1):
        if d0 * d1 <= 0.0:
            return 0.0
        w0, w1 = 2.0 * h1 + h0, h1 + 2.0 * h0
        return (w0 + w1) / (w0 / d0 + w1 / d1)

    m = [0.0] * n
    for i in range(1, n - 1):
        m[i] = between(h[i - 1], h[i], d[i - 1], d[i])
    if loop and n > 2:
        m[0] = m[-1] = between(h[-1], h[0], d[-1], d[0])
    return m


class Curve:
    """Tuples of numbers through keys at times `ts`, each number on its own monotone cubic."""

    def __init__(self, ts, values, loop=False):
        self.ts = list(ts)
        self.ys = [tuple(v) for v in values]
        dims = len(self.ys[0])
        if len(self.ts) < 2:
            self.ms = [(0.0,) * dims]
        else:
            self.ms = list(zip(*(pchip_slopes(self.ts, [y[c] for y in self.ys], loop)
                                 for c in range(dims))))

    def __call__(self, t):
        ts = self.ts
        if len(ts) < 2 or t <= ts[0]:
            return self.ys[0]
        if t >= ts[-1]:
            return self.ys[-1]
        i = bisect.bisect_right(ts, t) - 1
        h = ts[i + 1] - ts[i]
        s = (t - ts[i]) / h
        s2, s3 = s * s, s * s * s
        h00, h10, h01, h11 = 2 * s3 - 3 * s2 + 1, s3 - 2 * s2 + s, -2 * s3 + 3 * s2, s3 - s2
        return tuple(h00 * a + h10 * h * ma + h01 * b + h11 * h * mb
                     for a, b, ma, mb in zip(self.ys[i], self.ys[i + 1], self.ms[i], self.ms[i + 1]))


def warped(ts, delta):
    """Key times with every key but the first and the last moved by `delta`, still in order."""
    if len(ts) < 2:
        return list(ts)
    out = [ts[0]]
    for i in range(1, len(ts) - 1):
        out.append(min(max(ts[i] + delta, out[-1] + 0.02), ts[-1] - 0.02 * (len(ts) - 1 - i)))
    return out + [ts[-1]]


def turns(qs):
    """Quaternions each on the same side as the one before, so a curve takes the short way."""
    out = []
    for q in qs:
        if out and out[-1].dot(q) < 0.0:
            q = -q
        out.append(q)
    return out


def packed(hands):
    """Hands as numbers to run curves through: the wrist, then the hand's turn as a quaternion."""
    qs = turns([frame(Vector(), h.along, h.back).to_quaternion() for h in hands])
    return [(*h.wrist, *q) for h, q in zip(hands, qs)]


def unpacked(v):
    """A packed hand back, its wrist and its turn; its fingers are for the caller to fill in."""
    r = Quaternion(v[3:7]).normalized().to_matrix()
    return Hand(Vector(v[:3]), r @ Vector((0.0, 1.0, 0.0)), r @ Vector((0.0, 0.0, 1.0)), None, None)


class Clip:
    """A clip's keys made continuous: `at(t)` is its Frame at any instant."""

    def __init__(self, keys, loop=False):
        self.seconds = keys[-1][0]

        def part(get):
            got = [(t, get(f)) for t, f in keys if get(f) is not None]
            if got[0][0] != keys[0][0] or got[-1][0] != self.seconds:
                sys.exit("clip: every part has to be keyed at the clip's first and last instants")
            return [t for t, _ in got], [v for _, v in got]

        ts, look = part(lambda f: None if f.lean is None else (f.lean, f.pitch))
        self.look = Curve(warped(ts, -VIEW_LEAD), look, loop)
        ts, cages = part(lambda f: f.cage)
        self.cage_move = Curve(ts, [tuple(c.translation) for c in cages], loop)
        self.cage_turn = Curve(ts, turns([c.to_quaternion() for c in cages]), loop)
        self.hands = {}
        for s, _ in SIDES:
            ts, hands = part(lambda f, s=s: f.hands[s])
            free = Curve(ts, packed([in_body(h, self.cage_at(t)) for t, h in zip(ts, hands)]),
                         loop)
            # From one grip to the next the hand rides the cage, on a curve in the cage's frame
            # through that run of grips.
            runs = [None] * max(len(ts) - 1, 1)
            i = 0
            while i < len(ts) - 1:
                j = i
                while j < len(ts) - 1 and isinstance(hands[j], Grip) and \
                        isinstance(hands[j + 1], Grip):
                    j += 1
                if j > i:
                    run = Curve(ts[i:j + 1], packed([g.hand for g in hands[i:j + 1]]))
                    runs[i:j] = [run] * (j - i)
                i = max(j, i + 1)
            plain = [h.hand if isinstance(h, Grip) else h for h in hands]
            fingers = {n: Curve(warped(ts, FINGER_LAG + FINGER_STAGGER[n]),
                                [p.fingers[n] for p in plain], loop) for n in FINGER_NAMES}
            thumb = Curve(warped(ts, FINGER_LAG + FINGER_STAGGER["Thumb"]),
                          [p.thumb for p in plain], loop)
            self.hands[s] = (ts, free, runs, fingers, thumb)

    def cage_at(self, t):
        m = Quaternion(self.cage_turn(t)).normalized().to_matrix().to_4x4()
        m.translation = Vector(self.cage_move(t))
        return m

    def at(self, t):
        lean, pitch = self.look(t)
        cage = self.cage_at(t)
        hands = {}
        for s, _ in SIDES:
            ts, free, runs, fingers, thumb = self.hands[s]
            i = min(max(bisect.bisect_right(ts, t) - 1, 0), len(runs) - 1)
            run = runs[i]
            h = in_body(Grip(unpacked(run(t))), cage) if run else unpacked(free(t))
            hands[s] = h._replace(fingers={n: fingers[n](t) for n in FINGER_NAMES},
                                  thumb=thumb(t))
        return Frame(lean, pitch, cage, hands)


# Hands in the poses the clips are made of.

def low(side):
    """Hanging at the side, out of sight below the view, relaxed, the palm to the thigh."""
    hand = Hand(Vector((0.24 * side, -0.70, 0.03)), Vector((-0.1 * side, -1.0, 0.12)),
                Vector((1.0 * side, 0.0, 0.1)), RELAXED, RELAXED_THUMB)
    return settle(hand, side, 0.0, lambda h: h, 0.8)


def side_grip(side, cage, fingers, thumb, lean=0.0):
    """Taking the cage by its side at CAGE_MID: the palm on it, the fingers round toward its back,
    the thumb up its near side, the hand turned toward its forearm as far as it holds on. The cage
    stands upright."""
    out = Vector((side, 0.0, -0.3)).normalized()  # from its axis toward this hand, toward the eye
    wrap = Vector((0.0, -0.3, 1.0))
    wrap = (wrap - out * wrap.dot(out)).normalized()
    hand = Hand(Vector(), wrap, out + Vector((0.0, 0.35, 0.0)), fingers, thumb)
    at = cage.translation + cage.to_3x3() @ Vector((0.0, CAGE_MID, 0.0)) + out * CAGE_R
    return close_on(settle(hand, side, lean, lambda h: wrist_for_grip(h, side, at), 0.75), side,
                    cage)


def cradle(cage):
    """The left hand under the cage's base, palm up, its fingers along under it toward the far
    edge and curling up it, the thumb along the near side."""
    hand = Hand(Vector(), Vector((-0.5, 0.05, 0.85)), Vector((0.05, -1.0, 0.1)), CRADLE,
                CRADLE_THUMB)
    # The palm cups: its middle hangs below the base, which rests on the ball of the thumb.
    at = cage.translation + Vector((0.015, -0.01, -0.015))
    return close_on(settle(hand, 1.0, 0.0, lambda h: wrist_for_grip(h, 1.0, at), 0.5), 1.0, cage)


def reach(grip, cage, offset, fingers=OPEN, thumb=OPEN_THUMB, lean=0.0):
    """A grip on the cage as the body has it, moved by `offset` and opened: the hand on its way to
    it, or leaving it, leading with its turn less than it will hold it."""
    hand = in_body(grip, cage)
    side = 1.0 if hand.wrist.x > 0.0 else -1.0
    hand = hand._replace(wrist=hand.wrist + offset, fingers=fingers, thumb=thumb)
    return settle(hand, side, lean, lambda h: h, 0.4)


def key(t, look=None, cage=None, left=None, right=None):
    """A key at `t` seconds: the look as (lean, pitch), the cage, and each hand, None for no key
    there."""
    lean, pitch = look if look else (None, None)
    return t, Frame(lean, pitch, cage, {"L": left, "R": right})


BENCH_CAGE = cage_frame(BENCH)
HOLD_CAGE = cage_frame(HOLD)
LIFT_CAGE = cage_frame(Vector((0.0, -0.47, 0.5)), tilt=math.radians(4.0))
# It comes up a touch past where it is held and settles.
ARRIVE_CAGE = cage_frame(HOLD + Vector((0.0, 0.006, 0.01)), tilt=math.radians(5.0))
# Turned by the left hand to put the door toward the right one, for opening it and for the key.
PRESENT_CAGE = cage_frame(HOLD + Vector((0.025, 0.015, 0.05)), yaw=math.radians(35.0),
                          tilt=math.radians(4.0))
LEAN = 20.0        # degrees the upper body leans to reach the bench, the elbows bent at it
LOOK_DOWN = -60.0  # where the eye looks at the cage on the bench
HOLD_PITCH = -27.0


# Where the left hand goes on its way under the base and back, from where it cradles it: straight
# below, and below and out past the rim.
BELOW = Vector((0.0, -0.045, -0.01))
BELOW_OUT = Vector((0.09, -0.05, -0.01))


def held():
    """The cage as it is held: on the left palm, the right hand at its side."""
    return (in_cage(cradle(HOLD_CAGE), HOLD_CAGE),
            in_cage(side_grip(-1.0, HOLD_CAGE, STEADY, STEADY_THUMB), HOLD_CAGE))


def side_grips(fingers, thumb, cage=HOLD_CAGE, lean=0.0):
    """Both hands at the cage's sides, in its frame, as they take it standing at `cage`."""
    return {s: in_cage(side_grip(side, cage, fingers, thumb, lean), cage) for s, side in SIDES}


def clip_raise():
    """From the hands at the sides: reach for the cage on the bench, take it by its sides, lift it
    to the chest, and settle it on the left palm with the right hand at its side."""
    touch = side_grips(CONTACT, CONTACT_THUMB, BENCH_CAGE, LEAN)
    grip = side_grips(GRIP, GRIP_THUMB, BENCH_CAGE, LEAN)
    carry = side_grips(GRIP, GRIP_THUMB)
    held_l, held_r = held()
    return [
        key(0.0, (0.0, LOOK_DOWN + 10.0), BENCH_CAGE, low(1.0), low(-1.0)),
        key(0.3, (LEAN * 0.6, LOOK_DOWN + 2.0),
            left=reach(grip["L"], BENCH_CAGE, Vector((0.10, 0.08, -0.06)), lean=LEAN * 0.6)),
        key(0.36, right=reach(grip["R"], BENCH_CAGE, Vector((-0.11, 0.07, -0.07)),
                              lean=LEAN * 0.6)),
        key(0.58, (LEAN, LOOK_DOWN), left=touch["L"]),
        key(0.64, right=touch["R"]),
        key(0.78, cage=BENCH_CAGE, left=grip["L"], right=grip["R"]),
        key(1.12, (LEAN * 0.25, -37.0), LIFT_CAGE, grip["L"], grip["R"]),
        key(1.42, (0.0, HOLD_PITCH - 1.0), ARRIVE_CAGE, carry["L"], carry["R"]),
        # The left hand lets go, goes round the base -- out and down past its rim -- and comes up
        # under it from straight below.
        key(1.52, left=reach(held_l, HOLD_CAGE, BELOW_OUT, RELAXED, RELAXED_THUMB)),
        key(1.64, left=reach(held_l, HOLD_CAGE, BELOW)),
        key(1.8, cage=HOLD_CAGE, left=held_l, right=held_r),
        key(2.1, (0.0, HOLD_PITCH), HOLD_CAGE, held_l, held_r),
    ]


BREATH = 4.0  # seconds of a held breath: the length of each loop the cage is held in


def breathed(cage, a):
    """`cage` as the chest moves it at phase `a` of a breath, 0 to 2 pi: up and down by a few
    millimetres, swaying and tilting by under a degree, each a whole number of times a breath."""
    move = Vector((0.0012 * math.sin(2.0 * a + 0.6), 0.0035 * math.sin(a),
                   0.0015 * math.sin(a + 1.2)))
    sway = (Matrix.Rotation(math.radians(0.8 * math.sin(2.0 * a + 1.9)), 4, "Y") @
            Matrix.Rotation(math.radians(0.6 * math.sin(3.0 * a + 0.4)), 4, "X"))
    return Matrix.Translation(move) @ cage @ sway


def fidget(fingers, a):
    """Fingers at phase `a` of a breath: never quite still."""
    return {n: (fan, m + 1.5 * math.sin(a + i), p + 2.0 * math.sin(2.0 * a + 1.3 * i), d)
            for i, (n, (fan, m, p, d)) in enumerate(fingers.items())}


def held_loop(cage, pitch, right):
    """A loop of a held breath: the cage at `cage` on the left palm, breathing, the view at
    `pitch`, and the right hand `right(a)` at each phase of it, in the cage's frame."""
    held_l, _ = held()
    keys = []
    for k in range(25):
        t = BREATH * k / 24.0
        a = 2.0 * math.pi * t / BREATH
        keys.append(key(t, (0.0, pitch), breathed(cage, a), held_l, right(a)))
    return keys


def clip_hold():
    """Holding it to look at: the right hand at its side."""
    return held_loop(HOLD_CAGE, HOLD_PITCH, lambda a: in_cage(
        side_grip(-1.0, HOLD_CAGE, fidget(STEADY, a), STEADY_THUMB), HOLD_CAGE))


# A pinch: the forefinger's pad and the thumb's closing PINCH_GAP apart, on the door's edge or the
# key's bow -- a forefinger bent 125 degrees in all, and the thumb rolled across to meet it.
# Opened, the pads stand apart to go round what they take.
PINCH = shape(0.3, (50, 60, 15), (45, 60, 35), (52, 65, 38), (58, 68, 40))
PINCH_THUMB = (0.6, 0.0, 0.0)
PINCH_OPEN = shape(0.5, (25, 30, 10), (35, 50, 30), (42, 55, 33), (48, 58, 35))
PINCH_OPEN_THUMB = (0.35, 0.0, 0.0)
PAD = 0.006         # a pad's middle under its bone's line, toward the palm
PINCH_IN = 0.012    # how far into the door's opening the pinch reaches for the key
PINCH_LEAN = 25.0   # degrees the upright hand leans in over the door
READY_OUT = 0.07    # how far in front of the opening the hand waits, ready to take the key


def pads(hand, side):
    """The forefinger's pad and the thumb's: each the middle of the digit's last bone, PAD toward
    the palm."""
    out = []
    for name in ("Index", "Thumb"):
        joints, frames = finger_chain(hand, side, name)
        out.append((joints[3] + joints[4]) * 0.5 - frames[3][1] * PAD)
    return out


def wrist_for_pinch(hand, side, at):
    """The hand moved so the middle between its forefinger's pad and its thumb's is at `at`."""
    index, thumb = pads(hand, side)
    return hand._replace(wrist=hand.wrist + (at - (index + thumb) * 0.5))


def pinching(cage, at, fingers=PINCH, thumb=PINCH_THUMB, turn=0.0):
    """The right hand upright before the cage's door, its palm to it and leaning in over it, its
    forefinger and thumb pinching at `at` in the cage's frame; `turn` turns the hand about the
    cage's up with what it holds. A pinch closes in front of the palm, so this is how a hand takes
    the door's edge or something from just inside a hole, and the other fingers curl clear."""
    r, lean = cage.to_3x3() @ rot_y(turn), math.radians(PINCH_LEAN)
    up, out = r @ Vector((0.0, 1.0, 0.0)), r @ Vector((0.0, 0.0, 1.0))
    hand = Hand(Vector(), up * math.cos(lean) - out * math.sin(lean),
                out * math.cos(lean) + up * math.sin(lean), fingers, thumb)
    return close_on(wrist_for_pinch(hand, -1.0, cage @ at), -1.0, cage,
                    skip=("Index", "Thumb"))


def pinch_point():
    """Where the pinch takes the key, in the cage's frame: just inside the door's opening, at its
    middle."""
    rim = cage_model.DOOR_FRAME_R + cage_model.RING_WIRE
    return Vector((0.0, cage_model.DOOR_Y, rim - PINCH_IN))


def ready():
    """The right hand waiting before the open door, ready to take what is inside, in the cage's
    frame as it is presented."""
    hand = pinching(PRESENT_CAGE, pinch_point() + Vector((0.0, 0.0, PINCH_IN + READY_OUT)),
                    RELAXED, RELAXED_THUMB)
    return in_cage(hand, PRESENT_CAGE)


def clip_wait():
    """Holding it open, presented to the right hand, which waits before the door, relaxed: a hand
    opens to pinch only as it gets there."""
    hand = ready().hand
    return held_loop(PRESENT_CAGE, HOLD_PITCH + 1.5, lambda a: Grip(
        hand._replace(fingers=fidget(RELAXED, a))))


# The door's swing in `open`: it turns from shut to DOOR_OPEN between these, eased as `smooth`
# eases, and the pinch rides its free edge for the first DOOR_RIDE of the way.
DOOR_FROM, DOOR_TO = 0.5, 0.98
DOOR_RIDE = 0.55


def door_angle(t):
    return cage_model.DOOR_OPEN * smooth((t - DOOR_FROM) / (DOOR_TO - DOOR_FROM))


def edge_point(angle):
    """Where the pinch takes the door, in the cage's frame: its free edge half way through its
    plate, the door swung `angle` about its hinge."""
    hinge = cage_model.hinge_point()
    edge = cage_model.door_point(1.0, 0.0, cage_model.DOOR_BACK + 0.5 * cage_model.DOOR_T)
    return hinge + rot_y(angle) @ (edge - hinge)


def letting_go(held_r):
    """The right hand letting go of the cage's side: the fingers open on it, then it comes away."""
    return (reach(held_r, HOLD_CAGE, Vector((-0.008, 0.0, -0.004)), CONTACT, CONTACT_THUMB),
            reach(held_r, HOLD_CAGE, Vector((-0.06, 0.0, -0.03)), RELAXED, RELAXED_THUMB))


def clip_open():
    """The left hand presents the door to the right, which lets go of the cage's side, pinches the
    door's free edge and swings it open, lets it go on swinging, and waits before it."""
    held_l, held_r = held()
    opening, away = letting_go(held_r)
    out = Vector((0.0, 0.0, 1.0))
    keys = [
        key(0.0, (0.0, HOLD_PITCH), HOLD_CAGE, held_l, held_r),
        key(0.1, right=opening),
        key(0.22, right=away),
        key(0.36, (0.0, HOLD_PITCH + 1.5), PRESENT_CAGE,
            right=pinching(PRESENT_CAGE, edge_point(0.0) + out * 0.06, RELAXED, RELAXED_THUMB)),
        key(0.46, right=pinching(PRESENT_CAGE, edge_point(0.0), PINCH_OPEN, PINCH_OPEN_THUMB)),
        key(DOOR_FROM, right=pinching(PRESENT_CAGE, edge_point(0.0))),
    ]
    # Riding the edge as it swings, turned half as far as it: a key at each of a few angles on the
    # way. Turned all the way with it, the wrist bent 46 degrees sideways.
    for u in (0.18, 0.33, 0.45, 0.533):
        t = DOOR_FROM + (DOOR_TO - DOOR_FROM) * u
        a = door_angle(t)
        keys.append(key(t, right=pinching(PRESENT_CAGE, edge_point(a), turn=0.5 * a)))
    a = door_angle(0.84)
    keys += [
        key(0.84, right=pinching(PRESENT_CAGE, edge_point(a) + out * 0.02, RELAXED,
                                 RELAXED_THUMB, turn=0.5 * a)),
        key(1.1, right=ready()),
        key(1.3, (0.0, HOLD_PITCH + 1.5), PRESENT_CAGE, held_l, ready()),
    ]
    return keys


KEY_TAKEN = 0.4   # seconds into `take` the key is in the pinch: it is drawn out from here
KEY_GONE = 1.05   # and is put away, out of sight


def clip_take():
    """From the hand waiting before the open door: it pinches just inside it -- the key is between
    its fingers there -- draws the key out through the door bow first and takes it down out of
    sight, and comes back to the cage's side empty, the cage turned back to be held."""
    held_l, held_r = held()
    at, out = pinch_point(), Vector((0.0, 0.0, 1.0))
    pocket = low(-1.0)._replace(fingers=PINCH, thumb=PINCH_THUMB)
    # Back from the pocket it comes up outside the cage's right side, never through its base.
    returning = reach(held_r, HOLD_CAGE, Vector((-0.08, -0.03, -0.04)), STEADY, STEADY_THUMB)
    return [
        key(0.0, (0.0, HOLD_PITCH + 1.5), PRESENT_CAGE, held_l, ready()),
        key(0.26, right=pinching(PRESENT_CAGE, at, PINCH_OPEN, PINCH_OPEN_THUMB)),
        key(0.33, right=pinching(PRESENT_CAGE, at)),
        key(KEY_TAKEN, right=pinching(PRESENT_CAGE, at)),
        key(0.7, cage=PRESENT_CAGE, right=pinching(PRESENT_CAGE, at + out * 0.13)),
        key(KEY_GONE, right=pocket),
        key(1.35, right=returning),
        key(1.6, (0.0, HOLD_PITCH), HOLD_CAGE, held_l, held_r),
    ]


def clip_lower():
    """The raise backwards: the left hand from under the base to its side, the cage set down on
    the bench and let go, and the hands dropped to the sides."""
    held_l, held_r = held()
    carry = side_grips(GRIP, GRIP_THUMB)
    touch = side_grips(CONTACT, CONTACT_THUMB, BENCH_CAGE, LEAN)
    grip = side_grips(GRIP, GRIP_THUMB, BENCH_CAGE, LEAN)
    return [
        key(0.0, (0.0, HOLD_PITCH), HOLD_CAGE, held_l, held_r),
        # The left hand drops from under the base and goes round its rim to its side.
        key(0.1, left=reach(held_l, HOLD_CAGE, BELOW)),
        key(0.22, left=reach(held_l, HOLD_CAGE, BELOW_OUT, RELAXED, RELAXED_THUMB)),
        key(0.3, left=reach(carry["L"], HOLD_CAGE, Vector((0.04, -0.01, -0.01)))),
        key(0.4, cage=HOLD_CAGE, left=carry["L"], right=held_r),
        key(0.72, (LEAN * 0.3, -40.0), LIFT_CAGE),
        key(0.95, (LEAN, LOOK_DOWN), BENCH_CAGE, grip["L"], grip["R"]),
        key(1.05, left=touch["L"], right=touch["R"]),
        key(1.25, (LEAN * 0.5, LOOK_DOWN + 4.0),
            left=reach(grip["L"], BENCH_CAGE, Vector((0.09, 0.07, -0.08)), RELAXED,
                       RELAXED_THUMB, LEAN * 0.5),
            right=reach(grip["R"], BENCH_CAGE, Vector((-0.1, 0.06, -0.09)), RELAXED,
                        RELAXED_THUMB, LEAN * 0.5)),
        key(1.5, (0.0, LOOK_DOWN + 10.0), BENCH_CAGE, low(1.0), low(-1.0)),
    ]


# name: (seconds, looping, keys, events (name, seconds))
CLIPS = {
    "raise": (2.1, False, clip_raise, [("grip", 0.64), ("look", 1.42)]),
    "hold": (BREATH, True, clip_hold, []),
    "open": (1.3, False, clip_open, []),
    "wait": (BREATH, True, clip_wait, []),
    "take": (1.6, False, clip_take, [("key", KEY_TAKEN), ("away", KEY_GONE)]),
    "lower": (1.5, False, clip_lower, [("down", 0.95)]),
}


# ---------------------------------------------------------------------------------------------
# The armature, in Blender space


def build_armature():
    data = bpy.data.armatures.new("arms_rig")
    arm = bpy.data.objects.new("arms", data)
    bpy.context.scene.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    rest = solve(REST)
    for name, parent in BONES:
        eb = data.edit_bones.new(name)
        eb.head = (0.0, 0.0, 0.0)
        eb.tail = (0.0, bone_length(name), 0.0)
        eb.matrix = BV4 @ rest[name]
        eb.length = bone_length(name)
        if parent:
            eb.parent = data.edit_bones[parent]
            eb.use_connect = False
        eb.use_deform = name in DEFORMING
    bpy.ops.object.mode_set(mode="OBJECT")
    for pb in arm.pose.bones:
        pb.rotation_mode = "QUATERNION"
    return arm


class Rig:
    def __init__(self, arm):
        self.arm = arm
        self.rest = {b.name: b.matrix_local.copy() for b in arm.data.bones}
        self.parent = {b.name: (b.parent.name if b.parent else None) for b in arm.data.bones}
        self.rest_rel = {}
        for n, _ in BONES:
            p = self.parent[n]
            self.rest_rel[n] = (self.rest[p].inverted() @ self.rest[n]) if p else self.rest[n]

    def basis(self, f):
        """Each bone's matrix_basis for a Frame."""
        M = {n: BV4 @ m for n, m in solve(f).items()}
        out = {}
        for n, _ in BONES:
            p = self.parent[n]
            parent = M[p] if p else Matrix.Identity(4)
            # The bone's length is fixed by its rest; a posed frame says where it is and which
            # way it turns, and nothing scales it.
            out[n] = self.rest_rel[n].inverted() @ parent.inverted() @ M[n]
        return out


def clip_named(name):
    """A clip of CLIPS made continuous, its last key where CLIPS says it ends."""
    seconds, looping, keys, _ = CLIPS[name]
    clip = Clip(keys(), looping)
    if abs(clip.seconds - seconds) > 1e-6:
        sys.exit(f"clip {name}: its keys end at {clip.seconds} s, not {seconds}")
    return clip


def key_clip(arm, rig, name, clip):
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    arm.animation_data_create()
    arm.animation_data.action = act
    frames = int(round(clip.seconds * FPS))
    channels, prev = {}, {}
    for f in range(frames + 1):
        t = min(f / FPS, clip.seconds)
        for n, b in rig.basis(clip.at(t)).items():
            loc, q, _ = b.decompose()
            if n in prev and prev[n].dot(q) < 0.0:
                q = -q
            prev[n] = q
            ch = channels.setdefault(n, {"loc": [], "rot": []})
            ch["loc"].append((f, tuple(loc)))
            ch["rot"].append((f, tuple(q)))
    for n, ch in channels.items():
        path = f'pose.bones["{n}"]'
        for prop, size, keyset in (("location", 3, ch["loc"]), ("rotation_quaternion", 4, ch["rot"])):
            for i in range(size):
                fc = act.fcurve_ensure_for_datablock(arm, f"{path}.{prop}", index=i, group_name=n)
                fc.keyframe_points.add(len(keyset))
                co = []
                for f, v in keyset:
                    co += [float(f), v[i]]
                fc.keyframe_points.foreach_set("co", co)
                fc.keyframe_points.foreach_set("interpolation", [1] * len(keyset))
                fc.update()
    arm.animation_data.action = None


# ---------------------------------------------------------------------------------------------
# The meshes, built in the bind pose


def ring(centre, x_ax, z_ax, rx, rz, n, power=2.0):
    """A closed loop of n points round `centre` in the plane of x_ax and z_ax, a superellipse of
    half-axes rx and rz (power 2 an ellipse, more a rounder box)."""
    out = []
    for k in range(n):
        t = 2.0 * math.pi * k / n
        c, s = math.cos(t), math.sin(t)
        cx = math.copysign(abs(c) ** (2.0 / power), c)
        sz = math.copysign(abs(s) ** (2.0 / power), s)
        out.append(centre + x_ax * (rx * cx) + z_ax * (rz * sz))
    return out


def sweep(rings, cap_start=True, cap_end=True):
    """Faces joining a list of equal-length rings, capped by a point at either end."""
    n = len(rings[0])
    verts = [p for r in rings for p in r]
    faces = []
    for i in range(len(rings) - 1):
        for k in range(n):
            k2 = (k + 1) % n
            faces.append((i * n + k, i * n + k2, (i + 1) * n + k2, (i + 1) * n + k))
    for at, index, flip in ((cap_start, 0, True), (cap_end, len(rings) - 1, False)):
        if not at:
            continue
        c = len(verts)
        verts.append(sum(rings[index], Vector()) / n)
        for k in range(n):
            k2 = (k + 1) % n
            f = (c, index * n + k, index * n + k2)
            faces.append(f[::-1] if flip else f)
    return verts, faces


def hand_mesh(s, side):
    """The sculpted hand laid into this side's bind pose, its UVs and its skin weights carried
    over: the right as it was made, the left its mirror, its faces turned so they still face out."""
    rest = REST.hands[s]
    y, z, c_ax = hand_axes(rest, side)
    verts = [bv(*(rest.wrist + y * v.x + c_ax * v.y + z * v.z)) for v in HAND["verts"]]
    mirrored = side > 0
    faces = [f[::-1] for f in HAND["faces"]] if mirrored else HAND["faces"]
    me = bpy.data.meshes.new(f"hand_{s}")
    me.from_pydata(verts, [], faces)
    me.update()
    uv = me.uv_layers.new(name="UV")
    for p, uvs in zip(me.polygons, HAND["uvs"]):
        for loop, coord in zip(p.loop_indices, uvs[::-1] if mirrored else uvs):
            uv.data[loop].uv = coord
    for p in me.polygons:
        p.use_smooth = True
    me.materials.append(HAND["material"])
    ob = bpy.data.objects.new(f"hand_{s}", me)
    bpy.context.scene.collection.objects.link(ob)
    groups = {}
    for i, w in enumerate(HAND["weights"]):
        for name, x in w.items():
            full = f"{name}.{s}"
            if full not in groups:
                groups[full] = ob.vertex_groups.new(name=full)
            groups[full].add([i], x, "REPLACE")
    return ob


SLEEVE_TILE = 0.12  # metres of sleeve one copy of the cloth's scan covers
CUFF_SHORT = 0.015  # how far short of the wrist's joint the cuff ends, so a little wrist shows
CUFF_R = 0.043      # the cuff's radius, close round the wrist
LINING = 0.03       # how far inside the cuff its lining closes onto the arm


def sleeve_radius(along_fore):
    """The sleeve's radius `along_fore` metres down the forearm from the elbow, the cuff's band
    and its seam's lip included."""
    to_cuff = FORE - CUFF_SHORT - along_fore
    r = 0.058 - (0.058 - CUFF_R - 0.003) * smooth(along_fore / FORE)
    r -= 0.003 * smooth((0.06 - to_cuff) / 0.01)
    r += 0.0015 * math.exp(-((to_cuff - 0.052) / 0.003) ** 2)
    return r + 0.001 * math.exp(-(to_cuff / 0.004) ** 2)


def sleeve_mesh(f, side, parts, uvs):
    """The jacket's sleeve, from behind the shoulder to its cuff short of the wrist, wrinkled where
    an arm bends, its cuff a band with a lip and the cloth's edge turned in."""
    M = solve(f)
    s_name = "L" if side > 0 else "R"
    up_m, fore_m = M[f"UpperArm.{s_name}"], M[f"ForeArm.{s_name}"]
    shoulder, elbow = up_m.translation, fore_m.translation
    wrist = M[f"Hand.{s_name}"].translation
    fore_dir = (wrist - elbow).normalized()
    up_dir = (elbow - shoulder).normalized()
    path = []
    n_up, n_fore = 18, 26
    for j in range(n_up):
        u = j / n_up
        path.append((shoulder + up_dir * (-0.06 + (UPPER + 0.06) * u), up_dir, "up"))
    cuff_end = FORE - CUFF_SHORT
    for j in range(n_fore + 1):
        u = j / n_fore
        path.append((elbow + fore_dir * (cuff_end * u), fore_dir, "fore"))
    rings, rows = [], []
    length = 0.0
    prev = None
    n = 28
    for i, (p, d, part) in enumerate(path):
        if prev is not None:
            length += (p - prev).length
        prev = p
        dd = d if i == 0 or i == len(path) - 1 else (path[i + 1][0] - path[i - 1][0]).normalized()
        ref = Vector((0.0, 1.0, 0.0))
        across = dd.cross(ref).normalized()
        upv = across.cross(dd).normalized()
        r = 0.066 if part == "up" else sleeve_radius((p - elbow).dot(fore_dir))
        row = []
        for k in range(n):
            t = 2.0 * math.pi * k / n
            # Folds: rings of cloth bunched toward the elbow, and a slow wave round them.
            bunch = math.exp(-(((p - elbow).length) / 0.09) ** 2)
            fold = 0.004 * math.sin(t * 3.0 + length * 90.0) * (0.3 + bunch)
            fold += 0.0025 * math.sin(length * 140.0 + t) * bunch
            rr = r + fold
            row.append(p + across * (rr * math.cos(t)) + upv * (rr * 0.93 * math.sin(t)))
        rings.append(row)
        rows.append(length)
    # The cloth's edge turned in at the cuff, and its lining closing onto the arm inside, so a
    # look down the sleeve finds no hole: round, and inside the skin's narrowest, so it stays in
    # the arm however far the forearm's skin turns within it.
    last = rings[-1]
    centre = path[-1][0]
    inner = [centre + (v - centre) * 0.9 - fore_dir * 0.004 for v in last]
    arm = 0.93 * min(forearm_section(CUFF_SHORT + LINING + WRIST_CUT))
    lining = [centre + (v - centre).normalized() * arm - fore_dir * LINING for v in last]
    rings += [inner, lining]
    rows += [rows[-1] + 0.004, rows[-1] + LINING]
    verts, faces = sweep(rings, cap_start=True, cap_end=False)
    base = len(parts.v)
    parts.add(verts, faces)
    # The cloth's coordinates: round the sleeve and along it, in tiles of the scan.
    circ = 2.0 * math.pi * 0.06
    for i, row in enumerate(rings):
        for k in range(n):
            uvs[base + i * n + k] = (k / n * circ / SLEEVE_TILE, rows[i] / SLEEVE_TILE)
    uvs[base + len(rings) * n] = (0.0, 0.0)
    return wrist, fore_dir


def button_mesh(f, side, parts):
    """A horn button on the cuff, on its outer side."""
    s_name = "L" if side > 0 else "R"
    M = solve(f)
    elbow = M[f"ForeArm.{s_name}"].translation
    wrist = M[f"Hand.{s_name}"].translation
    fore = (wrist - elbow).normalized()
    across = fore.cross(Vector((0.0, 1.0, 0.0))).normalized()
    out = (across * side * 0.6 + Vector((0.0, 1.0, 0.0)) * 0.8).normalized()
    along = FORE - CUFF_SHORT - 0.03
    at = elbow + fore * along + out * (sleeve_radius(along) * 0.97)
    disc = lathe([(0.0, -0.0012), (0.0068, -0.0012), (0.0072, 0.0004), (0.0055, 0.0014),
                  (0.0, 0.0011)], 16)
    m = frame(Vector(), out, fore).to_3x3()
    verts = [at + m @ Vector((v.x, v.y, v.z)) for v in disc[0]]
    parts.add(verts, disc[1])


def nearest_on_segment(p, a, b):
    ab = b - a
    t = max(0.0, min(1.0, (p - a).dot(ab) / max(ab.length_squared, 1e-12)))
    return (p - (a + ab * t)).length


def weigh(obj, rest, names, width):
    """Every vertex weighted to the bones of `names` by how near each bone's segment is, the
    nearest most, blended over `width` across a joint."""
    groups = {n: obj.vertex_groups.new(name=n) for n in names}
    segs = {}
    for n in names:
        m = rest[n]
        segs[n] = (m.translation, m.translation + m.to_3x3() @ Vector((0.0, bone_length(n), 0.0)))
    for v in obj.data.vertices:
        p = mv(v.co)
        d = {n: nearest_on_segment(p, *segs[n]) for n in names}
        best = min(d.values())
        w = {n: math.exp(-((x - best) / width) ** 2) for n, x in d.items() if x - best < 3.0 * width}
        total = sum(w.values())
        for n, x in sorted(w.items(), key=lambda kv: -kv[1])[:4]:
            groups[n].add([v.index], x / total, "REPLACE")


def ambient(obj, rays=40, reach=0.05):
    """How open each vertex is to the sky, 0 shut in to 1 open, by rays against the object."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    tree = BVHTree.FromBMesh(bm)
    bm.free()
    dirs = []
    golden = math.pi * (3.0 - math.sqrt(5.0))
    for i in range(rays):
        zz = 1.0 - (i + 0.5) / rays
        r = math.sqrt(max(0.0, 1.0 - zz * zz))
        dirs.append(Vector((r * math.cos(golden * i), r * math.sin(golden * i), zz)))
    out = []
    for v in obj.data.vertices:
        n = v.normal
        q = n.to_track_quat("Z", "Y")
        hit = sum(1 for d in dirs if tree.ray_cast(v.co + n * 0.0006, q @ d, reach)[0] is not None)
        out.append(1.0 - hit / rays)
    return out


def paint(obj, colour_of):
    """Vertex colours, by a function of each vertex's model-space point and openness."""
    open_ = ambient(obj)
    attr = obj.data.color_attributes.new("Col", "BYTE_COLOR", "POINT")
    for v in obj.data.vertices:
        attr.data[v.index].color = (*colour_of(mv(v.co), open_[v.index]), 1.0)
    obj.data.color_attributes.active_color = attr


# ---------------------------------------------------------------------------------------------
# Materials


def material(name, colour, rough, metal=0.0):
    m = bpy.data.materials.new(name)
    m.use_backface_culling = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    return m, bsdf


def cloth_material():
    """The jacket's olive canvas: Poly Haven's CC0 `book_pattern` weave, repeated along the
    sleeve, darkened toward a field jacket's olive drab."""
    # It ships colourways rather than one Diffuse; the first is the olive.
    urls, _ = polyhaven_urls("book_pattern", {"albedo": "col1", "normal": "nor_gl",
                                              "rough": "Rough"}, "1k")
    images = {}
    for kind, url in urls.items():
        fetch(url)
        img = bpy.data.images.load(cached_path(url), check_existing=True)
        img.colorspace_settings.name = "sRGB" if kind == "albedo" else "Non-Color"
        images[kind] = img
    m, bsdf = material("arm_sleeve", (1.0, 1.0, 1.0), 0.85)
    tree = m.node_tree
    albedo = tree.nodes.new("ShaderNodeTexImage")
    albedo.image = images["albedo"]
    tree.links.new(albedo.outputs["Color"], bsdf.inputs["Base Color"])
    rough = tree.nodes.new("ShaderNodeTexImage")
    rough.image = images["rough"]
    tree.links.new(rough.outputs["Color"], bsdf.inputs["Roughness"])
    normal = tree.nodes.new("ShaderNodeTexImage")
    normal.image = images["normal"]
    nmap = tree.nodes.new("ShaderNodeNormalMap")
    tree.links.new(normal.outputs["Color"], nmap.inputs["Color"])
    tree.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    return m


def build_meshes(arm):
    rest = solve(REST)
    cloth_m = cloth_material()
    button_m, _ = material("arm_button", srgb("2A1E16"), 0.35)
    objects = []
    for s, side in SIDES:
        hand_ob = hand_mesh(s, side)
        cloth, uvs = Parts(), {}
        sleeve_mesh(REST, side, cloth, uvs)
        sleeve = finish(f"sleeve_{s}", cloth, cloth_m, sharp_degrees=80.0)
        uv = sleeve.data.uv_layers.new(name="UV")
        for loop in sleeve.data.loops:
            uv.data[loop.index].uv = uvs.get(loop.vertex_index, (0.0, 0.0))
        weigh(sleeve, rest, [f"UpperArm.{s}", f"ForeArm.{s}"], 0.03)
        paint(sleeve, lambda p, open_: (0.55 + 0.45 * open_,) * 3)

        buttons = Parts()
        button_mesh(REST, side, buttons)
        button = finish(f"button_{s}", buttons, button_m)
        weigh(button, rest, [f"ForeArm.{s}"], 0.01)
        objects += [hand_ob, sleeve, button]
    for ob in objects:
        mod = ob.modifiers.new("armature", "ARMATURE")
        mod.object = arm
        ob.parent = arm
    return objects


# ---------------------------------------------------------------------------------------------
# Export, and the header the game reads


def export(path):
    props = bpy.ops.export_scene.gltf.get_rna_type().properties.keys()
    want = dict(
        filepath=path, export_format="GLB", export_yup=True, use_selection=False,
        export_apply=False, export_animations=True, export_animation_mode="ACTIONS",
        export_force_sampling=True, export_frame_step=1, export_optimize_animation_size=False,
        export_optimize_animation_keep_anim_armature=True, export_def_bones=False,
        export_skins=True, export_all_influences=False, export_materials="EXPORT",
        export_vertex_color="ACTIVE", export_normals=True, export_texcoords=True,
        export_tangents=True, export_image_format="JPEG", export_jpeg_quality=90,
        export_cameras=False, export_lights=False, export_extras=False,
        export_copyright=HAND["credit"],
        export_rest_position_armature=True, export_anim_slide_to_zero=False,
        export_reset_pose_bones=True, export_bake_animation=False,
    )
    missing = [k for k in want if k not in props]
    if missing:
        print(f"export: this Blender has no {', '.join(missing)}; left at its defaults", flush=True)
    bpy.ops.export_scene.gltf(**{k: v for k, v in want.items() if k in props})


def node_globals(js):
    """Each node's global matrix at rest, glTF's own, by name."""
    nodes = js["nodes"]
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent[c] = i

    def local(i):
        n = nodes[i]
        if "matrix" in n:
            return Matrix([n["matrix"][k::4] for k in range(4)])
        t = Vector(n.get("translation", (0.0, 0.0, 0.0)))
        x, y, z, w = n.get("rotation", (0.0, 0.0, 0.0, 1.0))
        s = n.get("scale", (1.0, 1.0, 1.0))
        return (Matrix.Translation(t) @ Quaternion((w, x, y, z)).to_matrix().to_4x4()
                @ Matrix.Diagonal((*s, 1.0)))

    out = {}

    def glob(i):
        return local(i) if i not in parent else glob(parent[i]) @ local(i)

    for i, n in enumerate(nodes):
        out[n.get("name", "")] = glob(i)
    return out


def self_check(path):
    js, _ = read_glb(path)
    anims = {a["name"]: a for a in js.get("animations", [])}
    problems = [f"no clip '{c}' (have {sorted(anims)})" for c in CLIPS if c not in anims]
    problems += [f"a clip '{a}' that is none of this script's" for a in anims if a not in CLIPS]
    joints = {js["nodes"][j]["name"] for j in js["skins"][0]["joints"]}
    problems += [f"no bone '{b}'" for b in ("View", "Cage", "Lamp", "Hand.R") if b not in joints
                 and b not in {n.get("name") for n in js["nodes"]}]
    # The game reads View, Cage and Lamp as frames in the body's space: what the file says they
    # are at rest must be what was authored.
    g = node_globals(js)
    rest = solve(REST)
    for b in FRAMES[1:]:
        if b in g:
            have, want = g[b], rest[b]
            err = max(abs(have[i][j] - want[i][j]) for i in range(3) for j in range(4))
            if err > 1e-4:
                problems.append(f"{b} rests at\n{have}\nnot\n{want}")
    if problems:
        sys.exit("self-check:\n  " + "\n  ".join(problems))
    tris = sum(js["accessors"][p["indices"]]["count"] // 3
               for m in js["meshes"] for p in m["primitives"])
    print(f"self-check: {len(joints)} joints, {len(anims)} clips, {tris} triangles, "
          f"{os.path.getsize(path) / 1024:.0f} KiB", flush=True)


KEY_BONE = "Index3.R"  # the forefinger's last bone, which the key rides once it is taken


def key_on_finger():
    """The key's frame in KEY_BONE's while the pinch carries it, as it is when the pinch closes:
    its bow's wire between the pads, its face to them, lying along the door's opening with its bit
    toward the inside, so it is drawn out bow first. It leaves the hook for there while the hand
    covers the door."""
    f = clip_named("take").at(KEY_TAKEN)
    index, thumb = pads(f.hands["R"], -1.0)
    into = (f.cage.to_3x3() @ Vector((0.0, 0.0, -1.0))).normalized()
    face = index - thumb
    face = (face - into * face.dot(into)).normalized()
    m = Matrix((face, into.cross(face), into)).transposed().to_4x4()
    wire = 0.5 * cage_model.KEY_LEN - 0.0035  # from the key's middle back to its bow's top wire
    m.translation = (index + thumb) * 0.5 + into * wire
    return solve(f)[KEY_BONE].inverted() @ m


def write_header():
    names = list(CLIPS)
    width = max(1, max(len(c[3]) for c in CLIPS.values()))
    lines = [
        "// Generated by apps/silent/tools/make_arms_blender.py -- do not edit; rerun it.",
        "#ifndef _SILENT_ARMS_CLIPS_H_",
        "#define _SILENT_ARMS_CLIPS_H_",
        "",
        "// The clips in assets/models/arms.glb, by the names the file gives them.",
        "typedef enum {",
        *[f"    ARMS_CLIP_{n.upper()}," for n in names],
        "    ARMS_CLIP_COUNT",
        "} ArmsClipId;",
        "",
        "typedef struct ArmsClipEvent {",
        "    const char* name;",
        "    float seconds; // from the clip's start",
        "} ArmsClipEvent;",
        "",
        "typedef struct ArmsClipSpec {",
        "    const char* name;",
        "    float seconds;",
        "    int looping;",
        "    int event_count;",
        f"    ArmsClipEvent events[{width}];",
        "} ArmsClipSpec;",
        "",
        "static const ArmsClipSpec ARMS_CLIPS[ARMS_CLIP_COUNT] = {",
    ]
    for n in names:
        seconds, looping, _, events = CLIPS[n]
        ev = ", ".join(f'{{"{e}", {s:.4f}f}}' for e, s in events) or "{0}"
        lines.append(f'    [ARMS_CLIP_{n.upper()}] = {{"{n}", {seconds:.4f}f, {int(looping)}, '
                     f"{len(events)}, {{{ev}}}}},")
    m = key_on_finger()
    cols = ", ".join(f"{m[r][c]:.6f}f" for c in range(4) for r in range(4))
    lines += [
        "};",
        "",
        "// The pickup's framing, which `raise` begins and `lower` ends in: the eye standing",
        "// ARMS_PICKUP_DISTANCE metres from the cage's axis, square to its face, its base",
        "// ARMS_PICKUP_DROP below the eye.",
        f"#define ARMS_PICKUP_DISTANCE {PICKUP_DISTANCE:.4f}f",
        f"#define ARMS_PICKUP_DROP     {PICKUP_DROP:.4f}f",
        "// The door swings open in `open` between these seconds, eased by a smoothstep, to its",
        "// open angle in radians about its hinge.",
        f"#define ARMS_DOOR_FROM {DOOR_FROM:.4f}f",
        f"#define ARMS_DOOR_TO   {DOOR_TO:.4f}f",
        f"#define ARMS_DOOR_OPEN {cage_model.DOOR_OPEN:.4f}f",
        "// The bone the key rides from the `key` event to `away`, and the key's frame in it,",
        "// column-major.",
        f'#define ARMS_KEY_BONE "{KEY_BONE}"',
        f"static const float ARMS_KEY_ON_BONE[16] = {{{cols}}};",
        "// Where the flashlight's lens is clipped on, from the standing eye in the body's frame: x",
        "// the player's left, y up, z the way they face. The `Lamp` bone starts there.",
        "static const float ARMS_LAMP[3] = {"
        f"{LAMP_AT.x:.4f}f, {LAMP_AT.y:.4f}f, {LAMP_AT.z:.4f}f}};",
        "",
        "#endif // _SILENT_ARMS_CLIPS_H_",
        "",
    ]
    with open(HEADER, "w") as f:
        f.write("\n".join(lines))
    # Laid out as the commit hook lays it out, so a run that changes nothing changes no byte.
    import shutil
    import subprocess

    fmt = shutil.which("clang-format") or "/opt/homebrew/bin/clang-format"
    if os.path.exists(fmt):
        subprocess.run([fmt, "-i", HEADER], check=True)


# ---------------------------------------------------------------------------------------------
# The arms as the player sees them, lit by the lamp on their chest: stills of single instants and
# movies of the clips, each with the whole of the arms from the player's right beside it.


class Stage:
    """The cage on a bench in a dark room, the arms, the lamp on the chest and a camera at the
    eye."""

    def __init__(self, arm, rig, width):
        self.arm, self.rig = arm, rig
        scene = self.scene = bpy.context.scene
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=cage_model.CAGE_GLB)
        # By the names the file gives them, whatever Blender suffixed to a second import's.
        found = {re.sub(r"\.\d+$", "", o.name): o for o in set(bpy.data.objects) - before}
        self.cage, self.shell = found["cage"], found["cage_shell"]
        self.door, self.hang = found["cage_door"], found["key_hang"]
        self.door_rest = self.door.matrix_basis.copy()
        self.key = cage_model.key_stand_in(self.hang)
        # A bench, the wall behind it and the floor: dark, as the basement is.
        room_m, _ = material("preview_room", (0.06, 0.055, 0.05), 0.8)
        room = Parts()
        for lo, hi in ((Vector((-0.7, -PICKUP_DROP - 0.04, PICKUP_DISTANCE - 0.3)),
                        Vector((0.7, -PICKUP_DROP, PICKUP_DISTANCE + 0.3))),
                       (Vector((-1.5, -1.62, PICKUP_DISTANCE + 0.3)),
                        Vector((1.5, 0.8, PICKUP_DISTANCE + 0.32))),
                       (Vector((-1.5, -1.64, -1.0)), Vector((1.5, -1.62, 1.5)))):
            verts = [Vector((x, y, z)) for x in (lo.x, hi.x) for y in (lo.y, hi.y)
                     for z in (lo.z, hi.z)]
            room.add(verts, [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6),
                             (0, 2, 6, 4), (1, 5, 7, 3)])
        finish("preview_room", room, room_m)
        scene.render.resolution_x, scene.render.resolution_y = width, width * 9 // 16
        world = scene.world or bpy.data.worlds.new("w")
        scene.world = world
        world.use_nodes = True
        bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
        bg.inputs["Color"].default_value = (0.02, 0.02, 0.025, 1.0)
        self.cam_data = bpy.data.cameras.new("eye")
        self.cam_data.sensor_fit = "VERTICAL"
        self.cam_data.angle = math.radians(68.0)
        self.cam_data.clip_start = 0.05
        self.cam = bpy.data.objects.new("eye", self.cam_data)
        scene.collection.objects.link(self.cam)
        scene.camera = self.cam
        # The flashlight as the game has it: 900 candela, full inside 9 degrees and gone by 21. A
        # cage 0.4 m from it is lit at 5600 lux, so the picture is exposed for that.
        scene.view_settings.exposure = -2.0
        torch_data = bpy.data.lights.new("torch", "SPOT")
        torch_data.energy = 900.0 * 4.0 * math.pi / 683.0
        torch_data.spot_size = math.radians(42.0)
        torch_data.spot_blend = 1.0 - 9.0 / 21.0
        torch_data.shadow_soft_size = 0.01
        self.torch = bpy.data.objects.new("torch", torch_data)
        scene.collection.objects.link(self.torch)
        # The lamp itself, seen only from the side, as the game draws no body; the light leaves
        # its lens, so the lamp casts nothing. Nor do the arms, as in the game.
        before = set(bpy.data.objects)
        bpy.ops.import_scene.gltf(filepath=FLASHLIGHT_GLB)
        found = {re.sub(r"\.\d+$", "", o.name): o for o in set(bpy.data.objects) - before}
        self.lamp = found["flashlight"]
        self.lens = mv(found["lens"].location)
        self.lamp.visible_shadow = False
        for o in arm.children:
            o.visible_shadow = False
        # A soft light from above, so a pose can be read where the torch does not reach.
        fill_data = bpy.data.lights.new("fill", "AREA")
        fill_data.energy = 25.0
        fill_data.size = 1.0
        fill = bpy.data.objects.new("fill", fill_data)
        fill.location = bv(0.3, 1.2, 0.3)
        fill.rotation_euler = bv(0.0, -1.0, 0.0).to_track_quat("-Z", "Y").to_euler()
        scene.collection.objects.link(fill)

    def pose(self, name, clip, t):
        """Everything as `name` has it `t` seconds in: the arms, the cage, its door and the key."""
        f = clip.at(t)
        for n, b in self.rig.basis(f).items():
            self.arm.pose.bones[n].matrix_basis = b
        self.cage.matrix_world = BV4 @ f.cage @ BV4.inverted()
        angle = door_angle(t) if name == "open" else (
            cage_model.DOOR_OPEN if name in ("wait", "take") else 0.0)
        self.door.matrix_basis = self.door_rest @ Matrix.Rotation(angle, 4, "Z")
        bpy.context.view_layer.update()
        M = solve(f)
        if name == "take" and t >= KEY_TAKEN:
            # The stand-in's mesh is in Blender's space, so its frame is the body's turned there.
            self.key.matrix_world = BV4 @ M[KEY_BONE] @ key_on_finger() @ BV4.inverted()
            self.key.hide_render = t >= KEY_GONE
        else:
            self.key.matrix_world = self.hang.matrix_world
            self.key.hide_render = False
        self.view = M["View"]
        lamp = M["Lamp"]
        self.torch.location = bv(*lamp.translation)
        self.torch.rotation_euler = bv(*(lamp.to_3x3() @ Vector((0.0, 1.0, 0.0)))).to_track_quat(
            "-Z", "Y").to_euler()
        # The lamp's model, its lens toward +z, put with its lens on the bone and facing along it.
        self.lamp.matrix_world = (BV4 @ lamp @ LAMP_TURN @ Matrix.Translation(-self.lens) @
                                  BV4.inverted())

    def shoot(self, which, path):
        """The player's own view, or the whole of the arms from their right, into `path`."""
        self.lamp.hide_render = which == "eye"
        if which == "eye":
            self.cam_data.type = "PERSP"
            self.cam.location = bv(*self.view.translation)
            look = self.view.to_3x3() @ Vector((0.0, 1.0, 0.0))
            self.cam.rotation_euler = bv(*look).to_track_quat("-Z", "Y").to_euler()
        else:
            self.cam_data.type = "ORTHO"
            self.cam_data.ortho_scale = 1.3
            at = Vector((0.0, -0.4, 0.3))
            self.cam.location = bv(*(at + Vector((-2.0, 0.3, 0.0))))
            self.cam.rotation_euler = (bv(*at) - self.cam.location).to_track_quat(
                "-Z", "Y").to_euler()
        self.scene.render.filepath = path
        bpy.ops.render.render(write_still=True)


def stills(arm, rig, specs, width=640):
    """Each clip@seconds in `specs`, from the eye and from the side, tiled into one sheet."""
    import numpy as np

    stage = Stage(arm, rig, width)
    stage.scene.render.engine = "CYCLES"
    cage_model.use_gpu()
    stage.scene.cycles.samples = 96
    stage.scene.cycles.use_denoising = True
    os.makedirs(os.path.join(OUT, "preview"), exist_ok=True)
    clips = {}
    tiles = []
    for spec in specs:
        name, _, at = spec.partition("@")
        t = float(at or 0.0)
        clip = clips.setdefault(name, clip_named(name))
        stage.pose(name, clip, t)
        for which in ("eye", "side"):
            path = os.path.join(OUT, "preview", f"{name}_{t:.2f}_{which}.png")
            stage.shoot(which, path)
            img = bpy.data.images.load(path)
            px = np.array(img.pixels[:], dtype=np.float32).reshape(img.size[1], img.size[0], 4)
            bpy.data.images.remove(img)
            tiles.append(px)
    cols = 4
    while len(tiles) % cols:
        tiles.append(np.zeros_like(tiles[0]))
    rows = [np.concatenate(tiles[i:i + cols], axis=1) for i in range(0, len(tiles), cols)]
    sheet = np.concatenate(rows[::-1], axis=0)
    h, w = sheet.shape[:2]
    out = bpy.data.images.new("sheet", w, h, alpha=True)
    out.pixels.foreach_set(sheet.ravel())
    out.filepath_raw = os.path.join(OUT, "preview", "sheet.png")
    out.file_format = "PNG"
    out.save()
    print(f"preview: {out.filepath_raw}", flush=True)


def movies(arm, rig, names, width=640):
    """The clips in `names` played in turn at FPS, the eye's view beside the side's, rendered by
    EEVEE into one movie with ffmpeg."""
    import shutil
    import subprocess

    stage = Stage(arm, rig, width)
    for engine in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
        try:
            stage.scene.render.engine = engine
            break
        except TypeError:
            continue
    stage.scene.eevee.taa_render_samples = 16
    frames = os.path.join(OUT, "movie")
    os.makedirs(frames, exist_ok=True)
    n = 0
    for name in names:
        clip = clip_named(name)
        for f in range(int(round(clip.seconds * FPS))):
            stage.pose(name, clip, f / FPS)
            for which in ("eye", "side"):
                stage.shoot(which, os.path.join(frames, f"{which}_{n:05d}.png"))
            n += 1
        print(f"movie: {name}, {n} frames so far", flush=True)
    ffmpeg = shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"
    path = os.path.join(OUT, "movie.mp4")
    subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-framerate", str(FPS),
                    "-i", os.path.join(frames, "eye_%05d.png"), "-framerate", str(FPS),
                    "-i", os.path.join(frames, "side_%05d.png"),
                    "-filter_complex", "[0:v][1:v]hstack=inputs=2", "-frames:v", str(n),
                    "-pix_fmt", "yuv420p", "-crf", "18", path], check=True)
    print(f"movie: {path}", flush=True)


CLIP_TOL = 0.001  # how far skin may sink into the iron before it counts as going through it
# Rays a point is tested inside the iron by, each counting the surfaces it crosses: odd is
# inside. The nearest face's facing cannot say, at a crease like the shell's bottom rim.
INSIDE_RAYS = (Vector((0.31, 0.77, 0.56)).normalized(), Vector((-0.62, 0.21, -0.75)).normalized(),
               Vector((0.48, -0.83, 0.28)).normalized())


def inside(tree, p):
    """Whether `p` is inside the closed mesh `tree` holds: most of INSIDE_RAYS cross it an odd
    number of times."""
    odd = 0
    for d in INSIDE_RAYS:
        crossings, o = 0, p
        while True:
            hit = tree.ray_cast(o, d)[0]
            if hit is None:
                break
            crossings += 1
            o = hit + d * 1e-5
        odd += crossings % 2
    return odd >= 2


def clipping(arm, rig, names):
    """Where the arms' skin, and the key once it is taken, goes into the cage's iron over each
    clip: how many frames, the deepest, where and in what. A hand on the cage touches it and
    never goes in, and a key drawn out of the door never scrapes through its rim."""
    import numpy as np

    stage = Stage(arm, rig, 64)
    dg = bpy.context.evaluated_depsgraph_get()
    solids = []
    for ob in (stage.cage, stage.shell, stage.door):
        lo = Vector(tuple(min(c[i] for c in ob.bound_box) for i in range(3)))
        hi = Vector(tuple(max(c[i] for c in ob.bound_box) for i in range(3)))
        solids.append((ob, BVHTree.FromObject(ob, dg), lo - Vector((0.01,) * 3),
                       hi + Vector((0.01,) * 3)))
    skins = [o for o in arm.children if o.type == "MESH"]
    for name in names:
        clip = clip_named(name)
        runs = []  # [first frame, last frame, deepest, what]
        for f in range(int(round(clip.seconds * FPS)) + 1):
            t = f / FPS
            stage.pose(name, clip, t)
            dg = bpy.context.evaluated_depsgraph_get()
            carried = name == "take" and KEY_TAKEN <= t < KEY_GONE
            worst = (0.0, None)
            for ob in skins + ([stage.key] if carried else []):
                ev = ob.evaluated_get(dg)
                me = ev.to_mesh()
                co = np.empty(len(me.vertices) * 3, dtype=np.float64)
                me.vertices.foreach_get("co", co)
                ev.to_mesh_clear()
                pts = co.reshape(-1, 3) @ np.array(ob.matrix_world)[:3, :3].T + \
                    np.array(ob.matrix_world)[:3, 3]
                for solid, tree, lo, hi in solids:
                    inv = np.array(solid.matrix_world.inverted())
                    local = pts @ inv[:3, :3].T + inv[:3, 3]
                    near = np.nonzero(np.all((local >= lo) & (local <= hi), axis=1))[0]
                    for i in near[::3]:
                        p = Vector(local[i])
                        loc, _, _, dist = tree.find_nearest(p)
                        if loc is not None and dist > CLIP_TOL and inside(tree, p) and \
                                dist > worst[0]:
                            groups = ob.vertex_groups
                            bone = max(ob.data.vertices[i].groups, key=lambda g: g.weight,
                                       default=None)
                            worst = (dist, f"{t:.2f} s {ob.name}"
                                           f"{' ' + groups[bone.group].name if bone else ''}"
                                           f" in {solid.name} at "
                                           f"{tuple(round(c, 3) for c in mv(p))}")
            if worst[1] is None:
                continue
            if runs and runs[-1][1] == f - 1:
                run = runs[-1]
                run[1] = f
                if worst[0] > run[2]:
                    run[2:] = list(worst)
            else:
                runs.append([f, f, *worst])
        print(f"clip check {name}: {sum(r[1] - r[0] + 1 for r in runs)} frames through the cage",
              flush=True)
        for first, last, depth, what in runs:
            print(f"    {first / FPS:.2f}..{last / FPS:.2f} s: {1000.0 * depth:.1f} mm, deepest "
                  f"{what}", flush=True)


def wrist_report(name, clip):
    """How far each wrist bends and its forearm turns over a clip, degrees, so a pose no wrist
    takes shows: the back of the hand toward the forearm (+) or the palm, toward the thumb (+) or
    away, and the forearm's turn from the bind pose's."""
    rest = solve(REST)
    for s, side in SIDES:
        bend, dev, turn = [], [], []
        q0 = rest[f"ForeArm.{s}"].to_quaternion().rotation_difference(
            rest[f"ForeTwist.{s}"].to_quaternion())
        a0 = 2.0 * math.atan2(q0.y, q0.w)
        for f in range(int(round(clip.seconds * FPS)) + 1):
            M = solve(clip.at(f / FPS))
            d = M[f"Hand.{s}"].to_3x3().transposed() @ (
                M[f"ForeArm.{s}"].to_3x3() @ Vector((0.0, 1.0, 0.0)))
            bend.append(math.degrees(math.asin(max(-1.0, min(1.0, -d.z)))))
            dev.append(math.degrees(math.asin(max(-1.0, min(1.0, -d.x * side)))))
            q = M[f"ForeArm.{s}"].to_quaternion().rotation_difference(
                M[f"ForeTwist.{s}"].to_quaternion())
            a = 2.0 * math.atan2(q.y, q.w) - a0
            turn.append(math.degrees(math.atan2(math.sin(a), math.cos(a))))
        print(f"wrist {name} {s}: back {min(bend):+.0f}..{max(bend):+.0f}, toward the thumb "
              f"{min(dev):+.0f}..{max(dev):+.0f}, forearm turned {min(turn):+.0f}.."
              f"{max(turn):+.0f}", flush=True)


# ---------------------------------------------------------------------------------------------


def args_after_dashes():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--preview", default="", help="comma-separated clip@seconds stills")
    ap.add_argument("--movie", default="",
                    help="comma-separated clips to play in turn into out/arms_blender/movie.mp4")
    ap.add_argument("--clip-check", action="store_true",
                    help="report every frame the arms or the key go into the cage")
    ap.add_argument("--no-export", action="store_true", help="build and preview only")
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    return ap.parse_args(argv)


def main():
    args = args_after_dashes()
    specs = [s for s in args.preview.split(",") if s]
    played = [s for s in args.movie.split(",") if s]
    bad = [s for s in specs + played if s.partition("@")[0] not in CLIPS]
    if bad:
        sys.exit(f"no preview of {', '.join(bad)}: a clip ({', '.join(CLIPS)})[@seconds]")
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = FPS
    load_hand()
    arm = build_armature()
    objects = build_meshes(arm)
    tris = sum(len(p.vertices) - 2 for ob in objects for p in ob.data.polygons)
    print(f"arms: {len(BONES)} bones, {tris} triangles", flush=True)
    rig = Rig(arm)
    for name in CLIPS:
        clip = clip_named(name)
        key_clip(arm, rig, name, clip)
        print(f"clip {name}: {clip.seconds:.2f} s", flush=True)
        wrist_report(name, clip)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, "arms.blend"))
    if args.no_export:
        print("not exported", flush=True)
    else:
        export(GLB)
        self_check(GLB)
        write_header()
        print(f"wrote {GLB} and {HEADER}", flush=True)
    if args.clip_check:
        clipping(arm, rig, list(CLIPS))
    if specs:
        stills(arm, rig, specs)
    if played:
        movies(arm, rig, played)


if __name__ == "__main__":
    main()
