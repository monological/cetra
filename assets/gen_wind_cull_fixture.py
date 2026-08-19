#!/usr/bin/env python3
"""Generate assets/wind_cull_fixture.gltf -- a wind mesh is cullable, and its margin is right (spec 11.53).

Two questions in one frame, and they fail in opposite directions, which is the
whole point of putting them together.

  MUST CULL. Three wind quads sit behind the camera. Before 11.53 a wind
  material was DRAW_UNBOUNDED and every one of these was submitted every frame,
  by the camera pass and by every shadow cascade. The arm reads `meshes culled`
  and wants exactly 3.

  MUST NOT CULL. One wind quad sits just outside the LEFT frustum plane at bind
  and is displaced back into view by the wind. Culling it on its import AABB
  would drop geometry that is on screen -- which is what makes this the arm that
  can only be written wrong once. It is read as 0 px against --no-frustum-cull.

A margin that is too small fails the second arm; no margin at all (the old
exemption) fails the first. Nothing passes both by accident, and neither arm can
be satisfied by a build where the feature did not run.

THE GEOMETRY IS DERIVED, NOT EYEBALLED. The camera's half-width at the marginal
quad's depth is computed here from the same fov and aspect the .cscn and the
gate render at, so the quad's offset is stated as a fraction of the frustum edge
rather than as a number that happened to work. Change the camera and the quad
follows it; change it far enough and the assertion below fails loudly instead of
the fixture silently ceasing to test anything.

WIND MODE 0, deliberately. Cloth needs no UV1, so this fixture tests the margin
without also depending on the flex path -- wind_uv_fixture already owns that.
Mode 0 also displaces along +X ONLY (`sway` is 0..1, never negative), so the
travel direction is known rather than oscillating through zero.

Regenerate with: python3 assets/gen_wind_cull_fixture.py
"""

import base64
import json
import math
import os
import struct

# --- camera, and the frustum edge it implies ---------------------------------
FOV_DEG = 45.0
ASPECT = 4.0 / 3.0  # the gate renders 400x300
EYE_Z = 8.0
QUAD_Z = 0.0  # the marginal quad's plane

# Half-width of the view frustum at the marginal quad's depth.
_half_h = math.tan(math.radians(FOV_DEG) * 0.5) * (EYE_Z - QUAD_Z)
HALF_W_AT_QUAD = _half_h * ASPECT

QUAD_HALF = 0.3
QUAD_H = 1.6

# The wind field's own speed, needed here as well as in the .cscn because the
# vegetation quad's placement depends on the phase the gate reads at.
WIND_SPEED = math.pi

# windOffset's fixed amplitude coefficients, from cetra/shaders/include/
# wind_bounds.glsl. C and GLSL share that file by #include; Python cannot, so
# these are a THIRD copy and the only defence is that they are named the same
# and used for the same arithmetic. Anything changed there has to change here,
# and the asserts below are what notices -- a quad that stops being marginal
# fails them loudly rather than silently testing nothing.
WIND_LEAN_COEFF = 0.6   # WIND_VEG_LEAN
WIND_SWAY_COEFF = 0.5   # WIND_VEG_SWAY
WIND_TURB_COEFF = 0.25  # WIND_VEG_TURB
WIND_LEAF_DIR = math.sqrt(1.0 + 0.4 * 0.4 + 0.6 * 0.6)  # |vec3(1, Y, -Z)|
SQRT2 = math.sqrt(2.0)

WIND_STRENGTH = 1.2
WIND_RESPONSE = 1.0
WIND_TURBULENCE = 0.35

# Travel available to a CLOTH hem: strength x response, with the gust envelope
# pinned to 1 by gustAmount 0. This mirrors the mode-0 branch of wind.c's
# wind_max_offset minus its turbulence term -- deliberately, because that term
# is LATERAL (it can push -x as easily as +x) and this number is used to place a
# quad the wind has to carry INWARD. Under-stating it is the safe direction.
TRAVEL = WIND_STRENGTH * WIND_RESPONSE

