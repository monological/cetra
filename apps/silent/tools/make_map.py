"""Draw the street map of the town apps/silent's player finds on the fridge (spec 13.43).

A visitors' street map of Pale Ridge, printed in four colours on cheap paper and kept folded in a
kitchen for years: tan blocks and house footprints, pale streets with their names, the woods,
the lake and the gorge, Blackwood Manor on its hill as the landmark, and the title in the empty
corner with a compass and a scale. Kept plain, to be read at a glance. Then the paper: its folds worn white, crumpled, stained by a mug and by
water, foxed, grimed where it was held, its edges torn and two holes worn through where the
folds cross. And the ink the player adds: an X and a note at each road out of town that cannot
be followed, the cabin sketched in by the lake, and a red arrow for where they stand.

Everything printed is read from apps/silent/tools/town_plan.txt, which `silent --map-export`
writes from the code that builds the town, so nothing here restates where anything is. The
names are invented; each is one string in NAMES.

The fonts are OFL fonts from Google Fonts, downloaded and cached rather than committed: PT Sans
Narrow for the streets and the legend, Alfa Slab One for the title, and IM Fell English for the
natural features.

Writes assets/textures/silent/ui_town_map.png, RGBA with the torn edge in alpha, and
ui_town_map_marks.png beside it: the arrow turned in 64 steps, then each find's mark. Both are
UI pictures, so they are stored TOP ROW FIRST and hold display values, unlike the world's
textures. `--debug` also writes out/town_map_debug.png, the plan drawn raw in red over the
print, and out/town_map_preview.png, the print with every mark on it.

    python3 apps/silent/tools/make_map.py [--debug]
"""

import functools
import hashlib
import io
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage

from fetch_textures import OUT_DIR, ROOT, fetch
from make_cards import FONTS, HAND_FONT, SERIF_ITALIC, noise, pack

PLAN = os.path.join(ROOT, "apps", "silent", "tools", "town_plan.txt")
PRINT = os.path.join(OUT_DIR, "ui_town_map.png")
MARKS = os.path.join(OUT_DIR, "ui_town_map_marks.png")
FOLDED = os.path.join(OUT_DIR, "town_map_folded_%s.png")
HEADER = os.path.join(ROOT, "apps", "silent", "src", "map_art.h")
DEBUG = os.path.join(ROOT, "out", "town_map_debug.png")
PREVIEW = os.path.join(ROOT, "out", "town_map_preview.png")

# The marks atlas: the arrow's turns in a grid of cells, and each find's mark packed under them,
# all in print pixels so a mark is drawn at the size it has on the print.
MARKS_W, MARKS_H = 1024, 1024
ARROW_FRAMES, ARROW_CELL, ARROW_COLS = 64, 80, 8
MARK_PAD = 6  # pixels round each mark, so filtering does not reach its neighbour
INK_SS = 4
# How a hand writes a mark on: a line at PEN_SPEED print pixels a second, a lift between lines,
# and handwriting a letter at a time.
PEN_SPEED = 240.0
PEN_LIFT = 0.12
CHAR_SECONDS = 0.07
BLUE = (30, 52, 138)  # a ballpoint's blue
RED = (170, 32, 28)  # the felt-tip that marks where you stand

SEED = 1343
OUT_W, OUT_H = 2048, 1536
SS = 2  # drawn at twice the size and reduced, so every line and letter is smooth
W, H = OUT_W * SS, OUT_H * SS

# The sheet inside the picture, in output pixels, leaving room round it for a torn edge.
PAPER_INSET = 22
# The fold crossings worn through, as (vertical fold, horizontal fold): two clear of any lettering.
WORN_THROUGH = ((0, 0), (2, 1))
# The printed frame, in output pixels, and the world it shows: x east and z south, in metres.
FRAME = (100, 96, 1948, 1440)
WORLD_X0, WORLD_Z0 = -127.0, -52.0
SCALE = (FRAME[2] - FRAME[0]) / 272.0  # output pixels a metre
M = SCALE * SS  # canvas pixels a metre

# The title's baseline middle, in output pixels: the empty corner past the world's edge, with
# the compass and the scale beside it, inside one fold's panel.
TITLE = (1275, 1110)
# The folded map's two faces: its printed cover, and the sheet's (column, row) panel between its
# folds that shows when it hangs half open.
INSIDE_PANEL = (1, 0)
FOLDED_FACE = 512  # pixels a side of each face in the folded set
ROUGH_COVER, ROUGH_INSIDE = 0.40, 0.88  # coated and glossy; newsprint
FT = 3.28084  # feet a metre, for the scale

NARROW_BOLD = FONTS + "ptsansnarrow/PT_Sans-Narrow-Web-Bold.ttf"
NARROW = FONTS + "ptsansnarrow/PT_Sans-Narrow-Web-Regular.ttf"
SLAB = FONTS + "alfaslabone/AlfaSlabOne-Regular.ttf"

NAMES = {
    "town": "PALE RIDGE",
    "street": "ALDER STREET",
    "cross": "MILL ROAD",
    "drive": "BLACKWOOD DRIVE",
    "track": "LAKE ROAD",
    "bridge": "GORGE BRIDGE",
    "house": "BLACKWOOD MANOR",
    "hill": "Blackwood Hill",
    "graveyard": ("Old Hill", "Burying Ground"),
    "lake": "Calder Lake",
    "gorge": "WIDOW'S GORGE",
    "creek": "Widow's Creek",
    "north_woods": "North Woods",
    "south_woods": "Calder Woods",
    "north_road": "TO ROUTE 9",
    "west_road": "TO ASHGROVE",
    "compliments": "Compliments of the Pale Ridge Chamber of Commerce  -  1987",
    "chamber": "Pale Ridge Chamber of Commerce",
}


def rgb(r, g, b):
    return (int(round(r * 255)), int(round(g * 255)), int(round(b * 255)))


# The colour plate: flat tints, multiplied into the paper.
LOT = rgb(0.93, 0.84, 0.70)
LOT_LINE = rgb(0.80, 0.69, 0.56)
HOUSE_LIT = rgb(0.76, 0.60, 0.47)
HOUSE_DARK = rgb(0.64, 0.49, 0.39)
SHADOW = rgb(0.84, 0.74, 0.62)
LAWN = rgb(0.80, 0.87, 0.67)
WOODS = rgb(0.72, 0.82, 0.62)
WATER = rgb(0.52, 0.76, 0.92)
SIDEWALK = rgb(0.95, 0.94, 0.91)
CHASM_LIP = rgb(0.86, 0.82, 0.78)
CHASM_DEEP = rgb(0.70, 0.66, 0.63)
# The key plate: the line work and the lettering.
INK = rgb(0.24, 0.19, 0.15)
SOFT_INK = rgb(0.42, 0.35, 0.29)
CASING = rgb(0.56, 0.46, 0.36)
CURB = rgb(0.80, 0.74, 0.66)
CONTOUR = rgb(0.84, 0.73, 0.59)
CONTOUR_INDEX = rgb(0.74, 0.60, 0.46)
WOODS_INK = rgb(0.25, 0.37, 0.22)
WATER_INK = rgb(0.26, 0.45, 0.56)
WHITE = (255, 255, 255)
# The cover's inks.
COVER_RED = rgb(0.74, 0.15, 0.12)
COVER_NAVY = rgb(0.18, 0.24, 0.40)

PAPER = np.array([0.95, 0.91, 0.80], dtype=np.float32)  # newsprint gone yellow
COVER_STOCK = np.array([0.96, 0.94, 0.87], dtype=np.float32)  # the cover's coated card
BROWN = np.array([0.66, 0.47, 0.27], dtype=np.float32)  # what coffee and age leave
GRIME = np.array([0.52, 0.48, 0.44], dtype=np.float32)
EDGE_BROWN = np.array([0.74, 0.55, 0.36], dtype=np.float32)


@functools.lru_cache(maxsize=None)
def font(url, size):
    """A font at `size` output pixels."""
    return ImageFont.truetype(io.BytesIO(fetch(url)), int(round(size * SS)))


class Plan:
    """town_plan.txt, read."""

    def __init__(self, path):
        self.boxes, self.lines, self.polys, self.places = [], {}, {}, {}
        cover, height = [], []
        with open(path) as f:
            for raw in f:
                v = raw.split()
                if not v or v[0].startswith("#"):
                    continue
                if v[0] == "seed":
                    self.seed = int(v[1])
                elif v[0] == "box":
                    self.boxes.append((v[1], v[2], *map(float, v[3:7])))
                elif v[0] == "line":
                    n = int(v[3])
                    self.lines[v[1]] = (float(v[2]), np.array(v[4:4 + 2 * n], float).reshape(n, 2))
                elif v[0] == "poly":
                    n = int(v[2])
                    self.polys[v[1]] = np.array(v[3:3 + 2 * n], float).reshape(n, 2)
                elif v[0] == "place":
                    self.places[v[1]] = (float(v[2]), float(v[3]))
                elif v[0] == "grid":
                    self.grid = (float(v[1]), float(v[2]), float(v[3]), int(v[4]), int(v[5]))
                elif v[0] == "cover":
                    cover.append(list(v[1]))
                elif v[0] == "height":
                    height.append([float(x) for x in v[1:]])
        self.cover = np.array(cover)
        self.height = np.array(height, dtype=np.float32)

    def all(self, kind):
        return [b for b in self.boxes if b[0] == kind]

    def one(self, kind, name):
        return next(b for b in self.boxes if b[0] == kind and b[1] == name)


# -- Geometry -------------------------------------------------------------------------------------

def px(x, z):
    """A world point as canvas pixels."""
    return ((FRAME[0] + (x - WORLD_X0) * SCALE) * SS, (FRAME[1] + (z - WORLD_Z0) * SCALE) * SS)


def px_all(pts):
    pts = np.asarray(pts, float)
    return np.stack([(FRAME[0] + (pts[:, 0] - WORLD_X0) * SCALE) * SS,
                     (FRAME[1] + (pts[:, 1] - WORLD_Z0) * SCALE) * SS], axis=1)


def rect(x0, x1, z0, z1):
    a, b = px(x0, z0), px(x1, z1)
    return [min(a[0], b[0]), min(a[1], b[1]), max(a[0], b[0]), max(a[1], b[1])]


def box_rect(b):
    return rect(b[2], b[3], b[4], b[5])


def dedupe(pts, eps=1e-6):
    keep = [0] + [i for i in range(1, len(pts)) if np.hypot(*(pts[i] - pts[i - 1])) > eps]
    return pts[keep]


