#!/usr/bin/env python3
"""Generate assets/local_exposure_fixture.gltf -- a window in a wall, for spec 13.19.

A grey wall lit evenly and head-on, exposed so it sits at middle grey, and in front of it a
flat emissive panel -- the WINDOW -- about six stops brighter, which a global exposure can
only clip. Everything a local exposure arm reads is a property of this picture rather than
of a room's clutter:

  - the window is ONE luminance, so how far its base comes down is a number, and whether it
    still clips is a count;
  - the wall is ONE luminance, so a halo -- the wall beside the window darkened because the
    window's light leaked into the wall's base -- reads as the wall near the edge against the
    wall far from it, and a pure blur of the frame, which the bilateral grid exists to beat,
    is what produces one.

No bloom (the post block turns it off) and no environment, so nothing but the two surfaces
and the light reaches the frame.

Regenerate with: python3 assets/generators/gen_local_exposure_fixture.py
"""

import base64
import json
import struct
from fixture_paths import asset_path, asset_ref

FOV_DEG = 50.0
CAM_Z = 4.0

WALL_HALF = (4.0, 3.0)
WINDOW_HALF = (1.0, 0.6)
# In front of the wall by enough that the depth test never mixes them.
WINDOW_Z = 0.01

ALBEDO = 0.5
SUN_INTENSITY = 3.0
# The wall's exposed radiance, albedo / pi * intensity times this, at 0.18: middle grey.
EXPOSURE = 0.18 / (ALBEDO / 3.14159265 * SUN_INTENSITY)
# The window's radiance: about six stops over the wall's 0.477, so 64 times it.
WINDOW_STRENGTH = 32.0


def quad(half, z):
    hx, hy = half
    return [(-hx, -hy, z), (hx, -hy, z), (hx, hy, z), (-hx, hy, z)]


# Wound counter-clockwise seen from +Z, so the face normal is +Z and the camera sees it.
INDICES = [0, 1, 2, 0, 2, 3]

wall = quad(WALL_HALF, 0.0)
window = quad(WINDOW_HALF, WINDOW_Z)
normals = [(0.0, 0.0, 1.0)] * 4
uvs = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]


def pack3(pts):
    return b"".join(struct.pack("<3f", *p) for p in pts)


chunks = [
    (pack3(wall), 34962),
    (pack3(window), 34962),
    (pack3(normals), 34962),
    (b"".join(struct.pack("<2f", *t) for t in uvs), 34962),
    (b"".join(struct.pack("<H", i) for i in INDICES), 34963),
]
buffer_bytes = b"".join(c for c, _ in chunks)

views, offset = [], 0
for data, target in chunks:
    views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data), "target": target})
    offset += len(data)


def bounds(pts):
    return ([min(p[i] for p in pts) for i in range(3)],
            [max(p[i] for p in pts) for i in range(3)])


wall_mn, wall_mx = bounds(wall)
win_mn, win_mx = bounds(window)

gltf = {
    "asset": {"version": "2.0", "generator": "gen_local_exposure_fixture.py"},
    "extensionsUsed": ["KHR_materials_emissive_strength"],
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [{"name": "wall", "mesh": 0}, {"name": "window", "mesh": 1}],
    "meshes": [
        {"name": "wall", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 2,
                                                         "TEXCOORD_0": 3},
                                          "indices": 4, "material": 0}]},
        {"name": "window", "primitives": [{"attributes": {"POSITION": 1, "NORMAL": 2,
                                                           "TEXCOORD_0": 3},
                                            "indices": 4, "material": 1}]},
    ],
    "materials": [
        {"name": "le_wall",
         "pbrMetallicRoughness": {"baseColorFactor": [ALBEDO, ALBEDO, ALBEDO, 1.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 1.0}},
        {"name": "le_window",
         "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 1.0},
         "emissiveFactor": [1.0, 1.0, 1.0],
         "extensions": {"KHR_materials_emissive_strength":
                        {"emissiveStrength": WINDOW_STRENGTH}}},
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": wall_mn, "max": wall_mx},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": win_mn, "max": win_mx},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 4, "componentType": 5123, "count": len(INDICES), "type": "SCALAR"},
    ],
    "bufferViews": views,
    "buffers": [{"uri": "data:application/octet-stream;base64," +
                        base64.b64encode(buffer_bytes).decode("ascii"),
                 "byteLength": len(buffer_bytes)}],
}

cscn = {
    "version": 1,
    "models": [{"path": asset_ref("local_exposure_fixture.gltf")}],
    "lights": [{"name": "WallSun", "type": "directional", "direction": [0.0, 0.0, -1.0],
                "color": [1.0, 1.0, 1.0], "intensity": SUN_INTENSITY, "cast_shadows": False}],
    "camera": {"eye": [0.0, 0.0, CAM_Z], "target": [0.0, 0.0, 0.0], "fov": FOV_DEG},
    "post": {"tonemap": "neutral", "exposure": EXPOSURE, "auto_exposure": False,
             "bloom": {"enabled": False}},
}

with open(asset_path("local_exposure_fixture.gltf"), "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
with open(asset_path("local_exposure_fixture.cscn"), "w") as f:
    json.dump(cscn, f, indent=1)
    f.write("\n")

print(f"exposure {EXPOSURE:.6f}: the wall at middle grey, the window {WINDOW_STRENGTH:g} "
      f"radiance, {WINDOW_STRENGTH / (ALBEDO / 3.14159265 * SUN_INTENSITY):.1f}x the wall")
