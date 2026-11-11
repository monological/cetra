#include "ui_table.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "ext/log.h"
#include "ui_internal.h"
#include "ui_text.h"
#include "util.h"

/*
 * A table (spec 14.30): no element per cell, only what is kept per row and per cell -- each cell's
 * widest line and each row's height and top -- measured once and again only for the rows that
 * change, so a FIT column is the widest of every row and not of the rows seen so far.
 */
typedef struct UITable UITable;

struct UITable {
    UITableColumn columns[UI_TABLE_COLUMNS_MOST]; // titles owned
    int column_count;
    UITableSource source;
    int count;

    // Each cell's widest line as last measured, row by row, a column to a cell; each row's height,
    // and where it starts below the header. Doubles, since 100,000 rows reach millions of points,
    // past which a float cannot hold a sum to a point. The tops are summed for the first `summed`
    // rows, which a shrink lowers and a measure raises to the count: what is placed, pressed and
    // searched is bounded by it, never by the count, which may run ahead of the sums.
    float* widths;
    float* heights;
    double* tops;
    bool* stale; // a row to measure at the next layout
    size_t widths_capacity, heights_capacity, tops_capacity, stale_capacity;
    int summed;
    int stale_lo, stale_hi; // every stale row, in [lo, hi)
    bool every;             // every row and title stale, whatever `stale` says
    bool widest_stale;      // a row taken off may have been a column's widest

    // What the measures are for: another face, size, tracking, line spacing or row padding
    // measures every row and every title again.
    const Font* font;
    float size, tracking, line_spacing, row_padding;
    float widest[UI_TABLE_COLUMNS_MOST];      // a column's cells', over every row
    float title_width[UI_TABLE_COLUMNS_MOST]; // its title's
    float header_h;                           // 0 for no header

    // As the last layout placed it: its content rect, each column's start in it and width, the
    // points of a pixel and a wheel unit's; what the panes round it show, and where its header
    // stands, held at the top of that while its own top is above it.
    bool laid;
    UIRect content, shown;
    float x[UI_TABLE_COLUMNS_MOST], w[UI_TABLE_COLUMNS_MOST];
    float pixel, step, header_y;
    int reveal;    // the row a reveal asked for before its height was known; -1 for none
    int shown_row; // a row to show in the panes round it at the next place; -1 for none

    // The cursor's row, -1 for none; whether a turn of keys is under way and moved it, so
    // on_cursor is told once at its end, after a hook's ui_table_move_cursor too.
    int cursor;
    bool in_keys, moved;
    UIActionFn on_cursor;
    void* cursor_user;
};

struct UITable* ui_table_make(void) {
    UITable* t = calloc(1, sizeof(UITable));
    if (t) {
        t->stale_lo = INT_MAX;
        t->reveal = t->shown_row = -1;
        t->cursor = -1;
    }
    return t;
}

static void _titles_free(UITable* t) {
    for (int c = 0; c < t->column_count; c++)
        free((char*)t->columns[c].title);
}

void ui_table_free(struct UITable* t) {
    if (!t)
        return;
    _titles_free(t);
    free(t->widths);
    free(t->heights);
    free(t->tops);
    free(t->stale);
    free(t);
}

// The table of `el`, or NULL logged under `who` for anything not a table.
static UITable* _table(const char* who, UIElement* el) {
    UITable* t = el && el->kind == UI_TABLE ? el->table : NULL;
    if (!t)
        log_error("%s: not a table", who);
    return t;
}

// The same for a call given the table to read.
static const UITable* _seen(const char* who, const UIElement* el) {
    const UITable* t = el && el->kind == UI_TABLE ? el->table : NULL;
    if (!t)
        log_error("%s: not a table", who);
    return t;
}

// Room for `rows` rows of `columns` cells; false, and logged, out of memory.
static bool _reserve(UITable* t, int rows, int columns) {
    const size_t n = rows > 0 ? (size_t)rows : 1;
    const size_t cells = n * (size_t)(columns > 0 ? columns : 1);
    if (grow_array((void**)&t->widths, &t->widths_capacity, cells, sizeof(float), 64) &&
        grow_array((void**)&t->heights, &t->heights_capacity, n, sizeof(float), 64) &&
        grow_array((void**)&t->tops, &t->tops_capacity, n + 1, sizeof(double), 64) &&
        grow_array((void**)&t->stale, &t->stale_capacity, n, sizeof(bool), 64))
        return true;
    log_error("ui_table: out of memory for %d rows of %d columns", rows, columns);
    return false;
}

