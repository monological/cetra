# Cetra for EDA — the 14.x Roadmap

The master plan for the 14.x series: what cetra has to gain so that an EDA program, a tool for
designing circuit boards, can be built on it rather than a game. The program is pcb, kept closed in
its own repository with cetra as a submodule (`apps/pcb`, spec 11.105): one window holding a code
editor, a schematic, a board layout, a 3D view of the board, datasheet pages and an agent's chat.
pcb's `ARCH.md` (section 9, "cetra changes this needs") lists eight changes it wants from the
engine. This document takes each of them, says what cetra already has toward it, what is still to
decide, and which step of pcb's build first needs it.

**Who owns what.** pcb's `ARCH.md` owns what pcb wants; this document owns how cetra provides it.
Every change goes through cetra's own spec process, numbered 14.x, and pcb picks it up by bumping its
submodule. Nothing in cetra may name a pcb type or depend on pcb's code: each item is a general
engine feature that pcb happens to be the first user of.

**Keeping it current.** This is a living plan. Every 14.x spec updates it on its own branch, as
`AGENTS.md` (Planning Workflow) requires: the item's status, the decisions its spec made, anything
it did differently from what is written here and why, and any new item it found.

_Last updated: 2026-10-29._

---

## What the tool asks of the engine

These come from pcb's `ARCH.md` sections 8 and 9, and every item below has to respect them.

- **One window, one GL context.** Every view stays loaded in memory and on the GPU, so switching
  views costs one frame. The 2D and 3D views share the window, the text, the UI, the headless
  capture and the test suite; a second rendering engine for 2D was considered and refused.
- **The layout of the window.** A main area holding one view, or two side by side (the split); a
  status line under it; the agent's chat always visible down the right, collapsible by one key. The
  background is black and there are no toolbars, menus or panels.
- **Speed.** Input reaches the screen within one frame. The main thread only draws and applies
  results; parsing, checks, routing and AI calls run on other threads and publish finished,
  unchanging snapshots, so drawing never waits on a lock. Nothing is drawn unless something changed;
  live routing turns continuous drawing back on.
- **Headless.** `pcb render` draws any view to an image through cetra's screenshot path, and that is
  how pcb's agent sees the board. So every item here has to work with no visible window.
- **True units.** A board is about 100 mm across and an 0402 part is 1 mm, all in real millimetres.
  Spec 13.57 made cetra render at that scale (item 7).
- **The 3D view must look first-rate**, real time and not ray traced, using everything cetra has:
  translucent FR4, glossy solder mask over extruded copper, plated holes as instances, a studio
  environment with shadows, reflections and TAA, and a still view that keeps accumulating TAA's
  jitter until it is clean.

---

## Status at a glance

| # | Item | Status | First needed by pcb's step | Spec |
|---|------|--------|----------------------------|------|
| 1 | The 2D canvas | **Built**, spec 14.0; images, drag offsets and the rest below are left for later | 6, the window (the schematic); then 7, the layout | 14.0 |
| 2 | Idle mode: drawing only when something changed | **Built**, spec 14.1, for frames that draw no 3D scene | 6 | 14.1 |
| 3 | Skipping the 3D scene pass when no 3D view is shown | Not started; takes a still 3D view drawn on demand, moved from item 2 | 8, the 3D view | |
| 4 | Drawing the 3D scene into part of the window | Not started | 8 | |
| 5 | Text input and scrolling panes in the UI | Not started | 6 (the command palette and the `/` jump); 9 (the chat) | |
| 6 | The text editor widget | Not started | 6 (the code view) | |
| 7 | Rendering at any scale, in true units | **Built**, specs 13.57 and 13.58; the macro depth of field left | Already used by pcb's pictures | |
| 8 | What the glTF import does not read, or reads otherwise | Not needed by pcb; three bugs worth fixing for cetra's own sake | None | |

**Proposed order**: 1, 2, 5, 6, then 3 and 4 together when pcb starts its 3D view. The canvas goes
first because the schematic and the layout both stand on it, and it is the largest. Item 8's bugs
and the macro depth of field can be taken whenever they fit.

