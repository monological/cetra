"""The dither group: output dither (8-bit contour bands, spec 11.24 / E1)."""

import os

from gatelib.harness import asset, render
from gatelib.images import (
    DITHER_MAX_RUN_FRAC, DITHER_MIN_BAND_FRAC, DITHER_MIN_SPAN, _flat_run_and_span, _read_ppm,
)


DITHER_MEAN_TOL = 0.05       # LSB the frame mean may move when dither turns on


def run_dither_gate(workdir):
    fixture = asset("aerial_fixture.gltf")
    if not os.path.exists(fixture):
        print("  dither       SKIP  (missing aerial_fixture.gltf)")
        return []

    def measure(tag, extra):
        out = os.path.join(workdir, f"dither_{tag}.ppm")
        if render(fixture, out, ["--no-auto-exposure", "-E", "1.0"] + extra):
            return None
        w, h, pix = _read_ppm(out)
        return _flat_run_and_span(pix, w, h) + (h,)

    off = measure("off", ["--no-dither"])
    on = measure("on", [])
    strong = measure("strong", ["--dither", "2.0"])
    if off is None or on is None or strong is None:
        print("  dither       ERROR while rendering the gradient fixture")
        return ["dither"]
    (off_run, off_span, h), (on_run, _, _), (strong_run, _, _) = off, on, strong
    min_band, max_run = DITHER_MIN_BAND_FRAC * h, DITHER_MAX_RUN_FRAC * h

    failures = []
    # The inverse arm, and it takes TWO statistics. A run-length floor alone is
    # maximized by a constant image -- exactly what a fixture that lost its sky
    # produces -- so it would pass at its best possible score while measuring
    # nothing. The span is the half that a constant image fails. (Spec 11.22:
    # six MSM arms passed silently because the frame they measured *was* the
    # feature-off frame.)
    ok = off_span >= DITHER_MIN_SPAN and off_run >= min_band
    print(f"  band-inverse {'PASS' if ok else 'FAIL'}  undithered run {off_run} px "
          f"(needs >= {min_band:.0f}) over a {off_span}-level span "
          f"(needs >= {DITHER_MIN_SPAN}: a flat frame is not a gradient)")
    if not ok:
        failures.append("dither-band-inverse")

    ok = on_run <= max_run and on_run < off_run
    print(f"  band-run     {'PASS' if ok else 'FAIL'}  {on_run} px dithered run "
          f"(bound {max_run:.0f}, undithered {off_run})")
    if not ok:
        failures.append("dither-band-run")

    # Strength must do something. "Off is off" would NOT cover this: the C side
    # short-circuits, so both arms take one path and the check passes against a
    # dead feature (spec 11.21 shipped exactly that arm). Comparing two live
    # amplitudes fails if the shader ignores the uniform.
    ok = strong_run < on_run
    print(f"  band-linear  {'PASS' if ok else 'FAIL'}  {strong_run} px at 2.0 LSB "
          f"(must beat {on_run} px at the 1.0 default)")
    if not ok:
        failures.append("dither-band-linearity")

    # Dither must not move the picture, only its quantization error. The arm
    # that matters is at the CLIP POINTS: a value already at 0 or 1 has no error
    # to decorrelate, so a dither that is added and then clamped keeps one half
    # of its distribution and shifts the mean -- flat white stipples to 254,
    # flat black lifts off zero. That is a DC bias, and none of the run-length
    # arms above can see it. Measured on a deliberately blown frame (ACES holds
    # its clip rather than rolling off, so ~63% of channels sit at 255).
    def clip_mean(tag, extra):
        out = os.path.join(workdir, f"dither_{tag}.ppm")
        if render(fixture, out, ["--no-auto-exposure", "-E", "500",
                                 "--tonemap", "aces"] + extra):
            return None
        _, _, pix = _read_ppm(out)
        return sum(pix) / len(pix)

    m_off, m_on = clip_mean("clipoff", ["--no-dither"]), clip_mean("clipon", [])
    if m_off is None or m_on is None:
        print("  band-mean    ERROR while rendering the clipped frame")
        return failures + ["dither-band-mean"]
    ok = abs(m_on - m_off) <= DITHER_MEAN_TOL
    print(f"  band-mean    {'PASS' if ok else 'FAIL'}  clipped-frame mean moves "
          f"{abs(m_on - m_off):.4f} LSB (bound {DITHER_MEAN_TOL})")
    if not ok:
        failures.append("dither-band-mean")

    return failures
