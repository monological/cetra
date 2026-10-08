#!/usr/bin/env python3
"""Generate the cached-shadow rounding instrument (spec 13.31): a wall squarely facing a cached
light with no body, half a metre off it, far from the world's origin.

    wall      a grey plane facing +Z, 1.6 m square, its centre 50 m out along x and along z.
    light     a point light with no body, 0.49 m in front of the wall's centre, its near plane
              5 cm, cached. Nothing stands between it and the wall.

The middle of the wall is in the light's -Z face, squarely facing it, so the face stores one
depth across all of it. A face is drawn through a matrix carrying the light's world position,
which rounds at this distance from the origin by more than the depth test's floor allows, so a
lookup with no allowance for that rounding finds the whole middle of the wall behind itself at
some light positions and not others. The shadow-tiles gate group moves the light by fractions
of a millimetre and reads the middle of the wall against its unshadowed twin. Render with
--no-recenter, or the viewer moves the wall back to the origin.

Regenerate with: python3 assets/generators/gen_tile_rounding_fixture.py
"""

import base64
import json
import struct
from fixture_paths import asset_path, asset_ref

FAR = 50.0      # the wall's centre, along x and along z
CENTRE_Y = 1.0
THROW = 0.49    # the light's distance in front of the wall
NEAR = 0.05     # the light's near plane

# The wall: one box, its front face at z = FAR, a quad per face so each carries its own normal.
LO = (FAR - 0.8, CENTRE_Y - 0.8, FAR - 0.05)
HI = (FAR + 0.8, CENTRE_Y + 0.8, FAR)
FACES = [
    ((1, 0, 0), [(1, 0, 1), (1, 0, 0), (1, 1, 0), (1, 1, 1)]),
    ((-1, 0, 0), [(0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)]),
    ((0, 1, 0), [(0, 1, 1), (1, 1, 1), (1, 1, 0), (0, 1, 0)]),
    ((0, -1, 0), [(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)]),
    ((0, 0, 1), [(0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]),
    ((0, 0, -1), [(1, 0, 0), (0, 0, 0), (0, 1, 0), (1, 1, 0)]),
]
positions, normals, indices = [], [], []
for n, quad in FACES:
    base = len(positions)
    positions += [tuple(HI[i] if c[i] else LO[i] for i in range(3)) for c in quad]
    normals += [n] * 4
    indices += [base, base + 1, base + 2, base, base + 2, base + 3]


def pack(fmt, rows):
    return b"".join(struct.pack(fmt, *row) for row in rows)


def accessor_chunks():
    pos = pack("<3f", positions)
    nrm = pack("<3f", [map(float, n) for n in normals])
    idx = pack("<H", [(i,) for i in indices])
    lo = [min(p[i] for p in positions) for i in range(3)]
    hi = [max(p[i] for p in positions) for i in range(3)]
    return [(pos, 5126, 34962, len(positions), "VEC3", (lo, hi)),
            (nrm, 5126, 34962, len(normals), "VEC3", None),
            (idx, 5123, 34963, len(indices), "SCALAR", None)]


def gltf():
    data, buffer_views, accessors = b"", [], []
    for blob, component, target, count, kind, b in accessor_chunks():
        buffer_views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(blob),
                             "target": target})
        data += blob
        accessor = {"bufferView": len(buffer_views) - 1, "componentType": component,
                    "count": count, "type": kind}
        if b:
            accessor["min"], accessor["max"] = b
        accessors.append(accessor)
    return {
        "asset": {"version": "2.0", "generator": "gen_tile_rounding_fixture.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "wall", "mesh": 0}],
        "meshes": [{"name": "wall", "primitives": [
            {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
        "materials": [{"name": "rounding_wall", "pbrMetallicRoughness": {
            "baseColorFactor": [0.7, 0.7, 0.7, 1.0], "metallicFactor": 0.0,
            "roughnessFactor": 0.9}}],
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"uri": "data:application/octet-stream;base64,"
                     + base64.b64encode(data).decode("ascii"), "byteLength": len(data)}],
    }


scene_desc = {
    "version": 1,
    "_comment": [
        "The cached-shadow rounding instrument (spec 13.31): a wall squarely facing a cached",
        "light with no body, half a metre off it, 50 m out along x and z. Render with",
        "--no-recenter; nothing stands between the light and the wall.",
        "",
        "Regenerate with: python3 assets/generators/gen_tile_rounding_fixture.py",
    ],
    "models": [{"path": asset_ref("tile_rounding_fixture.gltf")}],
    "environment": {"ambient": [0.0, 0.0, 0.0]},
    "lights": [
        {"name": "lamp", "type": "point", "position": [FAR, CENTRE_Y, FAR + THROW],
         "color": [1.0, 1.0, 1.0], "intensity": 0.5, "range": 6.0, "cast_shadows": True,
         "shadow_cache": True, "shadow_near": NEAR},
    ],
    "camera": {"eye": [FAR, CENTRE_Y, FAR + 2.0], "target": [FAR, CENTRE_Y, FAR], "fov": 30},
    "post": {"exposure": 1.0},
}


def write_json(name, obj):
    with open(asset_path(name), "w") as f:
        json.dump(obj, f, indent=1)
        f.write("\n")


write_json("tile_rounding_fixture.gltf", gltf())
write_json("tile_rounding_fixture.cscn", scene_desc)
print("wrote tile_rounding_fixture (.gltf + .cscn)")
