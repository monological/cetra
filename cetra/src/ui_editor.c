/*
 * The code editor (spec 14.4): a TextDoc's lines wrapped at the editor's width, drawn in a
 * monospaced face from bitmaps made at the size shown, with any number of carets.
 *
 * Every row is a run of cells, so a character's place is its column times the cell's width and
 * nothing is measured. Only the rows in view are wrapped and drawn. Every other line's row count is
 * a guess from its length until it is wrapped, and the scroll is held to a line and a row of it, so
 * a guess put right never moves what is on screen.
 */

#include "ui_editor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <GLFW/glfw3.h>

#include "engine.h"
#include "ext/log.h"
#include "glyph_cache.h"
#include "text.h"
#include "text_doc_edit.h"
#include "ui_editor_hooks.h"
#include "ui_editor_internal.h"
#include "ui_internal.h"
#include "ui_typing.h"
#include "util.h"

// Columns between the line numbers and the text.
#define GUTTER_GAP 2
// The fewest columns the line numbers take, so the gutter does not widen at line 10 and 100.
#define GUTTER_DIGITS 3
// The most digits a gutter is asked for: a line count's, at the most.
#define GUTTER_MOST 20
// Rows a unit of the wheel scrolls.
#define WHEEL_ROWS 3.0f
// The face's size when the theme names none, in points.
#define EDITOR_SIZE 13.0f
// The most columns between tab stops, and as many spaces for indenting by one.
#define TAB_MAX 32
static const char SPACES[TAB_MAX + 1] = "                                ";
// What the budget does a frame, past what is drawn: the bytes of guessed lines it wraps, and the
// lines a stateful highlighter's walk colours. Counted rather than timed, so a headless run
// repeats; sized from the bench to stay under a millisecond.
#define SETTLE_BYTES  (192 * 1024)
#define SETTLE_LINES  2000
#define SETTLE_VISITS (256 * 1024) // the most lines a frame's walk looks at, wrapped or not
// Lines an edit rewrote that the budget wraps before it walks the text for others; past this
// many it walks.
#define SETTLE_HINTS 1024
// The host's ranges a frame draws, from each layer.
#define RANGES_DRAWN 128

typedef struct UIEditor UIEditor;

/*
 * Where an editor is in one text: everything it keeps that belongs to the text it shows. Held in
 * the editor while it shows the text; PARKED on the text when the editor shows another, as a view
 * of its own, so the text moves its carets, its rows and its top through every change as it moves
 * an editor's, and the editor comes back to it exactly where it was, as a vim window comes back to
 * a buffer.
 */
typedef struct UIEditorPlace {
    // The selections, sorted by start, and each one's wanted column for moving by row, -1 for none.
    TextSelection* sel;
    int* want;
    size_t sel_count, sel_capacity, primary;
    // Each line's rows at the editor's columns, 0 while not wrapped, and the rows above each line,
    // good up to tops_good, and the tab stops the rows were wrapped at. Above `exact` no line that
    // shows is a guess.
    uint32_t* rows;
    uint64_t* tops;
    size_t line_capacity, tops_capacity, tops_good;
    size_t exact;
    int tab;
    // The folds, in order of their lines and none overlapping, each label the place's own copy; and
    // the fillers, in order of their lines, one at most a line, none of no rows.
    UIEditorFold* folds;
    size_t fold_count;
    UIEditorFiller* fillers;
    size_t filler_count;
    // The scroll: the line and row at the view's top, and how far that row is scrolled past.
    size_t top_line;
    size_t top_row;
    float top_frac;
    // The budget: the lines whose rows are still a guess, the lines edits rewrote, which it wraps
    // before walking the text for any others, where that walk looks next, the furthest line drawn,
    // which a stateful highlighter's walk reaches for, and whether that walk had lines left when it
    // last ran.
    size_t guessed;
    size_t hints[SETTLE_HINTS];
    size_t hint_count;
    size_t settle_line;
    size_t drawn_to;
    bool colouring;
} UIEditorPlace;

// A place parked on a text the editor left: a view of the text's own, and the columns its rows were
// wrapped at. `doc` is NULL once the text is freed.
typedef struct Parked {
    UIEditorPlace at;
    TextDoc* doc;
    int cols;
    struct Parked* next;
} Parked;

// A place asked of the view: an offset and how far down the view it stands, an offset to reveal, a
// line at the top and the rows scrolled into it, or the rows from the text's start to the top.
typedef struct {
    enum { PENDING_NONE, PENDING_PLACE, PENDING_REVEAL, PENDING_LINE, PENDING_ROW } kind;
    union {
        struct {
            size_t offset;
            float where;
        } place;
        size_t reveal;
        struct {
            size_t line;
            float rows;
        } top;
        double rows;
    };
} Pending;

struct UIEditor {
    UIElement* el;
    TextDoc* doc;
    uint64_t serial;  // ui_editor_serial: new at each text shown
    UIEditorPlace at; // where it is in the text it shows
    Parked* parked;   // where it was in each text it left
    // A place asked for before the editor was laid out on the text it shows, whose rows and gutter
    // it does not know yet, applied by the layout. `laid` is the serial the last layout placed it
    // under.
    Pending pending;
    uint64_t laid;
    // As the last layout left it: pixels a point, a cell and a row in points, the face, where the
    // content and the text are, and the columns.
    float px, cell, row;
    GlyphCache* face;
    UIRect content, text;
    int cols;
    int gutter_digits; // ui_editor_set_gutter_digits: the gutter's digits at least; 0 its text's
    int digits;        // the gutter's digits as the last layout gave it, which the draw numbers by
    uint64_t scrolls;  // ui_editor_scrolls
    uint64_t wraps;    // ui_editor_wraps
    // ui_editor_link: the editor it is held to; when it last took the lead, of every editor's
    // takings, so the later of a pair leads; and the layout that last placed it, which the second
    // of the pair placed compares with its own.
    UIEditor* partner;
    uint64_t led;
    uint64_t placed;
    // A run of presses, and the unit the last took, which a drag extends by.
    UITypingPresses presses;
    size_t grab_from, grab_to;
    UITypingCaret caret;
    // The last line wrapped: its row starts, which line at which version, columns and tab stops,
    // how many rows and its later rows' column.
    size_t* starts;
    size_t starts_capacity;
    size_t wrapped_line;
    uint64_t wrapped_version;
    int wrapped_cols, wrapped_tab, wrapped_hang;
    size_t wrapped_count;
    // The keys' mode, NULL the standard one; the UI it was last laid out in, for the clipboard; and
    // find's query.
    const UIEditorMode* mode;
    const UISystem* ui;
    char* find;
    size_t find_len;
    unsigned find_flags;
};

// The editor an element is, while the text it shows lives; NULL for anything else, and once the
// text is freed: an editor whose text is gone draws and does nothing.
static UIEditor* _of(UIElement* el) {
    UIEditor* ed = el && el->kind == UI_EDITOR ? el->editor : NULL;
    return ed && ed->doc ? ed : NULL;
}

// The same for a call given the element to read, whose editor still fills its own caches.
static UIEditor* _seen(const UIElement* el) {
    UIEditor* ed = el && el->kind == UI_EDITOR ? el->editor : NULL;
    return ed && ed->doc ? ed : NULL;
}

// ---- Characters

// Columns a character takes at `col`: a tab to the next stop, a byte-order mark none.
static int _width(uint32_t cp, int col, int tab) {
    if (cp == '\t')
        return tab - col % tab;
    return cp == 0xfeff ? 0 : 1;
}

// The column after the characters of s[from, to), from `col`.
static int _col_after(const char* s, size_t len, size_t from, size_t to, int col, int tab) {
    for (size_t i = from; i < to && i < len;)
        col += _width(ui_typing_next(s, len, &i), col, tab);
    return col;
}

// The byte in s[from, to) nearest column `want`, from `col` at `from`: `to` when it is past them.
static size_t _byte_near(const char* s, size_t len, size_t from, size_t to, int col, float want,
                         int tab) {
    for (size_t i = from; i < to;) {
        size_t n = i;
        const int w = _width(ui_typing_next(s, len, &n), col, tab);
        if (want < (float)col + (float)w * 0.5f)
            return i;
        col += w;
        i = n;
    }
    return to;
}

// The text's tab stops, held to what can be drawn and indented by.
static int _tab(const UIEditor* ed) {
    const int tab = ed->doc->tab_width;
    return tab < 1 ? 1 : tab > TAB_MAX ? TAB_MAX : tab;
}

// The line holding an offset, or a line by number: its start, its content and that content's
// length.
typedef struct {
    size_t line, at, len;
    const char* s;
} Line;

static Line _line_at(const UIEditor* ed, size_t line) {
    Line l;
    l.line = line;
    l.at = text_doc_line_start(ed->doc, line);
    l.s = text_doc_line(ed->doc, line, &l.len);
    return l;
}

static Line _line(const UIEditor* ed, size_t offset) {
    return _line_at(ed, text_doc_line_of(ed->doc, offset));
}

// ---- Wrapping

static int _hang(const char* s, size_t len, int cols, int tab) {
    size_t blanks = 0;
    while (blanks < len && (s[blanks] == ' ' || s[blanks] == '\t'))
        blanks++;
    const int col = _col_after(s, len, 0, blanks, 0, tab);
    return col * 2 > cols ? 0 : col;
}

size_t ui_editor_wrap(const char* s, size_t len, int cols, int tab, size_t* starts, size_t max,
                      int* hang_out) {
    if (cols < 1)
        cols = 1;
    tab = tab < 1 ? 1 : tab;
    const int hang = _hang(s, len, cols, tab);
    if (hang_out)
        *hang_out = hang;
    if (max)
        starts[0] = 0;
    size_t rows = 1, row_start = 0, brk = SIZE_MAX, i = 0;
    int col = 0;
    while (i < len) {
        size_t next = i;
        const uint32_t cp = ui_typing_next(s, len, &next);
        const int w = _width(cp, col, tab);
        if (col + w > cols && i > row_start) {
            // The row is full: break after its last space, or here for a word longer than a row,
            // and count again the characters the new row already holds.
            const size_t at = brk != SIZE_MAX && brk > row_start ? brk : i;
            if (rows < max)
                starts[rows] = at;
            rows++;
            row_start = at;
            brk = SIZE_MAX;
            col = _col_after(s, len, at, i, hang, tab);
            continue;
        }
        if (cp == ' ' || cp == '\t')
            brk = next;
        col += w;
        i = next;
    }
    return rows;
}

// ---- Rows

// A line's rows at the editor's columns: their starts in bytes of the line, how many, and the
// column its later rows start at. The starts are the editor's, good until another line is wrapped.
typedef struct {
    const size_t* starts;
    size_t count;
    int hang;
} Rows;

static size_t _estimate(const UIEditor* ed, size_t line) {
    size_t len = 0;
    text_doc_line(ed->doc, line, &len);
    return len / (size_t)ed->cols + 1;
}

// The rows above every line from `line` on are to be summed again, and any of those lines may show
// a guess: the exact mark, which only a walk wrapping the guesses raises, comes down with them.
static void _tops_from(UIEditorPlace* at, size_t line) {
    if (at->tops_good > line)
        at->tops_good = line;
    if (at->exact > line)
        at->exact = line;
}

static const UIEditorFold* _closed(const UIEditorPlace* at, size_t line);

// A line's rows, wrapped now unless it was the last wrapped and has not changed since; a count
// that was a guess, or has changed, is put right either way, as new columns may have made it a
// guess since. A line a closed fold holds shows the same rows whatever it wraps to, so the sums
// stand.
static Rows _wrap_line(UIEditor* ed, size_t line) {
    if (ed->wrapped_line != line || ed->wrapped_version != ed->doc->version ||
        ed->wrapped_cols != ed->cols || ed->wrapped_tab != _tab(ed)) {
        ed->wraps++;
        size_t len = 0;
        const char* s = text_doc_line(ed->doc, line, &len);
        int hang = 0;
        const int tab = _tab(ed);
        size_t n = ui_editor_wrap(s, len, ed->cols, tab, ed->starts, ed->starts_capacity, &hang);
        if (n > ed->starts_capacity) {
            if (!grow_array((void**)&ed->starts, &ed->starts_capacity, n, sizeof(size_t), 64))
                n = ed->starts_capacity;
            else
                n = ui_editor_wrap(s, len, ed->cols, tab, ed->starts, ed->starts_capacity, &hang);
        }
        ed->wrapped_line = line;
        ed->wrapped_version = ed->doc->version;
        ed->wrapped_cols = ed->cols;
        ed->wrapped_tab = tab;
        ed->wrapped_count = n ? n : 1;
        ed->wrapped_hang = hang;
    }
    const uint32_t count = (uint32_t)ed->wrapped_count;
    if (ed->at.rows[line] != count) {
        const size_t was = ed->at.rows[line] ? ed->at.rows[line] : _estimate(ed, line);
        if (was != count && !_closed(&ed->at, line))
            _tops_from(&ed->at, line);
        if (!ed->at.rows[line])
            ed->at.guessed--;
        ed->at.rows[line] = count;
    }
    return (Rows){ed->starts, count, ed->wrapped_hang};
}

// The fold holding `line`; NULL for none.
static UIEditorFold* _fold_of(const UIEditorPlace* at, size_t line) {
    size_t lo = 0, hi = at->fold_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (at->folds[mid].last < line)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < at->fold_count && at->folds[lo].first <= line ? &at->folds[lo] : NULL;
}

// The closed fold holding `line`, NULL for none. A fold of one line is never closed, as vim's
// 'foldminlines' has it: its row would hide nothing.
static const UIEditorFold* _closed(const UIEditorPlace* at, size_t line) {
    const UIEditorFold* f = _fold_of(at, line);
    return f && !f->open && f->last > f->first ? f : NULL;
}

// The line after `line` that shows: past the closed fold it starts, if any.
static size_t _next_shown(const UIEditorPlace* at, size_t line) {
    const UIEditorFold* f = _closed(at, line);
    return (f ? f->last : line) + 1;
}

// The line before `line` that shows: a closed fold's first line.
static size_t _prev_shown(const UIEditorPlace* at, size_t line) {
    const UIEditorFold* f = _closed(at, line - 1);
    return f ? f->first : line - 1;
}

// The filler rows above `line`.
static size_t _filler_rows(const UIEditorPlace* at, size_t line) {
    size_t lo = 0, hi = at->filler_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (at->fillers[mid].line < line)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < at->filler_count && at->fillers[lo].line == line ? at->fillers[lo].rows : 0;
}

// The filler rows under the text's last line, the fillers on its line count, which the last line
// that shows counts after its own; 0 for any other line. Sorted by line, they are the last fillers.
static size_t _under(const UIEditorPlace* at, size_t lines, size_t line) {
    const UIEditorFiller* end = at->filler_count ? &at->fillers[at->filler_count - 1] : NULL;
    return end && end->line == lines && line == _prev_shown(at, lines) ? end->rows : 0;
}

// The rows from the last line that shows summed again while fillers stand under the text: the
// line counting them moves with the folds and the line count.
static void _tops_from_under(UIEditorPlace* at, size_t lines) {
    if (_under(at, lines, _prev_shown(at, lines)))
        _tops_from(at, _prev_shown(at, lines));
}

/*
 * The rows a line shows, which every walk of the rows counts by: its fillers, then a closed fold's
 * first line one, its row, and the fold's other lines none at all; any other line its wrapped rows,
 * a guess while `wrap` is false and it is not wrapped yet; and the last line that shows, the
 * fillers under the text after them. A row of a line counts from its first filler. A line a closed
 * fold hides is never wrapped for drawing.
 */
static size_t _shown(UIEditor* ed, size_t line, bool wrap) {
    const UIEditorFold* f = _closed(&ed->at, line);
    if (f && f->first != line)
        return 0;
    const size_t fill = _filler_rows(&ed->at, line) + _under(&ed->at, ed->doc->line_count, line);
    if (f)
        return fill + 1;
    if (ed->at.rows[line])
        return fill + ed->at.rows[line];
    return fill + (wrap ? _wrap_line(ed, line).count : _estimate(ed, line));
}

// Whether (line, row) is one of the fillers under the text, past the last shown line's own rows,
// where no caret stands.
static bool _in_under(UIEditor* ed, size_t line, size_t row) {
    const size_t under = _under(&ed->at, ed->doc->line_count, line);
    return under && row + under >= _shown(ed, line, true);
}

// Wraps a guessed line for the budget: the bytes it read.
static size_t _settle_line(UIEditor* ed, size_t line) {
    if (line >= ed->doc->line_count || ed->at.rows[line])
        return 0;
    size_t len = 0;
    text_doc_line(ed->doc, line, &len);
    _wrap_line(ed, line);
    return len + 1;
}

// A frame's share of the work past what is drawn: the lines edits rewrote wrapped, then any other
// guessed line, up to SETTLE_BYTES of them, looked for round the text from where the last frame
// stopped; and a stateful highlighter's walk toward the furthest line drawn. Whether any is left.
static bool _settle(UIEditor* ed) {
    size_t bytes = 0, k = 0;
    for (; k < ed->at.hint_count && bytes < SETTLE_BYTES; k++)
        bytes += _settle_line(ed, ed->at.hints[k]);
    memmove(ed->at.hints, ed->at.hints + k, (ed->at.hint_count - k) * sizeof(size_t));
    ed->at.hint_count -= k;
    const size_t lines = ed->doc->line_count;
    for (size_t visited = 0;
         ed->at.guessed && !ed->at.hint_count && bytes < SETTLE_BYTES && visited < SETTLE_VISITS;
         visited++) {
        if (ed->at.settle_line >= lines)
            ed->at.settle_line = 0;
        bytes += _settle_line(ed, ed->at.settle_line++);
    }
    ed->at.colouring = text_doc_highlight_step(ed->doc, ed->at.drawn_to + 1, SETTLE_LINES);
    return ed->at.guessed || ed->at.colouring;
}

bool ui_editor_settled(const UIElement* el) {
    const UIEditor* ed = _seen(el);
    return !ed || (!ed->at.guessed && !ed->at.colouring);
}

uint64_t ui_editor_wraps(const UIElement* el) {
    const UIEditor* ed = _seen(el);
    return ed ? ed->wraps : 0;
}

