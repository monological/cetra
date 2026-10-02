"""Make the pictures apps/silent places whole rather than tiles.

Snapshots, a handwritten list and a typed letter for the kitchen, and the hall
clock's painted dial, its marquetry base panel and its fretted frieze, all
packed into one picture, cards_albedo.png, with its roughness and normal
beside it so the material loads like every other set. Where each card sits in
the picture and how big it is in the world are written to
apps/silent/src/cards.h, which the kitchen and the clock place them by -- this
file is the one statement of both, so the header is generated and never
edited.

The snapshots are cut from the horizon band of CC0 HDRI previews from Poly
Haven (https://polyhaven.com) and aged as prints that spent years on a fridge
door: faded, gone warm, and given a white border. The clock's woods are the
Poly Haven veneers its case is made of, at 1k. The list is in Reenie Beanie
and the dial in IM Fell English, OFL fonts from Google Fonts, downloaded and
cached rather than committed; the letter is in Cousine, which ships with Dear
ImGui.

Stored BOTTOM ROW FIRST, as fetch_textures.py explains, so V runs up.
Downloads are cached under out/polyhaven_cache. Run from anywhere:

    python3 apps/silent/tools/make_cards.py
"""

import io
import math
import os
import sys
import textwrap

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from fetch_textures import CACHE_DIR, ROOT, fetch, open_polyhaven

OUT_DIR = os.path.join(ROOT, "assets", "textures", "silent")
HEADER = os.path.join(ROOT, "apps", "silent", "src", "cards.h")
HDRI = "https://cdn.polyhaven.com/asset_img/primary/%s.png?height=1024"
FONTS = "https://raw.githubusercontent.com/google/fonts/main/ofl/"
HAND_FONT = FONTS + "reeniebeanie/ReenieBeanie.ttf"
SERIF = FONTS + "imfellenglish/IMFeENrm28P.ttf"
SERIF_ITALIC = FONTS + "imfellenglish/IMFeENit28P.ttf"
TYPE_FONT = os.path.join(ROOT, "cetra", "src", "ext", "cimgui", "imgui", "misc", "fonts",
                         "Cousine-Regular.ttf")

ATLAS_W, ATLAS_H = 2048, 1024  # wide rather than square: the cards fill a band
PAD = 6  # pixels between cards, so a mip does not reach its neighbour

# How rough each kind of surface is: prints are glossy, paper is not, and the
# clock's paint and veneer sit under old varnish between the two.
ROUGH_PRINT = 0.35
ROUGH_PAPER = 0.92
ROUGH_ENAMEL = 0.45
ROUGH_LACQUER = 0.4
ROUGH_SCALE = 4  # the roughness map is this much smaller than the picture

# The clock's art, in pixels a metre: fine enough to read the dial from a step
# away through its glass.
CLOCK_PX_PER_M = 1600

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


def clock_px(metres):
    return int(round(metres * CLOCK_PX_PER_M))


