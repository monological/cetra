#!/usr/bin/env python3
"""A fire flipbook (spec 13.14) from the frames a bake rendered: the one writer of the sheet and
sidecar the engine's fire_set_flipbook reads.

    python3 tools/fire_flipbook.py out/fire_blender [--name fire_hearth] [--publish]

Reads the bake's manifest.json and frames.npy -- premultiplied RGBA in linear light, rows bottom
first, as tools/bake_fire_blender.py exports them -- and:

  - crossfades the last --loop frames into the first, so the loop is seamless;
  - scales the frames to --width, averaging in linear light;
  - calls the brightest texel (the 99.95th percentile, so one hot pixel does not set it)
    --peak-nits: a render does not know what its units are in nits, so this is the calibration,
    and a wood flame's luminance is of the order of 1e3-1e4;
  - packs the frames into one sheet, frame k at column k % cols of row k // cols, row 0 at the
    bottom -- the engine uploads a PNG's first row at v = 0 -- the colour sRGB-encoded against
    the peak and the alpha the smoke's coverage;
  - writes a sidecar naming the sheet, with the layout, the fps, the peak, the size a frame spans
    in metres, and each frame's luminous intensity at that size (its luminance summed over each
    pixel's area) and centroid height, and the loop's colour, all of it from what the sheet holds.

Writes to out/fire_flipbook/; --publish writes into the asset tree instead, where the fire
fixture reads it.
"""

import argparse
import json
import math
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "out", "fire_flipbook")
# The one statement of which directory an asset kind lives in.
sys.path.insert(0, os.path.join(ROOT, "assets", "generators"))
from fixture_paths import asset_path  # noqa: E402

LUMA = np.array([0.2126, 0.7152, 0.0722])


def srgb_encode(linear):
    linear = np.clip(linear, 0.0, 1.0)
    return np.where(linear <= 0.0031308, 12.92 * linear,
                    1.055 * np.power(linear, 1.0 / 2.4) - 0.055)


def crossfade(frames, count, loop):
    """The first `count` frames, the `loop` after them faded into the first `loop`."""
    out = np.array(frames[:count], dtype=np.float64)
    for i in range(loop):
        a = (i + 0.5) / loop
        out[i] = frames[count + i] * (1.0 - a) + frames[i] * a
    return out


def scale(frames, width):
    """Every frame at `width` pixels across, each channel box-filtered, in linear light."""
    n, h, w, c = frames.shape
    if width >= w:
        return frames
    height = int(round(h * width / w))
    out = np.empty((n, height, width, c))
    for k in range(n):
        for ch in range(c):
            img = Image.fromarray(frames[k, :, :, ch].astype(np.float32), "F")
            out[k, :, :, ch] = np.asarray(img.resize((width, height), Image.BOX))
    return out


def pack(frames, cols, rows):
    """Frame k at column k % cols and row k // cols, rows counted from the array's first."""
    count, h, w, c = frames.shape
    sheet = np.zeros((rows * h, cols * w, c), dtype=frames.dtype)
    for k in range(count):
        y, x = (k // cols) * h, (k % cols) * w
        sheet[y:y + h, x:x + w] = frames[k]
    return sheet


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("bake", help="a directory holding a bake's manifest.json and frames.npy")
    ap.add_argument("--name", default="fire_hearth")
    ap.add_argument("--loop", type=int, default=16,
                    help="frames crossfaded into the first: the bake's last, past the loop")
    ap.add_argument("--width", type=int, default=256, help="a frame's width in the sheet, pixels")
    ap.add_argument("--peak-nits", type=float, default=4000.0,
                    help="the luminance the brightest texel stands for")
    ap.add_argument("--publish", action="store_true", help="write into the asset tree")
    args = ap.parse_args()

    with open(os.path.join(args.bake, "manifest.json")) as f:
        manifest = json.load(f)
    frames = np.load(os.path.join(args.bake, "frames.npy")).astype(np.float64)
    count = len(frames) - args.loop
    if count < 1:
        sys.exit(f"{len(frames)} frames leave none past a loop of {args.loop}")
    frames = scale(crossfade(frames, count, args.loop), args.width)
    n, h, w, _ = frames.shape

    peak = max(float(np.percentile(frames[..., :3].max(axis=-1), 99.95)), 1e-9)
    # What the sheet holds, in nits: the light is measured from this, so it is the light drawn.
    nits = np.clip(frames[..., :3] / peak * args.peak_nits, 0.0, args.peak_nits)
    alpha = np.clip(frames[..., 3], 0.0, 1.0)

    metres = manifest["metres"]
    pixel_area = (metres[0] / w) * (metres[1] / h)
    luminance = nits @ LUMA
    intensity = luminance.sum(axis=(1, 2)) * pixel_area
    heights = (np.arange(h) + 0.5) / h  # rows bottom first
    row_lum = luminance.sum(axis=2)
    centroid = (row_lum * heights[None, :]).sum(axis=1) / np.maximum(row_lum.sum(axis=1), 1e-12)
    total = nits.sum(axis=(0, 1, 2))
    color = total / max(float(total @ LUMA), 1e-12)

    rgb8 = np.round(srgb_encode(nits / args.peak_nits) * 255.0)
    a8 = np.round(alpha * 255.0)
    rgba8 = np.concatenate([rgb8, a8[..., None]], axis=-1).astype(np.uint8)
    cols = int(math.ceil(math.sqrt(n)))
    rows = int(math.ceil(n / cols))
    sheet_name, sidecar_name = f"{args.name}_color.png", f"{args.name}.json"
    if args.publish:
        sheet_path, sidecar_path = asset_path(sheet_name), asset_path(sidecar_name)
    else:
        os.makedirs(OUT, exist_ok=True)
        sheet_path, sidecar_path = os.path.join(OUT, sheet_name), os.path.join(OUT, sidecar_name)
    Image.fromarray(pack(rgba8, cols, rows), "RGBA").save(sheet_path, optimize=True)

    sidecar = {
        "_comment": "Made by tools/fire_flipbook.py (spec 13.14). The sheet is stored bottom row "
                    "first.",
        "source": manifest["source"],
        "credit": manifest["credit"],
        "license": manifest["license"],
        "sheet": sheet_name,
        "frames": n,
        "cols": cols,
        "rows": rows,
        "width": w,
        "height": h,
        "fps": manifest["fps"],
        "peak_nits": args.peak_nits,
        "box": [round(float(v), 6) for v in metres],
        "intensity": [round(float(v), 6) for v in intensity],
        "centroid_y": [round(float(v), 6) for v in centroid],
        "color": [round(float(c), 6) for c in color],
    }
    with open(sidecar_path, "w") as f:
        json.dump(sidecar, f, indent=1)
        f.write("\n")
    print(f"wrote {sheet_path} and {sidecar_path}: {n} frames of {w}x{h}, "
          f"intensity {intensity.mean():.1f} cd mean at {metres[0]:g} m wide")


if __name__ == "__main__":
    main()
