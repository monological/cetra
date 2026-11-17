# Cetra for EDA — the 14.x Roadmap

The master plan for the 14.x series: what cetra has to gain so that an EDA program, a tool for
designing circuit boards, can be built on it rather than a game. The program is pcb, kept closed in
its own repository with cetra as a submodule (`apps/pcb`, spec 11.105): one window holding a code
editor, a schematic, a board layout, a 3D view of the board, datasheet pages and an agent's chat.
pcb's `ARCH.md` (section 9, "cetra changes this needs") lists nine changes it wants from the
engine. This document takes each of them, says what cetra has toward it, what is still to decide,
and which step of pcb's build first needs it.

**Who owns what.** pcb's `ARCH.md` owns what pcb wants; this document owns how cetra provides it.
Every change goes through cetra's own spec process, numbered 14.x, and pcb picks it up by bumping its
submodule. Nothing in cetra may name a pcb type or depend on pcb's code: each item is a general
engine feature that pcb happens to be the first user of.

**Keeping it current.** Every 14.x spec updates this plan on its own branch, as `AGENTS.md`
(Workflow) requires: the item's status, the decisions its spec made, anything it did differently
from what is written here and why, and any new item it found.

_Last updated: 2026-11-17: 14.39, tags in 3D, built for item 4a._

---

## What the tool asks of the engine

These come from pcb's `ARCH.md` sections 8 and 9, and every item below has to respect them.

- **One window, one GL context.** Every view stays loaded in memory and on the GPU, so switching
  views costs one frame. The 2D and 3D views share the window, the text, the UI, the headless
  capture and the test suite; a second rendering engine for 2D was considered and refused.
