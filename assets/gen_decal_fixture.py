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
# There is NO border ring. One existed for the decals-edge arm, which was
# written, measured at 0 px and removed -- and the ring outlived it as pure
# contamination: it sat about a texel from where the feather arm reads, and
# bilinear pulled its white in, which reads as a coverage that disagrees between
# channels. Scaffolding for a deleted arm is not free.
SCORCH_CODE = (40, 40, 44)

# The scorch's surface map, packed as layers.glsl reads it.
SCORCH_ROUGH = 250  # against the substrate's own, which is far lower
SCORCH_AO = 120
# normal.x, encoded. 208 is +0.63 in tangent space -- a 39-degree tilt along the
# decal's own u axis, which is steep enough that the lit frame moves a long way
# if the shader picks the wrong basis vector, negates one, or drops the
# composition entirely.
SCORCH_NORMAL_X = 208

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

# The framing, named because two read points are DERIVED from it -- a mirror
# reflection cannot be stated as a screen position, only computed from where the
# eye is.
CAM_EYE = (0.0, 2.2, 5.5)
CAM_TARGET = (0.0, 1.5, -1.0)
CAM_FOV = 45

ANGLE_FADE = 60.0  # degrees; a surface tilted further takes no mark
# The edge ramp, in the box's own units, and it is 0.35 rather than something
# subtle FOR A REASON THE ASSERTS BELOW ENFORCE: the ramp covers |local| in
# [1 - FEATHER, 1], while the image's own alpha has already gone to zero past
# 1 - 2*MARGIN. At 0.12 those two bands were DISJOINT -- the picture ended at
# 0.75 and the ramp did not start until 0.88 -- so deleting the feather from the
# shader entirely moved 0 px, measured. That is the same "the transparent margin
# retires the alpha before the guard begins" mechanism that made the half-texel
# inset unobservable, found a second time and fixed here rather than admitted.
FEATHER = 0.35

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
def _on_wall(lx, ly):
    """A poster read point, at the box's local (lx, ly) but ON THE WALL PLANE.

    The z matters and it is not the box's. A read at the projector's own centre
    hangs a quarter of a unit in front of the surface, and projecting it to
    screen lands NEAR the right pixel rather than on it -- fine at the middle of
    a flat quadrant, wrong at an edge, where a fraction of a unit of parallax is
    the difference between inside the ramp and past it. That cost one arm a
    debugging round.
    """
    return (POSTER_POS[0] + lx * POSTER_HALF[0],
            POSTER_POS[1] + ly * POSTER_HALF[1], WALL_Z)


# Inside the poster's UPPER-RIGHT quadrant, so it returns POSTER_TR rather than
# whatever a seam rounds to -- it sat on the horizontal seam until the poster
# gained quadrants, which is exactly the read a one-colour image cannot notice.
POSTER_READ = _on_wall(0.55, 0.5)
OBLIQUE_READ = (POSTER_POS[0] + OBLIQUE_DX, POSTER_POS[1], POSTER_POS[2] + 0.25)
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

# Inside the poster's edge RAMP and inside its opaque region at once, which is
# the whole trick -- see FEATHER. Local (0.70, -0.30), so it sits in the
# lower-right quadrant and its coverage is decided by x alone.
FEATHER_LOCAL = (0.70, -0.30)
FEATHER_READ = _on_wall(*FEATHER_LOCAL)
# What the shader owes there: min over the three axes of (1 - |local|)/feather,
# clamped. Stated here so the arm asserts the fixture's own arithmetic rather
# than a number somebody read off a frame once. The z term is the wall's own
# depth in the box, which is why the read has to BE on the wall.
_WALL_LOCAL_Z = abs((WALL_Z - POSTER_POS[2]) / POSTER_HALF[2])
FEATHER_COVERAGE = min(1.0,
                       (1.0 - abs(FEATHER_LOCAL[0])) / FEATHER,
                       (1.0 - abs(FEATHER_LOCAL[1])) / FEATHER,
                       (1.0 - _WALL_LOCAL_Z) / FEATHER)
WALL_CLEAR = (1.6, 1.6, WALL_Z)
FLOOR_CLEAR = (-1.8, 0.0, 1.2)

def _mirror_read():
    """The floor point where a mirror shows the poster's centre.

    Derived rather than eyeballed: reflect the poster through the floor plane,
    aim the camera at the reflection, and take where that ray crosses y = 0.
    An arm asserting a decal reached a probe CAPTURE has to read a pixel that
    only the capture can explain, and 'somewhere on the floor' is not that.
    """
    ex, ey, ez = CAM_EYE
    px, py, pz = POSTER_POS[0], POSTER_POS[1], WALL_Z
    t = ey / (ey + py)  # crossing y = 0 on the way to the mirrored poster
    return (ex + t * (px - ex), 0.0, ez + t * (pz - ez))


MIRROR_READ = _mirror_read()

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
    return img


def _scorch_albedo():
    img = np.zeros((TEX, TEX, 4), dtype=np.uint8)
    m = int(TEX * MARGIN)
    img[m:TEX - m, m:TEX - m, :3] = SCORCH_CODE
    img[m:TEX - m, m:TEX - m, 3] = 255
    return img


