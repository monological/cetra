#ifndef _UI_TABLE_H_
#define _UI_TABLE_H_

/*
 * The table's calls (spec 14.30). The element itself is ui_table, in ui.h, beside the other kinds.
 *
 * Its columns are sized across every row, in the four modes an element is, and its rows are read
 * from the app's source as they are needed: a cell's text, a cell drawn by a callback, a row's
 * height, whether the cursor may stand on a row. No element is made for a cell, so a list of
 * 100,000 rows costs its measures and nothing per row it does not show.
 */

#include <stdbool.h>

#include "ui.h"

#define UI_TABLE_COLUMNS_MOST 32

// A column, copied when set.
typedef struct UITableColumn {
    const char* title; // the header's words, copied; NULL for none. No column with one, no header
    UISize size;       // FIT, FIXED, SHARE or GROW, across every row
    float length;      // FIXED's points, SHARE's share (0 to 1); FIT and GROW ignore it
    UIAlign align;     // its cells' text across them: START, CENTER or END
    bool drawn;        // its cells drawn by the source's `draw` rather than as text
} UITableColumn;

// A cell's text as the source gives it, read before the source is asked again.
typedef struct UITableCell {
    const char* text; // UTF-8, its lines broken at '\n'; NULL or "" for none
    vec4 colour;      // alpha 0 = the table's text colour
} UITableCell;

// What the table asks the app, `user` handed back to each call.
typedef struct UITableSource {
    // Row `row`'s cell in a column not drawn.
    void (*cell)(int row, int column, UITableCell* out, void* user);
    // A drawn column's cell, clipped to `cell`, in window points; `cursor` true on the cursor's
    // row. NULL with no column drawn.
    void (*draw)(UIDrawList* dl, UIRect cell, int row, int column, bool cursor, void* user);
    // Row `row`'s whole height in points; NULL for its tallest cell's lines and their padding.
    float (*height)(int row, void* user);
    // Whether the cursor may stand on row `row`; NULL for every row.
    bool (*holds)(int row, void* user);
    void* user;
} UITableSource;

// The columns, copied, every row measured again at the next layout. Refused by name: none, more
// than UI_TABLE_COLUMNS_MOST, a drawn column sized FIT, which has no text to fit, and a call on
// anything not a table.
void ui_table_set_columns(UIElement* table, const UITableColumn* columns, int count);
// `count` rows read through `source` (copied): every row measured at the next layout, the cursor
// on none and the scroll at the top. Refused by name for a count below 0 or a source with no
// `cell`.
void ui_table_set_rows(UIElement* table, int count, const UITableSource* source);
// The rows grown or shrunk at their end, keeping every measure: only the rows added are measured.
// A cursor on a row taken off is on none.
void ui_table_set_row_count(UIElement* table, int count);
// Rows `first` to `first + count` changed: measured again at the next layout and drawn as the
// source now gives them. Refused by name for rows not among the table's.
void ui_table_rows_changed(UIElement* table, int first, int count);
// Every row's height asked of the source's `height` again at the next layout, its cells' widths
// kept: for rows sized to the room the app has, as that room changes. Nothing to do for a source
// with no `height`, whose rows are as tall as their lines.
void ui_table_heights_changed(UIElement* table);

// The cursor's row; -1 for none.
int ui_table_cursor(const UIElement* table);
// The cursor put on `row`, or on none for -1, and the least scroll that shows it; no on_cursor,
// since the app did it. Refused by name for a row out of range or one the source says cannot hold
// it.
void ui_table_set_cursor(UIElement* table, int row);
// The cursor moved `by` rows that can hold it, stopping at the first and the last, as the keys move
// it: on_cursor follows. What a host's own keys call -- pcb's j and k.
void ui_table_move_cursor(UIElement* table, int by);
// Told when the person moved the cursor: once at the end of a turn of the table's keys, however
// many moved it, ui_table_move_cursor from its key hook among them; at once for a press, and for
// ui_table_move_cursor anywhere else. Never for a cursor left where it was.
void ui_table_set_on_cursor(UIElement* table, UIActionFn on_cursor, void* user);

// The least scroll that shows row `row`, easing there: of its own rows, then of each scrolling pane
// round it, the row whole under the header.
void ui_table_scroll_to_row(const UISystem* ui, UIElement* table, int row);
// Where row `row`'s cell in `column` stands, in window points, as the last layout placed it, the
// whole row's for column -1; false for a row not in view: out of the table's own, out of what the
// scrolling panes round it show, or under its header.
bool ui_table_cell_rect(const UIElement* table, int row, int column, UIRect* out);
// Where its header stands, in window points, as the last layout placed it: at its content's top, or
// held at the top of what the scrolling panes round it show while its top is above that. False with
// no header, or before the first layout. For an element beside the table drawing level with its
// rows: its own title level with the header, its rows kept under it.
bool ui_table_header_rect(const UIElement* table, UIRect* out);

#endif // _UI_TABLE_H_
