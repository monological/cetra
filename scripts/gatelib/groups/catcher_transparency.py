"""The catcher-transparency group: catcher vs transparency (panel through the plane)."""

import os

from gatelib.harness import RENDER, _run, asset
from gatelib.images import _linear_rgb, _read_ppm
from gatelib.scenes import _cscn_camera, _projector


# Shadow catcher vs transparency (spec 11.18), on assets/catcher_transparency_fixture.
#
# The catcher is a shadow decal at y=0 that also writes depth. Until 11.18 it
# drew last, so nothing was ever ordered against that depth and translucent
# geometry BEHIND the plane drew over the shadow instead of being hidden by the
# floor. Moving it ahead of the transparent pass fixes the OIT accumulate, the
# unsorted late pass and the particle depth resolve together, because all three
# read the same buffer.
#
# The fixture puts a vertical translucent panel through the plane, so one column
# of pixels crosses from in-front to behind. Measured against a second column
# beside it, which sees the same flat backdrop at both heights -- that is what
# the fixture's opaque wall is for, and it is why this needs no second render
# and no stored reference.
#
# The sample columns are this gate's own; the camera comes from the scene file.
CATCHER_TRANSPARENCY = _cscn_camera(
    "catcher_transparency_fixture.cscn",
    panel_x=1.2,   # inside the panel, which spans x 0.2 .. 2.2
    beside_x=3.0,  # clear of the panel, its cast shadow, and the caster's
    sample_y=0.7,  # sampled at +/- this, both well clear of the plane
)
# What "the floor hid it" is allowed to measure. Both samples read the same
# backdrop texel once the panel is occluded, so the honest bar is quantization:
# one 8-bit step is about 0.005 at this brightness, so this is two of them.
CATCHER_HIDDEN_TOL = 0.010
# The inverse half. Without it the gate passes on any frame where the panel
# failed to render, or where the camera stopped framing it. Measured 0.131.
CATCHER_VISIBLE_MIN = 0.05


def _catcher_samples(path):
    """((above on-panel, above beside), (below on-panel, below beside)) in linear RGB."""
    c = CATCHER_TRANSPARENCY
    w, h, pix = _read_ppm(path)
    project = _projector(c, w, h)

    def at(x, y):
        return _linear_rgb(pix, w, h, *project((x, y, 0.0)))

    return [(at(c["panel_x"], sy), at(c["beside_x"], sy))
            for sy in (c["sample_y"], -c["sample_y"])]


def _catcher_render(workdir, tag, extra):
    out = os.path.join(workdir, f"catchertr_{tag}.ppm")
    scene = asset("catcher_transparency_fixture.cscn")
    # --no-recenter is the fixture's whole premise: the app lifts a model's
    # bounding-box base onto y=0, and the geometry under test is the half of the
    # panel BELOW y=0. Recentred, there is nothing left to measure.
    # --no-dither beside --no-vignette: the hidden arm differences two SINGLE
    # pixels at different fragcoords, and an independent LSB on each can reach
    # CATCHER_HIDDEN_TOL on its own, before any ordering error contributes.
    cmd = [RENDER, "-m", scene, "--no-recenter", "-x", "-f", "30", "-W", "800", "-H", "500",
           "--no-vignette", "--no-dither", "-S", out] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    return out


def run_catcher_transparency_gate(workdir):
    fixture = asset("catcher_transparency_fixture.cscn")
    if not os.path.exists(fixture):
        print("  catcher-tr   SKIP  (missing catcher_transparency_fixture.cscn)")
        return []
    fails = []
    # Both transparency paths, because the fix is one ordering change that
    # serves both and a regression could easily reach only one of them.
    for tag, label, extra in (("oit", "moment OIT", []),
                              ("late", "unsorted late pass", ["--no-oit"])):
        frame = _catcher_render(workdir, tag, extra)
        if frame is None:
            print(f"  catcher-tr   ERROR while rendering the {label} frame")
            fails.append("catcher-transparency")
            continue
        (above_on, above_off), (below_on, below_off) = _catcher_samples(frame)
        visible = max(abs(a - b) for a, b in zip(above_on, above_off))
        hidden = max(abs(a - b) for a, b in zip(below_on, below_off))
        ok_v = visible >= CATCHER_VISIBLE_MIN
        ok_h = hidden <= CATCHER_HIDDEN_TOL
        print(f"  catcher-tr   {'PASS' if ok_v and ok_h else 'FAIL'}  {label}: "
              f"above the plane {visible:.4f} (want >= {CATCHER_VISIBLE_MIN}, the panel is there), "
              f"below it {hidden:.4f} (want <= {CATCHER_HIDDEN_TOL}, the floor hides it)")
        if not (ok_v and ok_h):
            fails.append("catcher-transparency")
    return fails
