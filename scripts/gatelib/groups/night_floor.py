"""The night-floor group: the night-sky floor lighting the world (spec 11.80)."""

import os

from gatelib.harness import asset, render
from gatelib.images import compare
from gatelib.scenes import cscn_copy
from gatelib.sky import (
    CLOUDSHADOW_FRAMES, NIGHTFLOOR_GROUND_BAND, STARS_BAND, STARS_DAY_ELEV, STARS_FIXTURE,
    STARS_NIGHT_ELEV, STARS_PIN, _stars_delta,
)


# Bars, calibrated on the shipping floor at 400x300 after the first run.
NIGHTFLOOR_SKY_MIN = 1e-3
NIGHTFLOOR_GROUND_MIN = 2e-4
# The fog arm is a RATIO of ground-band deltas (with fog over without), because a plain
# with-fog read is vacuous against its own mutation: the LUT and IBL halves lift either
# way, and only the fog ambient -- the CPU zenith march's output -- can push the with-fog
# delta past the no-fog one. The gap is narrow and DETERMINISTIC, and the bar sits at its
# midpoint: measured 1.043 live against 0.995 with the zenith term deleted (the fog's own
# attenuation of the IBL lift is what pulls the dead reading under 1).
NIGHTFLOOR_FOG_RATIO_MIN = 1.02


def _nightfloor_pair(workdir, tag, extra, frames=30):
    """Render the floor-on / floor-off twin; returns (on, off, err)."""
    scene = asset(STARS_FIXTURE)
    on = os.path.join(workdir, f"nfloor_{tag}_on.ppm")
    off = os.path.join(workdir, f"nfloor_{tag}_off.ppm")
    err = render(scene, on, ["--night-floor"] + extra, frames=frames) or \
        render(scene, off, ["--no-night-floor"] + extra, frames=frames)
    return on, off, err


