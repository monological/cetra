#!/usr/bin/env python3
"""Generate assets/terrain_fixture.r16 -- the heightfield instrument (spec 11.59).

A terrain fixture cannot be a .gltf. The thing under test is a height FIELD and a
sampler over it, so the fixture is a heightmap file and the consumers are
`forest --heightmap` plus `--terrain-height-probe`.

GROUND TRUTH IS PAINTED, NOT MEASURED. Every sample is a closed form of its two
normalised coordinates, so the gate knows in advance what the sampler owes at any
point -- including between nodes, which is where a filter is decided. A fixture
carved out of noise could only ever be compared against itself.

Three properties are load-bearing and each is asserted below, because a fixture
that quietly stops testing something is worse than no fixture:

  ASYMMETRY. The field must differ under swapping u and v, or a sampler that
  transposes its axes -- the single most common heightmap bug, and one that still
  renders as plausible terrain -- reads exactly correct.

  CURVATURE. It must have real second derivatives, or bicubic and bilinear agree
  and the filter arm cannot fail. This is the same trap the LUT fixture records:
  a table that degenerates under the interpolant being tested measures nothing.

  HEADROOM. Values must sit inside [0, 1] with margin, since the .r16 range maps
  onto exactly that and a clipped fixture would test the clamp rather than the
  sampler.
"""

import math
import os
import struct

RES = 256
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "terrain_fixture.r16")

# Deliberately not round, not symmetric between the two axes, and not a single
# frequency. The two trig terms run at different rates so a transpose is a large
# error rather than a subtle one, and the linear terms have opposite signs so even
# a field sampled at its own centre distinguishes them.
U_CYCLES = 3.0
V_CYCLES = 2.0
AMPLITUDE = 0.24
U_TILT = 0.12
V_TILT = -0.09
BASE = 0.50

# A second, much finer band, and it is what makes the FILTER testable at all.
#
# The low band alone spans ~85 samples per cycle, where a bilinear and a bicubic
# interpolant agree to a handful of 16-bit codes and no arm can separate them.
# Real fields are nothing like that smooth: forest bakes 512 cells over 1000 units
# with a finest octave of 12 units, which is six samples per cycle -- near Nyquist,
# which is exactly where the choice of filter decides the surface. 28 and 23
# cycles put this fixture in the same regime -- 7.5 samples per cycle -- and they are
# coprime so the two axes do not share a beat. The low band's amplitude came down
# to 0.24 to keep the sum inside [0, 1] with headroom.
FINE_AMPLITUDE = 0.08
FINE_U_CYCLES = 34.0
FINE_V_CYCLES = 29.0


def height01(u, v):
    """Normalised height at (u, v), both in [0, 1]. The whole fixture is this."""
    return (BASE
            + AMPLITUDE * math.sin(U_CYCLES * math.pi * u) * math.cos(V_CYCLES * math.pi * v)
            + FINE_AMPLITUDE * math.sin(2.0 * math.pi * FINE_U_CYCLES * u)
            * math.cos(2.0 * math.pi * FINE_V_CYCLES * v)
            + U_TILT * u
            + V_TILT * v)


def _sweep():
    return [[height01(i / (RES - 1), j / (RES - 1)) for i in range(RES)] for j in range(RES)]


_grid = _sweep()
_flat = [h for row in _grid for h in row]

# HEADROOM. Clipping at either end would make the arm a test of the clamp.
assert min(_flat) > 0.02, f"fixture floor {min(_flat):.4f} is too close to 0"
assert max(_flat) < 0.98, f"fixture ceiling {max(_flat):.4f} is too close to 1"

# ASYMMETRY. Measured against the quantisation step, not against zero: a field
# that differs under transpose by less than one 16-bit code would read identical
# through the file and the arm would pass on a transposing sampler.
_step = 1.0 / 65535.0
_swap = max(abs(height01(u / (RES - 1), v / (RES - 1)) - height01(v / (RES - 1), u / (RES - 1)))
            for u in range(0, RES, 7) for v in range(0, RES, 11))
assert _swap > 500 * _step, f"transposing the axes moves only {_swap / _step:.1f} codes"

# CURVATURE. A midpoint against the chord through its neighbours: zero here means
# the field is locally linear, where every interpolant agrees and the filter arm
# is green whatever it is testing.
_h = 1.0 / (RES - 1)
_mid = height01(0.5, 0.5)
_chord = 0.5 * (height01(0.5 - _h, 0.5) + height01(0.5 + _h, 0.5))
assert abs(_mid - _chord) > 4 * _step, (
    f"the field is locally linear at its centre ({abs(_mid - _chord) / _step:.1f} codes); "
    "bicubic and bilinear would agree and terrain-bicubic could not fail")

# FILTER SEPARATION. The previous assert only says the field is not a straight
# line. This one says how far apart the two interpolants actually land, which is
# the quantity terrain-bicubic reads -- and the first fixture passed the assert
# above while failing this one at 5 codes, so the filter arm was green against a
# bilinear sampler. Measured at a cell MIDPOINT, where the error peaks: bilinear
# there is the chord, bicubic is much closer to the curve.
_worst_sep = 0.0
for _i in range(3, RES - 4, 7):
    for _j in range(3, RES - 4, 11):
        _u, _v = (_i + 0.5) / (RES - 1), _j / (RES - 1)
        _bilinear = 0.5 * (height01(_i / (RES - 1), _v) + height01((_i + 1) / (RES - 1), _v))
        _worst_sep = max(_worst_sep, abs(height01(_u, _v) - _bilinear))
assert _worst_sep > 200 * _step, (
    f"bilinear and the true curve differ by only {_worst_sep / _step:.1f} codes at a cell "
    "midpoint; the field is too smooth for its resolution and terrain-bicubic cannot fail")


def main():
    with open(OUT, "wb") as f:
        for row in _grid:
            for h in row:
                # Round to nearest and clamp, matching heightmap_save exactly. A
                # generator that truncated where the C rounds would bias every
                # sample half a step and the round-trip arm would measure the
                # disagreement rather than the format.
                v = int(h * 65535.0 + 0.5)
                v = 0 if v < 0 else (65535 if v > 65535 else v)
                f.write(struct.pack("<H", v))
    print(f"wrote {OUT}: {RES}x{RES}, range {min(_flat):.4f}..{max(_flat):.4f}")


if __name__ == "__main__":
    main()
