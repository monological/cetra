"""The crt group: the CRT (lines, mask and tone, tube, dither last; spec 13.28)."""

import os

import numpy as np

from gatelib.harness import asset, render
from gatelib.images import (
    DITHER_MAX_RUN_FRAC, DITHER_MIN_BAND_FRAC, DITHER_MIN_SPAN, _flat_run_and_span, _ppm_array,
    _read_ppm, compare,
)
from gatelib.probes import _config_variant


# ---- The CRT (spec 13.28) ----------------------------------------------------
#
# On the dither gate's sky: a gradient smooth enough to bank on, covering every
# level from shadow to above mid-grey, and flat across the frame row by row. The
# exposure is pinned and the vignette off so the plain frame is the picture the
# CRT is fed; the dither is off except in the arm about it, since one LSB of noise
# is the size of what crt-mean reads.
CRT_FIXTURE = "aerial_fixture.gltf"
CRT_PIN = ["--no-auto-exposure", "-E", "1.0", "--no-vignette"]
# The CRT's period average against the closed form, over flat blocks. Measured
# 0.83% at worst; the model is exact for a flat field under fused lines, so what
# is left is the 8-bit rounding and a little light from the next block over.
CRT_MEAN_TOL = 0.02
CRT_MEAN_DARK = 0.7        # crt_mask 0.3, the default, as the mask's dark exposure
CRT_MEAN_MIN_BLOCKS = 1000
CRT_LINE_PERIOD_TOL = 0.05  # px
CRT_LINE_DEPTH_MIN = 0.5    # thin lines: the period's amplitude over the mean (0.86)
CRT_LINE_FUSED_MAX = 0.02   # fused lines: the same (0.008)
CRT_CENTRE_TOL = 0.01


def _crt_tone(thin, dark):
    """crt.c's _crt_tone, from CRTS's CrtsTone at contrast 1, restated."""
    m = (1.0 + 2.0 * dark) / 3.0 * (1.0 + dark) / 2.0
    mid_out = 0.18 / ((1.5 - thin) * m)
    return ((mid_out - 0.18) / ((1.0 - 0.18) * mid_out),
            (0.18 - 0.18 * mid_out) / ((1.0 - 0.18) * mid_out))


def _crt_slot_average(colour, dark):
    """The CRT's output averaged over one period of the slot mask, for a flat linear
    colour (..., 3), with the lines fused.

    Fused lines sum to one everywhere and the horizontal filter is normalised, so a
    flat field reaches the mask unchanged. The mask is three stripes, each passing
    its own channel whole and the other two at `dark`, with alternate rows at `dark`
    as a whole; CRTS's tone curve then acts on each pixel's PEAK channel. So six
    pixels make the period, and each is the curve at its peak times its colour over
    that peak.
    """
    tx, ty = _crt_tone(0.5, dark)
    total = np.zeros(colour.shape[:-1])
    for k in range(3):
        stripe = np.full(3, dark)
        stripe[k] = 1.0
        lit = colour * stripe
        peak = np.maximum(lit.max(axis=-1), 1e-9)
        for row in (1.0, dark):
            v = peak * row
            total += (v / (v * tx + ty) / peak * lit.mean(axis=-1))
    return total / 6.0


