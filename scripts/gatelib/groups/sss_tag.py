"""The sss-tag group: subsurface profile tag through the MSAA resolve (spec 11.37)."""

import os

from gatelib.harness import NO_HALOS, RENDER, _run, asset
from gatelib.images import _SRGB_TO_LINEAR, _read_ppm
from gatelib.scenes import _cscn_camera, _projector, cscn_copy


SSS_TAG_FIXTURE = "sss_msaa_fixture.cscn"
SSS_TAG_SIZE = ("400", "300")
# The tag-2 sphere in WORLD space: sss_fixture.gltf puts unit spheres at x = +/-1.3, y = 1.0.
# Projected rather than found in the image, because the obvious way to locate a sphere -- the
# extent of the pixels its own hue dominates -- includes its SCATTER HALO, so the radius moves
# with the thing being measured and two frames end up reading different rings.
SSS_TAG_CENTRE = (1.3, 1.0, 0.0)
SSS_TAG_TOP = (1.3, 2.0, 0.0)
# The band straddling the silhouette, where partial coverage lives. Multiples of the projected
# radius, on the OUTER half only -- the other sphere is to the left.
SSS_TAG_RIM = (0.92, 1.02)
# The defect's measured size. An OPEN defect: 11.37 phase 2 moved the tag into stencil, which
# took this to 0.000%, and was reverted -- a per-pixel pass can only read one stencil value per
# pixel, so resolving a per-sample tag discards coverage and trades a categorical error for an
# arbitrary one. See the spec. Bar an order below the effect: presence, not amount.
SSS_TAG_MIN_SHIFT = 0.010


def _sss_arc_rgb(pix, w, h, cx, cy, r0, r1):
    """Mean linear RGB over the OUTER half (px >= cx) of an annulus."""
    s = [0.0, 0.0, 0.0]
    n = 0
    for py in range(max(0, int(cy - r1)), min(h, int(cy + r1) + 1)):
        for px in range(max(0, int(cx)), min(w, int(cx + r1) + 1)):
            if not (r0 <= ((px - cx) ** 2 + (py - cy) ** 2) ** 0.5 <= r1):
                continue
            o = (py * w + px) * 3
            s[0] += _SRGB_TO_LINEAR[pix[o]]
            s[1] += _SRGB_TO_LINEAR[pix[o + 1]]
            s[2] += _SRGB_TO_LINEAR[pix[o + 2]]
            n += 1
    return [v / max(n, 1) for v in s], n


def _sss_tag_render(workdir, tag, scene, samples):
    """One frame. --no-bloom is REQUIRED, not tidiness -- see the gate docstring."""
    out = os.path.join(workdir, f"ssstag_{tag}.ppm")
    cmd = [RENDER, "-m", scene, "-x", "-f", "30", "-W", SSS_TAG_SIZE[0], "-H", SSS_TAG_SIZE[1],
           "--msaa", str(samples), *NO_HALOS, "-S", out]
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    return out


def _sss_tag_rim(path, cam):
    """Red/green of the tag-2 sphere's rim: which profile blurred those pixels."""
    w, h, pix = _read_ppm(path)
    project = _projector(cam, w, h)
    cx, cy = project(SSS_TAG_CENTRE)
    tx, ty = project(SSS_TAG_TOP)
    rad = ((tx - cx) ** 2 + (ty - cy) ** 2) ** 0.5
    rgb, _ = _sss_arc_rgb(pix, w, h, cx, cy, rad * SSS_TAG_RIM[0], rad * SSS_TAG_RIM[1])
    return rgb[0] / max(rgb[1], 1e-9)