// The rows above `line`, guessed for the lines not wrapped yet. With nothing folded or padded a
// line shows its wrapped rows, which the sums read straight, as they are summed over the whole
// text.
static uint64_t _top_of(UIEditor* ed, size_t line) {
    const bool plain = !ed->at.fold_count && !ed->at.filler_count;
    for (size_t i = ed->at.tops_good; i < line; i++)
        ed->at.tops[i + 1] =
            ed->at.tops[i] +
            (plain ? (ed->at.rows[i] ? ed->at.rows[i] : _estimate(ed, i)) : _shown(ed, i, false));
    if (ed->at.tops_good < line)
        ed->at.tops_good = line;
    return ed->at.tops[line];
}

// Moves (line, row) one row down, over the lines that show none; false at the text's last row.
static bool _row_down(UIEditor* ed, size_t* line, size_t* row) {
    if (*row + 1 < _shown(ed, *line, true)) {
        (*row)++;
        return true;
    }
    const size_t next = _next_shown(&ed->at, *line);
    if (next >= ed->doc->line_count)
        return false;
    *line = next;
    *row = 0;
    return true;
}

static bool _row_up(UIEditor* ed, size_t* line, size_t* row) {
    if (*row > 0) {
        (*row)--;
        return true;
    }
    if (*line == 0)
        return false;
    *line = _prev_shown(&ed->at, *line);
    *row = _shown(ed, *line, true) - 1;
    return true;
}

// A caret's move a row down or up, over a line's filler rows at a step, which no caret stands on;
// false with no row of a line's own that way, as from the last line onto the fillers under it.
static bool _caret_row(UIEditor* ed, size_t* line, size_t* row, bool down) {
    if (down ? !_row_down(ed, line, row) : !_row_up(ed, line, row))
        return false;
    if (_in_under(ed, *line, *row))
        return false;
    const size_t fill = _filler_rows(&ed->at, *line);
    if (*row >= fill)
        return true;
    // Onto fillers: going down, the line's own first row is past them; going up, the line before's
    // last row is above them.
    if (down) {
        *row = fill;
        return true;
    }
    *row = 0;
    return _row_up(ed, line, row);
}

// The row of `byte` in a line's rows: at a wrap, the row it begins.
static size_t _row_holding(Rows rows, size_t byte) {
    size_t r = 0;
    while (r + 1 < rows.count && rows.starts[r + 1] <= byte)
        r++;
    return r;
}

// The column `byte` stands at in row `r` of a line.
static int _column(const UIEditor* ed, const Line* l, Rows rows, size_t r, size_t byte) {
    return _col_after(l->s, l->len, rows.starts[r], byte, r ? rows.hang : 0, _tab(ed));
}

// The line a view can stand on nearest `line`: held to the text's lines, and a line a closed fold
// hides given as the fold's first, *hidden then true.
static size_t _held_line(const UIEditor* ed, size_t line, bool* hidden) {
    line = line < ed->doc->line_count ? line : ed->doc->line_count - 1;
    const UIEditorFold* f = _closed(&ed->at, line);
    *hidden = f && f->first != line;
    return f ? f->first : line;
}

// The line and row at the view's top, the line held to one that shows and the row to its rows: an
// edit can leave the line shorter than the row the view had scrolled to until the next layout, and
// a fold closed over the top puts its row there. `line` and `row` may be the place's own top.
static void _top(UIEditor* ed, size_t* line, size_t* row) {
    const size_t want = ed->at.top_row;
    bool hidden;
    *line = _held_line(ed, ed->at.top_line, &hidden);
    const size_t rows = _shown(ed, *line, true);
    *row = hidden ? _filler_rows(&ed->at, *line) : want >= rows ? rows - 1 : want;
}

// The view's top on a fold just closed over it: the fold's row, or the filler above it the top
// stood on. A row the first line wrapped to would read as a filler under the text after it.
static void _top_onto_fold(UIEditorPlace* at) {
    const UIEditorFold* f = _closed(at, at->top_line);
    if (!f)
        return;
    const size_t fill = _filler_rows(at, f->first);
    if (at->top_line != f->first || at->top_row > fill) {
        at->top_row = fill;
        at->top_frac = 0.0f;
    }
    at->top_line = f->first;
}

// The offset starting a row of a line, in its own rows: its fillers and a closed fold's row, its
// row 0 past the fillers, start at the line's start.
static size_t _row_offset(UIEditor* ed, size_t line, size_t row) {
    const size_t at = text_doc_line_start(ed->doc, line);
    const size_t fill = _filler_rows(&ed->at, line);
    return row <= fill ? at : at + _wrap_line(ed, line).starts[row - fill];
}

// The offset on (line, row) nearest column `want`; past a wrapped row's end, before its last
// character, as a click there is, so the caret stays on that row. A filler row is the line's start,
// and a closed fold's row its first line's first row.
static size_t _at_column(UIEditor* ed, size_t line, size_t row, float want) {
    const Line l = _line_at(ed, line);
    const size_t fill = _filler_rows(&ed->at, line);
    if (row < fill)
        return l.at;
    row -= fill;
    const Rows rows = _wrap_line(ed, line);
    row = row < rows.count ? row : rows.count - 1;
    const size_t from = rows.starts[row], to = row + 1 < rows.count ? rows.starts[row + 1] : l.len;
    const size_t at = _byte_near(l.s, l.len, from, to, row ? rows.hang : 0, want, _tab(ed));
    if (at == to && row + 1 < rows.count && to > from) {
        const char* p = l.s + to;
        text_utf8_prev(&p, l.s + from);
        return l.at + (size_t)(p - l.s);
    }
    return l.at + at;
}

// The line, the row and the column an offset stands at in its own line, folded or not: where a
// move along the row starts and the column a move by rows keeps. `col` may be NULL.
static void _line_place(UIEditor* ed, size_t offset, size_t* line, size_t* row, int* col) {
    const Line l = _line(ed, offset);
    const Rows rows = _wrap_line(ed, l.line);
    *line = l.line;
    *row = _row_holding(rows, offset - l.at);
    if (col)
        *col = _column(ed, &l, rows, *row, offset - l.at);
    *row += _filler_rows(&ed->at, l.line);
}

// Where an offset is drawn: its own place, or in a closed fold the start of the fold's row.
static void _place_of(UIEditor* ed, size_t offset, size_t* line, size_t* row, int* col) {
    const UIEditorFold* f = _closed(&ed->at, text_doc_line_of(ed->doc, offset));
    if (!f) {
        _line_place(ed, offset, line, row, col);
        return;
    }
    *line = f->first;
    *row = _filler_rows(&ed->at, f->first);
    if (col)
        *col = 0;
}

// Rows down from the view's top to (line, row), at most `limit` of them; -1 when it is not there.
static int _rows_down(UIEditor* ed, size_t line, size_t row, int limit) {
    size_t l, r;
    _top(ed, &l, &r);
    for (int down = 0; down <= limit; down++) {
        if (l == line && r == row)
            return down;
        if (!_row_down(ed, &l, &r))
            break;
    }
    return -1;
}

// Leads taken so far, by every editor; the UI is the main thread's.
static uint64_t g_leads;

// The editor takes the lead of the pair it is, or will be, linked in: the later taking leads, so
// each taking touches one editor.
static void _lead(UIEditor* ed) {
    ed->led = ++g_leads;
}

// A scroll asked of the view, by the host or by the editor itself: counted, and the editor leads
// the pair it is linked in. Never by a layout, which only takes what was asked before it.
static void _asked(UIEditor* ed) {
    ed->scrolls++;
    _lead(ed);
}

// The view scrolled by rows from its top as held, so a fold closed over the top since the last
// layout scrolls from its row.
static void _scroll_rows(UIEditor* ed, float delta) {
    _asked(ed);
    _top(ed, &ed->at.top_line, &ed->at.top_row);
    float t = ed->at.top_frac + delta;
    while (t >= 1.0f) {
        if (!_row_down(ed, &ed->at.top_line, &ed->at.top_row)) {
            t = 0.0f;
            break;
        }
        t -= 1.0f;
    }
    while (t < 0.0f) {
        if (!_row_up(ed, &ed->at.top_line, &ed->at.top_row)) {
            t = 0.0f;
            break;
        }
        t += 1.0f;
    }
    ed->at.top_frac = t;
}

// ---- Selections

static size_t _sel_min(const TextSelection* s) {
    return s->anchor < s->head ? s->anchor : s->head;
}

static size_t _sel_max(const TextSelection* s) {
    return s->anchor > s->head ? s->anchor : s->head;
}

// An offset held to the text and moved back to its character's start.
static size_t _edge(TextDoc* doc, size_t offset) {
    if (offset > doc->length)
        offset = doc->length;
    while (offset > 0 && text_utf8_is_continuation(text_doc_byte(doc, offset)))
        offset--;
    return offset;
}

// Whether selection a comes before b: by start, then by end, so a caret comes before a selection
// starting where it is.
static bool _sel_before(const TextSelection* a, const TextSelection* b) {
    const size_t x = _sel_min(a), y = _sel_min(b);
    return x < y || (x == y && _sel_max(a) < _sel_max(b));
}

typedef struct {
    TextSelection s;
    int want;
    bool primary;
} Caret;

static int _by_caret(const void* a, const void* b) {
    const TextSelection* x = &((const Caret*)a)->s;
    const TextSelection* y = &((const Caret*)b)->s;
    return _sel_before(x, y) ? -1 : _sel_before(y, x) ? 1 : 0;
}

// The selections in order, the primary followed.
static void _sort(UIEditor* ed) {
    size_t i = 1;
    while (i < ed->at.sel_count && !_sel_before(&ed->at.sel[i], &ed->at.sel[i - 1]))
        i++;
    if (i >= ed->at.sel_count)
        return;
    Caret* c = malloc(ed->at.sel_count * sizeof(Caret));
    if (!c)
        return;
    for (i = 0; i < ed->at.sel_count; i++)
        c[i] = (Caret){ed->at.sel[i], ed->at.want[i], i == ed->at.primary};
    qsort(c, ed->at.sel_count, sizeof(Caret), _by_caret);
    for (i = 0; i < ed->at.sel_count; i++) {
        ed->at.sel[i] = c[i].s;
        ed->at.want[i] = c[i].want;
        if (c[i].primary)
            ed->at.primary = i;
    }
    free(c);
}

static const UIEditorMode* _look_of(const UIEditor* ed);
static void _drawn(UIEditor* ed, const TextSelection* s, UIEditorSelectionShape shape, size_t* lo,
                   size_t* hi);

// Selections in order merged where they overlap, or touch where either is a caret, in place, the
// primary followed -- overlapping as drawn, so in a shape that takes the far character or whole
// lines, two sharing a character or a line are one. A merged selection runs the way the first that
// is not a caret ran.
static void _merge(UIEditor* ed) {
    const UIEditorSelectionShape shape = _look_of(ed)->selection;
    size_t n = 0;
    for (size_t i = 0; i < ed->at.sel_count; i++) {
        if (n) {
            TextSelection* last = &ed->at.sel[n - 1];
            const TextSelection* s = &ed->at.sel[i];
            const size_t lo = _sel_min(s), hi = _sel_max(s);
            const size_t llo = _sel_min(last), lhi = _sel_max(last);
            size_t dlo = lo, dhi = lhi, unused = 0;
            if (shape != UI_SELECTION_SPAN) {
                _drawn(ed, s, shape, &dlo, &unused);
                _drawn(ed, last, shape, &unused, &dhi);
            }
            if (lo < lhi || (lo == lhi && (lo == hi || llo == lhi)) || dlo < dhi) {
                const size_t to = hi > lhi ? hi : lhi;
                const TextSelection* way = llo == lhi ? s : last;
                *last =
                    way->head >= way->anchor ? (TextSelection){llo, to} : (TextSelection){to, llo};
                if (ed->at.primary == i)
                    ed->at.primary = n - 1;
                continue;
            }
        }
        ed->at.sel[n] = ed->at.sel[i];
        ed->at.want[n] = ed->at.want[i];
        if (ed->at.primary == i)
            ed->at.primary = n;
        n++;
    }
    ed->at.sel_count = n;
}

static void _normalize(UIEditor* ed) {
    _sort(ed);
    _merge(ed);
}

static bool _room(UIEditor* ed, size_t count) {
    size_t cap = ed->at.sel_capacity;
    if (!grow_array((void**)&ed->at.sel, &ed->at.sel_capacity, count, sizeof(TextSelection), 4))
        return false;
    return grow_array((void**)&ed->at.want, &cap, count, sizeof(int), 4);
}

// The selections given, held to the text, with their wanted columns (NULL for none) and the
// primary.
static void _set_at(UIEditor* ed, const TextSelection* s, const int* wants, size_t count,
                    size_t primary) {
    if (!count || !_room(ed, count))
        return;
    for (size_t i = 0; i < count; i++) {
        ed->at.sel[i] = (TextSelection){_edge(ed->doc, s[i].anchor), _edge(ed->doc, s[i].head)};
        ed->at.want[i] = wants ? wants[i] : -1;
    }
    ed->at.sel_count = count;
    ed->at.primary = primary < count ? primary : count - 1;
    _normalize(ed);
    ed->caret.moved = true;
}

// The selections given, held to the text, the last the primary. The text's call as well as the
// editor's, so it takes the editor as a view.
static void _set(void* view, const TextSelection* s, size_t count) {
    _set_at(view, s, NULL, count, count - 1);
}

// One selection, the primary.
static void _carets_one(UIEditor* ed, TextSelection s) {
    ed->at.sel[0] = s;
    ed->at.want[0] = -1;
    ed->at.sel_count = 1;
    ed->at.primary = 0;
}

// A selection added as the primary, with no wanted column; false out of memory.
static bool _carets_add(UIEditor* ed, TextSelection s) {
    if (!_room(ed, ed->at.sel_count + 1))
        return false;
    ed->at.sel[ed->at.sel_count] = s;
    ed->at.want[ed->at.sel_count] = -1;
    ed->at.primary = ed->at.sel_count++;
    return true;
}

// ---- The text's calls

// A change to the text's lines: `removed` lines from `first` became `inserted`, the first of them
// still the old first line when `head`, the change having begun past its start, and the last still
// the old last when `tail`, the text after the change starting a line of its own.
typedef struct {
    size_t first, removed, inserted;
    bool head, tail;
} Shift;

static Shift _shift_of(TextDoc* doc, const TextViewChange* c) {
    Shift s = {c->first_line, c->lines_removed, c->lines_inserted, true, true};
    if (s.removed == 1 && s.inserted == 1)
        return s; // within one line
    s.head = text_doc_line_start(doc, s.first) < c->start;
    s.tail = (s.removed > 1 || !s.head) &&
             text_doc_line_start(doc, s.first + s.inserted - 1) == c->start + c->inserted;
    return s;
}

// Where a change takes `line`; SIZE_MAX for a line it took out.
static size_t _moved(const Shift* s, size_t line) {
    if (line < s->first)
        return line;
    if (line >= s->first + s->removed)
        return line - s->removed + s->inserted;
    if (line == s->first && s->head)
        return s->first;
    if (line == s->first + s->removed - 1 && s->tail)
        return s->first + s->inserted - 1;
    return SIZE_MAX;
}

// The folds through a change: each keeps the lines of it the change kept and takes the lines put
// in among them, as vim's do; one it took every line of goes.
static void _shift_folds(UIEditorPlace* at, const Shift* s) {
    size_t kept = 0;
    for (size_t i = 0; i < at->fold_count; i++) {
        UIEditorFold f = at->folds[i];
        size_t lo = _moved(s, f.first), hi = _moved(s, f.last);
        // A first line taken out gives the fold the first new line not the old first; a last, the
        // last not the old last. A fold keeping a line ends at one at least.
        const bool gone = lo == SIZE_MAX && hi == SIZE_MAX;
        lo = lo != SIZE_MAX ? lo : s->first + s->head;
        hi = hi != SIZE_MAX ? hi : s->first + s->inserted - s->tail - 1;
        // Lines put in where two folds met are the first's.
        if (kept && lo <= at->folds[kept - 1].last)
            lo = at->folds[kept - 1].last + 1;
        if (gone || lo > hi) {
            free((char*)f.label);
            continue;
        }
        f.first = lo;
        f.last = hi;
        at->folds[kept++] = f;
    }
    at->fold_count = kept;
}

// The fillers through a change: each stays above its line, and goes with it.
static void _shift_fillers(UIEditorPlace* at, const Shift* s) {
    size_t kept = 0;
    for (size_t i = 0; i < at->filler_count; i++) {
        const size_t line = _moved(s, at->fillers[i].line);
        if (line != SIZE_MAX)
            at->fillers[kept++] = (UIEditorFiller){line, at->fillers[i].rows};
    }
    at->filler_count = kept;
}

// The lines a change rewrote, the view kept at its place in the text -- a line above the change
// stays, one below it moves with its lines, and one the change rewrote is held to what replaced
// it, which is at least one line. A line edited within itself that was wrapped is wrapped again
// now, so typing leaves the rows above every later line as they were unless its own count changed;
// any other line rewritten is a guess the budget wraps first.
static void _shift_lines(UIEditorPlace* at, size_t count, const Shift* s) {
    const size_t first = s->first, removed = s->removed, inserted = s->inserted;
    const size_t old = count - inserted + removed;
    if (!grow_array((void**)&at->rows, &at->line_capacity, count, sizeof(uint32_t), 64) ||
        !grow_array((void**)&at->tops, &at->tops_capacity, count + 1, sizeof(uint64_t), 64))
        return;
    for (size_t i = first; i < first + removed; i++)
        at->guessed -= at->rows[i] == 0;
    if (inserted != removed)
        memmove(&at->rows[first + inserted], &at->rows[first + removed],
                (old - first - removed) * sizeof(uint32_t));
    memset(&at->rows[first], 0, inserted * sizeof(uint32_t));
    at->guessed += inserted;
    // From a closed fold's first line when the change starts inside it: cut to that one line, the
    // fold no longer closes, and the line shows the rows it wraps to.
    const UIEditorFold* holding = _closed(at, first);
    _tops_from(at, holding ? holding->first : first);
    // The line counting the fillers under the text may stand above the change, before it and after.
    _tops_from_under(at, old);
    // The budget's lines below the change move with their lines, those it rewrote go, and the new
    // lines are its.
    size_t kept = 0;
    for (size_t k = 0; k < at->hint_count; k++) {
        const size_t h = at->hints[k];
        if (h < first || h >= first + removed)
            at->hints[kept++] = h < first ? h : h - removed + inserted;
    }
    at->hint_count = kept;
    for (size_t i = 0; i < inserted && at->hint_count < SETTLE_HINTS; i++)
        at->hints[at->hint_count++] = first + i;
    if (at->top_line >= first + removed) {
        at->top_line = at->top_line - removed + inserted;
    } else if (at->top_line >= first) {
        const size_t into = at->top_line - first;
        at->top_line = first + (into < inserted ? into : inserted - 1);
    }
    _shift_folds(at, s);
    _shift_fillers(at, s);
    _tops_from_under(at, count);
}

