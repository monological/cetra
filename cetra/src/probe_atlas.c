#include <stdlib.h>
#include <string.h>

#include "probe_atlas.h"
#include "probe_set.h"
#include "engine.h"
#include "gi_volume.h"
#include "postfx.h"
#include "scene.h"
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

// Lower-left texel of one (probe, row) tile INCLUDING its gutter.
static void atlas_tile_origin(const ProbeAtlas* atlas, int index, int row, int* out_x,
                              int* out_y) {
    int y = 0;
    for (int r = 0; r < row; ++r)
        y += atlas_row_pitch(atlas->row0, r);
    *out_x = atlas->spec_x + index * atlas_column_w(atlas->row0);
    *out_y = y;
}

ProbeAtlas* create_probe_atlas(struct Engine* engine, struct Scene* scene, int count, int row0) {
    if (!engine || count < 1)
        return NULL;

    if (row0 <= 0)
        row0 = PROBE_ATLAS_ROW0_DEFAULT;
    if (row0 < PROBE_ATLAS_ROW0_MIN)
        row0 = PROBE_ATLAS_ROW0_MIN;
    if (row0 > PROBE_ATLAS_ROW0_MAX)
        row0 = PROBE_ATLAS_ROW0_MAX;
    // Down to a power of two, and this is a correctness requirement rather than
    // neatness. The row sizes are computed twice -- here as an integer shift and
    // in the shader as a divide by exp2 -- because the shader has no integer
    // row0 to shift. Those two agree for every power of two and disagree the
    // moment row0 is anything else (513 >> 1 is 256, 513/2 is 256.5), and the
    // disagreement is a half-texel drift in every row origin below the first:
    // the atlas would be written at one set of offsets and read at another.
    int pot = PROBE_ATLAS_ROW0_MIN;
    while (pot * 2 <= row0)
        pot *= 2;
    if (pot != row0)
        log_warn("Probe atlas row size %d is not a power of two; using %d", row0, pot);
    row0 = pot;

    ProbeAtlas* atlas = calloc(1, sizeof(ProbeAtlas));
    if (!atlas) {
        log_error("Failed to allocate probe atlas");
        return NULL;
    }
    atlas->row0 = row0;
    atlas->capacity = count;

    // The GI volume takes the left columns when one exists. Side by side rather
    // than stacked: the two regions have wildly different widths, and stacking
    // would pad the narrow one out to the wide one's width -- 28 MB of nothing
    // at eight probes beside an 8x4x8 volume.
    int gi_w = 0, gi_h = 0;
    if (scene && scene->gi_volume)
        gi_volume_atlas_extent(scene->gi_volume, &gi_w, &gi_h);

    const int col_w = atlas_column_w(row0);
    const int col_h = atlas_column_h(row0);
    atlas->spec_x = gi_w;
    atlas->width = gi_w + count * col_w;
    atlas->height = col_h > gi_h ? col_h : gi_h;

    GLint max_tex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    if (max_tex > 0 && (atlas->width > max_tex || atlas->height > max_tex)) {
        log_error("Probe atlas %dx%d exceeds the driver's %d texture limit", atlas->width,
                  atlas->height, max_tex);
        free(atlas);
        return NULL;
    }

    glGenTextures(1, &atlas->texture);
    glBindTexture(GL_TEXTURE_2D, atlas->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, atlas->width, atlas->height, 0, GL_RGBA, GL_FLOAT,
                 NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &atlas->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, atlas->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, atlas->texture, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    if (!atlas->texture || !complete) {
        log_error("Probe atlas target incomplete; the probe set stays unpublished");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        free_probe_atlas(atlas);
        return NULL;
    }

    atlas->project_program = get_engine_shader_program_by_name(engine, "probe_project");
    if (!atlas->project_program) {
        log_error("Probe projection program missing; the probe set stays unpublished");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        free_probe_atlas(atlas);
        return NULL;
    }

    create_fullscreen_quad_vao(&atlas->quad_vao, &atlas->quad_vbo);

    // Clear the WHOLE texture, the GI region included: the volume adopts this
    // allocation and skips its own clear, because clearing there would erase
    // the columns this pass is about to write.
    GLint saved_viewport[4];
    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    glViewport(0, 0, atlas->width, atlas->height);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);

    if (scene && scene->gi_volume)
        gi_volume_adopt_atlas(scene->gi_volume, atlas->texture, atlas->width, atlas->height);

    log_info("Probe atlas: %d probes, %dx%d, row0 %d%s", count, atlas->width, atlas->height, row0,
             gi_w ? " (GI volume tenanted)" : "");
    return atlas;
}

