"""The overcast group: the overcast deck: CIE dome, sun taken away, fog and IBL (spec 13.7)."""

import json
import math
import os

from gatelib.harness import NO_HALOS, _probe_render, asset, render
from gatelib.images import _DISPLAY_TO_LINEAR, _box_luma_dense, _read_ppm, compare
from gatelib.scenes import _cscn_camera, _projector, cscn_copy
from gatelib.sky import CLOUDSHADOW_FRAMES, STARS_FIXTURE, STARS_PIN, _sliced_vs_atomic


# --- overcast (spec 13.7): the CIE overcast dome, and the sky's radiance scale ---------
#
# The frame arms run on the fixture's own scene file, because they turn a pixel ROW into
# a view elevation and need the camera it states; the flag arms use the glTF like the
# stars and floor groups. Every radiance read goes through the linear curve with the
# halos, the dither and the vignette off, decoded by displayEncode's inverse, so a pixel
# is OVERCAST_EXPOSURE times a radiance and a level can be held against the probe's Lz.
OVERCAST_SCENE = "aerial_fixture.cscn"
OVERCAST_EXPOSURE = 8.0
OVERCAST_LINEAR = ["--tonemap", "linear", *NO_HALOS, "--no-dither", "--no-vignette",
                   "-E", str(OVERCAST_EXPOSURE)]
# The four the dome arm holds against the twin: two days, a low sun, and one inside
# civil twilight, where only the faded horizon constant is left.
OVERCAST_DOME_ELEVS = ["35", "20", "4", "-3"]
OVERCAST_DOME_TOL = 1e-5
# Two sky rows well clear of the ridges, which top out near 6 degrees here.
OVERCAST_SHAPE_ELEVS = (26.0, 9.0)
OVERCAST_SHAPE_TOL = 0.03
# Measured 1 px at o=1 and 478,559 at o=0.5, of 480,000.
OVERCAST_AZIMUTH_MAX_PX = 50
OVERCAST_AZIMUTH_CONTROL_MIN_PX = 100000
# The flat foreground, bottom of frame, where the aerial transmittance is ~0.95.
OVERCAST_GROUND_BAND = (0.30, 0.90, 0.70, 0.98)
OVERCAST_GROUND_TOL = 0.10
# The far-left world edge: the ground plane's last rows and the sky directly over them.
OVERCAST_EDGE_GROUND = (0.005, 0.62, 0.045, 0.66)
OVERCAST_EDGE_MIN = 0.2
# Dense enough that the far bands are all medium: measured 0.998 of the zenith there.
OVERCAST_FOG_DENSITY = "5e-4"
OVERCAST_FOG_BAND = (0.05, 0.55, 0.95, 0.70)
OVERCAST_FOG_TOL = 0.02
# The twin of SKY_PHOTOMETRIC_SCALE: SKY_SUN_KLUX klux over SKY_SUN_ILLUMINANCE.
OVERCAST_PHOTOMETRIC = 127.5 * 1000.0 / 3.0
# Nearly overcast: clear 5e-4 lets the sun through at half the cast floor, so a light that
# carried the scale in its fade rather than its level would still cast.
OVERCAST_NEARLY = "0.9995"


def _overcast_zenith(sun_el_deg):
    """The Python twin of the dome's Lz (sky.c): Krochmann's zenith on the relative scale.

    (8.6 sin h + 0.123 day) kcd/m^2 over an unattenuated sun's 127.5 klux, per unit of
    SKY_SUN_ILLUMINANCE (3), with `day` the civil-twilight ramp's complement --
    glm_smoothstep(-8, 3, h) written out.
    """
    y = math.sin(math.radians(sun_el_deg))
    t = min(max((sun_el_deg + 8.0) / 11.0, 0.0), 1.0)
    day = t * t * (3.0 - 2.0 * t)
    return 3.0 / 127.5 * (8.6 * max(y, 0.0) + 0.123 * day)


def _sky_probe(scene, extra):
    """--sky-probe's rows keyed by tag, every value a float (a list for zenith). A tag the
    run did not print reads as an empty row, so a missing value reaches the arm as NaN."""
    rows, _ = _probe_render(scene, "--sky-probe", "sky-probe", frames=2, extra=extra)
    got = {kind: {} for kind in ("sun", "dome", "sun-light", "moon-light")}
    for rec in rows:
        vals = {}
        for k, v in rec.items():
            if k != "kind":
                vals[k] = [float(p) for p in v.split(",")] if "," in v else float(v)
        got[rec.get("kind")] = vals
    return got


