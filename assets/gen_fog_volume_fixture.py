#!/usr/bin/env python3
"""Generate assets/fog_volume_fixture.gltf + .cscn, the local-fog-volume instrument
(spec 11.39).

ONE emissive backdrop quad, and the whole subject of the fixture is in the .cscn: two
boxes of denser air standing in front of it, with a gap between them. Every read is
therefore in-frame, against a neighbouring strip of the same backdrop:

    left box    density d, white tint   ->  against the gap:  does a volume scatter at all
    right box   density d, warm tint    ->  against the LEFT box: does tint act, at equal
                                            density, so colour is isolated from amount

Three things here are load-bearing and each was chosen against an alternative:

  - The backdrop is EMISSIVE on black, so its radiance is its factor exactly. A lit
    backdrop's own shading would vary across the frame and the inside/outside strips
    would differ before the fog touched them.

  - The boxes are separated by GAPS of plain backdrop rather than butted together. The
    gap is the control, and it has to be the same backdrop at the same height, which
    rules out reading against the frame edge.

  - The scene authors NO global fog. `fogVolumes` alone arms the froxel pass -- that is
    the property the off-arm checks -- so a scene that also asked for height fog could
    not distinguish "the volume armed it" from "fog was on anyway".

The light is dim and aimed across the boxes rather than at the backdrop: the fog needs a
sun term to in-scatter, and the backdrop is emissive so nothing else needs lighting. The
render app injects a three-point rig into a no-IBL scene with zero lights, which would
put an uncontrolled second source into the in-scatter.

Regenerate with: python3 assets/gen_fog_volume_fixture.py
"""

import base64
import json
import os
import struct

# One unit quad in the XY plane, facing +Z (two triangles)
positions = [(-0.5, -0.5, 0.0), (0.5, -0.5, 0.0), (0.5, 0.5, 0.0), (-0.5, 0.5, 0.0)]
normals = [(0.0, 0.0, 1.0)] * 4
indices = [0, 1, 2, 0, 2, 3]

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
buffer_bytes = pos_bytes + nrm_bytes + idx_bytes

# Mid grey rather than 1.0: the in-scatter ADDS to what the backdrop transmits, so a
# backdrop near the top of the range would clip the inside-box strips at the 8-bit write
# and the ratio the arm reads would stop moving.
BACKDROP_EMISSIVE = 0.35

# The boxes, in world units: (centre x, half-extent x, tint). Y, Z, density and feather
# are shared, so the row varies in exactly one thing at a time.
BOX_HALF_Y = 1.2
BOX_HALF_Z = 0.6
BOX_DENSITY = 0.5
BOX_FEATHER = 0.12
BOXES = [
    (-1.30, 0.55, [1.0, 1.0, 1.0]),
    (1.30, 0.55, [1.0, 0.55, 0.25]),
]
WARM_TINT = [1.0, 0.55, 0.25]

materials = [
    {
        "name": "fogvol_backdrop",
        "doubleSided": True,
        "pbrMetallicRoughness": {
            "baseColorFactor": [0.0, 0.0, 0.0, 1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 1.0,
        },
        "emissiveFactor": [BACKDROP_EMISSIVE] * 3,
    },
]

# Oversized so no camera framing within reason can read past its edge.
nodes_spec = [(0, (0.0, 1.0, -2.5), (12.0, 8.0, 1.0))]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_fog_volume_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": list(range(len(nodes_spec)))}],
    "nodes": [
        {
            "name": materials[mat]["name"],
            "mesh": i,
            "translation": list(t),
            "scale": list(s),
        }
        for i, (mat, t, s) in enumerate(nodes_spec)
    ],
    "meshes": [
        {
            "name": materials[mat]["name"],
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": mat}
            ],
        }
        for mat, _, _ in nodes_spec
    ],
    "materials": materials,
    "accessors": [
        {
            "bufferView": 0,
            "componentType": 5126,
            "count": 4,
            "type": "VEC3",
            "min": [-0.5, -0.5, 0.0],
            "max": [0.5, 0.5, 0.0],
        },
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes), "target": 34962},
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

