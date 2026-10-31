#ifndef _UI_EDITOR_HOOKS_H_
#define _UI_EDITOR_HOOKS_H_

/*
 * A code editor element's private half (spec 14.4): its making and freeing, and where its rows and
 * its carets are as its last layout left them. ui.c lays an editor out, routes the pointer and the
 * keys to it and draws it through ui_editor_ops (ui_internal.h), as it does a text field.
 */

#include "ui_editor.h"

struct UIEditor* ui_editor_make(UIElement* el, TextDoc* doc);
void ui_editor_free(struct UIEditor* ed);

// The rows a line's content breaks into at `cols` columns: up to `max` of their starts, in bytes
// of the line, into `starts`, and their count. *hang is the column a wrapped line's later rows
// start at.
size_t ui_editor_wrap(const char* line, size_t len, int cols, int tab_width, size_t* starts,
                      size_t max, int* hang);
// Where the caret before `offset` stands, its row's top, in points, as the last pass laid the
// editor out; false while it is not on screen.
bool ui_editor_point_of(UIElement* el, size_t offset, float* x, float* y);
// The offset a press at the point places a caret at, as the last pass laid the editor out.
size_t ui_editor_offset_at(UIElement* el, float x, float y);
// The line and row at the top of the view, and the columns a row holds.
void ui_editor_top(const UIElement* el, size_t* line, size_t* row, int* columns);
// Whether the budget has nothing left to do: every line's rows wrapped rather than guessed, and a
// stateful highlighter's walk past every line drawn.
bool ui_editor_settled(const UIElement* el);

#endif // _UI_EDITOR_HOOKS_H_
