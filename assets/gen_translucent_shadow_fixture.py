#!/usr/bin/env python3
"""Generate assets/translucent_shadow_fixture.* -- the translucent-shadow
instrument (spec 11.26).

Nothing in assets/ could measure this before: there are ZERO ALPHA_MASK
materials in the whole corpus, and the two committed assets with real texture
alpha (oit_sphere_fixture, abandoned_window) both render 0 px against
--no-shadows because neither declares a shadow-casting light.

The trick that makes the answer knowable is the OIT card stack's, transposed
from the eye ray to the light ray. Transmittance through N translucent layers is

    T = prod(1 - a_k)

which is ORDER-INDEPENDENT, and identical to exp(-sum(-ln(1 - a_k))). So an
implementation that accumulates absorbance additively satisfies the same
prediction as one that multiplies transmittances: **the gate constrains the
answer, not the storage.**

Six disjoint ground regions, one render:

  staircase   4 BLEND panels, nested x-extents, alpha 0.35
              band j (j = 0..4) sees j panels -> T = 0.65^j
  ramp        1 MASK panel over a texture whose alpha ramps 0 -> 1 across u
              -> T(x) = 1 - alpha(x), a straight line
  red         1 BLEND panel, alpha 0.5, saturated red
              -> T = 0.5 in ALL THREE channels unless coloured transmittance ships
  opaque      a box -> T ~ 0, in the same frame as the translucent regions
  acne        bare ground -> T = 1
  (band 0 of the staircase is bare ground too, and doubles as the T=1 reference)

Why the panels are horizontal and the sun is near-vertical: a grazing stack
would put fractional layer counts at the band edges and there would be nothing
to predict. Elevation 75 rather than 90 keeps `light_space_up` (shadow.c:254) off
its degenerate branch (|dot| = 0.966 < 0.99); azimuth 180 puts the cast band
TOWARD the camera, so no caster is ever on a sight line to its own shadow and the
gate needs no visibility guard.

The alpha is authored as baseColorFactor[3] on the BLEND panels -- no base-colour
texture -- so coverage is exactly the authored number. The ramp is the one place
a texture is unavoidable: a MASK material's alpha comes from the texture, and a
constant one would be binary rather than graded, which is precisely the case the
`tsl-mask-ramp` arm exists to tell apart.

The sibling .cscn is generated here, not hand-authored, for gen_oit_cards's
reason: the camera is DERIVED (it must frame the ground and exclude the canopy),
so a copied one would stop framing the scene the moment a distance changed.

Regenerate with:
  python3 assets/gen_translucent_shadow_fixture.py
"""

import base64
import json
import math
import os
import struct
import zlib

# --- the canopy ------------------------------------------------------------
PANELS = 4
ALPHA = 0.35            # 1 - 0.35 = 0.65; band 4 lands at 0.1785, well clear of 0
PANEL_Y0 = 8.00         # first panel height
PANEL_DY = 0.04         # separation, so the panels are distinct depths
RAMP_Y = 8.20
RED_Y = 8.16

# Nested x-extents. Band j spans [BAND_X0 + j*BAND_W, BAND_X0 + (j+1)*BAND_W]
# and sees j panels, so panel k must cover bands k+1..PANELS.
BAND_X0 = -7.0
BAND_W = 2.0
CANOPY_XR = BAND_X0 + (PANELS + 1) * BAND_W   # 3.0

# z bands, kept disjoint on the GROUND after the sun's shift (below). They sit
# at negative z because the shift is +z and the camera looks down the +z axis:
# the frame's lower edge reaches only z ~ +3.7, so a band authored at z 0 would
# cast its shadow off the bottom of the picture. The generator prints the cast
# extents for exactly this reason -- check them against VISIBLE_Z below.
STAIR_Z = (-6.5, -4.5)
RAMP_Z = (-3.5, -1.5)
RED_Z = (-0.5, 0.5)

# --- ground and furniture --------------------------------------------------
GROUND_HALF = 12.0
GROUND_GREY = 0.55      # grey, so the Neutral tonemap's min-channel offset is
                        # exact per channel and the gate can invert it
OPAQUE_X = (5.0, 7.0)   # off to the side of every translucent band in x
OPAQUE_Z = (-4.0, -2.0)
OPAQUE_H = 1.6
ACNE_X = (-11.0, -8.0)  # bare ground, clear of every cast band: T must be 1

# --- sun -------------------------------------------------------------------
SUN_ELEV_DEG = 75.0
# direction travels +Z and -Y: a caster at height h lands h*cot(elev) further +Z
SUN_DIR = [0.0, -math.sin(math.radians(SUN_ELEV_DEG)),
           math.cos(math.radians(SUN_ELEV_DEG))]
SHIFT = PANEL_Y0 / math.tan(math.radians(SUN_ELEV_DEG))   # 2.14 at y=8, elev 75

# --- camera ----------------------------------------------------------------
CAM_EYE = [0.0, 5.0, 8.0]
CAM_TARGET = [0.0, 0.0, 0.0]
FOVY_DEG = 35.0
ASPECT = 1.6            # the 800x500 the gate and the golden render at

