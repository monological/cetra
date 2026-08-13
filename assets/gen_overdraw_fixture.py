#!/usr/bin/env python3
"""Generate the depth-complexity instruments spec 11.30 asked for and never built.

Two layouts from one generator, because they are the same geometry answering
opposite questions:

  overdraw_layers  N full-frame opaque quads stacked at staggered depths.
                   Every pixel is covered N times, so GL_SAMPLES_PASSED over the
                   opaque lane must read EXACTLY N -- an integer with no noise
                   floor, which is what `overdraw-probe` wants and what
                   apps/forest could never give it. Forest's reading moves with
                   its AA mode (the budget is multiplied by msaa_samples), with
                   the draw order, and with the scatter; the arm was reading 1.05
                   against a > 1.0 bar for those reasons and not because the
                   instrument was marginal.

  overdraw_tiles   An M x M grid of small opaque quads tiling the same rectangle,
                   none overlapping. Depth complexity 1.0 with real submission
                   and vertex cost and nothing whatever to reject, which is the
                   case a depth prepass should LOSE. 11.30 looked for that
                   crossover with instancing_fixture, expected it to lose at
                   complexity 0.71, and measured it winning by 10% -- an absence
                   of evidence at three points that it recorded as such.

Each tile is its own glTF mesh rather than one mesh under M*M nodes, and that is
the whole point of the layout: draw_run_can_join keys on the Mesh pointer, so
shared geometry would batch into a handful of instanced draws and the extra pass
would cost almost nothing. Distinct meshes make the prepass pay full submission
twice, which is the cost being measured.

Both layouts are OPAQUE with no alpha mode, no textures and no transparency, so
nothing here depends on the coverage path -- the quantity is the depth test and
the submission, and a fixture that also exercised masking could not say which
had moved.

The .cscn is generated rather than hand-authored, for gen_oit_cards's reason:
the camera is DERIVED (the distance that makes the far quad overfill the frame),
so a copied one would stop covering the frame the moment a spacing changed. The
quads are sized against a 16:9 frame with margin even though the gate renders
4:3, since a layout that stops covering the frame reads as a complexity BELOW N
and would look like an instrument fault rather than a framing one.

Regenerate with:
  python3 assets/gen_overdraw_fixture.py
"""

import argparse
import base64
import json
import math
import os
import struct

FOV_DEG = 45.0
NEAR_GAP = 4.0    # camera to the nearest quad
SPACING = 0.5     # between stacked layers
# Sized for 16:9 with margin, then rendered at 4:3 -- overfilling costs nothing
# and under-filling would read as a complexity below N.
WIDE_ASPECT = 16.0 / 9.0
COVER_MARGIN = 1.25


def _cover_half_height(distance):
    """Half-height of the frustum at `distance`, with margin."""
    return distance * math.tan(math.radians(FOV_DEG) * 0.5) * COVER_MARGIN


def _quad(x0, y0, x1, y1, z):
    """Corners of an axis-aligned quad facing +Z, counter-clockwise from below."""
    return [(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)]


def _build(quads):
    """Pack one position accessor per quad, sharing normals and indices."""
    normals = b"".join(struct.pack("<3f", 0.0, 0.0, 1.0) for _ in range(4))
    indices = b"".join(struct.pack("<H", i) for i in (0, 1, 2, 0, 2, 3))

    blob = normals + indices
    views = [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(normals), "target": 34962},
        {"buffer": 0, "byteOffset": len(normals), "byteLength": len(indices), "target": 34963},
    ]
    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 1, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ]

    meshes, nodes = [], []
    for i, corners in enumerate(quads):
        offset = len(blob)
        blob += b"".join(struct.pack("<3f", *c) for c in corners)
        mn = [min(c[k] for c in corners) for k in range(3)]
        mx = [max(c[k] for c in corners) for k in range(3)]
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": 48, "target": 34962})
        accessors.append({"bufferView": len(views) - 1, "componentType": 5126, "count": 4,
                          "type": "VEC3", "min": mn, "max": mx})
        meshes.append({"name": f"quad_{i}",
                       "primitives": [{"attributes": {"POSITION": len(accessors) - 1,
                                                      "NORMAL": 0},
                                       "indices": 1, "material": 0}]})
        nodes.append({"name": f"quad_{i}", "mesh": i})
    return blob, views, accessors, meshes, nodes


