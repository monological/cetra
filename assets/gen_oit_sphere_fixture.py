#!/usr/bin/env python3
"""Generate assets/oit_sphere_fixture.gltf, a richer OIT test asset.

Three overlapping *translucent* spheres (red / green / blue, alpha 0.5, alphaMode
BLEND, doubleSided, transmission 0 -- so they hit the OIT path, NOT the refraction
path) at staggered depths. Each double-sided sphere already layers its own front
and back faces, and the three overlap into a region where the depth order changes
across the surface -- the "no single correct draw order" case that sorting can't
solve. Under the unsorted late pass the layering is wrong from some angles and
pops as the camera orbits; under weighted-blended OIT it stays correct and stable.
Flat color, no texture deps. Regenerate with: python3 assets/gen_oit_sphere_fixture.py
"""

import base64
import json
import struct
import math
import os

RINGS = 32    # latitude divisions
SECTORS = 48  # longitude divisions
RADIUS = 0.8

positions = []
normals = []
for r in range(RINGS + 1):
    phi = (r / RINGS) * math.pi  # 0 (top) .. pi (bottom)
    for s in range(SECTORS + 1):
        theta = (s / SECTORS) * 2.0 * math.pi
        nx = math.sin(phi) * math.cos(theta)
        ny = math.cos(phi)
        nz = math.sin(phi) * math.sin(theta)
        positions.append((nx * RADIUS, ny * RADIUS, nz * RADIUS))
        normals.append((nx, ny, nz))

indices = []
for r in range(RINGS):
    for s in range(SECTORS):
        a = r * (SECTORS + 1) + s
        b = a + SECTORS + 1
        indices.extend([a, b, a + 1, a + 1, b, b + 1])  # CCW from outside

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)  # max index < 65535
buffer_bytes = pos_bytes + nrm_bytes + idx_bytes

mn = [min(p[i] for p in positions) for i in range(3)]
mx = [max(p[i] for p in positions) for i in range(3)]

# name, translation (staggered in x AND z so the depth order flips with view),
# base color + alpha. Drawn in scene order red -> green -> blue.
spheres = [
    ("oit_red", [-0.7, 1.0, -0.6], [0.85, 0.15, 0.15, 0.5]),
    ("oit_green", [0.0, 1.0, 0.0], [0.15, 0.7, 0.2, 0.5]),
    ("oit_blue", [0.7, 1.0, 0.6], [0.2, 0.35, 0.85, 0.5]),
]

nodes = [{"name": n, "mesh": i, "translation": t} for i, (n, t, _) in enumerate(spheres)]
meshes = [
    {"name": n, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2,
                                "material": i}]}
    for i, (n, _, _) in enumerate(spheres)
]
materials = [
    {
        "name": n,
        "alphaMode": "BLEND",
        "doubleSided": True,  # render front + back faces -> two translucent layers per sphere
        "pbrMetallicRoughness": {"baseColorFactor": c, "metallicFactor": 0.0, "roughnessFactor": 0.5},
    }
    for (n, _, c) in spheres
]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_oit_sphere_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1, 2]}],
    "nodes": nodes,
    "meshes": meshes,
    "materials": materials,
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": mn, "max": mx},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes), "byteLength": len(idx_bytes),
         "target": 34963},
    ],
    "buffers": [
        {"uri": "data:application/octet-stream;base64," + base64.b64encode(buffer_bytes).decode("ascii"),
         "byteLength": len(buffer_bytes)},
    ],
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "oit_sphere_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", out, "(", len(positions), "verts x 3 translucent spheres )")