def arc(pts):
    seg = np.hypot(*np.diff(pts, axis=0).T)
    return np.concatenate([[0.0], np.cumsum(seg)])


def along(pts, s, at, window):
    """The point `at` along a polyline with arc lengths `s`, and its direction, smoothed over
    `window` either side."""
    at = float(np.clip(at, 0.0, s[-1]))
    x, y = np.interp(at, s, pts[:, 0]), np.interp(at, s, pts[:, 1])
    a, b = max(at - window, 0.0), min(at + window, s[-1])
    dx = np.interp(b, s, pts[:, 0]) - np.interp(a, s, pts[:, 0])
    dy = np.interp(b, s, pts[:, 1]) - np.interp(a, s, pts[:, 1])
    return x, y, math.atan2(dy, dx)


def resample(pts, step):
    pts = dedupe(np.asarray(pts, float))
    s = arc(pts)
    t = np.arange(0.0, s[-1], step)
    return np.stack([np.interp(t, s, pts[:, 0]), np.interp(t, s, pts[:, 1])], axis=1)


def offset(pts, d):
    """A polyline moved `d` to its left (in canvas pixels, y down)."""
    t = np.gradient(pts, axis=0)
    t /= np.maximum(np.hypot(t[:, 0], t[:, 1]), 1e-9)[:, None]
    return pts + d * np.stack([t[:, 1], -t[:, 0]], axis=1)


def resample_grid(values, plan, order):
    """A per-cell grid of the plan sampled at every canvas pixel's centre."""
    gx0, gz0, step, _, _ = plan.grid
    k = 1.0 / (M * step)
    z_top = WORLD_Z0 - FRAME[1] / SCALE
    x_left = WORLD_X0 - FRAME[0] / SCALE
    off = ((z_top + 0.5 / M - gz0) / step - 0.5, (x_left + 0.5 / M - gx0) / step - 0.5)
    return ndimage.affine_transform(values.astype(np.float32), [k, k], offset=off,
                                    output_shape=(H, W), order=order, mode="nearest")


def mask_of(draw_fn):
    m = Image.new("L", (W, H), 0)
    draw_fn(ImageDraw.Draw(m))
    return m


def grow(mask, r):
    if r <= 0:
        return mask
    return Image.fromarray(ndimage.maximum_filter(np.asarray(mask), size=2 * r + 1))


def as_array(mask):
    return np.asarray(mask, dtype=np.float32) / 255.0


def to_mask(a):
    return Image.fromarray(np.clip(a * 255.0 + 0.5, 0, 255).astype(np.uint8))


# -- Lettering ------------------------------------------------------------------------------------

def stamp(layer, ch, f, fill, x, y, angle):
    """One character centred on (x, y), its baseline turned `angle` radians (y down)."""
    r = int(f.size * 1.2)
    g = Image.new("RGBA", (2 * r, 2 * r), (0, 0, 0, 0))
    ImageDraw.Draw(g).text((r, r), ch, font=f, fill=fill + (255,), anchor="mm")
    if abs(angle) > 1e-4:
        g = g.rotate(-math.degrees(angle), resample=Image.Resampling.BICUBIC)
    layer.alpha_composite(g, dest=(int(round(x)) - r, int(round(y)) - r))


def text_path(layer, pts, text, f, fill, at=0.5, tracking=0.0, upright=True):
    """`text` along a polyline of canvas pixels, centred `at` that fraction of its length,
    reading left to right whichever way the line was given."""
    pts = dedupe(np.asarray(pts, float))
    if upright and pts[-1, 0] < pts[0, 0] - 1.0:
        pts = pts[::-1]
    s = arc(pts)
    track = tracking * f.size
    adv = [f.getlength(c) + track for c in text]
    total = sum(adv) - track
    pos = at * s[-1] - total / 2.0
    for c, a in zip(text, adv):
        x, y, ang = along(pts, s, pos + (a - track) / 2.0, f.size * 0.8)
        if c != " ":
            stamp(layer, c, f, fill, x, y, ang)
        pos += a


def text_at(layer, x, y, text, f, fill, angle=0.0, tracking=0.0):
    """`text` centred on canvas point (x, y), turned `angle` radians."""
    d = (math.cos(angle) * W, math.sin(angle) * W)
    text_path(layer, [(x - d[0], y - d[1]), (x + d[0], y + d[1])], text, f, fill,
              tracking=tracking, upright=False)


def text_left(layer, x, y, text, f, fill, anchor="lm"):
    ImageDraw.Draw(layer).text((x, y), text, font=f, fill=fill + (255,), anchor=anchor)


# -- The print ------------------------------------------------------------------------------------

class Sheet:
    """The two printing plates and the lettering laid over them, `size` canvas pixels."""

    def __init__(self, size=(W, H)):
        self.colour = Image.new("RGB", size, WHITE)
        self.key = Image.new("RGB", size, WHITE)
        self.c = ImageDraw.Draw(self.colour)
        self.k = ImageDraw.Draw(self.key)
        # Lettering that knocks the key plate out round itself, lettering that does not, and
        # lettering left unprinted in the colour plate so the paper shows through as letters.
        self.haloed = Image.new("RGBA", size, (0, 0, 0, 0))
        self.plain = Image.new("RGBA", size, (0, 0, 0, 0))
        self.knockout = Image.new("RGBA", size, (0, 0, 0, 0))

    def tint(self, mask, colour):
        self.colour.paste(colour, mask=mask)

    def line_mask(self, mask, colour):
        self.key.paste(colour, mask=mask)

    def inked(self):
        """The plates printed: the lettering knocked out of the key plate round the haloed and
        left unprinted in the colour plate where it is knocked out, and the colour plate a
        little out of register, as multipliers of the paper."""
        halo = grow(self.haloed.getchannel("A"), int(0.35 * M))
        self.key.paste(WHITE, mask=halo)
        key = Image.alpha_composite(self.key.convert("RGBA"), self.haloed)
        key = Image.alpha_composite(key, self.plain).convert("RGB")
        self.colour.paste(WHITE, mask=self.knockout.getchannel("A"))
        colour = np.roll(np.asarray(self.colour, dtype=np.float32) / 255.0, (-SS // 2, SS),
                         axis=(0, 1))
        return colour * (np.asarray(key, dtype=np.float32) / 255.0)


def draw_ground(sheet, plan, rng):
    """The woods, the hillside's contours, the gorge and the lake."""
    chasm = chasm_mask(plan)
    water = mask_of(lambda d: d.polygon([tuple(p) for p in px_all(plan.polys["shore"])], fill=255))

    # The woods, from the plan's cover grid, given a hand-cut edge.
    woods = resample_grid(ndimage.gaussian_filter((plan.cover == "t").astype(np.float32), 0.6),
                          plan, order=1)
    woods = woods + 0.16 * (noise(H, W, int(2.5 * M), rng) - 0.5) > 0.5
    woods &= ~(as_array(chasm) > 0.5) & ~(as_array(water) > 0.5)
    # No stand of trees smaller than a house lot is worth printing.
    parts, n = ndimage.label(woods)
    area = ndimage.sum(woods, parts, index=np.arange(1, n + 1))
    woods &= np.isin(parts, 1 + np.nonzero(area >= 300.0 * M * M)[0])
    woods_mask = to_mask(woods.astype(np.float32))
    sheet.tint(woods_mask, WOODS)

    # The gorge, darkening as it falls away from its lip.
    chasm_a = as_array(chasm) > 0.5
    depth = ndimage.distance_transform_edt(chasm_a) / M
    t = np.clip(depth / 22.0, 0.0, 1.0)
    t = t * t * (3.0 - 2.0 * t)
    shade = (np.array(CHASM_LIP, np.float32) * (1.0 - t[..., None])
             + np.array(CHASM_DEEP, np.float32) * t[..., None])
    colour = np.asarray(sheet.colour, dtype=np.float32)
    colour = np.where(chasm_a[..., None], shade, colour)
    sheet.colour = Image.fromarray(colour.astype(np.uint8))
    sheet.c = ImageDraw.Draw(sheet.colour)

    sheet.tint(water, WATER)
    return woods_mask, chasm, water, depth


def chasm_mask(plan):
    lip = plan.lines["lip"][1]
    far = 400.0
    ring = np.concatenate([lip, [[lip[-1, 0] - far, lip[-1, 1]], [lip[-1, 0] - far, lip[0, 1] - far],
                                 [lip[0, 0], lip[0, 1] - far]]])
    return mask_of(lambda d: d.polygon([tuple(p) for p in px_all(ring)], fill=255))


def draw_contours(sheet, plan, keep):
    """Contour lines every two metres, every tenth metre heavier, over the open ground. The gorge
    and the world's edge carry no height worth drawing, so each of their cells takes its nearest
    neighbour's before the field is smoothed."""
    valid = (plan.cover == ".") | (plan.cover == "t")
    _, (ri, ci) = ndimage.distance_transform_edt(~valid, return_indices=True)
    heights = ndimage.gaussian_filter(plan.height[ri, ci], 1.3)
    h = resample_grid(heights, plan, order=3)
    keep = keep & (resample_grid(valid.astype(np.float32), plan, order=0) > 0.5)
    for step, colour, width in ((2.0, CONTOUR, 1), (10.0, CONTOUR_INDEX, 2)):
        level = np.floor(h / step)
        edge = np.zeros((H, W), bool)
        edge[:, 1:] |= level[:, 1:] != level[:, :-1]
        edge[1:, :] |= level[1:, :] != level[:-1, :]
        edge &= keep
        sheet.line_mask(grow(to_mask(edge.astype(np.float32)), width), colour)


def draw_gorge(sheet, plan, chasm, rng):
    """The gorge's lip, its drop drawn in tapering strokes down from it, and the creek along its
    floor, which no one in the town has seen in years."""
    inside = as_array(chasm) > 0.5
    lip = px_all(plan.lines["lip"][1])
    pts = resample(lip, 0.75 * M)
    for i in range(1, len(pts) - 1):
        x, y = pts[i]
        tx, ty = pts[i + 1] - pts[i - 1]
        n = np.array([ty, -tx]) / max(math.hypot(tx, ty), 1e-9)
        probe = (int(y + n[1] * 1.6 * M), int(x + n[0] * 1.6 * M))
        if not (0 <= probe[0] < H and 0 <= probe[1] < W):
            continue
        if not inside[probe]:
            n = -n
        length = (1.4 + 2.0 * rng.random() + (2.8 if i % 4 == 0 else 0.0)) * M
        base = 0.18 * M
        tip = (x + n[0] * length, y + n[1] * length)
        side = np.array([-n[1], n[0]])
        sheet.k.polygon([(x + side[0] * base, y + side[1] * base), tip,
                         (x - side[0] * base, y - side[1] * base)], fill=SOFT_INK)
    sheet.k.line([tuple(p) for p in lip], fill=INK, width=int(0.4 * M), joint="curve")

    # The creek: down the gorge 40 m off its east face, then bending wide to run west under the
    # ridge's face, set meandering. Laid out from where the two faces stand rather than offset
    # from the lip, whose corner is sharper than the creek stands off it.
    faces = plan.lines["lip"][1]
    corner = np.argmax(faces[:, 1])
    east_x = float(np.median(faces[:corner, 0]))
    ridge_z = float(np.median(faces[corner:, 1]))
    x, z = east_x - 40.0, ridge_z - 40.0
    controls = np.array([(x, WORLD_Z0 - 40.0), (x - 1.0, -20.0), (x - 2.0, z - 30.0),
                         (x - 7.0, z - 12.0), (x - 18.0, z - 2.0), (x - 60.0, z + 2.0)])
    creek = resample(controls, 0.5)
    creek = np.stack([ndimage.gaussian_filter1d(creek[:, 0], 24, mode="nearest"),
                      ndimage.gaussian_filter1d(creek[:, 1], 24, mode="nearest")], axis=1)
    s = arc(creek)
    meander = 2.2 * np.sin(s / 9.0) + 1.2 * np.sin(s / 3.7 + 1.3)
    t = np.gradient(creek, axis=0)
    t /= np.maximum(np.hypot(t[:, 0], t[:, 1]), 1e-9)[:, None]
    creek = creek + meander[:, None] * np.stack([t[:, 1], -t[:, 0]], axis=1)
    creek_px = px_all(creek)
    sheet.k.line([tuple(p) for p in creek_px], fill=WATER_INK, width=int(0.5 * M), joint="curve")
    # Where its name goes: up in the gorge north of the bridge.
    s = arc(creek)
    at = s[np.argmin(np.abs(creek[:, 1] + 30.0))] / s[-1]
    return creek_px, at


def draw_water_lines(sheet, water):
    """The shore inked, and two fainter lines inside it that follow it out into the lake."""
    inside = as_array(water) > 0.5
    d = ndimage.distance_transform_edt(inside) / M
    for r, w, fade in ((0.0, 0.34, 0.0), (1.8, 0.16, 0.45), (4.2, 0.13, 0.70)):
        band = inside & (np.abs(d - r - w / 2.0) < w / 2.0) if r > 0 else inside & (d < w)
        colour = tuple(int(c + (255 - c) * fade) for c in WATER_INK)
        sheet.line_mask(to_mask(band.astype(np.float32)), colour)


def street_boxes(plan):
    """The paved boxes, the street carried west over the gorge as its bridge, and the cross
    street carried north out of the frame."""
    street = plan.one("asphalt", "street")
    cross = plan.one("asphalt", "cross")
    walks = [b for b in plan.all("sidewalk")]
    off = -400.0
    asphalt = [street, cross, ("asphalt", "bridge", off, street[2], street[4], street[5]),
               ("asphalt", "north", cross[2], cross[3], off, cross[4])]
    walk_n = plan.one("sidewalk", "street_n")
    walk_s = plan.one("sidewalk", "street_s")
    walk_w = plan.one("sidewalk", "cross_w")
    walk_e = plan.one("sidewalk", "cross_e")
    walks += [("sidewalk", "bridge_n", off, walk_n[2], walk_n[4], walk_n[5]),
              ("sidewalk", "bridge_s", off, walk_s[2], walk_s[4], walk_s[5]),
              ("sidewalk", "north_w", walk_w[2], walk_w[3], off, walk_w[4]),
              ("sidewalk", "north_e", walk_e[2], walk_e[3], off, walk_e[4])]
    return asphalt, walks


def smooth_path(pts, step):
    """A polyline of canvas pixels resampled every `step` and eased, so a ribbon round it has no
    kinks."""
    pts = resample(pts, step)
    return np.stack([ndimage.gaussian_filter1d(pts[:, 0], 2.0, mode="nearest"),
                     ndimage.gaussian_filter1d(pts[:, 1], 2.0, mode="nearest")], axis=1)


def ribbon(pts, half):
    """The outline of a band `half` pixels either side of a polyline, as a polygon."""
    return [tuple(p) for p in np.concatenate([offset(pts, half), offset(pts, -half)[::-1]])]


class Roads:
    """Where the paving and the track are, which the ground's line work is cleared from."""

    def __init__(self, plan):
        self.asphalt, self.walks = street_boxes(plan)
        self.drive_half, drive = plan.lines["drive"]
        self.drive = smooth_path(px_all(dedupe(drive)), 0.4 * M)
        house = plan.one("landmark", "mansion")
        # The forecourt: the drive carried through the gate to the front door.
        self.front = px_all([[drive[-1, 0], drive[-1, 1]], [drive[-1, 0], house[4]]])
        self.track_half, track = plan.lines["track"]
        self.track = smooth_path(px_all(dedupe(track)), 0.4 * M)

        def paved(d):
            for b in self.asphalt + self.walks:
                d.rectangle(box_rect(b), fill=255)
            d.polygon(ribbon(self.drive, self.drive_half * M), fill=255)
            for p in (self.drive[0], self.drive[-1]):
                r = self.drive_half * M
                d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=255)
            d.polygon(ribbon(resample(self.front, 0.4 * M), 1.6 * M), fill=255)

        self.paved = mask_of(paved)
        self.walk = mask_of(lambda d: [d.rectangle(box_rect(b), fill=255) for b in self.walks])
        self.road = mask_of(lambda d: [d.rectangle(box_rect(b), fill=255) for b in self.asphalt])
        self.unpaved = mask_of(lambda d: d.polygon(ribbon(self.track, self.track_half * M),
                                                   fill=255))


