#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "ext/log.h"
#include "program.h"
#include "ui.h"
#include "uniform.h"
#include "util.h"

// Nesting depth for clip rectangles. A menu nests a screen inside a panel
// inside a scroll region and stops; this is far past that, and a fixed array
// keeps a push that would overflow a refusal rather than a reallocation in the
// middle of a frame's geometry.
#define UI_CLIP_MAX 16

#define UI_MODE_FILL    0.0f
#define UI_MODE_TEXTURE 1.0f
#define UI_MODE_GLYPH   2.0f

/*
 * One vertex format for every kind of thing the UI draws. Fills, images and
 * glyphs differ by the `mode` field rather than by buffer, which is what lets a
 * whole screen batch into one or two draws: a menu's panels and its text share
 * a buffer and split only where the bound texture changes.
 *
 * The rect and corner radius ride the VERTEX rather than a uniform so that many
 * rounded quads of different radii stay in one draw. That costs 80 bytes a
 * vertex against a uniform's zero, and buys the batching the uniform would have
 * destroyed -- a menu is hundreds of quads, not hundreds of thousands.
 */
typedef struct UIVertex {
    float x, y;
    float u, v;
    float color[4];
    float rect[4];   // min.xy, max.xy -- the quad's own bounds, for the corner SDF
    float params[4]; // corner radius, border width, mode, unused
    float border[4];
} UIVertex;

// A run of indices sharing everything the GL needs set before a draw. Anything
// that changes one of these four starts a new batch, and nothing else does.
typedef struct UIBatch {
    GLuint texture;
    ShaderProgram* program; // NULL = the default UI program
    bool clipped;
    UIRect clip;
    UIRect rect; // uRect, for a custom program
    float focus; // uFocus, likewise
    size_t first_index;
    size_t index_count;
} UIBatch;

struct UIDrawList {
    Engine* engine;         // borrowed: the program cache and the framebuffer size
    ShaderProgram* program; // the default program, owned by the engine's cache

    UIVertex* verts;
    size_t vcount, vcap;
    unsigned int* idx;
    size_t icount, icap;
    UIBatch* batches;
    size_t bcount, bcap;

    GLuint vao, vbo, ebo;

    // 1x1 opaque white. An untextured quad samples this rather than branching
    // in the shader, so every batch binds a texture and the bind is never
    // conditional.
    GLuint white;

    Font* font; // default, when a style names none
    float font_size;

    UIRect clip[UI_CLIP_MAX];
    int clip_depth;
    // Pushes that arrived past the depth limit. Counted rather than dropped, so
    // the matching pops consume these before they touch the real stack -- a
    // refused push that a pop still unwinds pops a level nobody pushed and
    // hands the rest of the frame its grandparent's clip.
    int clip_refused;

    ShaderProgram* cur_program;
    UIRect cur_rect;
    float cur_focus;

    int width, height; // points
    mat4 ortho;
    float time;

    // Added to every vertex as it is emitted. A screen sliding in moves ONE
    // number rather than an offset threaded through each element's rect --
    // which would have missed the furniture, since a slider's track is computed
    // from the element's own rect inside the draw rather than passed in.
    float offset_x, offset_y;

    // Multiplied into every vertex's alpha, for the same reason and with the
    // same reach. Applying it to the RESOLVED STYLE instead missed everything
    // that does not come from a style: a toggle's knob, a slider's fill and any
    // app-drawn element pass their colours to ui_draw_rounded as literals, so a
    // fading screen faded its surfaces while its furniture stayed solid.
    float alpha;
};

// ---------------------------------------------------------------- allocation

// grow_array, not a hand-rolled doubling loop. util.h's own comment records
// that it exists because there were four such loops with three different seed
// capacities and two overflow policies, none of which agreed and none of which
// reported -- and it carries the SIZE_MAX guards none of the copies had.
static bool _reserve_verts(UIDrawList* dl, size_t extra) {
    return grow_array((void**)&dl->verts, &dl->vcap, dl->vcount + extra, sizeof(UIVertex), 512);
}

static bool _reserve_idx(UIDrawList* dl, size_t extra) {
    return grow_array((void**)&dl->idx, &dl->icap, dl->icount + extra, sizeof(unsigned int), 768);
}

// ------------------------------------------------------------------ batching

