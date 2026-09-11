#ifndef _UI_H_
#define _UI_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>

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
 * theme's, rather than a black box with no padding -- which is what makes a
 * theme a resolution step and not a second drawing path.
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

    // A textured background. NULL draws the flat `bg` colour. The insets are a
    // 9-slice in texture pixels (top, right, bottom, left); all zero stretches
    // the image over the whole rect instead, which is what a plain image wants.
    Texture* bg_tex;
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
// An image stretched over the rect, multiplied by `tint`.
void ui_draw_textured_quad(UIDrawList* dl, UIRect r, const Texture* tex, vec4 tint);
// An image drawn as a nine-patch: the four corners keep their size, the four
// edges stretch along one axis and the centre stretches along both. `insets`
// is top, right, bottom, left in texture pixels.
void ui_draw_9slice(UIDrawList* dl, UIRect r, const Texture* tex, const float insets[4], vec4 tint);
// One line of text, positioned horizontally by `align` and vertically on the
// font's baseline inside `r`. Returns the advance width actually drawn.
float ui_draw_text(UIDrawList* dl, UIRect r, const char* text, const UIStyle* style, UIAlign align);

// Clipping. Nested pushes intersect, so a child can never draw outside its
// parent's clip however the rects are ordered.
void ui_push_clip(UIDrawList* dl, UIRect r);
void ui_pop_clip(UIDrawList* dl);

// An element's own fragment program for the next primitives emitted; NULL
// returns to the default. The uniform contract a replacement is written
// against is declared in ui_frag.glsl: uRect, uResolution, uTime, uFocus, uTex.
void ui_set_draw_program(UIDrawList* dl, struct ShaderProgram* program, UIRect rect, float focus);

// Measurement, so layout can ask what a string costs without drawing it.
// The font is non-const because a glyph is baked into the atlas on first use:
// measuring a string is what CAUSES its glyphs to exist.
float ui_text_width(Font* font, float size, const char* text);
float ui_line_height(const Font* font, float size);
// The byte offset at which `text` exceeds `max_width`, breaking on the last
// word boundary at or before it. Returns the length when the whole string fits,
// so a caller loops until it consumes the string.
size_t ui_text_wrap_point(Font* font, float size, const char* text, float max_width);

#endif // _UI_H_
