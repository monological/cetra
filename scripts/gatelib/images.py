"""Reading pictures and measuring them."""

import math
from itertools import groupby
import subprocess
import sys

import numpy as np


# Pass on PEAK error, not on a differing-pixel count.
#
# A per-gate pixel budget is the wrong instrument: the numbers drift whenever
# float arithmetic reassociates (moving one multiply out of a sum is enough),
# so the budget gets raised to match, and after a few rounds it is large enough
# to hide a real defect. Peak error does not drift -- 1/255 is one 8-bit
# quantization step, so anything at or below it cannot be seen in the output at
# all, and anything above it is a genuine difference no matter how few pixels
# carry it. AE is still reported, because a jump in how MANY pixels round
# differently is worth a look even when none of them moved visibly.
LSB = 1.0 / 255.0 + 1e-6


def compare(a, b):
    """Return (differing pixels, peak absolute error as a 0..1 fraction).

    A magick that could not read one of the two answers with a message rather
    than a number ("compare: unable to open image ..."), which used to raise out
    of here and take the whole run down over one missing frame. Reported as a
    full-frame difference instead: the caller is asking whether two images agree,
    and one that does not exist does not agree.
    """
    def metric(name):
        r = subprocess.run(["magick", "compare", "-metric", name, a, b, "null:"],
                           capture_output=True, text=True)
        return (r.stderr or r.stdout).strip()

    try:
        ae = int(float(metric("AE").split()[0]))
    except (ValueError, IndexError):
        return sys.maxsize, 1.0
    # PAE prints "257 (0.00392157)"; the parenthesised value is the normalised one.
    pae_raw = metric("PAE")
    pae = float(pae_raw.split("(")[1].rstrip(")")) if "(" in pae_raw else 0.0
    return ae, pae


def _read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    fields, i = [], 0
    while len(fields) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    return int(fields[1]), int(fields[2]), data[i + 1:]


# The sRGB decode, once, indexed by the raw byte. Every measurement here is on
# a linear signal, and the per-pixel pow is slow enough in pure Python to notice
# on the gates that scan whole frames.
_SRGB_TO_LINEAR = [(c / 255.0 / 12.92) if c / 255.0 <= 0.04045
                   else (((c / 255.0) + 0.055) / 1.055) ** 2.4 for c in range(256)]
# The inverse of tonemap_frag's displayEncode, which is a plain 2.2 gamma and not sRGB.
# An arm reading an ABSOLUTE radiance through --tonemap linear decodes with this; the
# sRGB table reads the same codes up to 2% dark, which a ratio forgives and a level
# does not.
_DISPLAY_TO_LINEAR = [(c / 255.0) ** 2.2 for c in range(256)]


def _linear_luma(pix, w, h, px, py, decode=_SRGB_TO_LINEAR):
    """Undo the display encode -- the transition is measured on a linear signal."""
    x = max(0, min(w - 1, int(round(px))))
    y = max(0, min(h - 1, int(round(py))))
    o = (y * w + x) * 3
    return (decode[pix[o]] + decode[pix[o + 1]] + decode[pix[o + 2]]) / 3.0


def _linear_rgb(pix, w, h, px, py):
    """Per-channel sibling of _linear_luma; the reddening assertion needs the
    channels apart, and an average would hide exactly what it is looking for."""
    x = max(0, min(w - 1, int(round(px))))
    y = max(0, min(h - 1, int(round(py))))
    o = (y * w + x) * 3
    return [_SRGB_TO_LINEAR[pix[o + k]] for k in range(3)]


def _oit_untonemap(rgb):
    """Display sample -> linear HDR, inverting displayEncode and Neutral.

    Neutral subtracts an offset driven by the sample's MINIMUM channel: a flat
    0.04 above a 0.08 toe, and 6.25x^2 below it. Both branches invert in closed
    form, and the fixture's colours are chosen to stay in the first one -- the
    second is here so a future colour edit fails loudly rather than quietly
    reading 4% low.

    pow(2.2), NOT the sRGB piecewise curve _linear_rgb uses: tonemap_frag's
    displayEncode is a plain 1/2.2 gamma, so this is the one that matches the
    engine. The two differ by ~0.01 in the shadows, which matters here because
    this gate compares absolute radiance against arithmetic rather than
    comparing two samples of one render. Reconciling _linear_rgb would re-tune
    every threshold fitted through it, so the divergence stands and is recorded
    rather than silently inherited.
    """
    ldr = [max(0.0, min(1.0, c)) ** 2.2 for c in rgb]
    m = min(ldr)
    offset = 0.4 * math.sqrt(m) - m if m < 0.04 else 0.04
    return [c + offset for c in ldr]


# Output dither (spec 11.24 / E1), measured on assets/aerial_fixture.
#
# The sky is a shallow gradient, which is exactly where the 8-bit write leaves
# contour bands: long runs of one value separated by 1-LSB steps. Dither breaks
# the runs without moving the local mean, so the longest run down a column is
# the statistic that discriminates -- and it needs no stored reference.
#
# Bounds are FRACTIONS OF FRAME HEIGHT, not pixel counts. A run length scales
# with the frame, and `render()` asks for a 400x300 window whose framebuffer is
# 800x600 on a HiDPI display and 400x300 everywhere else -- so absolute counts
# calibrated on one machine sit at zero margin on the other. Measured both ways:
# 115/20/4 px at 800x600 and 60/14/4 at 400x300, i.e. ~19%/3.3%/0.7% of height
# either way. The fractions below sit about halfway between those.
DITHER_MAX_RUN_FRAC = 0.08   # dithered: the band must collapse past this
DITHER_MIN_BAND_FRAC = 0.10  # undithered: the fixture must still BAND
DITHER_MIN_SPAN = 24         # 8-bit levels the sampled columns must cover


