#!/usr/bin/env python3
"""Generate the soft-shadow core instrument (spec 13.16): a light with a body standing just
past an occluder, over a wall, so the size of the full shadow is a number on the wall.

    wall      a grey plane facing +Z at z = 0, 1.6 m square.
    rim       a slab 2.4 cm square and 2 cm deep, its far face 40 cm from the wall: a candle's
              top, which is what stands this close to a flame.
    light     a point light whose body is a capsule 3.5 cm long overall and 6 mm round,
              pointing away from the wall and standing 2 mm clear of the rim, as a candle's
              flame stands off its wax: a taper's flame, laid on its side so the camera can
              look straight at the wall. Cached, so its shadow is the tiles'.

From the flame's centre the rim hides the wall to about 26 cm from the axis, and from its tip
to about 9 cm. A lookup that sees the flame from one point puts the edge of the full shadow at
the first; the shadow itself has it nearer the second. core_truth.py traces the same body
against the rim exactly, with no shadow map, and is what both are measured against.

The camera is orthographic in practice: tile_core_fixture renders with --ortho 0.8, so a
pixel is a fixed fraction of a metre across the wall, and --tonemap linear, which a scene file
cannot ask for, so a pixel's value divided by its unshadowed twin's is how much light reaches.

Regenerate with: python3 assets/generators/gen_tile_core_fixture.py
"""

import base64
import json
import struct
from fixture_paths import asset_path, asset_ref

# A unit cube centred on the origin, one quad per face so each face carries its own normal.
FACES = [
    ((1, 0, 0), [(0.5, -0.5, 0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (0.5, 0.5, 0.5)]),
    ((-1, 0, 0), [(-0.5, -0.5, -0.5), (-0.5, -0.5, 0.5), (-0.5, 0.5, 0.5), (-0.5, 0.5, -0.5)]),
    ((0, 1, 0), [(-0.5, 0.5, 0.5), (0.5, 0.5, 0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5)]),
    ((0, -1, 0), [(-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, -0.5, 0.5), (-0.5, -0.5, 0.5)]),
    ((0, 0, 1), [(-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5)]),
    ((0, 0, -1), [(0.5, -0.5, -0.5), (-0.5, -0.5, -0.5), (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5)]),
]
positions, normals, indices = [], [], []
for n, quad in FACES:
    base = len(positions)
    positions += quad
    normals += [n] * 4
    indices += [base, base + 1, base + 2, base, base + 2, base + 3]


def pack(fmt, rows):
    return b"".join(struct.pack(fmt, *row) for row in rows)


def bounds(points):
    return ([min(p[i] for p in points) for i in range(3)],
            [max(p[i] for p in points) for i in range(3)])


# Name -> (base colour, roughness).
MATERIALS = {
    "core_wall": ([0.7, 0.7, 0.7, 1.0], 0.9),
    "core_wax": ([0.85, 0.8, 0.7, 1.0], 0.5),
}


def box(name, material, lo, hi):
    """A piece spanning world `lo` to `hi`, as the cube's node."""
    centre = tuple((a + b) / 2 for a, b in zip(lo, hi))
    size = tuple(b - a for a, b in zip(lo, hi))
    return (name, material, centre, size)


CENTRE_Y = 1.0
RIM_HALF = 0.012         # the rim's half-width
RIM_FAR, RIM_NEAR = 0.38, 0.40  # its faces' distances from the wall
# The flame's body: the length between its caps, its radius, and its clearance off the rim.
FLAME_LENGTH, FLAME_RADIUS, FLAME_CLEAR = 0.023, 0.006, 0.002
LIGHT_Z = RIM_NEAR + FLAME_CLEAR + FLAME_RADIUS + FLAME_LENGTH / 2

PIECES = [
    box("wall", "core_wall", (-0.8, CENTRE_Y - 0.8, -0.05), (0.8, CENTRE_Y + 0.8, 0.0)),
    box("rim", "core_wax", (-RIM_HALF, CENTRE_Y - RIM_HALF, RIM_FAR),
        (RIM_HALF, CENTRE_Y + RIM_HALF, RIM_NEAR)),
]

CUBE_ATTRIBUTES = [
    ("POSITION", pack("<3f", positions), "VEC3", len(positions), bounds(positions)),
    ("NORMAL", pack("<3f", [map(float, n) for n in normals]), "VEC3", len(normals), None),
]
CUBE_INDICES = (pack("<H", [(i,) for i in indices]), len(indices))


def gltf_of(pieces):
    """The pieces as a glTF: the cube packed once, one mesh per material, a node per piece."""
    data, buffer_views, accessors = b"", [], []
    attributes = {}
    chunks = [(name, blob, kind, count, b, 5126, 34962)
              for name, blob, kind, count, b in CUBE_ATTRIBUTES]
    chunks.append((None, CUBE_INDICES[0], "SCALAR", CUBE_INDICES[1], None, 5123, 34963))
    for name, blob, kind, count, b, component, target in chunks:
        buffer_views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(blob),
                             "target": target})
        data += blob
        accessor = {"bufferView": len(buffer_views) - 1, "componentType": component,
                    "count": count, "type": kind}
        if b:
            accessor["min"], accessor["max"] = b
        accessors.append(accessor)
        if name:
            attributes[name] = len(accessors) - 1
    index = len(accessors) - 1

    materials = []
    for _, material, *_ in pieces:
        if material not in materials:
            materials.append(material)
    meshes = [{"name": m, "primitives": [{"attributes": attributes, "indices": index,
                                          "material": i}]} for i, m in enumerate(materials)]
    nodes = [{"name": name, "mesh": materials.index(material), "translation": list(centre),
              "scale": list(size)} for name, material, centre, size in pieces]
    materials_out = [{"name": m, "pbrMetallicRoughness": {
        "baseColorFactor": MATERIALS[m][0], "metallicFactor": 0.0,
        "roughnessFactor": MATERIALS[m][1]}} for m in materials]
    return {
        "asset": {"version": "2.0", "generator": "gen_tile_core_fixture.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials_out,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"uri": "data:application/octet-stream;base64,"
                     + base64.b64encode(data).decode("ascii"), "byteLength": len(data)}],
    }


scene_desc = {
    "version": 1,
    "_comment": [
        "The soft-shadow core instrument (spec 13.16): a flame-sized light 2 cm past a",
        "candle-sized rim, 40 cm from a wall. Render with --ortho 0.8 --tonemap linear to",
        "read the full shadow's radius off the wall in pixels.",
        "",
        "Regenerate with: python3 assets/generators/gen_tile_core_fixture.py",
    ],
    "models": [{"path": asset_ref("tile_core_fixture.gltf")}],
    "environment": {"ambient": [0.0, 0.0, 0.0]},
    "lights": [
        {"name": "flame", "type": "point", "position": [0.0, CENTRE_Y, LIGHT_Z],
         "direction": [0.0, 0.0, 1.0], "color": [1.0, 1.0, 1.0], "intensity": 0.5,
         "range": 3.0, "cast_shadows": True, "shadow_cache": True,
         "source_radius": FLAME_RADIUS, "source_length": FLAME_LENGTH},
    ],
    "camera": {"eye": [0.0, CENTRE_Y, 2.0], "target": [0.0, CENTRE_Y, 0.0], "fov": 30},
    "post": {"exposure": 1.0},
}


def write_json(name, obj):
    with open(asset_path(name), "w") as f:
        json.dump(obj, f, indent=1)
        f.write("\n")


write_json("tile_core_fixture.gltf", gltf_of(PIECES))
write_json("tile_core_fixture.cscn", scene_desc)
print("wrote tile_core_fixture (.gltf + .cscn)")