def draw_roads(sheet, plan, roads):
    # Nothing on the ground is printed through a road.
    for m in (roads.paved, roads.unpaved):
        sheet.key.paste(WHITE, mask=m)
        sheet.tint(m, WHITE)
    sheet.tint(roads.walk, SIDEWALK)
    sheet.tint(roads.road, WHITE)

    # The track is unpaved, so edged in dashes, which stop at the paving.
    dashes = Image.new("L", (W, H), 0)
    dd = ImageDraw.Draw(dashes)
    for side in (-1.0, 1.0):
        edge = resample(offset(roads.track, side * (roads.track_half + 0.15) * M), 0.25 * M)
        dash = int(1.6 * M / (0.25 * M))
        for i in range(0, len(edge) - dash, dash * 2):
            dd.line([tuple(p) for p in edge[i:i + dash]], fill=255, width=int(0.3 * M))
    dashes = as_array(dashes) * (1.0 - as_array(grow(roads.paved, 2)))
    sheet.line_mask(to_mask(dashes), CASING)

    # Casing round everything paved, a fainter line at each curb.
    casing = as_array(grow(roads.paved, int(0.22 * M))) * (1.0 - as_array(roads.paved))
    sheet.line_mask(to_mask(casing), CASING)
    curb = (as_array(grow(roads.road, int(0.12 * M))) * (1.0 - as_array(roads.road))
            * as_array(roads.walk))
    sheet.line_mask(to_mask(curb), CURB)

    # The bridge: its parapets drawn heavy, flaring out at the end it starts from.
    street = plan.one("asphalt", "street")
    walk_n, walk_s = plan.one("sidewalk", "street_n"), plan.one("sidewalk", "street_s")
    x_end = street[2]
    for z, flare in ((walk_n[4], -1.0), (walk_s[5], 1.0)):
        a, b = px(WORLD_X0 - 5.0, z), px(x_end, z)
        sheet.k.line([a, b], fill=INK, width=int(0.42 * M))
        c = px(x_end + 1.6, z + flare * 1.6)
        sheet.k.line([b, c], fill=INK, width=int(0.42 * M))


def draw_lots(sheet, plan):
    lots = plan.all("lot")
    for b in lots:
        sheet.c.rectangle(box_rect(b), fill=LOT)
    for b in lots:
        sheet.k.rectangle(box_rect(b), outline=LOT_LINE, width=SS + 1)
    for side in ("near", "far"):
        group = [b for b in lots if b[1].startswith(side)]
        sheet.k.rectangle(rect(min(b[2] for b in group), max(b[3] for b in group),
                               min(b[4] for b in group), max(b[5] for b in group)),
                          outline=CASING, width=int(0.2 * M))
    # The terrace's retaining wall, ticked on its high side.
    wall = plan.one("wall", "terrace")
    z = wall[5]
    sheet.k.line([px(wall[2], z), px(wall[3], z)], fill=INK, width=int(0.28 * M))
    for x in np.arange(wall[2] + 0.5, wall[3], 1.0):
        sheet.k.line([px(x, z), px(x, z - 0.7)], fill=INK, width=SS)


def draw_houses(sheet, plan):
    """Each house as a gabled roof seen from above, lit from the north-west, with its shadow."""
    houses = plan.all("house") + plan.all("home")
    for b in houses:
        r = box_rect(b)
        d = 0.5 * M
        sheet.c.rectangle([r[0] + d, r[1] + d, r[2] + d, r[3] + d], fill=SHADOW)
    for b in houses:
        r = box_rect(b)
        mid = (r[0] + r[2]) / 2.0
        sheet.c.rectangle([r[0], r[1], mid, r[3]], fill=HOUSE_LIT)
        sheet.c.rectangle([mid, r[1], r[2], r[3]], fill=HOUSE_DARK)
        sheet.k.rectangle(r, outline=INK, width=SS + 1)
        sheet.k.line([(mid, r[1]), (mid, r[3])], fill=INK, width=SS)