def _blocks(img, by, bx):
    """Mean of each by x bx block of an (h, w, c) array."""
    h, w = img.shape[0] // by * by, img.shape[1] // bx * bx
    return img[:h, :w].reshape(h // by, by, w // bx, bx, -1).mean(axis=(1, 3))


def run_crt_gate(workdir):
    """The CRT, a consumer television over the finished picture (spec 13.28).

    Each arm reads a property of the picture against the plain frame of the same run:
      crt-off          every setting moved and the switch off draws exactly the frame
                       with every setting at its default, and the switch on does not
      crt-lines        thin scanlines put a period of the window's height over the
                       signal's lines into the frame, and fused lines put none
      crt-mean         over flat ground the period average is the slot mask times CRTS's
                       tone curve, predicted block by block from the plain frame
      crt-curve        a curved tube blacks out the corners and leaves the centre where
                       a flat one has it
      crt-dither-last  with the CRT on, the window's write is dithered: the sky's bands
                       break with the dither on and stand without it

    crt-mean is the arm about light. The tone curve is built to hold mid-grey, and it
    compresses whatever is brighter -- CRTS says it will not get back the peak -- so a
    frame's mean is NOT preserved and is not what is asserted; the curve is.
    """
    fixture = asset(CRT_FIXTURE)
    if not os.path.exists(fixture):
        print(f"  crt-off SKIP  ({CRT_FIXTURE} not found)")
        return []

    def shot(tag, extra, dither=False):
        out = os.path.join(workdir, f"crt_{tag}.ppm")
        err = render(fixture, out, CRT_PIN + ([] if dither else ["--no-dither"]) + extra)
        return out if not err else None

    failures = []
    # --- crt-off ------------------------------------------------------------------
    # Through the snapshot, since every CRT flag implies --crt. Both legs restore,
    # so they differ in the CRT's settings and nothing else.
    dump = os.path.join(workdir, "crt_plain.json")
    plain = shot("plain", ["--config-dump", dump])
    if plain is None or not os.path.exists(dump):
        print("  crt-off ERROR while rendering the plain frame")
        return ["crt"]

    def moved(enabled):
        def mutate(d):
            d["postfx"]["crt"] = {"enabled": enabled, "lines": 200.0, "scanlines": 1.0,
                                  "mask": 1.0, "curvature": 1.0, "bleed": 1.0}
        return mutate

    restored = shot("restored", ["--config", dump])
    off = shot("moved_off", ["--config", _config_variant(dump, os.path.join(
        workdir, "crt_moved_off.json"), moved(False))])
    on = shot("moved_on", ["--config", _config_variant(dump, os.path.join(
        workdir, "crt_moved_on.json"), moved(True))])
    if None in (restored, off, on):
        print("  crt-off ERROR while restoring")
        return ["crt"]
    px_off, _ = compare(restored, off)
    px_on, _ = compare(restored, on)
    ok = px_off == 0 and px_on > 0
    print(f"  crt-off {'PASS' if ok else 'FAIL'}  {px_off} px with every setting moved and the "
          f"switch off (want 0); {px_on} px with it on (want > 0)")
    if not ok:
        failures.append("crt-off")

    base = _ppm_array(plain)
    h, w = base.shape[:2]

    # --- crt-lines ----------------------------------------------------------------
    # Four window pixels a line, past the fade that fuses lines too fine for the grid.
    lines = h // 4
    still = ["--crt-lines", str(lines), "--crt-mask", "0", "--crt-curvature", "0",
             "--crt-bleed", "0"]
    depth = {}
    for scan in ("1", "0"):
        path = shot(f"lines_{scan}", still + ["--crt-scanlines", scan])
        if path is None:
            print("  crt-lines ERROR while rendering")
            return failures + ["crt-lines"]
        lin = (_ppm_array(path) / 255.0) ** 2.2
        prof = lin.mean(axis=2)[h // 4:3 * h // 4, w // 3:2 * w // 3].mean(axis=1)
        n = len(prof)
        trend = np.polyval(np.polyfit(np.arange(n), prof, 2), np.arange(n))
        spec = np.abs(np.fft.rfft(prof - trend))
        k = int(np.argmax(spec[4:])) + 4
        depth[scan] = (n / k, 2.0 * spec[round(n * lines / h)] / n / prof.mean())
    want = h / lines
    period, thin = depth["1"]
    ok = (abs(period - want) <= CRT_LINE_PERIOD_TOL and thin >= CRT_LINE_DEPTH_MIN
          and depth["0"][1] <= CRT_LINE_FUSED_MAX)
    print(f"  crt-lines {'PASS' if ok else 'FAIL'}  period {period:.2f} px (want {want:.2f}), "
          f"depth {thin:.3f} of the mean thin (want >= {CRT_LINE_DEPTH_MIN}), "
          f"{depth['0'][1]:.4f} fused (want <= {CRT_LINE_FUSED_MAX})")
    if not ok:
        failures.append("crt-lines")

    # --- crt-mean -----------------------------------------------------------------
    path = shot("mean", ["--crt", "--crt-curvature", "0", "--crt-scanlines", "0"])
    if path is None:
        print("  crt-mean ERROR while rendering")
        return failures + ["crt-mean"]
    lin_off = (base / 255.0) ** 2.2
    lin_on = (_ppm_array(path) / 255.0) ** 2.2
    # Blocks of whole mask periods (three stripes, two rows), four of them each way.
    b_off, b_on = _blocks(lin_off, 8, 12), _blocks(lin_on, 8, 12)
    level = b_off.mean(axis=2)
    spread = _blocks(lin_off.mean(axis=2, keepdims=True) ** 2, 8, 12)[..., 0] - level ** 2
    flat = spread < (0.02 * level) ** 2 + 1e-7
    # Only blocks whose neighbours are flat as well: the beam and the resample reach
    # a block over, and the tube's rim darkens the outermost.
    inner = flat.copy()
    inner[[0, -1], :] = False
    inner[:, [0, -1]] = False
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            inner &= np.roll(np.roll(flat, dy, 0), dx, 1)
    model = _crt_slot_average(b_off, CRT_MEAN_DARK)
    err = b_on.mean(axis=2)[inner] / model[inner] - 1.0
    # Anti-vacuity: the model must stand away from the plain frame, or a CRT that
    # did nothing would pass. It does at both ends of the sky's range.
    reach = np.abs(model[inner] / level[inner] - 1.0).max() if inner.any() else 0.0
    worst = np.abs(err).max() if inner.any() else 1.0
    ok = worst <= CRT_MEAN_TOL and inner.sum() >= CRT_MEAN_MIN_BLOCKS and reach >= 3 * CRT_MEAN_TOL
    print(f"  crt-mean {'PASS' if ok else 'FAIL'}  worst block {worst:.4f} from the slot mask "
          f"and tone curve (bound {CRT_MEAN_TOL}), mean {err.mean() if inner.any() else 0:+.4f}, "
          f"over {inner.sum()} flat blocks; the model stands {reach:.3f} from the plain frame")
    if not ok:
        failures.append("crt-mean")

    # --- crt-curve ----------------------------------------------------------------
    curved = shot("curve1", ["--crt", "--crt-curvature", "1"])
    flat_tube = shot("curve0", ["--crt", "--crt-curvature", "0"])
    if curved is None or flat_tube is None:
        print("  crt-curve ERROR while rendering")
        return failures + ["crt-curve"]
    c1, c0 = _ppm_array(curved), _ppm_array(flat_tube)
    s = max(4, h // 50)

    def corners(img):
        return [int(img[:s, :s].max()), int(img[:s, -s:].max()),
                int(img[-s:, :s].max()), int(img[-s:, -s:].max())]

    cy, cx, r = h // 2, w // 2, max(4, h // 40)
    centre = (((c1[cy - r:cy + r, cx - r:cx + r] / 255.0) ** 2.2).mean()
              / ((c0[cy - r:cy + r, cx - r:cx + r] / 255.0) ** 2.2).mean())
    ok = max(corners(c1)) <= 1 and min(corners(c0)) >= 20 and abs(centre - 1.0) <= CRT_CENTRE_TOL
    print(f"  crt-curve {'PASS' if ok else 'FAIL'}  corners {corners(c1)} curved (want <= 1), "
          f"{corners(c0)} flat (want >= 20); centre {centre:.4f} of the flat tube's "
          f"(bound {CRT_CENTRE_TOL})")
    if not ok:
        failures.append("crt-curve")

    # --- crt-dither-last ----------------------------------------------------------
    # A tube with nothing on it but the resample, so the sky stays a smooth gradient
    # and only the dither decides whether it bands.
    smooth = ["--crt", "--crt-scanlines", "0", "--crt-mask", "0", "--crt-curvature", "0",
              "--crt-bleed", "0"]
    runs = {}
    for tag, extra in (("on", []), ("off", ["--no-dither"])):
        path = shot(f"dither_{tag}", smooth + extra, dither=True)
        if path is None:
            print("  crt-dither-last ERROR while rendering")
            return failures + ["crt-dither-last"]
        pw, ph, pix = _read_ppm(path)
        runs[tag] = _flat_run_and_span(pix, pw, ph)
    (on_run, _), (off_run, off_span) = runs["on"], runs["off"]
    ok = (on_run <= DITHER_MAX_RUN_FRAC * h and off_run >= DITHER_MIN_BAND_FRAC * h
          and off_span >= DITHER_MIN_SPAN)
    print(f"  crt-dither-last {'PASS' if ok else 'FAIL'}  longest run {on_run} px dithered "
          f"(bound {DITHER_MAX_RUN_FRAC * h:.0f}), {off_run} px undithered over a {off_span}-level "
          f"span (needs >= {DITHER_MIN_BAND_FRAC * h:.0f}: the gradient must band without it)")
    if not ok:
        failures.append("crt-dither-last")
    return failures
