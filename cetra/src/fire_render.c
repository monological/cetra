#include "fire_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "fire.h"
#include "fire_internal.h"
#include "mesh.h"
#include "postfx.h"
#include "profiler.h"
#include "scene.h"
#include "texture.h"
#include "util.h"
#include "ext/log.h"

// The march's and the cards' units: the scene depth, the blackbody table, a GRID fire's scalars
// or a flipbook's sheet, the fog volume.
#define FIRE_DEPTH_UNIT     0
#define FIRE_BLACKBODY_UNIT 1
#define FIRE_SCALAR_UNIT    2
#define FIRE_FOG_UNIT       3

// A GRID fire's march step, in cells: under half a cell, since the march is not dithered (it
// draws after the temporal seam, where nothing would average a dither away) and a coarser fixed
// step prints the slices it takes as bands.
#define FIRE_MARCH_PER_CELL 0.4f
#define FIRE_MARCH_MAX      512
// A FLAME is marched at a tenth of its width.
#define FLAME_MARCH_PER_WIDTH 0.1f
#define FLAME_MARCH_MAX       128

static ShaderProgram* (*const FIRE_PROGRAM_BUILD[FIRE_PROGRAM_COUNT])(void) = {
    [FIRE_PROGRAM_ADVECT] = create_fire_advect_program,
    [FIRE_PROGRAM_CORRECT] = create_fire_correct_program,
    [FIRE_PROGRAM_CURL] = create_fire_curl_program,
    [FIRE_PROGRAM_REACT] = create_fire_react_program,
    [FIRE_PROGRAM_DIVERGENCE] = create_fire_divergence_program,
    [FIRE_PROGRAM_JACOBI] = create_fire_jacobi_program,
    [FIRE_PROGRAM_PROJECT] = create_fire_project_program,
    [FIRE_PROGRAM_REDUCE] = create_fire_reduce_program,
    [FIRE_PROGRAM_SUM] = create_fire_sum_program,
    [FIRE_PROGRAM_SLICE] = create_fire_slice_program,
    [FIRE_PROGRAM_MARCH] = create_fire_march_program,
    [FIRE_PROGRAM_CARD] = create_fire_card_program,
};

FireRenderer* create_fire_renderer(void) {
    FireRenderer* r = calloc(1, sizeof(FireRenderer));
    if (!r) {
        log_error("Failed to allocate FireRenderer");
        return NULL;
    }
    glGenFramebuffers(1, &r->fbo);
    glGenVertexArrays(1, &r->vao);
    create_fullscreen_quad_vao(&r->quad_vao, &r->quad_vbo);
    r->blackbody_lut =
        create_texture_2d_float(FIRE_BB_LUT_SIZE, 1, GL_RGBA32F, GL_RGBA, fire_blackbody_table());
    r->result_tex = create_texture_2d_float(2 * FIRE_MAX, 1, GL_RGBA32F, GL_RGBA, NULL);
    glBindTexture(GL_TEXTURE_2D, r->blackbody_lut);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenBuffers(FIRE_READBACK_LATENCY, r->pbo);
    for (int i = 0; i < FIRE_READBACK_LATENCY; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, r->pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)(2 * FIRE_MAX * 4 * sizeof(float)), NULL,
                     GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return r;
}

void free_fire_renderer(FireRenderer* r) {
    if (!r)
        return;
    for (int i = 0; i < FIRE_PROGRAM_COUNT; i++)
        if (r->programs[i])
            free_program(r->programs[i]);
    for (int i = 0; i < FIRE_MAX; i++)
        fire_grid_gpu_free(&r->grids[i]);
    gl_delete_fbo(&r->fbo);
    if (r->vao)
        glDeleteVertexArrays(1, &r->vao);
    if (r->quad_vao)
        glDeleteVertexArrays(1, &r->quad_vao);
    if (r->quad_vbo)
        glDeleteBuffers(1, &r->quad_vbo);
    gl_delete_texture(&r->blackbody_lut);
    gl_delete_texture(&r->result_tex);
    glDeleteBuffers(FIRE_READBACK_LATENCY, r->pbo);
    free(r);
}

