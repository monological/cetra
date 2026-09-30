"""Make the cards apps/silent pins to its fridge and leaves on its table.

Snapshots, a handwritten list and a typed letter, all packed into one
picture, cards_albedo.png, with its roughness and normal beside it so the
material loads like every other set. Where each card sits in the picture and
how big it is in the world are written to apps/silent/src/cards.h, which the
kitchen places them by -- this file is the one statement of both, so the
header is generated and never edited.

The snapshots are cut from the horizon band of CC0 HDRI previews from Poly
Haven (https://polyhaven.com) and aged as prints that spent years on a fridge
door: faded, gone warm, and given a white border. The list is in Reenie
Beanie, an OFL font from Google Fonts, downloaded and cached rather than
committed; the letter is in Cousine, which ships with Dear ImGui.

Stored BOTTOM ROW FIRST, as fetch_textures.py explains, so V runs up.
Downloads are cached under out/polyhaven_cache. Run from anywhere:

    python3 apps/silent/tools/make_cards.py
"""

import io
import os
import sys
import textwrap
import urllib.request

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT_DIR = os.path.join(ROOT, "assets", "textures", "silent")
HEADER = os.path.join(ROOT, "apps", "silent", "src", "cards.h")
CACHE_DIR = os.path.join(ROOT, "out", "polyhaven_cache")
HDRI = "https://cdn.polyhaven.com/asset_img/primary/%s.png?height=1024"
HAND_FONT = "https://raw.githubusercontent.com/google/fonts/main/ofl/reeniebeanie/ReenieBeanie.ttf"
TYPE_FONT = os.path.join(ROOT, "cetra", "src", "ext", "cimgui", "imgui", "misc", "fonts",
                         "Cousine-Regular.ttf")

ATLAS = 1024
PAD = 6  # pixels between cards, so a mip does not reach its neighbour

# How rough each kind of surface is: prints are glossy, paper is not.
ROUGH_PRINT = 0.35
ROUGH_PAPER = 0.92

PAPER = np.array([0.93, 0.90, 0.80])  # a sheet gone yellow
INK = (38, 46, 92)                    # a ballpoint's blue-black
TYPE_INK = (34, 32, 30)

# Each snapshot: the Poly Haven HDRI, where in its panorama the frame is
# centred (fractions across and down), and how wide a slice of the panorama
# it takes -- a quarter of it is a wide lens.
PHOTOS = [
    ("lake", "lakeside", 0.33, 0.50, 0.26, "landscape"),
    ("park", "autumn_park", 0.55, 0.50, 0.22, "landscape"),
    ("garden", "suburban_garden", 0.50, 0.54, 0.22, "landscape"),
    ("road", "misty_farm_road", 0.29, 0.56, 0.12, "portrait"),
    ("tree", "belfast_farmhouse", 0.12, 0.44, 0.20, "polaroid"),
    ("snow", "snowy_park_01", 0.50, 0.55, 0.22, "landscape"),
]

# Print formats: pixels in the atlas, metres in the world, and the border.
FORMATS = {
    "landscape": {"px": (300, 200), "m": (0.15, 0.10), "border": (10, 10, 10, 10)},
    "portrait": {"px": (200, 300), "m": (0.10, 0.15), "border": (10, 10, 10, 10)},
    # left, top, right, bottom: a Polaroid's wide strip is at the bottom.
    "polaroid": {"px": (220, 268), "m": (0.088, 0.107), "border": (12, 12, 12, 60)},
}

NOTE_LINES = ["milk", "eggs", "bread", "bulbs for the hall", "call about the noise",
              "(it's back)"]

LETTER = """COUNTY WATER DEPARTMENT

FINAL NOTICE

Account No. 0017-4452-09

Dear Resident,

Our records show that the balance on the account for the address below
remains unpaid despite our previous notices. Unless payment in full is
received within ten (10) days of the date of this letter, water service to
the property will be discontinued without further notice.

If you have already paid, please disregard this notice. If you are unable to
pay, you may contact our office during business hours to arrange a payment
plan. Meter readings indicate continued use at the property.

Collections Office"""


def fetch(url, cache_name):
    path = os.path.join(CACHE_DIR, cache_name)
    if not os.path.exists(path):
        req = urllib.request.Request(url, headers={"User-Agent": "cetra-silent/1.0"})
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read()
        with open(path, "wb") as f:
            f.write(data)
    with open(path, "rb") as f:
        return f.read()


def to_image(a):
    return Image.fromarray(np.clip(a * 255.0 + 0.5, 0, 255).astype(np.uint8))


def to_array(img):
    return np.asarray(img.convert("RGB"), dtype=np.float32) / 255.0