def run_nightfloor_gate(workdir):
    """The night-sky floor (spec 11.80), every read a delta against a --no-night-floor twin:

      nightfloor-sky      at -12 the sky band lifts: the LUT carries the floor.
      nightfloor-ground   at -12 the TERRAIN band lifts too -- the env->IBL path, which is
                          the feature's entire reason to exist. A floor implemented as a
                          background-shader term lifts the sky and never the ground, and
                          this is the arm that refuses that wrong-layer implementation.
      nightfloor-fog      the ground-band delta WITH --fog exceeds the no-fog delta by a
                          real ratio. Only the fog ambient (the CPU zenith march's floor
                          term) can supply the excess; a plain with-fog read would pass
                          over a dead C twin, which is this arm's named mutation.
      nightfloor-daylight at +35 on vs off is 0 px: the shared civil-twilight ramp zeroes
                          and the LUT is bit-identical.
      nightfloor-cscn     the authored environment.night_floor block IS the flag path
                          (0 px from its flag twin), and --no-night-floor beats the
                          authoring file at 0 px. Deleting the parse is what it catches.

    Read as deltas throughout: the floor is a small absolute lift, so an on-frame alone
    says nothing about whether the term arrived.
    """
    scene = asset(STARS_FIXTURE)
    if not os.path.exists(scene):
        print(f"  nightfloor-sky SKIP  ({STARS_FIXTURE} not present)")
        return []
    failures = []

    night = ["--sun-elevation", STARS_NIGHT_ELEV] + STARS_PIN
    on, off, err = _nightfloor_pair(workdir, "night", night)
    if err:
        print(f"  nightfloor-sky ERROR render failed: {err.strip()[-200:]}")
        return ["nightfloor-sky"]
    sky_rgb, _ = _stars_delta(on, off, STARS_BAND)
    ground_rgb, _ = _stars_delta(on, off, NIGHTFLOOR_GROUND_BAND)
    sky_l = sum(sky_rgb) / 3.0
    ground_l = sum(ground_rgb) / 3.0

    ok = sky_l >= NIGHTFLOOR_SKY_MIN
    print(f"  nightfloor-sky {'PASS' if ok else 'FAIL'}  sky-band lift {sky_l:.6f} "
          f"(want >={NIGHTFLOOR_SKY_MIN})")
    if not ok:
        failures.append("nightfloor-sky")

    ok = ground_l >= NIGHTFLOOR_GROUND_MIN
    print(f"  nightfloor-ground {'PASS' if ok else 'FAIL'}  terrain-band lift {ground_l:.6f} "
          f"(want >={NIGHTFLOOR_GROUND_MIN}: only the env->IBL path can put it there)")
    if not ok:
        failures.append("nightfloor-ground")

    f_on, f_off, err = _nightfloor_pair(workdir, "fog", night + ["--fog"],
                                        frames=CLOUDSHADOW_FRAMES)
    if err:
        print(f"  nightfloor-fog ERROR render failed: {err.strip()[-200:]}")
        failures.append("nightfloor-fog")
    else:
        fog_rgb, _ = _stars_delta(f_on, f_off, NIGHTFLOOR_GROUND_BAND)
        fog_l = sum(fog_rgb) / 3.0
        ratio = fog_l / max(ground_l, 1e-9)
        ok = ratio >= NIGHTFLOOR_FOG_RATIO_MIN
        print(f"  nightfloor-fog {'PASS' if ok else 'FAIL'}  with-fog/no-fog ground lift "
              f"{ratio:.3f} (want >={NIGHTFLOOR_FOG_RATIO_MIN}: the excess is the zenith "
              f"march's floor term)")
        if not ok:
            failures.append("nightfloor-fog")

    d_on, d_off, err = _nightfloor_pair(workdir, "day",
                                        ["--sun-elevation", STARS_DAY_ELEV] + STARS_PIN)
    if err:
        print(f"  nightfloor-daylight ERROR render failed: {err.strip()[-200:]}")
        failures.append("nightfloor-daylight")
    else:
        day_moved, _ = compare(d_on, d_off)
        ok = day_moved == 0
        print(f"  nightfloor-daylight {'PASS' if ok else 'FAIL'}  {day_moved} px at "
              f"+{STARS_DAY_ELEV} (want 0: the shared ramp must zero exactly)")
        if not ok:
            failures.append("nightfloor-daylight")

    cscn_src = asset("aerial_fixture.cscn")
    authored = os.path.join(workdir, "nfloor_cscn.cscn")

    def _floor_author(d):
        d["environment"]["sun"] = {"elevation": -12.0, "azimuth": 40.0}
        d["environment"]["night_floor"] = {"enabled": True, "brightness": 2.0}

    cscn_copy(cscn_src, authored, _floor_author)
    a_frame = os.path.join(workdir, "nfloor_cscn_a.ppm")
    b_frame = os.path.join(workdir, "nfloor_cscn_b.ppm")
    off_frame = os.path.join(workdir, "nfloor_cscn_off.ppm")
    err = render(authored, a_frame, STARS_PIN) or \
        render(scene, b_frame,
               ["--night-floor", "--night-floor-brightness", "2.0", "--sun-elevation",
                STARS_NIGHT_ELEV] + STARS_PIN) or \
        render(authored, off_frame, ["--no-night-floor"] + STARS_PIN)
    if err:
        print(f"  nightfloor-cscn ERROR render failed: {err.strip()[-200:]}")
        return failures + ["nightfloor-cscn"]
    flag_px, _ = compare(a_frame, b_frame)
    cli_px, _ = compare(off_frame, off)
    ok = flag_px == 0 and cli_px == 0
    print(f"  nightfloor-cscn {'PASS' if ok else 'FAIL'}  authored vs flags {flag_px} px "
          f"(want 0), --no-night-floor over the file {cli_px} px (want 0)")
    if not ok:
        failures.append("nightfloor-cscn")

    return failures
