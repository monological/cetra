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
import json
import math
import os
import struct
import sys

import bpy
import bmesh
from mathutils import Matrix, Quaternion, Vector
from mathutils.bvhtree import BVHTree

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


def _sided(name, parent, head, tail):
    """The left bone as stated and its mirror on the right."""
    out = []
    for side, s in (("L", 1.0), ("R", -1.0)):
        par = parent.replace(".L", f".{side}") if parent.endswith(".L") else parent
        out.append((f"{name}.{side}", par, (s * head[0], head[1], head[2]),
                    (s * tail[0], tail[1], tail[2])))
    return out


BONES = [
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
    ("Head", "Neck2", (0.0, 0.27, 0.18), (0.0, 0.285, 0.262)),
    ("Jaw", "Head", (0.0, 0.256, 0.21), (0.0, 0.248, 0.245)),
    *_sided("Ear", "Head", (0.03, 0.305, 0.2), (0.044, 0.344, 0.197)),
    *_sided("Eye", "Head", (0.022, 0.284, 0.246), (0.022, 0.284, 0.262)),
    *_sided("Lid", "Head", (0.022, 0.284, 0.246), (0.022, 0.284, 0.262)),
    # Legs lie in planes of constant x, so a roll of 0 puts every leg bone's X along the model's.
    *_sided("Shoulder", "Chest", (0.028, 0.24, 0.09), (0.042, 0.182, 0.118)),
    *_sided("UpperArm", "Shoulder.L", (0.045, 0.182, 0.118), (0.045, 0.108, 0.094)),
    *_sided("Forearm", "UpperArm.L", (0.045, 0.108, 0.094), (0.045, 0.03, 0.104)),
    *_sided("Hand", "Forearm.L", (0.045, 0.03, 0.104), (0.045, 0.008, 0.121)),
    *_sided("FrontToe", "Hand.L", (0.045, 0.008, 0.121), (0.045, 0.004, 0.143)),
    *_sided("Thigh", "Rump", (0.045, 0.198, -0.118), (0.045, 0.133, -0.063)),
    *_sided("Shin", "Thigh.L", (0.045, 0.133, -0.063), (0.045, 0.068, -0.15)),
    *_sided("Foot", "Shin.L", (0.045, 0.068, -0.15), (0.045, 0.008, -0.12)),
    *_sided("Toe", "Foot.L", (0.045, 0.008, -0.12), (0.045, 0.004, -0.097)),
    *_tail_chain(),
]

# Bones the heat weighting leaves alone: each carries a separate part weighted to it alone, and
# Hips carries travel, not flesh.
PART_BONES = {"Hips", "Jaw", "Ear.L", "Ear.R", "Eye.L", "Eye.R", "Lid.L", "Lid.R"}

# The legs: (leg, upper bone, lower bone, metapodial bone, toe bone).
LEGS = {
    "FL": ("UpperArm.L", "Forearm.L", "Hand.L", "FrontToe.L"),
    "FR": ("UpperArm.R", "Forearm.R", "Hand.R", "FrontToe.R"),
    "HL": ("Thigh.L", "Shin.L", "Foot.L", "Toe.L"),
    "HR": ("Thigh.R", "Shin.R", "Foot.R", "Toe.R"),
}

EYE_RADIUS = 0.0095   # the visible eye, about 15% over life so it reads at render scale 0.5
LID_CLOSE = 145.0     # degrees a lid turns from open (up and back inside the skull) to closed


# ---------------------------------------------------------------------------------------------
# Materials: linear colours. The game re-creates these with its own colours; these are what the
# render app shows.

