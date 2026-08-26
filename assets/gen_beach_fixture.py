#!/usr/bin/env python3
"""Generate assets/beach_fixture.gltf + .cscn, the SURF instrument (spec 11.44).

The water corpus could not see foam at all. Both water goldens are Gerstner, where
crest foam does not exist by construction, and neither has a shoaling bed, so shore
foam is identically zero in them -- spec 11.43 landed three foam fixes and moved
zero golden pixels. This is the frame that can see them.

A DOME rising out of the water, matching the analytic bed `--water-bed dome`
installs, so one frame carries:

  - open water at the rim, where the bed is 9 units down and the surface is
    unshoaled;
  - the whole shoaling ramp, where waves shorten and the shore foam band lives;
  - a dry crown, 0.6 units above the still level, so the shoreline is in frame
    rather than at its edge.

THE MESH DUPLICATES render_dome_bed_height AND THAT IS THE POINT. The water shoals
against the analytic field; this mesh is what the eye sees it shoal against. If the
two ever disagree the surface will shoal against nothing visible, which is exactly
the kind of defect a fixture should make obvious -- and beach-shoreline asserts
they agree by reading where the waterline actually lands.

Emissive, like the water fixture, so what reaches the water is authored rather
than the product of a light rig. Regenerate with:
    python3 assets/gen_beach_fixture.py
"""

import base64
import json
import math
import os
import struct
import sys

WATER_LEVEL = 0.0
# The extent the .cscn authors. render_dome_bed_height sizes itself off this, so the
# two cannot be set independently: the dome's radius IS extent * 0.62.
WATER_EXTENT = 60.0
DOME_RADIUS = WATER_EXTENT * 0.62
# render_dome_bed_height's own floor and crown, relative to the still level.
DOME_FLOOR = WATER_LEVEL - 9.0
DOME_CROWN = WATER_LEVEL + 0.6

RINGS = 96
SEGMENTS = 128

# A flat plate under everything, so the frame has a bed past the dome's toe rather
# than open sky under the water at the rim.
PLATE_HALF = 220.0

DOME_EMISSIVE = (0.62, 0.56, 0.42)
PLATE_EMISSIVE = (0.30, 0.31, 0.30)


def dome_height(r):
    """render_dome_bed_height, in Python. Smoothstep in r so the ramp has no crease."""
    t = r / DOME_RADIUS
    if t >= 1.0:
        return DOME_FLOOR
    s = 1.0 - t
    return DOME_FLOOR + (DOME_CROWN - DOME_FLOOR) * s * s * (3.0 - 2.0 * s)


def dome_normal(r, ca, sa):
    """Central difference on dome_height, so the normal follows the surface drawn."""
    h = 0.35
    d = (dome_height(max(r + h, 0.0)) - dome_height(max(r - h, 0.0))) / (2.0 * h)
    # A surface of revolution: the radial slope is d, and there is none around.
    n = (-d * ca, 1.0, -d * sa)
    m = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) ** 0.5
    return (n[0] / m, n[1] / m, n[2] / m)


dome_positions = []
dome_normals = []
dome_indices = []

# Centre vertex, then RINGS rings outward to the dome's toe.
dome_positions.append((0.0, dome_height(0.0), 0.0))
dome_normals.append((0.0, 1.0, 0.0))
for ring in range(1, RINGS + 1):
    r = DOME_RADIUS * ring / RINGS
    for s in range(SEGMENTS):
        a = 2.0 * math.pi * s / SEGMENTS
        ca, sa = math.cos(a), math.sin(a)
        dome_positions.append((r * ca, dome_height(r), r * sa))
        dome_normals.append(dome_normal(r, ca, sa))

for s in range(SEGMENTS):
    dome_indices += [0, 1 + (s + 1) % SEGMENTS, 1 + s]
for ring in range(1, RINGS):
    inner = 1 + (ring - 1) * SEGMENTS
    outer = 1 + ring * SEGMENTS
    for s in range(SEGMENTS):
        s1 = (s + 1) % SEGMENTS
        dome_indices += [inner + s, outer + s1, outer + s]
        dome_indices += [inner + s, inner + s1, outer + s1]

plate_positions = [
    (-PLATE_HALF, DOME_FLOOR, -PLATE_HALF),
    (PLATE_HALF, DOME_FLOOR, -PLATE_HALF),
    (PLATE_HALF, DOME_FLOOR, PLATE_HALF),
    (-PLATE_HALF, DOME_FLOOR, PLATE_HALF),
]
plate_normals = [(0.0, 1.0, 0.0)] * 4
plate_indices = [0, 1, 2, 0, 2, 3]


def mesh_bytes(positions, normals, indices, wide):
    pos = b"".join(struct.pack("<3f", *p) for p in positions)
    nrm = b"".join(struct.pack("<3f", *n) for n in normals)
    fmt = "<I" if wide else "<H"
    idx = b"".join(struct.pack(fmt, i) for i in indices)
    # glTF requires bufferView offsets aligned to the component size.
    while len(idx) % 4:
        idx += b"\0"
    return pos, nrm, idx


dp, dn, di = mesh_bytes(dome_positions, dome_normals, dome_indices, True)
pp, pn, pi = mesh_bytes(plate_positions, plate_normals, plate_indices, False)
buffer_bytes = dp + dn + di + pp + pn + pi

offsets = {}
cursor = 0
for name, blob in (("dp", dp), ("dn", dn), ("di", di), ("pp", pp), ("pn", pn), ("pi", pi)):
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


