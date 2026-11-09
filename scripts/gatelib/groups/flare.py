"""The flare group: lens flare and chromatic aberration (spec 11.21 / B7)."""

import math
import os

from gatelib.harness import NO_HALOS, RENDER, _run, asset
from gatelib.images import _read_ppm


# --- lens flare and chromatic aberration (spec 11.21 / B7) --------------------

# Frame-plane positions of the two dim marks in assets/models/flare_fixture.gltf, and
# the camera that sees them. Kept here rather than derived from the .gltf: the
# gate's whole claim is that measured separation matches an ANALYTIC prediction,
# and reading the geometry back from the asset under test would let a broken
# generator move the marks and the prediction together.
FLARE_MARKS = {"inner": (1.5, -1.05), "corner": (2.6, -1.9)}
FLARE_MARK_HALF = 0.16
FLARE_EYE_Z = 5.0
FLARE_FOV = 50.0
# Channel separation the shader is documented to produce AT THE CORNER, so the
# gate asserts the unit the flag is denominated in and not just "something moved".
FLARE_CA_PIXELS = 20.0
FLARE_CA_TOL = 0.15
# 8-bit sRGB -> linear, once. The flare composite is a linear gain on linear
# radiance; comparing encoded samples would measure the encode's curve instead.
_SRGB_LIN = [((v / 255.0 + 0.055) / 1.055) ** 2.4 if v / 255.0 > 0.04045
             else (v / 255.0) / 12.92 for v in range(256)]


def _flare_render(workdir, tag, extra):
    out = os.path.join(workdir, f"flare_{tag}.ppm")
    scene = asset("flare_fixture.cscn")
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "-W", "800", "-H", "500",
           "--no-auto-exposure", "-E", "1.0", "-S", out] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    return _read_ppm(out)


def _flare_mark_px(wx, wy, w, h):
    """Frame-plane pixel centre of a mark, and the visible half-extents."""
    half_h = FLARE_EYE_Z * math.tan(math.radians(FLARE_FOV * 0.5))
    half_w = half_h * (w / float(h))
    return ((wx / half_w + 1.0) * 0.5 * w, (1.0 - wy / half_h) * 0.5 * h, half_w)


def _flare_centroid(pix, w, cx, cy, rad, ch):
    """Intensity-weighted centroid of one channel inside a window.

    A radial split moves each channel's image of the mark bodily, so the
    distance between the R and B centroids IS the separation -- no edge-finding,
    and robust to the mark being a few pixels off where the maths says.
    """
    sx = sy = sw = 0.0
    for y in range(int(cy - rad), int(cy + rad) + 1):
        for x in range(int(cx - rad), int(cx + rad) + 1):
            v = pix[(y * w + x) * 3 + ch] / 255.0
            if v < 0.02:  # backdrop, not mark
                continue
            sx += x * v
            sy += y * v
            sw += v
    return (sx / sw, sy / sw) if sw > 0 else None


def _flare_separation(w, h, pix, name):
    px, py, half_w = _flare_mark_px(*FLARE_MARKS[name], w, h)
    rad = (FLARE_MARK_HALF / half_w) * (w * 0.5) + 30  # mark, plus room for the shift
    r = _flare_centroid(pix, w, px, py, rad, 0)
    b = _flare_centroid(pix, w, px, py, rad, 2)
    if r is None or b is None:
        return None, None
    t = math.hypot(px / w - 0.5, py / h - 0.5) / 0.70710678  # 1.0 at the corner
    return math.hypot(r[0] - b[0], r[1] - b[1]), t