static UIRect _active_clip(const UIDrawList* dl, bool* clipped) {
    if (dl->clip_depth <= 0) {
        *clipped = false;
        return (UIRect){0, 0, (float)dl->width, (float)dl->height};
    }
    *clipped = true;
    return dl->clip[dl->clip_depth - 1];
}

static bool _same_rect(UIRect a, UIRect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// The batch the next primitive belongs in, opening a new one when any of the
// four batch-breaking facts differs from the open one.
static UIBatch* _batch_for(UIDrawList* dl, GLuint texture) {
    bool clipped = false;
    const UIRect clip = _active_clip(dl, &clipped);

    if (dl->bcount > 0) {
        UIBatch* b = &dl->batches[dl->bcount - 1];
        if (b->texture == texture && b->program == dl->cur_program && b->clipped == clipped &&
            (!clipped || _same_rect(b->clip, clip)) &&
            (dl->cur_program == NULL ||
             (_same_rect(b->rect, dl->cur_rect) && b->focus == dl->cur_focus)))
            return b;
    }

    if (!grow_array((void**)&dl->batches, &dl->bcap, dl->bcount + 1, sizeof(UIBatch), 32))
        return NULL;

    UIBatch* b = &dl->batches[dl->bcount++];
    *b = (UIBatch){.texture = texture,
                   .program = dl->cur_program,
                   .clipped = clipped,
                   .clip = clip,
                   .rect = dl->cur_rect,
                   .focus = dl->cur_focus,
                   .first_index = dl->icount,
                   .index_count = 0};
    return b;
}

// The one place geometry enters the list. Every primitive below is this call
// with different numbers, which is why there is no second place for a corner
// radius or a clip to be forgotten.
// The quad's own corners ARE the rect the corner SDF measures against -- there
// was a `rect` parameter for a caller that wanted to differ, every call site
// passed NULL, and the fallback was the only path that ever ran.
static void _push_quad(UIDrawList* dl, GLuint texture, float x0, float y0, float x1, float y1,
                       float u0, float v0, float u1, float v1, const float color[4], float radius,
                       float border_w, float mode, const float border[4]) {
    if (!dl || x1 <= x0 || y1 <= y0)
        return;
    if (!_reserve_verts(dl, 4) || !_reserve_idx(dl, 6))
        return;
    UIBatch* batch = _batch_for(dl, texture);
    if (!batch)
        return;

    static const float opaque_white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const float no_border[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float* c = color ? color : opaque_white;
    const float* bc = border ? border : no_border;
    const float rc[4] = {x0, y0, x1, y1};

    const unsigned int base = (unsigned int)dl->vcount;
    const float params[4] = {radius, border_w, mode, 0.0f};

    const float xs[4] = {x0, x1, x1, x0};
    const float ys[4] = {y0, y0, y1, y1};
    const float us[4] = {u0, u1, u1, u0};
    const float vs[4] = {v0, v0, v1, v1};
    for (int i = 0; i < 4; i++) {
        UIVertex* v = &dl->verts[dl->vcount++];
        v->x = xs[i] + dl->offset_x;
        v->y = ys[i] + dl->offset_y;
        v->u = us[i];
        v->v = vs[i];
        memcpy(v->color, c, sizeof(float) * 4);
        v->color[3] *= dl->alpha;
        memcpy(v->rect, rc, sizeof(float) * 4);
        memcpy(v->params, params, sizeof(float) * 4);
        memcpy(v->border, bc, sizeof(float) * 4);
    }

    const unsigned int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++)
        dl->idx[dl->icount++] = base + order[i];
    batch->index_count += 6;
}

// ---------------------------------------------------------------- primitives

void ui_draw_quad(UIDrawList* dl, UIRect r, vec4 color) {
    if (!dl)
        return;
    _push_quad(dl, dl->white, r.x, r.y, r.x + r.w, r.y + r.h, 0, 0, 1, 1, color, 0.0f, 0.0f,
               UI_MODE_FILL, NULL);
}

void ui_draw_rect(UIDrawList* dl, UIRect r, const UIStyle* style) {
    if (!dl)
        return;
    if (!style) {
        vec4 white = {1, 1, 1, 1};
        ui_draw_quad(dl, r, white);
        return;
    }

    const bool has_fill = style->bg[3] > 0.0f || style->bg_tex != NULL;
    const bool has_border = style->border[3] > 0.0f && style->border_width > 0.0f;
    if (!has_fill && !has_border)
        return;

    if (style->bg_tex) {
        const bool sliced = style->bg_slice[0] > 0.0f || style->bg_slice[1] > 0.0f ||
                            style->bg_slice[2] > 0.0f || style->bg_slice[3] > 0.0f;
        // The TINT, not the fill. They were one field, which made an untinted
        // image unspellable -- the value meaning "no tint" is the value meaning
        // "inherit" -- so an image was multiplied by whatever fill the theme
        // happened to carry, or by transparent black, and vanished.
        if (sliced) {
            ui_draw_9slice(dl, r, style->bg_tex, style->bg_slice, (float*)style->bg_tint);
            return;
        }
        ui_draw_textured_quad(dl, r, style->bg_tex, (float*)style->bg_tint);
        return;
    }

    _push_quad(dl, dl->white, r.x, r.y, r.x + r.w, r.y + r.h, 0, 0, 1, 1, style->bg,
               style->corner_radius, has_border ? style->border_width : 0.0f, UI_MODE_FILL,
               style->border);
}

void ui_draw_rounded(UIDrawList* dl, UIRect r, float radius, vec4 fill, vec4 border,
                     float border_width) {
    if (!dl)
        return;
    _push_quad(dl, dl->white, r.x, r.y, r.x + r.w, r.y + r.h, 0, 0, 1, 1, fill, radius,
               border_width, UI_MODE_FILL, border);
}

void ui_draw_textured_quad(UIDrawList* dl, UIRect r, const Texture* tex, vec4 tint) {
    if (!dl)
        return;
    _push_quad(dl, tex ? tex->id : dl->white, r.x, r.y, r.x + r.w, r.y + r.h, 0, 0, 1, 1, tint,
               0.0f, 0.0f, tex ? UI_MODE_TEXTURE : UI_MODE_FILL, NULL);
}

void ui_draw_9slice(UIDrawList* dl, UIRect r, const Texture* tex, const float insets[4],
                    vec4 tint) {
    if (!dl || !tex || !insets || tex->width <= 0 || tex->height <= 0) {
        ui_draw_textured_quad(dl, r, tex, tint);
        return;
    }

    const float tw = (float)tex->width, th = (float)tex->height;
    float top = insets[0], right = insets[1], bottom = insets[2], left = insets[3];

    // A rect smaller than its own corners would overlap them, which reads as a
    // doubled border. Scale the whole inset set down instead of clamping each
    // edge, so the nine-patch shrinks in proportion rather than deforming.
    const float hsum = left + right, vsum = top + bottom;
    if (hsum > r.w && hsum > 0.0f) {
        const float k = r.w / hsum;
        left *= k;
        right *= k;
    }
    if (vsum > r.h && vsum > 0.0f) {
        const float k = r.h / vsum;
        top *= k;
        bottom *= k;
    }

    const float xs[4] = {r.x, r.x + left, r.x + r.w - right, r.x + r.w};
    const float ys[4] = {r.y, r.y + top, r.y + r.h - bottom, r.y + r.h};
    const float us[4] = {0.0f, insets[3] / tw, 1.0f - insets[1] / tw, 1.0f};
    const float vs[4] = {0.0f, insets[0] / th, 1.0f - insets[2] / th, 1.0f};

    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            _push_quad(dl, tex->id, xs[col], ys[row], xs[col + 1], ys[row + 1], us[col], vs[row],
                       us[col + 1], vs[row + 1], tint, 0.0f, 0.0f, UI_MODE_TEXTURE, NULL);
        }
    }
}

