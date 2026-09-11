#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "ext/log.h"
#include "ui.h"
#include "util.h"

// How fast hover and focus reach their target, in seconds. Short enough that a
// menu feels immediate, long enough that a shader reading uFocus has something
// to animate with.
#define UI_EASE_SECONDS 0.12f

// How many focusable elements one screen's navigation can see. A menu reaches
// nowhere near this; a screen that exceeds it says so rather than leaving the
// overflow drawn, clickable and unreachable by pad in silence.
#define UI_NAV_MAX 128

struct UIScreen {
    char* name;
    UIElement* root;
    bool modal;
    UISystem* ui;

    // How this screen arrives, and how far in it is. `t_in` eases 0 -> 1 while
    // the screen is on the stack and is reset when it is pushed, so a screen
    // pushed, popped and pushed again plays its entrance each time rather than
    // appearing finished.
    UITransition transition;
    float transition_seconds;
    float t_in;
    // Which element the keyboard or pad is on. Per screen, so pushing a screen
    // and popping it again returns to where the player was rather than to the
    // top of the list.
    UIElement* focused;
};

struct UISystem {
    Engine* engine; // borrowed
    UIDrawList* dl; // owned

    UITheme theme;
    Font* font;
    float font_size;

    UIScreen** screens; // owned, every screen ever created
    size_t screen_count, screen_cap;

    UIScreen** stack; // borrowed into `screens`
    size_t stack_count, stack_cap;

    // The element a pointer press captured, held until release. Release over
    // this same element activates it; release anywhere else cancels, which is
    // what makes press-slide-off-release do nothing.
    UIElement* pressed;

    // Last frame's pointer, so hover can move focus only when the pointer has
    // actually MOVED. Moving it every frame would mean a resting mouse
    // overrode the pad on every single frame, and the stick could never take
    // the highlight off whatever the cursor happened to be sitting on.
    //
    // Seeded off-screen so the first frame counts as movement without a second
    // field to say the first frame has not happened yet.
    float last_pointer_x, last_pointer_y;
};

// ------------------------------------------------------------------- theme

/*
 * The engine default, and the bottom of the resolution chain: an element's own
 * style, then the theme's entry for its kind and state, then this. Every step
 * fills only what the step above left at zero, so a partly-filled style is a
 * legal style rather than a mostly-black one.
 */
static UIStyle _default_style(UIKind kind, UIState state) {
    UIStyle s = {0};
    s.font_size = 16.0f;
    // The bottom of the inherit chain, so these are what a style that names
    // nothing resolves to: the face's own leading, no added tracking, and an
    // image drawn at its own brightness rather than multiplied by a fill.
    s.line_spacing = 1.0f;
    s.tracking = 0.0f;
    s.bg_tint[0] = s.bg_tint[1] = s.bg_tint[2] = s.bg_tint[3] = 1.0f;
    switch (kind) {
        case UI_ROOT:
            // A screen root is a container, not a surface: no fill, no padding, no
            // radius. Every field stays at the zero it was initialised with.
            break;
        case UI_LABEL:
            s.fg[0] = s.fg[1] = s.fg[2] = 0.92f;
            s.fg[3] = 1.0f;
            s.padding[0] = s.padding[2] = 4.0f;
            break;
        case UI_PANEL:
            s.bg[0] = 0.07f;
            s.bg[1] = 0.08f;
            s.bg[2] = 0.10f;
            s.bg[3] = 0.88f;
            s.corner_radius = 10.0f;
            s.padding[0] = s.padding[1] = s.padding[2] = s.padding[3] = 16.0f;
            break;
        default: // the four controls share a shape and differ by state
            s.fg[0] = s.fg[1] = s.fg[2] = 0.90f;
            s.fg[3] = 1.0f;
            s.bg[0] = 0.13f;
            s.bg[1] = 0.14f;
            s.bg[2] = 0.18f;
            s.bg[3] = 0.95f;
            s.corner_radius = 6.0f;
            s.padding[0] = s.padding[2] = 10.0f;
            s.padding[1] = s.padding[3] = 18.0f;
            if (state == UI_STATE_HOVER) {
                // Clearly apart from NORMAL. The first version moved the
                // channels by 0.05 and read as no feedback at all, which makes
                // a menu feel broken long before anyone calls it subtle.
                s.bg[0] = 0.26f;
                s.bg[1] = 0.29f;
                s.bg[2] = 0.38f;
                s.fg[0] = s.fg[1] = s.fg[2] = 1.0f;
            } else if (state == UI_STATE_FOCUS || state == UI_STATE_ACTIVE) {
                s.bg[0] = 0.16f;
                s.bg[1] = 0.34f;
                s.bg[2] = 0.62f;
                s.border[0] = 0.45f;
                s.border[1] = 0.70f;
                s.border[2] = 1.00f;
                s.border[3] = 1.0f;
                s.border_width = 1.5f;
                s.fg[0] = s.fg[1] = s.fg[2] = 1.0f;
            } else if (state == UI_STATE_DISABLED) {
                s.fg[0] = s.fg[1] = s.fg[2] = 0.45f;
                s.bg[3] = 0.55f;
            }
            break;
    }
    return s;
}

static bool _vec4_absent(const vec4 v) {
    return v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f;
}

// Copies any field of `from` that `into` left absent. The one place the
// zero-means-inherit rule is implemented, so there is no second place for it to
// mean something else.
//
// Every colour asks the same question through _vec4_absent. `border` used to
// test its ALPHA alone, which quietly destroyed authored colour: a style saying
// "gold border, currently switched off" -- {0.8, 0.6, 0.2, 0} -- had its RGB
// overwritten wholesale and its alpha raised to the level below's, turning on a
// border the author had turned off, in a colour they never chose. `bg` in the
// same state was preserved, so one rule contradicted the other.
static void _inherit(UIStyle* into, const UIStyle* from) {
    if (_vec4_absent(into->bg))
        glm_vec4_copy((float*)from->bg, into->bg);
    if (_vec4_absent(into->fg))
        glm_vec4_copy((float*)from->fg, into->fg);
    if (_vec4_absent(into->border))
        glm_vec4_copy((float*)from->border, into->border);
    if (into->border_width == 0.0f)
        into->border_width = from->border_width;
    if (into->corner_radius == 0.0f)
        into->corner_radius = from->corner_radius;
    for (int i = 0; i < 4; i++)
        if (into->padding[i] == 0.0f)
            into->padding[i] = from->padding[i];
    if (into->font_size == 0.0f)
        into->font_size = from->font_size;
    if (!into->font)
        into->font = from->font;
    if (into->tracking == 0.0f)
        into->tracking = from->tracking;
    if (into->line_spacing == 0.0f)
        into->line_spacing = from->line_spacing;
    if (_vec4_absent(into->bg_tint))
        glm_vec4_copy((float*)from->bg_tint, into->bg_tint);
    if (!into->bg_tex) {
        into->bg_tex = from->bg_tex;
        memcpy(into->bg_slice, from->bg_slice, sizeof(float) * 4);
    }
}

