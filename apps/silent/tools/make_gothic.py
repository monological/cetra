"""Make the pictures the Gothic house (spec 13.13) places whole, and its leaded glass.

One picture, gothic_albedo.png, holds every card -- carved mahogany panel bays and a frieze,
the Persian rug and the runners' pieces, three stained-glass lancets, strips of book spines, a
carved coat of arms, a cast-iron fireback and the stand-in fire's flames -- with a normal map
beside it drawn from each card's own height field, so carving catches the light. Where each
card sits and its size in metres are written to apps/silent/src/gothic.h, with the edges of
every spine in each strip, so a shelf can cut one book's spine out whole. That header is
generated and never edited.

leaded_glass_*.png is a tile of its own, not a card: diamond quarries in lead, laid by the
world's metres over the great hall's tall lancets like any photo set.

The mahogany is Poly Haven's lacquered_cherry_wood, the clock case's own; the spines' leather
is brown_leather; both CC0, at 1k. Everything else is drawn here. Stored BOTTOM ROW FIRST, as
fetch_textures.py explains. Run from anywhere:

    python3 apps/silent/tools/make_gothic.py
"""

import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

from fetch_textures import CACHE_DIR, ROOT, open_polyhaven
from make_cards import noise, place, save_rough, to_array, to_image, veneer, write_header

OUT_DIR = os.path.join(ROOT, "assets", "textures", "silent")
HEADER = os.path.join(ROOT, "apps", "silent", "src", "gothic.h")
ATLAS = (2048, 1024)
# The roughness map is this much smaller than the picture: it is a layer of the engine's
# material array, which brings every layer up to the largest, and at 256 it adds nothing.
ROUGH_SCALE = 8
TILE = 256  # the leaded glass's own tile, like every photo set's

# Pixels a metre: carving needs the most, a rug seen from standing height the least.
PANEL_PX = 440
RUG_PX = 160
GLASS_PX = 280
BOOK_PX = 420
STONE_PX = 320

GILT = np.array([0.80, 0.62, 0.28])
LEAD = np.array([0.05, 0.05, 0.05])


def grid(h, w):
    """Pixel centres as (u, v) in 0..1 across and DOWN the card, top row first."""
    v, u = np.mgrid[0:h, 0:w].astype(np.float32)
    return (u + 0.5) / w, (v + 0.5) / h


def mask_of(img):
    return np.asarray(img, dtype=np.float32) / 255.0


def soften(m, radius):
    img = Image.fromarray((np.clip(m, 0, 1) * 255).astype(np.uint8))
    return mask_of(img.filter(ImageFilter.GaussianBlur(radius)))



