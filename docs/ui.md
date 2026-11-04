# The UI

A retained UI (`ui.h`, spec 12.2): a tree of elements, a box layout, a geometric focus model, keys
routed to the focus, a text field, a code editor with a vim mode, the text the editor edits, and
the text rendering under all of it. It began as the game's menus; the panes, the field and the
editor are there for pcb (`docs/eda-roadmap.md`).

The UI is not a Game subsystem: `create_ui_system` takes an `Engine`, draws as one of its overlays
(`ui_attach`), and takes its input as plain values, so it works with no game framework.
`docs/game-framework.md` says where a game puts the input pass and how a menu takes input away from
the game.

## The element tree and layout

**The element list is closed**: panel, label, button, toggle, slider, selector, the scrolling pane
(`ui_scroll`), the text field (`ui_text_field`) and the code editor (`ui_editor`). Three escape
hatches sit under it, each keeping strictly more than the last:
- the draw-list primitives keep nothing of the tree;
- `ui_set_element_program` keeps layout, focus and input and swaps only the fragment stage;
- `ui_set_draw` keeps layout, focus and input while the app paints, and a panel taking the keys
  keeps its focus border.

An app's fragment stage is built over the UI's own vertex stage by `create_ui_draw_program`
(`program.h`), so no app restates the UI's vertex layout.

**Everything is in window points**, top-left origin, +Y down: the space `text.c`'s ortho and GLFW's
cursor agree on, and deliberately not the engine's `InputState` space (framebuffer pixels;
`engine_fb_to_points` converts).

**`ui_layout` takes no GL, no clock and no input**, which is what lets the `ui` gate group assert a
menu with no GPU.

### Layout, one axis at a time

The layout measures and arranges one axis at a time: widths (measure, then arrange), then line
breaks, then heights. That order is what lets a label with `wrap` take its height from the width it
was given.

- **Every element with text keeps its lines** (`UILines`): a label with `wrap` broken at its width,
  any other at its `'\n'`s alone, which its height counts; with them its widest line and longest
  word, the measure its width takes. They are kept until its text or runs change or its face, size
  or tracking do, and a wrapping label's also until its width does. Every element draws its text
  through them, so a control's height and its text agree: a button's, a toggle's and a selector's
  rows centred in it, a slider's at its top with its track below.
- **A FIXED axis measures no content**, since its size is its own.
- **`max_size` is the most an element measures along each axis**, 0 for none, for FIT and GROW
  alike. A pane held at its most scrolls. A GROW child held at its most leaves its room to the
  siblings still growing, shared again round by round, and what is left after that is placed by
  `align_main` rather than left as a gap. Across the axis a stretched child is clamped before the
  cross alignment places it.
- **`UI_SHARE` is a size as a share of the parent** (spec 14.9), `size[axis]` from 0 to 1: of the
  inner length less the gaps between the children along the parent's main axis, of the inner
  length across it, rounded down to a whole point so the edge it puts down draws sharp. It is
  settled before GROW, so a split is a SHARE side and a GROW side and the second takes exactly the
  rest whatever it measures; a weight on GROW was rejected because it misses by the gap a nested
  split measures. A SHARE axis measures 0, so a FIT parent is the size of its other children and the
  share is of what that comes to.
- **`UI_ALIGN_BASELINE` lines a row's children up by their text** (spec 14.13), as a row's
  `align_cross`: each child with a baseline moves down until its first line stands on the deepest
  one's, and the row is as tall as its lowest child then reaches. A label's baseline is its top
  padding plus its face's ascent; a row on the baseline's is its line; any other panel's is its
  first child's, where that child stands in it. A control, a field, an editor, a scrolling pane, a
  column that centres or ends its children, an element with no text, and a child stretched across
  the row have none and stand at the top.
- **Each element keeps its baseline** (`UIElement.baseline`), set as the measure finishes its height,
  its children's first, so the arrange reads what the measure found. Asked live instead, a row asks
  its first child three times and nested rows cost three to the power of their depth.
- **A label draws its text inside its padding**, where its height and its breaks count it.
- **An element's `spacing` and `padding` of 0 mean the theme's**, and a negative one none.

### Theme

**Every zero in a theme means "inherit."** Styles are per state (`UI_STATE_NORMAL` to
`_DISABLED`). A panel taking the keys is drawn in `theme.panel`'s FOCUS look, by default the
editor's border, drawn over a draw hook's painting too, so a pane an app draws shows the keys are in
it as the code editor does. A focusable panel with no key hook is not (`_state_of`): a list's row,
which the focus follows the pointer over, would be outlined wherever the mouse is. A focused pane
takes its FOCUS look while the focus is anywhere inside it.

### Styled text

A label or a read-only field takes styled runs (`UITextRun`, `ui_set_text_runs`): a byte range with
its own colour and face, in the element's size and line. Runs out of order, overlapping, or
splitting a codepoint are refused by name, and setting the text clears them. `ui_set_text` on a
field is refused by name: a field's text is `ui_field_set_text` and its hint
`ui_field_set_placeholder`.

