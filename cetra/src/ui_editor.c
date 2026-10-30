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
#include "util.h"

// Two presses this close in time and place are one double press.
#define PRESS_SECONDS 0.4
#define PRESS_POINTS  4.0f
// The caret's blink, the field's rhythm.
#define CARET_LIT_SECONDS  0.8
#define CARET_DARK_SECONDS 0.4
#define CARET_WIDTH        1.5f
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

// Words for a double press: letters, digits and '_' are one class, spaces another, the rest a
// third.
static int _class(uint32_t cp) {
    if (cp == ' ' || cp == '\t')
        return 0;
    if (cp >= 0x80 || cp == '_' || (cp >= '0' && cp <= '9') ||
        ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z'))
        return 1;
    return 2;
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
    if (len == 0)
        return;
    size_t a = offset - at, b;
    // The class of the character after the offset, or of the one before it at the line's end.
    size_t probe = a;
    if (a >= len) {
        const char* p = s + len;
        text_utf8_prev(&p, s);
        probe = (size_t)(p - s);
    }
    size_t k = probe;
    const int cls = _class(_next(s, len, &k));
    while (a > 0) {
        const char* p = s + a;
        const uint32_t cp = text_utf8_prev(&p, s);
        if (_class(cp) != cls)
            break;
        a = (size_t)(p - s);
    }
    b = probe;
    while (b < len) {
        size_t n = b;
        if (_class(_next(s, len, &n)) != cls)
            break;
        b = n;
    }
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
    const bool again = time - ed->press_time < PRESS_SECONDS &&
                       fabsf(x - ed->press_x) < PRESS_POINTS &&
                       fabsf(y - ed->press_y) < PRESS_POINTS;
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

bool ui_editor_key(UISystem* ui, UIElement* el, UIRect content, const InputKeyEvent* ev) {
    (void)ui;
    (void)el;
    (void)content;
    (void)ev;
    return false;
}

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
    bool lit = focused;
    if (focused) {
        if (ed->caret_moved) {
            ed->caret_time = now;
            ed->caret_moved = false;
        }
        if (blink) {
            const double period = CARET_LIT_SECONDS + CARET_DARK_SECONDS;
            double phase = fmod(now - ed->caret_time, period);
            phase = phase < 0.0 ? 0.0 : phase;
            lit = phase < CARET_LIT_SECONDS;
            wake = (lit ? CARET_LIT_SECONDS : period) - phase;
        }
    }

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
                    dl,
                    (UIRect){_snap(ed, ed->text.x + (float)c * ed->cell), y, CARET_WIDTH, ed->row},
                    fg);
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
