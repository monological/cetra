#include <stdlib.h>
#include <string.h>

#include "lighting_atlas.h"
#include "engine.h"
#include "texture.h"
#include "uniform.h"
#include "util.h"
#include "ext/log.h"

// Row r's interior edge. Halving stops at PROBE_ATLAS_ROW_MIN: below that a
// tile is mostly gutter, and the roughness levels reading those rows have a
// lobe wide enough that the resolution stopped mattering several rows earlier.
static int atlas_row_res(int row0, int row) {
    int res = row0 >> row;
    return res < PROBE_ATLAS_ROW_MIN ? PROBE_ATLAS_ROW_MIN : res;
}

static int atlas_row_pitch(int row0, int row) {
    return atlas_row_res(row0, row) + 2 * PROBE_ATLAS_GUTTER;
}

// A probe's column is as wide as row 0, which is the widest row.
static int atlas_column_w(int row0) {
    return atlas_row_pitch(row0, 0);
}

static int atlas_column_h(int row0) {
    int h = 0;
    for (int r = 0; r < PROBE_ATLAS_ROWS; ++r)
        h += atlas_row_pitch(row0, r);
    return h;
}

// Probe columns stacked in each vertical stack: as many as the GI slots' height holds, so a
// tall GI region does not pad every column out to its height. At least one.
static int atlas_columns_per_stack(const LightingAtlas* atlas) {
    const int col_h = atlas_column_h(atlas->row0);
    const int stack = col_h > 0 ? atlas->gi_slot_h / col_h : 1;
    return stack > 1 ? stack : 1;
}

// Lower-left texel of a probe column, gutter included.
static void atlas_column_origin(const LightingAtlas* atlas, int index, int* out_x, int* out_y) {
    const int stack = atlas_columns_per_stack(atlas);
    *out_x = atlas->spec_x + (index / stack) * atlas_column_w(atlas->row0);
    *out_y = (index % stack) * atlas_column_h(atlas->row0);
}

// Lower-left texel of one (probe, row) tile INCLUDING its gutter.
static void atlas_tile_origin(const LightingAtlas* atlas, int index, int row, int* out_x,
                              int* out_y) {
    int y = 0;
    for (int r = 0; r < row; ++r)
        y += atlas_row_pitch(atlas->row0, r);
    atlas_column_origin(atlas, index, out_x, out_y);
    *out_y += y;
}

static int clamp_row0(int row0) {
    if (row0 <= 0)
        row0 = PROBE_ATLAS_ROW0_DEFAULT;
    if (row0 < PROBE_ATLAS_ROW0_MIN)
        row0 = PROBE_ATLAS_ROW0_MIN;
    if (row0 > PROBE_ATLAS_ROW0_MAX)
        row0 = PROBE_ATLAS_ROW0_MAX;
    // Any size in range. This used to round down to a power of two, because the
    // shader recomputed the row sizes with a divide by exp2 where this file
    // shifts, and the two agree only on powers of two. The shader reads a
    // published table now, so there is nothing left to agree with.
    return row0;
}

// A cleared RGBA16F texture and the FBO that renders into it. False leaves nothing behind.
static bool make_target(int w, int h, GLuint* out_texture, GLuint* out_fbo) {
    GLuint texture = create_texture_2d_float(w, h, GL_RGBA16F, GL_RGBA, NULL), fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (!texture || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &texture);
        return false;
    }

    // The whole texture, so a slot nothing has written yet reads black rather than
    // whatever the driver handed back.
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    *out_texture = texture;
    *out_fbo = fbo;
    return true;
}

static void copy_rect(GLuint from_fbo, GLuint to_fbo, int sx, int sy, int dx, int dy, int w,
                      int h) {
    if (w <= 0 || h <= 0)
        return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, from_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to_fbo);
    glBlitFramebuffer(sx, sy, sx + w, sy + h, dx, dy, dx + w, dy + h, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
}

