"""The cycle group: the day/night clock and its sliced env re-bake (spec 11.81)."""

import json
import os

from gatelib.harness import asset, render
from gatelib.images import _SRGB_TO_LINEAR, _read_ppm, compare
from gatelib.scenes import cscn_copy
from gatelib.sky import (
    CYCLE_LATITUDE, CYCLE_QUIESCE_HOUR, STARS_FIXTURE, STARS_PIN, _cycle_sun_path,
    _sliced_vs_atomic,
)


# Bars measured on the shipping schedule; the two 0 px arms are exact by construction.
CYCLE_MOVES_MIN_PX = 20000
CYCLE_ORDER_MIN = 1.15


def run_cycle_gate(workdir):
    """The day/night cycle (spec 11.81): one clock, and a bake spread across frames.

      cycle-quiesce  a FROZEN cycle at hour H renders 0 px from explicit --sun-elevation
                     /--sun-azimuth taken from this file's own twin of sky_sun_path. Pins
                     the path formula AND proves an armed-but-frozen clock touches
                     nothing -- which is also what makes --day-cycle 0 a no-op rather
                     than a sun teleport, the defect the review caught here.
      cycle-slice    --cycle-rebake-at drives ONE sliced re-bake over an unmoved sun; the
                     frame reads 0 px against the same run without it. The acceptance bar
                     the roadmap row named: it says the slicer IS the atomic bake, byte
                     for byte, with no hour arithmetic anywhere in the comparison. A
                     dropped face, a skipped mip, a swap that forgets max_reflection_lod
                     or any divergence between the two drivers lands here.
      cycle-moves    under a running clock the frame changes a lot, and sky brightness
                     ORDERS across a sunset (day > dusk > night). A dead driver or a
                     clock that does not advance fails.
      cycle-det      two identical runs of a moving cycle are 0 px -- the clock rides the
                     frame clock, not the wall.
      cycle-cscn     the authored environment.cycle block IS the flag path (0 px), and
                     --no-day-cycle beats a file that asked for one. The stars-cscn and
                     nightfloor-cscn idiom; without it the whole scene-key path is
                     unexercised.
      cycle-config   a dumped session restores to the same frame. The ONLY cover for the
                     three cycle rows and for CFG_DOUBLE's write/decode/store:
                     config-roundtrip is blind to a dropped store (the default lands in
                     both dumps) and config-perturb now excuses these rows by name.

    The GUI's disabled-slider guards are deliberately NOT covered -- no headless arm can
    reach them, and saying so beats leaving them looking tested.
    """
    scene = asset(STARS_FIXTURE)
    if not os.path.exists(scene):
        print(f"  cycle-quiesce SKIP  ({STARS_FIXTURE} not present)")
        return []
    failures = []

    el, az = _cycle_sun_path(CYCLE_LATITUDE, CYCLE_QUIESCE_HOUR)
    frozen = os.path.join(workdir, "cycle_frozen.ppm")
    static = os.path.join(workdir, "cycle_static.ppm")
    err = render(scene, frozen,
                 ["--day-cycle", "0", "--time-of-day", repr(CYCLE_QUIESCE_HOUR)] + STARS_PIN) or \
        render(scene, static,
               ["--sun-elevation", repr(el), "--sun-azimuth", repr(az)] + STARS_PIN)
    if err:
        print(f"  cycle-quiesce ERROR render failed: {err.strip()[-200:]}")
        return ["cycle-quiesce"]
    px, _ = compare(frozen, static)
    ok = px == 0
    print(f"  cycle-quiesce {'PASS' if ok else 'FAIL'}  frozen cycle vs the twin's angles "
          f"{px} px (want 0; el={el:.6f} az={az:.6f})")
    if not ok:
        failures.append("cycle-quiesce")

    # The slicer against the atomic bake, sun held still. Frames well past the
    # rebake so the schedule has converged and swapped.
    #
    # TWO scenes, and the second is not thoroughness. This fixture has no
    # sheen, so nothing in its frame reads the Charlie prefilter chain --
    # deleting that chain from the slicer entirely leaves it 0 px, measured.
    # sheen_fixture is what makes the arm cover all four handles the swap
    # moves; without it a third of the schedule is asserted by nothing.
    base = ["--sun-elevation", "12", "--sun-azimuth", "40"] + STARS_PIN
    slice_scenes = [("sky", scene, base),
                    ("sheen", asset("sheen_fixture.gltf"),
                     ["--sky"] + base)]
    for tag, path, flags in slice_scenes:
        if not os.path.exists(path):
            print(f"  cycle-slice SKIP  ({os.path.basename(path)} not present)")
            break
        px, err = _sliced_vs_atomic(workdir, f"cycle_{tag}", path, flags)
        if err:
            print(f"  cycle-slice ERROR render failed: {err.strip()[-200:]}")
            failures.append("cycle-slice")
            break
        ok = px == 0
        print(f"  cycle-slice {'PASS' if ok else 'FAIL'}  {tag}: sliced re-bake vs atomic "
              f"{px} px (want 0: the slicer IS the atomic bake)")
        if not ok:
            failures.append("cycle-slice")
            break

    # A moving clock: three captures across a sunset from one run.
    moving = os.path.join(workdir, "cycle_move.ppm")
    err = render(scene, moving,
                 ["--day-cycle", "6", "--time-of-day", "10"] + STARS_PIN +
                 ["--screenshot-every", "30"], frames=90)
    if err:
        print(f"  cycle-moves ERROR render failed: {err.strip()[-200:]}")
        failures.append("cycle-moves")
    else:
        shots = [moving.replace(".ppm", f"_{n:06d}.ppm") for n in (30, 60, 90)]
        if not all(os.path.exists(s) for s in shots):
            print("  cycle-moves FAIL  the schedule wrote no intermediate frames")
            failures.append("cycle-moves")
        else:
            lum = []
            for s in shots:
                w, h, pix = _read_ppm(s)
                acc, n = 0.0, 0
                x0, x1 = int(0.05 * w), int(0.95 * w)
                y0, y1 = int(0.04 * h), int(0.42 * h)
                for y in range(y0, y1, 3):
                    for x in range(x0, x1, 3):
                        o = (y * w + x) * 3
                        acc += sum(_SRGB_TO_LINEAR[pix[o + k]] for k in range(3)) / 3.0
                        n += 1
                lum.append(acc / max(n, 1))
            moved, _ = compare(shots[0], shots[-1])
            falling = all(lum[i] > lum[i + 1] * CYCLE_ORDER_MIN for i in range(len(lum) - 1))
            ok = moved >= CYCLE_MOVES_MIN_PX and falling
            steps = " -> ".join(f"{v:.5f}" for v in lum)
            print(f"  cycle-moves {'PASS' if ok else 'FAIL'}  sky {steps} "
                  f"(want each >={CYCLE_ORDER_MIN}x the next), {moved} px first-to-last "
                  f"(want >={CYCLE_MOVES_MIN_PX})")
            if not ok:
                failures.append("cycle-moves")

    det = os.path.join(workdir, "cycle_det.ppm")
    err = render(scene, det, ["--day-cycle", "6", "--time-of-day", "10"] + STARS_PIN,
                 frames=90)
    if err:
        print(f"  cycle-det   ERROR render failed: {err.strip()[-200:]}")
        failures.append("cycle-det")
    else:
        px, _ = compare(det, moving)
        ok = px == 0
        print(f"  cycle-det    {'PASS' if ok else 'FAIL'}  {px} px between two moving runs "
              f"(want 0)")
        if not ok:
            failures.append("cycle-det")

    # The authored block against its flag twin, and the way off it.
    cscn_src = asset("aerial_fixture.cscn")
    authored = os.path.join(workdir, "cycle_cscn.cscn")

    def _cycle_author(d):
        d["environment"]["cycle"] = {"enabled": True, "day_seconds": 0.0,
                                     "hour": CYCLE_QUIESCE_HOUR}

    cscn_copy(cscn_src, authored, _cycle_author)
    a_frame = os.path.join(workdir, "cycle_cscn_a.ppm")
    off_frame = os.path.join(workdir, "cycle_cscn_off.ppm")
    plain_sun = os.path.join(workdir, "cycle_cscn_plain.ppm")
    err = render(authored, a_frame, STARS_PIN) or \
        render(authored, off_frame, ["--no-day-cycle"] + STARS_PIN) or \
        render(cscn_src, plain_sun, STARS_PIN)
    if err:
        print(f"  cycle-cscn  ERROR render failed: {err.strip()[-200:]}")
        failures.append("cycle-cscn")
    else:
        flag_px, _ = compare(a_frame, frozen)
        off_px, _ = compare(off_frame, plain_sun)
        ok = flag_px == 0 and off_px == 0
        print(f"  cycle-cscn   {'PASS' if ok else 'FAIL'}  authored vs flags {flag_px} px "
              f"(want 0), --no-day-cycle back to the scene's own sun {off_px} px (want 0)")
        if not ok:
            failures.append("cycle-cscn")

    # The three rows and CFG_DOUBLE's whole pipeline, which nothing else covers.
    dump = os.path.join(workdir, "cycle_config.json")
    restored = os.path.join(workdir, "cycle_restored.ppm")
    err = render(scene, os.path.join(workdir, "cycle_dump.ppm"),
                 ["--day-cycle", "0", "--time-of-day", repr(CYCLE_QUIESCE_HOUR),
                  "--config-dump", dump] + STARS_PIN)
    if err or not os.path.exists(dump):
        print(f"  cycle-config ERROR dump failed: {(err or 'no file').strip()[-200:]}")
        failures.append("cycle-config")
    else:
        err = render(scene, restored, ["--config", dump] + STARS_PIN)
        if err:
            print(f"  cycle-config ERROR restore failed: {err.strip()[-200:]}")
            failures.append("cycle-config")
        else:
            px, _ = compare(restored, frozen)
            with open(dump) as fh:
                stored = json.load(fh).get("sky", {})
            hour_ok = abs(stored.get("cycle_hour", -1) - CYCLE_QUIESCE_HOUR) < 1e-9
            ok = px == 0 and hour_ok and stored.get("cycle") is True
            print(f"  cycle-config {'PASS' if ok else 'FAIL'}  restored {px} px (want 0), "
                  f"cycle_hour {stored.get('cycle_hour')} round-tripped={hour_ok}")
            if not ok:
                failures.append("cycle-config")

    return failures