pcb's build steps, for reference: 6 is the window (code editor, schematic, keys, palette, help,
shared selection, diff overlays and blink); 7 the layout view; 8 the 3D view; 9 the agent; 10
datasheets.

---

## 1. The 2D canvas

**What pcb needs** (`ARCH.md` sections 6 and 9):

- **Primitives**: segments with a width (routes, wires), arcs, circles, rounded rectangles (pads),
  filled polygons (copper pours, the board outline) and text.
- **Exact edges**: each primitive is a quad whose shader computes its exact outline, so edges are
  clean at any zoom with no multisampling.
- **After tone mapping**: it draws at display resolution, so nothing grades, blooms or rescales it.
- **Tiles per layer**: each tile is one GPU buffer, a change re-uploads only the tiles it touched,
  and tiles out of view are skipped.
- **Detail by zoom**: zoomed far out, small text and fine pads drop out, and thin lines stay at
  least one pixel wide.
- **Precision**: geometry stays in nanometres on the CPU, and each tile sends positions relative to
  its own origin, so zooming deep into any corner stays exact.
- **Instant highlighting**: each primitive carries its net's ID, and the shader reads a small table
  of net states (selected, dimmed, hidden, a diff colour). Highlighting a net or flipping a diff
  changes one table entry and rebuilds no geometry.
- **Layers** draw in order, each with its own colour and transparency, the active one on top.
- **Beyond copper**, the same canvas draws schematic symbols, group frames, and datasheet pages as
  tiled images.
- **Diffs**: two versions overlaid -- added in green, removed as a red ghost, a moved part as a
  ghost at its old place with an arrow. Blink flips the view between the two versions; the split
  shows them side by side, both panned and zoomed together.

**Not cetra's: picking.** "What is under the cursor" is answered on the CPU by pcb, from a spatial
index over its exact geometry, ranked by its own rules (mode, shown layers, the more specific
object). It never reads back from the GPU.

**What cetra had before 14.0:**

- **The overlay hook**: the one place an app draws after tone mapping, at display resolution,
  captured by a headless screenshot. It was a single slot, which the game UI took.
- **The UI's draw list** (`ui_draw.c`): quads, 9-slice, rounded rectangles whose corners and
  borders are an exact signed-distance evaluation, and SDF glyph runs, batched by texture, program
  and clip. It works in window points, top-left origin, +Y down, and is rebuilt every frame.
- **SDF text** (`text.c`), whose `Font` and `font_get_glyph` the UI already shares.
- **The canvas controller** (`app.h`): pan and zoom on a square-on orthographic camera over a plane.
  It moves a scene's camera, in float world units, so it does not fit the canvas.
- **No polygon triangulation** anywhere in `cetra/src`.

**Decided in spec 14.0** (`specs/14.0-the-2d-canvas.md` has the reasons):

- **Sharing the overlay: an ordered list** of up to eight post-tonemap draws (`engine_add_overlay`),
  the UI at `UI_OVERLAY_ORDER`, over a canvas added lower. Built in phase 1, which also made
  `free_ui_system` take the UI off the list (it used to leave the engine holding a freed pointer)
  and renamed the canvas controller `PlaneDrag`, so the canvas could have the name.
- **Filling a polygon: triangulated on the CPU** by a port of earcut, in cetra (`triangulate.h`,
  public), with antialiased contour edges. A stencil had nowhere to live: the window is
  multisampled and the picture FBO has colour only. Built in phase 4: a fill's inner ramp is
  measured against the outline edges at its triangle's corners, since an ear thinner than half a
  pixel leaves its band to the triangle beside it.
- **A layer resolves its coverage by the largest any shape gives a pixel**, then paints each pixel
  once, inside the view's one render pass. Max rather than union, because a shape cut at a tile and
  a polygon's corners cover a pixel twice and max is idempotent there. Built in phase 4, earlier
  than planned, because fills needed it.
