"""The sun, moon and star helpers the sky groups share."""

import math
import os

from gatelib.harness import render
from gatelib.images import _SRGB_TO_LINEAR, _read_ppm, compare
from gatelib.scenes import _projector


# Same accumulator the fog-volume fixture needs 60 frames for; these arms drive it too.
CLOUDSHADOW_FRAMES = 60


# --- stars (spec 11.79): the procedural night star field --------------------------------
#
# All on aerial_fixture, the suite's sky fixture -- NOT tree, which is documented at 31k
# px run-to-run. Exposure pinned on every arm. Every quantitative read is the star
# CONTRIBUTION -- the delta against a --no-stars twin at the same configuration -- never
# raw brightness: the twilight sky's own radiance dominates a raw read and moves the
# OPPOSITE way as the sun drops (the beach-shoreline lesson, where an argmax read dry
# sand for four specs).
STARS_FIXTURE = "aerial_fixture.gltf"
STARS_PIN = ["--no-auto-exposure", "-E", "1.0"]
# The upper sky. The fixture's terrain silhouette cuts the sky off around 0.55 of frame
# height at this camera; box fractions are vertical-fov fractions, so they hold at any
# width the suite renders.
STARS_BAND = (0.05, 0.04, 0.95, 0.42)
# The emerge ladder. +5 is above the ramp's top edge (+3), so its contribution is an
# exact zero and the ladder's first step doubles as the ramp-zeroing read. Four other
# arms hang off the last rung's pair, so the night elevation is DEFINED as that rung
# rather than restated beside it.
STARS_EMERGE_ELEVS = ["5", "0", "-6", "-12"]
STARS_NIGHT_ELEV = STARS_EMERGE_ELEVS[-1]
STARS_DAY_ELEV = "35"


def _stars_delta(on_path, off_path, box):
    """Mean per-channel star contribution over a fractional box, plus moved pixels.

    Linear-decoded on-minus-off, so the sky under the stars and the deterministic
    dither pattern cancel exactly and what remains is what the stars added.
    """
    w, h, on = _read_ppm(on_path)
    _, _, off = _read_ppm(off_path)
    x0, x1 = int(box[0] * w), int(box[2] * w)
    y0, y1 = int(box[1] * h), int(box[3] * h)
    n = 0
    moved = 0
    acc = [0.0, 0.0, 0.0]
    for y in range(y0, y1):
        row = y * w
        for x in range(x0, x1):
            o = (row + x) * 3
            hit = False
            for k in range(3):
                acc[k] += _SRGB_TO_LINEAR[on[o + k]] - _SRGB_TO_LINEAR[off[o + k]]
                hit = hit or on[o + k] != off[o + k]
            if hit:
                moved += 1
            n += 1
    return [c / max(n, 1) for c in acc], moved


# --- night-floor (spec 11.80): the sky between the stars, lighting the world ------------
#
# Same fixture, same pinning and the same twin-delta doctrine as the stars group. The
# ground band sits over the fixture's terrain silhouette, below the stars' sky band: what
# lifts there at night can only have arrived through the env cube and the IBL, which is
# the path this feature exists for and the one no stars arm touches.
NIGHTFLOOR_GROUND_BAND = (0.05, 0.62, 0.95, 0.90)


# --- cycle (spec 11.81): the day/night clock and its time-sliced env re-bake ------------
#
# Same fixture and pinning as the stars and floor groups. The latitude is the sky's own
# default, which is what the twin below reproduces.
CYCLE_LATITUDE = 45.0
CYCLE_QUIESCE_HOUR = 7.25


def _sliced_vs_atomic(workdir, tag, path, flags):
    """One sliced env re-bake (--cycle-rebake-at) over an unmoved sun against the same run
    without it: (differing px, None), or (None, the render error).

    Sixty frames so the schedule has converged and swapped. Written once because the
    schedule's parameters belong to the slicer and not to any one arm that exercises it.
    """
    sliced = os.path.join(workdir, f"{tag}_sliced.ppm")
    atomic = os.path.join(workdir, f"{tag}_atomic.ppm")
    err = render(path, sliced, flags + ["--cycle-rebake-at", "2"], frames=60) or \
        render(path, atomic, flags, frames=60)
    if err:
        return None, err
    px, _ = compare(sliced, atomic)
    return px, None


def _cycle_sun_path(latitude_deg, hour):
    """The Python twin of sky_sun_path (sky.c), in doubles.

    Equinox path: the sun rides the celestial equator of `latitude_deg`, so noon altitude
    is 90-lat and sunrise/sunset land at 6 and 18. Written out rather than imported
    because there is nothing to import -- and that is the point: the C is the thing under
    test, and a twin that shared its arithmetic would asserts nothing.

    DOUBLES throughout, matching the C, and printed at repr precision by the caller: the
    quiesce arm demands the path-derived sun and the flag-passed sun land on the same
    float, which a float-domain twin would not deliver.
    """
    h = (hour - 12.0) * (math.pi / 12.0)
    lat = latitude_deg * (math.pi / 180.0)
    x = -math.sin(h)
    y = math.cos(lat) * math.cos(h)
    z = -math.sin(lat) * math.cos(h)
    el = math.asin(max(-1.0, min(1.0, y)))
    az = math.atan2(x, z)
    if az < 0.0:
        az += 2.0 * math.pi
    return el * 180.0 / math.pi, az * 180.0 / math.pi


def _moon_dir(el_deg, az_deg):
    """The twin of sky_update_moon: elevation/azimuth -> unit vector, azimuth 0 = +Z."""
    el, az = math.radians(el_deg), math.radians(az_deg)
    return (math.cos(el) * math.sin(az), math.sin(el), math.cos(el) * math.cos(az))


def _moon_disc_geometry(cam, disc_deg, m_el, m_az, w, h):
    """(radius_px, cx, cy) for the predicted disc -- PURE PROJECTION, no pixels read.

    Split from the scan below because it is a closed form of the camera and the two
    angles, so an arm that wants only the geometry should not pay two PPM decodes and a
    Python walk to recover it. The radius comes from the angular size through the same
    vertical fov the projector uses, so an arm asserts the render against geometry rather
    than against itself.
    """
    project = _projector(cam, w, h)
    eye = cam["eye"]
    d = _moon_dir(m_el, m_az)
    cx, cy = project(tuple(e + 20000.0 * c for e, c in zip(eye, d)))
    radius = (h * 0.5) * math.tan(math.radians(disc_deg * 0.5)) / \
        math.tan(math.radians(cam["fovy_deg"]) * 0.5)
    return radius, cx, cy


def _moon_face_pixels(cx, cy, radius, w, h, frac=1.0):
    """Yield (x, y, offset) over the disc at `frac` of its radius.

    One statement of the mask geometry, which three arms were walking with their own
    copies at 1.0, 0.9 and 0.85 -- differences that were silent because each was written
    out separately.
    """
    r = radius * frac
    for y in range(max(0, int(cy - r)), min(h, int(cy + r) + 1)):
        row = y * w
        for x in range(max(0, int(cx - r)), min(w, int(cx + r) + 1)):
            if math.hypot(x - cx, y - cy) <= r:
                yield x, y, (row + x) * 3
