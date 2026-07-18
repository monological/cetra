#!/usr/bin/env python3
"""Generate assets/glass_fixture.gltf, the screen-space-refraction test asset.

A 2x2 colored backdrop wall sits behind two glass panels:
  - clear panel  (transmission 1, roughness 0,   ior 1.5, volume thickness 0.3)
      -> shows the pure refraction BEND of the backdrop
  - frosted panel (transmission 1, roughness 0.4, ior 1.5, thin)
      -> shows the pure roughness BLUR of the backdrop
All geometry shares one unit-quad; materials are flat colors so the fixture
has no texture dependencies. Regenerate with: python3 assets/gen_glass_fixture.py
"""

import base64
import json
import struct
import os

# One unit quad in the XY plane, facing +Z (two triangles)
positions = [(-0.5, -0.5, 0.0), (0.5, -0.5, 0.0), (0.5, 0.5, 0.0), (-0.5, 0.5, 0.0)]
normals = [(0.0, 0.0, 1.0)] * 4
indices = [0, 1, 2, 0, 2, 3]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
if len(idx_bytes) % 4:
    idx_bytes += b"\x00\x00"  # 4-byte alignment for the buffer tail
buffer_bytes = pos_bytes + nrm_bytes + idx_bytes


def material(name, color, rough, transmission=None, ior=None, thickness=None):
    m = {
        "name": name,
        "doubleSided": True,
        "pbrMetallicRoughness": {
            "baseColorFactor": list(color) + [1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": rough,
        },
    }
    ext = {}
    if transmission is not None:
        ext["KHR_materials_transmission"] = {"transmissionFactor": transmission}
    if ior is not None:
        ext["KHR_materials_ior"] = {"ior": ior}
    if thickness is not None:
        ext["KHR_materials_volume"] = {"thicknessFactor": thickness}
    if ext:
        m["extensions"] = ext
    return m


materials = [
    material("backdrop_red", (0.85, 0.10, 0.10), 0.9),
    material("backdrop_green", (0.10, 0.75, 0.15), 0.9),
    material("backdrop_blue", (0.10, 0.25, 0.85), 0.9),
    material("backdrop_yellow", (0.90, 0.80, 0.10), 0.9),
    material("glass_clear", (1.0, 1.0, 1.0), 0.0, transmission=1.0, ior=1.5, thickness=0.3),
    material("glass_frosted", (1.0, 1.0, 1.0), 0.4, transmission=1.0, ior=1.5),
]

# (material index, translation, scale) -- backdrop wall at z=-1.5 spanning
# x[-1,1] y[0,2]; panels in front at z=0
nodes_spec = [
    (0, (-0.5, 0.5, -1.5), 1.0),
    (1, (0.5, 0.5, -1.5), 1.0),
    (2, (-0.5, 1.5, -1.5), 1.0),
    (3, (0.5, 1.5, -1.5), 1.0),
    (4, (-0.5, 1.0, 0.0), 0.8),
    (5, (0.5, 1.0, 0.0), 0.8),
]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_glass_fixture.py"},
    "extensionsUsed": [
        "KHR_materials_transmission",
        "KHR_materials_ior",
        "KHR_materials_volume",
    ],
    "scene": 0,
    "scenes": [{"nodes": list(range(len(nodes_spec)))}],
    "nodes": [
        {
            "name": materials[mat]["name"],
            "mesh": i,
            "translation": list(t),
            "scale": [s, s, s],
        }
        for i, (mat, t, s) in enumerate(nodes_spec)
    ],
    "meshes": [
        {
            "name": materials[mat]["name"],
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": mat}
            ],
        }
        for mat, _, _ in nodes_spec
    ],
    "materials": materials,
    "accessors": [
        {
            "bufferView": 0,
            "componentType": 5126,
            "count": 4,
            "type": "VEC3",
            "min": [-0.5, -0.5, 0.0],
            "max": [0.5, 0.5, 0.0],
        },
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"},
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

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "glass_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", out, "(", len(buffer_bytes), "buffer bytes )")
