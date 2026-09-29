"""Fetch the photo textures apps/silent uses and cut them down to size.

Every surface is a CC0 set from Poly Haven (https://polyhaven.com) or, where
Poly Haven has no scan of it, ambientCG (https://ambientcg.com), taken at 1k
and reduced to 256 square: the low, even texel density the whole app is
built around. Some are regraded on the way in, and each change is listed in
SOURCES so the committed files can be rebuilt exactly.

Every map is stored BOTTOM ROW FIRST. The engine uploads a PNG's first row as
v = 0 and apps/silent's kit points V up every wall, so a picture stored the
usual way round would hang upside down, and its normal map's slopes with it.
Flipping the rows is the whole correction: +Y in an OpenGL normal map means
"toward the top of the picture", which after the flip is +V.

Downloads are cached under out/polyhaven_cache (the build tree, gitignored);
the results land in assets/textures/silent/. Run from anywhere:

    python3 apps/silent/tools/fetch_textures.py [--sheet PATH]
"""

import argparse
import io
import json
import os
import sys
import urllib.request
import zipfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT_DIR = os.path.join(ROOT, "assets", "textures", "silent")
CACHE_DIR = os.path.join(ROOT, "out", "polyhaven_cache")
SIZE = 256
API = "https://api.polyhaven.com/files/%s"
INFO = "https://api.polyhaven.com/info/%s"
FONT = "/System/Library/Fonts/Supplemental/Arial.ttf"

# Poly Haven id -> what is done to it. `saturation` scales chroma (1 = as
# shot); `gain` scales the albedo; `rotate` turns the set a quarter turn
# clockwise, normal map included.
SOURCES = {
    "grey_plaster_02": {},
    "white_plaster_rough_02": {"saturation": 0.5},
    "worn_tile_floor": {"gain": 1.6},
    "rusty_metal_02": {"saturation": 0.3},
    "concrete_wall_003": {"saturation": 0.35},
    "metal_plate_02": {},
    "wood_table_worn": {},
    "old_wood_floor": {},
    "white_planks_clean": {"rotate": True},
    "blue_painted_planks": {},
    "grass_ground": {},
    "asphalt_02": {},
    "concrete_pavement": {},
    "roof_slates_02": {},
    "brick_wall_006": {},
    "dirty_carpet": {},
    "fabric_pattern_05": {"diffuse": "col_01"},  # ships colourways instead of one Diffuse
    # From ambientCG (https://ambientcg.com), also CC0: what Poly Haven has no
    # scan of -- brushed stainless, paper, and a kitchen smear for the glass.
    "Metal009": {"source": "ambientcg"},
    "Paper003": {"source": "ambientcg"},
    "Smear008": {"source": "ambientcg"},
}

ACG_ZIP = "https://ambientcg.com/get?file=%s_1K-JPG.zip"
# ambientCG's map names for the three this app keeps.
ACG_MAPS = {"albedo": "Color", "normal": "NormalGL", "rough": "Roughness"}


def fetch(url):
    name = url.rsplit("/", 1)[-1]
    path = os.path.join(CACHE_DIR, name)
    if not os.path.exists(path):
        req = urllib.request.Request(url, headers={"User-Agent": "cetra-silent/1.0"})
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read()
        with open(path, "wb") as f:
            f.write(data)
    with open(path, "rb") as f:
        return f.read()


def fetch_json(url, name):
    path = os.path.join(CACHE_DIR, name)
    if not os.path.exists(path):
        with open(path, "wb") as f:
            f.write(fetch_raw(url))
    with open(path) as f:
        return json.load(f)


