#!/usr/bin/env python3
"""Generate assets/light_import_fixture.gltf, the light-unit import test asset.

A single flat red cube (baseColorFactor, no textures) lit by an embedded
KHR_lights_punctual directional light with intensity 2049 -- the lux value
Blender's exporter writes for its default 3 W/m^2 sun (watts x683).

What this fixture guards INVERTED at spec 10.0. It used to prove that import.c
divided that 2049 down to renderer scale (3.0); a raw 2049 clipped the cube to
white, so "renders red" meant the conversion ran. Since 9.9 made intensity
candela/lux outright, that divide made every imported light 683x too dim and was
deleted -- so the same asset now guards the opposite property:

  - 2049 lux must arrive on Light.intensity as 2049, unmodified. Check the import
    log line, not the pixels: the value is the assertion.
  - exposure is what makes it viewable now, not a rescale at import. The sibling
    .cscn authors a post.camera bright enough for a 2049 lux sun, so a red cube
    still means "correct", but for a different reason -- if it clips white, the
    camera is wrong, not the importer.
  - the render app must still report "skipping auto key light", since the asset
    ships its own.

Flat color, no texture deps.
Regenerate with: python3 assets/gen_light_import_fixture.py
"""

import base64
import json
import math
import os
import struct

# Unit cube, 24 verts (per-face normals), CCW from outside.
FACES = [
    # normal, four corners
    ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
    ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]),
    ((1, 0, 0), [(1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)]),
    ((-1, 0, 0), [(-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)]),
    ((0, 1, 0), [(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)]),
    ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),
]

positions = []
normals = []
indices = []
for normal, corners in FACES:
    base = len(positions)
    for c in corners:
        positions.append(tuple(0.5 * v for v in c))
        normals.append(normal)
    indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + idx_bytes

mn = [min(p[i] for p in positions) for i in range(3)]
mx = [max(p[i] for p in positions) for i in range(3)]

# Light node: glTF directional lights shine along the node's -Z. Aim the
# beam diagonally (down, leftward, toward -Z) so the top, right, and front
# faces each catch a different NdotL -- the cube reads red from any orbit
# angle instead of leaving five faces pitch black (single light, no IBL).
_d = (-0.45, -0.8, -0.5)
_len = math.sqrt(sum(c * c for c in _d))
_d = tuple(c / _len for c in _d)
# Quaternion rotating -Z onto _d: axis = cross(-Z, d), w = 1 + dot(-Z, d).
_f = (0.0, 0.0, -1.0)
_ax = (
    _f[1] * _d[2] - _f[2] * _d[1],
    _f[2] * _d[0] - _f[0] * _d[2],
    _f[0] * _d[1] - _f[1] * _d[0],
)
_w = 1.0 + (_f[0] * _d[0] + _f[1] * _d[1] + _f[2] * _d[2])
_qlen = math.sqrt(_ax[0] ** 2 + _ax[1] ** 2 + _ax[2] ** 2 + _w * _w)
light_rot = [_ax[0] / _qlen, _ax[1] / _qlen, _ax[2] / _qlen, _w / _qlen]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_light_import_fixture.py"},
    "extensionsUsed": ["KHR_lights_punctual"],
    "extensions": {
        "KHR_lights_punctual": {
            "lights": [
                {
                    "name": "hot_sun",
                    "type": "directional",
                    "color": [1.0, 1.0, 1.0],
                    # Blender default 3 W/m^2 sun after the exporter's x683:
                    # raw lux. Converts to 3.0, which the neutral tonemap
                    # renders as saturated red; unconverted it clips to white.
                    "intensity": 2049.0,
                }
            ]
        }
    },
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [
        {"name": "red_cube", "mesh": 0, "translation": [0.0, 0.5, 0.0]},
        {
            "name": "hot_sun",
            "rotation": light_rot,
            "extensions": {"KHR_lights_punctual": {"light": 0}},
        },
    ],
    "meshes": [
        {
            "name": "red_cube",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}
            ],
        }
    ],
    # Saturated but not primary-pure red: the green/blue floor keeps the hue
    # readable in logs, and any unit bug still clips all channels to white.
    "materials": [
        {
            "name": "light_import_red",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.8, 0.05, 0.05, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.7,
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
        {"bufferView": 2, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
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

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "light_import_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote", out, "(", len(positions), "verts,", len(indices) // 3, "tris )")