def _write(name, quads, eye_z, centre_y, note):
    blob, views, accessors, meshes, nodes = _build(quads)
    gltf = {
        "asset": {"version": "2.0", "generator": "gen_overdraw_fixture.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        # One material for every quad, so `material switches` stays flat and the
        # submission columns move only with the draw count.
        "materials": [{"name": "overdraw",
                       "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.55, 0.58, 1.0],
                                                "metallicFactor": 0.0,
                                                "roughnessFactor": 0.6}}],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"uri": "data:application/octet-stream;base64," +
                            base64.b64encode(blob).decode("ascii"),
                     "byteLength": len(blob)}],
    }
    cscn = {
        "version": 1,
        "models": [{"path": f"{name}.gltf"}],
        "lights": [{"name": "OverdrawSun", "type": "directional",
                    "direction": [0.0, -0.25, -0.97], "color": [1.0, 1.0, 1.0],
                    "intensity": 3.0, "cast_shadows": False}],
        "camera": {"eye": [0.0, centre_y, eye_z], "target": [0.0, centre_y, 0.0],
                   "fov": FOV_DEG},
        "post": {"tonemap": "neutral", "exposure": 1.0},
    }

    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, f"{name}.gltf"), "w") as f:
        json.dump(gltf, f, indent=1)
        f.write("\n")
    with open(os.path.join(here, f"{name}.cscn"), "w") as f:
        json.dump(cscn, f, indent=1)
        f.write("\n")
    print(f"wrote {name}.gltf and {name}.cscn ({len(quads)} quads, {note})")


def layers(n):
    """N stacked full-frame quads: depth complexity is exactly N."""
    # z centred on the origin so the app's recenter is a no-op in z, and the
    # base on y = 0 so it is a no-op in y as well. The camera is authored in
    # pre-recenter world space, so an offset would slide the geometry out from
    # under it.
    span = (n - 1) * SPACING
    eye_z = NEAR_GAP + span * 0.5
    half_h = _cover_half_height(eye_z + span * 0.5)
    half_w = half_h * WIDE_ASPECT
    # FARTHEST FIRST, which is what makes the count exactly N. Depth complexity
    # counts samples that PASS, so submitted near-to-far the first quad occludes
    # every one behind it and the fixture would read 1.00 however many layers it
    # had. Far-to-near every layer passes and shades, which is also the worst
    # case an overdraw instrument should be built on.
    #
    # It is therefore ALSO a direct test of the front-to-back sort: with the
    # sort on, this same scene must collapse back toward 1.
    quads = [_quad(-half_w, 0.0, half_w, 2.0 * half_h, -span * 0.5 + k * SPACING)
             for k in range(n)]
    return quads, eye_z, half_h


def tiles(m):
    """M x M non-overlapping quads over the same rectangle: complexity 1.0."""
    eye_z = NEAR_GAP
    half_h = _cover_half_height(eye_z)
    half_w = half_h * WIDE_ASPECT
    quads = []
    for row in range(m):
        for col in range(m):
            x0 = -half_w + 2.0 * half_w * col / m
            x1 = -half_w + 2.0 * half_w * (col + 1) / m
            y0 = 2.0 * half_h * row / m
            y1 = 2.0 * half_h * (row + 1) / m
            quads.append(_quad(x0, y0, x1, y1, 0.0))
    return quads, eye_z, half_h


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--layers", type=int, default=8,
                    help="stacked full-frame quads; depth complexity equals this")
    ap.add_argument("--tiles", type=int, default=24,
                    help="grid edge; emits this squared non-overlapping quads")
    args = ap.parse_args()

    quads, eye_z, half_h = layers(args.layers)
    _write("overdraw_layers", quads, eye_z, half_h,
           f"depth complexity should read exactly {args.layers}.00")

    quads, eye_z, half_h = tiles(args.tiles)
    _write("overdraw_tiles", quads, eye_z, half_h,
           f"{args.tiles}x{args.tiles} grid, complexity 1.0, one draw each")