// Grow to at least this layout. Every existing slot and column goes across to where the new
// layout puts it, so nothing resident has to be captured again.
static bool atlas_reserve(LightingAtlas* atlas, const LightingAtlasLayout* layout, int max_tex) {
    LightingAtlas next = *atlas;
    if (layout->gi_slots > next.gi_slots)
        next.gi_slots = layout->gi_slots;
    if (layout->gi_w > next.gi_slot_w)
        next.gi_slot_w = layout->gi_w;
    if (layout->gi_h > next.gi_slot_h)
        next.gi_slot_h = layout->gi_h;
    if (layout->probe_slots > next.capacity)
        next.capacity = layout->probe_slots;
    if (next.capacity > 0 && atlas->capacity == 0)
        next.row0 = clamp_row0(layout->probe_row0);

    if (next.gi_slots == atlas->gi_slots && next.gi_slot_w == atlas->gi_slot_w &&
        next.gi_slot_h == atlas->gi_slot_h && next.capacity == atlas->capacity && atlas->texture)
        return true;

    const int gi_cols_w = next.gi_slots * next.gi_slot_w;
    int stacks = 0, stack_h = 0;
    if (next.capacity > 0) {
        const int per_stack = atlas_columns_per_stack(&next);
        stacks = (next.capacity + per_stack - 1) / per_stack;
        stack_h =
            atlas_column_h(next.row0) * (next.capacity < per_stack ? next.capacity : per_stack);
    }
    next.spec_x = gi_cols_w;
    next.width = gi_cols_w + stacks * atlas_column_w(next.row0);
    next.height = next.gi_slot_h > stack_h ? next.gi_slot_h : stack_h;
    if (next.width <= 0 || next.height <= 0)
        return false;

    if (max_tex > 0 && (next.width > max_tex || next.height > max_tex)) {
        if (!atlas->refused)
            log_error("Lighting atlas %dx%d (%d GI slots of %dx%d, %d probe columns) exceeds the "
                      "driver's %d texture limit",
                      next.width, next.height, next.gi_slots, next.gi_slot_w, next.gi_slot_h,
                      next.capacity, max_tex);
        atlas->refused = true;
        return false;
    }
    next.refused = false;

    GLint saved_fbo = 0, saved_read = 0, saved_draw = 0, saved_viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw);
    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

    if (!make_target(next.width, next.height, &next.texture, &next.fbo)) {
        log_error("Lighting atlas target incomplete at %dx%d", next.width, next.height);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
        glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
        if (scissor)
            glEnable(GL_SCISSOR_TEST);
        return false;
    }

    if (atlas->texture) {
        for (int s = 0; s < atlas->gi_slots; ++s)
            copy_rect(atlas->fbo, next.fbo, s * atlas->gi_slot_w, 0, s * next.gi_slot_w, 0,
                      atlas->gi_slot_w, atlas->gi_slot_h);
        const int col_w = atlas->capacity > 0 ? atlas_column_w(atlas->row0) : 0;
        const int col_h = atlas->capacity > 0 ? atlas_column_h(atlas->row0) : 0;
        for (int c = 0; c < atlas->capacity; ++c) {
            int sx, sy, dx, dy;
            atlas_column_origin(atlas, c, &sx, &sy);
            atlas_column_origin(&next, c, &dx, &dy);
            copy_rect(atlas->fbo, next.fbo, sx, sy, dx, dy, col_w, col_h);
        }
        glDeleteFramebuffers(1, &atlas->fbo);
        glDeleteTextures(1, &atlas->texture);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)saved_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)saved_draw);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);

    *atlas = next;
    log_info("Lighting atlas: %dx%d, %d GI slots of %dx%d, %d probe columns at row0 %d",
             atlas->width, atlas->height, atlas->gi_slots, atlas->gi_slot_w, atlas->gi_slot_h,
             atlas->capacity, atlas->row0);
    return true;
}

LightingAtlas* lighting_atlas_reserve(LightingAtlas** atlas, const LightingAtlasLayout* layout,
                                      struct Engine* engine) {
    if (!atlas || !layout || !engine)
        return NULL;
    if (layout->gi_slots == 0 && layout->probe_slots == 0)
        return *atlas;
    if (!*atlas) {
        LightingAtlas* made = calloc(1, sizeof(LightingAtlas));
        if (!made) {
            log_error("Failed to allocate the lighting atlas");
            return NULL;
        }
        made->project_program = engine_get_program(engine, "probe_project");
        create_fullscreen_quad_vao(&made->quad_vao, &made->quad_vbo);
        *atlas = made;
    }
    return atlas_reserve(*atlas, layout, engine->max_texture_size) ? *atlas : NULL;
}