static const UIStyle* _theme_entry(const UITheme* t, UIKind kind, UIState state) {
    static const UIStyle none = {0};
    switch (kind) {
        case UI_ROOT:
            return &none; // nothing to theme; see the UIKind comment
        case UI_PANEL:
            return &t->panel;
        case UI_LABEL:
            return &t->label;
        case UI_BUTTON:
            return &t->button[state];
        case UI_TOGGLE:
            return &t->toggle[state];
        case UI_SLIDER:
            return &t->slider[state];
        case UI_SELECTOR:
            return &t->selector[state];
        default:
            return &t->panel;
    }
}

/*
 * Four levels, one rule: the element's own style, then the theme's entry for
 * its kind and state, then the screen's globals, then the engine default. Each
 * fills only what the level above left absent.
 *
 * The globals used to be three special cases bolted on after the chain -- a
 * font fallback, a font_size re-test that called _theme_entry a SECOND time to
 * work out whether anything above had set it, and spacing's own function with
 * its own hard-coded default. Being outside the chain is what made them wrong:
 * the font_size patch read the THEME's size and never the system's, so
 * ui_set_font's size reached the draw list and no element at all, and every
 * menu silently rendered at the default 16 no matter what the app asked for.
 * Measured consistently by the layout, so nothing ever contradicted it.
 *
 * As a level they cost nothing and UITheme.font -- which no code read -- starts
 * working.
 */
static UIStyle _resolve(const UISystem* ui, const UIElement* el, UIState state) {
    UIStyle s = el->style ? *el->style : (UIStyle){0};
    _inherit(&s, _theme_entry(&ui->theme, el->kind, state));

    const UIStyle globals = {.font = ui->theme.font ? ui->theme.font : ui->font,
                             .font_size =
                                 ui->theme.font_size > 0.0f ? ui->theme.font_size : ui->font_size};
    _inherit(&s, &globals);

    const UIStyle def = _default_style(el->kind, state);
    _inherit(&s, &def);

    /*
     * GEOMETRY is state-independent, and that is enforced here rather than
     * promised in a comment somewhere.
     *
     * Layout resolves at UI_STATE_NORMAL and the drawing resolves at the LIVE
     * state, so any field a per-state theme entry carries that layout also
     * reads would size the box at one number and paint it at another. A theme
     * setting button[UI_STATE_FOCUS].padding got a label drifting out of a
     * control that had measured correctly -- and nothing would have failed,
     * because both halves are individually doing what they were told.
     *
     * The paint fields stay per state, which is the whole point of the array.
     * These do not.
     */
    if (state != UI_STATE_NORMAL) {
        const UIStyle g = _resolve(ui, el, UI_STATE_NORMAL);
        memcpy(s.padding, g.padding, sizeof(s.padding));
        s.font = g.font;
        s.font_size = g.font_size;
        s.tracking = g.tracking;
        s.line_spacing = g.line_spacing;
    }
    return s;
}

// ------------------------------------------------------------------ elements

static UIElement* _new_element(UIElement* parent, UIKind kind) {
    UIElement* el = calloc(1, sizeof(UIElement));
    if (!el) {
        log_error("ui: out of memory creating an element");
        return NULL;
    }
    el->kind = kind;
    el->dir = UI_COLUMN;
    el->size_mode[0] = UI_FIT;
    el->size_mode[1] = UI_FIT;
    el->align_main = UI_ALIGN_START;
    el->align_cross = UI_ALIGN_START;
    el->focusable =
        (kind == UI_BUTTON || kind == UI_TOGGLE || kind == UI_SLIDER || kind == UI_SELECTOR);
    el->parent = parent;

    if (parent) {
        if (!grow_array((void**)&parent->children, &parent->child_capacity, parent->child_count + 1,
                        sizeof(UIElement*), 8)) {
            free(el);
            return NULL;
        }
        parent->children[parent->child_count++] = el;
    }
    return el;
}

static void _free_element(UIElement* el) {
    if (!el)
        return;
    for (size_t i = 0; i < el->child_count; i++)
        _free_element(el->children[i]);
    free(el->children);
    free(el->text);
    free(el->style);
    free(el);
}

UIElement* ui_panel(UIElement* parent) {
    return _new_element(parent, UI_PANEL);
}

UIElement* ui_label(UIElement* parent, const char* text) {
    UIElement* el = _new_element(parent, UI_LABEL);
    ui_set_text(el, text);
    return el;
}

UIElement* ui_button(UIElement* parent, const char* text, UIActionFn action, void* user) {
    UIElement* el = _new_element(parent, UI_BUTTON);
    ui_set_text(el, text);
    if (el) {
        el->action = action;
        el->action_user = user;
    }
    return el;
}

UIElement* ui_toggle(UIElement* parent, const char* text, bool* bound, UIActionFn action,
                     void* user) {
    UIElement* el = _new_element(parent, UI_TOGGLE);
    ui_set_text(el, text);
    if (el) {
        el->bound_bool = bound;
        el->action = action;
        el->action_user = user;
    }
    return el;
}