def draw_grounds(sheet, plan, rng, f_label):
    """The manor's grounds, fenced, with the gate the drive comes in by; the manor pictured on
    them; and the burying ground down the hill."""
    grounds = plan.one("grounds", "mansion")
    sheet.c.rectangle(box_rect(grounds), fill=LAWN)
    gate_x = plan.lines["drive"][1][-1, 0]
    x0, x1, z0, z1 = grounds[2] + 0.6, grounds[3] - 0.6, grounds[4] + 0.6, grounds[5] - 0.6
    a, c = px(x0, z0), px(x1, z1)
    sheet.k.rectangle([a[0], a[1], c[0], c[1]], outline=WOODS_INK, width=SS + 1)
    sheet.k.line([px(gate_x - 2.8, z0), px(gate_x + 2.8, z0)], fill=WHITE, width=SS + 3)
    for side in (-1.0, 1.0):
        cx, cy = px(gate_x + side * 3.0, z0)
        r = 0.55 * M
        sheet.k.rectangle([cx - r, cy - r, cx + r, cy + r], fill=INK)

    b = plan.one("landmark", "mansion")
    draw_manor(sheet, b)
    cx, cy = px((b[2] + b[3]) / 2.0, b[5] + 3.2)
    text_at(sheet.haloed, cx, cy, NAMES["house"], f_label, INK, tracking=0.12)

    # The burying ground: its stones in rows, a fence round it.
    g = plan.one("graveyard", "graveyard")
    sheet.c.rectangle(box_rect(g), fill=LAWN)
    for z in np.arange(g[4] + 1.0, g[5] - 0.5, 1.3):
        for x in np.arange(g[2] + 0.9, g[3] - 0.5, 1.1):
            cx, cy = px(x + 0.15 * (rng.random() - 0.5), z + 0.15 * (rng.random() - 0.5))
            a, w = 0.42 * M, 0.26 * M
            sheet.k.line([(cx, cy - a), (cx, cy + a)], fill=SOFT_INK, width=SS)
            sheet.k.line([(cx - w, cy - a * 0.35), (cx + w, cy - a * 0.35)], fill=SOFT_INK, width=SS)
    dashed_rect(sheet.k, box_rect(g), 0.8 * M, SOFT_INK, SS + 1)
    return g


def draw_manor(sheet, b):
    """The manor pictured on its grounds, the picture's foot on the house's south face."""
    draw_manor_at(sheet, *px((b[2] + b[3]) / 2.0, b[5]), 0.65 * M)


def draw_manor_at(sheet, bx, by, u):
    """The manor drawn standing, as a visitors' map pictures its sight: a steep gabled house of
    dark boards, a gable brought forward over the door, and the tower with its spire at the
    corner where the real one stands. Its foot's middle at canvas point (bx, by), `u` canvas
    pixels to one of the drawing's units; it is 30 units wide and 24 tall."""

    def p(x, y):
        return (bx + x * u, by - y * u)

    wall, roof, spire = rgb(0.56, 0.52, 0.50), rgb(0.40, 0.38, 0.40), rgb(0.33, 0.31, 0.33)

    def shape(points, fill):
        sheet.c.polygon([p(*q) for q in points], fill=fill)
        sheet.k.polygon([p(*q) for q in points], outline=INK, width=SS + 1)

    def window(x, y, w=1.0, h=1.8):
        pts = [(x - w / 2, y), (x - w / 2, y + h), (x, y + h + w * 0.7), (x + w / 2, y + h),
               (x + w / 2, y)]
        sheet.k.polygon([p(*q) for q in pts], fill=INK)

    # The ground it stands on, a few tufts along it.
    sheet.k.line([p(-15, 0), p(15, 0)], fill=INK, width=SS + 1)
    for x in (-13.5, -12.2, 11.8, 13.4):
        sheet.k.line([p(x, 0), p(x - 0.3, 0.8)], fill=WOODS_INK, width=SS)
        sheet.k.line([p(x, 0), p(x + 0.4, 0.9)], fill=WOODS_INK, width=SS)
    # Chimneys behind the roof, the house, its roof, the forward gable.
    shape([(-1.0, 8), (-1.0, 13.6), (0.2, 13.6), (0.2, 8)], wall)
    shape([(8.4, 7), (8.4, 11.6), (9.6, 11.6), (9.6, 7)], wall)
    shape([(-6, 0), (-6, 7), (10, 7), (10, 0)], wall)
    shape([(-6.8, 7), (2.0, 13.4), (10.8, 7)], roof)
    shape([(2.6, 0), (2.6, 8.4), (8.4, 8.4), (8.4, 0)], wall)
    shape([(2.0, 8.4), (5.5, 13.0), (9.0, 8.4)], roof)
    # The tower: its shaft, a band under the spire, and the spire with its finial.
    shape([(-11, 0), (-11, 12), (-6, 12), (-6, 0)], wall)
    shape([(-11.6, 12), (-11.6, 13), (-5.4, 13), (-5.4, 12)], roof)
    shape([(-11.4, 13), (-8.5, 22), (-5.6, 13)], spire)
    sheet.k.line([p(-8.5, 22), p(-8.5, 23.6)], fill=INK, width=SS + 1)
    # Windows in pointed arches, and the door under the gable.
    for x in (-4.2, -1.4, 1.2):
        window(x, 1.4)
        window(x, 4.4)
    window(5.5, 4.6, 1.2, 2.0)
    for y in (2.0, 5.4, 8.8):
        window(-8.5, y, 1.0, 1.6)
    window(5.5, 0.0, 1.8, 2.4)


def dashed_rect(draw, r, dash, fill, width):
    corners = [(r[0], r[1]), (r[2], r[1]), (r[2], r[3]), (r[0], r[3]), (r[0], r[1])]
    for (ax, ay), (bx, by) in zip(corners, corners[1:]):
        n = max(int(math.hypot(bx - ax, by - ay) / dash), 1)
        for i in range(0, n, 2):
            t0, t1 = i / n, min((i + 1) / n, 1.0)
            draw.line([(ax + (bx - ax) * t0, ay + (by - ay) * t0),
                       (ax + (bx - ax) * t1, ay + (by - ay) * t1)], fill=fill, width=width)


def compass(sheet, x, y):
    r = 5.2 * M
    sheet.k.ellipse([x - r, y - r, x + r, y + r], outline=INK, width=SS + 1)
    sheet.k.ellipse([x - r * 0.86, y - r * 0.86, x + r * 0.86, y + r * 0.86], outline=SOFT_INK,
                    width=SS)
    n, w = r * 1.25, r * 0.22
    sheet.k.polygon([(x, y - n), (x + w, y), (x - w, y)], fill=INK)
    sheet.k.polygon([(x, y + n), (x + w, y), (x - w, y)], outline=INK, width=SS)
    sheet.k.line([(x - r * 0.95, y), (x + r * 0.95, y)], fill=INK, width=SS)
    text_at(sheet.plain, x, y - n - 2.2 * M, "N", font(NARROW_BOLD, 22), INK)


def draw_labels(sheet, plan, roads, creek, g):
    f_street = font(NARROW_BOLD, 21)
    f_road = font(NARROW_BOLD, 16)
    f_track = font(NARROW, 15)
    street = plan.one("asphalt", "street")
    cross = plan.one("asphalt", "cross")
    cx = (cross[2] + cross[3]) / 2.0
    text_path(sheet.plain, [px(-20.0, 0.0), px(30.0, 0.0)], NAMES["street"], f_street, INK,
              tracking=0.22)
    text_path(sheet.plain, [px(cx, cross[5] - 2.0), px(cx, plan.one("sidewalk", "street_s")[5] + 1.0)],
              NAMES["cross"], f_street, INK, tracking=0.18, upright=False)
    text_path(sheet.plain, [px(WORLD_X0, 0.0), px(street[2], 0.0)], NAMES["bridge"], f_road, INK,
              at=0.62, tracking=0.2)
    # The roads out of town, with an arrow each.
    f_out = font(NARROW_BOLD, 14)
    y_out = WORLD_Z0 + 13.0
    text_path(sheet.plain, [px(cx, y_out + 7.0), px(cx, y_out - 7.0)], NAMES["north_road"], f_out,
              INK, tracking=0.12, upright=False)
    ax, ay = px(cx, WORLD_Z0 + 0.6)
    a = 1.4 * M
    sheet.k.polygon([(ax, ay), (ax + a, ay + 1.6 * a), (ax - a, ay + 1.6 * a)], fill=INK)
    wx, wy = px(WORLD_X0 + 0.6, 0.0)
    sheet.k.polygon([(wx, wy), (wx + 1.6 * a, wy - a), (wx + 1.6 * a, wy + a)], fill=INK)
    text_path(sheet.plain, [px(WORLD_X0 + 3.5, 0.0), px(WORLD_X0 + 22.0, 0.0)], NAMES["west_road"],
              f_out, INK, at=0.5, tracking=0.12)

    text_path(sheet.plain, roads.drive, NAMES["drive"], f_road, INK, at=0.40, tracking=0.16)
    text_path(sheet.plain, roads.track, NAMES["track"], f_track, INK, at=0.42, tracking=0.16)

    # The country round the town.
    f_woods = font(SERIF_ITALIC, 30)
    text_at(sheet.haloed, *px(8.0, -43.0), NAMES["north_woods"], f_woods, WOODS_INK, tracking=0.3)
    text_at(sheet.haloed, *px(5.0, 58.0), NAMES["south_woods"], f_woods, WOODS_INK, tracking=0.3)
    text_at(sheet.haloed, *px(-100.0, 88.0), NAMES["south_woods"], font(SERIF_ITALIC, 24), WOODS_INK,
            tracking=0.3)
    lake = plan.polys["shore"].mean(axis=0)
    text_at(sheet.haloed, *px(lake[0] - 1.0, lake[1] - 2.0), NAMES["lake"], font(SERIF_ITALIC, 34),
            WATER_INK, tracking=0.22)
    text_path(sheet.knockout, [px(-87.0, 66.0), px(-87.0, 10.0)], NAMES["gorge"],
              font(NARROW_BOLD, 28), WHITE, tracking=0.45, upright=False)
    creek_px, creek_at = creek
    text_path(sheet.haloed, offset(creek_px, -1.8 * M), NAMES["creek"], font(SERIF_ITALIC, 20),
              WATER_INK, at=creek_at, tracking=0.2, upright=False)
    text_at(sheet.haloed, *px(78.0, -20.0), NAMES["hill"], font(SERIF_ITALIC, 28), SOFT_INK,
            tracking=0.35)
    f_small = font(SERIF_ITALIC, 16)
    gx, gy = px((g[2] + g[3]) / 2.0, g[5] + 2.6)
    text_at(sheet.haloed, gx, gy, NAMES["graveyard"][0], f_small, SOFT_INK)
    text_at(sheet.haloed, gx, gy + 0.9 * f_small.size, NAMES["graveyard"][1], f_small, SOFT_INK)


def draw_frame(sheet):
    """The border: a heavy line round the map and a fine one outside it."""
    f = (FRAME[0] * SS, FRAME[1] * SS, FRAME[2] * SS, FRAME[3] * SS)
    sheet.k.rectangle(f, outline=INK, width=4 * SS)
    g = 9 * SS
    sheet.k.rectangle((f[0] - g, f[1] - g, f[2] + g, f[3] + g), outline=INK, width=SS + 1)


