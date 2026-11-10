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

    // Each cell's widest line as last measured, row by row, `stride` cells to a row; each row's
    // height, and where it starts below the header, count + 1 of them. Doubles, since 100,000 rows
    // reach millions of points, past which a float cannot hold a sum to a point.
    float* widths;
    float* heights;
    double* tops;
    bool* stale; // a row to measure at the next layout
    size_t widths_capacity, heights_capacity, tops_capacity, stale_capacity;
    int stride;
    int stale_lo, stale_hi; // every stale row, in [lo, hi)
    bool every;             // every row stale, whatever `stale` says
    bool widest_stale;      // a row taken off may have been a column's widest

    // What the measures are for: another face, size, tracking, line spacing or row padding
    // measures every row and every title again.
    const Font* font;
    float size, tracking, line_spacing, row_padding;
    bool titles_stale;
    float widest[UI_TABLE_COLUMNS_MOST];      // a column's cells', over every row
    float title_width[UI_TABLE_COLUMNS_MOST]; // its title's
    float header_h;                           // 0 for no header

    // As the last layout placed it: its content rect, each column's start in it and width, the
    // rows it placed, the points of a pixel and a wheel unit's.
    bool laid;
    UIRect content;
    float x[UI_TABLE_COLUMNS_MOST], w[UI_TABLE_COLUMNS_MOST];
    int placed;
    float pixel, step;
};