UniformManager* fire_use(FireRenderer* r, FireProgram which) {
    if (!r->programs[which]) {
        if (r->program_failed[which])
            return NULL;
        r->programs[which] = FIRE_PROGRAM_BUILD[which]();
        if (!r->programs[which]) {
            log_error("Fire: a program would not build; what needs it will not simulate or draw");
            r->program_failed[which] = true;
            return NULL;
        }
    }
    glUseProgram(r->programs[which]->id);
    return r->programs[which]->uniforms;
}

void fire_bind(UniformManager* u, int unit, GLuint tex, const char* name) {
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(GL_TEXTURE_2D, tex);
    uniform_set_int(u, name, unit);
}

void fire_emission_uniforms(FireRenderer* r, UniformManager* u, const Fire* fire,
                            int blackbody_unit) {
    const FireParams* p = &fire->params;
    uniform_set_float(u, "ambient", p->ambient);
    uniform_set_float(u, "sootAbsorption", p->soot_absorption);
    uniform_set_float(u, "blueCore", p->blue_core);
    uniform_set_vec3(u, "blueColor", (float*)fire_blue_color());
    uniform_set_float(u, "brightness", p->brightness);
    uniform_set_mat3(u, "adaptation", (const float*)fire->adaptation);
    fire_bind(u, blackbody_unit, r->blackbody_lut, "blackbodyLut");
}

static int _steps_through(float length, float step, int cap) {
    const int n = (int)ceilf(length / step) + 2;
    return n < cap ? n : cap;
}

// The world box a GRID or FLAME fire is marched through: a grid's cells, or a flame's spine
// padded by its widest radius.
static void _march_box(const FireRenderer* r, const Fire* fire, int index, vec3 lo, vec3 hi) {
    if (fire->kind == FIRE_FLAME) {
        AABB box;
        aabb_empty(&box);
        float pad = 0.0f;
        for (int i = 0; i < FIRE_SPINE_POINTS; i++) {
            aabb_add_point(&box, fire->spine[i]);
            pad = fmaxf(pad, fire->spine[i][3]);
        }
        aabb_expand(&box, pad);
        glm_vec3_copy(box.min, lo);
        glm_vec3_copy(box.max, hi);
        return;
    }
    const FireGridGPU* g = &r->grids[index];
    for (int a = 0; a < 3; a++) {
        lo[a] = fire->origin[a] + g->lo[a];
        hi[a] = lo[a] + (float)g->dims[a] * g->cell;
    }
}

// One thing the late draw composites: a marched fire (card -1) or one card of a flipbook.
typedef struct FireDrawable {
    int fire;
    int card;
    float depth; // view-space, metres in front of the eye: what back to front means under either
                 // projection
} FireDrawable;

static float _view_depth(const Engine* engine, const vec3 p) {
    vec3 v = {0.0f, 0.0f, 0.0f};
    glm_mat4_mulv3((vec4*)engine->view_matrix, (float*)p, 1.0f, v);
    return -v[2];
}

// The frame's view, the same for every drawable.
typedef struct FireView {
    mat4 view_proj;
    mat4 inv_view_proj;
} FireView;

// A card's bottom centre in the world, and its size.
static void _card_world(const Fire* fire, const FireCard* card, vec3 base, vec2 size) {
    glm_vec3_add((float*)fire->origin, (float*)card->base, base);
    fire_card_size(fire, card, size);
}

