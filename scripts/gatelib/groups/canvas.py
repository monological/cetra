"""The canvas group: the 2D canvas (areas, ramp, thin lines, four twins, fills, text; spec 14.0)."""

import functools
import math
import os

import numpy as np

from gatelib.arms import Arms
from gatelib.images import _ppm_array, compare
from gatelib.probes import _canvas_probe, _canvas_run, _canvas_shot, _scribbled


def _canvas_sheet(probe, sheet):
    """A probe's lines of one sheet word, by name."""
    return {name: tokens for (word, name), tokens in probe.items() if word == sheet}


def _canvas_cell(tokens):
    """A probe line's cell: its box in framebuffer pixels and its named measures."""
    box = tuple(int(v) for v in tokens[1:5])
    return box, {tokens[k]: float(tokens[k + 1]) for k in range(5, len(tokens) - 1, 2)}


def _rgb_near(pixel, want, codes):
    """Whether a pixel's red, green and blue are each within `codes` of 0xRRGGBB `want`."""
    return all(abs(int(c) - ((want >> sh) & 255)) <= codes for c, sh in zip(pixel[:3], (16, 8, 0)))


def _hex_rgb(code):
    """A probe's RRGGBB as red, green and blue, floats."""
    c = int(code, 16)
    return np.array([(c >> sh) & 255 for sh in (16, 8, 0)], float)


def _off_rgb(pixels, rgb, codes):
    """How many of `pixels` have a channel more than `codes` from `rgb`."""
    return int((np.abs(pixels - rgb).max(axis=-1) > codes).sum())


def _ink_cover(pixels, back, ink):
    """Each pixel's coverage between two colours: how far it lies from `back` toward `ink`."""
    span = ink - back
    return (pixels - back) @ span / (span @ span)


def _canvas_wrong_pixels(path, cells):
    """The probe's pixels ("NAME at X Y want RRGGBB") not within 2 codes of the colour each
    wants, each named with the colour it is."""
    img = _ppm_array(path)
    wrong = []
    for name, tokens in sorted(cells.items()):
        x, y, want = int(tokens[1]), int(tokens[2]), int(tokens[4], 16)
        got = img[y, x]
        if not _rgb_near(got, want, 2):
            wrong.append(f"{name} {int(got[0]):02x}{int(got[1]):02x}{int(got[2]):02x} "
                         f"(want {want:06x})")
    return wrong


def _canvas_area_rows(path, cells, area_of):
    """Each cell's coverage against its closed form, `area_of(name, measures)` its (area, edge)
    in framebuffer pixels: the worst error a pixel of edge, and a row a cell."""
    cov = _canvas_coverage(path)
    rows, worst = [], 0.0
    for name, tokens in sorted(cells.items()):
        (x0, y0, x1, y1), m = _canvas_cell(tokens)
        area, edge = area_of(name, m)
        got = float(cov[y0:y1, x0:x1].sum())
        per_edge = (got - area) / edge
        worst = max(worst, abs(per_edge))
        rows.append(f"{name} {got:.1f}/{area:.1f} ({per_edge:+.4f})")
    return worst, rows


def _canvas_area(name, m):
    """A shape of the areas sheet as (area, perimeter) in framebuffer pixels, from its measures."""
    if name == "circle":
        return math.pi * m["r"] ** 2, 2 * math.pi * m["r"]
    if name == "ring":
        return 2 * math.pi * m["r"] * m["s"], 4 * math.pi * m["r"]
    if name == "rbox":
        return (m["w"] * m["h"] - (4 - math.pi) * m["c"] ** 2,
                2 * (m["w"] + m["h"]) - (8 - 2 * math.pi) * m["c"])
    if name == "capsule":
        return m["len"] * m["w"] + math.pi * (m["w"] / 2) ** 2, 2 * m["len"] + math.pi * m["w"]
    if name == "arc":
        return (m["sweep"] * m["r"] * m["w"] + math.pi * (m["w"] / 2) ** 2,
                2 * m["sweep"] * m["r"] + math.pi * m["w"])
    if name == "turned":
        return m["w"] * m["h"], 2 * (m["w"] + m["h"])
    if name == "outline":
        # Its stroke centred on the edge: the outside offset rounds the corners by half the
        # stroke, the inside stays square.
        hs = m["s"] / 2
        outer = m["w"] * m["h"] + 2 * (m["w"] + m["h"]) * hs + math.pi * hs * hs
        inner = (m["w"] - m["s"]) * (m["h"] - m["s"])
        return (outer - inner,
                2 * (m["w"] + m["h"]) + 2 * math.pi * hs + 2 * (m["w"] + m["h"] - 2 * m["s"]))
    if name == "bar":
        return m["len"] * m["w"], 2 * (m["len"] + m["w"])
    if name == "flat-arc":
        return m["sweep"] * m["r"] * m["w"], 2 * m["sweep"] * m["r"] + 2 * m["w"]
    return None


