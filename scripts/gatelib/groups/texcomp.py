"""The texcomp group: block-compressed textures (format, budget, lever; spec 11.85)."""

import os
import subprocess

from gatelib.harness import RENDER, _run, asset, render
from gatelib.images import compare
from gatelib.probes import _texcomp_probe
from gatelib.scenes import TEXCOMP_FIXTURE


TEXCOMP_FRAMES = 4
# BC5 and BC4 against their own uncompressed twin, as a fraction of full scale.
#
# PLACED BY MEASUREMENT at 0.0189, bar a shade over. It read 0.0058 until the Z
# rebuild was gated on the storage actually being two-channel, and the rise is
# the arm becoming HONEST rather than a regression: both legs used to rebuild Z,
# so the term cancelled and the number described the encoder alone. Now the
# uncompressed leg keeps its stored Z, so this measures what choosing BC5
# actually costs -- the encoder AND the rebuild BC5 forces. The rebuild is the
# larger half, because the mip filter does not renormalise and a rebuilt Z runs
# systematically over the stored one down the chain (20.6 codes by mip 5).
TEXCOMP_DATA_RMSE_MAX = 0.025
# DXT on the hue wheel, which is its worst case by construction. Measured 0.0331,
# so this bounds it rather than blessing any value: the point of the arm is that
# the number stays in the range a human already looked at and accepted.
TEXCOMP_COLOUR_RMSE_MAX = 0.050
# ...and a FLOOR under the same number, which is the anti-vacuity half. DXT that
# silently failed to arm would read 0.0000 and pass a ceiling-only arm.
TEXCOMP_COLOUR_RMSE_MIN = 0.010
# Bytes the fixture's three maps must fall to. 1.833 MB compressed against
# 3.000 MB raw; the bar sits between, so either a lost format or a lost saving
# moves it.
TEXCOMP_MB_MAX = 2.4


def _texcomp_rmse(a, b):
    """RMSE as a 0..1 fraction, which is what an error budget is stated in."""
    r = subprocess.run(["magick", "compare", "-metric", "RMSE", a, b, "null:"],
                       capture_output=True, text=True)
    out = (r.stderr or r.stdout).strip()
    try:
        return float(out.split("(")[1].rstrip(")"))
    except (ValueError, IndexError):
        return 1.0