void ui_draw_list_set_offset(UIDrawList* dl, float dx, float dy) {
    if (!dl) {
        return;
    }
    dl->offset_x = dx;
    dl->offset_y = dy;
}

void ui_draw_list_set_alpha(UIDrawList* dl, float alpha) {
    if (!dl)
        return;
    dl->alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
}

void ui_push_clip(UIDrawList* dl, UIRect r) {
    if (!dl)
        return;
    if (dl->clip_depth >= UI_CLIP_MAX) {
        // Counted, not dropped: the matching pop has to unwind THIS rather than
        // a real level. Everything deeper keeps the current clip, which is the
        // conservative answer -- narrower than asked for, never wider.
        log_error("ui: clip stack is %d deep; this push keeps the current clip", UI_CLIP_MAX);
        dl->clip_refused++;
        return;
    }
    // Intersect with the enclosing clip, so a child cannot escape its parent
    // whatever order the rects arrive in.
    if (dl->clip_depth > 0) {
        const UIRect p = dl->clip[dl->clip_depth - 1];
        const float x0 = r.x > p.x ? r.x : p.x;
        const float y0 = r.y > p.y ? r.y : p.y;
        const float x1 = (r.x + r.w < p.x + p.w) ? r.x + r.w : p.x + p.w;
        const float y1 = (r.y + r.h < p.y + p.h) ? r.y + r.h : p.y + p.h;
        r = (UIRect){x0, y0, x1 > x0 ? x1 - x0 : 0.0f, y1 > y0 ? y1 - y0 : 0.0f};
    }
    dl->clip[dl->clip_depth++] = r;
}

