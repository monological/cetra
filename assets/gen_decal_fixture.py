#!/usr/bin/env python3
"""Generate the clustered-decal instrument -- assets/decal_fixture.* (spec 11.73).

A FLOOR, a WALL facing the camera, and an OBLIQUE PLATE, with two decals: a
poster of exact painted codes on the wall, and a scorch carrying a surface map
on the floor.

GROUND TRUTH IS PAINTED, NOT MEASURED, the layer fixture's rule and for its
reason. Every colour arm reads the frame through `--render-mode 6`, the albedo
view, where a correct renderer hands back the exact code this script wrote with
no lighting, exposure or tonemap in the path to argue about. A decal's albedo is
stored in the material texture array as authored sRGB codes and decoded once
after the blend, so a hardware decode creeping in -- or one applied twice --
moves the poster's band and nothing else.

WHAT EACH PIECE IS FOR, since none of it is decoration:

  wall      Faces the camera square on, and carries the poster. The plate a
            projector is SUPPOSED to mark.
  oblique   The anti-vacuity, and the whole reason a third plate exists. It sits
            inside the poster's box and past its angle fade, so a correct build
            leaves it at the substrate's own code. Delete the facing test and a
            projector smears its image down every surface it grazes -- which is
            the classic decal artifact, and without this plate no arm here could
            see it. Its angle is derived from ANGLE_FADE below, not guessed.
  floor     Carries the scorch, whose surface map is the only thing in the
            fixture that can move roughness or a normal. A poster with an albedo
            alone leaves the second splice half unexercised.

  poster    Hard-edged colour bands plus a BRIGHT BORDER ring one texel wide.
            The border is not decoration either: the material array wraps with
            GL_REPEAT, so without the half-texel inset a tap at one edge
            bilinears against the opposite one and the border bleeds across the
            far side of the mark. The bands are flat so a read is a code and not
            a gradient anyone has to model.
  scorch    Its albedo is flat, so what it contributes to a colour read is
            nothing; what it carries is the packed surface map -- normal.xy,
            roughness in b, occlusion in a -- at values no substrate has.

Both images carry a fully TRANSPARENT margin, which is what makes the edge
feather measurable at all: an image opaque to its own border cannot show the
difference between the feather ramping and the box clipping.

Regenerate with: python3 assets/gen_decal_fixture.py
"""

import base64
import json
import math
import os
import struct

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))

TEX = 64  # every painted image is TEX x TEX

# --- the marks --------------------------------------------------------------
# Codes chosen away from both endpoints and from each other, so a misread is a
# large number rather than a plausible one.
# The poster's four quadrants, named by where they sit IN THE PNG (row 0 is the
# top). All four differ in every channel, so a mirror, a flip, a transpose and a
# 180-degree rotation each produce a different wrong answer rather than a
# plausible one -- the terrain fixture's "a transpose must move >500 codes" rule,
# applied to the thing a decal actually is.
POSTER_TL = (216, 64, 32)
POSTER_TR = (48, 200, 96)
POSTER_BL = (96, 112, 200)
POSTER_BR = (160, 32, 176)
# The ring, distinct from all four in every channel for the same reason: where
# it appears is an answer, so it must not be confusable with a quadrant.
POSTER_BORDER = (248, 248, 248)
SCORCH_CODE = (40, 40, 44)

# The scorch's surface map, packed as layers.glsl reads it.
SCORCH_ROUGH = 250  # against the substrate's own, which is far lower
SCORCH_AO = 120

# Fraction of each image that is fully transparent margin.
MARGIN = 0.125

# --- geometry ---------------------------------------------------------------
HALF = 3.0        # floor half-width
WALL_H = 4.0
WALL_Z = -3.0     # the wall's plane

# The poster's box, on the wall and facing +Z (out of the wall, toward the eye).
POSTER_POS = (-1.2, 1.6, WALL_Z + 0.25)
POSTER_HALF = (1.0, 1.0, 0.6)

# The scorch's box, on the floor and facing -Y.
SCORCH_POS = (1.3, 0.25, -0.6)
SCORCH_HALF = (0.9, 0.5, 0.9)

ANGLE_FADE = 60.0  # degrees; a surface tilted further takes no mark
FEATHER = 0.12

