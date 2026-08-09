#!/usr/bin/env python3
"""Generate assets/flare_fixture.gltf, the lens-flare test asset (spec 11.21).

    python3 assets/gen_flare_fixture.py

WHAT THIS IS FOR

A lens flare is ghosts of a BRIGHT POINT, and the two things that make it
measurable are both geometric:

  * the source is SMALL and the rest of the frame is DARK. On a scene with
    large bright areas -- a lit white room, say -- every pixel is a source, so
    the ghosts of everything cover everything and the effect reads as a wash
    rather than an artifact. Measured on cornell_point, even a strength of
    0.001 moved 75% of the frame, which says nothing about the flare and
    everything about the scene.

  * the source is OFF-CENTRE. Ghosts are the source mirrored through the
    optical axis, so they land on the OPPOSITE side of frame centre. That is
    the claim the gate makes, and a centred emitter would make it unfalsifiable
    -- "ghosts appeared opposite the source" and "the image got brighter" would
    be the same measurement.

The sibling .cscn declares a zero-intensity light for the first of those. Not
because the scene wants light, but because a scene with NO lights gets a
three-point rig added for it, which lifts the backdrop off black and dilutes
the thing being measured: measured, the opposite-half rise is +3.5% with the
rig and +276% without it.

THE CORNER SUBJECT

Chromatic aberration falls off as r^2 from the optical centre, so a fixture
whose only feature sits mid-frame can measure that the channels separated but
not that the separation follows r^2 -- a linear ramp passes that identically,
and distinguishing the two is the entire reason the falloff is r^2.

So there is a second, DIM quad in the far corner. Dim is load-bearing: at 0.6
it sits below the bloom threshold of 1.0, so it never enters the pyramid, casts
no ghosts of its own, and leaves the flare measurement untouched. It exists
purely as a high-contrast edge at large r.

GEOMETRY

Quads in the XY plane facing +Z, camera on +Z looking back:
  - backdrop, oversized so no clear colour shows at any sane aspect
  - emitter, small, up and to the LEFT, emissive far above the bloom threshold
  - corner mark, small, bottom-RIGHT, emissive below the bloom threshold
"""

import base64
import json
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
GLTF = os.path.join(HERE, "flare_fixture.gltf")

# Up and to the LEFT, so the ghosts owe the gate the lower-right.
EMITTER_CENTRE = (-1.6, 1.1)
EMITTER_HALF = 0.125
# Far corner, for the r^2 assertion. Pushed out to sit near the frame edge
# where the aberration is strongest.
CORNER_CENTRE = (2.6, -1.9)
CORNER_HALF = 0.16
# A second dim mark at a SMALLER radius. Two of them is what makes the falloff
# falsifiable: with one feature a gate can only say the channels separated,
# which a linear ramp does too. With two, the RATIO of their separations
# distinguishes r^2 (~3.1x here) from linear (~1.8x). Both marks are dim for
# the same reason -- the bright emitter's bloom halo is NOT shifted by
# aberration, so its centroid would be dragged toward an unshifted average.
INNER_CENTRE = (1.5, -1.05)
INNER_HALF = 0.16
# Wider than the frame at any aspect the gate might render: at fov 50 and eye
# z 5 the visible half-width is ~4.1 at 16:10 and more at 16:9, and a backdrop
# that stops short leaves a clear-colour strip whose edge is an ACCIDENTAL
# high-contrast feature a gate could latch onto without noticing.
BACKDROP_HALF = (6.0, 4.0)


def quad(cx, cy, hx, hy, z):
    """Two triangles, CCW seen from +Z."""
    positions = [(cx - hx, cy + hy, z), (cx + hx, cy + hy, z),
                 (cx + hx, cy - hy, z), (cx - hx, cy - hy, z)]
    return positions, [(0.0, 0.0, 1.0)] * 4, [0, 3, 2, 0, 2, 1]


def main():
    positions, normals, indices = [], [], []
    prims = []
    for cx, cy, hx, hy, z in ((0.0, 0.0, BACKDROP_HALF[0], BACKDROP_HALF[1], -0.5),
                              (EMITTER_CENTRE[0], EMITTER_CENTRE[1], EMITTER_HALF, EMITTER_HALF,
                               0.0),
                              (CORNER_CENTRE[0], CORNER_CENTRE[1], CORNER_HALF, CORNER_HALF, 0.0),
                              (INNER_CENTRE[0], INNER_CENTRE[1], INNER_HALF, INNER_HALF, 0.0)):
        p, n, idx = quad(cx, cy, hx, hy, z)
        base = len(positions)
        prims.append((len(indices), len(idx)))
        positions += p
        normals += n
        indices += [base + i for i in idx]

    pos_b = b"".join(struct.pack("<3f", *p) for p in positions)
    nrm_b = b"".join(struct.pack("<3f", *n) for n in normals)
    idx_b = b"".join(struct.pack("<H", i) for i in indices)
    blob = pos_b + nrm_b + idx_b

    views, offset = [], 0
    for data in (pos_b, nrm_b, idx_b):
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data)})
        offset += len(data)
    views[2]["target"] = 34963

    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": [min(p[k] for p in positions) for k in range(3)],
         "max": [max(p[k] for p in positions) for k in range(3)]},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
    ]
    # One accessor per primitive's index range, so the quads can carry different
    # materials off one shared position/normal buffer.
    for first, count in prims:
        accessors.append({"bufferView": 2, "byteOffset": first * 2, "componentType": 5123,
                          "count": count, "type": "SCALAR"})

    def emissive(name, strength):
        return {"name": name,
                "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                                         "metallicFactor": 0.0, "roughnessFactor": 1.0},
                "emissiveFactor": [1.0, 0.96, 0.9],
                "extensions": {"KHR_materials_emissive_strength":
                               {"emissiveStrength": strength}}}

    gltf = {
        "asset": {"version": "2.0", "generator": "gen_flare_fixture.py"},
        "extensionsUsed": ["KHR_materials_emissive_strength"],
        "materials": [
            {"name": "backdrop",
             "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                                      "metallicFactor": 0.0, "roughnessFactor": 1.0}},
            # Far above the bloom threshold so the pyramid sees it by a wide
            # margin -- a source that only just crosses the knee would make the
            # gate sensitive to the threshold rather than to the flare.
            emissive("emitter", 60.0),
            # BELOW the threshold of 1.0 on purpose: an edge for the aberration
            # assertion that contributes nothing to the pyramid and therefore
            # casts no ghosts of its own.
            emissive("corner_mark", 0.6),
            emissive("inner_mark", 0.6),
        ],
        "meshes": [{"name": "flare_scene", "primitives": [
            {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0},
            {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 3, "material": 1},
            {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 4, "material": 2},
            {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 5, "material": 3},
        ]}],
        "nodes": [{"name": "flare_scene", "mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob),
                     "uri": "data:application/octet-stream;base64," +
                            base64.b64encode(blob).decode("ascii")}],
    }
    with open(GLTF, "w") as f:
        json.dump(gltf, f, indent=1)
        f.write("\n")
    print("wrote %s (emitter %s, marks %s and %s)"
          % (os.path.basename(GLTF), str(EMITTER_CENTRE), str(INNER_CENTRE), str(CORNER_CENTRE)))


if __name__ == "__main__":
    main()
