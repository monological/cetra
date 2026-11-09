"""The grain group: film grain (new every frame, and its strength; spec 13.28)."""

import math
import os

import numpy as np

from gatelib.harness import asset, render
from gatelib.images import _ppm_array


# ---- Film grain (spec 13.28) -------------------------------------------------
#
# The grain is read as a FIELD: a capture with grain less its grain-free twin at
# the same frame. The scene is static and the exposure pinned, so the twin is the
# picture the grain was laid over, and everything left is grain plus a rounding.
GRAIN_SCENE = "cornell_point.cscn"
GRAIN_PIN = ["--no-auto-exposure", "-E", "1.0"]
GRAIN_STRENGTH = 0.2
# |correlation| between two consecutive frames' fields, at any shift in [-2, 2]^2.
# The diagonal crawl this spec fixed measured 0.994 at (1, 1); independent fields
# measure about 0.005 here.
GRAIN_CORR_MAX = 0.05
# Each pixel's grain divided by what the shader scales it by is a draw from a
# uniform on [-0.5, 0.5), whose spread is 1/sqrt(12). The rounding of two 8-bit
# captures adds to it; only pixels whose scale is at least this much are read, so
# the rounding stays a small fraction of the grain.
GRAIN_MIN_WEIGHT = 0.3
GRAIN_SPREAD_TOL = 0.05   # relative, against 1/sqrt(12)
GRAIN_BIAS_MAX = 0.02     # the field's mean, in the same units


def run_grain_gate(workdir):
    """Film grain: new every frame, and as strong as it says (spec 13.28).

    Both arms read the grain alone, a capture less its grain-free twin:
      grain-still    two consecutive frames' fields are uncorrelated at every shift in
                     [-2, 2]^2. A field that slides a pixel a frame correlates near 1 at
                     that shift, and one that holds still near 1 at no shift; both read
                     as structure crawling over the picture rather than as grain
      grain-amount   the field divided by what the shader scales it by -- the strength
                     and the midtone weight, read from the twin -- has the spread of a
                     uniform on [-0.5, 0.5) and no mean
    """
    scene = asset(GRAIN_SCENE)
    if not os.path.exists(scene):
        print(f"  grain-still SKIP  ({GRAIN_SCENE} not found)")
        return []

    def shot(tag, frames, extra):
        out = os.path.join(workdir, f"grain_{tag}.ppm")
        err = render(scene, out, GRAIN_PIN + extra, frames=frames)
        return None if err else _ppm_array(out)

    grain = ["--grain", str(GRAIN_STRENGTH)]
    base_a, base_b = shot("base30", 30, []), shot("base31", 31, [])
    with_a, with_b = shot("g30", 30, grain), shot("g31", 31, grain)
    if any(x is None for x in (base_a, base_b, with_a, with_b)):
        print("  grain-still ERROR while rendering")
        return ["grain"]
    field_a = (with_a - base_a).mean(axis=2)
    field_b = (with_b - base_b).mean(axis=2)
    lum = base_a.mean(axis=2)
    live = (lum > 20) & (lum < 235)

    failures = []
    # --- grain-still --------------------------------------------------------------
    worst, at = 0.0, (0, 0)
    inner = live[2:-2, 2:-2]
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            sa = field_a[2 + dy:field_a.shape[0] - 2 + dy, 2 + dx:field_a.shape[1] - 2 + dx]
            c = np.corrcoef(sa[inner], field_b[2:-2, 2:-2][inner])[0, 1]
            if abs(c) > abs(worst):
                worst, at = c, (dx, dy)
    # Anti-vacuity: a run whose grain never reached the frame correlates at nothing.
    spread = field_a[live].std()
    ok = abs(worst) <= GRAIN_CORR_MAX and spread > 1.0 and live.mean() > 0.3
    print(f"  grain-still {'PASS' if ok else 'FAIL'}  largest |corr| between frames 30 and 31 "
          f"{abs(worst):.3f} at shift {at} (bound {GRAIN_CORR_MAX}); field spread {spread:.2f} "
          f"codes over {live.mean():.0%} of the frame")
    if not ok:
        failures.append("grain-still")

    # --- grain-amount -------------------------------------------------------------
    # The shader weights by the luma of the colour it adds to, display-encoded; the
    # twin's codes are that colour to within the dither's one LSB.
    luma = (base_a @ np.array([0.299, 0.587, 0.114])) / 255.0
    weight = 1.0 - np.abs(2.0 * luma - 1.0)
    sel = live & (weight >= GRAIN_MIN_WEIGHT)
    draws = field_a[sel] / (255.0 * GRAIN_STRENGTH * weight[sel])
    want = 1.0 / math.sqrt(12.0)
    rel = draws.std() / want - 1.0
    ok = abs(rel) <= GRAIN_SPREAD_TOL and abs(draws.mean()) <= GRAIN_BIAS_MAX and sel.sum() > 10000
    print(f"  grain-amount {'PASS' if ok else 'FAIL'}  spread {draws.std():.4f} against "
          f"1/sqrt(12) = {want:.4f} ({rel:+.1%}, bound {GRAIN_SPREAD_TOL:.0%}), mean "
          f"{draws.mean():+.4f} (bound {GRAIN_BIAS_MAX}), over {sel.sum()} px")
    if not ok:
        failures.append("grain-amount")
    return failures
