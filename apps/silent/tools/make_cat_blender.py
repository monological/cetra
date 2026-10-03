"""silent's cat, modelled, rigged and animated in Blender (spec 13.17).

    /Applications/Blender.app/Contents/MacOS/Blender -b --factory-startup \
        -P apps/silent/tools/make_cat_blender.py -- [--clips walk,sit] [--preview walk@0.2,sit@1]

Builds everything from nothing, so nothing in a user's Blender reaches it, and writes:

  - assets/models/cat.glb: the mesh, the skin, the materials and every clip;
  - apps/silent/src/cat_clips.h, GENERATED: each clip's length, loop flag, stated travel and
    events, and the anchors the game reads -- rerun this, never edit the header;
  - out/cat_blender/cat.blend, to open and look at, and with --preview, PNG stills of poses.

Everything is authored in MODEL space as the engine sees it: x is the cat's left, y up, z
forward, the feet at y = 0. Blender is z up and the cat faces its -y; `bv` is the one place the
two meet.

THE RIG. `Hips` carries the travel and nothing else: the engine reads root motion off its
translation and its heading, so it is bound with an identity rotation and every posture lives on
its child `Rump`. A clip that sits or curls therefore never moves Hips, never states a travel and
never switches root motion on. The legs are posed by IK from where each paw stands, so a planted
paw is planted: a walk's stance paw holds one model-space point while Hips carries the body past
it. Everything else is forward kinematics from per-bone rotations stated about MODEL axes, which
compose down the chain the way they read.

THE CLIPS are functions of time sampled at 30 fps into one action each. Every bone gets a
translation track: an imported joint with rotation keys and none for position reads position 0
and collapses onto its parent (gen_puppet_fixture.py's lesson), and the self-check holds the
export to it.
"""

import argparse
import math
import os
import re
import sys
from functools import partial
from typing import NamedTuple

import bpy
import bmesh
from mathutils import Matrix, Quaternion, Vector
from mathutils.bvhtree import BVHTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glb import accessor, read_glb  # noqa: E402 -- the tools' own module, found beside this one

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT = os.path.join(ROOT, "out", "cat_blender")
GLB = os.path.join(ROOT, "assets", "models", "cat.glb")
HEADER = os.path.join(ROOT, "apps", "silent", "src", "cat_clips.h")
FPS = 30


# ---------------------------------------------------------------------------------------------
# Spaces


def bv(x, y, z):
    """A model-space point (x left, y up, z forward) in Blender's space."""
    return Vector((x, -z, y))


def mv(v):
    """A Blender-space vector in model space."""
    return Vector((v.x, v.z, -v.y))


# The model axes as Blender vectors: a rotation about one of these is the same rotation in
# either space.
AX = Vector((1.0, 0.0, 0.0))   # the cat's left
AY = Vector((0.0, 0.0, 1.0))   # up
AZ = Vector((0.0, -1.0, 0.0))  # forward


def rot(pitch=0.0, yaw=0.0, roll=0.0):
    """Degrees about the model axes, roll applied first and yaw last. Positive pitch lifts the
    nose, positive yaw turns left, positive roll drops the right side."""
    return (Quaternion(AY, math.radians(yaw)) @ Quaternion(AX, math.radians(-pitch))
            @ Quaternion(AZ, math.radians(roll)))


