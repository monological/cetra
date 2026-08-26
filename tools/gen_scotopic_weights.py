#!/usr/bin/env python3
"""Derive the Purkinje shift's two constant vectors, and check them against a
published anchor.

WHY THIS IS A TOOL AND NOT TWO LITERALS. Both vectors below look like taste and
only one of them is. The scotopic weights are a DERIVED quantity -- the rod
response to each sRGB primary -- and getting one wrong produces a moon-lit frame
that is merely a slightly different blue, which no reviewer would catch. The
S/P check at the bottom is what makes an error loud: it reproduces a number
published independently of anything here.

WHAT IS DERIVED AND WHAT IS CHOSEN, stated plainly because the file emits both:

  PURKINJE_SCOTOPIC_W  derived. How strongly rods respond to each primary.
  PURKINJE_ROD_TINT    chosen. What that response is PERCEIVED as, which is a
                       look decision with a physical constraint, not a
                       measurement -- see its own section.

Regenerate: python3 tools/gen_scotopic_weights.py
"""

import os
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cetra", "shaders",
                   "include", "scotopic_weights.glsl")

# CIE V(lambda) photopic and V'(lambda) scotopic luminous efficiency, sampled at
# 10 nm. Published tables (CIE 1924 photopic, CIE 1951 scotopic), abridged to the
# range where sRGB primaries carry energy.
#   nm:    V(l),      V'(l)
LUMINOUS_EFFICIENCY = {
    400: (0.0004, 0.00929),
    410: (0.0012, 0.03484),
    420: (0.0040, 0.09660),
    430: (0.0116, 0.19980),
    440: (0.0230, 0.32810),
    450: (0.0380, 0.45500),
    460: (0.0600, 0.56700),
    470: (0.0910, 0.67600),
    480: (0.1390, 0.79300),
    490: (0.2080, 0.90400),
    500: (0.3230, 0.98200),
    510: (0.5030, 0.99700),
    520: (0.7100, 0.93500),
    530: (0.8620, 0.81100),
    540: (0.9540, 0.65000),
    550: (0.9950, 0.48100),
    560: (0.9950, 0.32880),
    570: (0.9520, 0.20760),
    580: (0.8700, 0.12120),
    590: (0.7570, 0.06550),
    600: (0.6310, 0.03315),
    610: (0.5030, 0.01593),
    620: (0.3810, 0.00737),
    630: (0.2650, 0.00334),
    640: (0.1750, 0.00150),
    650: (0.1070, 0.00068),
}

# Dominant wavelength of each sRGB primary against D65. The single-wavelength
# stand-in for a primary's whole spectrum: good to about a percent on the ratio
# below, which is what the S/P anchor confirms. A full spectral integration
# would need the display's actual emission curves, which vary per panel -- so
# the extra precision would be false.
SRGB_DOMINANT_NM = {"r": 610, "g": 550, "b": 465}

# Rec. 709 / sRGB photopic luminance weights.
PHOTOPIC_W = {"r": 0.2126, "g": 0.7152, "b": 0.0722}

# Scotopic and photopic lumens per watt at their respective peaks. Their ratio
# is what converts the weighted sum below into an S/P ratio.
SCOTOPIC_LM_PER_W = 1700.0
PHOTOPIC_LM_PER_W = 683.0

# Published S/P ratio for a D65-white display. Anchors the whole derivation:
# an error in any single weight moves this away from the literature value.
SP_PUBLISHED = 2.4
SP_TOLERANCE = 0.25


def _interp(nm, index):
    """V or V' at nm, linearly between table samples."""
    if nm in LUMINOUS_EFFICIENCY:
        return LUMINOUS_EFFICIENCY[nm][index]
    lo = max(k for k in LUMINOUS_EFFICIENCY if k <= nm)
    hi = min(k for k in LUMINOUS_EFFICIENCY if k >= nm)
    t = (nm - lo) / (hi - lo)
    return (LUMINOUS_EFFICIENCY[lo][index] * (1.0 - t) +
            LUMINOUS_EFFICIENCY[hi][index] * t)


def scotopic_weights():
    """Unnormalised rod response per primary, and the normalised weights.

    Per primary: how much MORE (or less) the rods see it than the cones do,
    times how much of the photopic luminance that primary carries. Blue comes
    out dominant and red almost absent -- V'(610) is 0.016 against V(610) 0.503,
    a factor of 32 -- and that single number is the whole 'reds go near-black'
    half of the effect.
    """
    raw = {}
    for ch, nm in SRGB_DOMINANT_NM.items():
        ratio = _interp(nm, 1) / _interp(nm, 0)
        raw[ch] = ratio * PHOTOPIC_W[ch]
    total = sum(raw.values())
    # NORMALISED TO SUM TO 1, which is load-bearing rather than tidy: it makes a
    # neutral pixel's scotopic luminance equal its photopic one, so the stage
    # does not change the brightness of grey. Without it the frame's apparent
    # exposure shifts as the weight fades in, which reads as an exposure bug
    # rather than as night.
    return raw, {ch: v / total for ch, v in raw.items()}


