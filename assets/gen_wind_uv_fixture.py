#!/usr/bin/env python3
"""Generate assets/wind_uv_fixture.gltf -- UV1.y reaches the shader as flex (spec 11.51).

Three identical quads, side by side, differing ONLY in the TEXCOORD_1.y they
carry: 1.0, 0.5, 0.0. Under `windMode: "vegetation leaf"` that channel is a flex
weight -- a raw linear multiplier on the sway term -- so the quads must displace
1.0 : 0.5 : 0.0 of the flex-driven travel, and the differences between adjacent
quads must be equal.

WHY THIS EXISTS. The failure it guards is SILENT. A mesh that reaches the engine
without UV1 does not error: tex_coords2 is NULL, attribute 8 reads (0,0), and
every surface gets phase 0 and flex 0 -- which still leans, because the
height-mask body term carries no flex. The result is a stiff, uniformly-phased
canopy that looks like a plausible calm-day render rather than like a bug. Spec
11.51 measured the whole path precisely because nothing in the frame announces
when it breaks.

WHAT IT PINS, and what it deliberately does not. Between an authoring tool and
this shader sit three V-flips: the exporter writes v_gltf = 1 - v_blender, then
assimp's glTF2 importer flips every texcoord set back, then cetra's
aiProcess_FlipUVs flips them all again. The last two CANCEL, which is why the
shader sees the raw accessor value -- and that cancellation is the part cetra
owns and can regress. An assimp bump, or a change to uv_flip_flag(), would
invert the convention with nothing to catch it and every imported plant would
animate wrong. So this file writes glTF accessor values DIRECTLY rather than
going through Blender: the arm asserts "the accessor value is what flex means",
which is checkable in CI. The exporter's own flip is glTF-spec behaviour on the
far side of the file boundary and is recorded in the spec, not here.

Colour, not geometry, separates the quads for the reader. Each carries a
saturated primary so a channel-dominance test isolates it, which survives any
change to exposure, tonemap or the vignette -- the arm measures WHERE each quad
is, never how bright it is. The backdrop is deliberately NEUTRAL dark grey: a
tinted one would give some channel a majority and a quad's detector would start
matching the background it is supposed to be measured against.

The three quads must share their wind settings exactly, so the .cscn's material
entries are built from one dict rather than written out three times -- "only the
UV differs" is then structural instead of a promise maintained across three
literals.

Regenerate with: python3 assets/gen_wind_uv_fixture.py
"""

import base64
import json
import os
import struct

HALF_W = 0.5
HEIGHT = 2.0
SPACING = 1.7  # centre-to-centre; wide enough that no quad's travel reaches another

# The flex values, as glTF TEXCOORD_1.y accessor values -- i.e. exactly what the
# shader must see. Descending so the leftmost quad travels furthest, which makes
# a frame that has silently lost its UV1 (every quad identical) obvious by eye
# as well as to the arm.
FLEX = [1.0, 0.5, 0.0]
CHANNELS = ["red", "green", "blue"]
COLORS = [[0.85, 0.05, 0.05, 1.0], [0.05, 0.85, 0.05, 1.0], [0.05, 0.05, 0.85, 1.0]]

positions = [(-HALF_W, 0.0, 0.0), (HALF_W, 0.0, 0.0),
             (HALF_W, HEIGHT, 0.0), (-HALF_W, HEIGHT, 0.0)]
normals = [(0.0, 0.0, 1.0)] * 4
indices = [0, 1, 2, 0, 2, 3]
# UV0 is required: glTF wants contiguous TEXCOORD_n, and assimp skips TEXCOORD_1
# outright when TEXCOORD_0 is absent. It runs 0 at the base to 1 at the top,
# which is also the mode-2 flutter pivot -- inert here, since the fixture's wind
# authors turbulence 0 and the flutter term is the only consumer of it.
uv0 = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]

# Big enough to fill the frame behind the quads at this camera, so every pixel a
# detector might claim is either quad or a known neutral, never the clear colour.
BACK_X, BACK_LO, BACK_HI, BACK_Z = 6.0, -3.0, 5.0, -2.0
back_positions = [(-BACK_X, BACK_LO, BACK_Z), (BACK_X, BACK_LO, BACK_Z),
                  (BACK_X, BACK_HI, BACK_Z), (-BACK_X, BACK_HI, BACK_Z)]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
uv0_bytes = b"".join(struct.pack("<2f", *t) for t in uv0)
flex_bytes = [b"".join(struct.pack("<2f", 0.0, f) for _ in range(4)) for f in FLEX]
back_bytes = b"".join(struct.pack("<3f", *p) for p in back_positions)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)

_chunks = ([(pos_bytes, 34962), (nrm_bytes, 34962), (uv0_bytes, 34962)] +
           [(b, 34962) for b in flex_bytes] +
           [(back_bytes, 34962), (idx_bytes, 34963)])
buffer_bytes = b"".join(c for c, _ in _chunks)


def _views(chunks):
    """One bufferView per chunk, offsets accumulated rather than re-summed."""
    views, offset = [], 0
    for data, target in chunks:
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data),
                      "target": target})
        offset += len(data)
    return views