def draw_title(sheet):
    """The title in the empty corner past the world's edge, a word to a line so the whole block
    fits the one panel the folded map shows in front; the compass beside it and a scale under
    it."""
    cx, cy = TITLE[0] * SS, TITLE[1] * SS
    title = font(SLAB, 104)
    words = NAMES["town"].split()
    tw = max(title.getlength(w) for w in words)
    for i, word in enumerate(words):
        ImageDraw.Draw(sheet.plain).text((cx, cy + 92 * SS * i), word, font=title,
                                         fill=INK + (255,), anchor="ms")
    y = cy + 92 * SS * (len(words) - 1)
    sheet.k.rectangle([cx - tw / 2, y + 20 * SS, cx + tw / 2, y + 24 * SS], fill=INK)
    text_path(sheet.plain, [(cx - tw / 2, y + 54 * SS), (cx + tw / 2, y + 54 * SS)],
              "STREET MAP", font(NARROW_BOLD, 32), INK, tracking=0.6)
    text_at(sheet.plain, cx, y + 94 * SS, "and Visitors' Guide", font(SERIF_ITALIC, 28), SOFT_INK)
    text_at(sheet.plain, cx, y + 130 * SS, NAMES["compliments"], font(NARROW, 15), SOFT_INK)
    compass(sheet, cx + 330 * SS, cy + 40 * SS)

    # The scale, in feet, under the title, centred with its unit.
    small = font(NARROW, 14)
    seg = 50.0 / FT * M
    ticks = (0, 50, 100, 150, 200)
    bar = seg * (len(ticks) - 1)
    unit = 14 * SS + small.getlength("FEET")
    sx, sy = cx - (bar + unit) / 2.0, y + 172 * SS
    for i in range(len(ticks) - 1):
        sheet.k.rectangle([sx + seg * i, sy, sx + seg * (i + 1), sy + 6 * SS],
                          fill=INK if i % 2 == 0 else WHITE, outline=INK, width=SS)
    for i, t in enumerate(ticks):
        text_at(sheet.plain, sx + seg * i, sy - 11 * SS, str(t), small, INK)
    text_left(sheet.plain, sx + bar + 14 * SS, sy + 3 * SS, "FEET", small, INK)


def print_map(plan, rng):
    """The plates, inked: the map as it came off the press, as multipliers of the paper."""
    sheet = Sheet()
    woods, chasm, water, _ = draw_ground(sheet, plan, rng)
    roads = Roads(plan)
    lots = mask_of(lambda d: [d.rectangle(box_rect(b), fill=255) for b in plan.all("lot")])
    fenced = mask_of(lambda d: [d.rectangle(box_rect(b), fill=255)
                                for b in plan.all("grounds") + plan.all("graveyard")])
    keep = (as_array(grow(Image.fromarray(np.maximum.reduce([
        np.asarray(lots), np.asarray(fenced), np.asarray(roads.paved), np.asarray(roads.unpaved),
        np.asarray(water), np.asarray(chasm), np.asarray(woods)])), int(0.8 * M))) < 0.5)
    draw_contours(sheet, plan, keep)
    creek = draw_gorge(sheet, plan, chasm, rng)
    draw_water_lines(sheet, water)
    draw_roads(sheet, plan, roads)
    draw_lots(sheet, plan)
    draw_houses(sheet, plan)
    g = draw_grounds(sheet, plan, rng, font(NARROW_BOLD, 21))
    draw_labels(sheet, plan, roads, creek, g)

    # Nothing of the map is printed past its frame.
    outside = Image.new("L", (W, H), 255)
    ImageDraw.Draw(outside).rectangle([v * SS for v in FRAME], fill=0)
    for layer in (sheet.colour, sheet.key):
        layer.paste(WHITE, mask=outside)
    for layer in (sheet.haloed, sheet.plain, sheet.knockout):
        layer.paste((0, 0, 0, 0), mask=outside)
    draw_frame(sheet)
    draw_title(sheet)

    return sheet.inked()


# -- The paper ------------------------------------------------------------------------------------

def noise1(n, cell, rng):
    k = n // cell + 3
    return np.interp(np.arange(n) / cell, np.arange(k), rng.random(k)) - 0.5


def ragged(n, rng, amp):
    return amp * (noise1(n, 220 * SS, rng) + 0.6 * noise1(n, 50 * SS, rng)
                  + 0.35 * noise1(n, 9 * SS, rng) + 0.2 * noise1(n, 2 * SS, rng))


def folds(rng):
    """The sheet folded in four across and three down: each fold's position, its tilt, and
    whether it is a valley (+1) or a mountain (-1) as the sheet now lies."""
    p = PAPER_INSET * SS
    vertical = [(p + (W - 2 * p) * i / 4.0 + rng.normal(0, 3 * SS), rng.normal(0, 0.002), s)
                for i, s in zip((1, 2, 3), (1, -1, 1))]
    horizontal = [(p + (H - 2 * p) * j / 3.0 + rng.normal(0, 3 * SS), rng.normal(0, 0.002), s)
                  for j, s in zip((1, 2), (-1, 1))]
    return vertical, horizontal


def sheet_alpha(rng, vertical, horizontal):
    """The sheet's outline: torn a little all round, nicked where the folds meet its edge, a
    corner torn away, and worn through where two folds cross."""
    p = PAPER_INSET * SS
    yy = np.arange(H)[:, None]
    xx = np.arange(W)[None, :]
    left = p + 2 * SS + ragged(H, rng, 9 * SS)[:, None]
    right = W - p - 2 * SS + ragged(H, rng, 9 * SS)[:, None]
    top = p + 2 * SS + ragged(W, rng, 9 * SS)[None, :]
    bottom = H - p - 2 * SS + ragged(W, rng, 9 * SS)[None, :]
    a = (xx > left) & (xx < right) & (yy > top) & (yy < bottom)
    m = Image.fromarray((a * 255).astype(np.uint8))
    d = ImageDraw.Draw(m)

    def tear(points):
        d.polygon([(float(x), float(y)) for x, y in points], fill=0)

    # Nicks where the folds reach the edges, where a folded sheet wears first.
    for f, _, _ in vertical:
        for edge, sign in ((p, 1), (H - p, -1)):
            if rng.random() < 0.7:
                depth, width = rng.uniform(8, 22) * SS, rng.uniform(5, 11) * SS
                tear([(f - width, edge - sign * 20 * SS), (f - width * 0.3, edge + sign * depth * 0.5),
                      (f + rng.normal(0, 2) * SS, edge + sign * depth),
                      (f + width * 0.4, edge + sign * depth * 0.4), (f + width, edge - sign * 20 * SS)])
    for f, _, _ in horizontal:
        for edge, sign in ((p, 1), (W - p, -1)):
            if rng.random() < 0.7:
                depth, width = rng.uniform(8, 22) * SS, rng.uniform(5, 11) * SS
                tear([(edge - sign * 20 * SS, f - width), (edge + sign * depth * 0.5, f - width * 0.3),
                      (edge + sign * depth, f + rng.normal(0, 2) * SS),
                      (edge + sign * depth * 0.4, f + width * 0.4), (edge - sign * 20 * SS, f + width)])
    # A corner torn off.
    cx, cy = W - p, p
    n = 14
    pts = [(cx - 120 * SS, cy - 30 * SS)]
    for i in range(n + 1):
        t = i / n
        pts.append((cx - 96 * SS * (1 - t) + rng.normal(0, 3) * SS,
                    cy + 70 * SS * t + rng.normal(0, 3) * SS))
    pts += [(cx + 30 * SS, cy + 90 * SS), (cx + 30 * SS, cy - 30 * SS)]
    tear(pts)
    # Holes worn through where folds cross.
    for i, j in WORN_THROUGH:
        x, y = vertical[i][0], horizontal[j][0]
        k = 9
        r = rng.uniform(5, 9) * SS
        tear([(x + r * math.cos(2 * math.pi * j / k) * rng.uniform(0.5, 1.3),
               y + r * math.sin(2 * math.pi * j / k) * rng.uniform(0.5, 1.3)) for j in range(k)])
    return as_array(m) > 0.5


