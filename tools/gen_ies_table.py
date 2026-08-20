#!/usr/bin/env python3
"""Read an IESNA LM-63 photometric file and resample it to the engine's table.

    python3 tools/gen_ies_table.py path/to/luminaire.ies        # report it
    python3 tools/gen_ies_table.py path/to/luminaire.ies --dump # + the table

Offline reference and authoring aid, NOT part of the build -- but unlike the
other tools here it has a second job, which is why it is written to be imported:
assets/gen_ies_fixture.py runs it over synthesised files whose candela are known
by construction, so the fixture verifies the tool rather than trusting it, and
the C reader in cetra/src/ies.c is then checked against both. Stdlib only.

WHAT AN LM-63 FILE CONTAINS

A photometric report for a real luminaire. After a header of IESNA-defined
keywords comes TILT, then two numeric lines, then three angle/candela blocks:

    <n_lamps> <lumens_per_lamp> <multiplier> <n_vert> <n_horiz> <type> ...
    <ballast> <future> <input_watts>
    <n_vert vertical angles, degrees>
    <n_horiz horizontal angles, degrees>
    <n_vert * n_horiz candela values, horizontal-major>

Candela are absolute, in cd, and must be scaled by `multiplier` (and by the
ballast factor, which real files use to derate a lamp). Whitespace is NOT
line-structured -- the standard permits the numeric blocks to wrap anywhere, so
this reads them as one token stream rather than line by line. That is the single
most common way a naive LM-63 reader breaks.

THE SYMMETRY IS DECLARED, NOT GUESSED

The horizontal angle set says how much of the sphere was measured:

    one angle (0)   fully rotationally symmetric -- one plane describes it
    0 .. 90         quadrant symmetric, mirror into all four
    0 .. 180        bilaterally symmetric, mirror about the 0-180 plane
    0 .. 360        no symmetry, measured all the way round

So "is this profile symmetric" is a question the file answers, not one the
resampler has to infer from the values. Resolving it here rather than in the
shader is what lets the runtime lookup be one bilinear fetch with no cases.

WHAT COMES OUT

A grid resampled to at most IES_MAX_VERT x IES_MAX_HORIZ, NORMALISED so its peak
is exactly 1.0, plus that peak reported separately in candela. The engine
multiplies the normalised shape by the light's own intensity, and seeds that
intensity from `peak_cd` when the scene authored none -- so normalising loses
none of the file's absolute output while leaving `intensity` the one brightness
control (see specs/11.57).

THE TAIL TAP IS EXACTLY ZERO, and that is a contract rather than an accident:
pbr_frag skips nine shadow taps and the whole GGX chain on `attenuation <= 0.0`,
which is only safe because every factor reaches zero exactly. A profile that
merely got small there would silently change which fragments take that path.
"""

import sys

# The engine's ceiling, mirrored from cetra/src/ies.h. 32 vertical taps is 5.6
# degrees, at or above what real files carry (19 or 37 vertical angles, i.e. 10
# or 5 degree steps), so a finer grid would mostly resample noise upward.
IES_MAX_VERT = 32
IES_MAX_HORIZ = 16


class IesError(Exception):
    """A file this reader will not guess about."""


def _tokens(text):
    """The numeric block as one token stream.

    LM-63 lets the angle and candela blocks wrap at any column, so anything that
    reads them line by line works on the files it was tested against and fails on
    the next exporter. The header is line-structured and is consumed first.
    """
    out = []
    for line in text.splitlines():
        out.extend(line.replace(",", " ").split())
    return out