def smooth(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


# ---------------------------------------------------------------------------------------------
# The skeleton, in model space: (name, parent, head, tail). Parent first.

def _tail_chain():
    """Six bones of 5 cm from the tail's root, drooping and then curling up at the tip."""
    droop = (20.0, 32.0, 30.0, 20.0, 5.0, -15.0)
    bones, p, parent = [], Vector((0.0, 0.212, -0.165)), "Rump"
    for i, a in enumerate(droop):
        d = Vector((0.0, -math.sin(math.radians(a)), -math.cos(math.radians(a))))
        q = p + d * 0.05
        bones.append((f"Tail{i + 1}", parent, tuple(p), tuple(q)))
        p, parent = q, f"Tail{i + 1}"
    return bones


# The left eye's centre: on the front of the skull and a little to its side. Its depth here is a
# first guess; `place_eyes` sets it from the face the metaballs actually made, since blending
# swells a surface by an amount no number written down beforehand knows, and a guess that lands
# a few millimetres short buries the eyes.
EYE_GUESS = (0.0215, 0.279, 0.233)
# How far the front of the eye stands proud of the face round it.
EYE_PROUD = 0.003


def _limb(start, segments):
    """Joint positions down a leg from `start`: each segment a length in metres and a direction
    as degrees from straight down, positive toward the front."""
    out, p = [tuple(start)], Vector(start)
    for length, ahead in segments:
        a = math.radians(ahead)
        p = p + Vector((0.0, -math.cos(a), math.sin(a))) * length
        out.append((p.x, p.y, p.z))
    return out


# Shoulder joint, elbow, wrist, ball of the paw, tip of the toes.
FRONT = _limb((0.045, 0.199, 0.118), [(0.098, -40.0), (0.092, 8.0), (0.028, 25.0), (0.021, 80.0)])
# Hip joint, stifle, hock, ball of the paw, tip of the toes. The femur stands 30 degrees off
# vertical: at 45 the stifle sat 7 cm ahead of the hip, under the belly, and the leg read as a
# dog's. The shin's angle is what puts the paw on the floor from there.
HIND = _limb((0.045, 0.215, -0.125), [(0.105, 30.0), (0.112, -60.6), (0.062, 10.0), (0.024, 80.0)])


def _sided(name, parent, head, tail):
    """The left bone as stated and its mirror on the right."""
    out = []
    for side, s in (("L", 1.0), ("R", -1.0)):
        par = parent.replace(".L", f".{side}") if parent.endswith(".L") else parent
        out.append((f"{name}.{side}", par, (s * head[0], head[1], head[2]),
                    (s * tail[0], tail[1], tail[2])))
    return out


def skeleton(eye_at):
    """The bones, parent first, with the eyes at `eye_at` (the left one)."""
    return [
    # Hips points straight UP: the glTF export re-expresses a bone's frame in its own axes, and
    # only a vertical bone with no roll comes out with its local +Z forward and an identity
    # rotation, which is how the engine reads a heading off it.
    ("Hips", None, (0.0, 0.215, -0.12), (0.0, 0.265, -0.12)),
    ("Rump", "Hips", (0.0, 0.215, -0.12), (0.0, 0.215, -0.165)),
    ("Spine1", "Rump", (0.0, 0.215, -0.12), (0.0, 0.225, -0.04)),
    ("Spine2", "Spine1", (0.0, 0.225, -0.04), (0.0, 0.23, 0.04)),
    ("Chest", "Spine2", (0.0, 0.23, 0.04), (0.0, 0.232, 0.11)),
    ("Neck1", "Chest", (0.0, 0.232, 0.11), (0.0, 0.25, 0.15)),
    ("Neck2", "Neck1", (0.0, 0.25, 0.15), (0.0, 0.27, 0.18)),
    ("Head", "Neck2", (0.0, 0.27, 0.18), (0.0, 0.28, 0.255)),
    ("Jaw", "Head", (0.0, 0.252, 0.212), (0.0, 0.246, 0.248)),
    *_sided("Ear", "Head", (0.0249, 0.2958, 0.1964), (0.0332, 0.3307, 0.1936)),
    *_sided("Eye", "Head", eye_at, (eye_at[0], eye_at[1], eye_at[2] + 0.016)),
    *_sided("Lid", "Head", eye_at, (eye_at[0], eye_at[1], eye_at[2] + 0.016)),
    # Legs lie in planes of constant x, so a roll of 0 puts every leg bone's X along the model's.
    # Measured, not guessed: an adult domestic cat's humerus is about 98 mm, its radius 92, its
    # femur 105 (78-129), its tibia 112 (CT, Pantangco et al. 2026), its third metatarsal about
    # 62 -- set at a standing cat's angles, the hind leg crouched in its zigzag and the elbow up
    # at the bottom of the chest.
    *_sided("Shoulder", "Chest", (0.03, 0.248, 0.09), FRONT[0]),
    *_sided("UpperArm", "Shoulder.L", FRONT[0], FRONT[1]),
    *_sided("Forearm", "UpperArm.L", FRONT[1], FRONT[2]),
    *_sided("Hand", "Forearm.L", FRONT[2], FRONT[3]),
    *_sided("FrontToe", "Hand.L", FRONT[3], FRONT[4]),
    *_sided("Thigh", "Rump", HIND[0], HIND[1]),
    *_sided("Shin", "Thigh.L", HIND[1], HIND[2]),
    *_sided("Foot", "Shin.L", HIND[2], HIND[3]),
    *_sided("Toe", "Foot.L", HIND[3], HIND[4]),
    *_tail_chain(),
    ]


class Leg(NamedTuple):
    girdle: str  # the bone it hangs from, whose solve places it
    body: str    # what a paw rides when the body carries it rather than the floor
    fore: bool
    upper: str
    lower: str
    meta: str    # the metapodial
    toe: str

    @property
    def bones(self):
        return (self.upper, self.lower, self.meta, self.toe)


LEGS = {
    "FL": Leg("Shoulder.L", "Chest", True, "UpperArm.L", "Forearm.L", "Hand.L", "FrontToe.L"),
    "FR": Leg("Shoulder.R", "Chest", True, "UpperArm.R", "Forearm.R", "Hand.R", "FrontToe.R"),
    "HL": Leg("Rump", "Rump", False, "Thigh.L", "Shin.L", "Foot.L", "Toe.L"),
    "HR": Leg("Rump", "Rump", False, "Thigh.R", "Shin.R", "Foot.R", "Toe.R"),
}
GIRDLES = {leg.girdle for leg in LEGS.values()}

EYE_RADIUS = 0.0085   # the eye, a little over life so it reads at render scale 0.5
EYE_SHAPE = (1.0, 0.66, 0.8)  # across, up and deep: an almond rather than a ball
LID_CLOSE = 145.0     # degrees a lid turns from open (up and back inside the skull) to closed


# ---------------------------------------------------------------------------------------------
# Materials: linear colours. The game re-creates these with its own colours; these are what the
# render app shows.

def srgb(hexcode):
    c = [int(hexcode[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
    return tuple(x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c)


MATERIALS = {
    # name: (base colour, roughness, sheen tint or None)
    # A trace of sheen, to lift a black coat off a dark room: every fur shell is shaded with it,
    # and at the 0.22 a bare surface wants it painted the whole cat grey.
    "cat_fur": (srgb("262424"), 0.65, (0.05, 0.05, 0.06)),
    "cat_eye": (srgb("E8B923"), 0.08, None),
    "cat_pupil": (srgb("050505"), 0.05, None),
    # A black cat's nose leather is the coat's colour with a moist shine, which is all that
    # tells it from the fur round it.
    "cat_nose": (srgb("1C1819"), 0.32, None),
    "cat_ear": (srgb("362225"), 0.8, None),  # the dusky pink inside a black cat's ear
    # The lip line, the philtrum and the nostrils when the mouth is shut, and its inside when it
    # opens: a black cat's lips are black, and the dark of the mouth reads as one from a step
    # away.
    "cat_mouth": (srgb("100C0D"), 0.5, None),
    # A black cat's whiskers are black too, a shade off the coat so they catch a light.
    "cat_whisker": (srgb("3A3836"), 0.35, None),
}

# The nose leather, measured off the reference's front view against the eye's centre line: a
# rounded shield whose top edge sits just under the eyes and whose point is a short philtrum
# above the parting of the lip. In model metres.
NOSE_TOP = 0.2655
NOSE_POINT = 0.2575
NOSE_WIDTH = 0.0125
NOSE_CORNER = 0.0016   # radius the shield's corners are rounded by
NOSE_DOME = 0.0006     # how far its middle stands proud of the face under it
MOUTH_CORNER = 0.0165  # x of the mouth's corners

class Lip:
    """The upper lip's lower edge, (x, y) from the middle out: where the whisker pads overhang
    the chin, seen from the front. The jaw is everything in front below it."""

    def __init__(self, points):
        self.points = points

    def y(self, x):
        """The edge at a distance x out from the middle."""
        x = abs(x)
        for (x0, y0), (x1, y1) in zip(self.points, self.points[1:]):
            if x <= x1:
                return y0 + (y1 - y0) * (x - x0) / max(x1 - x0, 1e-9)
        return self.points[-1][1]

    def jaw(self, p):
        """Whether a model-space point is part of the lower jaw."""
        return p.z > 0.226 and abs(p.x) < MOUTH_CORNER and p.y < self.y(p.x)


# A guess at the lip, which `measure_lip` replaces with what the metaballs made wherever it can
# find the edge.
LIP_GUESS = Lip([(0.0, 0.2505), (0.008, 0.2478), (MOUTH_CORNER, 0.249)])


def nose_outline(per=14):
    """The nose leather's outline in front view, (x, y) clockwise from its top left: a triangle
    point down, the top corners rounded by NOSE_CORNER and the point by less, so it stays a
    point."""
    corners = [(Vector((-NOSE_WIDTH / 2, NOSE_TOP)), NOSE_CORNER),
               (Vector((NOSE_WIDTH / 2, NOSE_TOP)), NOSE_CORNER),
               (Vector((0.0, NOSE_POINT)), NOSE_CORNER * 0.45)]
    out = []
    for k in range(3):
        a, (b, r), c = corners[k - 1][0], corners[k], corners[(k + 1) % 3][0]
        e0, e1 = (b - a).normalized(), (c - b).normalized()
        n0, n1 = Vector((-e0.y, e0.x)), Vector((-e1.y, e1.x))  # outward, for a clockwise loop
        inward = (-n0 - n1).normalized()
        half = math.acos(max(-1.0, min(1.0, -e0.dot(e1)))) / 2.0
        centre = b + inward * (r / math.sin(half))
        t0, t1 = math.atan2(n0.y, n0.x), math.atan2(n1.y, n1.x)
        while t1 > t0:
            t1 -= 2.0 * math.pi
        for i in range(per):
            t = t0 + (t1 - t0) * i / (per - 1)
            out.append(centre + Vector((math.cos(t), math.sin(t))) * r)
    return out


NOSE = nose_outline()
NOSE_CENTRE = sum(NOSE, Vector((0.0, 0.0))) / len(NOSE)


def nose_reach(x, y):
    """How far out from the nose's centre toward its edge a front-view point lies: 0 at the
    centre, 1 on the outline, past 1 outside it."""
    d = Vector((x, y)) - NOSE_CENTRE
    if d.length < 1e-9:
        return 0.0
    u = d.normalized()
    best = None
    for a, b in zip(NOSE, NOSE[1:] + NOSE[:1]):
        # where the ray from the centre along u crosses the edge a-b
        e = b - a
        den = u.x * e.y - u.y * e.x
        if abs(den) < 1e-12:
            continue
        w = a - NOSE_CENTRE
        t = (w.x * e.y - w.y * e.x) / den
        s = (w.x * u.y - w.y * u.x) / den
        if t > 0.0 and -1e-6 <= s <= 1.0 + 1e-6:
            best = t if best is None else min(best, t)
    return d.length / best if best else 99.0


def make_materials():
    mats = {}
    for name, (col, rough, sheen) in MATERIALS.items():
        m = bpy.data.materials.new(name)
        # Single-sided: the glTF export calls a culled material single-sided, and a fold of skin
        # stretched open should show nothing behind it rather than its own lit inside. The
        # whiskers are the exception, being open tubes with no outside to cull by.
        m.use_backface_culling = name != "cat_whisker"
        bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
        bsdf.inputs["Base Color"].default_value = (*col, 1.0)
        bsdf.inputs["Roughness"].default_value = rough
        if sheen:
            bsdf.inputs["Sheen Weight"].default_value = 1.0
            bsdf.inputs["Sheen Tint"].default_value = (*sheen, 1.0)
            bsdf.inputs["Sheen Roughness"].default_value = 0.45
        mats[name] = m
    return mats


# ---------------------------------------------------------------------------------------------
# The armature


def build_armature(bones):
    arm_data = bpy.data.armatures.new("cat_rig")
    arm = bpy.data.objects.new("cat", arm_data)
    bpy.context.scene.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    for name, parent, head, tail in bones:
        eb = arm_data.edit_bones.new(name)
        eb.head, eb.tail, eb.roll = bv(*head), bv(*tail), 0.0
        if parent:
            eb.parent = arm_data.edit_bones[parent]
            eb.use_connect = False
    bpy.ops.object.mode_set(mode="OBJECT")
    for pb in arm.pose.bones:
        pb.rotation_mode = "QUATERNION"
    return arm


# ---------------------------------------------------------------------------------------------
# The body: metaballs blended, polygonised, decimated

# Metaball elements in model space. ("ell", centre, semi-axes) and ("cap", end, end, radius),
# sizes being the SURFACE the element would make alone; `build_body` turns them into Blender's
# influence radii through SURFACE.
BODY = [
    # torso
    # The belly stops short of the thighs and rides above the knees, so a swinging hind leg
    # pulls no web of skin off it.
    ("ell", (0.0, 0.195, -0.115), (0.05, 0.056, 0.064)),    # haunches and pelvis
    ("ell", (0.0, 0.188, -0.012), (0.051, 0.056, 0.076)),   # belly
    ("ell", (0.0, 0.182, 0.068), (0.054, 0.07, 0.072)),     # chest
    ("ell", (0.0, 0.212, 0.158), (0.03, 0.036, 0.032)),     # throat
    ("ell", (0.038, 0.18, 0.098), (0.022, 0.05, 0.036)),    # left shoulder
    ("ell", (-0.038, 0.18, 0.098), (0.022, 0.05, 0.036)),
    ("ell", (0.0, 0.222, 0.095), (0.038, 0.032, 0.05)),     # withers
    # neck and head: a short thick neck, and a head that is a wedge rather than a ball -- a
    # rounded crown narrowing to a short muzzle, a brow over the eyes, a small chin
    ("cap", (0.0, 0.212, 0.1), (0.0, 0.252, 0.168), 0.039),
    ("ell", (0.0, 0.278, 0.205), (0.038, 0.034, 0.036)),    # skull
    ("ell", (0.0, 0.289, 0.222), (0.026, 0.014, 0.016)),    # brow
    ("ell", (0.024, 0.262, 0.217), (0.018, 0.016, 0.018)),  # cheeks, behind the eyes
    ("ell", (-0.024, 0.262, 0.217), (0.018, 0.016, 0.018)),
    # The muzzle in profile is a slope back from the nose: the nose leather is the most forward
    # point, the upper lip under the whisker pads stands 8 mm behind it and the chin 2 cm. Pads
    # and chin as far forward as the nose make a dog's snout.
    ("ell", (0.0, 0.262, 0.232), (0.017, 0.012, 0.014)),    # muzzle, short
    ("ell", (0.0, 0.2715, 0.239), (0.0085, 0.0075, 0.012)),  # the bridge of the nose
    ("ell", (0.0, 0.2615, 0.2475), (0.0062, 0.0052, 0.0065)),  # under the nose leather
    ("ell", (0.0078, 0.2545, 0.2425), (0.0088, 0.0068, 0.0072)),  # whisker pads
    ("ell", (-0.0078, 0.2545, 0.2425), (0.0088, 0.0068, 0.0072)),
    ("ell", (0.0, 0.2465, 0.234), (0.0065, 0.0048, 0.0075)),   # chin, small and set back
]


# The legs' shape: the sections the body's ring mesh (`cage_graph`) is grown through. Seen from
# the side a cat's leg is one outline from the flank to the paw; built from blended blobs it was
# lumps joined by thin stretches, every overlap swelling.

# Each bone's section, rows of (t along the bone, front, back, breadth, inward): how far the
# leg's front and back edges lie from the bone in the side view, its half breadth across, and
# how far its middle sits in toward the body from the bone. Metres, before the coat. Measured
# off the reference's side silhouette at 0.42 mm a pixel (its body is this one's depth and
# length to 5%), less 5 mm for the coat: the forearm 3.3 cm deep below the elbow and 2.2 at
# the wrist, the hind leg 5.5 cm just above the hock, 4.5 at it, the metatarsus 2.4.
HIND_FOOT = [(0.0, 0.01, 0.012, 0.0105, 0.0), (1.0, 0.009, 0.01, 0.0098, 0.0)]
# The hind leg above the hock is ONE mass, not a thigh, a shin and a hamstring each swept along
# its own line: three tubes leave gaps between them that read from the side as a second leg,
# and come apart in the walk. Its front edge follows the bones -- in front of the femur, round
# the stifle, down the front of the shin -- and its back edge is the line a cat's hind leg
# shows from under the tail to the point of the heel, nearly straight and a little hollow.
# Filled by horizontal slices between the two, so there is nothing between them to open.
HIND_FRONT = (0.018, 0.012, 0.01)   # in front of the femur, the stifle, the shin at the hock
HIND_SEAT = (0.192, -0.17)          # (y, z) of the back edge's top, under the tail
# The flank fold: the web of skin from the belly to the stifle, (y, z) where it leaves the belly.
# Without it the thigh is a thin tube hanging from the body with nothing in front of it.
HIND_FOLD = (0.15, -0.035)
HIND_HOLLOW = 0.006                 # how far the back edge bows in between seat and heel
# (y, half breadth across) and (y, most depth front to back), down the leg. A cat's hind leg
# is 4 cm across at the top of the thigh and under 2 at the hock, and seen from the side it
# narrows from the thigh to the hock rather than running down as a column.
HIND_BREADTH = ((0.172, 0.02), (0.13, 0.017), (0.11, 0.013), (0.09, 0.011), (0.074, 0.0095))
HIND_DEPTH = ((0.15, 0.12), (0.13, 0.095), (0.11, 0.065), (0.095, 0.048), (0.08, 0.034),
              (0.07, 0.03))


def _by_height(table, y):
    """A (y, value) table, highest first, read at y and held at its ends."""
    if y >= table[0][0]:
        return table[0][1]
    for (y0, a), (y1, b) in zip(table, table[1:]):
        if y1 <= y <= y0:
            return a + (b - a) * (y0 - y) / (y0 - y1)
    return table[-1][1]
UPPER_ARM = [(0.0, 0.018, 0.024, 0.02, 0.004), (0.6, 0.016, 0.024, 0.019, 0.003),
             (1.0, 0.014, 0.018, 0.017, 0.002)]
FOREARM = [(0.0, 0.016, 0.016, 0.017, 0.002), (0.35, 0.015, 0.013, 0.016, 0.0),
           (1.0, 0.012, 0.01, 0.013, 0.0)]
FORE_FOOT = [(0.0, 0.011, 0.012, 0.013, 0.0), (1.0, 0.01, 0.009, 0.013, 0.0)]
PAW = (0.014, 0.011)   # half breadth and height, the ring the paw is grown through


def _row(table, t):
    for a, b in zip(table, table[1:]):
        if t <= b[0]:
            w = (t - a[0]) / (b[0] - a[0])
            return [x + (y - x) * w for x, y in zip(a[1:], b[1:])]
    return list(table[-1][1:])


def bone_section(a, b, table, t, s):
    """A leg's section a fraction t along the bone a->b: its centre, and its half breadth and
    half depth. The centre sits behind the bone by half the difference of its edges."""
    d = b - a
    u = d.normalized()
    behind = Vector((0.0, -u.z, u.y))
    if behind.z > 0.0:
        behind = -behind
    front, back, breadth, inward = _row(table, t)
    c = a + d * t + behind * ((back - front) / 2.0) - Vector((inward * s, 0.0, 0.0))
    return c, (breadth, (front + back) / 2.0)


def hind_section(h, s, y):
    """The hind leg's horizontal section at height y, between its front edge -- in front of
    the femur, round the stifle with the flank fold above it, down the front of the shin --
    and its back edge, from the seat bone to the point of the heel: its centre, and its half
    breadth and half depth."""
    hip, knee, hock = h[0], h[1], h[2]
    shin = (hock - knee).normalized()
    heel = hock + Vector((0.0, -shin.z, shin.y)) * 0.012

    def along(a, b):
        return a.z + (b.z - a.z) * (a.y - y) / (a.y - b.y)

    if y >= knee.y:
        w = (hip.y - y) / (hip.y - knee.y)
        front = along(hip, knee) + HIND_FRONT[0] + (HIND_FRONT[1] - HIND_FRONT[0]) * w
        stifle = knee.z + HIND_FRONT[1]
        if y <= HIND_FOLD[0]:
            front = max(front, stifle + (HIND_FOLD[1] - stifle) * (y - knee.y)
                        / (HIND_FOLD[0] - knee.y))
    else:
        w = (knee.y - y) / (knee.y - hock.y)
        front = along(knee, hock) + HIND_FRONT[1] + (HIND_FRONT[2] - HIND_FRONT[1]) * w
    u = min(1.0, max(0.0, (HIND_SEAT[0] - y) / (HIND_SEAT[0] - heel.y)))
    back = HIND_SEAT[1] + (heel.z - HIND_SEAT[1]) * u + HIND_HOLLOW * math.sin(math.pi * u)
    back = max(back, front - _by_height(HIND_DEPTH, y))
    x = s * (0.04 + 0.005 * (HIND_SEAT[0] - y) / (HIND_SEAT[0] - hock.y))
    return Vector((x, y, (front + back) / 2.0)), (_by_height(HIND_BREADTH, y), (front - back) / 2.0)


TAIL_ROOT, TAIL_TIP = 0.0155, 0.0095   # radius at the tail's root and at its tip


STIFFNESS = 2.0
THRESHOLD = 0.6
# The surface of a lone element, as a fraction of its influence radius, at that stiffness and
# threshold: Blender's field is s * (1 - d^2/R^2)^3, so the surface sits where that equals the
# threshold.
SURFACE = math.sqrt(1.0 - (THRESHOLD / STIFFNESS) ** (1.0 / 3.0))


# Blender will not polygonise a metaball finer than 5 mm, and a nose is 12 mm across, so the
# elements are built this many times life size and the mesh is scaled back down.
MB_SCALE = 10.0


def build_body(resolution):
    k = MB_SCALE
    mb = bpy.data.metaballs.new("cat_body")
    mb.resolution = resolution * k
    mb.render_resolution = resolution * k
    mb.threshold = THRESHOLD
    for el in BODY:
        if el[0] == "ell":
            _, c, semi = el
            e = mb.elements.new(type="ELLIPSOID")
            e.co = bv(*c) * k
            e.radius = 1.0 / SURFACE
            # Blender's sizes are in its own axes: model x, z and y are Blender x, y and z.
            e.size_x, e.size_y, e.size_z = semi[0] * k, semi[2] * k, semi[1] * k
        else:
            _, a, b, r = el
            a, b = bv(*a) * k, bv(*b) * k
            e = mb.elements.new(type="CAPSULE")
            e.co = (a + b) / 2
            e.radius = r * k / SURFACE
            e.size_x = (b - a).length / 2
            e.rotation = Vector((1.0, 0.0, 0.0)).rotation_difference((b - a).normalized())
        e.stiffness = STIFFNESS
    obj = bpy.data.objects.new("cat_body_mb", mb)
    bpy.context.scene.collection.objects.link(obj)
    dg = bpy.context.evaluated_depsgraph_get()
    mesh = bpy.data.meshes.new_from_object(obj.evaluated_get(dg))
    mesh.transform(Matrix.Scale(1.0 / k, 4))
    bpy.data.objects.remove(obj)
    body = bpy.data.objects.new("cat_mesh", mesh)
    bpy.context.scene.collection.objects.link(body)
    return body


# ---------------------------------------------------------------------------------------------
# The body below the head as a ring mesh. Polygonised and collapsed, a metaball body is a mesh
# with no structure at the joints, and skinned it creases wherever its triangles happen to fall:
# a swinging thigh folded into a fin. So the body is grown as quads by Blender's skin modifier
# over a stick figure -- a ring at every point and rings between, which is the topology a limb
# bends along -- and subdivided. The stick figure carries the SHAPE: its points are the middles
# of the torso's and the legs' sections, not the bones, and each ring is an oval sized to its
# section. The head keeps its metaball mesh, whose face is millimetres, bridged on at the neck.

# Where the head's mesh is cut from the body's: a plane across the neck, square to it.
NECK_CUT = (0.0, 0.245, 0.145)
NECK_AXIS = (0.0, 0.477, 0.879)
NECK_GAP = 0.003   # the body's mesh stops this far short of the cut; the bridge fills it
NECK_SEAM = 0.012  # how far either side of the cut the skin is relaxed across the bridge
CAGE_LEVELS = 2    # subdivisions of the skin modifier's quads: 16 faces round a limb
# A skin modifier ring is a square of half side `radius`, and subdivided twice its surface comes
# in to 0.917 of that across the middle of each side, so every radius is stated over it.
CAGE_FILL = 1.0 / 0.917

# The torso's sections from the root of the tail to the base of the neck: (centre, half width,
# half height), the measured shape the metaball torso had -- the haunch, the loin, the belly,
# the deep chest, the shoulders.
TORSO = [((0.0, 0.203, -0.175), 0.028, 0.03), ((0.0, 0.197, -0.135), 0.048, 0.054),
         ((0.0, 0.192, -0.07), 0.049, 0.055), ((0.0, 0.19, -0.005), 0.051, 0.058),
         ((0.0, 0.185, 0.055), 0.054, 0.068), ((0.0, 0.19, 0.1), 0.052, 0.064),
         ((0.0, 0.217, 0.128), 0.044, 0.05)]
HIND_LEVELS = (0.165, 0.14, 0.12, 0.1, 0.085)   # heights the hind leg's sections are taken at


def neck_ring(bm):
    """The head's cut edge measured: its centre, half breadth and half height in the cut's own
    plane, which the body's last rings are sized to so the bridge between them is level."""
    n = Vector(NECK_AXIS).normalized()
    up = Vector((0.0, n.z, -n.y))
    pts = [mv(v.co) for v in bm.verts if v.is_boundary]
    centre = sum(pts, Vector()) / len(pts)
    across = max(abs(p.x - centre.x) for p in pts)
    tall = (max(p.dot(up) for p in pts) - min(p.dot(up) for p in pts)) / 2.0
    return centre, across, tall


class Seg(NamedTuple):
    """A segment of the stick figure, between two of its points: which bones move the skin grown
    round it -- its `kind`, SPINE, BRANCH or CHAIN, says how -- and the leg it belongs to."""
    a: int
    b: int
    kind: str
    bones: dict = None
    ja: object = None  # a CHAIN's joint at a, eased into: bones, or SPINE
    jb: dict = None    # and at b
    leg: str = None    # "FL" and so on; None on the spine and the tail


def cage_graph(neck):
    """The stick figure the body is grown on: points, each point's (half breadth, half depth
    across its chain), and the segments between them. A segment says which bones move the skin
    grown round it: SPINE (the spine's own partition, `spine_weights`), BRANCH (from the spine
    at its start to its leg's top bones at its end), or a CHAIN segment's bones, eased into the
    joint at either end where one is given."""
    pts, rad, segs = [], [], []

    def node(p, r):
        pts.append(Vector(p))
        rad.append(r)
        return len(pts) - 1

    def chain(start, links, first, leg=None):
        """A leg or the tail: `links` are (point, radii, bones, joint at its end)."""
        prev, joint = start, None
        for i, (p, r, bones, end_joint) in enumerate(links):
            k = node(p, r)
            if i == 0 and first == BRANCH:
                segs.append(Seg(prev, k, BRANCH, bones, leg=leg))
                joint = bones
            else:
                segs.append(Seg(prev, k, CHAIN, bones, joint if i else first, end_joint, leg))
                joint = end_joint
            prev = k

    # Along the torso from the tail's root, then two rings at the neck straddling the cut.
    ids = [node(c, (w, h)) for c, w, h in TORSO]
    for a, b in zip(ids, ids[1:]):
        segs.append(Seg(a, b, SPINE))
    n = Vector(NECK_AXIS).normalized()
    centre, across, tall = neck
    # The far ring is well past the cut, so the end's rounding is cut away with it.
    n1 = node(centre - n * 0.008, (across, tall))
    n2 = node(centre + n * 0.03, (across, tall))
    segs.append(Seg(ids[-1], n1, SPINE))
    segs.append(Seg(n1, n2, SPINE))
    tail = _tail_chain()
    links = []
    for i, (name, _, _, tl) in enumerate(tail):
        r = TAIL_ROOT + (TAIL_TIP - TAIL_ROOT) * (i + 1) / len(tail)
        nxt = tail[i + 1][0] if i + 1 < len(tail) else None
        links.append((tl, (r, r), {name: 1.0}, {name: 0.5, nxt: 0.5} if nxt else None))
    chain(ids[0], links, SPINE)
    for s, side in ((1.0, "L"), (-1.0, "R")):
        f = [Vector((p[0] * s, p[1], p[2])) for p in FRONT]
        h = [Vector((p[0] * s, p[1], p[2])) for p in HIND]

        def b(name):
            return f"{name}.{side}"

        def one(name):
            return {b(name): 1.0}

        def joint(x, y):
            return {b(x): 0.5, b(y): 0.5}

        fore = [
            (*bone_section(f[0], f[1], UPPER_ARM, 0.3, s), {b("UpperArm"): 0.6, b("Shoulder"): 0.4}, None),
            (*bone_section(f[0], f[1], UPPER_ARM, 0.75, s), one("UpperArm"), None),
            (*bone_section(f[1], f[2], FOREARM, 0.0, s), one("UpperArm"), joint("UpperArm", "Forearm")),
            (*bone_section(f[1], f[2], FOREARM, 0.45, s), one("Forearm"), None),
            (*bone_section(f[2], f[3], FORE_FOOT, 0.0, s), one("Forearm"), joint("Forearm", "Hand")),
            (f[3] + Vector((0.0, 0.005, 0.002)), PAW, one("Hand"), joint("Hand", "FrontToe")),
            (Vector((f[4].x, 0.0075, f[4].z)), (0.011, 0.0065), one("FrontToe"), None),
        ]
        hind = [(*hind_section(h, s, HIND_LEVELS[0]), {b("Thigh"): 0.7, "Rump": 0.3}, None),
                (*hind_section(h, s, HIND_LEVELS[1]), one("Thigh"), None),
                (*hind_section(h, s, HIND_LEVELS[2]), one("Thigh"), joint("Thigh", "Shin")),
                (*hind_section(h, s, HIND_LEVELS[3]), one("Shin"), None),
                (*hind_section(h, s, HIND_LEVELS[4]), one("Shin"), None),
                (*bone_section(h[2], h[3], HIND_FOOT, 0.0, s), one("Shin"), joint("Shin", "Foot")),
                (*bone_section(h[2], h[3], HIND_FOOT, 0.5, s), one("Foot"), None),
                (h[3] + Vector((0.0, 0.005, 0.002)), PAW, one("Foot"), joint("Foot", "Toe")),
                (Vector((h[4].x, 0.0075, h[4].z)), (0.011, 0.0065), one("Toe"), None)]
        chain(ids[5], fore, BRANCH, "F" + side)
        chain(ids[1], hind, BRANCH, "H" + side)
    return pts, rad, segs


def _tube(pts, rad, segs, root):
    """A run of the stick figure as skin: the skin modifier's quads over `segs`, subdivided,
    rooted at the point `root`."""
    used = sorted({i for a, b, *_ in segs for i in (a, b)})
    index = {i: k for k, i in enumerate(used)}
    me = bpy.data.meshes.new("cat_tube")
    me.from_pydata([bv(*pts[i]) for i in used], [(index[a], index[b]) for a, b, *_ in segs], [])
    obj = bpy.data.objects.new("cat_tube", me)
    bpy.context.scene.collection.objects.link(obj)
    skin_mod = obj.modifiers.new("skin", "SKIN")
    skin_mod.branch_smoothing = 0.5
    for i in used:
        r = rad[i]
        me.skin_vertices[0].data[index[i]].radius = (r[0] * CAGE_FILL, r[1] * CAGE_FILL)
    me.skin_vertices[0].data[index[root]].use_root = True
    sub = obj.modifiers.new("subdivide", "SUBSURF")
    sub.levels = CAGE_LEVELS
    apply_modifiers(obj)
    return obj


# The forelegs are joined on by a boolean union, and the skin within this reach of where each
# meets the chest relaxed this many rounds, so the join is a fillet and not a crease.
FORE_JOIN_REACH = 0.05
FORE_JOIN_RELAX = 15


def _fore(seg):
    """Whether a segment belongs to a foreleg, its branch from the chest included."""
    return seg.leg is not None and LEGS[seg.leg].fore


def build_cage(graph):
    """The body below the head. The torso, the neck, the tail and the hind legs are one
    branched skin. The forelegs are grown apart, each starting inside the chest, and joined on
    by a boolean union: grown as branches of the same skin, a foreleg's first ring lay inside
    the chest's, and the skin modifier's hull round the two folded into flat panels with
    ridges down the chest and across the shoulder. The hind legs' first rings sit below the
    haunch's and their hull is clean, so they stay branches."""
    pts, rad, segs = graph
    body = _tube(pts, rad, [s for s in segs if not _fore(s)], 3)
    joins = [pts[s.a].lerp(pts[s.b], 0.5) for s in segs if _fore(s) and s.kind == BRANCH]
    for name in ("FL", "FR"):
        mine = [s for s in segs if s.leg == name and s.kind != BRANCH]
        # Rooted where its branch off the chest ends, the leg's first ring.
        first = next(s.b for s in segs if s.leg == name and s.kind == BRANCH)
        leg = _tube(pts, rad, mine, first)
        union = body.modifiers.new("union", "BOOLEAN")
        union.operation = "UNION"
        union.solver = "EXACT"
        union.object = leg
        apply_modifiers(body)
        bpy.data.objects.remove(leg)
    bm = bmesh.new()
    bm.from_mesh(body.data)
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 4])
    near = [v for v in bm.verts if min((mv(v.co) - j).length for j in joins) < FORE_JOIN_REACH]
    for _ in range(FORE_JOIN_RELAX):
        bmesh.ops.smooth_vert(bm, verts=near, factor=0.5, use_axis_x=True, use_axis_y=True,
                              use_axis_z=True)
    bm.to_mesh(body.data)
    bm.free()
    return body


# What moves the skin, said once: a ring of the body follows the bones its segment of the stick
# figure names, eased into a joint's half-and-half over JOINT_EASE either side of it. Heat
# weighting guessed this from distance and got the back of the thigh, 4 cm behind the femur and
# nearer the rump, wrong: it stayed while the leg swung and stretched into a sail between them.
SPINE, BRANCH, CHAIN = "spine", "branch", "chain"
JOINT_EASE = 0.012
# The spine's bones along the body, (where one hands to the next, from, to): by z along the
# torso, then by distance along the neck from the cut, where the neck turns up.
SPINE_ALONG_Z = [(-0.17, "Tail1", "Rump"), (-0.12, "Rump", "Spine1"),
                 (-0.04, "Spine1", "Spine2"), (0.04, "Spine2", "Chest")]
SPINE_ALONG_NECK = [(-0.037, "Chest", "Neck1"), (0.0068, "Neck1", "Neck2"),
                    (0.0427, "Neck2", "Head")]


def _handover(x, table, ease):
    """A partition of unity along one coordinate: each bone owns its stretch and hands to the
    next over `ease` either side of their boundary."""
    w = {table[0][1]: 1.0}
    for at, a, b in table:
        t = smooth((x - at + ease) / (2.0 * ease))
        held = w.pop(a, 0.0)
        if held:
            w[a] = held * (1.0 - t)
            w[b] = w.get(b, 0.0) + held * t
    return {k: v for k, v in w.items() if v > 1e-4}


def spine_weights(p):
    """The torso's, the neck's and the head's bones at a model-space point."""
    n = Vector(NECK_AXIS).normalized()
    d = (p - Vector(NECK_CUT)).dot(n)
    if d > -0.06:
        return _handover(d, SPINE_ALONG_NECK, 0.015)
    return _handover(p.z, SPINE_ALONG_Z, 0.02)


def _blend_weights(a, b, t):
    out = {k: v * (1.0 - t) for k, v in a.items()}
    for k, v in b.items():
        out[k] = out.get(k, 0.0) + v * t
    return out


def segment_weights(p, seg, pts):
    """The bones a point on the skin of one stick-figure segment follows."""
    A, B = pts[seg.a], pts[seg.b]
    ab = B - A
    t = max(0.0, min(1.0, (p - A).dot(ab) / max(ab.dot(ab), 1e-12)))
    if seg.kind == SPINE:
        return spine_weights(p)
    if seg.kind == BRANCH:
        return _blend_weights(spine_weights(p), seg.bones, smooth(t))
    length = ab.length
    if seg.ja is not None and t * length < JOINT_EASE:
        start = spine_weights(p) if seg.ja == SPINE else seg.ja
        return _blend_weights(start, seg.bones, smooth(t * length / JOINT_EASE))
    if seg.jb is not None and (1.0 - t) * length < JOINT_EASE:
        return _blend_weights(seg.jb, seg.bones, smooth((1.0 - t) * length / JOINT_EASE))
    return dict(seg.bones)


SURE = 0.3          # how much nearer one segment must be than the next for a vertex to be sure
DIFFUSION = 300     # rounds of surface averaging that fill the unsure vertices in
SMOOTHING = 2       # rounds of averaging every vertex with its neighbours, after the fill


def body_weights(body, graph, head_vertices):
    """Every body vertex's bones. The head's come from `spine_weights`. A ring-mesh vertex
    plainly on one segment's surface -- nearer it, relative to its radius, than to any other by
    SURE -- takes that segment's. The rest lie where a leg joins the body, where nearness cannot
    tell a chest from an elbow or a belly from a thigh: given the nearest, a chest followed the
    forearm into a spike. They are filled in by averaging over the surface from the sure ones
    round them, so each follows what it is joined to, not what happens to be near."""
    import numpy as np

    pts, rad, segs = graph
    me = body.data
    weights, sure = [], []
    for v in me.vertices:
        p = mv(v.co)
        if v.index < head_vertices:
            weights.append(spine_weights(p))
            sure.append(True)
            continue
        ranked = []
        for seg in segs:
            # A leg is only ever its own side's: under the belly a point near the middle is as
            # near one leg's root as the other's.
            if seg.leg and p.x * (1.0 if seg.leg[1] == "L" else -1.0) < 0.0:
                continue
            A, B = pts[seg.a], pts[seg.b]
            ab = B - A
            t = max(0.0, min(1.0, (p - A).dot(ab) / max(ab.dot(ab), 1e-12)))
            r = max(rad[seg.a]) * (1.0 - t) + max(rad[seg.b]) * t
            ranked.append(((p - (A + ab * t)).length / max(r, 1e-6), seg))
        ranked.sort(key=lambda x: x[0])
        nearest, best = ranked[0]
        weights.append(segment_weights(p, best, pts))
        # Two segments of one chain meet at a joint and are both near there; that is what the
        # joint easing is for, and not a doubt about which limb a vertex is on.
        rival = next((q for q, seg in ranked[1:] if seg.a not in (best.a, best.b)
                      and seg.b not in (best.a, best.b)), None)
        sure.append(best.kind != BRANCH and (rival is None or rival - nearest > SURE))
    names = sorted({k for w in weights for k in w})
    col = {k: i for i, k in enumerate(names)}
    W = np.zeros((len(weights), len(names)))
    for i, w in enumerate(weights):
        for k, x in w.items():
            W[i, col[k]] = x
    edges = np.array([e.vertices[:] for e in me.edges])
    a, b = edges[:, 0], edges[:, 1]
    degree = np.bincount(np.concatenate([a, b]), minlength=len(W)).astype(float)[:, None]
    # A vertex no edge reaches has no neighbours to be filled in from, and keeps its own.
    free = ~np.array(sure) & (degree[:, 0] > 0)

    def neighbour_mean(M):
        total = np.zeros_like(M)
        np.add.at(total, a, M[b])
        np.add.at(total, b, M[a])
        return total / np.maximum(degree, 1.0)

    for _ in range(DIFFUSION):
        W[free] = neighbour_mean(W)[free]
    for _ in range(SMOOTHING):
        W = np.where(degree > 0, 0.5 * W + 0.5 * neighbour_mean(W), W)
    W /= np.maximum(W.sum(axis=1, keepdims=True), 1e-9)
    print(f"weights: {int(free.sum())} of {len(W)} vertices filled in from their neighbours",
          flush=True)
    return [{names[j]: float(W[i, j]) for j in np.nonzero(W[i] > 1e-4)[0]} for i in range(len(W))]


def cut(bm, keep_head, offset=0.0):
    """Cut a mesh across the neck, keeping the head's side or the body's."""
    n = Vector(NECK_AXIS).normalized()
    co = Vector(NECK_CUT) - n * offset
    bmesh.ops.bisect_plane(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], dist=1e-7,
                           plane_co=bv(*co), plane_no=bv(*n),
                           clear_inner=keep_head, clear_outer=not keep_head)


def rebuild_body(head_src):
    """The cat's body: the ring mesh below the neck, the metaball head above it, bridged.
    Returns the body, the stick figure it was grown on, which says how it is weighted, and how
    many of its vertices, from the first, are the head's."""
    head = bmesh.new()
    head.from_mesh(head_src.data)
    cut(head, keep_head=True)
    head_vertices = len(head.verts)
    graph = cage_graph(neck_ring(head))
    cage = build_cage(graph)
    body = bmesh.new()
    body.from_mesh(cage.data)
    cut(body, keep_head=False, offset=NECK_GAP)
    # One mesh holding both, the head's vertices first. The head goes out to a mesh and back
    # rather than being appended to: the trip settles what the cut left, and the bridge and the
    # relaxing below come out differently on a bmesh that has not made it.
    me = bpy.data.meshes.new("cat_mesh")
    head.to_mesh(me)
    joined = bmesh.new()
    joined.from_mesh(me)
    tmp = bpy.data.meshes.new("cat_body_part")
    body.to_mesh(tmp)
    joined.from_mesh(tmp)
    loops = [e for e in joined.edges if e.is_boundary]
    bmesh.ops.bridge_loops(joined, edges=loops)
    bmesh.ops.recalc_face_normals(joined, faces=joined.faces[:])
    # The two cut edges are each other's shape only to a millimetre or two, and bridged as they
    # are the step between them reads as a collar: the skin either side is relaxed across it.
    n = Vector(NECK_AXIS).normalized()
    seam = [v for v in joined.verts if abs((mv(v.co) - Vector(NECK_CUT)).dot(n)) < NECK_SEAM]
    for _ in range(10):
        bmesh.ops.smooth_vert(joined, verts=seam, factor=0.5, use_axis_x=True, use_axis_y=True,
                              use_axis_z=True)
    left_open = sum(1 for e in joined.edges if e.is_boundary)
    joined.to_mesh(me)
    for b in (head, body, joined):
        b.free()
    bpy.data.meshes.remove(tmp)
    bpy.data.objects.remove(cage)
    head_src.data, old = me, head_src.data
    bpy.data.meshes.remove(old)
    if left_open:
        sys.exit(f"rebuild_body: {left_open} edges left open after bridging the neck")
    print(f"body: {len(loops)} edges bridged at the neck, {len(me.polygons)} faces", flush=True)
    return head_src, graph, head_vertices


def in_head(p):
    """The head's side of the neck cut, and a little past it: everything the metaball mesh
    still gives the cat once the ring mesh has replaced the body."""
    return (p - Vector(NECK_CUT)).dot(Vector(NECK_AXIS).normalized()) > -0.012


def in_face(p):
    """The muzzle, nose and mouth: the part of the head whose features are millimetres."""
    return p.z > 0.222 and p.y < 0.285 and abs(p.x) < 0.03


# The head's share of the budget, and the face's share of that. Left to itself the collapse
# gives the head about a ninth, too few to carry a face, which is read at a few pixels and
# carries the expression where a flank is only a silhouette; and the muzzle, under even shares,
# gets triangles 5 mm across for a nose 12 mm wide.
HEAD_TRIANGLES = 1500
FACE_TRIANGLES = 1100


def triangle_count(obj, where=None):
    return sum(len(p.vertices) - 2 for p in obj.data.polygons
               if where is None or where(mv(p.center)))


def place_eyes(body):
    """The left eye's centre, its depth found from the face itself: a ray along the eye's line
    of sight finds the surface at its height and breadth, and the eye is set so its front stands
    EYE_PROUD of it."""
    x, y = EYE_GUESS[0], EYE_GUESS[1]
    surface = Face(body).need(x, y)[0].z
    eye_at = (x, y, surface + EYE_PROUD - EYE_RADIUS * EYE_SHAPE[2])
    print(f"eyes: the face is at z {surface:.4f} there, the eye centre at {eye_at[2]:.4f}",
          flush=True)
    return eye_at


class Face:
    """The body seen from in front: where a ray along -z through a front-view point first meets
    it, and the surface's normal there, in model space."""

    def __init__(self, body):
        bm = bmesh.new()
        bm.from_mesh(body.data)
        self.tree = BVHTree.FromBMesh(bm)
        bm.free()

    def at(self, x, y):
        loc, normal, *_ = self.tree.ray_cast(bv(x, y, 0.6), bv(0.0, 0.0, -1.0), 1.0)
        return (None, None) if loc is None else (mv(loc), mv(normal).normalized())

    def need(self, x, y):
        p, n = self.at(x, y)
        if p is None:
            sys.exit(f"no face in front of ({x:.4f}, {y:.4f}); the head has moved")
        return p, n


def pad_edge(face, x, step=0.0001, drop=0.0015):
    """The lowest point of the whisker pad a ray from in front still meets at x: the first
    height under the nose past which the ray falls back to the chin. The first and not the
    largest drop, since the chin's own underside is a bigger one. None where the pad does not
    overhang anything."""
    prev, y = None, NOSE_POINT
    while y > 0.243:
        p, _ = face.at(x, y)
        if p is not None and prev is not None and prev.z - p.z > drop:
            return prev
        prev = p
        y -= step
    return None


def measure_lip(face):
    """The upper lip's edge on the face the metaballs made, out from the middle."""
    lip = []
    for i in range(12):
        x = MOUTH_CORNER * i / 11
        edge = pad_edge(face, x)
        lip.append([x, edge.y if edge else LIP_GUESS.y(x)])
    ys = [y for _, y in lip]
    for i in range(len(lip)):
        near = ys[max(0, i - 1):i + 2]
        lip[i][1] = sum(near) / len(near)
    print("lip: " + " ".join(f"{x:.4f}:{y:.4f}" for x, y in lip), flush=True)
    return Lip([tuple(p) for p in lip])


def decimate(obj, triangles):
    """The body collapsed to its share with the head locked, then the head to its share with
    the rest locked, then the face. Blender's weighted collapse locks a group outright at any
    strength rather than weighing it, so the split is a pass a region and not one."""
    body_share = triangles - HEAD_TRIANGLES - FACE_TRIANGLES
    collapse(obj, in_head, body_share + triangle_count(obj, in_head))
    collapse(obj, lambda p: not in_head(p) or in_face(p),
             triangle_count(obj, lambda p: not in_head(p)) + HEAD_TRIANGLES
             + triangle_count(obj, in_face))
    collapse(obj, lambda p: not in_face(p), triangles)
    print(f"decimate: {triangle_count(obj)} triangles, {triangle_count(obj, in_head)} of them "
          f"on the head and {triangle_count(obj, in_face)} on the face", flush=True)


def collapse(obj, locked, target):
    """Collapse to `target` triangles, the vertices `locked` says leaving untouched."""
    faces = triangle_count(obj)
    keep = obj.vertex_groups.new(name="locked")
    keep.add([v.index for v in obj.data.vertices if locked(mv(v.co))], 1.0, "REPLACE")
    mod = obj.modifiers.new("decimate", "DECIMATE")
    mod.decimate_type = "COLLAPSE"
    mod.ratio = min(1.0, target / max(faces, 1))
    mod.use_symmetry = True
    mod.symmetry_axis = "X"
    mod.use_collapse_triangulate = True
    # The group says what MAY collapse, so the locked group is inverted.
    mod.vertex_group = "locked"
    mod.invert_vertex_group = True
    mod.vertex_group_factor = 1.0
    apply_modifiers(obj)
    obj.vertex_groups.remove(obj.vertex_groups["locked"])


def apply_modifiers(obj):
    dg = bpy.context.evaluated_depsgraph_get()
    mesh = bpy.data.meshes.new_from_object(obj.evaluated_get(dg))
    old = obj.data
    obj.modifiers.clear()
    obj.data = mesh
    bpy.data.meshes.remove(old)


# ---------------------------------------------------------------------------------------------
# The separate parts: each a small mesh weighted wholly to one bone


def part_object(name, verts, faces, mats_per_face, bone, mats, outward_from=None, fur=0.0,
                keep_winding=False):
    """A mesh from model-space vertices; faces index the vertices, each with a material name.
    Its faces are turned to face out -- away from `outward_from` for an open cap, by the mesh's
    own closure otherwise, or left as wound when `keep_winding` -- since every material is
    single-sided. `fur` is the coat's share at each vertex, one number or one per vertex, carried
    in the vertex colour's alpha with the colour itself left white."""
    me = bpy.data.meshes.new(name)
    me.from_pydata([bv(*v) for v in verts], [], faces)
    if not keep_winding:
        bm = bmesh.new()
        bm.from_mesh(me)
        if outward_from is None:
            bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        else:
            c = bv(*outward_from)
            for f in bm.faces:
                if f.normal.dot(f.calc_center_median() - c) < 0.0:
                    f.normal_flip()
        bm.to_mesh(me)
        bm.free()
    shares = fur if isinstance(fur, (list, tuple)) else [fur] * len(verts)
    col = me.color_attributes.new("Col", "BYTE_COLOR", "POINT")
    for i, a in enumerate(shares):
        col.data[i].color = (1.0, 1.0, 1.0, a)
    names = sorted(set(mats_per_face))
    for n in names:
        me.materials.append(mats[n])
    for poly, n in zip(me.polygons, mats_per_face):
        poly.material_index = names.index(n)
    obj = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(obj)
    vg = obj.vertex_groups.new(name=bone)
    vg.add(list(range(len(verts))), 1.0, "REPLACE")
    return obj


def sphere_points(rings, segments, radius, scale=(1.0, 1.0, 1.0), cap=math.pi):
    """A UV sphere about +z (forward), cut at the polar angle `cap`: vertices then quads/tris."""
    verts, faces = [(0.0, 0.0, radius * scale[2])], []
    for r in range(1, rings + 1):
        th = cap * r / rings
        for s in range(segments):
            ph = 2.0 * math.pi * s / segments
            verts.append((radius * scale[0] * math.sin(th) * math.cos(ph),
                          radius * scale[1] * math.sin(th) * math.sin(ph),
                          radius * scale[2] * math.cos(th)))
    for s in range(segments):
        faces.append((0, 1 + s, 1 + (s + 1) % segments))
    for r in range(rings - 1):
        a, b = 1 + r * segments, 1 + (r + 1) * segments
        for s in range(segments):
            s1 = (s + 1) % segments
            faces.append((a + s, b + s, b + s1, a + s1))
    return verts, faces


def placed(verts, origin, rotation):
    """Model-space vertices moved to `origin` after turning them by a model-space rotation
    (a Quaternion built by `rot`, which acts in Blender's space)."""
    out = []
    for v in verts:
        b = rotation @ Vector((v[0], -v[2], v[1]))  # model -> Blender without the origin
        m = mv(b)
        out.append((m.x + origin[0], m.y + origin[1], m.z + origin[2]))
    return out


NOSE_LIFT = 0.0002     # the nose leather's edge above the face it is laid on
LINE_LIFT = 0.00015    # the lip line's and the philtrum's


def nose_height(x, y):
    """The nose leather's surface above the face under it at a front-view point."""
    return NOSE_LIFT + NOSE_DOME * max(0.0, 1.0 - nose_reach(x, y) ** 2)


def nose(face, mats, rings=6):
    """The nose leather as its own small mesh, a shallow dome laid on the face: the body's
    triangles there are a millimetre across and an outline drawn by them is a staircase."""
    c = NOSE_CENTRE
    verts, faces = [], []
    for r in range(rings + 1):
        for q in (NOSE if r else [c]):
            v = c + (q - c) * (r / rings)
            p, n = face.need(v.x, v.y)
            verts.append(tuple(p + n * nose_height(v.x, v.y)))
    k = len(NOSE)
    faces += [(0, 1 + s, 1 + (s + 1) % k) for s in range(k)]
    for r in range(rings - 1):
        a, b = 1 + r * k, 1 + (r + 1) * k
        faces += [(a + s, b + s, b + (s + 1) % k, a + (s + 1) % k) for s in range(k)]
    return part_object("nose", verts, faces, ["cat_nose"] * len(faces), "Head", mats,
                       outward_from=(c.x, c.y, 0.2))


def ribbon(name, path, widths, lift, face, mats):
    """A dark strip drawn in front view and laid on the face like a decal projected from in
    front: both edges of the strip taken to the surface along the view ray, then raised
    `lift(x, y)` along its normal, so from ahead it is the strip drawn. Widening it on the
    surface instead turns it wherever the facets do, and on a lip that is a zigzag. The lip
    line, the philtrum and the nostrils."""
    verts, faces = [], []
    pts = [Vector(p) for p in path]
    for k, (q, w) in enumerate(zip(pts, widths)):
        t = pts[min(k + 1, len(pts) - 1)] - pts[max(k - 1, 0)]
        across = Vector((-t.y, t.x)).normalized() * (w / 2.0)
        for e in (q - across, q + across):
            p, n = face.need(e.x, e.y)
            verts.append(tuple(p + n * lift(e.x, e.y)))
    for k in range(len(pts) - 1):
        f = (2 * k, 2 * k + 1, 2 * k + 3, 2 * k + 2)
        a, b, c = (Vector(verts[i]) for i in f[:3])
        faces.append(f if (b - a).cross(c - a).z >= 0.0 else tuple(reversed(f)))
    return part_object(name, verts, faces, ["cat_mouth"] * len(faces), "Head", mats,
                       keep_winding=True)


def mouth_lines(face, mats, lip, n=17):
    """The inverted Y under the nose: the philtrum down from the nose's point, and the upper
    lip's two arcs out under the whisker pads, along the edge `measure_lip` found."""
    out = []
    line = lambda x, y: LINE_LIFT
    # The strip's lower edge stays a hair above the lip's: below it the view ray passes under
    # the pad and lays that corner on the chin.
    width = 0.0008
    above = width / 2.0 + 0.0005
    top, bottom = NOSE_POINT + 0.0004, lip.y(0.0) + above
    path = [(0.0, top + (bottom - top) * k / 4) for k in range(5)]
    out.append(ribbon("philtrum", path, [0.0006] * 5, line, face, mats))
    xs = [MOUTH_CORNER * (2.0 * k / (n - 1) - 1.0) for k in range(n)]
    path = [(x, lip.y(x) + above) for x in xs]
    widths = [width * (1.0 - 0.6 * (abs(x) / MOUTH_CORNER) ** 2) for x in xs]
    out.append(ribbon("lip", path, widths, line, face, mats))
    # The nostrils: commas along the leather's sides, open at the top and running down and in.
    # In the nose's own terms: across as a share of its half width, up as a share of its height.
    for side, s in (("L", 1.0), ("R", -1.0)):
        nostril = [(s * u * NOSE_WIDTH / 2, NOSE_POINT + v * (NOSE_TOP - NOSE_POINT))
                   for u, v in ((0.62, 0.72), (0.5, 0.55), (0.36, 0.42), (0.22, 0.33))]
        out.append(ribbon(f"nostril_{side}", nostril, [0.0011, 0.0009, 0.0006, 0.0003],
                          lambda x, y: nose_height(x, y) + 0.0001, face, mats))
    return out


def build_parts(mats, face, eye_at, lip):
    parts = [nose(face, mats)] + mouth_lines(face, mats, lip)
    for side, s in (("L", 1.0), ("R", -1.0)):
        eye = (eye_at[0] * s, eye_at[1], eye_at[2])
        # Almond eyes: wide and shallow, looking forward and a little out, the outer corner
        # raised -- a cat's slant, where a sphere set on the face reads as an owl.
        turn = rot(yaw=14.0 * s, pitch=-2.0, roll=12.0 * s)
        v, f = sphere_points(4, 10, EYE_RADIUS, EYE_SHAPE, cap=math.pi * 0.5)
        parts.append(part_object(f"eye_{side}", placed(v, eye, turn), f, ["cat_eye"] * len(f),
                                 f"Eye.{side}", mats, outward_from=eye))
        # A slit pupil, just proud of the eye's front.
        pv, pf = [], []
        n = 10
        front = EYE_RADIUS * EYE_SHAPE[2] + 0.0005
        pv.append((0.0, 0.0, front))
        for k in range(n):
            a = 2.0 * math.pi * k / n
            pv.append((0.0016 * math.cos(a), 0.0058 * math.sin(a), front - 0.0008))
        for k in range(n):
            pf.append((0, 1 + k, 1 + (k + 1) % n))
        parts.append(part_object(f"pupil_{side}", placed(pv, eye, turn), pf,
                                 ["cat_pupil"] * len(pf), f"Eye.{side}", mats, outward_from=eye))
        # The lid: a cap a little larger than the eye, bound OPEN, turned up inside the skull.
        # Wider than the eye's visible cap, so shut it covers the whole eye; open, its edge
        # shows over the top of the eye as the upper lid.
        lv, lf = sphere_points(3, 10, EYE_RADIUS * 1.08,
                               (EYE_SHAPE[0] * 1.04, EYE_SHAPE[1] * 1.06, EYE_SHAPE[2] * 1.05),
                               cap=math.pi * 0.58)
        open_turn = turn @ rot(pitch=LID_CLOSE)
        parts.append(part_object(f"lid_{side}", placed(lv, eye, open_turn), lf,
                                 ["cat_fur"] * len(lf), f"Lid.{side}", mats, outward_from=eye))
        parts.append(ear(side, s, mats))
        for k, (y, fwd, down, length) in enumerate(WHISKERS):
            # Rooted just under the pad's own surface, wherever the metaballs put it.
            root, _ = face.need(WHISKER_X * s, y)
            parts.append(whisker(f"whisker_{side}{k}", (root.x, root.y, root.z - 0.0004),
                                 (s, -down, fwd), length, mats))
    return parts


# A cat's ear is a funnel of skin and cartilage: a curved shell opening forward and out, broad at
# the base and narrowing to a rounded tip that leans a little forward, with real thickness, a
# coat on its back and a fringe of fur in its hollow.
EAR_HEIGHT = 0.044     # from the base, which sits under the skin of the skull
EAR_RADIUS = 0.0158    # half the breadth of the base
EAR_DEPTH = 0.8        # front to back against across: the funnel is flattened
EAR_OPEN = 125.0       # degrees round from the back to each edge of the opening
EAR_SHELL = 0.0018     # the ear's thickness at its base, half that at the tip
# Where the ear stands: its base's middle, set in from the side of the skull and sunk into it,
# so the whole base ring is under the head's skin -- at the old 2.9 cm out its outer half hung
# in the air. Mounds raised under the ears to meet them read as a pair of headphones.
EAR_BASE = (0.023, 0.288, 0.197)


def ear(side, s, mats, rings=9, segments=10):
    base = Vector((EAR_BASE[0] * s, EAR_BASE[1], EAR_BASE[2]))
    up = Vector((0.24 * s, 1.0, -0.08)).normalized()           # up, leaning out
    ahead = Vector((0.45 * s, 0.0, 1.0)).normalized()           # the opening faces forward, out
    fwd = (ahead - up * ahead.dot(up)).normalized()
    lat = up.cross(fwd)
    outer, inner = [], []
    for i in range(rings + 1):
        v = i / rings
        r = EAR_RADIUS * (1.0 - v) ** 0.8 + 0.0012
        t = EAR_SHELL * (1.0 - 0.5 * v)
        centre = base + up * (EAR_HEIGHT * v) + fwd * (0.006 * v * v)
        for j in range(segments + 1):
            th = math.radians(-EAR_OPEN + 2.0 * EAR_OPEN * j / segments)
            radial = -fwd * (math.cos(th) * EAR_DEPTH) + lat * math.sin(th)
            outer.append(centre + radial * r)
            inner.append(centre + radial * max(r - t, 0.0004))
    tip = base + up * (EAR_HEIGHT + 0.0015) + fwd * 0.006
    verts = outer + inner + [tip]
    n_ring = segments + 1
    off_in, tip_i = len(outer), len(outer) + len(inner)

    def at(i, j, inside=False):
        return (off_in if inside else 0) + i * n_ring + j

    faces, mats_of, sense = [], [], []
    for i in range(rings):
        for j in range(segments):
            faces.append((at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)))
            mats_of.append("cat_fur")
            sense.append(1.0)   # the back faces away from the ear's axis
            faces.append((at(i, j, True), at(i + 1, j, True), at(i + 1, j + 1, True),
                          at(i, j + 1, True)))
            mats_of.append("cat_ear")
            sense.append(-1.0)  # the hollow faces in, toward it
        for j in (0, segments):  # the two edges of the opening, outer skin to inner
            faces.append((at(i, j), at(i + 1, j), at(i + 1, j, True), at(i, j, True)))
            mats_of.append("cat_fur")
            sense.append(1.0)
    for j in range(segments):  # the rim round the tip, and the tip's cap
        faces.append((at(rings, j), at(rings, j + 1), at(rings, j + 1, True), at(rings, j, True)))
        mats_of.append("cat_fur")
        sense.append(1.0)
        faces.append((at(rings, j), tip_i, at(rings, j + 1)))
        mats_of.append("cat_fur")
        sense.append(1.0)

    # Wound by hand rather than by closure, since the funnel is open: each face against the
    # direction from the ear's axis to it.
    wound = []
    for f, want in zip(faces, sense):
        a, b, c = verts[f[0]], verts[f[1]], verts[f[2]]
        normal = (b - a).cross(c - a)
        centre = sum((verts[k] for k in f), Vector()) / len(f)
        away = centre - (base + up * (centre - base).dot(up))
        wound.append(f if normal.dot(away) * want >= 0.0 else tuple(reversed(f)))
    # A coat on the back as long as the head's at the base, so the seam is under fur, and
    # shortening to the tip; a longer fringe in the hollow.
    fur = ([0.45 - 0.2 * (i / rings) for i in range(rings + 1) for _ in range(segments + 1)]
           + [0.8] * len(inner) + [0.25])
    return part_object(f"ear_{side}", [tuple(p) for p in verts], wound, mats_of, f"Ear.{side}",
                       mats, fur=fur, keep_winding=True)


