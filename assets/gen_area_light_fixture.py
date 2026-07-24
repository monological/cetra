#!/usr/bin/env python3
"""Generate assets/area_light_fixture.gltf, the LTC area-light test asset (spec 9.2).

The canonical LTC validation image: a roughness sweep over a diffuse ground
plane, lit by one rectangular panel. Each element checks something the analytic
point-light path cannot show:
  - the SPHERES sweep roughness 0.05 -> 0.95 across the row, so the specular
    reflection of the panel goes from a sharp rectangle (you can read the
    panel's shape and aspect in it) to a broad wash. A sharp low-roughness
    sphere that shows a rotated or smeared rectangle means the inverse-M
    reconstruction is transposed; a black one means the winding is flipped.
  - the PLANE is a mid-grey dielectric with no specular tint, so it isolates
    the diffuse term (the identity-matrix evaluation), which is independent of
    the matrix packing -- "plane right, spheres wrong" localizes a bug to the
    specular path immediately.
  - the plane extends well past the panel so the single-sided cutoff and the
    distance falloff are both visible in one frame.

All materials are dielectric (metallic 0) -- a metallic subject has no diffuse
response at all, which makes it useless for validating the diffuse half.

Pair it with a panel, e.g.:
  ./out/bin/render -m assets/area_light_fixture.gltf --no-key-light \\
      --ibl-intensity 0 --area-light 0,2,0,0,-1,0,1.5,0.6,30

Regenerate with:
  python3 assets/gen_area_light_fixture.py
"""

import base64
import json
import struct
import math
import os

# ---- geometry: one unit UV sphere (radius 0.5) + one ground quad -----------
STACKS = 48
SECTORS = 96
positions, normals, uvs, indices = [], [], [], []
for i in range(STACKS + 1):
    phi = math.pi * i / STACKS
    for j in range(SECTORS + 1):
        theta = 2.0 * math.pi * j / SECTORS
        nx = math.sin(phi) * math.cos(theta)
        ny = math.cos(phi)
        nz = math.sin(phi) * math.sin(theta)
        positions.append((0.5 * nx, 0.5 * ny, 0.5 * nz))
        normals.append((nx, ny, nz))
        uvs.append((j / SECTORS, i / STACKS))
for i in range(STACKS):
    for j in range(SECTORS):
        a = i * (SECTORS + 1) + j
        b = a + SECTORS + 1
        indices += [a, b, a + 1, a + 1, b, b + 1]

sphere_vertex_count = len(positions)
sphere_index_count = len(indices)

# Ground quad in the XZ plane, normal +Y, wider than the sphere row so the
# panel's falloff has somewhere to land
QUAD = 8.0
quad_positions = [(-QUAD, 0.0, -QUAD), (QUAD, 0.0, -QUAD), (QUAD, 0.0, QUAD), (-QUAD, 0.0, QUAD)]
quad_normals = [(0.0, 1.0, 0.0)] * 4
quad_uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
quad_indices = [0, 1, 2, 0, 2, 3]

positions += quad_positions
normals += quad_normals
uvs += quad_uvs
indices += [i + sphere_vertex_count for i in quad_indices]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
uv_bytes = b"".join(struct.pack("<2f", *u) for u in uvs)
idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + uv_bytes + idx_bytes


def bounds(pts):
    return ([min(p[k] for p in pts) for k in range(3)], [max(p[k] for p in pts) for k in range(3)])


sphere_min, sphere_max = bounds(positions[:sphere_vertex_count])
quad_min, quad_max = bounds(quad_positions)


def material(name, color, rough):
    return {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": list(color) + [1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": rough,
        },
    }


# Roughness sweep incl. the LUT's extremes, which is where a half-texel or
# orientation error in the table fetch shows up first
ROUGHNESS = [0.05, 0.2, 0.4, 0.6, 0.8, 0.95]
SPACING = 1.3
sphere_color = (0.6, 0.6, 0.6)

materials = [material("ltc_rough_%02d" % int(r * 100), sphere_color, r) for r in ROUGHNESS]
materials.append(material("ltc_ground", (0.35, 0.35, 0.35), 0.9))
ground_material = len(materials) - 1

row_x = [(i - (len(ROUGHNESS) - 1) * 0.5) * SPACING for i in range(len(ROUGHNESS))]

nodes = [
    {"name": materials[i]["name"], "mesh": 0, "translation": [x, 0.5, 0.0]}
    for i, x in enumerate(row_x)
]
# Per-sphere material means a mesh per material; reuse the same accessors
meshes = [
    {
        "name": materials[i]["name"],
        "primitives": [
            {
                "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                "indices": 3,
                "material": i,
            }
        ],
    }
    for i in range(len(ROUGHNESS))
]
for i in range(len(ROUGHNESS)):
    nodes[i]["mesh"] = i

nodes.append({"name": "ltc_ground", "mesh": len(meshes), "translation": [0.0, 0.0, 0.0]})
meshes.append(
    {
        "name": "ltc_ground",
        "primitives": [
            {
                "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                "indices": 4,
                "material": ground_material,
            }
        ],
    }
)

gltf = {
    "asset": {"version": "2.0", "generator": "gen_area_light_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": list(range(len(nodes)))}],
    "nodes": nodes,
    "meshes": meshes,
    "materials": materials,
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": [min(sphere_min[k], quad_min[k]) for k in range(3)],
         "max": [max(sphere_max[k], quad_max[k]) for k in range(3)]},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
        # sphere indices, then the quad's (offset into the same buffer view)
        {"bufferView": 3, "componentType": 5125, "count": sphere_index_count, "type": "SCALAR"},
        {"bufferView": 3, "byteOffset": sphere_index_count * 4, "componentType": 5125,
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
        {
            "uri": "data:application/octet-stream;base64,"
            + base64.b64encode(buffer_bytes).decode("ascii"),
            "byteLength": len(buffer_bytes),
        }
    ],
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "area_light_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote %s (%d spheres + ground)" % (out, len(ROUGHNESS)))