UIElement* ui_slider(UIElement* parent, const char* text, float lo, float hi, float* bound,
                     UIActionFn action, void* user) {
    UIElement* el = _new_element(parent, UI_SLIDER);
    ui_set_text(el, text);
    if (el) {
        el->bound_float = bound;
        el->range_lo = lo;
        el->range_hi = hi;
        el->action = action;
        el->action_user = user;
        // A slider with no width is a slider you cannot aim at, so this is the
        // one kind whose main axis defaults away from FIT.
        el->size_mode[0] = UI_FIXED;
        el->size[0] = 220.0f;
    }
    return el;
}

UIElement* ui_selector(UIElement* parent, const char* text, const char* const* options, int count,
                       int* bound, UIActionFn action, void* user) {
    UIElement* el = _new_element(parent, UI_SELECTOR);
    ui_set_text(el, text);
    if (el) {
        el->options = options;
        el->option_count = count;
        el->bound_int = bound;
        el->action = action;
        el->action_user = user;
        el->size_mode[0] = UI_FIXED;
        el->size[0] = 220.0f;
    }
    return el;
}

void ui_set_text(UIElement* el, const char* text) {
    if (!el)
        return;
    free(el->text);
    el->text = text ? safe_strdup(text) : NULL;
}

void ui_set_style(UIElement* el, const UIStyle* style) {
    if (!el)
        return;
    free(el->style);
    el->style = NULL;
    if (!style)
        return;
    el->style = malloc(sizeof(UIStyle));
    if (el->style)
        *el->style = *style;
}

void ui_set_draw(UIElement* el, UIDrawFn draw, void* user) {
    if (!el)
        return;
    el->draw = draw;
    el->draw_user = user;
}

void ui_set_element_program(UIElement* el, ShaderProgram* program) {
    if (el)
        el->program = program;
}

void ui_set_size(UIElement* el, UISize x_mode, float x, UISize y_mode, float y) {
    if (!el)
        return;
    el->size_mode[0] = x_mode;
    el->size_mode[1] = y_mode;
    el->size[0] = x;
    el->size[1] = y;
}

// -------------------------------------------------------------------- layout

// The furniture a control draws BESIDE its label: the toggle's knob, the
// selector's value, the slider's track. Measured here and nowhere else, because
// a control sized from its text alone draws its furniture on top of that text --
// which is exactly what the first build of this did.
#define UI_KNOB_W        38.0f
#define UI_KNOB_H        20.0f
#define UI_TRACK_H       4.0f
#define UI_FURNITURE_GAP 14.0f

static float _widest_option(const UIElement* el, const UIStyle* s) {
    if (!el->options || el->option_count <= 0 || !s->font)
        return 0.0f;
    float widest = 0.0f;
    for (int i = 0; i < el->option_count; i++) {
        if (!el->options[i])
            continue;
        const float w = ui_text_width(s->font, s->font_size, s->tracking, el->options[i]);
        widest = w > widest ? w : widest;
    }
    return widest;
}

/*
 * The element's own padding where it names one, else the resolved style's.
 *
 * ONE rule, taking the style the caller already has: the box layout MEASURES
 * and the box a control DRAWS INTO have to be the same box, and this was
 * written out twice -- once here and once inline in _intrinsic -- which is two
 * places for it to stop being.
 */
static void _padding_from(const UIElement* el, const UIStyle* s, float out[4]) {
    if (el->padding[0] || el->padding[1] || el->padding[2] || el->padding[3]) {
        memcpy(out, el->padding, sizeof(float) * 4);
        return;
    }
    memcpy(out, s->padding, sizeof(float) * 4);
}

// What one element's own content wants, before any parent has a say.
static void _intrinsic(const UISystem* ui, const UIElement* el, float* out_w, float* out_h) {
    const UIStyle s = _resolve(ui, el, UI_STATE_NORMAL);
    float w = 0.0f, h = 0.0f;
    if (el->text && el->text[0] && s.font) {
        w = ui_text_width(s.font, s.font_size, s.tracking, el->text);
        // One line height PER LINE. ui_draw_text starts a new line at every
        // '\n' it is given, so charging one made an authored two-line label
        // measure half its height and draw outside its own rect and its
        // parent's -- with the layout numbers all looking correct.
        int lines = 1;
        for (const char* p = el->text; *p; p++)
            if (*p == '\n')
                lines++;
        h = ui_line_height(s.font, s.font_size, s.line_spacing) * (float)lines;
    }

    switch (el->kind) {
        case UI_TOGGLE:
            w += UI_FURNITURE_GAP + UI_KNOB_W;
            h = h > UI_KNOB_H ? h : UI_KNOB_H;
            break;
        case UI_SELECTOR:
            w += UI_FURNITURE_GAP + _widest_option(el, &s);
            break;
        case UI_SLIDER:
            // The track sits on its own row under the label, so the height grows
            // rather than the width.
            h += UI_FURNITURE_GAP + UI_TRACK_H;
            break;
        default:
            break;
    }

    float pad[4];
    _padding_from(el, &s, pad);
    *out_w = w + pad[1] + pad[3];
    *out_h = h + pad[0] + pad[2];
}

static float _spacing_of(const UISystem* ui, const UIElement* el) {
    if (el->spacing > 0.0f)
        return el->spacing;
    return ui->theme.spacing > 0.0f ? ui->theme.spacing : 8.0f;
}

// The same rule for a caller that has no style in hand and must resolve one.
static void _padding_of(const UISystem* ui, const UIElement* el, float out[4]) {
    const UIStyle s = _resolve(ui, el, UI_STATE_NORMAL);
    _padding_from(el, &s, out);
}

/*
 * Pass 1, bottom-up: every element gets the size its own content wants. A GROW
 * axis is measured too, at its FIT size -- that is the MINIMUM it will accept,
 * and it is what a FIT parent measures against, which is what stops the two
 * modes being mutually recursive.
 */