# Six a side, in three rows on the whisker pad: (root y, forward lean, downward lean, length).
# They fan from forward to back of straight out, and droop under their own weight.
WHISKERS = [
    (0.2585, 0.35, -0.08, 0.066), (0.2585, 0.05, -0.04, 0.07),
    (0.2555, 0.25, 0.05, 0.064), (0.2555, -0.12, 0.08, 0.068),
    (0.2525, 0.15, 0.17, 0.058), (0.2525, -0.25, 0.2, 0.06),
]
WHISKER_X = 0.0115
# A whisker's radius at its root and its tip, a little over life: at a few metres and half
# resolution it is under a pixel, which is how a real one reads too.
WHISKER_ROOT, WHISKER_TIP = 0.00045, 0.00012


def whisker(name, root, direction, length, mats, segments=5, droop=0.012):
    """A whisker as a thin three-sided tube along a drooping curve, weighted to the head."""
    d0 = Vector(direction).normalized()
    pts = [Vector(root) + d0 * length * (k / segments)
           + Vector((0.0, -droop * (k / segments) ** 2, 0.0)) for k in range(segments + 1)]
    verts, faces = [], []
    for k, p in enumerate(pts):
        t = k / segments
        r = WHISKER_ROOT + (WHISKER_TIP - WHISKER_ROOT) * t
        d = (pts[min(k + 1, segments)] - pts[max(k - 1, 0)]).normalized()
        a = d.cross(Vector((0.0, 1.0, 0.0)))
        a = a.normalized() if a.length > 1e-6 else Vector((1.0, 0.0, 0.0))
        b = d.cross(a)
        for j in range(3):
            ang = 2.0 * math.pi * j / 3.0
            q = p + (a * math.cos(ang) + b * math.sin(ang)) * r
            verts.append((q.x, q.y, q.z))
    for k in range(segments):
        for j in range(3):
            j1 = (j + 1) % 3
            faces.append((k * 3 + j, (k + 1) * 3 + j, (k + 1) * 3 + j1, k * 3 + j1))
    return part_object(name, verts, faces, ["cat_whisker"] * len(faces), "Head", mats)


