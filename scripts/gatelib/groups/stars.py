"""The stars group: night star field (contribution, ramp, extinction, deck; spec 11.79)."""

import os

from gatelib.harness import asset, render
from gatelib.images import compare
from gatelib.scenes import cscn_copy
from gatelib.sky import (
    CLOUDSHADOW_FRAMES, STARS_BAND, STARS_DAY_ELEV, STARS_EMERGE_ELEVS, STARS_FIXTURE,
    STARS_NIGHT_ELEV, STARS_PIN, _stars_delta,
)


# LOW vs HIGH view elevation for the extinction arm. The committed frame spans about
# -12..+28 degrees (eye (0,400,500) -> target (0,3211,-19500) at fov 40), so there is no
# zenith to read: low sits just above the silhouette (~+6..9 deg, airmass ~8), high at
# the top of frame (~+23..27 deg, airmass ~2.3), which still separates cleanly.
STARS_LOW_BAND = (0.05, 0.47, 0.95, 0.54)
STARS_HIGH_BAND = (0.05, 0.02, 0.95, 0.12)
# Bars, measured on the shipping field at 400x300. Wide floors: the direction is the
# claim. Night, emerge and clouds sit an order of magnitude past their bars; horizon's
# margin is ~2x and CANNOT be wider, since the airmass arithmetic itself predicts ~0.39
# against the 0.75 bar. Measured: night 163,822 px; emerge 0 / 0.00144 / 0.00977 /
# 0.01127; horizon luma 0.355 with R/B 2.245 against 1.256; deck/clear 0.017 -- the
# default 0.45-coverage deck is nearly opaque at night, and the after-the-multiply
# mutation this arm exists for reads ~1.0.
STARS_NIGHT_MIN_PX = 2000
STARS_EMERGE_FLOOR = 5e-4
STARS_HORIZON_LUMA_MAX = 0.75
STARS_HORIZON_RB_MIN = 1.05
STARS_CLOUDS_MAX = 0.85
STARS_CSCN_MIN_PX = 2000


def _stars_pair(workdir, tag, elev, extra=None, frames=30):
    """Render the stars-on / stars-off twin at one elevation; returns (on, off, err)."""
    scene = asset(STARS_FIXTURE)
    on = os.path.join(workdir, f"stars_{tag}_on.ppm")
    off = os.path.join(workdir, f"stars_{tag}_off.ppm")
    common = ["--sun-elevation", elev] + STARS_PIN + (extra or [])
    err = render(scene, on, ["--stars"] + common, frames=frames) or \
        render(scene, off, ["--no-stars"] + common, frames=frames)
    return on, off, err


