"""The fog-volume group: local fog volumes (density, tint, arming; spec 11.39)."""

import os

from gatelib.harness import asset, render
from gatelib.images import _absorb_box_rgb, _read_ppm
from gatelib.scenes import FOGVOL_BOXES, FOGVOL_DARKEN_MAX, FOGVOL_FIXTURE, FOGVOL_FRAMES


# A white tint must leave the air's own colour alone, so the untinted box stays neutral.
FOGVOL_NEUTRAL_EPS = 0.01
# The warm box authors tint b=0.25 against the white box's 1.0 at EQUAL density, so blue
# has to fall relative to red. Measured 0.9164 against the white box's 1.0000; 0.985 is a
# wide floor that still fails the tint being dropped entirely.
FOGVOL_TINT_MAX = 0.985
# The sigma-weighted fold, on its own fixture (spec 11.40). Same geometry, same camera and
# the same crop boxes as above -- only the volume set differs:
#   left  strip -- warm alone at D
#   right strip -- white at D/2 and warm at D/2, COINCIDENT in one box
# Total extinction is D on both, so transmittance and the backdrop share are identical and
# the only variable is what the tint sum is made of.
FOGVOL_MIX_FIXTURE = "fog_volume_mix_fixture.cscn"
# The coincident strip must read LESS tinted than the warm-alone one. A plain `S *= tint`
# cannot produce this at all -- white is its identity, so both strips would be equal and the
# ratio would sit at 1.0. Measured 1.0211 in sRGB; the floor is set well inside that and
# still far above the R-channel control, which agrees to four decimals.
FOGVOL_MIX_MIN = 1.008