// Rows [lo, hi) measured again at the next layout.
static void _measure_rows(UITable* t, int lo, int hi) {
    for (int r = lo; r < hi; r++)
        t->stale[r] = true;
    if (lo < hi) {
        t->stale_lo = lo < t->stale_lo ? lo : t->stale_lo;
        t->stale_hi = hi > t->stale_hi ? hi : t->stale_hi;
    }
}

void ui_table_set_columns(UIElement* table, const UITableColumn* columns, int count) {
    UITable* t = _table("ui_table_set_columns", table);
    if (!t)
        return;
    if (!columns || count <= 0) {
        log_error("ui_table_set_columns: no columns");
        return;
    }
    if (count > UI_TABLE_COLUMNS_MOST) {
        log_error("ui_table_set_columns: %d columns, past the most, %d", count,
                  UI_TABLE_COLUMNS_MOST);
        return;
    }
    for (int c = 0; c < count; c++)
        if (columns[c].drawn && columns[c].size == UI_FIT) {
            log_error("ui_table_set_columns: column %d is drawn and sized FIT, with no text to fit",
                      c);
            return;
        }
    if (!_reserve(t, t->count, count))
        return;
    char* titles[UI_TABLE_COLUMNS_MOST] = {0};
    for (int c = 0; c < count; c++) {
        if (!columns[c].title)
            continue;
        titles[c] = safe_strdup(columns[c].title);
        if (!titles[c]) {
            for (int k = 0; k < c; k++)
                free(titles[k]);
            log_error("ui_table_set_columns: out of memory");
            return;
        }
    }
    _titles_free(t);
    for (int c = 0; c < count; c++) {
        t->columns[c] = columns[c];
        t->columns[c].title = titles[c];
    }
    t->column_count = count;
    t->every = true;
}

void ui_table_set_rows(UIElement* table, int count, const UITableSource* source) {
    UITable* t = _table("ui_table_set_rows", table);
    if (!t)
        return;
    if (count < 0 || !source || !source->cell) {
        log_error("ui_table_set_rows: wants a count of none or more and a source with a cell");
        return;
    }
    if (!_reserve(t, count, t->column_count))
        return;
    t->source = *source;
    t->count = count;
    t->summed = 0;
    t->every = true;
    t->reveal = -1;
    t->cursor = -1;
    table->scroll = table->scroll_target = 0.0f;
}

void ui_table_set_row_count(UIElement* table, int count) {
    UITable* t = _table("ui_table_set_row_count", table);
    if (!t)
        return;
    if (count < 0 || !t->source.cell) {
        log_error("ui_table_set_row_count: a count under none, or a table given no rows");
        return;
    }
    if (!_reserve(t, count, t->column_count))
        return;
    if (count > t->count) {
        // Nothing yet, so the measure finds a widest raised and every height changed.
        const size_t n = (size_t)(count - t->count);
        memset(&t->widths[(size_t)t->count * (size_t)t->column_count], 0,
               n * (size_t)t->column_count * sizeof(float));
        memset(&t->heights[t->count], 0, n * sizeof(float));
        _measure_rows(t, t->count, count);
    } else
        t->widest_stale = true;
    t->stale_hi = t->stale_hi < count ? t->stale_hi : count;
    t->summed = t->summed < count ? t->summed : count;
    t->count = count;
    if (t->cursor >= count)
        t->cursor = -1;
}

void ui_table_rows_changed(UIElement* table, int first, int count) {
    UITable* t = _table("ui_table_rows_changed", table);
    if (!t)
        return;
    if (first < 0 || count < 0 || (long long)first + count > t->count) {
        log_error("ui_table_rows_changed: rows %d to %lld are not among the %d", first,
                  (long long)first + count, t->count);
        return;
    }
    _measure_rows(t, first, first + count);
}

// ---- Measuring

