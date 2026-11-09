"""The local-exposure group: local exposure, an exposure per pixel (spec 13.19)."""

import json
import os

from gatelib.harness import RENDER, asset, render
from gatelib.images import LSB, _read_ppm, compare
from gatelib.scenes import cscn_copy


# Local exposure (spec 13.19) on its own fixture: a grey wall at middle grey and a window six
# stops over it, the case a global exposure can only clip. Without AO, which draws a contact ring
# round the window that has nothing to do with exposure.
LE_FIXTURE = "local_exposure_fixture.cscn"
LE_FLAGS = ["--no-ssao"]
LE_ON = ["--local-exposure", "--le-highlights", "0.5", "--le-blend", "0"]
# The bilateral base keeps the wall beside the window within LE_HALO_KEEP of the wall without the
# feature, at every distance read. A pure blur (blend 1) put the wall 2 px from the edge at 0.37
# of it when measured; it must stay under LE_HALO_BLUR_MAX, or the arm is not looking at a halo.
LE_HALO_KEEP = 0.02
LE_HALO_BLUR_MAX = 0.8
LE_HALO_DISTANCES = (2, 4, 8, 16, 32)
LE_SCENE_MIN_PX = 10000


def _le_code_luma(pix, w, x, y):
    i = (y * w + x) * 3
    return 0.2126 * pix[i] + 0.7152 * pix[i + 1] + 0.0722 * pix[i + 2]


def _le_window(img):
    """The window's box: the pixels clipped white in a frame without the feature."""
    w, h, pix = img
    xs, ys = [], []
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 3
            if min(pix[i], pix[i + 1], pix[i + 2]) >= 254:
                xs.append(x)
                ys.append(y)
    return (min(xs), max(xs), min(ys), max(ys)) if xs else None


def _le_clipped(img, box):
    """The share of `box` clipped white."""
    w, _, pix = img
    x0, x1, y0, y1 = box
    n = clipped = 0
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            i = (y * w + x) * 3
            clipped += min(pix[i], pix[i + 1], pix[i + 2]) >= 254
            n += 1
    return clipped / max(n, 1)


