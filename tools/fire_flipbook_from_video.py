#!/usr/bin/env python3
"""A fire flipbook (spec 13.14) from filmed fire: real flames, keyed off their background.

    python3 tools/fire_flipbook_from_video.py

Fetches the source video, cuts --frames + --loop frames from it at its own 30 fps, crops and
scales them, and keys the flames out:

  - the key is the GREEN channel, in linear light: a flame is yellow to white and carries green,
    where the red brick it was filmed against carries almost none, so green separates the two
    where brightness would not;
  - the bottom of the crop, where the logs glow as yellow as the flames, fades out over
    --base-fade of the frame's height: in a scene the fire's own logs sit there, and the card's
    flames rise out of them;
  - what is left is pure emission (alpha 0), drawn additively.

Then, as tools/bake_fire_flipbook.py does for a simulated fire: the last --loop frames are
crossfaded into the first so the loop is seamless; the frames are packed into a sheet stored
bottom row first (the engine uploads a PNG's first row as v = 0), normalised by --peak-nits
for the brightest texel and sRGB-encoded; and a JSON sidecar records the layout, the fps, the
peak, the card's physical width, and each frame's luminous intensity in cd at that width -- the
sum of its luminance times each pixel's area -- its centroid height and its colour.

The source and its licence are SOURCE below. Deterministic: ffmpeg decodes the same frames.
"""

import argparse
import json
import os
import subprocess
import sys
import urllib.request

import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT_DIR = os.path.join(ROOT, "assets", "textures")
CACHE = os.path.join(ROOT, "out", "fire_footage")

# The footage. Wikimedia Commons lists it CC BY 3.0, credited to the YouTube channel "free video
# library"; the uploader's own description calls it CC0. It is treated as CC BY and credited.
SOURCE = {
    "url": "https://upload.wikimedia.org/wikipedia/commons/transcoded/b/bb/"
           "Fantastic-fireplace-fire-chimney-hearth-_background_-_texture_-_motion_graphics_-"
           "_free_video_library.webm/Fantastic-fireplace-fire-chimney-hearth-_background_-"
           "_texture_-_motion_graphics_-_free_video_library.webm.1080p.vp9.webm",
    "page": "https://commons.wikimedia.org/wiki/File:Fantastic-fireplace-fire-chimney-hearth-"
            "_background_-_texture_-_motion_graphics_-_free_video_library.webm",
    "credit": "\"fantastic-fireplace-fire-chimney-hearth\" by free video library "
              "(youtube.com/watch?v=aowVUMniV-I), via Wikimedia Commons, CC BY 3.0",
    "license": "CC BY 3.0",
    "file": "fantastic_fireplace_1080p.webm",
}
# The footage's fire, in its own pixels: the log bed and the flames over it.
FOOTAGE_CROP = [360, 140, 1200, 940]

# A fire baked here, which is cetra's own work under its licence.
BAKED = {
    "page": "tools/bake_fire_blender.py",
    "credit": "Simulated and rendered in Blender by cetra's tools/bake_fire_blender.py",
    "license": "MIT, as cetra",
}


def fetch():
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, SOURCE["file"])
    if not os.path.exists(path):
        req = urllib.request.Request(SOURCE["url"], headers={"User-Agent": "cetra-asset-fetch/1.0"})
        with urllib.request.urlopen(req) as r, open(path, "wb") as f:
            f.write(r.read())
    return path


def frame_size(source):
    """A video's or an image sequence's frame size, (w, h)."""
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                            "stream=width,height", "-of", "csv=p=0"] + source[-1:],
                           capture_output=True, text=True, check=True).stdout.strip()
    return tuple(int(v) for v in probe.split(","))