static void _draw_card(FireRenderer* r, const Engine* engine, const Fire* fire, int c,
                       const PostFXLateDraw* late, const FireView* fv) {
    UniformManager* u = fire_use(r, FIRE_PROGRAM_CARD);
    if (!u)
        return;
    const FireFlipbook* b = &fire->flipbook;
    const FireCard* card = &fire->cards.list[c];
    vec3 base = {0.0f, 0.0f, 0.0f};
    vec2 size = {0.0f, 0.0f};
    _card_world(fire, card, base, size);
    uniform_set_mat4(u, "viewProj", (const float*)fv->view_proj);
    uniform_set_mat4(u, "view", (const float*)engine->view_matrix);
    uniform_set_mat4(u, "projection", (const float*)engine->projection_matrix);
    uniform_set_vec3(u, "cameraPos", engine->camera->position);
    uniform_set_vec3(u, "cardBase", base);
    uniform_set_vec2(u, "cardSize", size);
    const int layout[4] = {b->frames, b->cols, b->rows, 0};
    uniform_set_ivec4(u, "sheetLayout", layout);
    uniform_set_vec2(u, "frameTexels", (vec2){(float)b->width, (float)b->height});
    uniform_set_float(u, "sheetGutter", (float)b->gutter);
    uniform_set_float(u, "framePos", (float)fire_card_frame(b, card, engine->render_time));
    uniform_set_float(u, "peakNits", b->peak_nits);
    uniform_set_float(u, "brightness", fire->params.brightness);
    postfx_late_draw_bind(late, u, FIRE_DEPTH_UNIT, FIRE_FOG_UNIT);
    fire_bind(u, FIRE_SCALAR_UNIT, b->sheet->id, "sheet");
    glDisable(GL_CULL_FACE);
    glBindVertexArray(r->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void _draw_marched(FireRenderer* r, const Engine* engine, const Scene* scene,
                          const Fire* fire, int index, const PostFXLateDraw* late,
                          const FireView* fv) {
    UniformManager* u = fire_use(r, FIRE_PROGRAM_MARCH);
    if (!u)
        return;
    const FireParams* p = &fire->params;
    vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f};
    _march_box(r, fire, index, lo, hi);
    uniform_set_mat4(u, "view", (const float*)engine->view_matrix);
    uniform_set_mat4(u, "projection", (const float*)engine->projection_matrix);
    uniform_set_mat4(u, "viewProj", (const float*)fv->view_proj);
    uniform_set_mat4(u, "invViewProj", (const float*)fv->inv_view_proj);
    uniform_set_vec3(u, "ambientRadiance", (float*)scene->ambient_radiance);
    postfx_late_draw_bind(late, u, FIRE_DEPTH_UNIT, FIRE_FOG_UNIT);
    fire_emission_uniforms(r, u, fire, FIRE_BLACKBODY_UNIT);
    uniform_set_vec3(u, "boxMin", lo);
    uniform_set_vec3(u, "boxMax", hi);
    uniform_set_float(u, "smokeAlbedo", glm_clamp(p->smoke_albedo, 0.0f, 0.99f));
    const float diag = glm_vec3_distance(lo, hi);
    if (fire->kind == FIRE_FLAME) {
        const float step = fmaxf(fire->flame.width * FLAME_MARCH_PER_WIDTH, 1e-4f);
        uniform_set_int(u, "fireKind", 1);
        uniform_set_vec4_array(u, "spine", (const float*)fire->spine, FIRE_SPINE_POINTS);
        uniform_set_float(u, "flameTemperature", p->temperature);
        uniform_set_float(u, "flameSoot", p->flame_soot);
        uniform_set_float(u, "stepLength", step);
        uniform_set_int(u, "maxSteps", _steps_through(diag, step, FLAME_MARCH_MAX));
        fire_bind(u, FIRE_SCALAR_UNIT, r->blackbody_lut, "scalarTex");
    } else {
        const FireGridGPU* g = &r->grids[index];
        const float step = g->cell * FIRE_MARCH_PER_CELL;
        const int dims[4] = {g->dims[0], g->dims[1], g->dims[2], g->tiles[0]};
        uniform_set_int(u, "fireKind", 0);
        uniform_set_int(u, "floorSolid", fire->grid.floor ? 1 : 0);
        uniform_set_ivec4(u, "gridDims", dims);
        uniform_set_float(u, "cell", g->cell);
        uniform_set_float(u, "stepLength", step);
        uniform_set_int(u, "maxSteps", _steps_through(diag, step, FIRE_MARCH_MAX));
        fire_bind(u, FIRE_SCALAR_UNIT, g->scalars[0], "scalarTex");
    }
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glBindVertexArray(r->vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
}

void fire_render_slice(FireRenderer* r, const FireSystem* fs, int height) {
    if (!r || !fs)
        return;
    const int d = fs->debug_fire;
    if (fs->debug_field < 0 || d < 0 || d >= fs->count || fs->fires[d].kind != FIRE_GRID ||
        !r->grids[d].velocity[0])
        return;
    UniformManager* u = fire_use(r, FIRE_PROGRAM_SLICE);
    if (!u)
        return;
    const FireGridGPU* g = &r->grids[d];
    const GLPassState pass = gl_pass_begin();
    const int h = height / 2;
    glViewport(0, 0, h * g->dims[0] / g->dims[1], h);
    const int dims[4] = {g->dims[0], g->dims[1], g->dims[2], g->tiles[0]};
    uniform_set_ivec4(u, "gridDims", dims);
    uniform_set_int(u, "field", fs->debug_field);
    uniform_set_int(u, "slice", fs->debug_slice);
    fire_bind(u, 0, g->scalars[0], "scalarTex");
    fire_bind(u, 1, g->velocity[0], "velocityTex");
    draw_fullscreen_quad(r->quad_vao);
    gl_pass_end(&pass);
}

void fire_render_draw(FireRenderer* r, Engine* engine, const Scene* scene,
                      const PostFXLateDraw* late) {
    const FireSystem* fs = scene ? scene->fire : NULL;
    if (!r || !fs || !late || !late->scene_depth || !engine->camera)
        return;
    profiler_scope_begin(engine->profiler, "fire");
    // Every marched fire and every card, back to front, so what is seen through what composites
    // in the right order.
    FireDrawable list[FIRE_MAX * FIRE_MAX_CARDS];
    int n = 0;
    for (int i = 0; i < fs->count; i++) {
        const Fire* fire = &fs->fires[i];
        if (!fire->enabled || !fire->started)
            continue;
        if (fire->kind == FIRE_FLIPBOOK) {
            if (!fire->flipbook.sheet)
                continue;
            for (int c = 0; c < fire_card_count(fire); c++) {
                vec3 base = {0.0f, 0.0f, 0.0f};
                vec2 size = {0.0f, 0.0f};
                _card_world(fire, &fire->cards.list[c], base, size);
                base[1] += 0.5f * size[1];
                list[n++] = (FireDrawable){i, c, _view_depth(engine, base)};
            }
            continue;
        }
        if (fire->kind == FIRE_GRID && !r->grids[i].velocity[0])
            continue;
        vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f}, mid = {0.0f, 0.0f, 0.0f};
        _march_box(r, fire, i, lo, hi);
        glm_vec3_center(lo, hi, mid);
        list[n++] = (FireDrawable){i, -1, _view_depth(engine, mid)};
    }
    for (int a = 1; a < n; a++) {
        const FireDrawable d = list[a];
        int at = a;
        while (at > 0 && list[at - 1].depth < d.depth) {
            list[at] = list[at - 1];
            at--;
        }
        list[at] = d;
    }

    FireView fv;
    glm_mat4_copy(engine->view_proj, fv.view_proj);
    glm_mat4_inv(fv.view_proj, fv.inv_view_proj);
    const GLPassState pass = gl_pass_begin();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (int k = 0; k < n; k++) {
        const Fire* fire = &fs->fires[list[k].fire];
        if (list[k].card >= 0)
            _draw_card(r, engine, fire, list[k].card, late, &fv);
        else
            _draw_marched(r, engine, scene, fire, list[k].fire, late, &fv);
    }
    glBindVertexArray(0);
    gl_pass_end(&pass);
    check_gl_error("fire draw");
    profiler_scope_end(engine->profiler);
}
