#!/usr/bin/env python3
"""Generate assets/skinned_cull_fixture.gltf -- a posed mesh is bounded by its POSE (spec 11.53).

The corpus had no asset that could show this. `skinned_instance_fixture` is
skinned but has no animation at all -- its joints sit at the origin with identity
inverse-bind matrices so the pose cannot drift, deliberately, because that
fixture is about which PROGRAM a mesh takes. A mesh that never leaves its bind
pose can never leave its bind bounds either, so nothing in the corpus could tell
a pose-aware bound from a bind-pose one.

This fixture is the opposite: one arm that swings a panel from OUTSIDE the left
frustum plane into full view, so the two bounds give different answers and the
difference is the whole frame.

  MUST NOT CULL. At the frame the gate reads, the panel is on screen but its
  BIND position is not. Culling on `mesh->aabb` -- which is what every skinned
  mesh would get if the pose were ignored -- drops it. Read as 0 px against
  --no-frustum-cull.

  MUST CULL. Aimed away, every mesh here has to be rejected. Before 11.53 a
  skinned mesh was DRAW_UNBOUNDED and none of them ever was.

WHY AN ANIMATION AND NOT A POSED BIND. The render app plays an embedded
animation automatically, and the pose has to be LIVE for `g_current_animation_state`
to be non-NULL -- which is the branch under test. Baking the swing into the bind
pose would test nothing: the import AABB would already contain it.

THE SWING IS A ROTATION ABOUT THE ORIGIN, so the panel's distance from the pivot
is what carries it, and the arm's geometry is derived from the camera below
rather than tuned by eye.

Regenerate with: python3 assets/gen_skinned_cull_fixture.py
"""

import base64
import json
import math
import os
import struct

FOV_DEG = 45.0
ASPECT = 4.0 / 3.0  # the gate renders 400x300
EYE_Z = 8.0

# Half-width of the frustum in the panel's plane (z = 0).
HALF_W = math.tan(math.radians(FOV_DEG) * 0.5) * EYE_Z * ASPECT

# The arm swings in the XY plane about the origin. At bind it points left, well
# outside the frustum; the animation brings it to vertical, which is centre
# frame. RADIUS is set from the frustum edge so "outside at bind" is a property
# of the geometry rather than a number that happened to work.
RADIUS = HALF_W * 1.45
PANEL_HALF_W = 0.55
PANEL_HALF_H = 0.55

# Bind pose: the panel hangs at (-RADIUS, 0). Its near edge is still outside.
assert -RADIUS + PANEL_HALF_W < -HALF_W, "panel is not outside the frustum at bind"

# The animation rotates -90 degrees about Z, which maps (x, y) to (y, -x) and so
# takes the panel from (-RADIUS, 0) to (0, RADIUS) -- straight up. The camera
# looks up at that; the point is only that the bind and posed positions are far
# apart and that the posed one is on screen.
CAM_TARGET_Y = RADIUS

# One joint at the origin. The panel's vertices are authored at the bind
# position, and the inverse bind matrix is the identity, so `bone` is exactly the
# animated rotation.
panel_pos = [(-RADIUS - PANEL_HALF_W, -PANEL_HALF_H, 0.0),
             (-RADIUS + PANEL_HALF_W, -PANEL_HALF_H, 0.0),
             (-RADIUS + PANEL_HALF_W, PANEL_HALF_H, 0.0),
             (-RADIUS - PANEL_HALF_W, PANEL_HALF_H, 0.0)]

BACK_X, BACK_LO, BACK_HI, BACK_Z = 14.0, -8.0, 14.0, -4.0
back_pos = [(-BACK_X, BACK_LO, BACK_Z), (BACK_X, BACK_LO, BACK_Z),
            (BACK_X, BACK_HI, BACK_Z), (-BACK_X, BACK_HI, BACK_Z)]

normals = [(0.0, 0.0, 1.0)] * 4
uv0 = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
indices = [0, 1, 2, 0, 2, 3]

# Every vertex fully weighted to joint 0: the bound has to follow one bone
# exactly, with no blend to soften a wrong answer.
joints = [(0, 0, 0, 0)] * 4
weights = [(1.0, 0.0, 0.0, 0.0)] * 4

# Identity inverse bind matrix, column-major.
ibm = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]

# The gate reads frame 30 of a fixed 1/60 clock, i.e. t = 0.5 s. The swing
# completes at 0.3 s and is then HELD to 4 s, so the arm reads a pose at rest
# rather than mid-sweep -- and, more importantly, nowhere near the loop point.
# The render app plays looping, so a clip that ended at 0.5 s would wrap to the
# bind pose in exactly the frame being measured.
_c, _s = math.cos(math.radians(45.0)), math.sin(math.radians(45.0))
UP = (0.0, 0.0, -_s, _c)  # -90 degrees about Z: (x, y) -> (y, -x)
TIMES = [0.0, 0.3, 4.0]
ROTATIONS = [(0.0, 0.0, 0.0, 1.0), UP, UP]