### Scrolling panes and virtual rows

- **A pane's rects stay window positions.** `ui_scroll`'s children are arranged at their full
  height from its top less its scroll, clipped in the emit, and hit only where it shows them.
- **A GROWING pane measures as its padding down the axis it scrolls.** Its FIT height is its
  content, so measured that way and asked of a parent it would be as tall as everything in it.
- **Virtual rows are built and freed inside `ui_layout`** (`ui_scroll_set_rows`): the rows in view,
  one either side, and the one holding the focus, from where the last layout left the scroll. So
  `ui_layout` runs again on what it built, three times at the most; a screen with no rows lays out
  once. The rows in view where the pane is easing to are built too, so a focus moved on faster than
  the scroll finds its rows.
- **A built row's height is taken at every layout** and kept for when it is not built. A height
  changing above the first row any of which shows moves the scroll with it -- not above the row at
  the scroll, which counts the gap after a row as that row's.
- **A built row is reached with `ui_scroll_find_row`**, NULL while it is not built and valid until
  the next layout, so an app changes one in place (appending to a streamed message) rather than
  rebuilding with `ui_scroll_rows_changed`.

### Removing and moving elements

`ui_remove(el)` takes an element out and `ui_move(el, parent, before)` moves it, which is all a
host's panes need: a split is a row put in front of the pane with the pane moved into it, a close
is a removal. `before` is the child of `parent` to go in front of, NULL for the end, as the DOM's
`insertBefore`; an index was rejected because it has to say whether it counts the element itself.

- **What the system or a screen held inside what goes is let go first** (`_forget`, which a pane's
  rows share): the pressed element, a held bar, the focus last seen, a screen's focus.
- **An element removed during a pass is taken out at once and freed only as the pass ends** -- by
  its own action, or by a key an editor took, as `:q` closes a pane. `ui_layout`, the input pass and
  the build count themselves in `UISystem.passes`, since the pass goes on reading what removed it
  (the hovered element's state, the element taking keys); freed at once, it reads freed memory.
  What waits is a list through the elements themselves (`removed_next`), so taking one out cannot
  fail for memory. A pane's dropped rows leave by the same path (`_leave`).
- **An element knows its screen through its root** (`UIElement.screen`, set on a screen's root
  alone), and one taken out has none. That is what `ui_remove` refuses a second time, `ui_move`
  refuses to move and `ui_set_focus` refuses to focus, with an element of another screen.
- **`ui_move` keeps everything the element holds**, focus included.
- Both refuse a screen's root and a pane's virtual row. `ui_move` also refuses a move out of or into
  a pane with rows or one of its rows (which the pane would free), into the element itself or under
  it, and to another screen.

## Focus, input and key routing

**`ui_input_read` fills a `UIInput` from the engine**: the pointer, its press and release and their
modifiers, the wheel, the clock, and the keys (`UIInput.key_events`, the engine's ordered list of
keys, repeats and characters; `docs/architecture.md`, Input). It leaves `nav_*`, `accept` and
`back` to the app's own bindings. No app gathers pointer state of its own.

**Focus**: a press takes a text field's focus and the pointer passing over does not; a press on
nothing in a screen that is not modal lets go. `ui_wants_keyboard` is what an app asks before
taking a key itself.

### Key routing

These rules live in `_route_keys` and are the easiest part of the UI to get wrong:

1. **A pass's keys go one at a time to the focus at their turn**: the focused element's key hook
   first, then a focused field or editor, and what neither uses -- Esc, Tab, a page key on one
   line -- to the screen. So a character after a Tab reaches where the Tab moved the focus, and the
   keys stop at a screen one of them closed.
2. **A key something used takes its press's character with it**: the character event straight after
   a used key that makes one is dropped, in one place, whatever holds the focus at its turn. A
   binding on Space types no space, and a split bound to `|` types no `|` into the pane it made. So
   a widget reports a key used only when it acted on it: the text field types a letter from its
   character and reports the letter's key as not used, or no field would type.
3. **An element the app focused takes none of that pass's keys**, which came before it had the
   focus. An app opens a palette from its own key callback and the key that asked is not typed into
   it.
4. **While what takes the keys holds the focus, `nav_*` and `accept` are ignored**, so an app
   mapping the arrows to navigation does not move the focus while typing.
5. **Esc closes a modal screen and lets go in one that is not.** An app that also maps Esc to `back`
   lets go of a pane whatever its hook does with the Esc key.

### Key hooks

`ui_set_on_key` (`UIElement.on_key`) lets a panel, a pane, a label or a field take the keys. A
pane an app draws -- pcb's schematic -- is handed every key and every character while it holds the
focus, before it acts on any, and takes the keys as a field does: the focus kept under a passing
pointer, `ui_wants_keyboard` true, and what the hook declines left to the screen, Tab and Esc among
them. It takes characters because a pane's keys are characters on any layout: `+` is Shift and `=`
on one keyboard and a key of its own on another. A field's hook is the same call, asked about a
character before the field types it.

- **A hook does not make an element `focusable`**, which a press needs.
- **A code editor refuses a hook**: its keys are its modes'.
- **A button, a toggle, a slider and a selector refuse one.** Navigation and accept reach the UI
  apart from the keys (`nav_*`, `accept`), so a hook could not hand them back, and a control holding
  the keys would be neither pressed nor left. Lifting this wants navigation to come through the
  keys, as ImGui's key owners, Qt's and GTK's propagation and the responder chain do.
- **The pointer's press, drag and release ask whether an element has the widget table**, not whether
  it takes the keys: a hooked panel has no field's press to call.

## Drawing and text

### The draw list (`ui_draw.c`)

The UI's own draw list: flat fills, quads (a whole picture, or a part of one with
`ui_draw_textured_quad_uv`, the whole-picture quad being that quad over 0..1), 9-slice, rounded
rects and SDF glyph runs, batched by (texture, program, clip) into one vertex buffer. It is drawn as
an engine overlay after tone mapping, so a menu is never graded, bloomed, grained or rescaled by
`--render-scale`, and is captured by a headless screenshot, which is what makes a menu golden
possible. The shader is `ui_vert/frag` (`docs/shaders.md`, Game UI).