# The oblique plate's tilt away from the poster's facing direction. Past the
# fade by a real margin rather than a hair, so the arm is not reading the ramp.
OBLIQUE_DEG = 78.0
# Its half-width, and how far LEFT of the poster's centre it sits. It has to be
# inside the poster's box to be refused by the angle rather than by the box --
# and it stands in FRONT of the wall, so it must also leave the poster's own
# read point visible. Off to one side is what satisfies both.
OBLIQUE_HALF = 0.30
OBLIQUE_DX = -0.62

# Where each arm reads. Stated here, with the geometry, because a read point is
# part of what the fixture IS: an arm sampling a point some other plate occludes
# reads plausible bytes off the wrong surface, which is the failure this file's
# asserts exist to make impossible.
# Inside the poster's UPPER-RIGHT quadrant, so it returns POSTER_TR rather than
# whatever a seam rounds to -- it sat on the horizontal seam until the poster
# gained quadrants, which is exactly the read a one-colour image cannot notice.
POSTER_READ = (POSTER_POS[0] + 0.55 * POSTER_HALF[0],
               POSTER_POS[1] + 0.5 * POSTER_HALF[1], POSTER_POS[2])
OBLIQUE_READ = (POSTER_POS[0] + OBLIQUE_DX, POSTER_POS[1], POSTER_POS[2] + 0.25)
# Just inside the poster's RIGHT edge, where the image's own margin has faded
# out. The far side from it carries the border ring, so this is where a missing
# half-texel inset shows up: the array wraps with GL_REPEAT and the tap reaches
# across the image to a colour that cannot belong here.
# 0.70 of the half-extent: past the poster's centre toward its right edge, and
# comfortably inside the opaque region the margin leaves (asserted below).
EDGE_READ = (POSTER_POS[0] + 0.70 * POSTER_HALF[0], POSTER_POS[1], POSTER_POS[2])

# The four world points an orientation arm reads, at 0.5 of the half-extent so
# each sits well inside its own quadrant and away from the seams. Named for the
# VIEWER's frame -- the camera looks down -Z at the wall, so +x is the viewer's
# right and +y is up. Which PNG quadrant each ought to return is the whole
# question: the projector decides it, and until this fixture had four colours
# nothing in the suite could ask.
# The scorch's own centre, on the floor plane, and the two CONTROL points --
# substrate a decal must not have touched. Every arm reads a control in the same
# frame as its signal, so a build that painted the whole wall fails rather than
# reading plausibly.
SCORCH_READ = (SCORCH_POS[0], 0.0, SCORCH_POS[2])
WALL_CLEAR = (1.6, 1.6, WALL_Z)
FLOOR_CLEAR = (-1.8, 0.0, 1.2)

QUAD_READS = {
    "upper_right": (POSTER_POS[0] + 0.5 * POSTER_HALF[0],
                    POSTER_POS[1] + 0.5 * POSTER_HALF[1], POSTER_POS[2]),
    "upper_left": (POSTER_POS[0] - 0.5 * POSTER_HALF[0],
                   POSTER_POS[1] + 0.5 * POSTER_HALF[1], POSTER_POS[2]),
    "lower_right": (POSTER_POS[0] + 0.5 * POSTER_HALF[0],
                    POSTER_POS[1] - 0.5 * POSTER_HALF[1], POSTER_POS[2]),
    "lower_left": (POSTER_POS[0] - 0.5 * POSTER_HALF[0],
                   POSTER_POS[1] - 0.5 * POSTER_HALF[1], POSTER_POS[2]),
}


def _poster():
    """Four distinct QUADRANTS, a bright one-texel border, a transparent margin.

    The quadrants are the whole point and they were missing until the review:
    every earlier version of this image was invariant under horizontal mirror,
    vertical flip, transpose AND 180-degree rotation, so a projector that landed
    the picture backwards, upside down, or both painted a frame no arm could
    tell from a correct one. A decal whose pitch is "a poster, a logo" has to be
    able to fail that way in the fixture before it can be asserted not to.

    Named by where they sit IN THE PNG -- row 0 is the image's top -- so an arm
    can say which world corner each ought to reach.
    """
    img = np.zeros((TEX, TEX, 4), dtype=np.uint8)
    m = int(TEX * MARGIN)
    img[m:TEX - m, m:TEX - m, 3] = 255
    mid = TEX // 2
    img[m:mid, m:mid, :3] = POSTER_TL
    img[m:mid, mid:TEX - m, :3] = POSTER_TR
    img[mid:TEX - m, m:mid, :3] = POSTER_BL
    img[mid:TEX - m, mid:TEX - m, :3] = POSTER_BR
    # The ring, exactly one texel inside the opaque region on every side.
    img[m, m:TEX - m, :3] = POSTER_BORDER
    img[TEX - m - 1, m:TEX - m, :3] = POSTER_BORDER
    img[m:TEX - m, m, :3] = POSTER_BORDER
    img[m:TEX - m, TEX - m - 1, :3] = POSTER_BORDER
    return img


