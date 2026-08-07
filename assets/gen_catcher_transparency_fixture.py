#!/usr/bin/env python3
"""Generate assets/catcher_transparency_fixture.gltf (spec 11.18).

The shadow catcher is not a floor. It is a shadow decal: a quad at y=0 that
darkens the projected environment where the shadow maps say the sun is blocked,
so a model looks like it stands on a backdrop that has no geometry. But it DOES
write depth, and until 11.18 it drew last, so nothing was ever ordered against
it. Translucent geometry behind the plane floated over the shadow instead of
being hidden by the floor.

Nothing in the repo could show that. Every catcher-enabled fixture is fully
opaque, and every alpha-blend fixture lacks an environment, so none of them gets
a catcher at all -- the two sets do not intersect anywhere. This is the
intersection.

Three pieces, and each is doing a job:

  backdrop  a large opaque matte wall well behind everything, so both sample
            columns read one flat lit value instead of a sky gradient. Without
            it the gate would be asserting on the sky's angular falloff.
  caster    an opaque box resting on y=0, off to the left, so the catcher has a
            real shadow to draw. Not needed for the DEPTH the gate measures --
            under SSR, which is the default, the whole quad writes depth whether
            it is shadowed or not -- but a catcher fixture with no visible
            shadow would be unreadable, and the box is what exercises the
            SSR-off path where only the shadowed footprint occludes.
  panel     a vertical translucent quad spanning y = -1.2 to +1.2, so half of it
            is behind the catcher plane and half in front. The measurement is
            the step between those halves.

The camera sits above the plane, so the sightline to the panel's lower half
crosses y=0 well in front of it: at world (1.2, -0.7, 0) the crossing is at
z = 1.6, comfortably inside the catcher quad. That is what puts the floor
between the eye and the glass.

    ./out/bin/render -m assets/catcher_transparency_fixture.cscn --no-recenter

**--no-recenter is mandatory, not tidiness.** The render app translates a model
so its bounding box base sits on y=0, and this fixture's whole point is geometry
BELOW y=0. Letting it recentre lifts the panel clear of the catcher plane and
the fixture measures nothing. It is the one fixture here that cannot be authored
around the recenter, because the property under test is the thing the recenter
removes.

Regenerate with:
  python3 assets/gen_catcher_transparency_fixture.py
(the .cscn is hand-authored, not generated.)
"""

import base64
import json
import os
import struct

BACKDROP_Z = -5.0    # far enough back that the panel is unambiguously in front
BACKDROP_HALF_X = 10.0
BACKDROP_TOP = 8.0
BACKDROP_BOTTOM = -8.0

BOX_MIN = (-2.8, 0.0, 0.4)  # rests ON the plane, so its shadow lands on the catcher
BOX_MAX = (-1.6, 1.2, 1.6)

PANEL_Z = 0.0
PANEL_X0, PANEL_X1 = 0.2, 2.2
PANEL_Y0, PANEL_Y1 = -1.2, 1.2
PANEL_ALPHA = 0.5

positions, normals, uvs = [], [], []
index_runs = []


def add_quad(corners, normal):
    """One quad from four corners listed counter-clockwise seen from `normal`.

    Winding matters and is easy to get backwards -- gen_area_shadow_fixture.py
    records the same trap for horizontal quads, where the natural corner order
    faces DOWN and both triangles cull to an empty frame.
    """
    base = len(positions)
    for p in corners:
        positions.append(p)
        normals.append(normal)
    uvs.extend(((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)))
    index_runs.append([base + 0, base + 1, base + 2, base + 0, base + 2, base + 3])


