#!/usr/bin/env python3
"""Bake a fire flipbook (spec 13.14) from a bake scene, and encode it as the engine reads it.

    python3 tools/bake_fire_flipbook.py assets/scenes/fire_bake_logs.cscn fire_logs

Runs the render app headless on the scene with --fire-bake, which marches the scene's first
grid fire from in front of its box every --stride frames and writes raw float frames: colour in
absolute nits, adapted and premultiplied by coverage, and the gas's motion across the image in
pixels a frame. This script then

  - crossfades the last --loop frames into the first, so --frames frames play as a seamless
    loop: frame i < loop is the baked frame frames + i faded into frame i;
  - packs them into a sheet, colour normalised by the sheet's 99.95th-percentile channel and
    sRGB-encoded, alpha linear, both RGBA8 -- `<name>_color.png`;
  - encodes the motion into a second sheet, red and green about 128 over +-motion_range pixels
    -- `<name>_motion.png`;
  - writes `<name>.json`: the layout, the fps, the peak in nits, the motion range, the bake's
    box, and each frame's intensity in cd, centroid height and colour, crossfaded like the
    frames.

Both sheets are stored BOTTOM ROW FIRST, as the bake read them back, because the engine uploads
a PNG's first row as v = 0. They look upside down in an image viewer; `--preview` writes a
right-way-up copy of the colour sheet beside them.

Deterministic: the bake is a pure function of the scene and the frame count, so a second run
writes the same files.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RENDER = os.path.join(ROOT, "out", "bin", "render")
OUT_DIR = os.path.join(ROOT, "assets", "textures")


def read_layout(path):
    """bake.txt: one key and its values a line."""
    layout = {}
    with open(path) as f:
        for line in f:
            key, *values = line.split()
            layout[key] = [float(v) for v in values]
    return layout


def crossfade(frames, count, loop):
    """`count` frames from `count + loop`, the extra `loop` faded into the first `loop`."""
    out = np.array(frames[:count], dtype=np.float64)
    for i in range(loop):
        a = (i + 0.5) / loop
        out[i] = frames[count + i] * (1.0 - a) + frames[i] * a
    return out


def srgb_encode(linear):
    linear = np.clip(linear, 0.0, 1.0)
    return np.where(linear <= 0.0031308, 12.92 * linear,
                    1.055 * np.power(linear, 1.0 / 2.4) - 0.055)


def pack(frames, cols, rows):
    """Frames of (h, w, c) side by side, frame k at column k % cols and row k // cols, row 0 at
    the bottom of the sheet -- the same order the frames are stored in, bottom row first."""
    count, h, w, c = frames.shape
    sheet = np.zeros((rows * h, cols * w, c), dtype=frames.dtype)
    for k in range(count):
        y, x = (k // cols) * h, (k % cols) * w
        sheet[y:y + h, x:x + w] = frames[k]
    return sheet


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("scene")
    ap.add_argument("name")
    ap.add_argument("--frames", type=int, default=64, help="frames in the loop")
    ap.add_argument("--loop", type=int, default=16, help="frames crossfaded into the loop's start")
    ap.add_argument("--size", type=int, default=192, help="frame width in pixels")
    ap.add_argument("--stride", type=int, default=2, help="simulation frames a baked frame")
    ap.add_argument("--preview", action="store_true", help="also write a right-way-up preview")
    args = ap.parse_args()

    raw = tempfile.mkdtemp(prefix="fire_bake_")
    total = args.frames + args.loop
    cmd = [RENDER, "-m", args.scene, "-x", "-f", str(total * args.stride), "-W", "64", "-H", "64",
           "--fire-bake", raw, "--fire-bake-size", str(args.size),
           "--fire-bake-stride", str(args.stride)]
    run = subprocess.run(cmd, capture_output=True, text=True)
    if run.returncode != 0:
        sys.exit(f"bake failed:\n{run.stdout}{run.stderr}")

    layout = read_layout(os.path.join(raw, "bake.txt"))
    w, h = int(layout["width"][0]), int(layout["height"][0])
    data = np.fromfile(os.path.join(raw, "frames.f32"), dtype=np.float32)
    per_frame = w * h * 4 * 2
    if data.size < per_frame * total:
        sys.exit(f"bake wrote {data.size // per_frame} frames, wanted {total}")
    data = data[:per_frame * total].reshape(total, 2, h, w, 4)
    table = np.loadtxt(os.path.join(raw, "frames.txt"))[:total]

    color = crossfade(data[:, 0], args.frames, args.loop)
    motion = crossfade(data[:, 1, :, :, :2], args.frames, args.loop)
    rows_of = crossfade(table, args.frames, args.loop)

    # The colour's scale: its 99.95th-percentile channel, so a few hot texels do not crush the
    # rest of the sheet into the bottom codes.
    peak = float(np.percentile(color[..., :3].max(axis=-1), 99.95))
    peak = max(peak, 1e-6)
    rgb = srgb_encode(color[..., :3] / peak)
    alpha = np.clip(color[..., 3:4], 0.0, 1.0)
    color8 = np.round(np.concatenate([rgb, alpha], axis=-1) * 255.0).astype(np.uint8)

    motion_range = max(float(np.abs(motion).max()), 1e-3)
    enc = np.clip(0.5 + motion / (2.0 * motion_range), 0.0, 1.0)
    motion8 = np.round(np.concatenate([enc, np.zeros_like(enc[..., :1]),
                                       np.ones_like(enc[..., :1])], axis=-1) * 255.0)
    motion8 = motion8.astype(np.uint8)

    cols = int(np.ceil(np.sqrt(args.frames)))
    rows = int(np.ceil(args.frames / cols))
    color_sheet = pack(color8, cols, rows)
    motion_sheet = pack(motion8, cols, rows)
    Image.fromarray(color_sheet, "RGBA").save(os.path.join(OUT_DIR, f"{args.name}_color.png"),
                                              optimize=True)
    Image.fromarray(motion_sheet, "RGBA").save(os.path.join(OUT_DIR, f"{args.name}_motion.png"),
                                               optimize=True)
    if args.preview:
        Image.fromarray(color_sheet[::-1], "RGBA").save(
            os.path.join(raw, f"{args.name}_preview.png"))
        print(f"preview: {os.path.join(raw, args.name + '_preview.png')}")

    meta = {
        "_comment": f"Baked by tools/bake_fire_flipbook.py from {os.path.relpath(args.scene, ROOT)}"
                    f" (spec 13.14). Sheets are stored bottom row first.",
        "frames": args.frames,
        "cols": cols,
        "rows": rows,
        "width": w,
        "height": h,
        "fps": round(1.0 / layout["frame_seconds"][0], 6),
        "peak_nits": round(peak, 6),
        "motion_range": round(motion_range, 6),
        "box": [round(v, 6) for v in layout["box"]],
        "min": [round(v, 6) for v in layout["min"]],
        "intensity": [round(float(r[0]), 6) for r in rows_of],
        "centroid_y": [round(float(r[2]), 6) for r in rows_of],
        "color": [round(float(c), 6) for c in rows_of[:, 4:7].mean(axis=0)],
    }
    with open(os.path.join(OUT_DIR, f"{args.name}.json"), "w") as f:
        json.dump(meta, f, indent=1)
        f.write("\n")
    print(f"wrote {args.name}_color.png, {args.name}_motion.png and {args.name}.json: "
          f"{args.frames} frames of {w}x{h}, peak {peak:.1f} nits, motion +-{motion_range:.2f} px")


if __name__ == "__main__":
    main()