def assign_body_materials(body, mats, lip):
    """Fur everywhere, the leather's colour under the nose part, pads under the paws, and the
    mouth along the line where the jaw parts from the head."""
    me = body.data
    order = ["cat_fur", "cat_nose", "cat_mouth"]
    for n in order:
        me.materials.append(mats[n])
    for p in me.polygons:
        c = mv(p.center)
        jaw = [lip.jaw(mv(me.vertices[i].co)) for i in p.vertices]
        idx = 0
        if c.z > 0.235 and nose_reach(c.x, c.y) < 1.0:
            idx = 1
        elif c.y < 0.004:
            idx = 1
        elif any(jaw) and not all(jaw) and c.z > 0.232 and mv(p.normal).y < -0.2:
            # Only along the front, where the jaw's back edge would draw a collar round the
            # throat, and only facing down under the pads: a face across the seam that looks
            # forward shows with the mouth shut, as a dark block a millimetre or two across.
            idx = 2
        p.material_index = idx


# ---------------------------------------------------------------------------------------------
# Skinning


def skin(body, parts, arm, graph, head_vertices, lip):
    """The body weighted and the parts joined onto it, bound to the armature. Returns how many
    of its vertices, from the first, are the body's own: the parts' follow them."""
    for i, w in enumerate(body_weights(body, graph, head_vertices)):
        for name, x in w.items():
            group = body.vertex_groups.get(name) or body.vertex_groups.new(name=name)
            group.add([i], x, "REPLACE")
    # The chin below the mouth line goes wholly to the jaw.
    jaw = body.vertex_groups.new(name="Jaw")
    for v in body.data.vertices:
        if lip.jaw(mv(v.co)):
            for g in list(v.groups):
                body.vertex_groups[g.group].remove([v.index])
            jaw.add([v.index], 1.0, "REPLACE")
    # The parts join the body: their one vertex group each comes along by name. The body needs
    # the colour attribute the parts carry before the join, or theirs is dropped; its own colours
    # are baked over it afterwards.
    own = len(body.data.vertices)
    body.data.color_attributes.new("Col", "BYTE_COLOR", "POINT")
    bpy.ops.object.select_all(action="DESELECT")
    for p in parts:
        p.select_set(True)
    body.select_set(True)
    bpy.context.view_layer.objects.active = body
    bpy.ops.object.join()
    limit_weights(body, 4)
    mod = body.modifiers.new("armature", "ARMATURE")
    mod.object = arm
    body.parent = arm
    return own


