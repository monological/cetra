# The 2D canvas

Exact 2D shapes drawn after tone mapping, at display resolution, kept on the GPU and edited in
place (`canvas.c/h`, `canvas_edit.c`, `canvas_record.h`, spec 14.0): segments, arcs, circles,
rounded boxes, polylines, filled polygons with holes, and text. It is there for pcb's schematic and
board (`docs/eda-roadmap.md`); `apps/canvas` is its test bed. The shaders are listed in
`docs/shaders.md` under 2D canvas.

## How it is drawn

- **An app owns the `Canvas`** and draws it from an engine overlay with `canvas_draw(canvas, &view)`,
  once per view.
- **A view is a plain value**: a rectangle, a pan, a zoom, a mirror, the layers it shows, and its
  active layer, which is drawn after the others but under any layer kept `above_active` (holes, a
  ratsnest).
- **Several views share one target** the canvas owns, one framebuffer switch each.
- **Shapes live in tiles**, `2^22` units on a side unless the canvas's desc sets `tile_shift`. A
  tile is a range of its layer's buffer, uploaded alone, not a buffer of its own: a buffer per tile
  was a draw per tile per layer, about 2 µs each on Apple's GL.
- **The tables are buffer textures.**
- **`canvas_generation`** adds the tag table's writes to the canvas's own, so idle mode can tell
  when a canvas needs drawing.

## Strokes

A stroke is drawn as PDF's is, by three things on its paint (`CanvasPaint`, spec 14.22): its cap,
its join and its dash. A paint of zeros is round, round and solid.

- **Caps** (`CanvasCap`: round, butt, square) end a segment, an arc and an open polyline, and every
  dash. A square end is flat half the width past the end point. A closed shape has no ends but its
  dashes'.
- **Joins** (`CanvasJoin`: round, mitre, bevel) turn a polyline's points, every point of a stroked
  polygon's rings, and a stroked box's corners when its corner radius is 0. A mitre longer than four
  widths, a turn whose cosine is under -0.875, is drawn as a bevel; a turn back on itself is round.
  Each segment draws its own side of a corner, its own body to its end inside the turn and up to
  the bisector outside it, half a pixel past it within the neighbour's width, so the two meet with
  no seam.
- **Dashes** (`CanvasDash`: length, gap and phase, in units) cut the stroke into pieces, each a
  piece of the stroke with the cap at its ends and the join at a corner inside it. A corner in a gap
  is drawn by nothing; whether a corner lies inside a dash is decided once, in double, for both
  segments that meet there.

| Shape | Its pattern starts |
|---|---|
| Segment | at `a` |
| Polyline | at its first point, carried through every point |
| Arc | at its own `a`, whichever way round it goes |
| Circle | at its rightmost point, counter-clockwise |
| Stroked box | at the middle of its bottom side, counter-clockwise |
| Stroked polygon | each ring at its first point |

- **A closed shape's pattern is stretched** to the nearest whole number of periods round it, so its
  dashes meet themselves where it closes.
- **Fills and text take no cap, join or dash.**
- **A dashed shape too fine to draw as dashes** -- a dash or a gap under `Canvas.dash_min_px`, 0.5
  by default -- is drawn solid at its pattern's average, the dash's share of the period, as an
  opacity: its pixels keep the rank their coverage gives, so a raised ghost still shows over what
  it crosses.
- **A dashed shape costs a 64-byte record** in a store of its own, its pattern after the shape's
  record; a solid one stays 48 bytes, and a solid round line writes no joins.

## Ten things easy to get backwards

1. **Coordinates are int64 units (pcb's nanometres) and never become floats until they are a
   distance from the view.** A record holds int32 offsets from its tile's origin, the tile table adds
   the tile's int32 offset from the view's integer centre, and only then is anything a float, so a
   sheet 2^40 units from the origin draws to the pixel what it draws at it (`canvas-translation`).
   A segment or arc reaching past a tile, and a fill triangle crossing one, is cut at the tile grid,
   every crossing computed from the original ends in one order so neighbours share their corners to
   the unit; the cut pieces overlap by half a pixel each.
2. **Coverage is `clamp(0.5 - d)`**, with d the exact signed distance in framebuffer pixels from the
   known scale, never `fwidth`, so a shape's coverage summed is its area (`canvas-areas`). A line is
   never narrower than a pixel.
3. **A layer resolves its coverage before it paints** (`include/canvas_pass.glsl`). The largest
   coverage any shape gives a pixel goes into the target's alpha, times the layer's opacity; then
   colour passes paint by that alpha and zero it -- raised, solid, faint, and outline rings last --
   so a translucent layer is translucent as a whole and its overlaps draw once.
   - Max and not the union, because a cut shape and a polygon's corners cover a pixel twice, and max
     is idempotent there.
   - Every pixel a layer covers is painted by one of its passes, so each layer hands the next an
     alpha of zero and none is cleared.
   - Where shapes of a layer overlap, a raised one's colour shows, its fringe being a fringe like any
     other. Otherwise which colour shows is not defined (fills draw before shapes, and a tile's range
     moves as it grows), so overlapping shapes want one colour.
4. **Rings are drawn last.** A ring is not a distance that falls away, so where it crosses another
   shape of its layer -- the next segment of a picked trace at its joint -- that shape must win.
   Drawn raised, the rings crossed every joint.
5. **A fill is triangulated on the CPU** (`triangulate.h`), and each triangle measures its inner ramp
   against the six outline edges at its corners, not its own edges: an earcut ear thinner than half a
   pixel otherwise leaves its band to the triangle beside it. EDGE records draw the outer half. A
   fill triangle is one instance, as a shape record is, and an edit settles every arc
   counter-clockwise and every edge with its polygon on its left, so the shaders hold one
   orientation.
6. **A shape refused part way is taken out whole** (an edit's mark and rollback), or a polygon whose
   triangles cross too many tiles would keep the ones added before and its whole outline.
7. **Every shape carries two tags, a net and an object**: indices into a `CanvasTags` table a canvas
   may share with another, so one write highlights a net in the schematic and the layout. A tag
   carries colour, a pick outline, dim, hidden and raised. A tag or style write uploads no geometry.
8. **Edits are built on any thread.** A `CanvasEdit` holds no GL; layout, splitting and triangulating
   happen there, and `canvas_apply` merges one on the main thread.
9. **Text is a glyph record each, placed by its pen in ems** from the string's anchor
   (`font_pen_step`, `docs/ui.md`), so its size and turn ride the record. A canvas draws with at most
   four fonts (`CANVAS_FONT_MAX`), a texture unit each, and cuts at the font's true edge
   (`Font.sdf_edge`), half a code under stb's stated value because stb truncates each field texel.
10. **A stroke is baked into its records.** The edit lays the paint's cap, join and dash along the
    shape in double, so changing one is an edit of the shape, not a write to a table as a tag's
    colour is.

## Triangulation

`triangulate.c/h` turns an outline with holes into triangles. It is a port of Mapbox's earcut v2.2,
with its ISC notice. Holes may wind either way and are bridged at their leftmost points. Every
triangle's corners are points given, and there are n - 2 + 2h triangles for n points and h holes.
It is pure CPU and safe on any thread, so the canvas's edits call it off the main thread, and it is
public, for an app with polygons of its own.
