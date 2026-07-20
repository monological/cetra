#!/usr/bin/env python3
"""Generate assets/oit_fixture.gltf, the order-independent-transparency test asset.

Three overlapping translucent quads (red / green / blue, alpha 0.6, alphaMode
BLEND, no transmission) at staggered depths, so their screen-space overlap has no
consistent draw order. Under the unsorted alpha-blend late pass the layering is
wrong from one side and flips (pops) as the camera orbits to the other; under
weighted-blended OIT it stays correct and stable from every angle. Flat color, no
texture deps. Regenerate with: python3 assets/gen_oit_fixture.py
"""

import base64
import json
import struct
import os

HALF = 0.7  # quad half-extent

# One quad in the XY plane facing +Z; instanced by three nodes/materials.
positions = [(-HALF, -HALF, 0.0), (HALF, -HALF, 0.0), (HALF, HALF, 0.0), (-HALF, HALF, 0.0)]
normals = [(0.0, 0.0, 1.0)] * 4
indices = [0, 1, 2, 0, 2, 3]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + idx_bytes

mn = [min(p[i] for p in positions) for i in range(3)]
mx = [max(p[i] for p in positions) for i in range(3)]

# name, translation (staggered in x and z so the overlap order flips with view),
# base color + alpha. Drawn in scene order red -> green -> blue; that order does
# NOT match depth order from every angle, which is exactly what OIT fixes.
quads = [
    ("oit_red", [-0.35, 1.0, -0.4], [0.85, 0.15, 0.15, 0.6]),
    ("oit_green", [0.0, 1.0, 0.0], [0.15, 0.75, 0.2, 0.6]),
    ("oit_blue", [0.35, 1.0, 0.4], [0.2, 0.35, 0.85, 0.6]),
]

nodes = [{"name": n, "mesh": i, "translation": t} for i, (n, t, _) in enumerate(quads)]
meshes = [
    {"name": n, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2,
                                "material": i}]}
    for i, (n, _, _) in enumerate(quads)
]
materials = [
    {
        "name": n,
        "alphaMode": "BLEND",
        "doubleSided": True,
        "pbrMetallicRoughness": {"baseColorFactor": c, "metallicFactor": 0.0, "roughnessFactor": 0.7},
    }
    for (n, _, c) in quads
]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_oit_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1, 2]}],
    "nodes": nodes,
    "meshes": meshes,
    "materials": materials,
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": mn, "max": mx},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"},
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

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "oit_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", out, "(3 translucent quads)")
