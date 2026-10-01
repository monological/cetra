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
# normal. Every piece of the yard is this cube, translated and scaled by its node.
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

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *map(float, n)) for n in normals)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)

GREY = [0.35, 0.35, 0.35, 1.0]
MATERIALS = [
    ("rain_porous", GREY, 0.85),
    ("rain_sealed", GREY, 0.85),
    ("rain_roof", [0.12, 0.12, 0.13, 1.0], 0.7),
    ("rain_post", [0.3, 0.28, 0.25, 1.0], 0.8),
    ("rain_wall", [0.45, 0.28, 0.22, 1.0], 0.9),
]
GLASS_MATERIALS = MATERIALS + [("rain_glass", [0.9, 0.95, 0.92, 1.0], 0.05)]

# The yard, in metres: (name, material, centre, size).
ROOF_Y = 3.0  # underside; the slab spans x -4..4 and z -7..-1
PIECES = [
    ("ground_porous", "rain_porous", (-5.0, -0.1, 0.0), (10.0, 0.2, 24.0)),
    ("ground_sealed", "rain_sealed", (5.0, -0.1, 0.0), (10.0, 0.2, 24.0)),
    ("roof", "rain_roof", (0.0, ROOF_Y + 0.1, -4.0), (8.0, 0.2, 6.0)),
    ("post_a", "rain_post", (-3.8, ROOF_Y / 2, -1.2), (0.2, ROOF_Y, 0.2)),
    ("post_b", "rain_post", (3.8, ROOF_Y / 2, -1.2), (0.2, ROOF_Y, 0.2)),
    ("post_c", "rain_post", (-3.8, ROOF_Y / 2, -6.8), (0.2, ROOF_Y, 0.2)),
    ("post_d", "rain_post", (3.8, ROOF_Y / 2, -6.8), (0.2, ROOF_Y, 0.2)),
    ("wall", "rain_wall", (0.0, 2.0, -11.5), (20.0, 4.0, 0.4)),
    ("lamp_pole", "rain_post", (6.5, 2.2, -3.0), (0.12, 4.4, 0.12)),
]

# The GLASS twin's panes (spec 13.12), thin -- 6 mm. Two stand facing +z, the side the rain
# strikes: one in the open and one under the middle of the roof. The third lies FLAT, a glass
# canopy in the open between the roof and the camera: it shelters the ground under it as any
# roof does, and seen from below shows the drops on its top. Kept to x and z ranges of their
# own so each reads alone from the fixture's camera.
PANES = [
    ("pane_open", "rain_glass", (2.0, 1.05, 4.0), (2.0, 1.5, 0.006)),
    ("pane_covered", "rain_glass", (-2.0, 1.05, -4.0), (2.0, 1.5, 0.006)),
    ("pane_roof", "rain_glass", (0.0, 2.4, 1.0), (2.0, 0.006, 2.0)),
]


# The RELIEF twin's ground (spec 13.12): one plane in place of the two halves, at the same
# height, carrying texture coordinates so a height map can lie on it. The map repeats every
# RELIEF_PERIOD metres along z and is constant along x: a TRENCH across the middle half of each
# repeat and a PLATEAU either side. The band is symmetric about the repeat's centre, so it
# lands in the same place whichever way an importer flips V: trenches centred on z = 2 + 4k,
# plateaus on z = 4k. Its mean is exactly a half.
RELIEF_PERIOD = 4.0
RELIEF_X = (-10.0, 10.0)
RELIEF_Z = (-12.0, 12.0)
RELIEF_MAP = 64
RELIEF_ALBEDO = "rain_relief_albedo.png"
# The engine pairs a material with `<albedo stem>_height.png` beside its albedo, and only that
# way: a .cscn has no height key.
RELIEF_HEIGHT = "rain_relief_height.png"
RELIEF_MATERIALS = MATERIALS + [("rain_relief", GREY, 0.85)]