# The quad's near edge sits this far OUTSIDE the frustum. A fraction of the
# travel rather than an absolute distance: it has to be far enough out that a
# cull on the import AABB rejects it, and near enough in that the wind carries
# it back past the plane by a visible margin.
OUTSIDE_BY = TRAVEL * 0.35
MARGINAL_X = -(HALF_W_AT_QUAD + OUTSIDE_BY + QUAD_HALF)

# The two properties the arms rest on, asserted here rather than hoped for.
assert MARGINAL_X + QUAD_HALF < -HALF_W_AT_QUAD, "marginal quad is not outside the frustum at bind"
assert MARGINAL_X + QUAD_HALF + TRAVEL > -HALF_W_AT_QUAD + 0.2, \
    "wind does not carry the marginal quad far enough inside to be measurable"

# --- and the same again on the VEGETATION path -------------------------------
# Without this quad the fixture tests one multiply. `turbulence` is 0 and the
# mode defaults to 0, so wind_max_offset's mode-0 branch collapses to
# strength x response and NONE of wind_bounds.glsl's six coefficients, neither
# wind_flex_max nor wind_leaf_max, and none of the vegetation arithmetic affects
# the value under test -- which is the branch apps/forest ships on.
#
# So: a second quad on the far side of the same frustum edge, windMode
# "vegetation leaf", carrying UV1, with turbulence on. Its inward travel is the
# whole-body LEAN, the one vegetation term that cannot go negative along the
# wind (`0.5 + 0.5*sin` is never below 0), evaluated at the phase the gate reads.
# The sway, turbulence and leaf-flutter terms ride on top and are bounded by
# VEG_FLEX_MAX, which is held low enough that they cannot cancel the lean.
# Flat across the quad, not a ramp: UV0.y here runs 1 at the base and 0 at the
# top, so a flex that ramped WITH height would meet uv0.y = 0 at every vertex
# where it was non-zero and the leaf term's max |uv1.y * uv0.y| would come out 0
# -- inert, which is the thing this quad exists to stop. A constant flex makes
# both wind_flex_max and wind_leaf_max non-zero. The flex RAMP is
# wind_uv_fixture's subject, not this one's.
VEG_FLEX_MAX = 0.2

# The lean's own envelope at the read frame. speed x t with t = 0.5 s, which is
# where render() lands at its default 30 frames on a fixed 1/60 clock.
_lean_phase = math.sin(WIND_SPEED * 0.5 * 0.35)
VEG_LEAN = (0.5 + 0.5 * _lean_phase) * WIND_LEAN_COEFF

# What the lean alone is guaranteed to move the quad's TOP edge, less the worst
# the flex-weighted terms can take back. Mirrors wind.c's vegetation branch; the
# coefficients are the same ones wind_bounds.glsl holds, restated here because a
# Python generator cannot include a GLSL header.
_veg_takeback = VEG_FLEX_MAX * (WIND_SWAY_COEFF + SQRT2 * WIND_TURB_COEFF * WIND_TURBULENCE
                                + WIND_LEAF_DIR * WIND_TURBULENCE)
VEG_TRAVEL = WIND_STRENGTH * WIND_RESPONSE * (VEG_LEAN - _veg_takeback)
assert VEG_TRAVEL > 0.0, "the flex-weighted terms can cancel the lean; lower VEG_FLEX_MAX"

VEG_OUTSIDE_BY = VEG_TRAVEL * 0.3
VEG_X = -(HALF_W_AT_QUAD + VEG_OUTSIDE_BY + QUAD_HALF)
assert VEG_X + QUAD_HALF < -HALF_W_AT_QUAD, "vegetation quad is not outside the frustum at bind"
assert VEG_X + QUAD_HALF + VEG_TRAVEL > -HALF_W_AT_QUAD + 0.1, \
    "wind does not carry the vegetation quad far enough inside to be measurable"

