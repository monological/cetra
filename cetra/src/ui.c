#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "ext/log.h"
#include "ui.h"
#include "ui_field.h"
#include "ui_text.h"
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
    bool opaque;
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
    Engine* engine;   // borrowed
    Engine* attached; // borrowed: the engine ui_attach added this to, which free takes it off
    UIDrawList* dl;   // owned

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

    // The pane whose bar the pointer holds, and how far down its thumb it took hold.
    UIElement* bar_drag;
    float bar_grab;
    // The focus as the last input pass left it. A top screen's focus that differs was set by the
    // app or arrived with a screen, and is scrolled into view like one navigated to; one the
    // pointer moved is not, or hovering a row half out of a pane would scroll it.
    UIElement* focus_seen;

    // The draw's clock, which a focused field's caret blinks on: the wall clock in a window, set
    // by the overlay, and 0 to a build an app calls itself. And the seconds until that caret next
    // changes, which the overlay asks idle mode to wake for; INFINITY when it does not.
    double now;
    double caret_wake;
};

// A scrolling pane's bar: the thumb's width, its inset from the pane's edge, the shortest it is
// drawn, and the strip along the edge a press lands on.
#define UI_BAR_W     6.0f
#define UI_BAR_INSET 2.0f
#define UI_BAR_MIN   24.0f
#define UI_BAR_HIT   14.0f
// How close an eased scroll comes before it is there: a tenth of a point, under any pixel.
#define UI_SCROLL_SNAP 0.1f

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
        case UI_TEXT_FIELD:
            // A well rather than a raised control: darker than a button, a faint edge, and the
            // focus colour's border while it takes the keys.
            s.fg[0] = s.fg[1] = s.fg[2] = 0.92f;
            s.fg[3] = 1.0f;
            s.bg[0] = 0.05f;
            s.bg[1] = 0.06f;
            s.bg[2] = 0.08f;
            s.bg[3] = 0.95f;
            s.border[0] = s.border[1] = s.border[2] = 1.0f;
            s.border[3] = 0.14f;
            s.border_width = 1.0f;
            s.corner_radius = 6.0f;
            s.padding[0] = s.padding[2] = 8.0f;
            s.padding[1] = s.padding[3] = 10.0f;
            if (state == UI_STATE_FOCUS || state == UI_STATE_ACTIVE) {
                s.border[0] = 0.45f;
                s.border[1] = 0.70f;
                s.border[2] = 1.00f;
                s.border[3] = 1.0f;
                s.border_width = 1.5f;
            } else if (state == UI_STATE_DISABLED) {
                s.fg[0] = s.fg[1] = s.fg[2] = 0.45f;
            }
            break;
        case UI_SCROLL:
            // A window onto its content: no fill and no padding of its own, a faint thumb, and a
            // subtle border while the focus is inside it.
            s.fg[0] = s.fg[1] = s.fg[2] = 1.0f;
            s.fg[3] = 0.35f;
            if (state == UI_STATE_FOCUS || state == UI_STATE_ACTIVE) {
                s.border[0] = 0.45f;
                s.border[1] = 0.70f;
                s.border[2] = 1.00f;
                s.border[3] = 0.45f;
                s.border_width = 1.0f;
            }
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
        case UI_SCROLL:
            return &t->scroll[state];
        case UI_TEXT_FIELD:
            return &t->text_field[state];
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
    el->focusable = (kind == UI_BUTTON || kind == UI_TOGGLE || kind == UI_SLIDER ||
                     kind == UI_SELECTOR || kind == UI_TEXT_FIELD);
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

static void _drop_lines(UIElement* el) {
    if (!el->lines)
        return;
    free(el->lines->starts);
    free(el->lines);
    el->lines = NULL;
}

/*
 * A pane's virtual rows (ui_scroll_set_rows). Heights and tops are per row; the pane's children
 * are the rows built, in row order, `child_row` saying which row each holds. Building and freeing
 * happen in the layout, where the system is in hand to let go of anything it held in a freed row.
 * A built row's height is taken again at every layout, so a kept height is only ever an estimate
 * for a row not built.
 */
struct UIRows {
    int count;
    float estimate;
    UIRowFn build;
    void* user;
    float* height; // as last measured, or the estimate
    float* top;    // where each row starts, the gaps before it included; count + 1 of them
    size_t height_capacity, top_capacity;
    float gap;  // the pane's spacing the tops were summed with
    bool dirty; // the tops need summing again
    int* child_row;
    size_t child_row_capacity;
    bool restart; // every child freed before the rows are built
    int reveal;   // the row ui_scroll_to_row asked for, until it is built; -1 for none
};

// What a built row changed since it was built holds in `child_row`: a row past any count, so the
// next layout frees it and, if it is still wanted, builds it again.
#define ROW_CHANGED INT_MAX

static void _rows_free(struct UIRows* r) {
    if (!r)
        return;
    free(r->height);
    free(r->top);
    free(r->child_row);
    free(r);
}

static bool _rows_reserve(struct UIRows* r, int count) {
    const size_t n = count > 0 ? (size_t)count : 1;
    if (grow_array((void**)&r->height, &r->height_capacity, n, sizeof(float), 64) &&
        grow_array((void**)&r->top, &r->top_capacity, n + 1, sizeof(float), 64))
        return true;
    log_error("ui: out of memory for %d rows", count);
    return false;
}

// Rows [from, to) as new, at the estimate's height.
static void _rows_fresh(struct UIRows* r, int from, int to) {
    for (int i = from; i < to; i++)
        r->height[i] = r->estimate;
    r->dirty = true;
}

static void _free_element(UIElement* el) {
    if (!el)
        return;
    for (size_t i = 0; i < el->child_count; i++)
        _free_element(el->children[i]);
    free(el->children);
    free(el->text);
    free(el->runs);
    _drop_lines(el);
    ui_field_free(el->field);
    _rows_free(el->rows);
    free(el->style);
    free(el);
}

void ui_scroll_set_rows(UIElement* pane, int count, float estimate, UIRowFn build, void* user) {
    if (!pane || pane->kind != UI_SCROLL || !build || count < 0) {
        log_error(
            "ui_scroll_set_rows: wants a scrolling pane, a count of none or more, and a build");
        return;
    }
    // A pane's rows are its own only once there is room for them, so a refusal leaves it as it was.
    struct UIRows* r = pane->rows ? pane->rows : calloc(1, sizeof(struct UIRows));
    if (!r || !_rows_reserve(r, count)) {
        log_error("ui_scroll_set_rows: out of memory for %d rows", count);
        if (r != pane->rows)
            _rows_free(r);
        return;
    }
    pane->rows = r;
    r->count = count;
    r->estimate = estimate > 0.0f ? estimate : 1.0f;
    r->build = build;
    r->user = user;
    r->restart = true;
    r->reveal = -1;
    _rows_fresh(r, 0, count);
    pane->scroll = pane->scroll_target = 0.0f;
}

void ui_scroll_set_row_count(UIElement* pane, int count) {
    if (!pane || !pane->rows || count < 0) {
        log_error("ui_scroll_set_row_count: not a pane with rows, or a count under none");
        return;
    }
    struct UIRows* r = pane->rows;
    if (!_rows_reserve(r, count))
        return;
    if (count > r->count)
        _rows_fresh(r, r->count, count);
    r->count = count;
    r->dirty = true;
}