def run_canvas_gate(workdir):
    """The 2D canvas (spec 14.0): its shapes measured against closed forms, and four twins,
    each a picture that must equal another drawn a different way. Every sheet is white on black
    in apps/canvas, so a pixel's red code over 255 is its coverage, and the app prints where it
    put each shape (--canvas-probe) so nothing here restates its layout. The noise floor is 0 px:
    two runs of one sheet are identical.

      canvas-areas   each shape's coverage summed over its cell against its closed-form area --
                     a circle, a ring, a rounded box, a capsule, a quarter arc, a box turned 45
                     degrees, an outline, a flat-ended bar, and a flat-ended arc of three quarters
                     of a turn given clockwise, whose flat ends' inner ramp once cut away most of
                     its band (a third of its area drawn). The error grows with the edge and
                     not the area, so it is bounded per pixel of perimeter: within 0.03. Sharp
                     corners at 45 degrees sit at 0.02, from the method rather than the shader --
                     every row of such an edge falls at the same phase of the pixel grid, and an
                     exact distance sampled at pixel centres gives the same sum.
      canvas-ramp    a box edge a known fraction f across a pixel: that pixel holds 1 - f to the
                     8-bit store's rounding, the one before it 0 and the one after 1. Coverage is
                     clamp(0.5 - d) with d the distance past the edge, so this is its shape.
      canvas-thin    lines a tenth of a pixel wide, across and down, off the pixel grid: every
                     column across the first and every row across the second sums to one pixel.
                     A line is never drawn narrower than a pixel.
      canvas-translation the areas sheet and the glyphs sheet 2^40 units from the origin each
                     draw the sheet at it, 0 px: every position reaches the shader as an int32
                     offset from a tile and a tile's int32 offset from the view, so nothing is a
                     float until it is a distance from the view.
      canvas-long-segment a line 3.8e9 units long, cut into hundreds of pieces, seen 10 um across
                     in the middle of a piece, against a short copy of it: within 5 codes. A
                     piece ends up to 2^23 units from the view, exact as a float, and scaling it
                     to pixels rounds it by about 0.02 px at this zoom, 80 px a micrometre. The
                     line must be drawn, or two empty frames would agree.
      canvas-mirror  the shapes sheet drawn mirrored against it drawn plain and flipped, to a
                     code: mirroring is exact in the geometry, and the rasterizer interpolates a
                     mirrored triangle's varyings in the other order, which rounds a handful of
                     pixels a code apart (5 at 400x300, none at 960x600).
      canvas-target  the shapes sheet drawn into the picture the CRT and an app's after-tonemap
                     pass draw into, against it drawn into the window, 0 px. The app says which
                     it drew into, or the two runs could be the same path.
      canvas-tiles   the shapes sheet with 65 um tiles, every shape cut into pieces, against it
                     with the default 4 mm tiles, to a code. The pieces overlap at each cut by a
                     pixel, and a layer resolves its coverage by the LARGEST any piece gives a
                     pixel, which is idempotent there: before that resolve it was 2,482 px apart,
                     by up to 20%.
      canvas-triangulation each polygon of the polygons sheet -- holed, a star, an L, a plate
                     with three holes wound either way, a 64-gon, a comb -- triangulates to its
                     own area by the shoelace formula, to a part in a billion, in n - 2 + 2h
                     triangles for n points and h holes: every point a corner and no more.
      canvas-polygon-areas each polygon's coverage against that area, within 0.01 px per pixel
                     of outline, with the default tiles and with 65 um ones, which cut every
                     triangle at the grid. A pixel's inner ramp is measured against the outline
                     edges at its triangle's corners, not its triangle's own: an ear of the 64-gon
                     is under half a pixel thick, and asking only it left most of the inner ramp
                     to the triangle beside it, at 0.047 px of outline over.
      canvas-refused-whole the polygons sheet with tiles of 4096 units, where some polygons'
                     triangles cross more tiles than an edit splits a shape into and are refused:
                     every polygon is drawn whole or not at all. A refused polygon used to keep
                     the triangles added before the refused one and its whole outline.
      canvas-glyph-edge five characters at a 100 px em -- a B, a g, an @, a W and an e -- each
                     summed against stb_truetype's own exact-area rasterizer at the same em:
                     each within 0.6% and the five's mean within 0.3%. stb TRUNCATES each
                     distance-field texel to its byte, so an outline is stored half a code under
                     its edge value on average; cut at the edge value itself, every glyph came
                     out 0.022 atlas pixels thin, a mean of -0.65%. What remains is the field's
                     bilinear reconstruction, which rounds a W's sharp points and fills an @'s
                     inner curls. At a 30 px em the @ is 2.1% over: clamp(0.5 - d) takes the
                     edge through a pixel as straight, and its curls are not.
      canvas-text-width a kerned string's width from canvas_text_width against the font's own
                     advances and pair adjustments, summed by the app in double: within 1e-5.
      canvas-kern-pairs every pair of the atlas's glyphs, font_kern_advance by codepoint and
                     the second of two font_pen_step calls against stb_truetype's own pair
                     adjustment at the font's base size: equal for every pair both ways, the
                     step joined and advancing by the second glyph's width, and some pairs
                     kerned. The font answers from a table it builds at load, since asking stb
                     at every step was three quarters of a text walk, and the pen reads it from
                     the glyphs it holds, which is what the UI's walk and the canvas take.
      canvas-atlas-fields every glyph's texels in the font's atlas, read back from the GPU,
                     against stb_truetype's own distance field for that glyph rasterised by the
                     app with the font's stated parameters: equal to the byte, for every glyph,
                     and the glyph's metrics with them. The fields are rasterised across threads
                     and packed afterwards, so a band handing one glyph's field to another's
                     place is what this sees.
      canvas-font-memory the app's three fonts are built into it (spec 14.6): each one's bytes
                     equal its file's; a distance-field font made from them equals one loaded by
                     path, every glyph's metrics and place, the kerning table and the atlas read
                     back; and an editor face made from them equals one made by path, the same
                     glyphs landing in the same places with the same bitmaps. The font sheet then
                     draws from the built-in JetBrains Mono.
      canvas-overlap-once two capsules crossing in a layer at half opacity: the crossing the same
                     code as one arm, half of white. A layer's coverage is resolved before it is
                     painted, so it is translucent as a whole and not shape by shape.
      canvas-tags    a box a state -- a style's colour, a net's, an object's over its net's, a
                     dim, a hidden one, a raised one winning an overlap, and its edge pixel under
                     half covered by it going to the box beneath (a raised fringe once took the
                     whole pixel), the first added winning one without, an outline's ring and its
                     body -- and an outlined trace
                     of two segments at its joint, where one segment's ring runs inside the
                     other's body: the trace's colour shows there, not the ring's. Rings are
                     painted last, after every shape of the layer has spent its pixels. Each
                     within 2 codes.
      canvas-fade    the fade sheet under a view fading by half toward its white paper (specs
                     14.24, 14.25): the lit box and the untagged one red, a tagged one far from the
                     light and one a hair beside it faded to half white with no room, an untagged
                     blue box on a layer over a faded one blue, and a faded box over a lit green one
                     faded over it, each within 2 codes; and the near one, with 20 pt of room, under
                     that green by 64 codes or more, the faded box over the lit one faded as with no
                     room. Again at a fade of 1, the mask: the faded boxes gone to white, the blue
                     blue and the green showing through; with 20 pt of room as well, the near box
                     coming back under 191 green. A lit H's red ink drawn in each run with room. A
                     shape fades as it is drawn, so what is drawn over it is never faded with it, a
                     light under it never spares it, and the mask hides what it leaves out rather
                     than painting the paper over what lies beneath.
      canvas-uploads the board sheet over eight frames, what each uploaded: a tag written and a
                     style written upload nothing, a segment added one range and removed the same
                     range, and a transient layer of fifty lines, cleared and filled again, one
                     range of 75 slots each frame whatever the board holds. Every step changes
                     canvas_generation, the tag table's writes included.
      canvas-edit-thread the board filled into its edit on a thread of its own, against it filled
                     on the main thread: 0 px. An edit holds no GL and reads nothing the main
                     thread writes.
      canvas-split   the board as two views side by side, the second mirrored and showing only
                     the plane, the bottom copper and the outline, each with its own active
                     layer: each half equals its view drawn alone, 0 px. The views share one
                     target and one tile table, both refilled per view.
      canvas-panes   apps/canvas --editor twice, the editors as panes (spec 14.6), driven by a
                     script: one pane opens on the first file; F8 splits it, the new pane taking
                     the focus and "X" typed into the first file there; F10 shows the second file
                     in it and "Y" is typed there; a click on the first pane and F6 land an edit
                     at the first file's start; a click back and F10 return the second pane to
                     the first file with its caret at 15 -- the 1 it left at, which the editor
                     kept and the text brought through the 14 bytes landed since. Then F9 closes
                     it from its own key: one pane is left, with the focus, and "Z" typed lands in
                     the first file. A pane split and the new one split and closed leaves the
                     inner row's pane in the row's place, the focus on it; and under --vim a pane
                     closed while the line it asked for is open hands the line to the pane left.
                     The probe prints the panes' tree, so a row left in place is seen (end to
                     end). The closes run with freed memory scribbled over, so a pane freed
                     inside the key that closed it crashes the run rather than passing.
      canvas-under-ui the game UI's panel over the board: its centre the panel's colour, and
                     outside it the canvas as it is with no UI, 0 px. The canvas is an overlay
                     under UI_OVERLAY_ORDER.
      canvas-ui-borders a UI border laid over its fill by Porter-Duff over (spec 14.12):
                     four boxes with one border on one opaque backdrop, each compared pixel
                     for pixel. Over a clear fill -- magenta at alpha 0, a colour that must
                     count for nothing -- it draws as over a fill of the backdrop's colour,
                     and over a half-alpha fill as over that fill laid on the backdrop; an
                     opaque border's outermost two pixels draw as a box filled with its
                     colour, which they do only if the edge's coverage counts once; each
                     within 2 codes. The border stands 20 codes or more off the backdrop,
                     so a box drawing nothing cannot pass.
      canvas-ui-seams a UI edge one pixel wide, centred on it (spec 14.23): on an opaque backdrop,
                     a column of quads whole pixels high meeting on pixel boundaries, every pixel
                     of it the quads' colour, and a quad on whole pixels filled whole with nothing
                     of it in the 3 pixels round it, each within 2 codes; a quad whose edge halves
                     a column of pixels, that column half way between the colours within 4; along
                     a rounded corner's diagonal one pixel at most between them; and the column
                     again with its rows meeting part way into pixels, the backdrop a quarter of
                     a join at most, f(1 - f). A two-pixel ramp left a line where two quads met,
                     and a quad drawn only over its own rect had its edge counted twice on a
                     multisampled target, by its samples and by the ramp.
      canvas-ui-lines lines and convex shapes in the UI's draw list under its one-pixel edge
                     (spec 14.28): on an opaque backdrop, a line 2 pixels wide level on a pixel
                     boundary, its two rows its colour and the rows beside them the backdrop's,
                     within 2 codes; one at 30 degrees covering its area, length times width and
                     its ends' disc, within 2%; one half a pixel wide within 5%, which it does
                     because it is drawn a pixel wide at half its alpha, the ramp alone covering
                     a pixel by about half; a polyline turning 60 degrees at each join, the
                     join's pixel and one on its outer side, which only the round ends cover, its
                     colour within 2. A trapezoid whose top and bottom fall just short of a
                     pixel's centre covering its area within 1%, which needs its box drawn a
                     pixel past; a convex square on whole pixels matching a quad of the same rect
                     within 1 code; a square whose edges halve pixels, each corner pixel a
                     quarter covered within 2, the product of its edges' ramps and not their
                     minimum; two triangles meeting on a diagonal, the backdrop a quarter of it
                     at most; a concave shape and one of five corners drawing nothing; and a
                     triangle with a fourth corner placed on an edge in floats, which rounding
                     turns the other way by a few float steps, drawn, its centroid its colour.
      canvas-ui-table a table of 500 rows (spec 14.30) under a header, its cursor on row 3, its
                     colours opaque: the header's rule a pixel row of its colour across the
                     content, within 2 codes; down each gap between two columns, where no cell
                     draws, the cursor's fill on its row and the table's colour under the titles
                     and on every other row, though one column's notes are three times its width.
                     A glyph drawn past its cell lands in a gap.
      canvas-ui-table-pane that table in a scrolling pane beside a drawn panel (spec 14.32),
                     scrolled 200 points: the header held at the pane's top, its rule a pixel row
                     of its colour there within 2 codes; and a strip along the rounded box's foot
                     with only its bottom corners rounded: a pixel in from each top corner its
                     fill, from each bottom corner the backdrop.
      canvas-caps    the strokes sheet (spec 14.22): a segment and an arc each capped round, butt
                     and square, their areas in closed form within 0.03 px per pixel of edge, as
                     canvas-areas. A round cap adds a disc, a square one the width squared. And a
                     wide square-capped segment at 45 degrees alone in its tile, seen through a
                     view whose edge falls between its cap's corner and a half width past its
                     end: a pixel of the corner white. Its tile's bound once grew by the half
                     width alone, and the view culled the tile.
      canvas-joins   two legs at a right angle and at 25 degrees and a box with square corners,
                     each joined round, mitre and bevel, their areas in closed form within 0.03; a
                     pixel just inside the right angle's mitre point white and the same pixel
                     past its bevel's cut black, within 2 codes. The 25-degree mitre's point
                     reaches past four widths, so it is drawn as its bevel, and its area says so.
      canvas-dash-areas the dashes sheet: each cell's coverage against its closed form, within
                     0.03 -- a butt bar ending on a dash's end, the same round, each dash a
                     capsule; two legs at a right angle with the corner in a gap and the far end
                     in a dash, whose pattern begun again at the corner would cover half a dash
                     more; the same with the corner in a dash and mitred, that dash turning it
                     square; a ring, a butt arc, a box and a polygon over the same square from the
                     middle of its bottom side, its corners in gaps; a mitred square polygon from
                     a corner whose pattern is stretched from 9.14 periods to 9; two mitred legs
                     whose dash ends a tenth of a half width past the corner, which turns it; a
                     mitred square-cornered box whose every corner ends a dash, none drawn; and a
                     circle, a box and a polygon filled in a dashed paint, each its solid area. A
                     fill and text never take a dash.
      canvas-dash-places pixels inside the first dash and the first gap of the bar, the arc, a
                     clockwise arc from its own a, the ring from its rightmost point, the box and
                     the polygon from the middle of the bottom side: each white or black within 2
                     codes. Each kind's pattern starts where the contract says. A raised red
                     dashed bar where it crosses a white one, red. And a pixel in
                     the stretched square's mitre at its first point, black: a closed path's
                     last segment comes to that point a hair past a whole period, and deciding
                     there on its own whether it lay in a dash drew its half of a join no dash
                     runs through. A pixel inside the two legs' turn on the first leg's body
                     past the bisector, white: cut at the bisector, the dash ending just past
                     the corner on the second left it drawn by neither. And a pixel in the
                     box's corner square, black: the square squeezed to the corner's one place
                     drew a dash ending there as a block of half coverage.
      canvas-dash-world the bar centred in views 8 mm and 4 mm across: along its middle row,
                     three dashes each within a pixel of 0.2 mm in pixels, 20 and then 40. A
                     dash is a length on the board, not on the screen.
      canvas-dash-twins the dashes sheet 2^40 units from the origin against it at the origin,
                     0 px; mirrored against it flipped, and with 65 um tiles against the default
                     ones, each within a code -- every piece cut at the grid carries its place
                     along the shape across the cut; and the string in a dashed paint against
                     the same one plain a whole number of pixels along, 0 px.
      canvas-dash-solid the dashes sheet under --dash-min-px 30: in the 8 mm view, where a dash
                     is 20 px, the bar is one run of half white within 2 codes along its middle
                     row, the pattern's average strength with no dash showing; in the 4 mm view,
                     where it is 40 px, three dashes. The threshold is in pixels and is the
                     setting's doing. And the raised red bar a quarter dash, drawn at its
                     average, still red where it crosses the white bar: the average is an
                     opacity, so its pixels keep the rank their coverage gives. Taken off the
                     coverage, under a half it ranked as a fringe and the white bar's colour won.
      canvas-dash-tags a dashed bar in an object tagged red, its first dash red within 2 codes;
                     and one whose object was removed in the same edit, its cell black.
      canvas-dash-refused a segment whose dash gap is negative and an arc whose dash is 2^30
                     units: each refused by name in the log, and their cell black.
      canvas-budget  both views of the board split at 1920x1080 points, a framebuffer of
                     3840x2160 on this machine, each frame's canvas draws timed between two
                     glFinish calls: the median under 6 ms. A timing arm: the bound is this
                     machine's (2.55 ms measured on an M1 Max) and does not travel.
    """
    arm = Arms(19)

    # The runs more than one arm reads, each made once, by the first chosen arm that asks for it.
    @functools.cache
    def sheet_run(sheet):
        return _canvas_run(workdir, sheet, "--sheet", sheet)

    @functools.cache
    def board_run():
        # The board with no UI over it, which a UI over it must leave as it is.
        return _canvas_run(workdir, "board_plain", "--sheet", "board")

    @functools.cache
    def dashes_run():
        # Its log too: the refused dash is named there.
        dashes = os.path.join(workdir, "canvas_dashes.ppm")
        said = _canvas_shot(dashes, 2, "--canvas-probe", "--sheet", "dashes", log=True)
        dashes, probe = (dashes, _canvas_probe(said)) if said is not None else (None, {})
        return dashes, probe, said

    # ---- areas
    @arm("canvas-areas")
    def _():
        areas, probe = sheet_run("areas")
        if not areas:
            ok, detail = False, "the app failed"
        else:
            worst, rows = _canvas_area_rows(areas, _canvas_sheet(probe, "areas"), _canvas_area)
            ok = len(rows) == 9 and worst <= 0.03
            detail = (f"{len(rows)} shapes (want 9), worst {worst:.4f} px of edge (want <= 0.03): "
                      + ", ".join(rows))
        return ok, detail

    # ---- ramp
    @arm("canvas-ramp")
    def _():
        path, probe = _canvas_run(workdir, "ramp", "--sheet", "ramp")
        if not path:
            ok, detail = False, "the app failed"
        else:
            cov = _canvas_coverage(path)
            rows, ok = [], True
            for (sheet, name), tokens in sorted(probe.items()):
                if sheet != "ramp":
                    continue
                col, y0, y1, want = int(tokens[1]), int(tokens[3]), int(tokens[4]), float(tokens[6])
                before, at, after = cov[y0:y1, col - 1], cov[y0:y1, col], cov[y0:y1, col + 1]
                good = (before.max() == 0.0 and after.min() == 1.0
                        and np.abs(at * 255.0 - want * 255.0).max() <= 1.0)
                ok = ok and good
                rows.append(f"{name} {at.min():.3f}..{at.max():.3f} (want {want:.3f})")
            ok = ok and len(rows) == 5
            detail = f"{len(rows)} edges (want 5), the pixel each crosses: " + ", ".join(rows)
        return ok, detail

    # ---- thin
    @arm("canvas-thin")
    def _():
        path, probe = _canvas_run(workdir, "thin", "--sheet", "thin")
        if not path or ("thin", "across") not in probe or ("thin", "down") not in probe:
            ok, detail = False, "the app failed or printed no lines"
        else:
            cov = _canvas_coverage(path)
            t = probe[("thin", "across")]
            across = cov[int(t[4]):int(t[5]), int(t[1]):int(t[2])].sum(axis=0)
            t = probe[("thin", "down")]
            down = cov[int(t[1]):int(t[2]), int(t[4]):int(t[5])].sum(axis=1)
            ok = (np.abs(across - 1.0).max() <= 2.0 / 255.0
                  and np.abs(down - 1.0).max() <= 2.0 / 255.0)
            detail = (f"columns across {across.min():.4f}..{across.max():.4f}, rows down "
                      f"{down.min():.4f}..{down.max():.4f} (want 1 within 2/255)")
        return ok, detail

    # ---- translation
    @arm("canvas-translation")
    def _():
        areas, _ = sheet_run("areas")
        far, _ = _canvas_run(workdir, "areas_far", "--sheet", "areas", "--offset", str(1 << 40))
        near_text, _ = sheet_run("glyphs")
        far_text, _ = _canvas_run(workdir, "glyphs_far", "--sheet", "glyphs", "--offset",
                                  str(1 << 40))
        if not areas or not far or not near_text or not far_text:
            ok, detail = False, "a run failed"
        else:
            ae, pae = compare(areas, far)
            te, tpae = compare(near_text, far_text)
            drawn = float(_canvas_coverage(areas).sum())
            text_drawn = float(_canvas_coverage(near_text).sum())
            ok = ae == 0 and te == 0 and drawn > 1000.0 and text_drawn > 1000.0
            detail = (f"shapes {ae} px differ, text {te} px (want 0 and 0), {drawn:.0f} and "
                      f"{text_drawn:.0f} px^2 drawn")
        return ok, detail

    # ---- long segment
    @arm("canvas-long-segment")
    def _():
        long_, _ = _canvas_run(workdir, "long", "--sheet", "long")
        short, _ = _canvas_run(workdir, "long_short", "--sheet", "long-short")
        if not long_ or not short:
            ok, detail = False, "a run failed"
        else:
            ae, pae = compare(long_, short)
            drawn = float(_canvas_coverage(long_).mean())
            ok = pae <= 5.0 / 255.0 and drawn > 0.1
            detail = (f"{ae} px differ, peak {pae * 255:.0f} codes (want <= 5), the line covers "
                      f"{drawn:.1%} of the frame")
        return ok, detail

    # ---- mirror
    @arm("canvas-mirror")
    def _():
        plain, _ = sheet_run("shapes")
        mirrored, _ = _canvas_run(workdir, "shapes_mirror", "--sheet", "shapes", "--mirror")
        if not plain or not mirrored:
            ok, detail = False, "a run failed"
        else:
            a, b = _ppm_array(plain), _ppm_array(mirrored)
            differ = int(np.any(a[:, ::-1] != b, axis=2).sum())
            peak = float(np.abs(a[:, ::-1] - b).max())
            asymmetric = int(np.any(a[:, ::-1] != a, axis=2).sum())
            ok = peak <= 1.0 and asymmetric > 0
            detail = (f"{differ} px differ from the plain sheet flipped, peak {peak:.0f} code(s) "
                      f"(want <= 1); the sheet is not its own mirror image ({asymmetric} px)")
        return ok, detail

    # ---- target
    @arm("canvas-target")
    def _():
        plain, _ = sheet_run("shapes")
        picture, probe = _canvas_run(workdir, "shapes_picture", "--sheet", "shapes", "--picture")
        if not plain or not picture:
            ok, detail = False, "a run failed"
        else:
            target = (probe.get(("frame", "target")) or ["?"])[0]
            ae, pae = compare(plain, picture)
            ok = ae == 0 and target == "picture"
            detail = (f"{ae} px differ, peak {pae:.4f} (want 0), drawn into the {target} (want "
                      f"picture)")
        return ok, detail

    # ---- tiles
    @arm("canvas-tiles")
    def _():
        plain, _ = sheet_run("shapes")
        tiny, _ = _canvas_run(workdir, "shapes_tiny", "--sheet", "shapes", "--tile-shift", "16")
        if not plain or not tiny:
            ok, detail = False, "a run failed"
        else:
            ae, pae = compare(plain, tiny)
            ok = round(pae * 255) <= 1
            detail = f"{ae} px differ, peak {pae * 255:.0f} code(s) (want <= 1)"
        return ok, detail

    # ---- polygons
    @arm("canvas-triangulation")
    def _():
        polygons, probe = sheet_run("polygons")
        cells = _canvas_sheet(probe, "polygons")
        if not polygons or len(cells) != 6:
            ok, detail = False, f"the app failed or printed {len(cells)} polygons (want 6)"
        else:
            rows, ok = [], True
            for name, tokens in sorted(cells.items()):
                _, m = _canvas_cell(tokens)
                want_count = m["points"] - 2 + 2 * m["holes"]
                good = (abs(m["triangulated"] - m["area"]) <= 1e-9 * m["area"]
                        and m["triangles"] == want_count)
                ok = ok and good
                rows.append(f"{name} {m['triangles']:.0f}/{want_count:.0f}")
            detail = "areas agree, triangles (want n - 2 + 2h): " + ", ".join(rows)
        return ok, detail

    @arm("canvas-polygon-areas")
    def _():
        polygons, probe = sheet_run("polygons")
        cells = _canvas_sheet(probe, "polygons")
        tiny, _ = _canvas_run(workdir, "polygons_tiny", "--sheet", "polygons", "--tile-shift",
                              "16")
        if not polygons or not tiny or len(cells) != 6:
            ok, detail = False, "a run failed"
        else:
            worst, rows = 0.0, []
            for label, path in (("4 mm", polygons), ("65 um", tiny)):
                cov = _canvas_coverage(path)
                for name, tokens in sorted(cells.items()):
                    (x0, y0, x1, y1), m = _canvas_cell(tokens)
                    per_edge = (float(cov[y0:y1, x0:x1].sum()) - m["area"]) / m["perimeter"]
                    worst = max(worst, abs(per_edge))
                    if label == "65 um":
                        rows.append(f"{name} {per_edge:+.4f}")
            ok = worst <= 0.01
            detail = (f"worst {worst:.4f} px of outline (want <= 0.01) over both tile sizes; "
                      f"with 65 um tiles: " + ", ".join(rows))
        return ok, detail

    @arm("canvas-refused-whole")
    def _():
        whole, probe = _canvas_run(workdir, "polygons_refused", "--sheet", "polygons",
                                   "--tile-shift", "12")
        cells = _canvas_sheet(probe, "polygons")
        if not whole or len(cells) != 6:
            ok, detail = False, f"the app failed or printed {len(cells)} polygons (want 6)"
        else:
            cov = _canvas_coverage(whole)
            drawn, refused, partial = [], [], []
            for name, tokens in sorted(cells.items()):
                (x0, y0, x1, y1), m = _canvas_cell(tokens)
                got = float(cov[y0:y1, x0:x1].sum())
                if got == 0.0:
                    refused.append(name)
                elif abs(got - m["area"]) <= 0.01 * m["perimeter"]:
                    drawn.append(name)
                else:
                    partial.append(f"{name} {got:.0f}/{m['area']:.0f}")
            ok = not partial and drawn and refused
            detail = (f"drawn whole: {', '.join(drawn) or 'none'}; refused whole: "
                      f"{', '.join(refused) or 'none'}; part drawn: {', '.join(partial) or 'none'} "
                      f"(want none, and at least one of each other)")
        return ok, detail

    # ---- text
    @arm("canvas-glyph-edge")
    def _():
        glyphs, probe = sheet_run("glyphs")
        cells = {name: tokens for (sheet, name), tokens in probe.items()
                 if sheet == "glyphs" and tokens[0] == "box"}
        if not glyphs or len(cells) != 5:
            ok, detail = False, f"the app failed or printed {len(cells)} glyphs (want 5)"
        else:
            cov = _canvas_coverage(glyphs)
            errors, rows = [], []
            for name, tokens in sorted(cells.items()):
                (x0, y0, x1, y1), m = _canvas_cell(tokens)
                stb = m["stb"]
                rel = (float(cov[y0:y1, x0:x1].sum()) - stb) / stb
                errors.append(rel)
                rows.append(f"{name} {rel:+.2%}")
            mean = sum(errors) / len(errors)
            ok = max(abs(e) for e in errors) <= 0.006 and abs(mean) <= 0.003
            detail = (f"at a {_canvas_cell(cells['B'])[1]['em']:.0f} px em, each within 0.6% "
                      f"(worst {max(abs(e) for e in errors):.2%}), mean {mean:+.2%} "
                      f"(want within 0.3%): " + ", ".join(rows))
        return ok, detail

    @arm("canvas-text-width")
    def _():
        _, probe = sheet_run("glyphs")
        width = probe.get(("glyphs", "width"))
        if not width:
            ok, detail = False, "the app printed no width"
        else:
            got, want = int(width[0]), float(width[2])
            ok = want > 0 and abs(got - want) <= 1e-5 * want
            detail = f"{got} units against the font's {want:.1f} (want within 1e-5)"
        return ok, detail

    @arm("canvas-kern-pairs")
    def _():
        _, probe = sheet_run("glyphs")
        pairs = probe.get(("glyphs", "pairs"))
        if not pairs:
            ok, detail = False, "the app printed no pairs"
        else:
            count, kerned, differ = int(pairs[0]), int(pairs[2]), int(pairs[4])
            by_pen = int(pairs[6]) if len(pairs) > 6 else -1
            ok = count > 0 and kerned > 0 and differ == 0 and by_pen == 0
            detail = (f"{count} pairs, {kerned} of them kerned; differing from stb_truetype's own, "
                      f"{differ} asked by codepoint and {by_pen} stepped by the pen (want some "
                      f"kerned and none differing)")
        return ok, detail

    @arm("canvas-atlas-fields")
    def _():
        _, probe = sheet_run("glyphs")
        atlas = probe.get(("glyphs", "atlas"))
        if not atlas:
            ok, detail = False, "the app printed no atlas"
        else:
            count, differ = int(atlas[0]), int(atlas[2])
            ok = count > 200 and differ == 0
            detail = (f"{count} glyphs, {differ} whose texels differ from stb_truetype's own field "
                      f"(want over 200 and none)")
        return ok, detail

    # ---- the fonts built into the program (spec 14.6)
    @arm("canvas-font-memory")
    def _():
        sheet, probe = _canvas_run(workdir, "font_memory", "--font-sheet")
        # "canvas fonts bytes B fonts F faces C of N": each count read by its label.
        line = ["bytes"] + probe.get(("fonts", "bytes"), [])
        counts = {line[k]: int(line[k + 1]) for k in range(0, len(line) - 1, 2)}
        if not sheet or set(counts) != {"bytes", "fonts", "faces", "of"}:
            ok, detail = False, "the app failed or printed no fonts line"
        else:
            count = counts["of"]
            ink = int((_ppm_array(sheet)[..., :3].max(axis=2) > 150).sum())
            ok = (count > 0 and counts["bytes"] == counts["fonts"] == counts["faces"] == count
                  and ink > 2000)
            detail = (f"of {count} built-in fonts, {counts['bytes']} with their file's bytes, "
                      f"{counts['fonts']} making the font the file makes and {counts['faces']} the "
                      f"face (want all); {ink} px of the sheet's text (want over 2000)")
        return ok, detail

    # ---- layers and tags
    @arm("canvas-overlap-once")
    def _():
        path, probe = _canvas_run(workdir, "overlap", "--sheet", "overlap")
        both, one = probe.get(("overlap", "both")), probe.get(("overlap", "one"))
        if not path or not both or not one:
            ok, detail = False, "the app failed or printed no points"
        else:
            img = _ppm_array(path)
            a = int(img[int(both[2]), int(both[1]), 0])
            b = int(img[int(one[2]), int(one[1]), 0])
            ok = a == b and abs(a - 127.5) <= 1.0
            detail = f"the crossing {a}, one arm {b} (want equal, and half of white)"
        return ok, detail

    @arm("canvas-tags")
    def _():
        path, probe = _canvas_run(workdir, "tags", "--sheet", "tags")
        cells = _canvas_sheet(probe, "tags")
        if not path or len(cells) != 11:
            ok, detail = False, f"the app failed or printed {len(cells)} points (want 11)"
        else:
            wrong = _canvas_wrong_pixels(path, cells)
            ok = not wrong
            detail = ("11 points each its state's colour within 2 codes" if ok
                      else "wrong: " + ", ".join(wrong))
        return ok, detail

    @arm("canvas-fade")
    def _():
        hard, probe = _canvas_run(workdir, "fade", "--sheet", "fade", "--fade", "0.5,0")
        roomy, _ = _canvas_run(workdir, "fade_room", "--sheet", "fade", "--fade", "0.5,20")
        masked, mask_probe = _canvas_run(workdir, "fade_mask", "--sheet", "fade", "--fade", "1,0")
        mask_roomy, _ = _canvas_run(workdir, "fade_mask_room", "--sheet", "fade", "--fade", "1,20")
        cells = _canvas_sheet(probe, "fade")
        mask_cells = _canvas_sheet(mask_probe, "fade")
        label = probe.get(("fade-text", "label"))
        if (not hard or not roomy or not masked or not mask_roomy or len(cells) != 6
                or len(mask_cells) != 6 or not label):
            ok, detail = False, (f"the app failed or printed {len(cells)} and {len(mask_cells)} "
                                 f"points (want 6 each, and the label)")
        else:
            # With room, the overlap is as without it, the light under a faded box sparing nothing;
            # under the mask with room, the near box comes back within the room.
            def want(cell_set, name, hexcode):
                return {name: cell_set[name][:4] + [hexcode]}
            wrong = (_canvas_wrong_pixels(hard, cells)
                     + [f"masked {w}" for w in _canvas_wrong_pixels(masked, mask_cells)]
                     + [f"room {w}" for w in _canvas_wrong_pixels(roomy, want(cells, "under",
                                                                              cells["under"][4]))])
            mask_room_wanted = {**want(mask_cells, "under", "00ff00"),
                                **want(mask_cells, "over", "0000ff"),
                                **want(mask_cells, "far", "ffffff")}
            wrong += [f"masked with room {w}"
                      for w in _canvas_wrong_pixels(mask_roomy, mask_room_wanted)]
            near = cells["near"]
            pale = int(near[4], 16) >> 8 & 255
            roomy_near = int(_ppm_array(roomy)[int(near[2]), int(near[1])][1])
            mask_near = int(_ppm_array(mask_roomy)[int(near[2]), int(near[1])][1])

            # The lit H's red ink round its anchor, which a fading view with room must still draw.
            def ink(path):
                x, y = int(label[1]), int(label[2])
                img = _ppm_array(path)[max(y - 15, 0):y + 15, max(x - 15, 0):x + 15].astype(int)
                return int(((img[..., 0] > 200) & (img[..., 1] < 80) & (img[..., 2] < 80)).sum())
            inks = [ink(hard), ink(roomy), ink(mask_roomy)]
            ok = (not wrong and roomy_near < pale - 64 and mask_near < 255 - 64
                  and min(inks) > 20)
            detail = (f"lit and untagged red, far and near faded to {pale} green with no room, "
                      f"blue over a faded box, a faded box over a lit one, with room too; masked, "
                      f"the faded gone and the lit showing, with room too; near {roomy_near} with "
                      f"20 pt (want under {pale - 64}), {mask_near} masked (want under "
                      f"{255 - 64}); the lit H's ink {inks} px (want over 20 each)"
                      + ("" if not wrong else "; wrong: " + ", ".join(wrong)))
        return ok, detail

    @arm("canvas-uploads")
    def _():
        board, probe = _canvas_run(workdir, "board", "--sheet", "board", "-f", "8")
        steps = _canvas_sheet(probe, "board")
        if not board or len(steps) != 7:
            ok, detail = False, f"the app failed or printed {len(steps)} steps (want 7)"
        else:
            s = {name: (int(t[1]), int(t[3]), int(t[5])) for name, t in steps.items()}
            transient = 75 * 48  # fifty records and their slack, a range of 1.5 times
            ok = (s["tag"][:2] == (0, 0) and s["style"][:2] == (0, 0)
                  and s["add"][0] == 1 and s["remove"] == (1, s["add"][1], s["remove"][2])
                  and s["transient"][:2] == (1, transient)
                  and s["transient-again"][:2] == (1, transient)
                  and all(v[2] > 0 for v in s.values()) and s["first"][1] > 100 * transient)
            detail = ", ".join(f"{name} {v[0]} range(s) {v[1]} B" for name, v in s.items()) + (
                "; every step changed the generation" if all(v[2] > 0 for v in s.values())
                else "; a step left the generation as it was")
        return ok, detail

    @arm("canvas-edit-thread")
    def _():
        threaded, _ = _canvas_run(workdir, "board_thread", "--sheet", "board", "--edit-thread")
        plain, _ = board_run()
        if not threaded or not plain:
            ok, detail = False, "a run failed"
        else:
            ae, pae = compare(plain, threaded)
            ok = ae == 0
            detail = f"{ae} px differ, peak {pae:.4f} (want 0)"
        return ok, detail

    # ---- views
    @arm("canvas-split")
    def _():
        split, _ = _canvas_run(workdir, "split", "--sheet", "board", "--split")
        left, _ = _canvas_run(workdir, "split_left", "--sheet", "board", "--split", "--only-view",
                              "0")
        right, _ = _canvas_run(workdir, "split_right", "--sheet", "board", "--split", "--only-view",
                               "1")
        if not split or not left or not right:
            ok, detail = False, "a run failed"
        else:
            s, a, b = _ppm_array(split), _ppm_array(left), _ppm_array(right)
            half = s.shape[1] // 2
            left_differ = int(np.any(s[:, :half] != a[:, :half], axis=2).sum())
            right_differ = int(np.any(s[:, half:] != b[:, half:], axis=2).sum())
            # The two halves are different pictures: one mirrored, showing fewer layers.
            unlike = int(np.any(s[:, :half] != s[:, half:2 * half], axis=2).sum())
            ok = left_differ == 0 and right_differ == 0 and unlike > 1000
            detail = (f"left half {left_differ} px from the left view alone, right half "
                      f"{right_differ} px from the right alone (want 0 and 0); the halves differ "
                      f"by {unlike} px")
        return ok, detail

    # ---- the editor as panes (spec 14.6)
    @arm("canvas-panes")
    def _():
        pane_a, pane_b = os.path.join(workdir, "pane_a.ckt"), os.path.join(workdir, "pane_b.ckt")
        with open(pane_a, "w", encoding="utf-8", newline="") as f:
            f.write("(a one)\n(a two)\n")
        with open(pane_b, "w", encoding="utf-8", newline="") as f:
            f.write("(b one)\n")
        files = ("--editor", pane_a, "--editor", pane_b)
        opened, first = _canvas_run(workdir, "panes_open", *files)
        if not opened:
            ok, detail = False, "the app failed on two files"
        else:
            # A click on each pane's middle, the two side by side, in framebuffer pixels.
            fb_h, fb_w = _ppm_array(opened).shape[:2]
            left, right, mid = fb_w // 4, 3 * fb_w // 4, fb_h // 2
            steps = (f"2 key=f8\n4 type=X\n6 key=f10\n8 type=Y\n10 at={left},{mid}\n"
                     f"11 at={left},{mid} down\n12 at={left},{mid}\n14 key=f6\n"
                     f"16 at={right},{mid}\n17 at={right},{mid} down\n18 at={right},{mid}\n"
                     f"20 key=f10\n")
            split_script = os.path.join(workdir, "panes_split.script")
            close_script = os.path.join(workdir, "panes_close.script")
            with open(split_script, "w", encoding="utf-8") as f:
                f.write(steps)
            with open(close_script, "w", encoding="utf-8") as f:
                f.write(steps + "22 key=f9\n24 type=Z\n")
            _, two = _canvas_run(workdir, "panes_split", *files, "--input-script", split_script,
                                 frames=24)
            # Closing a pane from its own key frees it as the pass ends; freed memory scribbled
            # over, one freed sooner crashes the run.
            _, one = _canvas_run(workdir, "panes_close", *files, "--input-script", close_script,
                                 frames=28, env=_scribbled())
            # A pane split, the new one split again, and that one closed: the inner row hands its
            # pane up, the focus on the pane beside the one closed.
            nest_script = os.path.join(workdir, "panes_nest.script")
            with open(nest_script, "w", encoding="utf-8") as f:
                f.write("2 key=f8\n4 key=f8\n6 key=f9\n")
            _, nest = _canvas_run(workdir, "panes_nest", *files, "--input-script", nest_script,
                                  frames=8, env=_scribbled())
            # Under --vim, the line asked for by the second pane, which is then closed: the line
            # answers the pane left.
            vim_script = os.path.join(workdir, "panes_vim.script")
            with open(vim_script, "w", encoding="utf-8") as f:
                f.write(f"2 key=f8\n4 type=/\n6 at={right},{mid}\n7 at={right},{mid} down\n"
                        f"8 at={right},{mid}\n10 key=f9\n")
            _, vim = _canvas_run(workdir, "panes_vim", *files, "--vim", "--input-script",
                                 vim_script, frames=12, env=_scribbled())
            landed = b"(note landed)\nX(a one)\n(a two)\n"
            want = {
                "opened": (first.get(("pane", "0"), [])[:6]
                           == ["file", "0", "top", "0", "focused", "1"]
                           and first.get(("panes", "tree")) == ["(0)"]
                           and first.get(("file", "1")) == [b"(b one)\n".hex()]),
                "split": (two.get(("pane", "0"), [])[:6]
                          == ["file", "0", "top", "0", "focused", "0"]
                          and two.get(("pane", "1")) == ["file", "0", "top", "0", "focused", "1",
                                                         "sel", "1", "15", "15"]
                          and two.get(("panes", "tree")) == ["((0,1))"]
                          and two.get(("file", "0")) == [landed.hex()]
                          and two.get(("file", "1")) == [b"Y(b one)\n".hex()]),
                "closed": (one.get(("pane", "0"), [])[:6]
                           == ["file", "0", "top", "0", "focused", "1"]
                           and one.get(("panes", "tree")) == ["(0)"]
                           and one.get(("file", "0")) == [(landed + b"Z").hex()]),
                "nested": (nest.get(("panes", "tree")) == ["((0,1))"]
                           and nest.get(("pane", "1"), [])[4:6] == ["focused", "1"]),
                "vim": (vim.get(("panes", "tree")) == ["(0)"]
                        and vim.get(("panes", "asking")) == ["0"]),
            }
            ok = all(want.values())
            detail = (f"{want} -- second pane after the round trip "
                      f"{' '.join(two.get(('pane', '1'), ['none']))} (want its caret at 15)")
        return ok, detail

    @arm("canvas-under-ui")
    def _():
        plain, _ = board_run()
        under, probe = _canvas_run(workdir, "board_ui", "--sheet", "board", "--ui")
        panel = probe.get(("ui", "panel"))
        if not under or not panel or not plain:
            ok, detail = False, "a run failed or printed no panel"
        else:
            x0, y0, x1, y1 = (int(v) for v in panel[:4])
            want = int(panel[5], 16)
            u, p = _ppm_array(under), _ppm_array(plain)
            centre = [int(v) for v in u[(y0 + y1) // 2, (x0 + x1) // 2, :3]]
            good = _rgb_near(centre, want, 2)
            # Outside the panel, grown by a few pixels for its edge, the canvas is
            # as it is with no UI at all.
            mask = np.ones(u.shape[:2], bool)
            mask[max(y0 - 4, 0):y1 + 4, max(x0 - 4, 0):x1 + 4] = False
            outside = int((np.any(u != p, axis=2) & mask).sum())
            ok = good and outside == 0
            detail = (f"the panel's centre {centre[0]:02x}{centre[1]:02x}{centre[2]:02x} (want "
                      f"{want:06x}), {outside} px differ outside it (want 0)")
        return ok, detail

    @arm("canvas-ui-borders")
    def _():
        shot, probe = _canvas_run(workdir, "borders", "--ui-borders")
        names = ("opaque", "clear", "half", "mixed", "edge", "solid")
        boxes = {n: [int(v) for v in probe.get(("border", n), [])[:4]] for n in names}
        if not shot or not all(len(b) == 4 for b in boxes.values()):
            ok, detail = False, "a run failed or printed no boxes"
        else:
            img = _ppm_array(shot).astype(int)

            def box(n):
                x0, y0, x1, y1 = boxes[n]
                return img[y0:y1, x0:x1, :3]

            shapes = {box(n).shape for n in names}
            if len(shapes) != 1:
                ok, detail = False, f"the boxes differ in size: {shapes}"
            else:
                h, w, _ = shapes.pop()
                # The opaque box's fill is the backdrop's colour, so its centre is the backdrop.
                backdrop = box("opaque")[h // 2, w // 2]
                # Porter-Duff over: a border over a clear fill draws as one over the backdrop's own
                # colour, and one over a half-alpha fill as one over that fill laid on the backdrop.
                clear = int(np.abs(box("clear") - box("opaque")).max())
                half = int(np.abs(box("half") - box("mixed")).max())
                ring = int(np.abs(box("opaque") - backdrop).max())
                # The outermost two pixels lie wholly in an opaque border's band: drawn there, it
                # matches a box filled with its colour only if the edge's coverage counts once.
                frame = np.zeros((h, w), bool)
                frame[:2, :] = frame[-2:, :] = frame[:, :2] = frame[:, -2:] = True
                edge = int(np.abs(box("edge") - box("solid"))[frame].max())
                ok = clear <= 2 and half <= 2 and edge <= 2 and ring >= 20
                detail = (f"over a clear fill against over the backdrop's colour {clear} codes "
                          f"apart, over a half-alpha fill against over that fill on the backdrop "
                          f"{half}, an opaque border's outer edge against a filled box's {edge} "
                          f"(want 2 at most); the border {ring} codes off the backdrop (want 20 or "
                          f"more)")
        return ok, detail

    @arm("canvas-ui-seams")
    def _():
        shot, probe = _canvas_run(workdir, "seams", "--ui-seams")
        seams = _canvas_sheet(probe, "seams")
        names = ("stack", "alone", "half", "round", "offgrid")
        colours = seams.get("colours", [])
        if not shot or len(colours) != 4 or not all(len(seams.get(n, [])) == 4 for n in names):
            ok, detail = False, "the run failed or printed no boxes"
        else:
            img = _ppm_array(shot)
            boxes = {n: [int(v) for v in seams[n]] for n in names}
            box_rgb, back_rgb = _hex_rgb(colours[1]), _hex_rgb(colours[3])

            def inside(n):
                x0, y0, x1, y1 = boxes[n]
                return img[y0:y1, x0:x1]

            # The stack's quads meet on pixel boundaries: nothing of the backdrop between them.
            stack = _off_rgb(inside("stack"), box_rgb, 2)
            # A quad on whole pixels: filled whole, and nothing of it in the 3 pixels round it.
            x0, y0, x1, y1 = boxes["alone"]
            ring = np.ones((y1 - y0 + 6, x1 - x0 + 6), bool)
            ring[3:-3, 3:-3] = False
            around = img[y0 - 3:y1 + 3, x0 - 3:x1 + 3][ring]
            alone = _off_rgb(inside("alone"), box_rgb, 2) + _off_rgb(around, back_rgb, 2)
            # The half quad's right edge through the middle of its last column: that column half
            # way.
            x0, y0, x1, y1 = boxes["half"]
            half = _off_rgb(img[y0:y1, x1 - 1], np.floor((box_rgb + back_rgb) / 2), 4)
            # Along a rounded corner's diagonal the edge crosses d by 1.4 pixels a step, so a
            # one-pixel ramp leaves one pixel between the colours; a two-pixel ramp leaves two.
            x0, y0, x1, y1 = boxes["round"]
            diagonal = np.array([img[y0 + k, x0 + k] for k in range(min(x1 - x0, y1 - y0) // 2)])
            between = int(((np.abs(diagonal - box_rgb).max(axis=-1) > 2)
                           & (np.abs(diagonal - back_rgb).max(axis=-1) > 2)).sum())
            # Rows meeting part way into a pixel each cover it in part, f and 1 - f, and the
            # backdrop shows by f(1 - f), a quarter at most, when each edge's coverage counts once.
            # Counted by a multisampled target's samples as well, half of it showed. The share of
            # the backdrop in every pixel above the column's last, which is its own edge.
            x0, y0, x1, y1 = boxes["offgrid"]
            share = float((1 - _ink_cover(img[y0:y1 - 1, x0:x1], back_rgb, box_rgb)).max())
            ok = (stack == 0 and alone == 0 and half == 0 and between <= 1
                  and share <= 0.25 + 2 / np.abs(box_rgb - back_rgb).max())
            detail = (f"{stack} px of the stack not its colour, {alone} px of the quad on the grid "
                      f"or round it wrong (want 0 each), {half} px of the halved column not half "
                      f"way (want 0), {between} px between the colours on a corner's diagonal "
                      f"(want 1 at most), the backdrop {share:.3f} of an off-grid join (want 0.25 "
                      f"at most)")
        return ok, detail

    @arm("canvas-ui-lines")
    def _():
        shot, probe = _canvas_run(workdir, "lines", "--ui-lines")
        lines = _canvas_sheet(probe, "lines")
        names = ("level", "slant", "hair", "joined", "trapezoid", "square", "quad", "halves",
                 "meeting", "concave", "five", "straight")
        colours = lines.get("colours", [])
        if not shot or len(colours) != 4 or not all(n in lines for n in names):
            ok, detail = False, "the run failed or printed no lines"
        else:
            img = _ppm_array(shot)[..., :3].astype(float)
            ink, back = _hex_rgb(colours[1]), _hex_rgb(colours[3])
            span = ink - back

            def shape(n):
                # Its width, then its points, in pixels.
                v = [float(t) for t in lines[n]]
                return v[0], np.array(v[1:]).reshape(-1, 2)

            def box(n, grow):
                # The shape's bounds, grown by `grow` pixels and its half width, as array bounds.
                w, p = shape(n)
                x0, y0 = np.floor(p.min(axis=0) - w / 2 - grow).astype(int)
                x1, y1 = np.ceil(p.max(axis=0) + w / 2 + grow).astype(int)
                return x0, y0, x1, y1

            def area_error(n):
                # Its coverage summed round the shape, against its area: a line's a capsule's, a
                # convex shape's by the shoelace.
                w, p = shape(n)
                x0, y0, x1, y1 = box(n, 3)
                got = float(_ink_cover(img[y0:y1, x0:x1], back, ink).sum())
                if w > 0:
                    want = _canvas_area("capsule", {"len": float(np.linalg.norm(p[1] - p[0])),
                                                    "w": w})[0]
                else:
                    q = np.roll(p, -1, axis=0)
                    want = abs(float((p[:, 0] * q[:, 1] - q[:, 0] * p[:, 1]).sum())) / 2
                return abs(got - want) / want

            # Level on a pixel boundary: its rows between the ends its colour, the rows either side
            # the backdrop's, round ends and all.
            w, p = shape("level")
            ax, bx, y, r = int(p[0][0]), int(p[1][0]), int(p[0][1]), int(w // 2)
            level = (_off_rgb(img[y - r:y + r, ax:bx], ink, 2)
                     + _off_rgb(img[[y - r - 1, y + r], ax - r - 2:bx + r + 2], back, 2))
            slant, hair = area_error("slant"), area_error("hair")
            # At each join, its pixel and the pixel half way out along the bisector on the outer
            # side, which only the two round ends cover.
            w, p = shape("joined")
            joined = 0
            for q, j, k in zip(p, p[1:], p[2:]):
                inward = (q - j) / np.linalg.norm(q - j) + (k - j) / np.linalg.norm(k - j)
                for at in (j, j - inward / np.linalg.norm(inward) * (w / 2 - 1)):
                    x, y = np.floor(at).astype(int)
                    joined += _off_rgb(img[y, x], ink, 2)
            # A trapezoid whose top and bottom fall just short of a pixel's centre, where the ramp's
            # outer half covers the next row by 0.4: drawn only to its bounds, it misses that row.
            trapezoid = area_error("trapezoid")
            # A convex square on whole pixels against a quad of the same rect, pixel for pixel.
            x0, y0, x1, y1 = box("square", 2)
            u0, v0, _, _ = box("quad", 2)
            square = int(np.abs(img[y0:y1, x0:x1] - img[v0:v0 + y1 - y0, u0:u0 + x1 - x0]).max())
            # A square whose edges halve pixels: each corner pixel a quarter covered, the box
            # filter's coverage and the product of the edges' halves. Their minimum would be a half.
            _, p = shape("halves")
            corners = [img[int(y), int(x)] for x, y in p]
            quarter = max(int(np.abs(c - (back + span / 4)).max()) for c in corners)
            # Two triangles meeting on a diagonal: no pixel inside the pair shows more of the
            # backdrop than a quarter, f(1 - f) for coverages f and 1 - f.
            x0, y0, x1, y1 = box("meeting", -2)
            meeting = float((1 - _ink_cover(img[y0:y1, x0:x1], back, ink)).max())
            # A concave shape and one of five corners draw nothing.
            refused = sum(_off_rgb(img[y0:y1, x0:x1], back, 2)
                          for x0, y0, x1, y1 in (box("concave", 2), box("five", 2)))
            # A triangle with a fourth corner on an edge, which rounding turns the other way by a
            # few float steps, is drawn: its first three corners' centroid its colour.
            _, p = shape("straight")
            x, y = np.floor(p[:3].mean(axis=0)).astype(int)
            straight = _off_rgb(img[y, x], ink, 2)
            ok = (level == 0 and slant <= 0.02 and hair <= 0.05 and joined == 0
                  and trapezoid <= 0.01 and square <= 1 and quarter <= 2
                  and meeting <= 0.25 + 2 / np.abs(span).max() and refused == 0 and straight == 0)
            detail = (f"{level} px of the level line or the rows beside it wrong (want 0), the "
                      f"slanted line's coverage {slant:.1%} off its area (want 2% at most), the "
                      f"hairline's {hair:.1%} (want 5% at most), {joined} px at the joins not the "
                      f"line's colour (want 0); the trapezoid's {trapezoid:.1%} (want 1% at most), "
                      f"the convex square {square} codes off the quad (want 1 at most), a halved "
                      f"square's corner {quarter} codes off a quarter covered (want 2 at most), "
                      f"the backdrop {meeting:.3f} of the triangles' diagonal (want 0.25 at most), "
                      f"{refused} px drawn by the refused shapes (want 0), the triangle with a "
                      f"corner on an edge {'drawn' if straight == 0 else 'refused'} (want drawn)")
        return ok, detail

    @arm("canvas-ui-table")
    def _():
        shot, probe = _canvas_run(workdir, "table", "--ui-table")
        table = _canvas_sheet(probe, "table")
        gaps = [table.get(f"gap{c}") for c in range(6)]
        colours = table.get("colours", [])
        if not shot or len(colours) != 6 or not all(gaps) or "overflow" not in table:
            return False, "the run failed or printed no table"
        img = _ppm_array(shot)[..., :3].astype(float)
        bg, rule_rgb, cursor_rgb = (_hex_rgb(colours[k]) for k in (1, 3, 5))

        def box(name):
            return [int(v) for v in table[name]]

        # The rule's pixel row across the content, the rule's colour.
        x0, y0, x1, y1 = box("rule")
        rule = _off_rgb(img[y0:y1, x0:x1], rule_rgb, 2)
        # The cursor's row and the rest, down each gap between two columns -- a pixel in from
        # each column's edge -- where no cell draws: the cursor's colour on its row, the table's
        # above and below it and under the header's titles. A glyph past its cell lands here.
        _, cy0, _, cy1 = box("cursor")
        _, hy0, _, hy1 = box("header")
        _, ry0, _, ry1 = box("rows")
        cursor = stray = 0
        for g in gaps:
            a, b = math.ceil(float(g[0])) + 1, math.floor(float(g[1])) - 1
            cursor += _off_rgb(img[cy0:cy1, a:b], cursor_rgb, 2)
            stray += (_off_rgb(img[hy0:hy1, a:b], bg, 2) + _off_rgb(img[ry0:cy0, a:b], bg, 2)
                      + _off_rgb(img[cy1:ry1, a:b], bg, 2))
        note, room = (float(v) for v in table["overflow"])
        ok = rule == 0 and cursor == 0 and stray == 0 and note > room
        detail = (f"{rule} px of the header's rule not its colour (want 0); {cursor} px of the "
                  f"cursor's row in the gaps between columns not its fill (want 0); {stray} px "
                  f"in the gaps elsewhere not the table's colour (want 0), with notes "
                  f"{note:.0f} points long in a column {room:.0f} wide")
        return ok, detail

    @arm("canvas-ui-table-pane")
    def _():
        shot, probe = _canvas_run(workdir, "table_pane", "--ui-table-pane")
        pane = _canvas_sheet(probe, "pane")
        colours = pane.get("colours", [])
        corners = {k: pane[k] for k in ("tl", "tr", "bl", "br") if k in pane}
        if not shot or len(colours) != 2 or "held" not in pane or len(corners) != 4:
            return False, "the run failed or printed no pane"
        img = _ppm_array(shot)[..., :3].astype(float)
        head_y, pane_y = (float(v) for v in pane["held"])
        x0, y0, x1, y1 = (int(v) for v in pane["rule"])
        rule = _off_rgb(img[y0:y1, x0:x1], _hex_rgb(colours[1]), 2)
        wrong = _canvas_wrong_pixels(shot, corners)
        ok = abs(head_y - pane_y) <= 1.0 and rule == 0 and not wrong
        detail = (f"the header at {head_y:.1f} px with the pane's top at {pane_y:.1f} (want "
                  f"within 1), {rule} px of its rule not its colour (want 0); the strip's top "
                  f"corners its fill and its bottom ones the backdrop"
                  + (f", but {'; '.join(wrong)}" if wrong else ""))
        return ok, detail

    # ---- strokes and dashes (spec 14.22)
    def printed(name, m):
        return m["area"], m["edge"]

    def peak(img, tokens):
        (x0, y0, x1, y1), _ = _canvas_cell(tokens)
        return float(img[y0:y1, x0:x1].max())

    def bar_runs(path, probe):
        # The runs of white along the bar's middle row, as (start, length) in pixels: over 0.75,
        # which a bar drawn at its pattern's average of a half never reaches.
        (x0, y0, x1, y1), _ = _canvas_cell(probe[("dashes", "bar")])
        row = _canvas_coverage(path)[(y0 + y1) // 2, x0:x1] > 0.75
        edges = np.flatnonzero(np.diff(np.concatenate(([0], row.astype(np.int8), [0]))))
        return [(int(a), int(b - a)) for a, b in zip(edges[0::2], edges[1::2])]

    @arm("canvas-caps")
    def _():
        strokes, probe = sheet_run("strokes")
        caps = {n: t for n, t in _canvas_sheet(probe, "strokes").items()
                if n.startswith(("segment-", "arc-"))}
        # The view's left edge 0.6 mm past the far segment's end, between its cap's corner and the
        # half width a bound once grew by.
        reach, reach_probe = _canvas_run(workdir, "strokes_reach", "--sheet", "strokes", "--view",
                                         "24.6,0,8")
        corner = _canvas_sheet(reach_probe, "strokes-reach")
        if not strokes or not reach or len(caps) != 6 or len(corner) != 1:
            ok, detail = False, f"a run failed or printed {len(caps)} cells (want 6) and no corner"
        else:
            worst, rows = _canvas_area_rows(strokes, caps, printed)
            wrong = _canvas_wrong_pixels(reach, corner)
            ok = worst <= 0.03 and not wrong
            detail = (f"worst {worst:.4f} px of edge (want <= 0.03), the square cap's corner "
                      + ("past its tile's bound drawn; " if not wrong
                         else f"past its tile's bound {wrong[0]}; ")
                      + ", ".join(rows))
        return ok, detail

    @arm("canvas-joins")
    def _():
        strokes, probe = sheet_run("strokes")
        joins = {n: t for n, t in _canvas_sheet(probe, "strokes").items()
                 if not n.startswith(("segment-", "arc-"))}
        places = _canvas_sheet(probe, "strokes-at")
        if not strokes or len(joins) != 9 or len(places) != 2:
            ok, detail = False, (f"the app failed or printed {len(joins)} cells and {len(places)} "
                                 f"places (want 9 and 2)")
        else:
            worst, rows = _canvas_area_rows(strokes, joins, printed)
            wrong = _canvas_wrong_pixels(strokes, places)
            ok = worst <= 0.03 and not wrong
            detail = (f"worst {worst:.4f} px of edge (want <= 0.03), "
                      + (f"places wrong: {', '.join(wrong)}; " if wrong else "both places right; ")
                      + ", ".join(rows))
        return ok, detail

    @arm("canvas-dash-areas")
    def _():
        dashes, probe, _ = dashes_run()
        cells = _canvas_sheet(probe, "dashes")
        if not dashes or len(cells) != 14:
            ok, detail = False, f"the app failed or printed {len(cells)} cells (want 14)"
        else:
            worst, rows = _canvas_area_rows(dashes, cells, printed)
            ok = worst <= 0.03
            detail = f"worst {worst:.4f} px of edge (want <= 0.03): " + ", ".join(rows)
        return ok, detail

    @arm("canvas-dash-places")
    def _():
        dashes, probe, _ = dashes_run()
        places = {n: t for n, t in _canvas_sheet(probe, "dashes-at").items() if n != "tagged-dash"}
        if not dashes or len(places) != 16:
            ok, detail = False, f"the app failed or printed {len(places)} places (want 16)"
        else:
            wrong = _canvas_wrong_pixels(dashes, places)
            ok = not wrong
            detail = ("16 places each their colour within 2 codes" if ok
                      else "wrong: " + ", ".join(wrong))
        return ok, detail

    # The bar centred in views 8 mm and 4 mm across; its dashes are 0.2 mm (the sheet's DASH_ON).
    @arm("canvas-dash-world")
    def _():
        seen = {}
        for across in (8, 4):
            path, near = _canvas_run(workdir, f"dashes_{across}mm", "--sheet", "dashes", "--view",
                                     f"-3.25,2.25,{across}")
            if path and ("dashes", "bar") in near:
                seen[across] = (bar_runs(path, near), _ppm_array(path).shape[1] / across * 0.2)
        if len(seen) != 2:
            ok, detail = False, "a run failed"
        else:
            ok = all(len(runs) == 3 and all(abs(n - want) <= 1.0 for _, n in runs)
                     for runs, want in seen.values())
            detail = "; ".join(f"{across} mm across: {len(runs)} dashes of "
                               f"{', '.join(str(n) for _, n in runs)} px (want 3 of {want:.0f} "
                               f"within 1)"
                               for across, (runs, want) in seen.items())
        return ok, detail

    @arm("canvas-dash-twins")
    def _():
        dashes, probe, _ = dashes_run()
        far, _ = _canvas_run(workdir, "dashes_far", "--sheet", "dashes", "--offset", str(1 << 40))
        mirrored, _ = _canvas_run(workdir, "dashes_mirror", "--sheet", "dashes", "--mirror")
        tiny, _ = _canvas_run(workdir, "dashes_tiny", "--sheet", "dashes", "--tile-shift", "16")
        texts = _canvas_sheet(probe, "dashes-text")
        if not dashes or not far or not mirrored or not tiny or len(texts) != 2:
            ok, detail = False, "a run failed or printed no strings"
        else:
            far_ae, _ = compare(dashes, far)
            a, b = _ppm_array(dashes), _ppm_array(mirrored)
            mirror_peak = float(np.abs(a[:, ::-1] - b).max())
            _, tiles_pae = compare(dashes, tiny)
            (px0, py0, px1, py1), _ = _canvas_cell(texts["plain"])
            (dx0, dy0, dx1, dy1), _ = _canvas_cell(texts["dashed"])
            plain_text, dashed_text = a[py0:py1, px0:px1], a[dy0:dy1, dx0:dx1]
            text_differ = int(np.any(plain_text != dashed_text, axis=2).sum())
            ink = float(plain_text[..., 0].sum() / 255.0)
            ok = (far_ae == 0 and mirror_peak <= 1.0 and round(tiles_pae * 255) <= 1
                  and text_differ == 0 and ink > 100.0)
            detail = (f"2^40 units off {far_ae} px (want 0), mirrored peak {mirror_peak:.0f} "
                      f"code(s) and 65 um tiles {tiles_pae * 255:.0f} (want <= 1 each), the dashed "
                      f"string {text_differ} px from the plain one (want 0, {ink:.0f} px^2 of ink)")
        return ok, detail

    # dash_min_px at 30 px: the default view's 20 px dashes are drawn solid at the pattern's
    # average, the 4 mm view's 40 px ones still dashed.
    @arm("canvas-dash-solid")
    def _():
        avg, avg_probe = _canvas_run(workdir, "dashes_min_8mm", "--sheet", "dashes",
                                     "--dash-min-px", "30")
        still, still_probe = _canvas_run(workdir, "dashes_min_4mm", "--sheet", "dashes", "--view",
                                         "-3.25,2.25,4", "--dash-min-px", "30")
        crossing = {n: t for n, t in _canvas_sheet(avg_probe, "dashes-at").items()
                    if n == "raised-crossing"}
        if not avg or not still or ("dashes", "bar") not in avg_probe or not crossing:
            ok, detail = False, "a run failed"
        else:
            runs = bar_runs(avg, avg_probe)
            (x0, y0, x1, y1), _ = _canvas_cell(avg_probe[("dashes", "bar")])
            row = _ppm_array(avg)[(y0 + y1) // 2, x0:x1, 0]
            lit = row[row > 64.0]
            inside = lit[1:-1] if len(lit) > 2 else lit
            dashed = len(bar_runs(still, still_probe))
            flat = len(inside) > 0 and float(np.abs(inside - 127.5).max()) <= 2.0
            raised = _canvas_wrong_pixels(avg, crossing)
            ok = not runs and flat and len(lit) >= 90 and dashed == 3 and not raised
            detail = (f"20 px dashes: {len(lit)} px of the bar's row lit, "
                      f"{inside.min():.0f}..{inside.max():.0f} inside (want one run, half white "
                      f"within 2 codes); 40 px dashes: {dashed} (want 3); the raised ghost "
                      + ("red where it crosses a bar" if not raised else raised[0]) if len(inside)
                      else "the bar drew nothing")
        return ok, detail

    @arm("canvas-dash-tags")
    def _():
        dashes, probe, _ = dashes_run()
        places = _canvas_sheet(probe, "dashes-at")
        empty = _canvas_sheet(probe, "dashes-empty")
        if not dashes or "tagged-dash" not in places or "removed" not in empty:
            ok, detail = False, "the app failed or printed no tagged dash"
        else:
            wrong = _canvas_wrong_pixels(dashes, {"tagged-dash": places["tagged-dash"]})
            left = peak(_ppm_array(dashes), empty["removed"])
            ok = not wrong and left == 0.0
            detail = (("the tagged dash its object's red within 2 codes" if not wrong
                       else "wrong: " + ", ".join(wrong))
                      + f", the removed bar's cell peak {left:.0f} (want 0)")
        return ok, detail

    @arm("canvas-dash-refused")
    def _():
        dashes, probe, said = dashes_run()
        empty = _canvas_sheet(probe, "dashes-empty")
        refusals = ("a segment whose dash gap is -1 is refused",
                    "an arc whose dash length is 1073741824 is refused")
        named = [r for r in refusals if r in (said or "")]
        if not dashes or "refused" not in empty:
            ok, detail = False, "the app failed or printed no refused cell"
        else:
            left = peak(_ppm_array(dashes), empty["refused"])
            ok = len(named) == 2 and left == 0.0
            detail = (f"{len(named)} of 2 refusals named in the log, the cell's peak {left:.0f} "
                      f"(want both, and 0)")
        return ok, detail

    @arm("canvas-budget")
    def _():
        _, probe = _canvas_run(workdir, "budget", "--sheet", "board", "--split", "--budget", "-W",
                               "1920", "-H", "1080", "-f", "30")
        timed = sorted((int(name), float(t[1])) for (sheet, name), t in probe.items()
                       if sheet == "budget")
        frames = [ms for _, ms in timed]
        if len(frames) < 10:
            ok, detail = False, f"{len(frames)} frames timed (want 30)"
        else:
            steady = sorted(frames[2:])
            median = steady[len(steady) // 2]
            ok = median <= 6.0
            detail = (f"the board split at 1920x1080 points, both views: median {median:.2f} ms "
                      f"over {len(steady)} frames (want <= 6; a timing arm, this machine's)")
        return ok, detail

    return arm.failed


def _canvas_coverage(path):
    """A white-on-black canvas picture as an (h, w) array of coverage, 0..1."""
    return _ppm_array(path)[:, :, 0] / 255.0