# Behind the camera, so no framing choice can bring them back. Three of them
# because one culled mesh is indistinguishable from an off-by-one.
BEHIND_Z = EYE_Z + 12.0
BEHIND_X = [-2.0, 0.0, 2.0]

BACK_X, BACK_LO, BACK_HI, BACK_Z = 9.0, -5.0, 5.0, -3.0


def quad(half_w, lo_y, hi_y, z):
    return [(-half_w, lo_y, z), (half_w, lo_y, z), (half_w, hi_y, z), (-half_w, hi_y, z)]


marginal_pos = quad(QUAD_HALF, 0.0, QUAD_H, 0.0)
behind_pos = quad(QUAD_HALF, 0.0, QUAD_H, 0.0)
back_pos = quad(BACK_X, BACK_LO, BACK_HI, BACK_Z)

normals = [(0.0, 0.0, 1.0)] * 4
# UV0 is required: glTF wants contiguous TEXCOORD_n and assimp drops later sets
# without it. Inert under mode 0, which reads neither UV set.
uv0 = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
indices = [0, 1, 2, 0, 2, 3]

veg_pos = quad(QUAD_HALF, 0.0, QUAD_H, 0.0)
# Constant, so both maxima the vegetation bound reads are non-zero. See
# VEG_FLEX_MAX for why a ramp would not be.
veg_uv1 = [(0.0, VEG_FLEX_MAX)] * 4

pos_b = b"".join(struct.pack("<3f", *p) for p in marginal_pos)
behind_b = b"".join(struct.pack("<3f", *p) for p in behind_pos)
back_b = b"".join(struct.pack("<3f", *p) for p in back_pos)
nrm_b = b"".join(struct.pack("<3f", *n) for n in normals)
uv0_b = b"".join(struct.pack("<2f", *t) for t in uv0)
veg_uv1_b = b"".join(struct.pack("<2f", *t) for t in veg_uv1)
idx_b = b"".join(struct.pack("<H", i) for i in indices)

_chunks = [(pos_b, 34962), (behind_b, 34962), (back_b, 34962), (nrm_b, 34962),
           (uv0_b, 34962), (veg_uv1_b, 34962), (idx_b, 34963)]
buffer_bytes = b"".join(c for c, _ in _chunks)


def _views(chunks):
    views, offset = [], 0
    for data, target in chunks:
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data),
                      "target": target})
        offset += len(data)
    return views


def _bounds(pts):
    return ([min(p[i] for p in pts) for i in range(3)],
            [max(p[i] for p in pts) for i in range(3)])


marg_mn, marg_mx = _bounds(marginal_pos)
behind_mn, behind_mx = _bounds(behind_pos)
back_mn, back_mx = _bounds(back_pos)

# Bright against a dark backdrop: the second arm compares whole frames, so the
# marginal quad has to be a large enough share of the image that losing it
# cannot read as noise.
MARGINAL_COLOR = [0.9, 0.75, 0.1, 1.0]
BEHIND_COLOR = [0.8, 0.2, 0.2, 1.0]
VEG_COLOR = [0.25, 0.85, 0.2, 1.0]
BACK_ALBEDO = [0.04, 0.04, 0.05, 1.0]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_wind_cull_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1, 2, 3, 4, 5]}],
    "nodes": (
        [{"name": "backdrop", "mesh": 2},
         {"name": "wind_marginal", "mesh": 0, "translation": [MARGINAL_X, 0.0, QUAD_Z]}] +
        [{"name": f"wind_behind_{i}", "mesh": 1,
          "translation": [BEHIND_X[i], 0.0, BEHIND_Z]} for i in range(3)] +
        [{"name": "wind_vegetation", "mesh": 3,
          "translation": [VEG_X, 0.0, QUAD_Z]}]
    ),
    "meshes": [
        {"name": "wind_marginal",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 3, "TEXCOORD_0": 4},
                         "indices": 6, "material": 0}]},
        {"name": "wind_behind",
         "primitives": [{"attributes": {"POSITION": 1, "NORMAL": 3, "TEXCOORD_0": 4},
                         "indices": 6, "material": 1}]},
        {"name": "backdrop",
         "primitives": [{"attributes": {"POSITION": 2, "NORMAL": 3, "TEXCOORD_0": 4},
                         "indices": 6, "material": 2}]},
        {"name": "wind_vegetation",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 3, "TEXCOORD_0": 4,
                                        "TEXCOORD_1": 5},
                         "indices": 6, "material": 3}]},
    ],
    "materials": [
        {"name": "cull_marginal",
         "pbrMetallicRoughness": {"baseColorFactor": MARGINAL_COLOR, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.9}},
        {"name": "cull_behind",
         "pbrMetallicRoughness": {"baseColorFactor": BEHIND_COLOR, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.9}},
        {"name": "cull_backdrop",
         "pbrMetallicRoughness": {"baseColorFactor": BACK_ALBEDO, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.9}},
        {"name": "cull_vegetation",
         "pbrMetallicRoughness": {"baseColorFactor": VEG_COLOR, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.9}},
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": marg_mn, "max": marg_mx},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": behind_mn, "max": behind_mx},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": back_mn, "max": back_mx},
        {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 5, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 6, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": _views(_chunks),
    "buffers": [
        {"uri": "data:application/octet-stream;base64," +
                base64.b64encode(buffer_bytes).decode("ascii"),
         "byteLength": len(buffer_bytes)},
    ],
}

