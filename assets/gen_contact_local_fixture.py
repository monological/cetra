#!/usr/bin/env python3
"""Generate the local-light contact-shadow fixture (spec 11.56).

assets/contact_fixture.gltf is the corpus's contact-shadow asset and it cannot
serve this feature: its sibling .cscn ships a procedural sky, so the ONLY light
in it is a shadow-casting directional -- exactly the population 11.56 does not
touch. Nothing in the corpus put a local light near a contact, and so nothing
could tell whether one casts a contact shadow at all.

What this fixture is built around is the screen-space march's one hard
requirement: the ray from the receiver toward the light must pass BEHIND a
surface the camera can see. A cube resting on the ground with the receiver strip
in FRONT of it and the occluded light BEHIND it satisfies that exactly -- the
face the ray crosses is the cube's camera-facing front, at full incidence -- and
a configuration that does not is not a weaker fixture, it is one that reads
"no contact shadow" for a reason that has nothing to do with the feature.

    practical_front (0, LIGHT_Y, STRIP_Z + LIGHT_DZ)   camera
                    o                                    @
                     \\                                  /
                      \\   +-------+                    /
        strip ---------- x |  cube |
      (z = STRIP_Z)     /  +-------+
                       /       |
                      o        |  practical_back (0, LIGHT_Y, STRIP_Z - LIGHT_DZ)
    ground -----------+--------+------------------

The coordinates are symbolic on purpose: the pair is DERIVED from STRIP_Z, and a
diagram carrying numbers is a hand-typed copy that drifts the moment the strip
moves. This one already had.

TWO LIGHTS, MIRRORED ABOUT THE STRIP, and the mirror is the whole instrument.
Both sit at x = 0 and the same height, equidistant from the strip, so at EVERY
point of the line z = STRIP_Z they have identical distance and identical N.L --
their contribution weights therefore differ by exactly the ratio of their
authored intensities, and nothing else. One of them is occluded by the cube and
the other is not, so a contribution-weighted fold of the two visibilities has a
value predictable in closed form from the intensities alone:

    1 - vis  =  occ_back * I_back / (I_back + I_front)

An UNWEIGHTED fold reads the same number whatever the intensities are, which is
what makes scaling one light the decisive test of the weighting rather than of
the march.

Neither light casts a shadow map, which is the point: the punctual atlas holds 8
layers and a point light spends 6, so past the first one there is no map to be
had, and a light authored cast_shadows FALSE never had one. A gate arm turns the
BACK light's map on to check the march then skips it -- with the front light
unoccluded, the strip must return to exactly 1.

Two glTFs come out of one script so the falsifier is mechanical: contact_local_bare
is the same ground with the cube deleted and nothing else changed, so an arm that
swaps the model path is varying the occluder and only the occluder.

Regenerate the geometry AND the scene with:
  python3 assets/gen_contact_local_fixture.py
"""

import base64
import json
import math
import os
import struct

# ---- the geometry, and the numbers every assert below is written against ----
GROUND_HALF = 5.0     # ground quad half-extent
CUBE_HALF = 0.40      # cube half-width in x and z
CUBE_TOP = 0.50       # cube height; it RESTS on the ground, so the base is y = 0
STRIP_Z = 0.55        # receiver line, in front of the cube's z = +CUBE_HALF face
STRIP_HALF_X = 0.30   # how much of the line an arm may sample and stay behind the cube

# Mirrored about the strip: same y, same |dz|, so the weight ratio at the strip
# IS the intensity ratio (see the module docstring). DERIVED from STRIP_Z rather
# than written out, because the mirror is the fixture -- a hand-typed pair drifts
# the moment the strip moves, and the drift is a wrong prediction, not a failure.
# Both at x = 0, which extends the mirror along the whole line rather than
# holding it at one point.
LIGHT_Y = 1.5
LIGHT_DZ = 3.4
# Rounded at the CONSTANT rather than on the way out, so the asserts below and
# the emitted scene hold the same number to the bit -- rounding at the write
# would put a hair between what is checked and what is rendered, which is the
# whole failure this derivation exists to avoid.
LIGHT_BACK = (0.0, LIGHT_Y, round(STRIP_Z - LIGHT_DZ, 6))
LIGHT_FRONT = (0.0, LIGHT_Y, round(STRIP_Z + LIGHT_DZ, 6))
# EQUAL on both, and load-bearing rather than tidy: the fold's weight is
# luminance x getDistanceAtt x N.L, so an unequal colour or range makes the
# weight ratio something other than the intensity ratio and contact-fold predicts
# a number it cannot derive. An AUTHORED range also keeps the arm's intensity
# scaling a pure weight change -- without one, light_cull_radius derives the cull
# sphere from intensity and scaling would resize that too.
LIGHT_INTENSITY = 12.0
LIGHT_RANGE = 12.0