static void _measure(const UISystem* ui, UIElement* el) {
    for (size_t i = 0; i < el->child_count; i++)
        _measure(ui, el->children[i]);

    float w = 0.0f, h = 0.0f;
    _intrinsic(ui, el, &w, &h);

    if (el->child_count > 0) {
        float pad[4];
        _padding_of(ui, el, pad);
        const float gap = _spacing_of(ui, el);
        float main = 0.0f, cross = 0.0f;
        size_t counted = 0;
        for (size_t i = 0; i < el->child_count; i++) {
            const UIElement* c = el->children[i];
            if (c->fill)
                continue; // a backdrop is outside the flow
            const float cw = c->rect.w, ch = c->rect.h;
            if (el->dir == UI_ROW) {
                main += cw;
                cross = ch > cross ? ch : cross;
            } else {
                main += ch;
                cross = cw > cross ? cw : cross;
            }
            counted++;
        }
        if (counted > 1)
            main += gap * (float)(counted - 1);

        const float kids_w = (el->dir == UI_ROW ? main : cross) + pad[1] + pad[3];
        const float kids_h = (el->dir == UI_ROW ? cross : main) + pad[0] + pad[2];
        w = kids_w > w ? kids_w : w;
        h = kids_h > h ? kids_h : h;
    }

    el->rect.w = (el->size_mode[0] == UI_FIXED) ? el->size[0] : w;
    el->rect.h = (el->size_mode[1] == UI_FIXED) ? el->size[1] : h;
}

// Pass 2, top-down: `el` already has its final rect; place the children inside
// it, expanding GROW axes into whatever is left over.
static void _arrange(const UISystem* ui, UIElement* el, UIRect screen) {
    float pad[4];
    _padding_of(ui, el, pad);
    const UIRect inner = ui_rect_inset(el->rect, pad[0], pad[1], pad[2], pad[3]);
    const float gap = _spacing_of(ui, el);

    // A fill element ignores the flow entirely and covers the screen. It is
    // still laid out afterwards, so a backdrop can have children of its own.
    for (size_t i = 0; i < el->child_count; i++) {
        UIElement* c = el->children[i];
        if (c->fill) {
            c->rect = screen;
            _arrange(ui, c, screen);
        }
    }

    float used = 0.0f;
    size_t counted = 0, grow_count = 0;
    for (size_t i = 0; i < el->child_count; i++) {
        const UIElement* c = el->children[i];
        if (c->fill)
            continue;
        used += (el->dir == UI_ROW) ? c->rect.w : c->rect.h;
        if (c->size_mode[el->dir == UI_ROW ? 0 : 1] == UI_GROW)
            grow_count++;
        counted++;
    }
    if (counted > 1)
        used += gap * (float)(counted - 1);

    const float avail = (el->dir == UI_ROW) ? inner.w : inner.h;
    const float leftover = avail - used;
    const float share = (grow_count > 0 && leftover > 0.0f) ? leftover / (float)grow_count : 0.0f;

    // With nothing growing, the whole run is placed by the main-axis alignment.
    float cursor = (el->dir == UI_ROW) ? inner.x : inner.y;
    if (grow_count == 0 && leftover > 0.0f) {
        if (el->align_main == UI_ALIGN_CENTER)
            cursor += leftover * 0.5f;
        else if (el->align_main == UI_ALIGN_END)
            cursor += leftover;
    }

    for (size_t i = 0; i < el->child_count; i++) {
        UIElement* c = el->children[i];
        if (c->fill)
            continue;

        if (el->dir == UI_ROW) {
            if (c->size_mode[0] == UI_GROW)
                c->rect.w += share;
            if (c->size_mode[1] == UI_GROW)
                c->rect.h = inner.h;
            c->rect.x = cursor;
            c->rect.y = inner.y;
            if (c->rect.h < inner.h) {
                if (el->align_cross == UI_ALIGN_CENTER)
                    c->rect.y += (inner.h - c->rect.h) * 0.5f;
                else if (el->align_cross == UI_ALIGN_END)
                    c->rect.y += inner.h - c->rect.h;
            }
            cursor += c->rect.w + gap;
        } else {
            if (c->size_mode[1] == UI_GROW)
                c->rect.h += share;
            if (c->size_mode[0] == UI_GROW)
                c->rect.w = inner.w;
            c->rect.y = cursor;
            c->rect.x = inner.x;
            if (c->rect.w < inner.w) {
                if (el->align_cross == UI_ALIGN_CENTER)
                    c->rect.x += (inner.w - c->rect.w) * 0.5f;
                else if (el->align_cross == UI_ALIGN_END)
                    c->rect.x += inner.w - c->rect.w;
            }
            cursor += c->rect.h + gap;
        }
        _arrange(ui, c, screen);
    }
}

void ui_layout(UIScreen* screen, float width, float height) {
    if (!screen || !screen->root || !screen->ui)
        return;
    const UIRect full = {0.0f, 0.0f, width, height};
    _measure(screen->ui, screen->root);
    screen->root->rect = full;
    _arrange(screen->ui, screen->root, full);
}

// --------------------------------------------------------------------- emit

// A control's label sits on the vertical centre of its row, not at the top of
// its padding box: ui_draw_text places a baseline an ascent below the rect's
// top, which is what a LABEL wants and what makes a button's text ride high.
static UIRect _centred_row(UIRect box, const UIStyle* s) {
    const float lh = ui_line_height(s->font, s->font_size, s->line_spacing);
    UIRect r = {box.x + s->padding[3], box.y + (box.h - lh) * 0.5f,
                box.w - s->padding[1] - s->padding[3], lh};
    return r;
}

// The input pass writes `state`; the easing follows it. Deriving the state back
// out of the eased weights instead would be circular -- the weights chase the
// state -- and a focus ring would take a frame to notice it had moved.
static UIState _state_of(const UIElement* el) {
    return el->disabled ? UI_STATE_DISABLED : el->state;
}

// Where a slider's track is. ONE definition, read by the draw and by the hit
// test alike: two copies of this rectangle would drift, and the symptom would
// be a bar that fills to a different place than the one you clicked.
static UIRect _slider_track(const UISystem* ui, const UIElement* el) {
    const UIStyle s = _resolve(ui, el, _state_of(el));
    const float lh = ui_line_height(s.font, s.font_size, s.line_spacing);
    return (UIRect){el->rect.x + s.padding[3], el->rect.y + s.padding[0] + lh + UI_FURNITURE_GAP,
                    el->rect.w - s.padding[1] - s.padding[3], UI_TRACK_H};
}

