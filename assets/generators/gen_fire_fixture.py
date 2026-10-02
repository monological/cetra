#!/usr/bin/env python3
"""Generate the fire instruments (spec 13.14): a night room in metres with a stone fireplace
burning two logs, and a candle on its mantel. Two scenes, each with its model.

    room          6 x 5 m, 3 m high, stone back wall, plaster sides, a wooden floor; nothing
                  lights it but the fire and the candle, so whatever the room shows is what
                  they cast.
    fireplace     a firebox 1.0 m wide, 0.9 m high and 0.6 m deep in a chimney breast, with
                  a FLUE 0.5 x 0.4 m rising from the back of its roof.
    logs          two, side by side, over a bed of embers whose glow follows the fire.
    hearth light  an area panel across the firebox's mouth facing the room, casting shadows:
                  the light the fire drives, its luminance set each frame so the panel's
                  intensity along its normal is the fire's.
    candle        a FLAME on the mantel's right, driving a small point light.

fire_fixture.cscn burns the hearth as a FLIPBOOK, the fire_hearth sheet played on a card standing
behind the logs. fire_grid_fixture.cscn burns it as a combustion GRID instead, 1.1 x 1.6 x 0.8 m
at 2.5 cm cells, whose obstacles are the breast, the jambs and the logs: what burns in the box
has to go up the flue or roll out of the opening, and the gate's obstacle arm reads that no heat
is found inside them.

Regenerate with: python3 assets/generators/gen_fire_fixture.py
"""

import base64
import json
import struct
from fixture_paths import asset_path, asset_ref

# A unit cube centred on the origin, one quad per face so each face carries its own normal.
# Every piece of the room is this cube, translated and scaled by its node.
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
    "fire_stone": ([0.32, 0.30, 0.28, 1.0], 0.9),
    "fire_soot": ([0.04, 0.04, 0.04, 1.0], 0.95),
    "fire_plaster": ([0.6, 0.57, 0.52, 1.0], 0.85),
    "fire_floor": ([0.28, 0.17, 0.10, 1.0], 0.6),
    "fire_log": ([0.12, 0.08, 0.05, 1.0], 0.9),
    "fire_embers": ([0.05, 0.03, 0.02, 1.0], 0.95),
    "fire_wax": ([0.85, 0.8, 0.7, 1.0], 0.5),
}

# Name -> (emissive colour, strength): the embers, at about 65 cd/m^2 as authored, which is
# what a bed of coals near 1100 K shows. A fire naming them as its embers swings them about it.
EMISSIVE = {
    "fire_embers": ([1.0, 0.32, 0.06], 150.0),
}


def box(name, material, lo, hi):
    """A piece spanning world `lo` to `hi`, as the cube's node."""
    centre = tuple((a + b) / 2 for a, b in zip(lo, hi))
    size = tuple(b - a for a, b in zip(lo, hi))
    return (name, material, centre, size, lo, hi)


# The fireplace, in metres. Its back wall's face is z = -1.7 and the breast's front z = -1.1.
WALL_Z = -1.7
BREAST_Z = -1.1
HEARTH_Y = 0.1   # the firebox floor
OPENING_Y = 1.0  # the opening's top
FLUE = (-0.25, 0.25, -1.7, -1.3)  # x0, x1, z0, z1

ROOM = [
    box("floor", "fire_floor", (-3.0, -0.1, -2.0), (3.0, 0.0, 3.0)),
    box("ceiling", "fire_plaster", (-3.0, 3.0, -2.0), (3.0, 3.1, 3.0)),
    box("wall_back", "fire_stone", (-3.0, 0.0, -2.0), (3.0, 3.0, WALL_Z)),
    box("wall_left", "fire_plaster", (-3.2, 0.0, -2.0), (-3.0, 3.0, 3.0)),
    box("wall_right", "fire_plaster", (3.0, 0.0, -2.0), (3.2, 3.0, 3.0)),
    box("hearthstone", "fire_stone", (-1.1, 0.0, -1.7), (1.1, HEARTH_Y, -0.7)),
    box("mantel", "fire_stone", (-1.15, OPENING_Y + 0.12, -1.15), (1.15, OPENING_Y + 0.2, -0.85)),
    box("candle", "fire_wax", (0.68, OPENING_Y + 0.2, -1.02), (0.72, OPENING_Y + 0.38, -0.98)),
]

