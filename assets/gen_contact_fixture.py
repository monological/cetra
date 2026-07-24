#!/usr/bin/env python3
"""Generate assets/contact_fixture.gltf, the screen-space contact-shadow test asset (spec 9.3).

Contact shadows fill the near-contact darkening a cascaded shadow map's texels
are too coarse to draw. They only read on MATTE surfaces (a metal's contacts are
already dark crevices) and only where an occluder sits close to a lit receiver --
so neither raiden (chrome) nor the LTC sphere fixture (glossy, tangent to the
ground) shows them. This fixture is built for the effect:

  - a large MATTE ground plane (dielectric, rough) so darkening on it is visible;
  - a row of matte cubes hovering at increasing gaps above the plane
    (0.0, 0.06, 0.15, 0.30). Contact shadows darken the plane directly under the
    low-gap cubes and fade out as the gap grows -- the textbook "is it touching
    or floating?" cue that grounds an object. The cube resting flush (gap 0) is
    the baseline; the high-gap cube should get little to none.

The sibling assets/contact_fixture.cscn ships the lighting (a shadow-casting
directional sun) and the 3/4 camera, so the fixture is self-contained -- no
--sky/-e and no camera flags:

  ./out/bin/render -m assets/contact_fixture.gltf --contact-shadows

Toggle --contact-shadows (or the GUI checkbox) and watch the plane under the
cubes darken. --cs-strength / --cs-distance tune it.

Regenerate the geometry with:
  python3 assets/gen_contact_fixture.py
(the .cscn is hand-authored, not generated.)
"""

import base64
import json
import struct
import os

positions, normals, uvs, indices = [], [], [], []

# ---- one unit cube, half-extent 0.4, per-face normals (flat-shaded box) ------
HALF = 0.4
# face: (normal, four corner offsets CCW seen from outside)
faces = [
    ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),    # +Z
    ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]),  # -Z
    ((1, 0, 0), [(1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)]),    # +X
    ((-1, 0, 0), [(-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)]),  # -X
    ((0, 1, 0), [(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)]),    # +Y
    ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),  # -Y
]
for n, corners in faces:
    base = len(positions)
    for cx, cy, cz in corners:
        positions.append((cx * HALF, cy * HALF, cz * HALF))
        normals.append(n)
        uvs.append((0.0, 0.0))
    indices += [base, base + 1, base + 2, base, base + 2, base + 3]

cube_vertex_count = len(positions)
cube_index_count = len(indices)

# ---- ground quad in XZ, normal +Y, wide enough to catch the shadows ----------
QUAD = 6.0
quad_positions = [(-QUAD, 0.0, -QUAD), (QUAD, 0.0, -QUAD), (QUAD, 0.0, QUAD), (-QUAD, 0.0, QUAD)]
quad_normals = [(0.0, 1.0, 0.0)] * 4
quad_uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
quad_indices = [0, 1, 2, 0, 2, 3]

positions += quad_positions
normals += quad_normals
uvs += quad_uvs
indices += [i + cube_vertex_count for i in quad_indices]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
uv_bytes = b"".join(struct.pack("<2f", *u) for u in uvs)
idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + uv_bytes + idx_bytes


def bounds(pts):
    return ([min(p[k] for p in pts) for k in range(3)], [max(p[k] for p in pts) for k in range(3)])


all_min, all_max = bounds(positions)


def matte(name, color):
    return {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": list(color) + [1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 0.9,
        },
    }


materials = [matte("contact_ground", (0.55, 0.55, 0.55)), matte("contact_cube", (0.6, 0.6, 0.62))]

# Cubes hover at increasing gaps: contact shadow strong under the low ones,
# gone under the high one. Cube center sits at HALF + gap so its base is `gap`
# above the plane.
GAPS = [0.0, 0.06, 0.15, 0.30]
SPACING = 1.4
cube_x = [(i - (len(GAPS) - 1) * 0.5) * SPACING for i in range(len(GAPS))]

nodes = [
    {"name": "cube_gap_%02d" % int(g * 100), "mesh": 0,
     "translation": [x, HALF + g, 0.0]}
    for x, g in zip(cube_x, GAPS)
]
nodes.append({"name": "contact_ground", "mesh": 1, "translation": [0.0, 0.0, 0.0]})

meshes = [
    {"name": "contact_cube",
     "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                     "indices": 3, "material": 1}]},
    {"name": "contact_ground",
     "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                     "indices": 4, "material": 0}]},
]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_contact_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": list(range(len(nodes)))}],
    "nodes": nodes,
    "meshes": meshes,
    "materials": materials,
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": all_min, "max": all_max},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5125, "count": cube_index_count, "type": "SCALAR"},
        {"bufferView": 3, "byteOffset": cube_index_count * 4, "componentType": 5125,
         "count": len(quad_indices), "type": "SCALAR"},
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

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "contact_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote %s (%d cubes over a matte plane)" % (out, len(GAPS)))
