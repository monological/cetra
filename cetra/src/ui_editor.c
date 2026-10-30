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
#include "ui_field.h"
#include "ui_typing.h"
#include "util.h"

// Columns between the line numbers and the text.
#define GUTTER_GAP 2
// The fewest columns the line numbers take, so the gutter does not widen at line 10 and 100.
#define GUTTER_DIGITS 3
// The bar: its width, its inset from the edge, its shortest thumb, the strip a press lands on.
#define BAR_W     6.0f
#define BAR_INSET 2.0f
#define BAR_MIN   24.0f
#define BAR_HIT   14.0f
// Rows a unit of the wheel scrolls.
#define WHEEL_ROWS 3.0f
// The face's size when the theme names none, in points.
#define EDITOR_SIZE 13.0f

typedef struct UIEditor UIEditor;

struct UIEditor {
    UIElement* el;
    TextDoc* doc;
    // The selections, sorted by start, and each one's wanted column for moving by row, -1 for none.
    TextSelection* sel;
    int* want;
    size_t sel_count, sel_capacity, primary;
    // Each line's rows at `cols`, 0 while not wrapped, and the rows above each line, good up to
    // tops_good.
    uint32_t* rows;
    uint64_t* tops;
    size_t line_capacity, tops_capacity, tops_good;
    // The scroll: the line and row at the view's top, and how far that row is scrolled past.
    size_t top_line;
    size_t top_row;
    float top_frac;
    // As the last pass laid it out: pixels a point, a cell and a row in points, the face, where the
    // content and the text are, and the columns.
    float px, cell, row;
    GlyphCache* face;
    UIRect content, text;
    int cols;
    // A press: when and where, how many in a run, and the unit it took, which a drag extends by.
    double press_time;
    float press_x, press_y;
    int press_count;
    size_t grab_from, grab_to;
    bool bar_drag;
    float bar_grab;
    double caret_time;
    bool caret_moved;
    // A line's row starts, scratch.
    size_t* starts;
    size_t starts_capacity;
    // The keys' mode, NULL the standard one; the UI it was last laid out in, for the clipboard; and
    // find's query.
    const UIEditorMode* mode;
    const UISystem* ui;
    char* find;
    size_t find_len;
    unsigned find_flags;
};

// ---- Characters

// The codepoint at s[*i] of a run `len` long, never reading past it, *i moved past it.
static uint32_t _next(const char* s, size_t len, size_t* i) {
    char buf[5] = {0};
    const size_t n = len - *i < 4 ? len - *i : 4;
    memcpy(buf, s + *i, n);
    const char* p = buf;
    const uint32_t cp = text_utf8_next(&p);
    *i += p > buf ? (size_t)(p - buf) : 1;
    return cp;
}

// Columns a character takes at `col`: a tab to the next stop, a byte-order mark none.
static int _width(uint32_t cp, int col, int tab) {
    if (cp == '\t')
        return tab - col % tab;
    return cp == 0xfeff ? 0 : 1;
}

// ---- Wrapping

static int _hang(const char* s, size_t len, int cols, int tab) {
    int col = 0;
    for (size_t i = 0; i < len && (s[i] == ' ' || s[i] == '\t'); i++)
        col += s[i] == '\t' ? tab - col % tab : 1;
    return col * 2 > cols ? 0 : col;
}

