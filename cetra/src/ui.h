#ifndef _UI_H_
#define _UI_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>

#include "input.h"
#include "text.h"
#include "texture.h"

struct Engine;
struct GlyphCache;
struct ShaderProgram;
struct TextDoc;

/*
 * The game UI layer (spec 12.2): what a player looks at that is not the world.
 *
 * SPACE. Everything here is in WINDOW POINTS with the origin at the top left
 * and +Y pointing DOWN. That is the space the text renderer's ortho is built in
 * (text.c) and the space GLFW reports the cursor in, so text metrics, hit tests
 * and the pointer all agree and nothing converts. It is deliberately NOT the
 * engine's own InputState space, which is framebuffer pixels with +Y up. Points
 * rather than pixels is what makes the layer the same physical size on a Retina
 * display as anywhere else, while still being captured at full framebuffer
 * resolution.
 *
 * WHERE IT DRAWS. As an engine overlay (engine_add_overlay), after tone mapping
 * and before the debug GUI, so nothing here is graded, bloomed, grained,
 * temporally filtered or rescaled by --render-scale. A menu's text reaches the
 * display as authored.
 *
 * This header is the DRAW half. The element tree, layout and focus model are
 * built on top of it and reach the same primitives, so anything the element
 * vocabulary refuses can still be drawn by hand from an app.
 */

// A rectangle in window points. x/y is the top-left corner.
typedef struct UIRect {
    float x, y, w, h;
} UIRect;