def decode(source, count, crop, width):
    """`count` frames from `source` -- ffmpeg's input arguments -- cropped to (x, y, w, h) and
    scaled to `width`."""
    x, y, w, h = crop
    height = int(round(width * h / w))
    cmd = ["ffmpeg", "-v", "error"] + source + [
           "-vf", f"crop={w}:{h}:{x}:{y},scale={width}:{height}:flags=lanczos",
           "-frames:v", str(count), "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    frames = np.frombuffer(raw, dtype=np.uint8)
    got = frames.size // (width * height * 3)
    if got < count:
        sys.exit(f"decoded {got} frames, wanted {count}")
    return frames[:count * width * height * 3].reshape(count, height, width, 3)


def srgb_decode(c):
    c = c / 255.0
    return np.where(c <= 0.04045, c / 12.92, np.power((c + 0.055) / 1.055, 2.4))


def srgb_encode(linear):
    linear = np.clip(linear, 0.0, 1.0)
    return np.where(linear <= 0.0031308, 12.92 * linear,
                    1.055 * np.power(linear, 1.0 / 2.4) - 0.055)


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def crossfade(frames, count, loop):
    out = np.array(frames[:count], dtype=np.float64)
    for i in range(loop):
        a = (i + 0.5) / loop
        out[i] = frames[count + i] * (1.0 - a) + frames[i] * a
    return out


def pack(frames, cols, rows):
    """Frame k at column k % cols and row k // cols, row 0 at the bottom: the frames are stored
    bottom row first, so is the sheet."""
    count, h, w, c = frames.shape
    sheet = np.zeros((rows * h, cols * w, c), dtype=frames.dtype)
    for k in range(count):
        y, x = (k // cols) * h, (k % cols) * w
        sheet[y:y + h, x:x + w] = frames[k]
    return sheet


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--name", default="fire_hearth")
    ap.add_argument("--start", type=float, default=4.0, help="seconds into the video")
    ap.add_argument("--frames", type=int, default=64)
    ap.add_argument("--loop", type=int, default=16)
    ap.add_argument("--crop", default=None,
                    help="x,y,w,h in source pixels; the footage's fire by default, a "
                         "sequence's whole frame")
    ap.add_argument("--frames-dir", default=None,
                    help="frame_0000.png onward, as tools/bake_fire_blender.py renders them, "
                         "in place of the footage")
    ap.add_argument("--width", type=int, default=256, help="frame width in pixels")
    ap.add_argument("--key", default="0.03,0.25", help="linear green from no flame to all flame")
    ap.add_argument("--base-fade", type=float, default=0.18,
                    help="share of the frame's height at its bottom the flames fade in over")
    ap.add_argument("--peak-nits", type=float, default=4000.0,
                    help="luminance of the brightest texel: a wood flame's order is 1e3-1e4")
    ap.add_argument("--metres", type=float, default=0.9, help="physical width of a card's frame")
    ap.add_argument("--preview", action="store_true")
    args = ap.parse_args()

    if args.frames_dir:
        source = ["-framerate", "30", "-start_number", "0",
                  "-i", os.path.join(args.frames_dir, "frame_%04d.png")]
        origin = BAKED
    else:
        source = ["-ss", str(args.start), "-i", fetch()]
        origin = SOURCE
    if args.crop:
        crop = [int(v) for v in args.crop.split(",")]
    elif args.frames_dir:
        crop = [0, 0, *frame_size(source)]
    else:
        crop = FOOTAGE_CROP
    k0, k1 = (float(v) for v in args.key.split(","))
    total = args.frames + args.loop
    frames = decode(source, total, crop, args.width)
    lin = srgb_decode(frames.astype(np.float64))
    h, w = lin.shape[1:3]

    # The key, and the fade over the logs at the bottom. Rows run top-down from ffmpeg.
    alpha = smoothstep(k0, k1, lin[..., 1])
    rows = (np.arange(h)[::-1] + 0.5) / h  # height above the frame's bottom, 0..1
    alpha *= smoothstep(0.0, args.base_fade, rows)[None, :, None]
    emission = lin * alpha[..., None]
    emission = emission[:, ::-1]  # bottom row first

    emission = crossfade(emission, args.frames, args.loop)
    peak_linear = float(np.percentile(emission.max(axis=-1), 99.95))
    peak_linear = max(peak_linear, 1e-6)
    nits = emission / peak_linear * args.peak_nits

    # What each frame casts face on, at the card's physical width: luminance times pixel area.
    pixel_area = (args.metres / w) ** 2
    luminance = nits @ np.array([0.2126, 0.7152, 0.0722])
    intensity = luminance.sum(axis=(1, 2)) * pixel_area
    heights = (np.arange(h) + 0.5) / h
    row_lum = luminance.sum(axis=2)
    centroid = (row_lum * heights[None, :]).sum(axis=1) / np.maximum(row_lum.sum(axis=1), 1e-12)
    rgb_sum = nits.sum(axis=(0, 1, 2))
    color = rgb_sum / max(float(rgb_sum @ np.array([0.2126, 0.7152, 0.0722])), 1e-12)

    rgb8 = np.round(srgb_encode(nits / args.peak_nits) * 255.0).astype(np.uint8)
    rgba8 = np.concatenate([rgb8, np.zeros_like(rgb8[..., :1])], axis=-1)
    cols = int(np.ceil(np.sqrt(args.frames)))
    rows_n = int(np.ceil(args.frames / cols))
    sheet = pack(rgba8, cols, rows_n)
    Image.fromarray(sheet, "RGBA").save(os.path.join(OUT_DIR, f"{args.name}_color.png"),
                                        optimize=True)
    if args.preview:
        preview = os.path.join(CACHE, f"{args.name}_preview.png")
        Image.fromarray(sheet[::-1, :, :3], "RGB").save(preview)
        print(f"preview: {preview}")

    meta = {
        "_comment": "Made by tools/fire_flipbook_from_video.py (spec 13.14). "
                    "Sheets are stored bottom row first.",
        "source": origin["page"],
        "credit": origin["credit"],
        "license": origin["license"],
        "frames": args.frames,
        "cols": cols,
        "rows": rows_n,
        "width": w,
        "height": h,
        "fps": 30.0,
        "peak_nits": args.peak_nits,
        "motion_range": 0.0,
        "box": [args.metres, round(args.metres * h / w, 6), 0.0],
        "intensity": [round(float(v), 6) for v in intensity],
        "centroid_y": [round(float(v), 6) for v in centroid],
        "color": [round(float(c), 6) for c in color],
    }
    with open(os.path.join(OUT_DIR, f"{args.name}.json"), "w") as f:
        json.dump(meta, f, indent=1)
        f.write("\n")
    print(f"wrote {args.name}_color.png and {args.name}.json: {args.frames} frames of {w}x{h}, "
          f"intensity {intensity.mean():.1f} cd mean at {args.metres} m wide")


if __name__ == "__main__":
    main()