def run_flare_gate(workdir):
    fixture = asset("flare_fixture.cscn")
    if not os.path.exists(fixture):
        print("  flare        SKIP  (missing flare_fixture.cscn)")
        return []

    fails = []

    # 1. The composite is LINEAR in strength.
    #
    # Not "off is off": that arm was written first and does not falsify. The C
    # side short-circuits on flare_strength > 0, so --flare 0 never runs the
    # pass and a deliberately broken composite still passed at 0 px. What the
    # default-off claim actually rests on is linearity -- a gain of zero
    # contributes exactly nothing -- and the 18 goldens already assert the
    # frame itself. So assert the gain instead, under a passthrough tonemap so
    # doubling the strength must double the DELTA rather than some compressed
    # image of it.
    #
    # Samples are linearised out of sRGB first, but that does NOT make the
    # assertion exact: Khronos Neutral is not identity here. Its toe expands
    # the darks and its shoulder compresses the brights, and a strength sweep
    # measures the doubling as 2.79 / 2.19 / 2.15 / 1.91 / 1.69 going from 0.1
    # to 4.0 -- the composite is linear and the INSTRUMENT is curved. Undoing
    # that would mean reimplementing the tonemap here, so the band below is the
    # curve's own spread rather than slack.
    #
    # So this arm resolves linear from "ignores strength" (gain 1.0) and from
    # anything squared (4.0). It cannot resolve finer than the tonemap bends.
    lin = {}
    for tag, extra in (("pt0", []), ("pt1", ["--flare", "0.5"]), ("pt2", ["--flare", "1.0"])):
        lin[tag] = _flare_render(workdir, tag, extra)
    if any(v is None for v in lin.values()):
        print("  flare-gain   ERROR while rendering the linearity triple")
        return ["flare-gain"]
    w, h, _ = lin["pt0"]

    def _opp_mean(pix):
        tot = n = 0.0
        for y in range(h // 2, h, 2):
            for x in range(w // 2, w, 2):
                i = (y * w + x) * 3
                tot += (0.2126 * _SRGB_LIN[pix[i]] + 0.7152 * _SRGB_LIN[pix[i + 1]]
                        + 0.0722 * _SRGB_LIN[pix[i + 2]])
                n += 1
        return tot / n

    m0 = _opp_mean(lin["pt0"][2])
    d1 = _opp_mean(lin["pt1"][2]) - m0
    d2 = _opp_mean(lin["pt2"][2]) - m0
    gain = d2 / d1 if d1 > 1e-6 else float("nan")
    ok = d1 > 1e-4 and abs(gain - 2.0) <= 0.40
    print(f"  flare-gain   {'PASS' if ok else 'FAIL'}  delta {d1:.5f} -> {d2:.5f} on 2x "
          f"strength: gain {gain:.3f} (want 2.00 +/- 0.40, tonemap-limited)")
    if not ok:
        fails.append("flare-gain")

    # 2. Ghosts land OPPOSITE the source.
    #
    # The emitter is up and to the LEFT, and ghosts are the source mirrored
    # through frame centre, so the energy owes the lower-right. Asserting the
    # emitter's own half stays put is what separates "these are ghosts" from
    # "the image got brighter", which any additive bug would also pass.
    base = _flare_render(workdir, "base", [])
    on = _flare_render(workdir, "on", ["--flare", "0.15"])
    if base is None or on is None:
        print("  flare-ghost  ERROR while rendering the flare pair")
        return fails + ["flare-ghost"]
    def _half_mean(pix, x0, y0):
        tot = n = 0.0
        for y in range(y0, y0 + h // 2, 2):
            for x in range(x0, x0 + w // 2, 2):
                i = (y * w + x) * 3
                tot += 0.2126 * pix[i] + 0.7152 * pix[i + 1] + 0.0722 * pix[i + 2]
                n += 1
        return tot / n / 255.0
    src_off = _half_mean(base[2], 0, 0)
    src_on = _half_mean(on[2], 0, 0)
    opp_off = _half_mean(base[2], w // 2, h // 2)
    opp_on = _half_mean(on[2], w // 2, h // 2)
    src_rise = (src_on - src_off) / max(src_off, 1e-9)
    opp_rise = (opp_on - opp_off) / max(opp_off, 1e-9)
    ok = opp_rise > 0.30 and src_rise < 0.10
    print(f"  flare-ghost  {'PASS' if ok else 'FAIL'}  opposite half {opp_rise * 100:+.0f}%, "
          f"source half {src_rise * 100:+.0f}% (want > +30% and < +10%)")
    if not ok:
        fails.append("flare-ghost")

    # 3. Aberration separates the channels, by r^2, in pixels at the corner.
    #
    # Two marks at different radii is what makes the FALLOFF falsifiable: with
    # one, "the channels separated" is all a gate can say, and a linear ramp
    # passes that identically. Bloom is off here -- it is added after the split
    # and is not itself shifted, so it would only dilute the centroids.
    ca_off = _flare_render(workdir, "ca_off", NO_HALOS)
    ca_on = _flare_render(workdir, "ca_on",
                          [*NO_HALOS, "--chromatic-aberration", str(FLARE_CA_PIXELS)])
    if ca_off is None or ca_on is None:
        print("  flare-ca     ERROR while rendering the aberration pair")
        return fails + ["flare-ca"]

    problems, meas = [], {}
    for name in ("inner", "corner"):
        sep_off, _ = _flare_separation(ca_off[0], ca_off[1], ca_off[2], name)
        sep_on, t = _flare_separation(ca_on[0], ca_on[1], ca_on[2], name)
        if sep_on is None or sep_off is None:
            problems.append(f"{name} mark not found in frame")
            continue
        meas[name] = sep_on
        if sep_off > 0.5:
            problems.append(f"{name} separated by {sep_off:.2f} px with CA OFF")
        pred = 2.0 * t * t * FLARE_CA_PIXELS  # R-to-B, twice the per-channel shift
        if abs(sep_on - pred) > FLARE_CA_TOL * pred:
            problems.append(f"{name} {sep_on:.2f} px vs {pred:.2f} predicted")

    if len(meas) == 2:
        ratio = meas["corner"] / meas["inner"]
        _, ti = _flare_separation(ca_on[0], ca_on[1], ca_on[2], "inner")
        _, tc = _flare_separation(ca_on[0], ca_on[1], ca_on[2], "corner")
        sq, lin = (tc / ti) ** 2, tc / ti
        if abs(ratio - sq) > abs(ratio - lin):
            problems.append(f"falloff ratio {ratio:.2f} is closer to linear "
                            f"({lin:.2f}) than to r^2 ({sq:.2f})")
        detail = (f"{meas['inner']:.1f} / {meas['corner']:.1f} px, "
                  f"ratio {ratio:.2f} (r^2 predicts {sq:.2f}, linear {lin:.2f})")
    else:
        detail = "; ".join(problems)

    ok = not problems
    print(f"  flare-ca     {'PASS' if ok else 'FAIL'}  {detail if ok else '; '.join(problems)}")
    if not ok:
        fails.append("flare-ca")
    return fails