static void _emit(UISystem* ui, UIElement* el) {
    UIDrawList* dl = ui->dl;
    const UIState state = _state_of(el);
    const UIStyle s = _resolve(ui, el, state);

    if (el->program)
        ui_set_draw_program(dl, el->program, el->rect, el->t_focus);

    if (el->draw) {
        // The app paints, and keeps everything else: this element was laid out,
        // can hold focus, and takes input exactly like a built-in one.
        el->draw(el, dl, el->draw_user);
    } else {
        switch (el->kind) {
            case UI_ROOT:
                break; // lays its children out and draws nothing itself
            case UI_LABEL:
                ui_draw_text(dl, el->rect, el->text, &s, el->align_cross);
                break;
            case UI_PANEL:
                ui_draw_rect(dl, el->rect, &s);
                break;
            case UI_TOGGLE: {
                ui_draw_rect(dl, el->rect, &s);
                // The label stops where the knob begins -- the same reservation
                // _intrinsic measured, so the two cannot disagree about the gap.
                UIRect inner = _centred_row(el->rect, &s);
                inner.w -= UI_KNOB_W + UI_FURNITURE_GAP;
                ui_draw_text(dl, inner, el->text, &s, UI_ALIGN_START);
                // Everything below is driven by t_value, not by the bool: the
                // value flips at once and the PICTURE crosses over, which is
                // what makes a switch feel thrown rather than teleported.
                const float t = el->t_value;

                const UIRect bed_rect = {el->rect.x + el->rect.w - s.padding[1] - UI_KNOB_W,
                                         el->rect.y + (el->rect.h - UI_KNOB_H) * 0.5f, UI_KNOB_W,
                                         UI_KNOB_H};
                const vec4 bed_on = {0.26f, 0.70f, 0.40f, 1.0f};
                const vec4 bed_off = {0.24f, 0.25f, 0.30f, 1.0f};
                vec4 bed_colour;
                glm_vec4_lerp((float*)bed_off, (float*)bed_on, t, bed_colour);
                ui_draw_rounded(dl, bed_rect, UI_KNOB_H * 0.5f, bed_colour, NULL, 0.0f);

                // The knob travels between the two ends rather than appearing
                // at one of them.
                const float inset = 2.0f;
                const float dia = UI_KNOB_H - inset * 2.0f;
                const float x_off = bed_rect.x + inset;
                const float x_on = bed_rect.x + bed_rect.w - inset - dia;
                const UIRect knob_rect = {glm_lerp(x_off, x_on, t), bed_rect.y + inset, dia, dia};
                vec4 cap_bg = {0.97f, 0.98f, 1.00f, 1.0f};
                ui_draw_rounded(dl, knob_rect, dia * 0.5f, cap_bg, NULL, 0.0f);
                break;
            }
            case UI_SLIDER: {
                ui_draw_rect(dl, el->rect, &s);
                // The label takes the text row; the track takes the row below it,
                // which is the height _intrinsic reserved.
                const UIRect label = {el->rect.x + s.padding[3], el->rect.y + s.padding[0],
                                      el->rect.w - s.padding[1] - s.padding[3],
                                      ui_line_height(s.font, s.font_size, s.line_spacing)};
                ui_draw_text(dl, label, el->text, &s, UI_ALIGN_START);

                const float span = el->range_hi - el->range_lo;
                float t = 0.0f;
                if (el->bound_float && span != 0.0f)
                    t = (*el->bound_float - el->range_lo) / span;
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);

                const UIRect track = _slider_track(ui, el);
                vec4 bar_bg = {0.22f, 0.23f, 0.28f, 1.0f};
                ui_draw_rounded(dl, track, UI_TRACK_H * 0.5f, bar_bg, NULL, 0.0f);

                UIRect filled = track;
                filled.w *= t;
                vec4 lit_bg = {0.45f, 0.70f, 1.00f, 1.0f};
                ui_draw_rounded(dl, filled, UI_TRACK_H * 0.5f, lit_bg, NULL, 0.0f);
                break;
            }
            case UI_SELECTOR: {
                ui_draw_rect(dl, el->rect, &s);
                const UIRect inner = _centred_row(el->rect, &s);
                ui_draw_text(dl, inner, el->text, &s, UI_ALIGN_START);
                const int idx = el->bound_int ? *el->bound_int : 0;
                if (el->options && idx >= 0 && idx < el->option_count)
                    ui_draw_text(dl, inner, el->options[idx], &s, UI_ALIGN_END);
                break;
            }
            case UI_BUTTON:
            default:
                ui_draw_rect(dl, el->rect, &s);
                ui_draw_text(dl, _centred_row(el->rect, &s), el->text, &s, UI_ALIGN_CENTER);
                break;
        }
    }

    if (el->program)
        ui_set_draw_program(dl, NULL, el->rect, 0.0f);

    for (size_t i = 0; i < el->child_count; i++)
        _emit(ui, el->children[i]);
}

static void _ease(UIElement* el, float dt) {
    const float step = (UI_EASE_SECONDS > 0.0f) ? dt / UI_EASE_SECONDS : 1.0f;
    const float hover_target =
        (el->state == UI_STATE_HOVER || el->state == UI_STATE_ACTIVE) ? 1.0f : 0.0f;
    const float focus_target =
        (el->state == UI_STATE_FOCUS || el->state == UI_STATE_ACTIVE) ? 1.0f : 0.0f;
    const float value_target =
        (el->kind == UI_TOGGLE && el->bound_bool && *el->bound_bool) ? 1.0f : 0.0f;
    const float k = step < 1.0f ? step : 1.0f;
    el->t_hover += (hover_target - el->t_hover) * k;
    el->t_focus += (focus_target - el->t_focus) * k;
    el->t_value += (value_target - el->t_value) * k;
    for (size_t i = 0; i < el->child_count; i++)
        _ease(el->children[i], dt);
}

// --------------------------------------------------------------------- input

/*
 * The deepest, topmost element claiming the point, or NULL.
 *
 * REVERSE child order: a child draws after its parent and a later sibling over
 * an earlier one, so the last match is the one actually on top and the one the
 * player believes they clicked. This is UIKit's hitTest walking subviews
 * backwards, for exactly the same reason.
 *
 * Only a focusable element claims. A panel or a label is transparent to the
 * pointer by construction -- the equivalent of pointer-events: none, defaulted
 * the safe way round, so decoration never swallows a click meant for the
 * control underneath it.
 */