def parse(text):
    """Parse LM-63 text into a dict. Raises IesError on anything ambiguous."""
    lines = text.splitlines()
    tilt_at = None
    for i, line in enumerate(lines):
        if line.strip().upper().startswith("TILT="):
            tilt_at = i
            break
    if tilt_at is None:
        raise IesError("no TILT= line; not an LM-63 file")
    if lines[tilt_at].strip().upper() != "TILT=NONE":
        # TILT=INCLUDE embeds a whole second table describing how output varies
        # with mounting angle. Refused rather than ignored: silently dropping it
        # renders a tilted luminaire at its untilted output.
        raise IesError("TILT is not NONE; tilt tables are not supported")

    tok = _tokens("\n".join(lines[tilt_at + 1:]))
    if len(tok) < 13:
        raise IesError("truncated before the photometric header")

    try:
        n_lamps = int(float(tok[0]))
        lumens_per_lamp = float(tok[1])
        multiplier = float(tok[2])
        n_vert = int(float(tok[3]))
        n_horiz = int(float(tok[4]))
        photometric_type = int(float(tok[5]))
        ballast = float(tok[10])
    except (ValueError, IndexError) as exc:
        raise IesError("malformed photometric header: %s" % exc)

    if n_vert < 2 or n_horiz < 1:
        raise IesError("degenerate angle grid: %d vertical x %d horizontal" % (n_vert, n_horiz))

    at = 13
    vert = [float(v) for v in tok[at:at + n_vert]]
    at += n_vert
    horiz = [float(v) for v in tok[at:at + n_horiz]]
    at += n_horiz
    raw = [float(v) for v in tok[at:at + n_vert * n_horiz]]
    if len(vert) != n_vert or len(horiz) != n_horiz or len(raw) != n_vert * n_horiz:
        raise IesError("truncated angle or candela block")
    if any(vert[i] >= vert[i + 1] for i in range(n_vert - 1)):
        raise IesError("vertical angles are not strictly increasing")

    # Horizontal-major: all vertical angles for horizontal plane 0, then plane 1.
    # candela[h][v], which is the order the file is written in and the order the
    # resample below walks.
    scale = multiplier * (ballast if ballast > 0.0 else 1.0)
    candela = [[raw[h * n_vert + v] * scale for v in range(n_vert)] for h in range(n_horiz)]

    return {
        "n_lamps": n_lamps,
        "lumens_per_lamp": lumens_per_lamp,
        "photometric_type": photometric_type,
        "vert": vert,
        "horiz": horiz,
        "candela": candela,
    }


def horizontal_span(horiz):
    """The angular span one measured sweep covers, in degrees.

    LM-63 declares symmetry by which horizontal angles it lists, so this reads
    the declaration rather than inspecting the values. A single plane is the
    rotationally symmetric case and spans the whole circle by itself.
    """
    if len(horiz) == 1:
        return 360.0
    last = horiz[-1]
    # The listed range IS the span: 90 quadrant, 180 bilateral, 360 none. Files
    # occasionally stop just short (89.5, 179.5), so snap to the nearest of the
    # three rather than treating 179.5 as an unmirrored sweep.
    for span in (90.0, 180.0, 360.0):
        if abs(last - span) <= 1.0:
            return span
    raise IesError("horizontal angles end at %g, which is none of 90/180/360" % last)


def fold_horizontal(a, span):
    """Fold a horizontal angle into the measured sweep [0, span].

    A partial sweep is a MIRROR, not a repeat: a bilateral file measured 0..180
    describes 190 degrees as 170, the reflection of the query about the measured
    plane -- not as 10, which is what a modulo gives and which reads the far side
    of the luminaire as if it were the near side. Quadrant files mirror twice.
    Both fall out of reflecting within a period of 2*span, and a full 360 sweep
    passes through untouched because nothing in [0, 360) exceeds its span.

    Mirrored in cetra/src/ies.c and in lights_ubo.glsl's iesFold, and the three
    cannot share a token: %, fmodf and mod() disagree on negative input. This
    copy is asserted directly by assets/gen_ies_fixture.py, which is the only
    one of the three a Python test can reach.
    """
    period = 2.0 * span
    f = a % period
    return period - f if f > span else f


