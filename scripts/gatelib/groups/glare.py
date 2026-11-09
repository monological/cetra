"""The glare group: the aperture's diffraction glare (spec 13.4)."""

import os

from gatelib.harness import RENDER, _probe_rows, _run, asset, render
from gatelib.images import compare
from gatelib.scenes import WATER_FIXTURE, cscn_copy


# The glare gate (spec 13.4). The water fixture looking into its sun, where the glitter carries
# thousands of pixels past the threshold, and the AO fixture, where nothing reaches it.
# --glare on both: it is off by default (spec 13.5), and these arms are about it.
GLARE_BRIGHT_FLAGS = ["--glare", "--water-waves", "fft", "--cam-eye", "-3,1.2,3", "--cam-target",
                      "4,0,-4", "--no-auto-exposure", "-E", "1.0"]
GLARE_DARK_FLAGS = ["--glare", "--no-auto-exposure", "-E", "1.0"]
# The glare's light over its source's, per channel. The pattern is normalised, so it can only
# lose what spreads past the frame's edge: 0.999 when measured.
GLARE_ENERGY_RANGE = (0.9, 1.02)
# The frame's light the star puts back over what the tonemap takes out, per channel, per pixel
# (spec 13.5): 1.010 when measured. Not exactly 1 because the grid's texels cover unequal
# blocks of the frame, so the star's source is a mean of block means; a source thresholded
# after its downsample instead of before measured 0.58.
GLARE_CONSERVE_RANGE = (0.97, 1.03)
GLARE_LIVE_MIN_PX = 20000


def _glare_probe(extra, scene):
    """Run --glare-probe and return the last frame's fields as RGB lists by name, or None."""
    cmd = [RENDER, "-m", scene, "-x", "-f", "10", "-W", "400", "-H", "300", "--glare-probe"] + extra
    r = _run(cmd, capture_output=True, text=True)
    rows = _probe_rows(r.stdout + r.stderr, "glare-probe")
    if not rows:
        return None
    return {k: [float(v) for v in rows[-1][k].split(",")]
            for k in ("source", "glare", "removed", "added")}


def run_glare_gate(workdir):
    """The aperture's diffraction star (spec 13.4): it conserves light, it is there, and it is
    nowhere a frame has nothing bright enough to star.

      glare-energy  the glare's light over its thresholded source's, per channel, within
                    GLARE_ENERGY_RANGE: the pattern is normalised, so the only loss is what spills
                    past the frame's edge, and a transform that scaled, lost or doubled light --
                    a missing normalisation, a kernel built for another grid -- lands outside it.
      glare-conserves  the FRAME's light: what the star puts back over what the tonemap takes
                    out of the pixels it came from, per channel, within GLARE_CONSERVE_RANGE
                    (spec 13.5). glare-energy compares the star with its own source and cannot
                    see a glare that adds its light on top of the frame instead of moving it.
      glare-live    against --no-glare the sunward frame moves at least GLARE_LIVE_MIN_PX.
      glare-dark    a frame with nothing past the threshold is identical with and without it:
                    the transform's rounding must not reach an 8-bit code, which is what keeps
                    every golden without a highlight unmoved.
      glare-scene   the glare is off by default and a scene file turns it on (spec 13.5): the
                    sunward frame from a copy of the fixture carrying post.glare.enabled is the
                    frame --glare renders, and not the frame without it.
    """
    failures = []
    water = asset(WATER_FIXTURE)
    probed = _glare_probe(GLARE_BRIGHT_FLAGS, water)
    if not probed:
        print("  glare-energy FAIL  the probe printed nothing")
        failures.append("glare-energy")
    else:
        ratios = [g / max(s, 1e-9) for s, g in zip(probed["source"], probed["glare"])]
        lo, hi = GLARE_ENERGY_RANGE
        ok = all(lo <= r <= hi for r in ratios) and min(probed["source"]) > 0.0
        print(f"  glare-energy {'PASS' if ok else 'FAIL'}  glare over source "
              f"{'/'.join(f'{r:.4f}' for r in ratios)} (want {lo}..{hi})")
        if not ok:
            failures.append("glare-energy")
        ratios = [a / max(r, 1e-9) for r, a in zip(probed["removed"], probed["added"])]
        lo, hi = GLARE_CONSERVE_RANGE
        ok = all(lo <= r <= hi for r in ratios) and min(probed["removed"]) > 0.0
        print(f"  glare-conserves {'PASS' if ok else 'FAIL'}  put back over taken out "
              f"{'/'.join(f'{r:.4f}' for r in ratios)} (want {lo}..{hi})")
        if not ok:
            failures.append("glare-conserves")

    on = os.path.join(workdir, "glare_on.ppm")
    off = os.path.join(workdir, "glare_off.ppm")
    dark_on = os.path.join(workdir, "glare_dark_on.ppm")
    dark_off = os.path.join(workdir, "glare_dark_off.ppm")
    dark = asset("ao_fixture.cscn")
    err = (render(water, on, GLARE_BRIGHT_FLAGS) or
           render(water, off, GLARE_BRIGHT_FLAGS + ["--no-glare"]) or
           render(dark, dark_on, GLARE_DARK_FLAGS) or
           render(dark, dark_off, GLARE_DARK_FLAGS + ["--no-glare"]))
    if err:
        print(f"  glare-live   ERROR render failed: {err.strip()[-200:]}")
        return failures + ["glare-live", "glare-dark"]
    moved, _ = compare(on, off)
    ok = moved >= GLARE_LIVE_MIN_PX
    print(f"  glare-live   {'PASS' if ok else 'FAIL'}  {moved} px against --no-glare "
          f"(want >={GLARE_LIVE_MIN_PX})")
    if not ok:
        failures.append("glare-live")
    still, _ = compare(dark_on, dark_off)
    ok = still == 0
    print(f"  glare-dark   {'PASS' if ok else 'FAIL'}  {still} px on a frame with nothing to "
          f"star (want 0)")
    if not ok:
        failures.append("glare-dark")

    scene = os.path.join(workdir, "glare_scene.cscn")
    cscn_copy(water, scene,
              lambda d: d.setdefault("post", {}).__setitem__("glare", {"enabled": True}))
    authored = os.path.join(workdir, "glare_scene.ppm")
    plain = os.path.join(workdir, "glare_default.ppm")
    unflagged = [f for f in GLARE_BRIGHT_FLAGS if f != "--glare"]
    err = render(scene, authored, unflagged) or render(water, plain, unflagged)
    if err:
        print(f"  glare-scene  ERROR render failed: {err.strip()[-200:]}")
        return failures + ["glare-scene"]
    as_flag, _ = compare(authored, on)
    as_default, _ = compare(authored, plain)
    default_off, _ = compare(plain, off)
    ok = as_flag == 0 and as_default >= GLARE_LIVE_MIN_PX and default_off == 0
    print(f"  glare-scene  {'PASS' if ok else 'FAIL'}  scene-authored against --glare {as_flag} px "
          f"(want 0), against the default {as_default} px (want >={GLARE_LIVE_MIN_PX}), default "
          f"against --no-glare {default_off} px (want 0)")
    if not ok:
        failures.append("glare-scene")
    return failures
