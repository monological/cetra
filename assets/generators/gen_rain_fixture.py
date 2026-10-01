#!/usr/bin/env python3
"""Generate rain_fixture.gltf + .cscn, the rain instrument, rain_water_fixture.cscn, its
flooded variant for the rings rain leaves on water (spec 13.9), rain_glass_fixture.gltf
+ .cscn, its glazed variant for the drops rain leaves on glass, and rain_relief_fixture.gltf
+ .cscn, its variant whose ground carries a height map for the puddles to find (spec 13.12).

A night yard in metres, built so that every question the rain gate asks has an answer
known from the geometry alone:

    roof on posts     8 x 6 m at 3 m, over BOTH ground halves. Rain that falls on it
                      never reaches the ground beneath, so a point under its middle
                      must stay dry while one in the open wets -- on either material.
    ground halves     the same grey at x < 0 and x > 0, one POROUS and one SEALED. A
                      porous surface darkens as it wets and a sealed one only takes a
                      film, so the pair isolates porosity with colour held fixed.
    wall              facing +z, into the wind. Slanted rain reaches a vertical face
                      the wind drives it at, and nothing else here does.
    lamp              an IES downlight on a pole beside the roof, so the rain has
                      something to be seen against at night.

The wind is light and blows toward the wall (-z), so the roof's dry patch is displaced
by (roof height) x (wind / fall speed) toward the wall -- 0.84 m at the rate authored
here. Most of the gate's points sit well inside or outside the patch; two sit just
either side of the roof's own footprint, where that shift decides the answer, and are
what hold the map to the rain's direction.

Everything the probe prints is a function of the rate, and the physics arms hold it
against closed forms written out in gates.py rather than read back from here.

Regenerate with: python3 assets/generators/gen_rain_fixture.py
"""

import base64
import json
import struct
import zlib
from fixture_paths import asset_path, asset_ref

# A unit cube centred on the origin, one quad per face so each face carries its own
# normal. Every piece of the yard but the relief twin's ground is this cube, translated and
# scaled by its node.
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


def bounds(points):
    """A POSITION accessor's min and max."""
    return ([min(p[i] for p in points) for i in range(3)],
            [max(p[i] for p in points) for i in range(3)])


def pack(fmt, rows):
    return b"".join(struct.pack(fmt, *row) for row in rows)


# A geometry is its attributes -- (name, bytes, type, count, bounds or None) -- and its indices.
CUBE = {
    "attributes": [("POSITION", pack("<3f", positions), "VEC3", len(positions), bounds(positions)),
                   ("NORMAL", pack("<3f", [map(float, n) for n in normals]), "VEC3",
                    len(normals), None)],
    "indices": (pack("<H", [(i,) for i in indices]), len(indices)),
}

# Name -> (base colour, roughness[, albedo image]).
GREY = [0.35, 0.35, 0.35, 1.0]
MATERIALS = {
    "rain_porous": (GREY, 0.85),
    "rain_sealed": (GREY, 0.85),
    "rain_roof": ([0.12, 0.12, 0.13, 1.0], 0.7),
    "rain_post": ([0.3, 0.28, 0.25, 1.0], 0.8),
    "rain_wall": ([0.45, 0.28, 0.22, 1.0], 0.9),
    "rain_glass": ([0.9, 0.95, 0.92, 1.0], 0.05),
    "rain_relief": (GREY, 0.85, "rain_relief_albedo.png"),
}