size_t ui_editor_wrap(const char* s, size_t len, int cols, int tab, size_t* starts, size_t max,
                      int* hang_out) {
    if (cols < 1)
        cols = 1;
    if (tab < 1)
        tab = 1;
    const int hang = _hang(s, len, cols, tab);
    if (hang_out)
        *hang_out = hang;
    if (max)
        starts[0] = 0;
    size_t rows = 1, row_start = 0, brk = SIZE_MAX, i = 0;
    int col = 0;
    while (i < len) {
        size_t next = i;
        const uint32_t cp = _next(s, len, &next);
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
            col = hang;
            for (size_t k = at; k < i;) {
                const uint32_t c = _next(s, len, &k);
                col += _width(c, col, tab);
            }
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

static size_t _estimate(const UIEditor* ed, size_t line) {
    size_t len = 0;
    text_doc_line(ed->doc, line, &len);
    return len / (size_t)ed->cols + 1;
}

// A line's rows, wrapped now if they were a guess: its row starts in ed->starts, and their count.
static size_t _wrap_line(UIEditor* ed, size_t line, int* hang) {
    size_t len = 0;
    const char* s = text_doc_line(ed->doc, line, &len);
    size_t n =
        ui_editor_wrap(s, len, ed->cols, ed->doc->tab_width, ed->starts, ed->starts_capacity, hang);
    if (n > ed->starts_capacity) {
        if (!grow_array((void**)&ed->starts, &ed->starts_capacity, n, sizeof(size_t), 64))
            n = ed->starts_capacity;
        else
            n = ui_editor_wrap(s, len, ed->cols, ed->doc->tab_width, ed->starts,
                               ed->starts_capacity, hang);
    }
    const uint32_t count = (uint32_t)(n ? n : 1);
    if (ed->rows[line] != count) {
        const size_t was = ed->rows[line] ? ed->rows[line] : _estimate(ed, line);
        if (was != count && ed->tops_good > line)
            ed->tops_good = line;
        ed->rows[line] = count;
    }
    return count;
}

static size_t _rows_of(UIEditor* ed, size_t line) {
    return ed->rows[line] ? ed->rows[line] : _wrap_line(ed, line, NULL);
}

// The rows above `line`, guessed for the lines not wrapped yet.
static uint64_t _top_of(UIEditor* ed, size_t line) {
    for (size_t i = ed->tops_good; i < line; i++)
        ed->tops[i + 1] = ed->tops[i] + (ed->rows[i] ? ed->rows[i] : _estimate(ed, i));
    if (ed->tops_good < line)
        ed->tops_good = line;
    return ed->tops[line];
}

// Moves (line, row) one row down; false at the text's last row.
static bool _row_down(UIEditor* ed, size_t* line, size_t* row) {
    if (*row + 1 < _rows_of(ed, *line)) {
        (*row)++;
        return true;
    }
    if (*line + 1 >= ed->doc->line_count)
        return false;
    (*line)++;
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
    (*line)--;
    *row = _rows_of(ed, *line) - 1;
    return true;
}

// The row of `byte` in a line whose row starts are in ed->starts: at a wrap, the row it begins.
static size_t _row_holding(const UIEditor* ed, size_t count, size_t byte) {
    size_t r = 0;
    while (r + 1 < count && ed->starts[r + 1] <= byte)
        r++;
    return r;
}

// The column `byte` stands at in row `r` of a line.
static int _column(const UIEditor* ed, const char* s, size_t len, size_t r, int hang, size_t byte) {
    int col = r ? hang : 0;
    for (size_t i = ed->starts[r]; i < byte && i < len;) {
        const uint32_t cp = _next(s, len, &i);
        col += _width(cp, col, ed->doc->tab_width);
    }
    return col;
}

static void _scroll_rows(UIEditor* ed, float delta) {
    float t = ed->top_frac + delta;
    while (t >= 1.0f) {
        if (!_row_down(ed, &ed->top_line, &ed->top_row)) {
            t = 0.0f;
            break;
        }
        t -= 1.0f;
    }
    while (t < 0.0f) {
        if (!_row_up(ed, &ed->top_line, &ed->top_row)) {
            t = 0.0f;
            break;
        }
        t += 1.0f;
    }
    ed->top_frac = t;
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
    char byte = 0;
    while (offset > 0 && offset < doc->length && text_doc_copy(doc, offset, offset + 1, &byte) &&
           text_utf8_is_continuation(byte))
        offset--;
    return offset;
}

typedef struct {
    TextSelection s;
    int want;
    bool primary;
} Caret;

static int _by_caret_start(const void* a, const void* b) {
    const size_t x = _sel_min(&((const Caret*)a)->s), y = _sel_min(&((const Caret*)b)->s);
    return x < y ? -1 : x > y ? 1 : 0;
}

// Sorted by start and merged where they overlap, or are carets at one place, the primary followed.
static void _normalize(UIEditor* ed) {
    if (!ed->sel_count)
        return;
    Caret* c = malloc(ed->sel_count * sizeof(Caret));
    if (!c)
        return;
    for (size_t i = 0; i < ed->sel_count; i++)
        c[i] = (Caret){ed->sel[i], ed->want[i], i == ed->primary};
    qsort(c, ed->sel_count, sizeof(Caret), _by_caret_start);
    size_t n = 0;
    for (size_t i = 0; i < ed->sel_count; i++) {
        if (n) {
            Caret* last = &c[n - 1];
            const size_t lo = _sel_min(&c[i].s), hi = _sel_max(&c[i].s);
            const size_t llo = _sel_min(&last->s), lhi = _sel_max(&last->s);
            if (lo < lhi || (lo == lhi && lo == hi && llo == lhi)) {
                const size_t to = hi > lhi ? hi : lhi;
                const bool forward = last->s.head >= last->s.anchor;
                last->s = forward ? (TextSelection){llo, to} : (TextSelection){to, llo};
                last->primary |= c[i].primary;
                continue;
            }
        }
        c[n++] = c[i];
    }
    ed->sel_count = n;
    ed->primary = n - 1;
    for (size_t i = 0; i < n; i++) {
        ed->sel[i] = c[i].s;
        ed->want[i] = c[i].want;
        if (c[i].primary)
            ed->primary = i;
    }
    free(c);
}

static bool _room(UIEditor* ed, size_t count) {
    size_t cap = ed->sel_capacity;
    if (!grow_array((void**)&ed->sel, &ed->sel_capacity, count, sizeof(TextSelection), 4))
        return false;
    return grow_array((void**)&ed->want, &cap, count, sizeof(int), 4);
}

static void _set(UIEditor* ed, const TextSelection* s, size_t count) {
    if (!count || !_room(ed, count))
        return;
    for (size_t i = 0; i < count; i++) {
        ed->sel[i] = (TextSelection){_edge(ed->doc, s[i].anchor), _edge(ed->doc, s[i].head)};
        ed->want[i] = -1;
    }
    ed->sel_count = count;
    ed->primary = count - 1;
    _normalize(ed);
    ed->caret_moved = true;
}

// ---- The text's calls

static void _view_lines(void* view, size_t first, size_t removed, size_t inserted) {
    UIEditor* ed = view;
    const size_t count = ed->doc->line_count;
    const size_t old = count - inserted + removed;
    if (!grow_array((void**)&ed->rows, &ed->line_capacity, count, sizeof(uint32_t), 64) ||
        !grow_array((void**)&ed->tops, &ed->tops_capacity, count + 1, sizeof(uint64_t), 64))
        return;
    memmove(&ed->rows[first + inserted], &ed->rows[first + removed],
            (old - first - removed) * sizeof(uint32_t));
    memset(&ed->rows[first], 0, inserted * sizeof(uint32_t));
    if (ed->tops_good > first)
        ed->tops_good = first;
    // The view keeps its place in the text: a line above the change stays, one below it moves with
    // its lines, and one the change rewrote is held to what replaced it.
    if (ed->top_line >= first + removed) {
        ed->top_line = ed->top_line - removed + inserted;
    } else if (ed->top_line >= first) {
        const size_t into = ed->top_line - first;
        ed->top_line = first + (inserted && into < inserted ? into : (inserted ? inserted - 1 : 0));
    }
    if (ed->top_line >= count) {
        ed->top_line = count - 1;
        ed->top_row = 0;
    }
}

static void _view_map(void* view, size_t start, size_t removed, size_t inserted, bool actor) {
    UIEditor* ed = view;
    for (size_t i = 0; i < ed->sel_count; i++) {
        TextSelection* s = &ed->sel[i];
        const TextSelection was = *s;
        s->anchor = text_doc_map_offset(s->anchor, start, removed, inserted, actor);
        s->head = text_doc_map_offset(s->head, start, removed, inserted, actor);
        if (s->head != was.head)
            ed->want[i] = -1;
    }
    _normalize(ed);
    if (actor)
        ed->caret_moved = true;
}

static size_t _view_selections(void* view, const TextSelection** out) {
    const UIEditor* ed = view;
    *out = ed->sel;
    return ed->sel_count;
}

static void _view_set(void* view, const TextSelection* s, size_t count) {
    _set(view, s, count);
}

UIEditor* ui_editor_make(UIElement* el, TextDoc* doc) {
    UIEditor* ed = calloc(1, sizeof(UIEditor));
    if (!ed)
        return NULL;
    ed->el = el;
    ed->doc = doc;
    ed->cols = 1;
    ed->px = 1.0f;
    const size_t lines = doc->line_count;
    ed->rows = calloc(lines, sizeof(uint32_t));
    ed->tops = calloc(lines + 1, sizeof(uint64_t));
    ed->line_capacity = lines;
    ed->tops_capacity = lines + 1;
    if (!ed->rows || !ed->tops || !_room(ed, 1)) {
        ui_editor_free(ed);
        return NULL;
    }
    ed->sel[0] = (TextSelection){0, 0};
    ed->want[0] = -1;
    ed->sel_count = 1;
    text_doc_add_view(doc, &(TextDocView){.view = ed,
                                          .element = el,
                                          .lines = _view_lines,
                                          .map = _view_map,
                                          .selections = _view_selections,
                                          .set_selections = _view_set});
    return ed;
}

void ui_editor_free(UIEditor* ed) {
    if (!ed)
        return;
    if (ed->doc)
        text_doc_remove_view(ed->doc, ed);
    free(ed->sel);
    free(ed->want);
    free(ed->rows);
    free(ed->tops);
    free(ed->starts);
    free(ed->find);
    free(ed);
}

// ---- Geometry

static float _snap(const UIEditor* ed, float v) {
    return roundf(v * ed->px) / ed->px;
}

static int _digits(size_t n) {
    int d = 1;
    for (; n >= 10; n /= 10)
        d++;
    return d < GUTTER_DIGITS ? GUTTER_DIGITS : d;
}

// The face at its size, the gutter, where the text goes and how many columns a row holds; rows
// wrapped at other columns are guesses again.
static void _layout(UIEditor* ed, const UISystem* ui, UIRect content) {
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
    const float gutter = (float)(_digits(ed->doc->line_count) + GUTTER_GAP) * ed->cell;
    ed->text.x = _snap(ed, content.x + gutter);
    ed->text.y = _snap(ed, content.y);
    ed->text.w = content.x + content.w - BAR_W - BAR_INSET - ed->text.x;
    ed->text.h = content.y + content.h - ed->text.y;
    int cols = (int)floorf(ed->text.w / ed->cell);
    cols = cols < 1 ? 1 : cols;
    if (cols != ed->cols) {
        memset(ed->rows, 0, ed->doc->line_count * sizeof(uint32_t));
        ed->tops_good = 0;
        ed->cols = cols;
    }
    if (ed->top_line >= ed->doc->line_count)
        ed->top_line = ed->doc->line_count - 1;
    const size_t rows = _rows_of(ed, ed->top_line);
    if (ed->top_row >= rows)
        ed->top_row = rows - 1;
}

static float _view_rows(const UIEditor* ed) {
    return ed->row > 0.0f ? ed->text.h / ed->row : 0.0f;
}

// The bar's track and thumb; false while the whole text shows.
static bool _bar(UIEditor* ed, UIRect* track, UIRect* thumb) {
    const uint64_t total = _top_of(ed, ed->doc->line_count);
    const float view = _view_rows(ed);
    if ((float)total <= view)
        return false;
    *track = (UIRect){ed->content.x + ed->content.w - BAR_W - BAR_INSET, ed->content.y + BAR_INSET,
                      BAR_W, ed->content.h - 2.0f * BAR_INSET};
    float h = track->h * view / (float)total;
    h = h < BAR_MIN ? BAR_MIN : (h > track->h ? track->h : h);
    const double at = (double)_top_of(ed, ed->top_line) + (double)ed->top_row + ed->top_frac;
    const double t = total > 1 ? at / (double)(total - 1) : 0.0;
    *thumb = (UIRect){track->x, track->y + (track->h - h) * (float)(t > 1.0 ? 1.0 : t), BAR_W, h};
    return true;
}

// The line holding the global row `row`, and the row in it.
static void _line_at_row(UIEditor* ed, uint64_t row, size_t* line, size_t* in) {
    _top_of(ed, ed->doc->line_count);
    size_t lo = 0, hi = ed->doc->line_count;
    while (hi - lo > 1) {
        const size_t mid = lo + (hi - lo) / 2;
        if (ed->tops[mid] <= row)
            lo = mid;
        else
            hi = mid;
    }
    *line = lo;
    const uint64_t r = row - ed->tops[lo];
    const size_t rows = _rows_of(ed, lo);
    *in = r < rows ? (size_t)r : rows - 1;
}

// ---- Where things are

// The offset a point at (x, y) is nearest, in the rows as last laid out.
size_t ui_editor_offset_at(UIElement* el, float x, float y) {
    UIEditor* ed = el ? el->editor : NULL;
    if (!ed || ed->row <= 0.0f)
        return 0;
    float down = (y - ed->text.y) / ed->row + ed->top_frac;
    size_t line = ed->top_line, row = ed->top_row;
    bool past = false;
    for (; down >= 1.0f; down -= 1.0f) {
        if (!_row_down(ed, &line, &row)) {
            past = true;
            break;
        }
    }
    for (; down < 0.0f; down += 1.0f) {
        if (!_row_up(ed, &line, &row))
            return 0;
    }
    const size_t at = text_doc_line_start(ed->doc, line);
    size_t len = 0;
    const char* s = text_doc_line(ed->doc, line, &len);
    if (past)
        return at + len;
    int hang = 0;
    const size_t count = _wrap_line(ed, line, &hang);
    const size_t from = ed->starts[row], to = row + 1 < count ? ed->starts[row + 1] : len;
    const float want = (x - ed->text.x) / ed->cell;
    int col = row ? hang : 0;
    for (size_t i = from; i < to;) {
        const size_t here = i;
        const uint32_t cp = _next(s, len, &i);
        const int w = _width(cp, col, ed->doc->tab_width);
        if (want < (float)col + (float)w * 0.5f)
            return at + here;
        col += w;
    }
    // Past a wrapped row's end, before its last character, so the caret stays on this row.
    if (row + 1 < count && to > from) {
        const char* p = s + to;
        text_utf8_prev(&p, s + from);
        return at + (size_t)(p - s);
    }
    return at + to;
}

bool ui_editor_point_of(UIElement* el, size_t offset, float* x, float* y) {
    UIEditor* ed = el ? el->editor : NULL;
    if (!ed || ed->row <= 0.0f || offset > ed->doc->length)
        return false;
    const size_t line = text_doc_line_of(ed->doc, offset);
    size_t len = 0;
    const char* s = text_doc_line(ed->doc, line, &len);
    const size_t byte = offset - text_doc_line_start(ed->doc, line);
    int hang = 0;
    const size_t count = _wrap_line(ed, line, &hang);
    const size_t row = _row_holding(ed, count, byte);
    const int col = _column(ed, s, len, row, hang, byte);
    // Rows down from the view's top, while on screen.
    size_t l = ed->top_line, r = ed->top_row;
    const int view = (int)ceilf(_view_rows(ed)) + 1;
    for (int down = 0; down <= view; down++) {
        if (l == line && r == row) {
            *x = ed->text.x + (float)col * ed->cell;
            *y = ed->text.y + ((float)down - ed->top_frac) * ed->row;
            return true;
        }
        if (!_row_down(ed, &l, &r))
            break;
    }
    return false;
}

void ui_editor_top(const UIElement* el, size_t* line, size_t* row, int* columns) {
    const UIEditor* ed = el ? el->editor : NULL;
    *line = ed ? ed->top_line : 0;
    *row = ed ? ed->top_row : 0;
    *columns = ed ? ed->cols : 0;
}

void ui_editor_reveal(UIElement* el, size_t offset) {
    UIEditor* ed = el ? el->editor : NULL;
    if (!ed || ed->row <= 0.0f)
        return;
    offset = offset > ed->doc->length ? ed->doc->length : offset;
    const size_t line = text_doc_line_of(ed->doc, offset);
    size_t len = 0;
    text_doc_line(ed->doc, line, &len);
    const size_t count = _wrap_line(ed, line, NULL);
    const size_t row = _row_holding(ed, count, offset - text_doc_line_start(ed->doc, line));
    // Above the view's top: that row becomes the top.
    if (line < ed->top_line || (line == ed->top_line && row <= ed->top_row)) {
        ed->top_line = line;
        ed->top_row = row;
        ed->top_frac = 0.0f;
        return;
    }
    // Inside the rows wholly shown: nothing to do. Below them, or far below: the row becomes the
    // last wholly shown.
    const int shown = (int)floorf(_view_rows(ed));
    const int last = shown > 0 ? shown - 1 : 0;
    size_t l = ed->top_line, r = ed->top_row;
    for (int down = 0; down <= last; down++) {
        if (l == line && r == row) {
            if (down < last || ed->top_frac == 0.0f)
                return;
            break;
        }
        if (!_row_down(ed, &l, &r))
            break;
    }
    l = line;
    r = row;
    for (int up = 0; up < last; up++)
        if (!_row_up(ed, &l, &r))
            break;
    ed->top_line = l;
    ed->top_row = r;
    ed->top_frac = 0.0f;
}

// ---- Selections, public

size_t ui_editor_selections(const UIElement* el, const TextSelection** out, size_t* primary) {
    const UIEditor* ed = el && el->kind == UI_EDITOR ? el->editor : NULL;
    if (out)
        *out = ed ? ed->sel : NULL;
    if (primary)
        *primary = ed ? ed->primary : 0;
    return ed ? ed->sel_count : 0;
}

void ui_editor_set_selections(UIElement* el, const TextSelection* selections, size_t count) {
    if (!el || el->kind != UI_EDITOR || !el->editor) {
        log_error("ui_editor_set_selections: not an editor");
        return;
    }
    if (!selections || !count) {
        log_error("ui_editor_set_selections: an editor keeps one selection at least");
        return;
    }
    _set(el->editor, selections, count);
    text_doc_seal_undo(el->editor->doc);
}

// ---- The pointer

// The word or the line round `offset`, by the press count: one press neither.
static void _unit(UIEditor* ed, size_t offset, int count, size_t* from, size_t* to) {
    *from = *to = offset;
    if (count < 2)
        return;
    const size_t line = text_doc_line_of(ed->doc, offset);
    const size_t at = text_doc_line_start(ed->doc, line);
    size_t len = 0;
    const char* s = text_doc_line(ed->doc, line, &len);
    if (count >= 3) {
        *from = at;
        *to = line + 1 < ed->doc->line_count ? text_doc_line_start(ed->doc, line + 1) : at + len;
        return;
    }
    size_t a = 0, b = 0;
    ui_typing_word_round(s, len, offset - at, &a, &b);
    *from = at + a;
    *to = at + b;
}

void ui_editor_press(const UISystem* ui, UIElement* el, UIRect content, float x, float y, int mods,
                     double time) {
    UIEditor* ed = el ? el->editor : NULL;
    if (!ed)
        return;
    _layout(ed, ui, content);
    UIRect track, thumb;
    if (x >= content.x + content.w - BAR_HIT && _bar(ed, &track, &thumb)) {
        if (y >= thumb.y && y < thumb.y + thumb.h) {
            ed->bar_drag = true;
            ed->bar_grab = y - thumb.y;
        } else {
            const float page = _view_rows(ed) * 0.9f;
            _scroll_rows(ed, y < thumb.y ? -page : page);
        }
        return;
    }
    const bool again = ui_typing_again(time, ed->press_time, x, y, ed->press_x, ed->press_y);
    ed->press_count = again ? (ed->press_count % 3) + 1 : 1;
    ed->press_time = time;
    ed->press_x = x;
    ed->press_y = y;
    const size_t at = ui_editor_offset_at(el, x, y);
    size_t from, to;
    _unit(ed, at, ed->press_count, &from, &to);
    ed->grab_from = from;
    ed->grab_to = to;
    if (mods & GLFW_MOD_SHIFT) {
        TextSelection* p = &ed->sel[ed->primary];
        const size_t anchor = p->anchor;
        p->head = anchor <= from ? to : from;
        ed->want[ed->primary] = -1;
        _normalize(ed);
    } else if ((mods & GLFW_MOD_ALT) && _room(ed, ed->sel_count + 1)) {
        ed->sel[ed->sel_count] = (TextSelection){from, to};
        ed->want[ed->sel_count] = -1;
        ed->primary = ed->sel_count++;
        _normalize(ed);
    } else {
        ed->sel[0] = (TextSelection){from, to};
        ed->want[0] = -1;
        ed->sel_count = 1;
        ed->primary = 0;
    }
    ed->caret_moved = true;
    text_doc_seal_undo(ed->doc);
}

void ui_editor_drag(const UISystem* ui, UIElement* el, UIRect content, float x, float y) {
    UIEditor* ed = el ? el->editor : NULL;
    if (!ed)
        return;
    _layout(ed, ui, content);
    if (ed->bar_drag) {
        UIRect track, thumb;
        if (_bar(ed, &track, &thumb)) {
            const float room = track.h - thumb.h;
            float t = room > 0.0f ? (y - ed->bar_grab - track.y) / room : 0.0f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            const uint64_t total = _top_of(ed, ed->doc->line_count);
            const double at = (double)t * (double)(total ? total - 1 : 0);
            _line_at_row(ed, (uint64_t)at, &ed->top_line, &ed->top_row);
            ed->top_frac = (float)(at - floor(at));
        }
        return;
    }
    // Held past the top or the bottom, the view follows.
    if (y < ed->text.y)
        _scroll_rows(ed, -1.0f);
    else if (y > ed->text.y + ed->text.h)
        _scroll_rows(ed, 1.0f);
    const size_t at = ui_editor_offset_at(el, x, y);
    size_t from, to;
    _unit(ed, at, ed->press_count, &from, &to);
    TextSelection* p = &ed->sel[ed->primary];
    if (from < ed->grab_from)
        *p = (TextSelection){ed->grab_to, from};
    else
        *p = (TextSelection){ed->grab_from, to > ed->grab_to ? to : ed->grab_to};
    ed->want[ed->primary] = -1;
    _normalize(ed);
    ed->caret_moved = true;
}

void ui_editor_wheel(const UISystem* ui, UIElement* el, UIRect content, float wheel) {
    UIEditor* ed = el ? el->editor : NULL;
    if (!ed)
        return;
    _layout(ed, ui, content);
    _scroll_rows(ed, -wheel * WHEEL_ROWS);
}

// ---- Motions

// The line holding an offset, its start, its content and that content's length.
typedef struct {
    size_t line, at, len;
    const char* s;
} Line;

static Line _line(UIEditor* ed, size_t offset) {
    Line l;
    l.line = text_doc_line_of(ed->doc, offset);
    l.at = text_doc_line_start(ed->doc, l.line);
    l.s = text_doc_line(ed->doc, l.line, &l.len);
    return l;
}

static Line _line_at(UIEditor* ed, size_t line) {
    Line l;
    l.line = line;
    l.at = text_doc_line_start(ed->doc, line);
    l.s = text_doc_line(ed->doc, line, &l.len);
    return l;
}

static size_t _line_end(UIEditor* ed, size_t line) {
    const Line l = _line_at(ed, line);
    return l.at + l.len;
}

static size_t _char_left(UIEditor* ed, size_t offset) {
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
    _next(l.s, l.len, &i);
    return l.at + i;
}

// A word move inside its line, or across the line break at either end of it.
static size_t _word_left(UIEditor* ed, size_t offset) {
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
    size_t i = offset - l.at;
    if (i >= l.len)
        return _char_right(ed, offset);
    while (i < l.len && ui_typing_class(l.s[i]) == 0)
        _next(l.s, l.len, &i);
    if (i >= l.len)
        return l.at + l.len;
    const int k = ui_typing_class(l.s[i]);
    while (i < l.len && ui_typing_class(l.s[i]) == k)
        _next(l.s, l.len, &i);
    return l.at + i;
}

// The offset on (line, row) nearest column `want`; past a wrapped row's end, before its last
// character, as a click there is.
static size_t _at_column(UIEditor* ed, size_t line, size_t row, int want) {
    const Line l = _line_at(ed, line);
    int hang = 0;
    const size_t count = _wrap_line(ed, line, &hang);
    const size_t from = ed->starts[row], to = row + 1 < count ? ed->starts[row + 1] : l.len;
    int col = row ? hang : 0;
    size_t i = from;
    while (i < to) {
        size_t n = i;
        const uint32_t cp = _next(l.s, l.len, &n);
        const int w = _width(cp, col, ed->doc->tab_width);
        if ((float)want < (float)col + (float)w * 0.5f)
            return l.at + i;
        col += w;
        i = n;
    }
    if (row + 1 < count && to > from) {
        const char* p = l.s + to;
        text_utf8_prev(&p, l.s + from);
        return l.at + (size_t)(p - l.s);
    }
    return l.at + to;
}

// The line, the row and the column an offset stands at.
static void _place(UIEditor* ed, size_t offset, size_t* line, size_t* row, int* col) {
    const Line l = _line(ed, offset);
    int hang = 0;
    const size_t count = _wrap_line(ed, l.line, &hang);
    *line = l.line;
    *row = _row_holding(ed, count, offset - l.at);
    *col = _column(ed, l.s, l.len, *row, hang, offset - l.at);
}

// `count` rows up or down from `offset`, at the column `*want`, which is set from the offset when
// it is -1: so a caret moved through a short row comes back to its column.
static size_t _rows_moved(UIEditor* ed, size_t offset, int count, int* want) {
    size_t line, row;
    int col;
    _place(ed, offset, &line, &row, &col);
    if (*want < 0)
        *want = col;
    for (int k = 0; k < (count < 0 ? -count : count); k++) {
        if (count < 0 ? !_row_up(ed, &line, &row) : !_row_down(ed, &line, &row))
            return count < 0 ? 0 : ed->doc->length;
    }
    return _at_column(ed, line, row, *want);
}

// The column in a line's own count, wrapping aside: what a line move keeps.
static int _line_column(UIEditor* ed, size_t offset) {
    const Line l = _line(ed, offset);
    int col = 0;
    for (size_t i = 0; i < offset - l.at && i < l.len;) {
        const uint32_t cp = _next(l.s, l.len, &i);
        col += _width(cp, col, ed->doc->tab_width);
    }
    return col;
}

static size_t _at_line_column(UIEditor* ed, size_t line, int want) {
    const Line l = _line_at(ed, line);
    int col = 0;
    size_t i = 0;
    while (i < l.len) {
        size_t n = i;
        const uint32_t cp = _next(l.s, l.len, &n);
        const int w = _width(cp, col, ed->doc->tab_width);
        if ((float)want < (float)col + (float)w * 0.5f)
            break;
        col += w;
        i = n;
    }
    return l.at + i;
}

static size_t _lines_moved(UIEditor* ed, size_t offset, int count, int* want) {
    if (*want < 0)
        *want = _line_column(ed, offset);
    const size_t line = text_doc_line_of(ed->doc, offset);
    const size_t last = ed->doc->line_count - 1;
    const size_t to = count < 0 ? (line >= (size_t)-count ? line - (size_t)-count : 0)
                                : (line + (size_t)count < last ? line + (size_t)count : last);
    return _at_line_column(ed, to, *want);
}

static size_t _row_start(UIEditor* ed, size_t offset) {
    size_t line, row;
    int col;
    _place(ed, offset, &line, &row, &col);
    return text_doc_line_start(ed->doc, line) + ed->starts[row];
}

static size_t _row_end(UIEditor* ed, size_t offset) {
    size_t line, row;
    int col;
    _place(ed, offset, &line, &row, &col);
    return _at_column(ed, line, row, INT32_MAX / 2);
}

static size_t _line_first(UIEditor* ed, size_t offset) {
    const Line l = _line(ed, offset);
    size_t i = 0;
    while (i < l.len && (l.s[i] == ' ' || l.s[i] == '\t'))
        i++;
    return l.at + i;
}

// A byte of the text, 0 past its end.
static char _byte(UIEditor* ed, size_t offset) {
    char c = 0;
    if (offset < ed->doc->length)
        text_doc_copy(ed->doc, offset, offset + 1, &c);
    return c;
}

static const char BRACKET_OPEN[] = "([{";
static const char BRACKET_CLOSE[] = ")]}";

// From an opening bracket at `from`, its closer: or from a closer, its opener; SIZE_MAX for none.
static size_t _bracket_match(UIEditor* ed, size_t from) {
    const char c = _byte(ed, from);
    const char* o = c ? strchr(BRACKET_OPEN, c) : NULL;
    const char* k = c ? strchr(BRACKET_CLOSE, c) : NULL;
    if (!o && !k)
        return SIZE_MAX;
    const char open = o ? c : BRACKET_OPEN[k - BRACKET_CLOSE];
    const char close = o ? BRACKET_CLOSE[o - BRACKET_OPEN] : c;
    int depth = 0;
    if (o) {
        for (size_t i = from; i < ed->doc->length; i++) {
            const char b = _byte(ed, i);
            depth += b == open ? 1 : b == close ? -1 : 0;
            if (!depth)
                return i;
        }
    } else {
        for (size_t i = from + 1; i-- > 0;) {
            const char b = _byte(ed, i);
            depth += b == close ? 1 : b == open ? -1 : 0;
            if (!depth)
                return i;
        }
    }
    return SIZE_MAX;
}

static bool _is_word_byte(char c) {
    return ui_typing_class(c) == 2;
}

// Whether the query matches at byte `i` of a line.
static bool _find_at(const UIEditor* ed, const char* s, size_t len, size_t i) {
    if (i + ed->find_len > len)
        return false;
    for (size_t k = 0; k < ed->find_len; k++) {
        char a = s[i + k], b = ed->find[k];
        if (!(ed->find_flags & UI_EDITOR_FIND_CASE)) {
            a = (char)(a >= 'A' && a <= 'Z' ? a + 32 : a);
            b = (char)(b >= 'A' && b <= 'Z' ? b + 32 : b);
        }
        if (a != b)
            return false;
    }
    if (ed->find_flags & UI_EDITOR_FIND_WORD) {
        if (i > 0 && _is_word_byte(s[i - 1]) && _is_word_byte(s[i]))
            return false;
        const size_t e = i + ed->find_len;
        if (e < len && _is_word_byte(s[e]) && _is_word_byte(s[e - 1]))
            return false;
    }
    return true;
}

// The next match after `offset` (or before it), round the text's end; SIZE_MAX for none.
static size_t _find(UIEditor* ed, size_t offset, bool forward) {
    if (!ed->find_len)
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
            for (size_t i = from; i + ed->find_len <= l.len; i++)
                if (_find_at(ed, l.s, l.len, i))
                    return l.at + i;
        } else {
            const size_t below = step == 0 ? offset - l.at : l.len + 1;
            for (size_t i = below < l.len + 1 ? below : l.len + 1; i-- > 0;)
                if (_find_at(ed, l.s, l.len, i))
                    return l.at + i;
        }
    }
    return SIZE_MAX;
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
            const size_t at = _find(ed, offset, motion == UI_MOTION_FIND_NEXT);
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
    UIEditor* ed = el && el->kind == UI_EDITOR ? el->editor : NULL;
    if (!ed)
        return offset;
    int want = -1;
    return _motion(ed, motion, offset > ed->doc->length ? ed->doc->length : offset, count, &want);
}