// The selections through a change, which moves every offset the same way, so they stay in order;
// a caret it moved wants no column.
static void _map_selections(UIEditorPlace* at, const TextViewChange* c) {
    for (size_t i = 0; i < at->sel_count; i++) {
        const size_t was = at->sel[i].head;
        text_doc_map_selection(&at->sel[i], c);
        if (at->sel[i].head != was)
            at->want[i] = -1;
    }
}

static void _change_lines(UIEditor* ed, const TextViewChange* c) {
    if (c->lines_removed == 1 && c->lines_inserted == 1 && ed->at.rows[c->first_line]) {
        _wrap_line(ed, c->first_line);
        return;
    }
    const Shift s = _shift_of(ed->doc, c);
    _shift_lines(&ed->at, ed->doc->line_count, &s);
}

static void _view_changed(void* view, const TextViewChange* c) {
    UIEditor* ed = view;
    _change_lines(ed, c);
    if (c->held)
        return;
    _map_selections(&ed->at, c);
    _merge(ed);
    if (c->actor)
        ed->caret.moved = true;
}

// A parked place through the text's changes, as the editor itself would be moved. A line rewritten
// is a guess, wrapped when the editor comes back; selections a change ran together are merged then.
static void _parked_changed(void* view, const TextViewChange* c) {
    Parked* p = view;
    const Shift s = _shift_of(p->doc, c);
    _shift_lines(&p->at, p->doc->line_count, &s);
    if (!c->held)
        _map_selections(&p->at, c);
}

static size_t _parked_selections(void* view, const TextSelection** out) {
    const Parked* p = view;
    *out = p->at.sel;
    return p->at.sel_count;
}

static void _parked_gone(void* view) {
    ((Parked*)view)->doc = NULL;
}

static void _folds_free(UIEditorFold* folds, size_t count) {
    for (size_t i = 0; i < count; i++)
        free((char*)folds[i].label);
    free(folds);
}

static void _place_free(UIEditorPlace* at) {
    free(at->sel);
    free(at->want);
    free(at->rows);
    free(at->tops);
    _folds_free(at->folds, at->fold_count);
    free(at->fillers);
}

static size_t _view_selections(void* view, const TextSelection** out) {
    const UIEditor* ed = view;
    *out = ed->at.sel;
    return ed->at.sel_count;
}

static void _view_gone(void* view) {
    ((UIEditor*)view)->doc = NULL;
}

// Serials handed out so far; the UI is the main thread's.
static uint64_t g_serials;

// Every line's rows a guess, from the first: a text new to the place, or rows wrapped at other
// columns.
static void _rows_guessed(UIEditorPlace* at, size_t lines) {
    memset(at->rows, 0, lines * sizeof(uint32_t));
    at->tops[0] = 0; // the rows above the first line: every other top is summed from it
    at->tops_good = 0;
    at->exact = 0;
    at->guessed = lines;
    at->settle_line = 0;
}

// The editor leaves the text it shows: where it was there parked on the text, which keeps it
// current as it keeps an editor's carets. A text freed first, or no room to park in, and the place
// is let go.
static void _park(UIEditor* ed) {
    Parked* p = ed->doc ? malloc(sizeof(Parked)) : NULL;
    if (ed->doc)
        text_doc_remove_view(ed->doc, ed);
    if (p) {
        *p = (Parked){.at = ed->at, .doc = ed->doc, .cols = ed->cols, .next = ed->parked};
        ed->parked = p;
        text_doc_add_view(p->doc, &(TextDocView){.view = p,
                                                 .changed = _parked_changed,
                                                 .selections = _parked_selections,
                                                 .gone = _parked_gone});
    } else {
        _place_free(&ed->at);
    }
    ed->at = (UIEditorPlace){0};
    ed->doc = NULL;
}

// The place parked on `doc` taken back into the editor; false when there is none. Places parked on
// texts since freed are let go of on the way.
static bool _unpark(UIEditor* ed, TextDoc* doc) {
    for (Parked** pp = &ed->parked; *pp;) {
        Parked* p = *pp;
        if (p->doc && p->doc != doc) {
            pp = &p->next;
            continue;
        }
        *pp = p->next;
        if (p->doc) {
            text_doc_remove_view(doc, p);
            ed->at = p->at;
            if (p->cols != ed->cols)
                _rows_guessed(&ed->at, doc->line_count);
            free(p);
            return true;
        }
        _place_free(&p->at);
        free(p);
    }
    return false;
}

// Leaves the text it shows, if any, and shows `doc` where it last was in it -- or from its start,
// as a new editor would, one caret at 0 and the view at the top -- with no press under way, under a
// new serial. NULL shows nothing. False out of memory, when it shows nothing.
static bool _show(UIEditor* ed, TextDoc* doc) {
    _park(ed);
    ed->serial = ++g_serials;
    ed->wrapped_line = SIZE_MAX;
    ed->presses = (UITypingPresses){0};
    ed->grab_from = ed->grab_to = 0;
    ed->caret.moved = true;
    ed->pending.kind = PENDING_NONE;
    // Another text follows its partner to where it stands, until a scroll is asked of it.
    if (ed->partner)
        _lead(ed->partner);
    if (!doc)
        return true;
    const bool back = _unpark(ed, doc);
    if (!back) {
        const size_t lines = doc->line_count;
        if (!_room(ed, 1) ||
            !grow_array((void**)&ed->at.rows, &ed->at.line_capacity, lines, sizeof(uint32_t), 64) ||
            !grow_array((void**)&ed->at.tops, &ed->at.tops_capacity, lines + 1, sizeof(uint64_t),
                        64)) {
            _place_free(&ed->at);
            ed->at = (UIEditorPlace){0};
            return false;
        }
        _carets_one(ed, (TextSelection){0, 0});
        _rows_guessed(&ed->at, lines);
    }
    ed->doc = doc;
    text_doc_add_view(doc, &(TextDocView){.view = ed,
                                          .element = ed->el,
                                          .changed = _view_changed,
                                          .selections = _view_selections,
                                          .set_selections = _set,
                                          .gone = _view_gone});
    // Selections a change ran together while it was parked are one again.
    if (back)
        _normalize(ed);
    return true;
}

UIEditor* ui_editor_make(UIElement* el, TextDoc* doc) {
    UIEditor* ed = calloc(1, sizeof(UIEditor));
    if (!ed)
        return NULL;
    ed->el = el;
    ed->cols = 1;
    ed->px = 1.0f;
    if (!_show(ed, doc)) {
        ui_editor_free(ed);
        return NULL;
    }
    return ed;
}

void ui_editor_set_doc(UIElement* el, TextDoc* doc) {
    UIEditor* ed = el && el->kind == UI_EDITOR ? el->editor : NULL;
    if (!ed) {
        log_error("ui_editor_set_doc: not an editor");
        return;
    }
    if (doc == ed->doc)
        return;
    if (!_show(ed, doc))
        log_error("ui_editor_set_doc: out of memory, so the editor shows nothing");
}

// The editor's pair let go of, on both sides.
static void _unlink(UIEditor* ed) {
    if (ed->partner)
        ed->partner->partner = NULL;
    ed->partner = NULL;
}

void ui_editor_free(UIEditor* ed) {
    if (!ed)
        return;
    _unlink(ed);
    if (ed->doc)
        text_doc_remove_view(ed->doc, ed);
    _place_free(&ed->at);
    while (ed->parked) {
        Parked* p = ed->parked;
        ed->parked = p->next;
        if (p->doc)
            text_doc_remove_view(p->doc, p);
        _place_free(&p->at);
        free(p);
    }
    free(ed->starts);
    free(ed->find);
    free(ed);
}

// ---- Geometry

static float _snap(const UIEditor* ed, float v) {
    return roundf(v * ed->px) / ed->px;
}

// The digits the gutter holds: its text's line count's, or the more the editor was asked for, and
// GUTTER_DIGITS at the fewest.
static int _gutter_digits(const UIEditor* ed) {
    int d = 1;
    for (size_t n = ed->doc->line_count; n >= 10; n /= 10)
        d++;
    const int least = ed->gutter_digits > GUTTER_DIGITS ? ed->gutter_digits : GUTTER_DIGITS;
    return d < least ? least : d;
}

static void _take_pending(UIEditor* ed);
static void _hold(UIEditor* ed);

// The layout's: the face at its size, the gutter, where the text goes and how many columns a row
// holds; rows wrapped at other columns or tab stops are guesses again.
static void _place(const UISystem* ui, UIElement* el, UIRect content) {
    UIEditor* ed = _of(el);
    if (!ed)
        return;
    const UITheme* t = ui_theme(ui);
    const Engine* engine = ui_system_engine(ui);
    ed->px =
        engine && engine->win_width > 0 ? (float)engine->fb_width / (float)engine->win_width : 1.0f;
    const float size = t->editor_size > 0.0f ? t->editor_size : EDITOR_SIZE;
    ed->ui = ui;
    ed->face = t->editor_face;
    if (ed->face) {
        glyph_cache_set_size(ed->face, size * ed->px);
        ed->cell = (float)ed->face->cell_width / ed->px;
        ed->row = (float)ed->face->line_height / ed->px;
    } else {
        ed->cell = size * 0.6f;
        ed->row = size * 1.3f;
    }
    ed->content = content;
    ed->digits = _gutter_digits(ed);
    const float gutter = (float)(ed->digits + GUTTER_GAP) * ed->cell;
    ed->text.x = _snap(ed, content.x + gutter);
    ed->text.y = _snap(ed, content.y);
    ed->text.w = content.x + content.w - UI_BAR_W - UI_BAR_INSET - ed->text.x;
    ed->text.h = content.y + content.h - ed->text.y;
    int cols = (int)floorf(ed->text.w / ed->cell);
    cols = cols < 1 ? 1 : cols;
    if (cols != ed->cols || _tab(ed) != ed->at.tab) {
        _rows_guessed(&ed->at, ed->doc->line_count);
        ed->cols = cols;
        ed->at.tab = _tab(ed);
    }
    _top(ed, &ed->at.top_line, &ed->at.top_row);
    // A place asked for before the editor was laid out on this text.
    ed->laid = ed->serial;
    _take_pending(ed);
    // Placed by this layout: the second of a pair placed holds the two together.
    ed->placed = ui_system_layouts(ui);
    _hold(ed);
}

static float _view_rows(const UIEditor* ed) {
    return ed->row > 0.0f ? ed->text.h / ed->row : 0.0f;
}

// The view's top in rows from the text's start, with the part of the top row scrolled past.
static double _top_row(UIEditor* ed) {
    size_t line, row;
    _top(ed, &line, &row);
    return (double)_top_of(ed, line) + (double)row + ed->at.top_frac;
}

// Where the view is in the text's rows, the last row at the top being all the way along; false
// while the whole text shows.
static bool _scroll(const UIElement* el, UIScrollPlace* place) {
    UIEditor* ed = _seen(el);
    if (!ed || ed->row <= 0.0f)
        return false;
    const uint64_t total = _top_of(ed, ed->doc->line_count);
    const double at = _top_row(ed);
    *place = (UIScrollPlace){_view_rows(ed), (float)total,
                             total > 1 ? (float)(at / (double)(total - 1)) : 0.0f, WHEEL_ROWS};
    return place->total > place->view;
}

// The line holding the global row `row`, and the row in it: never one showing none, since the
// search takes the last line whose top is at or above the row.
static void _line_at_row(UIEditor* ed, uint64_t row, size_t* line, size_t* in) {
    _top_of(ed, ed->doc->line_count);
    size_t lo = 0, hi = ed->doc->line_count;
    while (hi - lo > 1) {
        const size_t mid = lo + (hi - lo) / 2;
        if (ed->at.tops[mid] <= row)
            lo = mid;
        else
            hi = mid;
    }
    *line = lo;
    const uint64_t r = row - ed->at.tops[lo];
    const size_t rows = _shown(ed, lo, true);
    *in = r < rows ? (size_t)r : rows - 1;
}

// The text's last row from its start: as far as the view's top goes, as a wheel's scroll stops.
static double _last_row(UIEditor* ed) {
    const uint64_t total = _top_of(ed, ed->doc->line_count);
    return (double)(total ? total - 1 : 0);
}

// The view's top put `rows` rows from the text's start, held to the first row and the last.
static void _scroll_row(UIEditor* ed, double rows) {
    const double last = _last_row(ed);
    rows = !(rows >= 0.0) ? 0.0 : rows > last ? last : rows;
    _line_at_row(ed, (uint64_t)rows, &ed->at.top_line, &ed->at.top_row);
    ed->at.top_frac = (float)(rows - floor(rows));
}

// The view put `at` the way along its rows: the bar's thumb held there. The rows are the sums as
// they stand, so a drag through a large text wraps none of it.
static void _scroll_to(UIElement* el, float at) {
    UIEditor* ed = _of(el);
    if (!ed)
        return;
    _asked(ed);
    _scroll_row(ed, (double)at * _last_row(ed));
}

static void _scroll_by(UIElement* el, float rows) {
    UIEditor* ed = _of(el);
    if (ed)
        _scroll_rows(ed, rows);
}

// ---- Where things are

// The row a point at height `y` is on, in the rows as last laid out: -1 above the text's first row,
// 1 below its last line's rows, the fillers under the text among them, else 0 and (line, row).
static int _row_at(UIEditor* ed, float y, size_t* line, size_t* row) {
    float down = (y - ed->text.y) / ed->row + ed->at.top_frac;
    _top(ed, line, row);
    for (; down >= 1.0f; down -= 1.0f)
        if (!_row_down(ed, line, row))
            return 1;
    for (; down < 0.0f; down += 1.0f)
        if (!_row_up(ed, line, row))
            return -1;
    return _in_under(ed, *line, *row) ? 1 : 0;
}

// The offset a point at (x, y) is nearest, in the rows as last laid out.
size_t ui_editor_offset_at(UIElement* el, float x, float y) {
    UIEditor* ed = _of(el);
    if (!ed || ed->row <= 0.0f)
        return 0;
    size_t line, row;
    const int where = _row_at(ed, y, &line, &row);
    return where < 0   ? 0
           : where > 0 ? ed->doc->length
                       : _at_column(ed, line, row, (x - ed->text.x) / ed->cell);
}

bool ui_editor_point_of(UIElement* el, size_t offset, float* x, float* y) {
    UIEditor* ed = _of(el);
    if (!ed || ed->row <= 0.0f || offset > ed->doc->length)
        return false;
    size_t line, row;
    int col;
    _place_of(ed, offset, &line, &row, &col);
    const int down = _rows_down(ed, line, row, (int)ceilf(_view_rows(ed)) + 1);
    if (down < 0)
        return false;
    *x = ed->text.x + (float)col * ed->cell;
    *y = ed->text.y + ((float)down - ed->at.top_frac) * ed->row;
    return true;
}

int ui_editor_columns(const UIElement* el) {
    const UIEditor* ed = _seen(el);
    return ed ? ed->cols : 0;
}

// The offset starting the row `down` rows below the view's top, held to the text's last row; on a
// filler under the text, the text's end.
static size_t _row_down_from_top(UIEditor* ed, int down) {
    size_t line, row;
    _top(ed, &line, &row);
    for (int k = 0; k < down && _row_down(ed, &line, &row); k++) {
    }
    return _in_under(ed, line, row) ? ed->doc->length : _row_offset(ed, line, row);
}

size_t ui_editor_top(const UIElement* el) {
    UIEditor* ed = _seen(el);
    return ed ? _row_down_from_top(ed, 0) : 0;
}

size_t ui_editor_top_line(const UIElement* el, float* rows) {
    UIEditor* ed = _seen(el);
    size_t line = 0, row = 0;
    if (ed)
        _top(ed, &line, &row);
    *rows = ed ? (float)row + ed->at.top_frac : 0.0f;
    return line;
}

// Whether the last layout placed the editor on the text it shows: its rows, columns and gutter are
// that text's, wrapped at its tab stops as they stand. Before then, where a view goes is asked of
// the next layout.
static bool _laid_out(const UIEditor* ed) {
    return ed->row > 0.0f && ed->laid == ed->serial && ed->at.tab == _tab(ed);
}

/*
 * Every line that shows from the mark up to `line`, or up to and through the one holding the
 * global row `row` if that comes first, wrapped where it was a guess as _shown wraps one, so the
 * rows above a place are the rows it wraps to and two editors on texts that stand level agree on
 * every row. A closed fold's lines show its one row whatever they wrap to and are left guesses.
 * The mark keeps a second ask from walking the lines again. Rows wrapped before the layout gives
 * the columns and the tab stops would be wrapped at the wrong ones.
 */
static void _exact(UIEditor* ed, size_t line, double row) {
    if (!_laid_out(ed))
        return;
    size_t l = ed->at.exact;
    for (; l < line && (double)_top_of(ed, l) <= row; l = _next_shown(&ed->at, l))
        _shown(ed, l, true);
    if (l > ed->at.exact)
        ed->at.exact = l;
}

// The view's top as ui_editor_top_row counts it, the rows above it exact.
static double _exact_top_row(UIEditor* ed) {
    size_t line, row;
    _top(ed, &line, &row);
    _exact(ed, line, INFINITY);
    return _top_row(ed);
}

// The view's top put `rows` exact rows from the text's start, as ui_editor_scroll_row puts it.
static void _exact_scroll_row(UIEditor* ed, double rows) {
    _exact(ed, ed->doc->line_count, rows);
    _scroll_row(ed, rows);
}

/*
 * A pair held row for row, by the second of it this layout places: both then have this layout's
 * columns, gutters and places asked, so the follower is put at the leader's exact row, as
 * ui_editor_scroll_row would put it, whenever the two differ. Not a scroll asked, so it leads
 * nothing. Done in the layout because that is where rows change -- new columns, a fold turned,
 * fillers set, the budget's wraps -- and the draw follows it: a host copying the row between two
 * layouts draws a frame with the two apart whenever the second layout moves the leader.
 */
