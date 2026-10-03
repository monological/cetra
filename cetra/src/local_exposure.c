#include "local_exposure.h"

#include <math.h>
#include <stdio.h> // local_exposure_probe prints to stdout, like the other probes
#include <stdlib.h>

#include "ext/log.h"
#include "program.h"
#include "texture.h"
#include "util.h"

// Half-res texels a grid cell spans each way, and the bins across log2 luminance: Unreal's.
// le_grid_frag.glsl's LE_CELL is the first.
#define LE_CELL 64
#define LE_BINS 32
// The bins' span in stops (Unreal: log2 -10..20), and how much of it lies below middle grey.
// The centring is ours: Unreal's range is in absolute luminance, and this buffer is exposed.
#define LE_STOPS      30.0f
#define LE_BELOW_GREY 12.0f
// Tiles across the atlas; LE_BINS / LE_TILES_X rows of them.
#define LE_TILES_X 8
// Half-res texels a blurred-luminance texel averages each way: 32 frame pixels, Unreal's.
// le_block_frag.glsl's LE_BLOCK.
#define LE_BLOCK 16
// The most texels either side the blurred luminance's Gaussian reaches: a 50% kernel on a 4K
// frame's 120 blocks is 30.
#define LE_RADIUS_MAX 64

struct LocalExposure {
    ShaderProgram *half, *grid, *grid_blur, *block, *blur;
    int frame_w, frame_h; // what the targets were built for; 0 = not yet
    int half_w, half_h;
    int cells_w, cells_h;          // the grid's cells
    int blur_w, blur_h;            // the blurred luminance's texels
    float range_lo;                // log2 luminance where bin 0 starts, this frame
    GLuint half_tex, half_fbo;     // RGBA16F: the mean colour of each 2x2, its log2 luminance
    GLuint block_tex, block_fbo;   // R32F: log2 luminance of each block
    GLuint across_tex, across_fbo; // R32F: the blocks blurred across
    // RG32F: the grid's tiles from the bottom, the blurred luminance above them. Ping-pong for
    // the grid's blur; the result is always in atlas[0].
    GLuint atlas[2], atlas_fbo[2];
};

LocalExposure* create_local_exposure(void) {
    LocalExposure* le = calloc(1, sizeof(LocalExposure));
    if (!le)
        return NULL;
    le->half = create_le_half_program();
    le->grid = create_le_grid_program();
    le->grid_blur = create_le_grid_blur_program();
    le->block = create_le_block_program();
    le->blur = create_le_blur_program();
    if (!le->half || !le->grid || !le->grid_blur || !le->block || !le->blur) {
        log_error("Local exposure: programs unavailable");
        free_local_exposure(le);
        return NULL;
    }
    return le;
}

static void _le_free_targets(LocalExposure* le) {
    gl_delete_texture(&le->half_tex);
    gl_delete_fbo(&le->half_fbo);
    gl_delete_texture(&le->block_tex);
    gl_delete_fbo(&le->block_fbo);
    gl_delete_texture(&le->across_tex);
    gl_delete_fbo(&le->across_fbo);
    for (int i = 0; i < 2; i++) {
        gl_delete_texture(&le->atlas[i]);
        gl_delete_fbo(&le->atlas_fbo[i]);
    }
    le->frame_w = le->frame_h = 0;
}

void free_local_exposure(LocalExposure* le) {
    if (!le)
        return;
    _le_free_targets(le);
    free_program(le->half);
    free_program(le->grid);
    free_program(le->grid_blur);
    free_program(le->block);
    free_program(le->blur);
    free(le);
}

