#!/usr/bin/env python3
"""Generate rain_fixture.gltf + .cscn, the rain instrument, rain_water_fixture.cscn, its
flooded variant for the rings rain leaves on water (spec 13.9), and rain_glass_fixture.gltf
+ .cscn, its glazed variant for the drops rain leaves on glass (spec 13.12).

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
buffer_bytes = pos_bytes + nrm_bytes + idx_bytes

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


def gltf_of(pieces, materials):
    mat = {name: i for i, (name, _, _) in enumerate(materials)}
    return {
        "asset": {"version": "2.0", "generator": "gen_rain_fixture.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(pieces)))}],
        "nodes": [
            {"name": name, "mesh": mat[m], "translation": list(t), "scale": list(s)}
            for name, m, t, s in pieces
        ],
        "meshes": [
            {
                "name": name,
                "primitives": [
                    {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": i}
                ],
            }
            for i, (name, _, _) in enumerate(materials)
        ],
        "materials": [
            {
                "name": name,
                "pbrMetallicRoughness": {
                    "baseColorFactor": color,
                    "metallicFactor": 0.0,
                    "roughnessFactor": rough,
                },
            }
            for name, color, rough in materials
        ],
        "accessors": [
            {
                "bufferView": 0,
                "componentType": 5126,
                "count": len(positions),
                "type": "VEC3",
                "min": [-0.5, -0.5, -0.5],
                "max": [0.5, 0.5, 0.5],
            },
            {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes),
             "target": 34962},
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
print("wrote rain_fixture.gltf + .cscn, rain_water_fixture.cscn, rain_glass_fixture.gltf + .cscn")
