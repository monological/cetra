"""The cloud-shadow group: cloud shadows into the fog and onto the ground (specs 11.39, 11.41)."""

import os

from gatelib.harness import RENDER, _run, asset, render
from gatelib.images import _absorb_box_rgb, _read_ppm
from gatelib.sky import CLOUDSHADOW_FRAMES


CLOUDSHADOW_FIXTURE = "aerial_fixture.gltf"
# Eight boxes across a band, tiled the same way on the sky and on the ground so the two
# pairs of arms differ in one thing only.
def _cloudshadow_strip(y0, y1):
    return [(0.02 + 0.12 * i, y0, 0.12 + 0.12 * i, y1) for i in range(8)]


# The SKY band, for the froxel half: it shadows in-scattered light, so it lands where the
# sight line crosses the most air.
CLOUDSHADOW_BAND = _cloudshadow_strip(0.05, 0.33)
# The GROUND band, for the surface half (spec 11.41). Read WITHOUT --fog, which is what makes
# it unambiguous: with no medium in the frame the froxel half contributes nothing, so every
# moving pixel is a shaded surface. Clear of the horizon, where the band would average terrain
# with sky.
CLOUDSHADOW_GROUND = _cloudshadow_strip(0.62, 0.88)
# The deck must remove light from whatever is under it. Measured 0.8476 on the air and 0.5647
# on the ground; wide floors on both, because these two arms' job is direction and presence and
# the variance arms below are what read content.
CLOUDSHADOW_DARKEN_MAX = 0.99
CLOUDSHADOW_GROUND_DARKEN_MAX = 0.97
# ...and it must remove DIFFERENT amounts in different places. Measured 0.9003 on the air and
# 0.2992 on the ground.
#
# On the GROUND this reads the map's contents through the lookup, and is the only arm that does.
# On the AIR it does NOT -- measured, and the opposite of what this comment claimed until 11.41.
# Against a stub returning a constant 0.5, taken at the configuration these arms ship in:
#
#   cloudshadow-dapple  0.0535  FAILS      the only arm that catches it
#   cloudshadow-ground  0.6777  passes     a constant still darkens
#   cloudshadow-vary    0.9268  passes     6x its own floor, on a dead lookup
#   -map / -lowsun              pass       correctly: the MAP is fine, the lookup is not
#
# The air arm's spread comes from the froxel composite's own structure whatever the map says. So
# -vary is a liveness arm and nothing more; -dapple plus the map arms are what separate a working
# lookup from a broken one, and a dead map from a dead lookup.
CLOUDSHADOW_SPREAD_MIN = 0.15
# The --sky-debug tile for the shadow map, at -W 400 -H 400 (framebuffer 800x800). The layout
# is sky.c's: column two at x = 20 + 2*SKY_TRANSMITTANCE_W, below the two noise tiles, and
# CLOUD_SHADOW_DEBUG_W a side. Rendered taller than the suite's usual 300 because at that
# height the tile's GL y goes negative and it falls off the bottom of the frame.
CLOUDSHADOW_TILE_BOX = (538, 420, 718, 600)
CLOUDSHADOW_TILE_SIZE = ("400", "400")
# The map must carry RANGE: some texel lit, and real variation between texels.
#
# This reads the map itself rather than its effect on anything, which is what lets it tell a bad
# MAP from a bad LOOKUP. Shipped 11.39 marched the full shell traverse and saturated to literally
# zero -- max below 7.8e-5 at every one of 256^2 texels -- and both downstream arms passed over
# it. The two floors catch the two saturations: peak catches all-black, sigma catches either.
# Re-derived at the coverage this arm now pins (it rode the engine default until 11.41, and the
# march it measures was rewritten in the same spec, so every earlier number here was stale):
# measured peak 1.0000 / sigma 0.3625, against 0.0 / 0.0 black and 1.0 / 0.0 white.
CLOUDSHADOW_MAP_MAX_MIN = 0.30
CLOUDSHADOW_MAP_SIGMA_MIN = 0.10
# And the same map at a LOW sun, which is a SEPARATE question the arm above cannot reach: it
# renders at one elevation, so it can only ever speak for that one.
#
# It could not have been more separate, as it turns out. The shipped map capped the marched path
# at a fixed 1.2 km while crossing the deck takes thickness/sin(elevation), so the fraction of
# cloud actually traversed fell with the sun -- 48% at the zenith, 4% at 5 degrees -- and below
# ~10 degrees the march never left the cloud base at all and the map came back uniformly 1.0.
# apps/tree runs a 0.8 degree sun and so had never had a cloud shadow from either half.
#
# Keyed on SIGMA and MEAN, deliberately not on peak: the failure was a uniformly WHITE map, whose
# peak is a perfect 1.0. A peak floor passes it. Measured sigma 0.0000 / mean 1.0000 broken
# against 0.2745 / 0.1148 fixed -- and against the bug reinstated, this arm fails while all five
# others pass, which is the whole reason it exists.
CLOUDSHADOW_LOWSUN_ELEV = "5"
CLOUDSHADOW_LOWSUN_SIGMA_MIN = 0.05
CLOUDSHADOW_LOWSUN_MEAN_MAX = 0.60
# Coverage for the ground and map arms, stated rather than inherited: an arm that rode the
# engine default would be silently re-tuned by anyone who changed it, and the default is 0.45.
#
# 0.10 is where this field has GAPS, and gaps are the whole mechanism -- extinction 25/km over a
# 2.5 km deck makes any real cloud in a column opaque, so a dappled ground is a map of the holes.
# Measured 47.9% of the map above half transmittance at 0.10 against 0.0% at 0.45, and the middle
# is no good either: at 0.2 the corrected march leaves the ground uniformly shaded, spread 0.0618
# against the 0.15 the dapple arm wants.
CLOUDSHADOW_COVERAGE = "0.10"


