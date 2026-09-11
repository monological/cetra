#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "ext/log.h"
#include "ui.h"

// How fast hover and focus reach their target, in seconds. Short enough that a
// menu feels immediate, long enough that a shader reading uFocus has something
// to animate with.
#define UI_EASE_SECONDS 0.12f

struct UIScreen {
    char* name;
    UIElement* root;
    bool modal;
    UISystem* ui;
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
    // nothing resolves to: the face's own leading, and no added tracking.
    s.line_spacing = 1.0f;
    s.tracking = 0.0f;
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
                s.bg[0] = 0.18f;
                s.bg[1] = 0.20f;
                s.bg[2] = 0.26f;
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

// Copies any field of `from` that `into` left at zero. The one place the
// zero-means-inherit rule is implemented, so there is no second place for it to
// mean something else.
static void _inherit(UIStyle* into, const UIStyle* from) {
    if (into->bg[3] == 0.0f && into->bg[0] == 0.0f && into->bg[1] == 0.0f && into->bg[2] == 0.0f)
        memcpy(into->bg, from->bg, sizeof(vec4));
    if (into->fg[3] == 0.0f && into->fg[0] == 0.0f && into->fg[1] == 0.0f && into->fg[2] == 0.0f)
        memcpy(into->fg, from->fg, sizeof(vec4));
    if (into->border[3] == 0.0f)
        memcpy(into->border, from->border, sizeof(vec4));
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

static UIStyle _resolve(const UISystem* ui, const UIElement* el, UIState state) {
    UIStyle s = el->style ? *el->style : (UIStyle){0};
    _inherit(&s, _theme_entry(&ui->theme, el->kind, state));
    const UIStyle def = _default_style(el->kind, state);
    _inherit(&s, &def);
    if (!s.font)
        s.font = ui->font;
    if (ui->theme.font_size > 0.0f && (!el->style || el->style->font_size == 0.0f) &&
        _theme_entry(&ui->theme, el->kind, state)->font_size == 0.0f)
        s.font_size = ui->theme.font_size;
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
        if (parent->child_count + 1 > parent->child_capacity) {
            size_t cap = parent->child_capacity ? parent->child_capacity * 2 : 8;
            UIElement** kids = realloc(parent->children, cap * sizeof(UIElement*));
            if (!kids) {
                log_error("ui: out of memory growing a child list to %zu", cap);
                free(el);
                return NULL;
            }
            parent->children = kids;
            parent->child_capacity = cap;
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
    el->text = NULL;
    if (!text)
        return;
    const size_t n = strlen(text) + 1;
    el->text = malloc(n);
    if (el->text)
        memcpy(el->text, text, n);
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
#define UI_KNOB_W        26.0f
#define UI_KNOB_H        16.0f
#define UI_TRACK_H       4.0f
#define UI_FURNITURE_GAP 14.0f

static float _widest_option(const UISystem* ui, const UIElement* el, const UIStyle* s) {
    if (!el->options || el->option_count <= 0 || !s->font)
        return 0.0f;
    float widest = 0.0f;
    for (int i = 0; i < el->option_count; i++) {
        if (!el->options[i])
            continue;
        const float w = ui_text_width(s->font, s->font_size, s->tracking, el->options[i]);
        widest = w > widest ? w : widest;
    }
    (void)ui;
    return widest;
}

// What one element's own content wants, before any parent has a say.
static void _intrinsic(const UISystem* ui, UIElement* el, float* out_w, float* out_h) {
    const UIStyle s = _resolve(ui, el, UI_STATE_NORMAL);
    float w = 0.0f, h = 0.0f;
    if (el->text && el->text[0] && s.font) {
        w = ui_text_width(s.font, s.font_size, s.tracking, el->text);
        h = ui_line_height(s.font, s.font_size, s.line_spacing);
    }

    switch (el->kind) {
        case UI_TOGGLE:
            w += UI_FURNITURE_GAP + UI_KNOB_W;
            h = h > UI_KNOB_H ? h : UI_KNOB_H;
            break;
        case UI_SELECTOR:
            w += UI_FURNITURE_GAP + _widest_option(ui, el, &s);
            break;
        case UI_SLIDER:
            // The track sits on its own row under the label, so the height grows
            // rather than the width.
            h += UI_FURNITURE_GAP + UI_TRACK_H;
            break;
        default:
            break;
    }

    const float* pad = (el->padding[0] || el->padding[1] || el->padding[2] || el->padding[3])
                           ? el->padding
                           : s.padding;
    *out_w = w + pad[1] + pad[3];
    *out_h = h + pad[0] + pad[2];
}

static float _spacing_of(const UISystem* ui, const UIElement* el) {
    if (el->spacing > 0.0f)
        return el->spacing;
    return ui->theme.spacing > 0.0f ? ui->theme.spacing : 8.0f;
}

static void _padding_of(const UISystem* ui, const UIElement* el, float out[4]) {
    if (el->padding[0] || el->padding[1] || el->padding[2] || el->padding[3]) {
        memcpy(out, el->padding, sizeof(float) * 4);
        return;
    }
    const UIStyle s = _resolve(ui, el, UI_STATE_NORMAL);
    memcpy(out, s.padding, sizeof(float) * 4);
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

static UIState _state_of(const UIElement* el) {
    if (el->disabled)
        return UI_STATE_DISABLED;
    if (el->state == UI_STATE_ACTIVE)
        return UI_STATE_ACTIVE;
    if (el->t_focus > 0.5f)
        return UI_STATE_FOCUS;
    if (el->t_hover > 0.5f)
        return UI_STATE_HOVER;
    return UI_STATE_NORMAL;
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
                const bool on = el->bound_bool && *el->bound_bool;
                const UIRect box = {el->rect.x + el->rect.w - s.padding[1] - UI_KNOB_W,
                                    el->rect.y + (el->rect.h - UI_KNOB_H) * 0.5f, UI_KNOB_W,
                                    UI_KNOB_H};
                UIStyle knob = {.corner_radius = UI_KNOB_H * 0.5f};
                const vec4 knob_on = {0.30f, 0.72f, 0.42f, 1.0f};
                const vec4 knob_off = {0.25f, 0.26f, 0.30f, 1.0f};
                memcpy(knob.bg, on ? knob_on : knob_off, sizeof(vec4));
                ui_draw_rect(dl, box, &knob);
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

                const UIRect track = {label.x, label.y + label.h + UI_FURNITURE_GAP, label.w,
                                      UI_TRACK_H};
                UIStyle bar = {.corner_radius = UI_TRACK_H * 0.5f};
                const vec4 bar_bg = {0.22f, 0.23f, 0.28f, 1.0f};
                memcpy(bar.bg, bar_bg, sizeof(vec4));
                ui_draw_rect(dl, track, &bar);

                UIRect filled = track;
                filled.w *= t;
                UIStyle lit = {.corner_radius = UI_TRACK_H * 0.5f};
                const vec4 lit_bg = {0.45f, 0.70f, 1.00f, 1.0f};
                memcpy(lit.bg, lit_bg, sizeof(vec4));
                ui_draw_rect(dl, filled, &lit);
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
    el->t_hover += (hover_target - el->t_hover) * (step < 1.0f ? step : 1.0f);
    el->t_focus += (focus_target - el->t_focus) * (step < 1.0f ? step : 1.0f);
    for (size_t i = 0; i < el->child_count; i++)
        _ease(el->children[i], dt);
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
        _ease(s->root, dt);
        ui_layout(s, width, height);
        _emit(ui, s->root);
    }
}

// ---------------------------------------------------------------- the system

static void _ui_overlay(Engine* engine, void* user) {
    UISystem* ui = user;
    if (!ui || !engine)
        return;
    ui_build(ui, (float)engine->win_width, (float)engine->win_height, (float)engine->delta_time);
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
    if (name) {
        const size_t n = strlen(name) + 1;
        s->name = malloc(n);
        if (s->name)
            memcpy(s->name, name, n);
    }
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

    if (ui->screen_count + 1 > ui->screen_cap) {
        size_t cap = ui->screen_cap ? ui->screen_cap * 2 : 8;
        UIScreen** ns = realloc(ui->screens, cap * sizeof(UIScreen*));
        if (!ns) {
            log_error("ui: out of memory growing the screen list");
            _free_element(s->root);
            free(s->name);
            free(s);
            return NULL;
        }
        ui->screens = ns;
        ui->screen_cap = cap;
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

void ui_push(UISystem* ui, UIScreen* screen) {
    if (!ui || !screen)
        return;
    if (ui->stack_count + 1 > ui->stack_cap) {
        size_t cap = ui->stack_cap ? ui->stack_cap * 2 : 8;
        UIScreen** ns = realloc(ui->stack, cap * sizeof(UIScreen*));
        if (!ns) {
            log_error("ui: out of memory growing the screen stack");
            return;
        }
        ui->stack = ns;
        ui->stack_cap = cap;
    }
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