def normal_from_height(height, px_per_m, depth_m):
    """An OpenGL normal map from a 0..1 height field `depth_m` deep at most: +Y toward the
    top of the picture as it is drawn here, which the row flip on saving keeps as +V."""
    h = height * depth_m * px_per_m  # in pixels of height
    dx = np.gradient(h, axis=1)
    dy = np.gradient(h, axis=0)
    n = np.stack([-dx, dy, np.ones_like(h)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def card(name, albedo, height, px, depth, rough, alpha=None):
    h, w = albedo.shape[:2]
    rgba = np.concatenate([albedo, (alpha if alpha is not None else np.ones((h, w)))[..., None]],
                          axis=-1)
    normal = normal_from_height(height, px, depth) if height is not None else None
    if np.isscalar(rough):
        rough = np.full((h, w), rough, dtype=np.float32)
    return {"name": name, "pixels": rgba, "normal": normal, "rough": rough,
            "m": (w / px, h / px)}


# ---------------------------------------------------------------------------------------------
# Carved mahogany
# ---------------------------------------------------------------------------------------------

def mahogany(w, h):
    """The clock case's lacquered cherry, deepened toward mahogany's red-brown."""
    return veneer("lacquered_cherry_wood", w, h, turn=True) * np.array([1.05, 0.8, 0.66])


def framed(w, h, border):
    """A panel bay's frame -- stiles and rails standing at 1 -- chamfered down to a field at 0.25."""
    u, v = grid(h, w)
    edge = np.minimum(np.minimum(u * w, (1 - u) * w), np.minimum(v * h, (1 - v) * h))
    return 0.25 + 0.75 * np.clip((border - edge) / (0.25 * border) + 1.0, 0.0, 1.0)


def carve(albedo, height):
    """Shade a carving into its colour: the low parts gather dark, as they do in wood."""
    return albedo * (0.45 + 0.55 * soften(height, 2))[..., None]


def panel_linenfold():
    """Linenfold: the field carved as cloth hung in vertical folds, each fold's top and foot
    cut in the curl a napkin's hem makes."""
    w, h = int(0.55 * PANEL_PX), int(0.85 * PANEL_PX)
    border = int(0.06 * PANEL_PX)
    height = framed(w, h, border)
    u, v = grid(h, w)
    x = (u * w - border * 1.4) / (w - border * 2.8)
    y = (v * h - border * 1.6) / (h - border * 3.2)
    folds = 4
    t = np.clip(x, 0, 1) * folds
    ridge = np.abs(np.sin(np.pi * t)) ** 0.5
    crease = 1.0 - np.exp(-((t - np.round(t)) ** 2) / 0.004)
    fold = 0.1 + 0.85 * ridge * crease
    # The hems: each fold's end follows a scalloped line, deeper at its middle.
    phase = t - np.floor(t)
    hem = 0.06 + 0.07 * np.sin(np.pi * phase)
    inside = (x > 0) & (x < 1) & (y > hem) & (y < 1 - hem)
    height = np.where(inside, np.maximum(height, fold), height)
    return card("panel_linenfold", carve(mahogany(w, h), height), height, PANEL_PX, 0.02, 0.45)


def arch_points(x0, x1, spring, rise, n=24):
    """A pointed arch's head in pixels, from (x0, spring) over to (x1, spring), y DOWN."""
    half = 0.5 * (x1 - x0)
    r = (half * half + rise * rise) / (2.0 * half)
    pts = []
    for side in (0, 1):
        cx = x0 + r if side == 0 else x1 - r
        end = math.atan2(rise, half - r)
        for i in range(n + 1):
            t = math.pi + (end - math.pi) * i / n
            px = cx + (r * math.cos(t) if side == 0 else -r * math.cos(t))
            py = spring - r * math.sin(t)
            pts.append((px, py))
    left, right = pts[:n + 1], pts[n + 1:]
    return left + right[::-1][1:]


def panel_tracery():
    """Blind tracery: a lancet carved in the field, its head cusped in three, and a ring over
    it holding a quatrefoil -- the window's own shapes in wood."""
    w, h = int(0.55 * PANEL_PX), int(0.85 * PANEL_PX)
    border = int(0.06 * PANEL_PX)
    height = framed(w, h, border)
    rib = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(rib)
    x0, x1 = border * 1.6, w - border * 1.6
    spring, rise = h * 0.42, (x1 - x0) * 0.95
    head = arch_points(x0, x1, spring, rise)
    outline = [(x0, h - border * 1.6)] + head + [(x1, h - border * 1.6)]
    width = max(4, int(0.012 * PANEL_PX))
    draw.line(outline + [outline[0]], fill=255, width=width, joint="curve")
    # Three cusps hanging inside the head.
    cx, top = 0.5 * (x0 + x1), spring - rise
    for k, dx in enumerate((-0.28, 0.0, 0.28)):
        r = (x1 - x0) * (0.17 if k != 1 else 0.2)
        cy = spring - rise * (0.35 if k != 1 else 0.62)
        draw.ellipse([cx + dx * (x1 - x0) - r, cy - r, cx + dx * (x1 - x0) + r, cy + r],
                     outline=255, width=width - 1)
    # The ring and its quatrefoil, over the arch in the bay's top.
    ry = (top - border) * 0.5 + border * 0.5
    rr = min((x1 - x0) * 0.32, (top - border * 1.2) * 0.5)
    if rr > 6:
        draw.ellipse([cx - rr, ry - rr, cx + rr, ry + rr], outline=255, width=width)
        q = rr * 0.42
        for ox, oy in ((0, -q), (0, q), (-q, 0), (q, 0)):
            draw.ellipse([cx + ox - q, ry + oy - q, cx + ox + q, ry + oy + q], outline=255,
                         width=max(2, width - 2))
    ribs = soften(mask_of(rib), 1.2)
    height = np.maximum(height, 0.25 + 0.6 * ribs)
    return card("panel_tracery", carve(mahogany(w, h), height), height, PANEL_PX, 0.02, 0.45)


def frieze():
    """A frieze of quatrefoils in squares between two rails, to run along the top of the
    panelling; it tiles end to end."""
    w, h = int(1.1 * PANEL_PX), int(0.16 * PANEL_PX)
    height = np.full((h, w), 0.25, dtype=np.float32)
    u, v = grid(h, w)
    rails = (v < 0.14) | (v > 0.86)
    height[rails] = 1.0
    rib = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(rib)
    cell = h * 0.72
    count = int(round(w / cell))
    cell = w / count
    for i in range(count):
        cx, cy = (i + 0.5) * cell, 0.5 * h
        s = h * 0.34
        draw.rectangle([cx - s, cy - s, cx + s, cy + s], outline=255, width=3)
        q = s * 0.45
        for ox, oy in ((0, -q), (0, q), (-q, 0), (q, 0)):
            draw.ellipse([cx + ox - q, cy + oy - q, cx + ox + q, cy + oy + q], outline=255,
                         width=3)
    height = np.maximum(height, 0.25 + 0.6 * soften(mask_of(rib), 1.0))
    return card("frieze", carve(mahogany(w, h), height), height, PANEL_PX, 0.015, 0.45)


# ---------------------------------------------------------------------------------------------
# The Persian rug and the runners
# ---------------------------------------------------------------------------------------------

RED = np.array([0.46, 0.08, 0.07])
INDIGO = np.array([0.09, 0.11, 0.27])
IVORY = np.array([0.82, 0.75, 0.60])
GOLD = np.array([0.72, 0.52, 0.20])
BROWN = np.array([0.17, 0.09, 0.05])
GREEN = np.array([0.19, 0.29, 0.19])


def paint(a, mask, colour):
    return a * (1 - mask[..., None]) + mask[..., None] * colour


def rosette(dx, dy, r, petals=8):
    """A petalled flower of radius r about the origin: 1 inside."""
    rad = np.hypot(dx, dy)
    ang = np.arctan2(dy, dx)
    edge = r * (0.72 + 0.28 * np.abs(np.cos(0.5 * petals * ang)))
    return (rad < edge).astype(np.float32)


def lattice(u_m, v_m, period, a):
    """The field's all-over pattern: a diamond lattice of fine lines with a small flower in
    every cell, in metres so it is the same size on the rug and the runners."""
    du = (u_m / period) % 1.0 - 0.5
    dv = (v_m / period) % 1.0 - 0.5
    line = (np.abs(np.abs(du) + np.abs(dv) - 0.5) < 0.035).astype(np.float32)
    a = paint(a, line, GOLD * 0.8)
    flower = rosette(du * period, dv * period, period * 0.16, 6)
    a = paint(a, flower, IVORY * 0.9)
    a = paint(a, rosette(du * period, dv * period, period * 0.06, 4), INDIGO)
    return a


def border(a, inner_u, inner_v, band):
    """Guard stripes and a main band round a field, from each edge in: `inner_u`/`inner_v` are
    each pixel's distance in metres to the nearest long and short edge, `band` the main band's
    width in metres. Returns the colour and the distance to the field."""
    d = np.minimum(inner_u, inner_v)
    guard = 0.025
    a = np.where((d < guard)[..., None], GOLD, a)
    a = np.where(((d >= guard) & (d < guard + band))[..., None], INDIGO, a)
    a = np.where(((d >= guard + band) & (d < 2 * guard + band))[..., None], IVORY, a)
    # A running chain of rosettes down the middle of the main band.
    mid = guard + 0.5 * band
    along = np.where(inner_u < inner_v, inner_v, inner_u)
    step = band * 1.1
    s = (along / step) % 1.0 - 0.5
    near = np.abs(d - mid)
    a = paint(a, rosette(s * step, near, band * 0.3) * (np.abs(d - mid) < band * 0.45), RED)
    a = paint(a, rosette(s * step, near, band * 0.12) * (np.abs(d - mid) < band * 0.45), GOLD)
    dots = (np.abs(d - (1.5 * guard + band)) < 0.006) & ((along / 0.03) % 1.0 < 0.5)
    a = paint(a, dots.astype(np.float32), RED)
    return a, 2 * guard + band


def pile(a, rng, h, w):
    """Wear and pile: the wool's grain, paler where it is walked on, and a fine height for the
    normal map."""
    grain = rng.random((h, w)).astype(np.float32)
    wear = noise(h, w, 24, rng)
    a = a * (0.88 + 0.12 * grain[..., None]) * (0.85 + 0.25 * wear[..., None])
    return a, 0.5 + 0.5 * grain


def rug(rng):
    """The great hall's rug, 3 by 4 metres: borders round a madder field under a lattice, a
    lobed indigo medallion with an ivory heart, quarter-medallions in the field's corners, and
    a short fringe at each end."""
    w_m, h_m = 3.0, 4.0
    w, h = int(w_m * RUG_PX), int(h_m * RUG_PX)
    u, v = grid(h, w)
    um, vm = u * w_m, v * h_m
    fringe = 0.05
    a = np.zeros((h, w, 3), dtype=np.float32) + RED
    a = lattice(um, vm, 0.2, a)
    edge_u = np.minimum(um, w_m - um)
    edge_v = np.minimum(vm, h_m - vm) - fringe
    a, field = border(a, edge_u, edge_v, 0.24)
    # The medallion: a lobed diamond, an ivory heart, a red flower in it; pendants above and
    # below; and a quarter of it in each corner of the field.
    cx, cy = 0.5 * w_m, 0.5 * h_m
    dx, dy = um - cx, vm - cy
    lobes = 0.62 + 0.06 * np.cos(8 * np.arctan2(dy, dx))
    med = (np.abs(dx) / 0.85 + np.abs(dy) / 1.15) < lobes
    a = paint(a, med.astype(np.float32), INDIGO)
    a = paint(a, ((np.abs(dx) / 0.85 + np.abs(dy) / 1.15) < lobes - 0.04).astype(np.float32) *
              (((np.abs(dx) / 0.85 + np.abs(dy) / 1.15) > lobes - 0.07)), GOLD)
    heart = (np.abs(dx) / 0.45 + np.abs(dy) / 0.6) < 0.5
    a = paint(a, heart.astype(np.float32), IVORY)
    a = paint(a, rosette(dx, dy, 0.13), RED)
    a = paint(a, rosette(dx, dy, 0.05), GOLD)
    for sy in (-1, 1):
        py = cy + sy * 0.95
        a = paint(a, ((np.abs(dx) / 0.16 + np.abs(vm - py) / 0.2) < 1).astype(np.float32), INDIGO)
        a = paint(a, rosette(dx, vm - py, 0.05), IVORY)
    inset = field + 0.02
    for sx in (-1, 1):
        for sy in (-1, 1):
            qx, qy = cx + sx * (cx - inset), cy + sy * (cy - inset - fringe)
            q = (np.abs(um - qx) / 0.55 + np.abs(vm - qy) / 0.7) < 1
            a = paint(a, q.astype(np.float32), INDIGO)
            a = paint(a, rosette(um - qx, vm - qy, 0.12), GOLD)
    # The fringe: ivory threads past each end.
    ends = (np.minimum(vm, h_m - vm) < fringe)
    threads = ((um / 0.008) % 1.0 < 0.55).astype(np.float32)
    a = np.where(ends[..., None], IVORY * (0.6 + 0.4 * threads[..., None]), a)
    a, height = pile(a, rng, h, w)
    height = np.where(ends, 0.2 * threads, height)
    return card("rug", a, height, RUG_PX, 0.004, 0.95)


def runner(rng):
    """A runner's two pieces, 0.75 m wide: a middle a metre long whose lattice and border
    chain repeat at that length, so pieces end to end are one runner, and an end with its
    border across and a fringe."""
    w_m = 0.75
    out = []
    for name, h_m, end in (("runner", 1.0, False), ("runner_end", 0.3, True)):
        w, h = int(w_m * RUG_PX), int(h_m * RUG_PX)
        u, v = grid(h, w)
        um, vm = u * w_m, v * h_m
        a = np.zeros((h, w, 3), dtype=np.float32) + RED
        a = lattice(um, vm, 0.125, a)
        edge_u = np.minimum(um, w_m - um)
        # An end piece's border runs across its foot, the runner's end; its fringe past that.
        fringe = 0.04
        edge_v = (h_m - vm - fringe) if end else np.full_like(vm, 99.0)
        a, _ = border(a, edge_u, edge_v, 0.1)
        ends = np.zeros_like(vm, dtype=bool)
        if end:
            ends = (h_m - vm) < fringe
            threads = ((um / 0.008) % 1.0 < 0.55).astype(np.float32)
            a = np.where(ends[..., None], IVORY * (0.6 + 0.4 * threads[..., None]), a)
        a, height = pile(a, rng, h, w)
        height = np.where(ends, 0.2, height)
        out.append(card(name, a, height, RUG_PX, 0.004, 0.95))
    return out


# ---------------------------------------------------------------------------------------------
# Stained glass
# ---------------------------------------------------------------------------------------------

RUBY = (0.78, 0.10, 0.12)
COBALT = (0.14, 0.28, 0.78)
EMERALD = (0.12, 0.56, 0.28)
AMBER = (0.95, 0.70, 0.18)
VIOLET = (0.48, 0.18, 0.58)
CLEAR = (0.84, 0.88, 0.80)

# The study's lancet: 0.7 m wide, springing 1.8 m up, its head rising 0.7 m; two lights under
# a ring, as ornament.c's tracery parts it.
LANCET_W, LANCET_SPRING, LANCET_RISE = 0.7, 1.8, 0.7


def quarries(draw, ld, w, h, q, base, tone, spread, width, rng):
    """Diamond panes `q` pixels across over w by h, a pane past every edge, each `base` toned by
    tone..tone + spread, and their lead `width` pixels wide."""
    for gy in range(-q, h + q, q):
        for gx in range(-q, w + q, q):
            for off in (0, q // 2):
                cx, cy = gx + off, gy + off
                t = tone + spread * rng.random()
                poly = [(cx, cy - q // 2), (cx + q // 2, cy), (cx, cy + q // 2), (cx - q // 2, cy)]
                draw.polygon(poly, fill=tuple(int(255 * min(1.0, k * t)) for k in base))
                ld.line(poly + [poly[0]], fill=255, width=width)


def lancet(name, scheme, rng):
    """A lancet's glass, laid out for its tracery: each light a column of roundels on a field
    in diamond leading inside a border of short panes; a rose in the ring; the spandrels
    starred. Lead between every piece; each piece mottled as old glass is."""
    w = int(LANCET_W * GLASS_PX)
    h = int((LANCET_SPRING + LANCET_RISE) * GLASS_PX)
    img = Image.new("RGB", (w, h), (0, 0, 0))
    lead = Image.new("L", (w, h), 0)
    draw, ld = ImageDraw.Draw(img), ImageDraw.Draw(lead)
    field, roundel, inner, border_a, border_b = scheme
    px = GLASS_PX
    mull = 0.05 * px
    light_w = (w - mull) * 0.5
    # The field: diamond panes in the background colour, a little varied.
    quarries(draw, ld, w, h, int(0.09 * px), field, 0.85, 0.25, 2, rng)
    # Each light: a border of short panes, alternating, and three roundels up it.
    for side in (0, 1):
        x0 = side * (light_w + mull)
        x1 = x0 + light_w
        b = 0.035 * px
        for y in range(0, h, int(0.06 * px)):
            c = border_a if (y // int(0.06 * px)) % 2 else border_b
            colour = tuple(int(255 * k) for k in c)
            for xa, xb in ((x0, x0 + b), (x1 - b, x1)):
                draw.rectangle([xa, y, xb, y + 0.06 * px], fill=colour)
                ld.rectangle([xa, y, xb, y + 0.06 * px], outline=255, width=2)
        cx = 0.5 * (x0 + x1)
        r = 0.4 * light_w
        for k in range(3):
            cy = h - (0.35 + 0.55 * k) * px
            draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=tuple(int(255 * c) for c in roundel))
            ld.ellipse([cx - r, cy - r, cx + r, cy + r], outline=255, width=3)
            # In each roundel a quatrefoil, and a jewel at its heart.
            s = r * 0.42
            for ox, oy in ((0, -s), (0, s), (-s, 0), (s, 0)):
                bb = [cx + ox - s, cy + oy - s, cx + ox + s, cy + oy + s]
                draw.ellipse(bb, fill=tuple(int(255 * c) for c in inner))
                ld.ellipse(bb, outline=255, width=2)
            j = r * 0.22
            draw.ellipse([cx - j, cy - j, cx + j, cy + j], fill=tuple(int(255 * c) for c in AMBER))
            ld.ellipse([cx - j, cy - j, cx + j, cy + j], outline=255, width=2)
    # The rose in the ring, between the lights' heads and the window's point.
    sub_rise = 0.85 * (LANCET_W - 0.05) * 0.5
    low, high = LANCET_SPRING + sub_rise + 0.035, LANCET_SPRING + LANCET_RISE
    ring_r = min(0.42 * (high - low), 0.3 * LANCET_W) * px
    rcx, rcy = 0.5 * w, h - 0.5 * (low + high) * px
    for k in range(8):
        a0 = 2 * math.pi * k / 8
        pts = [(rcx, rcy)] + [(rcx + ring_r * math.cos(a0 + t), rcy + ring_r * math.sin(a0 + t))
                              for t in np.linspace(0, 2 * math.pi / 8, 5)]
        colour = roundel if k % 2 else inner
        draw.polygon(pts, fill=tuple(int(255 * c) for c in colour))
        ld.line(pts + [pts[0]], fill=255, width=2)
    draw.ellipse([rcx - ring_r * 0.3, rcy - ring_r * 0.3, rcx + ring_r * 0.3, rcy + ring_r * 0.3],
                 fill=tuple(int(255 * c) for c in AMBER))
    a = to_array(img)
    # Mottle: old glass is never one colour across a piece.
    a = a * (0.82 + 0.3 * noise(h, w, 9, rng)[..., None])
    lines = mask_of(lead)
    a = a * (1 - lines[..., None]) + lines[..., None] * LEAD
    height = 0.2 + 0.8 * soften(lines, 0.8)
    rough = 0.08 + 0.5 * lines
    return card(name, a, height, GLASS_PX, 0.003, rough)


GLASS_SCHEMES = {
    # field, roundels, quatrefoils, border panes
    "glass_rose": ((0.20, 0.26, 0.62), RUBY, (0.92, 0.85, 0.55), RUBY, AMBER),
    "glass_lily": ((0.18, 0.46, 0.30), CLEAR, AMBER, EMERALD, VIOLET),
    "glass_star": ((0.42, 0.16, 0.50), COBALT, AMBER, AMBER, RUBY),
}


def leaded_glass(rng):
    """A tile of diamond quarries: pale old glass, each pane its own tint, in lead -- laid by
    the world's metres, so its size is the material's repeat, not this file's."""
    s = TILE
    img = Image.new("RGB", (s, s))
    lead = Image.new("L", (s, s), 0)
    draw, ld = ImageDraw.Draw(img), ImageDraw.Draw(lead)
    # Four quarries across the tile, so it repeats.
    quarries(draw, ld, s, s, s // 4, (0.78, 0.84, 0.76), 0.9, 0.12, 3, rng)
    a = to_array(img) * (0.9 + 0.12 * noise(s, s, 12, rng)[..., None])
    lines = mask_of(lead)
    a = a * (1 - lines[..., None]) + lines[..., None] * LEAD
    height = soften(lines, 1.0)
    normal = normal_from_height(height, s / 0.4, 0.004)
    rough = 0.1 + 0.5 * lines
    return a, normal, rough


# ---------------------------------------------------------------------------------------------
# Books
# ---------------------------------------------------------------------------------------------

LEATHERS = [np.array(c) for c in ((0.36, 0.07, 0.06), (0.10, 0.20, 0.12), (0.10, 0.12, 0.24),
                                  (0.28, 0.16, 0.08), (0.08, 0.06, 0.05), (0.42, 0.30, 0.16),
                                  (0.22, 0.06, 0.10))]
STRIP_W, STRIP_H = 1.2, 0.3
SPINE_STRIPS = 4


def spines(name, rng):
    """A strip of book spines side by side, each its own leather with raised bands and gilt
    rules, a dark title label with gilt in it, and wear at head and foot. Returns the card and
    the edges between spines in metres from the strip's left."""
    w, h = int(STRIP_W * BOOK_PX), int(STRIP_H * BOOK_PX)
    leather = veneer("brown_leather", w, h)
    lum = leather.mean(axis=-1, keepdims=True)
    a = np.zeros((h, w, 3), dtype=np.float32)
    height = np.full((h, w), 0.5, dtype=np.float32)
    rough = np.full((h, w), 0.6, dtype=np.float32)
    edges = [0.0]
    x = 0
    while True:
        bw = int(rng.uniform(0.022, 0.06) * BOOK_PX)
        if x + bw > w:
            break
        colour = LEATHERS[rng.integers(len(LEATHERS))] * rng.uniform(0.75, 1.15)
        sl = slice(x, x + bw)
        a[:, sl] = colour * (0.55 + 0.9 * lum[:, sl])
        # A rounded spine: lighter down its middle.
        across = np.linspace(-1, 1, bw)
        a[:, sl] *= (0.75 + 0.25 * np.sqrt(np.clip(1 - across ** 2, 0, 1)))[None, :, None]
        height[:, sl] = 0.4 + 0.4 * np.sqrt(np.clip(1 - across ** 2, 0, 1))[None, :]
        # Raised bands with gilt rules either side, and a dark label with gilt in it.
        bands = rng.integers(2, 5)
        for k in range(bands):
            y = int(h * (0.18 + 0.64 * k / max(1, bands - 1)))
            a[y - 3:y + 3, sl] *= 1.25
            height[y - 3:y + 3, sl] += 0.3
            for g in (y - 5, y + 5):
                a[g:g + 1, sl] = GILT
                rough[g:g + 1, sl] = 0.3
        ly0 = int(h * rng.uniform(0.25, 0.32))
        ly1 = ly0 + int(h * 0.12)
        a[ly0:ly1, x + 2:x + bw - 2] = LEATHERS[4] * 1.2
        for line in range(ly0 + 4, ly1 - 3, 5):
            span = rng.uniform(0.4, 0.85)
            g0 = x + int(bw * (0.5 - span / 2))
            g1 = x + int(bw * (0.5 + span / 2))
            a[line:line + 2, g0:g1] = GILT
            rough[line:line + 2, g0:g1] = 0.3
        # Rubbed at head and foot, and a dark seam to the next book.
        for y0, y1 in ((0, int(h * 0.04)), (h - int(h * 0.04), h)):
            a[y0:y1, sl] *= 0.6
        a[:, x:x + 1] *= 0.25
        x += bw
        edges.append(x / BOOK_PX)
    a = a[:, :x]
    c = card(name, a, height[:, :x], BOOK_PX, 0.006, rough[:, :x])
    return c, edges


# ---------------------------------------------------------------------------------------------
# Stone and iron for the hearth
# ---------------------------------------------------------------------------------------------

def coat_of_arms(rng):
    """The hearth hood's carving: a heater shield in a moulded border, a chevron across it
    between three rosettes, under a helm's crest of leaves -- the relief in its height, on a
    ground of the hood's own stone, the castle scan's mean colour, so the panel reads as cut
    into it rather than laid on it."""
    w, h = int(0.7 * STONE_PX), int(0.85 * STONE_PX)
    shape = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(shape)
    sx0, sx1, sy0 = w * 0.18, w * 0.82, h * 0.26
    sy1 = h * 0.93
    shield = [(sx0, sy0), (sx1, sy0), (sx1, sy0 + (sy1 - sy0) * 0.45),
              (0.5 * w, sy1), (sx0, sy0 + (sy1 - sy0) * 0.45)]
    draw.polygon(shield, fill=150)
    draw.line(shield + [shield[0]], fill=255, width=6)
    cy = sy0 + (sy1 - sy0) * 0.5
    draw.line([(sx0 + 6, cy + 25), (0.5 * w, cy - 25), (sx1 - 6, cy + 25)], fill=230, width=14)
    for rx, ry in ((0.32, 0.36), (0.68, 0.36), (0.5, 0.78)):
        px_, py_ = w * rx, sy0 + (sy1 - sy0) * ry
        draw.ellipse([px_ - 14, py_ - 14, px_ + 14, py_ + 14], fill=230)
        draw.ellipse([px_ - 5, py_ - 5, px_ + 5, py_ + 5], fill=150)
    # The crest: leaves fanning up from the shield's top.
    for k in range(7):
        ang = math.pi * (0.15 + 0.7 * k / 6)
        ex, ey = 0.5 * w + math.cos(ang) * w * 0.32, sy0 - math.sin(ang) * h * 0.2
        draw.line([(0.5 * w, sy0), (ex, ey)], fill=200, width=10)
        draw.ellipse([ex - 9, ey - 9, ex + 9, ey + 9], fill=210)
    height = soften(mask_of(shape), 2.0)
    scan = Image.open(os.path.join(OUT_DIR, "medieval_blocks_03_albedo.png"))
    ground = to_array(scan).reshape(-1, 3).mean(axis=0)
    stone = ground * (0.85 + 0.25 * noise(h, w, 10, rng)[..., None])
    # The sunk ground a little darker than the face round it, the relief a little lighter: the
    # normal map carries the carving, and a ground much darker than the hood was a dark tile.
    a = stone * (0.9 + 0.2 * height[..., None] / max(1e-3, height.max()))
    return card("coat_of_arms", a, height, STONE_PX, 0.04, 0.85)


def fireback(rng):
    """The cast-iron plate at the back of the hearth: an arched panel in a raised rope border,
    a tree in it, blackened and rusted at its foot."""
    w, h = int(0.9 * STONE_PX), int(0.75 * STONE_PX)
    shape = Image.new("L", (w, h), 60)
    draw = ImageDraw.Draw(shape)
    head = arch_points(w * 0.08, w * 0.92, h * 0.42, h * 0.34)
    outline = [(w * 0.08, h * 0.97)] + head + [(w * 0.92, h * 0.97)]
    draw.line(outline + [outline[0]], fill=230, width=10)
    draw.line([(0.5 * w, h * 0.95), (0.5 * w, h * 0.35)], fill=200, width=12)
    for k in range(6):
        y = h * (0.45 + 0.08 * k)
        span = w * (0.12 + 0.03 * k)
        draw.line([(0.5 * w, y), (0.5 * w - span, y - h * 0.08)], fill=190, width=7)
        draw.line([(0.5 * w, y), (0.5 * w + span, y - h * 0.08)], fill=190, width=7)
    height = soften(mask_of(shape), 1.5)
    iron = np.array([0.07, 0.065, 0.06]) * (0.8 + 0.4 * noise(h, w, 8, rng)[..., None])
    _, v = grid(h, w)
    rust = np.clip((v - 0.7) / 0.3, 0, 1) * noise(h, w, 6, rng)
    a = iron * (1 - 0.6 * rust[..., None]) + rust[..., None] * np.array([0.22, 0.09, 0.04])
    a = a * (0.7 + 0.6 * height[..., None])
    return card("fireback", a, height, STONE_PX, 0.02, 0.7)


def flame(name, rng):
    """A stand-in flame for the hearth (13.14 brings the real fire): a tongue of white heat
    going yellow, orange and red to its edge, its alpha the shape, so it is drawn as a card."""
    w, h = int(0.5 * 220), int(0.8 * 220)
    u, v = grid(h, w)
    wob = noise(h, w, 14, rng) - 0.5
    x = (u - 0.5 + 0.25 * wob * (1 - v)) * 2.0
    y = 1.0 - v  # up the flame
    width = np.clip(0.95 * np.sqrt(np.clip(y, 0, 1)) * (1.2 - y), 0, None) + 1e-3
    inside = np.clip(1.0 - np.abs(x) / width, 0, 1) * np.clip((1.0 - y) * 3.0, 0, 1)
    inside *= np.clip(y * 12.0, 0, 1)
    heat = inside ** 0.6
    colour = np.stack([np.clip(0.6 + 1.2 * heat, 0, 1), np.clip(0.1 + 1.3 * heat ** 1.6, 0, 1),
                       np.clip(0.02 + 1.4 * heat ** 4, 0, 1)], axis=-1)
    alpha = np.clip(inside * 2.5, 0, 1)
    return card(name, colour, None, 220, 0.0, 1.0, alpha=alpha)


def main():
    os.makedirs(CACHE_DIR, exist_ok=True)
    rng = np.random.default_rng(1888)
    cards = [panel_linenfold(), panel_tracery(), frieze(), rug(rng)]
    cards += runner(rng)
    cards += [lancet(name, scheme, rng) for name, scheme in GLASS_SCHEMES.items()]
    extra = ["// The spines in each strip: the edges between books, in metres from its left.",
             "// Book i of a strip is between edges i and i + 1."]
    for k in range(SPINE_STRIPS):
        c, edges = spines("spines_%d" % k, rng)
        cards.append(c)
        extra.append("static const float GOTHIC_SPINES_%d_EDGES[] = {%s};" % (
            k, ", ".join("%.4ff" % e for e in edges)))
        extra.append("#define GOTHIC_SPINES_%d_BOOKS %d" % (k, len(edges) - 1))
    extra += ["",
              "// Every strip, for a shelf to pick from.",
              "typedef struct GothicStrip {",
              "    GothicId id;",
              "    const float* edges;",
              "    int books;",
              "} GothicStrip;",
              "#define GOTHIC_STRIP_COUNT %d" % SPINE_STRIPS,
              "static const GothicStrip GOTHIC_STRIPS[GOTHIC_STRIP_COUNT] = {"]
    extra += ["    {GOTHIC_SPINES_%d, GOTHIC_SPINES_%d_EDGES, GOTHIC_SPINES_%d_BOOKS}," % (k, k, k)
              for k in range(SPINE_STRIPS)]
    extra += ["};", ""]
    cards += [coat_of_arms(rng), fireback(rng), flame("flame_a", rng), flame("flame_b", rng)]

    spots = place(cards, ATLAS)
    W, H = ATLAS
    albedo = np.zeros((H, W, 4), dtype=np.float32)
    albedo[..., :3] = 0.2
    albedo[..., 3] = 1.0
    normal = np.zeros((H, W, 3), dtype=np.float32) + np.array([0.5, 0.5, 1.0])
    rough = np.full((H, W), 0.8, dtype=np.float32)
    for c, (x, y) in zip(cards, spots):
        h, w = c["pixels"].shape[:2]
        albedo[y:y + h, x:x + w] = c["pixels"]
        if c["normal"] is not None:
            normal[y:y + h, x:x + w] = c["normal"]
        rough[y:y + h, x:x + w] = c["rough"]
    flip = Image.Transpose.FLIP_TOP_BOTTOM
    out = lambda name: os.path.join(OUT_DIR, name)  # noqa: E731
    Image.fromarray(np.clip(albedo * 255 + 0.5, 0, 255).astype(np.uint8), "RGBA").transpose(
        flip).save(out("gothic_albedo.png"))
    to_image(normal).transpose(flip).save(out("gothic_normal.png"))
    save_rough(rough, out("gothic_rough.png"), ROUGH_SCALE)
    print(out("gothic_albedo.png"))

    glass, glass_n, glass_r = leaded_glass(rng)
    to_image(glass).transpose(flip).save(out("leaded_glass_albedo.png"))
    to_image(glass_n).transpose(flip).save(out("leaded_glass_normal.png"))
    to_image(np.repeat(glass_r[..., None], 3, axis=-1)).transpose(flip).save(
        out("leaded_glass_rough.png"))
    print(out("leaded_glass_albedo.png"))

    write_header(cards, spots, path=HEADER, prefix="GOTHIC", picture="gothic_albedo.png",
                 atlas=ATLAS, extra=extra)
    return 0


if __name__ == "__main__":
    sys.exit(main())