static void _hold(UIEditor* ed) {
    // Only _place stamps an editor, and only one showing a text.
    UIEditor* other = ed->partner;
    if (!other || other->ui != ed->ui || other->placed != ed->placed)
        return;
    UIEditor* lead = other->led > ed->led ? other : ed;
    UIEditor* follow = lead == ed ? other : ed;
    const double rows = _exact_top_row(lead);
    if (_exact_top_row(follow) != rows)
        _exact_scroll_row(follow, rows);
}

static void _reveal(UIEditor* ed, size_t offset) {
    offset = offset > ed->doc->length ? ed->doc->length : offset;
    size_t line, row, top_line, top_row;
    _place_of(ed, offset, &line, &row, NULL);
    _top(ed, &top_line, &top_row);
    // Above the view's top: that row becomes the top.
    if (line < top_line || (line == top_line && row <= top_row)) {
        ed->at.top_line = line;
        ed->at.top_row = row;
        ed->at.top_frac = 0.0f;
        return;
    }
    // Inside the rows wholly shown: nothing to do. Below them, or far below: the row becomes the
    // last wholly shown.
    const int shown = (int)floorf(_view_rows(ed));
    const int last = shown > 0 ? shown - 1 : 0;
    const int down = _rows_down(ed, line, row, last);
    if (down >= 0 && (down < last || ed->at.top_frac == 0.0f))
        return;
    size_t l = line, r = row;
    for (int up = 0; up < last; up++)
        if (!_row_up(ed, &l, &r))
            break;
    ed->at.top_line = l;
    ed->at.top_row = r;
    ed->at.top_frac = 0.0f;
}

static void _scroll_place(UIEditor* ed, size_t offset, float where) {
    size_t line, row;
    _place_of(ed, offset > ed->doc->length ? ed->doc->length : offset, &line, &row, NULL);
    const int shown = (int)floorf(_view_rows(ed));
    const int up = (int)floorf((float)((shown > 1 ? shown : 1) - 1) * where);
    for (int k = 0; k < up && _row_up(ed, &line, &row); k++) {
    }
    ed->at.top_line = line;
    ed->at.top_row = row;
    ed->at.top_frac = 0.0f;
}

// The line's top row at the view's top, then `rows` of it scrolled past, at most the line's own
// rows: past them is the next line's top, so a view held to another's line never runs on into
// lines below it. A line a closed fold hides is the fold's row, past its fillers.
static void _scroll_line(UIEditor* ed, size_t line, float rows) {
    bool hidden;
    line = _held_line(ed, line, &hidden);
    const double most = (double)_shown(ed, line, true);
    const double into = hidden ? (double)_filler_rows(&ed->at, line) : fmin(fmax(rows, 0.0), most);
    _scroll_row(ed, (double)_top_of(ed, line) + into);
}

// The place asked for, if any, put now that the editor is laid out on its text.
static void _take_pending(UIEditor* ed) {
    const Pending p = ed->pending;
    ed->pending.kind = PENDING_NONE;
    switch (p.kind) {
        case PENDING_PLACE:
            _scroll_place(ed, p.place.offset, p.place.where);
            break;
        case PENDING_REVEAL:
            _reveal(ed, p.reveal);
            break;
        case PENDING_LINE:
            _scroll_line(ed, p.top.line, p.top.rows);
            break;
        case PENDING_ROW:
            _exact_scroll_row(ed, p.rows);
            break;
        case PENDING_NONE:
            break;
    }
}

// A place asked for: put at once when the editor is laid out on its text, else at its next layout;
// counted and led by when it is asked either way, so the layout taking it later counts nothing.
static void _pend(UIElement* el, Pending p) {
    UIEditor* ed = _of(el);
    if (!ed)
        return;
    _asked(ed);
    ed->pending = p;
    if (_laid_out(ed))
        _take_pending(ed);
}

void ui_editor_reveal(UIElement* el, size_t offset) {
    _pend(el, (Pending){.kind = PENDING_REVEAL, .reveal = offset});
}

void ui_editor_scroll_place(UIElement* el, size_t offset, float where) {
    _pend(el, (Pending){.kind = PENDING_PLACE, .place = {offset, where}});
}

void ui_editor_scroll_line(UIElement* el, size_t line, float rows) {
    _pend(el, (Pending){.kind = PENDING_LINE, .top = {line, rows}});
}

double ui_editor_top_row(const UIElement* el) {
    UIEditor* ed = _seen(el);
    if (!ed)
        return 0.0;
    return _exact_top_row(ed);
}

void ui_editor_scroll_row(UIElement* el, double rows) {
    _pend(el, (Pending){.kind = PENDING_ROW, .rows = rows});
}

uint64_t ui_editor_scrolls(const UIElement* el) {
    const UIEditor* ed = _seen(el);
    return ed ? ed->scrolls : 0;
}

void ui_editor_link(UIElement* el, UIElement* other) {
    UIEditor* ed = el && el->kind == UI_EDITOR ? el->editor : NULL;
    UIEditor* to = other && other->kind == UI_EDITOR ? other->editor : NULL;
    if (!ed || (other && !to)) {
        log_error("ui_editor_link: not an editor");
        return;
    }
    if (to == ed) {
        log_error("ui_editor_link: an editor linked to itself");
        return;
    }
    _unlink(ed);
    if (!to)
        return;
    _unlink(to);
    ed->partner = to;
    to->partner = ed;
    _lead(to);
}

UIElement* ui_editor_linked(const UIElement* el) {
    const UIEditor* ed = el && el->kind == UI_EDITOR ? el->editor : NULL;
    return ed && ed->partner ? ed->partner->el : NULL;
}

void ui_editor_set_gutter_digits(UIElement* el, int digits) {
    UIEditor* ed = el && el->kind == UI_EDITOR ? el->editor : NULL;
    if (!ed) {
        log_error("ui_editor_set_gutter_digits: not an editor");
        return;
    }
    if (digits < 0 || digits > GUTTER_MOST) {
        log_error("ui_editor_set_gutter_digits: %d digits, past what a line number has", digits);
        return;
    }
    // New columns: a place asked for now waits for the layout that has them.
    if (digits != ed->gutter_digits)
        ed->laid = 0;
    ed->gutter_digits = digits;
}

// ---- Folds

static int _by_fold_first(const void* a, const void* b) {
    const size_t x = ((const UIEditorFold*)a)->first, y = ((const UIEditorFold*)b)->first;
    return x < y ? -1 : x > y ? 1 : 0;
}

void ui_editor_set_folds(UIElement* el, const UIEditorFold* folds, size_t count) {
    UIEditor* ed = _of(el);
    if (!ed) {
        log_error("ui_editor_set_folds: not an editor showing a text");
        return;
    }
    UIEditorFold* fresh = count ? malloc(count * sizeof(UIEditorFold)) : NULL;
    if (count && (!folds || !fresh)) {
        log_error(folds ? "ui_editor_set_folds: out of memory" : "ui_editor_set_folds: no folds");
        free(fresh);
        return;
    }
    if (count) {
        memcpy(fresh, folds, count * sizeof(UIEditorFold));
        qsort(fresh, count, sizeof(UIEditorFold), _by_fold_first);
    }
    for (size_t i = 0; i < count; i++) {
        const UIEditorFold* f = &fresh[i];
        const bool outside = f->first > f->last || f->last >= ed->doc->line_count;
        if (outside || (i && f->first <= fresh[i - 1].last)) {
            log_error("ui_editor_set_folds: the fold over lines %zu to %zu %s", f->first, f->last,
                      outside ? "is not lines of the text" : "overlaps another");
            free(fresh);
            return;
        }
    }
    for (size_t i = 0; i < count; i++)
        fresh[i].label = fresh[i].label ? safe_strdup(fresh[i].label) : NULL;
    // The rows from the first fold line, old or new, are summed again.
    UIEditorPlace* at = &ed->at;
    if (at->fold_count)
        _tops_from(at, at->folds[0].first);
    if (count)
        _tops_from(at, fresh[0].first);
    const bool was_folded = _closed(at, at->top_line) != NULL;
    _folds_free(at->folds, at->fold_count);
    at->folds = fresh;
    at->fold_count = count;
    if (!was_folded)
        _top_onto_fold(at);
}

size_t ui_editor_folds(const UIElement* el, const UIEditorFold** out) {
    const UIEditor* ed = _seen(el);
    if (out)
        *out = ed ? ed->at.folds : NULL;
    return ed ? ed->at.fold_count : 0;
}

static void _fold_open(UIEditorPlace* at, UIEditorFold* f, bool open) {
    if (f->open != open)
        _tops_from(at, f->first);
    const bool closing = f->open && !open;
    f->open = open;
    if (closing)
        _top_onto_fold(at);
}

/*
 * A fold of the editor's turned, and the fold of the same index on its partner with it while the
 * two hold as many: a host sets a pair's folds in pairs, the i-th of each over counterpart lines.
 * With counts apart, as after an edit took a fold's lines out on one side and before the host sets
 * them again, the partner's are left alone. Pairing by lines would need the two texts' lines
 * mapped, which is the host's to do. The partner is handed the state the fold took, turned or not,
 * so a pair set apart comes together at the next turn and a `zM` closes both. The partner's turn
 * goes no further.
 */
static void _fold_turn(UIEditor* ed, UIEditorFold* f, bool open) {
    _fold_open(&ed->at, f, open);
    UIEditor* other = ed->partner;
    if (other && other->doc && other->at.fold_count == ed->at.fold_count)
        _fold_open(&other->at, &other->at.folds[f - ed->at.folds], open);
}

bool ui_editor_set_fold_open(UIElement* el, size_t line, bool open) {
    UIEditor* ed = _of(el);
    UIEditorFold* f = ed ? _fold_of(&ed->at, line) : NULL;
    if (f)
        _fold_turn(ed, f, open);
    return f != NULL;
}

// The fold each caret stands in opened (`how` 1), closed (0) or turned the other way (-1), once
// however many carets stand in it; false when none stands in one.
static bool _fold_carets(UIEditor* ed, int how) {
    const UIEditorFold* done = NULL;
    for (size_t i = 0; ed->at.fold_count && i < ed->at.sel_count; i++) {
        UIEditorFold* f = _fold_of(&ed->at, text_doc_line_of(ed->doc, ed->at.sel[i].head));
        if (f && f != done)
            _fold_turn(ed, f, how < 0 ? !f->open : how > 0);
        done = f ? f : done;
    }
    return done != NULL;
}

// Before an edit at the carets: the folds they stand in opened, so what changes is seen -- unless
// the text refuses changes, when nothing will.
static void _open_for_edit(UIEditor* ed) {
    if (!ed->doc->read_only)
        _fold_carets(ed, 1);
}

// ---- Fillers

// A filler as given, and its place among those given, so the last given for a line is found.
typedef struct {
    UIEditorFiller f;
    size_t given;
} Given;

static int _by_line_then_given(const void* a, const void* b) {
    const Given* x = a;
    const Given* y = b;
    if (x->f.line != y->f.line)
        return x->f.line < y->f.line ? -1 : 1;
    return x->given < y->given ? -1 : x->given > y->given ? 1 : 0;
}

void ui_editor_set_fillers(UIElement* el, const UIEditorFiller* fillers, size_t count) {
    UIEditor* ed = _of(el);
    if (!ed || (count && !fillers)) {
        log_error(ed ? "ui_editor_set_fillers: no fillers"
                     : "ui_editor_set_fillers: not an editor showing a text");
        return;
    }
    Given* given = count ? malloc(count * sizeof(Given)) : NULL;
    UIEditorFiller* fresh = count ? malloc(count * sizeof(UIEditorFiller)) : NULL;
    if (count && (!given || !fresh)) {
        log_error("ui_editor_set_fillers: out of memory");
        free(given);
        free(fresh);
        return;
    }
    for (size_t i = 0; i < count; i++)
        given[i] = (Given){fillers[i], i};
    if (count)
        qsort(given, count, sizeof(Given), _by_line_then_given);
    // The last given for each line, which is the one before another line's, but those of no rows;
    // those on the line count stand under the last line, and those past it are sorted last, and
    // left out.
    const size_t lines = ed->doc->line_count;
    size_t kept = 0, i = 0;
    for (; i < count && given[i].f.line <= lines; i++)
        if ((i + 1 == count || given[i + 1].f.line != given[i].f.line) && given[i].f.rows)
            fresh[kept++] = given[i].f;
    if (i < count)
        log_error("ui_editor_set_fillers: fillers past the text's line count are left out");
    free(given);
    // The rows from the first filler line, old or new, are summed again, and those under the text,
    // old or new, from the last line that shows, which counts them.
    UIEditorPlace* at = &ed->at;
    _tops_from_under(at, lines);
    if (at->filler_count)
        _tops_from(at, at->fillers[0].line);
    if (kept)
        _tops_from(at, fresh[0].line);
    free(at->fillers);
    at->fillers = fresh;
    at->filler_count = kept;
    _tops_from_under(at, lines);
}

size_t ui_editor_fillers(const UIElement* el, const UIEditorFiller** out) {
    const UIEditor* ed = _seen(el);
    const bool any = ed && ed->at.filler_count;
    if (out)
        *out = any ? ed->at.fillers : NULL;
    return any ? ed->at.filler_count : 0;
}

// The count each line keeps answers, a guess wrapped first: _place makes every count a guess again
// at new columns or tab stops, and a change makes the lines it rewrote guesses.
size_t ui_editor_line_rows(const UIElement* el, size_t line) {
    UIEditor* ed = _seen(el);
    if (!ed || line >= ed->doc->line_count)
        return 0;
    if (!_laid_out(ed))
        return 1;
    return ed->at.rows[line] ? ed->at.rows[line] : _wrap_line(ed, line).count;
}

// ---- Selections, public

size_t ui_editor_selections(const UIElement* el, const TextSelection** out, size_t* primary) {
    const UIEditor* ed = _seen(el);
    if (out)
        *out = ed ? ed->at.sel : NULL;
    if (primary)
        *primary = ed ? ed->at.primary : 0;
    return ed ? ed->at.sel_count : 0;
}

void ui_editor_set_selections(UIElement* el, const TextSelection* selections, size_t count) {
    UIEditor* ed = _of(el);
    if (!ed) {
        log_error("ui_editor_set_selections: not an editor showing a text");
        return;
    }
    if (!selections || !count) {
        log_error("ui_editor_set_selections: an editor keeps one selection at least");
        return;
    }
    _set(ed, selections, count);
    text_doc_seal_undo(ed->doc);
}

// ---- The pointer

// The word round `offset`, of the class of the character at it -- or before it, at a word's end.
static TextSelection _word_round(const UIEditor* ed, size_t offset) {
    const Line l = _line(ed, offset);
    size_t a = 0, b = 0;
    ui_typing_word_round(l.s, l.len, offset - l.at, &a, &b);
    return (TextSelection){l.at + a, l.at + b};
}

// The line holding `offset`, its line break included.
static TextSelection _line_round(const UIEditor* ed, size_t offset) {
    const Line l = _line(ed, offset);
    const bool last = l.line + 1 >= ed->doc->line_count;
    return (TextSelection){l.at, last ? l.at + l.len : text_doc_line_start(ed->doc, l.line + 1)};
}

static const UIEditorMode* _look(const UIElement* el);

// The mode told that the pointer changed the selections.
static void _pointed(UIElement* el) {
    const UIEditorMode* m = _look(el);
    if (m->pointer)
        m->pointer(el, m->user);
}

// What a run of presses takes at `offset`: a caret, then the word round it, then its line.
static TextSelection _unit(const UIEditor* ed, size_t offset, int presses) {
    return presses >= 3   ? _line_round(ed, offset)
           : presses == 2 ? _word_round(ed, offset)
                          : (TextSelection){offset, offset};
}

// A click places a caret, a second one takes a word and a third a line; Shift extends the primary
// selection and Alt adds a caret. A press on a closed fold's row opens it, its caret on the fold's
// first line.
static void _press(const UISystem* ui, UIElement* el, UIRect content, float x, float y, int mods,
                   double time) {
    (void)ui;
    (void)content;
    UIEditor* ed = _of(el);
    if (!ed)
        return;
    const int presses = ui_typing_press(&ed->presses, time, x, y, 3);
    const TextSelection u = _unit(ed, ui_editor_offset_at(el, x, y), presses);
    // On the fold's own row, past any fillers above it.
    size_t line, row;
    if (ed->row > 0.0f && !_row_at(ed, y, &line, &row) && _closed(&ed->at, line) &&
        row >= _filler_rows(&ed->at, line))
        _fold_turn(ed, _fold_of(&ed->at, line), true);
    ed->grab_from = u.anchor;
    ed->grab_to = u.head;
    if (mods & GLFW_MOD_SHIFT) {
        TextSelection* p = &ed->at.sel[ed->at.primary];
        p->head = p->anchor <= u.anchor ? u.head : u.anchor;
        ed->at.want[ed->at.primary] = -1;
    } else if (!((mods & GLFW_MOD_ALT) && _carets_add(ed, u))) {
        _carets_one(ed, u);
    }
    _normalize(ed);
    ed->caret.moved = true;
    text_doc_seal_undo(ed->doc);
    _pointed(el);
}

// The pointer held after a press: the selection follows it, by the unit the press took, and the
// view follows it past the top or the bottom.
static void _drag(const UISystem* ui, UIElement* el, UIRect content, float x, float y) {
    (void)ui;
    (void)content;
    UIEditor* ed = _of(el);
    if (!ed)
        return;
    if (y < ed->text.y)
        _scroll_rows(ed, -1.0f);
    else if (y > ed->text.y + ed->text.h)
        _scroll_rows(ed, 1.0f);
    const TextSelection u = _unit(ed, ui_editor_offset_at(el, x, y), ed->presses.count);
    // The unit pressed on, held to the text: a host's edit since may have shortened it.
    const size_t from = _edge(ed->doc, ed->grab_from), to = _edge(ed->doc, ed->grab_to);
    TextSelection* p = &ed->at.sel[ed->at.primary];
    if (u.anchor < from)
        *p = (TextSelection){to, u.anchor};
    else
        *p = (TextSelection){from, u.head > to ? u.head : to};
    ed->at.want[ed->at.primary] = -1;
    _normalize(ed);
    ed->caret.moved = true;
    _pointed(el);
}