- **It reuses `Font` and its pen from `text.c`, not `text.c`'s renderer**, which is one draw call per
  mesh, hardcodes its SDF smoothing and sets GL state rather than restoring it.
- **Every measure, break, caret and glyph comes from one walk** (`ui_text.h`, private: `UIText`,
  `ui_text_span_width`, `ui_text_pen`, `ui_text_break`, `ui_text_draw_span`), over UTF-8 and across
  styled runs, kerning stopping at a change of face, so the four cannot disagree.
- **`ui_lines_break` is the one line break**, kept while the width and the style are unchanged, for
  a label's lines and a multi-line field's rows alike: the layout runs several times a frame and
  must not break a text again each time. `ui_lines_break_from` breaks again from one line on, for a
  text changed from there (an edit from line 0, an append from its last), and drops the measures;
  `ui_lines_measure` keeps an element's widest line and longest word under the same face, size and
  tracking. Zeroed lines hold nothing, and at no width a line is found by its `'\n'` without a walk
  of its glyphs.
- **The draw list's offset moves the rounded-corner rect, the clips and `uRect` with the
  vertices**, which a slide transition and a scrolled pane need.
- `ui_draw_list_quad_count` and `ui_draw_list_quad` read the quads back for the probe.

### Fonts (`text.c`)

SDF text from stb_truetype, with glow and plasma effects on `text.c`'s own renderer.

- **A `Font` states its em** (`em_size`) **and its field's edge and falloff** (`sdf_edge`,
  `sdf_falloff`). The edge is half a code under stb's on-edge value, because stb truncates each
  texel to its byte; cut at the stated value every glyph is about 0.65% thin against stb's own
  rasterizer.
- **The atlas is built once at load** and holds printable ASCII, Latin-1, Greek and a short list of
  symbols (dashes, curly quotes, the ellipsis, arrows, the minus, Ω, ∞, ≈, ≠, ≤, ≥), less any the
  font lacks. Its UVs are divided only once the last glyph is placed. Every glyph's distance field
  is rasterised across threads before any is packed, and the packing stays serial, so the atlas is
  the same to the byte (157 ms to 33 for Roboto at 64).
- **The font keeps its kerning as a table** (`Font.kern_pairs`, indexed by each glyph's
  `pair_row`): every pair of its atlas's glyphs, each entry stb's own answer. A glyph's row is built
  in the same band as its field and only read after, so the canvas's edit threads read it safely.
  Asking stb at each step was three quarters of a text walk. A pair the atlas does not hold still
  asks stb, and a face with no kerning keeps no table.
- **The glyphs under U+0100 are found by codepoint without the hash** (`glyph_latin`).
- **A text layout steps a pen** (`font_pen_step` over a `FontPen`, spec 14.3): the one advance rule,
  each character's glyph looked up once and the kerning read from the glyph before and this one, so
  the UI's walk and the canvas's layout cannot space a string differently. A caller passes only
  characters, and a change of face starts the pen afresh. Its common case -- a character under
  U+0100 after one of the same face -- is a branch that makes no call, and the rare paths are
  `noinline`: inlined, they made every character save eight registers and a walk a fifth longer.
  `font_kern_advance` is for a single pair.
- **`font_advance` spaces a character the font does not hold**, and the UI draws it as an outlined
  box.