def run_texcomp_gate(workdir):
    """Block-compressed textures (spec 11.85), on the one fixture that minifies:

      texcomp-off       --no-texture-compression is byte-identical to a run with
                        every format declined -- 0 px, the bisect lever. It is
                        provable here where the error arms are not, because
                        declining a format is not an approximation of it.
      texcomp-format    the probe shows BC5 on the normal and BC4 on the single-
                        channel mask, and the ledger falls below the bar. Reading
                        the FORMAT and not just the byte count is what separates
                        "compressed" from "compressed as the wrong thing" -- a
                        normal stored DXT5 also shrinks, and also renders.
      texcomp-data      BC5 + BC4 cost less than the stated budget against their
                        own uncompressed twin. The only item in this roadmap
                        whose bar cannot be 0 px, so the budget IS the test.
      texcomp-colour    DXT on albedo is OFF by default (0 px against a
                        --no-texture-compression twin over the two data formats
                        alone is NOT the test -- see below), moves when asked
                        for, and stays inside its own budget. Bounded ABOVE and
                        BELOW: a build where the colour switch silently did
                        nothing reads 0.0000 and would pass a ceiling alone.
      texcomp-config    a --texture-compress-colour session restores from its own
                        snapshot at 0 px. Its own arm because the failure is
                        silent: the switches are a one-shot at LOAD, so a
                        descriptor-table row would be stored, read back and
                        refused by every consumer with the textures already
                        uploaded -- the config-clouds shape, and spec 11.71's
                        claim is that the JSON reproduces the pixels.

    WHY THIS FIXTURE AND NOT ANY OTHER. Every golden in the corpus is 0 px with
    every mip level painted solid black, and so is the raiden recipe -- they sit
    near the camera and never minify, so none of them samples a level above 0.
    Most of a compressed texture's bytes are in the chain. This plane recedes to
    its vanishing point, which is what makes the chain reachable at all.
    """
    scene = asset(TEXCOMP_FIXTURE)
    if not os.path.exists(scene):
        print(f"  texcomp-off SKIP  ({TEXCOMP_FIXTURE} not present)")
        return []
    failures = []

    shots = {}
    for name, extra in (("on", []), ("raw", ["--no-texture-compression"]),
                        ("colour", ["--texture-compress-colour"])):
        shots[name] = os.path.join(workdir, f"texcomp_{name}.ppm")
        err = render(scene, shots[name], extra, frames=TEXCOMP_FRAMES)
        if err:
            print(f"  texcomp-off ERROR render failed: {err.strip()[-200:]}")
            return ["texcomp render"]

    # texcomp-off: the lever reaches the uncompressed frame exactly.
    raw = _texcomp_probe(workdir, ["--no-texture-compression"])
    if raw is None:
        print("  texcomp-off    ERROR the --no-texture-compression render failed")
        return ["texcomp-off"]
    raw_mb, raw_formats, raw_ncomp = raw
    # All three counts, not just the zero: a render that died prints no ledger at
    # all, and `0 == 0` is satisfied by nothing having happened.
    ok = raw_ncomp == 0 and len(raw_formats) == 3 and raw_mb > 0.0
    print(f"  texcomp-off    {'PASS' if ok else 'FAIL'}  --no-texture-compression stores "
          f"{raw_ncomp} compressed of any format (want 0) across {len(raw_formats)} formats "
          f"(want 3, or the render never drew), ledger {raw_mb:.3f} MB (want > 0)")
    if not ok:
        failures.append("texcomp-off")

    # texcomp-format: the right format on the right map, and the saving landed.
    got = _texcomp_probe(workdir, [])
    if got is None:
        print("  texcomp-format ERROR the default render failed")
        return failures + ["texcomp-format"]
    mb, formats, ncomp = got
    ok = "BC5" in formats and "BC4" in formats and ncomp == 2 and mb <= TEXCOMP_MB_MAX
    print(f"  texcomp-format {'PASS' if ok else 'FAIL'}  formats={sorted(formats)} "
          f"compressed={ncomp} (want BC5 and BC4, 2 of 3), ledger {mb:.3f} MB "
          f"(want <= {TEXCOMP_MB_MAX}, raw reads {raw_mb:.3f})")
    if not ok:
        failures.append("texcomp-format")

    # texcomp-data: the error budget for the two formats that are on by default.
    data_rmse = _texcomp_rmse(shots["raw"], shots["on"])
    ok = data_rmse <= TEXCOMP_DATA_RMSE_MAX
    print(f"  texcomp-data   {'PASS' if ok else 'FAIL'}  BC5+BC4 RMSE {data_rmse:.4f} of full "
          f"scale (want <= {TEXCOMP_DATA_RMSE_MAX}), about "
          f"{data_rmse * 255.0:.1f} of one 8-bit code")
    if not ok:
        failures.append("texcomp-data")

    # texcomp-colour: off by default, live when asked, and inside its budget.
    default_moved, _ = compare(shots["on"], shots["raw"])
    colour_rmse = _texcomp_rmse(shots["on"], shots["colour"])
    got_colour = _texcomp_probe(workdir, ["--texture-compress-colour"])
    if got_colour is None:
        print("  texcomp-colour ERROR the colour render failed")
        return failures + ["texcomp-colour"]
    _, colour_formats, colour_ncomp = got_colour
    # The sRGB spelling and ONLY it. A linear DXT1 on an sRGB albedo shrinks
    # and renders, and is one enum away from the decode bug 50fd0f1d fixed --
    # accepting both spellings is accepting the one that is wrong.
    colour_armed = "DXT1-srgb" in colour_formats and "DXT1" not in colour_formats
    ok = (colour_armed and colour_ncomp == 3
          and TEXCOMP_COLOUR_RMSE_MIN <= colour_rmse <= TEXCOMP_COLOUR_RMSE_MAX)
    print(f"  texcomp-colour {'PASS' if ok else 'FAIL'}  default leaves albedo uncompressed "
          f"({default_moved} px is BC5+BC4 alone); --texture-compress-colour arms "
          f"{sorted(colour_formats)} at {colour_ncomp} of 3 and costs RMSE {colour_rmse:.4f} "
          f"(want {TEXCOMP_COLOUR_RMSE_MIN} to {TEXCOMP_COLOUR_RMSE_MAX})")
    if not ok:
        failures.append("texcomp-colour")


    # texcomp-config: the snapshot carries the storage choice.
    #
    # Its own arm because the failure is SILENT and total -- the settings are a
    # one-shot at load, so a row in the descriptor table would be stored, read
    # back, and refused by every consumer with the textures already uploaded.
    # That is the config-clouds failure spec 11.72 found once; this is the same
    # shape, and spec 11.71's whole claim is that the JSON reproduces the pixels.
    cfg = os.path.join(workdir, "texcomp_cfg.json")
    flag_shot = os.path.join(workdir, "texcomp_cfg_flag.ppm")
    rest_shot = os.path.join(workdir, "texcomp_cfg_restored.ppm")
    base = [RENDER, "-m", scene, "-x", "-f", str(TEXCOMP_FRAMES), "-W", "300", "-H", "220"]
    r1 = _run(base + ["--texture-compress-colour", "--config-dump", cfg, "-S", flag_shot],
              capture_output=True, text=True)
    r2 = _run([RENDER, "--config", cfg, "-x", "-f", str(TEXCOMP_FRAMES), "-S", rest_shot],
              capture_output=True, text=True)
    if r1.returncode != 0 or r2.returncode != 0 or not os.path.exists(rest_shot):
        print("  texcomp-config ERROR the dump/restore pair failed to render")
        failures.append("texcomp-config")
    else:
        moved, _ = compare(flag_shot, rest_shot)
        ok = moved == 0
        print(f"  texcomp-config {'PASS' if ok else 'FAIL'}  a --texture-compress-colour session "
              f"restores from its snapshot alone at {moved} px (want 0; without the source-block "
              f"rows the restore silently renders uncompressed)")
        if not ok:
            failures.append("texcomp-config")

    return failures