RAMP_PX = 512


def _visible_z():
    """Ground z the frame actually contains, from the camera alone.

    Asserted against every cast band below rather than checked by eye: the bands
    are placed by the sun's shift, so a change to the elevation or the canopy
    height silently walks a band off the picture and every arm reading it starts
    measuring bare ground -- which passes a floor and fails nothing.
    """
    dz = CAM_TARGET[2] - CAM_EYE[2]
    dy = CAM_TARGET[1] - CAM_EYE[1]
    pitch = math.atan2(dy, abs(dz))                       # negative, looking down
    half = math.radians(FOVY_DEG) * 0.5
    near = CAM_EYE[2] - CAM_EYE[1] / math.tan(-(pitch - half))
    far = CAM_EYE[2] - CAM_EYE[1] / math.tan(-(pitch + half))
    return far, near


VISIBLE_Z = _visible_z()


def png_rgba(width, height, rows):
    """Minimal RGBA8 PNG. No PIL in this tree; every other generator that needs
    a texture writes one this way."""
    raw = b"".join(b"\x00" + bytes(row) for row in rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


positions, normals, uvs = [], [], []
index_runs = []


def add_xz_quad(x0, x1, z0, z1, y, flip_u=False):
    """A quad in the XZ plane at height `y`, facing +Y (up, toward the sun).

    Winding is counter-clockwise seen from +Y so the front face points at the
    light -- gen_area_shadow_fixture.py's convention, the opposite of the XY
    quads in gen_oit_cards_fixture.py.
    """
    base = len(positions)
    corners = ((x0, z1), (x1, z1), (x1, z0), (x0, z0))
    for (x, z) in corners:
        positions.append((x, y, z))
        normals.append((0.0, 1.0, 0.0))
    # u runs with x, which is what makes the ramp's alpha a function of world x
    us = (0.0, 1.0, 1.0, 0.0) if not flip_u else (1.0, 0.0, 0.0, 1.0)
    vs = (0.0, 0.0, 1.0, 1.0)
    uvs.extend(zip(us, vs))
    index_runs.append([base + 0, base + 1, base + 2, base + 0, base + 2, base + 3])


def add_box(x0, x1, z0, z1, y0, y1):
    """An axis-aligned box, outward-facing. Only the top matters for casting,
    but a closed solid is what makes it read as furniture in the golden."""
    base = len(positions)
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1),
         (x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)]
    faces = [(4, 5, 6, 7), (1, 0, 3, 2), (0, 1, 5, 4),
             (2, 3, 7, 6), (3, 0, 4, 7), (1, 2, 6, 5)]
    nrm = [(0, 1, 0), (0, -1, 0), (0, 0, -1), (0, 0, 1), (-1, 0, 0), (1, 0, 0)]
    for f, n in zip(faces, nrm):
        b = len(positions)
        for i in f:
            positions.append(v[i])
            normals.append(n)
        uvs.extend(((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)))
        index_runs.append([b + 0, b + 1, b + 2, b + 0, b + 2, b + 3])
    return base


# 0: ground
add_xz_quad(-GROUND_HALF, GROUND_HALF, -GROUND_HALF, GROUND_HALF, 0.0)
# 1..PANELS: the staircase, panel k covering bands k+1..PANELS
for k in range(PANELS):
    add_xz_quad(BAND_X0 + (k + 1) * BAND_W, CANOPY_XR,
                STAIR_Z[0], STAIR_Z[1], PANEL_Y0 + k * PANEL_DY)
# PANELS+1: the mask ramp
add_xz_quad(BAND_X0 + BAND_W, CANOPY_XR, RAMP_Z[0], RAMP_Z[1], RAMP_Y)
# PANELS+2: the red panel
add_xz_quad(BAND_X0 + BAND_W, CANOPY_XR, RED_Z[0], RED_Z[1], RED_Y)
# PANELS+3: the opaque block
add_box(OPAQUE_X[0], OPAQUE_X[1], OPAQUE_Z[0], OPAQUE_Z[1], 0.0, OPAQUE_H)

pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
uv_bytes = b"".join(struct.pack("<2f", *u) for u in uvs)
idx_bytes = b"".join(struct.pack("<I", i) for run in index_runs for i in run)
buffer_bytes = pos_bytes + nrm_bytes + uv_bytes + idx_bytes

all_min = [min(p[k] for p in positions) for k in range(3)]
all_max = [max(p[k] for p in positions) for k in range(3)]

# The ramp texture: white RGB, alpha linear in u. White so the MASK panel's own
# shading cannot tint what lands under it, and linear because a straight line is
# preserved exactly by any symmetric filter -- PCF, bilinear, a mip -- so the
# `tsl-mask-ramp` arm can only fail on a wrong LAW, never on filtering.
ramp_rows = []
for _ in range(4):
    row = []
    for x in range(RAMP_PX):
        a = int(round(255.0 * (x + 0.5) / RAMP_PX))
        row += [255, 255, 255, a]
    ramp_rows.append(row)