- **UTF-8 has one home here.** A codepoint is a `uint32_t` throughout cetra, stb_truetype's `int`
  met only inside this file: `text_utf8_next`, `text_utf8_prev` (its exact mirror, malformed bytes
  included), `text_utf8_encode` and `text_utf8_is_continuation`. What is no character -- a
  surrogate, a value past U+10FFFF -- is refused both ways, and decoding refuses overlong forms, the
  lead bytes C0, C1 and F5 to FF, and a sequence cut short: each such byte is one U+FFFD, so
  malformed text never swallows the character after it.
- **A font is made from a file or from its bytes** (`load_font_memory`, a font built into the
  program by `cetra_embed_files`), through one maker that keeps the buffer it is given, since
  stb_truetype reads it for as long as the font lives: a path hands it what it read, bytes a copy.
  The pool keeps either under its name, a path or the name given. The same bytes make the same
  font to the byte, atlas and kerning table included (`canvas-font-memory`).

### The editor's face (`glyph_cache.c`)

The code editor draws coverage bitmaps made at the exact pixel size shown, not the UI's distance
fields, which go soft at the 12 to 14 pixels code is read at, worst on a 1x screen.

- Glyphs are rasterised by stb_truetype as first asked for and packed in rows into one growing
  `GL_R8` atlas swizzled to white with the coverage as alpha, so the UI's textured quad draws a
  glyph in its tint with no shader of its own.
- A size is a pixel height, as a `Font`'s is. The cell is the advance of `M` rounded to whole
  pixels, and the line height and ascent are rounded too, so every glyph lands on the grid it was
  made for.
- A new size or screen scale empties the atlas, whose texture is cleared at the next glyph asked
  for, so a size can be set by the layout pass, which touches no GL.
- It is its own object rather than a `Font`, since `load_font` keeps one font per path whatever
  size is asked.
- Made from a file or from bytes built into the program (`create_glyph_cache_memory`), through one
  maker. The face shipped for it is JetBrains Mono (`assets/fonts/`, its OFL licence beside it).

## The text field

`ui_field.c` (spec 14.2) is ImGui's copy of stb_textedit, compiled as C from this one file behind a
block of five compatibility names (`ImS8`, `ImGuiDir_Left`/`_Right`, the `LastMoveDirectionLR` and
`Stb` members, `IM_ASSERT`), run on a growing UTF-8 buffer. A field is single-line, multi-line
(growing to `max_rows`, 0 for no most, then scrolling inside itself), or read-only with runs. Page
Up and Down page by the rows the field shows.

Its header is private. `ui.c` lays a field out, routes input to it and draws it through
`ui_field_ops`, the table it reaches the field and the code editor alike by (`ui_internal.h`), so
`ui.c` names neither kind except where it picks the table. The public calls are `ui_field_text`,
`_set_text`, `_set_placeholder`, `_append` and `_set_on_change` in `ui.h`; its key hook is any
element's. The placeholder is drawn dim while the field is empty, and never sizes it.

- **`ui_field_append` adds to the end without disturbing anything**: its runs counted from the start
  of what it adds, checked as `ui_set_text_runs` checks them, and refused on an editable field; the
  caret, the selection, the scroll and undo kept; no `on_change`; and only the last line broken
  again (`ui_lines_break_from`). That is what lets a streamed reply grow word by word while a
  selection in it holds.
- **Setting a field's runs breaks its rows again at once** (`ui_field_runs_changed`), so an append
  made before the next layout cannot keep rows broken in the old faces.
- **stb's widths are the UI's own walk** (`ui_text.h`), so the caret stands where the glyph is drawn,
  to the pixel snap. A glyph's width is walked from the glyph itself (its advance and its pair with
  the next), and a row's from the rows already broken: walked from the row's start, each width was a
  walk of the row, and stb asks for every width along a row to place a click.
- **stb's undo records say what undo does**: a typed insert is a record that deletes, its length in
  `delete_length`, and joining typing a word at a time reads that.
- **The keys are the platform's**: Cmd on macOS and Ctrl elsewhere for the shortcuts, Option or Ctrl
  for a word, Cmd+arrow to a line's or the text's end.
- **The blink is the wall clock's**, not the UI's `dt`, which idle mode keeps waits out of. The
  overlay sets `UISystem.now` and asks `engine_request_frame_after` for the caret's next change, so
  under idle mode the window wakes only for the blink. Headless the caret does not blink, so a
  picture repeats and an idle run ends.