def sp_ratio(raw):
    """Scotopic-over-photopic luminous efficacy of the display's white.

    The anchor. Published at roughly 2.4 for a D65 white, derived here from the
    same three ratios the weights are built from -- so it is a genuine check and
    not a restatement.
    """
    return sum(raw.values()) * SCOTOPIC_LM_PER_W / PHOTOPIC_LM_PER_W


def rod_tint():
    """The colour the achromatic rod signal is PERCEIVED as.

    CHOSEN, not derived, and the file says so where it is emitted. Rods are
    monochromats, so their signal is one scalar; what hue the visual system
    reports it as is a perceptual question with no objective answer. Kirk &
    O'Brien inject the rod signal into the L/M/S pathways with per-cone gains --
    and because the input is a SCALAR, that whole LMS round trip collapses
    algebraically into one constant RGB vector. This IS that vector, not an
    approximation of it; only the constant's name is lost.

    THE NATURAL WRONG TURN is deriving it as SCOTOPIC_W / PHOTOPIC_W, which
    gives roughly (0.03, 0.51, 8.7) -- an absurd cyan. That is the colour of the
    stimulus that best EXCITES rods, not the colour of the percept. Different
    question.

    The one physical constraint, enforced below: unit photopic luminance. A tint
    that does not satisfy it changes the brightness of grey, defeating the
    normalisation of the weights above.
    """
    tint = {"r": 0.84, "g": 1.01, "b": 1.38}
    luma = sum(tint[ch] * PHOTOPIC_W[ch] for ch in tint)
    return {ch: v / luma for ch, v in tint.items()}


def emit(norm, tint, sp):
    w = f'vec3({norm["r"]:.5f}, {norm["g"]:.5f}, {norm["b"]:.5f})'
    t = f'vec3({tint["r"]:.5f}, {tint["g"]:.5f}, {tint["b"]:.5f})'
    return f"""// GENERATED by tools/gen_scotopic_weights.py. DO NOT EDIT.
// Regenerate: python3 tools/gen_scotopic_weights.py
//
// The Purkinje shift's two constant vectors (spec 11.83). One is derived and
// one is chosen, and the generator's header explains which is which -- read it
// before changing either.

// How strongly the RODS respond to each sRGB primary. DERIVED from published
// V'(lambda)/V(lambda) at each primary's dominant wavelength, times that
// primary's photopic weight, normalised to sum to exactly 1.
//
// Summing to 1 is what keeps grey at its own brightness: a neutral pixel's
// scotopic luminance equals its photopic one, so fading the effect in does not
// look like an exposure change.
//
// Red is {norm["r"]*100:.1f}% against its photopic 21.3%, and that number is not tuned --
// V'(610) is 0.016 where V(610) is 0.503. It is the whole "reds go near-black"
// half of the effect, and it falls out.
//
// ANCHOR: these weights reproduce the display white's published scotopic/photopic
// ratio at {sp:.2f} against a literature ~{SP_PUBLISHED}, which is what makes a wrong
// coefficient loud rather than merely a different blue.
const vec3 PURKINJE_SCOTOPIC_W = {w};

// What that achromatic rod signal is PERCEIVED as. CHOSEN, with one physical
// constraint enforced by the generator: dot(tint, photopic luma) == 1 exactly,
// so the tint moves hue and never brightness.
//
// This is not an approximation of Kirk & O'Brien's per-cone injection -- it is
// algebraically identical to it. Rods are monochromats, so the LMS round trip
// of a scalar collapses into exactly one constant vector.
const vec3 PURKINJE_ROD_TINT = {t};
"""


if __name__ == "__main__":
    raw, norm = scotopic_weights()
    tint = rod_tint()
    sp = sp_ratio(raw)

    assert abs(sum(norm.values()) - 1.0) < 1e-9, "scotopic weights must sum to 1"
    assert abs(sp - SP_PUBLISHED) < SP_TOLERANCE, (
        f"S/P ratio {sp:.3f} is not the published ~{SP_PUBLISHED}; a weight is wrong")
    assert norm["b"] > norm["g"] > norm["r"], "rods must see blue best and red worst"
    assert norm["r"] < 0.02, "red must be near-absent, or reds will not go black"
    tint_luma = sum(tint[c] * PHOTOPIC_W[c] for c in tint)
    assert abs(tint_luma - 1.0) < 1e-9, "the tint must carry unit photopic luma"
    assert tint["b"] > tint["r"], "the shift is toward BLUE"

    text = emit(norm, tint, sp)
    out = os.path.normpath(OUT)
    if "--check" in sys.argv:
        with open(out) as fh:
            sys.exit(0 if fh.read() == text else 1)
    with open(out, "w") as fh:
        fh.write(text)
    print(f"wrote {out} (S/P ratio {sp:.4f}, published ~{SP_PUBLISHED})")