void free_probe_atlas(ProbeAtlas* atlas) {
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

bool probe_atlas_project(ProbeAtlas* atlas, const ReflectionProbe* probe, int index) {
    if (!atlas || !probe || !probe->prefiltered || index < 0 || index >= atlas->capacity)
        return false;

    GLint saved_viewport[4];
    GLint saved_fbo;
    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);

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
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);

    check_gl_error("probe atlas project");
    return true;
}

void probe_atlas_bind(const ProbeAtlas* atlas, ShaderProgram* program, int count) {
    if (!atlas || !program || !program->uniforms)
        return;

    UniformManager* u = program->uniforms;
    uniform_set_int(u, "giAtlasTex", GI_ATLAS_TEXTURE_UNIT);

    glActiveTexture(GL_TEXTURE0 + GI_ATLAS_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, atlas->texture);
    glActiveTexture(GL_TEXTURE0);

    uniform_set_int(u, "probeCount", count);
}

void probe_atlas_publish_to_postfx(const ProbeAtlas* atlas, const ReflectionProbeSet* set,
                                   PostFX* fx) {
    if (!fx)
        return;

    fx->probe_multi = atlas != NULL;
    fx->probe_atlas = atlas ? atlas->texture : 0;
    fx->probe_count = set ? set->count : 0;
    // SSR reads the same block the surface program does, so it needs no copy of
    // the descriptors -- only the texture and the count that arms the loop.
    fx->probe_enabled = false;
    fx->probe_cubemap = 0;
}

void probe_atlas_fill_rect(const ProbeAtlas* atlas, int index, float out[4]) {
    if (!atlas || !out)
        return;
    int ox, oy;
    atlas_tile_origin(atlas, index, 0, &ox, &oy);
    out[0] = (float)ox;
    out[1] = (float)oy;
    out[2] = (float)atlas->row0;
    out[3] = (float)PROBE_ATLAS_GUTTER;
}

void probe_atlas_size(const ProbeAtlas* atlas, int* out_w, int* out_h) {
    if (out_w)
        *out_w = atlas ? atlas->width : 0;
    if (out_h)
        *out_h = atlas ? atlas->height : 0;
}

void probe_atlas_rect(const ProbeAtlas* atlas, int index, int* out_x, int* out_y, int* out_rows) {
    int x = 0, y = 0;
    if (atlas && index >= 0 && index < atlas->capacity)
        atlas_tile_origin(atlas, index, 0, &x, &y);
    if (out_x)
        *out_x = x;
    if (out_y)
        *out_y = y;
    if (out_rows)
        *out_rows = atlas ? PROBE_ATLAS_ROWS : 0;
}

void probe_atlas_debug_blit(const ProbeAtlas* atlas, struct Engine* engine, int screen_w,
                            int screen_h) {
    if (!atlas || !atlas->texture || !engine)
        return;
    // The same shared textured-quad overlay the sky LUTs and the GI atlas draw
    // through (a DRAW, not a blit: the default framebuffer is multisample and a
    // single-sample blit into it is illegal on core profile).
    ShaderProgram* prog = get_engine_shader_program_by_name(engine, "sky_debug");
    if (!prog)
        return;

    GLint prev_viewport[4];
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

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
    // Point-sampled: what this view separates is a bad column from a bad
    // lookup, and the tile grid and its gutter are the structure that answers
    // it. Bilinear at this magnification washes both away.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    uniform_set_int(prog->uniforms, "lut", 0);
    uniform_set_float(prog->uniforms, "scale", 1.0f);
    draw_fullscreen_quad(atlas->quad_vao);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    if (blend_was)
        glEnable(GL_BLEND);
    glUseProgram(0);
}
