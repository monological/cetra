#!/usr/bin/env python3
"""Generate assets/shadow_lag_fixture.gltf -- a caster whose shadow must not trail it (spec 11.96).

Nothing in the corpus could show this. Every golden is a static scene under a
static camera, so a shadow drawn from LAST frame's transforms is pixel-identical
to one drawn from this frame's, and the whole 29-fixture suite is 0 px against a
build with the bug in it. The two things in the tree that move and cast -- the
wind quads and the abandoned-window curtains -- move by vertex displacement in
the shader, which was never stale.

TWO PROBLEMS SHAPE THIS FIXTURE, and both have the same answer.

CONSTANT VELOCITY CANNOT SHOW A LAG. A shadow one frame behind a body moving at a
constant rate is displaced by exactly one frame's travel -- and so is a correct
shadow between consecutive frames. Comparing frames measures the same number
either way. So the caster HOLDS STILL and then starts, and the arm reads the
onset: on the first moved frame a correct shadow has moved and a lagging one has
not.

THE BODY MUST NOT BE VISIBLE. If the caster is in frame, its own movement dirties
the comparison and the arm has to separate two regions. So the sun is overhead,
the caster floats above the camera, and the camera looks down at bare ground: the
ONLY thing in the frame that can change is the shadow. That turns the read into a
plain whole-frame compare with no projector, no threshold and no region mask.

  shadow-lag-still   frames 14 and 15 are identical. The ramp has not started, so
                     nothing in the scene moves. Without it the arm below could
                     be reading noise.
  shadow-lag-tracks  frames 15 and 16 DIFFER. Frame 16 is the first with the
                     caster displaced; a shadow drawn from frame 15's transforms
                     is identical to frame 15 and the arm reads 0.

WHY AN ANIMATION AND NOT APP CODE. A .cscn cannot express a moving object at all,
and a schedule flag driven from the render app's per-frame update hook would run
BEFORE the shadow pass -- so the fixture would be green against the very build it
exists to catch. The motion has to be authored so that PROPAGATION is the step
under test.

WHY A SKINNED CLIP. The importer resolves every animation channel against the
SKELETON: a node-TRS animation on an unskinned mesh has no path into this engine.
One joint, identity inverse bind, every vertex fully weighted to it.

Regenerate with: python3 assets/gen_shadow_lag_fixture.py
"""

import base64
import json
import math
import os
import struct

FOV_DEG = 40.0

# The camera looks DOWN at the ground from above. The caster floats higher than
# the camera, so it is outside the frustum no matter how wide the lens: the top
# of a downward-tilted frustum still points below horizontal.
CAM_Y, CAM_Z = 3.0, 6.0
CASTER_Y = 3.5
assert CASTER_Y > CAM_Y, "caster must sit above the camera or it lands in frame"

# Down and very slightly along -Z. Exactly vertical is avoided because it makes
# the light basis degenerate; the tilt is small enough that the shadow lands
# essentially under the caster.
SUN_TILT = 0.05
SUN_DIR = [0.0, -1.0, -SUN_TILT]

GROUND_HALF = 14.0
CASTER_HALF = 1.6

# Hold, then ramp. HOLD_T lands exactly on a frame boundary of the fixed 1/60
# headless clock, so the still frames and the first moved frame are unambiguous.
HOLD_T = 0.25
HOLD_FRAME = int(round(HOLD_T * 60.0))
END_T = 1.25
TRAVEL_X = 8.0

# Per-frame travel once moving, in world units.
STEP = TRAVEL_X / (END_T - HOLD_T) / 60.0

# The shadow map is 2048 across an ortho box this scene comfortably fits in 30
# units of. A step smaller than a few texels would let a lagging shadow land on
# the same texels as a tracking one and the arm would read 0 for the wrong
# reason -- which is the failure mode a culling-style arm has, and the reason
# this assert is here rather than a comment.
TEXELS_PER_UNIT = 2048.0 / 30.0
assert STEP * TEXELS_PER_UNIT > 4.0, (
    f"one frame of travel is {STEP * TEXELS_PER_UNIT:.2f} shadow texels; too few to read")