// ---- Text objects

bool ui_editor_object(UIElement* el, UIEditorObject object, size_t offset, TextSelection* out) {
    UIEditor* ed = el && el->kind == UI_EDITOR ? el->editor : NULL;
    if (!ed || !out)
        return false;
    offset = offset > ed->doc->length ? ed->doc->length : offset;
    switch (object) {
        case UI_OBJECT_WORD: {
            size_t a, b;
            _unit(ed, offset, 2, &a, &b);
            *out = (TextSelection){a, b};
            return true;
        }
        case UI_OBJECT_LINE: {
            size_t a, b;
            _unit(ed, offset, 3, &a, &b);
            *out = (TextSelection){a, b};
            return true;
        }
        case UI_OBJECT_BRACKETS:
        case UI_OBJECT_BRACKETS_INNER: {
            // Back to the innermost opener not closed before the caret, then on to its closer.
            int depth[3] = {0, 0, 0};
            for (size_t i = offset; i-- > 0;) {
                const char c = _byte(ed, i);
                const char* k = strchr(BRACKET_CLOSE, c);
                const char* o = strchr(BRACKET_OPEN, c);
                if (c && k)
                    depth[k - BRACKET_CLOSE]++;
                else if (c && o && depth[o - BRACKET_OPEN]-- == 0) {
                    const size_t close = _bracket_match(ed, i);
                    if (close == SIZE_MAX || close < offset)
                        return false;
                    *out = object == UI_OBJECT_BRACKETS ? (TextSelection){i, close + 1}
                                                        : (TextSelection){i + 1, close};
                    return true;
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

static void _after_move(UIEditor* ed) {
    _normalize(ed);
    ed->caret_moved = true;
    text_doc_seal_undo(ed->doc);
    ui_editor_reveal(ed->el, ed->sel[ed->primary].head);
}

static int _by_edit_start(const void* a, const void* b) {
    const size_t x = ((const TextEdit*)a)->start, y = ((const TextEdit*)b)->start;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Applies edits from every caret as one action, those that overlap merged, and follows the
// primary caret.
static void _edit(UIEditor* ed, TextEdit* edits, size_t n, bool typed) {
    if (!n)
        return;
    qsort(edits, n, sizeof(TextEdit), _by_edit_start);
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (kept && edits[i].start < edits[kept - 1].end) {
            if (edits[i].end > edits[kept - 1].end)
                edits[kept - 1].end = edits[i].end;
            continue;
        }
        edits[kept++] = edits[i];
    }
    text_doc_apply(ed->doc, edits, kept, ed, typed);
    for (size_t i = 0; i < ed->sel_count; i++)
        ed->want[i] = -1;
    ed->caret_moved = true;
    ui_editor_reveal(ed->el, ed->sel[ed->primary].head);
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

// Text whose line breaks are '\n', with each one the text's own line ending: into a buffer the
// caller frees.
static char* _with_newlines(const UIEditor* ed, const char* text, size_t len, size_t* out_len) {
    const bool crlf = ed->doc->newline == TEXT_NEWLINE_CRLF;
    char* out = malloc(len * 2 + 1);
    if (!out)
        return NULL;
    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n' && crlf)
            out[k++] = '\r';
        out[k++] = text[i];
    }
    *out_len = k;
    return out;
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
    const bool split = ed->sel_count > 1 && lines == ed->sel_count;
    TextEdit* edits = calloc(ed->sel_count, sizeof(TextEdit));
    char** texts = calloc(ed->sel_count, sizeof(char*));
    if (!edits || !texts) {
        free(edits);
        free(texts);
        free(clean);
        return;
    }
    size_t from = 0;
    for (size_t i = 0; i < ed->sel_count; i++) {
        size_t piece = len, start = 0;
        if (split) {
            const char* nl = memchr(clean + from, '\n', len - from);
            start = from;
            piece = (nl ? (size_t)(nl - clean) : len) - from;
            from += piece + 1;
        }
        size_t out_len = 0;
        texts[i] = _with_newlines(ed, clean + start, piece, &out_len);
        edits[i] = (TextEdit){_sel_min(&ed->sel[i]), _sel_max(&ed->sel[i]),
                              texts[i] ? texts[i] : "", texts[i] ? out_len : 0};
    }
    _edit(ed, edits, ed->sel_count, false);
    for (size_t i = 0; i < ed->sel_count; i++)
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
    static const char SPACES[] = "                                ";
    const int tab = ed->doc->tab_width < 32 ? ed->doc->tab_width : 32;
    for (size_t i = 0; i < ed->sel_count; i++) {
        const size_t lo = _sel_min(&ed->sel[i]), hi = _sel_max(&ed->sel[i]);
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
    UIEditor* ed = el && el->kind == UI_EDITOR ? el->editor : NULL;
    if (!ed)
        return;
    if (op == UI_OP_PASTE) {
        _paste(ed);
        return;
    }
    if (op == UI_OP_INDENT || op == UI_OP_OUTDENT) {
        _indent(ed, op == UI_OP_INDENT);
        return;
    }
    if (op == UI_OP_MOVE || op == UI_OP_EXTEND || op == UI_OP_SELECT) {
        for (size_t i = 0; i < ed->sel_count; i++) {
            TextSelection* s = &ed->sel[i];
            int* want = &ed->want[i];
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
    TextEdit* ranges = calloc(ed->sel_count, sizeof(TextEdit));
    if (!ranges)
        return;
    size_t n = 0;
    for (size_t i = 0; i < ed->sel_count; i++) {
        const TextSelection* s = &ed->sel[i];
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
    TextEdit* edits = calloc(ed->sel_count, sizeof(TextEdit));
    if (!edits)
        return;
    for (size_t i = 0; i < ed->sel_count; i++)
        edits[i] = (TextEdit){_sel_min(&ed->sel[i]), _sel_max(&ed->sel[i]), text, len};
    _edit(ed, edits, ed->sel_count, typed);
    free(edits);
}

void ui_editor_type(UIElement* el, const char* text) {
    UIEditor* ed = el && el->kind == UI_EDITOR ? el->editor : NULL;
    if (!ed || !text || !*text)
        return;
    size_t len = 0;
    char* t = _with_newlines(ed, text, strlen(text), &len);
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
static bool _run_home(UIElement* el, void* user) {
    UIEditor* ed = el->editor;
    const bool extend = user != NULL;
    for (size_t i = 0; i < ed->sel_count; i++) {
        TextSelection* s = &ed->sel[i];
        const size_t row = _row_start(ed, s->head), first = _line_first(ed, s->head);
        const size_t line_at = text_doc_line_start(ed->doc, text_doc_line_of(ed->doc, s->head));
        size_t to = row;
        if (row == line_at && first != s->head && first <= _row_end(ed, s->head))
            to = first;
        s->head = to;
        if (!extend)
            s->anchor = to;
        ed->want[i] = -1;
    }
    _after_move(ed);
    return true;
}

// Enter: a line break at each caret, the new line indented as the caret's line is up to the caret.
static bool _run_newline(UIElement* el, void* user) {
    (void)user;
    UIEditor* ed = el->editor;
    TextEdit* edits = calloc(ed->sel_count, sizeof(TextEdit));
    char** texts = calloc(ed->sel_count, sizeof(char*));
    if (!edits || !texts) {
        free(edits);
        free(texts);
        return false;
    }
    const bool crlf = ed->doc->newline == TEXT_NEWLINE_CRLF;
    for (size_t i = 0; i < ed->sel_count; i++) {
        const size_t lo = _sel_min(&ed->sel[i]), hi = _sel_max(&ed->sel[i]);
        const Line l = _line(ed, lo);
        size_t indent = 0;
        while (indent < l.len && indent < lo - l.at && (l.s[indent] == ' ' || l.s[indent] == '\t'))
            indent++;
        texts[i] = malloc(indent + 3);
        if (!texts[i])
            continue;
        size_t k = 0;
        if (crlf)
            texts[i][k++] = '\r';
        texts[i][k++] = '\n';
        memcpy(texts[i] + k, l.s, indent);
        edits[i] = (TextEdit){lo, hi, texts[i], k + indent};
    }
    _edit(ed, edits, ed->sel_count, false);
    for (size_t i = 0; i < ed->sel_count; i++)
        free(texts[i]);
    free(texts);
    free(edits);
    return true;
}

// Tab: the lines a selection spans indented, or spaces to the next tab stop at each caret.
static bool _run_tab(UIElement* el, void* user) {
    (void)user;
    UIEditor* ed = el->editor;
    for (size_t i = 0; i < ed->sel_count; i++) {
        const TextSelection* s = &ed->sel[i];
        if (text_doc_line_of(ed->doc, s->anchor) != text_doc_line_of(ed->doc, s->head)) {
            _indent(ed, true);
            return true;
        }
    }
    static const char SPACES[] = "                                ";
    const int tab = ed->doc->tab_width < 32 ? ed->doc->tab_width : 32;
    TextEdit* edits = calloc(ed->sel_count, sizeof(TextEdit));
    if (!edits)
        return false;
    for (size_t i = 0; i < ed->sel_count; i++) {
        const size_t lo = _sel_min(&ed->sel[i]), hi = _sel_max(&ed->sel[i]);
        const int col = _line_column(ed, lo);
        edits[i] = (TextEdit){lo, hi, SPACES, (size_t)(tab - col % tab)};
    }
    _edit(ed, edits, ed->sel_count, false);
    free(edits);
    return true;
}

static bool _run_undo(UIElement* el, void* user) {
    UIEditor* ed = el->editor;
    const bool done = user ? text_doc_redo_in(ed->doc, ed) : text_doc_undo_in(ed->doc, ed);
    if (done) {
        ed->caret_moved = true;
        ui_editor_reveal(el, ed->sel[ed->primary].head);
    }
    return done;
}

static bool _add_selection(UIEditor* ed, TextSelection s) {
    for (size_t i = 0; i < ed->sel_count; i++)
        if (_sel_min(&ed->sel[i]) == _sel_min(&s) && _sel_max(&ed->sel[i]) == _sel_max(&s))
            return false;
    if (!_room(ed, ed->sel_count + 1))
        return false;
    ed->sel[ed->sel_count] = s;
    ed->want[ed->sel_count] = -1;
    ed->primary = ed->sel_count++;
    _after_move(ed);
    return true;
}

// Cmd+D: with nothing selected the word at the caret; then the next match of the selection.
static bool _run_add_next(UIElement* el, void* user) {
    (void)user;
    UIEditor* ed = el->editor;
    TextSelection* p = &ed->sel[ed->primary];
    if (p->anchor == p->head) {
        TextSelection w;
        ui_editor_object(el, UI_OBJECT_WORD, p->head, &w);
        *p = w;
        _after_move(ed);
        return true;
    }
    const size_t n = _sel_max(p) - _sel_min(p);
    char* query = malloc(n + 1);
    if (!query)
        return false;
    text_doc_copy(ed->doc, _sel_min(p), _sel_max(p), query);
    query[n] = '\0';
    char* was = ed->find;
    const size_t was_len = ed->find_len;
    const unsigned was_flags = ed->find_flags;
    ed->find = query;
    ed->find_len = n;
    ed->find_flags = UI_EDITOR_FIND_CASE;
    const size_t after = _sel_max(&ed->sel[ed->sel_count - 1]);
    const size_t at = _find(ed, after > 0 ? after - 1 : 0, true);
    ed->find = was;
    ed->find_len = was_len;
    ed->find_flags = was_flags;
    free(query);
    return at != SIZE_MAX && _add_selection(ed, (TextSelection){at, at + n});
}

// A caret a row above the topmost, or below the bottommost, at the primary's column.
static bool _run_add_row(UIElement* el, void* user) {
    UIEditor* ed = el->editor;
    const bool below = user != NULL;
    const TextSelection* edge = &ed->sel[below ? ed->sel_count - 1 : 0];
    int want = ed->want[ed->primary];
    if (want < 0) {
        size_t line, row;
        _place(ed, ed->sel[ed->primary].head, &line, &row, &want);
    }
    size_t line, row;
    int col;
    _place(ed, edge->head, &line, &row, &col);
    if (below ? !_row_down(ed, &line, &row) : !_row_up(ed, &line, &row))
        return false;
    const size_t at = _at_column(ed, line, row, want);
    const bool added = _add_selection(ed, (TextSelection){at, at});
    if (added)
        ed->want[ed->primary] = want;
    return added;
}

// Esc: one caret, the primary; with one already, nothing, so the key falls to the screen.
static bool _run_single(UIElement* el, void* user) {
    (void)user;
    UIEditor* ed = el->editor;
    if (ed->sel_count < 2)
        return false;
    ed->sel[0] = ed->sel[ed->primary];
    ed->want[0] = ed->want[ed->primary];
    ed->sel_count = 1;
    ed->primary = 0;
    _after_move(ed);
    return true;
}

// The next or the previous match, selected; with no query, the selection's text is the query.
static bool _run_find(UIElement* el, void* user) {
    UIEditor* ed = el->editor;
    const bool forward = user == NULL;
    TextSelection* p = &ed->sel[ed->primary];
    if (!ed->find_len && p->anchor != p->head) {
        const size_t n = _sel_max(p) - _sel_min(p);
        char* q = malloc(n + 1);
        if (!q)
            return false;
        text_doc_copy(ed->doc, _sel_min(p), _sel_max(p), q);
        q[n] = '\0';
        ui_editor_find(el, q, UI_EDITOR_FIND_CASE);
        free(q);
    }
    const size_t from = forward ? _sel_max(p) : _sel_min(p);
    const size_t at = _find(ed, forward ? (from > 0 ? from - 1 : 0) : from, forward);
    if (at == SIZE_MAX)
        return false;
    *p = (TextSelection){at, at + ed->find_len};
    _after_move(ed);
    return true;
}

#define STEP(name, op, motion) static Step name = {op, motion}
STEP(S_MOVE_LEFT, UI_OP_MOVE, UI_MOTION_CHAR_LEFT);
STEP(S_MOVE_RIGHT, UI_OP_MOVE, UI_MOTION_CHAR_RIGHT);
STEP(S_MOVE_WORD_LEFT, UI_OP_MOVE, UI_MOTION_WORD_LEFT);
STEP(S_MOVE_WORD_RIGHT, UI_OP_MOVE, UI_MOTION_WORD_RIGHT);
STEP(S_MOVE_UP, UI_OP_MOVE, UI_MOTION_ROW_UP);
STEP(S_MOVE_DOWN, UI_OP_MOVE, UI_MOTION_ROW_DOWN);
STEP(S_MOVE_ROW_START, UI_OP_MOVE, UI_MOTION_ROW_START);
STEP(S_MOVE_ROW_END, UI_OP_MOVE, UI_MOTION_ROW_END);
STEP(S_MOVE_PAGE_UP, UI_OP_MOVE, UI_MOTION_PAGE_UP);
STEP(S_MOVE_PAGE_DOWN, UI_OP_MOVE, UI_MOTION_PAGE_DOWN);
STEP(S_MOVE_DOC_START, UI_OP_MOVE, UI_MOTION_DOC_START);
STEP(S_MOVE_DOC_END, UI_OP_MOVE, UI_MOTION_DOC_END);
STEP(S_MOVE_BRACKET, UI_OP_MOVE, UI_MOTION_BRACKET);
STEP(S_SELECT_LEFT, UI_OP_EXTEND, UI_MOTION_CHAR_LEFT);
STEP(S_SELECT_RIGHT, UI_OP_EXTEND, UI_MOTION_CHAR_RIGHT);
STEP(S_SELECT_WORD_LEFT, UI_OP_EXTEND, UI_MOTION_WORD_LEFT);
STEP(S_SELECT_WORD_RIGHT, UI_OP_EXTEND, UI_MOTION_WORD_RIGHT);
STEP(S_SELECT_UP, UI_OP_EXTEND, UI_MOTION_ROW_UP);
STEP(S_SELECT_DOWN, UI_OP_EXTEND, UI_MOTION_ROW_DOWN);
STEP(S_SELECT_ROW_START, UI_OP_EXTEND, UI_MOTION_ROW_START);
STEP(S_SELECT_ROW_END, UI_OP_EXTEND, UI_MOTION_ROW_END);
STEP(S_SELECT_PAGE_UP, UI_OP_EXTEND, UI_MOTION_PAGE_UP);
STEP(S_SELECT_PAGE_DOWN, UI_OP_EXTEND, UI_MOTION_PAGE_DOWN);
STEP(S_SELECT_DOC_START, UI_OP_EXTEND, UI_MOTION_DOC_START);
STEP(S_SELECT_DOC_END, UI_OP_EXTEND, UI_MOTION_DOC_END);
STEP(S_DELETE_LEFT, UI_OP_DELETE, UI_MOTION_CHAR_LEFT);
STEP(S_DELETE_RIGHT, UI_OP_DELETE, UI_MOTION_CHAR_RIGHT);
STEP(S_DELETE_WORD_LEFT, UI_OP_DELETE, UI_MOTION_WORD_LEFT);
STEP(S_DELETE_WORD_RIGHT, UI_OP_DELETE, UI_MOTION_WORD_RIGHT);
STEP(S_DELETE_ROW_START, UI_OP_DELETE, UI_MOTION_ROW_START);
STEP(S_DELETE_ROW_END, UI_OP_DELETE, UI_MOTION_ROW_END);
STEP(S_COPY, UI_OP_COPY, UI_MOTION_NONE);
STEP(S_CUT, UI_OP_CUT, UI_MOTION_NONE);
STEP(S_PASTE, UI_OP_PASTE, UI_MOTION_NONE);
STEP(S_OUTDENT, UI_OP_OUTDENT, UI_MOTION_NONE);
#undef STEP

static bool _run_select_all(UIElement* el, void* user) {
    (void)user;
    UIEditor* ed = el->editor;
    ed->sel[0] = (TextSelection){0, ed->doc->length};
    ed->want[0] = -1;
    ed->sel_count = 1;
    ed->primary = 0;
    _after_move(ed);
    return true;
}

static int g_one = 1;

static const UIEditorCommand COMMANDS[] = {
    {"move-left", "Move each caret a character left", _run_step, &S_MOVE_LEFT},
    {"move-right", "Move each caret a character right", _run_step, &S_MOVE_RIGHT},
    {"move-word-left", "Move each caret back a word", _run_step, &S_MOVE_WORD_LEFT},
    {"move-word-right", "Move each caret on a word", _run_step, &S_MOVE_WORD_RIGHT},
    {"move-up", "Move each caret up a row", _run_step, &S_MOVE_UP},
    {"move-down", "Move each caret down a row", _run_step, &S_MOVE_DOWN},
    {"move-home", "Move each caret to its line's first character, then its row's start", _run_home,
     NULL},
    {"move-row-start", "Move each caret to its row's start", _run_step, &S_MOVE_ROW_START},
    {"move-row-end", "Move each caret to its row's end", _run_step, &S_MOVE_ROW_END},
    {"move-page-up", "Move each caret up a page", _run_step, &S_MOVE_PAGE_UP},
    {"move-page-down", "Move each caret down a page", _run_step, &S_MOVE_PAGE_DOWN},
    {"move-doc-start", "Move each caret to the start of the text", _run_step, &S_MOVE_DOC_START},
    {"move-doc-end", "Move each caret to the end of the text", _run_step, &S_MOVE_DOC_END},
    {"move-bracket", "Move each caret to the bracket matching the one at it", _run_step,
     &S_MOVE_BRACKET},
    {"select-left", "Extend each selection a character left", _run_step, &S_SELECT_LEFT},
    {"select-right", "Extend each selection a character right", _run_step, &S_SELECT_RIGHT},
    {"select-word-left", "Extend each selection back a word", _run_step, &S_SELECT_WORD_LEFT},
    {"select-word-right", "Extend each selection on a word", _run_step, &S_SELECT_WORD_RIGHT},
    {"select-up", "Extend each selection up a row", _run_step, &S_SELECT_UP},
    {"select-down", "Extend each selection down a row", _run_step, &S_SELECT_DOWN},
    {"select-home", "Extend each selection to its line's first character, then its row's start",
     _run_home, &g_one},
    {"select-row-start", "Extend each selection to its row's start", _run_step,
     &S_SELECT_ROW_START},
    {"select-row-end", "Extend each selection to its row's end", _run_step, &S_SELECT_ROW_END},
    {"select-page-up", "Extend each selection up a page", _run_step, &S_SELECT_PAGE_UP},
    {"select-page-down", "Extend each selection down a page", _run_step, &S_SELECT_PAGE_DOWN},
    {"select-doc-start", "Extend each selection to the start of the text", _run_step,
     &S_SELECT_DOC_START},
    {"select-doc-end", "Extend each selection to the end of the text", _run_step,
     &S_SELECT_DOC_END},
    {"select-all", "Select the whole text", _run_select_all, NULL},
    {"delete-left", "Delete the selection, or the character before each caret", _run_step,
     &S_DELETE_LEFT},
    {"delete-right", "Delete the selection, or the character after each caret", _run_step,
     &S_DELETE_RIGHT},
    {"delete-word-left", "Delete back a word from each caret", _run_step, &S_DELETE_WORD_LEFT},
    {"delete-word-right", "Delete on a word from each caret", _run_step, &S_DELETE_WORD_RIGHT},
    {"delete-row-start", "Delete from each caret to its row's start", _run_step,
     &S_DELETE_ROW_START},
    {"delete-row-end", "Delete from each caret to its row's end", _run_step, &S_DELETE_ROW_END},
    {"newline", "Start a line at each caret, indented as its line is", _run_newline, NULL},
    {"tab", "Indent the lines a selection spans, or put spaces to the next tab stop", _run_tab,
     NULL},
    {"outdent", "Take the lines the selections touch back a tab stop", _run_step, &S_OUTDENT},
    {"copy", "Copy the selections", _run_step, &S_COPY},
    {"cut", "Cut the selections", _run_step, &S_CUT},
    {"paste", "Paste over each selection, a line to each caret when there are as many", _run_step,
     &S_PASTE},
    {"undo", "Take back the last change", _run_undo, NULL},
    {"redo", "Make the last change taken back again", _run_undo, &g_one},
    {"add-next-match", "Select the word at the caret, then add the next match of the selection",
     _run_add_next, NULL},
    {"add-caret-above", "Add a caret a row above the topmost", _run_add_row, NULL},
    {"add-caret-below", "Add a caret a row below the bottommost", _run_add_row, &g_one},
    {"single-caret", "Keep only the primary caret", _run_single, NULL},
    {"find-next", "Select the next match", _run_find, NULL},
    {"find-previous", "Select the previous match", _run_find, &g_one},
};

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
    if (!el || el->kind != UI_EDITOR || !el->editor || !c)
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
    const UIEditor* ed = el && el->kind == UI_EDITOR ? el->editor : NULL;
    return ed && ed->mode ? ed->mode : ui_editor_standard_mode();
}

void ui_editor_find(UIElement* el, const char* query, unsigned flags) {
    UIEditor* ed = el && el->kind == UI_EDITOR ? el->editor : NULL;
    if (!ed)
        return;
    free(ed->find);
    ed->find = NULL;
    ed->find_len = 0;
    ed->find_flags = flags;
    if (query && *query) {
        ed->find_len = strlen(query);
        ed->find = malloc(ed->find_len + 1);
        if (ed->find)
            memcpy(ed->find, query, ed->find_len + 1);
        else
            ed->find_len = 0;
    }
}

// ---- Keys

bool ui_editor_key(const UISystem* ui, UIElement* el, UIRect content, const InputKeyEvent* ev) {
    UIEditor* ed = el ? el->editor : NULL;
    if (!ed || !ev)
        return false;
    _layout(ed, ui, content);
    const UIEditorMode* top = ui_editor_mode(el);
    const int mods = ev->mods & (SHIFT | CTRL | ALT | SUPER);
    // Each mode up the chain: its own reader first, then its table.
    for (const UIEditorMode* m = top; m; m = m->parent) {
        if (m->read_key && m->read_key(el, ev, m->user))
            return true;
        for (size_t i = 0; ev->key && i < m->binding_count; i++) {
            const UIEditorBinding* b = &m->bindings[i];
            if (b->key == ev->key && b->mods == mods && b->command)
                return b->command->run(el, b->command->user);
        }
    }
    if (ev->codepoint && top->types && ev->codepoint >= 0x20 && ev->codepoint != 0x7f) {
        char utf8[4];
        const size_t n = text_utf8_encode(ev->codepoint, utf8);
        if (n)
            _type(ed, utf8, n, true);
        return true;
    }
    return false;
}

#undef SHIFT
#undef CTRL
#undef ALT
#undef SUPER
#undef SHORTCUT
#undef WORD

// ---- Drawing

// One character's glyph in its cell, `base` the row's baseline in pixels.
static void _glyph(UIDrawList* dl, const UIEditor* ed, uint32_t cp, float x_px, float base_px,
                   const float* color) {
    GlyphBitmap g;
    if (!glyph_cache_get(ed->face, cp, &g))
        return;
    if (g.missing) {
        // A box a cell wide, as the UI draws a character its atlas lacks.
        const float w = (float)ed->face->cell_width / ed->px, h = (float)ed->face->ascent / ed->px;
        const UIRect box = {x_px / ed->px + w * 0.15f, base_px / ed->px - h * 0.8f, w * 0.7f,
                            h * 0.8f};
        const float t = 1.0f / ed->px;
        vec4 c = {color[0], color[1], color[2], color[3]};
        ui_draw_quad(dl, (UIRect){box.x, box.y, box.w, t}, c);
        ui_draw_quad(dl, (UIRect){box.x, box.y + box.h - t, box.w, t}, c);
        ui_draw_quad(dl, (UIRect){box.x, box.y, t, box.h}, c);
        ui_draw_quad(dl, (UIRect){box.x + box.w - t, box.y, t, box.h}, c);
        return;
    }
    if (!g.width)
        return;
    const UIRect r = {(x_px + (float)g.left) / ed->px, (base_px + (float)g.top) / ed->px,
                      (float)g.width / ed->px, (float)g.height / ed->px};
    ui_draw_textured_quad_uv(dl, r, &ed->face->atlas, g.uv, (float*)color);
}

static const UIEditorStyle* _style(const UITheme* t, uint16_t i) {
    return i < UI_EDITOR_STYLES ? &t->editor_styles[i] : &t->editor_styles[0];
}

// The ranges any layer holds that reach [from, to), into `out`.
static size_t _ranges_in(const TextDoc* doc, size_t from, size_t to, TextRange* out, size_t max) {
    size_t n = 0;
    for (int layer = 0; layer < TEXT_DOC_LAYERS; layer++) {
        size_t count = 0;
        const TextRange* r = text_doc_ranges(doc, layer, &count);
        for (size_t i = 0; i < count && r[i].start < to && n < max; i++)
            if (r[i].end > from)
                out[n++] = r[i];
    }
    return n;
}

#define RANGES_DRAWN 256

double ui_editor_draw(const UISystem* ui, UIElement* el, const UIStyle* s, UIRect content,
                      bool focused, double now, bool blink) {
    UIEditor* ed = el ? el->editor : NULL;
    UIDrawList* dl = ui_draw_list(ui);
    if (!ed || !dl)
        return INFINITY;
    ui_draw_rect(dl, el->rect, s);
    _layout(ed, ui, content);
    if (!ed->face)
        return INFINITY;
    const UITheme* t = ui_theme(ui);
    TextDoc* doc = ed->doc;
    const int tab = doc->tab_width;
    vec4 fg = {s->fg[0], s->fg[1], s->fg[2], s->fg[3]};
    vec4 dim = {s->fg[0], s->fg[1], s->fg[2], s->fg[3] * 0.38f};
    vec4 sel_colour = {t->selection[0], t->selection[1], t->selection[2], t->selection[3]};
    if (sel_colour[3] <= 0.0f)
        glm_vec4_copy((vec4){0.25f, 0.45f, 0.85f, 0.45f}, sel_colour);

    // The caret's blink.
    double wake = INFINITY;
    bool lit = false;
    if (focused) {
        if (ed->caret_moved) {
            ed->caret_time = now;
            ed->caret_moved = false;
        }
        lit = ui_typing_caret_lit(now, ed->caret_time, blink, &wake);
    }
    // A mode may draw its caret a cell wide: a vim mode's normal mode.
    const UIEditorMode* mode = ed->mode ? ed->mode : ui_editor_standard_mode();
    const float caret_w = mode->block_caret ? ed->cell : UI_TYPING_CARET_WIDTH;
    vec4 caret_colour = {fg[0], fg[1], fg[2], mode->block_caret ? 0.45f : fg[3]};

    // The rows in view: their text, the host's ranges, the selections and the carets, row by row.
    ui_push_clip(dl, content);
    const float cell_px = (float)ed->face->cell_width;
    const float text_x_px = roundf(ed->text.x * ed->px);
    const size_t first_line = ed->top_line;
    size_t line = ed->top_line, row = ed->top_row;
    float y = ed->text.y - ed->top_frac * ed->row;
    const size_t window_from = text_doc_line_start(doc, first_line);
    TextRange ranges[RANGES_DRAWN];
    size_t range_count = 0;
    bool ranges_read = false;
    while (y < ed->text.y + ed->text.h) {
        const size_t at = text_doc_line_start(doc, line);
        size_t len = 0;
        const char* bytes = text_doc_line(doc, line, &len);
        int hang = 0;
        const size_t count = _wrap_line(ed, line, &hang);
        const TextSpan* spans = NULL;
        size_t span_count = 0;
        text_doc_line_spans(doc, line, &spans, &span_count);
        if (!ranges_read) {
            // Every range reaching the rows in view, once a frame: the window runs to the end of
            // the screenful of lines that could show, at most.
            const size_t last = line + (size_t)ceilf(_view_rows(ed)) + 1;
            range_count =
                _ranges_in(doc, window_from, text_doc_line_start(doc, last), ranges, RANGES_DRAWN);
            ranges_read = true;
        }
        for (; row < count && y < ed->text.y + ed->text.h; row++, y += ed->row) {
            const size_t from = ed->starts[row], to = row + 1 < count ? ed->starts[row + 1] : len;
            const bool last_row = row + 1 == count;
            const float base_px = roundf(y * ed->px) + (float)ed->face->ascent;
            // The line's number, on its first row.
            if (row == 0) {
                char num[24];
                const int n = snprintf(num, sizeof(num), "%zu", line + 1);
                const int digits = _digits(doc->line_count);
                float nx = roundf(content.x * ed->px) + (float)(digits - n) * cell_px;
                for (int k = 0; k < n; k++, nx += cell_px)
                    _glyph(dl, ed, (uint32_t)num[k], nx, base_px, dim);
            }
            // Backgrounds: the host's ranges, then the selections over them.
            const size_t row_from = at + from, row_to = at + to;
            for (size_t k = 0; k < range_count; k++) {
                const UIEditorStyle* st = _style(t, ranges[k].style);
                if (st->background[3] <= 0.0f || ranges[k].end <= row_from ||
                    ranges[k].start > row_to)
                    continue;
                const size_t a = ranges[k].start > row_from ? ranges[k].start - at : from;
                const size_t b = ranges[k].end < row_to ? ranges[k].end - at : to;
                const int ca = _column(ed, bytes, len, row, hang, a);
                const int cb = _column(ed, bytes, len, row, hang, b);
                ui_draw_quad(dl,
                             (UIRect){ed->text.x + (float)ca * ed->cell, y,
                                      (float)(cb - ca) * ed->cell, ed->row},
                             (float*)st->background);
            }
            // Find's matches, a fainter tint of the selection's colour, where they cross the row.
            for (size_t i = 0; ed->find_len && i + ed->find_len <= len && i < to; i++) {
                if (i + ed->find_len <= from || !_find_at(ed, bytes, len, i))
                    continue;
                const size_t a = i > from ? i : from;
                const size_t b = i + ed->find_len < to ? i + ed->find_len : to;
                const int ca = _column(ed, bytes, len, row, hang, a);
                const int cb = _column(ed, bytes, len, row, hang, b);
                vec4 tint = {sel_colour[0], sel_colour[1], sel_colour[2], sel_colour[3] * 0.5f};
                ui_draw_quad(dl,
                             (UIRect){ed->text.x + (float)ca * ed->cell, y,
                                      (float)(cb - ca) * ed->cell, ed->row},
                             tint);
            }
            for (size_t k = 0; k < ed->sel_count; k++) {
                const size_t lo = _sel_min(&ed->sel[k]), hi = _sel_max(&ed->sel[k]);
                if (lo == hi || hi < row_from || lo > row_to || (lo == row_to && !last_row))
                    continue;
                const int ca = _column(ed, bytes, len, row, hang, lo > row_from ? lo - at : from);
                int cb = _column(ed, bytes, len, row, hang, hi < row_to ? hi - at : to);
                // A selection running on past the line's end shows its line break as a cell.
                if (last_row && hi > at + len)
                    cb++;
                if (cb > ca)
                    ui_draw_quad(dl,
                                 (UIRect){ed->text.x + (float)ca * ed->cell, y,
                                          (float)(cb - ca) * ed->cell, ed->row},
                                 sel_colour);
            }
            // The characters, in the highlighter's colours, a range's colour over them.
            int col = row ? hang : 0;
            size_t sp = 0;
            for (size_t i = from; i < to;) {
                const size_t here = i;
                const uint32_t cp = _next(bytes, len, &i);
                const int w = _width(cp, col, tab);
                const float* colour = fg;
                while (sp < span_count && spans[sp].start + spans[sp].len <= here)
                    sp++;
                if (sp < span_count && spans[sp].start <= here) {
                    const UIEditorStyle* st = _style(t, spans[sp].style);
                    if (st->text[3] > 0.0f)
                        colour = st->text;
                }
                for (size_t k = 0; k < range_count; k++) {
                    const UIEditorStyle* st = _style(t, ranges[k].style);
                    if (st->text[3] > 0.0f && ranges[k].start <= at + here &&
                        ranges[k].end > at + here)
                        colour = st->text;
                }
                if (cp != ' ' && cp != '\t' && w > 0)
                    _glyph(dl, ed, cp, text_x_px + (float)col * cell_px, base_px, colour);
                col += w;
            }
            // Lines under the ranges that ask for one.
            for (size_t k = 0; k < range_count; k++) {
                const UIEditorStyle* st = _style(t, ranges[k].style);
                if (st->underline[3] <= 0.0f || ranges[k].end <= row_from ||
                    ranges[k].start >= row_to)
                    continue;
                const size_t a = ranges[k].start > row_from ? ranges[k].start - at : from;
                const size_t b = ranges[k].end < row_to ? ranges[k].end - at : to;
                const int ca = _column(ed, bytes, len, row, hang, a);
                const int cb = _column(ed, bytes, len, row, hang, b);
                const float uy = (base_px + 2.0f) / ed->px;
                ui_draw_quad(dl,
                             (UIRect){ed->text.x + (float)ca * ed->cell, uy,
                                      (float)(cb - ca) * ed->cell, 1.0f / ed->px},
                             (float*)st->underline);
            }
            // The carets on this row.
            for (size_t k = 0; lit && k < ed->sel_count; k++) {
                const size_t h = ed->sel[k].head;
                if (h < row_from || h > row_to || (h == row_to && !last_row))
                    continue;
                const int c = _column(ed, bytes, len, row, hang, h - at);
                ui_draw_quad(
                    dl, (UIRect){_snap(ed, ed->text.x + (float)c * ed->cell), y, caret_w, ed->row},
                    caret_colour);
            }
        }
        if (y >= ed->text.y + ed->text.h || line + 1 >= doc->line_count)
            break;
        line++;
        row = 0;
    }
    ui_pop_clip(dl);

    // The bar, while the text is taller than the view.
    UIRect track, thumb;
    if (_bar(ed, &track, &thumb)) {
        vec4 bar = {s->fg[0], s->fg[1], s->fg[2], 0.28f};
        ui_draw_rounded(dl, thumb, BAR_W * 0.5f, bar, NULL, 0.0f);
    }
    return wake;
}