- **Tiles and precision:** tiles of 2^22 units; each shape stored as int32 offsets from its tile,
  added to the tile's int32 offset from the view in the shader, so precision does not depend on how
  far from the origin the view is. Long shapes are split at tile edges.
- **Text in world units:** laid out by the canvas, sized by the em, cut at the font's true edge,
  dropped below a pixel size. Built in phase 5:
  - A string is a record a glyph, laid out on the edit's thread, its pen in ems, so its size and
    turn ride the record.
  - A canvas draws with at most four fonts, one texture unit each.
  - The font's true edge is half a code under stb's stated on-edge value, since stb truncates the
    field's texels. Cut at the stated value, every glyph was 0.65% thin against stb's own
    rasterizer.
  - Glyphs are indexed by codepoint, so the table holds ASCII, as the atlas does. A denser index
    comes with glyphs beyond ASCII (items 5 and 6).
  - The UI's glyphs and the text meshes cut at the same edge, judged on before and after pictures
    of silent's and gametest's menus: they had been drawn about two atlas pixels bolder than the
    face.
- **Several views at once:** a view is a value (a rectangle, a pan, a zoom, and its own shown layers
  and active layer), drawn by `canvas_draw` into one target the canvas owns. Built in phases 2
  and 7: a board split as two views, the second from below with its own layers, each equal to
  itself drawn alone; both at 4K in 2.4 ms on an M1 Max.
- **Retained, not rebuilt**, with three deviations from what is written above:
  - **A tile is a range of its layer's buffer**, not a buffer of its own, uploaded alone when it
    changes. A buffer per tile meant a draw per tile per layer, thousands zoomed out, at about 2 µs
    a draw on Apple's GL.
  - **Each shape carries two tags**, a net and an object, where ARCH has a net ID only: diff colours
    and selection belong to objects. Built in phase 6: a tag's state is a colour, a pick outline
    (a ring drawn last, under its layer's other shapes, so it stays outside a picked trace at its
    joints), a dim, hidden and raised. Where shapes of a layer overlap, a raised one's colour
    shows; otherwise which shows is not defined.
  - **Images, and datasheet pages with them, moved out of 14.0** to pcb's datasheet step, which
    needs texture residency of its own.

**What 14.0 found that this list did not say:**
- **The ring of a picked shape has a pixel or two of error at a joint's inner corners.** The ring
  wants the least distance over all of an object's shapes, which max blending cannot give; three
  more passes a layer would. Invisible at 1x.
- **Where shapes of one layer overlap in different colours, which colour shows is not defined**,
  beyond a raised shape winning and a shape covering a pixel well winning over another's edge.
  Fills draw before shapes, and a tile's place in its buffer moves as it grows, so neither the
  order of adding nor any other order holds. pcb gives shapes that overlap one colour, or raises
  the one that should show.
- **The active layer is drawn after the others**, as ARCH says, **except layers kept above it**
  (`CanvasLayer.above_active`, added by the reviews): holes, a ratsnest and a selection overlay
  stay over whichever copper is active.
- **For idle mode (item 2):** `canvas_generation` changes with every edit applied, layer or style
  added, and style or tag written, a shared tag table's included. A layer's opacity and order and
  a view are plain settings and change nothing there; an app knows when it writes them.
- **Buffer textures are not yet checked on Linux and Windows.** They are core in GL 3.1, below the
  4.1 the engine requires, so the risk is a driver's bug; the build VMs were gone for this spec.

**Left for later**, each with a need named:
- **Images and datasheet pages**, with the texture residency they need, for pcb's step 10. A record
  kind is free for them.
- **A per-tag offset**, so a dragged object moves without an edit, for pcb's step 7.
- **Triangles supplied by the caller**, such as pcb's own constrained triangulation, in place of
  the canvas's earcut.