# speed x the gate's frame time (30 frames at a fixed 1/60) puts sin at its peak,
# so the hem is at full travel in the frame the arm reads rather than at whatever
# phase it landed on. gustAmount 0 pins the envelope to 1 and turbulence 0
# removes the lateral terms, leaving one displacement along +X.
WIND = {"direction": [1.0, 0.0, 0.0], "strength": WIND_STRENGTH, "speed": WIND_SPEED,
        "gustFrequency": 0.15, "gustAmount": 0.0, "turbulence": WIND_TURBULENCE}

# Casts, so the frame carries a shadow-cascade row as well as an opaque one and
# the group can ask whether the two passes agree about what is bounded. They cull
# against different volumes and must both reject the quads behind the camera.
LIGHT = {"name": "CullSun", "type": "directional", "direction": [0.0, -0.25, -0.97],
         "color": [1.0, 1.0, 1.0], "intensity": 3.0, "cast_shadows": True}
CAMERA = {"eye": [0.0, 1.0, EYE_Z], "target": [0.0, 1.0, 0.0], "fov": FOV_DEG}
# Bloom off: a bleeding silhouette would spread the marginal quad's pixels over
# the frame and blunt the comparison that is supposed to notice it vanishing.
POST = {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False,
        "bloom": {"enabled": False}}

cscn = {
    "version": 1,
    "models": [{"path": "wind_cull_fixture.gltf"}],
    "lights": [LIGHT],
    "camera": CAMERA,
    "post": POST,
    "wind": WIND,
    "materials": {
        "cull_marginal": {"windResponse": WIND_RESPONSE},
        "cull_behind": {"windResponse": WIND_RESPONSE},
        "cull_vegetation": {"windResponse": WIND_RESPONSE,
                            "windMode": "vegetation leaf"},
    },
}

here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "wind_cull_fixture.gltf"), "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
with open(os.path.join(here, "wind_cull_fixture.cscn"), "w") as f:
    json.dump(cscn, f, indent=1)
    f.write("\n")

print(f"frustum half-width at the quad plane: {HALF_W_AT_QUAD:.4f}")
print(f"vegetation quad spans x [{VEG_X - QUAD_HALF:.4f}, {VEG_X + QUAD_HALF:.4f}] "
      f"at bind, and the lean reaches {VEG_X + QUAD_HALF + VEG_TRAVEL:.4f}")
print(f"marginal quad spans x [{MARGINAL_X - QUAD_HALF:.4f}, {MARGINAL_X + QUAD_HALF:.4f}] "
      f"at bind, and the hem reaches {MARGINAL_X + QUAD_HALF + TRAVEL:.4f}")