# The yard, in metres: (name, material, centre, size[, geometry]), a cube when no geometry is
# given.
ROOF_Y = 3.0  # underside; the slab spans x -4..4 and z -7..-1
GROUNDS = [
    ("ground_porous", "rain_porous", (-5.0, -0.1, 0.0), (10.0, 0.2, 24.0)),
    ("ground_sealed", "rain_sealed", (5.0, -0.1, 0.0), (10.0, 0.2, 24.0)),
]
YARD = [
    ("roof", "rain_roof", (0.0, ROOF_Y + 0.1, -4.0), (8.0, 0.2, 6.0)),
    ("post_a", "rain_post", (-3.8, ROOF_Y / 2, -1.2), (0.2, ROOF_Y, 0.2)),
    ("post_b", "rain_post", (3.8, ROOF_Y / 2, -1.2), (0.2, ROOF_Y, 0.2)),
    ("post_c", "rain_post", (-3.8, ROOF_Y / 2, -6.8), (0.2, ROOF_Y, 0.2)),
    ("post_d", "rain_post", (3.8, ROOF_Y / 2, -6.8), (0.2, ROOF_Y, 0.2)),
    ("wall", "rain_wall", (0.0, 2.0, -11.5), (20.0, 4.0, 0.4)),
    ("lamp_pole", "rain_post", (6.5, 2.2, -3.0), (0.12, 4.4, 0.12)),
]
PIECES = GROUNDS + YARD

# The GLASS twin's panes (spec 13.12), thin -- 6 mm. Two stand facing +z, the side the rain
# strikes: one in the open and one under the middle of the roof. The third lies FLAT, a glass
# canopy in the open between the roof and the camera: it shelters the ground under it as any
# roof does, and seen from below shows the drops on its top. Kept to x and z ranges of their
# own so each reads alone from the fixture's camera.
PANES = [
    ("pane_open", "rain_glass", (2.0, 1.05, 4.0), (2.0, 1.5, 0.006)),
    ("pane_covered", "rain_glass", (-2.0, 1.05, -4.0), (2.0, 1.5, 0.006)),
    ("pane_canopy", "rain_glass", (0.0, 2.4, 1.0), (2.0, 0.006, 2.0)),
]


# The RELIEF twin's ground (spec 13.12): one plane in place of the two halves, at the same
# height, carrying texture coordinates so a height map can lie on it. The map repeats every
# RELIEF_PERIOD metres along z and is constant along x: a TRENCH across the middle half of each
# repeat and a PLATEAU either side. The band is symmetric about the repeat's centre, so it
# lands in the same place whichever way an importer flips V: trenches centred on z = 2 + 4k,
# plateaus on z = 4k. Its mean is exactly a half.
RELIEF_PERIOD = 4.0
RELIEF_MAP = 64
# The engine pairs a material with `<albedo stem>_height.png` beside its albedo, and only that
# way: a .cscn has no height key.
RELIEF_HEIGHT = "rain_relief_height.png"
RELIEF_CORNERS = [(-10.0, 12.0), (10.0, 12.0), (10.0, -12.0), (-10.0, -12.0)]  # CCW from above
RELIEF_POSITIONS = [(x, 0.0, z) for x, z in RELIEF_CORNERS]
PLANE = {
    "attributes": [
        ("POSITION", pack("<3f", RELIEF_POSITIONS), "VEC3", 4, bounds(RELIEF_POSITIONS)),
        ("NORMAL", pack("<3f", [(0.0, 1.0, 0.0)] * 4), "VEC3", 4, None),
        ("TEXCOORD_0", pack("<2f", [(x / RELIEF_PERIOD, z / RELIEF_PERIOD)
                                    for x, z in RELIEF_CORNERS]), "VEC2", 4, None),
    ],
    "indices": (pack("<H", [(i,) for i in (0, 1, 2, 0, 2, 3)]), 6),
}
RELIEF_GROUND = ("ground_relief", "rain_relief", None, None, PLANE)


