"""The ladder group: the alpha mip ladder (the dither on the live path, spec 11.101)."""

import re
import os

from gatelib.harness import RENDER, _import_fixture_gen, _run, asset, render
from gatelib.images import _alphacov_mean, _pixels_past, _read_ppm, compare


# The alpha mip ladder (spec 11.101), on assets/alpha_ladder_fixture: three
# rows of facing cards, each card one mip deeper, on a flat backdrop. The
# instrument that separated the physics from the geometry after the grazing
# dot plane had confounded every live-path measurement. Its arms read the
# DOTS row -- the one content class on which distribution fires before the
# 1x1 tail -- as mean luma over each card minus the sky, in raw codes.
LADDER_FIXTURE = "alpha_ladder_fixture.cscn"
LADDER_SKY = (0.0, 0.02, 1.0, 0.10)   # backdrop rows above the top row
# What each of the first-fire levels must read: -1 on the foliage and speck
# rows (distribution reaches only their 1x1, which a binary-AND-mixed scan
# rightly cannot call a dither) and 3 on the lattice. The finding in one
# arm: irregular content never enters the visible dither regime.
LADDER_FIRE = {"alpha_ladder_leaves.png": -1, "alpha_ladder_specks.png": -1,
               "alphacov_dots.png": 3}
# TAA off, the truth: what 11.100 stored. Dots cards 4-6 (mips 4-6, all past
# the lattice's first fire at 3) measured 24.0 / 16.6 / 18.9; a build with
# distribution reverted reads ~0 there, since the rescale cannot reach a
# uniform level's fractional target.
LADDER_TRUTH_MIN = 10.0
# Alpha-to-coverage carries the dither through the live path: MSAA 4 + TAA
# settles at 11.1 / 10.6 / 14.1 on those cards, 0.60 of the truth's mean,
# where one sample settles at 4.3 / 4.0 / 3.1 -- 0.19 -- because a binary test
# under a clamped history cannot hold an 11% haze. The bar is the mean ratio,
# two thirds of the settled reading and twice one sample's.
#
# MEASURED SETTLED, frames 62 to 602 alike, since spec 14.8 put the textures in
# before frame 1. Until then the cards drew untextured for their first frames,
# and at frame 62 TAA's history was still fading from them: 0.89, the reading
# this bar (then 0.6) was set against, and 0.72 with the lookup's A2C gate
# removed. Run long, the build before 14.8 settles where this one does.
LADDER_A2C_MIN_RATIO = 0.4
# And it is STILL there: dots-row strong flips (> LADDER_FLIP_CODES) between
# adjacent frames of one MSAA 4 + TAA run, measured 8 -- against 358 at one
# sample with the lookup, 2,917 without it, and 454 with the lookup let
# loose under A2C. The bar sits 25x above the reading and under all three.
LADDER_FLIP_CODES = 8
LADDER_A2C_CHURN_MAX = 200
# The jittered lookup's own claim, on the path that has no samples: one
# sample + TAA, adjacent-frame dots-row flips 358 with it against 2,917
# without (--no-alpha-jitter is the falsifier); the bar is 3.4x above the
# reading and 2.4x under the falsifier. It does NOT recover the coverage
# that path loses; that is what the A2C arms are for. Every number here is
# at the gate's 800x600 framebuffer, where the ladder's mips are calibrated
# -- a hand render at -W 800 lands at 1600x1200 on a HiDPI host and shifts
# every card one mip finer.
LADDER_CHURN_MAX = 1200


_LADDER_PROBE = re.compile(r"^texture-probe tex .*?distribute_from=(-?\d+) name=(.+)$",
                           re.MULTILINE)


def _ladder_card_box(gen, row, k):
    """The read box of card k on `row`, inset 5% each side, as frame fractions."""
    cy = 0.5 - 0.5 * gen.ROW_Y[row]
    half_h = gen.CARD_H * 0.5
    half_w = half_h * gen.GATE_H / gen.GATE_W
    cx = (k + 0.5) / gen.CARDS
    inset = 0.9
    return (cx - half_w * inset, cy - half_h * inset, cx + half_w * inset, cy + half_h * inset)


def _ladder_lifts(gen, pix, w, h, row, cards):
    """Mean luma over each named card minus the sky, in raw codes."""
    sky = _alphacov_mean(pix, w, h, LADDER_SKY)
    return [_alphacov_mean(pix, w, h, _ladder_card_box(gen, row, k)) - sky for k in cards]