def limit_weights(obj, limit):
    """At most `limit` influences a vertex, normalised: what the engine reads."""
    for v in obj.data.vertices:
        gs = sorted(((g.weight, g.group) for g in v.groups if g.weight > 0.0), reverse=True)
        keep, drop = gs[:limit], gs[limit:]
        total = sum(w for w, _ in keep) or 1.0
        for _, gi in drop:
            obj.vertex_groups[gi].remove([v.index])
        for w, gi in keep:
            obj.vertex_groups[gi].add([v.index], w / total, "REPLACE")
    empty = [v.index for v in obj.data.vertices if not v.groups]
    if empty:
        where = ", ".join(f"({mv(obj.data.vertices[i].co).x:.3f}, {mv(obj.data.vertices[i].co).y:.3f}, "
                          f"{mv(obj.data.vertices[i].co).z:.3f})" for i in empty[:4])
        sys.exit(f"{len(empty)} vertices carry no weight, at {where}")


# ---------------------------------------------------------------------------------------------
# The vertex colours: ambient occlusion in RGB, so a fur of any colour still shows its form, and
# the coat's length in alpha, which the engine's fur shells read and nothing else does.


def fur_share(p, eye_at, lip):
    """How much of the coat's full length grows at a model-space point, 0..1: bare round the
    eyes and on the nose, short over the face and down the legs, full on the body and tail."""
    s = 1.0
    if p.y > 0.235 and p.z > 0.17:
        s = 0.45                                     # the head
        s *= 1.0 - 0.6 * smooth((p.z - 0.236) / 0.012)  # shortening over the muzzle
        # Bare round the eyes, and wider than the eye: the coat leans down and back, so the
        # brow's would otherwise hang over the eye, which is the same thing as having none.
        for x in (eye_at[0], -eye_at[0]):
            d = (p - Vector((x, eye_at[1], eye_at[2]))).length
            s *= smooth((d - 0.012) / 0.01)
        if p.z > 0.235:
            s *= smooth((nose_reach(p.x, p.y) - 1.0) / 0.3)   # none on the leather
            # Short along the lip line and the philtrum, or the coat hides the mouth.
            along = abs(p.y - lip.y(p.x)) if abs(p.x) < MOUTH_CORNER else 1.0
            groove = abs(p.x) if lip.y(0.0) - 0.001 < p.y < NOSE_POINT else 1.0
            s *= 0.25 + 0.75 * smooth((min(along, groove) - 0.0008) / 0.003)
    s *= 0.75 + 0.25 * smooth(p.y / 0.05)            # a little shorter at the paws
    return max(0.0, min(1.0, s))


def crease(p, lip):
    """The soft shadow either side of the lip line and down the philtrum, which the strips
    drawn on them are too narrow to give: 1 away from both, darker toward them."""
    if p.z < 0.232:
        return 1.0
    along = abs(p.y - lip.y(p.x)) if abs(p.x) < MOUTH_CORNER else 1.0
    groove = abs(p.x) if lip.y(0.0) < p.y < NOSE_POINT else 1.0
    return 1.0 - 0.4 * math.exp(-(min(along, groove) / 0.0014) ** 2)


def bake_vertex_colours(obj, own, eye_at, lip, rays=48, reach=0.12):
    """Ambient occlusion and the coat's length into the first `own` vertices, the body's; the
    parts, joined after them, keep the colours their builders gave them."""
    me = obj.data
    bm = bmesh.new()
    bm.from_mesh(me)
    tree = BVHTree.FromBMesh(bm)
    bm.free()
    # A fixed hemisphere of directions, the same for every vertex.
    dirs = []
    golden = math.pi * (3.0 - math.sqrt(5.0))
    for i in range(rays):
        z = 1.0 - (i + 0.5) / rays
        r = math.sqrt(max(0.0, 1.0 - z * z))
        dirs.append(Vector((r * math.cos(golden * i), r * math.sin(golden * i), z)))
    attr = me.color_attributes["Col"]
    for v in me.vertices:
        if v.index >= own:
            continue
        n = v.normal
        frame = n.to_track_quat("Z", "Y")
        hit = 0
        for d in dirs:
            w = frame @ d
            loc, *_ = tree.ray_cast(v.co + n * 0.0015, w, reach)
            if loc is not None:
                hit += 1
        c = (0.72 + 0.28 * (1.0 - hit / rays)) * crease(mv(v.co), lip)
        attr.data[v.index].color = (c, c, c, fur_share(mv(v.co), eye_at, lip))
    me.color_attributes.active_color = attr


# ---------------------------------------------------------------------------------------------
# Posing


class Rig:
    """The armature's rest frames, and the pose they produce from a Pose's parameters."""

    def __init__(self, arm, bones):
        self.arm = arm
        self.rest = {b.name: b.matrix_local.copy() for b in arm.data.bones}
        self.parent = {b.name: (b.parent.name if b.parent else None) for b in arm.data.bones}
        self.length = {b.name: b.length for b in arm.data.bones}
        self.order = [b[0] for b in bones]
        self.rest_rel = {}
        for n in self.order:
            p = self.parent[n]
            self.rest_rel[n] = (self.rest[p].inverted() @ self.rest[n]) if p else self.rest[n]
        # Each leg as it stands at rest, which every pose starts from.
        self.rest_legs = {}
        for name, leg in LEGS.items():
            P = self.rest_point(leg.meta, "tail")
            C = self.rest_point(leg.meta, "head")
            m = C - P
            tip = self.rest_point(leg.toe, "tail") - P
            body = self.rest_point(leg.body, "head")
            self.rest_legs[name] = {
                "paw": (P.x, P.y, P.z),
                "paw_rel": tuple(P - body),  # from the girdle, in its turned frame
                "rel": 0.0,
                "meta": math.degrees(math.atan2(-m.z, m.y)),
                "toe": math.degrees(math.atan2(tip.y, tip.z)),
                "pole": (0.0, 0.1, -1.0) if leg.fore else (0.0, 0.3, 1.0),
            }

    def rest_point(self, bone, end):
        b = self.arm.data.bones[bone]
        return mv(b.tail_local if end == "tail" else b.head_local)

    def solve(self, pose):
        """Armature-space matrices for every bone."""
        M, D = {}, {}
        for n in self.order:
            p = self.parent[n]
            if n in pose.ik_bones:
                continue  # placed by the leg that owns it
            q, t = pose.local(n)
            R = self.rest[n].to_3x3()
            basis = Matrix.Translation(R.inverted() @ t) @ (R.inverted() @ q.to_matrix() @ R).to_4x4()
            M[n] = (M[p] if p else Matrix.Identity(4)) @ self.rest_rel[n] @ basis
            D[n] = (D[p] if p else Quaternion()) @ q
            if n in GIRDLES:
                self._legs_from(n, pose, M, D)
        return M

    def _legs_from(self, girdle, pose, M, D):
        for name, leg in LEGS.items():
            if leg.girdle != girdle:
                continue
            L = pose.legs[name]
            A = (M[girdle] @ self.rest_rel[leg.upper]).translation
            # A paw stands at a model-space point, or rides its girdle -- the chest for a fore
            # paw, the pelvis for a hind -- when the body bends round it; `rel` blends the two.
            rel = L["rel"]
            turned = D[leg.body]
            model_fwd = Quaternion(AY, math.radians(pose.root_yaw)) @ AZ
            fwd = model_fwd.lerp(turned @ AZ, rel).normalized()
            paw = bv(*L["paw"]).lerp(M[leg.body].translation + turned @ bv(*L["paw_rel"]), rel)
            up = AY.lerp(turned @ AY, rel).normalized()
            a = math.radians(L["meta"])
            m_dir = (up * math.cos(a) - fwd * math.sin(a)).normalized()
            upper, lower, meta, toe = leg.bones
            wrist = paw + m_dir * self.length[meta]
            pole = bv(*L["pole"]).lerp(turned @ bv(*L["pole"]), rel).normalized()
            B, C = two_bone(A, wrist, self.length[upper], self.length[lower], pole)
            P = C - m_dir * self.length[meta]
            ta = math.radians(L["toe"])
            t_dir = (fwd * math.cos(ta) + up * math.sin(ta)).normalized()
            for bone, head, d in ((upper, A, B - A), (lower, B, C - B), (meta, C, P - C), (toe, P, t_dir)):
                M[bone] = frame_along(head, d)


def two_bone(A, T, l1, l2, pole):
    v = T - A
    d = max(min(v.length, l1 + l2 - 1e-5), abs(l1 - l2) + 1e-5)
    u = v.normalized()
    w = pole - u * pole.dot(u)
    w = w.normalized() if w.length > 1e-6 else Vector((0.0, 0.0, 1.0))
    a = (l1 * l1 - l2 * l2 + d * d) / (2.0 * d)
    h = math.sqrt(max(l1 * l1 - a * a, 0.0))
    B = A + u * a + w * h
    return B, A + u * d


def frame_along(head, d):
    """A bone frame at `head` with its Y along `d` and its X as near the model's X as it can be."""
    y = d.normalized()
    x = AX - y * AX.dot(y)
    x = x.normalized() if x.length > 1e-6 else Vector((0.0, 0.0, 1.0)).cross(y).normalized()
    z = x.cross(y)
    m = Matrix((x, y, z)).transposed().to_4x4()
    m.translation = head
    return m


class Pose:
    """One frame's parameters. Rotations are degrees about the model axes (pitch, yaw, roll);
    offsets are metres in the parent's turned frame; paws are model-space points."""

    ik_bones = {b for leg in LEGS.values() for b in leg.bones}

    def __init__(self, rig):
        self.root = Vector((0.0, 0.0, 0.0))
        self.root_yaw = 0.0
        self.rump = Vector((0.0, 0.0, 0.0))
        self.rot = {}
        self.legs = {name: dict(rest) for name, rest in rig.rest_legs.items()}

    def local(self, bone):
        """The bone's rotation about the model axes and its offset in its parent's frame."""
        q = rot(*self.rot.get(bone, (0.0, 0.0, 0.0)))
        t = Vector((0.0, 0.0, 0.0))
        if bone == "Hips":
            q = Quaternion(AY, math.radians(self.root_yaw)) @ q
            t = bv(*self.root)
        elif bone == "Rump":
            t = bv(*self.rump)
        return q, t


def lerp_pose(rig, a, b, t):
    """Every parameter of two poses mixed by t: what a transition is between its ends."""
    p = Pose(rig)
    p.root = a.root.lerp(b.root, t)
    p.root_yaw = _mix(a.root_yaw, b.root_yaw, t)
    p.rump = a.rump.lerp(b.rump, t)
    for n in set(a.rot) | set(b.rot):
        p.rot[n] = _mix(a.rot.get(n, (0.0, 0.0, 0.0)), b.rot.get(n, (0.0, 0.0, 0.0)), t)
    for leg in LEGS:
        la, lb = a.legs[leg], b.legs[leg]
        p.legs[leg] = {k: _mix(la[k], lb[k], t) for k in la}
    return p


def _mix(x, y, t):
    if isinstance(x, tuple):
        return tuple(u + (v - u) * t for u, v in zip(x, y))
    return x + (y - x) * t


def window(t, a, b):
    """0 before a, 1 after b, smooth between."""
    return smooth((t - a) / (b - a)) if b > a else float(t >= a)


def bump(t, at, width):
    """A smooth pulse of height 1 centred on `at`."""
    x = (t - at) / width
    return math.exp(-x * x * 4.0)


def add_rot(p, bone, pitch=0.0, yaw=0.0, roll=0.0):
    r = p.rot.get(bone, (0.0, 0.0, 0.0))
    p.rot[bone] = (r[0] + pitch, r[1] + yaw, r[2] + roll)


def tail(p, pitches, k=1.0, yaws=(0.0,) * 6):
    """Each tail bone turned by its pitch, root first, times k; and by its yaw."""
    for i, (pitch, yaw) in enumerate(zip(pitches, yaws)):
        add_rot(p, f"Tail{i + 1}", pitch=pitch * k, yaw=yaw)


def ears(p, pitch=0.0, yaw=0.0):
    """Both ears turned alike: the same pitch, and a yaw outward on each side for a positive one
    -- the left ear's to the left."""
    add_rot(p, "Ear.L", pitch=pitch, yaw=yaw)
    add_rot(p, "Ear.R", pitch=pitch, yaw=-yaw)


def shoulders(p, pitch):
    for s in ("L", "R"):
        p.rot[f"Shoulder.{s}"] = (pitch, 0.0, 0.0)


def paws(p, end, x, y, z, at="paw", **kw):
    """The pair at one end, "F" or "H", set x either side of the middle at height y and depth
    z -- a model-space point, or with at="paw_rel" one in the girdle's turned frame -- and the
    rest of the leg's parameters as given."""
    for side, s in (("L", 1.0), ("R", -1.0)):
        p.legs[end + side].update({at: (s * x, y, z)}, **kw)


# ---------------------------------------------------------------------------------------------
# Key poses


def stand(rig):
    return Pose(rig)


def sit(rig):
    p = Pose(rig)
    # The pelvis drops almost to the floor and the body pitches up over straight front legs.
    p.rump = Vector((0.0, -0.145, 0.0))
    p.rot["Rump"] = (40.0, 0.0, 0.0)
    p.rot["Spine1"] = (0.0, 0.0, 0.0)
    p.rot["Spine2"] = (2.0, 0.0, 0.0)
    p.rot["Chest"] = (2.0, 0.0, 0.0)
    p.rot["Neck1"] = (-12.0, 0.0, 0.0)
    p.rot["Neck2"] = (-10.0, 0.0, 0.0)
    p.rot["Head"] = (-18.0, 0.0, 0.0)
    shoulders(p, -30.0)
    paws(p, "F", 0.03, 0.008, 0.075, meta=22.0, pole=(0.0, 0.0, -1.0))
    paws(p, "H", 0.05, 0.008, 0.0, meta=88.0, toe=-4.0, pole=(0.0, 1.0, 0.8))
    # The tail lies along the floor and curls round the front paws.
    tail(p, (-55.0, -8.0, 6.0, 6.0, 4.0, 6.0), yaws=(18.0, 26.0, 30.0, 34.0, 34.0, 30.0))
    return p