void ui_pop_clip(UIDrawList* dl) {
    if (!dl)
        return;
    if (dl->clip_refused > 0) {
        dl->clip_refused--; // unwind a push that never reached the stack
        return;
    }
    if (dl->clip_depth > 0)
        dl->clip_depth--;
}

void ui_set_draw_program(UIDrawList* dl, ShaderProgram* program, UIRect rect, float focus) {
    if (!dl)
        return;
    dl->cur_program = program;
    dl->cur_rect = rect;
    dl->cur_focus = focus;
}

// ----------------------------------------------------------------- text

/*
 * The one advance rule, used by measuring, wrapping and drawing alike.
 *
 * Between two glyphs the pen moves by the face's own KERN pair plus the style's
 * TRACKING; after a glyph it moves by that glyph's advance. Both are applied
 * BETWEEN glyphs rather than after each, so neither a leading nor a trailing
 * gap creeps into a measured width -- which is what would make a centred string
 * sit off-centre by half a tracking.
 *
 * Everything that needs a width has to go through this, or a string is centred
 * against a width it does not have and a hit test lands beside the glyph the
 * player aimed at.
 */
static float _advance_between(const Font* font, int prev, int cp, float scale, float tracking) {
    if (prev <= 0)
        return 0.0f;
    return font_kern_advance(font, prev, cp) * scale + tracking;
}

float ui_line_height(const Font* font, float size, float line_spacing) {
    if (!font || font->base_size <= 0.0f)
        return size;
    const float lh = font->line_height * (size / font->base_size);
    return lh * (line_spacing > 0.0f ? line_spacing : 1.0f);
}

float ui_text_width(Font* font, float size, float tracking, const char* text) {
    if (!font || !text || font->base_size <= 0.0f)
        return 0.0f;
    const float scale = size / font->base_size;
    float w = 0.0f, best = 0.0f;
    int prev = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (*p == '\n') {
            if (w > best)
                best = w;
            w = 0.0f;
            prev = 0;
            continue;
        }
        const GlyphInfo* g = font_get_glyph(font, (int)*p);
        if (!g) {
            prev = 0;
            continue;
        }
        w += _advance_between(font, prev, (int)*p, scale, tracking);
        w += g->advance_x * scale;
        prev = (int)*p;
    }
    return w > best ? w : best;
}

size_t ui_text_wrap_point(Font* font, float size, float tracking, const char* text,
                          float max_width) {
    if (!font || !text || font->base_size <= 0.0f)
        return text ? strlen(text) : 0;
    const float scale = size / font->base_size;
    const size_t len = strlen(text);
    float w = 0.0f;
    size_t last_break = 0;
    int prev = 0;
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)text[i];
        if (c == '\n')
            return i;
        const GlyphInfo* g = font_get_glyph(font, (int)c);
        // A glyph the atlas cannot produce contributes NOTHING: no advance, and
        // no pair adjustment either. Charging kerning and tracking for an absent
        // glyph made this loop measure a longer string than the width and draw
        // loops did -- three spellings of one rule, already disagreeing, under a
        // header promising they cannot.
        if (!g) {
            prev = 0;
            if (c == ' ')
                last_break = i + 1;
            continue;
        }
        const float adv =
            _advance_between(font, prev, (int)c, scale, tracking) + g->advance_x * scale;
        if (w + adv > max_width && i > 0) {
            // Break at the last space if there was one; a single word longer
            // than the line breaks mid-word rather than overflowing, because
            // the alternative is a menu whose text leaves its panel.
            return last_break > 0 ? last_break : i;
        }
        w += adv;
        prev = (int)c;
        if (c == ' ')
            last_break = i + 1;
    }
    return len;
}