def _ladder_pair(workdir, scene, tag, extra):
    """Frames 61 and 62 of ONE run -- adjacent, from one process (the _taa_churn
    arrangement), so they share a history. They also shared a load order once,
    before spec 14.8 put a headless run's textures in before frame 1.
    Returns (pix61, pix62, w, h) or None."""
    base = os.path.join(workdir, f"ladder_{tag}.ppm")
    cmd = [RENDER, "-m", scene, "-x", "-f", "62", "--screenshot-every", "61",
           "-W", "400", "-H", "300", "-S", base] + extra
    r = _run(cmd, capture_output=True, text=True)
    f61 = base[:-4] + "_000061.ppm"
    if r.returncode != 0 or not (os.path.exists(f61) and os.path.exists(base)):
        print(f"  ladder       ERROR {tag} run failed: {(r.stdout + r.stderr).strip()[-200:]}")
        return None
    w, h, a = _read_ppm(f61)
    _, _, b = _read_ppm(base)
    return a, b, w, h


def run_ladder_gate(workdir):
    """The dither on the LIVE path, judged where the geometry cannot lie (spec 11.101).

      ladder-fire    which content dithers at all. The texture probe's
                     distribute_from reads -1 on the foliage and speck rows
                     and 3 on the lattice: on irregular content the 11.88
                     rescale finds a scale at every level down to the 1x1, so
                     distribution never enters a visible mip and the jittered
                     lookup never fires there. Render-free -- read off the
                     truth run's probe rows.

      ladder-truth   what 11.100 stored reaches the screen with TAA off: the
                     dots cards past the first fire hold their coverage.

      ladder-a2c     and alpha-to-coverage carries it through the live path:
                     MSAA 4 + TAA holds three fifths of the truth's coverage
                     on those cards where one sample keeps a fifth -- a binary
                     test under a clamped history cannot hold a fractional
                     haze. This is the finding: the MSAA path IS the live-path
                     fix.

      ladder-still   and it holds it STILL: adjacent-frame strong flips on the
                     dots row of one MSAA 4 + TAA run stay under the bar, at
                     twenty times fewer than one sample shows.

      ladder-churn   the jittered lookup's own claim, on the path with no
                     samples: at one sample + TAA the dots row's adjacent
                     churn stays under the bar with it and clears it by 2x
                     without -- --no-alpha-jitter is the standing falsifier.

      ladder-gate    and the lookup is INERT under alpha-to-coverage, where it
                     measured a net loss: an MSAA 4 + TAA frame is byte-
                     identical with and without it.

    WHY A LADDER. Every card is the same size at the same distance; the UV
    range doubles per card, so card k samples mip k+1 of a seamless texture
    and one frame shows every regime side by side under one AA state. The
    grazing dot plane -- anisotropic filtering over a continuous lod gradient
    -- had read the jittered lookup as WORSE than the crawl it dissolves; on
    facing geometry the same lookup cuts the one-sample churn 5x. Both were
    measured; the plane's number is the one that does not transfer.

    WHAT IS NOT CLAIMED. The one-sample path's lost coverage: no lookup
    recovers it, and no arm pretends to. Two-process jittered frames are
    byte-compared only by ladder-gate, whose 0 px identity has held on this
    small fixture. The first suspect for a handful of pixels there used to be
    the async-load caveat in apps/render/src/render.c; since spec 14.8 a
    headless run's start-up textures are in before frame 1, so it no longer is.
    """
    arms = ("ladder-fire", "ladder-truth", "ladder-a2c", "ladder-still", "ladder-churn",
            "ladder-gate")
    scene = asset(LADDER_FIXTURE)
    if not os.path.exists(scene):
        for arm in arms:
            print(f"  {arm} SKIP  ({LADDER_FIXTURE} not present)")
        return []
    gen = _ladder_gen()
    if gen is None:
        for arm in arms[1:]:
            print(f"  {arm:<13} SKIP  (gen_alpha_ladder_fixture unavailable)")
        return []
    dots = len(gen.ROW_Y) - 1
    deep = [k for k in range(gen.CARDS) if k + gen.FIRST_MIP > LADDER_FIRE["alphacov_dots.png"]]
    failures = []

    # The truth run doubles as the probe run.
    out = os.path.join(workdir, "ladder_truth.ppm")
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "-W", "400", "-H", "300", "--texture-probe",
           "-S", out]
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        print(f"  ladder-fire ERROR truth run failed: {(r.stdout + r.stderr).strip()[-200:]}")
        return list(arms)
    seen = {}
    for m in _LADDER_PROBE.finditer(r.stdout + r.stderr):
        seen[os.path.basename(m.group(2).strip())] = int(m.group(1))
    got = {name: seen.get(name) for name in LADDER_FIRE}
    ok = got == LADDER_FIRE
    print(f"  ladder-fire {'PASS' if ok else 'FAIL'}  first dithered level per texture "
          f"{got} (want {LADDER_FIRE}: the foliage and speck rows "
          f"never enter a visible dither, the lattice does at 3)")
    if not ok:
        failures.append("ladder-fire")

    w, h, pix = _read_ppm(out)
    truth = _ladder_lifts(gen, pix, w, h, dots, deep)
    ok = all(v >= LADDER_TRUTH_MIN for v in truth)
    print(f"  ladder-truth {'PASS' if ok else 'FAIL'}  TAA off, dots cards {deep[0] + 1}-"
          f"{deep[-1] + 1} lift {' / '.join(f'{v:.1f}' for v in truth)} over the sky (want each "
          f">= {LADDER_TRUTH_MIN}; a build with distribution reverted reads ~0)")
    if not ok:
        failures.append("ladder-truth")

    a2c = _ladder_pair(workdir, scene, "a2c", ["--taa", "--headless-jitter", "--msaa", "4"])
    if a2c is None:
        return failures + list(arms[2:])
    pa, pb, w, h = a2c
    lifts = _ladder_lifts(gen, pb, w, h, dots, deep)
    ratio = (sum(lifts) / len(lifts)) / (sum(truth) / len(truth)) if sum(truth) > 0 else 0.0
    ok = ratio >= LADDER_A2C_MIN_RATIO
    print(f"  ladder-a2c {'PASS' if ok else 'FAIL'}  MSAA 4 + TAA, the same cards lift "
          f"{' / '.join(f'{v:.1f}' for v in lifts)}, {ratio:.2f} of the truth (want >= "
          f"{LADDER_A2C_MIN_RATIO}; one sample reads 0.19 -- a binary test under a clamped "
          f"history cannot hold a fractional haze)")
    if not ok:
        failures.append("ladder-a2c")

    row_box = (0.0, 0.5 - 0.5 * gen.ROW_Y[dots] - gen.CARD_H * 0.5, 1.0,
               0.5 - 0.5 * gen.ROW_Y[dots] + gen.CARD_H * 0.5)
    flips = _pixels_past(pa, pb, w, h, row_box, LADDER_FLIP_CODES)
    ok = flips <= LADDER_A2C_CHURN_MAX
    print(f"  ladder-still {'PASS' if ok else 'FAIL'}  MSAA 4 + TAA, {flips} dots-row pixels move "
          f"more than {LADDER_FLIP_CODES} codes between adjacent frames (want <= "
          f"{LADDER_A2C_CHURN_MAX}; one sample reads 358 with the lookup and 2,917 without, "
          f"and the lookup let loose under A2C reads 454)")
    if not ok:
        failures.append("ladder-still")

    one = _ladder_pair(workdir, scene, "one", ["--taa", "--headless-jitter", "--msaa", "1"])
    if one is None:
        return failures + list(arms[4:])
    qa, qb, _, _ = one
    flips = _pixels_past(qa, qb, w, h, row_box, LADDER_FLIP_CODES)
    ok = flips <= LADDER_CHURN_MAX
    print(f"  ladder-churn {'PASS' if ok else 'FAIL'}  one sample + TAA with the jittered lookup, "
          f"{flips} dots-row pixels move between adjacent frames (want <= {LADDER_CHURN_MAX}; "
          f"--no-alpha-jitter reads 2,917)")
    if not ok:
        failures.append("ladder-churn")

    off = os.path.join(workdir, "ladder_a2c_off.ppm")
    err = render(scene, off, ["--taa", "--headless-jitter", "--msaa", "4", "--no-alpha-jitter"],
                 frames=62)
    if err:
        print(f"  ladder-gate ERROR render failed: {err.strip()[-200:]}")
        return failures + ["ladder-gate"]
    base = os.path.join(workdir, "ladder_a2c.ppm")
    px = compare(base, off)[0]
    ok = px == 0
    print(f"  ladder-gate {'PASS' if ok else 'FAIL'}  MSAA 4 + TAA with and without the lookup: "
          f"{px} px (want exactly 0: under alpha-to-coverage the lookup measured a net loss and "
          f"is gated off)")
    if not ok:
        failures.append("ladder-gate")
    return failures


_LADDER_GEN = None


def _ladder_gen():
    global _LADDER_GEN
    if _LADDER_GEN is None:
        _LADDER_GEN = _import_fixture_gen("gen_alpha_ladder_fixture.py", "ladder-fire")
    return _LADDER_GEN
