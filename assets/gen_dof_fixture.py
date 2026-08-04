#!/usr/bin/env python3
"""Generate assets/dof_fixture.gltf + dof_fixture_checker.png, the bokeh chart.

A focus panel at the focus plane with emissive point rows staggered through
depth, sized so every DoF regime is one glance:

  - focus panel   z = 0   (6 m)   fine checker + rings: the sharpness judge
  - near row      z = +3  (3 m)   3 warm spheres, CoC clamps to -max; one
                                  overlaps the panel's screen-left edge, so
                                  its blur must spill OVER sharp content
  - mid row       z = -0.8 (6.8m) 5 white spheres, coc ~ 0.53*max: pins the
                                  sharp-to-far transition band
  - far row       z = -3  (9 m)   5 cool spheres at +max clamp; one half
                                  behind the panel's right edge (far blur
                                  stays UNDER sharp), one directly behind a
                                  near sphere (near disc occludes far disc)
  - farthest row  z = -6  (12 m)  4 magenta spheres, also +max clamp -- same
                                  disc size as the cool row, different color:
                                  a near/far field swap is instantly visible
                                  as wrong-colored spill
  - ground        y = 0           dark quad giving a continuous CoC ramp

Depth layout vs the app's scene-derived planes (why 0.5-50 m would fail):
bounds here give scene radius ~8.5, so auto_near = 0.05*8.5 ~ 0.42 m -- the
nearest element at 3 m clears it. The scene-scaled default focus range
(1.5*radius ~ 12.7) would leave everything sharp, so recipes must pin
--dof-focus 6 --dof-range 1.5. Sphere r=0.05 at 9 m projects ~3.4 half-res
texels at -W 800: above the 64-tap spacing floor at gather radius 8, so
discs render filled, not dotted.

First asset to author KHR_materials_emissive_strength (the import path at
aiTextureType EMISSIVE_INTENSITY was live but never exercised); strengths
60-150 land 4-7 stops over diffuse white under pinned exposure -- real HDR
highlights, far below the fp16/WS_SCENE_MAX ceilings.

The sibling dof_fixture.cscn authors the light, ambient, camera (eye z=+6,
target the panel -- the target IS the autofocus distance), and pinned
exposure. Regenerate with:
  python3 assets/gen_dof_fixture.py
"""

import base64
import json
import math
import os
import struct

import numpy as np
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))

# ---- panel texture: fine checker with concentric rings ---------------------
RES = 512
yy, xx = np.mgrid[0:RES, 0:RES].astype(np.float64)
u = xx / RES
v = yy / RES
checker = ((xx // 8 + yy // 8) % 2) * 0.55 + 0.35  # 8 px cells, mid contrast
r = np.sqrt((u - 0.5) ** 2 + (v - 0.5) ** 2)
rings = 0.5 + 0.5 * np.cos(r * 2.0 * math.pi * 24.0)
tex = np.clip(checker * 0.7 + rings * 0.3, 0.0, 1.0)
Image.fromarray((tex * 255).astype(np.uint8), "L").convert("RGB").save(
    os.path.join(here, "dof_fixture_checker.png"))

# ---- geometry, one primitive per material ----------------------------------
# Each primitive owns contiguous vertex/index ranges in the shared buffer.
prims = []  # list of dicts: positions/normals/uvs/indices


def new_prim():
    p = {"positions": [], "normals": [], "uvs": [], "indices": []}
    prims.append(p)
    return p


def add_quad(p, p0, p1, p2, p3, n):
    base = len(p["positions"])
    p["positions"].extend([p0, p1, p2, p3])
    p["normals"].extend([n] * 4)
    p["uvs"].extend([(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)])
    p["indices"].extend([base, base + 1, base + 2, base, base + 2, base + 3])


def add_sphere(p, cx, cy, cz, radius, lat=12, lon=16):
    base = len(p["positions"])
    for i in range(lat + 1):
        theta = math.pi * i / lat
        for j in range(lon + 1):
            phi = 2.0 * math.pi * j / lon
            nx = math.sin(theta) * math.cos(phi)
            ny = math.cos(theta)
            nz = math.sin(theta) * math.sin(phi)
            p["positions"].append((cx + radius * nx, cy + radius * ny, cz + radius * nz))
            p["normals"].append((nx, ny, nz))
            p["uvs"].append((j / lon, i / lat))
    for i in range(lat):
        for j in range(lon):
            a = base + i * (lon + 1) + j
            b = a + lon + 1
            p["indices"].extend([a, a + 1, b, a + 1, b + 1, b])


# prim 0: focus panel (checker material), 1.2 x 0.9 facing +Z at the focus plane
panel = new_prim()
add_quad(panel, (-0.6, 0.55, 0.0), (0.6, 0.55, 0.0), (0.6, 1.45, 0.0),
         (-0.6, 1.45, 0.0), (0.0, 0.0, 1.0))

# prim 1: ground (dark material)
ground = new_prim()
add_quad(ground, (-6.0, 0.0, 4.0), (6.0, 0.0, 4.0), (6.0, 0.0, -8.0),
         (-6.0, 0.0, -8.0), (0.0, 1.0, 0.0))

# prim 2: near row (warm) -- 3 m from the camera at z=+6. The x=-0.3 sphere
# sits on the same screen ray as the panel's left edge (x/3 == -0.6/6).
near = new_prim()
for x, y in [(-0.3, 1.1), (-0.55, 1.35), (-0.75, 0.85)]:
    add_sphere(near, x, y, 3.0, 0.05)

# prim 3: mid row (white), coc ~ 0.53*max -- above the panel's top edge
mid = new_prim()
for x in [-1.2, -0.6, 0.0, 0.6, 1.2]:
    add_sphere(mid, x, 1.7, -0.8, 0.05)

# prim 4: far row (cool) at the +max clamp. x=-0.9 is directly behind the
# near x=-0.3 sphere (same screen ray); x=0.95 is half behind the panel's
# right edge; x=0.4 hides fully behind the panel.
far = new_prim()
for x in [-1.8, -0.9, 0.4, 0.95, 1.6]:
    add_sphere(far, x, 0.9, -3.0, 0.05)

# prim 5: farthest row (magenta), also +max clamp -- same disc size as the
# cool row at the recipe's maxCoC, different color. Higher than the mid row
# in screen space (y chosen for (2.4-1.2)/12 > (1.7-1.2)/6.8) so the two
# rows read as separate lines, not pairs.
farthest = new_prim()
for x in [-2.4, -0.8, 0.8, 2.4]:
    add_sphere(farthest, x, 2.4, -6.0, 0.08)

# ---- pack one shared buffer -------------------------------------------------
blob = b""
buffer_views = []
accessors = []
gltf_prims = []


def push_view(data, target):
    buffer_views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data),
                         "target": target})
    return len(buffer_views) - 1