static inline bool ui_rect_hit(UIRect r, float px, float py) {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

static inline UIRect ui_rect_inset(UIRect r, float t, float right, float b, float l) {
    return (UIRect){r.x + l, r.y + t, r.w - l - right, r.h - t - b};
}

// Horizontal placement of a string inside the rect it is drawn into. Vertical
// placement is always the rect's top plus the font's ascent, so two strings at
// different sizes in one row share a baseline rather than a bounding box.
typedef enum { UI_ALIGN_START = 0, UI_ALIGN_CENTER = 1, UI_ALIGN_END = 2 } UIAlign;

/*
 * How one element looks. Every field's ZERO is "inherit or absent", which is
 * the CameraDesc / LightDesc convention: fill the fields you mean and leave the
 * rest alone. A zeroed UIStyle is therefore a legal style that resolves to the
 * theme's, rather than a black box with no padding -- a theme is a resolution
 * step and not a second drawing path.
 *
 * The cost of that convention, stated plainly because it is easy to meet by
 * accident: a zero cannot be an authored VALUE. There is no way to say "no
 * border on focus" or "no padding on the left" against a level below that sets
 * one, because the field that would say so is the field that means inherit.
 */
typedef struct UIStyle {
    vec4 bg;     // fill; alpha 0 = no fill drawn
    vec4 fg;     // text
    vec4 border; // alpha 0 = no border drawn
    float border_width;
    float corner_radius;
    float padding[4]; // top, right, bottom, left
    float font_size;  // 0 = inherit
    Font* font;       // NULL = inherit

    /*
     * Typography, per element and themeable like everything else here.
     *
     * `tracking` is extra advance after every glyph, in POINTS at the drawn
     * size -- positive opens a line up, negative tightens it. `line_spacing`
     * multiplies the font's own line height, so 1.0 is the face's leading and
     * 0 means the same thing (inherit, then default to 1.0).
     *
     * Kerning is NOT a field: it is the face's own pair data and is always
     * applied. There is no reason for an app to ask for badly spaced text, and
     * a switch for it is a switch somebody eventually ships turned off.
     */
    float tracking;
    float line_spacing;

    // A textured background. NULL draws the flat `bg` colour. The insets are a
    // 9-slice in texture pixels (top, right, bottom, left); all zero stretches
    // the image over the whole rect instead, which is what a plain image wants.
    //
    // `bg_tint` multiplies the image and resolves to OPAQUE WHITE, not to `bg`.
    // They were one field, and that made an untinted image unspellable: the
    // value meaning "no tint" is zero, which is also the value meaning
    // "inherit", so a style naming only bg_tex was multiplied by the theme's
    // fill and arrived at a tenth of its brightness -- or, with nothing above
    // it, by transparent black, and drew nothing at all.
    Texture* bg_tex;
    vec4 bg_tint;
    float bg_slice[4];
} UIStyle;

/*
 * A frame's accumulated geometry. One vertex format carries all three kinds of
 * thing the UI draws, so a screen of flat panels and one font is one or two
 * draws rather than one per element.
 *
 * Batching breaks on exactly three things: the bound texture, the clip
 * rectangle, and an element's own program. Nothing else splits a draw.
 */
typedef struct UIDrawList UIDrawList;

// Owns the vertex buffers and the default program. The engine is borrowed, for
// its program cache only.
UIDrawList* create_ui_draw_list(struct Engine* engine);
void free_ui_draw_list(UIDrawList* dl);

// The font a primitive uses when its style names none. Borrowed.
void ui_draw_list_set_font(UIDrawList* dl, Font* font, float size);

// Clears the frame's geometry and rebuilds the ortho for this size, in points.
void ui_draw_list_begin(UIDrawList* dl, int width_points, int height_points);
// Uploads and issues the frame's batches. Blend on, depth test off, and both
// are RESTORED to what they were rather than forced to a default -- this draws
// into a live frame between the post chain and the debug GUI, and clobbering
// the depth state there is a defect the text renderer already has. True when a
// program it drew with reads uTime, so the next frame draws something new.
bool ui_draw_list_render(UIDrawList* dl);

// Solid fill, with the style's corner radius and border applied. The rounding
// is evaluated in the fragment stage rather than tessellated, so a radius costs
// no vertices and is exact at any size.
void ui_draw_rect(UIDrawList* dl, UIRect r, const UIStyle* style);
// The same without a style: a flat colour, square corners, no border.
void ui_draw_quad(UIDrawList* dl, UIRect r, vec4 color);
// A rounded fill with an optional border, without building a UIStyle to say so.
// This is what a custom-drawn element reaches for: the furniture inside a
// control -- a switch bed, a slider track -- is a rounded rectangle and a
// colour, not a themed surface, and expressing it as a style meant assembling a
// throwaway struct per piece.
void ui_draw_rounded(UIDrawList* dl, UIRect r, float radius, vec4 fill, vec4 border,
                     float border_width);
// An image stretched over the rect, multiplied by `tint`.
void ui_draw_textured_quad(UIDrawList* dl, UIRect r, const Texture* tex, vec4 tint);
// A part of an image stretched over the rect, multiplied by `tint`: `uv` is {u0, v0, u1, v1}, v0
// at the rect's top, so a picture stored top row first is addressed as it is seen. Several
// pictures in one texture, or a view onto part of a large one, each cost a quad and no texture.
void ui_draw_textured_quad_uv(UIDrawList* dl, UIRect r, const Texture* tex, const float uv[4],
                              vec4 tint);
// An image drawn as a nine-patch: the four corners keep their size, the four
// edges stretch along one axis and the centre stretches along both. `insets`
// is top, right, bottom, left in texture pixels.
void ui_draw_9slice(UIDrawList* dl, UIRect r, const Texture* tex, const float insets[4], vec4 tint);
/*
 * A part of a label's text in its own colour and face (spec 14.2): what highlights the letters a
 * search matched, or sets a name in a monospace face inside a sentence. `start` and `length` are
 * BYTES of the UTF-8 text. A run changes the colour and the face, never the size: the line keeps
 * the element's height and baseline, so a run in another face sits in the line.
 */
typedef struct UITextRun {
    size_t start, length;
    vec4 color; // alpha 0 = the element's text colour
    Font* font; // NULL = the element's face; borrowed
} UITextRun;

// Text positioned horizontally by `align` and vertically on the font's baseline
// inside `r`, starting a new line at every '\n' it carries.
//
// It returns nothing. The advance width it used to return cost a second walk of
// the whole string to produce and no caller ever read it; `ui_text_width` is
// the way to ask, and it is what the layout pass already uses.
void ui_draw_text(UIDrawList* dl, UIRect r, const char* text, const UIStyle* style, UIAlign align);

// Shifts everything emitted after it, in points -- a quad, its rounded corners, a clip pushed and
// a program's rect alike. One number rather than an offset applied to each rect, so furniture a
// control computes internally moves with the element that owns it.
void ui_draw_list_set_offset(UIDrawList* dl, float dx, float dy);

// Multiplies the alpha of everything emitted after it. Here rather than on a
// resolved style for the same reason: a control's furniture and an app-drawn
// element pass their colours in directly, and would not fade with the screen
// that carries them.
void ui_draw_list_set_alpha(UIDrawList* dl, float alpha);

// Clipping. Nested pushes intersect, so a child can never draw outside its
// parent's clip however the rects are ordered.
void ui_push_clip(UIDrawList* dl, UIRect r);
void ui_pop_clip(UIDrawList* dl);

/*
 * A hash of every vertex the list currently holds.
 *
 * It exists so that two ways of asking for the same drawing can be shown to BE
 * the same drawing: "zero means inherit" is a resolution step rather than a
 * second drawing path only if a zeroed style and the theme's values written out
 * in full emit identical geometry. Comparing pictures would establish that only
 * to whatever tolerance the comparison used, and a colour that resolves one step
 * differently can land inside it.
 *
 * The value means nothing on its own -- it is not stable across builds or
 * layouts, and is only ever compared against another taken the same frame.
 */
uint64_t ui_draw_list_signature(const UIDrawList* dl);

// The quads the list holds, in the order they were emitted, read back: quad `i`'s corners in
// points, its colour and its kind (0 a fill, 1 an image, 2 a glyph). For a test or a tool asserting
// what a draw produced; false past the end, with the outputs untouched.
size_t ui_draw_list_quad_count(const UIDrawList* dl);
bool ui_draw_list_quad(const UIDrawList* dl, size_t i, UIRect* rect, vec4 color, int* kind);

// An element's own fragment program for the next primitives emitted; NULL
// returns to the default. The uniform contract a replacement is written
// against is declared in ui_frag.glsl: uRect, uResolution, uTime, uFocus, uTex.
void ui_set_draw_program(UIDrawList* dl, struct ShaderProgram* program, UIRect rect, float focus);

/*
 * Measurement, so layout can ask what a string costs without drawing it.
 *
 * These take `tracking` for the same reason ui_draw_text does: measuring and
 * drawing MUST apply identical kerning and tracking, or a centred string is
 * centred against a width it does not have and a hit test lands beside the
 * glyph the player aimed at. One advance rule, used by both.
 */
float ui_text_width(const Font* font, float size, float tracking, const char* text);
float ui_line_height(const Font* font, float size, float line_spacing);
// The byte offset at which `text` exceeds `max_width`, breaking on the last
// word boundary at or before it. Returns the length when the whole string fits,
// so a caller loops until it consumes the string.
size_t ui_text_wrap_point(const Font* font, float size, float tracking, const char* text,
                          float max_width);

/*
 * ===========================================================================
 * The element tree.
 *
 * THE ELEMENT LIST IS CLOSED: panel, label, button, toggle, slider, selector,
 * and since spec 14.2, which argued for them, the scrolling pane and the text
 * field. That is not a stage the layer is passing through on its way to a widget
 * toolkit -- it is the property that keeps this a UI layer instead of a worse
 * Dear ImGui. Another kind is a later spec with its own argument, never a patch
 * to this one.
 *
 * What that costs is nothing, because three escape hatches sit under it and
 * each keeps strictly more than the last: ui_set_draw keeps layout, focus and
 * input while the app paints; ui_set_element_program keeps all of that and
 * swaps only the fragment stage; and the draw-list primitives above take
 * neither, for anything the vocabulary refuses outright.
 * ===========================================================================
 */

typedef enum { UI_ROW = 0, UI_COLUMN = 1 } UIDir;

// How an axis is sized. FIT hugs the content, GROW shares what the parent has
// left over, FIXED is the authored number. Two passes settle them: measure
// bottom-up for FIT, arrange top-down for GROW.
typedef enum { UI_FIT = 0, UI_FIXED = 1, UI_GROW = 2 } UISize;

/*
 * UI_ROOT is a container and not an element of the list: it has no constructor, an
 * app cannot make one, and `ui-elements-closed` counts constructors rather than
 * enum entries. It exists because a screen's root must have NO look at all --
 * zero padding, no background, no corner radius -- and borrowing UI_PANEL's
 * styling meant cancelling it afterwards, which a zero cannot express while a
 * zero means "inherit". A kind whose every default is already zero says it
 * once, in the place that decides it.
 */
typedef enum {
    UI_ROOT = 0,
    UI_PANEL,
    UI_LABEL,
    UI_BUTTON,
    UI_TOGGLE,
    UI_SLIDER,
    UI_SELECTOR,
    UI_SCROLL,
    UI_TEXT_FIELD,
    UI_EDITOR,
    UI_KIND_COUNT
} UIKind;

// How the code editor draws a span its text's highlighter coloured, or a range its host marked: the
// text's colour, a fill behind it and a line under it, each alpha 0 for none -- the text's own
// colour, then, for `text`. Style 0 is plain text.
typedef struct UIEditorStyle {
    vec4 text;
    vec4 background;
    vec4 underline;
} UIEditorStyle;

#define UI_EDITOR_STYLES 32

typedef enum {
    UI_STATE_NORMAL = 0,
    UI_STATE_HOVER,
    UI_STATE_FOCUS,
    UI_STATE_ACTIVE,
    UI_STATE_DISABLED,
    UI_STATE_COUNT
} UIState;

/*
 * The look of a whole screen. Every UIStyle in here follows the zero-means-
 * inherit rule above, so a theme filled in partway is legal and the rest comes
 * from the engine default -- which is what makes `ui-theme-identity` assertable:
 * a zeroed style must produce the same vertices as the resolved one spelled out
 * in full, or "zero is the default" is really a second drawing path.
 */
typedef struct UITheme {
    Font* font;
    float font_size;
    float spacing; // gap between siblings, when a container names none
    // No entry for UI_ROOT: a screen root draws nothing, so a style for it
    // would be a field that cannot have an effect.
    UIStyle panel;
    UIStyle label;
    UIStyle button[UI_STATE_COUNT];
    UIStyle toggle[UI_STATE_COUNT];
    UIStyle slider[UI_STATE_COUNT];
    UIStyle selector[UI_STATE_COUNT];
    // A scrolling pane: `bg` behind its content, `fg` its bar's thumb, and FOCUS -- which a pane
    // takes while the focus is anywhere inside it -- its border, the subtle one round the pane
    // the keys are in.
    UIStyle scroll[UI_STATE_COUNT];
    float scroll_step; // points a wheel unit scrolls; 0 = the theme's font size
    // A text field: `fg` its text and caret, its placeholder `fg` dimmed; FOCUS its border while
    // it takes the keys.
    UIStyle text_field[UI_STATE_COUNT];
    vec4 selection; // behind a field's selected text; alpha 0 = the engine's
    // The code editor (spec 14.4): `bg` behind its text, `fg` its plain text and carets, its line
    // numbers `fg` dimmed; FOCUS its border while it takes the keys. Its face is drawn from
    // bitmaps made at the size shown, not from `font`.
    UIStyle editor[UI_STATE_COUNT];
    struct GlyphCache* editor_face;                // borrowed; NULL draws no text
    float editor_size;                             // points; 0 = 13
    UIEditorStyle editor_styles[UI_EDITOR_STYLES]; // a span's or a range's style, by its index
} UITheme;

typedef struct UIElement UIElement;
typedef struct UIScreen UIScreen;
typedef struct UISystem UISystem;

// Fired after the value has already been written through the bound pointer, so
// a handler reads the new value rather than being told what it will become.
typedef void (*UIActionFn)(UIElement* el, void* user);
typedef void (*UIDrawFn)(UIElement* el, UIDrawList* dl, void* user);

struct UIElement {
    // ENGINE-OWNED: the layout pass and the input pass write these; read them,
    // never write them.
    UIKind kind;
    UIRect rect;   // settled by ui_layout, in points
    UIState state; // settled by the input pass
    float t_hover; // 0..1, eased toward whether the pointer is inside
    float t_focus; // 0..1, eased toward whether this element has focus
    // 0..1, eased toward the element's own VALUE -- a toggle's bound bool. The
    // value itself flips instantly, because a handler reading a half-flipped
    // bool would be a bug; this is only what the drawing interpolates, so a
    // switch travels instead of teleporting.
    float t_value;
    UIElement* parent;
    UIElement** children;
    size_t child_count, child_capacity;
    // An element's text broken into lines by its last layout, and its text's measures, kept until
    // its text, its runs or its style changes -- and for a wrapping label's lines, its width.
    struct UILines* lines;
    unsigned wraps;    // how many times the layout has broken them: a test of the keeping
    unsigned measures; // how many times the layout has walked its text to measure it: the same
    // A scrolling pane's offset down its content, in points: `scroll` is what is drawn, eased
    // toward `scroll_target` after a key or a reveal and set with it by the wheel and the bar.
    // `scroll_content` is its children's whole height, from the last layout.
    float scroll, scroll_target, scroll_content;
    struct UIField* field;   // a text field's buffer, caret and editing; ui_field_* reads it
    struct UIEditor* editor; // a code editor's carets, rows and scroll; ui_editor_* reads it
    // A pane's virtual rows (ui_scroll_set_rows): their heights and where each starts, and which
    // child holds which row. `row_builds` counts the rows built, a test of the building.
    struct UIRows* rows;
    unsigned row_builds;

    // BY FUNCTION
    char* text;                    // ui_set_text (owned)
    UITextRun* runs;               // ui_set_text_runs (owned copy)
    size_t run_count;              //   "
    UIStyle* style;                // ui_set_style (owned copy); NULL = the theme's
    UIDrawFn draw;                 // ui_set_draw
    void* draw_user;               //   "
    struct ShaderProgram* program; // ui_set_element_program

    // SETTINGS: plain writes at any time.
    UIDir dir;           // how this element's CHILDREN are laid out
    UISize size_mode[2]; // x, y
    float size[2];       // the number FIXED uses
    // The most a FIT or GROW element measures along each axis; 0 = no most. A scrolling pane past
    // it scrolls; anything else draws past it.
    float max_size[2];
    // An element's two inheritable lengths: 0 takes the style's or the theme's, and a negative one
    // is none. The padding is top, right, bottom, left, and taken whole: all four 0 is the style's,
    // and otherwise each side is its own, a 0 side none as a negative one is.
    float padding[4];
    float spacing; // the gap between children
    UIAlign align_main, align_cross;
    bool focusable;
    bool disabled;
    bool fill; // ignore layout and cover the whole screen (a backdrop)
    // A label's text breaks into lines at the width its parent gives it. Its own width measures
    // as its longest word, so it is given one: grown, fixed, or stretched across a column.
    bool wrap;
    // A scrolling pane at its end stays at its end as its content grows, as a terminal does; one
    // scrolled up stays where it was.
    bool follow_end;
    int max_rows; // the most rows a multi-line field grows to before it scrolls; 0 = no most

    // What a control writes through. Borrowed, and exactly one is non-NULL for
    // the kind that uses it: the control is a VIEW of the app's own variable,
    // not a copy the app has to read back.
    bool* bound_bool;
    float* bound_float;
    int* bound_int;
    float range_lo, range_hi;   // slider
    const char* const* options; // selector (borrowed)
    int option_count;           //   "
    UIActionFn action;
    void* action_user;
};

// The order ui_attach adds the system's overlay at: over anything an app adds
// at a lower order, such as a 2D canvas.
#define UI_OVERLAY_ORDER 100

// The system owns the screens, the theme, the font and the draw list. Freeing
// it takes it off the engine ui_attach added it to.
UISystem* create_ui_system(struct Engine* engine);
void free_ui_system(UISystem* ui);
// Adds the system as an overlay at UI_OVERLAY_ORDER, so it draws itself every
// frame; once only.
void ui_attach(UISystem* ui, struct Engine* engine);
void ui_set_font(UISystem* ui, Font* font, float size);
void ui_set_theme(UISystem* ui, const UITheme* theme); // copied
const UITheme* ui_theme(const UISystem* ui);
UIDrawList* ui_draw_list(const UISystem* ui);

// A screen is full-screen by construction; there are no floating windows, which
// is most of what this layer does not have to implement.
UIScreen* ui_screen(UISystem* ui, const char* name);
// The screen created under this name, or NULL. It exists because the name was
// otherwise stored, freed and read by nothing, while an app that wanted to open
// a screen BY name -- which is what a `--ui-screen` flag or a save file has --
// had to keep its own chain of string compares beside its own handles, and
// extend it for every screen added.
UIScreen* ui_find_screen(const UISystem* ui, const char* name);
UIElement* ui_screen_root(const UIScreen* screen);
// A modal screen consumes input: the game reads zero from every non-ui action
// while one is on the stack. A non-modal screen (a HUD) draws and takes nothing.
void ui_screen_set_modal(UIScreen* screen, bool modal);
// An opaque screen hides every screen under it while it is on the stack: a menu that takes the
// place of the one it was opened from, which keeps its focus for when it is back on top. The
// world behind is still drawn; only the UI under it is not.
void ui_screen_set_opaque(UIScreen* screen, bool opaque);

/*
 * How a screen arrives. The transition is the SCREEN's, not the stack's, so a
 * pause menu can slide while a settings screen over it fades, and it runs on
 * the wall clock rather than the sim's -- a menu that stopped animating because
 * it had paused the game would be animating for exactly nobody.
 *
 * Zero seconds, or UI_TRANSITION_NONE, is the whole off state: the screen is
 * simply there on the frame it is pushed.
 */
typedef enum {
    UI_TRANSITION_NONE = 0,
    UI_TRANSITION_FADE,  // alpha 0 -> 1
    UI_TRANSITION_SLIDE, // up from below, fading as it comes
} UITransition;

void ui_screen_transition(UIScreen* screen, UITransition kind, float seconds);

void ui_push(UISystem* ui, UIScreen* screen);
void ui_pop(UISystem* ui);
void ui_pop_all(UISystem* ui);
UIScreen* ui_top(const UISystem* ui);
// Whether `screen` is on the stack at all, on top or under others.
bool ui_on_stack(const UISystem* ui, const UIScreen* screen);

// The element of `screen` the keyboard and pad are on, or NULL before the first press.
UIElement* ui_focused(const UIScreen* screen);
// Put them on `el`, an element of `screen`, as though navigated there: a menu that opens on its
// first item. NULL leaves nothing focused, where the first press lands on the first element.
void ui_set_focus(UIScreen* screen, UIElement* el);
// True while a modal screen is on the stack -- what the game gates its own
// input on, and what input_set_suppressed is driven from.
bool ui_captures_input(const UISystem* ui);

// The elements. Each appends to `parent` and returns the new element.
UIElement* ui_panel(UIElement* parent);
UIElement* ui_label(UIElement* parent, const char* text);
UIElement* ui_button(UIElement* parent, const char* text, UIActionFn action, void* user);
UIElement* ui_toggle(UIElement* parent, const char* text, bool* bound, UIActionFn action,
                     void* user);
UIElement* ui_slider(UIElement* parent, const char* text, float lo, float hi, float* bound,
                     UIActionFn action, void* user);
UIElement* ui_selector(UIElement* parent, const char* text, const char* const* options, int count,
                       int* bound, UIActionFn action, void* user);
// Where a slider's track lies in the rect the last layout gave it, in window points: what a drag
// is measured along, for a draw of its own (ui_set_draw) to put the track there too.
UIRect ui_slider_track(const UISystem* ui, const UIElement* el);

/*
 * A SCROLLING PANE (spec 14.2): a column whose children are laid out at their whole height and
 * seen through the pane's rect, moved by the wheel over it, by Page Up, Page Down, Home and End
 * while the focus is in it, by dragging its bar, and by navigation, which scrolls a focused child
 * into view. Vertical only. Give it a height -- grown or fixed -- since a pane that fits its
 * content has nothing to scroll.
 */
UIElement* ui_scroll(UIElement* parent);
// Scrolls every pane round `el` by the least that shows it whole, easing there.
void ui_scroll_into_view(const UISystem* ui, UIElement* el);

/*
 * VIRTUAL ROWS, for a history too long to lay out whole: the pane's content becomes `count` rows
 * that `build` fills on demand -- each into a container the pane makes, with no look of its own,
 * stretched across the pane -- and only the rows in view are built, with one either side and the
 * one holding the focus. A row's height is measured when it is first built and kept; one never
 * built counts at `estimate` points. A height changing above the view moves the scroll with it,
 * so the first row in view stays put. Calling it again starts the rows over, and frees whatever
 * the pane held.
 */
typedef void (*UIRowFn)(UIElement* row, int index, void* user);
void ui_scroll_set_rows(UIElement* pane, int count, float estimate, UIRowFn build, void* user);
// Grows or shrinks the rows at their end, keeping every height measured: a message appended.
void ui_scroll_set_row_count(UIElement* pane, int count);
// Rows `first` to `first + count` change: those built are built again at the next layout, and
// measured again.
void ui_scroll_rows_changed(UIElement* pane, int first, int count);
// Scrolls the pane by the least that shows row `index`, easing there.
void ui_scroll_to_row(const UISystem* ui, UIElement* pane, int index);
// Row `index`'s container while it is built, NULL while it is not: what an app changes in place,
// a message growing, rather than having the row built again. Valid until the next layout, which
// frees a row ui_scroll_rows_changed marked.
UIElement* ui_scroll_find_row(const UIElement* pane, int index);
// Whether the point, in window points, is over anything a screen on the stack draws as of its last
// layout -- a pane's children only where the pane shows them -- or a modal screen is up: what an
// app asks before it takes the wheel or a drag for itself.
bool ui_pointer_over(const UISystem* ui, float x, float y);

/*
 * A TEXT FIELD (spec 14.2): a caret, a selection, the clipboard and undo over a UTF-8 buffer,
 * driven by the keys and characters UIInput carries and by the pointer. It takes the keys while it
 * holds the focus: arrows move its caret rather than the focus, Enter submits, Esc lets go -- or
 * closes a modal screen -- and Tab moves the focus on, the keys after it in the pass going where
 * it put the focus. A field the app focuses takes none of the next pass's keys, which came before
 * it had the focus, so an app may focus one from its own key callback and the key that asked is
 * not typed into it. Its element's `text` is not used: the text is ui_field_set_text's, and the dim
 * hint shown while it is empty ui_field_set_placeholder's. Fixed at 240 points wide by default,
 * as a slider is.
 */
enum {
    UI_FIELD_MULTILINE = 1,       // wraps, growing to `max_rows` rows, if any, then scrolls inside
    UI_FIELD_READ_ONLY = 2,       // selected and copied, never edited; may carry styled runs
    UI_FIELD_SUBMIT_ON_ENTER = 4, // multi-line: Enter submits and Shift+Enter starts a line
};
// A key the focused field sees before it acts on it; true takes it.
typedef bool (*UIKeyFn)(UIElement* el, const InputKeyEvent* key, void* user);

UIElement* ui_text_field(UIElement* parent, unsigned flags, UIActionFn on_submit, void* user);
// The field's text, UTF-8; valid until it next changes. "" for anything not a field.
const char* ui_field_text(const UIElement* el);
// Replaces the text -- no on_change, since the app did it -- with the caret at its end, its undo
// forgotten and its runs cleared.
void ui_field_set_text(UIElement* el, const char* text);
// The dim hint shown while the field is empty: "Type a command". Copied; NULL for none.
void ui_field_set_placeholder(UIElement* el, const char* text);
// Adds `text` to the end, with styled runs of its own counted from its start: a reply streamed
// into a message. The caret, the selection, the scroll and undo stay, there is no on_change, and
// only the last line is broken again. Refused, and nothing added, when the field would pass its
// most (1 MiB), or for runs that do not fit `text` or are given to an editable field.
bool ui_field_append(UIElement* el, const char* text, const UITextRun* runs, size_t run_count);
// Once after a pass whose keys edited the text.
void ui_field_set_on_change(UIElement* el, UIActionFn on_change, void* user);
void ui_field_set_on_key(UIElement* el, UIKeyFn on_key, void* user);

/*
 * The code editor (spec 14.4): a TextDoc shown and edited, its lines wrapped at its width, in the
 * theme's monospaced face. It borrows the text, which must outlive it, and two editors may show
 * one. It measures nothing, so it is sized FIXED or GROW, and grows by default. Its carets, keys
 * and modes are ui_editor_*, in ui_editor.h.
 */
UIElement* ui_editor(UIElement* parent, struct TextDoc* doc);
// Whether the UI takes the keyboard: a text field or an editor holds the top screen's focus, or a
// modal screen is up. An app's own keys -- a view's bindings -- go where this is false.
bool ui_wants_keyboard(const UISystem* ui);

// Replaces the text, and clears its runs: they name bytes of the text they were given for. Refused
// by name on a text field, whose text is ui_field_set_text and whose hint is
// ui_field_set_placeholder.
void ui_set_text(UIElement* el, const char* text);
// A label's or a read-only field's styled runs, copied; NULL or none clears them. Refused by name,
// and the runs left as they were, when they are out of order, overlap, reach past the text or start
// or end inside a UTF-8 sequence, or the element is neither.
bool ui_set_text_runs(UIElement* el, const UITextRun* runs, size_t count);
void ui_set_style(UIElement* el, const UIStyle* style);
void ui_set_draw(UIElement* el, UIDrawFn draw, void* user);
void ui_set_element_program(UIElement* el, struct ShaderProgram* program);
// Convenience for the two size axes, since every element sets them together.
void ui_set_size(UIElement* el, UISize x_mode, float x, UISize y_mode, float y);

/*
 * Settles every element's rect for a screen at this size, in points. No GL, no
 * clock and no input. A pane of virtual rows builds and frees its rows here, from
 * where the last layout left its scroll, and lays out again on what it built; a
 * screen with none allocates nothing beyond the lines its labels keep.
 *
 * A menu's geometry is a function of a tree and a size, so keeping it free of the
 * GPU and the clock is what lets it be checked numerically with no window at all.
 */
void ui_layout(UIScreen* screen, float width, float height);

// Advances the hover/focus easing and emits the stack's geometry into the
// system's draw list. Reads no input. True while anything is still moving --
// an ease short of its target, a screen still entering -- so the next frame
// draws something new.
//
// It is NOT what ui_update calls, and ui_update is not this plus an input pass:
// the two are separate walks fired from different frame hooks -- ui_update from
// the frame-input hook before the fixed steps, this one from the overlay after
// tone mapping -- and each lays out what it is about to use.
bool ui_build(UISystem* ui, float width, float height, float dt);

/*
 * One frame of input, as VALUES rather than as a device.
 *
 * Deliberately not a GameInputState: this layer is part of the engine and the
 * game framework is optional, so taking a game type here would make a menu
 * impossible without the game loop and would point the dependency backwards.
 * It also means the whole input pass is a pure function of a struct, so the
 * gate arms drive navigation and activation with no window, no GPU and no
 * Game at all.
 *
 * The caller supplies EDGES, already computed, because it is the caller that
 * knows what a press means -- a key, a pad button, or an action that is both.
 * Positions are in window points, the space everything else here uses.
 */
typedef struct UIInput {
    float pointer_x, pointer_y;
    bool pointer_down;     // held this frame
    bool pointer_pressed;  // went down this frame
    bool pointer_released; // came up this frame
    int pointer_mods;      // GLFW's modifier bits held at the press: Shift extends a selection

    bool nav_up, nav_down, nav_left, nav_right; // edges
    bool accept;                                // edge: activate the focused element
    bool back;                                  // edge: leave the top screen

    // What a desktop keyboard and wheel add (spec 14.2); zero from an app that has neither, which
    // is every game.
    float wheel_y;                   // the wheel this frame, in its own units; + is away
    const InputKeyEvent* key_events; // typed this frame, in order; borrowed for the call
    int key_event_count;             //
    double time;                     // seconds, any monotonic clock: double clicks measure on it
} UIInput;

// Fills `in` with what the engine read this frame: the pointer, in window points, with its left
// button's edges and the modifiers held at its press; the wheel and the typing the poll delivered;
// and the time, the wall clock in a window and the frame count headless, so a headless double
// click is the same on every run. The navigation, accept and back are left as they were: they are
// the app's, from its own bindings, before or after this.
void ui_input_read(UIInput* in, const struct Engine* engine);

/*
 * The INPUT pass. Lay the top screen out, hit-test the pointer, move focus,
 * activate, fire callbacks, and settle whether the UI is holding input.
 *
 * WHERE THIS GOES MATTERS, and getting it wrong costs a frame of input in each
 * direction. It belongs immediately after the frame's input poll and BEFORE the
 * game's fixed steps -- which under the game framework is the frame-input hook,
 * game_set_frame_input, and NOT pre-render or the update hook. A game reads
 * its actions during those steps, so a UI that opened a menu after them would
 * let the frame that opened it also walk the character, and the frame that
 * closed it would drop a step of real input.
 *
 * Layout runs here rather than at draw time for the same reason: a hit test
 * against last frame's rectangles lands a click where an element used to be.
 */
void ui_update(UISystem* ui, const UIInput* in, float width, float height);

#endif // _UI_H_