- **Glyphs beyond ASCII**, with a dense glyph index, alongside items 5 and 6, which need them too.
- **Dashed lines**, for schematic buses and courtyards.
- **A transparent view background**, for the canvas over the 3D view (items 3 and 4).
- **One target shared by several canvases**, should pcb draw two canvases in one window.

---

## 2. Idle mode

**What pcb needs**: nothing drawn unless something changed, and continuous drawing back on while
something moves (live routing). Input still reaches the screen within one frame.

**What cetra had before 14.1**: `engine_run` drew every frame, throttled by vsync. The only path
that waited was a minimized window, which blocks on `glfwWaitEventsTimeout(0.1)`.

**Built in spec 14.1** (`specs/14.1-idle-mode.md` has the reasons):

- **Idle mode covers the frames that draw no 3D scene**: the schematic, the layout, the code editor
  and the chat. A frame whose scene has anything to draw keeps the loop drawing every frame, as
  before: an item, an IBL, a sky, water, falling rain, a fire, a particle system.
- **It is a switch an app turns on, off by default** (`Engine.idle`, `EngineConfig.idle`), so every
  game, every other app and every golden is unchanged. pcb and `apps/canvas` turn it on.
- **A still 3D view drawn on demand moved to item 3** (below), with what keeps a 3D frame alive and
  the accumulation pcb's ARCH asks for. Only with pcb's 3D view is there a board to judge it on.
- **Who says something changed:**
  - input: in a window any wake draws a frame, so every event does;
  - `engine_request_frame`, from any thread, which wakes a waiting loop with `glfwPostEmptyEvent`;
  - `engine_request_frame_after`, a timed wake, for pcb's blink;
  - `engine_begin_work` and `engine_end_work`, around a thread's result, such as a parse;
  - the engine's own: the UI while it eases or a screen enters, the debug GUI while ImGui takes
    queued input, the texture loader when a decode is done, the loading screen, a pointer script.

  A change made inside a frame is drawn by that frame, so pcb asks only for changes made elsewhere.
- **The clock across a wait.** The wait is kept out of `delta_time` and the windowed `render_time`,
  so what animates resumes where it stopped.
- **The frame counter** advances only on drawn frames, so the frame after a wait is adjacent to the
  last and no history is dropped.
- **Headless**, a run in idle mode ends at the first frame after which nothing asks for another and
  no work is out, and that frame is its screenshot: what `pcb render` wants. `--frames N` is then
  the most it draws. A run without idle mode is unchanged.
- **pcb's part:**
  - set `idle`;
  - call `engine_end_work` (or `engine_request_frame`) when a worker publishes a snapshot;
  - while live routing runs, call `engine_request_frame` each frame;
  - read a gamepad, if ever, the same way, since pads send no events.

**Left for later:** a gamepad that wakes the loop by itself. GLFW polls pads, and sends no event
for them.

---

## 3. Skipping the 3D scene pass

**What pcb needs**: when no 3D view is on screen, the 3D scene costs nothing, while everything stays
loaded so that coming back costs one frame.

**What cetra has today**: the loading screen (spec 13.34) already has a frame that draws no camera
pass and no post chain, while uploads and captures go on. That is a precedent for skipping the scene
and keeping the overlay.

**To decide in its spec:**

- **The switch**: a field on the engine, or the app's render callback choosing not to draw.
- **What still runs** while the scene is off: uploads, captures, the shadow tiles.
- **Coming back.** The temporal histories are many frames old when the scene returns, so the first
  frame back is treated as a cut, as a render-scale change resets them (spec 11.8).

**A still 3D view drawn on demand** (moved from item 2 while planning spec 14.1). CAD programs
redraw a 3D view only when it moves or its model changes (KiCad, FreeCAD, Fusion); redrawing every
frame is a game's habit. None of this is for a game, which moves every frame.