# The clip runs well past the frames the arm samples, so the render app's looping
# playback cannot wrap to the bind pose inside the read window -- the trap
# gen_skinned_cull_fixture.py records.
TIMES = [0.0, HOLD_T, END_T, 6.0]
TRANSLATIONS = [(0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (TRAVEL_X, 0.0, 0.0), (TRAVEL_X, 0.0, 0.0)]
assert TIMES[-1] / 1.0 > 4.0, "clip must outlast the sampled window by a margin"

# The ground is a quad in XZ. The caster is a closed BOX and not a quad, which
# is the one non-obvious thing here: the depth pass culls a face, so a
# zero-thickness plane writes no depth from the light and casts nothing at all.
# A box has a back face whichever one is dropped. Every existing shadow fixture
# uses closed volumes and never had to discover this.
CASTER_HALF_Y = 0.4
ground_pos = [(-GROUND_HALF, 0.0, -GROUND_HALF), (GROUND_HALF, 0.0, -GROUND_HALF),
              (GROUND_HALF, 0.0, GROUND_HALF), (-GROUND_HALF, 0.0, GROUND_HALF)]

_lo = (-CASTER_HALF, CASTER_Y - CASTER_HALF_Y, -CASTER_HALF)
_hi = (CASTER_HALF, CASTER_Y + CASTER_HALF_Y, CASTER_HALF)
caster_pos = [(x, y, z) for y in (_lo[1], _hi[1]) for z in (_lo[2], _hi[2])
              for x in (_lo[0], _hi[0])]
# WINDING IS NOT IRRELEVANT HERE, which is the trap this box already fell into.
# A first draft emitted all six faces in one index order; three came out wound
# inward, were backface-culled, and left two VERTICAL quads -- whose shadow under
# a near-vertical sun is a line rather than a rectangle. It rendered as two thin
# lines and read as a shadow-map problem.
#
# So the winding is asserted rather than trusted: every triangle's normal must
# point away from the box centre.
_F = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
caster_idx = []
for a, b, c, d in _F:
    caster_idx += [a, b, c, a, c, d]

_centre = tuple((_lo[i] + _hi[i]) * 0.5 for i in range(3))
for _t in range(0, len(caster_idx), 3):
    _p = [caster_pos[caster_idx[_t + k]] for k in range(3)]
    _u = [_p[1][i] - _p[0][i] for i in range(3)]
    _v = [_p[2][i] - _p[0][i] for i in range(3)]
    _n = (_u[1] * _v[2] - _u[2] * _v[1], _u[2] * _v[0] - _u[0] * _v[2],
          _u[0] * _v[1] - _u[1] * _v[0])
    _out = [_p[0][i] - _centre[i] for i in range(3)]
    assert sum(_n[i] * _out[i] for i in range(3)) > 0.0, (
        f"caster triangle {_t // 3} is wound inward; it will be culled and cast nothing")

ground_nrm = [(0.0, 1.0, 0.0)] * 4
ground_uv = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
# Wound so the face normal is +Y. The obvious 0,1,2 / 0,2,3 over these corners
# gives -Y, which a camera above never sees -- the plane is culled and the frame
# is empty background. Worth stating because an empty frame reads as "the shadow
# did not render" and sends you looking at the shadow pass.
ground_idx = [0, 2, 1, 0, 3, 2]

caster_nrm = [(0.0, 1.0, 0.0)] * 8
caster_uv = [(0.0, 0.0)] * 8
joints = [(0, 0, 0, 0)] * 8
weights = [(1.0, 0.0, 0.0, 0.0)] * 8
ibm = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]

ground_b = b"".join(struct.pack("<3f", *p) for p in ground_pos)
caster_b = b"".join(struct.pack("<3f", *p) for p in caster_pos)
gnrm_b = b"".join(struct.pack("<3f", *n) for n in ground_nrm)
guv_b = b"".join(struct.pack("<2f", *t) for t in ground_uv)
cnrm_b = b"".join(struct.pack("<3f", *n) for n in caster_nrm)
cuv_b = b"".join(struct.pack("<2f", *t) for t in caster_uv)
joint_b = b"".join(struct.pack("<4H", *j) for j in joints)
weight_b = b"".join(struct.pack("<4f", *w) for w in weights)
ibm_b = struct.pack("<16f", *ibm)
time_b = b"".join(struct.pack("<f", t) for t in TIMES)
trans_b = b"".join(struct.pack("<3f", *t) for t in TRANSLATIONS)
gidx_b = b"".join(struct.pack("<H", i) for i in ground_idx)
cidx_b = b"".join(struct.pack("<H", i) for i in caster_idx)

_chunks = [(ground_b, 34962), (caster_b, 34962), (gnrm_b, 34962), (guv_b, 34962),
           (cnrm_b, 34962), (cuv_b, 34962), (joint_b, 34962), (weight_b, 34962),
           (ibm_b, None), (time_b, None), (trans_b, None),
           (gidx_b, 34963), (cidx_b, 34963)]
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


ground_mn, ground_mx = _bounds(ground_pos)
caster_mn, caster_mx = _bounds(caster_pos)