def _scorch_albedo():
    img = np.zeros((TEX, TEX, 4), dtype=np.uint8)
    m = int(TEX * MARGIN)
    img[m:TEX - m, m:TEX - m, :3] = SCORCH_CODE
    img[m:TEX - m, m:TEX - m, 3] = 255
    return img


def _scorch_surface():
    """Packed normal.xy + roughness + AO, the layer surface format."""
    img = np.zeros((TEX, TEX, 4), dtype=np.uint8)
    img[:, :, 0] = 128  # normal.x = 0
    img[:, :, 1] = 128  # normal.y = 0 -- flat, so the arm reads roughness alone
    img[:, :, 2] = SCORCH_ROUGH
    img[:, :, 3] = SCORCH_AO
    return img


def _assert_fixture_still_tests_something():
    # The oblique plate must be past the fade, or the angle arm is asserting
    # that a decal lands where it is supposed to land -- which is the other
    # arm's job and passes on a build with no facing test at all.
    assert OBLIQUE_DEG > ANGLE_FADE + 10.0, (
        f"the oblique plate at {OBLIQUE_DEG} deg is not clear of the "
        f"{ANGLE_FADE} deg fade; the angle arm would read the ramp")

    # ...and it must be INSIDE the poster's box, or it is refused by the box
    # test and the facing test is never reached.
    depth = abs(OBLIQUE_CENTER[2] - POSTER_POS[2])
    assert depth < POSTER_HALF[2], (
        f"the oblique plate sits {depth:.3f} from the poster's plane, outside "
        f"its {POSTER_HALF[2]} depth -- the box rejects it before the angle does")
    for axis, half, name in ((0, POSTER_HALF[0], "x"), (1, POSTER_HALF[1], "y")):
        off = abs(OBLIQUE_CENTER[axis] - POSTER_POS[axis])
        assert off < half, (
            f"the oblique plate's {name} centre is {off:.3f} from the poster's, "
            f"outside its {half} half-extent")

    # The border must survive the inset: a ring thinner than the half-texel the
    # shader clamps by would be unreadable whether or not the inset exists.
    assert int(TEX * MARGIN) >= 2, "the transparent margin is too thin to feather into"

    # The transparent margin has to be real, or the edge arm reads the box
    # clipping the image rather than the image's own alpha.
    poster = _poster()
    assert poster[0, 0, 3] == 0 and poster[TEX // 2, TEX // 2, 3] == 255, (
        "the poster is not transparent at its margin and opaque at its centre")

    # The border must be on the border. Painted at the wrong index it would sit
    # in the interior and the wrap arm would read it on both sides legitimately.
    m = int(TEX * MARGIN)
    assert tuple(poster[m, TEX // 2, :3]) == POSTER_BORDER, "the border ring moved"

    # The four quadrants must be four DIFFERENT colours, and differ in a way no
    # symmetry can undo -- this is the assert that makes the orientation arm
    # possible. An image invariant under mirror, flip, transpose or rotation
    # cannot fail an orientation test, which is what every earlier version of
    # this poster was.
    q = {"TL": POSTER_TL, "TR": POSTER_TR, "BL": POSTER_BL, "BR": POSTER_BR}
    marks = dict(q, RING=POSTER_BORDER)
    assert len(set(marks.values())) == 5, "the poster's marks are not five distinct colours"
    for a in marks:
        for b in marks:
            if a < b:
                assert all(abs(x - y) > 16 for x, y in zip(marks[a], marks[b])), (
                    f"{a} and {b} are within 16 codes in some channel; a "
                    f"misprojection would read as noise rather than as an answer")
    lo, hi = m + 1, TEX - m - 2  # inside the border ring
    mid = TEX // 2
    for name, (r, c) in (("TL", (lo, lo)), ("TR", (lo, hi)),
                         ("BL", (hi, lo)), ("BR", (hi, hi))):
        assert tuple(poster[r, c, :3]) == q[name], f"the {name} quadrant is not where it says"
    assert mid - m >= 4, "the quadrants are too small to sample away from their seams"

    # Every control point must be OUTSIDE the box of every decal, or it is not a
    # control -- it is a second read of the same mark. Checked against both
    # boxes rather than the obvious one, since a control's whole job is to be
    # untouched by anything.
    for pt, name in ((WALL_CLEAR, "wall"), (FLOOR_CLEAR, "floor")):
        for pos, half, dname in ((POSTER_POS, POSTER_HALF, "poster"),
                                 (SCORCH_POS, SCORCH_HALF, "scorch")):
            inside = all(abs(pt[k] - pos[k]) < half[k] for k in range(3))
            assert not inside, f"the {name} control sits inside the {dname}'s box"

    # ...and the scorch read must be INSIDE the scorch's box, on the floor.
    assert all(abs(SCORCH_READ[k] - SCORCH_POS[k]) < SCORCH_HALF[k] for k in range(3)), (
        "the scorch read point is outside the scorch's own box")

    # POSTER_READ must sit in ONE quadrant, clear of both seams, or it returns
    # whatever the seam rounds to and the arm reading it against a single
    # quadrant code is asserting a coin flip.
    for k, axis, edge in ((0, "x", POSTER_HALF[0]), (1, "y", POSTER_HALF[1])):
        off = POSTER_READ[k] - POSTER_POS[k]
        assert abs(off) > 0.15 * edge, (
            f"POSTER_READ is {abs(off):.3f} from the poster's {axis} seam; it must "
            f"be unambiguously inside one quadrant")
    assert POSTER_READ[0] > POSTER_POS[0] and POSTER_READ[1] > POSTER_POS[1], (
        "POSTER_READ is not in the upper-right quadrant its arm reads POSTER_TR for")

    # Every quadrant read must be inside the poster's box in x and y, and must
    # sit in the quadrant its name claims -- the arm's whole premise.
    for name, pt in QUAD_READS.items():
        for k, axis in ((0, "x"), (1, "y")):
            assert abs(pt[k] - POSTER_POS[k]) < POSTER_HALF[k], (
                f"the {name} quadrant read is outside the poster's box in {axis}")
        want_right = "right" in name
        want_upper = "upper" in name
        assert (pt[0] > POSTER_POS[0]) == want_right, f"{name} is not on the {'right' if want_right else 'left'}"
        assert (pt[1] > POSTER_POS[1]) == want_upper, f"{name} is not in the {'upper' if want_upper else 'lower'} half"

    # The oblique plate stands in FRONT of the wall, so it must not cover the
    # point the poster arm reads -- a sample the plate occludes returns the
    # plate's own colour, which is exactly what the angle arm expects to see and
    # so reads as a decal that failed to land. That was this fixture's first
    # draft, and the whole arm inverted on it.
    ob_lo = OBLIQUE_CENTER[0] - OBLIQUE_HALF
    ob_hi = OBLIQUE_CENTER[0] + OBLIQUE_HALF
    assert not (ob_lo <= POSTER_READ[0] <= ob_hi), (
        f"the oblique plate spans x {ob_lo:.2f}..{ob_hi:.2f} and covers the "
        f"poster's read point at x {POSTER_READ[0]:.2f}")

    # ...while the point the ANGLE arm reads must be on the plate.
    assert ob_lo <= OBLIQUE_READ[0] <= ob_hi, (
        "the angle arm's read point is not on the oblique plate")

    # The edge read is on the far side from the plate, for the same reason.
    assert not (ob_lo <= EDGE_READ[0] <= ob_hi), (
        "the oblique plate covers the edge read point")

    # Every read point must be inside the poster's box in x, or an arm is
    # asking about a region the decal never claimed.
    for pt, name in ((POSTER_READ, "poster"), (OBLIQUE_READ, "oblique"),
                     (EDGE_READ, "edge")):
        assert abs(pt[0] - POSTER_POS[0]) < POSTER_HALF[0], (
            f"the {name} read point is outside the poster's box in x")

    # The edge read must be inside the image's OPAQUE region -- past the
    # transparent margin -- or it reads bare substrate whether the inset exists
    # or not, and the arm passes on a build with no clamp at all.
    edge_local = abs(EDGE_READ[0] - POSTER_POS[0]) / POSTER_HALF[0]
    assert edge_local < 1.0 - 2.0 * MARGIN, (
        f"the edge read sits at {edge_local:.3f} of the box, inside the "
        f"image's {MARGIN} transparent margin -- it would read substrate")

    # The scorch must actually differ from the substrate in the channel the
    # surface arm reads, or that arm passes on a build that ignores the map.
    assert SCORCH_ROUGH > 200, "the scorch's roughness is not far from a plain floor's"


# The oblique plate's centre and its rotation. Tilted about X, so its normal
# leans away from the poster's +Z facing by OBLIQUE_DEG.
OBLIQUE_CENTER = (POSTER_POS[0] + OBLIQUE_DX, POSTER_POS[1], POSTER_POS[2] + 0.25)
_ob = math.radians(OBLIQUE_DEG)
_ob_n = (0.0, math.sin(_ob), math.cos(_ob))
_ob_u = (OBLIQUE_HALF, 0.0, 0.0)
_ob_v = (0.0, OBLIQUE_HALF * math.cos(_ob), -OBLIQUE_HALF * math.sin(_ob))


def _plate(center, u, v):
    """Four corners of a quad spanning center +/- u +/- v."""
    return [
        (center[0] - u[0] - v[0], center[1] - u[1] - v[1], center[2] - u[2] - v[2]),
        (center[0] + u[0] - v[0], center[1] + u[1] - v[1], center[2] + u[2] - v[2]),
        (center[0] + u[0] + v[0], center[1] + u[1] + v[1], center[2] + u[2] + v[2]),
        (center[0] - u[0] + v[0], center[1] - u[1] + v[1], center[2] - u[2] + v[2]),
    ]


floor_pos = [(-HALF, 0.0, HALF), (HALF, 0.0, HALF), (HALF, 0.0, WALL_Z), (-HALF, 0.0, WALL_Z)]
floor_nrm = [(0.0, 1.0, 0.0)] * 4

wall_pos = [(-HALF, 0.0, WALL_Z), (HALF, 0.0, WALL_Z), (HALF, WALL_H, WALL_Z),
            (-HALF, WALL_H, WALL_Z)]
wall_nrm = [(0.0, 0.0, 1.0)] * 4

oblique_pos = _plate(OBLIQUE_CENTER, _ob_u, _ob_v)
oblique_nrm = [_ob_n] * 4

uv0 = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
indices = [0, 1, 2, 0, 2, 3]

_chunks = [
    (b"".join(struct.pack("<3f", *p) for p in floor_pos), 34962),
    (b"".join(struct.pack("<3f", *n) for n in floor_nrm), 34962),
    (b"".join(struct.pack("<3f", *p) for p in wall_pos), 34962),
    (b"".join(struct.pack("<3f", *n) for n in wall_nrm), 34962),
    (b"".join(struct.pack("<3f", *p) for p in oblique_pos), 34962),
    (b"".join(struct.pack("<3f", *n) for n in oblique_nrm), 34962),
    (b"".join(struct.pack("<2f", *t) for t in uv0), 34962),
    (b"".join(struct.pack("<H", i) for i in indices), 34963),
]
buffer_bytes = b"".join(c for c, _ in _chunks)


def _views(chunks):
    views, offset = [], 0
    for data, target in chunks:
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data),
                      "target": target})
        offset += len(data)
    return views