// ---- Motions

static size_t _line_end(const UIEditor* ed, size_t line) {
    const Line l = _line_at(ed, line);
    return l.at + l.len;
}

static size_t _char_left(const UIEditor* ed, size_t offset) {
    if (offset == 0)
        return 0;
    const Line l = _line(ed, offset);
    if (offset == l.at)
        return _line_end(ed, l.line - 1);
    const size_t b = offset - l.at < l.len ? offset - l.at : l.len;
    const char* p = l.s + b;
    text_utf8_prev(&p, l.s);
    return l.at + (size_t)(p - l.s);
}

static size_t _char_right(UIEditor* ed, size_t offset) {
    if (offset >= ed->doc->length)
        return ed->doc->length;
    const Line l = _line(ed, offset);
    if (offset - l.at >= l.len)
        return text_doc_line_start(ed->doc, l.line + 1);
    size_t i = offset - l.at;
    ui_typing_next(l.s, l.len, &i);
    return l.at + i;
}

// A word move inside its line, or across the line break at either end of it.
static size_t _word_left(const UIEditor* ed, size_t offset) {
    const Line l = _line(ed, offset);
    if (offset == l.at)
        return _char_left(ed, offset);
    return l.at + ui_typing_word_left(l.s, l.len, offset - l.at);
}

static size_t _word_right(UIEditor* ed, size_t offset) {
    const Line l = _line(ed, offset);
    if (offset - l.at >= l.len)
        return _char_right(ed, offset);
    return l.at + ui_typing_word_right(l.s, l.len, offset - l.at);
}

// To the end of this word or the next, whichever the platform's word move does not: vim's `e`.
static size_t _word_end(UIEditor* ed, size_t offset) {
    const Line l = _line(ed, offset);
    if (offset - l.at >= l.len)
        return _char_right(ed, offset);
    return l.at + ui_typing_word_end(l.s, l.len, offset - l.at);
}

// `count` rows up or down from `offset`, at the column `*want`, which is set from the offset when
// it is -1: so a caret moved through a short row comes back to its column. A caret in a closed fold
// keeps the column it has in its line, though it is drawn at the fold's start.
static size_t _rows_moved(UIEditor* ed, size_t offset, int count, int* want) {
    size_t line, row;
    int col;
    _line_place(ed, offset, &line, &row, &col);
    if (*want < 0)
        *want = col;
    _place_of(ed, offset, &line, &row, NULL);
    for (int k = 0; k < (count < 0 ? -count : count); k++) {
        if (!_caret_row(ed, &line, &row, count > 0))
            return count < 0 ? 0 : ed->doc->length;
    }
    return _at_column(ed, line, row, (float)*want);
}

// A line move keeps the column in the line's own count, wrapping aside. A closed fold is one line:
// a move down goes on from its last line, one up lands on its first.
static size_t _lines_moved(UIEditor* ed, size_t offset, int count, int* want) {
    const Line from = _line(ed, offset);
    if (*want < 0)
        *want = _col_after(from.s, from.len, 0, offset - from.at, 0, _tab(ed));
    const size_t last = ed->doc->line_count - 1;
    const UIEditorFold* f = _closed(&ed->at, from.line);
    size_t to = from.line;
    if (count > 0) {
        to = f ? f->last : to;
        for (int k = 0; k < count && to < last; k++)
            to = _next_shown(&ed->at, to);
        to = to < last ? to : last;
    } else {
        to = f ? f->first : to;
        for (int k = 0; k < -count && to > 0; k++)
            to = _prev_shown(&ed->at, to);
    }
    const Line l = _line_at(ed, to);
    return l.at + _byte_near(l.s, l.len, 0, l.len, 0, (float)*want, _tab(ed));
}

// The start and end of the row an offset stands in, in its own line though a fold hides it.
static size_t _row_start(UIEditor* ed, size_t offset) {
    size_t line, row;
    _line_place(ed, offset, &line, &row, NULL);
    return _row_offset(ed, line, row);
}

static size_t _row_end(UIEditor* ed, size_t offset) {
    size_t line, row;
    _line_place(ed, offset, &line, &row, NULL);
    return _at_column(ed, line, row, INFINITY);
}

static size_t _line_first(const UIEditor* ed, size_t offset) {
    const Line l = _line(ed, offset);
    size_t i = 0;
    while (i < l.len && (l.s[i] == ' ' || l.s[i] == '\t'))
        i++;
    return l.at + i;
}

static const char BRACKET_OPEN[] = "([{";
static const char BRACKET_CLOSE[] = ")]}";

// From an opening bracket at `from`, its closer: or from a closer, its opener; SIZE_MAX for none.
// Read a line's content at a time, since no line ending holds a bracket.
static size_t _bracket_match(UIEditor* ed, size_t from) {
    const char c = text_doc_byte(ed->doc, from);
    const char* o = c ? strchr(BRACKET_OPEN, c) : NULL;
    const char* k = c ? strchr(BRACKET_CLOSE, c) : NULL;
    if (!o && !k)
        return SIZE_MAX;
    const char open = o ? c : BRACKET_OPEN[k - BRACKET_CLOSE];
    const char close = o ? BRACKET_CLOSE[o - BRACKET_OPEN] : c;
    const int sign = o ? 1 : -1; // the depth the bracket at `from` opens, walking away from it
    int depth = 0;
    size_t line = text_doc_line_of(ed->doc, from);
    for (Line l = _line_at(ed, line);; l = _line_at(ed, o ? ++line : --line)) {
        if (o) {
            for (size_t i = l.at < from ? from - l.at : 0; i < l.len; i++) {
                depth += l.s[i] == open ? sign : l.s[i] == close ? -sign : 0;
                if (!depth)
                    return l.at + i;
            }
            if (line + 1 >= ed->doc->line_count)
                return SIZE_MAX;
        } else {
            for (size_t i = from - l.at < l.len ? from - l.at + 1 : l.len; i-- > 0;) {
                depth += l.s[i] == open ? sign : l.s[i] == close ? -sign : 0;
                if (!depth)
                    return l.at + i;
            }
            if (!line)
                return SIZE_MAX;
        }
    }
}

static bool _is_word_byte(char c) {
    return ui_typing_class(c) == 2;
}

// What find looks for: the editor's own query, or one a command makes.
typedef struct {
    const char* s;
    size_t len;
    unsigned flags; // UI_EDITOR_FIND_*
} Query;

static Query _query(const UIEditor* ed) {
    return (Query){ed->find, ed->find_len, ed->find_flags};
}

// Whether the query matches at byte `i` of a line.
static bool _find_at(const Query* q, const char* s, size_t len, size_t i) {
    if (i + q->len > len)
        return false;
    for (size_t k = 0; k < q->len; k++) {
        char a = s[i + k], b = q->s[k];
        if (!(q->flags & UI_EDITOR_FIND_CASE)) {
            a = (char)(a >= 'A' && a <= 'Z' ? a + 32 : a);
            b = (char)(b >= 'A' && b <= 'Z' ? b + 32 : b);
        }
        if (a != b)
            return false;
    }
    if (q->flags & UI_EDITOR_FIND_WORD) {
        if (i > 0 && _is_word_byte(s[i - 1]) && _is_word_byte(s[i]))
            return false;
        const size_t e = i + q->len;
        if (e < len && _is_word_byte(s[e]) && _is_word_byte(s[e - 1]))
            return false;
    }
    return true;
}

// The next match after `offset` (or before it), round the text's end; SIZE_MAX for none.
static size_t _find(UIEditor* ed, const Query* q, size_t offset, bool forward) {
    if (!q->len)
        return SIZE_MAX;
    // Every line from the caret's on, round the end, and the caret's own line again whole.
    const size_t lines = ed->doc->line_count;
    const Line first = _line(ed, offset);
    for (size_t step = 0; step <= lines; step++) {
        const size_t line =
            forward ? (first.line + step) % lines : (first.line + lines - step % lines) % lines;
        const Line l = _line_at(ed, line);
        if (forward) {
            const size_t from = step == 0 ? offset - l.at + 1 : 0;
            for (size_t i = from; i + q->len <= l.len; i++)
                if (_find_at(q, l.s, l.len, i))
                    return l.at + i;
        } else {
            const size_t below = step == 0 ? offset - l.at : l.len + 1;
            for (size_t i = below < l.len + 1 ? below : l.len + 1; i-- > 0;)
                if (_find_at(q, l.s, l.len, i))
                    return l.at + i;
        }
    }
    return SIZE_MAX;
}

// A selection's text, NUL-terminated, for the caller to free; *n its length. NULL out of memory.
static char* _selection_text(UIEditor* ed, const TextSelection* s, size_t* n) {
    *n = _sel_max(s) - _sel_min(s);
    char* text = malloc(*n + 1);
    if (text)
        text[text_doc_copy(ed->doc, _sel_min(s), _sel_max(s), text)] = '\0';
    return text;
}

static size_t _page(const UIEditor* ed) {
    const int rows = (int)floorf(_view_rows(ed)) - 1;
    return rows > 1 ? (size_t)rows : 1;
}

// One step of a motion from `offset`, `want` the caret's wanted column.
static size_t _motion_once(UIEditor* ed, UIEditorMotion motion, size_t offset, int* want) {
    switch (motion) {
        case UI_MOTION_CHAR_LEFT:
            return _char_left(ed, offset);
        case UI_MOTION_CHAR_RIGHT:
            return _char_right(ed, offset);
        case UI_MOTION_WORD_LEFT:
            return _word_left(ed, offset);
        case UI_MOTION_WORD_RIGHT:
            return _word_right(ed, offset);
        case UI_MOTION_WORD_END:
            return _word_end(ed, offset);
        case UI_MOTION_ROW_UP:
            return _rows_moved(ed, offset, -1, want);
        case UI_MOTION_ROW_DOWN:
            return _rows_moved(ed, offset, 1, want);
        case UI_MOTION_ROW_START:
            return _row_start(ed, offset);
        case UI_MOTION_ROW_END:
            return _row_end(ed, offset);
        case UI_MOTION_LINE_FIRST:
            return _line_first(ed, offset);
        case UI_MOTION_LINE_UP:
            return _lines_moved(ed, offset, -1, want);
        case UI_MOTION_LINE_DOWN:
            return _lines_moved(ed, offset, 1, want);
        case UI_MOTION_PAGE_UP:
            return _rows_moved(ed, offset, -(int)_page(ed), want);
        case UI_MOTION_PAGE_DOWN:
            return _rows_moved(ed, offset, (int)_page(ed), want);
        case UI_MOTION_DOC_START:
            return 0;
        case UI_MOTION_DOC_END:
            return ed->doc->length;
        case UI_MOTION_BRACKET: {
            size_t at = _bracket_match(ed, offset);
            if (at == SIZE_MAX && offset > 0)
                at = _bracket_match(ed, offset - 1);
            return at == SIZE_MAX ? offset : at;
        }
        case UI_MOTION_FIND_NEXT:
        case UI_MOTION_FIND_PREV: {
            const Query q = _query(ed);
            const size_t at = _find(ed, &q, offset, motion == UI_MOTION_FIND_NEXT);
            return at == SIZE_MAX ? offset : at;
        }
        default:
            return offset;
    }
}

// Whether a motion keeps the caret's wanted column rather than forgetting it.
static bool _keeps_column(UIEditorMotion motion) {
    return motion == UI_MOTION_ROW_UP || motion == UI_MOTION_ROW_DOWN ||
           motion == UI_MOTION_LINE_UP || motion == UI_MOTION_LINE_DOWN ||
           motion == UI_MOTION_PAGE_UP || motion == UI_MOTION_PAGE_DOWN;
}

static size_t _motion(UIEditor* ed, UIEditorMotion motion, size_t offset, int count, int* want) {
    for (int k = 0; k < (count < 1 ? 1 : count); k++)
        offset = _motion_once(ed, motion, offset, want);
    return offset;
}

size_t ui_editor_motion(UIElement* el, UIEditorMotion motion, size_t offset, int count) {
    UIEditor* ed = _of(el);
    if (!ed)
        return offset;
    int want = -1;
    return _motion(ed, motion, offset > ed->doc->length ? ed->doc->length : offset, count, &want);
}

// ---- Text objects

bool ui_editor_object(UIElement* el, UIEditorObject object, size_t offset, TextSelection* out) {
    UIEditor* ed = _of(el);
    if (!ed || !out)
        return false;
    offset = offset > ed->doc->length ? ed->doc->length : offset;
    switch (object) {
        case UI_OBJECT_WORD:
            *out = _word_round(ed, offset);
            return true;
        case UI_OBJECT_LINE:
            *out = _line_round(ed, offset);
            return true;
        case UI_OBJECT_BRACKETS:
        case UI_OBJECT_BRACKETS_INNER: {
            // Back to the innermost opener not closed before the caret, a line at a time, then on
            // to its closer.
            int depth[3] = {0, 0, 0};
            for (size_t line = text_doc_line_of(ed->doc, offset) + 1; line-- > 0;) {
                const Line l = _line_at(ed, line);
                for (size_t i = offset - l.at < l.len ? offset - l.at : l.len; i-- > 0;) {
                    const char* k = l.s[i] ? strchr(BRACKET_CLOSE, l.s[i]) : NULL;
                    const char* o = l.s[i] ? strchr(BRACKET_OPEN, l.s[i]) : NULL;
                    if (k) {
                        depth[k - BRACKET_CLOSE]++;
                    } else if (o && depth[o - BRACKET_OPEN]-- == 0) {
                        const size_t at = l.at + i, close = _bracket_match(ed, at);
                        if (close == SIZE_MAX || close < offset)
                            return false;
                        *out = object == UI_OBJECT_BRACKETS ? (TextSelection){at, close + 1}
                                                            : (TextSelection){at + 1, close};
                        return true;
                    }
                }
            }
            return false;
        }
        case UI_OBJECT_STRING:
        case UI_OBJECT_STRING_INNER: {
            // The line's strings in order, a backslash escaping the byte after it.
            const Line l = _line(ed, offset);
            const size_t at = offset - l.at;
            size_t open = SIZE_MAX;
            for (size_t i = 0; i < l.len; i++) {
                if (open != SIZE_MAX && l.s[i] == '\\') {
                    i++;
                    continue;
                }
                if (l.s[i] != '"')
                    continue;
                if (open == SIZE_MAX) {
                    open = i;
                } else {
                    if (at >= open && at <= i) {
                        *out = object == UI_OBJECT_STRING
                                   ? (TextSelection){l.at + open, l.at + i + 1}
                                   : (TextSelection){l.at + open + 1, l.at + i};
                        return true;
                    }
                    open = SIZE_MAX;
                }
            }
            return false;
        }
        case UI_OBJECT_ALL:
            *out = (TextSelection){0, ed->doc->length};
            return true;
        default:
            return false;
    }
}

// ---- Operators

// The carets moved: their blink restarts, and the view follows the primary.
static void _follow(UIEditor* ed) {
    ed->caret.moved = true;
    ui_editor_reveal(ed->el, ed->at.sel[ed->at.primary].head);
}

// The carets moved by something that is not an edit: they are put in order, and the next typing
// starts an undo step of its own.
static void _after_move(UIEditor* ed) {
    _normalize(ed);
    text_doc_seal_undo(ed->doc);
    _follow(ed);
}

static int _by_edit_start(const void* a, const void* b) {
    const size_t x = ((const TextEdit*)a)->start, y = ((const TextEdit*)b)->start;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Where edits in order, none overlapping, leave the selections. A selection an edit covers -- the
// last edit starting at or before it, when that one reaches its end -- becomes a caret after what
// the edit writes, which is the caret's own edit even where another ends at it. Any other's ends
// are moved through every edit as an actor's. NULL out of memory.
static TextSelection* _landed(const UIEditor* ed, const TextEdit* e, size_t n) {
    TextSelection* out = malloc(ed->at.sel_count * sizeof(TextSelection));
    if (!out)
        return NULL;
    size_t k = 0;        // edits starting at or before the selection
    long long below = 0; // what the edits before e[k - 1] added
    for (size_t i = 0; i < ed->at.sel_count; i++) {
        const TextSelection* s = &ed->at.sel[i];
        const size_t lo = _sel_min(s), hi = _sel_max(s);
        for (; k < n && e[k].start <= lo; k++)
            if (k)
                below += (long long)e[k - 1].len - (long long)(e[k - 1].end - e[k - 1].start);
        if (k && e[k - 1].end >= hi) {
            const size_t to = (size_t)((long long)e[k - 1].start + below) + e[k - 1].len;
            out[i] = (TextSelection){to, to};
            continue;
        }
        out[i] = *s;
        for (size_t j = n; j-- > 0;) {
            const size_t removed = e[j].end - e[j].start;
            out[i].anchor = text_doc_map_offset(out[i].anchor, e[j].start, removed, e[j].len, true);
            out[i].head = text_doc_map_offset(out[i].head, e[j].start, removed, e[j].len, true);
        }
    }
    return out;
}

// Applies edits from every caret as one action, those that overlap merged and those that change
// nothing dropped, leaves the selections `to` -- or each caret after its own edit, with `to` NULL
// -- and follows the primary caret.
static void _edit_to(UIEditor* ed, TextEdit* edits, size_t n, bool typed, const TextSelection* to,
                     size_t to_count, size_t to_primary) {
    if (!n)
        return;
    qsort(edits, n, sizeof(TextEdit), _by_edit_start);
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (edits[i].start == edits[i].end && !edits[i].len)
            continue;
        if (kept && edits[i].start < edits[kept - 1].end) {
            if (edits[i].end > edits[kept - 1].end)
                edits[kept - 1].end = edits[i].end;
            continue;
        }
        edits[kept++] = edits[i];
    }
    if (!kept) {
        // Nothing changes, and the carets still go where they were asked to.
        if (to && to_count) {
            _set_at(ed, to, NULL, to_count, to_primary);
            _follow(ed);
        }
        return;
    }
    if (to) {
        const bool ok = to_count && text_doc_apply(ed->doc, edits, kept, ed, to, to_count, typed);
        // The selections given, held to the text: any part of an action made out of memory
        // leaves the editor's own held to it instead.
        if (ok)
            _set_at(ed, to, NULL, to_count, to_primary);
        else
            _set_at(ed, ed->at.sel, NULL, ed->at.sel_count, ed->at.primary);
        _follow(ed);
        return;
    }
    TextSelection* after = _landed(ed, edits, kept);
    const bool ok =
        text_doc_apply(ed->doc, edits, kept, ed, after, after ? ed->at.sel_count : 0, typed);
    // The action leaves the selections where they landed, in order with the primary kept; any
    // part of an action made out of memory leaves them held to the text.
    for (size_t i = 0; i < ed->at.sel_count; i++) {
        if (ok && after)
            ed->at.sel[i] = after[i];
        ed->at.sel[i] = (TextSelection){_edge(ed->doc, ed->at.sel[i].anchor),
                                        _edge(ed->doc, ed->at.sel[i].head)};
        ed->at.want[i] = -1;
    }
    free(after);
    _merge(ed);
    _follow(ed);
}