def sphinx(rig):
    p = Pose(rig)
    # Belly on the floor, front legs out ahead, hind legs folded under, head up.
    p.rump = Vector((0.0, -0.108, 0.01))
    p.rot["Rump"] = (6.0, 0.0, 0.0)
    p.rot["Spine1"] = (-3.0, 0.0, 0.0)
    p.rot["Spine2"] = (-2.0, 0.0, 0.0)
    p.rot["Chest"] = (-4.0, 0.0, 0.0)
    p.rot["Neck1"] = (12.0, 0.0, 0.0)
    p.rot["Neck2"] = (6.0, 0.0, 0.0)
    p.rot["Head"] = (-12.0, 0.0, 0.0)
    shoulders(p, -30.0)
    paws(p, "F", 0.032, 0.009, 0.2, meta=86.0, toe=-6.0, pole=(0.0, -1.0, -0.6))
    paws(p, "H", 0.055, 0.009, -0.035, meta=90.0, toe=-4.0, pole=(0.0, 1.0, 0.8))
    tail(p, (-30.0, -6.0, 2.0, 2.0, 2.0, 4.0), yaws=(14.0, 20.0, 22.0, 24.0, 22.0, 18.0))
    return p


CURL_ROLL = 75.0  # degrees the pelvis rolls onto its right side to curl


def curl(rig):
    """Curled asleep, the way a cat does it: lying on its side with the spine flexed, belly
    inside, so the head comes round to the hind legs and the tail follows the same way to the
    nose. In this rig that is a roll of the pelvis and nose-down pitch down the chain, which
    the roll lays flat into a ring on the floor."""
    p = Pose(rig)
    p.rump = Vector((0.0, -0.162, 0.0))
    p.rot["Rump"] = (0.0, 0.0, CURL_ROLL)
    for b, pitch in (("Spine1", -22.0), ("Spine2", -30.0), ("Chest", -30.0),
                     ("Neck1", -40.0), ("Neck2", -36.0), ("Head", -38.0)):
        p.rot[b] = (pitch, 0.0, 0.0)
    shoulders(p, -40.0)
    # The legs fold against the belly, inside the ring, posed in the turned body's own frame.
    paws(p, "F", 0.03, -0.11, 0.06, at="paw_rel", rel=1.0, meta=140.0, toe=-30.0,
         pole=(0.0, -0.3, -1.0))
    paws(p, "H", 0.05, -0.11, 0.07, at="paw_rel", rel=1.0, meta=95.0, toe=-6.0,
         pole=(0.0, 1.0, 1.0))
    # The tail curls the same way as the spine, round the outside to the nose.
    tail(p, (10.0, 30.0, 35.0, 35.0, 32.0, 25.0))
    lids(p, 1.0)
    return p


def lids(p, closed):
    for s in ("L", "R"):
        p.rot[f"Lid.{s}"] = (-LID_CLOSE * closed, 0.0, 0.0)


# ---------------------------------------------------------------------------------------------
# Clips: name -> (seconds, looping, function(rig, t) -> Pose, events, stated travel (model z))


def breathe(p, t, period, amount=1.2):
    s = math.sin(2.0 * math.pi * t / period)
    add_rot(p, "Spine2", pitch=amount * 0.5 * s)
    add_rot(p, "Chest", pitch=-amount * s)
    add_rot(p, "Neck1", pitch=amount * 0.5 * s)


def tail_wave(p, t, period, amp, lag=0.6):
    for i in range(1, 7):
        add_rot(p, f"Tail{i}", yaw=amp * (i / 6) * math.sin(2.0 * math.pi * t / period - lag * i))


def clip_idle(rig, t):
    p = stand(rig)
    T = 4.0
    w = math.sin(2.0 * math.pi * t / T)
    p.rump = Vector((0.003 * w, 0.0015 * math.cos(4.0 * math.pi * t / T), 0.0))
    add_rot(p, "Rump", roll=1.2 * w)
    add_rot(p, "Chest", roll=-1.0 * w)
    breathe(p, t, 2.0)
    add_rot(p, "Neck2", yaw=4.0 * math.sin(2.0 * math.pi * t / T + 0.8))
    add_rot(p, "Head", yaw=5.0 * math.sin(2.0 * math.pi * t / T + 0.8), pitch=2.0 * w)
    tail_wave(p, t, T, 8.0)
    e = bump(t, 2.6, 0.35)
    add_rot(p, "Ear.L", yaw=28.0 * e)
    add_rot(p, "Ear.R", yaw=-12.0 * e)
    return p


class Gait(NamedTuple):
    """A stepping gait: a cycle's length, how far the body goes in one, how much of it each
    paw is down, when each paw lands, and how high a fore and a hind paw lift."""
    seconds: float
    stride: float
    duty: float
    footfall: dict  # leg -> where in the cycle it lands, in the order the paws come down
    lift_fore: float
    lift_hind: float


def paw_events(g):
    """A gait's footfalls as clip events, paw_lh for the left hind and so on."""
    return [(f"paw_{leg[1].lower()}{leg[0].lower()}", at * g.seconds)
            for leg, at in g.footfall.items()]


# Lateral sequence: left hind, left fore, right hind, right fore, a quarter cycle apart; the
# first footfall a frame in, so no event sits on the loop's seam.
FOOTFALL = {"HL": 0.025, "FL": 0.275, "HR": 0.525, "FR": 0.775}
WALK = Gait(0.80, 0.44, 0.62, FOOTFALL, 0.035, 0.03)


def gait_paw(rig, leg, s, g, lift):
    """Where a paw is at cycle position s (cycles since the start) of gait g: planted through
    its stance, carried along an arc through its swing. Also how far through the swing it is,
    -1 in stance, and the z it took off from."""
    base = rig.rest_legs[leg]["paw"]
    tau = s - g.footfall[leg]
    n = math.floor(tau)
    u = tau - n
    z0 = base[2] + g.stride * (n + g.footfall[leg] + g.duty / 2.0)
    if u < g.duty:
        return (base[0], base[1], z0), -1.0, z0
    w = (u - g.duty) / (1.0 - g.duty)
    z = z0 + g.stride * smooth(w)
    y = base[1] + lift * math.sin(math.pi * w) ** 1.5
    return (base[0], y, z), w, z0


def gait(rig, t, g, ground=None):
    """Gait g at time t: the body carried a stride a cycle over paws planted in model space,
    each rolling over its paw in stance and folding through its swing. `ground(z)`, where given,
    is the height of what a paw stands on at model z, relative to where the body's feet are
    carried: a flight's treads."""
    p = stand(rig)
    s = t / g.seconds
    p.root = Vector((0.0, 0.0, g.stride * s))
    for name, leg in LEGS.items():
        paw, w, z0 = gait_paw(rig, name, s, g, g.lift_fore if leg.fore else g.lift_hind)
        if ground is not None:
            under = ground(z0) if w < 0.0 else _mix(ground(z0), ground(z0 + g.stride), smooth(w))
            paw = (paw[0], paw[1] + under, paw[2])
        # Paws are planted in model space; the body travels over them.
        p.legs[name]["paw"] = paw
        rest = p.legs[name]["meta"]
        if w < 0.0:
            # Through the stance the metapodial rolls forward over the paw.
            tau = (s - g.footfall[name]) % 1.0
            p.legs[name]["meta"] = rest + (tau / g.duty - 0.5) * (14.0 if leg.fore else 20.0)
        else:
            # Through the swing the paw folds back and opens again before it lands.
            fold = math.sin(math.pi * w)
            p.legs[name]["meta"] = rest + (60.0 if leg.fore else 35.0) * fold
            p.legs[name]["toe"] -= 25.0 * fold
    return p, s


def clip_walk(rig, t):
    T = WALK.seconds
    p, s = gait(rig, t, WALK)
    # Two small rises a cycle, a roll that follows the hind legs, the spine swinging with them.
    p.rump = Vector((0.0, 0.004 * math.cos(4.0 * math.pi * (s - 0.15)), 0.0))
    add_rot(p, "Rump", roll=2.0 * math.sin(2.0 * math.pi * (s - 0.1)), yaw=2.5 * math.sin(2.0 * math.pi * s))
    add_rot(p, "Spine2", yaw=-2.0 * math.sin(2.0 * math.pi * s))
    add_rot(p, "Chest", roll=-1.5 * math.sin(2.0 * math.pi * (s - 0.35)), yaw=-1.5 * math.sin(2.0 * math.pi * s))
    # The head stays level and pointed ahead while everything under it moves.
    add_rot(p, "Neck1", pitch=-3.0)
    add_rot(p, "Head", yaw=1.5 * math.sin(2.0 * math.pi * s), pitch=1.0 * math.cos(4.0 * math.pi * s))
    # The tail up, as a cat carries it walking about the house, its tip curling over.
    tail(p, TAIL_UP)
    tail_wave(p, t, T, 5.0)
    return p


# The trot: diagonal pairs together, left hind with right fore and then right hind with left
# fore, at 1.2 m/s, each paw down for under half the cycle.
TROT = Gait(0.5, 0.6, 0.45, {"HL": 0.025, "FR": 0.025, "HR": 0.525, "FL": 0.525}, 0.045, 0.04)


def clip_trot(rig, t):
    T = TROT.seconds
    p, s = gait(rig, t, TROT)
    # A bounce at each pair's push, the body a little lower and longer than at a walk.
    p.rump = Vector((0.0, -0.006 + 0.006 * math.cos(4.0 * math.pi * (s - 0.2)), 0.0))
    add_rot(p, "Rump", roll=1.5 * math.sin(2.0 * math.pi * s))
    add_rot(p, "Chest", roll=-1.5 * math.sin(2.0 * math.pi * s))
    add_rot(p, "Neck1", pitch=-6.0)
    add_rot(p, "Head", pitch=2.0 + 1.5 * math.cos(4.0 * math.pi * s))
    tail(p, TAIL_UP, 0.6)
    tail_wave(p, t, T, 4.0)
    return p


# The house's stairs, and how cat_places.c lays a flight's link: from half a tread before the
# first riser to half a tread past the last, so its line through the treads' middles runs
# from one end of the link to the other and a two-tread cycle lands every paw on a tread.
def layout_number(name):
    """A plain number apps/silent/src/layout.h #defines, so the clips stand on the house's own
    stairs rather than a copy of them."""
    path = os.path.join(ROOT, "apps", "silent", "src", "layout.h")
    m = re.search(rf"^#define {name}\s+([0-9.]+)f?\b", open(path).read(), re.M)
    if not m:
        sys.exit(f"{path} defines no plain number {name}")
    return float(m.group(1))


STAIR_RISE = ((layout_number("FLOOR2_Y") - layout_number("FLOOR_Y"))
              / layout_number("STAIR_RISERS"))
STAIR_GOING = layout_number("STAIR_GOING")
STAIR = Gait(0.8, 2.0 * STAIR_GOING, WALK.duty, FOOTFALL, 0.06, 0.05)
STAIR_PITCH = 22.0  # degrees the body leans with the flight


def tread_height(z, sign):
    """The tread under model z, up (sign 1) or down a flight starting at the body's feet,
    relative to where the flight's line through the treads carries those feet at the same z."""
    risers = math.floor((z - 0.5 * STAIR_GOING) / STAIR_GOING) + 1
    return sign * STAIR_RISE * risers


def clip_stair(rig, t, sign):
    """Up (sign 1) or down a flight, two treads a cycle. The flight's rise is not in the clip:
    the root goes forward only, and the game lifts the body along the line through the treads,
    so here a paw stands at its tread's height less that line's at the body."""
    T = STAIR.seconds
    slope = STAIR_RISE / STAIR_GOING
    root_z = STAIR.stride * t / T
    p, s = gait(rig, t, STAIR, ground=lambda z: tread_height(z, sign) - sign * slope * root_z)
    add_rot(p, "Rump", pitch=sign * STAIR_PITCH, roll=1.5 * math.sin(2.0 * math.pi * s))
    # The head is held level against the lean, looking where the paws go next.
    add_rot(p, "Neck1", pitch=-sign * 0.5 * STAIR_PITCH)
    add_rot(p, "Head", pitch=-sign * 0.4 * STAIR_PITCH - 8.0)
    tail(p, TAIL_UP, 0.5 if sign > 0 else 0.3)
    tail_wave(p, t, T, 4.0)
    return p


# A quarter turn on the spot: the body swings round over a quarter of a second more than half
# its length while the paws step to where they stand in the turned body, fore first.
TURN_SECONDS = 0.7
TURN_STEPS = (("FL", 0.05), ("FR", 0.22), ("HR", 0.38), ("HL", 0.52))
TURN_STEP_LENGTH = 0.38


def clip_turn(rig, t, sign):
    T = TURN_SECONDS
    u = t / T
    p = stand(rig)
    turn = sign * 90.0
    p.root_yaw = turn * smooth(window(u, 0.05, 0.9))
    a = math.radians(turn)
    for leg, start in TURN_STEPS:
        x, y, z = p.legs[leg]["paw"]
        # Where the paw stands once the body has turned: its place in the body, turned with it.
        end = (x * math.cos(a) + z * math.sin(a), y, -x * math.sin(a) + z * math.cos(a))
        w = window(u, start, start + TURN_STEP_LENGTH)
        p.legs[leg]["paw"] = (_mix(x, end[0], smooth(w)),
                              y + 0.03 * math.sin(math.pi * w),
                              _mix(z, end[2], smooth(w)))
    add_rot(p, "Head", yaw=sign * 25.0 * bump(u, 0.3, 0.5))
    add_rot(p, "Neck2", yaw=sign * 12.0 * bump(u, 0.3, 0.5))
    tail_wave(p, t, T, 10.0)
    return p


# Jumps, on the spot: the game carries the body along the arc between the clip's takeoff and
# its landing, and plays the clip at whatever rate makes the time between them the arc's own
# time of flight. Up: a crouch with the eyes on the place, the push, the forelegs reaching,
# the landing and its absorbing. Down: a crouch looking over the edge, a reach down, the fore
# paws landing a moment before the hind.
JUMP_SECONDS = 1.1
JUMP_UP_EVENTS = (("takeoff", 0.42), ("land", 0.80))
JUMP_DOWN_EVENTS = (("takeoff", 0.40), ("land", 0.72), ("land_hind", 0.80))


def _air(p, leg, rel, paw_rel):
    """A leg carried by its girdle through the air, `rel` of the way from where it stood."""
    p.legs[leg]["rel"] = rel
    p.legs[leg]["paw_rel"] = paw_rel


def clip_jump_up(rig, t):
    takeoff, land = (e[1] for e in JUMP_UP_EVENTS)
    p = stand(rig)
    crouch = smooth(window(t, 0.0, takeoff - 0.08)) * (1.0 - smooth(window(t, takeoff - 0.06, takeoff)))
    absorb = bump(t, land + 0.06, 0.16)
    p.rump = Vector((0.0, -0.06 * crouch - 0.035 * absorb, -0.01 * crouch))
    flight = window(t, takeoff - 0.02, takeoff + 0.04) * (1.0 - window(t, land - 0.05, land))
    air = (t - takeoff) / (land - takeoff)
    add_rot(p, "Rump", pitch=18.0 * flight * (1.0 - air) + 6.0 * crouch)
    add_rot(p, "Neck1", pitch=12.0 * crouch - 6.0 * flight)
    add_rot(p, "Head", pitch=10.0 * crouch)
    for side in ("L", "R"):
        x = 0.045 if side == "L" else -0.045
        # The fore reach up and forward for the edge, then come down onto it.
        fore = (x, _mix(-0.1, -0.2, smooth(air)), _mix(0.17, 0.08, smooth(air)))
        _air(p, f"F{side}", flight, fore)
        # The hind push out behind, then tuck under.
        hind = (x, _mix(-0.19, -0.12, smooth(air)), _mix(-0.17, -0.03, smooth(air)))
        _air(p, f"H{side}", flight, hind)
    tail(p, [pitch * 0.3 for pitch in TAIL_UP], flight)
    return p


def clip_jump_down(rig, t):
    takeoff, land, land_hind = (e[1] for e in JUMP_DOWN_EVENTS)
    p = stand(rig)
    crouch = smooth(window(t, 0.0, takeoff - 0.1)) * (1.0 - smooth(window(t, takeoff - 0.06, takeoff)))
    absorb = bump(t, land_hind + 0.05, 0.2)
    p.rump = Vector((0.0, -0.04 * crouch - 0.04 * absorb, 0.0))
    fore_air = window(t, takeoff - 0.02, takeoff + 0.04) * (1.0 - window(t, land - 0.04, land))
    hind_air = window(t, takeoff, takeoff + 0.05) * (1.0 - window(t, land_hind - 0.04, land_hind))
    air = (t - takeoff) / (land_hind - takeoff)
    add_rot(p, "Rump", pitch=-16.0 * fore_air - 6.0 * crouch)
    # Looking down over the edge, and at the floor coming up.
    add_rot(p, "Neck1", pitch=-14.0 * crouch - 6.0 * fore_air)
    add_rot(p, "Head", pitch=-18.0 * crouch - 8.0 * fore_air)
    for side in ("L", "R"):
        x = 0.045 if side == "L" else -0.045
        _air(p, f"F{side}", fore_air, (x, -0.25, _mix(0.13, 0.09, smooth(air))))
        _air(p, f"H{side}", hind_air, (x, _mix(-0.14, -0.19, smooth(air)), -0.06))
    tail(p, [pitch * 0.4 for pitch in TAIL_UP], max(fore_air, hind_air))
    return p


# Pitches that take the resting tail, which droops, to one carried up: about straight up from
# the root, the tip leaning forward.
TAIL_UP = (-90.0, -22.0, -3.0, 15.0, 25.0, 40.0)


