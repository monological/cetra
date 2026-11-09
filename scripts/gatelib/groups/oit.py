"""The oit group: order-independent transparency (analytic card stack)."""

import json
import math
import os

from gatelib.harness import NO_HALOS, RENDER, _run, asset
from gatelib.images import _oit_untonemap, _read_ppm


# Order-independent transparency (spec 11.17), measured on assets/oit_cards_fixture.
#
# Twelve emissive cards of one alpha over an opaque backdrop, arranged so a
# horizontal scan crosses regions of exactly 0, 1, ... 12 layers with each step
# adding one card IN FRONT. The correct composite of band b is then the front-to-
# back recursion S_b = C*a + (1-a)*S_(b-1) with no free parameters -- the card
# colours are the authored emissive constants, the backdrop is its own, and the
# tonemap and display gamma are inverted exactly (the fixture keeps every value
# inside the Neutral curve's affine stretch to make that possible).
#
# So this gate does not compare OIT against a stored image or against another
# OIT. It compares it against the arithmetic, and the sorted render is checked
# against the same arithmetic first, which is what earns it the right to be
# called ground truth.
#
# Why a new fixture at all: weighted-blended OIT is EXACT whenever every layer
# carries the same colour, at any weights, because the accumulator divides by its
# own alpha sum. oit_fixture is three near-identical quads, so it cannot separate
# any two OIT schemes and never could. Colour varying WITH depth is what
# discriminates -- see the header of assets/gen_oit_cards_fixture.py.
#
# Mirrors that generator's band layout; the colours and alpha are read from the
# .gltf so the one thing that cannot be checked by inspection cannot drift.
OIT_BAND_LEFT = 0.06
OIT_BAND_RIGHT = 0.94
# Fresnel-boosted opacity: pbr_frag mixes the authored alpha toward 1 by the
# Fresnel term, so the alpha that actually composites is not the authored one.
# The cards face the camera and the bands are sampled on the horizon line, which
# holds (1 - NdotV)^5 below 1e-5 -- so only the normal-incidence F0 survives, and
# the effective alpha is a constant rather than a per-pixel function.
OIT_IOR = 1.5

# What the SORTED render is allowed to deviate from the arithmetic. It measures
# 0.0026, and one 8-bit step is 0.0044 at the brightest band, so this is under
# two quantization steps -- and 25x below the error weighted-blended OIT commits
# on the same frame, which is the number it has to be small against.
OIT_TRUTH_TOL = 0.008
# The inverse half. If the fixture did not separate a real OIT approximation from
# the truth by a wide margin, then a later scheme matching the truth would prove
# nothing about the scheme. Weighted-blended measures 0.077 RMS here.
OIT_DISCRIMINATION_MIN = 0.02
# How much of weighted-blended's error moment weighting has to remove. It removes
# 4.9x of it (0.0773 -> 0.0157 RMS), which is the whole justification for the
# feature, so the bar is a ratio rather than an absolute: an absolute one would
# have to be re-derived every time the fixture's alpha or layer count changed.
#
# 2.5x, comfortably under the measurement and comfortably over "indistinguishable".
# The residual that survives is the method's own conservatism -- the
# reconstruction returns a LOWER bound on the absorbance in front, so twelve
# layers over a 25:1 depth range come out slightly under-occluded no matter how
# the moments are stored.
OIT_MOMENT_GAIN_MIN = 2.5


def _oit_fixture_materials():
    """(background rgb, [(card rgb, alpha)]) straight out of the fixture."""
    path = asset("oit_cards_fixture.gltf")
    with open(path) as f:
        mats = json.load(f)["materials"]
    bg = mats[0]["emissiveFactor"]
    cards = [(m["emissiveFactor"], m["pbrMetallicRoughness"]["baseColorFactor"][3])
             for m in mats[1:]]
    return bg, cards


def _oit_truth(bg, cards):
    """Correct front-to-back composite of every band, in linear HDR."""
    f0 = ((OIT_IOR - 1.0) / (OIT_IOR + 1.0)) ** 2
    out = [list(bg)]
    for color, alpha in cards:
        a = alpha + (1.0 - alpha) * f0
        out.append([color[c] * a + (1.0 - a) * out[-1][c] for c in range(3)])
    return out