**`ui_typing.c`** (private) holds the platform's editing conventions, which the text field and the
code editor both edit by, so the two cannot drift: word classes and word moves (to a word's end on
macOS, the next word's start elsewhere), the word round a point, a character stepped without reading
past its run, the paste cleaner, and the state of a run of presses (the double press) and of a
caret's blink.

## The code editor and its text

### The text (`text_doc.c`)

A `TextDoc` (spec 14.4) is kept apart from the editors showing it, as lite keeps its Doc and vis its
Text: two editors of one text each keep their own carets and scroll, and an edit through either, or
the host's, moves both. No GL and no UI. The editor's half -- views, `text_doc_apply`, the line
spans -- is the private `text_doc_edit.h`.

- **An array of lines**, lite's shape, each pointing into the bytes the text was made from until it
  is first edited, so a 10 MB file opens with one copy and one scan (2 ms) and typing touches one
  line. Line starts are made good lazily from the first line an edit touched.
- **Positions are byte offsets from the file's start**, a line break its one or two bytes and a
  byte-order mark three, which is what pcb's splices hold. A line keeps its ending (LF, CRLF, or
  none on the last) apart from its content, so the bytes come back exactly. A CR before an LF is the
  ending and any other is content, as pcb reads a file.
- **Every change is one replacement and `version` counts them.** An offset or a range held at an
  older version is brought forward through the last `TEXT_DOC_HISTORY` (4,096) changes
  (`text_doc_map`, `text_doc_set_ranges`): before a change it stays, after it moves by the length,
  inside the bytes replaced it goes to the end of the new ones, and a range keeps the new bytes
  outside it. That is how a host's colours from a parse a few keystrokes behind still land on the
  text they meant.
- **Every change goes through one path**: an action's, an undo's or the host's.
- **An action at several carets is applied from the highest start down**, so every change in it is
  in the action's starting coordinates, and `on_action` reports it so, with the selections before
  and after for a host's undo to restore. The editor making an action says where its selections
  land rather than having them moved through each change, since a selection ending where the next
  one's replacement begins would be pushed past that one's new text. The text is not changed from
  inside `on_action`; that is refused by name.
- **The host's edits** (`text_doc_replace`, and `text_doc_set_text`, which trims the unchanged ends
  to one replacement) are not reported back to it.
- **A text freed while an editor shows it** leaves the editor showing nothing.

**Undo.** The text keeps an undo of its own (`keep_undo`, on by default) for an app with no
timeline, typing joined into words as the field's is and a moved caret sealing a step; pcb turns
it off and binds the keys to its own.
- **A step is parts**, each an action's changes, undone last first and redone first first.
  `TextDocAction.step` is the serial the actions of one step share, so a host's timeline groups them
  as the text's own undo does.
- **The private `text_doc_group(doc, open, view)` holds one step open** across the actions of the
  editor `view` -- a vim command, an insert from `i` to Esc -- its `before` the view's selections as
  the group opened. `text_doc_empty_step` keeps a step that changes nothing, as vim keeps one for an
  operator over nothing.
- **A step is joined only by the action straight after its last**: each keeps the text's version
  after its last action, so typing after `keep_undo` was off, or after another editor's edit, starts
  a step of its own rather than adding bytes to one that no longer ends where they begin.
- **A view leaving a text** (`text_doc_remove_view`: an editor freed, or shown another text) closes a
  group held to it, keeping the step as far as it got, and seals the typing it was joining, since a
  view made later at its address must add to neither.

**Read-only** (`TextDoc.read_only`, a setting, spec 14.7). What a person does in an editor --
`text_doc_apply` with any edits, the editor's undo and redo -- is refused and told to `on_refused`
(`text_doc_set_on_refused`, with the editor), once an action. The host's edits, undo and redo go
through, which is how pcb appends its messages. The private `text_doc_refuse` tells a refusal the
text never saw (a vim command refused before it asked). In the standard mode a refused action
leaves the carets, and a cut copies before it deletes.

**Colour** comes two ways:
- a highlighter asked a line at a time (`text_doc_set_highlighter`), its spans kept until the line
  changes in a ring of the last 65,536 written. A line whose spans were written over colours itself
  again when drawn, so letting colour go never walks the lines. `stateful` is for a language whose
  lines begin inside something an earlier one opened;
- the host's ranges, in four layers.

### The editor (`ui_editor.c`)

The code editor (spec 14.4) is a UI element (`ui_editor`, in `ui.h`) on a `TextDoc` it borrows,
drawn in the theme's `editor_face`. Its line numbers are `theme.line_numbers` (spec 14.14), one
colour for every state; while that colour's alpha is 0 they are the editor's text at 38%.

- **Its lines wrap at its width**: after the last space that fits, at the edge for a word longer
  than a row, later rows hung at the line's indent. Only the rows in view are wrapped and drawn;
  every other line's rows are a guess from its length.
- **The scroll is held to a line and a row of it**, so a guess put right, or an edit above, never
  moves what is on screen. `ui_editor_top_line` reads it exactly, the line and the rows into it with
  the part of a row, and `ui_editor_scroll_line` puts it there, held to the text's lines and that
  line's rows as a wheel's scroll is: one editor held to another glides with it (spec 14.15).
- **A line edited within itself is wrapped again at once**, so typing moves no other line's rows.
- **A budget in each draw** (192 KB a frame) wraps the guessed rows -- the lines an edit rewrote
  first, then any others round the text -- and walks a stateful highlighter on. Its draw returns a
  wake of now while either has work, a drawn line came back uncoloured or the atlas grew mid-draw,
  which idle mode turns into the frames that finish them. At 10 MB a keystroke costs 0.11 ms, 0.50
  at 100 carets; Enter 0.20 at one caret and 6.5 at 100; a jump 0.12.