def fetch_raw(url):
    req = urllib.request.Request(url, headers={"User-Agent": "cetra-silent/1.0"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def open_ambientcg(name):
    """The maps an ambientCG 1K zip holds, by this app's names; missing ones absent."""
    data = fetch(ACG_ZIP % name)
    out = {}
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for kind, suffix in ACG_MAPS.items():
            member = "%s_1K-JPG_%s.jpg" % (name, suffix)
            if member in z.namelist():
                out[kind] = Image.open(io.BytesIO(z.read(member)))
    return out


def open_map(files, key):
    url = files[key]["1k"]["jpg"]["url"]
    return Image.open(io.BytesIO(fetch(url)))


def albedo(img, spec):
    rgb = np.asarray(img.convert("RGB"), dtype=np.float32) / 255.0
    sat = spec.get("saturation", 1.0)
    if sat != 1.0:
        grey = rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
        rgb = grey[..., None] + (rgb - grey[..., None]) * sat
    rgb *= spec.get("gain", 1.0)
    out = Image.fromarray(np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8))
    if spec.get("rotate"):
        out = out.transpose(Image.Transpose.ROTATE_270)
    return out.resize((SIZE, SIZE), Image.Resampling.LANCZOS)


def normal(img, spec):
    n = np.asarray(img.convert("RGB"), dtype=np.float32) / 127.5 - 1.0
    if spec.get("rotate"):
        # Turning the picture a quarter clockwise turns every slope with it:
        # what pointed along +x now points along -y, and +y along +x.
        n = np.rot90(n, k=-1)
        n = np.stack([n[..., 1], -n[..., 0], n[..., 2]], axis=-1)
    small = np.asarray(
        Image.fromarray(np.clip((n + 1.0) * 127.5 + 0.5, 0, 255).astype(np.uint8)).resize(
            (SIZE, SIZE), Image.Resampling.LANCZOS),
        dtype=np.float32) / 127.5 - 1.0
    # Averaging normals shortens them; put them back on the sphere.
    small /= np.maximum(np.linalg.norm(small, axis=-1, keepdims=True), 1e-6)
    return Image.fromarray(np.clip((small + 1.0) * 127.5 + 0.5, 0, 255).astype(np.uint8))


def rough(img, spec):
    out = img.convert("L")
    if spec.get("rotate"):
        out = out.transpose(Image.Transpose.ROTATE_270)
    # Grey in all three channels: the engine reads a roughness map's GREEN
    # (glTF's packing), and a one-channel file has none, which reads as a mirror.
    return out.resize((SIZE, SIZE), Image.Resampling.LANCZOS).convert("RGB")


def write_sheet(path, names):
    cols = 6
    label_h = 18
    rows = (len(names) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * SIZE, rows * (SIZE + label_h)), (20, 20, 20))
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.truetype(FONT, 13) if os.path.exists(FONT) else ImageFont.load_default()
    for i, name in enumerate(names):
        x, y = (i % cols) * SIZE, (i // cols) * (SIZE + label_h)
        img = Image.open(os.path.join(OUT_DIR, name + "_albedo.png"))
        sheet.paste(img.transpose(Image.Transpose.FLIP_TOP_BOTTOM), (x, y))
        draw.text((x + 4, y + SIZE + 2), name, fill=(220, 220, 220), font=font)
    sheet.save(path)
    print(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sheet", help="also write every albedo to one labelled PNG")
    args = parser.parse_args()
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(CACHE_DIR, exist_ok=True)
    for name, spec in SOURCES.items():
        if spec.get("source") == "ambientcg":
            raw = open_ambientcg(name)
            maps = {}
            if "albedo" in raw:
                maps["albedo"] = albedo(raw["albedo"], spec)
            if "normal" in raw:
                maps["normal"] = normal(raw["normal"], spec)
            if "rough" in raw:
                maps["rough"] = rough(raw["rough"], spec)
            print("%-24s ambientCG, maps: %s" % (name, ", ".join(sorted(maps))))
        else:
            files = fetch_json(API % name, name + "_files.json")
            info = fetch_json(INFO % name, name + "_info.json")
            colour = open_map(files, spec.get("diffuse", "Diffuse"))
            maps = {
                "albedo": albedo(colour, spec),
                "normal": normal(open_map(files, "nor_gl"), spec),
                "rough": rough(open_map(files, "Rough"), spec),
            }
            dims = info.get("dimensions") or [0, 0]
            print("%-24s %.2f x %.2f m" % (name, dims[0] / 1000.0, dims[1] / 1000.0))
        for kind, img in maps.items():
            img = img.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
            img.save(os.path.join(OUT_DIR, "%s_%s.png" % (name, kind)))
    if args.sheet:
        write_sheet(args.sheet, [n for n in SOURCES
                                 if os.path.exists(os.path.join(OUT_DIR, n + "_albedo.png"))])
    return 0


if __name__ == "__main__":
    sys.exit(main())