- **The layout of the window** (pcb spec 6.1, `ARCH.md` section 8). The main area is a tree of
  panes, each showing one view: `|` splits the focused pane left and right, `\` top and bottom,
  Space Enter opens another, `:q` closes the focused one, and two panes may show one view -- one
  file twice with a cursor in each. A code pane goes to another file through Cmd+P. Space m shows
  the messages along the bottom, under every pane, by splitting the top of the tree, and hides them
  again; Space a shows the agent's chat at the right edge. One status line runs along the bottom
  for whatever has focus, vim's line while `/` or `:` is typed. The background is black, with no
  toolbars, menus or panels, and the fonts are built into the binary.
- **Speed.** Input reaches the screen within one frame. The main thread only draws and applies
  results; parsing, checks, routing and AI calls run on other threads and publish finished,
  unchanging snapshots, so drawing never waits on a lock. Nothing is drawn unless something changed;
  live routing turns continuous drawing back on.
- **Headless.** `pcb render` draws any view to an image through cetra's screenshot path, and that is
  how pcb's agent sees the board. So every item here has to work with no visible window.
- **True units.** A board is about 100 mm across and an 0402 part is 1 mm, all in real millimetres
  (item 7).
- **The 3D view must look first-rate**, real time and not ray traced, using everything cetra has:
  translucent FR4, glossy solder mask over extruded copper, plated holes as instances, a studio
  environment with shadows, reflections and TAA, and a still view that keeps accumulating TAA's
  jitter until it is clean.

---

## Status at a glance

| # | Item | Status | First needed by pcb's step | Spec |
|---|------|--------|----------------------------|------|
| 1 | The 2D canvas | **Built**, with caps, joins and dashes; images, drag offsets and the rest below are left for later | 6, the window (the schematic); then 7, the layout | 14.0, 14.22, 14.36, 14.37 |
| 2 | Idle mode: drawing only when something changed | **Built**; with 14.38 it waits on 3D views at rest too | 6 | 14.1, 14.38 |
| 3 | Skipping the 3D scene pass when no 3D view is shown | **Built**, with a still view that averages jittered frames and rests; a TAA-only view resting and accumulating under TAAU left for later | 8, the 3D view | 14.38 |
| 4 | Drawing the 3D scene into part of the window | **Built**: any number of views, each a camera, a rectangle, its own settings and its own buffers; world work serving every view equally, a scene per view and a second window left for later | 8 | 14.38 |
| 4a | Tags in 3D: a surface's net or part, and its kind, drawn by a table | **Built**: hidden, painted, dimmed, faded see-through and outlined, per tag and per class; transmissive and late surfaces faded and painted, and a halo round what is lit, left for later | 8 (choosing, the spotlight, see-through, a pane's layers) | 14.39 |
| 5 | Text input and scrolling panes in the UI | **Built**; composing text in an input method, glyphs past the atlas and the rest below are left for later | 6 (the command palette and the `/` jump); 9 (the chat) | 14.2, 14.3 |
| 6 | The text editor widget | **Built**; sideways scrolling, a regular-expression find and the rest below are left for later | 6 (the code view) | 14.4 |
| 6a | The editor's vim mode | **Built**, held to vim 9.2 itself; named registers, macros, marks and the rest below are left for later | 6 (the code view, which is vim only) | 14.5, 14.9 |
| 7 | Rendering at any scale, in true units | **Built**; the physical depth of field left | Already used by pcb's pictures | 13.57, 13.58 |
| 8 | What the glTF import does not read, or reads otherwise | Not needed by pcb; three bugs worth fixing for cetra's own sake | None | |
| 9 | What the panes and the fonts need of the UI | **Built** | 6 (the window's panes, and its fonts built into pcb's binary) | 14.6 |

Specs 14.7, 14.9 to 14.21, 14.23 to 14.28 and 14.30 to 14.37 are the smaller changes pcb's window
asked for, listed under "Not among the nine". Spec 14.8 was not asked for by pcb: a headless run
waits for the textures its start-up asked for before frame 1 (`docs/architecture.md`, Render
Pipeline), which `pcb render` gets for free. Nor was 14.29, which splits the gate suite into files (`docs/verification.md`,
Running the gates).

**Order**: 1, 2, 5, 6, 6a and 9 are built, in that order; the canvas went first because the
schematic and the layout both stand on it, and the editor stands on 5's UTF-8, typed input,
clipboard and input script. 3 and 4 were built together in 14.38, for pcb's 3D view (its spec
8.1), with a panel's shadow fitted to the view at a board's scale (item 7); 4a, tags in 3D, was
built on 14.38's views in 14.39, which pcb's 8.1 asked for beside them and the nine did not name.
Item 8's bugs and the physical depth of field can be taken whenever they
fit.

pcb's build steps, for reference: 6 is the window (code editor, schematic, keys, palette, help,
shared selection, diff overlays and blink); 7 the layout view; 8 the 3D view; 9 the agent; 10
datasheets.

---

## 1. The 2D canvas

**What pcb needs** (`ARCH.md` sections 6 and 9): routes, wires, arcs, circles, pads, pours, the
board outline and text, with exact edges at any zoom, drawn after tone mapping at display
resolution; tiles per layer re-uploaded only where changed; precision in nanometres anywhere on the
board; highlighting a net or flipping a diff without rebuilding geometry; layers in order with the
active one on top; schematic symbols, group frames and datasheet pages on the same canvas; and
diffs, two versions overlaid, blinked or side by side.

**Not cetra's: picking.** "What is under the cursor" is answered on the CPU by pcb, from a spatial
index over its exact geometry, ranked by its own rules. It never reads back from the GPU.

**Built in spec 14.0** (`docs/canvas.md`): `canvas.h` and `triangulate.h`, drawn from an engine
overlay (`engine_add_overlay`, an ordered list the UI shares at `UI_OVERLAY_ORDER`). Coordinates in
int64 units cut into tiles of 2^22, a tile a range of its layer's buffer, fills triangulated on the
CPU by earcut (a stencil had nowhere to live: the window is multisampled and the picture FBO has
colour only), a layer's coverage resolved by max before it paints, text as glyph records in ems,
several views of one canvas, and two tags a shape, a net and an object, whose state (colour, pick
outline, dim, hidden, raised) is a table write. `canvas_generation` tells idle mode when to draw.

**Built in spec 14.22** (`docs/canvas.md`, Strokes): a stroke's cap, join and dash on its paint, as
PDF has them. Dashes are pieces of the stroke drawn per pixel, a pattern carried through a
polyline's points and across tile cuts and stretched round a closed shape, its corners joined where
a dash runs through them; under `Canvas.dash_min_px` a pattern is drawn as its average. A solid
shape's record and pixels are as they were.

**What pcb has to know:**
- **Where shapes of one layer overlap in different colours, which colour shows is not defined**,
  beyond a raised shape winning. pcb gives shapes that overlap one colour, or raises the one that
  should show.
- **The active layer is drawn after the others, except layers kept above it**
  (`CanvasLayer.above_active`): holes, a ratsnest and a selection overlay stay over whichever copper
  is active.
- **Buffer textures are not yet checked on Linux and Windows.** They are core in GL 3.1, below the
  4.1 the engine requires, so the risk is a driver's bug.
- **A stroke's cap, join and dash are the paint's**, baked into the records when the shape is added:
  a ghost is dashed by its paint, a dashed rectangle is one `canvas_edit_box` whose pattern runs
  round it, and changing a stroke is an edit.

**Left for later**, each with a need named:
- **Images and datasheet pages**, with the texture residency they need, for pcb's step 10. A record
  kind is free for them.
- **A per-tag offset**, so a dragged object moves without an edit, for pcb's step 7.
- **Triangles supplied by the caller**, such as pcb's own constrained triangulation, in place of
  the canvas's earcut.
- **Glyphs beyond ASCII on the canvas**, with a dense glyph index: its glyph table holds 128
  codepoints, though the font's atlas holds more since item 5.
- **A transparent view background**, for the canvas over the 3D view (items 3 and 4).
- **One target shared by several canvases**, should pcb draw two canvases in one window.

---

## 2. Idle mode

**What pcb needs**: nothing drawn unless something changed, and continuous drawing back on while
something moves (live routing). Input still reaches the screen within one frame.

**Built in spec 14.1** (`docs/architecture.md`, Render Pipeline, Idle mode): `Engine.idle`, off by
default; a 3D view keeps the loop drawing until it rests (item 3, spec 14.38). A
frame is asked for by any window event, `engine_request_frame` from any thread,
`engine_request_frame_after` for a time, `engine_end_work` when a thread's result is in, and the
engine's own work. Headless, a run ends at the first frame after which nothing asks for another and
no work is out, and that frame is its screenshot, which is what `pcb render` wants.

**pcb's part:**
- set `idle`;
- call `engine_end_work` (or `engine_request_frame`) when a worker publishes a snapshot, with
  `engine_begin_work` called as the work starts, never frames ahead;
- while live routing runs, call `engine_request_frame` each frame;
- read a gamepad, if ever, the same way, since pads send no events.

**Left for later:** a gamepad that wakes the loop by itself. GLFW polls pads, and sends no event
for them.

---

## 3. Skipping the 3D scene pass — built

**What pcb needs**: when no 3D view is on screen, the 3D scene costs nothing, while everything stays
loaded so that coming back costs one frame.

**Built in spec 14.38**, with item 4 (`docs/architecture.md`, Scene views and Still views). A frame
draws the views the app has added, and with none it draws no scene pass and no post chain while
uploads, captures and the world's work go on, as the loading screen's frame did. A hidden view
(`SceneView.hidden`) keeps its targets and its picture: shown again with nothing it shows changed,
the picture is reused; changed, its first frame is a cut.

**A still 3D view drawn on demand.** A view with `accumulate_frames` (16 to 64) averages that many
jittered frames once nothing it shows has changed, then rests: not drawn, asking for no frame, its
picture copied into the window each frame. Idle mode waits once every shown view rests, and
headless the run ends there, which is `pcb render`'s finished picture. A view with it 0, the
default, draws every frame, as games do.
- **Accumulating, not counting TAA's frames**: it ends in a known number of frames, and is cleaner
  on thin traces and small text, where TAA's leftover shimmer shows. The last TAA picture is shown
  until the average holds 4 frames.
- **Knowing it changed**: the engine sees a view's camera, rectangle and settings (byte for byte),
  meshes added or uploaded and nodes moved, by the views sharing the moved node's mask bit. An app
  says a plain write the engine cannot see: `scene_mark_changed` for a material, a light or a scene
  setting, `engine_mark_changed` for an engine-wide setting.
- **What keeps it drawing**: the engine's own work until it finishes (loads, GI and probe captures,
  kept shadow tiles opening, auto-exposure adapting), and what never finishes (water, rain, fire,
  particles, wind, the sky's clock, the clouds' drift, a pose, an animated surface hook).

**pcb's part:**
- give each 3D pane's view `accumulate_frames` 32;
- call `scene_mark_changed` after writing a material, a light or a scene setting, and
  `engine_mark_changed` after an engine-wide one.

**Left for later:**
- **A still TAA-only view resting**, by counting frames (about 200), for a view that wants TAA's
  picture rather than the average.
- **Accumulating under TAAU**: an averaged upsampling resolve. A view asking for both keeps TAAU,
  said once.
- **A pose counts as animating**: the engine cannot see whether an animator runs it, so a skinned
  scene never rests. pcb shows none.
- **An app pass reading `time`** is not seen; the app says so with `scene_mark_changed`.

---

## 4. Drawing the 3D scene into part of the window — built

**What pcb needs**: the split showing the layout beside the 3D view. Since the chat is always
visible, the main area is narrower than the window, so in practice every 3D view needs this, not only
the split.

**Built in spec 14.38** (`docs/architecture.md`, Scene views). A `SceneView` is a camera, a
rectangle in window points (the UI's units), its own settings (the post chain's, the meter's, the
sample count, the render scale and supersampling, the background, the debug modes) and its own
targets, histories and chain. Any number are drawn each frame, in list order, and copied into their
rectangles over `Engine.window_color` before the overlays. The app makes every view; the engine
starts with none.

**Decided in its spec:**
- **Views are explicit everywhere.** Nothing reads "the camera": a pass drawing a view is handed it,
  world work takes the focus view (`engine_set_focus_view`, the first added by default), and a tool
  takes the view it steers. Captures are views.
- **Targets sized to the rectangle**: a changed rectangle rebuilds that view's targets alone and
  starts it as a cut. A still view converges again in its count of frames.
- **The sun's cascades and the per-frame lamp shadows are drawn per view**, fitted to its camera
  and honouring its mask. Kept lamp shadows stay shared.
- **One scene, a mask per node.** Every view shows the engine's current scene; a 32-bit mask on
  each node, inherited down the tree, lets views show different parts of it, a part preview
  included.
- **An exact background**: a view's background can be the display colour wanted, which the engine
  turns back through the view's tone curve on the CPU, so a 3D pane meets the window's colour with
  no seam.
- **The overlay still covers the whole window**, drawn after the views are composited.
- **Headless**: a window at the view's size with one view filling it, as `pcb render` makes; no new
  capture path.
- **An environment from bytes** (`load_hdr_environment_memory`), for an HDR built into pcb's
  binary.

**Left for later:**
- **Every view served equally by world work.** Budgeted caches (GI, probes, kept lamps, terrain
  pages and votes) ranked from the nearest view; the rain's cover and water's caustic window drawn
  per view, as the sun's shadow is. Wanted by split-screen or any two far-apart views with rain,
  water or budgeted lighting; each reaches the eye through the focus view, so the change is local.
- **A scene per view**: the world's once-a-frame work run for each scene shown. One scene with the
  mask covers pcb's panes.
- **Kept lamp shadows per hidden set**, one copy per distinct mask among the views. A view hiding a
  node in a kept lamp's reach is said once.
- **A see-through view** (item 1's transparent background): coverage carried through the chain.
- **A second window**: a Window object gathering the window's parts.
- **A dither per view** when the window is drawn through its picture (a CRT, or a pass after the
  tone map); it takes the focus view's settings.
- **Several views in the session file**, and a view picker in the debug GUI, which acts on the
  focus view.
- **Spreading views' draws over frames**, if a heavy board's frame asks for it.
- **Several views' draws made cheaper**: one set of detail levels per view, which each view
  re-chooses for every item every frame; per-frame lamp shadows drawn once per distinct mask,
  which moves pixels, since casters draw at the drawing view's levels; and the picture's format
  decided per view, so one view's pass after its tone map stops making every view's picture 16-bit
  and restarting every still view. A board pays little for any of them.

---

## 4a. Tags in 3D — built

**What pcb needs** (its spec 8.1, phase 8): a click choosing what is under the pointer, with an
outline on what is chosen, shared with the layout and the schematic; Ctrl+click the spotlight,
which lights a net or a part and fades the rest, and in 3D turns it see-through, so a lit net's
copper shows inside the board; the board's FR4 and mask at an opacity stepped by tenths, each
copper layer in the layout's colour below 100%; a pane's layers shown or hidden from a list; the
part the height check finds, marked. Each is a table write: no mesh is rebuilt. Picking stays
pcb's, on the CPU (item 1).

**Built in spec 14.39** (`docs/architecture.md`, Tags; `docs/shader-subsystems.md`, Tags). A vertex
carries its object (`Mesh.tags`, a net); a node carries its class (`SceneNode.tag_class`, a kind of
surface: a copper layer, the mask, the FR4) and, for a part, its object (`SceneNode.tag`), inherited
down its tree. A `SceneTags` table borrowed by the views says how each tag is drawn, the canvas's
fields less `raised`; each view's settings say how each class is drawn and how far what is not lit
fades. Choosing, the spotlight, see-through, a pane's layers and the layout's colours are table
and setting writes; a write that changes a drawing restarts the views drawing it, an outline's
restarts none.

**Decided in its spec:**
- **A tag says how it is drawn, not what it means**: colour, outline, dim, hidden, lit. Choosing
  and the height check's mark stay pcb's, as its layout already writes them.
- **One table, borrowed by every 3D view**; the class rows and the fade are per view.
- **A fading view fades everything not lit, untagged included**, so the FR4 and mask, which carry
  no net, fade with the rest.
- **What is see-through or faded casts no shadow.**
- **The outline shows through what hides it, fainter**, and is drawn after a still view's average,
  so choosing restarts nothing.
- **The class is the node's, not the vertex's**: pcb builds one mesh a class a cell already, and a
  shared part model's vertices cannot say which part they are.

**What pcb has to know:**
- **Keep tags dense, from one count.** The table is as long as the largest tag set, 2^22 at most;
  `BOARD_3D_PART`'s bit 23 on part tags would ask for 8 million rows. Number nets and parts from
  one count.
- **Classes start at 1**: class 0 is none, its row never read.
- **A triangle's three corners carry one object**: a surface a pass drops is collapsed per vertex,
  so a triangle spanning two would stretch. pcb's surfaces are per-net regions.
- **A colour is a matte paint**, metal and gloss off with it; a class's `flat` is the share drawn
  unlit, so copper below 100% reads in shadow. A clearcoat stays over the paint.
- **Under the moment-based OIT a lone see-through layer reads about a tenth dark**: the moments'
  bias on every translucent surface. A board's many layers are what the moments are for.
- **Untagged surfaces fade with a view's fade**: the FR4, mask and silk need no tag to.

**Left for later**, each with a need named:
- **Transmissive and late surfaces faded and painted**; they honour hidden alone. No board surface
  is either.
- **A halo round what is lit**, the canvas's `fade_room`; a distance on screen means little in 3D.
- **Kept lamp shadows and captures that honour a view's tags**: both take every surface as it is.
  pcb's studio keeps no lamp and captures nothing.
- **A smoother outline**: the mask is one sample a pixel, so the ring's inner edge is stepped at
  close range, and round a QFN's lands it follows each land. A mask at the view's samples would
  smooth it, at its memory.
- **A per-tag offset**, so a dragged part moves without its node; pcb moves the node.
- **An item's lanes decided on the CPU.** A view that ghosts draws every opaque item in both lanes,
  each dropping the other's surfaces in the vertex stage, and a hidden surface is vertex-shaded in
  every pass to be dropped. For an item not tagged by vertex the CPU could skip the lane, but only
  by keeping a second copy of `tags.glsl`'s rule; left until a profile of pcb's board says the
  vertex work matters.
- **The ring scissored** to the outlined surfaces' boxes: it searches the whole picture, on the
  frames it is drawn.
- **The flat share after ambient occlusion**, which is applied later and darkens a flat surface by
  its neighbour; the gates measure the flat share with occlusion off.

---

## 5. Text input and scrolling panes

**What pcb needs**: the chat, a scrolling history with an input line; and, wanted earlier in step
6, the command palette (Cmd+K, a few letters typed to filter a list of commands, each shown with its
key) and the `/` jump to any part or net by name, fuzzy, both a text field over a list.

**Built in specs 14.2 and 14.3** (`docs/ui.md`; typed input, the pointer and the clipboard in
`docs/architecture.md`, Input): typed keys and characters in one ordered list on `InputState`,
handed to the UI with the pointer by `ui_input_read`; the clipboard; the input script typing; UTF-8
throughout with one home in `text.c`; an atlas of ASCII, Latin-1, Greek and a board's symbols, with a
kerning table; wrapped labels and styled runs; the scrolling pane with virtual rows; the text field;
`max_size`; `ui_wants_keyboard`, which is when pcb's keys stand down; and `fuzzy_match`.
`apps/canvas --palette` and `--history` exercise them.

**Settled for pcb:**
- **Matching** is the matcher in cetra; the lists and their order stay pcb's.
- **Row layout** is rows of ordinary elements the app builds, with runs for the matched letters.
- **A chat message's format** is pcb's step 9, built from rows and runs: change lines, accept and
  reject, speakers and Markdown need nothing more from cetra.
- **Input methods**: finished characters arrive, from dead keys, Option combinations, the emoji
  picker and a Chinese or Japanese input method once a character is chosen.

**Not built, with the reason:**
- **Laying out only when something changed**: the heaviest screen's two layouts take well under the
  1 ms bar, so apps keep writing fields directly.
- **Clearing the keys pressed while a loading screen is up**: silent's loading screen takes the
  press that starts the game through the same poll, so clearing there would swallow it.

**Left for later:**

- **Showing text while an input method composes it**, and placing its window at the caret: GLFW 3.4
  reports neither.
- **Glyphs past the atlas** (Chinese, Japanese, emoji): stored and copied, drawn as a box.
- **Right-to-left text**, and caret moves by grapheme rather than codepoint.
- **Horizontal scrolling panes**; **selecting across messages**, each being its own field.
- **Runs in an editable field**, and runs that change the size.
- **More fonts**: Roboto-Bold, a pixel face and JetBrains Mono ship, and pcb builds its own into its
  binary (item 9).

---

## 6. The text editor widget — built

**What pcb needs** (`ARCH.md` section 8): pcb's code view, an editor of its own in C, borrowing
freely from rxi's lite (MIT) and vis (ISC). The widget goes in cetra. What stays in pcb: the
colouring, from pcb's own parser; the link that lands edits made in other views in the open buffer;
and undo, since typing shares the project's one undo timeline, which is pcb's.

**Built in spec 14.4** (`docs/ui.md`, The code editor and its text): `text_doc.c/h`, the text, an
array of lines kept apart from the editors showing it; `ui_editor.c/h`, the editor, the UI's ninth
element; `glyph_cache.c/h`, JetBrains Mono as bitmaps at the exact pixel size; and `ui_typing.c/h`,
the platform's editing conventions the field shares. Positions are bytes from the file's start.
Several carets, keys as vis's motions, text objects and operators under modes that fall to their
parents, wrapping, and only the rows in view laid out. No code was copied from lite or vis, only
their designs, so no notice of theirs is owed.

- **Two text cores, not one**: the field edits a flat buffer through stb_textedit, which suits a
  few thousand characters; the editor needs large files, the host's colours and the host's undo.
- **Not a piece table**: its wins are cheap undo and free snapshots, and undo is pcb's while pcb's
  reader copies the bytes anyway.

**pcb's seams:** `text_doc_set_on_action` reports each action; `text_doc_replace` and
`text_doc_set_text` land the host's edits; `text_doc_map` brings pcb's positions forward;
`text_doc_set_highlighter` colours a line at a time; `text_doc_set_ranges` takes errors and the
shared selection at the version they were read from; `keep_undo` off with pcb's undo bound in a
mode of its own; `ui_editor_commands` lists the editor's commands for pcb's command table.
`apps/canvas --editor` stands in for pcb through all of these.

**Left for later:**

- sideways scrolling;
- a regular-expression find (find matches plain text within a line);
- wide characters as two cells, and moving by grapheme (item 5 lists these too);
- bold and italic styles;
- a fallback face for what JetBrains Mono lacks;
- input methods;
- a keystroke at 100 carets in a 64 MB text, 2.25 ms. The line starts are worked out again from the
  first caret's line, and a tree of line lengths would make that a logarithm;
- Enter at 100 carets: about 6.5 ms at 10 MB and 42 ms at 64 MB, over the 1 ms bar. Each line break
  moves every line after it in the text's array and in each editor's rows, once per caret; built
  once an action instead, it would cost one pass. A question for whoever plans the next editor
  work, since the fix reaches across the text and every view.

---

## 6a. The editor's vim mode — built

**What pcb needs**: its code view is vim only -- `i` and Esc, the visual modes, `/` searching the
text. pcb's own keys go over it: Space for help in normal mode, `/` searching whatever view has the
focus, `:` its command line. So the mode is built in cetra, beside the standard one, for any app to
use, and pcb stacks a layer of its keys on it.

**Built in spec 14.5, with the language's keys in 14.9** (`docs/ui.md`, The vim mode):
`ui_editor_vim.c/h`, one per host shared by its editors, held to vim 9.2 itself by an oracle; the
editor's general seams it needed (layer modes, commands that decline, a used key taking its
character, three selection shapes, a pointer hook, undo steps of parts); and the hooks for keys that
need the language (`ui_editor_vim_set_lookup`, `_set_indent`, `_set_format`, see "Not among the
nine").

**Left for later:**

- named registers, macros, marks and the jump list (Ctrl+O, Ctrl+I);
- `:s` and ranges, replace mode `R`, `gv`, Ctrl+A and Ctrl+X, insert completion and tag objects;
- regular expressions in a search, as in item 6's find;
- a host's own key sequences, a `:map`, once pcb wants a command vim has no meaning for;
- the differences from vim spec 14.5 lists: `A` after an area ending inside a character of several
  bytes, `.` after `v$` and after a visual `A` or `I`, `p` of lines over a block of empty lines
  then `u`, `r` with Enter in visual mode, and a quote object growing a visual area.

---

## 7. Rendering at any scale, in true units — built

**Built in specs 13.57 and 13.58** (`docs/architecture.md`, The Typical Size;
`docs/shader-subsystems.md`, Cascade soft shadows): `Engine.typical_size` (`--typical-size 0.001`
for a board) scaling every tuning length, a camera's planes fitted each frame (`Camera.fit_clip`),
and cascade soft-shadow thresholds counted in texels. pcb's pictures render at true size through
them, and its 3D view will make its engine at a typical size of 0.001.

**A panel's shadow at a board's scale: built in spec 14.38**, phase 10 (`docs/shader-subsystems.md`,
Area-light shadows). A tabletop panel's per-frame shadow was a cube of 1024 texels a face, about
0.63 mm a texel at the board, and a package's shadow stepped every texel. A panel's shadow is now
one layer fitted to the view drawing it, about 0.3 mm a texel in the board's three-quarter view,
softened by the penumbra the panel's size casts.

**Left for later:**

- **A physical depth of field.** The CoC today is a look with no focal length or aperture. A
  thin-lens CoC from the camera's focal length and aperture would put a macro shot's thin focus where
  a real lens does, at any scale. Likely the next spec pcb wants, for close-ups.
- **The cascade penumbra estimate** is normalised by a depth range the padding dominates, so its
  softness depends on how a cascade was fitted, not on the light.
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

## 9. What the panes and the fonts need of the UI — built

**What pcb needs** (`ARCH.md` sections 8 and 9, pcb spec 6.1): a closed pane goes; a split puts a
pane beside the one it splits; Space m puts the messages under every pane and takes them away; a
code pane moves between files, back where it was in each; and the fonts are built into the binary.

**Built in spec 14.6** (`docs/ui.md`, Removing and moving elements and The code editor and its
text; fonts from bytes in Drawing and text): `ui_remove` and `ui_move`, from which a split, the
messages and a close are made; `ui_editor_set_doc`, an editor that keeps its place in every text it
has shown, so pcb keeps nothing per pane and file; `ui_editor_scroll_place`, `ui_editor_top` and
`ui_editor_doc`; `load_font_memory`, `create_glyph_cache_memory` and `cetra_embed_files` for fonts
built into the program. `apps/canvas --editor A --editor B` is a test bed, not pcb's panes.

**Left for later**: dividers dragged to resize, a pane filling the window, and the layout kept per
project are pcb's own, on these calls. A UI call that wraps an element in a new parent, carrying
its size, as the canvas app's `take_size` does by hand, waits until pcb's splits repeat it.

---

## Not among the nine, but on the way

- **Read-only texts, a close the app can refuse, and headless frames that wait for work** -- spec
  14.7, built, from pcb 6.1's reviews: `TextDoc.read_only` with `text_doc_set_on_refused` (the vim
  mode does what vim does with `'nomodifiable'`), `engine_set_close_hook`, and a headless frame
  under idle mode waiting for the work an app counts. `docs/ui.md` and `docs/architecture.md`.
- **Vim keys the host answers, sizes by share, and every timed wake** -- spec 14.9, built:
  `ui_editor_vim_set_lookup` (`gd`, `gD`, Ctrl+] and `K`), `_set_indent` (`=` and every new line's
  indent) and `_set_format` (`gq`), with vim's own `find_decl` for `gd` and `gD` when the host
  finds nothing; `UI_SHARE`, a size as a share of the parent, which a GROW weight could not give
  since it misses the ratio by the gap; and `engine_request_frame_after` keeping every time asked.
  One difference from vim is kept: `gd` under an operator is always characterwise. Completion
  (Ctrl+X Ctrl+O) is a spec of its own, later.
- **A panel takes keys** -- spec 14.10, built: `ui_set_on_key` on a panel, a scrolling pane, a label
  or a text field, handed every key and character, for pcb's schematic pane and every pane kind
  after it (`docs/ui.md`, Key hooks). Refused on a code editor and on the controls. A pane that
  should keep the focus through Esc takes Esc, and pcb maps nothing to the UI's `back`, which would
  let go of it whatever the hook did.
- **A pane's focus border** -- specs 14.11 and 14.12, built: a panel taking the keys is drawn
  focused, with the editor's border, and a border is laid over its fill by Porter-Duff over, so a
  pane pcb draws shows its focus as the code editor does. pcb gives its schematic panel the
  editor's corner radius so the two borders match.
- **A label's text inside its padding, and rows on the baseline** -- spec 14.13, built, for pcb's
  key hints: a label draws its text inside its padding, and `UI_ALIGN_BASELINE` lines a row's
  children up by their text. pcb's key boxes use centring, which the first fix makes work.
- **A line-number colour** -- spec 14.14, built, for pcb's theme file: `UITheme.line_numbers`, the
  code editor's line numbers in a colour of their own (`code.line-number`). Every other name in pcb's theme reaches something cetra already
  had, the focus rings included.
- **Whole-line ranges and an exact scroll** -- spec 14.15, built, for pcb 6.5's side-by-side
  comparison of two versions: `UIEditorStyle.whole_lines` for its red and green lines, and
  `ui_editor_top_line` and `ui_editor_scroll_line` to hold one side's scroll to the other's. Moving
  between the sides is pcb's own, on its pane keys, Ctrl+H and Ctrl+L.
- **Folds, filler rows and gutter marks** -- spec 14.16, built, for the same comparison to look as
  GitHub's split diff does: folds a host makes (`ui_editor_set_folds`), a closed one a row of its
  own that the vim mode's `z` keys and the standard mode's commands open and close, held to vim's
  manual folds by `ui-vim-folds`; filler rows above a line (`ui_editor_set_fillers`), so a line and
  its counterpart stand on the same row; and `UIEditorStyle.gutter` and `.sign` for a `-` or `+`
  and a stronger colour beside a changed line's number. The changed words within a line needed
  nothing new: a plain range over them on the layer that holds the line. Folds the person makes
  (`zf`, `zd`, `foldmethod`) and nested folds are left out: the host makes them.
- **No scroll bar without room for one** -- spec 14.17, built, from a stray dot in pcb 6.5's
  window: an editor or a scrolling pane sized to nothing, or narrower than a bar, draws no scroll
  bar and takes no press on one. pcb sizes the views a pane is not showing to nothing to keep their
  state, and a code editor kept that way beside a schematic drew its thumb as a dot in the gap
  before the pane. It stops the dot, not its cause, which the hide below removes.
- **A hidden element** -- spec 14.18, built, for pcb's panes: `ui_set_hidden`, an element taking no
  room and no gap, neither laid out, drawn nor eased, taking no press, wheel or key and holding no
  focus, and keeping everything it holds -- an editor's carets, scroll and wrapped rows among it --
  until it is shown again (`docs/ui.md`, Removing, moving and hiding elements). pcb would hide the
  views a pane is not showing, which it sizes to nothing today: a view sized to nothing is still
  laid out, drawn and pressed, so a hidden box took room from its padding and labels, a code editor
  beside a schematic drew its scroll bar as a dot, and the two sides of a compared code pane cannot
  have a gap between them.
- **Two editors row for row** -- spec 14.19, built, for pcb 6.5's compared pane lining its two
  sides up as GitHub's split diff does: `ui_editor_top_row` and `ui_editor_scroll_row`, the top as
  rows from the text's start, which pcb copies from the leading side to the other, a line and its
  rows landing a row off for each filler one side has; `ui_editor_set_gutter_digits`, so two texts
  of different lengths keep the same columns; and a filler under the last line, for a change at the
  end of a text with no line after it (`docs/ui.md`, The editor).
- **Exact rows, fillers read back and scrolls counted** -- spec 14.20, built, from five reviews of
  that pane in pcb 6.5's phase 13, each of something pcb worked round: `ui_editor_line_rows`
  answering from the count the editor keeps, so asking for every changed line each frame wraps
  nothing once settled; `ui_editor_fillers`, so pcb compares the fillers the editor holds rather
  than keeping a copy an edit leaves stale; `ui_editor_top_row` and `ui_editor_scroll_row`
  counting exact rows, so a side on a text just shown stands level at once, which removes 14.19's
  known limit; and `ui_editor_scrolls`, so a side leads when the person scrolled it rather than
  when its top moved (`docs/ui.md`, The editor).
- **Linked editors** -- spec 14.21, built, from a review of that pane finding pcb held its two
  sides together at the wrong depth, polling each frame: `ui_editor_link`, two editors held row for
  row inside cetra's layout, by the second of the pair placed, the one a scroll was last asked of
  leading, and a fold turned on either turned on the other at once. pcb drops its lead from
  scrolls compared across frames, its copy of the row between layouts, which drew a frame with the
  two apart whenever a layout moved the leader, and its mirror of the folds' open-states
  (`docs/ui.md`, Linked editors).
- **An edge one pixel wide** -- spec 14.23, built, from a faint line across the via in pcb 7.1's
  board cross-section, where its rows, one quad each, met: the UI's edge was a two-pixel ramp, so
  two quads meeting on a pixel boundary each half covered it, and on the window's multisampled
  target an edge part way into a pixel was counted twice. An edge is now one pixel, centred on it,
  and a fill is drawn a pixel past its rect so the ramp is its only coverage. pcb puts its rows on
  whole pixels; a shape drawn in pieces meeting part way into a pixel still shows a quarter at most
  of what is under it there (`docs/ui.md`, The draw list).
- **Dimming by light** -- spec 14.24, from pcb 7.3.1's spotlight, Altium's net highlight: a tag
  lit, and a view dimming every tagged shape not lit toward the paper it is drawn on, by a strength,
  with room in points round what is lit where it dims less. The tag's own dim darkened toward black,
  which on a schematic's white paper made the faded parts stand out (`docs/canvas.md`, Dimming by
  light).
- **Fading each shape** -- spec 14.25, a correction to 14.24 found in pcb 7.3.1: 14.24 faded the
  picture after it was drawn, by the tagged shapes under each pixel, so pcb's selection box over a
  faded part faded with it, and under the mask a hidden shape still covered what was lit. Each shape
  now fades as it is drawn; the mask hides it. Whether a lit shape should draw over every faded one
  is pcb's to decide.
- **A gutter sign colour** -- spec 14.26, built, from pcb 7.4's look, where an error was shown only
  by its underline: `UIEditorStyle.sign_colour`, a style's gutter sign in a colour of its own, so
  pcb marks a line with an error or a warning with a `>` in its underline's colour (`docs/ui.md`,
  The editor).
- **Eight range layers** -- spec 14.27, built, from pcb 7.4.1's look, where the rule pcb's checks
  take the code to could not be told from the lines round it: `TEXT_DOC_LAYERS` from 4 to 8, so pcb
  tints the rule's line on a layer of its own, the four it had all in use (`docs/ui.md`, The text).
- **Lines and convex shapes in the UI** -- spec 14.28, built, from pcb 7.4.2's board modal, which
  shows each impedance profile's curves against frequency and its trace drawn as a trapezoid with
  dimension marks: `ui_draw_line`, `ui_draw_polyline` and `ui_draw_convex`, in the UI's draw list
  and program under its one-pixel edge, so a modal's panels draw them in order with their fills.
  The canvas could not: it is an overlay under the UI's, so a modal covers it (`docs/ui.md`, The
  draw list).
- **A table in the UI** -- spec 14.30, built, from pcb 7.4.2's board modal, whose stackup rows laid
  their columns out by hand in a draw hook, unnamed and run together: `ui_table`, a tenth kind of
  element, argued as the closed list asks. Its columns are sized across every row in the four
  modes an element is, as CSS grid's tracks are. Its rows are read from the app's source as
  `NSTableView` reads them, measured once and again only where they change, so 100,000 rows cost
  their measures and none is an element. It has a header that stays and a cursor the keys and a
  press move. pcb's parts, nets and checks will be lists too (`docs/ui.md`, The table).
- **A baseline for a draw hook** -- spec 14.31, built, from pcb 7.4.2's board modal, whose graph
  of Z against frequency kept a whole line for the frequencies along its foot and read high in its
  room: `ui_line_ascent`, beside `ui_line_height`, where `ui_draw_text` stands a line below the top
  it is given, so a hook drawing its own words ends their row at the baseline (`docs/ui.md`, The
  draw list).
- **A table in a scrolling pane** -- spec 14.32, built, from pcb 7.4.2's board modal, which holds
  its stack's picture beside the layers' table and wants the two to scroll together, the picture
  not a column of the table: a table as tall as its rows in a `ui_scroll`, its header held at the
  pane's top, the rows drawn those the pane shows, its cursor's row shown by the pane, and
  `ui_table_header_rect` for what stands beside it; the wheel passing on from what has nothing to
  scroll. With it, from the same modal's review: `UIStyle.corners`, so a strip along a rounded
  box's foot rounds its bottom corners alone, and `ui_table_heights_changed`, rows sized to the
  room asked again with no cell measured (`docs/ui.md`, The table, Scrolling panes, The draw list).
- **A panel measured by its app** -- spec 14.33, from pcb 7.4.3, which lays its board modal's graph
  out with the UI. Each plot's values are painted by its hook, beside the grid lines they name, and
  its title stands over the plot, right of them. `ui_set_measure` gives the layout the values'
  width, as Yoga's measure function does a leaf's. The values were not made labels spread by a new
  alignment, which would put each tick's place in two hands (`docs/ui.md`, Layout, one axis at a
  time).
- **A double click for the app** -- spec 14.34, built, from pcb 7.5, whose layout picks a trace's
  segment by a click and the trace whole by a double click. `UIPresses`, `ui_presses_add` and
  `ui_presses_end` in `ui.h` count a run of presses, as the text field and the code editor did
  privately, so pcb keeps no copy of what a double click is (`docs/ui.md`, Focus, input and key
  routing).
- **A host's edit joined to the last step** -- spec 14.35, built, from pcb 7.5, which puts the
  person's theme file in order as its pane saves it and writes the ordered text back. As a step of
  its own, `u` took back the ordering alone and the next save made it again, so the edit before it
  could never be undone. `text_doc_set_text_joined` joins the latest step, as vim's `:undojoin`
  (`docs/ui.md`, The text, Undo).
- **Labels on copper** -- spec 14.36, built, from pcb 7.5.4, which writes a pad's number and net on
  it, a trace's net along it and a via's in it, once each reads: `CanvasText.shown_from` and
  `shown_until`, a text drawn only within a band of zoom judged by another em, so a pad's number is
  centred until its net reads; a non-zero tag past the table counted as a tag, so a shape tagged
  there fades as `canvas.h` says; and `CanvasLayer.keeps_colour`, so a label carries its copper's
  tags without taking its colour. With them, at the user's word, a ring by its nearest shape: one
  pass more while a ring shows takes the least inner ramp by min, so a picked trace's ring is one
  round a joint, where 14.0 left a tick in the inner corner, seen at 2x (`docs/canvas.md`).
- **A polygon of thousands of holes** -- spec 14.37, built, from pcb 7.6, whose ground planes hold
  a hole for every via and pad of another net. Earcut bridges each hole to the outline by walking
  the whole ring, which grows by every hole joined, so such a plane cost its holes times its points.
  From 32 holes each bridge is found through a grid over the ring's edges instead, inside
  `triangulate.c`, with no change to its API (`docs/canvas.md`, Triangulation). What is left of a
  fill's time is the ear clipping. Triangles supplied by the caller (item 1, Left for later) stay
  the fallback. With it, at the user's word, a fill triangle crossing the tile grid is cut at
  every tile it crosses. Before, a triangle that fitted in two tiles was drawn whole beside a
  neighbour that was cut, and a pixel whose centre fell on their shared edge was left unfilled: a
  few in a plane of thousands of holes (`docs/canvas.md`).
- **Navigation through the keys**, which lifts 14.10's refusal on the controls: the arrows, Enter
  and the pad reaching the focused element as keys, each claimed or declined, with only what is
  declined moving the focus or pressing -- as ImGui's key owners, Qt's and GTK's propagation and the
  responder chain do. A control could then take a key of its own and keep its navigation. Not
  needed by pcb yet; not yet a spec.
- **A second window**, for a second monitor. pcb lists it as an open question and as a cetra change;
  nothing is decided.
- **The physical depth of field** of item 7, likely wanted before pcb's 3D view is judged on its
  look.