GROUND_ALBEDO = [0.72, 0.70, 0.66, 1.0]
CASTER_ALBEDO = [0.15, 0.15, 0.18, 1.0]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_shadow_lag_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1, 2]}],
    "nodes": [
        {"name": "ground", "mesh": 1},
        {"name": "slide_joint"},
        {"name": "slide_caster", "mesh": 0, "skin": 0},
    ],
    "skins": [{"inverseBindMatrices": 8, "joints": [1], "skeleton": 1}],
    "meshes": [
        {"name": "slide_caster",
         "primitives": [{"attributes": {"POSITION": 1, "NORMAL": 4, "TEXCOORD_0": 5,
                                        "JOINTS_0": 6, "WEIGHTS_0": 7},
                         "indices": 12, "material": 1}]},
        {"name": "ground",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 2, "TEXCOORD_0": 3},
                         "indices": 11, "material": 0}]},
    ],
    "animations": [{
        "name": "slide",
        "samplers": [{"input": 9, "output": 10, "interpolation": "LINEAR"}],
        "channels": [{"sampler": 0, "target": {"node": 1, "path": "translation"}}],
    }],
    "materials": [
        {"name": "shadow_lag_ground",
         "pbrMetallicRoughness": {"baseColorFactor": GROUND_ALBEDO, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.95}},
        {"name": "shadow_lag_caster",
         "pbrMetallicRoughness": {"baseColorFactor": CASTER_ALBEDO, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.9}},
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": ground_mn, "max": ground_mx},
        {"bufferView": 1, "componentType": 5126, "count": 8, "type": "VEC3",
         "min": caster_mn, "max": caster_mx},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 4, "componentType": 5126, "count": 8, "type": "VEC3"},
        {"bufferView": 5, "componentType": 5126, "count": 8, "type": "VEC2"},
        {"bufferView": 6, "componentType": 5123, "count": 8, "type": "VEC4"},
        {"bufferView": 7, "componentType": 5126, "count": 8, "type": "VEC4"},
        {"bufferView": 8, "componentType": 5126, "count": 1, "type": "MAT4"},
        {"bufferView": 9, "componentType": 5126, "count": len(TIMES), "type": "SCALAR",
         "min": [min(TIMES)], "max": [max(TIMES)]},
        {"bufferView": 10, "componentType": 5126, "count": len(TRANSLATIONS), "type": "VEC3"},
        {"bufferView": 11, "componentType": 5123, "count": len(ground_idx), "type": "SCALAR"},
        {"bufferView": 12, "componentType": 5123, "count": len(caster_idx), "type": "SCALAR"},
    ],
    "bufferViews": _views(_chunks),
    "buffers": [
        {"uri": "data:application/octet-stream;base64," +
                base64.b64encode(buffer_bytes).decode("ascii"),
         "byteLength": len(buffer_bytes)},
    ],
}

LIGHT = {"name": "ShadowLagSun", "type": "directional", "direction": SUN_DIR,
         "color": [1.0, 1.0, 1.0], "intensity": 3.0, "cast_shadows": True}
CAMERA = {"eye": [0.0, CAM_Y, CAM_Z], "target": [0.0, 0.0, 0.0], "fov": FOV_DEG}
POST = {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False,
        "bloom": {"enabled": False}}

cscn = {
    "version": 1,
    "models": [{"path": "shadow_lag_fixture.gltf"}],
    "lights": [LIGHT],
    "camera": CAMERA,
    "post": POST,
}

# THE STILL TWIN: the caster parked at the origin, so the arm can read the
# shadow's position ABSOLUTELY rather than as a change between frames.
#
# Comparing consecutive frames of the moving scene also works, but only while the
# motion onset falls exactly between the two sampled frames -- retime the clip or
# change the frame rate and it quietly stops testing anything. Against the twin,
# a correct shadow has left the parked position at the first displaced frame and
# a lagging one is still on it, whatever the sampling.
#
# Same pattern as wind_uv_fixture's _still twin, and for the same reason: one
# frame of the moving scene cannot substitute for a reference.
still_gltf = json.loads(json.dumps(gltf))
_still_trans = b"".join(struct.pack("<3f", 0.0, 0.0, 0.0) for _ in TRANSLATIONS)
_still_bytes = buffer_bytes.replace(trans_b, _still_trans)
assert _still_bytes != buffer_bytes and len(_still_bytes) == len(buffer_bytes), (
    "the still twin's translation block did not substitute cleanly")
still_gltf["buffers"][0]["uri"] = ("data:application/octet-stream;base64," +
                                   base64.b64encode(_still_bytes).decode("ascii"))
still_gltf["asset"]["generator"] = "gen_shadow_lag_fixture.py (still twin)"

still_cscn = json.loads(json.dumps(cscn))
still_cscn["models"] = [{"path": "shadow_lag_still.gltf"}]

here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "shadow_lag_fixture.gltf"), "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
with open(os.path.join(here, "shadow_lag_fixture.cscn"), "w") as f:
    json.dump(cscn, f, indent=1)
    f.write("\n")
with open(os.path.join(here, "shadow_lag_still.gltf"), "w") as f:
    json.dump(still_gltf, f, indent=1)
    f.write("\n")
with open(os.path.join(here, "shadow_lag_still.cscn"), "w") as f:
    json.dump(still_cscn, f, indent=1)
    f.write("\n")

print(f"hold until t={HOLD_T} (frame {HOLD_FRAME}), then {TRAVEL_X} units by t={END_T}")
print(f"one frame of travel: {STEP:.4f} units = {STEP * TEXELS_PER_UNIT:.1f} shadow texels")
print(f"caster at y={CASTER_Y} against a camera at y={CAM_Y}: out of frame by construction")
