# canvas

The 2D canvas's own app (`docs/canvas.md`), and the test bed for everything else cetra carries for
pcb: idle mode, the UI's text input and scrolling panes, the code editor and its vim mode, panes
split and closed at run time, and the window close an app may refuse. Nothing in it sketches pcb's
interface; each mode is there to drive one engine feature the way pcb will, and to print what a
gate arm measures.

## Running it

```bash
./out/bin/canvas                         # the shapes sheet
./out/bin/canvas --sheet board --split   # the two-layer board, top and underside
./out/bin/canvas --editor FILE --vim     # the code editor in vim mode
./out/bin/canvas --sheet board -x -S /tmp/board.ppm
```

It takes the capture set `-x`, `-f`, `-S`, `-W` and `-H`. In a window a drag pans and the wheel
zooms the view under the cursor; = and - zoom, 0 resets the view and P prints its centre. G opens the
debug GUI and F the FPS readout, which under `--idle` counts the frames drawn. The app's own keys
stand down while the UI wants the keyboard (`ui_wants_keyboard`): a field or the editor has the
focus, or the palette is up.

Every flag, and what each probe prints, is in `docs/cli-reference.md`, "The other apps" (the
`apps/canvas` entry).

## The test sheets

`--sheet NAME` picks what is drawn. Each is laid out in millimetres and drawn in nanometres, pcb's
units: `shapes` (the default), `areas`, `polygons`, `ramp`, `thin`, `long`, `long-short`,
`glyphs`, `tags`, `overlap`, a `symbol` of an integrated circuit with turned pin names, a
two-layer `board` with a ground plane, a highlighted net, a picked pad and a removed trace, and
`strokes` and `dashes`, a cell a cap, join or dash pattern, each with its area in closed form, and
`fade`, four boxes on white paper, one lit, and over them an untagged box on a faded one and a
faded box on a lit one, for a view that fades.

- `--fade STRENGTH,ROOM` makes the view fade (`docs/canvas.md`, Dimming by light): its tagged
  shapes not lit fade toward its sheet's paper by STRENGTH, less within ROOM points of a lit one; a
  STRENGTH of 1 hides them.

- `--dash LENGTH,GAP` (mm), `--cap round|butt|square` and `--join round|mitre|bevel` stroke every
  paint a sheet makes so; what a sheet sets on a paint itself stands. `--sheet symbol --dash
  0.5,0.5 --cap butt --join mitre` draws the symbol as pcb's ghost would. `--dash-min-px PX` sets
  the size under which a dash pattern is drawn as its average.

- `--view`, `--offset`, `--mirror` and `--tile-shift` move, mirror and re-tile what a sheet draws:
  `--offset 1099511627776` puts the whole sheet 2^40 units from the origin, which is what
  `canvas-translation` holds to the same pixels.
- `--split` draws the board as two views, the second the underside, mirrored; `--only-view` draws
  one of them alone.
- `--edit-thread` builds the board's edit on a thread of its own.
- `--canvas-probe` prints where each shape landed, in framebuffer pixels, and `--budget` times the
  canvas draws. The `canvas` gate group reads both.

## Idle mode

`--idle` runs the app in the engine's idle mode (`docs/architecture.md`, Render Pipeline): a frame
is drawn only when something asks. Its cases are the `idle` gate group's -- timed wakes
(`--idle-wake`), the board filled on a thread started mid-run (`--edit-thread`, with
`--work-delay` for the thread's pause), a texture loaded late (`--idle-load`), the UI panel fading
in (`--ui-enter`) and a click handed to the debug GUI (`--gui-click`). Headless, the run ends at the
first frame after which nothing asks for another.

## UI text and panes

- `--ui` puts a panel of the game UI over the canvas; `--ui-text` sets its label, which is how the
  glyph arms draw characters past ASCII and one the atlas lacks. `--ui-keys` makes the panel
  focusable with a key hook, as a pane pcb draws itself would have, and shows the last two things
  it was handed.
- `--palette` is a modal command palette over the app's own commands: Cmd+K (Ctrl+K off macOS)
  opens it, typing filters and orders the commands by `fuzzy_match` with the matched letters lit,
  and `/` opens a jump over 5,000 generated names, one list of virtual rows over every match. The
  list is as tall as its rows up to twelve, then scrolls, by its `max_size`.
- `--history` is a column of 200 messages as virtual rows of read-only fields, some in a second
  face (PressStart2P, a pixel font, so it is blocky on purpose), over a multi-line input line that
  submits on Enter. Enter in a view puts the caret in the input line and Esc returns. A sent message is answered by a reply streamed in a word at a time with `ui_field_append`, so a
  selection in it holds.
- `--ui-borders` draws boxes whose borders lie over clear, half-alpha and opaque fills, which
  `canvas-ui-borders` holds to Porter-Duff over.
- `--ui-seams` draws quads meeting edge to edge on whole pixels and part way into them, a quad on
  whole pixels, one whose edge halves a column of pixels and a rounded box, which
  `canvas-ui-seams` holds to a one-pixel edge.

The app reads the UI's input through `ui_input_read` alone and keeps no pointer state of its own.

## The code editor

- `--font-sheet` draws JetBrains Mono at 11, 13 and 16 points from the editor's bitmaps beside the
  same face as the UI's distance field.
- `--editor FILE` opens a file in the editor over the whole window, standing in for pcb: an
  S-expression highlighter on pcb's lexical rules, and a parse on a thread whose errors come back a
  few versions behind and are underlined where the typing since moved them. The result is taken on
  the frame the thread's `engine_end_work` wakes; `--editor-lag N` takes it N frames after it began
  instead, so a headless run without idle mode repeats. F6 lands an edit as though from another
  view, and F7 reads the file again.
- `--vim` puts the editor in the vim mode, with the app's keys a layer over it and a line under the
  editor that shows vim's state and takes a search or a `:` line as the host's prompt.
  `--vim-lookup` answers `gd`, `gD`, Ctrl+] and `K` as a host would, and `--vim-layout` answers `=`,
  `gq` and a new line's indent.
- `--read-only` makes every file's text read-only, each refusal counted and said on the vim line.
- `--editor-fold FIRST,LAST` folds the first pane's lines FIRST to LAST, as numbered, closed: the
  test bed for the editor's folds. `--editor-filler LINE,ROWS` puts ROWS blank filler rows above
  line LINE of the first pane.
- `--editor` may be given up to eight times, and the editors are panes, the test bed for
  `ui_remove`, `ui_move` and `ui_editor_set_doc`: F8 splits the focused pane, F9 closes it from its
  own key, and F10 shows the next file in it where that pane last was. `--split-share F` gives the
  split's first side a share of its row. The panes are the UI's tree itself: the app keeps no table
  of them, and finds a pane's file by the text it shows.

`--input-script PATH` replays a scripted pointer and typing in place of the mouse and keyboard,
which is how a headless run opens the palette and types into it, and how `ui-vim-script` drives
the vim mode. `--refuse-close` refuses the
first close the window is asked for and takes the next, through the engine's close hook.

## Fonts, and what else it checks

Its three fonts are built into the program with `cetra_embed_files`, so it reads no font from
beside it; under `--font-sheet --canvas-probe` it holds each to the file it was built from. It is
also a debug harness for `stb_truetype.h`, which the glyph arms check against -- the coverage, the
kerning of every pair, and every glyph's field in the atlas -- and for cimgui, through which
`--gui-click` reaches ImGui.