ramp_png = png_rgba(RAMP_PX, 4, ramp_rows)


def matte(name, rgb, alpha=1.0, mode=None, cutoff=None):
    mat = {
        "name": name,
        "doubleSided": True,
        "pbrMetallicRoughness": {
            "baseColorFactor": list(rgb) + [alpha],
            "metallicFactor": 0.0,
            "roughnessFactor": 1.0,
        },
    }
    if mode:
        mat["alphaMode"] = mode
    if cutoff is not None:
        mat["alphaCutoff"] = cutoff
    return mat


names = ["tsl_ground"]
materials = [matte("tsl_ground", [GROUND_GREY] * 3)]
for k in range(PANELS):
    names.append("tsl_panel_%d" % k)
    materials.append(matte("tsl_panel_%d" % k, [0.8, 0.8, 0.8], ALPHA, "BLEND"))
names.append("tsl_ramp")
ramp_mat = matte("tsl_ramp", [1.0, 1.0, 1.0], 1.0, "MASK", 0.5)
ramp_mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
materials.append(ramp_mat)
names.append("tsl_red")
materials.append(matte("tsl_red", [0.9, 0.05, 0.05], 0.5, "BLEND"))
names.append("tsl_opaque")
materials.append(matte("tsl_opaque", [0.25, 0.25, 0.25]))

# The box contributed six index runs, so its primitives share one material.
box_runs = list(range(len(index_runs) - 6, len(index_runs)))
prim_runs = [[i] for i in range(len(index_runs) - 6)] + [box_runs]

gltf = {
    "asset": {"version": "2.0", "generator": "gen_translucent_shadow_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": list(range(len(names)))}],
    "nodes": [{"name": n, "mesh": i} for i, n in enumerate(names)],
    "meshes": [
        {"name": n,
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                         "indices": 3 + r, "material": i} for r in prim_runs[i]]}
        for i, n in enumerate(names)
    ],
    "materials": materials,
    "textures": [{"source": 0, "sampler": 0}],
    "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}],
    "images": [{"uri": "translucent_shadow_ramp.png"}],
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

# One authored sun, casting. Without `cast_shadows` this fixture measures
# nothing at all -- which is exactly why oit_sphere_fixture and abandoned_window
# are both 0 px against --no-shadows and could not be reused here.
cscn = {
    "version": 1,
    "models": [{"path": "translucent_shadow_fixture.gltf"}],
    "lights": [{"name": "tsl_sun", "type": "directional", "direction": SUN_DIR,
                "intensity": 3.0, "cast_shadows": True}],
    "camera": {"eye": CAM_EYE, "target": CAM_TARGET, "fov": FOVY_DEG},
    "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False,
             "bloom": {"enabled": False}},
}

here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "translucent_shadow_ramp.png"), "wb") as f:
    f.write(ramp_png)
with open(os.path.join(here, "translucent_shadow_fixture.gltf"), "w") as f:
    json.dump(gltf, f, indent=1)
    f.write("\n")
with open(os.path.join(here, "translucent_shadow_fixture.cscn"), "w") as f:
    json.dump(cscn, f, indent=1)
    f.write("\n")

for label, (z0, z1) in (("staircase", STAIR_Z), ("ramp", RAMP_Z), ("red", RED_Z)):
    c0, c1 = z0 + SHIFT, z1 + SHIFT
    assert VISIBLE_Z[0] < c0 and c1 < VISIBLE_Z[1], (
        "%s casts to z %.2f..%.2f, outside the visible ground z %.2f..%.2f"
        % (label, c0, c1, VISIBLE_Z[0], VISIBLE_Z[1]))
assert all_min[0] == -all_max[0] and all_min[2] == -all_max[2] and all_min[1] == 0.0, \
    "bounds must centre in x/z with base y=0 or the app's recenter moves the scene"

print("panels %d alpha %.2f -> T = %s"
      % (PANELS, ALPHA, ", ".join("%.4f" % (1 - ALPHA) ** j for j in range(PANELS + 1))))
print("sun elevation %.0f -> ground shift %+.3f in z" % (SUN_ELEV_DEG, SHIFT))
print("staircase casts to z %.2f..%.2f, ramp %.2f..%.2f, red %.2f..%.2f"
      % (STAIR_Z[0] + SHIFT, STAIR_Z[1] + SHIFT,
         RAMP_Z[0] + SHIFT, RAMP_Z[1] + SHIFT,
         RED_Z[0] + SHIFT, RED_Z[1] + SHIFT))
print("bands in x: " + ", ".join(
    "band %d [%.1f, %.1f]" % (j, BAND_X0 + j * BAND_W, BAND_X0 + (j + 1) * BAND_W)
    for j in range(PANELS + 1)))
print("visible ground z %.2f..%.2f (every cast band asserted inside it)" % VISIBLE_Z)
print("bounds min %s max %s (recenter is a no-op when x/z centre on 0 and min y is 0)"
      % (["%.2f" % v for v in all_min], ["%.2f" % v for v in all_max]))
