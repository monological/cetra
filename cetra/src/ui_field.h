#ifndef _UI_FIELD_H_
#define _UI_FIELD_H_

/*
 * The text field's half of the UI (spec 14.2), private to the UI's files: ui.c lays a field out,
 * routes input to the focused one and draws it through ui_field_ops (ui_internal.h), and
 * ui_field.c holds the editing -- stb_textedit -- and everything that knows a field's insides.
 */

#include "ui.h"

// A field's state, made for its element by ui_text_field and freed with it.
struct UIField* ui_field_make(UIElement* el, unsigned flags);
void ui_field_free(struct UIField* f);
// The UI_FIELD_* flags it was made with; 0 for anything not a field.
unsigned ui_field_flags(const UIElement* el);
// What a test reads of a field's insides: its caret, its selection's two ends, its content's
// scroll and how many rows it is broken into. Any output may be NULL.
void ui_field_inspect(const UIElement* el, int* cursor, int* select_start, int* select_end,
                      float* scroll_x, float* scroll_y, size_t* rows);

// The element's runs were set: the field's rows are broken again in their faces now, for an edit
// or an append before the next layout.
void ui_field_runs_changed(UIElement* el);
// The height of the field's content: one line, or its rows up to its most.
float ui_field_content_height(const UIElement* el);

#endif // _UI_FIELD_H_