def _overcast_row_of(cam, el_deg, w, h):
    """The fractional row where the frame's centre column looks out at `el_deg`: a point
    along that direction, at the camera's own azimuth, through the shared projector."""
    eye, target = cam["eye"], cam["target"]
    fx, fz = target[0] - eye[0], target[2] - eye[2]
    m = math.hypot(fx, fz)
    el = math.radians(el_deg)
    d = (fx / m * math.cos(el), math.sin(el), fz / m * math.cos(el))
    _, y = _projector(cam, w, h)(tuple(e + 1000.0 * c for e, c in zip(eye, d)))
    return y / h


def run_overcast_gate(workdir):
    """The overcast (spec 13.7): the CIE dome, the sun it takes away, and the scale.

      overcast-dome     --sky-probe's Lz at four elevations against this file's twin of
                        Krochmann's fit, and the zenith radiance at o=1 is (7/9) Lz --
                        the dome's horizontal irradiance over pi, which is what the fog
                        and the water read it as. On the photometric scale the same Lz
                        is SKY_PHOTOMETRIC_SCALE times the twin, and the probe reports
                        the scale it took.
      overcast-shape    the sky at two elevations is E x Lz (1 + 2 sin h)/3, a LEVEL
                        and not a ratio, so a dome missing its /3 fails as surely as
                        one with the wrong gradient.
      overcast-azimuth  at o=1 the frame does not know where the sun is (az 40 against
                        220), while at o=0.5 it does. The frame-wide statement that no
                        sun-driven term survived the overcast -- the clear LUT, the sun
                        light and the aerial's clear half all live on one side of it.
      overcast-sun      the sun LIGHT, from the probe: half at o=0.5, zero and not
                        casting at o=1, against a fixture whose sun casts at o=0. And
                        NOT casting at o=0.9995 on the photometric scale either: the
                        cast floor is a fraction of the light's full level, which is
                        where the scale has to be for the floor to scale with it.
      overcast-ground   the flat foreground's IBL lift (on minus --ibl-intensity 0) is
                        its albedo x (7/9) Lz x E. With no sun, that lift is the dome
                        arriving through the env cube, the path this feature exists for.
      overcast-aerial   with the IBL off nothing lights the terrain, so the world edge
                        shows the aerial's dome term alone, (1 - T) Lz/3: above zero,
                        and never above the horizon radiance it converges to. A term
                        reading the zenith or the floor constant lands over the bar.
      overcast-fog      dense fog at o=1 converges on the probe's zenith radiance -- the
                        fog ambient the CPU march hands the medium.
      overcast-slice    the cycle's sliced re-bake at o=0.5 is 0 px from the atomic one.
                        Under a still sun the latched and live suns agree, so a slicer
                        reading the live one is NOT visible here; what is, is any path
                        that bakes the overcast differently from the atomic bake.
      overcast-cscn     the authored environment.overcast IS the flag path (0 px), and
                        --overcast 0 beats a file that asked for an overcast, landing on
                        the plain sky at 0 px -- which is also the default path's
                        identity, since both legs then carry an exact clear weight of 1.

    The three frame LEVELS -- shape, ground, fog -- are held against the probe's Lz from
    the same fixture, which is what ties the C helper to the shaders: the dome is
    computed once on the CPU and applied in four programs, and a program that applied it
    differently would still render a plausible grey sky.
    """
    scene = asset(STARS_FIXTURE)
    cscn = asset(OVERCAST_SCENE)
    if not os.path.exists(scene) or not os.path.exists(cscn):
        print(f"  overcast-dome SKIP  ({STARS_FIXTURE} not present)")
        return []
    failures = []

    probes = {el: _sky_probe(scene, ["--overcast", "1", "--sun-elevation", el] + STARS_PIN)
              for el in OVERCAST_DOME_ELEVS}
    photometric = _sky_probe(scene, ["--overcast", "1", "--sun-elevation", "20",
                                     "--sky-scale", "photometric"] + STARS_PIN)
    lz = {el: got["dome"].get("lz", float("nan")) for el, got in probes.items()}
    errs = []
    for el, got in probes.items():
        want = _overcast_zenith(float(el))
        errs.append(abs(lz[el] - want) / want)
        errs += [abs(z - 7.0 / 9.0 * want) / (7.0 / 9.0 * want)
                 for z in got["dome"].get("zenith", [float("nan")] * 3)]
    pho_want = OVERCAST_PHOTOMETRIC * _overcast_zenith(20.0)
    pho_lz = photometric["dome"].get("lz", float("nan"))
    pho_scale = photometric["sun"].get("scale", float("nan"))
    errs += [abs(pho_lz - pho_want) / pho_want,
             abs(pho_scale - OVERCAST_PHOTOMETRIC) / OVERCAST_PHOTOMETRIC]
    # max() passes a NaN over silently, and a missing row is exactly a NaN here.
    worst = float("inf") if any(math.isnan(e) for e in errs) else max(errs)
    ok = worst <= OVERCAST_DOME_TOL
    detail = " ".join(f"{el}:{v:.6g}" for el, v in lz.items())
    print(f"  overcast-dome {'PASS' if ok else 'FAIL'}  Lz {detail}, photometric {pho_lz:.6g} "
          f"at scale {pho_scale:.6g}; worst relative error {worst:.2e} against the twin, "
          f"(7/9) Lz and the scale (want <={OVERCAST_DOME_TOL})")
    if not ok:
        failures.append("overcast-dome")

    def frame(path):
        w, h, pix = _read_ppm(path)
        return lambda box: _box_luma_dense(pix, w, h, box, _DISPLAY_TO_LINEAR)

    full = os.path.join(workdir, "overcast_full.ppm")
    err = render(cscn, full, ["--overcast", "1"] + OVERCAST_LINEAR)
    if err:
        print(f"  overcast-shape ERROR render failed: {err.strip()[-200:]}")
        return failures + ["overcast-shape"]
    w, h, _ = _read_ppm(full)
    full_band = frame(full)
    cam = _cscn_camera(OVERCAST_SCENE)
    worst, detail = 0.0, []
    for el in OVERCAST_SHAPE_ELEVS:
        row = _overcast_row_of(cam, el, w, h)
        got = full_band((0.4, row - 2.0 / h, 0.6, row + 3.0 / h))
        want = OVERCAST_EXPOSURE * lz["20"] * (1.0 + 2.0 * math.sin(math.radians(el))) / 3.0
        worst = max(worst, abs(got / want - 1.0))
        detail.append(f"{el:g}deg {got:.4f}/{want:.4f}")
    ok = worst <= OVERCAST_SHAPE_TOL
    print(f"  overcast-shape {'PASS' if ok else 'FAIL'}  sky against E Lz (1+2 sin h)/3: "
          f"{', '.join(detail)} (want within {OVERCAST_SHAPE_TOL:.0%})")
    if not ok:
        failures.append("overcast-shape")

    turned = os.path.join(workdir, "overcast_turned.ppm")
    half = os.path.join(workdir, "overcast_half.ppm")
    half_turned = os.path.join(workdir, "overcast_half_turned.ppm")
    err = render(cscn, turned, ["--overcast", "1", "--sun-azimuth", "220"] + OVERCAST_LINEAR) or \
        render(cscn, half, ["--overcast", "0.5"] + OVERCAST_LINEAR) or \
        render(cscn, half_turned, ["--overcast", "0.5", "--sun-azimuth", "220"] + OVERCAST_LINEAR)
    if err:
        print(f"  overcast-azimuth ERROR render failed: {err.strip()[-200:]}")
        failures.append("overcast-azimuth")
    else:
        full_px, _ = compare(full, turned)
        half_px, _ = compare(half, half_turned)
        ok = full_px <= OVERCAST_AZIMUTH_MAX_PX and half_px >= OVERCAST_AZIMUTH_CONTROL_MIN_PX
        print(f"  overcast-azimuth {'PASS' if ok else 'FAIL'}  sun turned 180 degrees: {full_px} px "
              f"at o=1 (want <={OVERCAST_AZIMUTH_MAX_PX}), {half_px} px at o=0.5 (want "
              f">={OVERCAST_AZIMUTH_CONTROL_MIN_PX}: the fixture can see the sun)")
        if not ok:
            failures.append("overcast-azimuth")

    sun = {o: _sky_probe(scene, ["--overcast", o, "--sun-elevation", "20"] + STARS_PIN)
           ["sun-light"] for o in ("0", "0.5")}
    sun["1"] = probes["20"]["sun-light"]
    nearly = _sky_probe(scene, ["--overcast", OVERCAST_NEARLY, "--sun-elevation", "20",
                                "--sky-scale", "photometric"] + STARS_PIN)["sun-light"]
    i0 = sun["0"].get("intensity", 0.0)
    ok = (i0 > 0.0 and sun["0"].get("casts") == 1.0
          and abs(sun["0.5"].get("intensity", -1.0) - 0.5 * i0) <= 1e-6 * i0
          and sun["1"].get("intensity") == 0.0 and sun["1"].get("casts") == 0.0
          and nearly.get("intensity", 0.0) > 0.0 and nearly.get("casts") == 0.0)
    print(f"  overcast-sun {'PASS' if ok else 'FAIL'}  intensity {i0:g} / "
          f"{sun['0.5'].get('intensity')} / {sun['1'].get('intensity')} at o=0/0.5/1, casts "
          f"{sun['0'].get('casts')}/{sun['1'].get('casts')} at 0/1 (want half, then 0 and "
          f"not casting); photometric at o={OVERCAST_NEARLY} lit at "
          f"{nearly.get('intensity')} and casts {nearly.get('casts')} (want lit, not casting)")
    if not ok:
        failures.append("overcast-sun")

    dark = os.path.join(workdir, "overcast_noibl.ppm")
    err = render(cscn, dark, ["--overcast", "1", "--ibl-intensity", "0"] + OVERCAST_LINEAR)
    if err:
        print(f"  overcast-ground ERROR render failed: {err.strip()[-200:]}")
        return failures + ["overcast-ground"]
    dark_band = frame(dark)
    with open(scene) as fh:
        mats = {m["name"]: m for m in json.load(fh)["materials"]}
    albedo = sum(mats["aerial_ground"]["pbrMetallicRoughness"]["baseColorFactor"][:3]) / 3.0
    lift = full_band(OVERCAST_GROUND_BAND) - dark_band(OVERCAST_GROUND_BAND)
    want = albedo * 7.0 / 9.0 * lz["20"] * OVERCAST_EXPOSURE
    ok = abs(lift / want - 1.0) <= OVERCAST_GROUND_TOL
    print(f"  overcast-ground {'PASS' if ok else 'FAIL'}  foreground IBL lift {lift:.4f} against "
          f"albedo x (7/9) Lz x E {want:.4f} (want within {OVERCAST_GROUND_TOL:.0%})")
    if not ok:
        failures.append("overcast-ground")

    edge = dark_band(OVERCAST_EDGE_GROUND) / (OVERCAST_EXPOSURE * lz["20"] / 3.0)
    ok = OVERCAST_EDGE_MIN <= edge <= 1.0
    print(f"  overcast-aerial {'PASS' if ok else 'FAIL'}  unlit world edge {edge:.3f} of the "
          f"horizon radiance Lz/3 (want {OVERCAST_EDGE_MIN}..1: the aerial's (1 - T) share)")
    if not ok:
        failures.append("overcast-aerial")

    fog = os.path.join(workdir, "overcast_fog.ppm")
    err = render(cscn, fog, ["--overcast", "1", "--fog", "--fog-density", OVERCAST_FOG_DENSITY]
                 + OVERCAST_LINEAR, frames=CLOUDSHADOW_FRAMES)
    if err:
        print(f"  overcast-fog ERROR render failed: {err.strip()[-200:]}")
        failures.append("overcast-fog")
    else:
        zen = probes["20"]["dome"].get("zenith", [float("nan")])[0]
        ratio = frame(fog)(OVERCAST_FOG_BAND) / (OVERCAST_EXPOSURE * zen)
        ok = abs(ratio - 1.0) <= OVERCAST_FOG_TOL
        print(f"  overcast-fog {'PASS' if ok else 'FAIL'}  far bands under dense fog {ratio:.4f} "
              f"of E x the probe's zenith (want within {OVERCAST_FOG_TOL:.0%})")
        if not ok:
            failures.append("overcast-fog")

    px, err = _sliced_vs_atomic(workdir, "overcast", scene,
                                ["--overcast", "0.5", "--sun-elevation", "12", "--sun-azimuth",
                                 "40"] + STARS_PIN)
    if err:
        print(f"  overcast-slice ERROR render failed: {err.strip()[-200:]}")
        failures.append("overcast-slice")
    else:
        ok = px == 0
        print(f"  overcast-slice {'PASS' if ok else 'FAIL'}  sliced re-bake vs atomic {px} px "
              f"(want 0)")
        if not ok:
            failures.append("overcast-slice")

    # The flag leg IS the azimuth arm's `half`, the same overcast through the same curve,
    # so the file is authored at that amount rather than rendering it twice.
    authored = os.path.join(workdir, "overcast_cscn.cscn")
    cscn_copy(cscn, authored, lambda d: d["environment"].update({"overcast": 0.5}))
    a_file = os.path.join(workdir, "overcast_cscn_file.ppm")
    a_off = os.path.join(workdir, "overcast_cscn_off.ppm")
    a_plain = os.path.join(workdir, "overcast_cscn_plain.ppm")
    err = (None if os.path.exists(half) else
           render(cscn, half, ["--overcast", "0.5"] + OVERCAST_LINEAR)) or \
        render(authored, a_file, OVERCAST_LINEAR) or \
        render(authored, a_off, ["--overcast", "0"] + OVERCAST_LINEAR) or \
        render(cscn, a_plain, OVERCAST_LINEAR)
    if err:
        print(f"  overcast-cscn ERROR render failed: {err.strip()[-200:]}")
        return failures + ["overcast-cscn"]
    flag_px, _ = compare(a_file, half)
    off_px, _ = compare(a_off, a_plain)
    moved, _ = compare(a_file, a_plain)
    ok = flag_px == 0 and off_px == 0 and moved > 0
    print(f"  overcast-cscn {'PASS' if ok else 'FAIL'}  authored vs flag {flag_px} px (want 0), "
          f"--overcast 0 over the file {off_px} px (want 0: the plain sky), the overcast itself "
          f"{moved} px (want >0)")
    if not ok:
        failures.append("overcast-cscn")

    return failures