dome_min = [-DOME_RADIUS, DOME_FLOOR, -DOME_RADIUS]
dome_max = [DOME_RADIUS, DOME_CROWN, DOME_RADIUS]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_beach_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1]}],
    "nodes": [
        {"name": "beach_plate", "mesh": 0},
        {"name": "beach_dome", "mesh": 1},
    ],
    "meshes": [
        {
            "name": "beach_plate",
            "primitives": [
                {"attributes": {"POSITION": 3, "NORMAL": 4}, "indices": 5, "material": 0}
            ],
        },
        {
            "name": "beach_dome",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 1}
            ],
        },
    ],
    "materials": [
        emissive_material("plate", PLATE_EMISSIVE),
        emissive_material("dome", DOME_EMISSIVE),
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(dome_positions), "type": "VEC3",
         "min": dome_min, "max": dome_max},
        {"bufferView": 1, "componentType": 5126, "count": len(dome_normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5125, "count": len(dome_indices), "type": "SCALAR"},
        {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": [-PLATE_HALF, DOME_FLOOR, -PLATE_HALF],
         "max": [PLATE_HALF, DOME_FLOOR, PLATE_HALF]},
        {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 5, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": [
        view("dp", 34962), view("dn", 34962), view("di", 34963),
        view("pp", 34962), view("pn", 34962), view("pi", 34963),
    ],
    "buffers": [
        {
            "uri": "data:application/octet-stream;base64,"
            + base64.b64encode(buffer_bytes).decode("ascii"),
            "byteLength": len(buffer_bytes),
        }
    ],
}

# DERIVED from the dome, like the water fixture's camera is derived from its wedge: outside
# the shoal looking across it, high enough that the ramp is foreshortened rather than seen
# edge-on, so the whole band from open water to dry crown lands in frame.
EYE = [0.0, WATER_LEVEL + 27.0, DOME_RADIUS + 40.0]
TARGET = [0.0, WATER_LEVEL - 0.4, -DOME_RADIUS * 0.10]

scene_desc = {
    "version": 1,
    "models": [{"path": "beach_fixture.gltf"}],
    "lights": [
        {
            "name": "BeachKey",
            "type": "directional",
            "direction": [-0.30, -0.78, -0.55],
            "color": [1.0, 1.0, 1.0],
            "intensity": 1.2,
            "cast_shadows": False,
        }
    ],
    "environment": {"mode": "sky", "sun": {"elevation": 32.0, "azimuth": 150.0},
                    "intensity": 1.0},
    "camera": {"eye": EYE, "target": TARGET, "fov": 40},
    # SPECTRAL, unlike water_fixture, and that is the whole reason this file exists: crest
    # foam is FFT-only by construction, so a Gerstner fixture cannot show it whatever else
    # it authors. A rough enough sea that the horizontal map folds, over a bed that shoals.
    "water": {
        "enabled": True,
        "level": WATER_LEVEL,
        "extent": WATER_EXTENT,
        "waves": "fft",
        "windDirection": [0.0, -1.0],
        # Rough enough that the map folds and crest foam exists at all, calm enough that the
        # crests do not swamp the crown -- the shoreline is what half these arms read.
        #
        # Two TRAINS since spec 11.48. The swell's numbers are the ones that used to be
        # hardcoded, so the sea here is the same one these arms were calibrated against; the
        # old flat "swell": 0.25 was ONE knob doing two jobs and migrates to both of them --
        # the wind sea's `focus`, and the swell's `scale` at 0.25/0.38 of full strength.
        "seaDepth": 24.0,
        "windSea": {
            "windSpeed": 10.5,
            "fetch": 90000.0,
            "direction": 0.0,
            "scale": 1.0,
            "peakEnhancement": 3.3,
            "focus": 0.25,
            "spreadGain": 0.58,
            "spreadBlend": 0.68,
        },
        "swell": {
            "windSpeed": 8.4,
            "fetch": 310000.0,
            "direction": 0.82,
            # The arithmetic, not a rounded 0.6579: this IS the expression master computed
            # (sw_weight = swell / WATER_DEFAULT_SWELL, with the old default 0.38), so writing
            # it keeps the migration checkable instead of asking a reader to trust a rounding.
            # json.dump uses repr(float), which is deterministic, so fixture-gen's byte
            # comparison is as happy with this as with a literal.
            "scale": 0.25 / 0.38,
            "peakEnhancement": 2.6,
            "focus": 0.833,
            "spreadGain": 0.72,
            "spreadBlend": 1.0,
        },
        "wavelength": 9.0,
        "amplitude": 0.22,
        "steepness": 0.6,
        "spread": 0.8,
        "roughness": 0.06,
        "ior": 1.333,
        "absorption": [0.45, 0.09, 0.06],
        # A FRACTION of the light falling on the water, not a radiance (spec 11.84).
        # The previous absolute [0.04, 0.16, 0.18] divided by THIS scene's own daylight
        # incident, (4.7781, 4.1485, 3.3313) -- its own and not water_fixture's, since
        # the two sit at different sun elevations and this one was always the brighter
        # operating point of the pair.
        "scatterAlbedo": [0.0084, 0.0386, 0.054],
        # No self-lit floor: every arm here reads a depth ramp, and a constant under it
        # would flatten the ratios that ramp exists to produce.
        "scatterGlow": [0.0, 0.0, 0.0],
        "caustics": True,
        "shoreCoverage": True,
        "farLod": True,
    },
    "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
}

out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(out_dir, "beach_fixture.gltf"), "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
with open(os.path.join(out_dir, "beach_fixture.cscn"), "w") as f:
    json.dump(scene_desc, f, indent=1)
    f.write("\n")
print("wrote beach_fixture.gltf + beach_fixture.cscn")