# The camera the arms render through, emitted into the .cscn below so there is
# one copy. Every assert here is a claim about that frame, and a fixture whose
# asserts pass against a camera nothing renders is asserting nothing.
CAM_EYE = (0.0, 1.9, 4.4)
CAM_TARGET = (0.0, 0.25, 0.0)
CAM_FOV_DEG = 42.0
# Mirrored from the gate's render size, which nothing can share; asserts 2 and 7
# are claims about THAT frame, so a gate rendering another shape invalidates them.
GATE_W, GATE_H = 640.0, 400.0
CAM_ASPECT = GATE_W / GATE_H
CS_DISTANCE = 0.8

# Mirrored from contact_shadow_frag.glsl. Not shared -- a shader constant cannot
# be imported into Python, and the point of restating them is that a change to
# either one has to come here and re-run the asserts.
MAX_UV_LEN = 0.15     # the march's screen-reach clamp
GRAZE_LIFT = 4.0      # slope-scaled start-offset gain

# The in-frame control: ground on the SAME scan line as the strip that the cube
# cannot reach. Same line because that makes the two reads one row of pixels, so
# a projection off by a texel moves both together.
OPEN_X = 1.5
OPEN_HALF_X = 0.20


def _sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def _norm(v):
    m = math.sqrt(_dot(v, v))
    return tuple(c / m for c in v)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _uv_projector():
    """World -> [0,1] screen UV for the fixture camera. cglm's fov is VERTICAL.

    The pixel-space twin is scripts/gates.py::_projector, which flips V. Two
    copies because they run in separate processes with nothing importable
    between them -- gates.py cannot import this module (it writes its assets at
    import), and this module cannot import gates.py (fixture-gen runs it from a
    scratch tree holding only the generators). The asymmetry is what makes the
    duplication tolerable: _projector drifting fails twenty gate groups, this one
    drifting fails only the asserts below.
    """
    fwd = _norm(_sub(CAM_TARGET, CAM_EYE))
    right = _norm(_cross(fwd, (0.0, 1.0, 0.0)))
    up = _cross(right, fwd)
    ty = 1.0 / math.tan(math.radians(CAM_FOV_DEG) * 0.5)
    tx = ty / CAM_ASPECT

    def project(p):
        d = _sub(p, CAM_EYE)
        vx, vy, vz = _dot(d, right), _dot(d, up), -_dot(d, fwd)
        return ((vx * tx) / -vz * 0.5 + 0.5, (vy * ty) / -vz * 0.5 + 0.5, vz)

    return project


PROJECT = _uv_projector()


def _march_start(point, to_light):
    """Where the shader's ray actually begins: the start offset it lifts by.

    Mirrors marchOcclusion's opening in contact_shadow_frag.glsl -- the
    slope-scaled grazeLift and the max(0.02, 0.01 * viewZ) floor. A grazing ray
    gets up to (1 + GRAZE_LIFT)x the base offset, so a fixture that ignores it
    can be a whole step out about where the ray crosses the occluder.
    """
    ndl = to_light[1]   # the receiver is the ground plane, so N = +Y
    t = min(max((ndl - 0.5) / (0.1 - 0.5), 0.0), 1.0)
    graze = 1.0 + GRAZE_LIFT * (t * t * (3.0 - 2.0 * t))
    return max(0.02, 0.01 * -PROJECT(point)[2]) * graze


# The scene the arms render, emitted here rather than hand-authored beside the
# geometry. Every assert below is a claim about what those arms will see, and a
# fixture asserting against a camera or a light pair nothing renders is asserting
# nothing -- which is the failure this file's own header warns about and the
# reason five other generators in this directory emit their .cscn too.
def _practical(name, pos):
    return {"name": name, "type": "point", "position": list(pos),
            "color": [1.0, 1.0, 1.0], "intensity": LIGHT_INTENSITY,
            "intensity_unit": "candela", "range": LIGHT_RANGE, "cast_shadows": False}


CSCN = {
    "version": 1,
    "models": [{"path": "contact_local_fixture.gltf"}],
    "environment": {"ambient": [0.04, 0.042, 0.05]},
    "lights": [_practical("practical_back", LIGHT_BACK),
               _practical("practical_front", LIGHT_FRONT)],
    "camera": {"eye": list(CAM_EYE), "target": list(CAM_TARGET), "fov": CAM_FOV_DEG},
    "post": {"tonemap": "neutral", "exposure": 1.0},
}

_SAMPLE = (0.0, 0.0, STRIP_Z)
_TO_BACK = _norm(_sub(LIGHT_BACK, _SAMPLE))
_START_BIAS = _march_start(_SAMPLE, _TO_BACK)

