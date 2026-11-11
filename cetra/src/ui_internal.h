#ifndef _UI_INTERNAL_H_
#define _UI_INTERNAL_H_

/*
 * What the UI's own files share, private to them: the engine a system was made with, the scroll bar
 * every element that scrolls draws, and the table of calls through which ui.c lays out, routes
 * input to and draws an element that keeps state of its own -- the text field, the code editor and
 * the table -- so that ui.c names no such kind but where it picks the calls.
 */

#include "input.h"
#include "ui.h"

struct Engine;

// The engine a UI system was made with, for the clipboard and the screen's scale.
struct Engine* ui_system_engine(const UISystem* ui);
// The layouts a system has run so far: two elements a place saw under the same count were placed
// by one layout.
uint64_t ui_system_layouts(const UISystem* ui);
// The gap between an element's children: its own spacing, else the theme's, else 8.
float ui_element_spacing(const UISystem* ui, const UIElement* el);
// The points one unit of the wheel scrolls: the theme's step, else its font size.
float ui_scroll_step(const UISystem* ui);

// What scrolls in points through UIElement.scroll, a pane or a table: the least scroll from
// `target` that shows [y0, y1] whole through `view` points, its top if it is taller.
float ui_least_scroll(float target, float y0, float y1, float view);
// Its scroll held inside `content` points seen through `view`, following its end if it does; the
// scroll it is drawn at.
float ui_scroll_settle(UIElement* el, float content, float view);
// The part of the window the scrolling panes round `el` show, as the last layout placed them: the
// meet of what each one's clip lets through in its normal state; the root's rect with no pane round
// it.
UIRect ui_shown_rect(const UISystem* ui, const UIElement* el);
// Every pane round `el` scrolled, easing, by the least that shows the window's rows [y0, y1) whole,
// measured from where each pane is going: as a focus moved to is shown.
void ui_reveal_span(const UISystem* ui, const UIElement* el, float y0, float y1);
// What is drawn behind selected text or a list's cursor: the theme's selection, else the engine's.
void ui_selection_colour(const UITheme* theme, vec4 out);

// The bar: its width, its inset from the element's edge, its shortest thumb, and the strip along
// the element's right edge a press lands on it from.
#define UI_BAR_W     6.0f
#define UI_BAR_INSET 2.0f
#define UI_BAR_MIN   24.0f
#define UI_BAR_HIT   14.0f

// Where an element that scrolls is, in units of its own: how much shows, how much there is, how
// far along it the view is, 0..1, and how far a unit of the wheel moves it; and the strip its bar
// runs down, none wide for the element's whole rect.
typedef struct UIScrollPlace {
    float view, total, at, step;
    UIRect along;
} UIScrollPlace;

// A bar's track along `rect`'s right edge and its thumb: the thumb's length the share in view and
// its place the share scrolled. False while everything shows, or while a bar would not stand inside
// `rect`, its insets included.
bool ui_bar_place(UIRect rect, const UIScrollPlace* place, UIRect* track, UIRect* thumb);
// Where a thumb taken `grab` below its top and held at `y` puts the view, 0..1 along.
float ui_bar_at(UIRect track, UIRect thumb, float y, float grab);

/*
 * An element with state of its own, through which ui.c reaches it. The calls past the layout's take
 * the element's content rect, its rect less its padding.
 */
typedef struct UIWidgetOps {
    // The layout's, any NULL: what its content measures along `axis` (0 across), its padding
    // aside, which a FIT axis hugs; its content's width once the width pass has given it one,
    // which its height may count on; and its content rect once every rect is placed, which the
    // input pass and the draw then read.
    float (*content)(const UISystem* ui, UIElement* el, const UIStyle* s, int axis);
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
    // The span of the window, [*y0, *y1), the panes round it show as it takes the focus, set where
    // it names one; NULL, or left as given, for its whole rect.
    void (*reveal)(const UIElement* el, float* y0, float* y1);
} UIWidgetOps;

extern const UIWidgetOps ui_field_ops;
extern const UIWidgetOps ui_editor_ops;
extern const UIWidgetOps ui_table_ops;

// A table's own state, made empty and freed with its element.
struct UITable* ui_table_make(void);
void ui_table_free(struct UITable* t);

#endif // _UI_INTERNAL_H_
