#!/usr/bin/env python3
"""Generate assets/area_shadow_fixture.gltf -- the area-shadow instrument (spec 10.4).

Every other area-light asset in the repo tests SHADING. This one tests the shadow
PROJECTION, and it is the only fixture whose correct answer is known in advance
rather than eyeballed against a stored image.

The geometry is chosen so the penumbra has a closed form. A square panel of
half-width r at height H, a square occluder of half-width a at height h, and a
ground plane at y=0. The shadow of the occluder's edge, cast by a point of the
panel at horizontal offset s, lands at

    x(s) = s + (a - s) * H / (H - h)

so sweeping s over [-r, r] sweeps the edge over a band whose width is

    W = 2 * r * h / (H - h)

independent of the occluder's size. With H=3, h=1, r=0.3, a=0.5 that is a full
umbra out to |x| = 0.6, a penumbra from 0.6 to 0.9, and unoccluded ground beyond
-- a 0.3-unit band with round endpoints, measurable off a scanline.

Both limits are worth knowing, because they are what the measurement is FOR:
h -> 0 (occluder resting on the ground) gives W -> 0, a perfectly sharp contact;
h -> H (occluder against the panel) gives W -> infinity. A filter that is sized
by anything other than the source cannot reproduce both, which is exactly the
mistake this fixture exists to catch.

The panel is authored in the sibling .cscn pointing straight DOWN. That is
deliberate and load-bearing: a laterally offset panel still shades incorrectly
(spec 10.3, Open 1), and an overhead panel is the configuration verified to
light the top of an object rather than its bottom. The fixture must not depend
on the open bug.

No environment and no second light, so the umbra is genuinely black and the
transition spans the full available range.

Regenerate with:
  python3 assets/gen_area_shadow_fixture.py
(the .cscn is hand-authored, not generated.)
"""

import base64
import json
import os
import struct

GROUND = 4.0      # ground half-extent; wide enough that the lit region frames the band
OCCLUDER_A = 0.5  # occluder half-width  (a)
OCCLUDER_H = 1.0  # occluder height      (h)

positions, normals, uvs, indices = [], [], [], []


def add_quad(y, half, base_index_list):
    """A horizontal quad in XZ at height `y`, facing +Y.

    The index order is (0,2,1)/(0,3,2), not the (0,1,2)/(0,2,3) that reads as
    natural for this corner list: with +X to the right and +Z toward the viewer,
    walking the corners in listed order is CLOCKWISE seen from above, so the
    front face would point DOWN and both quads would be culled -- an empty
    frame, with the declared NORMAL attribute still claiming +Y and nothing to
    contradict it.
    """
    base = len(positions)
    for (x, z) in ((-half, -half), (half, -half), (half, half), (-half, half)):
        positions.append((x, y, z))
        normals.append((0.0, 1.0, 0.0))
    uvs.extend(((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)))
    base_index_list.extend((base + 0, base + 2, base + 1, base + 0, base + 3, base + 2))


ground_indices = []
add_quad(0.0, GROUND, ground_indices)
occluder_indices = []
add_quad(OCCLUDER_H, OCCLUDER_A, occluder_indices)
indices = ground_indices + occluder_indices

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
uv_bytes = b"".join(struct.pack("<2f", *u) for u in uvs)
idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + uv_bytes + idx_bytes

all_min = [min(p[k] for p in positions) for k in range(3)]
all_max = [max(p[k] for p in positions) for k in range(3)]


def matte(name, value):
    # Fully rough and non-metallic: the measurement reads a diffuse term, and any
    # specular lobe on the ground would ride the same scanline as the penumbra.
    return {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": [value, value, value, 1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 1.0,
        },
    }


gltf = {
    "asset": {"version": "2.0", "generator": "gen_area_shadow_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [
        {"name": "penumbra_ground", "mesh": 0},
        {"name": "penumbra_occluder", "mesh": 1},
    ],
    "meshes": [
        {"name": "penumbra_ground",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                         "indices": 3, "material": 0}]},
        {"name": "penumbra_occluder",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                         "indices": 4, "material": 1}]},
    ],
    "materials": [matte("penumbra_ground", 0.55), matte("penumbra_occluder", 0.25)],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": all_min, "max": all_max},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5125, "count": len(ground_indices), "type": "SCALAR"},
        {"bufferView": 3, "byteOffset": len(ground_indices) * 4, "componentType": 5125,
         "count": len(occluder_indices), "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes), "byteLength": len(uv_bytes),
         "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes) + len(uv_bytes),
         "byteLength": len(idx_bytes), "target": 34963},
    ],
    "buffers": [
        {"uri": "data:application/octet-stream;base64,"
         + base64.b64encode(buffer_bytes).decode("ascii"),
         "byteLength": len(buffer_bytes)}
    ],
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "area_shadow_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote %s (occluder a=%.2f at h=%.2f over a %.0f-unit ground)"
      % (out, OCCLUDER_A, OCCLUDER_H, GROUND * 2))