struct UITable* ui_table_make(void) {
    UITable* t = calloc(1, sizeof(UITable));
    if (t) {
        t->stale_lo = INT_MAX;
        t->titles_stale = true;
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

// Every row and every title measured again at the next layout.
static void _measure_all(UITable* t) {
    t->every = true;
    t->titles_stale = true;
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
    _measure_all(t);
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
    _measure_all(t);
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
    if (count > t->count)
        _measure_rows(t, t->count, count);
    else
        t->widest_stale = true;
    t->stale_hi = t->stale_hi < count ? t->stale_hi : count;
    t->count = count;
}

void ui_table_rows_changed(UIElement* table, int first, int count) {
    UITable* t = _table("ui_table_rows_changed", table);
    if (!t)
        return;
    const long long lo = first > 0 ? first : 0, hi = (long long)first + count;
    _measure_rows(t, (int)lo, (int)(hi < t->count ? hi : t->count));
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
    t->titles_stale = false;
}

// Row `r` measured: each text cell's widest line, counted into `measures`, and its height.
static void _measure_row(const UISystem* ui, UIElement* el, const UIStyle* s, int r, float line_h) {
    UITable* t = el->table;
    int lines = 0;
    for (int c = 0; c < t->column_count; c++) {
        float* w = &t->widths[(size_t)r * (size_t)t->stride + (size_t)c];
        *w = 0.0f;
        if (t->columns[c].drawn)
            continue;
        UITableCell cell = {0};
        t->source.cell(r, c, &cell, t->source.user);
        *w = _text_width(ui, s, cell.text);
        const int n = _lines_of(cell.text);
        lines = n > lines ? n : lines;
        el->measures++;
    }
    t->heights[r] = t->source.height
                        ? fmaxf(t->source.height(r, t->source.user), 0.0f)
                        : (float)(lines > 1 ? lines : 1) * line_h + 2.0f * t->row_padding;
}

/*
 * What is stale measured: every row and title after the columns, the rows or the style changed,
 * else the rows marked. Then, if anything was, each column's widest over every row kept and the
 * rows' tops summed again: a row that was a column's widest and shrank narrows it.
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
        _measure_all(t);
    }
    const float line_h = ui_line_height(s->font, s->font_size, s->line_spacing);
    if (t->titles_stale)
        _measure_titles(ui, t, s, line_h);
    const bool every = t->every;
    const int lo = every ? 0 : t->stale_lo, hi = every ? t->count : t->stale_hi;
    if (every)
        t->stride = t->column_count;
    bool measured = false;
    for (int r = lo; r < hi; r++) {
        if (!every && !t->stale[r])
            continue;
        t->stale[r] = false;
        _measure_row(ui, el, s, r, line_h);
        measured = true;
    }
    if (every)
        memset(t->stale, 0, (size_t)t->count * sizeof(bool));
    t->every = false;
    t->stale_lo = INT_MAX;
    t->stale_hi = 0;
    if (!measured && !every && !t->widest_stale)
        return;
    t->widest_stale = false;
    for (int c = 0; c < t->column_count; c++) {
        float most = 0.0f;
        for (int r = 0; r < t->count; r++)
            most = fmaxf(most, t->widths[(size_t)r * (size_t)t->stride + (size_t)c]);
        t->widest[c] = most;
    }
    t->tops[0] = 0.0;
    for (int r = 0; r < t->count; r++)
        t->tops[r + 1] = t->tops[r] + (double)t->heights[r];
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

// Every row's height summed; none before any row is measured.
static float _total(const UITable* t) {
    return t->tops ? (float)t->tops[t->count] : 0.0f;
}

// The rows' view: the content under the header.
static UIRect _rows_view(const UITable* t) {
    const float h = fmaxf(t->content.h - t->header_h, 0.0f);
    return (UIRect){t->content.x, t->content.y + t->header_h, t->content.w, h};
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

// The columns across the content, and the scroll held inside the rows.
static void _place(const UISystem* ui, UIElement* el, UIRect content) {
    UITable* t = el->table;
    const Engine* engine = ui_system_engine(ui);
    t->pixel = engine && engine->fb_width > 0 && engine->win_width > 0
                   ? (float)engine->win_width / (float)engine->fb_width
                   : 1.0f;
    t->step = ui_scroll_step(ui);
    t->content = content;
    t->laid = true;
    t->placed = t->count;
    _across(ui, el, content.w);
    const float total = _total(t);
    const float most = fmaxf(total - _rows_view(t).h, 0.0f);
    el->scroll_content = total;
    el->scroll = glm_clamp(el->scroll, 0.0f, most);
    el->scroll_target = glm_clamp(el->scroll_target, 0.0f, most);
}

bool ui_table_cell_rect(const UIElement* table, int row, int column, UIRect* out) {
    const UITable* t = _seen("ui_table_cell_rect", table);
    if (!t || !t->laid || row < 0 || row >= t->placed || column < -1 || column >= t->column_count)
        return false;
    const UIRect view = _rows_view(t);
    const float y = view.y + (float)(t->tops[row] - (double)table->scroll);
    const float h = t->heights[row];
    if (y >= view.y + view.h || y + h <= view.y)
        return false;
    if (out)
        *out = column < 0 ? (UIRect){t->content.x, y, t->content.w, h}
                          : (UIRect){t->content.x + t->x[column], y, t->w[column], h};
    return true;
}

// ---- Drawing

static double _draw(const UISystem* ui, UIElement* el, const UIStyle* s, UIRect content,
                    bool focused, double now, bool blink) {
    (void)content;
    (void)focused;
    (void)now;
    (void)blink;
    ui_draw_rect(ui_draw_list(ui), el->rect, s);
    return INFINITY;
}

// ---- The keys and the pointer

static bool _key(const UISystem* ui, UIElement* el, UIRect content, const InputKeyEvent* ev) {
    (void)ui;
    (void)el;
    (void)content;
    (void)ev;
    return false;
}

static void _press(const UISystem* ui, UIElement* el, UIRect content, float x, float y, int mods,
                   double time) {
    (void)ui;
    (void)el;
    (void)content;
    (void)x;
    (void)y;
    (void)mods;
    (void)time;
}

const UIWidgetOps ui_table_ops = {
    .content = _content,
    .width = _width,
    .place = _place,
    .key = _key,
    .press = _press,
    .draw = _draw,
};