def paper_relief(rng, vertical, horizontal):
    """How far the sheet stands off the table, in pixels: the folds, a crumpling, and the grain.
    It is the slope of this that lights the sheet."""
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    h = np.zeros((H, W), np.float32)
    for f, tilt, s in vertical:
        d = xx - (f + tilt * (yy - H / 2.0))
        h += s * (0.045 * np.abs(d) + 1.2 * np.exp(-(d / (4.0 * SS)) ** 2))
    for f, tilt, s in horizontal:
        d = yy - (f + tilt * (xx - W / 2.0))
        h += s * (0.030 * np.abs(d) + 1.2 * np.exp(-(d / (4.0 * SS)) ** 2))
    # Crumpling: many random creases, worked out at a quarter size (where a slope is the same
    # slope, and a height a quarter of one) and smoothed up.
    q = 4
    qy, qx = np.mgrid[0:H // q, 0:W // q].astype(np.float32)
    c = np.zeros_like(qx)
    for _ in range(70):
        ang = rng.uniform(0, math.pi)
        ox, oy = rng.uniform(0, W // q), rng.uniform(0, H // q)
        d = (qx - ox) * math.sin(ang) - (qy - oy) * math.cos(ang)
        reach = rng.uniform(25, 160)
        c += rng.choice((-1.0, 1.0)) * rng.uniform(0.008, 0.028) * np.maximum(reach - np.abs(d), 0)
    h += np.asarray(Image.fromarray(c).resize((W, H), Image.Resampling.BICUBIC)) * q
    h += 6.0 * SS * noise(H, W, 180 * SS, rng)
    h += 0.12 * noise(H, W, 3, rng)
    return h


def age(ink, rng):
    """The printed sheet, kept folded in a kitchen drawer for years, as RGBA in 0..1, and the
    folds it was kept in."""
    vertical, horizontal = folds(rng)
    alpha = sheet_alpha(rng, vertical, horizontal)
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)

    # The paper: yellowed, its grain, and browner in blotches.
    paper = PAPER * (0.985 + 0.03 * noise(H, W, 3, rng))[..., None]
    paper *= (0.97 + 0.06 * noise(H, W, 30 * SS, rng))[..., None]
    blotch = np.clip((noise(H, W, 420 * SS, rng) - 0.45) * 2.5, 0, 1)
    paper *= 1.0 - 0.10 * blotch[..., None] * (1.0 - BROWN)

    # Wear: the ink rubbed off along the folds, where they cross, at the edges, and in scuffs.
    wear = np.zeros((H, W), np.float32)
    crease_dirt = np.zeros((H, W), np.float32)
    broken = noise(H, W, 7 * SS, rng)
    for group, along_x in ((vertical, True), (horizontal, False)):
        for f, tilt, s in group:
            d = (xx - (f + tilt * (yy - H / 2.0))) if along_x else (yy - (f + tilt * (xx - W / 2.0)))
            wear = np.maximum(wear, np.exp(-(d / (6.0 * SS)) ** 2) * (0.35 + 0.65 * broken))
            if s > 0:
                crease_dirt = np.maximum(crease_dirt, np.exp(-(d / (1.4 * SS)) ** 2))
    for fv, _, _ in vertical:
        for fh, _, _ in horizontal:
            r2 = ((xx - fv) ** 2 + (yy - fh) ** 2) / (18.0 * SS) ** 2
            wear = np.maximum(wear, np.exp(-r2) * 0.9)
    edge = ndimage.distance_transform_edt(alpha).astype(np.float32)
    wear = np.maximum(wear, np.exp(-edge / (14.0 * SS)) * 0.7)
    scuff = np.clip((noise(H, W, 46 * SS, rng) - 0.74) * 3.0, 0, 1) * noise(H, W, 3 * SS, rng)
    wear = np.clip(wear + 0.25 * scuff, 0.0, 1.0)

    # The ink: unevenly laid, faded with age, and gone where it wore.
    density = (0.86 + 0.08 * noise(H, W, 5 * SS, rng)) * (1.0 - wear)
    out = paper * (1.0 - (1.0 - ink) * density[..., None])
    # Where it wore, the paper's fibres are lifted and paler, and dirt sits in the valleys.
    out *= (1.0 + 0.05 * wear)[..., None]
    out *= (1.0 - 0.20 * crease_dirt * (0.5 + 0.5 * broken))[..., None]

    # Lit from the top left: the folds and the crumpling.
    h = paper_relief(rng, vertical, horizontal)
    gy, gx = np.gradient(h)
    light = np.array([-0.42, -0.52, 0.74], np.float32)
    light /= np.linalg.norm(light)
    n = 1.0 / np.sqrt(gx * gx + gy * gy + 1.0)
    shade = (-gx * light[0] - gy * light[1] + light[2]) * n / light[2]
    out *= np.clip(shade, 0.7, 1.25)[..., None]

    # Stains.
    out = coffee_ring(out, rng, (1640 * SS, 300 * SS), 74 * SS)
    out = coffee_ring(out, rng, (1712 * SS, 352 * SS), 70 * SS, broken_share=0.65)
    out = water_mark(out, rng, (1010 * SS, 190 * SS), 170 * SS)
    out = foxing(out, rng)
    out = grime(out, rng, xx, yy)

    # Browner toward the edges, as handled paper goes: toward a warm brown, since only taking
    # blue and green away darkens yellowed paper to olive.
    a = (0.38 * np.exp(-edge / (22.0 * SS)) + 0.16 * np.exp(-edge / (60.0 * SS)))[..., None]
    out *= 1.0 - a * (1.0 - EDGE_BROWN)
    rgba = np.concatenate([np.clip(out, 0.0, 1.0), alpha[..., None].astype(np.float32)], axis=-1)
    return rgba, (vertical, horizontal)


def window(out, centre, r):
    x0, y0 = max(int(centre[0] - r), 0), max(int(centre[1] - r), 0)
    x1, y1 = min(int(centre[0] + r), W), min(int(centre[1] + r), H)
    yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
    return (slice(y0, y1), slice(x0, x1)), xx - centre[0], yy - centre[1]


def coffee_ring(out, rng, centre, radius, broken_share=0.25):
    """A mug's ring: a dark tide line where the coffee dried, a faint stain inside it."""
    sl, dx, dy = window(out, centre, radius * 1.4)
    theta = np.arctan2(dy, dx)
    r = np.hypot(dx, dy * 1.04)
    wobble = radius * (1.0 + 0.025 * np.sin(3 * theta + rng.uniform(0, 6)) +
                       0.015 * np.sin(7 * theta + rng.uniform(0, 6)))
    line = np.exp(-((r - wobble) / (2.6 * SS)) ** 2)
    gaps = np.clip(np.sin(theta * 2 + rng.uniform(0, 6)) * 0.5 + 0.5 + rng.uniform(-0.2, 0.2), 0, 1)
    line *= np.where(gaps < broken_share, 0.25, 1.0)
    inside = (r < wobble) * (0.05 + 0.05 * np.clip((r / wobble) ** 4, 0, 1))
    a = np.clip(0.45 * line + inside, 0, 1)[..., None]
    out[sl] *= 1.0 - a * (1.0 - BROWN)
    return out


def water_mark(out, rng, centre, radius):
    """Where something wet stood: a pale yellowed patch with a brown tide line round it."""
    sl, dx, dy = window(out, centre, radius * 1.3)
    h, w = dx.shape
    field = 1.0 - np.hypot(dx, dy) / radius + 0.45 * (noise(h, w, 50 * SS, rng) - 0.5)
    inside = (field > 0).astype(np.float32)
    rim = np.exp(-(field / 0.012) ** 2)
    a = (0.07 * inside + 0.22 * rim)[..., None]
    out[sl] *= 1.0 - a * (1.0 - BROWN)
    return out


def foxing(out, rng):
    """Brown spots, as damp paper takes them."""
    for _ in range(70):
        x, y = rng.uniform(0, W), rng.uniform(0, H)
        r = rng.uniform(1.5, 6.0) * SS
        sl, dx, dy = window(out, (x, y), r * 2.2)
        a = np.exp(-(np.hypot(dx, dy) / r) ** 2) * rng.uniform(0.15, 0.45)
        out[sl] *= 1.0 - a[..., None] * (1.0 - BROWN)
    return out


def grime(out, rng, xx, yy):
    """Grey where hands held it: the bottom corners and a thumb's smudge on the right edge."""
    a = np.zeros((H, W), np.float32)
    for cx, cy, r in ((260 * SS, 1430 * SS, 260 * SS), (1830 * SS, 1420 * SS, 240 * SS),
                      (1990 * SS, 760 * SS, 120 * SS)):
        a += np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / r ** 2)
    a *= 0.16 * (0.4 + 0.6 * noise(H, W, 24 * SS, rng))
    return out * (1.0 - np.clip(a, 0, 1)[..., None] * (1.0 - GRIME))


# -- The ink --------------------------------------------------------------------------------------