static void _edit(UIEditor* ed, TextEdit* edits, size_t n, bool typed) {
    _edit_to(ed, edits, n, typed, NULL, 0, 0);
}

static Engine* _engine(const UIEditor* ed) {
    return ed->ui ? ui_system_engine(ed->ui) : NULL;
}

// The ranges joined by line breaks, onto the clipboard.
static void _copy(UIEditor* ed, const TextEdit* ranges, size_t n) {
    Engine* engine = _engine(ed);
    if (!engine || !n)
        return;
    size_t total = 0;
    for (size_t i = 0; i < n; i++)
        total += ranges[i].end - ranges[i].start + 1;
    char* text = malloc(total + 1);
    if (!text)
        return;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (i)
            text[k++] = '\n';
        k += text_doc_copy(ed->doc, ranges[i].start, ranges[i].end, text + k);
    }
    text[k] = '\0';
    engine_clipboard_set(engine, text);
    free(text);
}

// The text's own line ending, which typed and pasted line breaks are written as.
// An edit over each selection, with no text yet, for the caller to fill and free; NULL out of
// memory.
static TextEdit* _caret_edits(const UIEditor* ed) {
    TextEdit* e = calloc(ed->at.sel_count, sizeof(TextEdit));
    for (size_t i = 0; e && i < ed->at.sel_count; i++)
        e[i] = (TextEdit){_sel_min(&ed->at.sel[i]), _sel_max(&ed->at.sel[i]), "", 0};
    return e;
}

static void _paste(UIEditor* ed) {
    Engine* engine = _engine(ed);
    if (!engine)
        return;
    const char* clip = engine_clipboard_get(engine);
    const size_t n = strlen(clip);
    char* clean = malloc(n + 1);
    if (!clean)
        return;
    size_t len = ui_typing_clean(clip, n, clean, true, true);
    // As many lines as carets: one to each, a trailing line break aside.
    size_t lines = 1;
    for (size_t i = 0; i < len; i++)
        lines += clean[i] == '\n' && i + 1 < len;
    const bool split = ed->at.sel_count > 1 && lines == ed->at.sel_count;
    TextEdit* edits = _caret_edits(ed);
    char** texts = calloc(ed->at.sel_count, sizeof(char*));
    if (!edits || !texts) {
        free(edits);
        free(texts);
        free(clean);
        return;
    }
    size_t from = 0;
    for (size_t i = 0; i < ed->at.sel_count; i++) {
        size_t piece = len, start = 0;
        if (split) {
            const char* nl = memchr(clean + from, '\n', len - from);
            start = from;
            piece = (nl ? (size_t)(nl - clean) : len) - from;
            from += piece + 1;
        }
        texts[i] = text_doc_with_newlines(ed->doc, clean + start, piece, &edits[i].len);
        edits[i].text = texts[i] ? texts[i] : "";
        edits[i].len = texts[i] ? edits[i].len : 0;
    }
    _edit(ed, edits, ed->at.sel_count, false);
    for (size_t i = 0; i < ed->at.sel_count; i++)
        free(texts[i]);
    free(texts);
    free(edits);
    free(clean);
}

// The lines the carets' selections touch, a tab stop in or out.
static void _indent(UIEditor* ed, bool in) {
    TextEdit* edits = NULL;
    size_t n = 0, cap = 0;
    size_t last = SIZE_MAX;
    const int tab = _tab(ed);
    for (size_t i = 0; i < ed->at.sel_count; i++) {
        const size_t lo = _sel_min(&ed->at.sel[i]), hi = _sel_max(&ed->at.sel[i]);
        size_t first = text_doc_line_of(ed->doc, lo), end = text_doc_line_of(ed->doc, hi);
        if (hi > lo && hi == text_doc_line_start(ed->doc, end) && end > first)
            end--;
        for (size_t line = first; line <= end; line++) {
            if (last != SIZE_MAX && line <= last)
                continue;
            last = line;
            const Line l = _line_at(ed, line);
            size_t cut = 0;
            if (!in) {
                if (l.len && l.s[0] == '\t')
                    cut = 1;
                else
                    while (cut < l.len && cut < (size_t)tab && l.s[cut] == ' ')
                        cut++;
                if (!cut)
                    continue;
            }
            if (!grow_array((void**)&edits, &cap, n + 1, sizeof(TextEdit), 16))
                break;
            edits[n++] = in ? (TextEdit){l.at, l.at, SPACES, (size_t)tab}
                            : (TextEdit){l.at, l.at + cut, "", 0};
        }
    }
    _edit(ed, edits, n, false);
    free(edits);
}

void ui_editor_apply(UIElement* el, UIEditorOperator op, UIEditorMotion motion, int count) {
    UIEditor* ed = _of(el);
    if (!ed)
        return;
    // An operator that changes the text opens the folds at the carets before it finds its range.
    if (op != UI_OP_MOVE && op != UI_OP_EXTEND && op != UI_OP_SELECT && op != UI_OP_COPY)
        _open_for_edit(ed);
    if (op == UI_OP_PASTE) {
        _paste(ed);
        return;
    }
    if (op == UI_OP_INDENT || op == UI_OP_OUTDENT) {
        _indent(ed, op == UI_OP_INDENT);
        return;
    }
    if (op == UI_OP_MOVE || op == UI_OP_EXTEND || op == UI_OP_SELECT) {
        for (size_t i = 0; i < ed->at.sel_count; i++) {
            TextSelection* s = &ed->at.sel[i];
            int* want = &ed->at.want[i];
            if (!_keeps_column(motion))
                *want = -1;
            if (motion == UI_MOTION_NONE) {
                if (op == UI_OP_MOVE)
                    s->anchor = s->head;
                continue;
            }
            // Left or right with a selection lets go of it at that side, as everywhere.
            if (op == UI_OP_MOVE && s->anchor != s->head &&
                (motion == UI_MOTION_CHAR_LEFT || motion == UI_MOTION_CHAR_RIGHT)) {
                s->head = s->anchor = motion == UI_MOTION_CHAR_LEFT ? _sel_min(s) : _sel_max(s);
                continue;
            }
            const size_t to = _motion(ed, motion, s->head, count, want);
            if (op == UI_OP_MOVE)
                *s = (TextSelection){to, to};
            else if (op == UI_OP_EXTEND)
                s->head = to;
            else
                *s = s->head < to ? (TextSelection){s->head, to} : (TextSelection){to, s->head};
        }
        _after_move(ed);
        return;
    }
    // Delete, cut and copy: each selection, or where there is none what the motion covers.
    TextEdit* ranges = calloc(ed->at.sel_count, sizeof(TextEdit));
    if (!ranges)
        return;
    size_t n = 0;
    for (size_t i = 0; i < ed->at.sel_count; i++) {
        const TextSelection* s = &ed->at.sel[i];
        size_t lo = _sel_min(s), hi = _sel_max(s);
        if (lo == hi && motion != UI_MOTION_NONE) {
            int want = -1;
            const size_t to = _motion(ed, motion, s->head, count, &want);
            lo = to < s->head ? to : s->head;
            hi = to < s->head ? s->head : to;
        }
        if (hi > lo)
            ranges[n++] = (TextEdit){lo, hi, "", 0};
    }
    if (op == UI_OP_COPY || op == UI_OP_CUT)
        _copy(ed, ranges, n);
    if (op == UI_OP_DELETE || op == UI_OP_CUT)
        _edit(ed, ranges, n, false);
    free(ranges);
}

static void _type(UIEditor* ed, const char* text, size_t len, bool typed) {
    _open_for_edit(ed);
    TextEdit* edits = _caret_edits(ed);
    if (!edits)
        return;
    for (size_t i = 0; i < ed->at.sel_count; i++) {
        edits[i].text = text;
        edits[i].len = len;
    }
    _edit(ed, edits, ed->at.sel_count, typed);
    free(edits);
}

void ui_editor_type(UIElement* el, const char* text) {
    UIEditor* ed = _of(el);
    if (!ed || !text || !*text)
        return;
    size_t len = 0;
    char* t = text_doc_with_newlines(ed->doc, text, strlen(text), &len);
    if (t)
        _type(ed, t, len, true);
    free(t);
}

// ---- Commands

typedef struct {
    UIEditorOperator op;
    UIEditorMotion motion;
} Step;

static bool _run_step(UIElement* el, void* user) {
    const Step* s = user;
    ui_editor_apply(el, s->op, s->motion, 1);
    return true;
}

// Home: the line's first character that is not a blank, then the row's start, back and forth.
static bool _home(UIElement* el, bool extend) {
    UIEditor* ed = _of(el);
    if (!ed)
        return false;
    for (size_t i = 0; i < ed->at.sel_count; i++) {
        TextSelection* s = &ed->at.sel[i];
        const size_t row = _row_start(ed, s->head), first = _line_first(ed, s->head);
        const size_t line_at = text_doc_line_start(ed->doc, text_doc_line_of(ed->doc, s->head));
        size_t to = row;
        if (row == line_at && first != s->head && first <= _row_end(ed, s->head))
            to = first;
        s->head = to;
        if (!extend)
            s->anchor = to;
        ed->at.want[i] = -1;
    }
    _after_move(ed);
    return true;
}

static bool _run_home(UIElement* el, void* user) {
    (void)user;
    return _home(el, false);
}

static bool _run_select_home(UIElement* el, void* user) {
    (void)user;
    return _home(el, true);
}

// Enter: a line break at each caret, the new line indented as the caret's line is up to the caret.
static bool _run_newline(UIElement* el, void* user) {
    (void)user;
    UIEditor* ed = _of(el);
    if (!ed)
        return false;
    _open_for_edit(ed);
    TextEdit* edits = _caret_edits(ed);
    char** texts = calloc(ed->at.sel_count, sizeof(char*));
    if (!edits || !texts) {
        free(edits);
        free(texts);
        return false;
    }
    const char* nl = text_doc_newline(ed->doc);
    const size_t nl_len = strlen(nl);
    for (size_t i = 0; i < ed->at.sel_count; i++) {
        const Line l = _line(ed, edits[i].start);
        size_t indent = 0;
        while (indent < l.len && indent < edits[i].start - l.at &&
               (l.s[indent] == ' ' || l.s[indent] == '\t'))
            indent++;
        texts[i] = malloc(nl_len + indent);
        if (!texts[i]) {
            edits[i].end = edits[i].start; // nothing at this caret, out of memory
            continue;
        }
        memcpy(texts[i], nl, nl_len);
        memcpy(texts[i] + nl_len, l.s, indent);
        edits[i].text = texts[i];
        edits[i].len = nl_len + indent;
    }
    _edit(ed, edits, ed->at.sel_count, false);
    for (size_t i = 0; i < ed->at.sel_count; i++)
        free(texts[i]);
    free(texts);
    free(edits);
    return true;
}

// Tab: the lines a selection spans indented, or spaces to the next tab stop at each caret.
static bool _run_tab(UIElement* el, void* user) {
    (void)user;
    UIEditor* ed = _of(el);
    if (!ed)
        return false;
    _open_for_edit(ed);
    for (size_t i = 0; i < ed->at.sel_count; i++) {
        const TextSelection* s = &ed->at.sel[i];
        if (text_doc_line_of(ed->doc, s->anchor) != text_doc_line_of(ed->doc, s->head)) {
            _indent(ed, true);
            return true;
        }
    }
    const int tab = _tab(ed);
    TextEdit* edits = _caret_edits(ed);
    if (!edits)
        return false;
    for (size_t i = 0; i < ed->at.sel_count; i++) {
        const Line l = _line(ed, edits[i].start);
        const int col = _col_after(l.s, l.len, 0, edits[i].start - l.at, 0, tab);
        edits[i].text = SPACES;
        edits[i].len = (size_t)(tab - col % tab);
    }
    _edit(ed, edits, ed->at.sel_count, false);
    free(edits);
    return true;
}

static bool _undo(UIElement* el, bool redo) {
    UIEditor* ed = _of(el);
    if (!ed)
        return false;
    const bool done = redo ? text_doc_redo_in(ed->doc, ed) : text_doc_undo_in(ed->doc, ed);
    if (done)
        _follow(ed);
    return done;
}

static bool _run_undo(UIElement* el, void* user) {
    (void)user;
    return _undo(el, false);
}

static bool _run_redo(UIElement* el, void* user) {
    (void)user;
    return _undo(el, true);
}

// A selection added as the primary, unless one is already there.
static bool _add_selection(UIEditor* ed, TextSelection s) {
    for (size_t i = 0; i < ed->at.sel_count; i++)
        if (_sel_min(&ed->at.sel[i]) == _sel_min(&s) && _sel_max(&ed->at.sel[i]) == _sel_max(&s))
            return false;
    if (!_carets_add(ed, s))
        return false;
    _after_move(ed);
    return true;
}

// Cmd+D: with nothing selected the word at the caret; then the next match of the selection.
static bool _run_add_next(UIElement* el, void* user) {
    (void)user;
    UIEditor* ed = _of(el);
    if (!ed)
        return false;
    TextSelection* p = &ed->at.sel[ed->at.primary];
    if (p->anchor == p->head) {
        *p = _word_round(ed, p->head);
        _after_move(ed);
        return true;
    }
    size_t n = 0;
    char* text = _selection_text(ed, p, &n);
    if (!text)
        return false;
    const Query q = {text, n, UI_EDITOR_FIND_CASE};
    const size_t after = _sel_max(&ed->at.sel[ed->at.sel_count - 1]);
    const size_t at = _find(ed, &q, after > 0 ? after - 1 : 0, true);
    free(text);
    return at != SIZE_MAX && _add_selection(ed, (TextSelection){at, at + n});
}

// A caret a row above the topmost, or below the bottommost, at the primary's column.
static bool _add_row(UIElement* el, bool below) {
    UIEditor* ed = _of(el);
    if (!ed)
        return false;
    const TextSelection* edge = &ed->at.sel[below ? ed->at.sel_count - 1 : 0];
    int want = ed->at.want[ed->at.primary];
    size_t line, row;
    if (want < 0)
        _line_place(ed, ed->at.sel[ed->at.primary].head, &line, &row, &want);
    _place_of(ed, edge->head, &line, &row, NULL);
    if (!_caret_row(ed, &line, &row, below))
        return false;
    const size_t at = _at_column(ed, line, row, (float)want);
    const bool added = _add_selection(ed, (TextSelection){at, at});
    if (added)
        ed->at.want[ed->at.primary] = want;
    return added;
}

static bool _run_add_above(UIElement* el, void* user) {
    (void)user;
    return _add_row(el, false);
}

static bool _run_add_below(UIElement* el, void* user) {
    (void)user;
    return _add_row(el, true);
}

// Esc: one caret, the primary; with one already, nothing, so the key falls to the screen.
static bool _run_single(UIElement* el, void* user) {
    (void)user;
    UIEditor* ed = _of(el);
    if (!ed || ed->at.sel_count < 2)
        return false;
    const int want = ed->at.want[ed->at.primary];
    _carets_one(ed, ed->at.sel[ed->at.primary]);
    ed->at.want[0] = want;
    _after_move(ed);
    return true;
}

// The next or the previous match, selected; with no query, the selection's text is the query.
static bool _find_step(UIElement* el, bool forward) {
    UIEditor* ed = _of(el);
    if (!ed)
        return false;
    TextSelection* p = &ed->at.sel[ed->at.primary];
    if (!ed->find_len && p->anchor != p->head) {
        size_t n = 0;
        char* text = _selection_text(ed, p, &n);
        if (!text)
            return false;
        free(ed->find);
        ed->find = text;
        ed->find_len = n;
        ed->find_flags = UI_EDITOR_FIND_CASE;
    }
    const Query q = _query(ed);
    const size_t from = forward ? _sel_max(p) : _sel_min(p);
    const size_t at = _find(ed, &q, forward ? (from > 0 ? from - 1 : 0) : from, forward);
    if (at == SIZE_MAX)
        return false;
    *p = (TextSelection){at, at + ed->find_len};
    _after_move(ed);
    return true;
}

static bool _run_find_next(UIElement* el, void* user) {
    (void)user;
    return _find_step(el, true);
}

static bool _run_find_previous(UIElement* el, void* user) {
    (void)user;
    return _find_step(el, false);
}

static bool _run_select_all(UIElement* el, void* user) {
    (void)user;
    UIEditor* ed = _of(el);
    if (!ed)
        return false;
    _carets_one(ed, (TextSelection){0, ed->doc->length});
    _after_move(ed);
    return true;
}

// The carets' folds turned as `how` says (_fold_carets), the view following the primary caret onto
// what a fold opened or closed.
static bool _turn_folds(UIEditor* ed, int how) {
    const bool any = _fold_carets(ed, how);
    if (any)
        _follow(ed);
    return any;
}

// open-fold, close-fold and toggle-fold, `how` given.
static bool _run_fold(UIElement* el, void* how) {
    UIEditor* ed = _of(el);
    return ed && _turn_folds(ed, *(const int*)how);
}

// A command that applies an operator over a motion at every caret.
#define STEP(name, description, op, motion)     \
    {                                           \
        name, description, _run_step, &(Step) { \
            op, motion                          \
        }                                       \
    }