def run_fog_volume_gate(workdir):
    """Local fog volumes (spec 11.39): a box of denser air, and its tint.

    Four arms, none implying the others:

      fogvol-density  the boxes differ from the bare gap between them. Fails the
                      volume never reaching the medium at all -- which is the whole
                      feature, and which every ratio arm below would still pass on an
                      empty frame, since two absent boxes agree perfectly.
      fogvol-neutral  the WHITE-tinted box stays neutral. This is the identity arm: it
                      fails a tint that is applied as an added radiance rather than a
                      sigma-weighted average, because adding tints white in too.
      fogvol-tint     the warm box's blue:red falls below the white box's, at equal
                      density. Colour isolated from amount -- the two boxes differ in
                      nothing else.
      fogvol-off      --no-fog-volumes returns the frame to no medium: the box strips
                      read the gap value again. The inverse arm, and it also proves the
                      volume ARMED the pass, since this scene authors no global fog.
      fogvol-fold     on the MIX fixture: a strip whose medium is half white and half warm
                      reads less tinted than one that is all warm at the same total
                      extinction. The only arm that can see the sigma-weighted combination
                      -- and the only one a plain `S *= tint` fails, since white is that
                      form's identity and the two strips would be equal.

    --no-ssao on every arm, and that is load-bearing rather than hygiene. GTAO also asks
    for the aux G-buffer, and the atmosphere pass returns early without it -- so with GTAO
    on, these arms cannot tell "the volume armed the pass" from "something else kept the
    depth alive and the volume rode along". Run without it and the arms test their own
    subject. Dither off because the reads are channel ratios where a +/-1 LSB is large.
    """
    scene = asset(FOGVOL_FIXTURE)
    if not os.path.exists(scene):
        print(f"  fogvol-density SKIP  ({FOGVOL_FIXTURE} not present)")
        return []

    base = ["--no-auto-exposure", "-E", "1.0", "--no-dither", "--no-ssao"]
    out = os.path.join(workdir, "fogvol_on.ppm")
    err = render(scene, out, base, frames=FOGVOL_FRAMES)
    if err:
        print(f"  fogvol-density ERROR render failed: {err.strip()[-200:]}")
        return ["fogvol-density"]

    failures = []
    w, h, pix = _read_ppm(out)
    rgb = {k: _absorb_box_rgb(pix, w, h, b) for k, b in FOGVOL_BOXES.items()}

    def luma(v):
        return (v[0] + v[1] + v[2]) / 3.0

    darken = max(luma(rgb["left"]), luma(rgb["right"])) / max(luma(rgb["gap"]), 1e-6)
    ok = darken <= FOGVOL_DARKEN_MAX
    print(f"  fogvol-density {'PASS' if ok else 'FAIL'}  box/gap={darken:.4f} "
          f"want <={FOGVOL_DARKEN_MAX} (gap={luma(rgb['gap']):.4f})")
    if not ok:
        failures.append("fogvol-density")

    lv = rgb["left"]
    spread = (max(lv) - min(lv)) / max(max(lv), 1e-6)
    ok = spread <= FOGVOL_NEUTRAL_EPS
    print(f"  fogvol-neutral {'PASS' if ok else 'FAIL'}  white-tint box "
          f"RGB={lv[0]:.4f}/{lv[1]:.4f}/{lv[2]:.4f} spread={spread:.4f} "
          f"want <={FOGVOL_NEUTRAL_EPS}")
    if not ok:
        failures.append("fogvol-neutral")

    br_white = lv[2] / max(lv[0], 1e-6)
    rv = rgb["right"]
    br_warm = rv[2] / max(rv[0], 1e-6)
    rel = br_warm / max(br_white, 1e-6)
    ok = rel <= FOGVOL_TINT_MAX
    print(f"  fogvol-tint    {'PASS' if ok else 'FAIL'}  B/R warm={br_warm:.4f} "
          f"white={br_white:.4f} ratio={rel:.4f} want <={FOGVOL_TINT_MAX}")
    if not ok:
        failures.append("fogvol-tint")

    off = os.path.join(workdir, "fogvol_off.ppm")
    err = render(scene, off, base + ["--no-fog-volumes"], frames=FOGVOL_FRAMES)
    if err:
        print(f"  fogvol-off     ERROR render failed: {err.strip()[-200:]}")
        return failures + ["fogvol-off"]
    w2, h2, pix2 = _read_ppm(off)
    off_rgb = {k: _absorb_box_rgb(pix2, w2, h2, b) for k, b in FOGVOL_BOXES.items()}
    off_darken = max(luma(off_rgb["left"]), luma(off_rgb["right"])) / \
        max(luma(off_rgb["gap"]), 1e-6)
    ok = abs(off_darken - 1.0) <= FOGVOL_NEUTRAL_EPS
    print(f"  fogvol-off     {'PASS' if ok else 'FAIL'}  box/gap={off_darken:.4f} "
          f"want 1.0 +/-{FOGVOL_NEUTRAL_EPS}")
    if not ok:
        failures.append("fogvol-off")

    mix_scene = asset(FOGVOL_MIX_FIXTURE)
    if not os.path.exists(mix_scene):
        print(f"  fogvol-fold    SKIP  ({FOGVOL_MIX_FIXTURE} not present)")
        return failures
    mixed = os.path.join(workdir, "fogvol_fold.ppm")
    err = render(mix_scene, mixed, base, frames=FOGVOL_FRAMES)
    if err:
        print(f"  fogvol-fold    ERROR render failed: {err.strip()[-200:]}")
        return failures + ["fogvol-fold"]
    w3, h3, pix3 = _read_ppm(mixed)
    fold = {k: _absorb_box_rgb(pix3, w3, h3, b) for k, b in FOGVOL_BOXES.items()}
    alone, half = fold["left"], fold["right"]
    br_alone = alone[2] / max(alone[0], 1e-6)
    br_half = half[2] / max(half[0], 1e-6)
    rel_fold = br_half / max(br_alone, 1e-6)
    # R is the control: equal total extinction means equal depth mix, so the untinted
    # channel must agree. If it drifts, the two strips are no longer comparable and the
    # B/R shift below stops being attributable to the fold.
    r_spread = abs(alone[0] - half[0]) / max(alone[0], 1e-6)
    ok = rel_fold >= FOGVOL_MIX_MIN and r_spread <= FOGVOL_NEUTRAL_EPS
    print(f"  fogvol-fold    {'PASS' if ok else 'FAIL'}  B/R half-white={br_half:.4f} vs "
          f"all-warm={br_alone:.4f}, ratio={rel_fold:.4f} want >={FOGVOL_MIX_MIN} "
          f"(R control spread {r_spread:.4f}, want <={FOGVOL_NEUTRAL_EPS})")
    if not ok:
        failures.append("fogvol-fold")

    return failures
