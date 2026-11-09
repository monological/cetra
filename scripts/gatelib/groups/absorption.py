"""The absorption group: volume absorption (path length and channel selectivity, spec 11.32)."""

import os

from gatelib.harness import NO_HALOS, asset, render
from gatelib.images import ABSORB_GRID, _absorb_box_rgb, _read_ppm


# KHR_materials_volume absorption (spec 11.32), on assets/absorption_fixture.
#
# Three transmissive panels over ONE uniform emissive backdrop, carrying the same
# attenuationColor (1.0, 0.35, 0.35) and attenuationDistance 0.3, differing only
# in volume thickness: 0.0, 0.3, 0.6. Thickness is the path length Beer-Lambert
# integrates over, so the transmitted green:red ratio should fall as it grows
# while red passes unabsorbed and stays put.
#
# Every arm here is a RATIO or an equality between regions of one frame, never a
# stored level. The tonemap is a monotone curve the fixture does not control --
# measured, PBR Neutral moves the middle panel's ratio from an analytic 0.35 to
# 0.30 -- and passthrough is unreachable from the CLI by design (render_args.h
# notes the sentinel deliberately coincides with it). A monotone curve preserves
# ordering and maps equal inputs to equal outputs, which is exactly what these
# three assertions rest on and all a correct implementation needs to satisfy.
ABSORB_FIXTURE = "absorption_fixture.cscn"
# Boxes are fractions of the frame, centred on each panel and well inside it, so
# the refraction bend at the panel edges never enters the read.
ABSORB_BOXES = {
    "t000": (0.2335, 0.44, 0.3335, 0.56),
    "t030": (0.4500, 0.44, 0.5500, 0.56),
    "t060": (0.6665, 0.44, 0.7665, 0.56),
}
# Bare backdrop seen through the GAP between the middle and right panels, at the
# same frame rows as the panel boxes so the vignette and the tone curve cancel out
# of the comparison. (A box in the frame corner does not: measured 0.1018 there
# against 0.5271 on a panel, which is the vignette, not absorption.)
#
# This is the presence floor. The backdrop is ONE uniform emissive grey, so if the
# panels fail to load or the transmissive lane is skipped, every panel box reads that
# same colour -- and absorb-thin and absorb-red are then satisfied perfectly by an
# empty frame. That is the hole run_mask_gate records at MASK_LIT_FLOOR. Green is the
# channel to compare: unabsorbed through the gap, heavily absorbed through the
# thickest panel, so the ratio is large and unambiguous.
ABSORB_GAP_BOX = (0.598, 0.44, 0.626, 0.56)
ABSORB_PRESENCE_MIN = 3.0
# Zero path length must be an exact identity, so this is quantization-tight rather
# than a tolerance band. Measured 1.0000.
ABSORB_THIN_EPS = 0.02
# Red carries attenuationColor 1.0 and must survive every thickness untouched.
# Relative, because the three panels share one backdrop and one Fresnel term.
# Measured spread 0.0000 across the three (0.5271 each).
ABSORB_RED_TOL = 0.02
# Each step of 0.3 in thickness should cost the green channel a large factor; 0.35
# per step analytically, and measured 3.32x then 4.47x (the tonemap steepens it), so
# 1.6x is a wide floor that still fails absorption applied without the thickness.
ABSORB_STEP_MIN = 1.6


def run_absorption_gate(workdir):
    """Volume absorption scales with path length, and only in the tinted channels.

    Four arms, in the order they run, none implying the others:

      absorb-panels the panels are IN the frame: bare backdrop in the gap reads bright
                   against the thickest panel beside it. Presence first, because every
                   arm below is an equality or a ratio and an empty frame satisfies two
                   of the three.
      absorb-thin  the zero-thickness panel's green:red is 1.0. Fails an
                   implementation that absorbs by a constant instead of over the
                   authored path -- the failure mode where thin glass, a legal
                   authoring combination, gets tinted for free.
      absorb-red   red is equal across all three panels. Fails an implementation
                   that absorbs luminance rather than per channel, which would
                   darken every panel and still produce a falling ratio.
      absorb-ramp  green:red falls by at least ABSORB_STEP_MIN per thickness
                   step. Fails absorption never being applied at all, which
                   leaves all three ratios at 1.0 and passes both arms above.

    Dither off and bloom off: the read is a channel ratio on a low-radiance
    surface, where a +/-1 LSB and a bright-pass bleed are both large.
    """
    scene = asset(ABSORB_FIXTURE)
    if not os.path.exists(scene):
        print(f"  absorb-thin  SKIP  ({ABSORB_FIXTURE} not present)")
        return []

    out = os.path.join(workdir, "absorption.ppm")
    err = render(scene, out, ["--no-auto-exposure", "-E", "1.0", *NO_HALOS, "--no-dither"])
    if err:
        print(f"  absorb-thin  ERROR render failed: {err.strip()[-200:]}")
        return ["absorb-thin"]

    failures = []
    w, h, pix = _read_ppm(out)
    rgb = {k: _absorb_box_rgb(pix, w, h, b, ABSORB_GRID) for k, b in ABSORB_BOXES.items()}
    ratio = {k: (v[1] / v[0] if v[0] > 1e-6 else float("nan")) for k, v in rgb.items()}

    # Presence first: every arm below is an equality or a ratio, and an empty frame
    # satisfies two of the three.
    gap = _absorb_box_rgb(pix, w, h, ABSORB_GAP_BOX, ABSORB_GRID)
    presence = gap[1] / max(rgb["t060"][1], 1e-6)
    ok = presence >= ABSORB_PRESENCE_MIN
    print(f"  absorb-panels {'PASS' if ok else 'FAIL'}  gap G={gap[1]:.4f} vs "
          f"thickest panel G={rgb['t060'][1]:.4f}, ratio={presence:.2f} "
          f"want >={ABSORB_PRESENCE_MIN}")
    if not ok:
        failures.append("absorb-panels")

    ok = abs(ratio["t000"] - 1.0) <= ABSORB_THIN_EPS
    print(f"  absorb-thin  {'PASS' if ok else 'FAIL'}  "
          f"G/R={ratio['t000']:.4f} want 1.0 +/-{ABSORB_THIN_EPS}")
    if not ok:
        failures.append("absorb-thin")

    reds = [rgb[k][0] for k in ("t000", "t030", "t060")]
    spread = (max(reds) - min(reds)) / max(max(reds), 1e-6)
    ok = spread <= ABSORB_RED_TOL
    print(f"  absorb-red   {'PASS' if ok else 'FAIL'}  "
          f"R={reds[0]:.4f}/{reds[1]:.4f}/{reds[2]:.4f} spread={spread:.4f} "
          f"want <={ABSORB_RED_TOL}")
    if not ok:
        failures.append("absorb-red")

    steps = [ratio["t000"] / max(ratio["t030"], 1e-6),
             ratio["t030"] / max(ratio["t060"], 1e-6)]
    ok = all(s >= ABSORB_STEP_MIN for s in steps)
    print(f"  absorb-ramp  {'PASS' if ok else 'FAIL'}  "
          f"G/R={ratio['t000']:.4f}/{ratio['t030']:.4f}/{ratio['t060']:.4f} "
          f"steps={steps[0]:.2f}x,{steps[1]:.2f}x want >={ABSORB_STEP_MIN}x")
    if not ok:
        failures.append("absorb-ramp")

    return failures