void free_lighting_atlas(LightingAtlas* atlas) {
    if (!atlas)
        return;
    if (atlas->texture)
        glDeleteTextures(1, &atlas->texture);
    if (atlas->fbo)
        glDeleteFramebuffers(1, &atlas->fbo);
    if (atlas->quad_vao)
        glDeleteVertexArrays(1, &atlas->quad_vao);
    if (atlas->quad_vbo)
        glDeleteBuffers(1, &atlas->quad_vbo);
    free(atlas);
}

int lighting_atlas_gi_x(const LightingAtlas* atlas, int slot) {
    return atlas && slot >= 0 && slot < atlas->gi_slots ? slot * atlas->gi_slot_w : 0;
}

bool lighting_atlas_project_probe(LightingAtlas* atlas, const ReflectionProbe* probe, int index) {
    if (!atlas || !atlas->texture || !atlas->project_program || !probe || !probe->prefiltered ||
        index < 0 || index >= atlas->capacity)
        return false;

    GLint saved_viewport[4];
    GLint saved_fbo;
    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);
    // Put back as found: a set is captured inside the frame, after the frame top has
    // set the culling the shadow and scene passes draw with.
    const GLboolean saved_cull = glIsEnabled(GL_CULL_FACE);

    glBindFramebuffer(GL_FRAMEBUFFER, atlas->fbo);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(atlas->project_program->id);
    glBindVertexArray(atlas->quad_vao);

    UniformManager* u = atlas->project_program->uniforms;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, probe->prefiltered);
    uniform_set_int(u, "sourceCube", 0);

    for (int r = 0; r < PROBE_ATLAS_ROWS; ++r) {
        int ox, oy;
        atlas_tile_origin(atlas, index, r, &ox, &oy);
        const int res = atlas_row_res(atlas->row0, r);

        glViewport(ox, oy, res + 2 * PROBE_ATLAS_GUTTER, res + 2 * PROBE_ATLAS_GUTTER);
        uniform_set_vec2(u, "tileOrigin", (const float[]){(float)ox, (float)oy});
        uniform_set_float(u, "tileRes", (float)res);
        // Row r reads mip r, which is what makes row r carry roughness
        // r/(rows-1) without a remap on either side.
        uniform_set_float(u, "sourceLod", (float)r);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    if (saved_cull)
        glEnable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);

    check_gl_error("lighting atlas project probe");
    return true;
}

void lighting_atlas_bind(const LightingAtlas* atlas, ShaderProgram* program) {
    if (!program || !program->uniforms)
        return;
    uniform_set_int(program->uniforms, "giAtlasTex", LIGHTING_ATLAS_TEXTURE_UNIT);
    if (!atlas || !atlas->texture)
        return;
    glActiveTexture(GL_TEXTURE0 + LIGHTING_ATLAS_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, atlas->texture);
    glActiveTexture(GL_TEXTURE0);
}

GLuint lighting_atlas_texture(const LightingAtlas* atlas) {
    return atlas ? atlas->texture : 0;
}

AtlasRect lighting_atlas_probe_rect(const LightingAtlas* atlas, int slot) {
    AtlasRect r = {0, 0, 0, 0};
    if (!atlas || slot < 0 || slot >= atlas->capacity)
        return r;
    atlas_column_origin(atlas, slot, &r.x, &r.y);
    r.w = atlas_column_w(atlas->row0);
    r.h = atlas_column_h(atlas->row0);
    return r;
}

// The bytes a rectangle's texels take on the CPU: RGBA half floats.
static size_t rect_bytes(AtlasRect rect) {
    return sizeof(uint16_t) * 4 * (size_t)rect.w * (size_t)rect.h;
}

uint16_t* lighting_atlas_keep(const LightingAtlas* atlas, AtlasRect rect) {
    if (!atlas || !atlas->fbo || rect.w <= 0 || rect.h <= 0)
        return NULL;
    uint16_t* texels = malloc(rect_bytes(rect));
    if (!texels)
        return NULL;
    GLint saved_read = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, atlas->fbo);
    glReadPixels(rect.x, rect.y, rect.w, rect.h, GL_RGBA, GL_HALF_FLOAT, texels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)saved_read);
    check_gl_error("lighting atlas keep");
    return texels;
}