// One LINE's width -- up to the next newline or the end of the string. Not the
// whole string's: alignment is per line, and positioning every line by the
// longest one's width left each shorter line aligned to that line's edge
// instead of to the rect it was drawn into.
static float _line_width(Font* font, float size, float tracking, const unsigned char* p) {
    const float scale = size / font->base_size;
    float w = 0.0f;
    int prev = 0;
    for (; *p && *p != '\n'; p++) {
        const GlyphInfo* g = font_get_glyph(font, (int)*p);
        if (!g) {
            prev = 0;
            continue;
        }
        w += _advance_between(font, prev, (int)*p, scale, tracking) + g->advance_x * scale;
        prev = (int)*p;
    }
    return w;
}

static float _align_pen(UIRect r, UIAlign align, float line_w) {
    if (align == UI_ALIGN_CENTER)
        return r.x + (r.w - line_w) * 0.5f;
    if (align == UI_ALIGN_END)
        return r.x + r.w - line_w;
    return r.x;
}

float ui_draw_text(UIDrawList* dl, UIRect r, const char* text, const UIStyle* style,
                   UIAlign align) {
    if (!dl || !text)
        return 0.0f;

    Font* font = (style && style->font) ? style->font : dl->font;
    const float size = (style && style->font_size > 0.0f) ? style->font_size : dl->font_size;
    if (!font || font->base_size <= 0.0f || size <= 0.0f)
        return 0.0f;

    static const float default_fg[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float* color = style ? style->fg : default_fg;
    const float tracking = style ? style->tracking : 0.0f;
    const float line_spacing = style ? style->line_spacing : 0.0f;

    const float scale = size / font->base_size;
    const float width = ui_text_width(font, size, tracking, text);
    const float line_h = ui_line_height(font, size, line_spacing);

    float pen_x =
        _align_pen(r, align, _line_width(font, size, tracking, (const unsigned char*)text));

    // The baseline convention text.c uses, deliberately: the pen starts an
    // ascent below the top, so a string drawn here and one drawn through the
    // text renderer sit on the same line rather than a few pixels apart.
    float pen_y = r.y + font->ascent * scale;

    // One framebuffer pixel, expressed in points. Every glyph QUAD CORNER is
    // snapped to this grid below.
    //
    // Snapping the string's origin alone is not enough, and that is worth
    // recording because it looks like it should be: a glyph's top edge is
    // pen_y - y1 * scale, and y1 is an integer in ATLAS pixels, so at a
    // fractional scale every letter lands on a different fraction of a pixel
    // however the origin was placed. The SDF edge then resolves differently per
    // letter and the line reads as a wavy baseline of uneven weight.
    //
    // Snapping the corner and advancing the pen in float is what keeps the
    // spacing exact: the pen never sees the rounding, so no advance is
    // quantised and the glyph's own size is untouched.
    const float px = (dl->width > 0 && dl->engine->fb_width > 0)
                         ? (float)dl->width / (float)dl->engine->fb_width
                         : 1.0f;

    int prev = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; p++) {
        if (*p == '\n') {
            // The NEXT line's own width decides where it starts.
            pen_x = _align_pen(r, align, _line_width(font, size, tracking, p + 1));
            pen_y += line_h;
            prev = 0; // no pair spans a line break
            continue;
        }
        const GlyphInfo* g = font_get_glyph(font, (int)*p);
        if (!g) {
            prev = 0;
            continue;
        }
        // The same rule the measurement used, so what is drawn is exactly as
        // wide as what was measured -- otherwise a centred string is centred
        // against a width it does not have.
        pen_x += _advance_between(font, prev, (int)*p, scale, tracking);
        // GlyphInfo's box is stb's, and stb's is Y-DOWN: y0 is the offset from
        // the baseline to the TOP of the bitmap, NEGATIVE above it. So the box
        // is ADDED to the pen, never subtracted.
        //
        // Negating it -- which is what text.c does -- displaces every glyph by
        // 2*y0 + height, a quantity that depends on that glyph's own ascent and
        // descent. Tall letters and x-height letters therefore land at
        // different heights and the line reads as a ragged baseline rather than
        // as a uniform offset anyone would notice.
        float x0 = pen_x + g->x0 * scale;
        float y0 = pen_y + g->y0 * scale;
        if (px > 0.0f) {
            x0 = floorf(x0 / px + 0.5f) * px;
            y0 = floorf(y0 / px + 0.5f) * px;
        }
        const float x1 = x0 + (g->x1 - g->x0) * scale;
        const float y1 = y0 + (g->y1 - g->y0) * scale;
        _push_quad(dl, font->atlas_texture_id, x0, y0, x1, y1, g->u0, g->v0, g->u1, g->v1, color,
                   0.0f, 0.0f, UI_MODE_GLYPH, NULL);
        pen_x += g->advance_x * scale;
        prev = (int)*p;
    }
    return width;
}