SIT_DOWN_SECONDS = 0.8
LIE_DOWN_SECONDS = 1.0
CURL_UP_SECONDS = 1.6


def reverse(name):
    """A transition played backwards: the clip `name` in CLIPS from its end to its start."""
    def played_back(rig, t):
        seconds, _, forward, _ = CLIPS[name]
        return forward(rig, seconds - t)
    return played_back


def clip_sit_down(rig, t):
    u = t / SIT_DOWN_SECONDS
    a, b = stand(rig), sit(rig)
    # The rump goes down first, the front legs straighten as it does.
    p = lerp_pose(rig, a, b, smooth(u))
    rump = smooth(min(1.0, u * 1.25))
    p.rump = a.rump.lerp(b.rump, rump)
    p.rot["Rump"] = tuple(x * rump for x in b.rot["Rump"])
    return p


def clip_sit(rig, t):
    p = sit(rig)
    breathe(p, t, 3.0, 1.0)
    for at in (1.2, 4.0):
        e = bump(t, at, 0.5)
        add_rot(p, "Tail5", pitch=14.0 * e)
        add_rot(p, "Tail6", pitch=22.0 * e)
    add_rot(p, "Ear.L", yaw=30.0 * bump(t, 2.2, 0.3))
    add_rot(p, "Ear.R", yaw=-30.0 * bump(t, 4.8, 0.3))
    add_rot(p, "Head", yaw=6.0 * math.sin(2.0 * math.pi * t / 6.0))
    return p


# Lying down the front paws walk forward one at a time: when each starts, as a share of the
# clip, and how long its step takes; each lands at the step's end.
LIE_DOWN_STEPS = (("FL", 0.15), ("FR", 0.38))
LIE_DOWN_STEP = 0.3


def clip_lie_down(rig, t):
    """Sit to sphinx: the chest lowers while the front paws walk forward, left then right."""
    u = t / LIE_DOWN_SECONDS
    a, b = sit(rig), sphinx(rig)
    p = lerp_pose(rig, a, b, smooth(u))
    for leg, start in LIE_DOWN_STEPS:
        w = window(u, start, start + LIE_DOWN_STEP)
        pa, pb = a.legs[leg]["paw"], b.legs[leg]["paw"]
        p.legs[leg]["paw"] = (pa[0] + (pb[0] - pa[0]) * w,
                              pa[1] + (pb[1] - pa[1]) * w + 0.03 * math.sin(math.pi * w),
                              pa[2] + (pb[2] - pa[2]) * w)
    return p


def clip_lie(rig, t):
    p = sphinx(rig)
    breathe(p, t, 2.5, 1.2)
    e = bump(t, 3.5, 0.5)
    add_rot(p, "Tail5", yaw=20.0 * e)
    add_rot(p, "Tail6", yaw=30.0 * e)
    add_rot(p, "Head", yaw=5.0 * math.sin(2.0 * math.pi * t / 5.0))
    return p


# Curling up the lids close over this stretch of the clip, and uncurling open over its mirror;
# the game takes them as shut from its middle, which `lids_shut` and `lids_open` carry.
CURL_LIDS = (1.15, 1.4)


def clip_curl_up(rig, t):
    u = t / CURL_UP_SECONDS
    p = lerp_pose(rig, sphinx(rig), curl(rig), smooth(u))
    lids(p, window(t, *CURL_LIDS))
    return p


def clip_sleep(rig, t):
    p = curl(rig)
    breathe(p, t, 3.0, 1.6)
    add_rot(p, "Ear.L", yaw=20.0 * bump(t, 4.4, 0.25))
    add_rot(p, "Tail6", pitch=18.0 * bump(t, 2.0, 0.4))
    return p


def jaw_open(t, peaks):
    """The jaw through a meow: each (time, open degrees, hold)."""
    a = 0.0
    for at, deg, hold in peaks:
        a = max(a, deg * smooth(1.0 - abs(t - at) / hold) if abs(t - at) < hold else 0.0)
    return a


def clip_meow_short(rig, t):
    p = stand(rig)
    add_rot(p, "Jaw", pitch=-jaw_open(t, [(0.22, 24.0, 0.2)]))
    ears(p, yaw=-14.0 * bump(t, 0.25, 0.4))
    return p


def clip_meow_long(rig, t):
    p = stand(rig)
    add_rot(p, "Jaw", pitch=-jaw_open(t, [(0.25, 18.0, 0.22), (0.62, 28.0, 0.42)]))
    ears(p, yaw=-16.0 * (window(t, 0.05, 0.3) * (1.0 - window(t, 0.8, 1.05))))
    return p


BLINK_SECONDS = 8 / FPS


def clip_blink(rig, t):
    p = stand(rig)
    lids(p, math.sin(math.pi * min(t / BLINK_SECONDS, 1.0)))
    return p


def clip_trill(rig, t):
    """A chirrup with the mouth barely open, the ears pricked forward."""
    p = stand(rig)
    add_rot(p, "Jaw", pitch=-jaw_open(t, [(0.14, 9.0, 0.12)]))
    ears(p, pitch=10.0 * bump(t, 0.18, 0.3))
    return p


def clip_yawn(rig, t):
    """The mouth wide and held, the eyes squeezed half shut and the ears laid back with it."""
    p = stand(rig)
    wide = window(t, 0.25, 0.7) * (1.0 - window(t, 1.2, 1.6))
    add_rot(p, "Jaw", pitch=-46.0 * wide)
    lids(p, 0.65 * wide)
    ears(p, pitch=-25.0 * wide)
    return p


def clip_slow_blink(rig, t):
    """What a cat says to someone it trusts: the lids down slowly, held, up slowly."""
    p = stand(rig)
    lids(p, window(t, 0.0, 0.55) * (1.0 - window(t, 1.0, 1.55)))
    return p


EAR_FLICK_SECONDS = 10 / FPS


def clip_ear_flick(rig, t, side):
    p = stand(rig)
    sign = 1.0 if side == "L" else -1.0
    e = bump(t, 0.1, 0.16)
    add_rot(p, f"Ear.{side}", yaw=sign * 32.0 * e, pitch=-8.0 * e)
    return p


# The run: a half-bound, the hind pair landing close together and then the fore pair, at
# 2.45 m/s. The spine gathers the hind paws under it and stretches the fore paws out.
RUN = Gait(11 / FPS, 0.9, 0.3, {"HL": 0.02, "HR": 0.1, "FL": 0.48, "FR": 0.58}, 0.06, 0.06)


def clip_run(rig, t):
    T = RUN.seconds
    p, s = gait(rig, t, RUN)
    flex = math.sin(2.0 * math.pi * (s - 0.1))
    p.rump = Vector((0.0, -0.012 + 0.012 * math.cos(4.0 * math.pi * (s - 0.25)), 0.0))
    add_rot(p, "Rump", pitch=-4.0 * flex)
    add_rot(p, "Spine1", pitch=7.0 * flex)
    add_rot(p, "Spine2", pitch=5.0 * flex)
    add_rot(p, "Chest", pitch=-8.0 * flex)
    add_rot(p, "Neck1", pitch=-10.0 + 4.0 * flex)
    add_rot(p, "Head", pitch=6.0 - 3.0 * flex)
    tail(p, TAIL_UP, 0.15)
    tail_wave(p, t, T, 3.0)
    return p


# Along the gallery's hand rail, 9 cm wide: slow and low, every paw set down on the line the
# body travels along, the tail straight out behind for balance.
BEAM = Gait(1.0, 0.40, 0.72, FOOTFALL, 0.025, 0.022)
BEAM_LINE = 0.012  # how far either side of the centre line a paw lands


def clip_beam_walk(rig, t):
    T = BEAM.seconds
    p, s = gait(rig, t, BEAM)
    for leg in LEGS:
        x, y, z = p.legs[leg]["paw"]
        p.legs[leg]["paw"] = (BEAM_LINE if x > 0.0 else -BEAM_LINE, y, z)
        # The knees and elbows go out to let the paws in under the body.
        px, py, pz = p.legs[leg]["pole"]
        p.legs[leg]["pole"] = (px + (0.35 if x > 0.0 else -0.35), py, pz)
    p.rump = Vector((0.0, -0.025 + 0.002 * math.cos(4.0 * math.pi * s), 0.0))
    add_rot(p, "Rump", roll=1.0 * math.sin(2.0 * math.pi * s))
    add_rot(p, "Chest", roll=-1.0 * math.sin(2.0 * math.pi * s))
    add_rot(p, "Neck1", pitch=-12.0)
    add_rot(p, "Head", pitch=-6.0)
    tail(p, (-35.0, 4.0, 4.0, 3.0, 2.0, 2.0))
    tail_wave(p, t, T, 6.0, lag=0.3)
    return p


# A rest-pose point on the face: where the tongue meets the paw.
MOUTH = (0.0, 0.249, 0.244)
EAR_FRONT_R = (-0.022, 0.29, 0.205)


def head_point(rig, M, point):
    """Where a rest-pose point on the head is in posed matrices M, model coordinates."""
    return mv(M["Head"] @ rig.rest["Head"].inverted() @ bv(*point))


GROOM_SECONDS = 4.0


def clip_groom(rig, t):
    """Sitting, the right fore paw up at the mouth and licked, then drawn over the ear and down
    the cheek, and back to the mouth. The paw stays up for the whole loop."""
    u = t / GROOM_SECONDS
    p = sit(rig)
    breathe(p, t, 3.0, 0.8)
    wipe = window(u, 0.48, 0.58) * (1.0 - window(u, 0.86, 0.97))
    lick = 1.0 - wipe
    pulse = max(0.0, math.sin(2.0 * math.pi * t / 0.45))
    add_rot(p, "Neck1", pitch=-8.0 * lick)
    add_rot(p, "Neck2", pitch=-6.0 * lick, yaw=-12.0 * wipe)
    add_rot(p, "Head", pitch=-12.0 * lick - 5.0 * pulse * lick, yaw=-10.0 * wipe, roll=-16.0 * wipe)
    add_rot(p, "Jaw", pitch=-7.0 * pulse * lick)
    add_rot(p, "Ear.R", pitch=-20.0 * wipe)
    M = rig.solve(p)
    mouth = head_point(rig, M, MOUTH)
    ear = head_point(rig, M, EAR_FRONT_R)
    lick_at = mouth + Vector((-0.008, -0.016, 0.01 + 0.004 * pulse))
    sweep = smooth((u - 0.58) / 0.26)
    wipe_at = ear.lerp(mouth, sweep) + Vector((-0.016, 0.006, 0.012))
    paw = lick_at.lerp(wipe_at, wipe)
    p.legs["FR"].update(paw=tuple(paw), meta=150.0, toe=120.0, pole=(-0.2, -1.0, -0.4))
    return p


STRETCH_SECONDS = 2.4


def clip_stretch(rig, t):
    """The bow, fore paws walked out and the chest down with the rump up, then forward over
    them with a hind leg drawn out straight behind. Starts and ends standing."""
    u = t / STRETCH_SECONDS
    p = stand(rig)
    bow = window(u, 0.06, 0.3) * (1.0 - window(u, 0.42, 0.56))
    reach = window(u, 0.56, 0.7) * (1.0 - window(u, 0.86, 0.98))
    p.rump = Vector((0.0, 0.012 * bow - 0.01 * reach, 0.0))
    add_rot(p, "Spine1", pitch=-5.0 * bow + 3.0 * reach)
    add_rot(p, "Spine2", pitch=-8.0 * bow + 3.0 * reach)
    add_rot(p, "Chest", pitch=-10.0 * bow + 4.0 * reach)
    add_rot(p, "Neck1", pitch=22.0 * bow + 6.0 * reach)
    add_rot(p, "Head", pitch=8.0 * bow)
    for leg in ("FL", "FR"):
        x, y, z = p.legs[leg]["paw"]
        w = window(u, 0.04 if leg == "FL" else 0.12, 0.2 if leg == "FL" else 0.28)
        back = window(u, 0.44 if leg == "FL" else 0.5, 0.56 if leg == "FL" else 0.62)
        out = w * (1.0 - back)
        lift = 0.025 * (math.sin(math.pi * w) if back == 0.0 else math.sin(math.pi * back))
        p.legs[leg].update(paw=(x, y + lift, z + 0.13 * out),
                           meta=p.legs[leg]["meta"] + 40.0 * bow)
    x, y, z = p.legs["HL"]["paw"]
    p.legs["HL"].update(paw=(x, y + 0.03 * reach, z - 0.13 * reach),
                        meta=p.legs["HL"]["meta"] - 50.0 * reach,
                        toe=p.legs["HL"]["toe"] - 60.0 * reach)
    tail(p, [pitch * 0.7 for pitch in TAIL_UP], bow)
    lids(p, 0.5 * bow)
    return p


def arched(rig, amount):
    """Startled: up on straight legs with the back humped, the head low, the ears flat and
    the tail up in an arch."""
    # The hump peaks over the middle of the back: the spine climbs steeply off the hips, runs
    # level for one segment, and comes down as steeply to the shoulders.
    p = stand(rig)
    p.rump = Vector((0.0, 0.03 * amount, 0.0))
    add_rot(p, "Spine1", pitch=28.0 * amount)
    add_rot(p, "Spine2", pitch=-26.0 * amount)
    add_rot(p, "Chest", pitch=-26.0 * amount)
    add_rot(p, "Neck1", pitch=-4.0 * amount)
    add_rot(p, "Neck2", pitch=4.0 * amount)
    add_rot(p, "Head", pitch=14.0 * amount)
    ears(p, pitch=-50.0 * amount, yaw=22.0 * amount)
    tail(p, (-80.0, -20.0, 10.0, 25.0, 30.0, 20.0), amount)
    return p


def clip_startle(rig, t):
    """A flinch down and back, then straight up into the arch, held at the end."""
    p = arched(rig, smooth(window(t, 0.08, 0.32)))
    flinch = bump(t, 0.06, 0.14)
    p.rump = p.rump + Vector((0.0, -0.03 * flinch, -0.02 * flinch))
    lids(p, 0.0)
    return p


HISS_SECONDS = 2.0


def clip_hiss(rig, t):
    """Arched, the head thrust low and forward with the mouth wide, swaying a little."""
    p = arched(rig, 1.0)
    open_ = window(t, 0.1, 0.3) * (1.0 - window(t, 1.3, 1.6))
    add_rot(p, "Jaw", pitch=-34.0 * open_)
    add_rot(p, "Neck1", pitch=-8.0 * open_)
    add_rot(p, "Head", pitch=10.0 * open_)
    add_rot(p, "Rump", yaw=2.0 * math.sin(2.0 * math.pi * t / HISS_SECONDS))
    return p


# name: (seconds, looping, pose function, events [(name, seconds)])
CLIPS = {
    "idle": (4.0, True, clip_idle, []),
    "walk": (WALK.seconds, True, clip_walk, paw_events(WALK)),
    "sit_down": (SIT_DOWN_SECONDS, False, clip_sit_down, []),
    "stand_up": (SIT_DOWN_SECONDS, False, reverse("sit_down"), []),
    "sit": (6.0, True, clip_sit, []),
    "lie_down": (LIE_DOWN_SECONDS, False, clip_lie_down,
                 [(f"paw_{leg[1].lower()}{leg[0].lower()}",
                   (start + LIE_DOWN_STEP) * LIE_DOWN_SECONDS) for leg, start in LIE_DOWN_STEPS]),
    "sit_up": (LIE_DOWN_SECONDS, False, reverse("lie_down"), []),
    "lie": (5.0, True, clip_lie, []),
    "curl_up": (CURL_UP_SECONDS, False, clip_curl_up, [("lids_shut", sum(CURL_LIDS) / 2.0)]),
    "uncurl": (CURL_UP_SECONDS, False, reverse("curl_up"),
               [("lids_open", CURL_UP_SECONDS - sum(CURL_LIDS) / 2.0)]),
    "sleep": (6.0, True, clip_sleep, []),
    "meow_short": (0.6, False, clip_meow_short, [("meow", 0.08)]),
    "meow_long": (1.1, False, clip_meow_long, [("meow", 0.08)]),
    "blink": (BLINK_SECONDS, False, clip_blink, []),
    "trot": (TROT.seconds, True, clip_trot, paw_events(TROT)),
    "stair_up": (STAIR.seconds, True, partial(clip_stair, sign=1.0), paw_events(STAIR)),
    "stair_down": (STAIR.seconds, True, partial(clip_stair, sign=-1.0), paw_events(STAIR)),
    "turn_l90": (TURN_SECONDS, False, partial(clip_turn, sign=1.0), []),
    "turn_r90": (TURN_SECONDS, False, partial(clip_turn, sign=-1.0), []),
    "jump_up": (JUMP_SECONDS, False, clip_jump_up, list(JUMP_UP_EVENTS)),
    "jump_down": (JUMP_SECONDS, False, clip_jump_down, list(JUMP_DOWN_EVENTS)),
    "run": (RUN.seconds, True, clip_run, paw_events(RUN)),
    "beam_walk": (BEAM.seconds, True, clip_beam_walk, paw_events(BEAM)),
    "groom": (GROOM_SECONDS, True, clip_groom, []),
    "stretch": (STRETCH_SECONDS, False, clip_stretch, []),
    "startle": (0.7, False, clip_startle, []),
    "hiss": (HISS_SECONDS, True, clip_hiss, [("hiss", 0.12)]),
    "trill": (0.5, False, clip_trill, [("trill", 0.04)]),
    "yawn": (1.8, False, clip_yawn, []),
    "slow_blink": (1.6, False, clip_slow_blink, []),
    "ear_flick_l": (EAR_FLICK_SECONDS, False, partial(clip_ear_flick, side="L"), []),
    "ear_flick_r": (EAR_FLICK_SECONDS, False, partial(clip_ear_flick, side="R"), []),
}