static UIElement* _hit(UIElement* el, float x, float y) {
    if (!el || el->disabled)
        return NULL;
    for (size_t i = el->child_count; i-- > 0;) {
        UIElement* got = _hit(el->children[i], x, y);
        if (got)
            return got;
    }
    return (el->focusable && ui_rect_hit(el->rect, x, y)) ? el : NULL;
}

static void _gather(UIElement* el, UIElement** out, size_t* n, size_t cap) {
    if (!el || *n >= cap)
        return;
    if (el->focusable && !el->disabled)
        out[(*n)++] = el;
    for (size_t i = 0; i < el->child_count; i++)
        _gather(el->children[i], out, n, cap);
}

static void _centre(const UIElement* el, float* cx, float* cy) {
    *cx = el->rect.x + el->rect.w * 0.5f;
    *cy = el->rect.y + el->rect.h * 0.5f;
}

/*
 * Focus movement is GEOMETRIC, not tree order: the nearest focusable element
 * whose centre lies in the direction asked for, wrapping to the far side when
 * there is none. Tree order would walk a two-column settings screen in the
 * order the columns happened to be built, which is not the order a player sees.
 */
static UIElement* _nav(UIElement* root, const UIElement* from, int dx, int dy) {
    if (dx == 0 && dy == 0)
        return NULL; // opposed keys in one frame: no direction was asked for

    UIElement* items[UI_NAV_MAX];
    size_t n = 0;
    _gather(root, items, &n, UI_NAV_MAX);
    if (n == 0)
        return NULL;
    if (n == UI_NAV_MAX)
        log_error("ui: more than %d focusable elements; the rest cannot be reached by pad or key",
                  UI_NAV_MAX);
    if (!from)
        return items[0];

    float fx, fy;
    _centre(from, &fx, &fy);

    UIElement* best = NULL;
    float best_score = 0.0f;
    UIElement* wrap = NULL;
    float wrap_score = 0.0f;

    /*
     * A real CONE, not a half-plane. `along > 0` alone put every element that
     * was not strictly ahead into the wrap bucket -- and in a single column
     * every centre shares an x exactly, so pressing LEFT made `along` zero for
     * all of them, nothing was ahead, and focus jumped to the furthest row in
     * the menu. Requiring the sideways drift to stay inside the forward
     * distance means an axis with nothing genuinely along it finds no candidate
     * and correctly does nothing.
     */
    const float cone = 1.0f; // across must not exceed along
    for (size_t i = 0; i < n; i++) {
        if (items[i] == from)
            continue;
        float cx, cy;
        _centre(items[i], &cx, &cy);
        const float along = (float)dx * (cx - fx) + (float)dy * (cy - fy);
        const float across = (float)dy * (cx - fx) + (float)dx * (cy - fy);
        const float across_abs = fabsf(across);

        if (along > 0.0f) {
            if (across_abs > along * cone)
                continue; // outside the cone: a neighbour, not a successor
            const float score = along + across_abs * 2.0f;
            if (!best || score < best_score) {
                best = items[i];
                best_score = score;
            }
        } else if (along < 0.0f) {
            // Wrapping lands on the furthest element that is still in line --
            // the same cone test, so wrap cannot reach across columns either.
            if (across_abs > -along * cone)
                continue;
            const float score = -along + across_abs * 2.0f;
            if (!wrap || score > wrap_score) {
                wrap = items[i];
                wrap_score = score;
            }
        }
    }
    return best ? best : wrap;
}

// Left/right on a control that has a value adjusts it rather than moving focus.
// Returns true when it consumed the press.
static bool _adjust(UIElement* el, int dir) {
    if (!el || el->disabled)
        return false;
    if (el->kind == UI_SLIDER && el->bound_float) {
        const float span = el->range_hi - el->range_lo;
        float v = *el->bound_float + (float)dir * span * 0.05f;
        v = v < el->range_lo ? el->range_lo : (v > el->range_hi ? el->range_hi : v);
        *el->bound_float = v;
        if (el->action)
            el->action(el, el->action_user);
        return true;
    }
    if (el->kind == UI_SELECTOR && el->bound_int && el->option_count > 0) {
        int i = *el->bound_int + dir;
        while (i < 0)
            i += el->option_count;
        *el->bound_int = i % el->option_count;
        if (el->action)
            el->action(el, el->action_user);
        return true;
    }
    return false;
}

// The value a control writes is written BEFORE its callback runs, so a handler
// reads the new state rather than being told what it is about to become.
static void _activate(UIElement* el) {
    if (!el || el->disabled)
        return;
    if (el->kind == UI_TOGGLE && el->bound_bool)
        *el->bound_bool = !*el->bound_bool;
    else if (el->kind == UI_SELECTOR && el->bound_int && el->option_count > 0)
        *el->bound_int = (*el->bound_int + 1) % el->option_count;
    if (el->action)
        el->action(el, el->action_user);
}

static void _clear_states(UIElement* el) {
    if (!el)
        return;
    if (el->state != UI_STATE_DISABLED)
        el->state = UI_STATE_NORMAL;
    for (size_t i = 0; i < el->child_count; i++)
        _clear_states(el->children[i]);
}

