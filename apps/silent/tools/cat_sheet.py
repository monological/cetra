#!/usr/bin/env python3
"""A sheet of the cat's clips as the engine draws them (spec 13.17).

    python3 apps/silent/tools/cat_sheet.py [--clips walk,sit] [--out out/cat_sheet.png]

Renders each clip of the cat headless through the render app and assets/scenes/cat.cscn, which
turns its coat on, at a quarter, a half, three quarters and the end of its length, and tiles the
frames with the clip's name. Run from the checkout whose build it should use: the render app is
./out/bin/render beside it.
"""

import argparse
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

from glb import clips as clips_in

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
RENDER = os.path.join(ROOT, "out", "bin", "render")
# The scene rather than the bare glb: it is what turns the coat on.
SCENE = os.path.join(ROOT, "assets", "scenes", "cat.cscn")
GLB = os.path.join(ROOT, "assets", "models", "cat.glb")
STEPS = 60  # the render app's fixed clock: frames a second
PHASES = 4
TILE = (360, 240)


# Where the camera stands, model space, the cat's travel aside: x its left, y up, z forward.
VIEWS = {"side": (0.85, 0.2, 0.0), "threeq": (0.55, 0.32, 0.58), "front": (0.0, 0.22, 0.8),
         "top": (0.0, 0.9, 0.01)}


def render(name, seconds, travel, view, work):
    every = max(1, round(seconds * STEPS / PHASES))
    base = os.path.join(work, f"{name}_{view}.ppm")
    # Framed on the middle of whatever ground the clip covers, so a walk stays in shot.
    mid = travel / 2.0
    ex, ey, ez = VIEWS[view]
    cmd = [RENDER, "-m", SCENE, "--anim-clip", name, "-x", "-f", str(every * PHASES),
           "--screenshot-every", str(every), "--cam-eye", f"{ex},{ey},{ez + mid}",
           "--cam-target", f"0,0.14,{mid - 0.05}", "-W", "480", "-H", "320", "-S", base]
    subprocess.run(cmd, cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    stem = base[:-4]
    return [f"{stem}_{every * k:06d}.ppm" for k in range(1, PHASES + 1)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--clips", default="", help="comma-separated; every clip when empty")
    ap.add_argument("--view", default="threeq", choices=sorted(VIEWS))
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "cat_sheet.png"))
    args = ap.parse_args()
    clips = clips_in(GLB)
    if args.clips:
        want = args.clips.split(",")
        missing = [c for c in want if c not in {n for n, _, _ in clips}]
        if missing:
            sys.exit(f"no clip named {', '.join(missing)} in {GLB}")
        clips = [c for c in clips if c[0] in want]
    rows = []
    with tempfile.TemporaryDirectory() as work:
        for name, seconds, travel in clips:
            frames = render(name, seconds, travel, args.view, work)
            row = Image.new("RGB", (TILE[0] * PHASES, TILE[1]))
            for k, path in enumerate(frames):
                img = Image.open(path).convert("RGB")
                img.thumbnail(TILE)
                row.paste(img, (k * TILE[0], 0))
            ImageDraw.Draw(row).text((8, 6), f"{name}  {seconds:.2f} s", fill=(235, 220, 120))
            rows.append(row)
            print(f"{name}: {len(frames)} frames", flush=True)
    sheet = Image.new("RGB", (TILE[0] * PHASES, TILE[1] * len(rows)))
    for i, row in enumerate(rows):
        sheet.paste(row, (0, i * TILE[1]))
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    sheet.save(args.out)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