static const UIEditorCommand COMMANDS[] = {
    STEP("move-left", "Move each caret a character left", UI_OP_MOVE, UI_MOTION_CHAR_LEFT),
    STEP("move-right", "Move each caret a character right", UI_OP_MOVE, UI_MOTION_CHAR_RIGHT),
    STEP("move-word-left", "Move each caret back a word", UI_OP_MOVE, UI_MOTION_WORD_LEFT),
    STEP("move-word-right", "Move each caret on a word", UI_OP_MOVE, UI_MOTION_WORD_RIGHT),
    STEP("move-up", "Move each caret up a row", UI_OP_MOVE, UI_MOTION_ROW_UP),
    STEP("move-down", "Move each caret down a row", UI_OP_MOVE, UI_MOTION_ROW_DOWN),
    {"move-home", "Move each caret to its line's first character, then its row's start", _run_home,
     NULL},
    STEP("move-row-start", "Move each caret to its row's start", UI_OP_MOVE, UI_MOTION_ROW_START),
    STEP("move-row-end", "Move each caret to its row's end", UI_OP_MOVE, UI_MOTION_ROW_END),
    STEP("move-page-up", "Move each caret up a page", UI_OP_MOVE, UI_MOTION_PAGE_UP),
    STEP("move-page-down", "Move each caret down a page", UI_OP_MOVE, UI_MOTION_PAGE_DOWN),
    STEP("move-doc-start", "Move each caret to the start of the text", UI_OP_MOVE,
         UI_MOTION_DOC_START),
    STEP("move-doc-end", "Move each caret to the end of the text", UI_OP_MOVE, UI_MOTION_DOC_END),
    STEP("move-bracket", "Move each caret to the bracket matching the one at it", UI_OP_MOVE,
         UI_MOTION_BRACKET),
    STEP("select-left", "Extend each selection a character left", UI_OP_EXTEND,
         UI_MOTION_CHAR_LEFT),
    STEP("select-right", "Extend each selection a character right", UI_OP_EXTEND,
         UI_MOTION_CHAR_RIGHT),
    STEP("select-word-left", "Extend each selection back a word", UI_OP_EXTEND,
         UI_MOTION_WORD_LEFT),
    STEP("select-word-right", "Extend each selection on a word", UI_OP_EXTEND,
         UI_MOTION_WORD_RIGHT),
    STEP("select-up", "Extend each selection up a row", UI_OP_EXTEND, UI_MOTION_ROW_UP),
    STEP("select-down", "Extend each selection down a row", UI_OP_EXTEND, UI_MOTION_ROW_DOWN),
    {"select-home", "Extend each selection to its line's first character, then its row's start",
     _run_select_home, NULL},
    STEP("select-row-start", "Extend each selection to its row's start", UI_OP_EXTEND,
         UI_MOTION_ROW_START),
    STEP("select-row-end", "Extend each selection to its row's end", UI_OP_EXTEND,
         UI_MOTION_ROW_END),
    STEP("select-page-up", "Extend each selection up a page", UI_OP_EXTEND, UI_MOTION_PAGE_UP),
    STEP("select-page-down", "Extend each selection down a page", UI_OP_EXTEND,
         UI_MOTION_PAGE_DOWN),
    STEP("select-doc-start", "Extend each selection to the start of the text", UI_OP_EXTEND,
         UI_MOTION_DOC_START),
    STEP("select-doc-end", "Extend each selection to the end of the text", UI_OP_EXTEND,
         UI_MOTION_DOC_END),
    {"select-all", "Select the whole text", _run_select_all, NULL},
    STEP("delete-left", "Delete the selection, or the character before each caret", UI_OP_DELETE,
         UI_MOTION_CHAR_LEFT),
    STEP("delete-right", "Delete the selection, or the character after each caret", UI_OP_DELETE,
         UI_MOTION_CHAR_RIGHT),
    STEP("delete-word-left", "Delete back a word from each caret", UI_OP_DELETE,
         UI_MOTION_WORD_LEFT),
    STEP("delete-word-right", "Delete on a word from each caret", UI_OP_DELETE,
         UI_MOTION_WORD_RIGHT),
    STEP("delete-row-start", "Delete from each caret to its row's start", UI_OP_DELETE,
         UI_MOTION_ROW_START),
    STEP("delete-row-end", "Delete from each caret to its row's end", UI_OP_DELETE,
         UI_MOTION_ROW_END),
    {"newline", "Start a line at each caret, indented as its line is", _run_newline, NULL},
    {"tab", "Indent the lines a selection spans, or put spaces to the next tab stop", _run_tab,
     NULL},
    STEP("outdent", "Take the lines the selections touch back a tab stop", UI_OP_OUTDENT,
         UI_MOTION_NONE),
    STEP("copy", "Copy the selections", UI_OP_COPY, UI_MOTION_NONE),
    STEP("cut", "Cut the selections", UI_OP_CUT, UI_MOTION_NONE),
    STEP("paste", "Paste over each selection, a line to each caret when there are as many",
         UI_OP_PASTE, UI_MOTION_NONE),
    {"undo", "Take back the last change", _run_undo, NULL},
    {"redo", "Make the last change taken back again", _run_redo, NULL},
    {"add-next-match", "Select the word at the caret, then add the next match of the selection",
     _run_add_next, NULL},
    {"add-caret-above", "Add a caret a row above the topmost", _run_add_above, NULL},
    {"add-caret-below", "Add a caret a row below the bottommost", _run_add_below, NULL},
    {"single-caret", "Keep only the primary caret", _run_single, NULL},
    {"find-next", "Select the next match", _run_find_next, NULL},
    {"find-previous", "Select the previous match", _run_find_previous, NULL},
    {"open-fold", "Open the fold each caret stands in", _run_fold, &(int){1}},
    {"close-fold", "Close the fold each caret stands in", _run_fold, &(int){0}},
    {"toggle-fold", "Open the fold each caret stands in, or close it if open", _run_fold,
     &(int){-1}},
};

#undef STEP

#define COMMAND_COUNT (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

const UIEditorCommand* ui_editor_commands(size_t* count) {
    if (count)
        *count = COMMAND_COUNT;
    return COMMANDS;
}

static const UIEditorCommand* _command(const char* name) {
    for (size_t i = 0; name && i < COMMAND_COUNT; i++)
        if (!strcmp(COMMANDS[i].name, name))
            return &COMMANDS[i];
    return NULL;
}

bool ui_editor_run(UIElement* el, const char* name) {
    const UIEditorCommand* c = _command(name);
    if (!_of(el) || !c)
        return false;
    return c->run(el, c->user);
}

// ---- The standard mode

#define SHIFT GLFW_MOD_SHIFT
#define CTRL  GLFW_MOD_CONTROL
#define ALT   GLFW_MOD_ALT
#define SUPER GLFW_MOD_SUPER
#ifdef __APPLE__
#define SHORTCUT SUPER
#define WORD     ALT
#else
#define SHORTCUT CTRL
#define WORD     CTRL
#endif
// The table, built on first asking from the commands by name.
static UIEditorBinding g_standard_keys[96];
static size_t g_standard_count;
static UIEditorMode g_standard = {.name = "standard", .types = true};

static void _bind(int key, int mods, const char* name) {
    if (g_standard_count < sizeof(g_standard_keys) / sizeof(g_standard_keys[0]))
        g_standard_keys[g_standard_count++] = (UIEditorBinding){key, mods, _command(name)};
}

static void _standard_keys(void) {
    if (g_standard_count)
        return;
    _bind(GLFW_KEY_LEFT, 0, "move-left");
    _bind(GLFW_KEY_RIGHT, 0, "move-right");
    _bind(GLFW_KEY_LEFT, SHIFT, "select-left");
    _bind(GLFW_KEY_RIGHT, SHIFT, "select-right");
    _bind(GLFW_KEY_LEFT, WORD, "move-word-left");
    _bind(GLFW_KEY_RIGHT, WORD, "move-word-right");
    _bind(GLFW_KEY_LEFT, WORD | SHIFT, "select-word-left");
    _bind(GLFW_KEY_RIGHT, WORD | SHIFT, "select-word-right");
    _bind(GLFW_KEY_UP, 0, "move-up");
    _bind(GLFW_KEY_DOWN, 0, "move-down");
    _bind(GLFW_KEY_UP, SHIFT, "select-up");
    _bind(GLFW_KEY_DOWN, SHIFT, "select-down");
    _bind(GLFW_KEY_HOME, 0, "move-home");
    _bind(GLFW_KEY_END, 0, "move-row-end");
    _bind(GLFW_KEY_HOME, SHIFT, "select-home");
    _bind(GLFW_KEY_END, SHIFT, "select-row-end");
    _bind(GLFW_KEY_PAGE_UP, 0, "move-page-up");
    _bind(GLFW_KEY_PAGE_DOWN, 0, "move-page-down");
    _bind(GLFW_KEY_PAGE_UP, SHIFT, "select-page-up");
    _bind(GLFW_KEY_PAGE_DOWN, SHIFT, "select-page-down");
#ifdef __APPLE__
    _bind(GLFW_KEY_LEFT, SUPER, "move-home");
    _bind(GLFW_KEY_RIGHT, SUPER, "move-row-end");
    _bind(GLFW_KEY_LEFT, SUPER | SHIFT, "select-home");
    _bind(GLFW_KEY_RIGHT, SUPER | SHIFT, "select-row-end");
    _bind(GLFW_KEY_UP, SUPER, "move-doc-start");
    _bind(GLFW_KEY_DOWN, SUPER, "move-doc-end");
    _bind(GLFW_KEY_UP, SUPER | SHIFT, "select-doc-start");
    _bind(GLFW_KEY_DOWN, SUPER | SHIFT, "select-doc-end");
    _bind(GLFW_KEY_BACKSPACE, SUPER, "delete-row-start");
    _bind(GLFW_KEY_DELETE, SUPER, "delete-row-end");
    _bind(GLFW_KEY_UP, SUPER | ALT, "add-caret-above");
    _bind(GLFW_KEY_DOWN, SUPER | ALT, "add-caret-below");
    _bind(GLFW_KEY_G, SUPER, "find-next");
    _bind(GLFW_KEY_G, SUPER | SHIFT, "find-previous");
#else
    _bind(GLFW_KEY_HOME, CTRL, "move-doc-start");
    _bind(GLFW_KEY_END, CTRL, "move-doc-end");
    _bind(GLFW_KEY_HOME, CTRL | SHIFT, "select-doc-start");
    _bind(GLFW_KEY_END, CTRL | SHIFT, "select-doc-end");
    _bind(GLFW_KEY_Y, CTRL, "redo");
    _bind(GLFW_KEY_UP, CTRL | ALT, "add-caret-above");
    _bind(GLFW_KEY_DOWN, CTRL | ALT, "add-caret-below");
    _bind(GLFW_KEY_F3, 0, "find-next");
    _bind(GLFW_KEY_F3, SHIFT, "find-previous");
#endif
    _bind(GLFW_KEY_BACKSPACE, 0, "delete-left");
    _bind(GLFW_KEY_BACKSPACE, SHIFT, "delete-left");
    _bind(GLFW_KEY_DELETE, 0, "delete-right");
    _bind(GLFW_KEY_BACKSPACE, WORD, "delete-word-left");
    _bind(GLFW_KEY_DELETE, WORD, "delete-word-right");
    _bind(GLFW_KEY_ENTER, 0, "newline");
    _bind(GLFW_KEY_KP_ENTER, 0, "newline");
    _bind(GLFW_KEY_ENTER, SHIFT, "newline");
    _bind(GLFW_KEY_TAB, 0, "tab");
    _bind(GLFW_KEY_TAB, SHIFT, "outdent");
    _bind(GLFW_KEY_A, SHORTCUT, "select-all");
    _bind(GLFW_KEY_C, SHORTCUT, "copy");
    _bind(GLFW_KEY_X, SHORTCUT, "cut");
    _bind(GLFW_KEY_V, SHORTCUT, "paste");
    _bind(GLFW_KEY_Z, SHORTCUT, "undo");
    _bind(GLFW_KEY_Z, SHORTCUT | SHIFT, "redo");
    _bind(GLFW_KEY_D, SHORTCUT, "add-next-match");
    _bind(GLFW_KEY_ESCAPE, 0, "single-caret");
    g_standard.bindings = g_standard_keys;
    g_standard.binding_count = g_standard_count;
}

const UIEditorMode* ui_editor_standard_mode(void) {
    _standard_keys();
    return &g_standard;
}

void ui_editor_set_mode(UIElement* el, const UIEditorMode* mode) {
    if (!el || el->kind != UI_EDITOR || !el->editor) {
        log_error("ui_editor_set_mode: not an editor");
        return;
    }
    el->editor->mode = mode;
}

const UIEditorMode* ui_editor_mode(const UIElement* el) {
    const UIEditor* ed = _seen(el);
    return ed && ed->mode ? ed->mode : ui_editor_standard_mode();
}

// What the editor reads of its mode besides the keys: the first mode up the chain that is not a
// layer.
static const UIEditorMode* _look_of(const UIEditor* ed) {
    const UIEditorMode* m = ed && ed->mode ? ed->mode : ui_editor_standard_mode();
    while (m->layer && m->parent)
        m = m->parent;
    return m;
}

static const UIEditorMode* _look(const UIElement* el) {
    return _look_of(_seen(el));
}

void ui_editor_find(UIElement* el, const char* query, unsigned flags) {
    UIEditor* ed = _of(el);
    if (!ed)
        return;
    free(ed->find);
    ed->find = query && *query ? safe_strdup(query) : NULL;
    ed->find_len = ed->find ? strlen(ed->find) : 0;
    ed->find_flags = flags;
}

// ---- Keys

// Each mode up the chain, its own reader first and then its table, a binding whose command did
// nothing handing the key on; then, in a mode that types, a character typed at every caret.
static bool _walk(UIElement* el, UIEditor* ed, const InputKeyEvent* ev) {
    const int mods = ev->mods & (SHIFT | CTRL | ALT | SUPER);
    for (const UIEditorMode* m = ui_editor_mode(el); m; m = m->parent) {
        if (m->read_key && m->read_key(el, ev, m->user))
            return true;
        for (size_t i = 0; ev->key && i < m->binding_count; i++) {
            const UIEditorBinding* b = &m->bindings[i];
            if (b->key == ev->key && b->mods == mods && b->command) {
                if (b->command->run(el, b->command->user))
                    return true;
                break;
            }
        }
    }
    if (ev->codepoint && _look(el)->types && ev->codepoint >= 0x20 && ev->codepoint != 0x7f) {
        char utf8[4];
        const size_t n = text_utf8_encode(ev->codepoint, utf8);
        if (n)
            _type(ed, utf8, n, true);
        return true;
    }
    return false;
}

// A key or a character through the mode. A key the mode used takes the character its press makes
// with it, which the UI's key routing sees to: a binding on Space types no space.
static bool _key(const UISystem* ui, UIElement* el, UIRect content, const InputKeyEvent* ev) {
    (void)ui;
    (void)content;
    UIEditor* ed = _of(el);
    return ed && ev && _walk(el, ed, ev);
}

#undef SHIFT
#undef CTRL
#undef ALT
#undef SUPER
#undef SHORTCUT
#undef WORD

// ---- Drawing

// One character's glyph in its cell, `base` the row's baseline in pixels; one the face lacks is a
// box a cell wide, as the UI draws a character its atlas lacks.
static void _glyph(UIDrawList* dl, const UIEditor* ed, uint32_t cp, float x_px, float base_px,
                   const float* colour) {
    GlyphBitmap g;
    if (!glyph_cache_get(ed->face, cp, &g))
        return;
    if (g.missing) {
        const float w = (float)ed->face->cell_width / ed->px, h = (float)ed->face->ascent / ed->px;
        const UIRect box = {x_px / ed->px + w * 0.15f, base_px / ed->px - h * 0.8f, w * 0.7f,
                            h * 0.8f};
        ui_draw_rounded(dl, box, 0.0f, (vec4){0.0f, 0.0f, 0.0f, 0.0f}, (float*)colour,
                        1.0f / ed->px);
        return;
    }
    if (!g.width)
        return;
    const UIRect r = {(x_px + (float)g.left) / ed->px, (base_px + (float)g.top) / ed->px,
                      (float)g.width / ed->px, (float)g.height / ed->px};
    ui_draw_textured_quad_uv(dl, r, &ed->face->atlas, g.uv, (float*)colour);
}

static const UIEditorStyle* _style(const UITheme* t, uint16_t i) {
    return i < UI_EDITOR_STYLES ? &t->editor_styles[i] : &t->editor_styles[0];
}

// Whether a range is a closed fold's alone, wholly inside the lines it hides behind its row.
static bool _folded_away(UIEditor* ed, const TextRange* r) {
    const UIEditorFold* f = _closed(&ed->at, text_doc_line_of(ed->doc, r->start));
    return f && r->end <= text_doc_line_start(ed->doc, f->last + 1);
}

// The ranges each layer holds that reach [from, to), up to RANGES_DRAWN of each, into `out`, so a
// screen full of errors does not hide the other layers -- nor do ranges a closed fold hides.
static size_t _ranges_in(UIEditor* ed, size_t from, size_t to, TextRange* out) {
    size_t n = 0;
    for (int layer = 0; layer < TEXT_DOC_LAYERS; layer++) {
        size_t count = 0, taken = 0;
        const TextRange* r = text_doc_ranges(ed->doc, layer, &count);
        for (size_t i = 0; i < count && r[i].start < to && taken < RANGES_DRAWN; i++) {
            if (r[i].end > from && !(ed->at.fold_count && _folded_away(ed, &r[i]))) {
                out[n++] = r[i];
                taken++;
            }
        }
    }
    return n;
}

// Whether a range reaches a line, its bytes from `at` with its line break, to `end`.
static bool _reaches_line(const TextRange* r, size_t at, size_t end) {
    return r->start < end && r->end > at;
}