def _sample(angles, values, a):
    """Linear interpolation of `values` over `angles` at `a`, clamped at both ends."""
    if a <= angles[0]:
        return values[0]
    if a >= angles[-1]:
        return values[-1]
    lo = 0
    hi = len(angles) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if angles[mid] <= a:
            lo = mid
        else:
            hi = mid
    t = (a - angles[lo]) / (angles[hi] - angles[lo])
    return values[lo] * (1.0 - t) + values[hi] * t


def resample(doc, max_vert=IES_MAX_VERT, max_horiz=IES_MAX_HORIZ):
    """Resample onto a uniform grid, normalised to peak 1.

    Returns (table, meta). `table` is v-major, hTaps entries per vertical tap, so
    it lands in memory the way the shader walks it. Vertical taps span the file's
    own measured range; horizontal taps span [0, span] INCLUSIVE of both ends,
    because a partial sweep mirrors rather than repeats and both endpoints are
    real measured planes. A symmetric file gets exactly one tap and the shader's
    horizontal lerp collapses onto it.
    """
    vert, horiz, candela = doc["vert"], doc["horiz"], doc["candela"]
    span = horizontal_span(horiz)
    symmetric = len(horiz) == 1

    v_taps = min(max_vert, max(2, len(vert)))
    h_taps = 1 if symmetric else min(max_horiz, max(2, len(horiz)))

    v_lo, v_hi = vert[0], vert[-1]
    # A column depends on the horizontal tap alone, so build them all first --
    # nested under the vertical loop each was rebuilt v_taps times over. No fold:
    # a_h is inside [0, span] by construction, and the fold exists for the
    # LOOKUP, which is the only place an out-of-range azimuth can arrive.
    by_v = list(zip(*candela))
    columns = []
    for ih in range(h_taps):
        if symmetric:
            columns.append(candela[0])
            continue
        a_h = span * ih / (h_taps - 1)
        columns.append([_sample(horiz, by_v[v], a_h) for v in range(len(vert))])

    table = []
    for iv in range(v_taps):
        a_v = v_lo + (v_hi - v_lo) * iv / (v_taps - 1)
        for ih in range(h_taps):
            table.append(_sample(vert, columns[ih], a_v))

    peak = max(table)
    if peak <= 0.0:
        raise IesError("every candela value is zero")
    table = [t / peak for t in table]

    # The tail tap is exactly zero when the file's own tail is, and the file's
    # tail is zero whenever it was measured out to a cutoff. Snap a tap that
    # rounds to nothing so the shader's early-out contract holds exactly rather
    # than nearly (see the module docstring).
    table = [0.0 if t < 1e-6 else t for t in table]

    return table, {
        "v_taps": v_taps,
        "h_taps": h_taps,
        "span": span,
        "symmetric": symmetric,
        "peak_cd": peak,
        "v_lo": v_lo,
        "v_hi": v_hi,
    }


def load(path, max_vert=IES_MAX_VERT, max_horiz=IES_MAX_HORIZ):
    """parse + resample for a path. The one entry point a caller should need."""
    with open(path, "r", errors="replace") as fh:
        return resample(parse(fh.read()), max_vert, max_horiz)


def main(argv):
    if not argv:
        print(__doc__.strip().splitlines()[0])
        print("usage: gen_ies_table.py <file.ies> [--dump]")
        return 2
    table, meta = load(argv[0])
    print("%s: %d x %d taps, span %g deg, %s, peak %.4f cd, vertical %g..%g"
          % (argv[0], meta["v_taps"], meta["h_taps"], meta["span"],
             "symmetric" if meta["symmetric"] else "asymmetric",
             meta["peak_cd"], meta["v_lo"], meta["v_hi"]))
    if "--dump" in argv[1:]:
        for iv in range(meta["v_taps"]):
            row = table[iv * meta["h_taps"]:(iv + 1) * meta["h_taps"]]
            a = meta["v_lo"] + (meta["v_hi"] - meta["v_lo"]) * iv / (meta["v_taps"] - 1)
            print("  %7.2f deg  %s" % (a, " ".join("%.4f" % t for t in row)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
