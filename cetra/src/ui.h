#ifndef _UI_H_
#define _UI_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>

#include "text.h"
#include "texture.h"

struct Engine;
struct ShaderProgram;

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
 * WHERE IT DRAWS. Through engine_set_overlay, after tone mapping and before the
 * debug GUI, so nothing here is graded, bloomed, grained, temporally filtered
 * or rescaled by --render-scale. A menu's text reaches the display as authored.
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
// the depth state there is a defect the text renderer already has.
void ui_draw_list_render(UIDrawList* dl);

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
// An image drawn as a nine-patch: the four corners keep their size, the four
// edges stretch along one axis and the centre stretches along both. `insets`
// is top, right, bottom, left in texture pixels.
void ui_draw_9slice(UIDrawList* dl, UIRect r, const Texture* tex, const float insets[4], vec4 tint);
// One line of text, positioned horizontally by `align` and vertically on the
// font's baseline inside `r`. Returns the advance width actually drawn.
float ui_draw_text(UIDrawList* dl, UIRect r, const char* text, const UIStyle* style, UIAlign align);

// Shifts everything emitted after it, in points. One number rather than an
// offset applied to each rect, so furniture a control computes internally moves
// with the element that owns it.
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
 *
 * The font is non-const because a glyph is baked into the atlas on first use:
 * measuring a string is what CAUSES its glyphs to exist.
 */
float ui_text_width(Font* font, float size, float tracking, const char* text);
float ui_line_height(const Font* font, float size, float line_spacing);
// The byte offset at which `text` exceeds `max_width`, breaking on the last
// word boundary at or before it. Returns the length when the whole string fits,
// so a caller loops until it consumes the string.
size_t ui_text_wrap_point(Font* font, float size, float tracking, const char* text,
                          float max_width);

/*
 * ===========================================================================
 * The element tree.
 *
 * THE ELEMENT LIST IS CLOSED: panel, label, button, toggle, slider, selector.
 * That is not a stage the layer is passing through on its way to a widget
 * toolkit -- it is the property that keeps this a menu layer instead of a worse
 * Dear ImGui. A seventh kind is a later spec with its own argument, never a
 * patch to this one.
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
 * UI_ROOT is a container and not a seventh element: it has no constructor, an
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
    UI_KIND_COUNT
} UIKind;

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

    // BY FUNCTION
    char* text;                    // ui_set_text (owned)
    UIStyle* style;                // ui_set_style (owned copy); NULL = the theme's
    UIDrawFn draw;                 // ui_set_draw
    void* draw_user;               //   "
    struct ShaderProgram* program; // ui_set_element_program

    // SETTINGS: plain writes at any time.
    UIDir dir;           // how this element's CHILDREN are laid out
    UISize size_mode[2]; // x, y
    float size[2];       // the number FIXED uses
    float padding[4];    // top, right, bottom, left; 0 = the style's
    float spacing;       // gap between children; 0 = the theme's
    UIAlign align_main, align_cross;
    bool focusable;
    bool disabled;
    bool fill; // ignore layout and cover the whole screen (a backdrop)

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

// The system owns the screens, the theme, the font and the draw list.
UISystem* create_ui_system(struct Engine* engine);
void free_ui_system(UISystem* ui);
// Installs the overlay hook, so the system draws itself every frame.
void ui_attach(UISystem* ui, struct Engine* engine);
void ui_set_font(UISystem* ui, Font* font, float size);
void ui_set_theme(UISystem* ui, const UITheme* theme); // copied
const UITheme* ui_theme(const UISystem* ui);
UIDrawList* ui_draw_list(UISystem* ui);

// A screen is full-screen by construction; there are no floating windows, which
// is most of what this layer does not have to implement.
UIScreen* ui_screen(UISystem* ui, const char* name);
UIElement* ui_screen_root(UIScreen* screen);
// A modal screen consumes input: the game reads zero from every non-ui action
// while one is on the stack. A non-modal screen (a HUD) draws and takes nothing.
void ui_screen_set_modal(UIScreen* screen, bool modal);

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
// True while a modal screen is on the stack -- what the game gates its own
// input on, and what input_set_suppressed is driven from.
bool ui_captures_input(const UISystem* ui);

// The six. Each appends to `parent` and returns the new element.
UIElement* ui_panel(UIElement* parent);
UIElement* ui_label(UIElement* parent, const char* text);
UIElement* ui_button(UIElement* parent, const char* text, UIActionFn action, void* user);
UIElement* ui_toggle(UIElement* parent, const char* text, bool* bound, UIActionFn action,
                     void* user);
UIElement* ui_slider(UIElement* parent, const char* text, float lo, float hi, float* bound,
                     UIActionFn action, void* user);
UIElement* ui_selector(UIElement* parent, const char* text, const char* const* options, int count,
                       int* bound, UIActionFn action, void* user);

void ui_set_text(UIElement* el, const char* text);
void ui_set_style(UIElement* el, const UIStyle* style);
void ui_set_draw(UIElement* el, UIDrawFn draw, void* user);
void ui_set_element_program(UIElement* el, struct ShaderProgram* program);
// Convenience for the two size axes, since every element sets them together.
void ui_set_size(UIElement* el, UISize x_mode, float x, UISize y_mode, float y);

/*
 * Settles every element's rect for a screen at this size, in points. PURE: no
 * GL, no clock, no input, no allocation beyond the tree that already exists.
 *
 * A menu's geometry is a function of a tree and a size, so keeping it pure is
 * what lets it be checked numerically with no window and no GPU at all.
 */
void ui_layout(UIScreen* screen, float width, float height);

// Advances the hover/focus easing and emits the stack's geometry into the
// system's draw list. Reads no input; ui_update is this plus the input pass.
void ui_build(UISystem* ui, float width, float height, float dt);

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

    bool nav_up, nav_down, nav_left, nav_right; // edges
    bool accept;                                // edge: activate the focused element
    bool back;                                  // edge: leave the top screen
} UIInput;

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