panel_b = b"".join(struct.pack("<3f", *p) for p in panel_pos)
back_b = b"".join(struct.pack("<3f", *p) for p in back_pos)
nrm_b = b"".join(struct.pack("<3f", *n) for n in normals)
uv0_b = b"".join(struct.pack("<2f", *t) for t in uv0)
joint_b = b"".join(struct.pack("<4H", *j) for j in joints)
weight_b = b"".join(struct.pack("<4f", *w) for w in weights)
ibm_b = struct.pack("<16f", *ibm)
time_b = b"".join(struct.pack("<f", t) for t in TIMES)
rot_b = b"".join(struct.pack("<4f", *r) for r in ROTATIONS)
idx_b = b"".join(struct.pack("<H", i) for i in indices)

_chunks = [(panel_b, 34962), (back_b, 34962), (nrm_b, 34962), (uv0_b, 34962),
           (joint_b, 34962), (weight_b, 34962), (ibm_b, None), (time_b, None),
           (rot_b, None), (idx_b, 34963)]
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


def _bounds(pts):
    return ([min(p[i] for p in pts) for i in range(3)],
            [max(p[i] for p in pts) for i in range(3)])


panel_mn, panel_mx = _bounds(panel_pos)
back_mn, back_mx = _bounds(back_pos)

PANEL_COLOR = [0.95, 0.8, 0.15, 1.0]
BACK_ALBEDO = [0.04, 0.04, 0.05, 1.0]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_skinned_cull_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1, 2]}],
    "nodes": [
        {"name": "backdrop", "mesh": 1},
        {"name": "swing_joint"},
        {"name": "swing_panel", "mesh": 0, "skin": 0},
    ],
    "skins": [{"inverseBindMatrices": 6, "joints": [1], "skeleton": 1}],
    "meshes": [
        {"name": "swing_panel",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 2, "TEXCOORD_0": 3,
                                        "JOINTS_0": 4, "WEIGHTS_0": 5},
                         "indices": 9, "material": 0}]},
        {"name": "backdrop",
         "primitives": [{"attributes": {"POSITION": 1, "NORMAL": 2, "TEXCOORD_0": 3},
                         "indices": 9, "material": 1}]},
    ],
    "animations": [{
        "name": "swing",
        "samplers": [{"input": 7, "output": 8, "interpolation": "LINEAR"}],
        "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}}],
    }],
    "materials": [
        {"name": "skin_cull_panel",
         "pbrMetallicRoughness": {"baseColorFactor": PANEL_COLOR, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.9}},
        {"name": "skin_cull_backdrop",
         "pbrMetallicRoughness": {"baseColorFactor": BACK_ALBEDO, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.9}},
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": panel_mn, "max": panel_mx},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": back_mn, "max": back_mx},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 4, "componentType": 5123, "count": 4, "type": "VEC4"},
        {"bufferView": 5, "componentType": 5126, "count": 4, "type": "VEC4"},
        {"bufferView": 6, "componentType": 5126, "count": 1, "type": "MAT4"},
        {"bufferView": 7, "componentType": 5126, "count": len(TIMES), "type": "SCALAR",
         "min": [min(TIMES)], "max": [max(TIMES)]},
        {"bufferView": 8, "componentType": 5126, "count": len(ROTATIONS), "type": "VEC4"},
        {"bufferView": 9, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": _views(_chunks),
    "buffers": [
        {"uri": "data:application/octet-stream;base64," +
                base64.b64encode(buffer_bytes).decode("ascii"),
         "byteLength": len(buffer_bytes)},
    ],
}

LIGHT = {"name": "SkinCullSun", "type": "directional", "direction": [0.0, -0.25, -0.97],
         "color": [1.0, 1.0, 1.0], "intensity": 3.0, "cast_shadows": False}
CAMERA = {"eye": [0.0, CAM_TARGET_Y, EYE_Z], "target": [0.0, CAM_TARGET_Y, 0.0],
          "fov": FOV_DEG}
POST = {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False,
        "bloom": {"enabled": False}}

cscn = {
    "version": 1,
    "models": [{"path": "skinned_cull_fixture.gltf"}],
    "lights": [LIGHT],
    "camera": CAMERA,
    "post": POST,
}

here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "skinned_cull_fixture.gltf"), "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
with open(os.path.join(here, "skinned_cull_fixture.cscn"), "w") as f:
    json.dump(cscn, f, indent=1)
    f.write("\n")

print(f"frustum half-width at z=0: {HALF_W:.4f}")
print(f"panel at bind spans x [{-RADIUS - PANEL_HALF_W:.4f}, {-RADIUS + PANEL_HALF_W:.4f}]")
print(f"posed it sits at y = {RADIUS:.4f}, which is where the camera looks")