def _flat_run_and_span(pix, w, h):
    """Longest single-value run down a column, and the value range covered.

    The run is the debanding statistic: a quantization contour is a long run of
    one value, and dither breaks it without moving the local mean. Clipped 0/255
    runs are excluded -- the dither is faded to nothing at the clip points, so a
    clamped region neither bands nor dithers and would only measure letterbox.

    The span is what stops the run being read on its own. A CONSTANT image
    MAXIMIZES run length, so a floor on the run alone is satisfied at its best
    possible score by a fixture that lost its gradient entirely.
    """
    worst, lo, hi = 0, 255, 0
    for x in range(20, w - 20, max(1, (w - 40) // 24)):
        for ch in range(3):
            col = pix[x * 3 + ch:w * h * 3:w * 3]
            lo, hi = min(lo, min(col)), max(hi, max(col))
            for v, run in groupby(col):
                if v not in (0, 255):
                    worst = max(worst, sum(1 for _ in run))
    return worst, hi - lo


def _ppm_array(path):
    """A capture as an (h, w, 3) float array of 8-bit codes."""
    w, h, data = _read_ppm(path)
    return np.frombuffer(data[:w * h * 3], dtype=np.uint8).reshape(h, w, 3).astype(np.float64)
MASK_GRID = 12


def _box_luma(pix, w, h, box):
    """Mean linear luma over a fractional box, on a MASK_GRID square grid.

    Named for what it does rather than for the group that first wanted it: the shared
    readers live a long way above the gates that need them, and a group-scoped name is
    how a later group ends up copying its neighbour instead of calling this.
    """
    x0, y0, x1, y1 = box
    n = MASK_GRID
    samples = [_linear_luma(pix, w, h, (x0 + (x1 - x0) * (ix + 0.5) / n) * w,
                            (y0 + (y1 - y0) * (iy + 0.5) / n) * h)
               for iy in range(n) for ix in range(n)]
    return sum(samples) / len(samples)
ABSORB_GRID = 12


def _absorb_box_rgb(pix, w, h, box, n=ABSORB_GRID):
    """Mean linear RGB over a fractional box. Per-channel, not luma: the whole
    measurement is that the channels separate, which an average would erase."""
    x0, y0, x1, y1 = box
    acc = [0.0, 0.0, 0.0]
    for iy in range(n):
        for ix in range(n):
            rgb = _linear_rgb(pix, w, h, (x0 + (x1 - x0) * (ix + 0.5) / n) * w,
                              (y0 + (y1 - y0) * (iy + 0.5) / n) * h)
            for k in range(3):
                acc[k] += rgb[k]
    return [a / (n * n) for a in acc]


def _box_luma_dense(pix, w, h, box, decode=_SRGB_TO_LINEAR):
    """Mean linear luma over EVERY pixel of a fractional box (`_box_luma` samples a grid).

    `decode` is the curve the frame was encoded with: sRGB by default, `_DISPLAY_TO_LINEAR`
    for a frame read as an absolute level through --tonemap linear.
    """
    x0, y0, x1, y1 = box
    vals = [_linear_luma(pix, w, h, px, py, decode)
            for py in range(int(y0 * h), int(y1 * h))
            for px in range(int(x0 * w), int(x1 * w))]
    return sum(vals) / len(vals)


def _alphacov_mean(pix, w, h, box):
    """Mean luma over a fractional (x0, y0, x1, y1) box.

    The deep arm's instrument, and deliberately not a bright-pixel count: a
    dither's coverage arrives as mass spread thin, so a threshold deletes most
    of it while a mean conserves it -- counted, the distributing build read
    LOWER than the vanishing one. And deliberately not _box_luma, twice over:
    its MASK_GRID point sampling can alias against a sparse dither's period
    where this scans exhaustively, and it averages LINEAR luma where the
    ALPHACOV_DEEP bounds are calibrated in raw 8-bit codes, the currency
    ALPHACOV_LUMA already uses.
    """
    x0, y0 = int(w * box[0]), int(h * box[1])
    x1, y1 = int(w * box[2]), int(h * box[3])
    s = n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            o = (y * w + x) * 3
            s += 0.2126 * pix[o] + 0.7152 * pix[o + 1] + 0.0722 * pix[o + 2]
            n += 1
    return s / n if n else 0.0


def _pixels_past(pix_a, pix_b, w, h, box, codes):
    """Pixels inside a fractional box that differ by more than `codes` on any channel between
    two frames: the churn a dither shows under TAA, or how far apart two pictures are."""
    x0, y0 = int(w * box[0]), int(h * box[1])
    x1, y1 = int(w * box[2]), int(h * box[3])
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            o = (y * w + x) * 3
            if max(abs(pix_a[o] - pix_b[o]), abs(pix_a[o + 1] - pix_b[o + 1]),
                   abs(pix_a[o + 2] - pix_b[o + 2])) > codes:
                n += 1
    return n