# Where the occluded ray crosses the cube's camera-facing front face...
HIT_T = (STRIP_Z - CUBE_HALF) / -_TO_BACK[2]
# ...and how far the march gets after the shader's SCREEN clamp, which is what
# actually binds: a march is limited by how far it travels across the FRAME, not
# by how far it travels in the world, so a fixture checked only against
# CS_DISTANCE can be short by a factor of two.
_SU, _SV, _ = PROJECT(_SAMPLE)
_EU, _EV, _ = PROJECT(tuple(_SAMPLE[k] + CS_DISTANCE * _TO_BACK[k] for k in range(3)))
UV_LEN = math.hypot(_EU - _SU, _EV - _SV)
REACH = CS_DISTANCE * min(1.0, MAX_UV_LEN / UV_LEN)


def _check_geometry():
    """Every way this fixture can silently stop testing what it claims to."""
    # 1. The occluded ray must cross the cube's front face BELOW its top, or the
    #    march sails over the cube and the strip is lit for a geometric reason.
    y_hit = _START_BIAS + HIT_T * _TO_BACK[1]
    assert y_hit < CUBE_TOP - 0.1, (
        "the ray toward practical_back clears the cube (y %.3f vs top %.2f)" % (y_hit, CUBE_TOP))

    # 2. ...and it must land inside the screen-clamped reach.
    assert HIT_T < REACH * 0.75, (
        "the cube sits past the march's screen-clamped reach (hit %.3f vs %.3f)"
        % (HIT_T, REACH))

    # 3. Everything the fold's weight ratio is made of EXCEPT intensity. The
    #    arms predict 1 - vis = occ * I_back/(I_back + I_front), and that is the
    #    weight ratio only while the pair agrees on distance, N.L, range (the
    #    shader's getDistanceAtt windows on 1/range^2) and colour (the weight is
    #    a luminance). The mirror itself is a CONSTRUCTION -- LIGHT_BACK and
    #    LIGHT_FRONT are derived from STRIP_Z -- so asserting it here would only
    #    restate the two lines above; what needs checking is the emitted scene,
    #    where a hand-edit or a new key could break the pair without touching them.
    back, front = CSCN["lights"]
    for key in ("range", "color", "intensity", "intensity_unit"):
        assert back[key] == front[key], (
            "the practicals differ in %s; the fold's share is no longer the intensity ratio"
            % key)
    assert back["position"][1] == front["position"][1], (
        "the practicals no longer share an N.L at the strip")
    assert abs((STRIP_Z - back["position"][2]) - (front["position"][2] - STRIP_Z)) < 1e-9, (
        "the practicals are no longer mirrored about the strip")
    # Dropping `range` does not merely change the falloff: light_cull_radius then
    # derives a radius from INTENSITY, so contact-fold's scaling would resize the
    # cull sphere as well as the weight and stop being a pure weight change.
    assert back["range"] > 0.0, "an authored range is what keeps intensity a pure weight change"

    # 4. The unoccluded ray must stay unoccluded: it leaves toward +z, away from
    #    the cube, so nothing can be in it.
    to_front = _norm(_sub(LIGHT_FRONT, _SAMPLE))
    assert to_front[2] > 0.0, "practical_front no longer sits in front of the strip"

    # 5. Both rays must survive the shader's N.L cull with room to spare, or the
    #    strip reads 1 because the march never ran.
    assert _TO_BACK[1] > 0.30, "the occluded ray grazes: N.L %.3f" % _TO_BACK[1]

    # 6. The in-frame control must be out of reach of the cube along its whole
    #    span, in the ONE direction that could reach it. Its job is to read
    #    exactly 1 in the same frame -- and on the same scan line -- that the
    #    strip reads dark.
    open_near = (OPEN_X - OPEN_HALF_X, 0.0, STRIP_Z)
    open_dir = _norm(_sub(LIGHT_BACK, open_near))
    x_at_reach = open_near[0] + CS_DISTANCE * open_dir[0]
    assert x_at_reach > CUBE_HALF + 0.5, (
        "the control's march comes within %.2f of the cube in x" % (x_at_reach - CUBE_HALF))

    # 7. Everything an arm reads has to be ON SCREEN, with margin. Off-frame is a
    #    silent zero.
    for name, pt in (("strip -x", (-STRIP_HALF_X, 0.0, STRIP_Z)),
                     ("strip +x", (STRIP_HALF_X, 0.0, STRIP_Z)),
                     ("control -x", open_near),
                     ("control +x", (OPEN_X + OPEN_HALF_X, 0.0, STRIP_Z))):
        u, v, vz = PROJECT(pt)
        assert vz < 0.0 and 0.04 < u < 0.96 and 0.04 < v < 0.96, (
            "%s lands off frame at uv (%.3f, %.3f)" % (name, u, v))


_check_geometry()

