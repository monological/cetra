#!/usr/bin/env python3
"""Generate assets/mask_fixture.gltf -- what ALPHA_MASK is supposed to mean (spec 11.31).

Two coplanar quads, side by side, same base colour, same normal, lit by one
directional light so the only thing that can differ between them is the alpha
path:

  left    ALPHA_OPAQUE, no vertex colours          -> the reference
  middle  ALPHA_MASK cutoff 0.4, COLOR_0 alpha 0.6 -> above the cutoff
  right   ALPHA_MASK cutoff 0.4, COLOR_0 alpha 0.2 -> below it, must vanish

glTF says MASK is binary: a fragment at or above alphaCutoff is rendered fully
opaque, one below is discarded. So on a single-sample target these two quads
must be THE SAME COLOUR. They were not: the opaque lane inherited global
SRC_ALPHA blending, so the right quad landed at 0.6 of itself over the clear
colour, and the corpus had nothing that could see it -- every golden's masked
geometry is either absent or, in translucent_shadow's case, a caster held above
the frame.

The third quad is the negative control, and without it the fixture proves less
than it looks. Two quads can only show that a surviving fragment is opaque; a
regression that dropped the cutoff test altogether -- or made alphaBelowCutoff
always false -- would render every masked fragment, still opaque, and pass. That
is the failure render.c names when it explains why A2C falls back to the binary
cutoff on a 1-sample buffer: "masked geometry would render as solid quads".

The alpha is carried on COLOR_0 rather than in a texture, and that is not just
convenience. `texAlpha` is seeded from the albedo texture's alpha and then
multiplied by VertexColor.a (pbr_frag), while baseColorFactor[3] becomes
`materialOpacity`, which is a different quantity on a different path. A MASK
material with a fractional baseColorFactor alpha and no texture has coverage
1.0 and never exercised this at all -- which is one reason it went unnoticed.

Deliberately NOT a golden. Under alpha-to-coverage a fractional alpha is
SUPPOSED to become fractional sample coverage, so the two quads legitimately
differ on the 4x MSAA path, and the invariant only holds where MSAA is off.
The gate arm renders it with --taa (which is what drops the engine to one
sample) and compares the two quads inside a single frame, so it needs no stored
image and no cross-run comparison.

Regenerate with: python3 assets/gen_mask_fixture.py
"""

import base64
import json
import os
import struct

HALF = 0.62
GAP = 0.22  # clear space between the quads, so none samples another's edge

# Both quads sit with their base on y = 0, which makes the render app's
# recentring (bbox base to y = 0) a no-op and keeps the authored camera honest.
positions = [(-HALF, 0.0, 0.0), (HALF, 0.0, 0.0), (HALF, 2.0 * HALF, 0.0),
             (-HALF, 2.0 * HALF, 0.0)]
normals = [(0.0, 0.0, 1.0)] * 4
indices = [0, 1, 2, 0, 2, 3]

# White RGB so the vertex colour tints nothing -- sRGBToLinear(1) is 1. 0.6 is
# comfortably above the 0.4 cutoff and comfortably below 1, so a fragment that
# still blends is unmistakable rather than marginal; 0.2 is as clearly below.
CUTOFF = 0.4
kept_colors = [(1.0, 1.0, 1.0, 0.6)] * 4
cut_colors = [(1.0, 1.0, 1.0, 0.2)] * 4

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
kept_bytes = b"".join(struct.pack("<4f", *c) for c in kept_colors)
cut_bytes = b"".join(struct.pack("<4f", *c) for c in cut_colors)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + kept_bytes + cut_bytes + idx_bytes

mn = [min(p[i] for p in positions) for i in range(3)]
mx = [max(p[i] for p in positions) for i in range(3)]