def _sss_tag_shift(workdir, samples):
    """How much a SECOND profile in the frame moves the tag-2 sphere's rim. Should be nothing."""
    scene = asset(SSS_TAG_FIXTURE)
    twin = os.path.join(workdir, "sss_msaa_one.cscn")
    cscn_copy(scene, twin, lambda d: d["materials"].pop("sss_skin_a", None))
    two = _sss_tag_render(workdir, f"two_{samples}", scene, samples)
    one = _sss_tag_render(workdir, f"one_{samples}", twin, samples)
    if not two or not one:
        return None
    cam = _cscn_camera(SSS_TAG_FIXTURE)
    a, b = _sss_tag_rim(two, cam), _sss_tag_rim(one, cam)
    return (a - b) / max(b, 1e-9), a, b


def run_sss_tag_gate(workdir):
    """Which SSS profile blurred a pixel must not depend on what else is in the frame (11.37).

    The profile a pixel belongs to used to be a LABEL in attachment 4's alpha, and that
    attachment is MSAA-resolved by a blit -- a box filter. Averaging a label destroys it: the
    mean of tag 2 and the uncovered 0 is 1, which does not mean "half of profile 2", it means
    PROFILE 1. So the partly covered pixels of the tag-2 sphere were blurred with the other
    profile's channel weights. Phase 2 moved the tag into stencil, which is integer and per
    sample and has no mean to take.

    The isolation holds the sample count FIXED and varies whether a second profile exists at
    all -- with one profile the same averaging is harmless, since a pixel either rounds back to
    that tag or to 0 and is dropped. Comparing 4x against 1 sample instead makes ANTIALIASING
    the dominant term and moves the rim the opposite way, which would read as the defect being
    absent.

    Two things this gate learned the hard way, both of which produced a confident wrong number:

    --no-bloom is LOAD-BEARING. The one-profile twin removes SSS from the OTHER sphere, which
    is a large shading change, and bloom spreads it across the whole frame -- including any
    band near the sphere being read. The first version of this arm reported 3.58% on a band
    OUTSIDE the silhouette, all of it bloom: the gather early-outs off-surface, so SSS provably
    never paints there and the band contained nothing else.

    And the fixture must be SIDE-LIT. Scatter is blur minus centre, so a uniformly lit sphere
    contributes nothing however wrong its profile is -- under a back key the whole gather moved
    129 px of the frame and the misfile moved 23. The terminator is what makes the defect
    exist to be measured.
    """
    scene = asset(SSS_TAG_FIXTURE)
    if not os.path.exists(scene):
        print(f"  sss-tag      SKIP  ({SSS_TAG_FIXTURE} not present)")
        return []

    failures = []
    got = _sss_tag_shift(workdir, 4)
    if got is None:
        print("  sss-tag      ERROR while rendering the 4x profile-count pair")
        return ["sss-tag"]
    shift, two, one = got
    ok = shift >= SSS_TAG_MIN_SHIFT
    print(f"  sss-tag      {'PASS' if ok else 'FAIL'}  4x MSAA, tag-2 rim R/G {two:.5f} with two "
          f"profiles vs {one:.5f} with one, {shift * 100.0:+.3f}% "
          f"(want >= {SSS_TAG_MIN_SHIFT * 100.0:.1f}%: this records an OPEN defect at its size, "
          f"so it goes to zero only by a deliberate change here)")
    if not ok:
        failures.append("sss-tag")

    got = _sss_tag_shift(workdir, 1)
    if got is None:
        print("  sss-tag-1x   ERROR while rendering the 1-sample profile-count pair")
        return failures + ["sss-tag-1x"]
    shift1, two1, one1 = got
    ok = abs(shift1) <= SSS_TAG_MIN_SHIFT
    print(f"  sss-tag-1x   {'PASS' if ok else 'FAIL'}  1 sample, same pair {two1:.5f} vs "
          f"{one1:.5f}, {shift1 * 100.0:+.3f}% (want <= {SSS_TAG_MIN_SHIFT * 100.0:.1f}%: with "
          f"no partial coverage there is nothing to average, so the arm above measures partial "
          f"coverage rather than the mere presence of a second profile)")
    if not ok:
        failures.append("sss-tag-1x")

    return failures