void ui_scroll_rows_changed(UIElement* pane, int first, int count) {
    if (!pane || !pane->rows) {
        log_error("ui_scroll_rows_changed: not a pane with rows");
        return;
    }
    struct UIRows* r = pane->rows;
    const long long lo = first, hi = (long long)first + count;
    for (size_t k = 0; k < pane->child_count; k++)
        if (r->child_row[k] >= lo && r->child_row[k] < hi)
            r->child_row[k] = ROW_CHANGED;
}

UIElement* ui_panel(UIElement* parent) {
    return _new_element(parent, UI_PANEL);
}

UIElement* ui_scroll(UIElement* parent) {
    return _new_element(parent, UI_SCROLL);
}

UIElement* ui_text_field(UIElement* parent, unsigned flags, UIActionFn on_submit, void* user) {
    UIElement* el = _new_element(parent, UI_TEXT_FIELD);
    if (!el)
        return NULL;
    el->field = ui_field_make(el, flags);
    el->action = on_submit;
    el->action_user = user;
    // A field with no width has nowhere to type, so its width defaults away from FIT, as a
    // slider's does.
    el->size_mode[0] = UI_FIXED;
    el->size[0] = 240.0f;
    return el;
}

Engine* ui_system_engine(const UISystem* ui) {
    return ui ? ui->engine : NULL;
}

// Whether `el` takes the keys while it holds the focus: a text field does, everything else leaves
// them to the screen and the app.
static bool _takes_keys(const UIElement* el) {
    return el && el->kind == UI_TEXT_FIELD;
}

bool ui_wants_keyboard(const UISystem* ui) {
    const UIScreen* top = ui_top(ui);
    if (top && _takes_keys(top->focused))
        return true;
    return ui_captures_input(ui);
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
    if (el->kind == UI_TEXT_FIELD)
        return; // a field's text is its placeholder, and its runs are its buffer's
    free(el->runs);
    el->runs = NULL;
    el->run_count = 0;
    _drop_lines(el);
}

// Whether byte `i` of `s` starts a codepoint or is the end: never inside a UTF-8 sequence.
static bool _codepoint_boundary(const char* s, size_t i) {
    return ((unsigned char)s[i] & 0xc0) != 0x80;
}