// The points above and below a row's lines, and a header's.
static float _row_padding(const UISystem* ui) {
    const float p = ui_theme(ui)->table_row_padding;
    return p > 0.0f ? p : 2.0f;
}

// How many lines `text` draws in: one more than its '\n's, none for no text.
static int _lines_of(const char* text) {
    if (!text || !text[0])
        return 0;
    int lines = 1;
    for (const char* p = strchr(text, '\n'); p; p = strchr(p + 1, '\n'))
        lines++;
    return lines;
}

// The widest line of `text` in `s`.
static float _text_width(const UISystem* ui, const UIStyle* s, const char* text) {
    if (!text || !text[0])
        return 0.0f;
    const UIText tx = ui_text_make(ui_draw_list(ui), text, strlen(text), s, NULL, 0);
    return ui_text_widest_line(&tx);
}

// The titles, and the header's height: the most lines a title has, at the line height, and the
// row padding above and below them. No title, no header.
static void _measure_titles(const UISystem* ui, UITable* t, const UIStyle* s, float line_h) {
    int lines = 0;
    for (int c = 0; c < t->column_count; c++) {
        const char* title = t->columns[c].title;
        t->title_width[c] = _text_width(ui, s, title);
        const int n = _lines_of(title);
        lines = n > lines ? n : lines;
    }
    t->header_h = lines > 0 ? (float)lines * line_h + 2.0f * t->row_padding : 0.0f;
}

/*
 * Row `r` measured: each text cell's widest line, counted into `measures`, and its height. A cell
 * as wide as its column's widest or wider raises it; one that was the widest and shrank marks its
 * column in `*rescan`, whose widest is found again over every row -- NULL while every row is
 * measured, which finds every widest. True when the height changed.
 */
static bool _measure_row(const UISystem* ui, UIElement* el, const UIStyle* s, int r, float line_h,
                         uint32_t* rescan) {
    UITable* t = el->table;
    int lines = 0;
    for (int c = 0; c < t->column_count; c++) {
        float* w = &t->widths[(size_t)r * (size_t)t->column_count + (size_t)c];
        const float was = *w;
        *w = 0.0f;
        if (!t->columns[c].drawn) {
            UITableCell cell = {0};
            t->source.cell(r, c, &cell, t->source.user);
            *w = _text_width(ui, s, cell.text);
            const int n = _lines_of(cell.text);
            lines = n > lines ? n : lines;
            el->measures++;
        }
        if (*w >= t->widest[c])
            t->widest[c] = *w;
        else if (rescan && was >= t->widest[c])
            *rescan |= 1u << c;
    }
    const float h = t->source.height
                        ? fmaxf(t->source.height(r, t->source.user), 0.0f)
                        : (float)(lines > 1 ? lines : 1) * line_h + 2.0f * t->row_padding;
    const bool changed = h != t->heights[r];
    t->heights[r] = h;
    return changed;
}

// Each marked column's widest found again over every row kept, in one pass down the rows.
static void _rescan(UITable* t, uint32_t columns) {
    for (int c = 0; c < t->column_count; c++)
        if (columns & (1u << c))
            t->widest[c] = 0.0f;
    for (int r = 0; r < t->count; r++) {
        const float* row = &t->widths[(size_t)r * (size_t)t->column_count];
        for (int c = 0; c < t->column_count; c++)
            if (columns & (1u << c))
                t->widest[c] = fmaxf(t->widest[c], row[c]);
    }
}

/*
 * What is stale measured: every row and title after the columns, the rows or the style changed,
 * else the rows marked. A column is scanned again only where its widest shrank or a row was taken
 * off, and the tops summed again only from the first row whose height changed or was never summed:
 * a row changed or added costs its own measure, not a walk of every row.
 */
