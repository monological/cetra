"""The instruments group: the suite's own measures read what they claim."""

import os
import subprocess
import sys

import numpy as np

from gatelib.images import compare


# The constructed pair: a pattern, and a copy with a known set of pixels moved. 37 move one
# code on one channel, the least compare() must count; 5 move 200 codes on all three, which set
# the peak. 42 in all, and a peak of 200/255.
_SIZE = (48, 64)
_NUDGED = 37
_STRUCK = 5
_STRIKE = 200


def _pattern():
    h, w = _SIZE
    y, x = np.mgrid[0:h, 0:w]
    return np.stack([(x * 3 + y) % 200, (y * 5) % 200, (x + y * 2) % 200], axis=2).astype(np.uint8)


def _write_ppm(path, pix):
    h, w, _ = pix.shape
    with open(path, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (w, h) + pix.tobytes())


def run_instruments_gate(workdir):
    """compare() is the pixel count nearly every arm below reads, so it is held to one first.

      compare-count  a pair built with 42 pixels moved, 37 by one code on one channel and 5 by
                     200 on all three, reads 42 pixels and a peak of 200/255, as PPMs and again
                     as PNGs decoded through magick; the frame against itself reads 0 and 0; a
                     missing frame and a frame of another size read as a full difference.

    It exists because ImageMagick changed what `compare -metric AE` prints and nothing said so.
    7.1.2-35 prints the summed absolute error over the channels, in units of full scale: the
    glare pair that differs in 243,109 pixels read 2,997. compare() took that for a pixel count,
    so every arm asking that an effect move at least so many pixels failed, and every arm asking
    for 0 still passed, since identical frames still read 0. compare() now counts the pixels
    itself and asks magick only to decode, and this arm reads a pair whose answer is known.
    """
    a = _pattern()
    b = a.copy()
    flat = b.reshape(-1, 3)
    order = np.random.default_rng(1438).permutation(flat.shape[0])
    for i in order[:_NUDGED]:
        flat[i, i % 3] += 1
    for i in order[_NUDGED:_NUDGED + _STRUCK]:
        flat[i] = (flat[i].astype(np.int16) + _STRIKE).clip(0, 255).astype(np.uint8)
    want_peak = float(np.abs(a.astype(np.int16) - b.astype(np.int16)).max()) / 255.0
    paths = {}
    for name, pix in (("a", a), ("b", b), ("small", a[:-1])):
        paths[name] = os.path.join(workdir, f"instruments_{name}.ppm")
        _write_ppm(paths[name], pix)
    for name in ("a", "b"):
        png = os.path.join(workdir, f"instruments_{name}.png")
        subprocess.run(["magick", paths[name], png], check=True)
        paths[name + "_png"] = png
    readings = {
        "ppm": compare(paths["a"], paths["b"]),
        "png": compare(paths["a_png"], paths["b_png"]),
        "self": compare(paths["a"], paths["a"]),
        "missing": compare(paths["a"], os.path.join(workdir, "instruments_absent.ppm")),
        "size": compare(paths["a"], paths["small"]),
    }
    want = _NUDGED + _STRUCK
    ok = (all(n == want and abs(p - want_peak) < 1e-6
              for n, p in (readings["ppm"], readings["png"])) and
          readings["self"] == (0, 0.0) and
          readings["missing"] == (sys.maxsize, 1.0) and
          readings["size"] == (sys.maxsize, 1.0))
    shown = ", ".join(f"{k} {n if n != sys.maxsize else 'all'}/{p:.4f}"
                      for k, (n, p) in readings.items())
    print(f"  compare-count {'PASS' if ok else 'FAIL'}  {shown} (want ppm and png {want}/"
          f"{want_peak:.4f}, self 0/0.0000, missing and size all/1.0000)")
    return [] if ok else ["compare-count"]
