#!/usr/bin/env python3
"""Generate the short-occluder instrument (spec 13.58): occluders of known height on a floor, under
one sun, so whether each keeps its soft shadow is a number.

    floor     6 x 8.5 m, its top at y = 0.
    slabs     five 0.5 m squares in a row along x at z = 0, their heights 0.5%, 1%, 2%, 5% and
              10% of the scene's radius (the render app's: half the diagonal of the bounds).
    sphere    0.3 m round, resting on the floor behind the row, for a curved receiver.
    pillar    0.3 x 2 x 0.3 m, in front of the row, for a long penumbra.

The sun stands at azimuth 0, so every shadow falls toward -z, and an arm can say where a shadow
is from a slab's box alone. Three scenes share the pieces:

    short_occluder_fixture.cscn   the floor under the sun at 35 degrees
    short_occluder_grazing.cscn   the same at 8 degrees
    short_occluder_catcher.cscn   the pieces with no floor, over the sky's shadow catcher, the sky's
                                  sun at 35 degrees in the same place

Regenerate with: python3 assets/generators/gen_short_occluder_fixture.py
"""

import math
from fixture_paths import asset_ref
from gltf_pieces import cube, gltf_of, sphere, write_json

FLOOR_LO, FLOOR_HI = (-3.0, -0.1, -5.5), (3.0, 0.0, 3.0)
PILLAR_LO, PILLAR_HI = (-0.15, 0.0, -2.15), (0.15, 2.0, -1.85)
# The scene's radius as the render app measures it: the floor and the pillar fix the bounds.
RADIUS = 0.5 * math.dist(FLOOR_LO, (FLOOR_HI[0], PILLAR_HI[1], FLOOR_HI[2]))

SLAB_FRACTIONS = (0.005, 0.01, 0.02, 0.05, 0.10)
SLAB_X = (-1.6, -0.8, 0.0, 0.8, 1.6)
SLAB_HALF = 0.25
SPHERE_CENTRE, SPHERE_RADIUS = (-1.6, 0.3, 1.6), 0.3

# Name -> (base colour, metallic, roughness).
MATERIALS = {
    "short_floor": ([0.5, 0.5, 0.5, 1.0], 0.0, 0.9),
    "short_occluder": ([0.6, 0.6, 0.6, 1.0], 0.0, 0.7),
}


def box(name, lo, hi, shape="cube", material="short_occluder"):
    centre = tuple((a + b) / 2 for a, b in zip(lo, hi))
    size = tuple(b - a for a, b in zip(lo, hi))
    return (name, shape, material, centre, size)


pieces = [box(f"slab_{i}", (x - SLAB_HALF, 0.0, -SLAB_HALF),
              (x + SLAB_HALF, f * RADIUS, SLAB_HALF))
          for i, (x, f) in enumerate(zip(SLAB_X, SLAB_FRACTIONS))]
pieces.append(box("sphere", [c - SPHERE_RADIUS for c in SPHERE_CENTRE],
                  [c + SPHERE_RADIUS for c in SPHERE_CENTRE], shape="sphere"))
pieces.append(box("pillar", PILLAR_LO, PILLAR_HI))
floor = box("floor", FLOOR_LO, FLOOR_HI, material="short_floor")

SHAPES = {"cube": cube(), "sphere": sphere()}


def sun(elevation):
    """The direction a sun at this elevation and azimuth 0 travels."""
    e = math.radians(elevation)
    return [0.0, -math.sin(e), -math.cos(e)]


def scene(model, comment, **parts):
    return {"version": 1, "_comment": comment, "models": [{"path": asset_ref(model)}], **parts,
            "camera": {"eye": [3.5, 3.0, 3.5], "target": [0.0, 0.0, -0.5], "fov": 40}}


def lit(elevation):
    return {"environment": {"ambient": [0.3, 0.3, 0.3]},
            "lights": [{"name": "sun", "type": "directional", "direction": sun(elevation),
                        "color": [1.0, 1.0, 1.0], "intensity": 3.0, "cast_shadows": True}]}


REGENERATE = "Regenerate with: python3 assets/generators/gen_short_occluder_fixture.py"
write_json("short_occluder_fixture.gltf",
           gltf_of("gen_short_occluder_fixture.py", SHAPES, MATERIALS, [floor] + pieces))
write_json("short_occluder_bare.gltf",
           gltf_of("gen_short_occluder_fixture.py", SHAPES, MATERIALS, pieces))
write_json("short_occluder_fixture.cscn", scene(
    "short_occluder_fixture.gltf",
    ["The short-occluder instrument (spec 13.58): slabs 0.5% to 10% of the scene's radius tall",
     "on a floor, under a sun at 35 degrees whose shadows fall toward -z.", "", REGENERATE],
    **lit(35.0)))
write_json("short_occluder_grazing.cscn", scene(
    "short_occluder_fixture.gltf",
    ["The short-occluder instrument (spec 13.58) under a sun at 8 degrees, for a receiver that",
     "must not shadow itself.", "", REGENERATE],
    **lit(8.0)))
write_json("short_occluder_catcher.cscn", scene(
    "short_occluder_bare.gltf",
    ["The short-occluder instrument (spec 13.58) with no floor, over the sky's shadow catcher,",
     "the sky's sun at 35 degrees and azimuth 0.", "", REGENERATE],
    environment={"mode": "sky", "sun": {"elevation": 35.0, "azimuth": 0.0}}))
print(f"wrote short_occluder (radius {RADIUS:.4f}, slabs "
      + ", ".join(f"{f * RADIUS * 100:.2f} cm" for f in SLAB_FRACTIONS) + ")")
