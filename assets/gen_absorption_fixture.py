#!/usr/bin/env python3
"""Generate assets/absorption_fixture.gltf + .cscn, the KHR_materials_volume
absorption instrument.

Three transmissive panels sit in a row in front of ONE uniform emissive
backdrop. All three carry the same attenuationColor and attenuationDistance and
differ only in volume thickness -- 0.0, 0.3, 0.6 -- which is the path length
Beer-Lambert integrates over. attenuationColor is what survives exactly
attenuationDistance, so with the distance set equal to the middle thickness the
transmitted green:red ratio has a closed form:

    thickness 0.0  ->  1.0        (absorption cannot act over zero path)
    thickness 0.3  ->  0.35       (= attenuationColor.g, by definition)
    thickness 0.6  ->  0.1225     (= 0.35^2)

Red passes unabsorbed (attenuationColor.r = 1.0), so it is the in-frame
reference the green channel is measured against and no second render is needed.

Two design choices are load-bearing:

  - The backdrop is ONE uniform colour, not the coloured grid glass_fixture
    uses. Thickness also drives the refraction BEND (pbr_frag offsets the exit
    point along the refracted ray by it), so over a patterned backdrop the three
    panels would sample different colours and the channel ratio would stop
    isolating absorption.

  - The backdrop is EMISSIVE on black, so its radiance is its factor exactly and
    the ratio does not inherit a light rig's colour. It is kept at 0.6 rather
    than 1.0 because red is transmitted unabsorbed: at 1.0 the reference channel
    would clip at the 8-bit write and the ratio's denominator would be a
    constant regardless of what the shader did.

The .cscn ships one dim light aimed away from the panels: the render app injects
a three-point rig when a no-IBL scene has zero lights, and that rig's specular
would add a channel-equal term to both halves of the ratio and pull it toward 1.
Regenerate with: python3 assets/gen_absorption_fixture.py
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

# The middle panel's thickness, and the attenuation distance, are the same number
# on purpose: it makes that panel's transmittance equal attenuationColor exactly,
# which is the one point the extension defines without an exponential.
ATTENUATION_COLOR = (1.0, 0.35, 0.35)
ATTENUATION_DISTANCE = 0.3
THICKNESSES = (0.0, 0.3, 0.6)
BACKDROP_EMISSIVE = 0.6


def panel_material(name, thickness):
    return {
        "name": name,
        "doubleSided": True,
        "pbrMetallicRoughness": {
            "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 0.0,
        },
        "extensions": {
            "KHR_materials_transmission": {"transmissionFactor": 1.0},
            "KHR_materials_ior": {"ior": 1.5},
            "KHR_materials_volume": {
                "thicknessFactor": thickness,
                "attenuationColor": list(ATTENUATION_COLOR),
                "attenuationDistance": ATTENUATION_DISTANCE,
            },
        },
    }


materials = [
    {
        "name": "absorb_backdrop",
        "doubleSided": True,
        "pbrMetallicRoughness": {
            "baseColorFactor": [0.0, 0.0, 0.0, 1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 1.0,
        },
        "emissiveFactor": [BACKDROP_EMISSIVE] * 3,
    },
] + [
    panel_material("absorb_t%03d" % round(t * 100), t) for t in THICKNESSES
]

# (material index, translation, scale-xyz). The backdrop is oversized so the
# refracted exit point never leaves it, even for the thickest panel.
nodes_spec = [
    (0, (0.0, 1.0, -1.5), (5.0, 3.0, 1.0)),
    (1, (-1.1, 1.0, 0.0), (0.9, 0.9, 1.0)),
    (2, (0.0, 1.0, 0.0), (0.9, 0.9, 1.0)),
    (3, (1.1, 1.0, 0.0), (0.9, 0.9, 1.0)),
]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_absorption_fixture.py"},
    "extensionsUsed": [
        "KHR_materials_transmission",
        "KHR_materials_ior",
        "KHR_materials_volume",
    ],
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

# Camera is DERIVED from the panel row rather than copied: distance is chosen so
# the outer panels clear the frame edge, so moving a panel cannot silently leave
# the gate reading backdrop. Vertical fov 45 at 4:3.
FOV = 45.0
EYE_Z = 4.6
scene_desc = {
    "version": 1,
    "models": [{"path": "absorption_fixture.gltf"}],
    "lights": [
        {
            "name": "AbsorbFill",
            "type": "directional",
            "direction": [0.0, -1.0, 0.0],
            "color": [1.0, 1.0, 1.0],
            "intensity": 0.05,
            "cast_shadows": False,
        }
    ],
    "camera": {"eye": [0.0, 1.0, EYE_Z], "target": [0.0, 1.0, 0.0], "fov": FOV},
    "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
}

out_dir = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(out_dir, "absorption_fixture.gltf"), "w") as f:
    json.dump(gltf, f, indent=1)
with open(os.path.join(out_dir, "absorption_fixture.cscn"), "w") as f:
    json.dump(scene_desc, f, indent=1)
print("wrote absorption_fixture.gltf + absorption_fixture.cscn")
