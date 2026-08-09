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

So: one small emissive quad up and to the left, a large dark backdrop, and
nothing else. The ghosts belong down and to the right.

GEOMETRY

Two quads in the XY plane, both facing +Z, camera on +Z looking back:
  - backdrop, 8x6, near-black, non-emissive
  - emitter, 0.25x0.25, centred at (-1.6, 1.1), emissive far above 1 so it
    clears the bloom threshold by a wide margin
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
BACKDROP_HALF = (4.0, 3.0)


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
                               0.0)):
        p, n, idx = quad(cx, cy, hx, hy, z)
        base = len(positions)
        prims.append((base, len(indices), len(idx)))
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
    # One accessor per primitive's index range, so the two quads can carry
    # different materials off one shared buffer.
    for _, first, count in prims:
        accessors.append({"bufferView": 2, "byteOffset": first * 2, "componentType": 5123,
                          "count": count, "type": "SCALAR"})

    gltf = {
        "asset": {"version": "2.0", "generator": "gen_flare_fixture.py"},
        "extensionsUsed": ["KHR_materials_emissive_strength"],
        "materials": [
            {"name": "backdrop",
             "pbrMetallicRoughness": {"baseColorFactor": [0.02, 0.02, 0.025, 1.0],
                                      "metallicFactor": 0.0, "roughnessFactor": 1.0}},
            # Emissive far above 1 so it clears the bloom threshold by a wide
            # margin -- the flare reads the pyramid, and a source that only just
            # crosses the knee would make the gate sensitive to the threshold
            # rather than to the flare.
            {"name": "emitter",
             "pbrMetallicRoughness": {"baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                                      "metallicFactor": 0.0, "roughnessFactor": 1.0},
             "emissiveFactor": [1.0, 0.96, 0.9],
             "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 60.0}}},
        ],
        "meshes": [{"name": "flare_scene", "primitives": [
            {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0},
            {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 3, "material": 1},
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
    print("wrote %s (emitter at %s)" % (os.path.basename(GLTF), str(EMITTER_CENTRE)))


if __name__ == "__main__":
    main()