FOV = 45.0
EYE_Z = 4.4
scene_desc = {
    "version": 1,
    "_comment": [
        "Local fog volumes (spec 11.39). Two boxes of denser air in front of one uniform",
        "emissive backdrop, separated by a gap of plain backdrop that is the control.",
        "",
        "Both boxes carry the SAME density and differ only in tint, so the tint arm reads",
        "colour with amount held fixed. The gap between them reads amount with colour held",
        "fixed, against a strip of the same backdrop at the same height.",
        "",
        "No global fog is authored anywhere in this file. The froxel pass runs because",
        "fogVolumes is non-empty, which is the property the off-arm exists to check -- and",
        "a volume must never do that by setting fog_enabled, which belongs to the app and",
        "the GUI and is never cleared per frame.",
        "",
        "tint colours the SHARED lighting rather than adding radiance of its own, so a box",
        "can be smoke, dust or mist and cannot be a glow.",
    ],
    "models": [{"path": "fog_volume_fixture.gltf"}],
    "fogVolumes": [
        {
            "center": [cx, 1.0, 0.0],
            "extent": [hx, BOX_HALF_Y, BOX_HALF_Z],
            "density": BOX_DENSITY,
            "feather": BOX_FEATHER,
            "tint": tint,
        }
        for cx, hx, tint in BOXES
    ],
    "lights": [
        {
            "name": "FogKey",
            "type": "directional",
            "direction": [0.6, -0.5, -0.62],
            "color": [1.0, 1.0, 1.0],
            "intensity": 2.5,
            "cast_shadows": False,
        }
    ],
    "camera": {"eye": [0.0, 1.0, EYE_Z], "target": [0.0, 1.0, 0.0], "fov": FOV},
    "post": {"tonemap": "neutral", "exposure": 1.0},
}

def volume(cx, hx, density, tint):
    return {
        "center": [cx, 1.0, 0.0],
        "extent": [hx, BOX_HALF_Y, BOX_HALF_Z],
        "density": density,
        "feather": BOX_FEATHER,
        "tint": tint,
    }


# The MIX variant, on the same geometry and the same camera, so the gate reuses the
# fixture's crop boxes unchanged (spec 11.40).
#
# This exists because the fixture above cannot see the sigma-weighted combination at all.
# With no global fog every cell has airSigma 0, so the fold reduces to `S *= tint` exactly
# and a plain multiply -- the thing the fold is there to avoid -- passes every arm.
#
# Adding global fog does not fix it, and the reason is geometric rather than a matter of
# tuning: there are 3.8 world units of clear air between the camera and a box's front face,
# so raising the global density attenuates the box's contribution faster than it dilutes
# its tint. At density 0.5 the depth mix dominates and moves the read the wrong way; at 2.0
# the whole frame is uniform haze and the boxes have vanished. There is no window.
#
# So the diluting medium is a SECOND VOLUME occupying the same box, not global air. Then
# there is nothing in front to attenuate, and the total extinction is exactly controllable:
#
#   left  strip -- warm alone at D
#   right strip -- white at D/2 and warm at D/2, COINCIDENT
#
# Both strips therefore carry total sigma D, so transmittance, depth mix and backdrop share
# are identical and the ONLY difference is what the tint sum is made of. The sigma-weighted
# fold gives the right strip a source tint of (0.5 + 0.5*warm) and the two strips separate;
# a plain multiply cannot see the white volume at all -- white is its identity -- and the
# two strips read the same. One comparison, and it fails exactly the substitution the fold
# exists to prevent.
mix_desc = dict(scene_desc)
mix_desc["_comment"] = [
    "The sigma-weighted-fold instrument (spec 11.40). Same geometry, same camera and the",
    "same crop boxes as fog_volume_fixture; only the volume set differs.",
    "",
    "Left strip: warm alone at D. Right strip: white at D/2 and warm at D/2, occupying the",
    "SAME box. Total extinction is D on both, so the depth mix is identical and the only",
    "variable is the composition of the tint sum.",
    "",
    "A sigma-weighted average gives the right strip (0.5 + 0.5*warm) and the strips differ.",
    "A plain S *= tint cannot see a white volume at all, and they read identically. That is",
    "the whole point of the file -- no other fixture can distinguish the two.",
    "",
    "The diluting medium is a coincident VOLUME rather than global fog because 3.8 units of",
    "clear air sit between the camera and the box, so global density drowns the box before",
    "it dilutes it.",
]
mix_desc["fogVolumes"] = [
    volume(-1.30, 0.55, BOX_DENSITY, WARM_TINT),
    volume(1.30, 0.55, BOX_DENSITY * 0.5, [1.0, 1.0, 1.0]),
    volume(1.30, 0.55, BOX_DENSITY * 0.5, WARM_TINT),
]

here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "fog_volume_fixture.gltf"), "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
with open(os.path.join(here, "fog_volume_fixture.cscn"), "w") as f:
    json.dump(scene_desc, f, indent=1)
    f.write("\n")
with open(os.path.join(here, "fog_volume_mix_fixture.cscn"), "w") as f:
    json.dump(mix_desc, f, indent=1)
    f.write("\n")
print("wrote fog_volume_fixture.gltf + .cscn + fog_volume_mix_fixture.cscn")
