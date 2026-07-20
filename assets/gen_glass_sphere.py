#!/usr/bin/env python3
"""Generate assets/glass_sphere.gltf -- a premium crystal-glass sphere.

A single smooth UV sphere (radius 1, centered at origin) with a physically
based glass material: full transmission, ior 1.5, near-zero roughness for a
clean refraction, a volume thickness so the screen-space refraction BENDS the
environment through the body, and a faint cool attenuation tint for a crystal
(not plastic) read. No textures -- the look comes entirely from the material +
whatever environment it is rendered under (a detailed HDR sells it best).

Regenerate with: python3 assets/gen_glass_sphere.py
"""

import base64
import json
import struct
import os
import math

# UV sphere. Silhouette smoothness matters for glass (the Fresnel rim rides the
# outline), so tessellate finely. Normals are the unit position (exact for a
# sphere). (lon+1)*(lat+1) verts stays under the 65535 uint16 index ceiling.
LON = 256  # longitude segments (around)
LAT = 128  # latitude segments (pole to pole)

positions = []
normals = []
for i in range(LAT + 1):
    theta = math.pi * i / LAT          # 0..pi, pole to pole
    st, ct = math.sin(theta), math.cos(theta)
    for j in range(LON + 1):
        phi = 2.0 * math.pi * j / LON   # 0..2pi, around
        sp, cp = math.sin(phi), math.cos(phi)
        p = (st * cp, ct, st * sp)      # radius 1
        positions.append(p)
        normals.append(p)               # unit sphere: normal == position

indices = []
for i in range(LAT):
    for j in range(LON):
        a = i * (LON + 1) + j
        b = a + (LON + 1)
        # CCW when viewed from OUTSIDE (glTF front face). The reversed order
        # (a, b, a+1) winds inward: on a doubleSided mesh that makes the shader's
        # gl_FrontFacing test flip the normals (N=-N), splitting a mirror's
        # reflection at a hard seam.
        indices.extend((a, a + 1, b))
        indices.extend((a + 1, b + 1, b))

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
# uint32 indices (5125): keeps the generator correct at any tessellation, above
# or below the uint16 (65535) vertex ceiling. 256x128 is plenty for a smooth
# reflective silhouette.
idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + idx_bytes

# Premium crystal glass. metallic 0 (dielectric), near-zero roughness for a
# clean bend (a touch above 0 avoids a synthetic razor edge), full transmission,
# ior 1.5 (crown glass), a volume thickness that drives the refraction bend, and
# a faint cool attenuation tint over a long distance so thick paths pick up a
# hint of teal without tinting the thin rim (the crystal cue).
material = {
    "name": "crystal_glass",
    "doubleSided": False,
    "pbrMetallicRoughness": {
        "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
        "metallicFactor": 0.0,
        "roughnessFactor": 0.04,
    },
    "extensions": {
        "KHR_materials_transmission": {"transmissionFactor": 1.0},
        "KHR_materials_ior": {"ior": 1.5},
        "KHR_materials_volume": {
            "thicknessFactor": 1.0,
            "attenuationColor": [0.80, 0.94, 0.97],
            "attenuationDistance": 3.0,
        },
    },
}

gltf = {
    "asset": {"version": "2.0", "generator": "gen_glass_sphere.py"},
    "extensionsUsed": [
        "KHR_materials_transmission",
        "KHR_materials_ior",
        "KHR_materials_volume",
    ],
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "crystal_glass", "mesh": 0}],
    "meshes": [
        {
            "name": "crystal_glass",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}
            ],
        }
    ],
    "materials": [material],
    "accessors": [
        {
            "bufferView": 0,
            "componentType": 5126,
            "count": len(positions),
            "type": "VEC3",
            "min": [-1.0, -1.0, -1.0],
            "max": [1.0, 1.0, 1.0],
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

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "glass_sphere.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", out, "(", len(positions), "verts,", len(indices) // 3, "tris,",
      len(buffer_bytes), "buffer bytes )")
