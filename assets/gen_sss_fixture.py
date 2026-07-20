#!/usr/bin/env python3
"""Generate assets/sss_fixture.gltf, the separable-SSS (§4.12) test asset.

A single UV sphere with a flat skin-tone material and a mid roughness. glTF
carries no subsurface, so the render app sets subsurface / subsurface_color /
subsurface_radius on the "sss_skin" material after load (--sss-radius / --sss-
color override). Under the key light the sphere has a clean shadow terminator
where the SSS diffuse blur softens + reddens the falloff; a back/rim light (or a
moody env) shows the thin-edge translucency. Flat color, no texture deps.
Regenerate with: python3 assets/gen_sss_fixture.py
"""

import base64
import json
import struct
import math
import os

RINGS = 48    # latitude divisions
SECTORS = 96  # longitude divisions
RADIUS = 1.0

positions = []
normals = []
for r in range(RINGS + 1):
    v = r / RINGS
    phi = v * math.pi  # 0 (top) .. pi (bottom)
    for s in range(SECTORS + 1):
        u = s / SECTORS
        theta = u * 2.0 * math.pi
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
        # Two triangles per quad, CCW when viewed from outside.
        indices.extend([a, b, a + 1, a + 1, b, b + 1])

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + idx_bytes

mn = [min(p[i] for p in positions) for i in range(3)]
mx = [max(p[i] for p in positions) for i in range(3)]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_sss_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "sss_skin", "mesh": 0, "translation": [0.0, 1.0, 0.0]}],
    "meshes": [
        {
            "name": "sss_skin",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}
            ],
        }
    ],
    "materials": [
        {
            "name": "sss_skin",
            "pbrMetallicRoughness": {
                # A warm skin/wax tone; SSS reads best on a light, saturated-warm
                # base so the red scatter is visible at the terminator.
                "baseColorFactor": [0.85, 0.62, 0.52, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.45,
            },
        }
    ],
    "accessors": [
        {
            "bufferView": 0,
            "componentType": 5126,
            "count": len(positions),
            "type": "VEC3",
            "min": mn,
            "max": mx,
        },
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5125, "count": len(indices), "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes), "target": 34962},
        {
            "buffer": 0,
            "byteOffset": len(pos_bytes) + len(nrm_bytes),
            "byteLength": len(idx_bytes),
            "target": 34963,
        },
    ],
    "buffers": [
        {
            "uri": "data:application/octet-stream;base64,"
            + base64.b64encode(buffer_bytes).decode("ascii"),
            "byteLength": len(buffer_bytes),
        }
    ],
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sss_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", out, "(", len(positions), "verts,", len(indices) // 3, "tris )")
