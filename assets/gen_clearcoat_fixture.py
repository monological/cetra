#!/usr/bin/env python3
"""Generate assets/clearcoat_fixture.gltf, the clearcoat (4.10) test asset.

A row of UV spheres exercising KHR_materials_clearcoat. An HDR environment
(-e ...) shows the coat lobe at its best, reflecting the surroundings, but the
fixture reads under the app's fallback three-point rig too -- which is what its
golden and its gate arms render, since neither can depend on an HDR:
  - car_paint      : dark-red dielectric base (rough 0.55) + clearcoat 1.0,
                     coat roughness 0.06 + the weave below -> a broad soft base
                     highlight PLUS a tight sharp coat highlight (the dual-lobe
                     look), broken into dashes by the weave
  - car_paint_nocoat: the same base with NO clearcoat, for an in-frame A/B of
                      what the coat adds (the global --no-clearcoat flag also
                      toggles it)
  - carbon         : near-black metallic base (rough 0.4) + clearcoat 1.0, coat
                     roughness 0.08 + the weave -> lacquered carbon-fibre look

Geometry is one shared UV sphere. The two coated materials carry a clearcoat
NORMAL map -- a procedural carbon-fibre weave, embedded as a data URI, so the
fixture still has no external dependencies. It is not decoration: it is the one
tangent-space normal in the corpus that reaches pbr_frag through a path of its
own, since assimp exposes it at aiTextureType_CLEARCOAT index 2 and import.c's
index-0 mapping table cannot reach it. Regenerate with:
  python3 assets/gen_clearcoat_fixture.py
"""

import base64
import io
import json
import struct
import math
import os

import numpy as np
from PIL import Image


def carbon_weave_normal_datauri(size=256):
    """A 2x2 twill carbon-fibre weave as a tangent-space normal map (data URI).

    Height = smooth over/under ridges whose direction alternates per weave cell;
    normals are the height gradient. Encoded RGB = (n*0.5+0.5)."""
    ax = np.linspace(0, 2 * np.pi, size, endpoint=False)
    u, v = np.meshgrid(ax, ax)
    cells = 8  # weave repeats
    cu = np.floor(u / (2 * np.pi) * cells).astype(int)
    cv = np.floor(v / (2 * np.pi) * cells).astype(int)
    # Twill: strands run horizontal on even (cu+cv), vertical on odd
    horiz = ((cu + cv) % 2) == 0
    h = np.where(horiz, np.sin(v * cells), np.sin(u * cells)) * 0.5 + 0.5
    strength = 2.5
    gy, gx = np.gradient(h * strength)
    nx, ny, nz = -gx, -gy, np.ones_like(h)
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    rgb = np.stack([nx * inv, ny * inv, nz * inv], axis=-1) * 0.5 + 0.5
    img = Image.fromarray((rgb * 255).astype(np.uint8), "RGB")
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode("ascii")

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


def material(name, color, rough, metallic=0.0, clearcoat=None, coat_rough=None, coat_normal=False):
    m = {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": list(color) + [1.0],
            "metallicFactor": metallic,
            "roughnessFactor": rough,
        },
    }
    if clearcoat is not None:
        cc = {"clearcoatFactor": clearcoat}
        if coat_rough is not None:
            cc["clearcoatRoughnessFactor"] = coat_rough
        if coat_normal:
            cc["clearcoatNormalTexture"] = {"index": 0}
        m["extensions"] = {"KHR_materials_clearcoat": cc}
    return m


materials = [
    material("car_paint", (0.35, 0.02, 0.03), 0.55, clearcoat=1.0, coat_rough=0.06, coat_normal=True),
    material("car_paint_nocoat", (0.35, 0.02, 0.03), 0.55),
    material("carbon", (0.02, 0.02, 0.025), 0.4, metallic=1.0, clearcoat=1.0, coat_rough=0.08,
             coat_normal=True),
]

# (material index, x translation) -- a row spaced along X at eye height
nodes_spec = [(0, -1.2), (1, 0.0), (2, 1.2)]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_clearcoat_fixture.py"},
    "extensionsUsed": ["KHR_materials_clearcoat"],
    "samplers": [{"wrapS": 10497, "wrapT": 10497}],
    "images": [{"uri": carbon_weave_normal_datauri()}],
    "textures": [{"source": 0, "sampler": 0}],
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

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "clearcoat_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", out, "(", len(buffer_bytes), "buffer bytes )")
