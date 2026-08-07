#!/usr/bin/env python3
"""Generate assets/oit_cards_fixture.gltf -- the OIT accuracy instrument (spec 11.17).

assets/oit_fixture.gltf is three quads at nearly one depth. Spec 4.17 measured
the McGuire weight saturating to a constant across them, and that is worse than
it sounds: **weighted-blended OIT is EXACT whenever every layer carries the same
colour, at any weights whatsoever**, because the accumulator divides by its own
alpha sum. The old fixture is close to that degenerate case, so it cannot tell
weighted-blended and moment-based apart and never could. What discriminates them
is colour varying WITH depth; depth spread only matters through the weight curve.

So this fixture is a staircase of twelve translucent cards, each a different
colour, spread over a 25:1 range of view distance:

    band 0    background only          B
    band 1    card 0 over B            S_0 = C_0*a + (1-a)*B
    band 2    cards 0..1 over B        S_1 = C_1*a + (1-a)*S_0
    ...
    band 12   cards 0..11 over B       S_11 = C_11*a + (1-a)*S_10

Card k is NEARER than card k-1 and its x-extent is nested inside it, so scanning
left to right crosses regions of exactly 0, 1, ... 12 layers and each step
composites one more card ON TOP. That recursion is the correct answer, and
scripts/gates.py asserts it directly -- there is no stored reference here.

Three properties make the answer knowable rather than eyeballed:

  * The cards are EMISSIVE over black albedo, so a fragment's colour is its
    material constant -- no view or position dependence to model.
  * They are parallel to the image plane, so every pixel of a band sees an exact
    integer layer count. A grazing stack would read fractional coverage at the
    band edges and there would be nothing to predict.
  * Every colour, and therefore every composite of them, lands inside
    [0.08, 0.76], where the Khronos Neutral tonemap is exactly `c - 0.04`. The
    gate inverts that and the display gamma and measures in linear HDR.

The layout also makes the app's model recenter a NO-OP, which it has to be: the
camera is authored in the .cscn in pre-recenter world space, so a non-zero
offset would translate the geometry out from under the framing the gate
predicts. That needs the bounding box centred in x and z with its base at y=0,
which is why the background quad is the widest and lowest thing in the scene and
why the near card and the background sit at equal and opposite z.

The sibling .cscn is generated here too, unlike every other fixture's. Its
camera is not an authored viewpoint but a value DERIVED from the geometry -- the
height that puts the backdrop's lower edge on y=0 and the z that straddles the
bounding box -- so a hand-copied camera would silently stop framing the scene the
moment any distance above changed.

Regenerate with:
  python3 assets/gen_oit_cards_fixture.py
"""

import base64
import colorsys
import json
import math
import os
import struct

CARDS = 12          # layers in the deepest band
ALPHA = 0.15        # per-card opacity; (1-a)^12 = 0.142, so B stays visible
NEAR_DIST = 8.0     # view distance of the nearest card
FAR_DIST = 200.0    # view distance of the farthest card
BG_DIST = 210.0     # view distance of the opaque background

FOVY_DEG = 40.0     # must match the .cscn camera and the gate
ASPECT = 1.6        # the 800x500 the gate renders at

# Screen-space band layout, in UV (0 = left/top edge of the frame). Thirteen
# equal bands from BAND_LEFT to BAND_RIGHT: the first is background only, then
# one per card.
BAND_LEFT = 0.06
BAND_RIGHT = 0.94
CARD_V_HALF = 0.30  # card half-height as a fraction of frame height

BG_OVERSCAN = 1.2   # background scale past the frame, so it never leaves an edge

# Emissive colours. A grey floor of 0.12 keeps every channel above the Neutral
# tonemap's 0.08 toe (below it the curve is quadratic, not an offset), and the
# 0.45 span keeps the peak under its 0.76 compression knee. Saturated hues make
# the ordering error visible per channel -- a greyscale stack would compose
# identically under any weighting at all.
COLOR_FLOOR = 0.12
COLOR_SPAN = 0.45
BACKGROUND_GREY = 0.10


def card_color(k):
    r, g, b = colorsys.hsv_to_rgb(k / float(CARDS), 1.0, 1.0)
    return [COLOR_FLOOR + COLOR_SPAN * c for c in (r, g, b)]


def half_extents(dist):
    """World half-width / half-height of the full frame at a view distance."""
    hh = dist * math.tan(math.radians(FOVY_DEG) * 0.5)
    return hh * ASPECT, hh


positions, normals, uvs = [], [], []
index_runs = []


def add_quad(x0, x1, y0, y1, z):
    """A quad in the XY plane at depth `z`, facing +Z (toward the camera).

    Corners are listed counter-clockwise seen from +Z, so (0,1,2)/(0,2,3) puts
    the front face on the side the camera is on -- the opposite of the XZ quads
    in gen_area_shadow_fixture.py, where the same index order would cull.
    """
    base = len(positions)
    for (x, y) in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
        positions.append((x, y, z))
        normals.append((0.0, 0.0, 1.0))
    uvs.extend(((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)))
    index_runs.append([base + 0, base + 1, base + 2, base + 0, base + 2, base + 3])