void ui_update(UISystem* ui, const UIInput* in, float width, float height) {
    if (!ui)
        return;
    UIScreen* top = ui_top(ui);
    if (!top || !top->root) {
        ui->pressed = NULL;
        return;
    }

    // This frame's rectangles, before anything is tested against them.
    ui_layout(top, width, height);

    // Clear the WHOLE stack, assign only on the top. Clearing the top alone
    // left a screen that had been pushed over holding whatever hover or focus
    // it carried at that moment -- and ui_build draws every screen in the
    // stack, so the second screen a game pushes would light a focus ring on a
    // screen that no longer takes input, with _ease keeping it alive.
    for (size_t i = 0; i < ui->stack_count; i++)
        if (ui->stack[i] && ui->stack[i]->root)
            _clear_states(ui->stack[i]->root);

    if (!in)
        return;

    UIElement* hovered = _hit(top->root, in->pointer_x, in->pointer_y);

    // Moving the pointer over an item highlights it, which is what a menu does
    // everywhere -- and it keeps pointer and pad on ONE highlight instead of a
    // hover colour and a focus ring disagreeing about where the player is.
    // Only on actual movement, so a resting cursor does not out-vote the stick.
    const bool moved = in->pointer_x != ui->last_pointer_x || in->pointer_y != ui->last_pointer_y;
    ui->last_pointer_x = in->pointer_x;
    ui->last_pointer_y = in->pointer_y;
    if (moved && hovered)
        top->focused = hovered;

    // Press captures. Release over the captured element activates it; release
    // anywhere else cancels. That is what makes press-slide-off-release not
    // fire, which is the behaviour every pointer UI has and every user expects.
    if (in->pointer_pressed) {
        ui->pressed = hovered;
        if (hovered)
            top->focused = hovered;
    }
    // A held slider tracks the pointer: pressing anywhere on the row jumps the
    // value there and holding drags it, which is what every slider does. It
    // runs off the CAPTURED element rather than the hovered one, so the drag
    // survives the pointer leaving the track -- grabbing a knob and pulling
    // past the end should peg the value, not drop the grab.
    if (ui->pressed && ui->pressed->kind == UI_SLIDER && in->pointer_down &&
        ui->pressed->bound_float) {
        UIElement* sl = ui->pressed;
        const UIRect track = _slider_track(ui, sl);
        float t = track.w > 0.0f ? (in->pointer_x - track.x) / track.w : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const float v = sl->range_lo + t * (sl->range_hi - sl->range_lo);
        if (v != *sl->bound_float) {
            *sl->bound_float = v;
            if (sl->action)
                sl->action(sl, sl->action_user);
        }
    }

    if (in->pointer_released) {
        // A slider was already set by the drag above; activating it again on
        // release would fire its callback a second time for one gesture.
        if (ui->pressed && ui->pressed == hovered && ui->pressed->kind != UI_SLIDER)
            _activate(ui->pressed);
        ui->pressed = NULL;
    }

    if (in->nav_up || in->nav_down || in->nav_left || in->nav_right) {
        const int dx = (in->nav_right ? 1 : 0) - (in->nav_left ? 1 : 0);
        const int dy = (in->nav_down ? 1 : 0) - (in->nav_up ? 1 : 0);
        // Sideways on a value control adjusts it; only an unconsumed press
        // moves focus.
        if (!(dx != 0 && _adjust(top->focused, dx))) {
            UIElement* next = _nav(top->root, top->focused, dx, dy);
            if (next)
                top->focused = next;
        }
    }

    if (in->accept && top->focused)
        _activate(top->focused);
    if (in->back)
        ui_pop(ui);

    // States for this frame's draw. Focus first, then hover over it, then the
    // held element, so a pressed control looks pressed rather than merely
    // hovered.
    if (top->focused && top->focused->state != UI_STATE_DISABLED)
        top->focused->state = UI_STATE_FOCUS;
    if (hovered && hovered->state != UI_STATE_DISABLED && hovered != top->focused)
        hovered->state = UI_STATE_HOVER;
    if (ui->pressed && in->pointer_down && ui->pressed->state != UI_STATE_DISABLED)
        ui->pressed->state = UI_STATE_ACTIVE;
}

// How far this screen's entrance has played, 0..1. A screen with no transition
// is always 1, so every consumer reads one number and no caller branches.
static float _screen_t(const UIScreen* s) {
    if (!s || s->transition == UI_TRANSITION_NONE || s->transition_seconds <= 0.0f)
        return 1.0f;
    return s->t_in;
}

void ui_build(UISystem* ui, float width, float height, float dt) {
    if (!ui || !ui->dl || width <= 0.0f || height <= 0.0f)
        return;
    ui_draw_list_begin(ui->dl, (int)width, (int)height);
    // Bottom to top: a pause screen over a HUD draws both, and the one on top
    // is simply the one emitted last.
    for (size_t i = 0; i < ui->stack_count; i++) {
        UIScreen* s = ui->stack[i];
        if (!s || !s->root)
            continue;
        if (s->transition_seconds > 0.0f && s->t_in < 1.0f) {
            s->t_in += dt / s->transition_seconds;
            if (s->t_in > 1.0f)
                s->t_in = 1.0f;
        }
        _ease(s->root, dt);
        ui_layout(s, width, height);
        // Locals, because the subtree reads the entrance through the DRAW
        // LIST's offset and alpha -- which is what _emit actually consults.
        // Nothing walks back up to the system for them.
        const float screen_t = _screen_t(s);
        const float slide =
            (s->transition == UI_TRANSITION_SLIDE) ? (1.0f - screen_t) * height * 0.04f : 0.0f;
        ui_draw_list_set_offset(ui->dl, 0.0f, slide);
        ui_draw_list_set_alpha(ui->dl, screen_t);
        _emit(ui, s->root);
    }
    // Cleared, not left at the last screen's value: the draw list outlives this
    // loop and anything an app emits through the primitives afterwards would
    // otherwise inherit a transition it has nothing to do with.
    ui_draw_list_set_offset(ui->dl, 0.0f, 0.0f);
    ui_draw_list_set_alpha(ui->dl, 1.0f);
}

// ---------------------------------------------------------------- the system