// ------------------------------------------------------------------ lifecycle

static GLuint _make_white_texture(void) {
    const unsigned char px[4] = {255, 255, 255, 255};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

static void _setup_vao(UIDrawList* dl) {
    glGenVertexArrays(1, &dl->vao);
    glGenBuffers(1, &dl->vbo);
    glGenBuffers(1, &dl->ebo);

    glBindVertexArray(dl->vao);
    glBindBuffer(GL_ARRAY_BUFFER, dl->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dl->ebo);

    const GLsizei stride = (GLsizei)sizeof(UIVertex);
    // A private VAO, so these locations answer to ui_vert.glsl alone -- the
    // GL_ATTR_* ledger in common.h governs MESH VAOs and nothing here is one.
    const struct {
        GLuint loc;
        GLint count;
        size_t offset;
    } attrs[] = {
        {0, 2, offsetof(UIVertex, x)},      {1, 2, offsetof(UIVertex, u)},
        {2, 4, offsetof(UIVertex, color)},  {3, 4, offsetof(UIVertex, rect)},
        {4, 4, offsetof(UIVertex, params)}, {5, 4, offsetof(UIVertex, border)},
    };
    for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
        glEnableVertexAttribArray(attrs[i].loc);
        glVertexAttribPointer(attrs[i].loc, attrs[i].count, GL_FLOAT, GL_FALSE, stride,
                              (const void*)attrs[i].offset);
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

UIDrawList* create_ui_draw_list(Engine* engine) {
    if (!engine) {
        log_error("ui: create_ui_draw_list needs an engine for its program cache");
        return NULL;
    }

    UIDrawList* dl = calloc(1, sizeof(UIDrawList));
    if (!dl) {
        log_error("ui: out of memory creating the draw list");
        return NULL;
    }
    dl->engine = engine;

    dl->program = engine_find_program(engine, "ui");
    if (!dl->program) {
        dl->program = create_ui_program();
        if (!dl->program) {
            free(dl);
            return NULL;
        }
        engine_add_program(engine, dl->program);
    }

    dl->white = _make_white_texture();
    _setup_vao(dl);
    dl->font_size = 16.0f;
    return dl;
}

void free_ui_draw_list(UIDrawList* dl) {
    if (!dl)
        return;
    // The program belongs to the engine's cache, like the text renderer's.
    if (dl->white)
        glDeleteTextures(1, &dl->white);
    if (dl->ebo)
        glDeleteBuffers(1, &dl->ebo);
    if (dl->vbo)
        glDeleteBuffers(1, &dl->vbo);
    if (dl->vao)
        glDeleteVertexArrays(1, &dl->vao);
    free(dl->verts);
    free(dl->idx);
    free(dl->batches);
    free(dl);
}

void ui_draw_list_set_font(UIDrawList* dl, Font* font, float size) {
    if (!dl)
        return;
    dl->font = font;
    if (size > 0.0f)
        dl->font_size = size;
}

void ui_draw_list_begin(UIDrawList* dl, int width_points, int height_points) {
    if (!dl || width_points <= 0 || height_points <= 0)
        return;
    dl->vcount = 0;
    dl->icount = 0;
    dl->bcount = 0;
    dl->clip_depth = 0;
    dl->clip_refused = 0;
    dl->alpha = 1.0f;
    dl->cur_program = NULL;
    dl->cur_rect = (UIRect){0, 0, (float)width_points, (float)height_points};
    dl->cur_focus = 0.0f;
    dl->width = width_points;
    dl->height = height_points;
    // uTime is part of the contract a custom element program is written
    // against, so it has to actually advance. It was declared, uploaded to
    // every batch and never assigned, which made the spec's headline use for
    // ui_set_element_program -- a shader-driven animated background -- a
    // documented escape hatch with a frozen clock.
    if (dl->engine)
        dl->time = (float)dl->engine->total_frames * (float)ENGINE_FIXED_FRAME_DT;
    // Top-left origin with +Y down, the space the text renderer and GLFW's
    // cursor already agree on.
    glm_ortho(0.0f, (float)width_points, (float)height_points, 0.0f, -1.0f, 1.0f, dl->ortho);
}

void ui_draw_list_render(UIDrawList* dl) {
    if (!dl || dl->icount == 0 || !dl->program)
        return;

    // Save every piece of state this touches. The frame is live here -- the
    // post chain has written it and the debug GUI draws next -- so leaving the
    // depth test off or the blend func changed corrupts whatever runs after.
    const GLboolean had_depth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean had_blend = glIsEnabled(GL_BLEND);
    const GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean had_cull = glIsEnabled(GL_CULL_FACE);
    GLint blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO;
    GLint blend_src_a = GL_ONE, blend_dst_a = GL_ZERO;
    GLint scissor_box[4] = {0, 0, 0, 0};
    GLint prev_program = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a);
    glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(dl->vao);
    glBindBuffer(GL_ARRAY_BUFFER, dl->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dl->ebo);

    // One re-specification per buffer. A glBufferData WITH data already orphans
    // -- the driver is free to hand back a fresh block rather than stall on the
    // one still being read -- so the NULL call that preceded it bought nothing
    // and paid for a second allocation. The orphan idiom the old comment named
    // is glBufferData(NULL) followed by glBufferSubData; this was neither.
    const size_t vbytes = dl->vcount * sizeof(UIVertex);
    const size_t ibytes = dl->icount * sizeof(unsigned int);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vbytes, dl->verts, GL_STREAM_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)ibytes, dl->idx, GL_STREAM_DRAW);

    // The clip rect is in points and the scissor is in framebuffer pixels
    // measured from the BOTTOM, so it converts on both axes. On a Retina
    // display the scale is 2 and skipping it clips a quarter of the screen.
    const float sx = dl->width > 0 ? (float)dl->engine->fb_width / (float)dl->width : 1.0f;
    const float sy = dl->height > 0 ? (float)dl->engine->fb_height / (float)dl->height : 1.0f;

    for (size_t i = 0; i < dl->bcount; i++) {
        const UIBatch* b = &dl->batches[i];
        if (b->index_count == 0)
            continue;

        ShaderProgram* program = b->program ? b->program : dl->program;
        glUseProgram(program->id);
        UniformManager* u = program->uniforms;
        uniform_set_mat4(u, "uProjection", (float*)dl->ortho);
        const float resolution[2] = {(float)dl->width, (float)dl->height};
        uniform_set_vec2(u, "uResolution", resolution);
        uniform_set_float(u, "uTime", dl->time);
        uniform_set_float(u, "uFocus", b->focus);
        const float rect[4] = {b->rect.x, b->rect.y, b->rect.w, b->rect.h};
        uniform_set_vec4(u, "uRect", rect);
        uniform_set_int(u, "uTex", 0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, b->texture);

        if (b->clipped) {
            glEnable(GL_SCISSOR_TEST);
            glScissor((GLint)(b->clip.x * sx),
                      (GLint)(((float)dl->height - (b->clip.y + b->clip.h)) * sy),
                      (GLsizei)(b->clip.w * sx), (GLsizei)(b->clip.h * sy));
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        glDrawElements(GL_TRIANGLES, (GLsizei)b->index_count, GL_UNSIGNED_INT,
                       (const void*)(b->first_index * sizeof(unsigned int)));
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    glUseProgram((GLuint)prev_program);
    glScissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
    if (!had_scissor)
        glDisable(GL_SCISSOR_TEST);
    else
        glEnable(GL_SCISSOR_TEST);
    glBlendFuncSeparate((GLenum)blend_src_rgb, (GLenum)blend_dst_rgb, (GLenum)blend_src_a,
                        (GLenum)blend_dst_a);
    if (!had_blend)
        glDisable(GL_BLEND);
    if (had_depth)
        glEnable(GL_DEPTH_TEST);
    if (had_cull)
        glEnable(GL_CULL_FACE);
}