# Camera z, chosen so the bounding box straddles z=0 (see the recenter note
# above): the box runs from the background to the near card.
CAM_Z = (BG_DIST + NEAR_DIST) * 0.5
# Camera height, likewise: the background's lower edge lands exactly on y=0.
BG_HW, BG_HH = half_extents(BG_DIST)
BG_HW *= BG_OVERSCAN
BG_HH *= BG_OVERSCAN
CAM_Y = BG_HH

add_quad(-BG_HW, BG_HW, 0.0, 2.0 * BG_HH, CAM_Z - BG_DIST)

band = (BAND_RIGHT - BAND_LEFT) / (CARDS + 1)
dists = []
for k in range(CARDS):
    # Card 0 is the FARTHEST and the widest; each later card is nearer and
    # nested inside its predecessor, so band j shows cards 0..j-1.
    dist = FAR_DIST + (NEAR_DIST - FAR_DIST) * k / float(CARDS - 1)
    dists.append(dist)
    hw, hh = half_extents(dist)
    u0 = BAND_LEFT + (k + 1) * band
    add_quad((u0 - 0.5) * 2.0 * hw, (BAND_RIGHT - 0.5) * 2.0 * hw,
             CAM_Y - CARD_V_HALF * 2.0 * hh, CAM_Y + CARD_V_HALF * 2.0 * hh,
             CAM_Z - dist)

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
uv_bytes = b"".join(struct.pack("<2f", *u) for u in uvs)
idx_bytes = b"".join(struct.pack("<I", i) for run in index_runs for i in run)
buffer_bytes = pos_bytes + nrm_bytes + uv_bytes + idx_bytes

all_min = [min(p[k] for p in positions) for k in range(3)]
all_max = [max(p[k] for p in positions) for k in range(3)]


def unlit(name, color, alpha):
    """Emissive over black albedo: the fragment's colour is this constant.

    Black base colour zeroes the diffuse and leaves only a 0.04 dielectric F0,
    which has no environment to reflect in this scene, so nothing view-dependent
    survives to perturb the composite the gate predicts.
    """
    mat = {
        "name": name,
        "doubleSided": True,
        "emissiveFactor": color,
        "pbrMetallicRoughness": {
            "baseColorFactor": [0.0, 0.0, 0.0, alpha],
            "metallicFactor": 0.0,
            "roughnessFactor": 1.0,
        },
    }
    if alpha < 1.0:
        mat["alphaMode"] = "BLEND"
    return mat


names = ["oit_backdrop"] + ["oit_card_%02d" % k for k in range(CARDS)]
materials = [unlit(names[0], [BACKGROUND_GREY] * 3, 1.0)]
materials += [unlit(names[k + 1], card_color(k), ALPHA) for k in range(CARDS)]

# Node order IS draw order (the traversal is depth-first, left to right), and it
# runs far to near. That is what makes --no-oit the exact answer here: unsorted
# back-to-front alpha blending in scene order IS sorted back-to-front blending.
gltf = {
    "asset": {"version": "2.0", "generator": "gen_oit_cards_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": list(range(len(names)))}],
    "nodes": [{"name": n, "mesh": i} for i, n in enumerate(names)],
    "meshes": [
        {"name": n,
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                         "indices": 3 + i, "material": i}]}
        for i, n in enumerate(names)
    ],
    "materials": materials,
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": all_min, "max": all_max},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
    ] + [
        {"bufferView": 3, "byteOffset": i * 24, "componentType": 5125, "count": 6,
         "type": "SCALAR"}
        for i in range(len(index_runs))
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

# The lamp is switched off, and it is here precisely so that it can be: the
# render app installs a default three-point rig on any scene that declares no
# lights at all, and even against black albedo those three would leave a 0.04
# dielectric specular lobe on every card -- a view-dependent term riding on top
# of the constant the gate predicts. Declaring one dark light suppresses the rig
# and emits nothing.
cscn = {
    "version": 1,
    "models": [{"path": "oit_cards_fixture.gltf"}],
    "lights": [{"name": "oit_cards_no_rig", "type": "directional",
                "direction": [0.0, -1.0, 0.0], "intensity": 0.0}],
    "camera": {"eye": [0.0, CAM_Y, CAM_Z], "target": [0.0, CAM_Y, 0.0], "fov": FOVY_DEG},
    "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False,
             "bloom": {"enabled": False}},
}

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "oit_cards_fixture.gltf")
with open(out, "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
scene_out = os.path.join(here, "oit_cards_fixture.cscn")
with open(scene_out, "w") as f:
    json.dump(cscn, f, indent=1)
    f.write("\n")
print("wrote %s (%d cards, alpha %.2f, view distance %.0f..%.0f)"
      % (out, CARDS, ALPHA, NEAR_DIST, FAR_DIST))
print("wrote %s (eye 0,%.3f,%.3f target 0,%.3f,0 fovy %.0f)"
      % (scene_out, CAM_Y, CAM_Z, CAM_Y, FOVY_DEG))
print("bounds min %s max %s (recenter is a no-op when x/z centre on 0 and min y is 0)"
      % (["%.2f" % v for v in all_min], ["%.2f" % v for v in all_max]))