bool lighting_atlas_restore(const LightingAtlas* atlas, AtlasRect rect, const uint16_t* texels) {
    if (!atlas || !atlas->texture || !texels || rect.w <= 0 || rect.h <= 0)
        return false;
    glBindTexture(GL_TEXTURE_2D, atlas->texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.w, rect.h, GL_RGBA, GL_HALF_FLOAT,
                    texels);
    glBindTexture(GL_TEXTURE_2D, 0);
    check_gl_error("lighting atlas restore");
    return true;
}

void lighting_atlas_clear(const LightingAtlas* atlas, AtlasRect rect) {
    if (!atlas || !atlas->fbo || rect.w <= 0 || rect.h <= 0)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, atlas->fbo);
    glEnable(GL_SCISSOR_TEST);
    glScissor(rect.x, rect.y, rect.w, rect.h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // make_target's
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

uint32_t lighting_atlas_digest(const LightingAtlas* atlas, AtlasRect rect, const uint16_t* kept) {
    if (kept)
        return fnv1a_bytes(kept, rect_bytes(rect));
    uint16_t* read = lighting_atlas_keep(atlas, rect);
    const uint32_t digest = read ? fnv1a_bytes(read, rect_bytes(rect)) : 0u;
    free(read);
    return digest;
}

void lighting_atlas_fill_column(const LightingAtlas* atlas, float out_column[4],
                                float out_rows[][4]) {
    if (!atlas || !out_column || !out_rows)
        return;

    out_column[0] = 0.0f;
    out_column[1] = 0.0f;
    out_column[2] = (float)PROBE_ATLAS_GUTTER;
    out_column[3] = (float)(PROBE_ATLAS_ROWS - 1);

    // The row table the shader reads instead of re-deriving the halving rule.
    // Every probe's column has this same geometry, so it is published once.
    int y = 0;
    for (int r = 0; r < PROBE_ATLAS_ROWS; ++r) {
        out_rows[r][0] = (float)y;
        out_rows[r][1] = (float)atlas_row_res(atlas->row0, r);
        out_rows[r][2] = 0.0f;
        out_rows[r][3] = 0.0f;
        y += atlas_row_pitch(atlas->row0, r);
    }
}

void lighting_atlas_size(const LightingAtlas* atlas, int* out_w, int* out_h) {
    if (out_w)
        *out_w = atlas ? atlas->width : 0;
    if (out_h)
        *out_h = atlas ? atlas->height : 0;
}

void lighting_atlas_debug_blit(const LightingAtlas* atlas, struct Engine* engine, int screen_w,
                               int screen_h, float scale) {
    if (!atlas || !atlas->texture || !engine)
        return;
    // The same shared textured-quad overlay the sky LUTs draw through (a DRAW, not a
    // blit: the default framebuffer is multisample and a single-sample blit into it is
    // illegal on core profile).
    ShaderProgram* prog = engine_get_program(engine, "sky_debug");
    if (!prog)
        return;

    GLint prev_viewport[4];
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // As large as the screen allows at the atlas's own aspect: a GI slot is tall and narrow,
    // a row of probe columns wide and short, and the tiles are a few texels across.
    int w = screen_w - 20;
    int h = (int)((float)w * (float)atlas->height / (float)atlas->width);
    if (h > screen_h - 20) {
        h = screen_h - 20;
        w = (int)((float)h * (float)atlas->width / (float)atlas->height);
    }
    glViewport(10, screen_h - 10 - h, w, h);
    glUseProgram(prog->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas->texture);
    // Point-sampled: what this view separates is a bad tile from a bad lookup, and
    // the tile grid and its gutter are the structure that answers it. Bilinear at
    // this magnification washes both away.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    uniform_set_int(prog->uniforms, "lut", 0);
    uniform_set_float(prog->uniforms, "scale", scale);
    draw_fullscreen_quad(atlas->quad_vao);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    if (blend_was)
        glEnable(GL_BLEND);
    glUseProgram(0);
}