# ---------------------------------------------------------------------------------------------
# Keying


def pose_to_basis(rig, M):
    """Each bone's matrix_basis for armature-space pose matrices M."""
    out = {}
    for n in rig.order:
        p = rig.parent[n]
        parent = M[p] if p else Matrix.Identity(4)
        out[n] = rig.rest_rel[n].inverted() @ parent.inverted() @ M[n]
    return out


def key_clip(arm, rig, name, seconds, fn):
    """The clip keyed into an action of its own, at every frame. Returns how far it carries
    Hips along model z, first frame to last."""
    act = bpy.data.actions.new(name)
    act.use_fake_user = True
    arm.animation_data_create()
    arm.animation_data.action = act
    frames = int(round(seconds * FPS))
    channels = {}
    prev = {}
    for f in range(frames + 1):
        t = min(f / FPS, seconds)
        M = rig.solve(fn(rig, t))
        if f == 0:
            start = mv(M["Hips"].translation)
        basis = pose_to_basis(rig, M)
        for n, b in basis.items():
            loc, q, _ = b.decompose()
            if n in prev and prev[n].dot(q) < 0.0:
                q = -q
            prev[n] = q
            ch = channels.setdefault(n, {"loc": [], "rot": []})
            ch["loc"].append((f, tuple(loc)))
            ch["rot"].append((f, tuple(q)))
    for n, ch in channels.items():
        path = f'pose.bones["{n}"]'
        for prop, size in (("location", 3), ("rotation_quaternion", 4)):
            keys = ch["loc" if prop == "location" else "rot"]
            for i in range(size):
                fc = act.fcurve_ensure_for_datablock(arm, f"{path}.{prop}", index=i, group_name=n)
                fc.keyframe_points.add(len(keys))
                co = []
                for f, v in keys:
                    co += [float(f), v[i]]
                fc.keyframe_points.foreach_set("co", co)
                fc.keyframe_points.foreach_set("interpolation", [1] * len(keys))  # LINEAR
                fc.update()
    arm.animation_data.action = None
    return (mv(M["Hips"].translation) - start).z


# ---------------------------------------------------------------------------------------------
# Export, and what the export is held to


def export(path):
    props = bpy.ops.export_scene.gltf.get_rna_type().properties.keys()
    want = dict(
        filepath=path, export_format="GLB", export_yup=True, use_selection=False,
        export_apply=False, export_animations=True, export_animation_mode="ACTIONS",
        export_force_sampling=True, export_frame_step=1, export_optimize_animation_size=False,
        export_optimize_animation_keep_anim_armature=True, export_def_bones=False,
        export_skins=True, export_all_influences=False, export_materials="EXPORT",
        export_vertex_color="ACTIVE", export_normals=True, export_texcoords=False,
        export_tangents=False, export_cameras=False, export_lights=False, export_extras=False,
        export_rest_position_armature=True, export_anim_slide_to_zero=False,
        export_reset_pose_bones=True, export_bake_animation=False,
    )
    missing = [k for k in want if k not in props]
    if missing:
        print(f"export: this Blender has no {', '.join(missing)}; left at its defaults", flush=True)
    bpy.ops.export_scene.gltf(**{k: v for k, v in want.items() if k in props})


def self_check(path, clips):
    """What the engine needs of the file, refused by name when it is not so."""
    js, binary = read_glb(path)
    nodes = js["nodes"]
    joints = set(js["skins"][0]["joints"])
    names = {i: n.get("name", "") for i, n in enumerate(nodes)}
    hips = next(i for i, n in names.items() if n == "Hips")
    r = nodes[hips].get("rotation", [0.0, 0.0, 0.0, 1.0])
    if abs(r[3]) < 0.99999:
        sys.exit(f"Hips binds with rotation {r}, not the identity: root motion would read it")
    anims = {a["name"]: a for a in js.get("animations", [])}
    problems = []
    for name in clips:
        a = anims.get(name)
        if not a:
            problems.append(f"{name}: not in the file (have {sorted(anims)})")
            continue
        by_node = {}
        for ch in a["channels"]:
            by_node.setdefault(ch["target"]["node"], {})[ch["target"]["path"]] = ch["sampler"]
        for j in joints:
            paths = by_node.get(j, {})
            if "rotation" in paths and "translation" not in paths:
                problems.append(f"{name}: joint {names[j]} turns with no translation track")
        tr = by_node.get(hips, {}).get("translation")
        if tr is None:
            problems.append(f"{name}: Hips has no translation track")
            continue
        out = accessor(js, binary, a["samplers"][tr]["output"])
        d = [out[-1][k] - out[0][k] for k in range(3)]
        stated = clips[name][4]
        if abs(d[2] - stated) > 1e-3 or abs(d[0]) > 1e-3:
            problems.append(f"{name}: Hips travels {d}, stated {stated}")
        times = accessor(js, binary, a["samplers"][tr]["input"])
        if abs(times[-1][0] - times[0][0] - clips[name][0]) > 1e-4:
            problems.append(f"{name}: {times[-1][0] - times[0][0]:.4f} s long in the file, "
                            f"{clips[name][0]:.4f} s in the header")
    if problems:
        sys.exit("self-check:\n  " + "\n  ".join(problems))
    tris = 0
    for m in js["meshes"]:
        for prim in m["primitives"]:
            tris += js["accessors"][prim["indices"]]["count"] // 3
    print(f"self-check: {len(joints)} joints, {len(clips)} clips, {tris} triangles, "
          f"{os.path.getsize(path) / 1024:.0f} KiB", flush=True)


# ---------------------------------------------------------------------------------------------
# The header the game reads


def write_header(rig, clips, path=HEADER):
    names = list(clips)
    eye = rig.rest_point("Eye.L", "head")
    head = rig.rest_point("Head", "head")
    lines = [
        "// Generated by apps/silent/tools/make_cat_blender.py -- do not edit; rerun it.",
        "#ifndef _SILENT_CAT_CLIPS_H_",
        "#define _SILENT_CAT_CLIPS_H_",
        "",
        "// The clips in assets/models/cat.glb, by the names the file gives them.",
        "typedef enum {",
        *[f"    CAT_CLIP_{n.upper()}," for n in names],
        "    CAT_CLIP_COUNT",
        "} CatClipId;",
        "",
        "typedef struct CatClipEvent {",
        "    const char* name;",
        "    float seconds; // from the clip's start",
        "} CatClipEvent;",
        "",
        "typedef struct CatClipSpec {",
        "    const char* name;",
        "    float seconds;",
        "    int looping;",
        "    float travel; // metres forward, model space, from first frame to last",
        "    int event_count;",
        f"    CatClipEvent events[{max(len(c[3]) for c in clips.values())}];",
        "} CatClipSpec;",
        "",
        "static const CatClipSpec CAT_CLIPS[CAT_CLIP_COUNT] = {",
    ]
    for n in names:
        seconds, looping, _, events, travel = clips[n]
        ev = ", ".join(f'{{"{e}", {s:.4f}f}}' for e, s in events) or "{0}"
        lines.append(f'    [CAT_CLIP_{n.upper()}] = {{"{n}", {seconds:.4f}f, {int(looping)}, '
                     f"{travel:.4f}f, {len(events)}, {{{ev}}}}},")
    lines += [
        "};",
        "",
        "// Model space: x the cat's left, y up, z forward, the feet at y = 0.",
        f"#define CAT_EYE_X {abs(eye.x):.4f}f",
        f"#define CAT_EYE_Y {eye.y:.4f}f",
        f"#define CAT_EYE_Z {eye.z:.4f}f",
        f"#define CAT_HEAD_Y {head.y:.4f}f",
        f"#define CAT_HEAD_Z {head.z:.4f}f",
        "",
        "#endif // _SILENT_CAT_CLIPS_H_",
        "",
    ]
    with open(path, "w") as f:
        f.write("\n".join(lines))


# ---------------------------------------------------------------------------------------------
# Stills, to look at a pose without the engine


def preview_names_ok(specs):
    """Every preview spec names something it can show, or the run stops with what it could."""
    bad = []
    for spec in specs:
        name = spec.partition("@")[0]
        clip = name[5:] if name.startswith("legs:") else name
        if clip not in CLIPS and name not in ("rest", "face"):
            bad.append(spec)
    if bad:
        sys.exit(f"no preview of {', '.join(bad)}: a clip ({', '.join(CLIPS)}), rest, face, "
                 "or legs:<clip>, each @seconds")


def preview(arm, rig, specs, width=420):
    """Workbench stills, a row of views for each spec, tiled into out/cat_blender/preview:
      - `clip@seconds`, or `rest` for the bind pose: the whole cat from the side, in front,
        three-quarters and above, and its face close up;
      - `face`: the bind pose's head close up, from in front, the side, three-quarters and below;
      - `legs:clip@seconds`: the hindquarters close up in that pose, from the near side, the far
        side, behind and below."""
    import numpy as np

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "MATERIAL"
    scene.display.shading.show_cavity = False
    scene.render.resolution_x, scene.render.resolution_y = width, int(width * 0.75)
    scene.render.film_transparent = False
    world = scene.world or bpy.data.worlds.new("w")
    scene.world = world
    world.color = (0.55, 0.55, 0.58)
    # Brighter than the fur, so a black cat reads in a still.
    for m in bpy.data.materials:
        if m.name == "cat_fur":
            m.diffuse_color = (0.12, 0.11, 0.11, 1.0)
        elif m.name in MATERIALS:
            c = MATERIALS[m.name][0]
            m.diffuse_color = (*c, 1.0)
    cam_data = bpy.data.cameras.new("preview")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = 0.62
    cam = bpy.data.objects.new("preview", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    views = [("side", bv(-2.0, 0.17, 0.0)), ("front", bv(0.0, 0.25, 2.0)),
             ("threeq", bv(-1.3, 0.9, 1.3)), ("top", bv(0.0, 2.0, 0.001)),
             ("face", bv(-0.35, 0.3, 2.0))]
    face_views = [("front", bv(0.0, 0.0, 2.0)), ("profile", bv(-2.0, 0.0, 0.0)),
                  ("threeq", bv(-1.4, 0.3, 1.4)), ("below", bv(-0.5, -0.8, 1.6))]
    leg_views = [("near", bv(-2.0, 0.1, 0.0)), ("far", bv(2.0, 0.1, 0.0)),
                 ("behind", bv(-0.8, 0.5, -1.8)), ("under", bv(-0.6, -1.5, 0.4))]
    os.makedirs(os.path.join(OUT, "preview"), exist_ok=True)

    def pose_arm(clip, seconds):
        """The armature in a clip's pose, or the bind pose; the pose's matrices."""
        M = rig.solve(CLIPS[clip][2](rig, seconds) if clip in CLIPS else Pose(rig))
        for n, b in pose_to_basis(rig, M).items():
            arm.pose.bones[n].matrix_basis = b
        bpy.context.view_layer.update()
        return M

    def shoot(eye, target, scale, stem):
        """One still from `eye` looking at `target`, as pixels."""
        cam_data.ortho_scale = scale
        cam.location = eye
        cam.rotation_euler = (target - eye).to_track_quat("-Z", "Y").to_euler()
        scene.render.filepath = os.path.join(OUT, "preview", f"{stem}.png")
        bpy.ops.render.render(write_still=True)
        img = bpy.data.images.load(scene.render.filepath)
        px = np.array(img.pixels[:], dtype=np.float32).reshape(img.size[1], img.size[0], 4)
        bpy.data.images.remove(img)
        return px

    tiles = []
    for spec in specs:
        name, _, at = spec.partition("@")
        seconds = float(at or 0.0)
        row = []
        if name.startswith("legs:"):
            clip = name[5:]
            root = mv(pose_arm(clip, seconds)["Hips"].translation)
            target = bv(root.x, 0.12, root.z - 0.08)
            for vname, eye in leg_views:
                row.append(shoot(eye + target, target, 0.3, f"legs_{clip}_{seconds:.2f}_{vname}"))
        elif name == "face":
            pose_arm("rest", 0.0)
            target = bv(0.0, 0.262, 0.228)
            for vname, eye in face_views:
                row.append(shoot(eye + target, target, 0.075, f"face_{vname}"))
        else:
            M = pose_arm(name, seconds)
            # The camera follows the body, so a walking frame is framed like a standing one.
            root = mv(M["Hips"].translation)
            for vname, eye in views:
                target, scale = bv(root.x, 0.17, root.z - 0.02), 0.85
                if vname == "face":
                    target = bv(*(mv(M["Head"].translation) + Vector((0.0, 0.0, 0.05))))
                    scale = 0.16
                elif vname == "front":
                    target, scale = bv(root.x, 0.17, root.z), 0.5
                row.append(shoot(eye + target - bv(0.0, 0.17, 0.0), target, scale,
                                 f"{name}_{seconds:.2f}_{vname}"))
        tiles.append(np.concatenate(row, axis=1))
    wide = max(t.shape[1] for t in tiles)
    tiles = [np.pad(t, ((0, 0), (0, wide - t.shape[1]), (0, 0))) for t in tiles]
    sheet = np.concatenate(tiles[::-1], axis=0)
    h, w = sheet.shape[:2]
    out = bpy.data.images.new("sheet", w, h, alpha=True)
    out.pixels.foreach_set(sheet.ravel())
    out.filepath_raw = os.path.join(OUT, "preview", "sheet.png")
    out.file_format = "PNG"
    out.save()
    print(f"preview: {out.filepath_raw}", flush=True)
    for pb in arm.pose.bones:
        pb.matrix_basis = Matrix.Identity(4)


# ---------------------------------------------------------------------------------------------


def args_after_dashes():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--clips", default="", help="comma-separated clips to key; all when empty")
    ap.add_argument("--preview", default="",
                    help="comma-separated name@seconds stills to render, 'rest' for the bind pose")
    ap.add_argument("--no-export", action="store_true", help="build and preview only")
    ap.add_argument("--resolution", type=float, default=0.0025, help="metaball grid, metres")
    ap.add_argument("--triangles", type=int, default=8000,
                    help="what the metaball mesh is collapsed to before its head is cut out; the "
                         "head and face end at HEAD_TRIANGLES and FACE_TRIANGLES whatever it is")
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    return ap.parse_args(argv)


def main():
    args = args_after_dashes()
    chosen = [c for c in args.clips.split(",") if c] or list(CLIPS)
    unknown = [c for c in chosen if c not in CLIPS]
    if unknown:
        sys.exit(f"no clip named {', '.join(unknown)}; have {', '.join(CLIPS)}")
    # A length between frames is keyed to the frame either side of it, and the file then says
    # one length while the header says another.
    ragged = [f"{c} ({CLIPS[c][0] * FPS:.2f} frames)" for c in chosen
              if abs(CLIPS[c][0] * FPS - round(CLIPS[c][0] * FPS)) > 1e-6]
    if ragged:
        sys.exit(f"not a whole number of frames at {FPS} fps: {', '.join(ragged)}")
    specs = [s for s in args.preview.split(",") if s]
    preview_names_ok(specs)
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = FPS
    mats = make_materials()
    body = build_body(args.resolution)
    eye_at = place_eyes(body)
    bones = skeleton(eye_at)
    arm = build_armature(bones)
    raw = sum(len(p.vertices) - 2 for p in body.data.polygons)
    decimate(body, args.triangles)
    body, graph, head_vertices = rebuild_body(body)
    for p in body.data.polygons:
        p.use_smooth = True
    face = Face(body)
    lip = measure_lip(face)
    assign_body_materials(body, mats, lip)
    parts = build_parts(mats, face, eye_at, lip)
    own = skin(body, parts, arm, graph, head_vertices, lip)
    bake_vertex_colours(body, own, eye_at, lip)
    tris = sum(len(p.vertices) - 2 for p in body.data.polygons)
    print(f"body: {raw} triangles from the metaballs, {tris} with the parts", flush=True)

    rig = Rig(arm, bones)
    clips = {}
    for name in chosen:
        seconds, looping, fn, events = CLIPS[name]
        travel = key_clip(arm, rig, name, seconds, fn)
        clips[name] = (seconds, looping, fn, events, travel)
        print(f"clip {name}: {seconds:.2f} s, travel {travel:.3f}", flush=True)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, "cat.blend"))

    if args.no_export:
        print("not exported", flush=True)
    elif chosen != list(CLIPS):
        # The header names every clip the game plays, so a file of some of them would leave it
        # one the game cannot build against.
        print(f"not exported: only {len(chosen)} of the {len(CLIPS)} clips were keyed", flush=True)
    else:
        export(GLB)
        self_check(GLB, clips)
        write_header(rig, clips)
        print(f"wrote {GLB} and {HEADER}", flush=True)
    # Last, since it leaves a camera, a world and render settings in the scene an export walks.
    if specs:
        preview(arm, rig, specs)


main()