mn = [min(p[i] for p in positions) for i in range(3)]
mx = [max(p[i] for p in positions) for i in range(3)]
back_mn = [min(p[i] for p in back_positions) for i in range(3)]
back_mx = [max(p[i] for p in back_positions) for i in range(3)]

# Neutral, so no channel has a majority and no quad's dominance test can match
# it. Dark enough that the lit quads clear any sane floor.
BACK_ALBEDO = [0.05, 0.05, 0.05, 1.0]

# Accessor indices: 0 POSITION, 1 NORMAL, 2 TEXCOORD_0, 3..5 TEXCOORD_1 per
# quad, 6 backdrop POSITION, 7 indices.
FLEX_ACC = [3, 4, 5]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_wind_uv_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1, 2, 3]}],
    "nodes": (
        [{"name": "backdrop", "mesh": 3}] +
        [{"name": f"flex_{CHANNELS[i]}", "mesh": i,
          "translation": [(i - 1) * SPACING, 0.0, 0.0]} for i in range(3)]
    ),
    "meshes": (
        [{"name": f"flex_{CHANNELS[i]}",
          "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2,
                                         "TEXCOORD_1": FLEX_ACC[i]},
                          "indices": 7, "material": i}]} for i in range(3)] +
        [{"name": "backdrop",
          "primitives": [{"attributes": {"POSITION": 6, "NORMAL": 1}, "indices": 7,
                          "material": 3}]}]
    ),
    "materials": (
        [{"name": f"wind_uv_{CHANNELS[i]}",
          "pbrMetallicRoughness": {"baseColorFactor": COLORS[i], "metallicFactor": 0.0,
                                   "roughnessFactor": 0.9}} for i in range(3)] +
        [{"name": "wind_uv_backdrop",
          "pbrMetallicRoughness": {"baseColorFactor": BACK_ALBEDO, "metallicFactor": 0.0,
                                   "roughnessFactor": 0.9}}]
    ),
    "accessors": (
        [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
          "min": mn, "max": mx},
         {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
         {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"}] +
        [{"bufferView": 3 + i, "componentType": 5126, "count": 4, "type": "VEC2"}
         for i in range(3)] +
        [{"bufferView": 6, "componentType": 5126, "count": 4, "type": "VEC3",
          "min": back_mn, "max": back_mx},
         {"bufferView": 7, "componentType": 5123, "count": 6, "type": "SCALAR"}]
    ),
    "bufferViews": _views(_chunks),
    "buffers": [
        {"uri": "data:application/octet-stream;base64," +
                base64.b64encode(buffer_bytes).decode("ascii"),
         "byteLength": len(buffer_bytes)},
    ],
}

# One dict, three entries: the quads MUST agree on everything except the UV they
# carry, and writing this out three times is how that stops being true.
WIND_MATERIAL = {"windResponse": 1.0, "windMode": "vegetation leaf"}

# Directional and head-on: the quads are coplanar with one normal, so irradiance
# is identical across all three and a displacement cannot be confused with a
# shading difference.
LIGHT = {"name": "WindUvSun", "type": "directional", "direction": [0.0, -0.25, -0.97],
         "color": [1.0, 1.0, 1.0], "intensity": 3.0, "cast_shadows": False}
CAMERA = {"eye": [0.0, 1.0, 6.5], "target": [0.0, 1.0, 0.0], "fov": 45}
# Bloom off: a bleeding silhouette moves a centroid, and the centroid is the
# entire measurement here.
POST = {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False,
        "bloom": {"enabled": False}}

# speed x the gate's frame time puts the sway near its peak, so the arm reads the
# largest separation this wind can produce rather than whatever phase it landed
# on. gustAmount 0 pins the gust envelope to 1, and turbulence 0 removes BOTH
# turbulence terms -- the uniform lateral one and the uv0.y-weighted flutter --
# leaving the flex-weighted sway as the only thing that can differ between quads.
WIND = {"direction": [1.0, 0.0, 0.0], "strength": 0.6, "speed": 3.14159,
        "gustFrequency": 0.15, "gustAmount": 0.0, "turbulence": 0.0}


def _cscn(wind):
    return {
        "version": 1,
        "models": [{"path": "wind_uv_fixture.gltf"}],
        "lights": [LIGHT],
        "camera": CAMERA,
        "post": POST,
        "wind": wind,
        "materials": {f"wind_uv_{c}": dict(WIND_MATERIAL) for c in CHANNELS},
    }


here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "wind_uv_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")

# Two scene files rather than one plus a generated variant: wind has no CLI flag,
# so the still reference has to come from a file, and a committed one can be
# opened by hand to see what the arm is comparing against.
for name, wind in (("wind_uv_fixture.cscn", WIND),
                   ("wind_uv_fixture_still.cscn", dict(WIND, strength=0.0))):
    p = os.path.join(here, name)
    with open(p, "w") as f:
        json.dump(_cscn(wind), f, indent=1)
        f.write("\n")

print("wrote", out, "and both .cscn (flex", FLEX, "as glTF TEXCOORD_1.y)")