- **Its geometry is settled by the layout pass**, and its scroll bar is the pane's, driven through
  the editor's scroll calls.
- **Several carets from the start**: the selections are a sorted list, merged where they meet, the
  last made the primary.
- **Find** is `ui_editor_find`, its matches lit in view.
- **A range whose style has `whole_lines`** (spec 14.15) fills every row of each line it reaches,
  its line break counted, from the text's left edge to the editor's right, an empty line included.
  It is drawn under every other background, so a selection shows over it; its text colour and
  underline still go by its bytes.
- **A style's `gutter` and `sign`** (spec 14.16) mark the lines its ranges reach, by the same rule:
  `gutter` fills behind the gutter on each of the line's rows, under the number, and `sign` (a
  codepoint) is drawn in the gap after the number on its first row, in the text's plain colour.
  pcb's diff marks a changed line with `-` or `+` and a stronger red or green this way, through the
  ranges it already keeps.

**Rows the text does not make** (spec 14.16). Every walk of the rows -- the scroll, a row move, a
click, the draw -- counts the rows a line *shows*, which may differ from the rows it wraps to.
- **Folds** (`ui_editor_set_folds`) are runs of lines a host makes, none overlapping or nested. A
  closed one is one row: `theme.editor_fold` (alpha 0: the selection colour at a third) from the
  gutter to the editor's right, its label or "N lines" in the line numbers' colour, no number, the
  lines after it keeping theirs. A fold of one line shows its line, closed or not, as vim's
  `'foldminlines'` has it.
  - A caret may stand on any of a closed fold's lines and is drawn at the row's start. A row move
    and a line move take the fold as one; Home, End and the row's deletes act on the caret's own
    line.
  - A click on the fold's row opens it, and so does any edit at a caret in it (typing, pasting, a
    delete, an indent), unless the text is read-only and nothing will change.
  - Reveals and scrolls to an offset or line in it go to its row, and opening one by a command
    brings the caret into view. The ranges it hides wholly are not among the ones drawn.
  - The standard mode has `open-fold`, `close-fold` and `toggle-fold`, bound to no key.
- **Filler rows** (`ui_editor_set_fillers`) are blank rows above a line, `theme.editor_filler`
  (alpha 0: the text's colour at 4%) from the text's left edge, with no number, text or range. A row
  of a line counts from its first filler, so `ui_editor_top_line` and `ui_editor_scroll_line` count
  them; no caret stands on one, and a click on one lands at its line's start. A closed fold shows
  its first line's fillers above its row and none of the lines it hides. `ui_editor_line_rows` is a
  line's own rows at the editor's columns, which a host padding two editors to each other compares,
  polling `ui_editor_columns` for when they change.
- **Both belong to the editor's place in its text**, not to the text: two editors on one text fold
  it apart, as two vim windows do, and a parked place keeps them. Through an edit each moves with
  its lines: a line kept is the one whose start the change left a line's start, a line put in among
  a fold's lines joins it, and a fold or a filler whose lines are all taken out goes.

What `ui.c` calls on it is `ui_editor_ops` (`ui_internal.h`); what the vim mode reaches of it is
the private `ui_editor_internal.h` (an edit at every caret as one action, carets with wanted columns,
the rows in view and the scroll, the closed folds); what a test reads of it is the private
`ui_editor_hooks.h`.

**One editor, many texts.** `ui_editor_set_doc` shows an editor another text, and the editor keeps
its place in every text it has shown, as a vim window keeps a buffer's.
- A text new to it starts as a new editor would: one caret at 0, the view at the top, every line's
  rows a guess. One shown again comes back with its carets, their wanted columns, its view and its
  rows as it left them (`UIEditorPlace`, everything of an editor that belongs to the text it shows).
- A place left is parked on its text as a view of it, so every edit made there since moves it as it
  moves an editor showing the text, the changed lines' rows made guesses. Rows wrapped at other
  columns are guesses again on return. A text freed lets its parked places go.
- The mode, find's query and the look are the editor's and are kept. The text it leaves forgets it
  as a view, and an undo group held to it is closed.
- Keeping the place on the host (carets and a top with a version, mapped through `text_doc_map` on
  return) was rejected: every host would have written it.
- **A place asked for before the editor is laid out on its text is held for that layout**
  (`ui_editor_scroll_place`, `ui_editor_scroll_line`, `ui_editor_reveal`), since only then are its
  rows and its height known: a pane made by a split is placed where the one it split was at its
  first layout.
- `ui_editor(parent, NULL)` makes an editor showing nothing, and `ui_editor_doc` says what one shows,
  so a host finds a pane's file from the pane.
- **Each text shown gives the editor a new serial** (`ui_editor_serial`, private), never reused,
  which is how the vim mode tells another editor from the last it saw.

### Keys and modes

Keys are vis's three pieces, each applied at every caret: a **motion** (where a caret goes,
`ui_editor_motion`), a **text object** (the range round it -- a word, a line, the bracketed list;
`ui_editor_object`), an **operator** (move, extend, select, delete, cut, copy, paste, indent;
`ui_editor_apply`).