# One albedo for both, mid-grey rather than white: white clips against the
# tonemap and two clipped quads compare equal however wrong the alpha is.
ALBEDO = [0.55, 0.55, 0.58, 1.0]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_mask_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1, 2]}],
    # The two quads the gate COMPARES sit symmetrically about the frame centre,
    # and the one that must vanish sits between them. Symmetry is not tidiness:
    # the vignette is a function of radius from the centre, so equal radii make
    # it cancel exactly between the pair rather than biasing one of them. It also
    # puts the negative control where a failure to discard is unmissable.
    "nodes": [
        {"name": "opaque_ref", "mesh": 0, "translation": [-2.0 * (HALF + GAP), 0.0, 0.0]},
        {"name": "masked_cut", "mesh": 2, "translation": [0.0, 0.0, 0.0]},
        {"name": "masked_kept", "mesh": 1, "translation": [2.0 * (HALF + GAP), 0.0, 0.0]},
    ],
    "meshes": [
        # The reference carries no COLOR_0 at all, so vertexColorExists is 0 for
        # it and texAlpha stays 1.0 -- the comparison is against a fragment that
        # never entered the alpha path, not against one that entered it at 1.0.
        {"name": "opaque_ref",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 4,
                         "material": 0}]},
        {"name": "masked_kept",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "COLOR_0": 2}, "indices": 4,
                         "material": 1}]},
        # Same material parameters as the one beside it, so the ONLY difference
        # between kept and cut is which side of the cutoff its alpha falls.
        {"name": "masked_cut",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "COLOR_0": 3}, "indices": 4,
                         "material": 2}]},
    ],
    "materials": [
        {"name": "mask_ref",
         "pbrMetallicRoughness": {"baseColorFactor": ALBEDO, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.6}},
        {"name": "mask_kept",
         "alphaMode": "MASK",
         "alphaCutoff": CUTOFF,
         "pbrMetallicRoughness": {"baseColorFactor": ALBEDO, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.6}},
        {"name": "mask_cut",
         "alphaMode": "MASK",
         "alphaCutoff": CUTOFF,
         "pbrMetallicRoughness": {"baseColorFactor": ALBEDO, "metallicFactor": 0.0,
                                  "roughnessFactor": 0.6}},
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": mn, "max": mx},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC4"},
        {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC4"},
        {"bufferView": 4, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes), "byteLength": len(kept_bytes),
         "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes) + len(kept_bytes),
         "byteLength": len(cut_bytes), "target": 34962},
        {"buffer": 0,
         "byteOffset": len(pos_bytes) + len(nrm_bytes) + len(kept_bytes) + len(cut_bytes),
         "byteLength": len(idx_bytes), "target": 34963},
    ],
    "buffers": [
        {"uri": "data:application/octet-stream;base64," +
                base64.b64encode(buffer_bytes).decode("ascii"),
         "byteLength": len(buffer_bytes)},
    ],
}

# Directional, not punctual: the quads are coplanar with one normal, so a
# directional light delivers identical irradiance to both and any difference
# between them is the alpha path rather than the falloff.
cscn = {
    "version": 1,
    "models": [{"path": "mask_fixture.gltf"}],
    "lights": [{"name": "MaskSun", "type": "directional", "direction": [0.0, -0.3, -0.95],
                "color": [1.0, 1.0, 1.0], "intensity": 3.0, "cast_shadows": False}],
    # Aimed at the quads' mid-height so all three sit on one horizontal band,
    # which is what lets the gate sample them at a single y and lets the two it
    # compares sit symmetrically about the frame centre -- the vignette is a
    # function of radius, so equal radii make it cancel between them.
    "camera": {"eye": [0.0, HALF, 5.4], "target": [0.0, HALF, 0.0], "fov": 45},
    "post": {"tonemap": "neutral", "exposure": 1.0},
}

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "mask_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
scn = os.path.join(here, "mask_fixture.cscn")
with open(scn, "w") as f:
    json.dump(cscn, f, indent=1)
    f.write("\n")
print("wrote", out, "and", scn,
      f"(opaque reference, MASK above cutoff {CUTOFF}, MASK below it)")
