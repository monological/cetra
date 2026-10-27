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
the first; the shadow itself has it nearer the second. The shadow-tiles gate group traces the
same body against the rim exactly, with no shadow map, and that is what both are measured
against.

The camera is orthographic in practice: tile_core_fixture renders with --ortho 0.8, so a
pixel is a fixed fraction of a metre across the wall, and --tonemap linear, which a scene file
cannot ask for, so a pixel's value divided by its unshadowed twin's is how much light reaches.

Regenerate with: python3 assets/generators/gen_tile_core_fixture.py
"""

from fixture_paths import asset_ref
from gltf_pieces import cube, gltf_of, write_json

# Name -> (base colour, metallic, roughness).
MATERIALS = {
    "core_wall": ([0.7, 0.7, 0.7, 1.0], 0.0, 0.9),
    "core_wax": ([0.85, 0.8, 0.7, 1.0], 0.0, 0.5),
}


def box(name, material, lo, hi):
    """A piece spanning world `lo` to `hi`, as a cube's node."""
    centre = tuple((a + b) / 2 for a, b in zip(lo, hi))
    size = tuple(b - a for a, b in zip(lo, hi))
    return (name, "cube", material, centre, size)


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


write_json("tile_core_fixture.gltf",
           gltf_of("gen_tile_core_fixture.py", {"cube": cube()}, MATERIALS, PIECES))
write_json("tile_core_fixture.cscn", scene_desc)
print("wrote tile_core_fixture (.gltf + .cscn)")
