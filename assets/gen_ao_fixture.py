#!/usr/bin/env python3
"""Generate assets/ao_fixture.gltf + ao_fixture_ao.png, the baked-AO test asset.

A sphere on a ground quad, one white material carrying a glTF occlusionTexture
with an unmistakable pattern: a dark radial ring (the ground quad maps the full
texture, so the ring reads as a ring; the sphere's lat-long UVs wrap it into
dark bands). occlusionTexture.strength is authored at 0.8 so the strength path
is pinned too, not just the texture path.

What this asset guards, in order of discovery:
  - glTF occlusion IMPORT: assimp delivers occlusionTexture as LIGHTMAP, which
    the mapping table never consumed until spec 11.5 -- before that fix this
    texture silently vanished and aoMap was identically 1.0 engine-wide.
  - aoMap on the DIFFUSE share only (spec 11.5): under an IBL the ring must dim
    diffuse while ambient specular keeps its energy. The committed golden is
    HDR-free (HDRs are gitignored), so it pins the diffuse dimming under the
    sibling .cscn's authored ambient; the specular half is an interactive A/B.

The sibling ao_fixture.cscn authors the light, the no-IBL ambient fill (which
is the term aoMap multiplies -- without it the texture is invisible), the
camera, and pinned exposure. Regenerate with:
  python3 assets/gen_ao_fixture.py
"""

import base64
import json
import math
import os
import struct

import numpy as np
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))

# ---- occlusion texture: white with a dark radial ring ----------------------
RES = 512
yy, xx = np.mgrid[0:RES, 0:RES].astype(np.float64)
u = xx / RES
v = yy / RES
r = np.sqrt((u - 0.5) ** 2 + (v - 0.5) ** 2)
# Ring centred at radius 0.3, half-width 0.08, floor 0.25: dark enough to be
# unmistakable, non-zero so the strength blend still has room to move it.
ring = 1.0 - 0.75 * np.exp(-(((r - 0.30) / 0.08) ** 2))
Image.fromarray((np.clip(ring, 0.0, 1.0) * 255).astype(np.uint8), "L").save(
    os.path.join(here, "ao_fixture_ao.png"))

# ---- geometry --------------------------------------------------------------
positions = []
normals = []
uvs = []
indices = []


def add_quad(p0, p1, p2, p3, n):
    base = len(positions)
    positions.extend([p0, p1, p2, p3])
    normals.extend([n] * 4)
    uvs.extend([(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)])
    indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


# Ground quad in XZ (normal +Y), 4x4 units, UV [0,1]^2 so the ring reads 1:1.
add_quad((-2.0, 0.0, 2.0), (2.0, 0.0, 2.0), (2.0, 0.0, -2.0), (-2.0, 0.0, -2.0),
         (0.0, 1.0, 0.0))

# Lat-long sphere, radius 0.6, resting on the ground. The shared texture wraps
# into horizontal dark bands on it.
LAT = 24
LON = 32
R_S = 0.6
CY = 0.6
sphere_base = len(positions)
for i in range(LAT + 1):
    theta = math.pi * i / LAT  # 0 at the pole (+Y)
    for j in range(LON + 1):
        phi = 2.0 * math.pi * j / LON
        nx = math.sin(theta) * math.cos(phi)
        ny = math.cos(theta)
        nz = math.sin(theta) * math.sin(phi)
        positions.append((R_S * nx, CY + R_S * ny, R_S * nz))
        normals.append((nx, ny, nz))
        uvs.append((j / LON, i / LAT))
for i in range(LAT):
    for j in range(LON):
        a = sphere_base + i * (LON + 1) + j
        b = a + LON + 1
        # CCW seen from outside (right-handed, +Y up)
        indices.extend([a, a + 1, b, a + 1, b + 1, b])

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
uv_bytes = b"".join(struct.pack("<2f", *w) for w in uvs)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + uv_bytes + idx_bytes
pmin = [min(p[k] for p in positions) for k in range(3)]
pmax = [max(p[k] for p in positions) for k in range(3)]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_ao_fixture.py"},
    "samplers": [{"wrapS": 10497, "wrapT": 10497}],
    "images": [{"uri": "ao_fixture_ao.png"}],
    "textures": [{"source": 0, "sampler": 0}],
    "materials": [
        {
            "name": "ao_white",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.85, 0.85, 0.85, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.4,
            },
            "occlusionTexture": {"index": 0, "strength": 0.8},
        }
    ],
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "ao_fixture", "mesh": 0}],
    "meshes": [
        {
            "name": "ao_fixture",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                 "indices": 3, "material": 0}
            ],
        }
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": pmin, "max": pmax},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
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
         + base64.b64encode(buffer_bytes).decode("ascii"), "byteLength": len(buffer_bytes)}
    ],
}

out = os.path.join(here, "ao_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", out, "+ ao_fixture_ao.png")
