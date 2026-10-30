#ifndef _UI_FIELD_H_
#define _UI_FIELD_H_

/*
 * The text field's half of the UI (spec 14.2), private to the UI's files: ui.c lays a field out,
 * routes input to the focused one and draws it through these, and ui_field.c holds the editing --
 * stb_textedit -- and everything that knows a field's insides.
 */

#include "ui.h"

struct Engine;

// The engine a UI system was made with, for the clipboard; ui.c's.
struct Engine* ui_system_engine(const UISystem* ui);

// A field's state, made for its element by ui_text_field and freed with it.
struct UIField* ui_field_make(UIElement* el, unsigned flags);
void ui_field_free(struct UIField* f);
// The UI_FIELD_* flags it was made with; 0 for anything not a field.
unsigned ui_field_flags(const UIElement* el);
// What a test reads of a field's insides: its caret, its selection's two ends, its content's
// scroll and how many rows it is broken into. Any output may be NULL.
void ui_field_inspect(const UIElement* el, int* cursor, int* select_start, int* select_end,
                      float* scroll_x, float* scroll_y, size_t* rows);

// Breaks the field into rows at `width`, its content's width, after the layout's width pass, in
// its resolved style: a multi-line field wraps there, a single-line field is one row.
void ui_field_break(const UISystem* ui, UIElement* el, const UIStyle* s, float width);
// The height of the field's content: one line, or its rows up to its most.
float ui_field_content_height(const UIElement* el);

// What a field did with a pass's input that the UI has to act on.
typedef enum {
    UI_FIELD_KEPT = 0, // nothing for the UI
    UI_FIELD_BACK,     // Esc: close a modal screen, or let go of the focus
    UI_FIELD_NEXT,     // Tab: the next focusable element in tree order
    UI_FIELD_PREVIOUS, // Shift+Tab: the one before
} UIFieldVerdict;

// The focused field takes the pass's keys and characters, in order, the field's on_key first.
UIFieldVerdict ui_field_keys(const UISystem* ui, UIElement* el, const UIInput* in);
// Whether the focused field uses a key itself, so a pane round it does not scroll for it.
bool ui_field_uses_key(const UIElement* el, int key);
// A press on the field at the pointer, `content` its content box: the caret placed, the selection
// extended with Shift held, a word selected by a second press close in time and the line by a
// third.
void ui_field_press(UIElement* el, UIRect content, float x, float y, bool extend, double now);
// The pointer held after a press on the field: the selection follows it.
void ui_field_drag(UIElement* el, UIRect content, float x, float y);

// Draws the field in `s` with its content in `content`: its box, its selection, its text or its
// placeholder, and -- when it holds the focus, is editable and the caret is lit -- its caret. `now`
// is the draw's clock; with `blink` the caret is lit 0.8 s and dark 0.4 s from its last move,
// and otherwise always lit. The seconds until the caret next changes, or INFINITY.
double ui_field_draw(const UISystem* ui, UIElement* el, const UIStyle* s, UIRect content,
                     bool focused, double now, bool blink);

#endif // _UI_FIELD_H_