class Pen:
    """A sprite drawn by hand: coverage drawn at INK_SS times its size, in output pixels about
    its anchor, the point on the print it marks. A timed pen also keeps a clock, and for every
    pixel the first time the pen reached it, so the mark can be written on in the order it was
    drawn: a line at PEN_SPEED, a lift of PEN_LIFT between lines, and handwriting along its line
    at CHAR_SECONDS a letter."""

    def __init__(self, w, h, anchor, timed=True):
        self.w, self.h, self.anchor = w, h, anchor
        self.cov = Image.new("L", (w * INK_SS, h * INK_SS), 0)
        self.d = ImageDraw.Draw(self.cov)
        self.timed = timed
        self.clock = 0.0
        self.when = np.full((h * INK_SS, w * INK_SS), np.inf, dtype=np.float32)

    def at(self, x, y):
        return ((self.anchor[0] + x) * INK_SS, (self.anchor[1] + y) * INK_SS)

    def _reach(self, cx, cy, r, t):
        """The pen at time t over the disc of radius r round (cx, cy), in coverage pixels."""
        x0, y0 = max(int(cx - r - 1), 0), max(int(cy - r - 1), 0)
        x1, y1 = min(int(cx + r + 2), self.when.shape[1]), min(int(cy + r + 2), self.when.shape[0])
        if x0 >= x1 or y0 >= y1:
            return
        yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32) + 0.5
        inside = (xx - cx) ** 2 + (yy - cy) ** 2 <= (r + 0.5) ** 2
        window = self.when[y0:y1, x0:x1]
        window[inside] = np.minimum(window[inside], t)

    def stroke(self, pts, width, rng=None, wobble=0.6, press=255):
        """A pen line through `pts`, wobbling as a hand does and pressing hardest in its middle."""
        pts = resample(np.asarray(pts, float), 0.3)
        n = len(pts)
        if n < 2:
            return
        if rng is not None and wobble > 0.0:
            j = ndimage.gaussian_filter1d(rng.normal(0.0, 1.0, (n, 2)), 9.0, axis=0)
            pts = pts + j * (wobble / max(float(np.abs(j).max()), 1e-6))
        t = np.linspace(0.0, 1.0, n)
        radius = 0.5 * width * (0.55 + 0.45 * np.sin(np.pi * t)) * INK_SS
        times = self.clock + arc(pts) / PEN_SPEED
        for (x, y), r, when in zip(pts, radius, times):
            cx, cy = self.at(x, y)
            self.d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=press)
            if self.timed:
                self._reach(cx, cy, r, when)
        self.clock = float(times[-1]) + PEN_LIFT

    def over(self, pts, width, rng, passes=2, spread=1.1):
        """A line gone over `passes` times, each a little off the last."""
        for k in range(passes):
            shift = rng.normal(0.0, spread, 2) if k else np.zeros(2)
            self.stroke(np.asarray(pts, float) + shift, width * (1.0 if k == 0 else 0.8), rng)

    def write(self, x, y, text, size, angle=0.0):
        """Handwriting starting at (x, y), its line's middle, turned `angle` degrees."""
        f = ImageFont.truetype(io.BytesIO(fetch(HAND_FONT)), int(size * INK_SS))
        pad = 6 * INK_SS
        w, h = int(f.getlength(text)) + 2 * pad, int(size * INK_SS * 1.7)
        g = Image.new("L", (w, h), 0)
        ImageDraw.Draw(g).text((pad, h / 2), text, font=f, fill=255, anchor="lm",
                               stroke_width=max(INK_SS // 3, 1), stroke_fill=255)
        g = g.rotate(angle, resample=Image.Resampling.BICUBIC, expand=True)
        a = math.radians(angle)
        mid = (w / 2 - pad) / INK_SS
        cx, cy = self.at(x + mid * math.cos(a), y - mid * math.sin(a))
        left, top = int(cx - g.width / 2), int(cy - g.height / 2)
        self.cov.paste(255, (left, top), mask=g)
        if not self.timed:
            return
        # Each glyph pixel reached as far along the line as it lies, a letter's time a letter.
        seconds = CHAR_SECONDS * len(text)
        glyphs = np.asarray(g, dtype=np.float32) > 32.0
        gy, gx = np.mgrid[0:g.height, 0:g.width].astype(np.float32)
        along = ((gx - g.width / 2.0) * math.cos(a) - (gy - g.height / 2.0) * math.sin(a)
                 + (w / 2.0 - pad))
        times = self.clock + np.clip(along / max(w - 2 * pad, 1), 0.0, 1.0) * seconds
        y0, x0 = max(top, 0), max(left, 0)
        y1, x1 = min(top + g.height, self.when.shape[0]), min(left + g.width, self.when.shape[1])
        sub = (slice(y0 - top, y1 - top), slice(x0 - left, x1 - left))
        window = self.when[y0:y1, x0:x1]
        inked = glyphs[sub]
        window[inked] = np.minimum(window[inked], times[sub][inked])
        self.clock += seconds + PEN_LIFT

    def seconds(self):
        """How long the mark takes to write."""
        return max(self.clock - PEN_LIFT, 1e-3)

    def image(self, colour, rng):
        """The sprite at its size for looking at: the coverage in `colour`, the ink skipping a
        little. Also keeps its alpha and, for a timed pen, each pixel's time as a fraction of the
        mark's, which `atlas_image` puts into the marks picture."""
        cov = as_array(self.cov)
        skip = 0.80 + 0.20 * noise(cov.shape[0], cov.shape[1], 2 * INK_SS, rng)
        a = np.clip(cov * skip * 0.94, 0.0, 1.0)
        rgba = np.concatenate([np.broadcast_to(np.array(colour, np.float32) / 255.0, a.shape + (3,)),
                               a[..., None]], axis=-1)
        img = Image.fromarray((rgba * 255.0 + 0.5).astype(np.uint8))
        img = img.convert("RGBa").resize((self.w, self.h), Image.Resampling.LANCZOS).convert("RGBA")
        self.alpha = np.asarray(img)[..., 3]
        self.fraction = np.zeros((self.h, self.w), dtype=np.float32)
        if self.timed:
            # A pixel is written when the pen first reaches any of it, and the soft edge the
            # reduction left round the ink takes its neighbour's time.
            blocks = self.when.reshape(self.h, INK_SS, self.w, INK_SS).min(axis=(1, 3))
            spread = ndimage.minimum_filter(np.where(np.isfinite(blocks), blocks, 1e9), size=3)
            blocks = np.where(np.isfinite(blocks), blocks, spread)
            self.fraction = np.clip(np.where(blocks < 1e8, blocks, 0.0) / self.seconds(), 0.0, 1.0)
        return img

    def atlas_image(self):
        """The sprite as the marks picture holds it: when the pen reached each pixel in red, as a
        fraction of the mark's writing, and the ink in alpha. Its colour is the quad's tint."""
        rgba = np.zeros((self.h, self.w, 4), dtype=np.uint8)
        rgba[..., 0] = (self.fraction * 255.0 + 0.5).astype(np.uint8)
        rgba[..., 3] = self.alpha
        return Image.fromarray(rgba)


def cross(pen, rng, half):
    """An X, each arm gone over twice."""
    for a, b in (((-half, -half * 0.9), (half * 0.95, half)), ((half, -half * 0.95), (-half * 0.9, half))):
        mid = ((a[0] + b[0]) / 2.0 + rng.normal(0, 1.5), (a[1] + b[1]) / 2.0 + rng.normal(0, 1.5))
        pen.over([a, mid, b], 4.2, rng)


def mark_barricade(rng):
    """The north arm of Mill Road, barricaded."""
    pen = Pen(250, 150, (60, 90))
    cross(pen, rng, 26.0)
    pen.write(34, -56, "road blocked", 30, angle=4)
    pen.write(52, -24, "police?", 27, angle=2)
    return pen


def mark_road_end(rng):
    """The street's end at the lip, where the bridge is printed and is not."""
    pen = Pen(320, 180, (250, 120))
    cross(pen, rng, 30.0)
    pen.write(-232, -86, "no bridge!", 38, angle=-5)
    pen.stroke([(-228, -58), (-180, -62), (-128, -70)], 2.6, rng)
    pen.stroke([(-112, -88), (-70, -92), (-44, -46)], 2.2, rng)
    pen.stroke([(-56, -56), (-44, -44), (-40, -60)], 2.2, rng)
    return pen


def mark_cabin(rng):
    """The cabin by the lake, sketched in where it was found: a gable, a chimney with smoke
    going up, a door, and a ring round it."""
    pen = Pen(300, 160, (70, 82))
    w = 2.6
    pen.over([(-14, -3), (-14, 16), (14, 16), (14, -3)], w, rng)
    pen.over([(-19, -1), (0, -19), (19, -1)], w, rng)
    pen.stroke([(7, -10), (7, -20), (12, -20), (12, -5)], w, rng)
    pen.stroke([(-3, 16), (-3, 6), (3, 6), (3, 16)], w * 0.8, rng)
    pen.stroke([(10, -24), (6, -30), (12, -37), (7, -44), (13, -51)], w * 0.7, rng, wobble=1.0)
    ring = [(37 * math.cos(a), 33 * math.sin(a))
            for a in np.linspace(-2.4, -2.4 + 2 * math.pi + 0.5, 60)]
    pen.stroke(ring, 3.4, rng, wobble=2.0)
    pen.write(46, -16, "cabin", 38, angle=-3)
    pen.write(48, 20, "someone here?", 26, angle=-2)
    return pen


MARK_DRAWERS = {"barricade": mark_barricade, "road-end": mark_road_end, "cabin": mark_cabin}


def arrow_strokes(rng):
    """The arrow where you stand, pointing up, in a felt-tip: its outline gone round twice and
    hatched in. Jittered once, so every turned frame is the same arrow."""
    outline = [(0, -27), (15, 14), (0, 5), (-15, 14), (0, -27)]
    strokes = []
    for k in range(2):
        pts = resample(np.array(outline, float) + (rng.normal(0, 0.8, 2) if k else 0), 0.3)
        j = ndimage.gaussian_filter1d(rng.normal(0.0, 1.0, pts.shape), 9.0, axis=0)
        strokes.append((pts + j * (0.7 / np.abs(j).max()), 3.6 if k == 0 else 2.8))
    for y in np.arange(-18.0, 10.0, 3.2):
        half = 15.0 * (y + 27.0) / 41.0 - 2.0
        if half > 1.0:
            strokes.append((np.array([(-half, y + 1.2), (half, y - 1.2)]), 2.2))
    return strokes


def arrow_frame(strokes, theta):
    """The arrow turned `theta` radians clockwise from up, in its own cell."""
    pen = Pen(ARROW_CELL, ARROW_CELL, (ARROW_CELL / 2, ARROW_CELL / 2), timed=False)
    c, s = math.cos(theta), math.sin(theta)
    for pts, width in strokes:
        turned = np.stack([pts[:, 0] * c - pts[:, 1] * s, pts[:, 0] * s + pts[:, 1] * c], axis=1)
        pen.stroke(turned, width, None, wobble=0.0)
    return pen


def draw_marks(plan, rng):
    """The marks atlas: the arrow's turns in a grid, then each find's mark packed under it, each
    as `Pen.atlas_image` keeps it. Returns the atlas, per mark its place, its pen, its sprite to
    look at and its spot in the atlas, and the arrow pointing north-west to look at."""
    atlas = Image.new("RGBA", (MARKS_W, MARKS_H), (0, 0, 0, 0))
    strokes = arrow_strokes(rng)
    arrow = None
    for k in range(ARROW_FRAMES):
        pen = arrow_frame(strokes, 2.0 * math.pi * k / ARROW_FRAMES)
        sprite = pen.image(RED, rng)
        arrow = sprite if k == ARROW_FRAMES * 7 // 8 else arrow
        atlas.paste(pen.atlas_image(), ((k % ARROW_COLS) * ARROW_CELL, (k // ARROW_COLS) * ARROW_CELL))
    top = ARROW_CELL * ((ARROW_FRAMES + ARROW_COLS - 1) // ARROW_COLS)
    pens = [(pid, MARK_DRAWERS[pid](rng)) for pid in plan.places]
    sprites = [(pid, pen, pen.image(BLUE, rng)) for pid, pen in pens]
    spots = pack([(img.width + 2 * MARK_PAD, img.height + 2 * MARK_PAD) for _, _, img in sprites],
                 atlas=(MARKS_W, MARKS_H - top))
    marks = []
    for (pid, pen, img), (x, y) in zip(sprites, spots):
        spot = (x + MARK_PAD, y + top + MARK_PAD)
        atlas.paste(pen.atlas_image(), spot)
        marks.append((pid, pen, img, spot))
    return atlas, marks, arrow


def preview(img, plan, marks, arrow):
    """The print with every find marked and the arrow at the kitchen, as the map would look at
    the end of the walk."""
    out = img.copy()
    for pid, pen, sprite, _ in marks:
        x, y = px(*plan.places[pid])
        out.alpha_composite(sprite, (int(round(x / SS - pen.anchor[0])),
                                     int(round(y / SS - pen.anchor[1]))))
    x, y = px(0.7, 12.4)
    out.alpha_composite(arrow, (int(x / SS - ARROW_CELL / 2), int(y / SS - ARROW_CELL / 2)))
    return out


# -- Output ---------------------------------------------------------------------------------------

def reduce(rgba):
    """The sheet at its final size, its colour held to six bits a channel: the paper's grain
    dithers the steps away, and the file is 40% smaller for it."""
    img = Image.fromarray(np.clip(rgba * 255.0 + 0.5, 0, 255).astype(np.uint8))
    img = img.convert("RGBa").resize((OUT_W, OUT_H), Image.Resampling.LANCZOS).convert("RGBA")
    a = np.asarray(img).copy()
    a[..., :3] = (a[..., :3] // 4) * 4 + 2
    return Image.fromarray(a)


def debug_overlay(img, plan):
    """The plan drawn raw in red over the print, to see that the two agree."""
    out = img.convert("RGB")
    d = ImageDraw.Draw(out)

    def p(x, z):
        a = px(x, z)
        return (a[0] / SS, a[1] / SS)

    for b in plan.boxes:
        a, c = p(b[2], b[4]), p(b[3], b[5])
        d.rectangle([min(a[0], c[0]), min(a[1], c[1]), max(a[0], c[0]), max(a[1], c[1])],
                    outline=(255, 0, 0))
    for _, pts in plan.lines.values():
        d.line([p(*q) for q in pts], fill=(255, 0, 0))
    for pts in plan.polys.values():
        d.polygon([p(*q) for q in pts], outline=(255, 0, 0))
    for x, z in plan.places.values():
        c = p(x, z)
        d.ellipse([c[0] - 6, c[1] - 6, c[0] + 6, c[1] + 6], outline=(255, 0, 255), width=2)
    return out


def save(img, path):
    img.save(path, optimize=True)
    with open(path, "rb") as f:
        data = f.read()
    print("make_map: %s, %d KB, sha256 %s" % (os.path.relpath(path, ROOT), len(data) // 1024,
                                              hashlib.sha256(data).hexdigest()[:16]))


def draw_cover(rng):
    """The folded map's cover, as an old gas-station road map's were printed, to be the thing on
    the fridge door the eye goes to: a red band with the town's name knocked out of it in white, a
    FREE badge, STREET MAP and the guide's line under it in navy, and the manor's drawing. Then
    the coated card's wear -- white where the ink has rubbed off its edges and corners, scuffs and
    scratches -- as RGB at FOLDED_FACE pixels a side."""
    n = FOLDED_FACE * SS
    sheet = Sheet((n, n))

    def at(v):
        return v * SS

    band = at(250)
    sheet.c.rectangle([0, 0, n, band], fill=COVER_RED)
    sheet.c.rectangle([at(14), at(14), n - at(14), band - at(14)], outline=WHITE, width=at(3))
    title = font(SLAB, 92)
    for i, word in enumerate(NAMES["town"].split()):
        text_at(sheet.knockout, n / 2.0, at(78) + at(94) * i, word, title, WHITE, tracking=0.02)
    bx, by, r = n - at(66), at(58), at(34)
    sheet.c.ellipse([bx - r, by - r, bx + r, by + r], fill=COVER_NAVY)
    sheet.c.ellipse([bx - r + at(4), by - r + at(4), bx + r - at(4), by + r - at(4)],
                    outline=WHITE, width=at(2))
    text_at(sheet.knockout, bx, by, "FREE", font(NARROW_BOLD, 22), WHITE, tracking=0.08)

    text_path(sheet.plain, [(at(60), band + at(46)), (n - at(60), band + at(46))], "STREET MAP",
              font(NARROW_BOLD, 40), COVER_NAVY, tracking=0.42)
    text_at(sheet.plain, n / 2.0, band + at(86), "and Visitors' Guide", font(SERIF_ITALIC, 28),
            SOFT_INK)
    draw_manor_at(sheet, n / 2.0, band + at(214), at(98) / 24.0)
    sheet.c.rectangle([0, n - at(22), n, n], fill=COVER_RED)
    text_at(sheet.knockout, n / 2.0, n - at(11), NAMES["chamber"].upper(), font(NARROW_BOLD, 13),
            WHITE, tracking=0.25)
    return age_cover(sheet.inked(), rng)


def age_cover(ink, rng):
    """A coated cover handled for years: its ink rubbed white along its edges and hardest at its
    corners, scuffed and scratched, and gone a little yellow and grimy toward its edges."""
    h, w = ink.shape[:2]
    stock = COVER_STOCK * (0.99 + 0.02 * noise(h, w, 3, rng))[..., None]
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    edge = np.minimum(np.minimum(xx, w - 1 - xx), np.minimum(yy, h - 1 - yy))
    corner = np.minimum(np.minimum(np.hypot(xx, yy), np.hypot(w - xx, yy)),
                        np.minimum(np.hypot(xx, h - yy), np.hypot(w - xx, h - yy)))
    rub = noise(h, w, 5 * SS, rng)
    wear = np.exp(-edge / (5.0 * SS)) * (0.45 + 0.55 * rub)
    wear += 0.7 * np.exp(-corner / (26.0 * SS)) * rub
    wear += np.clip((noise(h, w, 40 * SS, rng) - 0.72) * 3.0, 0, 1) * rub * 0.35
    scratches = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(scratches)
    for _ in range(9):
        x, y = rng.uniform(0, w), rng.uniform(0, h)
        a = rng.uniform(0, math.pi)
        length = rng.uniform(30, 140) * SS
        d.line([(x, y), (x + length * math.cos(a), y + length * math.sin(a))], fill=150,
               width=int(rng.integers(1, 3)))
    wear = np.clip(wear + as_array(scratches), 0.0, 1.0)
    out = stock * (1.0 - (1.0 - ink) * (0.97 * (1.0 - wear))[..., None])
    a = (0.30 * np.exp(-edge / (24.0 * SS)))[..., None]
    out *= 1.0 - a * (1.0 - EDGE_BROWN)
    img = Image.fromarray(np.clip(out * 255.0 + 0.5, 0, 255).astype(np.uint8))
    return img.resize((FOLDED_FACE, FOLDED_FACE), Image.Resampling.LANCZOS)


def folded_set(img, folds, cover):
    """The map as it hangs folded on the fridge: its printed cover, and the sheet's panel that
    shows when it hangs half open, cut along its own folds over the paper's edge brown where the
    sheet is torn, side by side. A world texture set like any other, so stored bottom row first,
    with a flat normal and a roughness map -- the cover glossy, the newsprint not -- small enough
    not to grow the engine's material array. Returns the cover's and the inside's UVs, V up."""
    vertical, horizontal = folds
    xs = [PAPER_INSET] + [f / SS for f, _, _ in vertical] + [OUT_W - PAPER_INSET]
    ys = [PAPER_INSET] + [f / SS for f, _, _ in horizontal] + [OUT_H - PAPER_INSET]
    edge = tuple(int(c * 255 + 0.5) for c in PAPER * EDGE_BROWN) + (255,)
    flat = Image.alpha_composite(Image.new("RGBA", img.size, edge), img).convert("RGB")
    i, j = INSIDE_PANEL
    box = tuple(int(round(v)) for v in (xs[i], ys[j], xs[i + 1], ys[j + 1]))
    inside = flat.crop(box).resize((FOLDED_FACE, FOLDED_FACE), Image.Resampling.LANCZOS)
    albedo = Image.new("RGB", (2 * FOLDED_FACE, FOLDED_FACE))
    albedo.paste(cover, (0, 0))
    albedo.paste(inside, (FOLDED_FACE, 0))
    save(albedo.transpose(Image.Transpose.FLIP_TOP_BOTTOM), FOLDED % "albedo")
    Image.new("RGB", (64, 32), (128, 128, 255)).save(FOLDED % "normal")
    rough = Image.new("RGB", (64, 32), (int(ROUGH_INSIDE * 255 + 0.5),) * 3)
    rough.paste((int(ROUGH_COVER * 255 + 0.5),) * 3, (0, 0, 32, 32))
    rough.save(FOLDED % "rough")
    return (0.0, 0.0, 0.5, 1.0), (0.5, 0.0, 1.0, 1.0)


def write_art(plan, marks, faces):
    """map_art.h: the town map's row of the MapArt table town_map.h declares, as this run drew
    it."""
    def f(v):
        return "%.6ff" % v

    def rect(r):
        return "{%s}" % ", ".join(f(v) for v in r)

    lines = [
        "// Generated by apps/silent/tools/make_map.py -- do not edit; rerun it.",
        "// Included by town_map.c alone: it defines the table town_map.h declares.",
        "#ifndef _SILENT_MAP_ART_H_",
        "#define _SILENT_MAP_ART_H_",
        "",
        "const MapArt MAP_ART[MAP_COUNT] = {",
        "    [MAP_TOWN] =",
        "        {",
        '            .print_file = "%s",' % os.path.basename(PRINT),
        '            .marks_file = "%s",' % os.path.basename(MARKS),
        "            .print_size = {%s, %s}," % (f(OUT_W), f(OUT_H)),
        "            .marks_size = {%s, %s}," % (f(MARKS_W), f(MARKS_H)),
        "            .origin = {%s, %s}," % (f(WORLD_X0), f(WORLD_Z0)),
        "            .at = {%s, %s}," % (f(FRAME[0]), f(FRAME[1])),
        "            .px_per_m = %s," % f(SCALE),
        "            .arrow_frames = %d," % ARROW_FRAMES,
        "            .arrow_cell = %d," % ARROW_CELL,
        "            .arrow_cols = %d," % ARROW_COLS,
        "            .mark_count = %d," % len(marks),
        "            .marks =",
        "                {",
    ]
    # Laid out as clang-format lays it, so the commit hook leaves the file as written.
    for pid, pen, img, (x, y) in marks:
        uv = (x / MARKS_W, y / MARKS_H, (x + img.width) / MARKS_W, (y + img.height) / MARKS_H)
        lines += ["                    {PLACE_%s," % pid.upper().replace("-", "_"),
                  "                     %s," % rect(uv),
                  "                     {%s, %s}," % (f(img.width), f(img.height)),
                  "                     {%s, %s}," % (f(pen.anchor[0]), f(pen.anchor[1])),
                  "                     %s}," % f(pen.seconds())]
    lines += [
        "                },",
        "            .folded_cover = %s," % rect(faces[0]),
        "            .folded_inside = %s," % rect(faces[1]),
        "            .seed = %du," % plan.seed,
        "        },",
        "};",
        "",
        "#endif // _SILENT_MAP_ART_H_",
        "",
    ]
    with open(HEADER, "w") as out:
        out.write("\n".join(lines))
    print("make_map: %s" % os.path.relpath(HEADER, ROOT))


def main():
    plan = Plan(PLAN)
    # The print, the paper and the ink each draw from their own stream, so a change to what is
    # printed leaves the paper's wear and the hand's strokes where they were.
    print_rng, paper_rng, ink_rng = (np.random.default_rng([SEED, k]) for k in range(3))
    sheet, folds = age(print_map(plan, print_rng), paper_rng)
    img = reduce(sheet)
    save(img, PRINT)
    atlas, marks, arrow = draw_marks(plan, ink_rng)
    save(atlas, MARKS)
    cover_rng = np.random.default_rng([SEED, 3])
    write_art(plan, marks, folded_set(img, folds, draw_cover(cover_rng)))
    if "--debug" in sys.argv[1:]:
        debug_overlay(img, plan).save(DEBUG)
        preview(img, plan, marks, arrow).save(PREVIEW)
        print("make_map: %s, %s" % (os.path.relpath(DEBUG, ROOT), os.path.relpath(PREVIEW, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