def srgb(hexcode):
    c = [int(hexcode[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
    return tuple(x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c)


MATERIALS = {
    # name: (base colour, roughness, sheen tint or None)
    "cat_fur": (srgb("262424"), 0.65, (0.22, 0.22, 0.25)),
    "cat_eye": (srgb("E8B923"), 0.08, None),
    "cat_pupil": (srgb("050505"), 0.05, None),
    "cat_nose": (srgb("2B2225"), 0.45, None),
    "cat_ear": (srgb("3A2E31"), 0.6, None),
    # The lip line when the mouth is shut, and its inside when it opens.
    "cat_mouth": (srgb("5A2E36"), 0.5, None),
}

# The lower jaw is the body's own chin, below the mouth line, weighted wholly to Jaw: opening it
# stretches the faces along the line into the mouth.
MOUTH_Y = 0.2505


def in_jaw(p):
    """Whether a model-space point is part of the lower jaw."""
    return p.z > 0.228 and p.y < MOUTH_Y and abs(p.x) < 0.015


def make_materials():
    mats = {}
    for name, (col, rough, sheen) in MATERIALS.items():
        m = bpy.data.materials.new(name)
        # Single-sided: the glTF export calls a culled material single-sided, and a fold of skin
        # stretched open should show nothing behind it rather than its own lit inside.
        m.use_backface_culling = True
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


def build_armature():
    arm_data = bpy.data.armatures.new("cat_rig")
    arm = bpy.data.objects.new("cat", arm_data)
    bpy.context.scene.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    for name, parent, head, tail in BONES:
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
# sizes being the SURFACE the element would make alone; `calibrate` turns them into Blender's
# influence radii.
BODY = [
    # torso
    # The belly stops short of the thighs and rides above the knees, so a swinging hind leg
    # pulls no web of skin off it.
    ("ell", (0.0, 0.195, -0.115), (0.046, 0.052, 0.062)),   # haunches and pelvis
    ("ell", (0.0, 0.19, -0.012), (0.047, 0.052, 0.076)),    # belly
    ("ell", (0.0, 0.183, 0.068), (0.05, 0.066, 0.07)),      # chest
    ("ell", (0.0, 0.214, 0.158), (0.028, 0.034, 0.03)),     # throat
    ("ell", (0.04, 0.165, -0.1), (0.022, 0.052, 0.045)),    # left thigh muscle
    ("ell", (-0.04, 0.165, -0.1), (0.022, 0.052, 0.045)),
    ("ell", (0.036, 0.18, 0.098), (0.02, 0.048, 0.034)),    # left shoulder
    ("ell", (-0.036, 0.18, 0.098), (0.02, 0.048, 0.034)),
    ("ell", (0.0, 0.222, 0.095), (0.036, 0.03, 0.05)),      # withers
    # neck and head: a short thick neck, a round skull, a short muzzle
    ("cap", (0.0, 0.212, 0.1), (0.0, 0.252, 0.168), 0.038),
    ("ell", (0.0, 0.284, 0.21), (0.042, 0.038, 0.04)),      # skull
    ("ell", (0.026, 0.265, 0.222), (0.02, 0.018, 0.02)),    # cheeks, behind the eyes
    ("ell", (-0.026, 0.265, 0.222), (0.02, 0.018, 0.02)),
    ("ell", (0.0085, 0.258, 0.246), (0.011, 0.009, 0.011)),  # whisker pads
    ("ell", (-0.0085, 0.258, 0.246), (0.011, 0.009, 0.011)),
    ("ell", (0.0, 0.268, 0.244), (0.012, 0.012, 0.014)),    # the bridge of the nose
    ("ell", (0.0, 0.249, 0.238), (0.009, 0.007, 0.01)),     # chin
]


def _legs():
    out = []
    for s in (1.0, -1.0):
        x = 0.045 * s
        out += [
            ("cap", (x, 0.17, 0.11), (x, 0.108, 0.094), 0.018),     # upper arm
            ("cap", (x, 0.108, 0.094), (x, 0.03, 0.104), 0.012),    # forearm
            ("ell", (x, 0.012, 0.116), (0.015, 0.011, 0.021)),      # fore paw
            ("cap", (x, 0.165, -0.1), (x, 0.133, -0.063), 0.022),   # thigh
            ("cap", (x, 0.133, -0.063), (x, 0.068, -0.15), 0.015),  # shin
            ("cap", (x, 0.068, -0.15), (x, 0.013, -0.12), 0.0105),  # hock to paw
            ("ell", (x, 0.011, -0.11), (0.014, 0.011, 0.021)),      # hind paw
        ]
    return out


def _tail():
    out, n = [], 6
    for i, (_, _, head, tail) in enumerate(_tail_chain()):
        r = 0.0135 - 0.0045 * i / (n - 1)
        out.append(("cap", head, tail, r))
    return out


STIFFNESS = 2.0
THRESHOLD = 0.6
# The surface of a lone element, as a fraction of its influence radius, at that stiffness and
# threshold: Blender's field is s * (1 - d^2/R^2)^3, so the surface sits where that equals the
# threshold.
SURFACE = math.sqrt(1.0 - (THRESHOLD / STIFFNESS) ** (1.0 / 3.0))


def build_body(resolution):
    mb = bpy.data.metaballs.new("cat_body")
    mb.resolution = resolution
    mb.render_resolution = resolution
    mb.threshold = THRESHOLD
    for el in BODY + _legs() + _tail():
        if el[0] == "ell":
            _, c, semi = el
            e = mb.elements.new(type="ELLIPSOID")
            e.co = bv(*c)
            e.radius = 1.0 / SURFACE
            # Blender's sizes are in its own axes: model x, z and y are Blender x, y and z.
            e.size_x, e.size_y, e.size_z = semi[0], semi[2], semi[1]
        else:
            _, a, b, r = el
            a, b = bv(*a), bv(*b)
            e = mb.elements.new(type="CAPSULE")
            e.co = (a + b) / 2
            e.radius = r / SURFACE
            e.size_x = (b - a).length / 2
            e.rotation = Vector((1.0, 0.0, 0.0)).rotation_difference((b - a).normalized())
        e.stiffness = STIFFNESS
    obj = bpy.data.objects.new("cat_body_mb", mb)
    bpy.context.scene.collection.objects.link(obj)
    dg = bpy.context.evaluated_depsgraph_get()
    mesh = bpy.data.meshes.new_from_object(obj.evaluated_get(dg))
    bpy.data.objects.remove(obj)
    body = bpy.data.objects.new("cat_mesh", mesh)
    bpy.context.scene.collection.objects.link(body)
    return body


def in_head(p):
    return p.y > 0.235 and p.z > 0.175


# The face's share of the budget. Left to itself the collapse gives the head about a ninth of
# it, too few to carry a face, which is read at a few pixels and carries the expression where
# a flank is only a silhouette.
HEAD_TRIANGLES = 400


def triangle_count(obj, where=None):
    return sum(len(p.vertices) - 2 for p in obj.data.polygons
               if where is None or where(mv(p.center)))


def decimate(obj, triangles):
    """The body collapsed to its share with the face locked, then the face to its share with
    the body locked. Blender's weighted collapse locks a group outright at any strength rather
    than weighing it, so the split is two passes and not one."""
    body_share = triangles - HEAD_TRIANGLES
    collapse(obj, in_head, body_share + triangle_count(obj, in_head))
    collapse(obj, lambda p: not in_head(p), triangles)
    print(f"decimate: {triangle_count(obj)} triangles, {triangle_count(obj, in_head)} of them "
          f"on the head", flush=True)


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


def part_object(name, verts, faces, mats_per_face, bone, mats, outward_from=None):
    """A mesh from model-space vertices; faces index the vertices, each with a material name.
    Its faces are turned to face out -- away from `outward_from` for an open cap, by the
    mesh's own closure otherwise -- since every material is single-sided."""
    me = bpy.data.meshes.new(name)
    me.from_pydata([bv(*v) for v in verts], [], faces)
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


def placed(verts, origin, rotation=None):
    """Model-space vertices moved to `origin` after turning them by a model-space rotation
    (a Quaternion built by `rot`, which acts in Blender's space)."""
    out = []
    for v in verts:
        b = Vector((v[0], -v[2], v[1]))  # model -> Blender without the origin
        if rotation is not None:
            b = rotation @ b
        m = mv(b)
        out.append((m.x + origin[0], m.y + origin[1], m.z + origin[2]))
    return out


def build_parts(mats):
    parts = []
    for side, s in (("L", 1.0), ("R", -1.0)):
        eye = (0.022 * s, 0.284, 0.246)
        # The eyes look forward and a little out, as a cat's do.
        turn = rot(yaw=10.0 * s, pitch=-4.0)
        v, f = sphere_points(4, 10, EYE_RADIUS, (1.0, 0.92, 0.85), cap=math.pi * 0.55)
        parts.append(part_object(f"eye_{side}", placed(v, eye, turn), f, ["cat_eye"] * len(f),
                                 f"Eye.{side}", mats, outward_from=eye))
        # A wide night pupil: a vertical oval just proud of the eye's front.
        pv, pf = [], []
        n = 10
        front = EYE_RADIUS * 0.85 + 0.0006
        pv.append((0.0, 0.0, front))
        for k in range(n):
            a = 2.0 * math.pi * k / n
            pv.append((0.0042 * math.cos(a), 0.0068 * math.sin(a), front - 0.0012))
        for k in range(n):
            pf.append((0, 1 + k, 1 + (k + 1) % n))
        parts.append(part_object(f"pupil_{side}", placed(pv, eye, turn), pf,
                                 ["cat_pupil"] * len(pf), f"Eye.{side}", mats, outward_from=eye))
        # The lid: a cap a little larger than the eye, bound OPEN, turned up inside the skull.
        # Wider than the eye's visible cap, so shut it covers the whole eye; open, its edge
        # shows over the top of the eye as the upper lid.
        lv, lf = sphere_points(3, 10, EYE_RADIUS * 1.08, (1.05, 1.0, 0.95), cap=math.pi * 0.58)
        open_turn = turn @ rot(pitch=LID_CLOSE)
        parts.append(part_object(f"lid_{side}", placed(lv, eye, open_turn), lf,
                                 ["cat_fur"] * len(lf), f"Lid.{side}", mats, outward_from=eye))
        # The ear: a broad thin pyramid on the skull's shoulder, its inner face toward the front.
        verts = [
            (0.05 * s, 0.287, 0.208),   # front outer
            (0.012 * s, 0.312, 0.213),  # front inner
            (0.032 * s, 0.302, 0.186),  # back
            (0.046 * s, 0.346, 0.197),  # tip
            (0.032 * s, 0.296, 0.2),    # inside the head, closing the base
        ]
        faces = [(0, 1, 3), (1, 2, 3), (2, 0, 3), (0, 4, 1), (1, 4, 2), (2, 4, 0)]
        if s < 0:
            faces = [tuple(reversed(f_)) for f_ in faces]
        parts.append(part_object(f"ear_{side}", verts, faces,
                                 ["cat_ear", "cat_fur", "cat_fur", "cat_fur", "cat_fur", "cat_fur"],
                                 f"Ear.{side}", mats))
    return parts


def assign_body_materials(body, mats):
    """Fur everywhere, the nose leather on the muzzle's tip, pads under the paws, and the mouth
    along the line where the jaw parts from the head."""
    me = body.data
    order = ["cat_fur", "cat_nose", "cat_mouth"]
    for n in order:
        me.materials.append(mats[n])
    front = max(mv(v.co).z for v in me.vertices if abs(mv(v.co).x) < 0.01 and mv(v.co).y > 0.24)
    for p in me.polygons:
        c = mv(p.center)
        jaw = [in_jaw(mv(me.vertices[i].co)) for i in p.vertices]
        idx = 0
        if c.z > front - 0.006 and c.y > MOUTH_Y + 0.004 and abs(c.x) < 0.009:
            idx = 1
        elif c.y < 0.004:
            idx = 1
        elif any(jaw) and not all(jaw) and c.z > 0.238:
            # Only along the front: the jaw region's back edge crosses the throat, where the
            # same rule would draw a collar.
            idx = 2
        p.material_index = idx


# ---------------------------------------------------------------------------------------------
# Skinning


def skin(body, parts, arm):
    for b in arm.data.bones:
        b.use_deform = b.name not in PART_BONES
    bpy.ops.object.select_all(action="DESELECT")
    body.select_set(True)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.parent_set(type="ARMATURE_AUTO")
    for b in arm.data.bones:
        b.use_deform = True
    clean_weights(body, arm)
    # The chin below the mouth line goes wholly to the jaw.
    jaw = body.vertex_groups.new(name="Jaw")
    for v in body.data.vertices:
        if in_jaw(mv(v.co)):
            for g in list(v.groups):
                body.vertex_groups[g.group].remove([v.index])
            jaw.add([v.index], 1.0, "REPLACE")
    # The parts join the body: their one vertex group each comes along by name.
    bpy.ops.object.select_all(action="DESELECT")
    for p in parts:
        p.select_set(True)
    body.select_set(True)
    bpy.context.view_layer.objects.active = body
    bpy.ops.object.join()
    limit_weights(body, 4)
    for mod in body.modifiers:
        if mod.type == "ARMATURE":
            mod.object = arm
    if not any(m.type == "ARMATURE" for m in body.modifiers):
        mod = body.modifiers.new("armature", "ARMATURE")
        mod.object = arm
    body.parent = arm


# How far from its bone a bone may still move the skin, metres. Heat weighting diffuses: left
# alone it gives a shin a third of the flank above the knee, and every stride drags the flank
# with it into a spike.
REACH = {"Thigh": 0.05, "Shin": 0.03, "Foot": 0.025, "Toe": 0.02, "Shoulder": 0.06,
         "UpperArm": 0.04, "Forearm": 0.025, "Hand": 0.022, "FrontToe": 0.02, "Tail": 0.03}
LIMBS = {"Thigh", "Shin", "Foot", "Toe", "UpperArm", "Forearm", "Hand", "FrontToe"}


def segment_distance(p, a, b):
    ab = b - a
    t = max(0.0, min(1.0, (p - a).dot(ab) / max(ab.dot(ab), 1e-12)))
    return (p - (a + ab * t)).length


def clean_weights(body, arm):
    """Heat weighting's influences held to where each bone can move the skin, smoothed over the
    surface, and held to the rules again so the smoothing cannot carry an influence back where
    it was taken from. Every rule is a smooth function of position: a hard cut is worse than the
    diffusion it cures, since neighbours left on different bones tear apart at the first stride."""
    me = body.data
    bones = {b.name: (mv(b.head_local), mv(b.tail_local)) for b in arm.data.bones}
    names = {vg.index: vg.name for vg in body.vertex_groups}
    points = [mv(v.co) for v in me.vertices]
    knee, elbow = bones["Shin.L"][0].y, bones["Forearm.L"][0].y

    def allowed(name, p):
        if name not in bones:
            return 0.0
        family = name.split(".")[0].rstrip("0123456789")
        d = segment_distance(p, *bones[name])
        f = max(0.0, min(1.0, 2.0 - d / REACH.get(family, 1.0)))
        if family in LIMBS:
            # The belly and the chest between the legs belong to the body: a groin weighted to
            # a shin tears open with every stride.
            f *= smooth((abs(p.x) - 0.012) / 0.02)
        if family in ("Shin", "Foot", "Toe"):
            f *= smooth((knee + 0.008 - p.y) / 0.02)    # nothing above the knee
        if family in ("Forearm", "Hand", "FrontToe"):
            f *= smooth((elbow + 0.008 - p.y) / 0.02)   # nothing above the elbow
        return f

    def held(weights):
        out = []
        for p, w in zip(points, weights):
            kept = {gi: x * allowed(names[gi], p) for gi, x in w.items()}
            if sum(kept.values()) <= 0.0:
                best = min(w, key=lambda gi: segment_distance(p, *bones[names[gi]])
                           if names[gi] in bones else 1e9)
                kept = {best: 1.0}
            out.append(normalised(kept))
        return out

    weights = held([{g.group: g.weight for g in v.groups if g.weight > 0.0} for v in me.vertices])
    # Smoothed over the surface: each pass moves a vertex half way to its neighbours' mean.
    near = [[] for _ in me.vertices]
    for e in me.edges:
        a, b = e.vertices
        near[a].append(b)
        near[b].append(a)
    for _ in range(3):
        mixed = []
        for i, w in enumerate(weights):
            mean = {}
            for n in near[i]:
                for gi, x in weights[n].items():
                    mean[gi] = mean.get(gi, 0.0) + x / len(near[i])
            keys = set(w) | set(mean)
            mixed.append(normalised({k: 0.5 * w.get(k, 0.0) + 0.5 * mean.get(k, 0.0) for k in keys}))
        weights = mixed
    weights = held(weights)
    for vg in body.vertex_groups:
        vg.remove(list(range(len(me.vertices))))
    for i, w in enumerate(weights):
        for gi, x in w.items():
            if x > 1e-4:
                body.vertex_groups[gi].add([i], x, "REPLACE")


def normalised(w):
    total = sum(w.values())
    return {k: x / total for k, x in w.items()} if total > 0.0 else w


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
        sys.exit(f"{len(empty)} vertices carry no weight; the heat weighting failed")


# ---------------------------------------------------------------------------------------------
# Ambient occlusion in the vertex colours, so a fur of any colour still shows its form


def bake_occlusion(obj, rays=48, reach=0.12):
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
    part_groups = {obj.vertex_groups[n].index for n in PART_BONES if n in obj.vertex_groups}
    ao = []
    for v in me.vertices:
        if any(g.group in part_groups and g.weight > 0.5 for g in v.groups):
            ao.append(1.0)
            continue
        n = v.normal
        frame = n.to_track_quat("Z", "Y")
        hit = 0
        for d in dirs:
            w = frame @ d
            loc, *_ = tree.ray_cast(v.co + n * 0.0015, w, reach)
            if loc is not None:
                hit += 1
        ao.append(1.0 - hit / rays)
    attr = me.color_attributes.new("Col", "BYTE_COLOR", "POINT")
    for i, a in enumerate(ao):
        c = 0.72 + 0.28 * a
        attr.data[i].color = (c, c, c, 1.0)
    me.color_attributes.active_color = attr


# ---------------------------------------------------------------------------------------------
# Posing


class Rig:
    """The armature's rest frames, and the pose they produce from a Pose's parameters."""

    def __init__(self, arm):
        self.arm = arm
        self.rest = {b.name: b.matrix_local.copy() for b in arm.data.bones}
        self.parent = {b.name: (b.parent.name if b.parent else None) for b in arm.data.bones}
        self.length = {b.name: b.length for b in arm.data.bones}
        self.order = [b[0] for b in BONES]
        self.rest_rel = {}
        for n in self.order:
            p = self.parent[n]
            self.rest_rel[n] = (self.rest[p].inverted() @ self.rest[n]) if p else self.rest[n]

    def rest_point(self, bone, end="tail"):
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
            if n in ("Shoulder.L", "Shoulder.R", "Rump"):
                self._legs_from(n, pose, M, D)
        return M

    def _legs_from(self, girdle, pose, M, D):
        legs = ("FL",) if girdle == "Shoulder.L" else ("FR",) if girdle == "Shoulder.R" else ("HL", "HR")
        for leg in legs:
            upper, lower, meta, toe = LEGS[leg]
            L = pose.legs[leg]
            A = (M[girdle] @ self.rest_rel[upper]).translation
            # A paw stands at a model-space point, or rides its girdle -- the chest for a fore
            # paw, the pelvis for a hind -- when the body bends round it; `rel` blends the two.
            body = "Chest" if leg[0] == "F" else "Rump"
            rel = L["rel"]
            turned = D[body]
            model_fwd = Quaternion(AY, math.radians(pose.root_yaw)) @ AZ
            fwd = model_fwd.lerp(turned @ AZ, rel).normalized()
            paw = bv(*L["paw"]).lerp(M[body].translation + turned @ bv(*L["paw_rel"]), rel)
            up = AY.lerp(turned @ AY, rel).normalized()
            a = math.radians(L["meta"])
            m_dir = (up * math.cos(a) - fwd * math.sin(a)).normalized()
            side = Quaternion(fwd, math.radians(L.get("meta_side", 0.0)))
            m_dir = side @ m_dir
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

    ik_bones = {b for leg in LEGS.values() for b in leg}

    def __init__(self, rig):
        self.root = Vector((0.0, 0.0, 0.0))
        self.root_yaw = 0.0
        self.rump = Vector((0.0, 0.0, 0.0))
        self.rot = {}
        self.legs = {}
        for leg, (upper, lower, meta, toe) in LEGS.items():
            P = rig.rest_point(meta, "tail")
            C = rig.rest_point(meta, "head")
            m = C - P
            tip = rig.rest_point(toe, "tail") - P
            body = rig.rest_point("Chest" if leg[0] == "F" else "Rump", "head")
            self.legs[leg] = {
                "paw": (P.x, P.y, P.z),
                "paw_rel": tuple(P - body),  # from the girdle, in its turned frame
                "rel": 0.0,
                "meta": math.degrees(math.atan2(-m.z, m.y)),
                "toe": math.degrees(math.atan2(tip.y, tip.z)),
                "pole": (0.0, 0.3, 1.0) if leg[0] == "H" else (0.0, 0.1, -1.0),
            }

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

    def copy(self, rig):
        p = Pose(rig)
        p.root, p.root_yaw, p.rump = self.root.copy(), self.root_yaw, self.rump.copy()
        p.rot = dict(self.rot)
        p.legs = {k: dict(v) for k, v in self.legs.items()}
        return p


def lerp_pose(rig, a, b, t):
    """Every parameter of two poses mixed by t: what a transition is between its ends."""
    p = Pose(rig)
    p.root = a.root.lerp(b.root, t)
    p.root_yaw = a.root_yaw + (b.root_yaw - a.root_yaw) * t
    p.rump = a.rump.lerp(b.rump, t)
    for n in set(a.rot) | set(b.rot):
        ra, rb = a.rot.get(n, (0.0, 0.0, 0.0)), b.rot.get(n, (0.0, 0.0, 0.0))
        p.rot[n] = tuple(x + (y - x) * t for x, y in zip(ra, rb))
    for leg in LEGS:
        la, lb = a.legs[leg], b.legs[leg]
        p.legs[leg] = {k: _mix(la[k], lb[k], t) for k in la}
    return p


def _mix(x, y, t):
    if isinstance(x, tuple):
        return tuple(u + (v - u) * t for u, v in zip(x, y))
    return x + (y - x) * t


def smooth(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


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
    p.rot["Shoulder.L"] = (-30.0, 0.0, 0.0)
    p.rot["Shoulder.R"] = (-30.0, 0.0, 0.0)
    for leg, x in (("FL", 0.03), ("FR", -0.03)):
        p.legs[leg].update(paw=(x, 0.008, 0.075), meta=22.0, pole=(0.0, 0.0, -1.0))
    for leg, x in (("HL", 0.05), ("HR", -0.05)):
        p.legs[leg].update(paw=(x, 0.008, 0.0), meta=88.0, toe=-4.0, pole=(0.0, 1.0, 0.8))
    # The tail lies along the floor and curls round the front paws.
    tail = [(-55.0, 18.0, 0.0), (-8.0, 26.0, 0.0), (6.0, 30.0, 0.0), (6.0, 34.0, 0.0),
            (4.0, 34.0, 0.0), (6.0, 30.0, 0.0)]
    for i, r in enumerate(tail):
        p.rot[f"Tail{i + 1}"] = r
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
    p.rot["Shoulder.L"] = (-30.0, 0.0, 0.0)
    p.rot["Shoulder.R"] = (-30.0, 0.0, 0.0)
    for leg, x in (("FL", 0.032), ("FR", -0.032)):
        p.legs[leg].update(paw=(x, 0.009, 0.2), meta=86.0, toe=-6.0, pole=(0.0, -1.0, -0.6))
    for leg, x in (("HL", 0.055), ("HR", -0.055)):
        p.legs[leg].update(paw=(x, 0.009, -0.035), meta=90.0, toe=-4.0, pole=(0.0, 1.0, 0.8))
    tail = [(-30.0, 14.0, 0.0), (-6.0, 20.0, 0.0), (2.0, 22.0, 0.0), (2.0, 24.0, 0.0),
            (2.0, 22.0, 0.0), (4.0, 18.0, 0.0)]
    for i, r in enumerate(tail):
        p.rot[f"Tail{i + 1}"] = r
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
    p.rot["Shoulder.L"] = (-40.0, 0.0, 0.0)
    p.rot["Shoulder.R"] = (-40.0, 0.0, 0.0)
    # The legs fold against the belly, inside the ring, posed in the turned body's own frame.
    for leg, x in (("FL", 0.03), ("FR", -0.03)):
        p.legs[leg].update(rel=1.0, paw_rel=(x, -0.11, 0.06), meta=140.0, toe=-30.0,
                           pole=(0.0, -0.3, -1.0))
    for leg, x in (("HL", 0.05), ("HR", -0.05)):
        p.legs[leg].update(rel=1.0, paw_rel=(x, -0.11, 0.07), meta=95.0, toe=-6.0,
                           pole=(0.0, 1.0, 1.0))
    # The tail curls the same way as the spine, round the outside to the nose.
    for i, pitch in enumerate((10.0, 30.0, 35.0, 35.0, 32.0, 25.0)):
        p.rot[f"Tail{i + 1}"] = (pitch, 0.0, 0.0)
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


def tail_wave(p, t, period, amp, lag=0.6, start=1):
    for i in range(start, 7):
        k = (i - start + 1) / (7 - start)
        add_rot(p, f"Tail{i}", yaw=amp * k * math.sin(2.0 * math.pi * t / period - lag * i))


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


WALK_STRIDE = 0.44
WALK_SECONDS = 0.80
DUTY = 0.62
# Lateral sequence: left hind, left fore, right hind, right fore, a quarter cycle apart; the
# first footfall a frame in, so no event sits on the loop's seam.
FOOTFALL = {"HL": 0.025, "FL": 0.275, "HR": 0.525, "FR": 0.775}


def gait_paw(rig, leg, s, stride, duty, lift):
    """Where a paw is at cycle position s (cycles since the start) for a body moving `stride`
    a cycle: planted through its stance, carried along an arc through its swing. Also how far
    through the swing it is, -1 in stance."""
    base = Pose(rig).legs[leg]["paw"]
    tau = s - FOOTFALL[leg]
    n = math.floor(tau)
    u = tau - n
    z0 = base[2] + stride * (n + FOOTFALL[leg] + duty / 2.0)
    if u < duty:
        return (base[0], base[1], z0), -1.0
    w = (u - duty) / (1.0 - duty)
    z = z0 + stride * smooth(w)
    y = base[1] + lift * math.sin(math.pi * w) ** 1.5
    return (base[0], y, z), w


def clip_walk(rig, t):
    p = stand(rig)
    T, stride = WALK_SECONDS, WALK_STRIDE
    s = t / T
    p.root = Vector((0.0, 0.0, stride * s))
    for leg in LEGS:
        paw, w = gait_paw(rig, leg, s, stride, DUTY, 0.035 if leg[0] == "F" else 0.03)
        # Paws are planted in model space; the body travels over them.
        p.legs[leg]["paw"] = paw
        rest = p.legs[leg]["meta"]
        if w < 0.0:
            # Through the stance the metapodial rolls forward over the paw.
            tau = (s - FOOTFALL[leg]) % 1.0
            p.legs[leg]["meta"] = rest + (tau / DUTY - 0.5) * (20.0 if leg[0] == "H" else 14.0)
        else:
            # Through the swing the paw folds back and opens again before it lands.
            fold = math.sin(math.pi * w)
            p.legs[leg]["meta"] = rest + (60.0 if leg[0] == "F" else 35.0) * fold
            p.legs[leg]["toe"] -= 25.0 * fold
    # Two small rises a cycle, a roll that follows the hind legs, the spine swinging with them.
    p.rump = Vector((0.0, 0.004 * math.cos(4.0 * math.pi * (s - 0.15)), 0.0))
    add_rot(p, "Rump", roll=2.0 * math.sin(2.0 * math.pi * (s - 0.1)), yaw=2.5 * math.sin(2.0 * math.pi * s))
    add_rot(p, "Spine2", yaw=-2.0 * math.sin(2.0 * math.pi * s))
    add_rot(p, "Chest", roll=-1.5 * math.sin(2.0 * math.pi * (s - 0.35)), yaw=-1.5 * math.sin(2.0 * math.pi * s))
    # The head stays level and pointed ahead while everything under it moves.
    add_rot(p, "Neck1", pitch=-3.0)
    add_rot(p, "Head", yaw=1.5 * math.sin(2.0 * math.pi * s), pitch=1.0 * math.cos(4.0 * math.pi * s))
    # A low tail, swaying.
    add_rot(p, "Tail1", pitch=-8.0)
    tail_wave(p, t, T, 5.0)
    return p


def clip_sit_down(rig, t):
    T = 0.8
    u = t / T
    a, b = stand(rig), sit(rig)
    # The rump goes down first, the front legs straighten as it does.
    p = lerp_pose(rig, a, b, smooth(u))
    rump = smooth(min(1.0, u * 1.25))
    p.rump = a.rump.lerp(b.rump, rump)
    p.rot["Rump"] = tuple(x * rump for x in b.rot["Rump"])
    return p


def clip_stand_up(rig, t):
    return clip_sit_down(rig, 0.8 - t)


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


def clip_lie_down(rig, t):
    """Sit to sphinx: the chest lowers while the front paws walk forward, left then right."""
    T = 1.0
    u = t / T
    a, b = sit(rig), sphinx(rig)
    p = lerp_pose(rig, a, b, smooth(u))
    for leg, start in (("FL", 0.15), ("FR", 0.38)):
        w = window(u, start, start + 0.3)
        pa, pb = a.legs[leg]["paw"], b.legs[leg]["paw"]
        p.legs[leg]["paw"] = (pa[0] + (pb[0] - pa[0]) * w,
                              pa[1] + (pb[1] - pa[1]) * w + 0.03 * math.sin(math.pi * w),
                              pa[2] + (pb[2] - pa[2]) * w)
    return p


def clip_sit_up(rig, t):
    return clip_lie_down(rig, 1.0 - t)


def clip_lie(rig, t):
    p = sphinx(rig)
    breathe(p, t, 2.5, 1.2)
    e = bump(t, 3.5, 0.5)
    add_rot(p, "Tail5", yaw=20.0 * e)
    add_rot(p, "Tail6", yaw=30.0 * e)
    add_rot(p, "Head", yaw=5.0 * math.sin(2.0 * math.pi * t / 5.0))
    return p


def clip_curl_up(rig, t):
    T = 1.6
    u = t / T
    p = lerp_pose(rig, sphinx(rig), curl(rig), smooth(u))
    lids(p, window(t, 1.15, 1.4))
    return p


def clip_uncurl(rig, t):
    return clip_curl_up(rig, 1.6 - t)


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
    e = bump(t, 0.25, 0.4)
    add_rot(p, "Ear.L", yaw=-14.0 * e)
    add_rot(p, "Ear.R", yaw=14.0 * e)
    return p


def clip_meow_long(rig, t):
    p = stand(rig)
    add_rot(p, "Jaw", pitch=-jaw_open(t, [(0.25, 18.0, 0.22), (0.62, 28.0, 0.42)]))
    e = window(t, 0.05, 0.3) * (1.0 - window(t, 0.8, 1.05))
    add_rot(p, "Ear.L", yaw=-16.0 * e)
    add_rot(p, "Ear.R", yaw=16.0 * e)
    return p


def clip_blink(rig, t):
    p = stand(rig)
    lids(p, math.sin(math.pi * min(t / 0.25, 1.0)))
    return p


# name: (seconds, looping, pose function, events [(name, seconds)])
CLIPS = {
    "idle": (4.0, True, clip_idle, []),
    "walk": (WALK_SECONDS, True, clip_walk,
             [(f"paw_{k.lower()[1]}{k.lower()[0]}", FOOTFALL[k] * WALK_SECONDS)
              for k in ("HL", "FL", "HR", "FR")]),
    "sit_down": (0.8, False, clip_sit_down, []),
    "stand_up": (0.8, False, clip_stand_up, []),
    "sit": (6.0, True, clip_sit, []),
    "lie_down": (1.0, False, clip_lie_down, [("paw_lf", 0.45), ("paw_rf", 0.68)]),
    "sit_up": (1.0, False, clip_sit_up, []),
    "lie": (5.0, True, clip_lie, []),
    "curl_up": (1.6, False, clip_curl_up, []),
    "uncurl": (1.6, False, clip_uncurl, []),
    "sleep": (6.0, True, clip_sleep, []),
    "meow_short": (0.6, False, clip_meow_short, [("meow", 0.08)]),
    "meow_long": (1.1, False, clip_meow_long, [("meow", 0.08)]),
    "blink": (0.25, False, clip_blink, []),
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
    return act


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


def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67:
        sys.exit(f"{path} is not a GLB")
    off, js, binary = 12, None, None
    while off < length:
        clen, ctype = struct.unpack_from("<II", data, off)
        chunk = data[off + 8: off + 8 + clen]
        if ctype == 0x4E4F534A:
            js = json.loads(chunk)
        elif ctype == 0x004E4942:
            binary = chunk
        off += 8 + clen
    return js, binary


def accessor(js, binary, index):
    acc = js["accessors"][index]
    view = js["bufferViews"][acc["bufferView"]]
    comps = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[acc["type"]]
    if acc["componentType"] != 5126:
        sys.exit("self-check reads float accessors only")
    start = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = view.get("byteStride", comps * 4)
    return [struct.unpack_from(f"<{comps}f", binary, start + i * stride) for i in range(acc["count"])]


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
    problems, travel = [], {}
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
        travel[name] = d
        stated = clips[name][4]
        if abs(d[2] - stated) > 1e-3 or abs(d[0]) > 1e-3:
            problems.append(f"{name}: Hips travels {d}, stated {stated}")
    if problems:
        sys.exit("self-check:\n  " + "\n  ".join(problems))
    tris = 0
    for m in js["meshes"]:
        for prim in m["primitives"]:
            tris += js["accessors"][prim["indices"]]["count"] // 3
    print(f"self-check: {len(joints)} joints, {len(clips)} clips, {tris} triangles, "
          f"{os.path.getsize(path) / 1024:.0f} KiB", flush=True)
    return tris


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
        "    CatClipEvent events[6];",
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


def preview(arm, rig, specs, width=420):
    """Workbench stills of `name@seconds` poses from the side, the front and three-quarters."""
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
    os.makedirs(os.path.join(OUT, "preview"), exist_ok=True)
    tiles = []
    for spec in specs:
        name, _, at = spec.partition("@")
        seconds = float(at or 0.0)
        fn = CLIPS[name][2] if name in CLIPS else None
        pose = fn(rig, seconds) if fn else Pose(rig)
        M = rig.solve(pose)
        basis = pose_to_basis(rig, M)
        for n, b in basis.items():
            arm.pose.bones[n].matrix_basis = b
        bpy.context.view_layer.update()
        # The camera follows the body, so a walking frame is framed like a standing one.
        root = mv(M["Hips"].translation)
        row = []
        for vname, eye in views:
            target = bv(root.x, 0.17, root.z - 0.02)
            cam_data.ortho_scale = 0.85
            if vname == "face":
                target = bv(*(mv(M["Head"].translation) + Vector((0.0, 0.0, 0.05))))
                cam_data.ortho_scale = 0.16
            elif vname == "front":
                target = bv(root.x, 0.17, root.z)
                cam_data.ortho_scale = 0.5
            cam.location = eye + target - bv(0.0, 0.17, 0.0)
            cam.rotation_euler = (target - cam.location).to_track_quat("-Z", "Y").to_euler()
            file = os.path.join(OUT, "preview", f"{name}_{seconds:.2f}_{vname}.png")
            scene.render.filepath = file
            bpy.ops.render.render(write_still=True)
            img = bpy.data.images.load(file)
            px = np.array(img.pixels[:], dtype=np.float32).reshape(img.size[1], img.size[0], 4)
            bpy.data.images.remove(img)
            row.append(px)
        tiles.append(np.concatenate(row, axis=1))
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
    ap.add_argument("--resolution", type=float, default=0.004, help="metaball grid, metres")
    ap.add_argument("--triangles", type=int, default=1600, help="the body's budget after decimation")
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    return ap.parse_args(argv)


def main():
    args = args_after_dashes()
    os.makedirs(OUT, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = FPS
    mats = make_materials()
    arm = build_armature()
    body = build_body(args.resolution)
    raw = sum(len(p.vertices) - 2 for p in body.data.polygons)
    lo = [min(mv(v.co)[k] for v in body.data.vertices) for k in range(3)]
    hi = [max(mv(v.co)[k] for v in body.data.vertices) for k in range(3)]
    print("body bounds: " + ", ".join(f"{a:.3f}..{b:.3f}" for a, b in zip(lo, hi)), flush=True)
    decimate(body, args.triangles)
    for p in body.data.polygons:
        p.use_smooth = True
    assign_body_materials(body, mats)
    parts = build_parts(mats)
    skin(body, parts, arm)
    bake_occlusion(body)
    tris = sum(len(p.vertices) - 2 for p in body.data.polygons)
    print(f"body: {raw} triangles from the metaballs, {tris} after decimation and parts", flush=True)

    rig = Rig(arm)
    chosen = [c for c in args.clips.split(",") if c] or list(CLIPS)
    unknown = [c for c in chosen if c not in CLIPS]
    if unknown:
        sys.exit(f"no clip named {', '.join(unknown)}; have {', '.join(CLIPS)}")
    clips = {}
    for name in chosen:
        seconds, looping, fn, events = CLIPS[name]
        key_clip(arm, rig, name, seconds, fn)
        travel = (mv(rig.solve(fn(rig, seconds))["Hips"].translation)
                  - mv(rig.solve(fn(rig, 0.0))["Hips"].translation)).z
        clips[name] = (seconds, looping, fn, events, travel)
        print(f"clip {name}: {seconds:.2f} s, travel {travel:.3f}", flush=True)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, "cat.blend"))

    if args.preview:
        preview(arm, rig, [s for s in args.preview.split(",") if s])
    if args.no_export:
        return
    export(GLB)
    self_check(GLB, clips)
    write_header(rig, clips)
    print(f"wrote {GLB} and {HEADER}", flush=True)


main()