// The first selection that ends at or after `offset`: the selections are in order and do not
// overlap, so their ends are in order too.
static size_t _first_reaching(const UIEditor* ed, size_t offset) {
    size_t lo = 0, hi = ed->at.sel_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (_sel_max(&ed->at.sel[mid]) < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// What a selection's band covers as the mode takes it: from its start to its end, the character at
// its far end too, or the lines it touches whole with their line breaks. In order with the
// selections, whatever the shape.
static void _drawn(UIEditor* ed, const TextSelection* s, UIEditorSelectionShape shape, size_t* lo,
                   size_t* hi) {
    *lo = _sel_min(s);
    *hi = _sel_max(s);
    if (shape == UI_SELECTION_INCLUSIVE) {
        *hi = _char_right(ed, *hi);
    } else if (shape == UI_SELECTION_LINES) {
        const size_t last = text_doc_line_of(ed->doc, *hi);
        *lo = text_doc_line_start(ed->doc, text_doc_line_of(ed->doc, *lo));
        *hi = text_doc_line_start(ed->doc, last + 1);
    }
}

// A band across row `r` of a line from the column of byte `a` to that of byte `b`, `more` columns
// past it, at `y` and `h` tall.
static void _band(UIDrawList* dl, const UIEditor* ed, const Line* l, Rows rows, size_t r, size_t a,
                  size_t b, int more, float y, float h, const float* colour) {
    const int ca = _column(ed, l, rows, r, a), cb = _column(ed, l, rows, r, b) + more;
    if (cb > ca)
        ui_draw_quad(dl,
                     (UIRect){ed->text.x + (float)ca * ed->cell, y, (float)(cb - ca) * ed->cell, h},
                     (float*)colour);
}

// The part of a range on a row: false when it does not cross it, else its ends in bytes of the
// line, (*a, *b).
static bool _on_row(const TextRange* range, const Line* l, size_t from, size_t to, size_t* a,
                    size_t* b) {
    if (range->end <= l->at + from || range->start >= l->at + to)
        return false;
    *a = range->start > l->at + from ? range->start - l->at : from;
    *b = range->end < l->at + to ? range->end - l->at : to;
    return true;
}

// A closed fold's row at `y`, with no number: `fill` from the gutter's left to the editor's right,
// a selection reaching its lines over the text's part, and its label from the text's left edge in
// the line numbers' colour. Whether a caret stands on any of its lines, drawn at the row's start.
static bool _fold_row(UIDrawList* dl, UIEditor* ed, const UIEditorFold* f, UIRect content, float y,
                      const float* fill, const float* numbers, const float* sel_colour,
                      UIEditorSelectionShape shape) {
    const float right = content.x + content.w;
    ui_draw_quad(dl, (UIRect){content.x, y, right - content.x, ed->row}, (float*)fill);
    const size_t from = text_doc_line_start(ed->doc, f->first);
    const size_t to = text_doc_line_start(ed->doc, f->last + 1);
    bool selected = false, held = false;
    for (size_t k = _first_reaching(ed, from); k < ed->at.sel_count; k++) {
        size_t lo, hi;
        _drawn(ed, &ed->at.sel[k], shape, &lo, &hi);
        if (lo > to)
            break;
        selected = selected || (lo < hi && lo < to && hi > from);
        const size_t line = text_doc_line_of(ed->doc, ed->at.sel[k].head);
        held = held || (line >= f->first && line <= f->last);
    }
    if (selected)
        ui_draw_quad(dl, (UIRect){ed->text.x, y, right - ed->text.x, ed->row}, (float*)sel_colour);
    char count[32];
    const char* label = f->label;
    if (!label) {
        snprintf(count, sizeof(count), "%zu lines", f->last - f->first + 1);
        label = count;
    }
    const size_t len = strlen(label);
    const float base_px = roundf(y * ed->px) + (float)ed->face->ascent;
    const float x_px = roundf(ed->text.x * ed->px);
    int col = 0;
    for (size_t i = 0; i < len;) {
        const uint32_t cp = ui_typing_next(label, len, &i);
        const int w = _width(cp, col, _tab(ed));
        if (cp != ' ' && cp != '\t' && w > 0)
            _glyph(dl, ed, cp, x_px + (float)col * (float)ed->face->cell_width, base_px, numbers);
        col += w;
    }
    return held;
}

// The rows in view: their text, the host's ranges, find's matches, the selections and the carets,
// row by row, a closed fold's lines as its one row. The seconds until it next changes of itself: at
// once while the budget has work left, a line drawn came back uncoloured, or the atlas grew, which
// moves every glyph already drawn.
static double _draw(const UISystem* ui, UIElement* el, const UIStyle* s, UIRect content,
                    bool focused, double now, bool blink) {
    UIEditor* ed = _of(el);
    UIDrawList* dl = ui_draw_list(ui);
    if (!dl)
        return INFINITY;
    ui_draw_rect(dl, el->rect, s);
    if (!ed || !ed->face)
        return INFINITY;
    const UITheme* t = ui_theme(ui);
    TextDoc* doc = ed->doc;
    const int tab = _tab(ed);
    const int atlas_height = ed->face->atlas.height;
    vec4 fg = {s->fg[0], s->fg[1], s->fg[2], s->fg[3]};
    const vec4 dim = {s->fg[0], s->fg[1], s->fg[2], s->fg[3] * 0.38f};
    const float* numbers = t->line_numbers[3] > 0.0f ? t->line_numbers : dim;
    vec4 sel_colour = {t->selection[0], t->selection[1], t->selection[2], t->selection[3]};
    if (sel_colour[3] <= 0.0f)
        glm_vec4_copy((vec4){0.25f, 0.45f, 0.85f, 0.45f}, sel_colour);
    vec4 tint = {sel_colour[0], sel_colour[1], sel_colour[2], sel_colour[3] * 0.5f};
    vec4 filler = {fg[0], fg[1], fg[2], fg[3] * 0.04f};
    if (t->editor_filler[3] > 0.0f)
        glm_vec4_copy((float*)t->editor_filler, filler);
    vec4 fold_fill = {sel_colour[0], sel_colour[1], sel_colour[2], sel_colour[3] / 3.0f};
    if (t->editor_fold[3] > 0.0f)
        glm_vec4_copy((float*)t->editor_fold, fold_fill);

    double wake = INFINITY;
    const bool lit = focused && ui_typing_caret_lit(&ed->caret, now, blink, &wake);
    // A mode may draw its caret a cell wide, and its selections as it takes them.
    const UIEditorMode* mode = _look(el);
    const float caret_w = mode->block_caret ? ed->cell : UI_TYPING_CARET_WIDTH;
    vec4 caret_colour = {fg[0], fg[1], fg[2], mode->block_caret ? 0.45f : fg[3]};
    const Query q = _query(ed);

    size_t line, row;
    _top(ed, &line, &row);
    // Every range reaching the screenful of lines that could show, a closed fold one line; those of
    // them that colour their text, which every character asks about; those that mark the gutter of
    // the lines they reach; and those with a background, the `whole` filling whole lines first,
    // then those banded behind their characters.
    TextRange ranges[RANGES_DRAWN * TEXT_DOC_LAYERS], colouring[RANGES_DRAWN * TEXT_DOC_LAYERS],
        marking[RANGES_DRAWN * TEXT_DOC_LAYERS], backed[RANGES_DRAWN * TEXT_DOC_LAYERS];
    size_t past = line;
    for (size_t k = 0; k <= (size_t)ceilf(_view_rows(ed)) && past < doc->line_count; k++)
        past = _next_shown(&ed->at, past);
    const size_t range_count =
        _ranges_in(ed, text_doc_line_start(doc, line), text_doc_line_start(doc, past), ranges);
    size_t colouring_count = 0, marking_count = 0, backed_count = 0;
    for (size_t k = 0; k < range_count; k++) {
        const UIEditorStyle* st = _style(t, ranges[k].style);
        if (st->text[3] > 0.0f)
            colouring[colouring_count++] = ranges[k];
        if (st->gutter[3] > 0.0f || st->sign)
            marking[marking_count++] = ranges[k];
        if (st->whole_lines && st->background[3] > 0.0f)
            backed[backed_count++] = ranges[k];
    }
    const size_t whole = backed_count;
    for (size_t k = 0; k < range_count; k++) {
        const UIEditorStyle* st = _style(t, ranges[k].style);
        if (!st->whole_lines && st->background[3] > 0.0f)
            backed[backed_count++] = ranges[k];
    }
    const float edge_w = content.x + content.w - ed->text.x;

    ui_push_clip(dl, content);
    const float cell_px = (float)ed->face->cell_width;
    const float text_x_px = roundf(ed->text.x * ed->px);
    const float bottom = ed->text.y + ed->text.h;
    float y = ed->text.y - ed->at.top_frac * ed->row;
    bool uncoloured = false;
    while (y < bottom) {
        // The line's fillers from the row the view starts on, with no number, text or range; its
        // own rows count from after them.
        const size_t fill = _filler_rows(&ed->at, line);
        for (; row < fill && y < bottom; row++, y += ed->row)
            ui_draw_quad(dl, (UIRect){ed->text.x, y, edge_w, ed->row}, filler);
        if (y >= bottom)
            break;
        row -= fill;
        const UIEditorFold* fold = _closed(&ed->at, line);
        size_t own = 1; // the line's own rows, the fillers under the text after them
        if (fold) {
            if (row == 0) {
                if (_fold_row(dl, ed, fold, content, y, fold_fill, numbers, sel_colour,
                              mode->selection) &&
                    lit)
                    ui_draw_quad(dl, (UIRect){_snap(ed, ed->text.x), y, caret_w, ed->row},
                                 caret_colour);
                y += ed->row;
            }
        } else {
            const Line l = _line_at(ed, line);
            const Rows rows = _wrap_line(ed, line);
            own = rows.count;
            const TextSpan* spans = NULL;
            size_t span_count = 0;
            if (!text_doc_line_spans(doc, line, &spans, &span_count) && doc->highlight)
                uncoloured = true;
            const size_t first_sel = _first_reaching(ed, l.at);
            // The whole-line ranges overlapping the line's bytes, its line break counted: their
            // backgrounds go behind each of its rows.
            const size_t line_end = text_doc_line_start(doc, line + 1);
            const float* fills[RANGES_DRAWN * TEXT_DOC_LAYERS];
            size_t fill_count = 0;
            for (size_t k = 0; k < whole; k++)
                if (_reaches_line(&backed[k], l.at, line_end))
                    fills[fill_count++] = _style(t, backed[k].style)->background;
            // The ranges reaching it the same way mark its gutter: their fills behind each of its
            // rows, and the last one's sign on its first.
            const float* gutters[RANGES_DRAWN * TEXT_DOC_LAYERS];
            size_t gutter_count = 0;
            const UIEditorStyle* signer = NULL; // the last style reaching it with a sign
            for (size_t k = 0; k < marking_count; k++) {
                const UIEditorStyle* st = _style(t, marking[k].style);
                if (!_reaches_line(&marking[k], l.at, line_end))
                    continue;
                if (st->gutter[3] > 0.0f)
                    gutters[gutter_count++] = st->gutter;
                if (st->sign)
                    signer = st;
            }
            for (; row < rows.count && y < bottom; row++, y += ed->row) {
                const size_t from = rows.starts[row];
                const size_t to = row + 1 < rows.count ? rows.starts[row + 1] : l.len;
                const bool last_row = row + 1 == rows.count;
                const float base_px = roundf(y * ed->px) + (float)ed->face->ascent;
                const size_t row_to = l.at + to;
                size_t a, b;
                // The gutter's fills on every row, under the number; the number and the sign in the
                // gap after it on the first.
                for (size_t g = 0; g < gutter_count; g++)
                    ui_draw_quad(dl, (UIRect){content.x, y, ed->text.x - content.x, ed->row},
                                 (float*)gutters[g]);
                if (row == 0) {
                    char num[24];
                    const int n = snprintf(num, sizeof(num), "%zu", line + 1);
                    const float gutter_px = roundf(content.x * ed->px);
                    const int digits = ed->digits;
                    float nx = gutter_px + (float)(digits - n) * cell_px;
                    for (int k = 0; k < n; k++, nx += cell_px)
                        _glyph(dl, ed, (uint32_t)num[k], nx, base_px, numbers);
                    if (signer)
                        _glyph(dl, ed, signer->sign, gutter_px + (float)digits * cell_px, base_px,
                               signer->sign_colour[3] > 0.0f ? signer->sign_colour : fg);
                }
                // Backgrounds: the host's whole lines, its other ranges, find's matches, then the
                // selections over them.
                for (size_t f = 0; f < fill_count; f++)
                    ui_draw_quad(dl, (UIRect){ed->text.x, y, edge_w, ed->row}, (float*)fills[f]);
                for (size_t k = whole; k < backed_count; k++)
                    if (_on_row(&backed[k], &l, from, to, &a, &b))
                        _band(dl, ed, &l, rows, row, a, b, 0, y, ed->row,
                              _style(t, backed[k].style)->background);
                for (size_t i = q.len && from >= q.len - 1 ? from - (q.len - 1) : 0;
                     q.len && i + q.len <= l.len && i < to; i++) {
                    if (i + q.len > from && _find_at(&q, l.s, l.len, i))
                        _band(dl, ed, &l, rows, row, i > from ? i : from,
                              i + q.len < to ? i + q.len : to, 0, y, ed->row, tint);
                }
                for (size_t k = first_sel; k < ed->at.sel_count; k++) {
                    size_t lo, hi;
                    _drawn(ed, &ed->at.sel[k], mode->selection, &lo, &hi);
                    if (lo > row_to)
                        break;
                    if (lo == hi || hi < l.at + from || (lo == row_to && !last_row))
                        continue;
                    // A selection running on past the line's end shows its line break as a cell.
                    _band(dl, ed, &l, rows, row, lo > l.at + from ? lo - l.at : from,
                          hi < row_to ? hi - l.at : to, last_row && hi > l.at + l.len, y, ed->row,
                          sel_colour);
                }
                // The characters, in the highlighter's colours, a range's colour over them.
                int col = row ? rows.hang : 0;
                size_t sp = 0;
                for (size_t i = from; i < to;) {
                    const size_t here = i;
                    const uint32_t cp = ui_typing_next(l.s, l.len, &i);
                    const int w = _width(cp, col, tab);
                    const float* colour = fg;
                    while (sp < span_count && spans[sp].start + spans[sp].len <= here)
                        sp++;
                    if (sp < span_count && spans[sp].start <= here) {
                        const UIEditorStyle* st = _style(t, spans[sp].style);
                        if (st->text[3] > 0.0f)
                            colour = st->text;
                    }
                    for (size_t k = 0; k < colouring_count; k++)
                        if (colouring[k].start <= l.at + here && colouring[k].end > l.at + here)
                            colour = _style(t, colouring[k].style)->text;
                    if (cp != ' ' && cp != '\t' && w > 0)
                        _glyph(dl, ed, cp, text_x_px + (float)col * cell_px, base_px, colour);
                    col += w;
                }
                // Lines under the ranges that ask for one.
                for (size_t k = 0; k < range_count; k++) {
                    const UIEditorStyle* st = _style(t, ranges[k].style);
                    if (st->underline[3] > 0.0f && _on_row(&ranges[k], &l, from, to, &a, &b))
                        _band(dl, ed, &l, rows, row, a, b, 0, (base_px + 2.0f) / ed->px,
                              1.0f / ed->px, st->underline);
                }
                // The carets on this row.
                for (size_t k = first_sel;
                     lit && k < ed->at.sel_count && _sel_min(&ed->at.sel[k]) <= row_to; k++) {
                    const size_t h = ed->at.sel[k].head;
                    if (h < l.at + from || h > row_to || (h == row_to && !last_row))
                        continue;
                    const int c = _column(ed, &l, rows, row, h - l.at);
                    ui_draw_quad(
                        dl,
                        (UIRect){_snap(ed, ed->text.x + (float)c * ed->cell), y, caret_w, ed->row},
                        caret_colour);
                }
            }
        }
        const size_t next = _next_shown(&ed->at, line);
        // After the last line that shows, the fillers under the text, from the row the view starts
        // on.
        for (size_t r = row > own ? row - own : 0, under = _under(&ed->at, doc->line_count, line);
             r < under && y < bottom; r++, y += ed->row)
            ui_draw_quad(dl, (UIRect){ed->text.x, y, edge_w, ed->row}, filler);
        if (y >= bottom || next >= doc->line_count)
            break;
        line = next;
        row = 0;
    }
    ed->at.drawn_to = line;
    ui_pop_clip(dl);

    // The rest of the text's rows, and a stateful highlighter's walk, a frame's share at a time.
    if (_settle(ed) || uncoloured || ed->face->atlas.height != atlas_height)
        wake = 0.0;
    return wake;
}

// ---- For a mode built in cetra (ui_editor_internal.h)

TextDoc* ui_editor_doc(const UIElement* el) {
    const UIEditor* ed = _seen(el);
    return ed ? ed->doc : NULL;
}

uint64_t ui_editor_serial(const UIElement* el) {
    return el && el->kind == UI_EDITOR && el->editor ? el->editor->serial : 0;
}

void ui_editor_edit(UIElement* el, TextEdit* edits, size_t n, const TextSelection* after,
                    size_t after_count, size_t primary) {
    UIEditor* ed = _of(el);
    if (ed)
        _edit_to(ed, edits, n, false, after, after_count, primary);
}

void ui_editor_set_carets(UIElement* el, const TextSelection* sels, const int* wants, size_t count,
                          size_t primary) {
    UIEditor* ed = _of(el);
    if (!ed || !sels || !count)
        return;
    _set_at(ed, sels, wants, count, primary);
    text_doc_seal_undo(ed->doc);
    _follow(ed);
}

const int* ui_editor_wants(UIElement* el) {
    const UIEditor* ed = _of(el);
    return ed ? ed->at.want : NULL;
}

Engine* ui_editor_engine(const UIElement* el) {
    const UIEditor* ed = _seen(el);
    return ed ? _engine(ed) : NULL;
}

int ui_editor_rows_shown(const UIElement* el) {
    const UIEditor* ed = _seen(el);
    const int rows = ed ? (int)floorf(_view_rows(ed)) : 1;
    return rows > 1 ? rows : 1;
}

size_t ui_editor_row_at(UIElement* el, int down) {
    UIEditor* ed = _of(el);
    return ed ? _row_down_from_top(ed, down) : 0;
}

void ui_editor_scroll_rows(UIElement* el, float rows) {
    UIEditor* ed = _of(el);
    if (ed)
        _scroll_rows(ed, rows);
}

bool ui_editor_fold_carets(UIElement* el, int how) {
    UIEditor* ed = _of(el);
    return ed && _turn_folds(ed, how);
}

bool ui_editor_closed_fold(const UIElement* el, size_t line, size_t* first, size_t* last) {
    const UIEditor* ed = _seen(el);
    const UIEditorFold* f = ed ? _closed(&ed->at, line) : NULL;
    if (f && first)
        *first = f->first;
    if (f && last)
        *last = f->last;
    return f != NULL;
}

const UIWidgetOps ui_editor_ops = {
    .place = _place,
    .key = _key,
    .press = _press,
    .drag = _drag,
    .draw = _draw,
    .scroll = _scroll,
    .scroll_to = _scroll_to,
    .scroll_by = _scroll_by,
};