static bool _le_target(int w, int h, GLenum internal, GLenum format, GLint filter, GLuint* tex,
                       GLuint* fbo) {
    *tex = create_texture_2d_float(w, h, internal, format, NULL);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static int _le_ceil_div(int a, int b) {
    return (a + b - 1) / b;
}

static bool _le_build_targets(LocalExposure* le, int frame_w, int frame_h) {
    _le_free_targets(le);
    le->half_w = _le_ceil_div(frame_w, 2);
    le->half_h = _le_ceil_div(frame_h, 2);
    le->cells_w = _le_ceil_div(le->half_w, LE_CELL);
    le->cells_h = _le_ceil_div(le->half_h, LE_CELL);
    le->blur_w = _le_ceil_div(le->half_w, LE_BLOCK);
    le->blur_h = _le_ceil_div(le->half_h, LE_BLOCK);
    const int grid_w = le->cells_w * LE_TILES_X;
    const int grid_h = le->cells_h * (LE_BINS / LE_TILES_X);
    const int atlas_w = grid_w > le->blur_w ? grid_w : le->blur_w;
    const int atlas_h = grid_h + le->blur_h;
    bool ok = _le_target(le->half_w, le->half_h, GL_RGBA16F, GL_RGBA, GL_NEAREST, &le->half_tex,
                         &le->half_fbo) &&
              _le_target(le->blur_w, le->blur_h, GL_R32F, GL_RED, GL_NEAREST, &le->block_tex,
                         &le->block_fbo) &&
              _le_target(le->blur_w, le->blur_h, GL_R32F, GL_RED, GL_NEAREST, &le->across_tex,
                         &le->across_fbo);
    // The atlas filters: the tonemap's two taps are hardware bilinear within a tile.
    for (int i = 0; i < 2 && ok; i++)
        ok = _le_target(atlas_w, atlas_h, GL_RG32F, GL_RG, GL_LINEAR, &le->atlas[i],
                        &le->atlas_fbo[i]);
    if (!ok)
        return false;
    le->frame_w = frame_w;
    le->frame_h = frame_h;
    return true;
}

void local_exposure_upload_layout(const LocalExposure* le, UniformManager* u) {
    const int grid[4] = {le->cells_w, le->cells_h, LE_BINS, LE_TILES_X};
    uniform_set_ivec4(u, "leGrid", grid);
    const float range[2] = {le->range_lo, LE_STOPS / LE_BINS};
    uniform_set_vec2(u, "leRange", range);
    const int blur_rect[4] = {0, le->cells_h * (LE_BINS / LE_TILES_X), le->blur_w, le->blur_h};
    uniform_set_ivec4(u, "leBlurRect", blur_rect);
    // A frame uv's position in cells: the half-res texels it covers over a cell's.
    const float cell_scale[2] = {(float)le->half_w / LE_CELL, (float)le->half_h / LE_CELL};
    uniform_set_vec2(u, "leCellScale", cell_scale);
}

static void _le_pass(ShaderProgram* p, GLuint fbo, int x, int y, int w, int h, const char* src_name,
                     GLuint src, GLuint quad) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(x, y, w, h);
    glUseProgram(p->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    uniform_set_int(p->uniforms, src_name, 0);
    draw_fullscreen_quad(quad);
}

static int _le_float_order(const void* a, const void* b) {
    const float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}

void local_exposure_probe(const LocalExposure* le, float middle_grey) {
    if (!le || !le->half_tex)
        return;
    const size_t n = (size_t)le->half_w * le->half_h;
    float* px = malloc(sizeof(float) * n * 4);
    float* lum = malloc(sizeof(float) * n);
    if (px && lum) {
        glBindTexture(GL_TEXTURE_2D, le->half_tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, px);
        for (size_t i = 0; i < n; i++)
            lum[i] = px[i * 4 + 3] - middle_grey;
        qsort(lum, n, sizeof(float), _le_float_order);
        static const float at[] = {0.01f, 0.10f, 0.25f, 0.50f, 0.75f, 0.90f, 0.95f, 0.99f};
        printf("le-probe grey=%.3f", middle_grey);
        for (size_t k = 0; k < sizeof(at) / sizeof(at[0]); k++)
            printf(" p%02d=%.2f", (int)lroundf(at[k] * 100.0f), lum[(size_t)(at[k] * (n - 1))]);
        printf(" max=%.2f\n", lum[n - 1]);
    }
    free(px);
    free(lum);
}

GLuint local_exposure_run(LocalExposure* le, GLuint hdr_tex, int frame_w, int frame_h,
                          float middle_grey, float kernel, GLuint quad) {
    if (!le || frame_w <= 0 || frame_h <= 0)
        return 0;
    if ((frame_w != le->frame_w || frame_h != le->frame_h) &&
        !_le_build_targets(le, frame_w, frame_h)) {
        log_error("Local exposure: targets unavailable");
        return 0;
    }
    le->range_lo = middle_grey - LE_BELOW_GREY;
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    _le_pass(le->half, le->half_fbo, 0, 0, le->half_w, le->half_h, "hdrTex", hdr_tex, quad);

    // The blurred luminance: blocks, blurred across, then down into the atlas's region for it.
    // Unreal's kernel is a share of the frame's width, r half of it.
    _le_pass(le->block, le->block_fbo, 0, 0, le->blur_w, le->blur_h, "halfTex", le->half_tex, quad);
    int radius = (int)lroundf(0.5f * kernel * (float)le->blur_w);
    radius = radius < 1 ? 1 : (radius > LE_RADIUS_MAX ? LE_RADIUS_MAX : radius);
    const int blur_y = le->cells_h * (LE_BINS / LE_TILES_X);
    glUseProgram(le->blur->id);
    uniform_set_int(le->blur->uniforms, "leRadius", radius);
    const int across[4] = {1, 0, 0, 0};
    uniform_set_ivec4(le->blur->uniforms, "leStep", across);
    _le_pass(le->blur, le->across_fbo, 0, 0, le->blur_w, le->blur_h, "blockTex", le->block_tex,
             quad);
    const int down[4] = {0, 1, 0, blur_y};
    uniform_set_ivec4(le->blur->uniforms, "leStep", down);
    _le_pass(le->blur, le->atlas_fbo[0], 0, blur_y, le->blur_w, le->blur_h, "blockTex",
             le->across_tex, quad);

    // The grid, into atlas[1], then Chen's Gaussian one axis a pass: across into 0, down into 1,
    // along the bins into 0.
    const int grid_w = le->cells_w * LE_TILES_X;
    glUseProgram(le->grid->id);
    local_exposure_upload_layout(le, le->grid->uniforms);
    _le_pass(le->grid, le->atlas_fbo[1], 0, 0, grid_w, blur_y, "halfTex", le->half_tex, quad);
    glUseProgram(le->grid_blur->id);
    local_exposure_upload_layout(le, le->grid_blur->uniforms);
    for (int axis = 0; axis < 3; axis++) {
        const int dst = axis % 2 == 0 ? 0 : 1;
        uniform_set_int(le->grid_blur->uniforms, "leAxis", axis);
        _le_pass(le->grid_blur, le->atlas_fbo[dst], 0, 0, grid_w, blur_y, "gridTex",
                 le->atlas[1 - dst], quad);
    }
    check_gl_error("local exposure");
    return le->atlas[0];
}
