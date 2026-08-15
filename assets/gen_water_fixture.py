#!/usr/bin/env python3
"""Generate assets/water_fixture.gltf + .cscn, the water surface instrument.

A wedge rising out of the water, so ONE frame contains the whole optical range
the surface is supposed to produce:

  - a shoreline, where the wedge crosses the waterline and the surface has to
    stop rather than draw a film over dry ground;
  - a depth ramp, because the wedge's submerged face recedes to the deep end, so
    absorption is sampled at every path length across one continuous surface;
  - a reflection, from the environment cubemap the .cscn asks for.

The wedge is the instrument's whole point: absorption is monotone in path length,
and a ramp is the shape that makes a monotone quantity readable as a gradient
along a single scanline. Two flat boxes at two depths would prove ordering; a
ramp proves it does not step.

The floor is a large flat quad at the deep end's level, which puts a KNOWN
constant depth under the far half of the frame. A gate reading absorption there
is reading one surface at one depth rather than an average over a slope.

Everything is emissive on black. An emitter's radiance is its factor exactly, so
what reaches the water is authored rather than the product of a light rig, and the
transmitted colour a gate reads is not also measuring the lighting. Regenerate
with: python3 assets/gen_water_fixture.py
"""

import base64
import json
import os
import struct
import sys

WATER_LEVEL = 0.0
# The wedge spans from well under the water to well over it: the shoreline has to
# land inside the frame, not at its edge.
WEDGE_LOW = -1.6
WEDGE_HIGH = 0.9
WEDGE_NEAR_Z = 2.2
WEDGE_FAR_Z = -3.4
WEDGE_HALF_X = 1.5
FLOOR_Y = -1.6
FLOOR_HALF = 14.0

# Emissive factors. The floor is brighter than the wedge so the two read apart
# through the same water.
FLOOR_EMISSIVE = (0.55, 0.55, 0.55)
WEDGE_EMISSIVE = (0.42, 0.34, 0.22)


def mesh_bytes(positions, normals, indices):
    pos = b"".join(struct.pack("<3f", *p) for p in positions)
    nrm = b"".join(struct.pack("<3f", *n) for n in normals)
    idx = b"".join(struct.pack("<H", i) for i in indices)
    return pos, nrm, idx


# The wedge: one doubleSided quad climbing from the deep far end to dry land at the
# near end. A sheet, not a solid -- the camera sits below its extended plane, so what
# the "dry land" boxes read is its underside. Fine for an emissive surface, and worth
# knowing before assuming a box is looking at a beach.
# The ramp descends TOWARD the camera: deep at the near edge, dry at the far one.
#
# The other way round -- which this was until 11.42 -- puts the high edge nearest the eye,
# and then the surface's own plane climbs past the camera (to y 2.78 at the eye's z, against
# an eye at 1.35). The camera is then BELOW the plane it is looking at, so the frame shows
# the ramp's underside, the near edge occludes the far one, and a zero-thickness quad seen
# that way reads as a paper blade slicing the water rather than as ground.
wedge_positions = [
    (-WEDGE_HALF_X, WEDGE_LOW, WEDGE_NEAR_Z),
    (WEDGE_HALF_X, WEDGE_LOW, WEDGE_NEAR_Z),
    (WEDGE_HALF_X, WEDGE_HIGH, WEDGE_FAR_Z),
    (-WEDGE_HALF_X, WEDGE_HIGH, WEDGE_FAR_Z),
]
wedge_indices = [0, 1, 2, 0, 2, 3]
# One normal for the whole ramp; it is planar. Taken from the edge vectors rather than
# written down, so it follows the positions above instead of having to be re-derived by
# hand whenever they move -- which is how it came to disagree with them in the first place.
#
# The edges come from the FIRST TRIANGLE'S OWN INDICES, not from a hand-written (0,1,2).
# That is what makes the check below real: with the triple written out, reversing the
# winding left the emitted normal untouched and the assert still passed, so it pinned
# nothing it claimed to. Now the two cannot disagree.
_i0, _i1, _i2 = wedge_indices[:3]
_e1 = tuple(b - a for a, b in zip(wedge_positions[_i0], wedge_positions[_i1]))
_e2 = tuple(b - a for a, b in zip(wedge_positions[_i0], wedge_positions[_i2]))
_n = (_e1[1] * _e2[2] - _e1[2] * _e2[1],
      _e1[2] * _e2[0] - _e1[0] * _e2[2],
      _e1[0] * _e2[1] - _e1[1] * _e2[0])
_len = (_n[0] * _n[0] + _n[1] * _n[1] + _n[2] * _n[2]) ** 0.5
wedge_normal = tuple(c / _len for c in _n)
# raise, not assert: a module-level assert is stripped under `python -O`, and a fixture
# generated with the ramp facing into the ground would look plausible enough to commit.
if wedge_normal[1] <= 0.0:
    raise ValueError("the ramp's winding and its normal disagree: normal points down")
wedge_normals = [wedge_normal] * 4

floor_positions = [
    (-FLOOR_HALF, FLOOR_Y, -FLOOR_HALF),
    (FLOOR_HALF, FLOOR_Y, -FLOOR_HALF),
    (FLOOR_HALF, FLOOR_Y, FLOOR_HALF),
    (-FLOOR_HALF, FLOOR_Y, FLOOR_HALF),
]
floor_normals = [(0.0, 1.0, 0.0)] * 4
floor_indices = [0, 1, 2, 0, 2, 3]