static void _settle(const UISystem* ui, UIElement* el, const UIStyle* s) {
    UITable* t = el->table;
    const float pad = _row_padding(ui);
    if (t->font != s->font || t->size != s->font_size || t->tracking != s->tracking ||
        t->line_spacing != s->line_spacing || t->row_padding != pad) {
        t->font = s->font;
        t->size = s->font_size;
        t->tracking = s->tracking;
        t->line_spacing = s->line_spacing;
        t->row_padding = pad;
        t->every = true;
    }
    const float line_h = ui_line_height(s->font, s->font_size, s->line_spacing);
    const bool every = t->every;
    if (every) {
        _measure_titles(ui, t, s, line_h);
        memset(t->widest, 0, sizeof(t->widest));
        t->summed = 0;
    }
    const int lo = every ? 0 : t->stale_lo, hi = every ? t->count : t->stale_hi;
    uint32_t rescan = 0;
    int from = t->summed;
    for (int r = lo; r < hi; r++) {
        if (!every && !t->stale[r])
            continue;
        t->stale[r] = false;
        if (_measure_row(ui, el, s, r, line_h, every ? NULL : &rescan) && r < from)
            from = r;
    }
    t->every = false;
    t->stale_lo = INT_MAX;
    t->stale_hi = 0;
    if (t->widest_stale && !every)
        rescan = ~0u;
    if (rescan)
        _rescan(t, rescan);
    t->widest_stale = false;
    if (from == 0)
        t->tops[0] = 0.0;
    for (int r = from; r < t->count; r++)
        t->tops[r + 1] = t->tops[r] + (double)t->heights[r];
    t->summed = t->count;
}

// A column's width before the room across is shared: a FIT column its widest cell or its title, a
// FIXED one its length, and a SHARE or GROW one none yet.
static float _column_own(const UITable* t, int c) {
    const UITableColumn* col = &t->columns[c];
    if (col->size == UI_FIT)
        return fmaxf(t->widest[c], t->title_width[c]);
    return col->size == UI_FIXED ? fmaxf(col->length, 0.0f) : 0.0f;
}

// The gaps between the columns.
static float _gaps(const UISystem* ui, const UIElement* el) {
    const int n = el->table->column_count;
    return n > 1 ? ui_element_spacing(ui, el) * (float)(n - 1) : 0.0f;
}

/*
 * The columns across `width`: FIXED and FIT first, then SHARE of what is across less the gaps,
 * rounded down to a whole point, then GROW evenly over what is left, each column's start on a whole
 * point of the content. Columns wider than the table run past it, and the draw clips them.
 */
static void _across(const UISystem* ui, UIElement* el, float width) {
    UITable* t = el->table;
    const float gap = ui_element_spacing(ui, el), gaps = _gaps(ui, el);
    float used = 0.0f;
    int growing = 0;
    for (int c = 0; c < t->column_count; c++) {
        const UITableColumn* col = &t->columns[c];
        t->w[c] = col->size == UI_SHARE ? fmaxf(floorf(col->length * (width - gaps)), 0.0f)
                                        : _column_own(t, c);
        used += t->w[c];
        growing += col->size == UI_GROW;
    }
    const float rest = width - gaps - used;
    const float share = growing > 0 && rest > 0.0f ? rest / (float)growing : 0.0f;
    float at = 0.0f;
    for (int c = 0; c < t->column_count; c++) {
        if (t->columns[c].size == UI_GROW)
            t->w[c] = share;
        t->x[c] = floorf(at);
        at += t->w[c] + gap;
    }
}

// The rows' heights as summed; none before any row is measured.
static float _total(const UITable* t) {
    return t->tops ? (float)t->tops[t->summed] : 0.0f;
}

// The rows' view: the content under the header.
static UIRect _rows_view(const UITable* t) {
    const float h = fmaxf(t->content.h - t->header_h, 0.0f);
    return (UIRect){t->content.x, t->content.y + t->header_h, t->content.w, h};
}

// What of the rows' view shows: met with what the panes round the table show, from the header's
// foot down. The rows' view itself for a table no pane scrolls.
static UIRect _rows_shown(const UITable* t) {
    const UIRect view = _rows_view(t);
    const float top = fmaxf(fmaxf(view.y, t->shown.y), t->header_y + t->header_h);
    const float bottom = fminf(view.y + view.h, t->shown.y + t->shown.h);
    return (UIRect){view.x, top, view.w, fmaxf(bottom - top, 0.0f)};
}

// Across, its columns' own widths and the gaps, a SHARE or GROW one none; down, its header and
// every row, or its header alone while it grows, since whatever height it is given its rows are
// seen through it.
static float _content(const UISystem* ui, UIElement* el, const UIStyle* s, int axis) {
    const UITable* t = el->table;
    _settle(ui, el, s);
    if (axis == 0) {
        float sum = _gaps(ui, el);
        for (int c = 0; c < t->column_count; c++)
            sum += _column_own(t, c);
        return sum;
    }
    if (el->size_mode[1] == UI_GROW)
        return t->header_h;
    return t->header_h + _total(t);
}

