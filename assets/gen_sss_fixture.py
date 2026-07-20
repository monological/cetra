#!/usr/bin/env python3
"""Generate assets/sss_fixture.gltf, the separable-SSS test asset.

Two UV spheres side by side (nodes "sss_skin_a" / "sss_skin_b"), each with a flat
skin/wax-tone material and a mid roughness. glTF carries no subsurface, so the
render app tags each named material with a distinct scatter profile after load
(configure_sss_materials): a warm, wide-scattering skin on the left and a cooler,
tight-scattering wax on the right, so the two read differently under the same
light -- the per-material-profile A/B. Under the key light each sphere has a
clean shadow terminator where the SSS blur softens + tints the falloff; a moody
env shows the thin-edge translucency. Flat color, no texture deps.
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
    "scenes": [{"nodes": [0, 1]}],
    # Two spheres side by side (centers 2.6 apart, radius 1 each -> a clean ~0.6
    # gap, no interpenetration) so the two scatter profiles read as distinct
    # surfaces. The cross-material reject was verified separately with a
    # temporarily-overlapping variant that forced a skin/skin seam.
    "nodes": [
        {"name": "sss_skin_a", "mesh": 0, "translation": [-1.3, 1.0, 0.0]},
        {"name": "sss_skin_b", "mesh": 1, "translation": [1.3, 1.0, 0.0]},
    ],
    "meshes": [
        {
            "name": "sss_skin_a",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}
            ],
        },
        {
            "name": "sss_skin_b",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 1}
            ],
        },
    ],
    # Both mid-value (not near-white) so bright IBL doesn't clip the diffuse -- a
    # blown-out sphere has no gradient for SSS to scatter. Roughness high-ish for
    # a soft, non-glossy wax sheen. The two base tones differ so the distinct
    # scatter profiles (set app-side) read against distinct surfaces.
    "materials": [
        {
            "name": "sss_skin_a",  # warm skin
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.62, 0.44, 0.36, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.6,
            },
        },
        {
            "name": "sss_skin_b",  # cool wax/jade
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.42, 0.52, 0.48, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.6,
            },
        },
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