static void _ui_overlay(Engine* engine, void* user) {
    UISystem* ui = user;
    if (!ui || !engine)
        return;
    /*
     * The frame's dt for everything the UI animates -- hover, focus, a toggle's
     * travel, a screen's entrance.
     *
     * Headless takes the FIXED step, which is what engine.c already does for
     * the frame it hands the update hook. Two reasons, and the second is the
     * one that bites. A headless run's early frames are seconds apart in wall
     * time because a scene is loading, so a transition measured against the
     * wall clock is over before the first frame anyone captures. And a picture
     * whose contents depend on how long a load took cannot be a golden: it
     * would differ between two runs of the same build.
     *
     * Windowed takes the wall clock, and deliberately NOT the sim clock: a menu
     * pauses the game, and a menu that stopped animating the moment it appeared
     * would be animating for nobody.
     */
    /*
     * NOT engine->render_delta, and this is the trap worth the paragraph.
     *
     * That field looks like exactly the right answer -- it is the rule every
     * other per-frame consumer uses, and under engine_run it IS the fixed step
     * headless. But the game framework SUBSTITUTES a frame clock, so under a
     * Game it carries the SIM clock's delta, which is 0 while the sim is
     * paused. A menu is the one thing that must keep animating when everything
     * else has stopped, because pausing is usually what opened it.
     *
     * Taken from there, a screen's entrance never advanced: it drew at alpha 0
     * and the menu was invisible, while the HUD -- carrying no transition, so
     * its weight is a constant 1 -- kept drawing, which makes the failure look
     * like a layout bug rather than a clock one. The menu goldens caught it and
     * no gate arm can, since every probe passes a dt of its own.
     */
    const float dt = engine->headless ? (float)ENGINE_FIXED_FRAME_DT : (float)engine->delta_time;
    ui_build(ui, (float)engine->win_width, (float)engine->win_height, dt);
    ui_draw_list_render(ui->dl);
}

UISystem* create_ui_system(Engine* engine) {
    if (!engine) {
        log_error("ui: create_ui_system needs an engine");
        return NULL;
    }
    UISystem* ui = calloc(1, sizeof(UISystem));
    if (!ui) {
        log_error("ui: out of memory creating the system");
        return NULL;
    }
    ui->engine = engine;
    ui->dl = create_ui_draw_list(engine);
    if (!ui->dl) {
        free(ui);
        return NULL;
    }
    ui->font_size = 16.0f;
    ui->last_pointer_x = -FLT_MAX;
    ui->last_pointer_y = -FLT_MAX;
    return ui;
}

void free_ui_system(UISystem* ui) {
    if (!ui)
        return;
    for (size_t i = 0; i < ui->screen_count; i++) {
        if (ui->screens[i]) {
            _free_element(ui->screens[i]->root);
            free(ui->screens[i]->name);
            free(ui->screens[i]);
        }
    }
    free(ui->screens);
    free(ui->stack);
    free_ui_draw_list(ui->dl);
    free(ui);
}

void ui_attach(UISystem* ui, Engine* engine) {
    if (!ui || !engine)
        return;
    engine_set_overlay(engine, _ui_overlay, ui);
}

void ui_set_font(UISystem* ui, Font* font, float size) {
    if (!ui)
        return;
    ui->font = font;
    if (size > 0.0f)
        ui->font_size = size;
    ui_draw_list_set_font(ui->dl, font, ui->font_size);
}

void ui_set_theme(UISystem* ui, const UITheme* theme) {
    if (!ui)
        return;
    if (theme)
        ui->theme = *theme;
    else
        memset(&ui->theme, 0, sizeof(UITheme));
}

const UITheme* ui_theme(const UISystem* ui) {
    return ui ? &ui->theme : NULL;
}

UIDrawList* ui_draw_list(UISystem* ui) {
    return ui ? ui->dl : NULL;
}

UIScreen* ui_find_screen(UISystem* ui, const char* name) {
    if (!ui || !name)
        return NULL;
    for (size_t i = 0; i < ui->screen_count; i++)
        if (ui->screens[i] && ui->screens[i]->name && !strcmp(ui->screens[i]->name, name))
            return ui->screens[i];
    return NULL;
}

UIScreen* ui_screen(UISystem* ui, const char* name) {
    if (!ui)
        return NULL;
    UIScreen* s = calloc(1, sizeof(UIScreen));
    if (!s) {
        log_error("ui: out of memory creating a screen");
        return NULL;
    }
    s->ui = ui;
    s->modal = true;
    s->name = name ? safe_strdup(name) : NULL;
    s->root = _new_element(NULL, UI_ROOT);
    if (!s->root) {
        free(s->name);
        free(s);
        return NULL;
    }
    // A screen's root fills the screen and holds no look of its own: a backdrop
    // is an explicit child, so a screen over live gameplay does not dim it by
    // accident.
    s->root->size_mode[0] = UI_GROW;
    s->root->size_mode[1] = UI_GROW;
    s->root->align_main = UI_ALIGN_CENTER;
    s->root->align_cross = UI_ALIGN_CENTER;

    if (!grow_array((void**)&ui->screens, &ui->screen_cap, ui->screen_count + 1, sizeof(UIScreen*),
                    8)) {
        _free_element(s->root);
        free(s->name);
        free(s);
        return NULL;
    }
    ui->screens[ui->screen_count++] = s;
    return s;
}

UIElement* ui_screen_root(UIScreen* screen) {
    return screen ? screen->root : NULL;
}

void ui_screen_set_modal(UIScreen* screen, bool modal) {
    if (screen)
        screen->modal = modal;
}

void ui_screen_transition(UIScreen* screen, UITransition kind, float seconds) {
    if (!screen)
        return;
    screen->transition = kind;
    screen->transition_seconds = seconds > 0.0f ? seconds : 0.0f;
}

void ui_push(UISystem* ui, UIScreen* screen) {
    if (!ui || !screen)
        return;
    if (!grow_array((void**)&ui->stack, &ui->stack_cap, ui->stack_count + 1, sizeof(UIScreen*), 8))
        return;
    // Replay the entrance. A screen pushed, popped and pushed again should
    // arrive the same way each time, not appear already finished.
    screen->t_in = 0.0f;
    ui->stack[ui->stack_count++] = screen;
}

void ui_pop(UISystem* ui) {
    if (ui && ui->stack_count > 0)
        ui->stack_count--;
}

void ui_pop_all(UISystem* ui) {
    if (ui)
        ui->stack_count = 0;
}

UIScreen* ui_top(const UISystem* ui) {
    if (!ui || ui->stack_count == 0)
        return NULL;
    return ui->stack[ui->stack_count - 1];
}

bool ui_captures_input(const UISystem* ui) {
    if (!ui)
        return false;
    for (size_t i = 0; i < ui->stack_count; i++)
        if (ui->stack[i] && ui->stack[i]->modal)
            return true;
    return false;
}