def run_stars_gate(workdir):
    """The procedural night star field (spec 11.79).

    Six arms, every read a delta against a --no-stars twin at the same configuration:

      stars-night     at -12 the field moves real pixels in the sky band.
      stars-daylight  at +35 stars on vs off is 0 px over the WHOLE frame. Both legs
                      upload starIntensity 0 by design (ramp at zero vs disabled), so
                      this is the guard that the ramp actually zeroes -- deleting or
                      mis-ranging the elevation ramp is what it catches.
      stars-emerge    the contribution at +5 / 0 / -6 / -12 is strictly increasing,
                      with +5 an exact zero (above the ramp's top edge). A constant
                      starIntensity fails the ordering.
      stars-horizon   the low-sky contribution is dimmer AND redder than the high-sky
                      one -- the transmittance term. Dropping transmittanceToSky from
                      the star term is what it catches.
      stars-clouds    with --clouds at -12 the sky-band contribution shrinks: the deck
                      occludes stars because the term sits under the cloud.a multiply.
                      Moving it after that multiply is what it catches. Whole band, not
                      deck-vs-gap boxes: the deck drifts with wind.
      stars-det       two runs of the -12 leg are 0 px.
      stars-cscn      the authored environment.stars block IS the flag path: a variant
                      authoring enabled+brightness renders 0 px from its flag twin (the
                      ies-flag idiom), a variant rotating the cscn-ONLY latitude and
                      hour moves real pixels (those two fields have no CLI flag, so
                      nothing else can prove their plumbing is alive), and --no-stars
                      beats an authoring file at 0 px. Deleting the parse is what the
                      first reading catches.
    """
    scene = asset(STARS_FIXTURE)
    if not os.path.exists(scene):
        print(f"  stars-night SKIP  ({STARS_FIXTURE} not present)")
        return []
    failures = []
    deltas = {}
    night_on = night_off = None
    for elev in STARS_EMERGE_ELEVS:
        on, off, err = _stars_pair(workdir, f"e{elev}", elev)
        if err:
            print(f"  stars-night ERROR render failed: {err.strip()[-200:]}")
            return ["stars-night"]
        deltas[elev] = _stars_delta(on, off, STARS_BAND)
        if elev == STARS_NIGHT_ELEV:
            night_on, night_off = on, off

    rgb, moved = deltas[STARS_NIGHT_ELEV]
    ok = moved >= STARS_NIGHT_MIN_PX
    print(f"  stars-night  {'PASS' if ok else 'FAIL'}  {moved} px moved in the sky band "
          f"at {STARS_NIGHT_ELEV} (want >={STARS_NIGHT_MIN_PX})")
    if not ok:
        failures.append("stars-night")

    d_on, d_off, err = _stars_pair(workdir, "day", STARS_DAY_ELEV)
    if err:
        print(f"  stars-daylight ERROR render failed: {err.strip()[-200:]}")
        failures.append("stars-daylight")
    else:
        day_moved, _ = compare(d_on, d_off)
        ok = day_moved == 0
        print(f"  stars-daylight {'PASS' if ok else 'FAIL'}  {day_moved} px at "
              f"+{STARS_DAY_ELEV} (want 0: the ramp must zero exactly)")
        if not ok:
            failures.append("stars-daylight")

    lumas = [sum(deltas[e][0]) / 3.0 for e in STARS_EMERGE_ELEVS]
    _, top_rung_moved = deltas[STARS_EMERGE_ELEVS[0]]
    steps = " -> ".join(f"{v:.6f}" for v in lumas)
    ok = (top_rung_moved == 0 and
          all(lumas[i] < lumas[i + 1] for i in range(len(lumas) - 1)) and
          lumas[-1] >= STARS_EMERGE_FLOOR)
    print(f"  stars-emerge {'PASS' if ok else 'FAIL'}  band contribution {steps} "
          f"(want strictly increasing, +{STARS_EMERGE_ELEVS[0]} exactly 0, "
          f"final >={STARS_EMERGE_FLOOR})")
    if not ok:
        failures.append("stars-emerge")

    lo, _ = _stars_delta(night_on, night_off, STARS_LOW_BAND)
    hi, _ = _stars_delta(night_on, night_off, STARS_HIGH_BAND)
    lo_l, hi_l = sum(lo) / 3.0, sum(hi) / 3.0
    lo_rb = lo[0] / max(lo[2], 1e-9)
    hi_rb = hi[0] / max(hi[2], 1e-9)
    ok = lo_l <= hi_l * STARS_HORIZON_LUMA_MAX and lo_rb >= hi_rb * STARS_HORIZON_RB_MIN
    print(f"  stars-horizon {'PASS' if ok else 'FAIL'}  low/high luma "
          f"{lo_l / max(hi_l, 1e-9):.3f} (want <={STARS_HORIZON_LUMA_MAX}), "
          f"low R/B {lo_rb:.3f} vs high {hi_rb:.3f} "
          f"(want >={STARS_HORIZON_RB_MIN}x)")
    if not ok:
        failures.append("stars-horizon")

    c_on, c_off, err = _stars_pair(workdir, "cloud", STARS_NIGHT_ELEV,
                                   extra=["--clouds"], frames=CLOUDSHADOW_FRAMES)
    if err:
        print(f"  stars-clouds ERROR render failed: {err.strip()[-200:]}")
        failures.append("stars-clouds")
    else:
        cloud_rgb, _ = _stars_delta(c_on, c_off, STARS_BAND)
        cloud_l = sum(cloud_rgb) / 3.0
        clear_l = sum(rgb) / 3.0
        ratio = cloud_l / max(clear_l, 1e-9)
        ok = ratio <= STARS_CLOUDS_MAX
        print(f"  stars-clouds {'PASS' if ok else 'FAIL'}  deck/clear contribution "
              f"{ratio:.3f} (want <={STARS_CLOUDS_MAX})")
        if not ok:
            failures.append("stars-clouds")

    det = os.path.join(workdir, "stars_det.ppm")
    err = render(scene, det, ["--stars", "--sun-elevation", STARS_NIGHT_ELEV] + STARS_PIN)
    if err:
        print(f"  stars-det   ERROR render failed: {err.strip()[-200:]}")
        failures.append("stars-det")
    else:
        det_moved, _ = compare(night_on, det)
        ok = det_moved == 0
        print(f"  stars-det    {'PASS' if ok else 'FAIL'}  {det_moved} px between two "
              f"runs (want 0)")
        if not ok:
            failures.append("stars-det")

    # The authored scene key, held to the flag path by generated twins (the ies-flag
    # idiom). The authored variant pins the library-default latitude/hour so the flag
    # twin CAN match it; the moved variant then rotates exactly the two fields no flag
    # reaches.
    cscn_src = asset("aerial_fixture.cscn")
    authored = os.path.join(workdir, "stars_cscn_authored.cscn")
    rotated = os.path.join(workdir, "stars_cscn_rotated.cscn")

    def _stars_author(hour, lat):
        def mutate(d):
            d["environment"]["sun"] = {"elevation": -12.0, "azimuth": 40.0}
            d["environment"]["stars"] = {"enabled": True, "brightness": 1.5,
                                         "latitude": lat, "hour_angle": hour}
        return mutate

    cscn_copy(cscn_src, authored, _stars_author(0.0, 45.0))
    cscn_copy(cscn_src, rotated, _stars_author(120.0, 10.0))
    a_frame = os.path.join(workdir, "stars_cscn_a.ppm")
    b_frame = os.path.join(workdir, "stars_cscn_b.ppm")
    c_frame = os.path.join(workdir, "stars_cscn_c.ppm")
    off_frame = os.path.join(workdir, "stars_cscn_off.ppm")
    err = render(authored, a_frame, STARS_PIN) or \
        render(scene, b_frame,
               ["--stars", "--stars-brightness", "1.5", "--sun-elevation",
                STARS_NIGHT_ELEV] + STARS_PIN) or \
        render(rotated, c_frame, STARS_PIN) or \
        render(authored, off_frame, ["--no-stars"] + STARS_PIN)
    if err:
        print(f"  stars-cscn  ERROR render failed: {err.strip()[-200:]}")
        return failures + ["stars-cscn"]
    flag_px, _ = compare(a_frame, b_frame)
    rot_px, _ = compare(a_frame, c_frame)
    cli_px, _ = compare(off_frame, night_off)
    ok = flag_px == 0 and rot_px >= STARS_CSCN_MIN_PX and cli_px == 0
    print(f"  stars-cscn   {'PASS' if ok else 'FAIL'}  authored vs flags {flag_px} px "
          f"(want 0), latitude+hour rotated {rot_px} px (want >={STARS_CSCN_MIN_PX}: "
          f"no flag reaches those two), --no-stars over the file {cli_px} px (want 0)")
    if not ok:
        failures.append("stars-cscn")

    return failures