A **mode** turns keys into them: a table of bindings to commands (named and described;
`ui_editor_commands` lists the editor's own for a host's command table), a reader of its own for
sequences like `d2w`, whether it types, and its caret's shape. A key a mode does not take falls to
its parent. The standard mode (`ui_editor_standard_mode`) is the platform's keys, through
`ui_typing.c`; the vim mode is another; the `ui-editor-mode` arm builds a toy mode from the public
calls alone. A host binds its own commands -- pcb's undo, its view keys -- in a mode whose parent is
either.

- **A mode may be a layer** (`UIEditorMode.layer`): keys only, its typing, caret, selection shape and
  pointer hook being those of the first mode up its chain that is not a layer, so a host's keys over
  the vim mode leave vim's state showing. `ui_editor_mode` still returns the mode set.
- **A command that declines hands its key to the parent**, and only with no parent left to the
  screen, so a host's Space can act in normal mode and hand the key on in insert mode.
- **A key a mode used takes its press's character with it** (rule 2 of key routing), so a binding on
  a printable key -- every letter of vim's normal mode -- types nothing.
- **A mode's `selection` shape only draws its bands** -- the span, inclusive of the far character
  (visual mode), or whole lines (visual line mode) -- while the mode decides what a command takes.
  Its `pointer` hook is called after a press and a drag.

## The vim mode

`ui_editor_vim.c` (spec 14.5) is the code editor's vim mode, held to vim 9.2 itself. The
`ui-vim-oracle` arm runs a corpus of 7,056 cases and `ui-vim-random` 1,600 seeded sequences of whole
commands through the mode and through headless vim fed the same keys as typed, text and cursor
compared byte for byte, CRLF, tabs and letters past ASCII among them; both skip without vim on the
path. Its rules are vim's as measured, not as remembered (an operator resetting the wanted column, a
block yank padding a short line with spaces, a `.` stopping where the change it repeats fails), and
where a case is unclear vim's own source decides. The differences left are listed in spec 14.5.

**One `UIEditorVim` per host, shared by its editors**, as vim's state is the person's and not the
buffer's: the mode, a half-typed command, the counts, the register, the last search, the last
`f`/`t`, the last change for `.`. The selections stay each editor's own.
- Keys reaching another editor than the last let go of whatever was unfinished in that one (a
  command, a line, an insert, a visual area), its undo group forgotten rather than closed, since
  that editor and its text may be gone. The text keeps a group to the editor that opened it and
  closes it when that editor leaves.
- **"Another editor" is compared by the editor's serial, never its address**: an editor freed and
  another made where it was (which the allocator does), or one shown another text, would inherit a
  half-typed operator.

**For a host**: the base mode is `ui_editor_vim_mode`, under a host's layer if it has one;
`ui_editor_vim_state` is NORMAL, INSERT, VISUAL, V-LINE or V-BLOCK, and `_pending` the keys of an
unfinished command, for a status line; `ui_editor_vim_keys` lists every key with a line of
description, for a help screen.

**What it does.** Keys are read from characters (so `$` and `%` come as typed on any layout) and
from key events (Esc, Enter, the arrows, the Ctrl keys). Normal, insert and the three visual modes:
counts before and inside an operator, the motions, the operators over every motion and text object
(words, brackets, quotes, paragraphs), the edits, `.`, a block's `I` and `A` at a caret on every
line, search (`/ ? n N * #`, plain text, ignoring case unless a capital is typed, matches lit through
`ui_editor_find` until `:noh`), and `:` (a line number, `$`, `noh`; anything else returns false and
is the host's -- pcb's `:w`). A keystroke at 10 MB costs 0.12 to 0.25 ms. Not supported: named
registers, macros, marks, the jump list, `:s` and ranges, `R`, `gv`, Ctrl+A and Ctrl+X, completion,
tag objects.

**Over the host's folds it is vim over manual folds** (spec 14.16), held to vim by `ui-vim-folds`,
each case's folds made by `:A,Bfold` in vim and `ui_editor_set_folds` in the mode.
- `zo`, `zO`, `zv`, `zc`, `za`, `zR` and `zM` open and close them; the person makes none.
- `j`, `k`, `+`, `-`, `_`, insert mode's arrows and the doubled operators count a closed fold as
  one line, from its last line going down and onto its first going up; `G` and `gg` land inside
  one without opening it.
