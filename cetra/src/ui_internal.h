#ifndef _UI_INTERNAL_H_
#define _UI_INTERNAL_H_

/*
 * What the UI's own files share, private to them: the engine a system was made with, the scroll bar
 * every element that scrolls draws, and the table of calls through which ui.c lays out, routes
 * input to and draws an element that keeps state of its own -- the text field and the code editor
 * -- so that ui.c names no such kind but where it picks the table.
 */

#include "input.h"
#include "ui.h"

struct Engine;

// The engine a UI system was made with, for the clipboard and the screen's scale.
struct Engine* ui_system_engine(const UISystem* ui);

// The bar: its width, its inset from the element's edge, its shortest thumb, and the strip along
// the element's right edge a press lands on it from.
#define UI_BAR_W     6.0f
#define UI_BAR_INSET 2.0f
#define UI_BAR_MIN   24.0f
#define UI_BAR_HIT   14.0f

// Where an element that scrolls is, in units of its own: how much shows, how much there is, how
// far along it the view is, 0..1, and how far a unit of the wheel moves it.
typedef struct UIScrollPlace {
    float view, total, at, step;
} UIScrollPlace;

// A bar's track along `rect`'s right edge and its thumb: the thumb's length the share in view and
// its place the share scrolled. False while everything shows, when there is no bar.
bool ui_bar_place(UIRect rect, const UIScrollPlace* place, UIRect* track, UIRect* thumb);
// Where a thumb taken `grab` below its top and held at `y` puts the view, 0..1 along.
float ui_bar_at(UIRect track, UIRect thumb, float y, float grab);

/*
 * An element with state of its own, through which ui.c reaches it. The calls past the layout's take
 * the element's content rect, its rect less its padding.
 */
typedef struct UIWidgetOps {
    // The layout's, either NULL: its content's width once the width pass has given it one, which
    // its height may count on; and its content rect once every rect is placed, which the input
    // pass and the draw then read.
    void (*width)(const UISystem* ui, UIElement* el, const UIStyle* s, float width);
    void (*place)(const UISystem* ui, UIElement* el, UIRect content);
    // Around the keys it takes in one turn of a pass; NULL for none.
    void (*keys_begin)(UIElement* el);
    void (*keys_end)(UIElement* el);
    // One key or character while it holds the focus; false for a key it does not use, which is
    // the screen's.
    bool (*key)(const UISystem* ui, UIElement* el, UIRect content, const InputKeyEvent* ev);
    // A press on it at the point, with the modifiers held, and the pointer held after one.
    void (*press)(const UISystem* ui, UIElement* el, UIRect content, float x, float y, int mods,
                  double time);
    void (*drag)(const UISystem* ui, UIElement* el, UIRect content, float x, float y);
    // Draws it, in `s`; the seconds until it next changes of itself, INFINITY for never.
    double (*draw)(const UISystem* ui, UIElement* el, const UIStyle* s, UIRect content,
                   bool focused, double now, bool blink);
    // A scroll the bar and the wheel drive, or NULL: where it is, false while everything shows; put
    // `at` along; moved by `by` of its units.
    bool (*scroll)(const UIElement* el, UIScrollPlace* place);
    void (*scroll_to)(UIElement* el, float at);
    void (*scroll_by)(UIElement* el, float by);
} UIWidgetOps;

extern const UIWidgetOps ui_field_ops;
extern const UIWidgetOps ui_editor_ops;

#endif // _UI_INTERNAL_H_