- **TAA never comes to rest by itself.** Each history blends the new frame in at a fixed 3%, the
  jitter cycles through 8 positions (16 when upscaling), and several noise seeds never repeat, so
  "draw until frames stop changing" has no end. There are two ways to stop:
  - **Count frames**: draw until every history has forgotten the last change to under half a colour
    code, about 200 frames with TAA and 420 upscaling, from the histories' blend factors and the
    chain AO and specular occlusion form into TAA. The picture is TAA's as today.
  - **Accumulate**: while the view is still, average jittered frames equally over a longer sequence,
    16 to 64 positions, then stop. This is supersampling spread over time, as Blender's Eevee
    viewport does, and as Unreal's Movie Render Queue does for cinematics. It is what pcb's ARCH asks
    for, and it is cleaner on what a board is full of, thin traces and small text, where TAA's
    leftover shimmer shows.
- **Accumulating needs an exact statement of when the 3D picture changed.** A false "changed" throws
  the clean image away, and today a material, light or setting write is a plain field write that
  nothing reports.
- **What keeps a 3D frame alive.** The engine's own work that finishes: GI sweeps and probe captures,
  cached shadow tiles opening and a mover's 120-frame hold, auto-exposure until it snaps. And what
  never finishes: wind, water, rain, fire, particles, cloud drift, the sky cycle, an animated surface
  hook, an app shader reading `time`.

---

## 4. Drawing the 3D scene into part of the window

**What pcb needs**: the split showing the layout beside the 3D view. Since the chat is always
visible, the main area is narrower than the window, so in practice every 3D view needs this, not only
the split. The exception is the chat collapsed and no split.

**What cetra has today**: the scene targets and the whole post chain are sized from the framebuffer,
and the frame top rebuilds them when the size changes. The present pass writes the whole window. The
CRT's picture (spec 13.28) must be exactly window-sized because the UI's scissor is in window pixels.

**To decide in its spec:**

- **Targets sized to the rectangle**, with the camera's aspect taken from it. Toggling the split
  changes the rectangle, which rebuilds the targets and resets the histories, a visible fraction of a
  second of re-converging today. Whether that is acceptable, or the targets keep a fixed size and draw
  into part of it, is the main choice.
- **The overlay still covers the whole window**, drawn after the scene's rectangle is composited.
- **Headless**: `pcb render` of the 3D view alone, at its own size.

---

## 5. Text input and scrolling panes

**What pcb needs**: the chat, a scrolling history with an input line. Also, as found writing this
plan rather than stated in `ARCH.md`: the command palette (Cmd+K, a few letters typed to filter a
list of commands, each shown with its key) and the `/` jump to any part or net by name, fuzzy, which
are both a text field over a list, and are wanted in step 6, before the chat.

**What cetra has today:**

- **The UI's element list is closed**: panel, label, button, toggle, slider, selector (`ui.h`). There
  is no text field, no scrolling pane, and no container that clips and scrolls.
- **Typed characters reach only ImGui.** The engine binds GLFW's character callback to ImGui
  (`engine.c`, `glfwSetCharCallback`); the game UI takes its input as a struct of values with no
  characters in it.
- **Wrapped text** already exists (`ui_text_width`, `ui_text_wrap_point`), and the draw list already
  batches by clip rectangle.

**To decide in its spec:**

- **The text field**: a caret, a selection, the clipboard (GLFW has it), key repeat, UTF-8, and
  whether input methods for other scripts are in scope.
- **The scrolling pane**: clipping, the wheel and the keys, scrolling to keep something in view, and
  long histories without laying out every line every frame.
- **Focus and keys**: keys belong to a view, Esc returns from the chat to the view, and the focused
  pane has a subtle border.
- **One text core or two.** A single-line field is a small case of item 6's buffer. Whether the two
  share their editing core is a decision for whichever spec comes first.

---

## 6. The text editor widget

**What pcb needs** (`ARCH.md` section 8): pcb's code view, an editor of its own in C, borrowing
freely from rxi's lite (MIT) and vis (ISC) and keeping their licence notices on copied code.
Standard keys first; key handling built on modes from the start, so that a vim mode is an addition
later and not a rewrite. The widget goes in cetra. What stays in pcb:

- **The colouring**, from pcb's own parser.
- **The link** that lands edits made in other views in the open buffer.
- **Undo**: typing shares the project's one undo timeline, which is pcb's.

**What cetra has today**: nothing of an editor; SDF text and the UI draw list's glyph runs.

**To decide in its spec:**

- **The buffer.** vis keeps its text in a piece table; lite keeps an array of lines.
- **Seams to the host**: undo handed to the host, colour spans taken from the host, and edits from
  outside applied without losing the caret, the selection or the scroll.
- **The mode and key-map seam**, so a vim mode is an addition.
- **Speed**: a large file drawn within a frame, only the visible lines laid out.
- **A monospaced font**, and how glyphs are cached for it.
- **Headless**: the widget drawn and driven with no window, for tests.

---

## 7. Rendering at any scale, in true units — built

**Built in specs 13.57 and 13.58.**

- **13.57**: the engine's typical size (`Engine.typical_size`, `--typical-size 0.001` for a board),
  every tuning length scaled by it, and a camera's near and far planes fitted each frame to what it
  sees (`Camera.fit_clip`, `--fit-clip`). `scale-twin` holds a scene at 0 px against itself shrunk
  1024 times.
- **13.58**: a short occluder keeps its soft shadow, the cascade thresholds counted in texels of the
  map rather than as a fraction of its depth range.

pcb's pictures already render at true size through them, and its 3D view will make its engine at a
typical size of 0.001.

**Left for later**, from those two specs:

- **A physical depth of field.** The CoC today is a look with no focal length or aperture. A
  thin-lens CoC from the camera's focal length and aperture would put a macro shot's thin focus where
  a real lens does, at any scale. 13.57 names it as the next spec pcb is likely to want for
  close-ups.
- **The cascade penumbra estimate** is normalised by a depth range the padding dominates, so its
  softness depends on how a cascade was fitted, not on the light (13.58).
- **Lights' default size** (50 m) and the shadow system's metre defaults stay; a small scene states
  its own.
- **A camera inside a large box** falls to the clip floor.
- **Reversed depth**, which needs GL 4.5's clip control that macOS's 4.1 lacks.

---

## 8. What the glTF import does not read, or reads otherwise

Found by pcb writing its generated models (pcb spec 4.3). pcb's 3D view will set cetra's materials
straight from pcb's own table, so none of these blocks pcb; they matter only to a `.glb` file and to
pcb's pictures, and pcb works round each one today. They fall into three kinds.

**Bugs against glTF**, worth fixing for cetra's own sake:

- **`COLOR_0` is decoded as sRGB** (`pbr_frag.glsl`, `sRGBToLinear(VertexColor.rgb)`); glTF says it
  is linear. The fix has to keep whatever FBX and OBJ files expect, which glTF does not decide.
- **One texture transform for every map.** `KHR_texture_transform` is read from the base colour
  texture alone (`import.c`) and applied to every map; glTF gives each texture its own.
- **An opaque material with an alpha below 1 goes to the blend pass.** `classify` (`draw_list.c`)
  sends any opaque material with opacity below 1 or an opacity map to the blend lane. That is right
  for formats with no alpha mode, FBX and OBJ, and wrong for glTF, which states OPAQUE and says its
  alpha is ignored.

**An extension cetra does not read**: `KHR_materials_anisotropy`. Subsurface scattering has no
ratified glTF extension, so that half is not cetra's gap; pcb sets it through a cetra scene file
beside each `.glb`, by material name.

**cetra's own choices, not bugs:**

- **Automatic levels of detail** on any mesh of 256 triangles or more (`lod.c`,
  `LOD_MIN_TRIANGLES`). pcb's pictures turn them off.
- **The shadow catcher is a plane at y = 0**, so pcb stands a board's bottom there.

---

## Not among the eight, but on the way

- **A second window**, for a second monitor. pcb lists it as an open question and as a cetra change;
  nothing is decided.
- **The physical depth of field** of item 7, likely wanted before pcb's 3D view is judged on its
  look.