def age_paper(a, rng, edge=0.18):
    """Darken toward the edges and lay a faint mottle, as handled paper goes."""
    h, w = a.shape[:2]
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.minimum(np.minimum(x, w - 1 - x) / w, np.minimum(y, h - 1 - y) / h)
    a = a * (1.0 - edge * np.exp(-d / 0.04))[..., None]
    mottle = to_array(Image.fromarray(
        (rng.random((h // 16 + 2, w // 16 + 2)) * 255).astype(np.uint8)).resize(
            (w, h), Image.Resampling.BICUBIC))[..., :1]
    return a * (0.95 + 0.05 * mottle)


def photo(name, hdri, cx, cy, span, fmt, rng):
    """One snapshot: a slice of the panorama, faded warm, in its border."""
    spec = FORMATS[fmt]
    pw, ph = spec["px"]
    left, top, right, bottom = spec["border"]
    iw, ih = pw - left - right, ph - top - bottom
    pano = Image.open(io.BytesIO(fetch(HDRI % hdri, hdri + "_primary.png"))).convert("RGB")
    w = span * pano.width
    h = w * ih / iw
    box = (cx * pano.width - w / 2, cy * pano.height - h / 2,
           cx * pano.width + w / 2, cy * pano.height + h / 2)
    img = to_array(pano.resize((iw, ih), Image.Resampling.LANCZOS, box=box))
    # Faded: contrast down, blacks lifted, the blue gone first, a warm cast.
    img = 0.5 + (img - 0.5) * 0.72
    img = 0.07 + img * 0.9
    img = img * np.array([1.06, 0.98, 0.80])
    grey = img @ np.array([0.2126, 0.7152, 0.0722])
    img = grey[..., None] + (img - grey[..., None]) * 0.7
    yy, xx = np.mgrid[0:ih, 0:iw].astype(np.float32)
    r2 = ((xx / iw - 0.5) ** 2 + (yy / ih - 0.5) ** 2) / 0.5
    img = img * (1.0 - 0.28 * r2)[..., None]
    img = img + rng.normal(0.0, 0.018, img.shape)
    card = np.ones((ph, pw, 3), dtype=np.float32) * np.array([0.95, 0.94, 0.88])
    card[top:top + ih, left:left + iw] = img
    return age_paper(card, rng, edge=0.12), ROUGH_PRINT


def ruled_note(rng):
    """A page off a notepad, ruled, with a list on it in pencil-soft ballpoint."""
    w, h, first, rule = 300, 420, 70, 30
    img = Image.fromarray((np.ones((h, w, 3)) * PAPER * 255).astype(np.uint8))
    draw = ImageDraw.Draw(img)
    for y in range(first, h, rule):
        draw.line([(0, y), (w, y)], fill=(150, 170, 200), width=1)
    draw.line([(40, 0), (40, h)], fill=(210, 130, 130), width=1)
    font = ImageFont.truetype(io.BytesIO(fetch(HAND_FONT, "ReenieBeanie.ttf")), 32)
    for i, line in enumerate(NOTE_LINES):
        # Each line written on its rule, a little crooked, one below the other.
        layer = Image.new("L", (w, 2 * rule), 0)
        ImageDraw.Draw(layer).text((50 + rng.uniform(-3, 5), rule + 2), line, font=font,
                                   fill=255, anchor="ls")
        layer = layer.rotate(rng.uniform(-2.0, 2.0), resample=Image.Resampling.BICUBIC)
        img.paste(Image.new("RGB", layer.size, INK), (0, first + rule * (i + 1) - rule), layer)
    return age_paper(to_array(img), rng), ROUGH_PAPER


def typed_letter(rng):
    """A typed notice, unevenly inked, with a mug's ring on it."""
    w, h = 420, 594
    img = Image.fromarray((np.ones((h, w, 3)) * PAPER * 255).astype(np.uint8))
    font = ImageFont.truetype(TYPE_FONT, 13)
    mask = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(mask)
    y = 50
    for para in LETTER.split("\n\n"):
        for line in textwrap.wrap(" ".join(para.split("\n")), 46) or [""]:
            draw.text((36, y), line, font=font, fill=255)
            y += 18
        y += 16
    ink = np.asarray(mask, dtype=np.float32) / 255.0
    ink *= 0.75 + 0.25 * rng.random(ink.shape)
    a = to_array(img)
    a = a * (1.0 - ink[..., None]) + np.array(TYPE_INK) / 255.0 * ink[..., None]
    # The ring a wet mug leaves: a thin dark edge round a faint stain.
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    r = np.hypot(xx - 300, (yy - 430) * 1.05)
    ring = np.exp(-((r - 52) / 3.0) ** 2) * 0.35 + (r < 52) * 0.06
    a = a * (1.0 - ring[..., None] * np.array([0.35, 0.55, 0.8]))
    return age_paper(a, rng), ROUGH_PAPER


def pack(sizes):
    """Skyline packing: each card at the lowest place it fits, leftmost first."""
    sky = np.zeros(ATLAS, dtype=np.int32)
    spots = []
    for w, h in sizes:
        best = None
        for x in range(0, ATLAS - w + 1, 2):
            y = int(sky[x:x + w].max())
            if best is None or y < best[1]:
                best = (x, y)
        x, y = best
        if y + h > ATLAS:
            raise SystemExit("the cards do not fit a %d atlas" % ATLAS)
        sky[x:x + w] = y + h + PAD
        spots.append((x, y))
    return spots


def write_header(cards, spots):
    lines = [
        "// Generated by apps/silent/tools/make_cards.py -- do not edit; rerun it.",
        "#ifndef _SILENT_CARDS_H_",
        "#define _SILENT_CARDS_H_",
        "",
        "// The cards in cards_albedo.png.",
        "typedef enum {",
    ]
    lines += ["    CARD_%s," % c["name"].upper() for c in cards]
    lines += [
        "    CARD_COUNT",
        "} CardId;",
        "",
        "typedef struct CardSpec {",
        "    float uv[4];   // {u0, v0, u1, v1}, V up",
        "    float size[2]; // metres: width, height",
        "} CardSpec;",
        "",
        "static const CardSpec CARDS[CARD_COUNT] = {",
    ]
    for c, (x, y) in zip(cards, spots):
        h, w = c["pixels"].shape[:2]
        u0, u1 = x / ATLAS, (x + w) / ATLAS
        v0, v1 = 1.0 - (y + h) / ATLAS, 1.0 - y / ATLAS
        lines.append("    [CARD_%s] = {{%.6ff, %.6ff, %.6ff, %.6ff}, {%.4ff, %.4ff}}," % (
            c["name"].upper(), u0, v0, u1, v1, c["m"][0], c["m"][1]))
    lines += ["};", "", "#endif // _SILENT_CARDS_H_", ""]
    with open(HEADER, "w") as f:
        f.write("\n".join(lines))
    print(HEADER)


def main():
    os.makedirs(CACHE_DIR, exist_ok=True)
    rng = np.random.default_rng(1983)
    cards = []
    note, rough = ruled_note(rng)
    cards.append({"name": "note", "pixels": note, "rough": rough, "m": (0.10, 0.14)})
    letter, rough = typed_letter(rng)
    cards.append({"name": "letter", "pixels": letter, "rough": rough, "m": (0.21, 0.297)})
    for name, hdri, cx, cy, span, fmt in PHOTOS:
        pixels, rough = photo(name, hdri, cx, cy, span, fmt, rng)
        cards.append({"name": "photo_" + name, "pixels": pixels, "rough": rough,
                      "m": FORMATS[fmt]["m"]})

    # Tallest first packs tightest; the header keeps the order above.
    order = sorted(range(len(cards)), key=lambda i: -cards[i]["pixels"].shape[0])
    placed = pack([(cards[i]["pixels"].shape[1], cards[i]["pixels"].shape[0]) for i in order])
    spots = [None] * len(cards)
    for i, spot in zip(order, placed):
        spots[i] = spot

    albedo = np.ones((ATLAS, ATLAS, 3), dtype=np.float32) * PAPER * 0.8
    rough = np.full((ATLAS, ATLAS), ROUGH_PAPER, dtype=np.float32)
    for c, (x, y) in zip(cards, spots):
        h, w = c["pixels"].shape[:2]
        albedo[y:y + h, x:x + w] = c["pixels"]
        rough[y:y + h, x:x + w] = c["rough"]
    flip = Image.Transpose.FLIP_TOP_BOTTOM
    to_image(albedo).transpose(flip).save(os.path.join(OUT_DIR, "cards_albedo.png"))
    to_image(np.repeat(rough[..., None], 3, axis=-1)).transpose(flip).save(
        os.path.join(OUT_DIR, "cards_rough.png"))
    flat = np.zeros((ATLAS, ATLAS, 3), dtype=np.uint8) + np.array([128, 128, 255], dtype=np.uint8)
    Image.fromarray(flat).save(os.path.join(OUT_DIR, "cards_normal.png"))
    print(os.path.join(OUT_DIR, "cards_albedo.png"))
    write_header(cards, spots)
    return 0


if __name__ == "__main__":
    sys.exit(main())