def png_grey(size, rows):
    """A minimal 8-bit RGB PNG, square: rows[y] is one grey level per row."""
    raw = b"".join(b"\x00" + bytes((v, v, v)) * size for v in rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def plane_bytes():
    """The relief ground's quad: positions, normals, texture coordinates and indices."""
    (x0, x1), (z0, z1) = RELIEF_X, RELIEF_Z
    corners = [(x0, z1), (x1, z1), (x1, z0), (x0, z0)]  # counter-clockwise from above
    return (b"".join(struct.pack("<3f", x, 0.0, z) for x, z in corners),
            struct.pack("<3f", 0.0, 1.0, 0.0) * 4,
            b"".join(struct.pack("<2f", x / RELIEF_PERIOD, z / RELIEF_PERIOD)
                     for x, z in corners),
            struct.pack("<6H", 0, 1, 2, 0, 2, 3))


def gltf_of(pieces, materials, relief=False):
    mat = {name: i for i, (name, _, _) in enumerate(materials)}
    views = [(pos_bytes, 34962), (nrm_bytes, 34962), (idx_bytes, 34963)]
    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
    ]
    meshes = [
        {"name": name,
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2,
                         "material": i}]}
        for i, (name, _, _) in enumerate(materials)
    ]
    nodes = [
        {"name": name, "mesh": mat[m], "translation": list(t), "scale": list(s)}
        for name, m, t, s in pieces
    ]
    extra = {}
    if relief:
        p, n, uv, idx = plane_bytes()
        views += [(p, 34962), (n, 34962), (uv, 34962), (idx, 34963)]
        accessors += [
            {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC3",
             "min": [RELIEF_X[0], 0.0, RELIEF_Z[0]], "max": [RELIEF_X[1], 0.0, RELIEF_Z[1]]},
            {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 5, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 6, "componentType": 5123, "count": 6, "type": "SCALAR"},
        ]
        meshes.append({"name": "ground_relief",
                       "primitives": [{"attributes": {"POSITION": 3, "NORMAL": 4,
                                                      "TEXCOORD_0": 5},
                                       "indices": 6, "material": mat["rain_relief"]}]})
        nodes.append({"name": "ground_relief", "mesh": len(meshes) - 1})
        extra = {"textures": [{"source": 0}], "images": [{"uri": asset_ref(RELIEF_ALBEDO)}]}
    data, buffer_views = b"", []
    for chunk, target in views:
        buffer_views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(chunk),
                             "target": target})
        data += chunk
    materials_out = []
    for name, color, rough in materials:
        pbr = {"baseColorFactor": color, "metallicFactor": 0.0, "roughnessFactor": rough}
        if name == "rain_relief":
            pbr["baseColorTexture"] = {"index": 0}
        materials_out.append({"name": name, "pbrMetallicRoughness": pbr})
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
# that resolve something to show.
glass_desc = dict(scene_desc)
glass_desc["_comment"] = [
    "The rain instrument's glass variant (spec 13.12): the yard with three thin panes facing",
    "the wind -- one in the open, one under the roof and one at the roof's front edge -- for",
    "the drops rain leaves on glass. Same camera, lights and rain.",
]
glass_desc["models"] = [{"path": asset_ref("rain_glass_fixture.gltf")}]
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

with open(asset_path(RELIEF_ALBEDO), "wb") as f:
    f.write(png_grey(4, [255] * 4))
with open(asset_path(RELIEF_HEIGHT), "wb") as f:
    q = RELIEF_MAP // 4
    f.write(png_grey(RELIEF_MAP, [0 if q <= y < 3 * q else 255 for y in range(RELIEF_MAP)]))
with open(asset_path("rain_relief_fixture.gltf"), "w") as f:
    json.dump(gltf_of([p for p in PIECES if not p[0].startswith("ground")], RELIEF_MATERIALS,
                      relief=True), f, indent=1)
    f.write("\n")
with open(asset_path("rain_relief_fixture.cscn"), "w") as f:
    json.dump(relief_desc, f, indent=1)
    f.write("\n")
with open(asset_path("rain_fixture.gltf"), "w") as f:
    json.dump(gltf_of(PIECES, MATERIALS), f, indent=1)
    f.write("\n")
with open(asset_path("rain_glass_fixture.gltf"), "w") as f:
    json.dump(gltf_of(PIECES + PANES, GLASS_MATERIALS), f, indent=1)
    f.write("\n")
with open(asset_path("rain_glass_fixture.cscn"), "w") as f:
    json.dump(glass_desc, f, indent=1)
    f.write("\n")
with open(asset_path("rain_fixture.cscn"), "w") as f:
    json.dump(scene_desc, f, indent=1)
    f.write("\n")
with open(asset_path("rain_water_fixture.cscn"), "w") as f:
    json.dump(water_desc, f, indent=1)
    f.write("\n")
print("wrote rain_fixture.gltf + .cscn, rain_water_fixture.cscn, rain_glass_fixture.gltf + .cscn, "
      "rain_relief_fixture.gltf + .cscn and its two maps")