# The breast and the logs, which are the fire's obstacles as well as geometry.
SOLIDS = [
    box("jamb_left", "fire_stone", (-1.0, HEARTH_Y, WALL_Z), (-0.5, OPENING_Y, BREAST_Z)),
    box("jamb_right", "fire_stone", (0.5, HEARTH_Y, WALL_Z), (1.0, OPENING_Y, BREAST_Z)),
    box("breast_left", "fire_stone", (-1.0, OPENING_Y, WALL_Z), (FLUE[0], 3.0, BREAST_Z)),
    box("breast_right", "fire_stone", (FLUE[1], OPENING_Y, WALL_Z), (1.0, 3.0, BREAST_Z)),
    box("lintel", "fire_stone", (FLUE[0], OPENING_Y, FLUE[3]), (FLUE[1], 3.0, BREAST_Z)),
    box("log_back", "fire_log", (-0.35, HEARTH_Y + 0.02, -1.52), (0.35, HEARTH_Y + 0.14, -1.40)),
    box("log_front", "fire_log", (-0.3, HEARTH_Y + 0.02, -1.36), (0.3, HEARTH_Y + 0.14, -1.24)),
]

# Inside the firebox the stone is blackened, and under the logs the coals glow.
SOOT = [box("firebox_back", "fire_soot", (-0.5, HEARTH_Y, WALL_Z), (0.5, OPENING_Y, WALL_Z + 0.01))]
EMBERS = [box("embers", "fire_embers", (-0.4, HEARTH_Y, -1.58), (0.4, HEARTH_Y + 0.04, -1.2))]

# A flipbook is flames only, so its room keeps the logs: they stand in front of the card and
# hide where its flames were cut from whatever burned under them. The grid burns the logs it is
# given, as obstacles and sources, over a bed of embers.
FILM_PIECES = ROOM + SOLIDS + SOOT
GRID_PIECES = ROOM + SOLIDS + SOOT + EMBERS

CUBE = {
    "attributes": [("POSITION", pack("<3f", positions), "VEC3", len(positions), bounds(positions)),
                   ("NORMAL", pack("<3f", [map(float, n) for n in normals]), "VEC3",
                    len(normals), None)],
    "indices": (pack("<H", [(i,) for i in indices]), len(indices)),
}