def add_box(lo, hi):
    """Axis-aligned box as six outward-facing quads, one index run for all."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    faces = [
        ((( x0, y0, z1), ( x1, y0, z1), ( x1, y1, z1), ( x0, y1, z1)), (0.0, 0.0, 1.0)),
        ((( x1, y0, z0), ( x0, y0, z0), ( x0, y1, z0), ( x1, y1, z0)), (0.0, 0.0, -1.0)),
        ((( x1, y0, z1), ( x1, y0, z0), ( x1, y1, z0), ( x1, y1, z1)), (1.0, 0.0, 0.0)),
        ((( x0, y0, z0), ( x0, y0, z1), ( x0, y1, z1), ( x0, y1, z0)), (-1.0, 0.0, 0.0)),
        ((( x0, y1, z1), ( x1, y1, z1), ( x1, y1, z0), ( x0, y1, z0)), (0.0, 1.0, 0.0)),
        ((( x0, y0, z0), ( x1, y0, z0), ( x1, y0, z1), ( x0, y0, z1)), (0.0, -1.0, 0.0)),
    ]
    start = len(index_runs)
    for corners, normal in faces:
        add_quad(corners, normal)
    merged = [i for run in index_runs[start:] for i in run]
    del index_runs[start:]
    index_runs.append(merged)


add_quad(((-BACKDROP_HALF_X, BACKDROP_BOTTOM, BACKDROP_Z),
          (BACKDROP_HALF_X, BACKDROP_BOTTOM, BACKDROP_Z),
          (BACKDROP_HALF_X, BACKDROP_TOP, BACKDROP_Z),
          (-BACKDROP_HALF_X, BACKDROP_TOP, BACKDROP_Z)), (0.0, 0.0, 1.0))
add_box(BOX_MIN, BOX_MAX)
add_quad(((PANEL_X0, PANEL_Y0, PANEL_Z), (PANEL_X1, PANEL_Y0, PANEL_Z),
          (PANEL_X1, PANEL_Y1, PANEL_Z), (PANEL_X0, PANEL_Y1, PANEL_Z)), (0.0, 0.0, 1.0))

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
uv_bytes = b"".join(struct.pack("<2f", *u) for u in uvs)
idx_bytes = b"".join(struct.pack("<I", i) for run in index_runs for i in run)
buffer_bytes = pos_bytes + nrm_bytes + uv_bytes + idx_bytes

all_min = [min(p[k] for p in positions) for k in range(3)]
all_max = [max(p[k] for p in positions) for k in range(3)]


def matte(name, value):
    return {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": [value, value, value, 1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 1.0,
        },
    }


# Saturated and doubleSided: the gate reads the step between the panel's halves
# per channel, and a grey panel over a grey backdrop would put that step entirely
# in luminance, where the catcher's own darkening also lives.
panel_material = {
    "name": "catcher_panel",
    "alphaMode": "BLEND",
    "doubleSided": True,
    "pbrMetallicRoughness": {
        "baseColorFactor": [0.1, 0.75, 0.9, PANEL_ALPHA],
        "metallicFactor": 0.0,
        "roughnessFactor": 0.6,
    },
}

names = ["catcher_backdrop", "catcher_caster", "catcher_panel"]
offsets = []
run_start = 0
for run in index_runs:
    offsets.append((run_start, len(run)))
    run_start += len(run) * 4

gltf = {
    "asset": {"version": "2.0", "generator": "gen_catcher_transparency_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1, 2]}],
    "nodes": [{"name": n, "mesh": i} for i, n in enumerate(names)],
    "meshes": [
        {"name": n,
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                         "indices": 3 + i, "material": i}]}
        for i, n in enumerate(names)
    ],
    "materials": [matte(names[0], 0.55), matte(names[1], 0.30), panel_material],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": all_min, "max": all_max},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
    ] + [
        {"bufferView": 3, "byteOffset": off, "componentType": 5125, "count": n, "type": "SCALAR"}
        for (off, n) in offsets
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes), "byteLength": len(uv_bytes),
         "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes) + len(uv_bytes),
         "byteLength": len(idx_bytes), "target": 34963},
    ],
    "buffers": [
        {"uri": "data:application/octet-stream;base64,"
         + base64.b64encode(buffer_bytes).decode("ascii"),
         "byteLength": len(buffer_bytes)}
    ],
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "catcher_transparency_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
print("wrote %s (panel y %.1f..%.1f across the catcher plane, alpha %.2f)"
      % (out, PANEL_Y0, PANEL_Y1, PANEL_ALPHA))
print("bounds min %s max %s -- render with --no-recenter or the panel lifts clear of y=0"
      % (["%.2f" % v for v in all_min], ["%.2f" % v for v in all_max]))