- An operator takes a closed fold at either end whole, so `x` and `dd` there delete the fold; it
  starts at the fold's first line, where undo puts the cursor back. A visual or visual line area
  takes a fold whole where it begins, as vim's does when visual mode starts; a block does not.
- Word motions, `{` `}` and searches start from a closed fold's edge, never finding a match inside
  the one the cursor is in.
- Vim's default `'foldopen'` opens the fold a cursor lands in after a move along the line, `%` from
  another line, `{` `}`, a search and an undo; entering insert mode opens it too. `o`, `O` and a
  linewise put go below a closed fold's last line or above its first.
- `gj` and `gk` take a fold as one row, as the editor's row moves do, but keep no column a `j`
  before them wanted, and at the text's first and last rows go to its start and end where vim's do
  nothing, folds or none. `b` from a closed fold at the text's start moves nowhere, where vim's
  moves to the fold's start though it fails.

**Undo goes by vim's units**: a command is one step and so is an insert from `i` to Esc, held by
`text_doc_group`. A host keeping its own timeline takes `u` and Ctrl+R through
`ui_editor_vim_set_undo`.

Easy to get backwards:
- **The register is the system clipboard**, vim's `unnamedplus`, with vim's memory of whether it put
  whole lines or a block there, which holds only while the clipboard still holds that text: what
  another app copied pastes as characters.
- **The line for `/ ? :` is the host's when it sets a prompt** (`ui_editor_vim_set_prompt`), answered
  through `ui_editor_vim_search` or `_command`, NULL letting it go, and lit as it is typed through
  `ui_editor_vim_light`, which takes case as the search will. With no prompt the mode reads the line
  from the keys itself and shows it through `_pending`, so any host has search with no code.
- **Esc in normal mode is always taken**: declined, it reached the screen, which let go of the
  editor, and vim users press it out of habit.

**Keys that need the language ask the host** (spec 14.9), as vim asks its `tagfunc`, `keywordprg`,
`indentexpr` and `formatexpr`, rather than through a `:map` of the host's own.
- `ui_editor_vim_set_lookup` answers `gd`, `gD`, Ctrl+] and `K` in normal mode with no operator, and
  Ctrl+] and `K` on a one-line visual area, which vim's `get_visual_text` alone accepts. The host
  reads the name from the editor and goes where it likes. With no host, or one finding nothing,
  `gd` and `gD` are vim's `find_decl` (the identifier's first use in the block or the text, a use on
  a comment line or in a C comment or quotes passed over, none on the cursor's line) and Ctrl+] and
  `K` do nothing. Under an operator `gd` is always characterwise exclusive, where vim's takes the
  kind of the motion before it: the one difference kept.
- `ui_editor_vim_set_indent` answers `=` for a whole run of lines at once -- a column each, worked
  down the run as vim's `indentexpr` is asked a line at a time, so `=` is one edit however many
  lines it takes -- and, as a run of one, the indent of every line `o`, `O` or Enter makes and of
  every change of whole lines (vim's `fix_indent`).
- `ui_editor_vim_set_format` answers `gq` with the lines' new text.
- With no hook `=` and `gq` do nothing. `ui-vim-layout-oracle` holds both to vim sourcing
  `scripts/vim_layout.vim`, writable and read-only, the same two rules written in C in the probe.

**On a read-only text it is vim with `'nomodifiable'`** (spec 14.7), held to vim by
`ui-vim-read-only-oracle`, the whole corpus run read-only.
- A command that would change the text is refused once, through `_refused`, before anything of it is
  done: the register untouched, an operator's carets at its start, a visual area let go. Only where
  it would do something: a `J` on the last line, an `r` past the line's end and a `~` on an empty
  line are beeped at, as vim beeps.
- **A refused command is what `.` repeats, as vim keeps it, and its caller keeps it**: the functions
  that refuse report it as made, so the keep is the one the writable path makes (a visual put's
  delete under it, a `3s`'s count), and a change takes the Esc its insert would have ended with.
- A refused `r` is kept for nothing, and normal mode's `i a I A o O` stay in normal mode and are not
  kept. A visual or block `I` or `A` begins its insert, where each character is refused -- except a
  block `A` whose top line is short of the block, which vim must pad first and so refuses there.
- A block insert that puts nothing in, read-only or not, ends as any insert does, not at the block's
  corner, keeping the top line's padding.

## Fuzzy matching

`fuzzy_match` (`fuzzy.c`, spec 14.2) says whether a query's characters appear in a name in order,
with a score and the byte offset of each matched character, in a `FuzzyPositions` that says how many
it wrote. It follows fzf's second algorithm: the best placement, by a dynamic program over a 32 by
256 table on the stack (`FUZZY_QUERY_MAX`, `FUZZY_NAME_MAX`), not the first placement found. A
word's start, a turn into upper case, a run and the name's start score higher; gaps cost; ASCII
letters match either case.

It searches nothing: the app loops over its own list, sorts by the score and its own tie-break, and
lights the positions with runs. No GL, no UI, no allocation.
