"""The hair group: hair lobes driven by the strand map (spec 11.20 / B8)."""

import os

from gatelib.harness import asset, render
from gatelib.images import _SRGB_TO_LINEAR, _read_ppm


# The map arm must split the card's halves at least this hard, and the two
# control arms must not split them at all. Measured at this gate's size: map
# 1.32x, no map 1.05x, GGX 1.05x.
#
# The margin is THIN, and that is a property of the model rather than of the
# gate. Anisotropic GGX only reshapes an existing lobe, so a per-texel direction
# redistributes a fixed amount of light; the fibre lobes this replaced keyed
# their whole magnitude on the strand angle and split the same fixture 7.2x.
# Modesty is the point -- it is why this version does not blow out -- but it
# leaves less headroom, so the size is pinned (minification mips the
# orientation away) and the threshold sits just above the controls.
HAIR_FLOW_MIN_RATIO = 1.20
HAIR_FLAT_MAX_RATIO = 1.10

def _hair_card_box(path):
    """Bounding box of the lit card, as (x0, x1, y0, y1)."""
    w, h, pix = _read_ppm(path)
    lum = [(_SRGB_TO_LINEAR[pix[i * 3]] + _SRGB_TO_LINEAR[pix[i * 3 + 1]] +
            _SRGB_TO_LINEAR[pix[i * 3 + 2]]) / 3.0 for i in range(w * h)]
    lit = [i for i, v in enumerate(lum) if v > 0.002]
    if not lit:
        return None
    xs = [i % w for i in lit]
    ys = [i // w for i in lit]
    return min(xs), max(xs), min(ys), max(ys)


def _hair_halves(path, box):
    """Linear-luma samples of the card's left and right halves, within `box`.

    The box is passed in rather than derived per image, because arms are
    compared element-wise: jitter changes brightness at the lit edges, so one
    pixel crossing the threshold in one arm and not another would shift the
    crop, misalign every subsequent sample, and silently change what the
    comparison means.

    Excludes a margin around the seam where the two painted strand fields meet:
    the structure tensor genuinely cannot resolve an orientation there, reports
    low coherence, and the shader correctly falls back -- which is right
    behaviour and would only add noise to the measurement.
    """
    w, h, pix = _read_ppm(path)
    lum = [(_SRGB_TO_LINEAR[pix[i * 3]] + _SRGB_TO_LINEAR[pix[i * 3 + 1]] +
            _SRGB_TO_LINEAR[pix[i * 3 + 2]]) / 3.0 for i in range(w * h)]
    x0, x1, y0, y1 = box
    mid = (x0 + x1) // 2
    pad = max(1, int(0.10 * (x1 - x0)))
    left, right = [], []
    for y in range(y0, y1 + 1):
        row = y * w
        left += [lum[row + x] for x in range(x0 + pad, mid - pad)]
        right += [lum[row + x] for x in range(mid + pad, x1 - pad)]
    return left, right


def _hair_ratio(halves):
    left, right = halves
    lm = sum(left) / max(len(left), 1)
    rm = sum(right) / max(len(right), 1)
    return rm / max(lm, 1e-6)


def run_hair_flow_gate(workdir):
    """The hair lobes must be driven by the strand map, not by the card tangent.

    One quad, one material, one draw, one tangent. The atlas paints strands
    along the tangent in the left half and across it in the right, with the key
    off to the side, so a shader that reads the map splits the halves and one
    that does not cannot. Geometry, normal, material, light and tangent are
    identical either side of the seam, so nothing else can produce a split.

    The two control arms are what make that attributable. Hair with NO map must
    come out flat, and so must plain GGX -- and 'flat' is also what an invented
    per-texel jitter measures as, which is the failure this replaced: a hash of
    the texel coordinate varies within each half but has the same distribution
    in both, so it moves the variance and leaves the ratio at 1.
    """
    arms = (("map", "hair_fixture.cscn"),
            ("no map", "hair_fixture_nomap.cscn"),
            ("ggx", "hair_fixture_ggx.cscn"))
    # Large enough that the atlas lands near 1:1 on screen. Minified past that
    # the mip chain averages the strand identity away -- correctly, but it is
    # not what this is measuring.
    size = ["-W", "800", "-H", "600"]
    fails, frames, box = [], {}, None
    for label, name in arms:
        scene = asset(name)
        if not os.path.exists(scene):
            print(f"  hair-flow    SKIP  (missing {name})")
            return []
        out = os.path.join(workdir, name.replace(".cscn", ".ppm"))
        err = render(scene, out, size)
        if err:
            print(f"  hair-flow    ERROR while rendering {name}")
            return ["hair-flow"]
        frames[label] = out
        if box is None:
            box = _hair_card_box(out)
            if box is None:
                print(f"  hair-flow    ERROR  {name} rendered an empty frame")
                return ["hair-flow"]

    # One crop for every arm, taken from the first: the arms are compared
    # element-wise, so a per-image box would misalign them (see _hair_halves).
    shots = {label: _hair_halves(path, box) for label, path in frames.items()}

    # Direction-agnostic: the claim is that the halves DIFFER, not which way.
    # Anisotropic GGX stretches the highlight along the grain, so the brighter
    # half is the opposite one to what a fibre lobe would give -- and the sign
    # is a property of the model, not of whether the map is being read.
    ratio = _hair_ratio(shots["map"])
    split = max(ratio, 1.0 / max(ratio, 1e-6))
    ok = split >= HAIR_FLOW_MIN_RATIO
    print(f"  hair-flow    {'PASS' if ok else 'FAIL'}  halves differ {split:.3f}x "
          f"(want >= {HAIR_FLOW_MIN_RATIO}; only the map can split these halves)")
    if not ok:
        fails.append("hair-flow")

    for label in ("no map", "ggx"):
        flat = _hair_ratio(shots[label])
        flat = max(flat, 1.0 / max(flat, 1e-6))
        ok = flat <= HAIR_FLAT_MAX_RATIO
        print(f"  hair-flat    {'PASS' if ok else 'FAIL'}  {label}: {flat:.3f}x "
              f"(want <= {HAIR_FLAT_MAX_RATIO}; the control that makes the split the map's doing)")
        if not ok:
            fails.append("hair-flat")

    return fails