# ---- mesh building ----------------------------------------------------------
positions, normals, uvs, indices = [], [], [], []


def add_quad(p0, p1, p2, p3, n):
    """One flat quad, four vertices, two CCW triangles seen from +n."""
    base = len(positions)
    for p in (p0, p1, p2, p3):
        positions.append(p)
        normals.append(n)
    uvs.extend([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)])
    indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])


def add_box(hx, y0, y1, hz):
    """Axis-aligned box centred on x = z = 0, standing on y = y0.

    Flat-shaded (each face its own four vertices), because the march tests the
    depth of the FRONT face and a smoothed corner would round it.
    """
    add_quad((-hx, y0, hz), (hx, y0, hz), (hx, y1, hz), (-hx, y1, hz), (0.0, 0.0, 1.0))
    add_quad((hx, y0, -hz), (-hx, y0, -hz), (-hx, y1, -hz), (hx, y1, -hz), (0.0, 0.0, -1.0))
    add_quad((hx, y0, hz), (hx, y0, -hz), (hx, y1, -hz), (hx, y1, hz), (1.0, 0.0, 0.0))
    add_quad((-hx, y0, -hz), (-hx, y0, hz), (-hx, y1, hz), (-hx, y1, -hz), (-1.0, 0.0, 0.0))
    add_quad((-hx, y1, hz), (hx, y1, hz), (hx, y1, -hz), (-hx, y1, -hz), (0.0, 1.0, 0.0))


add_quad((-GROUND_HALF, 0.0, GROUND_HALF), (GROUND_HALF, 0.0, GROUND_HALF),
         (GROUND_HALF, 0.0, -GROUND_HALF), (-GROUND_HALF, 0.0, -GROUND_HALF), (0.0, 1.0, 0.0))
ground_index_count = len(indices)

add_box(CUBE_HALF, 0.0, CUBE_TOP, CUBE_HALF)


def matte(name, color):
    return {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": list(color) + [1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 0.9,
        },
    }


def write_gltf(path, with_cube):
    """Serialise the shared vertex arrays, optionally without the cube.

    The cube's vertices are left in the buffer of the bare twin and only its
    index range is dropped: the two files then share byte-identical position,
    normal and UV data, so nothing but the drawn geometry can differ between them.
    """
    pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
    nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
    uv_bytes = b"".join(struct.pack("<2f", *u) for u in uvs)
    idx_bytes = b"".join(struct.pack("<I", i) for i in indices)
    buf = pos_bytes + nrm_bytes + uv_bytes + idx_bytes

    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
         "min": [min(p[k] for p in positions) for k in range(3)],
         "max": [max(p[k] for p in positions) for k in range(3)]},
        {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5125, "count": ground_index_count, "type": "SCALAR"},
    ]
    attrs = {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}
    meshes = [{"name": "contact_local_ground",
               "primitives": [{"attributes": attrs, "indices": 3, "material": 0}]}]
    nodes = [{"name": "contact_local_ground", "mesh": 0}]
    materials = [matte("contact_local_ground", (0.55, 0.55, 0.55))]

    if with_cube:
        accessors.append({"bufferView": 3, "byteOffset": ground_index_count * 4,
                          "componentType": 5125,
                          "count": len(indices) - ground_index_count, "type": "SCALAR"})
        materials.append(matte("contact_local_cube", (0.60, 0.58, 0.55)))
        meshes.append({"name": "contact_local_cube",
                       "primitives": [{"attributes": attrs, "indices": 4, "material": 1}]})
        nodes.append({"name": "contact_local_cube", "mesh": 1})

    gltf = {
        "asset": {"version": "2.0", "generator": "gen_contact_local_fixture.py"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes), "byteLength": len(nrm_bytes),
             "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes),
             "byteLength": len(uv_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes) + len(uv_bytes),
             "byteLength": len(idx_bytes), "target": 34963},
        ],
        "buffers": [
            {"uri": "data:application/octet-stream;base64,"
             + base64.b64encode(buf).decode("ascii"), "byteLength": len(buf)}
        ],
    }
    with open(path, "w") as f:
        json.dump(gltf, f, indent=1)
        f.write("\n")


here = os.path.dirname(os.path.abspath(__file__))
write_gltf(os.path.join(here, "contact_local_fixture.gltf"), with_cube=True)
write_gltf(os.path.join(here, "contact_local_bare.gltf"), with_cube=False)
with open(os.path.join(here, "contact_local_fixture.cscn"), "w") as fh:
    json.dump(CSCN, fh, indent=1)
    fh.write("\n")
print("wrote contact_local_fixture.gltf, contact_local_bare.gltf and contact_local_fixture.cscn")
print("  occluded ray crosses the cube at t=%.3f, screen-clamped reach %.3f (uv %.3f)"
      % (HIT_T, REACH, UV_LEN))