def _scorch_surface():
    """Packed normal.xy + roughness + AO, the layer surface format.

    The normal has REAL RELIEF, and it did not until the review: both channels
    were a flat 128, which encodes 0.0039 rather than 0 and tilts the world
    normal by about a third of a degree -- on a surface the map also drives to
    roughness 0.98, and toward a direction that IS the floor's own normal. So
    the entire tangent-frame construction in the shader was unobservable:
    swapping the two basis vectors was a literal no-op, and deleting the normal
    composition left the roughness delta alone and every arm green.

    A single-axis TILT rather than a pattern, and the axis is the decal's own u:
    that is what makes a swapped or negated basis vector move the lit frame, and
    it is asserted below to be a real tilt rather than a rounding of zero.
    """
    img = np.zeros((TEX, TEX, 4), dtype=np.uint8)
    img[:, :, 0] = SCORCH_NORMAL_X
    img[:, :, 1] = 128  # v is flat, so a basis error shows as a tilt, not a wash
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

    # The transparent margin has to be real, or a read at the box's own edge
    # meets the box clipping the image rather than the image's own alpha.
    poster = _poster()
    assert poster[0, 0, 3] == 0 and poster[TEX // 2, TEX // 2, 3] == 255, (
        "the poster is not transparent at its margin and opaque at its centre")

    # The four quadrants must be four DIFFERENT colours, and differ in a way no
    # symmetry can undo -- this is the assert that makes the orientation arm
    # possible. An image invariant under mirror, flip, transpose or rotation
    # cannot fail an orientation test, which is what every earlier version of
    # this poster was.
    m = int(TEX * MARGIN)
    q = {"TL": POSTER_TL, "TR": POSTER_TR, "BL": POSTER_BL, "BR": POSTER_BR}
    assert len(set(q.values())) == 4, "the poster's quadrants are not four distinct colours"
    for a in q:
        for b in q:
            if a < b:
                assert all(abs(x - y) > 16 for x, y in zip(q[a], q[b])), (
                    f"{a} and {b} are within 16 codes in some channel; a "
                    f"misprojection would read as noise rather than as an answer")
    lo, hi = m, TEX - m - 1
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

    # The mirror read must be ON the floor and clear of the scorch, or the arm
    # reading it is looking at a mark rather than at a reflection of one.
    assert abs(MIRROR_READ[0]) < HALF and WALL_Z < MIRROR_READ[2] < HALF, (
        f"the mirror read {MIRROR_READ} is off the floor")
    assert not all(abs(MIRROR_READ[k] - SCORCH_POS[k]) < SCORCH_HALF[k] for k in range(3)), (
        "the mirror read sits inside the scorch's box")

    # ...and the scorch read must be INSIDE the scorch's box, on the floor.
    assert all(abs(SCORCH_READ[k] - SCORCH_POS[k]) < SCORCH_HALF[k] for k in range(3)), (
        "the scorch read point is outside the scorch's own box")

    # THE FEATHER MUST BE OBSERVABLE, which is two conditions and the fixture
    # satisfied neither until it was measured at 0 px. The ramp has to begin
    # inside the region where the image still has alpha...
    opaque_local = 1.0 - 2.0 * MARGIN
    assert 1.0 - FEATHER < opaque_local, (
        f"the edge ramp starts at |local| {1.0 - FEATHER:.3f}, past where the "
        f"image's own alpha ends at {opaque_local:.3f} -- deleting the feather "
        f"would move 0 px, which is what it did")

    # ...and the read point has to sit in the overlap, with a coverage that is
    # neither 0 nor 1 by a real margin.
    assert 1.0 - FEATHER < abs(FEATHER_LOCAL[0]) < opaque_local, (
        f"the feather read at |local.x| {abs(FEATHER_LOCAL[0])} is outside the band "
        f"({1.0 - FEATHER:.3f}, {opaque_local:.3f}) where the ramp is visible")
    assert 0.15 < FEATHER_COVERAGE < 0.95, (
        f"the feather read's coverage is {FEATHER_COVERAGE:.3f}; too near either "
        f"end and the arm cannot tell a ramp from a step")
    # x must be what DECIDES the coverage, or the arm is measuring another axis.
    for k, name in ((1, "y"), (2, "z")):
        other = abs(FEATHER_LOCAL[1]) if k == 1 else abs(
            (WALL_Z - POSTER_POS[2]) / POSTER_HALF[2])
        assert (1.0 - other) / FEATHER > FEATHER_COVERAGE + 0.1, (
            f"the {name} axis is as close to its face as x is; the feather read's "
            f"coverage would not be a function of x alone")

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

    # The feather read is on the far side from the plate, for the same reason.
    assert not (ob_lo <= FEATHER_READ[0] <= ob_hi), (
        "the oblique plate covers the feather read point")

    # Every read point must be inside the poster's box in x, or an arm is
    # asking about a region the decal never claimed.
    for pt, name in ((POSTER_READ, "poster"), (OBLIQUE_READ, "oblique"),
                     (FEATHER_READ, "feather")):
        assert abs(pt[0] - POSTER_POS[0]) < POSTER_HALF[0], (
            f"the {name} read point is outside the poster's box in x")

    # The scorch must actually differ from the substrate in the channel the
    # surface arm reads, or that arm passes on a build that ignores the map.
    assert SCORCH_ROUGH > 200, "the scorch's roughness is not far from a plain floor's"

    # ...and its normal must be a REAL tilt. 128 encodes 0.0039, not 0, so a map
    # that looks flat is a third of a degree off the surface it lies on -- which
    # is indistinguishable from no normal composition at all.
    tilt = abs(SCORCH_NORMAL_X / 255.0 * 2.0 - 1.0)
    assert tilt > 0.3, (
        f"the scorch's normal.x is {tilt:.3f} in tangent space; a build that "
        f"dropped the tangent frame entirely would look the same")


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
    "camera": {"eye": list(CAM_EYE), "target": list(CAM_TARGET), "fov": CAM_FOV},
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
