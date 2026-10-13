#!/usr/bin/env python3
"""Generate the light index pool's two instruments (spec 13.51): lights crowded round the camera,
each reaching far past it, so every one claims every froxel out to its range.

    light_pool_fit    16 lights, needing more slots than a 16 KB pool holds and fewer than 64 KB
    light_pool_over   48 lights, needing more than 64 KB holds

A light whose sphere holds the eye touches every tile of every slice out to its range, which is
what spends the pool: the count of such lights sets the demand, nearly linearly. The geometry is
lod_fixture's ground and spheres, borrowed, since only the lights matter. Nothing casts a shadow.

Regenerate with: python3 assets/generators/gen_light_pool_fixture.py
"""

import json
import math
from fixture_paths import asset_path, asset_ref

EYE = [0.0, 3.0, 8.0]
TARGET = [0.0, 0.0, 0.0]
RING = 0.5     # the lights' circle round the eye, metres
RANGE = 40.0   # each light's reach, far past the eye, so each one's sphere holds it


def scene(count, comment):
    lights = []
    for i in range(count):
        a = 2.0 * math.pi * i / count
        lights.append({"name": f"pool_{i:02d}", "type": "point",
                       "position": [EYE[0] + RING * math.cos(a), EYE[1],
                                    EYE[2] + RING * math.sin(a)],
                       "color": [1.0, 1.0, 1.0], "intensity": 0.05, "range": RANGE,
                       "cast_shadows": False})
    return {
        "version": 1,
        "_comment": [comment, "", "Regenerate with: python3 assets/generators/gen_light_pool_fixture.py"],
        "models": [{"path": asset_ref("lod_fixture.gltf")}],
        "lights": lights,
        "camera": {"eye": EYE, "target": TARGET, "fov": 50},
        "post": {"exposure": 1.0},
    }


def write_json(name, obj):
    with open(asset_path(name), "w") as f:
        json.dump(obj, f, indent=1)
        f.write("\n")


write_json("light_pool_fit.cscn", scene(
    16, "16 lights round the eye: more index slots than a 16 KB pool holds, fewer than 64 KB (spec 13.51)."))
write_json("light_pool_over.cscn", scene(
    48, "48 lights round the eye: more index slots than the 64 KB pool holds (spec 13.51)."))
print("wrote light_pool_fit.cscn and light_pool_over.cscn")