def gltf_of(pieces):
    """The pieces as a glTF: the cube packed once, one mesh per material, a node per piece."""
    data, buffer_views, accessors = b"", [], []
    attributes = {}
    chunks = [(name, blob, kind, count, b, 5126, 34962)
              for name, blob, kind, count, b in CUBE["attributes"]]
    chunks.append((None, CUBE["indices"][0], "SCALAR", CUBE["indices"][1], None, 5123, 34963))
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
              "scale": list(size)} for name, material, centre, size, _, _ in pieces]
    materials_out = [{"name": m, "pbrMetallicRoughness": {
        "baseColorFactor": MATERIALS[m][0], "metallicFactor": 0.0,
        "roughnessFactor": MATERIALS[m][1]}} for m in materials]
    for out in materials_out:
        if out["name"] in EMISSIVE:
            colour, strength = EMISSIVE[out["name"]]
            out["emissiveFactor"] = colour
            out["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": strength}}
    return {
        "asset": {"version": "2.0", "generator": "gen_fire_fixture.py"},
        "extensionsUsed": ["KHR_materials_emissive_strength"],
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


def log_source(lo, hi):
    """Where a log burns: along its top, a little over half of it alight at once."""
    y = hi[1] + 0.01
    z = (lo[2] + hi[2]) / 2
    return {"shape": "capsule", "from": [lo[0] + 0.05, y, z], "to": [hi[0] - 0.05, y, z],
            "radius": 0.05, "coverage": 0.7, "lift": 0.8}


LOGS = [p for p in SOLIDS if p[0].startswith("log")]

# The flipbook's three files share a stem, which routes like the sheet beside it.
FLIPBOOK = asset_ref("fire_hearth_color.png")[:-len("_color.png")]

# Filmed fire, on a card standing in the middle of the firebox. The film's frame is 0.9 m across
# at the scale it was cut to, which is the firebox's width less its jambs' reveal.
HEARTH = {
    "name": "hearth",
    "kind": "flipbook",
    "flipbook": FLIPBOOK,
    "cards": [{"base": [0.0, HEARTH_Y, -1.4], "size": [0.9, 0.707], "phase": 0.0}],
    "light": "hearth_light",
}
HEARTH_GRID = {
    "name": "hearth",
    "kind": "grid",
    "center": [0.0, 0.9, -1.3],
    "size": [1.1, 1.6, 0.8],
    "cell": 0.025,
    "floor": True,
    "light": "hearth_light",
    "embers": "fire_embers",
    "sources": [log_source(p[4], p[5]) for p in LOGS],
    "obstacles": [{"min": list(p[4]), "max": list(p[5])} for p in SOLIDS],
    # The flue draws at a little over a metre a second, a domestic chimney's order once warm:
    # that is what pulls the room's air in through the mouth and the smoke up, not out.
    "draft": {"min": [FLUE[0], OPENING_Y, FLUE[2]], "max": [FLUE[1], 3.0, FLUE[3]],
              "speed": 1.2},
}
CANDLE = {
    "name": "candle",
    "kind": "flame",
    "center": [0.70, OPENING_Y + 0.40, -1.0],
    "size": [0.012, 0.035, 0.012],
    "light": "candle_light",
}

def room_scene(model, hearth, comment):
    """The room around one hearth fire and the candle."""
    return {
        "version": 1,
        "_comment": comment + [
            "The candle is a structural flame driving a small point light. Every light's",
            "intensity here is 0 because the fires write it each frame.",
            "",
            "Regenerate with: python3 assets/generators/gen_fire_fixture.py",
        ],
        "models": [{"path": asset_ref(model)}],
        "environment": {"ambient": [0.0, 0.0, 0.0]},
        "lights": [
            {"name": "hearth_light", "type": "area", "position": [0.0, 0.55, BREAST_Z + 0.02],
             "direction": [0.0, 0.0, 1.0], "size": [1.0, 0.9], "color": [1.0, 0.6, 0.3],
             "intensity": 0.0, "cast_shadows": True},
            {"name": "candle_light", "type": "point", "position": list(CANDLE["center"]),
             "color": [1.0, 0.6, 0.3], "intensity": 0.0, "range": 6.0, "cast_shadows": False},
        ],
        "fire": {"fires": [hearth, CANDLE]},
        "camera": {"eye": [0.6, 1.25, 2.4], "target": [0.0, 0.65, -1.3], "fov": 50},
        "post": {"tonemap": "neutral", "exposure": 0.25},
    }


scene_desc = room_scene("fire_fixture.gltf", HEARTH, [
    "The fire instrument (spec 13.14): a night room lit only by a fire in its stone fireplace",
    "and a candle on the mantel. The fire is a flipbook, the fire_hearth sheet on a card behind",
    "the logs, driving the area light across the firebox's mouth.",
])
grid_desc = room_scene("fire_grid_fixture.gltf", HEARTH_GRID, [
    "The fire instrument's GRID twin (spec 13.14): the same room with its hearth burning as a",
    "combustion grid whose obstacles are the chimney breast and the logs. It drives the same",
    "area light and embers.",
])


def write_json(name, obj):
    with open(asset_path(name), "w") as f:
        json.dump(obj, f, indent=1)
        f.write("\n")


write_json("fire_fixture.gltf", gltf_of(FILM_PIECES))
write_json("fire_grid_fixture.gltf", gltf_of(GRID_PIECES))
write_json("fire_fixture.cscn", scene_desc)
write_json("fire_grid_fixture.cscn", grid_desc)
print("wrote fire_fixture and fire_grid_fixture (.gltf + .cscn)")