static void _width(const UISystem* ui, UIElement* el, const UIStyle* s, float width) {
    (void)width;
    _settle(ui, el, s);
}

// How far the rows can scroll; 0 while they fit.
static float _most(const UITable* t) {
    return fmaxf(_total(t) - _rows_view(t).h, 0.0f);
}

// The row at `y` down the rows as summed: the last that starts at or above it.
static int _row_at(const UITable* t, double y) {
    int lo = 0, hi = t->summed;
    while (hi - lo > 1) {
        const int mid = lo + (hi - lo) / 2;
        if (t->tops[mid] <= y)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

// The least scroll from where the rows are going that shows row `row` whole.
static float _reveal_target(const UIElement* el, int row) {
    const UITable* t = el->table;
    const float y0 = (float)t->tops[row];
    return ui_least_scroll(el->scroll_target, y0, y0 + t->heights[row], _rows_view(t).h);
}

// The rows eased toward the least scroll that shows row `row`: now, if every row is measured and
// summed as it stands, else at the next layout, from the tops it sums. Where its rows cannot scroll
// to it, the panes round it show it, at the next place.
static void _reveal(UIElement* el, int row) {
    UITable* t = el->table;
    t->shown_row = row;
    if (!t->laid || t->every || t->stale_lo != INT_MAX || t->summed != t->count) {
        t->reveal = row;
        return;
    }
    el->scroll_target = glm_clamp(_reveal_target(el, row), 0.0f, _most(t));
}

// The columns across the content, the scroll held inside the rows, and a row a reveal asked for
// before it was measured shown.
static void _place(const UISystem* ui, UIElement* el, UIRect content) {
    UITable* t = el->table;
    const Engine* engine = ui_system_engine(ui);
    t->pixel = engine && engine->fb_width > 0 && engine->win_width > 0
                   ? (float)engine->win_width / (float)engine->fb_width
                   : 1.0f;
    t->step = ui_scroll_step(ui);
    t->content = content;
    t->laid = true;
    _across(ui, el, content.w);
    if (t->reveal >= 0 && t->reveal < t->summed)
        el->scroll_target = _reveal_target(el, t->reveal);
    t->reveal = -1;
    ui_scroll_settle(el, _total(t), _rows_view(t).h);
    // The header held at the top of what the panes show while the table's top is above it, and
    // going up with the last row once that rises past it.
    t->shown = ui_shown_rect(ui, el);
    t->header_y = content.y;
    if (t->shown.y > content.y)
        t->header_y = fminf(t->shown.y, content.y + content.h - t->header_h);
    // A row the rows cannot scroll to, shown whole by the panes round the table, under the header.
    const int row = t->shown_row;
    t->shown_row = -1;
    if (row >= 0 && row < t->summed && _most(t) <= 0.0f) {
        const float y = _rows_view(t).y + (float)(t->tops[row] - (double)el->scroll);
        ui_reveal_span(ui, el, y - t->header_h, y + t->heights[row]);
    }
}

void ui_table_scroll_to_row(const UISystem* ui, UIElement* table, int row) {
    (void)ui;
    UITable* t = _table("ui_table_scroll_to_row", table);
    if (!t)
        return;
    if (row < 0 || row >= t->count) {
        log_error("ui_table_scroll_to_row: no row %d of %d", row, t->count);
        return;
    }
    _reveal(table, row);
}

// ---- Scrolling, which the wheel and the bar drive, in points

// The rows' view of them, the bar beside the rows alone; false while every row shows.
static bool _scroll(const UIElement* el, UIScrollPlace* place) {
    const UITable* t = el->table;
    if (!t->laid)
        return false;
    const UIRect view = _rows_view(t);
    const float most = _most(t);
    *place = (UIScrollPlace){
        .view = view.h,
        .total = _total(t),
        .at = most > 0.0f ? el->scroll / most : 0.0f,
        .step = t->step,
        .along = {el->rect.x, view.y, el->rect.w, el->rect.y + el->rect.h - view.y},
    };
    return most > 0.0f;
}

// The wheel and the bar move the rows at once: what eases is a reveal.
static void _scroll_to(UIElement* el, float at) {
    el->scroll = el->scroll_target = glm_clamp(at, 0.0f, 1.0f) * _most(el->table);
}

static void _scroll_by(UIElement* el, float by) {
    el->scroll = el->scroll_target = glm_clamp(el->scroll_target + by, 0.0f, _most(el->table));
}

bool ui_table_cell_rect(const UIElement* table, int row, int column, UIRect* out) {
    const UITable* t = _seen("ui_table_cell_rect", table);
    if (!t || !t->laid || row < 0 || row >= t->summed || column < -1 || column >= t->column_count)
        return false;
    const UIRect shown = _rows_shown(t);
    const float y = _rows_view(t).y + (float)(t->tops[row] - (double)table->scroll);
    const float h = t->heights[row];
    if (y >= shown.y + shown.h || y + h <= shown.y)
        return false;
    if (out)
        *out = column < 0 ? (UIRect){t->content.x, y, t->content.w, h}
                          : (UIRect){t->content.x + t->x[column], y, t->w[column], h};
    return true;
}

bool ui_table_header_rect(const UIElement* table, UIRect* out) {
    const UITable* t = _seen("ui_table_header_rect", table);
    if (!t || !t->laid || t->header_h <= 0.0f)
        return false;
    if (out)
        *out = (UIRect){t->content.x, t->header_y, t->content.w, t->header_h};
    return true;
}

// ---- Drawing

// `lines` lines of `text` centred down `cell`, across it by `align`, in `s`; clipped to the cell
// only when they are taller than it, since the column's clip holds them across.
static void _draw_lines(UIDrawList* dl, UIRect cell, const char* text, int lines, const UIStyle* s,
                        UIAlign align) {
    const float h = (float)lines * ui_line_height(s->font, s->font_size, s->line_spacing);
    const bool over = h > cell.h;
    if (over)
        ui_push_clip(dl, cell);
    ui_draw_text(dl, (UIRect){cell.x, cell.y + (cell.h - h) * 0.5f, cell.w, h}, text, s, align);
    if (over)
        ui_pop_clip(dl);
}

// The header: each title in its column's alignment, and a pixel's rule along its foot, in the
// header's colour.
static void _draw_header(UIDrawList* dl, const UITable* t, const UIStyle* s, const UITheme* theme) {
    UIStyle hs = *s;
    if (theme->table_header[3] > 0.0f)
        glm_vec4_copy((float*)theme->table_header, hs.fg);
    else
        hs.fg[3] *= 0.6f;
    const UIRect head = {t->content.x, t->header_y, t->content.w, t->header_h};
    for (int c = 0; c < t->column_count; c++) {
        const char* title = t->columns[c].title;
        const int lines = _lines_of(title);
        if (lines == 0)
            continue;
        const UIRect cell = {head.x + t->x[c], head.y, t->w[c], head.h};
        ui_push_clip(dl, cell);
        _draw_lines(dl, cell, title, lines, &hs, t->columns[c].align);
        ui_pop_clip(dl);
    }
    ui_draw_quad(dl, (UIRect){head.x, head.y + head.h - t->pixel, head.w, t->pixel}, hs.fg);
}

// What is drawn behind the cursor's row: the theme's, else the selection colour.
static void _cursor_colour(const UITheme* theme, vec4 out) {
    if (theme->table_cursor[3] > 0.0f)
        glm_vec4_copy((float*)theme->table_cursor, out);
    else
        ui_selection_colour(theme, out);
}

// The rows shown: the cursor's fill behind its row, then column by column under each column's clip
// a text cell's lines, or a drawn cell handed its rect under a clip of its own.
static void _draw_rows(UIDrawList* dl, const UIElement* el, const UIStyle* s,
                       const UITheme* theme) {
    const UITable* t = el->table;
    const UIRect shown = _rows_shown(t);
    if (t->summed == 0 || shown.h <= 0.0f)
        return;
    // A row's top in the window is its top down the rows, from the rows' view's top, less the
    // scroll; what shows starts `from` down the rows.
    const float top = _rows_view(t).y;
    const double scroll = el->scroll, from = (double)(shown.y - top) + scroll;
    const int first = _row_at(t, from), last = _row_at(t, from + (double)shown.h);
    ui_push_clip(dl, shown);
    if (t->cursor >= first && t->cursor <= last) {
        vec4 fill = {0};
        _cursor_colour(theme, fill);
        ui_draw_quad(dl,
                     (UIRect){shown.x, top + (float)(t->tops[t->cursor] - scroll), shown.w,
                              t->heights[t->cursor]},
                     fill);
    }
    for (int c = 0; c < t->column_count; c++) {
        const UITableColumn* col = &t->columns[c];
        const float x = t->content.x + t->x[c];
        ui_push_clip(dl, (UIRect){x, shown.y, t->w[c], shown.h});
        for (int r = first; r <= last; r++) {
            const UIRect cell = {x, top + (float)(t->tops[r] - scroll), t->w[c], t->heights[r]};
            if (col->drawn) {
                if (!t->source.draw)
                    continue;
                ui_push_clip(dl, cell);
                t->source.draw(dl, cell, r, c, r == t->cursor, t->source.user);
                ui_pop_clip(dl);
                continue;
            }
            UITableCell out = {0};
            t->source.cell(r, c, &out, t->source.user);
            const int lines = _lines_of(out.text);
            if (lines == 0)
                continue;
            UIStyle cs = *s;
            if (out.colour[3] > 0.0f)
                glm_vec4_copy(out.colour, cs.fg);
            _draw_lines(dl, cell, out.text, lines, &cs, col->align);
        }
        ui_pop_clip(dl);
    }
    ui_pop_clip(dl);
}

// Its box, its header and its rows, inside its content: the bar is ui.c's, over them.
static double _draw(const UISystem* ui, UIElement* el, const UIStyle* s, UIRect content,
                    bool focused, double now, bool blink) {
    (void)content;
    (void)focused;
    (void)now;
    (void)blink;
    UIDrawList* dl = ui_draw_list(ui);
    ui_draw_rect(dl, el->rect, s);
    const UITable* t = el->table;
    if (!t->laid)
        return INFINITY;
    ui_push_clip(dl, t->content);
    if (t->header_h > 0.0f)
        _draw_header(dl, t, s, ui_theme(ui));
    _draw_rows(dl, el, s, ui_theme(ui));
    ui_pop_clip(dl);
    return INFINITY;
}

// ---- The cursor

// Whether the cursor may stand on row `row`.
static bool _holds(const UITable* t, int row) {
    return row >= 0 && row < t->count && (!t->source.holds || t->source.holds(row, t->source.user));
}

// The first row from `from` on, stepping `dir`, that holds the cursor; -1 for none.
static int _holding(const UITable* t, int from, int dir) {
    for (int r = from; r >= 0 && r < t->count; r += dir)
        if (_holds(t, r))
            return r;
    return -1;
}

// Where `by` rows that hold it take the cursor from `from`, stopping at the first and the last. No
// cursor stands before the first going down and past the last going up, so the first Down puts it
// on the first row and Up on the last.
static int _stepped(const UITable* t, int from, int by) {
    const int dir = by < 0 ? -1 : 1;
    int steps = by < 0 ? -by : by;
    int at = from;
    if (at < 0) {
        at = _holding(t, dir > 0 ? 0 : t->count - 1, dir);
        if (at < 0)
            return -1;
        steps--;
    }
    for (; steps > 0; steps--) {
        const int next = _holding(t, at + dir, dir);
        if (next < 0)
            break;
        at = next;
    }
    return at;
}

// A view's rows on from the cursor, by `dir`: the row the height of the rows shown from its top, or
// the nearest on from it that holds the cursor, else the nearest back.
static int _paged(const UIElement* el, int dir) {
    const UITable* t = el->table;
    if (t->cursor < 0 || t->cursor >= t->summed)
        return _stepped(t, t->cursor, dir);
    const double y = t->tops[t->cursor] + (double)dir * (double)_rows_shown(t).h;
    const int row = y > 0.0 ? _row_at(t, y) : 0;
    const int on = _holding(t, row, dir);
    return on >= 0 ? on : _holding(t, row, -dir);
}

// The cursor put on `row`, -1 for none, and its row shown; true when it moved.
static bool _put(UIElement* el, int row) {
    UITable* t = el->table;
    const bool moved = row != t->cursor;
    t->cursor = row;
    if (row >= 0)
        _reveal(el, row);
    return moved;
}

// The person moved the cursor: on_cursor told now, or at the end of a turn of keys, once.
static void _person_moved(UIElement* el) {
    UITable* t = el->table;
    if (t->in_keys)
        t->moved = true;
    else if (t->on_cursor)
        t->on_cursor(el, t->cursor_user);
}

// The person put the cursor on `row`, -1 for nowhere to go: on_cursor follows if it moved.
static void _person_put(UIElement* el, int row) {
    if (row >= 0 && _put(el, row))
        _person_moved(el);
}

int ui_table_cursor(const UIElement* table) {
    const UITable* t = _seen("ui_table_cursor", table);
    return t ? t->cursor : -1;
}

void ui_table_set_cursor(UIElement* table, int row) {
    const UITable* t = _table("ui_table_set_cursor", table);
    if (!t)
        return;
    if (row != -1 && !_holds(t, row)) {
        log_error("ui_table_set_cursor: row %d is out of range or cannot hold the cursor", row);
        return;
    }
    _put(table, row);
}

void ui_table_move_cursor(UIElement* table, int by) {
    const UITable* t = _table("ui_table_move_cursor", table);
    if (!t || by == 0)
        return;
    _person_put(table, _stepped(t, t->cursor, by));
}

void ui_table_set_on_cursor(UIElement* table, UIActionFn on_cursor, void* user) {
    UITable* t = _table("ui_table_set_on_cursor", table);
    if (!t)
        return;
    t->on_cursor = on_cursor;
    t->cursor_user = user;
}

// ---- The keys and the pointer

static void _keys_begin(UIElement* el) {
    el->table->in_keys = true;
}

static void _keys_end(UIElement* el) {
    UITable* t = el->table;
    t->in_keys = false;
    if (t->moved) {
        t->moved = false;
        _person_moved(el);
    }
}

// Up and Down a row that holds the cursor, Page Up and Page Down a view, Home and End the first and
// the last; any other key, and every character, is the screen's.
static bool _key(const UISystem* ui, UIElement* el, UIRect content, const InputKeyEvent* ev) {
    (void)ui;
    (void)content;
    const UITable* t = el->table;
    int to;
    switch (ev->key) {
        case GLFW_KEY_UP:
            to = _stepped(t, t->cursor, -1);
            break;
        case GLFW_KEY_DOWN:
            to = _stepped(t, t->cursor, 1);
            break;
        case GLFW_KEY_PAGE_UP:
            to = _paged(el, -1);
            break;
        case GLFW_KEY_PAGE_DOWN:
            to = _paged(el, 1);
            break;
        case GLFW_KEY_HOME:
            to = _holding(t, 0, 1);
            break;
        case GLFW_KEY_END:
            to = _holding(t, t->count - 1, -1);
            break;
        default:
            return false;
    }
    _person_put(el, to);
    return true;
}

// A press on a row that holds the cursor puts it there; one on the header, below the last row or
// on a row that cannot hold it does nothing.
static void _press(const UISystem* ui, UIElement* el, UIRect content, float x, float y, int mods,
                   double time) {
    (void)ui;
    (void)content;
    (void)x;
    (void)mods;
    (void)time;
    const UITable* t = el->table;
    const UIRect shown = _rows_shown(t);
    if (!t->laid || t->summed == 0 || y < shown.y || y >= shown.y + shown.h)
        return;
    const double at = (double)(y - _rows_view(t).y) + (double)el->scroll;
    const int row = _row_at(t, at);
    if (at < t->tops[t->summed] && _holds(t, row))
        _person_put(el, row);
}

const UIWidgetOps ui_table_ops = {
    .content = _content,
    .width = _width,
    .place = _place,
    .keys_begin = _keys_begin,
    .keys_end = _keys_end,
    .key = _key,
    .press = _press,
    .draw = _draw,
    .scroll = _scroll,
    .scroll_to = _scroll_to,
    .scroll_by = _scroll_by,
};