bool ui_set_text_runs(UIElement* el, const UITextRun* runs, size_t count) {
    if (!el) {
        log_error("ui_set_text_runs: NULL element");
        return false;
    }
    if (runs && count > 0) {
        const bool read_only =
            el->kind == UI_TEXT_FIELD && (ui_field_flags(el) & UI_FIELD_READ_ONLY);
        if (el->kind != UI_LABEL && !read_only) {
            log_error("ui_set_text_runs: only a label or a read-only field takes styled runs; an "
                      "editable field stays in one style");
            return false;
        }
        const char* text = el->kind == UI_TEXT_FIELD ? ui_field_text(el) : el->text ? el->text : "";
        const size_t len = strlen(text);
        size_t reached = 0;
        for (size_t i = 0; i < count; i++) {
            const UITextRun* r = &runs[i];
            if (r->start < reached) {
                log_error("ui_set_text_runs: run %zu starts at byte %zu, before the run before it "
                          "ends at %zu",
                          i, r->start, reached);
                return false;
            }
            if (r->start > len || r->length > len - r->start) {
                log_error("ui_set_text_runs: run %zu reaches past the text's %zu bytes", i, len);
                return false;
            }
            if (!_codepoint_boundary(text, r->start) ||
                !_codepoint_boundary(text, r->start + r->length)) {
                log_error("ui_set_text_runs: run %zu starts or ends inside a UTF-8 sequence", i);
                return false;
            }
            reached = r->start + r->length;
        }
    }
    UITextRun* copy = NULL;
    if (runs && count > 0) {
        copy = malloc(count * sizeof(UITextRun));
        if (!copy) {
            log_error("ui_set_text_runs: out of memory");
            return false;
        }
        memcpy(copy, runs, count * sizeof(UITextRun));
    }
    free(el->runs);
    el->runs = copy;
    el->run_count = copy ? count : 0;
    _drop_lines(el);
    return true;
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

// The text an element lays out and draws: its own, in a resolved style, with its runs.
static UIText _element_text(const UISystem* ui, const UIElement* el, const UIStyle* s) {
    return ui_text_make(ui->dl, el->text, el->text ? strlen(el->text) : 0, s, el->runs,
                        el->run_count);
}

/*
 * What one element's own content wants along `axis` (0 across, 1 down), before any parent has a
 * say. The height of a wrapping label is its lines, which its width decided: that is why the
 * layout settles every width before it measures any height.
 */
static float _intrinsic(const UISystem* ui, const UIElement* el, int axis) {
    const UIStyle s = _resolve(ui, el, UI_STATE_NORMAL);
    float v = 0.0f;
    // A field's `text` is its placeholder, which sizes nothing: its height is its rows.
    if (el->kind != UI_TEXT_FIELD && el->text && el->text[0] && s.font) {
        if (axis == 0) {
            const UIText t = _element_text(ui, el, &s);
            v = el->wrap ? ui_text_longest_word(&t) : ui_text_widest_line(&t);
        } else {
            // One line height PER LINE. ui_draw_text starts a new line at every
            // '\n' it is given, so charging one made an authored two-line label
            // measure half its height and draw outside its own rect and its
            // parent's -- with the layout numbers all looking correct.
            size_t lines = 1;
            if (el->wrap && el->lines)
                lines = el->lines->count;
            else
                for (const char* p = el->text; *p; p++)
                    if (*p == '\n')
                        lines++;
            v = ui_line_height(s.font, s.font_size, s.line_spacing) * (float)lines;
        }
    }

    switch (el->kind) {
        case UI_TOGGLE:
            if (axis == 0)
                v += UI_FURNITURE_GAP + UI_KNOB_W;
            else
                v = v > UI_KNOB_H ? v : UI_KNOB_H;
            break;
        case UI_SELECTOR:
            if (axis == 0)
                v += UI_FURNITURE_GAP + _widest_option(el, &s);
            break;
        case UI_SLIDER:
            // The track sits on its own row under the label, so the height grows
            // rather than the width.
            if (axis == 1)
                v += UI_FURNITURE_GAP + UI_TRACK_H;
            break;
        case UI_TEXT_FIELD:
            if (axis == 1)
                v = ui_field_content_height(el);
            break;
        default:
            break;
    }

    float pad[4];
    _padding_from(el, &s, pad);
    return axis == 0 ? v + pad[1] + pad[3] : v + pad[0] + pad[2];
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
 * The layout runs ONE AXIS AT A TIME: every width (measure, then arrange), then each wrapping
 * label's lines at the width it was given, then every height. Widths never depended on heights,
 * so splitting the pass changes no number; it is what lets a height depend on a width.
 */
static int _main_axis(const UIElement* el) {
    return el->dir == UI_ROW ? 0 : 1;
}

static float* _pos(UIRect* r, int axis) {
    return axis == 0 ? &r->x : &r->y;
}

static float* _len(UIRect* r, int axis) {
    return axis == 0 ? &r->w : &r->h;
}

// Sums a pane's row tops again when a height, the count or its spacing has changed.
static void _rows_tops(const UISystem* ui, UIElement* pane) {
    struct UIRows* r = pane->rows;
    const float gap = _spacing_of(ui, pane);
    if (!r->dirty && gap == r->gap)
        return;
    r->gap = gap;
    r->top[0] = 0.0f;
    for (int i = 0; i < r->count; i++)
        r->top[i + 1] = r->top[i] + r->height[i] + (i + 1 < r->count ? gap : 0.0f);
    r->dirty = false;
}

// The least scroll from `target` that shows [y0, y1] of a pane's content whole through `view`
// points -- its top, if it is taller than the view.
static float _least_scroll(float target, float y0, float y1, float view) {
    if (y0 < target)
        return y0;
    if (y1 > target + view)
        return y1 - view < y0 ? y1 - view : y0;
    return target;
}

// The least scroll from the pane's target that shows row `index` whole.
static float _row_reveal_target(const UIElement* pane, int index, float view) {
    const struct UIRows* r = pane->rows;
    return _least_scroll(pane->scroll_target, r->top[index], r->top[index] + r->height[index],
                         view);
}

// The row at `y` down a pane's content: the last that starts at or above it.
static int _row_at(const struct UIRows* r, float y) {
    if (r->count <= 0)
        return 0;
    int lo = 0, hi = r->count;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (r->top[mid] <= y)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

// The first row any of which shows below `y`: past the row at `y` when only its gap is there.
static int _row_seen(const struct UIRows* r, float y) {
    const int k = _row_at(r, y);
    return (k + 1 < r->count && r->top[k] + r->height[k] <= y) ? k + 1 : k;
}

/*
 * The heights of a pane's built rows, as this measure found them, kept for when they are not
 * built. A height that changed above the first row in view moves the scroll by as much, so the row
 * in view does not move.
 */
static void _rows_measure(const UISystem* ui, UIElement* pane) {
    struct UIRows* r = pane->rows;
    _rows_tops(ui, pane);
    const int first = _row_seen(r, pane->scroll);
    for (size_t k = 0; k < pane->child_count; k++) {
        const int row = r->child_row[k];
        const float h = pane->children[k]->rect.h;
        if (row >= r->count || r->height[row] == h)
            continue;
        if (row < first) {
            pane->scroll += h - r->height[row];
            pane->scroll_target += h - r->height[row];
        }
        r->height[row] = h;
        r->dirty = true;
    }
    _rows_tops(ui, pane);
}

/*
 * Pass 1, bottom-up: every element gets the size its own content wants. A GROW
 * axis is measured too, at its FIT size -- that is the MINIMUM it will accept,
 * and it is what a FIT parent measures against, which is what stops the two
 * modes being mutually recursive.
 */
static void _measure(const UISystem* ui, UIElement* el, int axis) {
    for (size_t i = 0; i < el->child_count; i++)
        _measure(ui, el->children[i], axis);

    float v = _intrinsic(ui, el, axis);

    if (el->rows && axis == 1) {
        // A pane of rows is as tall as all of them, built or not.
        _rows_measure(ui, el);
        float pad[4];
        _padding_of(ui, el, pad);
        v = el->rows->top[el->rows->count] + pad[0] + pad[2];
    } else if (el->child_count > 0) {
        float pad[4];
        _padding_of(ui, el, pad);
        const float gap = _spacing_of(ui, el);
        const bool along = _main_axis(el) == axis;
        float sum = 0.0f, most = 0.0f;
        size_t counted = 0;
        for (size_t i = 0; i < el->child_count; i++) {
            UIElement* c = el->children[i];
            if (c->fill)
                continue; // a backdrop is outside the flow
            const float cl = *_len(&c->rect, axis);
            sum += cl;
            most = cl > most ? cl : most;
            counted++;
        }
        if (along && counted > 1)
            sum += gap * (float)(counted - 1);

        const float kids = (along ? sum : most) + (axis == 0 ? pad[1] + pad[3] : pad[0] + pad[2]);
        v = kids > v ? kids : v;
    }
    // A growing pane asks nothing of its parent down the axis it scrolls: whatever height it is
    // given, its content is seen through it. Fitting, it is as tall as its content.
    if (el->kind == UI_SCROLL && axis == 1 && el->size_mode[1] == UI_GROW) {
        float pad[4];
        _padding_of(ui, el, pad);
        v = pad[0] + pad[2];
    }

    *_len(&el->rect, axis) = (el->size_mode[axis] == UI_FIXED) ? el->size[axis] : v;
}

static float _clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/*
 * A pane's scroll for this layout, its content `content` points tall seen through `view`: kept
 * inside what the content allows, and moved to the end if it was at the end, the content grew and
 * the pane follows its end. A pane that follows begins at its end, since nothing has been seen.
 */
static float _scroll_settle(UIElement* el, float content, float view) {
    const float was_max = el->scroll_content - view > 0.0f ? el->scroll_content - view : 0.0f;
    const float max = content - view > 0.0f ? content - view : 0.0f;
    if (el->follow_end && content > el->scroll_content && el->scroll_target >= was_max - 0.5f)
        el->scroll = el->scroll_target = max;
    el->scroll_content = content;
    el->scroll_target = _clampf(el->scroll_target, 0.0f, max);
    el->scroll = _clampf(el->scroll, 0.0f, max);
    return el->scroll;
}

// Pass 2, top-down: `el` already has its final extent on `axis`; place the children along it,
// expanding GROW into whatever is left over.
static void _arrange(const UISystem* ui, UIElement* el, UIRect screen, int axis) {
    float pad[4];
    _padding_of(ui, el, pad);
    UIRect inner = ui_rect_inset(el->rect, pad[0], pad[1], pad[2], pad[3]);
    const float in_pos = *_pos(&inner, axis), in_len = *_len(&inner, axis);
    const float gap = _spacing_of(ui, el);

    // A fill element ignores the flow entirely and covers the screen. It is
    // still laid out afterwards, so a backdrop can have children of its own.
    for (size_t i = 0; i < el->child_count; i++) {
        UIElement* c = el->children[i];
        if (c->fill) {
            *_pos(&c->rect, axis) = *_pos(&screen, axis);
            *_len(&c->rect, axis) = *_len(&screen, axis);
            _arrange(ui, c, screen, axis);
        }
    }

    if (_main_axis(el) != axis) {
        // Across: a GROW child stretches to the inner extent, a smaller one is placed by the
        // cross alignment.
        for (size_t i = 0; i < el->child_count; i++) {
            UIElement* c = el->children[i];
            if (c->fill)
                continue;
            float* cl = _len(&c->rect, axis);
            float* cp = _pos(&c->rect, axis);
            if (c->size_mode[axis] == UI_GROW)
                *cl = in_len;
            *cp = in_pos;
            if (*cl < in_len) {
                if (el->align_cross == UI_ALIGN_CENTER)
                    *cp += (in_len - *cl) * 0.5f;
                else if (el->align_cross == UI_ALIGN_END)
                    *cp += in_len - *cl;
            }
            _arrange(ui, c, screen, axis);
        }
        return;
    }

    // A pane's rows each at their own top, less the scroll.
    if (el->rows && axis == 1) {
        struct UIRows* r = el->rows;
        // A row asked for is aimed at again from what is measured now, until it is built and so
        // measured itself: its place was an estimate's.
        if (r->reveal >= 0 && r->reveal < r->count) {
            el->scroll_target = _row_reveal_target(el, r->reveal, in_len);
            for (size_t i = 0; i < el->child_count; i++)
                if (r->child_row[i] == r->reveal)
                    r->reveal = -1;
        }
        const float scroll = _scroll_settle(el, r->top[r->count], in_len);
        for (size_t i = 0; i < el->child_count; i++) {
            UIElement* c = el->children[i];
            const int row = r->child_row[i] < r->count ? r->child_row[i] : r->count;
            *_pos(&c->rect, 1) = in_pos - scroll + r->top[row];
            _arrange(ui, c, screen, 1);
        }
        return;
    }

    float used = 0.0f;
    size_t counted = 0, grow_count = 0;
    for (size_t i = 0; i < el->child_count; i++) {
        UIElement* c = el->children[i];
        if (c->fill)
            continue;
        used += *_len(&c->rect, axis);
        if (c->size_mode[axis] == UI_GROW)
            grow_count++;
        counted++;
    }
    if (counted > 1)
        used += gap * (float)(counted - 1);

    const float leftover = in_len - used;
    const float share = (grow_count > 0 && leftover > 0.0f) ? leftover / (float)grow_count : 0.0f;

    // With nothing growing, the whole run is placed by the main-axis alignment.
    float cursor = in_pos;
    if (grow_count == 0 && leftover > 0.0f) {
        if (el->align_main == UI_ALIGN_CENTER)
            cursor += leftover * 0.5f;
        else if (el->align_main == UI_ALIGN_END)
            cursor += leftover;
    }
    // A pane's content runs on past it, moved up by its scroll; the rects stay window positions.
    if (el->kind == UI_SCROLL && axis == 1)
        cursor -= _scroll_settle(el, used, in_len);

    for (size_t i = 0; i < el->child_count; i++) {
        UIElement* c = el->children[i];
        if (c->fill)
            continue;
        float* cl = _len(&c->rect, axis);
        if (c->size_mode[axis] == UI_GROW)
            *cl += share;
        *_pos(&c->rect, axis) = cursor;
        cursor += *cl + gap;
        _arrange(ui, c, screen, axis);
    }
}

/*
 * A wrapping or styled label's lines, broken at the width the first pass gave it and kept by
 * ui_lines_break while that and the style are unchanged. The text and the runs drop them when
 * set, so a kept break is never for other text.
 */
static void _break_lines(const UISystem* ui, UIElement* el) {
    for (size_t i = 0; i < el->child_count; i++)
        _break_lines(ui, el->children[i]);
    if (el->kind == UI_TEXT_FIELD) {
        // A field keeps rows of its own, through every edit; this gives it the width.
        const UIStyle s = _resolve(ui, el, UI_STATE_NORMAL);
        float pad[4];
        _padding_from(el, &s, pad);
        ui_field_break(ui, el, &s, el->rect.w - pad[1] - pad[3]);
        return;
    }
    if (el->kind != UI_LABEL || !el->text || !(el->wrap || el->runs)) {
        _drop_lines(el);
        return;
    }
    const UIStyle s = _resolve(ui, el, UI_STATE_NORMAL);
    float pad[4];
    _padding_from(el, &s, pad);
    if (!el->lines) {
        el->lines = calloc(1, sizeof(UILines));
        if (!el->lines) {
            log_error("ui: out of memory breaking a label into lines");
            return;
        }
    }
    const UIText t = _element_text(ui, el, &s);
    if (ui_lines_break(el->lines, &t, el->wrap ? el->rect.w - pad[1] - pad[3] : INFINITY))
        el->wraps++;
    else if (el->lines->count == 0)
        _drop_lines(el);
}

static float _pane_view(const UISystem* ui, const UIElement* el);

static bool _contains(const UIElement* el, const UIElement* target) {
    for (const UIElement* p = target; p; p = p->parent)
        if (p == el)
            return true;
    return false;
}

// Frees a pane's child `k`, letting go of anything the system or a screen held inside it.
static void _rows_drop(UISystem* ui, UIElement* pane, size_t k) {
    UIElement* c = pane->children[k];
    if (ui->pressed && _contains(c, ui->pressed))
        ui->pressed = NULL;
    if (ui->bar_drag && _contains(c, ui->bar_drag))
        ui->bar_drag = NULL;
    if (ui->focus_seen && _contains(c, ui->focus_seen))
        ui->focus_seen = NULL;
    for (size_t i = 0; i < ui->screen_count; i++)
        if (ui->screens[i]->focused && _contains(c, ui->screens[i]->focused))
            ui->screens[i]->focused = NULL;
    _free_element(c);
    struct UIRows* r = pane->rows;
    for (size_t i = k + 1; i < pane->child_count; i++) {
        pane->children[i - 1] = pane->children[i];
        r->child_row[i - 1] = r->child_row[i];
    }
    pane->child_count--;
}

// The rows [*lo, *hi) in view with the pane scrolled to `scroll`, and one either side.
static void _rows_window(const UISystem* ui, const UIElement* pane, float scroll, int* lo,
                         int* hi) {
    const struct UIRows* r = pane->rows;
    const float view = _pane_view(ui, pane);
    const int first = _row_at(r, scroll);
    const int last = _row_at(r, scroll + (view > 0.0f ? view : 0.0f));
    *lo = first > 0 ? first - 1 : 0;
    *hi = last + 2 < r->count ? last + 2 : r->count;
}

/*
 * The rows a pane should hold for this layout, from where the last one left its scroll and its
 * height: those in view, one either side, and the one holding the focus. Freed are the rest, the
 * rows past the count and the rows changed (ROW_CHANGED); built are the ones missing, kept in row
 * order. True when anything was built or freed, so the layout runs again on what is held now.
 */
static bool _rows_settle(UISystem* ui, const UIScreen* screen, UIElement* pane) {
    struct UIRows* r = pane->rows;
    bool changed = false;
    if (r->restart) {
        while (pane->child_count > 0)
            _rows_drop(ui, pane, pane->child_count - 1);
        r->restart = false;
        changed = true;
    }
    _rows_tops(ui, pane);
    // In view where the pane is, and where it is easing to: a focus moved on faster than the
    // scroll eases finds the rows it moves onto built.
    int lo[2], hi[2];
    _rows_window(ui, pane, pane->scroll, &lo[0], &hi[0]);
    _rows_window(ui, pane, pane->scroll_target, &lo[1], &hi[1]);
    int keep = -1;
    for (size_t k = 0; k < pane->child_count; k++)
        if (screen->focused && _contains(pane->children[k], screen->focused))
            keep = r->child_row[k];
    for (size_t k = pane->child_count; k-- > 0;) {
        const int row = r->child_row[k];
        const bool wanted = (row >= lo[0] && row < hi[0]) || (row >= lo[1] && row < hi[1]);
        if (row >= r->count || (!wanted && row != keep)) {
            _rows_drop(ui, pane, k);
            changed = true;
        }
    }
    for (int row = lo[0] < lo[1] ? lo[0] : lo[1]; row < (hi[0] > hi[1] ? hi[0] : hi[1]); row++) {
        if (!(row >= lo[0] && row < hi[0]) && !(row >= lo[1] && row < hi[1]))
            continue;
        bool held = false;
        for (size_t k = 0; k < pane->child_count && !held; k++)
            held = r->child_row[k] == row;
        if (held)
            continue;
        if (!grow_array((void**)&r->child_row, &r->child_row_capacity, pane->child_count + 1,
                        sizeof(int), 16))
            break;
        UIElement* c = _new_element(pane, UI_ROOT);
        if (!c)
            break;
        c->size_mode[0] = UI_GROW;
        r->child_row[pane->child_count - 1] = row;
        r->build(c, row, r->user);
        pane->row_builds++;
        changed = true;
    }
    // Row order, for drawing, hit testing and Tab: an insertion sort, the list being nearly sorted.
    for (size_t k = 1; k < pane->child_count; k++)
        for (size_t j = k; j > 0 && r->child_row[j - 1] > r->child_row[j]; j--) {
            UIElement* c = pane->children[j];
            pane->children[j] = pane->children[j - 1];
            pane->children[j - 1] = c;
            const int t = r->child_row[j];
            r->child_row[j] = r->child_row[j - 1];
            r->child_row[j - 1] = t;
        }
    return changed;
}

static bool _settle_rows(UISystem* ui, const UIScreen* screen, UIElement* el) {
    bool changed = el->rows ? _rows_settle(ui, screen, el) : false;
    for (size_t i = 0; i < el->child_count; i++)
        changed |= _settle_rows(ui, screen, el->children[i]);
    return changed;
}

static void _layout_once(UIScreen* screen, UIRect full) {
    UIElement* root = screen->root;
    _measure(screen->ui, root, 0);
    root->rect.x = full.x;
    root->rect.w = full.w;
    _arrange(screen->ui, root, full, 0);
    _break_lines(screen->ui, root);
    _measure(screen->ui, root, 1);
    root->rect.y = full.y;
    root->rect.h = full.h;
    _arrange(screen->ui, root, full, 1);
}

void ui_layout(UIScreen* screen, float width, float height) {
    if (!screen || !screen->root || !screen->ui)
        return;
    const UIRect full = {0.0f, 0.0f, width, height};
    // A pane of rows decides what to build from where the last layout left it, so the rows it
    // builds can change what this one finds: a first layout gave it no height, a row measured
    // taller than its estimate moves what is in view. It runs again on what is built then, three
    // times at the most; a screen with no rows lays out once.
    _settle_rows(screen->ui, screen, screen->root);
    _layout_once(screen, full);
    for (int again = 0; again < 2 && _settle_rows(screen->ui, screen, screen->root); again++)
        _layout_once(screen, full);
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
// test alike, and by an app drawing its own slider: two copies of this
// rectangle would drift, and the symptom would be a bar that fills to a
// different place than the one you clicked.
UIRect ui_slider_track(const UISystem* ui, const UIElement* el) {
    const UIStyle s = _resolve(ui, el, _state_of(el));
    const float lh = ui_line_height(s.font, s.font_size, s.line_spacing);
    return (UIRect){el->rect.x + s.padding[3], el->rect.y + s.padding[0] + lh + UI_FURNITURE_GAP,
                    el->rect.w - s.padding[1] - s.padding[3], UI_TRACK_H};
}

// An element's rect less its padding: where a field's text is.
static UIRect _content_rect(const UISystem* ui, const UIElement* el) {
    float pad[4];
    _padding_of(ui, el, pad);
    return ui_rect_inset(el->rect, pad[0], pad[1], pad[2], pad[3]);
}

// The height a pane shows its content through: its rect less its padding.
static float _pane_view(const UISystem* ui, const UIElement* el) {
    float pad[4];
    _padding_of(ui, el, pad);
    return el->rect.h - pad[0] - pad[2];
}

// How far a pane's content can scroll; 0 while it fits.
static float _scroll_max(const UISystem* ui, const UIElement* el) {
    const float m = el->scroll_content - _pane_view(ui, el);
    return m > 0.0f ? m : 0.0f;
}

/*
 * A pane's bar: the track along its right edge and the thumb in it, the thumb's length the share
 * of the content in view and its place the share scrolled. False while the content fits, when
 * there is no bar. ONE definition, read by the draw and by the press alike.
 */
static bool _pane_bar(const UISystem* ui, const UIElement* el, UIRect* track, UIRect* thumb) {
    const float max = _scroll_max(ui, el);
    if (max <= 0.0f || el->scroll_content <= 0.0f)
        return false;
    *track = (UIRect){el->rect.x + el->rect.w - UI_BAR_W - UI_BAR_INSET, el->rect.y + UI_BAR_INSET,
                      UI_BAR_W, el->rect.h - 2.0f * UI_BAR_INSET};
    float h = track->h * _pane_view(ui, el) / el->scroll_content;
    const float floor = UI_BAR_MIN < track->h ? UI_BAR_MIN : track->h;
    h = h < floor ? floor : h;
    *thumb = (UIRect){track->x, track->y + (track->h - h) * (el->scroll / max), UI_BAR_W, h};
    return true;
}

static void _emit_pane(UISystem* ui, UIElement* el, const UIStyle* s);

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
                if (el->lines) {
                    const UIText t = _element_text(ui, el, &s);
                    ui_draw_text_lines(dl, el->rect, &t, el->lines, s.line_spacing,
                                       el->align_cross);
                } else {
                    ui_draw_text(dl, el->rect, el->text, &s, el->align_cross);
                }
                break;
            case UI_PANEL:
            case UI_SCROLL:
                ui_draw_rect(dl, el->rect, &s);
                break;
            case UI_TEXT_FIELD: {
                const UIScreen* top = ui_top(ui);
                const bool blink = ui->engine && !ui->engine->headless;
                const double wake = ui_field_draw(ui, el, &s, _content_rect(ui, el),
                                                  top && top->focused == el, ui->now, blink);
                ui->caret_wake = wake < ui->caret_wake ? wake : ui->caret_wake;
                break;
            }
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

                const UIRect track = ui_slider_track(ui, el);
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

    if (el->kind == UI_SCROLL) {
        _emit_pane(ui, el, &s);
        return;
    }
    for (size_t i = 0; i < el->child_count; i++)
        _emit(ui, el->children[i]);
}

/*
 * A pane's content, seen through it: clipped inside its border so nothing scrolled draws over the
 * border or outside the pane, the children wholly out of view not emitted at all, and the bar over
 * them. The pane's fill and border were drawn before, under the content.
 */
static void _emit_pane(UISystem* ui, UIElement* el, const UIStyle* s) {
    UIDrawList* dl = ui->dl;
    const float b = s->border[3] > 0.0f ? s->border_width : 0.0f;
    const UIRect view = ui_rect_inset(el->rect, b, b, b, b);
    ui_push_clip(dl, view);
    for (size_t i = 0; i < el->child_count; i++) {
        UIElement* c = el->children[i];
        if (c->rect.y < view.y + view.h && c->rect.y + c->rect.h > view.y)
            _emit(ui, c);
    }
    ui_pop_clip(dl);
    UIRect track, thumb;
    if (_pane_bar(ui, el, &track, &thumb))
        ui_draw_rounded(dl, thumb, UI_BAR_W * 0.5f, (float*)s->fg, NULL, 0.0f);
}

// One eased value a step toward its target, and onto it once within a quarter of an 8-bit code:
// an exponential approach never arrives, and a UI that asks for frames until it is still would
// ask for them forever.
static float _ease_toward(float t, float target, float k) {
    t += (target - t) * k;
    return fabsf(target - t) < 1.0f / 1024.0f ? target : t;
}

// Eases `el` and its subtree; true while anything in it is still moving.
static bool _ease(UIElement* el, float dt) {
    const float step = (UI_EASE_SECONDS > 0.0f) ? dt / UI_EASE_SECONDS : 1.0f;
    const float hover_target =
        (el->state == UI_STATE_HOVER || el->state == UI_STATE_ACTIVE) ? 1.0f : 0.0f;
    const float focus_target =
        (el->state == UI_STATE_FOCUS || el->state == UI_STATE_ACTIVE) ? 1.0f : 0.0f;
    const float value_target =
        (el->kind == UI_TOGGLE && el->bound_bool && *el->bound_bool) ? 1.0f : 0.0f;
    const float k = step < 1.0f ? step : 1.0f;
    el->t_hover = _ease_toward(el->t_hover, hover_target, k);
    el->t_focus = _ease_toward(el->t_focus, focus_target, k);
    el->t_value = _ease_toward(el->t_value, value_target, k);
    bool moving =
        el->t_hover != hover_target || el->t_focus != focus_target || el->t_value != value_target;
    // A pane's scroll eases after a key or a reveal, by the same rate, onto its target within a
    // tenth of a point: the 1/1024 of a weight would be a thousand points' tail.
    if (el->kind == UI_SCROLL && el->scroll != el->scroll_target) {
        el->scroll += (el->scroll_target - el->scroll) * k;
        if (fabsf(el->scroll_target - el->scroll) < UI_SCROLL_SNAP)
            el->scroll = el->scroll_target;
        moving |= el->scroll != el->scroll_target;
    }
    for (size_t i = 0; i < el->child_count; i++)
        moving |= _ease(el->children[i], dt);
    return moving;
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
    // A pane's content is under the pointer only where the pane shows it.
    if (el->kind == UI_SCROLL && !ui_rect_hit(el->rect, x, y))
        return NULL;
    for (size_t i = el->child_count; i-- > 0;) {
        UIElement* got = _hit(el->children[i], x, y);
        if (got)
            return got;
    }
    return (el->focusable && ui_rect_hit(el->rect, x, y)) ? el : NULL;
}

// The deepest pane showing the point, or NULL: what the wheel and a press on a bar reach.
static UIElement* _pane_at(UIElement* el, float x, float y) {
    if (!el || el->disabled)
        return NULL;
    if (el->kind == UI_SCROLL && !ui_rect_hit(el->rect, x, y))
        return NULL;
    for (size_t i = el->child_count; i-- > 0;) {
        UIElement* got = _pane_at(el->children[i], x, y);
        if (got)
            return got;
    }
    return el->kind == UI_SCROLL ? el : NULL;
}

// The pane round `el`, the nearest, or NULL.
static UIElement* _pane_of(UIElement* el) {
    for (UIElement* p = el ? el->parent : NULL; p; p = p->parent)
        if (p->kind == UI_SCROLL)
            return p;
    return NULL;
}

// Whether anything under `el` but `el` itself is drawn at the point: a pane's content only where
// the pane shows it.
static bool _over(const UIElement* el, float x, float y, bool is_root) {
    if (!is_root && ui_rect_hit(el->rect, x, y))
        return true;
    if (el->kind == UI_SCROLL)
        return false; // the point is outside it, so outside all it shows
    for (size_t i = 0; i < el->child_count; i++)
        if (_over(el->children[i], x, y, false))
            return true;
    return false;
}

// The points one wheel unit scrolls: the theme's step, else its font size.
static float _scroll_step(const UISystem* ui) {
    if (ui->theme.scroll_step > 0.0f)
        return ui->theme.scroll_step;
    const float size = ui->theme.font_size > 0.0f ? ui->theme.font_size : ui->font_size;
    return size > 0.0f ? size : 16.0f;
}

static void _scroll_to(const UISystem* ui, UIElement* pane, float target, bool now) {
    pane->scroll_target = _clampf(target, 0.0f, _scroll_max(ui, pane));
    if (now)
        pane->scroll = pane->scroll_target;
}

/*
 * Scrolls every pane round `el` by the least that shows it whole -- or its top, if it is taller
 * than the pane -- measured where the pane is going rather than where it is drawn mid-ease, so a
 * reveal during a reveal does not overshoot.
 */
static void _reveal(const UISystem* ui, UIElement* el) {
    if (!el)
        return;
    for (UIElement* p = _pane_of(el); p; p = _pane_of(p)) {
        // `el` down the pane's content, from where the pane draws it now.
        const UIRect view = _content_rect(ui, p);
        const float y0 = el->rect.y - view.y + p->scroll;
        _scroll_to(ui, p, _least_scroll(p->scroll_target, y0, y0 + el->rect.h, view.h), false);
    }
}

void ui_scroll_to_row(const UISystem* ui, UIElement* pane, int index) {
    if (!ui || !pane || !pane->rows || index < 0 || index >= pane->rows->count) {
        log_error("ui_scroll_to_row: not a pane with rows, or no row %d", index);
        return;
    }
    _rows_tops(ui, pane);
    pane->rows->reveal = index;
    _scroll_to(ui, pane, _row_reveal_target(pane, index, _pane_view(ui, pane)), false);
}

void ui_scroll_into_view(const UISystem* ui, UIElement* el) {
    if (!ui || !el) {
        log_error("ui_scroll_into_view: NULL system or element");
        return;
    }
    _reveal(ui, el);
}

bool ui_pointer_over(const UISystem* ui, float x, float y) {
    if (!ui)
        return false;
    if (ui_captures_input(ui))
        return true;
    for (size_t i = 0; i < ui->stack_count; i++)
        if (ui->stack[i] && ui->stack[i]->root && _over(ui->stack[i]->root, x, y, true))
            return true;
    return false;
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

// Esc or back from a focused field: a modal screen closes -- the palette -- and any other lets go,
// so the keys return to the app.
static void _let_go(UISystem* ui, UIScreen* top) {
    if (top->modal)
        ui_pop(ui);
    else
        top->focused = NULL;
}

// Tab and Shift+Tab: the next or previous focusable element in tree order, wrapping, and scrolled
// into view. Tree order and not the geometry, since that is the order a form is filled in.
static void _tab(const UISystem* ui, UIScreen* top, int dir) {
    UIElement* items[UI_NAV_MAX];
    size_t n = 0;
    _gather(top->root, items, &n, UI_NAV_MAX);
    if (n == 0)
        return;
    size_t at = dir > 0 ? n - 1 : 0; // nothing focused: the first forward, the last back
    for (size_t i = 0; i < n; i++)
        if (items[i] == top->focused)
            at = i;
    top->focused = items[(at + n + (size_t)(dir > 0 ? 1 : -1)) % n];
    _reveal(ui, top->focused);
}

// Page Up, Page Down, Home or End: the pane the focus is in, or with nothing focused the pane
// under the pointer, scrolled a page or to an end, easing there.
static void _scroll_key(const UISystem* ui, const UIScreen* top, const UIInput* in, int key) {
    UIElement* pane =
        top->focused ? (top->focused->kind == UI_SCROLL ? top->focused : _pane_of(top->focused))
                     : _pane_at(top->root, in->pointer_x, in->pointer_y);
    if (!pane)
        return;
    const float page = _pane_view(ui, pane) * 0.9f;
    const float to = key == GLFW_KEY_PAGE_UP     ? pane->scroll_target - page
                     : key == GLFW_KEY_PAGE_DOWN ? pane->scroll_target + page
                     : key == GLFW_KEY_HOME      ? 0.0f
                                                 : _scroll_max(ui, pane);
    _scroll_to(ui, pane, to, false);
}

/*
 * A pass's keys and characters, each in order to what holds the focus at its turn: a focused field
 * first, and what it does not use -- or every key, with anything else focused -- to the screen:
 * Tab moves the focus in tree order, Esc lets go of a field, and Page Up, Page Down, Home and End
 * scroll. The keys stop at a screen closed by one of them, being meant for it. A field the app
 * focused since the last pass takes none of them, since they came before it had the focus: the
 * key that opened a palette is not typed into it. True when Esc let go of a field.
 */
static bool _route_keys(UISystem* ui, UIScreen* top, const UIInput* in, const UIElement* app_set) {
    if (app_set && top->focused == app_set && _takes_keys(app_set))
        return false;
    UIElement* field = NULL; // the field taking keys, between its begin and its end
    bool let_go = false;
    for (int i = 0; i < in->key_event_count && ui_top(ui) == top; i++) {
        UIElement* f = _takes_keys(top->focused) ? top->focused : NULL;
        if (f != field) {
            if (field)
                ui_field_keys_end(field);
            if (f)
                ui_field_keys_begin(f);
            field = f;
        }
        const InputKeyEvent* ev = &in->key_events[i];
        if ((f && ui_field_key(ui, f, ev)) || ev->codepoint)
            continue;
        if (ev->key == GLFW_KEY_TAB) {
            _tab(ui, top, (ev->mods & GLFW_MOD_SHIFT) ? -1 : 1);
        } else if (ev->key == GLFW_KEY_ESCAPE && f) {
            _let_go(ui, top);
            let_go = true;
        } else if (ev->key == GLFW_KEY_PAGE_UP || ev->key == GLFW_KEY_PAGE_DOWN ||
                   ev->key == GLFW_KEY_HOME || ev->key == GLFW_KEY_END) {
            _scroll_key(ui, top, in, ev->key);
        }
    }
    if (field)
        ui_field_keys_end(field);
    return let_go;
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

    // A focus the app set, or one a screen arrived with, is scrolled into view as one navigated
    // to is.
    UIElement* const app_set = top->focused != ui->focus_seen ? top->focused : NULL;
    if (app_set)
        _reveal(ui, app_set);

    // A pane's bar: a press on it is a press on the bar and on nothing under it, and the thumb
    // held follows the pointer down the track.
    bool on_bar = false;
    if (in->pointer_pressed) {
        UIElement* pane = _pane_at(top->root, in->pointer_x, in->pointer_y);
        UIRect track, thumb;
        if (pane && _pane_bar(ui, pane, &track, &thumb) &&
            in->pointer_x >= pane->rect.x + pane->rect.w - UI_BAR_HIT) {
            on_bar = true;
            if (in->pointer_y >= thumb.y && in->pointer_y < thumb.y + thumb.h) {
                ui->bar_drag = pane;
                ui->bar_grab = in->pointer_y - thumb.y;
            } else {
                // A page toward the pointer, as a press on a track does everywhere.
                const float page = _pane_view(ui, pane) * 0.9f;
                _scroll_to(ui, pane, pane->scroll_target + (in->pointer_y < thumb.y ? -page : page),
                           false);
            }
        }
    }
    if (ui->bar_drag) {
        UIElement* pane = ui->bar_drag;
        UIRect track, thumb;
        if (!in->pointer_down || !_pane_bar(ui, pane, &track, &thumb)) {
            ui->bar_drag = NULL;
        } else {
            const float room = track.h - thumb.h;
            const float t = room > 0.0f ? (in->pointer_y - ui->bar_grab - track.y) / room : 0.0f;
            _scroll_to(ui, pane, _clampf(t, 0.0f, 1.0f) * _scroll_max(ui, pane), true);
        }
    }

    // The wheel scrolls the deepest pane under the pointer, at once: a trackpad's own momentum is
    // already in what it sends.
    if (in->wheel_y != 0.0f) {
        UIElement* pane = _pane_at(top->root, in->pointer_x, in->pointer_y);
        if (pane)
            _scroll_to(ui, pane, pane->scroll_target - in->wheel_y * _scroll_step(ui), true);
    }

    UIElement* hovered =
        (on_bar || ui->bar_drag) ? NULL : _hit(top->root, in->pointer_x, in->pointer_y);

    // Moving the pointer over an item highlights it, which is what a menu does
    // everywhere -- and it keeps pointer and pad on ONE highlight instead of a
    // hover colour and a focus ring disagreeing about where the player is.
    // Only on actual movement, so a resting cursor does not out-vote the stick.
    const bool moved = in->pointer_x != ui->last_pointer_x || in->pointer_y != ui->last_pointer_y;
    ui->last_pointer_x = in->pointer_x;
    ui->last_pointer_y = in->pointer_y;
    // A text field holding the focus keeps it as the pointer passes over other things, since the
    // keys are going to it; a press moves it.
    if (moved && hovered && !_takes_keys(top->focused))
        top->focused = hovered;

    // Press captures. Release over the captured element activates it; release
    // anywhere else cancels. That is what makes press-slide-off-release not
    // fire, which is the behaviour every pointer UI has and every user expects.
    if (in->pointer_pressed) {
        ui->pressed = hovered;
        if (hovered)
            top->focused = hovered;
        else if (!top->modal && !on_bar)
            top->focused = NULL; // a press on nothing lets go, where nothing holds the screen
        if (hovered && hovered->kind == UI_TEXT_FIELD)
            ui_field_press(hovered, _content_rect(ui, hovered), in->pointer_x, in->pointer_y,
                           (in->pointer_mods & GLFW_MOD_SHIFT) != 0, in->time);
    }
    // A held field selects to the pointer, from the captured element as a slider does.
    if (ui->pressed && ui->pressed->kind == UI_TEXT_FIELD && in->pointer_down &&
        !in->pointer_pressed)
        ui_field_drag(ui->pressed, _content_rect(ui, ui->pressed), in->pointer_x, in->pointer_y);
    // A held slider tracks the pointer: pressing anywhere on the row jumps the
    // value there and holding drags it, which is what every slider does. It
    // runs off the CAPTURED element rather than the hovered one, so the drag
    // survives the pointer leaving the track -- grabbing a knob and pulling
    // past the end should peg the value, not drop the grab.
    if (ui->pressed && ui->pressed->kind == UI_SLIDER && in->pointer_down &&
        ui->pressed->bound_float) {
        UIElement* sl = ui->pressed;
        const UIRect track = ui_slider_track(ui, sl);
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
        // release would fire its callback a second time for one gesture. A field's
        // action is its submit, which a click is not.
        if (ui->pressed && ui->pressed == hovered && ui->pressed->kind != UI_SLIDER &&
            ui->pressed->kind != UI_TEXT_FIELD)
            _activate(ui->pressed);
        ui->pressed = NULL;
    }

    // While a field holds the focus -- as the pointer left it -- navigation and accept are not the
    // pass's to act on: an app that maps the arrows to navigation would move the focus out of the
    // field with every caret move. Back lets go of it, as Esc does.
    const bool typing = _takes_keys(top->focused);
    const bool let_go = _route_keys(ui, top, in, app_set);
    if (ui_top(ui) != top) {
        // A key closed the screen -- a palette's Enter -- and the rest of the pass was the
        // screen's.
        ui->focus_seen = ui_top(ui) ? ui_top(ui)->focused : NULL;
        return;
    }

    if (!typing && (in->nav_up || in->nav_down || in->nav_left || in->nav_right)) {
        const int dx = (in->nav_right ? 1 : 0) - (in->nav_left ? 1 : 0);
        const int dy = (in->nav_down ? 1 : 0) - (in->nav_up ? 1 : 0);
        // Sideways on a value control adjusts it; only an unconsumed press
        // moves focus.
        if (!(dx != 0 && _adjust(top->focused, dx))) {
            UIElement* next = _nav(top->root, top->focused, dx, dy);
            if (next) {
                top->focused = next;
                _reveal(ui, next);
            }
        }
    }

    if (!typing && in->accept && top->focused)
        _activate(top->focused);
    if (in->back && !let_go) {
        if (typing)
            _let_go(ui, top);
        else
            ui_pop(ui);
    }

    // States for this frame's draw. Focus first, then hover over it, then the
    // held element, so a pressed control looks pressed rather than merely
    // hovered.
    if (top->focused && top->focused->state != UI_STATE_DISABLED)
        top->focused->state = UI_STATE_FOCUS;
    // A pane is focused while the focus is anywhere inside it: what draws its border.
    for (UIElement* p = _pane_of(top->focused); p; p = _pane_of(p))
        if (p->state != UI_STATE_DISABLED)
            p->state = UI_STATE_FOCUS;
    if (hovered && hovered->state != UI_STATE_DISABLED && hovered != top->focused)
        hovered->state = UI_STATE_HOVER;
    if (ui->pressed && in->pointer_down && ui->pressed->state != UI_STATE_DISABLED)
        ui->pressed->state = UI_STATE_ACTIVE;
    ui->focus_seen = ui_top(ui) ? ui_top(ui)->focused : NULL;
}

// How far this screen's entrance has played, 0..1. A screen with no transition
// is always 1, so every consumer reads one number and no caller branches.
static float _screen_t(const UIScreen* s) {
    if (!s || s->transition == UI_TRANSITION_NONE || s->transition_seconds <= 0.0f)
        return 1.0f;
    return s->t_in;
}

bool ui_build(UISystem* ui, float width, float height, float dt) {
    if (!ui || !ui->dl || width <= 0.0f || height <= 0.0f)
        return false;
    ui_draw_list_begin(ui->dl, (int)width, (int)height);
    ui->caret_wake = INFINITY;
    // Bottom to top: a pause screen over a HUD draws both, and the one on top
    // is simply the one emitted last -- from the highest opaque screen, which
    // hides everything under it.
    size_t from = 0;
    for (size_t i = ui->stack_count; i-- > 0;) {
        if (ui->stack[i] && ui->stack[i]->opaque) {
            from = i;
            break;
        }
    }
    bool moving = false;
    for (size_t i = from; i < ui->stack_count; i++) {
        UIScreen* s = ui->stack[i];
        if (!s || !s->root)
            continue;
        if (s->transition_seconds > 0.0f && s->t_in < 1.0f) {
            s->t_in += dt / s->transition_seconds;
            if (s->t_in > 1.0f)
                s->t_in = 1.0f;
        }
        moving |= _screen_t(s) < 1.0f;
        moving |= _ease(s->root, dt);
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
    return moving;
}

// ---------------------------------------------------------------- the system

void ui_input_add_keyboard(UIInput* in, const Engine* engine) {
    if (!in || !engine) {
        log_error("ui_input_add_keyboard: NULL input or engine");
        return;
    }
    in->wheel_y = (float)engine->input.scroll_dy;
    in->key_events = engine->input.key_events;
    in->key_event_count = engine->input.key_event_count;
    // The wall clock and not the engine's: idle mode keeps its waits out of that one, and two
    // clicks a long wait apart would read as a double click.
    in->time =
        engine->headless ? (double)engine->total_frames * ENGINE_FIXED_FRAME_DT : glfwGetTime();
}

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
    // A caret blinks on the wall clock and not the engine's, which idle mode keeps its waits out
    // of: on that one a wait for the blink would never pass. Headless it does not blink at all.
    ui->now = engine->headless ? 0.0 : glfwGetTime();
    // Something still moving, or a program reading the clock, draws something new next frame,
    // and idle mode is asked for it (spec 14.1); a caret asks for the moment it next changes.
    const bool moving = ui_build(ui, (float)engine->win_width, (float)engine->win_height, dt);
    if (ui_draw_list_render(ui->dl) || moving)
        engine_request_frame(engine);
    if (isfinite(ui->caret_wake))
        engine_request_frame_after(engine, ui->caret_wake);
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
    if (ui->attached)
        engine_remove_overlay(ui->attached, _ui_overlay, ui);
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
    if (!ui || !engine || ui->attached)
        return;
    if (engine_add_overlay(engine, _ui_overlay, ui, UI_OVERLAY_ORDER))
        ui->attached = engine;
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

UIDrawList* ui_draw_list(const UISystem* ui) {
    return ui ? ui->dl : NULL;
}

UIScreen* ui_find_screen(const UISystem* ui, const char* name) {
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

UIElement* ui_screen_root(const UIScreen* screen) {
    return screen ? screen->root : NULL;
}

void ui_screen_set_modal(UIScreen* screen, bool modal) {
    if (screen)
        screen->modal = modal;
}

void ui_screen_set_opaque(UIScreen* screen, bool opaque) {
    if (screen)
        screen->opaque = opaque;
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

bool ui_on_stack(const UISystem* ui, const UIScreen* screen) {
    if (!ui || !screen)
        return false;
    for (size_t i = 0; i < ui->stack_count; i++)
        if (ui->stack[i] == screen)
            return true;
    return false;
}

UIElement* ui_focused(const UIScreen* screen) {
    return screen ? screen->focused : NULL;
}

void ui_set_focus(UIScreen* screen, UIElement* el) {
    if (screen)
        screen->focused = el;
}

bool ui_captures_input(const UISystem* ui) {
    if (!ui)
        return false;
    for (size_t i = 0; i < ui->stack_count; i++)
        if (ui->stack[i] && ui->stack[i]->modal)
            return true;
    return false;
}