def run_local_exposure_gate(workdir):
    """Local exposure (spec 13.19): the scene file and the flag are one switch, its defaults are
    the identity, it brings a clipped window down without moving the wall, and its edge-aware
    base keeps the wall beside the window from the halo a plain blur draws there.

      le-scene      post.local_exposure in a copy of the fixture renders the frame the flags do,
                    and moves the frame against the default; the default renders the frame
                    --no-local-exposure does.
      le-identity   on at its defaults (contrasts 1, detail 1) is within one code of off.
      le-compress   at highlights 0.5 the window, wholly clipped without the feature, clips
                    nowhere, and a patch of wall far from it moves by at most one code.
      le-halo       along the window's middle row, the wall LE_HALO_DISTANCES px from its edge
                    stays within LE_HALO_KEEP of the frame without the feature; a pure blur
                    (blend 1) darkens the wall 2 px from the edge under LE_HALO_BLUR_MAX of it.
                    The second half is the falsifier: it shows the arm can see a halo.
      le-config     the snapshot carries the seven postfx.local_exposure rows, enabled false and
                    every contrast at 1.

    Falsified by hand at 13.19: le-scene with the cscene parse of "enabled" dropped; le-identity
    with the factor taken against the base instead of the pixel (75 codes); le-compress with the
    factor forced to 1; le-halo with the grid replaced by the blurred luminance, which alone
    failed (0.369 at 2 px); le-config with the "blend" row deleted.
    """
    fixture = asset(LE_FIXTURE)
    if not os.path.exists(RENDER) or not os.path.exists(fixture):
        print("  le-scene     SKIP  (render or local_exposure_fixture not present)")
        return []
    arms = ["le-scene", "le-identity", "le-compress", "le-halo", "le-config"]
    paths = {k: os.path.join(workdir, f"le_{k}.ppm")
             for k in ("plain", "flagged_off", "on", "identity", "blur", "authored")}
    scene = os.path.join(workdir, "le_scene.cscn")
    cscn_copy(fixture, scene, lambda d: d.setdefault("post", {}).__setitem__(
        "local_exposure", {"enabled": True, "highlights": 0.5, "blend": 0.0}))
    dump = os.path.join(workdir, "le_config.json")
    err = (render(fixture, paths["plain"], LE_FLAGS + ["--config-dump", dump]) or
           render(fixture, paths["flagged_off"], LE_FLAGS + ["--no-local-exposure"]) or
           render(fixture, paths["on"], LE_FLAGS + LE_ON) or
           render(fixture, paths["identity"], LE_FLAGS + ["--local-exposure"]) or
           render(fixture, paths["blur"], LE_FLAGS + LE_ON + ["--le-blend", "1"]) or
           render(scene, paths["authored"], LE_FLAGS))
    if err:
        for arm in arms:
            print(f"  {arm:<12} ERROR render failed: {err.strip()[-200:]}")
        return arms
    failures = []

    as_flag, _ = compare(paths["authored"], paths["on"])
    moved, _ = compare(paths["authored"], paths["plain"])
    default_off, _ = compare(paths["plain"], paths["flagged_off"])
    ok = as_flag == 0 and moved >= LE_SCENE_MIN_PX and default_off == 0
    print(f"  le-scene     {'PASS' if ok else 'FAIL'}  scene-authored against the flags {as_flag} px "
          f"(want 0), against the default {moved} px (want >={LE_SCENE_MIN_PX}), default against "
          f"--no-local-exposure {default_off} px (want 0)")
    if not ok:
        failures.append("le-scene")

    _, pae = compare(paths["identity"], paths["plain"])
    ok = pae <= LSB
    print(f"  le-identity  {'PASS' if ok else 'FAIL'}  at its defaults against off: peak "
          f"{pae * 255:.1f} codes (want <= 1)")
    if not ok:
        failures.append("le-identity")

    off = _read_ppm(paths["plain"])
    on = _read_ppm(paths["on"])
    blur = _read_ppm(paths["blur"])
    box = _le_window(off)
    if not box:
        print("  le-compress  FAIL  no clipped window in the frame without the feature")
        return failures + ["le-compress", "le-halo", "le-config"]
    w, h, _ = off
    clipped_off, clipped_on = _le_clipped(off, box), _le_clipped(on, box)
    # A patch of wall in the top-left corner, as far from the window as the frame allows.
    patch = [(x, y) for y in range(h // 10, h // 10 + 10) for x in range(5, 25)]
    wall_off = sum(_le_code_luma(off[2], w, x, y) for x, y in patch) / len(patch)
    wall_on = sum(_le_code_luma(on[2], w, x, y) for x, y in patch) / len(patch)
    ok = clipped_off >= 0.99 and clipped_on <= 0.01 and abs(wall_on - wall_off) <= 1.0
    print(f"  le-compress  {'PASS' if ok else 'FAIL'}  window clipped {clipped_off:.3f} off "
          f"(want >= 0.99) and {clipped_on:.3f} on (want <= 0.01); far wall {wall_off:.1f} -> "
          f"{wall_on:.1f} codes (want within 1)")
    if not ok:
        failures.append("le-compress")

    x0, _, y0, y1 = box
    row = (y0 + y1) // 2
    ratios = [_le_code_luma(on[2], w, x0 - d, row) / max(_le_code_luma(off[2], w, x0 - d, row), 1e-6)
              for d in LE_HALO_DISTANCES if x0 - d >= 0]
    blur_near = (_le_code_luma(blur[2], w, x0 - LE_HALO_DISTANCES[0], row) /
                 max(_le_code_luma(off[2], w, x0 - LE_HALO_DISTANCES[0], row), 1e-6))
    ok = (len(ratios) == len(LE_HALO_DISTANCES) and
          all(abs(r - 1.0) <= LE_HALO_KEEP for r in ratios) and blur_near <= LE_HALO_BLUR_MAX)
    print(f"  le-halo      {'PASS' if ok else 'FAIL'}  wall beside the window over the wall "
          f"without the feature at {', '.join(str(d) for d in LE_HALO_DISTANCES)} px: "
          f"{' '.join(f'{r:.3f}' for r in ratios)} (want within {LE_HALO_KEEP} of 1); a pure "
          f"blur at {LE_HALO_DISTANCES[0]} px {blur_near:.3f} (want <= {LE_HALO_BLUR_MAX})")
    if not ok:
        failures.append("le-halo")

    with open(dump) as f:
        blk = json.load(f).get("postfx", {}).get("local_exposure", {})
    want = {"enabled", "highlights", "shadows", "detail", "blend", "kernel", "grey_bias"}
    have = want <= set(blk)
    ok = (have and blk.get("enabled") is False and
          all(abs(blk.get(k, -1) - 1.0) < 1e-6 for k in ("highlights", "shadows", "detail")))
    print(f"  le-config    {'PASS' if ok else 'FAIL'}  seven rows carried={have}, enabled="
          f"{blk.get('enabled')} (want False), contrasts {blk.get('highlights')}/"
          f"{blk.get('shadows')}/{blk.get('detail')} (want 1/1/1)")
    if not ok:
        failures.append("le-config")
    return failures