def _oit_bands(path, count):
    w, h, pix = _read_ppm(path)
    width = (OIT_BAND_RIGHT - OIT_BAND_LEFT) / (count + 1)
    out = []
    for b in range(count + 1):
        u = OIT_BAND_LEFT + (b + 0.5) * width
        x = max(0, min(w - 1, int(round(u * w))))
        y = h // 2
        o = (y * w + x) * 3
        out.append(_oit_untonemap([pix[o + k] / 255.0 for k in range(3)]))
    return out


def _oit_error(path, truth):
    """(worst channel deviation, RMS) of a render against the arithmetic."""
    meas = _oit_bands(path, len(truth) - 1)
    diffs = [abs(meas[b][c] - truth[b][c]) for b in range(len(truth)) for c in range(3)]
    return max(diffs), math.sqrt(sum(d * d for d in diffs) / len(diffs))


def _oit_render(workdir, tag, extra):
    out = os.path.join(workdir, f"oit_{tag}.ppm")
    scene = asset("oit_cards_fixture.cscn")
    # --no-vignette is load-bearing, not tidiness: the default vignette is on and
    # radial, so it would darken the outer bands by a fraction of the very error
    # being measured. The rest keeps anything that could add to a flat emissive
    # surface out of the frame. --no-dither is the same class and the same
    # reason: the bands are sampled ONE pixel each, so a per-pixel LSB of dither
    # lands on them as a function of framing and doubles the worst deviation.
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "-W", "800", "-H", "500",
           "--no-auto-exposure", "-E", "1.0", "--no-vignette", *NO_HALOS,
           "--no-ssao", "--no-ssr", "--no-dither", "-S", out] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    return out


def run_oit_gate(workdir):
    fixture = asset("oit_cards_fixture.cscn")
    if not os.path.exists(fixture):
        print("  oit-cards    SKIP  (missing oit_cards_fixture.cscn)")
        return []
    bg, cards = _oit_fixture_materials()
    truth = _oit_truth(bg, cards)

    # Every arm names its weighting explicitly. --oit alone would inherit
    # whichever one is default, so the weighted arm has to say --no-oit-moments
    # or it silently measures the moment path against itself.
    sorted_frame = _oit_render(workdir, "sorted", ["--no-oit"])
    weighted = _oit_render(workdir, "weighted", ["--oit", "--no-oit-moments"])
    moments = _oit_render(workdir, "moments", ["--oit-moments"])
    if sorted_frame is None or weighted is None or moments is None:
        print("  oit-cards    ERROR while rendering the card stack")
        return ["oit-cards"]

    fails = []
    worst, rms = _oit_error(sorted_frame, truth)
    ok = worst <= OIT_TRUTH_TOL
    print(f"  oit-sorted   {'PASS' if ok else 'FAIL'}  back-to-front vs arithmetic: "
          f"worst {worst:.5f} rms {rms:.5f} (want worst <= {OIT_TRUTH_TOL})")
    if not ok:
        fails.append("oit-cards")

    w_worst, w_rms = _oit_error(weighted, truth)
    ok = w_rms >= OIT_DISCRIMINATION_MIN
    print(f"  oit-weighted {'PASS' if ok else 'FAIL'}  weighted-blended vs arithmetic: "
          f"worst {w_worst:.5f} rms {w_rms:.5f} "
          f"(want rms >= {OIT_DISCRIMINATION_MIN}, else the fixture cannot discriminate)")
    if not ok:
        fails.append("oit-cards")

    m_worst, m_rms = _oit_error(moments, truth)
    gain = w_rms / m_rms if m_rms > 0 else float("inf")
    ok = gain >= OIT_MOMENT_GAIN_MIN
    print(f"  oit-moments  {'PASS' if ok else 'FAIL'}  moment-weighted vs arithmetic: "
          f"worst {m_worst:.5f} rms {m_rms:.5f}, {gain:.2f}x closer than weighted-blended "
          f"(want >= {OIT_MOMENT_GAIN_MIN}x)")
    if not ok:
        fails.append("oit-cards")
    return fails