def emit_prim(p, material):
    global blob
    pos = b"".join(struct.pack("<3f", *q) for q in p["positions"])
    nrm = b"".join(struct.pack("<3f", *q) for q in p["normals"])
    uv = b"".join(struct.pack("<2f", *q) for q in p["uvs"])
    idx = b"".join(struct.pack("<H", i) for i in p["indices"])
    pmin = [min(q[k] for q in p["positions"]) for k in range(3)]
    pmax = [max(q[k] for q in p["positions"]) for k in range(3)]
    attrs = {}
    for name, data, target, ctype, atype, count, mm in [
        ("POSITION", pos, 34962, 5126, "VEC3", len(p["positions"]), (pmin, pmax)),
        ("NORMAL", nrm, 34962, 5126, "VEC3", len(p["normals"]), None),
        ("TEXCOORD_0", uv, 34962, 5126, "VEC2", len(p["uvs"]), None),
    ]:
        view = push_view(data, target)
        blob += data
        acc = {"bufferView": view, "componentType": ctype, "count": count, "type": atype}
        if mm:
            acc["min"], acc["max"] = mm
        accessors.append(acc)
        attrs[name] = len(accessors) - 1
    view = push_view(idx, 34963)
    blob += idx
    accessors.append({"bufferView": view, "componentType": 5123,
                      "count": len(p["indices"]), "type": "SCALAR"})
    gltf_prims.append({"attributes": attrs, "indices": len(accessors) - 1,
                       "material": material})


def emissive(name, color, strength):
    return {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": [0.0, 0.0, 0.0, 1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 1.0,
        },
        "emissiveFactor": color,
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": strength}},
    }


materials = [
    {
        "name": "focus_panel",
        "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0},
            "metallicFactor": 0.0,
            "roughnessFactor": 0.6,
        },
    },
    {
        "name": "dark_ground",
        "pbrMetallicRoughness": {
            "baseColorFactor": [0.3, 0.3, 0.3, 1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 0.9,
        },
    },
    emissive("bokeh_warm", [1.0, 0.6, 0.25], 80.0),
    emissive("bokeh_white", [1.0, 1.0, 1.0], 60.0),
    emissive("bokeh_cool", [0.4, 0.7, 1.0], 100.0),
    emissive("bokeh_magenta", [1.0, 0.3, 0.8], 150.0),
]

for i, p in enumerate(prims):
    emit_prim(p, i)

gltf = {
    "asset": {"version": "2.0", "generator": "gen_dof_fixture.py"},
    "extensionsUsed": ["KHR_materials_emissive_strength"],
    "images": [{"uri": "dof_fixture_checker.png"}],
    "textures": [{"source": 0}],
    "materials": materials,
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "dof_fixture", "mesh": 0}],
    "meshes": [{"name": "dof_fixture", "primitives": gltf_prims}],
    "accessors": accessors,
    "bufferViews": buffer_views,
    "buffers": [
        {"uri": "data:application/octet-stream;base64,"
         + base64.b64encode(blob).decode("ascii"), "byteLength": len(blob)}
    ],
}

out = os.path.join(here, "dof_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", out, "+ dof_fixture_checker.png")