def _bounds(pts):
    return ([min(p[i] for p in pts) for i in range(3)],
            [max(p[i] for p in pts) for i in range(3)])


floor_mn, floor_mx = _bounds(floor_pos)
wall_mn, wall_mx = _bounds(wall_pos)
oblique_mn, oblique_mx = _bounds(oblique_pos)

GLTF = {
    "asset": {"version": "2.0", "generator": "gen_decal_fixture.py"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1, 2]}],
    "nodes": [{"name": "decal_floor", "mesh": 0},
              {"name": "decal_wall", "mesh": 1},
              {"name": "decal_oblique", "mesh": 2}],
    "meshes": [
        {"name": "decal_floor",
         "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 6},
                         "indices": 7, "material": 0}]},
        {"name": "decal_wall",
         "primitives": [{"attributes": {"POSITION": 2, "NORMAL": 3, "TEXCOORD_0": 6},
                         "indices": 7, "material": 1}]},
        {"name": "decal_oblique",
         "primitives": [{"attributes": {"POSITION": 4, "NORMAL": 5, "TEXCOORD_0": 6},
                         "indices": 7, "material": 2}]},
    ],
    # Three materials with DIFFERENT base colours, so every arm has an in-frame
    # control: a read that came from the wrong plate is a wrong code, not a
    # plausible one.
    "materials": [
        {"name": "decal_floor_mat",
         "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.55, 0.55, 1.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.25}},
        {"name": "decal_wall_mat",
         "pbrMetallicRoughness": {"baseColorFactor": [0.80, 0.78, 0.72, 1.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
        {"name": "decal_oblique_mat",
         "pbrMetallicRoughness": {"baseColorFactor": [0.20, 0.45, 0.75, 1.0],
                                  "metallicFactor": 0.0, "roughnessFactor": 0.9}},
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": floor_mn, "max": floor_mx},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": wall_mn, "max": wall_mx},
        {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": oblique_mn, "max": oblique_mx},
        {"bufferView": 5, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 6, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 7, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ],
    "bufferViews": _views(_chunks),
    "buffers": [{"uri": "data:application/octet-stream;base64," +
                        base64.b64encode(buffer_bytes).decode("ascii"),
                 "byteLength": len(buffer_bytes)}],
}

CSCN = {
    "version": 1,
    "models": [{"path": "decal_fixture.gltf"}],
    # Authored so a probe capture has an environment to be made of: a probe
    # cannot be created without a precomputed IBL, and a file whose probes are
    # refused still parses and still renders -- which is the trap cornell_rooms
    # and beach_fixture each fell into once.
    "environment": {"mode": "sky", "sun": {"elevation": 35.0, "azimuth": 200.0}},
    "decals": [
        {"position": list(POSTER_POS),
         "size": list(POSTER_HALF),
         "direction": [0.0, 0.0, -1.0],
         "image": "decal_poster.png",
         "opacity": 1.0,
         "angleFade": ANGLE_FADE,
         "feather": FEATHER},
        {"position": list(SCORCH_POS),
         "size": list(SCORCH_HALF),
         "direction": [0.0, -1.0, 0.0],
         "image": "decal_scorch.png",
         "surface": "decal_scorch_surface.png",
         "opacity": 1.0,
         "angleFade": ANGLE_FADE,
         "feather": FEATHER},
    ],
    "lights": [{"name": "DecalSun", "type": "directional",
                "direction": [-0.3, -0.7, -0.65], "color": [1.0, 1.0, 1.0],
                "intensity": 3.0, "cast_shadows": False}],
    # Both plates and the oblique in one frame, square on to the wall so the
    # poster is not foreshortened.
    "camera": {"eye": [0.0, 2.2, 5.5], "target": [0.0, 1.5, -1.0], "fov": 45},
    "post": {"tonemap": "neutral", "exposure": 1.0, "auto_exposure": False},
}


def main():
    _assert_fixture_still_tests_something()
    Image.fromarray(_poster(), "RGBA").save(os.path.join(HERE, "decal_poster.png"))
    Image.fromarray(_scorch_albedo(), "RGBA").save(os.path.join(HERE, "decal_scorch.png"))
    Image.fromarray(_scorch_surface(), "RGBA").save(
        os.path.join(HERE, "decal_scorch_surface.png"))
    with open(os.path.join(HERE, "decal_fixture.gltf"), "w") as f:
        json.dump(GLTF, f, indent=1)
        f.write("\n")
    with open(os.path.join(HERE, "decal_fixture.cscn"), "w") as f:
        json.dump(CSCN, f, indent=1)
        f.write("\n")
    print("wrote decal_fixture.gltf, decal_fixture.cscn and three painted images "
          f"at {TEX}x{TEX}")


if __name__ == "__main__":
    main()
