#ifndef _UI_EDITOR_HOOKS_H_
#define _UI_EDITOR_HOOKS_H_

/*
 * What the UI calls on a code editor element (spec 14.4), and what the probe reads of it. Private:
 * ui.c lays an editor out, routes the pointer and the keys to it and draws it through these, as it
 * does a text field through ui_field.h. Each takes the element's content rect -- its rect less its
 * padding -- and settles the editor's geometry from it first: the face at its size, the gutter,
 * the columns.
 */

#include "input.h"
#include "ui_editor.h"

struct UIEditor* ui_editor_make(UIElement* el, TextDoc* doc);
void ui_editor_free(struct UIEditor* ed);

// A press at the point, with the modifiers held: a click places a caret, a second one takes a word
// and a third a line; Shift extends the primary selection and Alt adds a caret. On the bar, the
// thumb is taken or the view paged toward the pointer.
void ui_editor_press(const UISystem* ui, UIElement* el, UIRect content, float x, float y, int mods,
                     double time);
// The pointer held after a press: the selection follows it, by the unit the press took.
void ui_editor_drag(const UISystem* ui, UIElement* el, UIRect content, float x, float y);
// The wheel, in its units, + away.
void ui_editor_wheel(const UISystem* ui, UIElement* el, UIRect content, float wheel);
// One key while the editor holds the focus; true when it used it.
bool ui_editor_key(const UISystem* ui, UIElement* el, UIRect content, const InputKeyEvent* ev);
// Draws it; the seconds until its caret next changes, INFINITY when it will not.
double ui_editor_draw(const UISystem* ui, UIElement* el, const UIStyle* s, UIRect content,
                      bool focused, double now, bool blink);

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

#endif // _UI_EDITOR_HOOKS_H_