def _cloudshadow_band(on_path, off_path, boxes):
    """Mean shaded/lit ratio over `boxes`, and the relative spread of the SHORTFALL.

    The shortfall (1 - ratio) is what the deck removed, and it is the quantity a constant
    lookup makes uniform -- which is why the spread is taken on it rather than on the ratio.
    Returns (mean ratio, spread/mean of the shortfall, mean shortfall).
    """
    w, h, on = _read_ppm(on_path)
    _, _, off = _read_ppm(off_path)

    def luma(box, pix):
        v = _absorb_box_rgb(pix, w, h, box)
        return (v[0] + v[1] + v[2]) / 3.0

    ratios = [luma(b, on) / max(luma(b, off), 1e-6) for b in boxes]
    short = [1.0 - r for r in ratios]
    mean_short = sum(short) / len(short)
    return (sum(ratios) / len(ratios),
            (max(short) - min(short)) / max(mean_short, 1e-6),
            mean_short)


def _cloudshadow_map_stats(workdir, scene, tag, extra):
    """(peak, mean, sigma) of the --sky-debug shadow-map tile, or (None, error).

    Hand-rolled rather than render()'d because the tile needs a taller frame than the
    suite's default: below that height its GL y goes negative and it falls off the bottom.
    """
    tile = os.path.join(workdir, f"cloudshadow_{tag}.ppm")
    cmd = [RENDER, "-m", scene, "-x", "-f", "30",
           "-W", CLOUDSHADOW_TILE_SIZE[0], "-H", CLOUDSHADOW_TILE_SIZE[1],
           "-S", tile, "--cloud-coverage", CLOUDSHADOW_COVERAGE,
           "--sky-debug", "--no-auto-exposure", "-E", "1.0"] + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(tile):
        return None, (r.stdout + r.stderr)[-200:]
    tw, _, tpix = _read_ppm(tile)
    x0, y0, x1, y1 = CLOUDSHADOW_TILE_BOX
    vals = [tpix[(y * tw + x) * 3] / 255.0
            for y in range(y0, y1, 4) for x in range(x0, x1, 4)]
    mean = sum(vals) / len(vals)
    sigma = (sum((v - mean) ** 2 for v in vals) / len(vals)) ** 0.5
    return (max(vals), mean, sigma), None


