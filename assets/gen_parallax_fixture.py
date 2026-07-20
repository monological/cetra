#!/usr/bin/env python3
"""Generate assets/parallax_fixture.gltf + sidecar PNGs, the POM (4.11) test asset.

A single brick floor tile viewed under an HDR (-e ...) at a grazing angle so the
parallax relief reads: with `--parallax` the raised bricks shift against the
recessed mortar as the camera moves, cast soft self-shadows, and recede at the
tile edge; with `--no-parallax` it is a flat normal-mapped plane.

Emits EXTERNAL textures (not data-URIs) so the engine's filename-convention
height loader (`resolve_height_maps`) resolves the height sibling:
  - parallax_fixture_albedo.png  (sRGB brick colour)         -> glTF baseColorTexture
  - parallax_fixture_normal.png  (tangent-space, from height)-> glTF normalTexture
  - parallax_fixture_height.png  (grayscale, WHITE = raised) -> loaded by convention
glTF carries no height slot; `parallax_fixture_albedo.png` -> `_height` sibling is
how the height map reaches Material.height_tex. Regenerate with:
  python3 assets/gen_parallax_fixture.py
"""

import base64
import json
import os
import struct

import numpy as np
from PIL import Image

RES = 512
COURSES = 5          # brick rows over the tile
BRICKS = 3           # bricks per row
MORTAR = 0.055       # mortar half-width as a fraction of a brick cell
H_BRICK = 0.80       # raised brick height (white = raised)
H_MORTAR = 0.12      # recessed mortar height

# ---- procedural brick height field (white = raised) ------------------------
yy, xx = np.mgrid[0:RES, 0:RES].astype(np.float64)
u = xx / RES
v = yy / RES
row = np.floor(v * COURSES).astype(int)
uu = u * BRICKS + np.where(row % 2 == 1, 0.5, 0.0)  # half-brick offset on odd rows
col = np.floor(uu).astype(int)
fu = uu - col                 # 0..1 across a brick
fv = v * COURSES - row        # 0..1 up a brick

# per-brick height jitter (deterministic)
rng = np.random.default_rng(7)
jitter = rng.uniform(-0.06, 0.06, size=(COURSES, BRICKS + 2))
cj = np.clip(col, 0, BRICKS + 1)
ri = np.clip(row, 0, COURSES - 1)
brick_top = H_BRICK + jitter[ri, cj]

# soft bevel: 0 at the mortar centre, 1 on the brick face
edge = np.minimum.reduce([fu, 1.0 - fu, fv, 1.0 - fv])
bevel = np.clip((edge - MORTAR) / MORTAR, 0.0, 1.0)
bevel = bevel * bevel * (3.0 - 2.0 * bevel)  # smoothstep
height = H_MORTAR + bevel * (brick_top - H_MORTAR)
height = np.clip(height, 0.0, 1.0)

# ---- tangent-space normal map from the height gradient ---------------------
STRENGTH = 3.0
gy, gx = np.gradient(height * STRENGTH)
nx, ny, nz = -gx, -gy, np.ones_like(height)
inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
normal_rgb = np.stack([nx * inv, ny * inv, nz * inv], axis=-1) * 0.5 + 0.5

# ---- albedo: brick faces reddish (with per-brick tint), mortar grey --------
on_brick = bevel > 0.5
tint = rng.uniform(-0.06, 0.06, size=(COURSES, BRICKS + 2))
brick_r = np.clip(0.52 + tint[ri, cj], 0.35, 0.7)
brick = np.stack([brick_r, np.full_like(u, 0.16), np.full_like(u, 0.12)], axis=-1)
mortar = np.stack([np.full_like(u, 0.55), np.full_like(u, 0.53), np.full_like(u, 0.50)], axis=-1)
albedo_rgb = np.where(on_brick[..., None], brick, mortar)

here = os.path.dirname(os.path.abspath(__file__))


def save_png(arr01, name, mode="RGB"):
    img = Image.fromarray((np.clip(arr01, 0.0, 1.0) * 255).astype(np.uint8), mode)
    img.save(os.path.join(here, name))


save_png(albedo_rgb, "parallax_fixture_albedo.png", "RGB")
save_png(normal_rgb, "parallax_fixture_normal.png", "RGB")
save_png(height, "parallax_fixture_height.png", "L")  # grayscale, white = raised

# ---- geometry: a brick WALL in the XY plane (normal +Z) --------------------
# A vertical wall frames face-on under the orbit camera (a flat floor sits
# edge-on); orbit to the side to see the parallax shift + self-shadows. CCW from
# +Z so the front face points at the camera. UV maps the full brick texture.
positions = [(-1.5, 0.0, 0.0), (1.5, 0.0, 0.0), (1.5, 2.0, 0.0), (-1.5, 2.0, 0.0)]
normals = [(0.0, 0.0, 1.0)] * 4
uvs = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
indices = [0, 1, 2, 0, 2, 3]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
uv_bytes = b"".join(struct.pack("<2f", *w) for w in uvs)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + uv_bytes + idx_bytes
pmin = [min(p[k] for p in positions) for k in range(3)]
pmax = [max(p[k] for p in positions) for k in range(3)]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_parallax_fixture.py"},
    "samplers": [{"wrapS": 10497, "wrapT": 10497}],
    "images": [
        {"uri": "parallax_fixture_albedo.png"},
        {"uri": "parallax_fixture_normal.png"},
    ],
    "textures": [{"source": 0, "sampler": 0}, {"source": 1, "sampler": 0}],
    "materials": [
        {
            "name": "parallax_brick",
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": 0},
                "metallicFactor": 0.0,
                "roughnessFactor": 0.85,
            },
            "normalTexture": {"index": 1},
        }
    ],
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "parallax_brick", "mesh": 0}],
    "meshes": [
        {
            "name": "parallax_brick",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                 "indices": 3, "material": 0}
            ],
        }
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": pmin, "max": pmax},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
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

out = os.path.join(here, "parallax_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", out, "+ albedo/normal/height PNGs")