def veneer(name, w, h, turn=False):
    """A w x h patch of one of the case's Poly Haven veneers at 1k, grain
    along its length, or across it when turned."""
    img = open_polyhaven(name, {})[0]["albedo"].convert("RGB")
    if turn:
        img = img.transpose(Image.Transpose.ROTATE_90)
    a = to_array(img)
    reps = (h // a.shape[0] + 1, w // a.shape[1] + 1, 1)
    return np.tile(a, reps)[:h, :w]


def paint_text(img, xy, text, font, fill, angle=0.0):
    """Text centred on xy, turned `angle` degrees clockwise."""
    box = font.getbbox(text)
    tw, th = box[2] - box[0], box[3] - box[1]
    layer = Image.new("L", (tw + 8, th + 8), 0)
    ImageDraw.Draw(layer).text((4 - box[0], 4 - box[1]), text, font=font, fill=255)
    layer = layer.rotate(-angle, resample=Image.Resampling.BICUBIC, expand=True)
    img.paste(Image.new("RGB", layer.size, fill),
              (int(xy[0] - layer.width / 2), int(xy[1] - layer.height / 2)), layer)


def painted_rose(draw, cx, cy, s, rng):
    """A spandrel rose as a dial painter did them: leaves, then petals round a
    dark heart."""
    for k in range(4):
        a = k * math.pi / 2 + math.pi / 4 + rng.uniform(-0.3, 0.3)
        lx, ly = cx + math.cos(a) * s * 1.1, cy + math.sin(a) * s * 1.1
        draw.ellipse([lx - s * 0.55, ly - s * 0.3, lx + s * 0.55, ly + s * 0.3],
                     fill=(74, 96, 52))
    for k in range(7):
        a = k * 2 * math.pi / 7
        px, py = cx + math.cos(a) * s * 0.45, cy + math.sin(a) * s * 0.45
        draw.ellipse([px - s * 0.5, py - s * 0.5, px + s * 0.5, py + s * 0.5],
                     fill=(178, 58, 52), outline=(120, 30, 30))
    draw.ellipse([cx - s * 0.4, cy - s * 0.4, cx + s * 0.4, cy + s * 0.4], fill=(140, 36, 36))
    draw.ellipse([cx - s * 0.15, cy - s * 0.15, cx + s * 0.15, cy + s * 0.15], fill=(90, 20, 20))


def clock_dial(rng):
    """The hood's painted break-arch dial, seen through the rosewood mask its
    door frames: a square dial with a round arch over it, Roman hours set
    radially, a minute track, a seconds ring and a date, roses in the four
    spandrels, and in the arch a moon's face in a starry sky over two
    hemispheres. The hands are geometry, turned by the app about the two
    arbors this returns as anchors, since only the painting knows where its
    rings are."""
    w, h, m = clock_px(0.34), clock_px(0.49), clock_px(0.02)
    s = w - 2 * m
    cx, cy = w // 2, h - m - s // 2  # the chapter ring's centre
    base = h - m - s                 # where the square meets the arch
    img = to_image(veneer("rosewood_veneer1", w, h))
    draw = ImageDraw.Draw(img)
    cream, black, gold = (232, 222, 194), (28, 24, 20), (176, 136, 64)
    draw.rectangle([m, base, w - m - 1, h - m - 1], fill=cream)
    draw.pieslice([cx - s // 2, base - s // 2, cx + s // 2, base + s // 2], 180, 360, fill=cream)

    # The arch: a night sky, a moon's face, and the two hemispheres it rises
    # between.
    sky = s // 2 - 22
    draw.pieslice([cx - sky, base - sky, cx + sky, base + sky], 180, 360, fill=(34, 44, 84))
    for _ in range(40):
        a, r = rng.uniform(math.pi, 2 * math.pi), rng.uniform(0.25, 0.95) * sky
        x, y = cx + math.cos(a) * r, base + math.sin(a) * r
        draw.ellipse([x - 1.5, y - 1.5, x + 1.5, y + 1.5], fill=(236, 218, 150))
    mr, my = 62, base - 118
    draw.ellipse([cx - mr, my - mr, cx + mr, my + mr], fill=(238, 214, 146), outline=gold, width=2)
    face = (150, 112, 60)
    for side in (-1, 1):
        draw.arc([cx + side * 24 - 12, my - 22, cx + side * 24 + 12, my - 6], 200, 340, fill=face,
                 width=3)
        draw.ellipse([cx + side * 34 - 9, my + 8, cx + side * 34 + 9, my + 20],
                     fill=(226, 170, 120))
    draw.line([(cx, my - 10), (cx - 6, my + 12), (cx + 2, my + 14)], fill=face, width=2)
    draw.arc([cx - 18, my + 12, cx + 18, my + 34], 20, 160, fill=face, width=3)
    hr = 92
    for side in (-1, 1):
        hx = cx + side * 118
        draw.pieslice([hx - hr, base - hr, hx + hr, base + hr], 180, 360, fill=(70, 104, 118))
        for k in range(1, 4):
            draw.arc([hx - hr * k / 4, base - hr, hx + hr * k / 4, base + hr], 180, 360, fill=gold)
        for k in range(1, 3):
            y = base - hr * k / 3
            half = math.sqrt(max(hr * hr - (base - y) ** 2, 0))
            draw.line([(hx - half, y), (hx + half, y)], fill=gold)
        draw.arc([hx - hr, base - hr, hx + hr, base + hr], 180, 360, fill=gold, width=3)
    draw.arc([cx - sky, base - sky, cx + sky, base + sky], 180, 360, fill=gold, width=3)
    draw.line([(m, base), (w - m, base)], fill=gold, width=3)

    # The spandrels, outside the chapter ring.
    inset = 58
    for x, y in ((m + inset, base + inset), (w - m - inset, base + inset),
                 (m + inset, h - m - inset), (w - m - inset, h - m - inset)):
        painted_rose(draw, x, y, 20, rng)

    # The chapter ring: a minute track, Arabic minutes outside it, Roman hours
    # inside, the hours' feet toward the centre.
    ring_in, ring_out = 196, 210
    for r in (ring_in, ring_out, 138):
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=black, width=2)
    for k in range(60):
        a = k * math.pi / 30
        inner = ring_in if k % 5 else ring_in - 6
        draw.line([(cx + math.sin(a) * inner, cy - math.cos(a) * inner),
                   (cx + math.sin(a) * ring_out, cy - math.cos(a) * ring_out)], fill=black,
                  width=3 if k % 5 == 0 else 1)
    serif = ImageFont.truetype(io.BytesIO(fetch(SERIF, "IMFeENrm28P.ttf")), 44)
    small = ImageFont.truetype(io.BytesIO(fetch(SERIF, "IMFeENrm28P.ttf")), 17)
    italic = ImageFont.truetype(io.BytesIO(fetch(SERIF_ITALIC, "IMFeENit28P.ttf")), 26)
    hours = ["XII", "I", "II", "III", "IIII", "V", "VI", "VII", "VIII", "IX", "X", "XI"]
    for k, numeral in enumerate(hours):
        a = k * 30.0
        r = 166
        paint_text(img, (cx + math.sin(math.radians(a)) * r, cy - math.cos(math.radians(a)) * r),
                   numeral, serif, black, a)
    for k in range(1, 13):
        a = k * 30.0
        r = 223
        paint_text(img, (cx + math.sin(math.radians(a)) * r, cy - math.cos(math.radians(a)) * r),
                   str(5 * k), small, black, a)

    # The seconds ring above the centre, the maker below it, and the date.
    sy, sr = cy - 78, 40
    draw.ellipse([cx - sr, sy - sr, cx + sr, sy + sr], outline=black, width=2)
    for k in range(60):
        a = k * math.pi / 30
        inner = sr - (8 if k % 5 == 0 else 4)
        draw.line([(cx + math.sin(a) * inner, sy - math.cos(a) * inner),
                   (cx + math.sin(a) * sr, sy - math.cos(a) * sr)], fill=black)
    paint_text(img, (cx, cy + 64), "Jas. Harrow", italic, black)
    paint_text(img, (cx, cy + 90), "KINGSBRIDGE", small, black)
    draw.rectangle([cx - 15, cy + 104, cx + 15, cy + 126], fill=(246, 240, 224), outline=black)
    paint_text(img, (cx, cy + 115), "17", small, black)

    a = to_array(img)
    # A century of varnish and smoke: yellowed toward the edges, mottled.
    anchors = {"hands": (cx, cy), "seconds": (cx, sy)}
    return age_paper(a * np.array([1.0, 0.97, 0.9]), rng, edge=0.22), ROUGH_ENAMEL, anchors


def sand_shaded(ang, rad, lobes, span, reach):
    """Maple lobes fanning over `span` radians and scorched dark at one edge,
    as marquetry shells are shaded in hot sand. Returns (inside, shade)."""
    f = ang / span * lobes
    lobe = np.floor(f)
    frac = f - lobe
    edge = reach * (0.9 + 0.1 * np.sin(np.pi * frac))
    inside = (ang >= 0) & (ang <= span) & (rad <= edge)
    shade = 1.0 - 0.6 * (1.0 - frac) ** 3 - 0.25 * np.clip(1.0 - rad / (0.35 * reach), 0, 1)
    shade = np.where(frac < 0.05, 0.35, shade)
    return inside, np.clip(shade, 0.2, 1.0)


def clock_panel(rng):
    """The plinth's panel: a rosewood field inside cross-banding of the case's
    darker wood, maple stringing either side of it, a shell in an oval at the
    centre and a quarter fan in each corner."""
    w, h, band, line = clock_px(0.40), clock_px(0.30), clock_px(0.02), 3
    a = veneer("rosewood_veneer1", w, h)
    across = veneer("lacquered_cherry_wood", w, band, turn=True)
    down = veneer("lacquered_cherry_wood", band, h)
    a[:band], a[h - band:] = across, across
    a[:, :band], a[:, w - band:] = down, down
    maple = veneer("white_maple_veneer", w, h)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    for inset in (0, band - line):
        ring = ((xx >= inset) & (xx < w - inset) & (yy >= inset) & (yy < h - inset)
                & ~((xx >= inset + line) & (xx < w - inset - line)
                    & (yy >= inset + line) & (yy < h - inset - line)))
        a[ring] = maple[ring]

    # The oval, its stringing and its shell, fanning up from the oval's foot.
    cx, cy, rx, ry = w / 2, h / 2, w * 0.27, h * 0.3
    e = ((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2
    a[e <= 1.0] = maple[e <= 1.0] * 0.92
    a[(e > 0.93) & (e <= 1.0)] *= 0.35
    ox, oy = cx, cy + ry * 0.62
    rad = np.hypot(xx - ox, yy - oy)
    ang = np.arctan2(-(yy - oy), xx - ox)
    inside, shade = sand_shaded(ang, rad, 11, math.pi, ry * 1.3)
    inside &= e < 0.9
    a[inside] = maple[inside] * shade[inside][:, None]
    a[(rad < ry * 0.18) & (yy < oy) & (e < 0.9)] *= 0.4

    # A quarter fan in each corner of the field.
    for fx, fy, turn in ((band, band, 0.0), (w - band, band, 0.5 * math.pi),
                         (w - band, h - band, math.pi), (band, h - band, 1.5 * math.pi)):
        rad = np.hypot(xx - fx, yy - fy)
        ang = np.mod(np.arctan2(yy - fy, xx - fx) - turn, 2 * math.pi)
        inside, shade = sand_shaded(ang, rad, 5, 0.5 * math.pi, band * 2.6)
        a[inside] = maple[inside] * shade[inside][:, None]
    return age_paper(a, rng, edge=0.15), ROUGH_LACQUER, {}


def clock_frieze(rng):
    """The hood's frieze under its cornice: a key pattern cut shallow into the
    rosewood -- blind fretwork, shadowed below each line and caught by the
    light above it."""
    w, h, unit = clock_px(0.52), clock_px(0.05), clock_px(0.025)
    a = veneer("rosewood_veneer1", w, h, turn=True)
    cut = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(cut)
    top, bottom = h * 0.18, h * 0.82
    draw.line([(0, top), (w, top)], fill=255, width=3)
    draw.line([(0, bottom), (w, bottom)], fill=255, width=3)
    step = (bottom - top) / 5
    for x in range(0, w, unit):
        draw.line([(x, bottom), (x, top + step), (x + unit * 0.75, top + step),
                   (x + unit * 0.75, bottom - step), (x + unit * 0.3, bottom - step),
                   (x + unit * 0.3, top + 2.5 * step), (x + unit * 0.5, top + 2.5 * step)],
                  fill=255, width=3, joint="curve")
    c = np.asarray(cut, dtype=np.float32) / 255.0
    lit = np.roll(c, -2, axis=0) * (1.0 - c)
    a = a * (1.0 - 0.6 * c[..., None]) + 0.12 * lit[..., None]
    return age_paper(a, rng, edge=0.1), ROUGH_LACQUER, {}


def pack(sizes, atlas=(ATLAS_W, ATLAS_H)):
    """Skyline packing: each card at the lowest place it fits, leftmost first."""
    atlas_w, atlas_h = atlas
    sky = np.zeros(atlas_w, dtype=np.int32)
    spots = []
    for w, h in sizes:
        best = None
        for x in range(0, atlas_w - w + 1, 2):
            y = int(sky[x:x + w].max())
            if best is None or y < best[1]:
                best = (x, y)
        x, y = best
        if y + h > atlas_h:
            raise SystemExit("the cards do not fit a %dx%d atlas" % (atlas_w, atlas_h))
        sky[x:x + w] = y + h + PAD
        spots.append((x, y))
    return spots


def place(cards, atlas=(ATLAS_W, ATLAS_H)):
    """Each card's spot, in the cards' own order: packed tallest first, which packs tightest."""
    order = sorted(range(len(cards)), key=lambda i: -cards[i]["pixels"].shape[0])
    placed = pack([(cards[i]["pixels"].shape[1], cards[i]["pixels"].shape[0]) for i in order],
                  atlas)
    spots = [None] * len(cards)
    for i, spot in zip(order, placed):
        spots[i] = spot
    return spots


def save_rough(rough, path, scale=ROUGH_SCALE):
    """An atlas's roughness at 1/scale its size, box-filtered. It only changes card to card, so
    the small map carries it -- and it has to: a roughness map is a layer of the engine's
    material array, which brings every layer up to its largest."""
    h, w = rough.shape
    flip = Image.Transpose.FLIP_TOP_BOTTOM
    to_image(np.repeat(rough[..., None], 3, axis=-1)).transpose(flip).resize(
        (w // scale, h // scale), Image.Resampling.BOX).save(path)


def write_header(cards, spots, path=HEADER, prefix="CARD", kind="Card", picture="cards_albedo.png",
                 tool="make_cards.py", atlas=(ATLAS_W, ATLAS_H), extra=()):
    """The header naming each card in a picture: an enum, and per card its UVs and its size
    in metres, then any anchors the cards carry and any `extra` lines."""
    atlas_w, atlas_h = atlas
    guard = "_SILENT_%sS_H_" % prefix
    lines = [
        "// Generated by apps/silent/tools/%s -- do not edit; rerun it." % tool,
        "#ifndef %s" % guard,
        "#define %s" % guard,
        "",
        "// The cards in %s." % picture,
        "typedef enum {",
    ]
    lines += ["    %s_%s," % (prefix, c["name"].upper()) for c in cards]
    lines += [
        "    %s_COUNT" % prefix,
        "} %sId;" % kind,
        "",
        "typedef struct %sSpec {" % kind,
        "    float uv[4];   // {u0, v0, u1, v1}, V up",
        "    float size[2]; // metres: width, height",
        "} %sSpec;" % kind,
        "",
        "static const %sSpec %sS[%s_COUNT] = {" % (kind, prefix, prefix),
    ]
    for c, (x, y) in zip(cards, spots):
        h, w = c["pixels"].shape[:2]
        u0, u1 = x / atlas_w, (x + w) / atlas_w
        v0, v1 = 1.0 - (y + h) / atlas_h, 1.0 - y / atlas_h
        lines.append("    [%s_%s] = {{%.6ff, %.6ff, %.6ff, %.6ff}, {%.4ff, %.4ff}}," % (
            prefix, c["name"].upper(), u0, v0, u1, v1, c["m"][0], c["m"][1]))
    lines += ["};", ""]
    for c in cards:
        for key, (x, y) in c.get("anchors", {}).items():
            lines.append("// Metres from the card's lower left.")
            lines.append("static const float %s_%s_%s[2] = {%.4ff, %.4ff};" % (
                prefix, c["name"].upper(), key.upper(), x, y))
            lines.append("")
    lines += list(extra)
    lines += ["#endif // %s" % guard, ""]
    with open(path, "w") as f:
        f.write("\n".join(lines))
    print(path)


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
    for name, paint in (("clock_dial", clock_dial), ("clock_panel", clock_panel),
                        ("clock_frieze", clock_frieze)):
        pixels, rough, anchors = paint(rng)
        h, w = pixels.shape[:2]
        cards.append({"name": name, "pixels": pixels, "rough": rough,
                      "m": (w / CLOCK_PX_PER_M, h / CLOCK_PX_PER_M),
                      "anchors": {k: (x / CLOCK_PX_PER_M, (h - y) / CLOCK_PX_PER_M)
                                  for k, (x, y) in anchors.items()}})

    spots = place(cards)
    albedo = np.ones((ATLAS_H, ATLAS_W, 3), dtype=np.float32) * PAPER * 0.8
    rough = np.full((ATLAS_H, ATLAS_W), ROUGH_PAPER, dtype=np.float32)
    for c, (x, y) in zip(cards, spots):
        h, w = c["pixels"].shape[:2]
        albedo[y:y + h, x:x + w] = c["pixels"]
        rough[y:y + h, x:x + w] = c["rough"]
    flip = Image.Transpose.FLIP_TOP_BOTTOM
    to_image(albedo).transpose(flip).save(os.path.join(OUT_DIR, "cards_albedo.png"))
    save_rough(rough, os.path.join(OUT_DIR, "cards_rough.png"))
    flat = np.zeros((32, 64, 3), dtype=np.uint8) + np.array([128, 128, 255], dtype=np.uint8)
    Image.fromarray(flat).save(os.path.join(OUT_DIR, "cards_normal.png"))
    print(os.path.join(OUT_DIR, "cards_albedo.png"))
    write_header(cards, spots)
    return 0


if __name__ == "__main__":
    sys.exit(main())
