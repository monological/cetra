#!/usr/bin/env python3
"""Generate assets/specular_fixture.gltf, the KHR_materials_specular test asset.

A row of UV spheres exercising KHR_materials_specular (specularColorFactor tints
the dielectric F0; specularFactor weights the dielectric specular), meant to be
viewed under an HDR environment (-e ...). The base is a near-black SMOOTH
dielectric (piano-lacquer look) so the tinted specular reflection IS the visible
surface -- on a bright diffuse body the 0.04 dielectric F0 tint is swamped:
  - spec_gold : near-black smooth dielectric + specularColorFactor (1, .71, .29)
                -> the reflected environment reads gold
  - spec_blue : same base + specularColorFactor (.35, .6, 1) -> blue reflection
  - spec_dim  : same base + specularFactor 0.1 -> the specular is nearly removed,
                so the sphere collapses to near-black (an in-frame test of the weight)
  - spec_none : the same base with NO KHR_materials_specular, for an in-frame A/B
                of the neutral white specular the extension re-tints (the global
                --no-specular flag also disables the effect)
Geometry is one shared UV sphere; materials are flat factors (no textures), so
the fixture has no external dependencies. Regenerate with:
  python3 assets/gen_specular_fixture.py
"""

import base64
import json
import struct
import math
import os

# ---- one unit UV sphere (radius 0.5), position + normal + uv ---------------
STACKS = 48
SECTORS = 96
positions, normals, uvs, indices = [], [], [], []
for i in range(STACKS + 1):
    phi = math.pi * i / STACKS  # 0..pi (top to bottom)
    for j in range(SECTORS + 1):
        theta = 2.0 * math.pi * j / SECTORS  # 0..2pi
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
        # CCW seen from OUTSIDE. The natural-looking (a, b, a+1) order faces
        # every triangle inward under this phi/theta sweep, and a sphere drawn
        # inside-out still reads as a plausible shaded disc.
        indices += [a, a + 1, b, a + 1, b + 1, b]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
uv_bytes = b"".join(struct.pack("<2f", *u) for u in uvs)
idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + uv_bytes + idx_bytes

pmin = [min(p[k] for p in positions) for k in range(3)]
pmax = [max(p[k] for p in positions) for k in range(3)]


def material(name, color, rough, spec_color=None, spec_factor=None):
    m = {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": list(color) + [1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": rough,
        },
    }
    if spec_color is not None or spec_factor is not None:
        ks = {}
        if spec_factor is not None:
            ks["specularFactor"] = spec_factor
        if spec_color is not None:
            ks["specularColorFactor"] = spec_color
        m["extensions"] = {"KHR_materials_specular": ks}
    return m


# specularColorFactor is boosted above 1 (valid glTF -- it scales F0 up toward 1)
# so the 0.04 dielectric tint reads clearly in the reflection for this A/B fixture.
base = (0.02, 0.02, 0.02)
materials = [
    material("spec_gold", base, 0.12, spec_color=[10.0, 5.0, 1.5]),
    material("spec_blue", base, 0.12, spec_color=[1.5, 4.0, 12.0]),
    material("spec_dim", base, 0.12, spec_factor=0.1),
    material("spec_none", base, 0.12),
]

# (material index, x translation) -- a row spaced along X at eye height
nodes_spec = [(0, -1.8), (1, -0.6), (2, 0.6), (3, 1.8)]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_specular_fixture.py"},
    "extensionsUsed": ["KHR_materials_specular"],
    "scene": 0,
    "scenes": [{"nodes": list(range(len(nodes_spec)))}],
    "nodes": [
        {"name": materials[mat]["name"], "mesh": i, "translation": [x, 0.0, 0.0]}
        for i, (mat, x) in enumerate(nodes_spec)
    ],
    "meshes": [
        {
            "name": materials[mat]["name"],
            "primitives": [
                {
                    "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                    "indices": 3,
                    "material": mat,
                }
            ],
        }
        for mat, _ in nodes_spec
    ],
    "materials": materials,
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": pmin, "max": pmax},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5125, "count": len(indices), "type": "SCALAR"},
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

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "specular_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", out, "(", len(buffer_bytes), "buffer bytes )")