def run_cloud_shadow_gate(workdir):
    """Cloud shadows in the froxel volume (spec 11.39) and on the ground (spec 11.41).

    Six arms, none implying the others. The first two read the AIR with --fog, the next
    two the GROUND without it, and the last two read the map itself:

      cloudshadow-dark    the deck darkens the medium under it.
      cloudshadow-vary    it darkens by DIFFERENT amounts across the frame. A liveness
                          arm and NOT a content one: measured, it passes a constant
                          lookup at 0.9268 against a 0.15 floor, because the froxel
                          composite's shortfall varies with the fog's own structure.
      cloudshadow-ground  the terrain darkens too, with NO fog in the frame -- so the
                          froxel half cannot be what moved. This pair used to assert 0 px
                          on exactly these two renders, which was correct only while the
                          ground was not a receiver.
      cloudshadow-dapple  and it varies across the terrain. Same argument as -vary, on the
                          surface: this is the arm that reads the map's content through
                          pbr_frag rather than through the medium.
      cloudshadow-map     the map itself carries range. Reads the --sky-debug tile, and it
                          is the only arm that can tell a bad MAP from a bad LOOKUP -- 11.39
                          shipped one saturated to zero at every texel with both of its
                          downstream arms passing over it.
      cloudshadow-lowsun  and it carries range at a LOW sun too, which is a different question
                          the arm above cannot reach because it renders at one elevation. 11.41
                          found the map blank below ~10 degrees; against that bug this arm reads
                          sigma 0.0000 / mean 1.0000 and fails while all FIVE others pass.

    The GROUND and MAP arms pin --cloud-coverage; -dark and -vary ride the engine default,
    which is where the froxel half was originally measured. A gate wants the configuration
    where its property is legible rather than a representative frame, and the four pinned
    arms need the deck to have GAPS -- see CLOUDSHADOW_COVERAGE.
    """
    scene = asset(CLOUDSHADOW_FIXTURE)
    if not os.path.exists(scene):
        print(f"  cloudshadow-dark SKIP  ({CLOUDSHADOW_FIXTURE} not present)")
        return []

    base = ["--clouds", "--no-auto-exposure", "-E", "1.0"]
    on = os.path.join(workdir, "cloudshadow_on.ppm")
    off = os.path.join(workdir, "cloudshadow_off.ppm")
    err = render(scene, on, base + ["--fog"], frames=CLOUDSHADOW_FRAMES) or \
        render(scene, off, base + ["--fog", "--no-cloud-shadows"], frames=CLOUDSHADOW_FRAMES)
    if err:
        print(f"  cloudshadow-dark ERROR render failed: {err.strip()[-200:]}")
        return ["cloudshadow-dark"]

    failures = []
    mean_ratio, spread, mean_short = _cloudshadow_band(on, off, CLOUDSHADOW_BAND)

    ok = mean_ratio <= CLOUDSHADOW_DARKEN_MAX
    print(f"  cloudshadow-dark {'PASS' if ok else 'FAIL'}  shaded/lit={mean_ratio:.4f} "
          f"want <={CLOUDSHADOW_DARKEN_MAX}")
    if not ok:
        failures.append("cloudshadow-dark")

    ok = spread >= CLOUDSHADOW_SPREAD_MIN
    print(f"  cloudshadow-vary {'PASS' if ok else 'FAIL'}  shortfall spread/mean="
          f"{spread:.4f} want >={CLOUDSHADOW_SPREAD_MIN} (mean {mean_short:.4f})")
    if not ok:
        failures.append("cloudshadow-vary")

    # The SURFACE half (spec 11.41), on the same fixture with NO fog. Where this pair used to
    # assert 0 px -- "no medium, so nothing to shadow" -- the ground is now a receiver, so the
    # same two renders carry the opposite claim and the arm reads the terrain instead of the
    # frame. Nothing else can move here: with no fog the froxel half is not in the frame at all.
    gbase = ["--cloud-coverage", CLOUDSHADOW_COVERAGE, "--no-auto-exposure", "-E", "1.0"]
    g_on = os.path.join(workdir, "cloudshadow_ground_on.ppm")
    g_off = os.path.join(workdir, "cloudshadow_ground_off.ppm")
    err = render(scene, g_on, gbase, frames=CLOUDSHADOW_FRAMES) or \
        render(scene, g_off, gbase + ["--no-cloud-shadows"], frames=CLOUDSHADOW_FRAMES)
    if err:
        # Named and CONTINUED, not returned: the two map arms below are independent renders,
        # and a skipped arm that prints nothing reads as a pass.
        print(f"  cloudshadow-ground ERROR render failed: {err.strip()[-200:]}")
        print("  cloudshadow-dapple SKIP  (the ground render failed)")
        failures.append("cloudshadow-ground")
    else:
        g_mean, g_spread, g_mean_short = _cloudshadow_band(g_on, g_off, CLOUDSHADOW_GROUND)

        ok = g_mean <= CLOUDSHADOW_GROUND_DARKEN_MAX
        print(f"  cloudshadow-ground {'PASS' if ok else 'FAIL'}  shaded/lit={g_mean:.4f} "
              f"want <={CLOUDSHADOW_GROUND_DARKEN_MAX}")
        if not ok:
            failures.append("cloudshadow-ground")

        ok = g_spread >= CLOUDSHADOW_SPREAD_MIN
        print(f"  cloudshadow-dapple {'PASS' if ok else 'FAIL'}  shortfall spread/mean="
              f"{g_spread:.4f} want >={CLOUDSHADOW_SPREAD_MIN} (mean {g_mean_short:.4f})")
        if not ok:
            failures.append("cloudshadow-dapple")

    stats, err = _cloudshadow_map_stats(workdir, scene, "map", [])
    if err:
        print(f"  cloudshadow-map  ERROR render failed: {err}")
        return failures + ["cloudshadow-map"]
    peak, mean, sigma = stats
    ok = peak >= CLOUDSHADOW_MAP_MAX_MIN and sigma >= CLOUDSHADOW_MAP_SIGMA_MIN
    print(f"  cloudshadow-map  {'PASS' if ok else 'FAIL'}  peak={peak:.4f} "
          f"(want >={CLOUDSHADOW_MAP_MAX_MIN}) sigma={sigma:.4f} "
          f"(want >={CLOUDSHADOW_MAP_SIGMA_MIN}), mean={mean:.4f}")
    if not ok:
        failures.append("cloudshadow-map")

    # The same map at a LOW sun, which is a different question and needs its own arm.
    stats, err = _cloudshadow_map_stats(workdir, scene, "map_lowsun",
                                        ["--sun-elevation", CLOUDSHADOW_LOWSUN_ELEV])
    if err:
        print(f"  cloudshadow-lowsun ERROR render failed: {err}")
        return failures + ["cloudshadow-lowsun"]
    peak, mean, sigma = stats
    ok = sigma >= CLOUDSHADOW_LOWSUN_SIGMA_MIN and mean <= CLOUDSHADOW_LOWSUN_MEAN_MAX
    print(f"  cloudshadow-lowsun {'PASS' if ok else 'FAIL'}  sigma={sigma:.4f} "
          f"(want >={CLOUDSHADOW_LOWSUN_SIGMA_MIN}) mean={mean:.4f} "
          f"(want <={CLOUDSHADOW_LOWSUN_MEAN_MAX}), peak={peak:.4f}")
    if not ok:
        failures.append("cloudshadow-lowsun")

    return failures