def png_grey(size, rows):
    """A minimal 8-bit RGB PNG, square: rows[y] is one grey level per row."""
    raw = b"".join(b"\x00" + bytes((v, v, v)) * size for v in rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def gltf_of(pieces):
    """The pieces as a glTF: each geometry packed once, one mesh per geometry and material, and
    the materials -- with their images -- in the order the pieces first use them."""
    geometries, materials, meshes, mesh_of = [], [], [], {}
    for _, material, _, _, *geometry in pieces:
        geometry = geometry[0] if geometry else CUBE
        if not any(g is geometry for g in geometries):
            geometries.append(geometry)
        if material not in materials:
            materials.append(material)

    data, buffer_views, accessors, primitive_of = b"", [], [], []
    for geometry in geometries:
        chunks = [(name, blob, kind, count, box, 5126, 34962)
                  for name, blob, kind, count, box in geometry["attributes"]]
        chunks.append((None, geometry["indices"][0], "SCALAR", geometry["indices"][1], None, 5123,
                       34963))
        attributes = {}
        for name, blob, kind, count, box, component, target in chunks:
            buffer_views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(blob),
                                 "target": target})
            data += blob
            accessor = {"bufferView": len(buffer_views) - 1, "componentType": component,
                        "count": count, "type": kind}
            if box:
                accessor["min"], accessor["max"] = box
            accessors.append(accessor)
            if name:
                attributes[name] = len(accessors) - 1
        primitive_of.append((attributes, len(accessors) - 1))

    nodes = []
    for name, material, t, s, *geometry in pieces:
        g = next(i for i, x in enumerate(geometries) if x is (geometry[0] if geometry else CUBE))
        if (g, material) not in mesh_of:
            attributes, index = primitive_of[g]
            mesh_of[(g, material)] = len(meshes)
            meshes.append({"name": material,
                           "primitives": [{"attributes": attributes, "indices": index,
                                           "material": materials.index(material)}]})
        node = {"name": name, "mesh": mesh_of[(g, material)]}
        if t:
            node["translation"], node["scale"] = list(t), list(s)
        nodes.append(node)

    materials_out, images = [], []
    for name in materials:
        color, rough, *image = MATERIALS[name]
        pbr = {"baseColorFactor": color, "metallicFactor": 0.0, "roughnessFactor": rough}
        if image:
            pbr["baseColorTexture"] = {"index": len(images)}
            images.append(image[0])
        materials_out.append({"name": name, "pbrMetallicRoughness": pbr})
    extra = {}
    if images:
        extra = {"textures": [{"source": i} for i in range(len(images))],
                 "images": [{"uri": asset_ref(image)} for image in images]}
    return {
        "asset": {"version": "2.0", "generator": "gen_rain_fixture.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials_out,
        **extra,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [
            {
                "uri": "data:application/octet-stream;base64,"
                + base64.b64encode(data).decode("ascii"),
                "byteLength": len(data),
            }
        ],
    }

RATE_MMH = 10.0
WIND = [0.0, 0.0, -1.5]
scene_desc = {
    "version": 1,
    "_comment": [
        "The rain instrument (spec 13.9): a night yard in metres with a roof on posts over",
        "both halves of a porous / sealed ground, a wall facing the wind and an IES lamp.",
        "",
        "The rain is SETTLED (the default), so the world is already as wet as this rate",
        "makes it and a headless frame does not depend on how many frames it waited.",
    ],
    "models": [{"path": asset_ref("rain_fixture.gltf")}],
    "environment": {"ambient": [0.03, 0.03, 0.035]},
    "lights": [
        {
            "name": "rain_lamp",
            "type": "point",
            "position": [6.5, 4.3, -3.0],
            "color": [1.0, 0.85, 0.6],
            "range": 14.0,
            "profile": asset_ref("ies_symmetric.ies"),
        }
    ],
    "rain": {"rate": RATE_MMH, "wind": WIND},
    # Stated rather than derived from the roughness, which is the same on both halves: the
    # pair exists to isolate porosity, so each half says what it is.
    "materials": {"rain_porous": {"porosity": 1.0}, "rain_sealed": {"porosity": 0.0}},
    "camera": {"eye": [0.0, 2.4, 11.0], "target": [0.0, 1.2, -4.0], "fov": 55},
    "post": {"tonemap": "neutral", "exposure": 1.0},
}

# The WATER variant: the same yard flooded a few centimetres deep with a calm surface, so the
# rings the rain leaves on water are the only thing moving it. The swell is kept long and
# small -- a pond, not a sea -- because a wave the rings ride on would hide how they read.
water_desc = dict(scene_desc)
water_desc["_comment"] = [
    "The rain instrument's water variant (spec 13.9): the yard flooded 5 cm deep under a",
    "near-still surface, for the rings rain leaves on water. Same camera, lights and rain.",
]
water_desc["water"] = {"level": 0.05, "extent": 14.0, "waves": "gerstner", "wavelength": 8.0,
                       "amplitude": 0.002, "steepness": 0.1}

# The GLASS variant (spec 13.12): the same yard with three thin panes, for the drops rain
# leaves on glass. The panes are thin transmissive glass, so what is seen through one is the
# refraction resolve at its own pixel, shifted only by the drops; the yard and the lamp give
# that resolve something to show. Seen from in front of the open pane, low enough that the
# covered one shows under the roof.
glass_desc = dict(scene_desc)
glass_desc["_comment"] = [
    "The rain instrument's glass variant (spec 13.12): the yard with three thin panes -- one",
    "facing the wind in the open, one facing it under the roof, and a flat canopy in the open",
    "-- for the drops rain leaves on glass. Same lights and rain; a camera on the panes.",
]
glass_desc["models"] = [{"path": asset_ref("rain_glass_fixture.gltf")}]
glass_desc["camera"] = {"eye": [0.5, 1.6, 8.0], "target": [0.5, 1.4, -2.0], "fov": 55}
glass_desc["materials"] = dict(scene_desc["materials"])
glass_desc["materials"]["rain_glass"] = {"transmission": 1.0, "thickness": 0.0, "ior": 1.5}

# The RELIEF variant (spec 13.12): the same yard on one ground carrying the banded height map,
# with the puddles told to follow it, seen from above so the bands read across the frame. The
# puddles are small beside a band, so each band holds many of them and the noise's share of
# either kind of band is the same.
relief_desc = dict(scene_desc)
relief_desc["_comment"] = [
    "The rain instrument's relief variant (spec 13.12): the yard on one ground whose height",
    "map is trenches and plateaus in bands across z, for puddles that stand in the lows. Same",
    "lights and rain; a camera above the open ground.",
]
relief_desc["models"] = [{"path": asset_ref("rain_relief_fixture.gltf")}]
relief_desc["materials"] = {}
relief_desc["rain"] = dict(scene_desc["rain"], puddleRelief=1.0, puddleScale=0.5)
relief_desc["camera"] = {"eye": [0.0, 6.0, 10.0], "target": [0.0, 0.0, 2.0], "fov": 55}



def write_json(name, obj):
    with open(asset_path(name), "w") as f:
        json.dump(obj, f, indent=1)
        f.write("\n")


with open(asset_path(MATERIALS["rain_relief"][2]), "wb") as f:
    f.write(png_grey(4, [255] * 4))
with open(asset_path(RELIEF_HEIGHT), "wb") as f:
    q = RELIEF_MAP // 4
    f.write(png_grey(RELIEF_MAP, [0 if q <= y < 3 * q else 255 for y in range(RELIEF_MAP)]))
write_json("rain_relief_fixture.gltf", gltf_of([RELIEF_GROUND] + YARD))
write_json("rain_relief_fixture.cscn", relief_desc)
write_json("rain_fixture.gltf", gltf_of(PIECES))
write_json("rain_glass_fixture.gltf", gltf_of(PIECES + PANES))
write_json("rain_glass_fixture.cscn", glass_desc)
write_json("rain_fixture.cscn", scene_desc)
write_json("rain_water_fixture.cscn", water_desc)
print("wrote rain_fixture.gltf + .cscn, rain_water_fixture.cscn, rain_glass_fixture.gltf + .cscn, "
      "rain_relief_fixture.gltf + .cscn and its two maps")