wp, wn, wi = mesh_bytes(wedge_positions, wedge_normals, wedge_indices)
fp, fn, fi = mesh_bytes(floor_positions, floor_normals, floor_indices)
buffer_bytes = wp + wn + wi + fp + fn + fi

offsets = {}
cursor = 0
for name, blob in (("wp", wp), ("wn", wn), ("wi", wi), ("fp", fp), ("fn", fn), ("fi", fi)):
    offsets[name] = (cursor, len(blob))
    cursor += len(blob)


def emissive_material(name, factor):
    return {
        "name": name,
        "doubleSided": True,
        "pbrMetallicRoughness": {
            "baseColorFactor": [0.0, 0.0, 0.0, 1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 1.0,
        },
        "emissiveFactor": list(factor),
    }


def view(key, target):
    off, length = offsets[key]
    return {"buffer": 0, "byteOffset": off, "byteLength": length, "target": target}


gltf = {
    "asset": {"version": "2.0", "generator": "gen_water_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [
        {"name": "water_bed", "mesh": 0},
        {"name": "water_ramp", "mesh": 1},
    ],
    "meshes": [
        {
            "name": "water_bed",
            "primitives": [
                {"attributes": {"POSITION": 3, "NORMAL": 4}, "indices": 5, "material": 0}
            ],
        },
        {
            "name": "water_ramp",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 1}
            ],
        },
    ],
    "materials": [
        emissive_material("bed", FLOOR_EMISSIVE),
        emissive_material("ramp", WEDGE_EMISSIVE),
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": [-WEDGE_HALF_X, WEDGE_LOW, WEDGE_FAR_Z],
         "max": [WEDGE_HALF_X, WEDGE_HIGH, WEDGE_NEAR_Z]},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"},
        {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": [-FLOOR_HALF, FLOOR_Y, -FLOOR_HALF],
         "max": [FLOOR_HALF, FLOOR_Y, FLOOR_HALF]},
        {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 5, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": [
        view("wp", 34962), view("wn", 34962), view("wi", 34963),
        view("fp", 34962), view("fn", 34962), view("fi", 34963),
    ],
    "buffers": [
        {
            "uri": "data:application/octet-stream;base64,"
            + base64.b64encode(buffer_bytes).decode("ascii"),
            "byteLength": len(buffer_bytes),
        }
    ],
}

# Camera is DERIVED from the waterline: it sits above the surface looking down the
# ramp, so the shoreline crosses the middle of the frame at any wedge geometry.
# A copied camera would stop framing the thing being measured the moment a
# dimension above changed.
EYE = [0.0, WATER_LEVEL + 1.35, WEDGE_NEAR_Z + 4.2]
TARGET = [0.0, WATER_LEVEL - 0.25, WEDGE_FAR_Z * 0.35]

scene_desc = {
    "version": 1,
    "models": [{"path": "water_fixture.gltf"}],
    "lights": [
        {
            "name": "WaterKey",
            "type": "directional",
            "direction": [-0.35, -0.82, -0.45],
            "color": [1.0, 1.0, 1.0],
            "intensity": 1.2,
            "cast_shadows": False,
        }
    ],
    "environment": {"mode": "sky", "sun_elevation": 26.0, "sun_azimuth": 135.0,
                    "intensity": 1.0},
    "camera": {"eye": EYE, "target": TARGET, "fov": 42},
    # The surface itself, and every property an arm reads. This block CREATES the water
    # (spec 11.33 phase 5) -- without it the fixture renders dry, so a regeneration that
    # dropped it would take every water arm and both water goldens with it. It was in fact
    # missing here from 11.33 until 11.42 while the committed .cscn carried it, which made
    # the docstring's "regenerate with" line a destructive instruction for three specs.
    #
    # Authors EVERY key the parser understands, deliberately: an arm then names only what
    # it varies, and a default that changes underneath cannot silently move the fixture.
    # Keep this in step with parse_water (cscene.c).
    "water": {
        "enabled": True,
        "level": WATER_LEVEL,
        "extent": 14.0,
        "waves": "gerstner",
        # The Gerstner train. Lake scale, and small enough that no crest ever shows its
        # underside -- which is why water-waterline has to author its own framing.
        "wavelength": 6.0,
        "amplitude": 0.06,
        "steepness": 0.6,
        "spread": 0.42,
        "windDirection": [0.86, 0.51],
        # The spectral sea state (spec 11.42), inert until --water-waves fft. Physical
        # quantities: a calmer spectral ocean is a lower wind speed, not a smaller
        # amplitude, and no CLI flag can set any of these.
        "windSpeed": 11.5,
        "fetch": 120000.0,
        "seaDepth": 54.0,
        "peakEnhancement": 3.3,
        "swell": 0.38,
        "roughness": 0.04,
        "ior": 1.333,
        # Clear water per metre, and this fixture's unit IS a metre (spec 11.36).
        "absorption": [0.45, 0.09, 0.06],
        "scatter": [0.02, 0.1, 0.12],
        "caustics": True,
        "shoreCoverage": True,
        "farLod": True,
    },
    "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
}

# Optional output directory, so a caller can regenerate somewhere harmless and diff the
# result against the committed pair. That is what `water-fixture-roundtrip` does, and it is
# the assertion whose absence let this script drift out of step with its own output for
# three specs -- it stopped emitting the `water` block the fixture needs to have a surface
# at all, and nothing noticed because nobody runs a generator that is already "done".
out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(out_dir, "water_fixture.gltf"), "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
with open(os.path.join(out_dir, "water_fixture.cscn"), "w") as f:
    json.dump(scene_desc, f, indent=1)
    f.write("\n")
print("wrote water_fixture.gltf + water_fixture.cscn")
