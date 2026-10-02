#include "fire_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "fire.h"
#include "light.h"
#include "postfx.h"
#include "profiler.h"
#include "scene.h"
#include "texture.h"
#include "uniform.h"
#include "util.h"
#include "ext/log.h"

// The units the simulation passes bind, each pass its own ledger: inputs from 0, the solids on
// FIRE_OBSTACLE_UNIT in every pass.
#define FIRE_OBSTACLE_UNIT 7
// The march's: the scene depth, the blackbody table, a GRID fire's scalars, the fog volume.
#define FIRE_DEPTH_UNIT     0
#define FIRE_BLACKBODY_UNIT 1
#define FIRE_SCALAR_UNIT    2
#define FIRE_FOG_UNIT       3

// Samples a metre of ray takes through a GRID fire, per cell: under one a cell, so no cell is
// stepped over, and over a half, since the field is trilinear and finer buys nothing.
#define FIRE_MARCH_PER_CELL 0.6f
#define FIRE_MARCH_MAX      512
// A FLAME is marched at a tenth of its width.
#define FLAME_MARCH_PER_WIDTH 0.1f
#define FLAME_MARCH_MAX       128

static int _steps_through(float length, float step, int cap) {
    const int n = (int)ceilf(length / step) + 2;
    return n < cap ? n : cap;
}

static GLuint _atlas_texture(int w, int h, GLenum format) {
    GLuint tex = create_texture_2d_float(w, h, format, gl_transfer_format(format), NULL);
    if (format == GL_R32F) {
        // Read by texelFetch only.
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    return tex;
}

FireRenderer* create_fire_renderer(void) {
    FireRenderer* r = calloc(1, sizeof(FireRenderer));
    if (!r) {
        log_error("Failed to allocate FireRenderer");
        return NULL;
    }
    for (int i = 0; i < FIRE_PROGRAM_COUNT; i++) {
        r->programs[i] = create_fire_program((FireProgram)i);
        if (!r->programs[i])
            r->failed = true;
    }
    if (r->failed)
        log_error("Fire: a program failed to build; fires will not simulate or draw");
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

static void _free_grid(FireGridGPU* g) {
    for (int i = 0; i < 4; i++) {
        gl_delete_texture(&g->velocity[i]);
        gl_delete_texture(&g->scalars[i]);
    }
    gl_delete_texture(&g->pressure[0]);
    gl_delete_texture(&g->pressure[1]);
    gl_delete_texture(&g->divergence);
    gl_delete_texture(&g->curl);
    gl_delete_texture(&g->obstacle);
    gl_delete_texture(&g->partial[0]);
    gl_delete_texture(&g->partial[1]);
    memset(g, 0, sizeof(*g));
}

void free_fire_renderer(FireRenderer* r) {
    if (!r)
        return;
    for (int i = 0; i < FIRE_PROGRAM_COUNT; i++)
        if (r->programs[i])
            free_program(r->programs[i]);
    for (int i = 0; i < FIRE_MAX; i++)
        _free_grid(&r->grids[i]);
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

// Attach up to two targets and point the draw at them.
static void _target(FireRenderer* r, GLuint t0, GLuint t1, int x, int y, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t0, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, t1, 0);
    static const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(t1 ? 2 : 1, bufs);
    glViewport(x, y, w, h);
}

static void _bind(UniformManager* u, int unit, GLuint tex, const char* name) {
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    glBindTexture(GL_TEXTURE_2D, tex);
    uniform_set_int(u, name, unit);
}

static void _clear(FireRenderer* r, GLuint tex, int w, int h) {
    _target(r, tex, 0, 0, 0, w, h);
    glClear(GL_COLOR_BUFFER_BIT);
}

// A grid's targets, allocated for `dims` cells and cleared; kept while the cells do not change.
static bool _ensure_grid(FireRenderer* r, FireGridGPU* g, const int dims[3]) {
    if (g->velocity[0] && g->dims[0] == dims[0] && g->dims[1] == dims[1] && g->dims[2] == dims[2])
        return true;
    _free_grid(g);
    memcpy(g->dims, dims, sizeof(g->dims));
    // Square-ish, so neither side of the atlas runs into the texture size limit first.
    int across = (int)lroundf(sqrtf((float)dims[2] * (float)dims[1] / (float)dims[0]));
    across = across < 1 ? 1 : (across > dims[2] ? dims[2] : across);
    g->tiles[0] = across;
    g->tiles[1] = (dims[2] + across - 1) / across;
    g->atlas[0] = g->tiles[0] * dims[0];
    g->atlas[1] = g->tiles[1] * dims[1];
    const int w = g->atlas[0], h = g->atlas[1];
    for (int i = 0; i < 4; i++) {
        g->velocity[i] = _atlas_texture(w, h, GL_RGBA16F);
        g->scalars[i] = _atlas_texture(w, h, GL_RGBA16F);
    }
    g->pressure[0] = _atlas_texture(w, h, GL_R32F);
    g->pressure[1] = _atlas_texture(w, h, GL_R32F);
    g->divergence = _atlas_texture(w, h, GL_R32F);
    g->curl = _atlas_texture(w, h, GL_RGBA16F);
    g->partial[0] = _atlas_texture(g->tiles[0], g->tiles[1], GL_RGBA32F);
    g->partial[1] = _atlas_texture(g->tiles[0], g->tiles[1], GL_RGBA32F);
    glGenTextures(1, &g->obstacle);
    glBindTexture(GL_TEXTURE_2D, g->obstacle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Freshly allocated memory is undefined, and the state starts still, cold and empty.
    GLfloat clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    _target(r, g->velocity[0], 0, 0, 0, w, h);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Fire: a grid's framebuffer is incomplete; fires will not simulate");
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        _free_grid(g);
        r->failed = true;
        return false;
    }
    for (int i = 0; i < 4; i++) {
        _clear(r, g->velocity[i], w, h);
        _clear(r, g->scalars[i], w, h);
    }
    _clear(r, g->pressure[0], w, h);
    _clear(r, g->pressure[1], w, h);
    _clear(r, g->divergence, w, h);
    _clear(r, g->curl, w, h);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    return true;
}

static uint32_t _obstacle_key(const Fire* fire) {
    uint32_t h = fnv1a_bytes(fire->obstacles, sizeof(FireBox) * (size_t)fire->obstacle_count);
    h ^= fnv1a_bytes(fire->center, sizeof(vec3)) * 31u;
    h ^= fnv1a_bytes(fire->size, sizeof(vec3)) * 131u;
    h ^= fnv1a_bytes(&fire->cell, sizeof(float)) * 1031u;
    // Never 0, which is what a grid that has not been voxelised holds.
    return h | 1u;
}

// The solids, a cell solid when its centre is inside any obstacle box.
static void _build_obstacles(FireGridGPU* g, const Fire* fire) {
    vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f};
    fire_grid_bounds(fire, lo, hi);
    const float cell = (hi[0] - lo[0]) / (float)g->dims[0];
    const int w = g->atlas[0], h = g->atlas[1];
    unsigned char* data = calloc((size_t)w * (size_t)h, 1);
    if (!data)
        return;
    for (int z = 0; z < g->dims[2]; z++) {
        const int tx = (z % g->tiles[0]) * g->dims[0];
        const int ty = (z / g->tiles[0]) * g->dims[1];
        for (int y = 0; y < g->dims[1]; y++) {
            for (int x = 0; x < g->dims[0]; x++) {
                const vec3 p = {lo[0] + ((float)x + 0.5f) * cell, lo[1] + ((float)y + 0.5f) * cell,
                                lo[2] + ((float)z + 0.5f) * cell};
                for (int o = 0; o < fire->obstacle_count; o++) {
                    const FireBox* b = &fire->obstacles[o];
                    if (p[0] >= b->min[0] && p[0] <= b->max[0] && p[1] >= b->min[1] &&
                        p[1] <= b->max[1] && p[2] >= b->min[2] && p[2] <= b->max[2]) {
                        data[(size_t)(ty + y) * (size_t)w + (size_t)(tx + x)] = 255;
                        break;
                    }
                }
            }
        }
    }
    glBindTexture(GL_TEXTURE_2D, g->obstacle);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(data);
    g->obstacle_key = _obstacle_key(fire);
}

// What every simulation pass reads about the grid it runs over.
static void _grid_uniforms(UniformManager* u, const FireGridGPU* g, const Fire* fire,
                           const vec3 wind, float dt, float cell) {
    const int dims[4] = {g->dims[0], g->dims[1], g->dims[2], g->tiles[0]};
    uniform_set_ivec4(u, "gridDims", dims);
    uniform_set_float(u, "dt", dt);
    uniform_set_float(u, "cell", cell);
    uniform_set_vec3(u, "wind", wind);
    uniform_set_int(u, "floorSolid", fire->floor ? 1 : 0);
    _bind(u, FIRE_OBSTACLE_UNIT, g->obstacle, "obstacleTex");
}

static UniformManager* _use(FireRenderer* r, FireProgram which) {
    ShaderProgram* p = r->programs[which];
    glUseProgram(p->id);
    return p->uniforms;
}

static void _draw(FireRenderer* r) {
    glBindVertexArray(r->quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void _swap(GLuint* a, GLuint* b) {
    const GLuint t = *a;
    *a = *b;
    *b = t;
}

/*
 * One fixed step of a GRID fire: advect, then react and force, then project. The state is in
 * index 0 of each pair before and after; every pass writes a texture none of its samplers is
 * bound to, which is why each pass binds all of them.
 */
static void _step(FireRenderer* r, FireGridGPU* g, Fire* fire, const FireSystem* fs,
                  const vec3 wind, float dt, float cell, const vec3 box_min) {
    const int w = g->atlas[0], h = g->atlas[1];
    GLuint* V = g->velocity;
    GLuint* S = g->scalars;

    UniformManager* u = _use(r, FIRE_PROGRAM_ADVECT);
    _grid_uniforms(u, g, fire, wind, dt, cell);
    _bind(u, 0, V[0], "velocityTex");
    if (fs->maccormack) {
        _target(r, V[1], S[1], 0, 0, w, h);
        uniform_set_int(u, "mode", 0);
        _bind(u, 1, V[0], "fromVelocity");
        _bind(u, 2, S[0], "fromScalars");
        _bind(u, 3, V[0], "baseVelocity");
        _bind(u, 4, S[0], "baseScalars");
        _bind(u, 5, V[0], "backVelocity");
        _bind(u, 6, S[0], "backScalars");
        _draw(r);
        _target(r, V[2], S[2], 0, 0, w, h);
        uniform_set_int(u, "mode", 1);
        _bind(u, 1, V[1], "fromVelocity");
        _bind(u, 2, S[1], "fromScalars");
        _draw(r);
        _target(r, V[3], S[3], 0, 0, w, h);
        uniform_set_int(u, "mode", 2);
        _bind(u, 5, V[2], "backVelocity");
        _bind(u, 6, S[2], "backScalars");
        _draw(r);
    } else {
        _target(r, V[3], S[3], 0, 0, w, h);
        uniform_set_int(u, "mode", 0);
        _bind(u, 1, V[0], "fromVelocity");
        _bind(u, 2, S[0], "fromScalars");
        _bind(u, 3, V[0], "baseVelocity");
        _bind(u, 4, S[0], "baseScalars");
        _bind(u, 5, V[0], "backVelocity");
        _bind(u, 6, S[0], "backScalars");
        _draw(r);
    }

    u = _use(r, FIRE_PROGRAM_CURL);
    _grid_uniforms(u, g, fire, wind, dt, cell);
    _target(r, g->curl, 0, 0, 0, w, h);
    _bind(u, 0, V[3], "velocityTex");
    _draw(r);

    const FireParams* p = &fire->params;
    u = _use(r, FIRE_PROGRAM_REACT);
    _grid_uniforms(u, g, fire, wind, dt, cell);
    _target(r, V[0], S[0], 0, 0, w, h);
    _bind(u, 0, V[3], "velocityTex");
    _bind(u, 1, S[3], "scalarTex");
    _bind(u, 2, g->curl, "curlTex");
    uniform_set_vec3(u, "boxMin", (float*)box_min);
    uniform_set_int(u, "stepIndex", fire->start_step + fire->steps);
    uniform_set_float(u, "ambient", p->ambient);
    uniform_set_float(u, "ignition", p->ignition);
    uniform_set_float(u, "burnRate", p->burn_rate);
    uniform_set_float(u, "heat", p->heat);
    uniform_set_float(u, "sootYield", p->soot_yield);
    uniform_set_float(u, "buoyancy", p->buoyancy);
    uniform_set_float(u, "sootWeight", p->soot_weight);
    uniform_set_float(u, "cooling", p->cooling);
    uniform_set_float(u, "vorticity", p->vorticity);
    uniform_set_float(u, "sootBurnout", p->soot_burnout);
    uniform_set_float(u, "sootBurnoutAt", p->soot_burnout_at);
    uniform_set_float(u, "smokeFade", p->smoke_fade);
    vec4 sa[FIRE_MAX_SOURCES], sb[FIRE_MAX_SOURCES], sr[FIRE_MAX_SOURCES];
    const int sources =
        fire->source_count < FIRE_MAX_SOURCES ? fire->source_count : FIRE_MAX_SOURCES;
    for (int i = 0; i < sources; i++) {
        const FireSource* s = &fire->sources[i];
        glm_vec4_copy((vec4){s->a[0], s->a[1], s->a[2], (float)s->shape}, sa[i]);
        glm_vec4_copy((vec4){s->b[0], s->b[1], s->b[2], s->radius}, sb[i]);
        glm_vec4_copy((vec4){s->rate, s->temperature, s->lift, 0.0f}, sr[i]);
    }
    uniform_set_int(u, "sourceCount", sources);
    if (sources > 0) {
        uniform_set_vec4_array(u, "sourceA", (const float*)sa, sources);
        uniform_set_vec4_array(u, "sourceB", (const float*)sb, sources);
        uniform_set_vec4_array(u, "sourceRate", (const float*)sr, sources);
    }
    _draw(r);

    u = _use(r, FIRE_PROGRAM_DIVERGENCE);
    _grid_uniforms(u, g, fire, wind, dt, cell);
    _target(r, g->divergence, 0, 0, 0, w, h);
    _bind(u, 0, V[0], "velocityTex");
    _draw(r);

    u = _use(r, FIRE_PROGRAM_JACOBI);
    _grid_uniforms(u, g, fire, wind, dt, cell);
    _bind(u, 1, g->divergence, "divergenceTex");
    for (int i = 0; i < fs->jacobi_iterations; i++) {
        _target(r, g->pressure[1], 0, 0, 0, w, h);
        _bind(u, 0, g->pressure[0], "pressureTex");
        _draw(r);
        _swap(&g->pressure[0], &g->pressure[1]);
    }

    u = _use(r, FIRE_PROGRAM_PROJECT);
    _grid_uniforms(u, g, fire, wind, dt, cell);
    _target(r, V[1], 0, 0, 0, w, h);
    _bind(u, 0, V[0], "velocityTex");
    _bind(u, 1, g->pressure[0], "pressureTex");
    _draw(r);
    _swap(&V[0], &V[1]);
}

// A GRID fire's two sums, into its two texels of the result row: what the ring reads back and
// what the probe compares against the CPU.
static void _reduce(FireRenderer* r, FireGridGPU* g, const Fire* fire, int index) {
    vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f};
    fire_grid_bounds(fire, lo, hi);
    const float cell = (hi[0] - lo[0]) / (float)g->dims[0];
    const FireParams* p = &fire->params;
    UniformManager* u = _use(r, FIRE_PROGRAM_REDUCE);
    const int dims[4] = {g->dims[0], g->dims[1], g->dims[2], g->tiles[0]};
    uniform_set_ivec4(u, "gridDims", dims);
    uniform_set_vec3(u, "boxMin", lo);
    uniform_set_float(u, "cell", cell);
    uniform_set_float(u, "ambient", p->ambient);
    uniform_set_float(u, "sootAbsorption", p->soot_absorption);
    uniform_set_float(u, "blueCore", p->blue_core);
    uniform_set_float(u, "heat", p->heat);
    uniform_set_float(u, "brightness", p->brightness);
    _bind(u, 1, r->blackbody_lut, "blackbodyLut");

    uniform_set_int(u, "mode", 0);
    _target(r, g->partial[0], g->partial[1], 0, 0, g->tiles[0], g->tiles[1]);
    _bind(u, 0, g->scalars[0], "scalarTex");
    // Not read in this mode, and bound away from the targets it writes.
    _bind(u, 2, r->blackbody_lut, "partial0");
    _bind(u, 3, r->blackbody_lut, "partial1");
    _draw(r);

    uniform_set_int(u, "mode", 1);
    const int tiles[4] = {g->tiles[0], g->tiles[1], 0, 0};
    uniform_set_ivec4(u, "tiles", tiles);
    uniform_set_int(u, "resultBase", 2 * index);
    _target(r, r->result_tex, 0, 2 * index, 0, 2, 1);
    _bind(u, 2, g->partial[0], "partial0");
    _bind(u, 3, g->partial[1], "partial1");
    _draw(r);
}

// One fire's two texels, as fire.h states them.
static void _take(Fire* fire, const float* t) {
    fire->intensity = t[0];
    if (t[0] > 0.0f)
        glm_vec3_scale((vec3){t[1], t[2], t[3]}, 1.0f / t[0], fire->centroid);
    else
        glm_vec3_copy(fire->center, fire->centroid);
    const float lum = 0.2126f * t[4] + 0.7152f * t[5] + 0.0722f * t[6];
    if (lum > 0.0f)
        glm_vec3_scale((vec3){t[4], t[5], t[6]}, 1.0f / lum, fire->color);
    fire->heat_release = t[7];
    fire->answered = true;
}

/*
 * The ring: retire the slot issued FIRE_READBACK_LATENCY frames ago, then issue this frame's
 * sums into it. No fence: the slot is always that old, and a read that has not landed stalls
 * the map rather than answering early, so the latency changes only how often a stall happens and
 * never the answer -- which is what keeps a headless run equal to itself.
 */
static void _readback(FireRenderer* r, FireSystem* fs) {
    const int slot = r->ring_passes % FIRE_READBACK_LATENCY;
    const GLsizeiptr bytes = (GLsizeiptr)(2 * FIRE_MAX * 4 * sizeof(float));
    if (r->ring_passes >= FIRE_READBACK_LATENCY) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, r->pbo[slot]);
        const float* data = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        if (data) {
            for (int i = 0; i < fs->count; i++)
                if (r->slot_issued[slot][i])
                    _take(&fs->fires[i], data + 8 * i);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        } else {
            log_error("Fire: the light readback could not be mapped");
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    for (int i = 0; i < FIRE_MAX; i++) {
        const Fire* fire = i < fs->count ? &fs->fires[i] : NULL;
        const bool issue = fire && fire->kind == FIRE_GRID && fire->enabled &&
                           r->grids[i].velocity[0] && fire->steps > 0;
        r->slot_issued[slot][i] = issue;
        if (issue)
            _reduce(r, &r->grids[i], fire, i);
    }
    _target(r, r->result_tex, 0, 0, 0, 2 * FIRE_MAX, 1);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, r->pbo[slot]);
    glReadPixels(0, 0, 2 * FIRE_MAX, 1, GL_RGBA, GL_FLOAT, NULL);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    r->ring_passes++;
}

void fire_simulate(FireRenderer* r, Engine* engine, Scene* scene) {
    FireSystem* fs = scene ? scene->fire : NULL;
    if (!r || !fs)
        return;
    bool any_grid = false;
    for (int i = 0; i < fs->count; i++)
        any_grid |= fs->fires[i].kind == FIRE_GRID && fs->fires[i].enabled;
    if (any_grid && !r->failed) {
        profiler_scope_begin(engine->profiler, "fire sim");
        const GLPassState pass = gl_pass_begin();
        vec3 air = {0.0f, 0.0f, 0.0f};
        fire_wind_air(scene->wind, engine->render_time, air);
        const float dt = 1.0f / fs->sim_hz;
        for (int i = 0; i < fs->count && !r->failed; i++) {
            Fire* fire = &fs->fires[i];
            if (fire->kind != FIRE_GRID || !fire->enabled)
                continue;
            FireGridGPU* g = &r->grids[i];
            if (!_ensure_grid(r, g, fire->grid))
                break;
            if (g->obstacle_key != _obstacle_key(fire))
                _build_obstacles(g, fire);
            vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f}, wind = {0.0f, 0.0f, 0.0f};
            fire_grid_bounds(fire, lo, hi);
            const float cell = (hi[0] - lo[0]) / (float)g->dims[0];
            glm_vec3_scale(air, fire->params.wind_response, wind);
            for (int s = 0; s < fire->pending; s++) {
                _step(r, g, fire, fs, wind, dt, cell, lo);
                fire->steps++;
            }
            fire->pending = 0;
        }
        if (!r->failed)
            _readback(r, fs);
        gl_pass_end(&pass);
        check_gl_error("fire sim");
        profiler_scope_end(engine->profiler);
    }
    for (int i = 0; i < fs->count; i++)
        fire_drive_light(&fs->fires[i], fs->fires[i].light);
}

// A FLAME's box: its spine's, padded by the widest radius.
static void _flame_box(const Fire* fire, vec3 lo, vec3 hi) {
    glm_vec3_copy((vec3){1e30f, 1e30f, 1e30f}, lo);
    glm_vec3_copy((vec3){-1e30f, -1e30f, -1e30f}, hi);
    float pad = 0.0f;
    for (int i = 0; i < FIRE_SPINE_POINTS; i++) {
        glm_vec3_minv(lo, (float*)fire->spine[i], lo);
        glm_vec3_maxv(hi, (float*)fire->spine[i], hi);
        pad = fmaxf(pad, fire->spine[i][3]);
    }
    glm_vec3_subs(lo, pad, lo);
    glm_vec3_adds(hi, pad, hi);
}

static void _box(const Fire* fire, vec3 lo, vec3 hi) {
    if (fire->kind == FIRE_FLAME)
        _flame_box(fire, lo, hi);
    else
        fire_grid_bounds(fire, lo, hi);
}

void fire_render_draw(FireRenderer* r, Engine* engine, const Scene* scene,
                      const PostFXLateDraw* late) {
    const FireSystem* fs = scene ? scene->fire : NULL;
    if (!r || r->failed || !fs || !late || !late->scene_depth || !engine->camera)
        return;
    profiler_scope_begin(engine->profiler, "fire");
    // Back to front, so one fire seen through another composites in the right order.
    int order[FIRE_MAX];
    float dist[FIRE_MAX];
    int n = 0;
    for (int i = 0; i < fs->count; i++) {
        const Fire* fire = &fs->fires[i];
        if (!fire->enabled || !fire->started)
            continue;
        if (fire->kind == FIRE_GRID && !r->grids[i].velocity[0])
            continue;
        vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f}, mid = {0.0f, 0.0f, 0.0f};
        _box(fire, lo, hi);
        glm_vec3_center(lo, hi, mid);
        dist[i] = glm_vec3_distance2(mid, engine->camera->position);
        int at = n++;
        while (at > 0 && dist[order[at - 1]] < dist[i]) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
    }

    ShaderProgram* program = r->programs[FIRE_PROGRAM_MARCH];
    glUseProgram(program->id);
    UniformManager* u = program->uniforms;
    mat4 view_proj, inv_view_proj;
    glm_mat4_mul(engine->projection_matrix, engine->view_matrix, view_proj);
    glm_mat4_inv(view_proj, inv_view_proj);
    uniform_set_mat4(u, "view", (const float*)engine->view_matrix);
    uniform_set_mat4(u, "projection", (const float*)engine->projection_matrix);
    uniform_set_mat4(u, "viewProj", (const float*)view_proj);
    uniform_set_mat4(u, "invViewProj", (const float*)inv_view_proj);
    uniform_set_vec2(u, "viewport", (vec2){(float)late->width, (float)late->height});
    uniform_set_vec3(u, "ambientRadiance", scene->ambient_radiance);
    glActiveTexture(GL_TEXTURE0 + FIRE_FOG_UNIT);
    glBindTexture(GL_TEXTURE_3D, late->fog_volume);
    uniform_set_int(u, "fogVolume", FIRE_FOG_UNIT);
    uniform_set_int(u, "fogSlices", late->fog_slices);
    uniform_set_float(u, "fogNear", late->fog_near);
    uniform_set_float(u, "fogFar", late->fog_far);
    uniform_set_float(u, "fogDepthDist", late->fog_depth_dist);
    _bind(u, FIRE_BLACKBODY_UNIT, r->blackbody_lut, "blackbodyLut");
    _bind(u, FIRE_DEPTH_UNIT, late->scene_depth, "sceneDepth");

    const GLboolean depth_test = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cull = glIsEnabled(GL_CULL_FACE);
    GLint cull_mode = GL_BACK;
    glGetIntegerv(GL_CULL_FACE_MODE, &cull_mode);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(r->vao);
    for (int k = 0; k < n; k++) {
        const int i = order[k];
        const Fire* fire = &fs->fires[i];
        const FireParams* p = &fire->params;
        vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f};
        _box(fire, lo, hi);
        uniform_set_vec3(u, "boxMin", lo);
        uniform_set_vec3(u, "boxMax", hi);
        uniform_set_float(u, "ambient", p->ambient);
        uniform_set_float(u, "sootAbsorption", p->soot_absorption);
        uniform_set_float(u, "smokeAlbedo", glm_clamp(p->smoke_albedo, 0.0f, 0.99f));
        uniform_set_float(u, "blueCore", p->blue_core);
        uniform_set_float(u, "brightness", p->brightness);
        const float diag = glm_vec3_distance(lo, hi);
        if (fire->kind == FIRE_FLAME) {
            const float step = fmaxf(fire->size[0] * FLAME_MARCH_PER_WIDTH, 1e-4f);
            uniform_set_int(u, "fireKind", 1);
            uniform_set_vec4_array(u, "spine", (const float*)fire->spine, FIRE_SPINE_POINTS);
            uniform_set_float(u, "flameTemperature", p->flame_temperature);
            uniform_set_float(u, "flameSoot", p->flame_soot);
            uniform_set_float(u, "stepLength", step);
            uniform_set_int(u, "maxSteps", _steps_through(diag, step, FLAME_MARCH_MAX));
            _bind(u, FIRE_SCALAR_UNIT, r->blackbody_lut, "scalarTex");
        } else {
            const FireGridGPU* g = &r->grids[i];
            const float cell = (hi[0] - lo[0]) / (float)g->dims[0];
            const float step = cell * FIRE_MARCH_PER_CELL;
            const int dims[4] = {g->dims[0], g->dims[1], g->dims[2], g->tiles[0]};
            uniform_set_int(u, "fireKind", 0);
            uniform_set_ivec4(u, "gridDims", dims);
            uniform_set_float(u, "cell", cell);
            uniform_set_float(u, "stepLength", step);
            uniform_set_int(u, "maxSteps", _steps_through(diag, step, FIRE_MARCH_MAX));
            _bind(u, FIRE_SCALAR_UNIT, g->scalars[0], "scalarTex");
        }
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    glBindVertexArray(0);
    glCullFace((GLenum)cull_mode);
    if (!cull)
        glDisable(GL_CULL_FACE);
    if (depth_test)
        glEnable(GL_DEPTH_TEST);

    // --fire-slice, opaque, into the frame's lower-left third.
    const int d = fs->debug_fire;
    if (fs->debug_field >= 0 && d >= 0 && d < fs->count && fs->fires[d].kind == FIRE_GRID &&
        r->grids[d].velocity[0]) {
        const FireGridGPU* g = &r->grids[d];
        glDisable(GL_BLEND);
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        const int h = late->height / 2;
        const int w = h * g->dims[0] / g->dims[1];
        glViewport(0, 0, w, h);
        u = _use(r, FIRE_PROGRAM_SLICE);
        const int dims[4] = {g->dims[0], g->dims[1], g->dims[2], g->tiles[0]};
        uniform_set_ivec4(u, "gridDims", dims);
        uniform_set_int(u, "field", fs->debug_field);
        uniform_set_int(u, "slice", fs->debug_slice);
        _bind(u, 0, g->scalars[0], "scalarTex");
        _bind(u, 1, g->velocity[0], "velocityTex");
        _draw(r);
        glBindVertexArray(0);
        glViewport(vp[0], vp[1], vp[2], vp[3]);
    }
    // Back to the chain's resting state, as the rain leaves it.
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    check_gl_error("fire draw");
    profiler_scope_end(engine->profiler);
}

void fire_probe_grids(Engine* engine, const Scene* scene) {
    if (engine)
        fire_render_probe(engine->fire_renderer, engine, scene);
}

static void _read_texture(GLuint tex, GLenum format, GLenum type, void* out) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, format, type, out);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void fire_render_probe(FireRenderer* r, Engine* engine, const Scene* scene) {
    FireSystem* fs = scene ? scene->fire : NULL;
    if (!r || !fs || r->failed)
        return;
    (void)engine;
    const GLPassState pass = gl_pass_begin();
    for (int i = 0; i < fs->count; i++) {
        Fire* fire = &fs->fires[i];
        FireGridGPU* g = &r->grids[i];
        if (fire->kind != FIRE_GRID || !g->velocity[0])
            continue;
        const int w = g->atlas[0], h = g->atlas[1];
        const size_t texels = (size_t)w * (size_t)h;
        float* sca = malloc(texels * 4 * sizeof(float));
        float* vel = malloc(texels * 4 * sizeof(float));
        float* div = malloc(texels * sizeof(float));
        unsigned char* solid = malloc(texels);
        if (!sca || !vel || !div || !solid) {
            free(sca);
            free(vel);
            free(div);
            free(solid);
            continue;
        }
        _read_texture(g->scalars[0], GL_RGBA, GL_FLOAT, sca);
        _read_texture(g->velocity[0], GL_RGBA, GL_FLOAT, vel);
        _read_texture(g->divergence, GL_RED, GL_FLOAT, div);
        _read_texture(g->obstacle, GL_RED, GL_UNSIGNED_BYTE, solid);

        vec3 lo = {0.0f, 0.0f, 0.0f}, hi = {0.0f, 0.0f, 0.0f};
        fire_grid_bounds(fire, lo, hi);
        const float cell = (hi[0] - lo[0]) / (float)g->dims[0];
        const float volume = cell * cell * cell;
        const FireParams* p = &fire->params;
        const int X = g->dims[0], Y = g->dims[1], Z = g->dims[2];
#define TEXEL(x, y, z) \
    ((size_t)(((z) / g->tiles[0]) * Y + (y)) * (size_t)w + (size_t)(((z) % g->tiles[0]) * X + (x)))
        double peak = 0.0, fuel = 0.0, soot = 0.0, solid_heat = 0.0, solid_soot = 0.0;
        double pre_max = 0.0, pre_sq = 0.0, post_max = 0.0, post_sq = 0.0, intensity = 0.0;
        double m[3] = {0.0, 0.0, 0.0};
        int fluid = 0;
        for (int z = 0; z < Z; z++) {
            for (int y = 0; y < Y; y++) {
                for (int x = 0; x < X; x++) {
                    const size_t t = TEXEL(x, y, z);
                    const float* s = &sca[4 * t];
                    if (solid[t]) {
                        solid_heat = fmax(solid_heat, (double)s[0]);
                        solid_soot = fmax(solid_soot, (double)s[2]);
                        continue;
                    }
                    peak = fmax(peak, (double)s[0]);
                    fuel += (double)s[1];
                    soot += (double)s[2];
                    vec3 rgb = {0.0f, 0.0f, 0.0f};
                    const float lum = fire_blackbody(p->ambient + fmaxf(s[0], 0.0f), rgb);
                    const double e = ((double)(fmaxf(s[2], 0.0f) * p->soot_absorption * lum) +
                                      (double)(p->blue_core * fmaxf(s[3], 0.0f))) *
                                     (double)volume * (double)p->brightness;
                    intensity += e;
                    m[0] += e * (double)(lo[0] + ((float)x + 0.5f) * cell);
                    m[1] += e * (double)(lo[1] + ((float)y + 0.5f) * cell);
                    m[2] += e * (double)(lo[2] + ((float)z + 0.5f) * cell);
                    // The divergence the last projection left, by the stencil the GPU used.
                    double d = 0.0;
                    for (int a = 0; a < 3; a++) {
                        int q[2][3] = {{x, y, z}, {x, y, z}};
                        q[0][a] += 1;
                        q[1][a] -= 1;
                        double v[2];
                        for (int k = 0; k < 2; k++) {
                            // fireNeighbourVelocity: a solid floor is still, an open face takes
                            // the edge cell's own velocity.
                            if (q[k][1] < 0 && fire->floor) {
                                v[k] = 0.0;
                                continue;
                            }
                            const int qx = q[k][0] < 0 ? 0 : (q[k][0] >= X ? X - 1 : q[k][0]);
                            const int qy = q[k][1] < 0 ? 0 : (q[k][1] >= Y ? Y - 1 : q[k][1]);
                            const int qz = q[k][2] < 0 ? 0 : (q[k][2] >= Z ? Z - 1 : q[k][2]);
                            const size_t qt = TEXEL(qx, qy, qz);
                            const bool inside = qx == q[k][0] && qy == q[k][1] && qz == q[k][2];
                            v[k] = inside && solid[qt] ? 0.0 : (double)vel[4 * qt + (size_t)a];
                        }
                        d += v[0] - v[1];
                    }
                    d /= 2.0 * (double)cell;
                    post_max = fmax(post_max, fabs(d));
                    post_sq += d * d;
                    pre_max = fmax(pre_max, fabs((double)div[t]));
                    pre_sq += (double)div[t] * (double)div[t];
                    fluid++;
                }
            }
        }
#undef TEXEL
        // The same sums on the GPU, this frame, rather than the ring's three frames ago.
        _reduce(r, g, fire, i);
        float res[2 * FIRE_MAX * 4];
        _read_texture(r->result_tex, GL_RGBA, GL_FLOAT, res);
        const float* got = &res[8 * i];
        const double inv = intensity > 0.0 ? 1.0 / intensity : 0.0;
        printf("fire-probe grid index=%d nx=%d ny=%d nz=%d tiles=%dx%d cell=%.9g fluid=%d "
               "peak_kelvin=%.9g fuel=%.9g soot=%.9g solid_heat=%.9g solid_soot=%.9g "
               "div_pre_max=%.9g div_pre_rms=%.9g div_post_max=%.9g div_post_rms=%.9g "
               "cpu_intensity=%.9g gpu_intensity=%.9g cpu_cx=%.9g cpu_cy=%.9g cpu_cz=%.9g "
               "gpu_cx=%.9g gpu_cy=%.9g gpu_cz=%.9g heat_release=%.9g\n",
               i, X, Y, Z, g->tiles[0], g->tiles[1], (double)cell, fluid, (double)p->ambient + peak,
               fuel, soot, solid_heat, solid_soot, pre_max, fluid ? sqrt(pre_sq / fluid) : 0.0,
               post_max, fluid ? sqrt(post_sq / fluid) : 0.0, intensity, (double)got[0], m[0] * inv,
               m[1] * inv, m[2] * inv, got[0] > 0.0f ? (double)(got[1] / got[0]) : 0.0,
               got[0] > 0.0f ? (double)(got[2] / got[0]) : 0.0,
               got[0] > 0.0f ? (double)(got[3] / got[0]) : 0.0, (double)got[7]);
        free(sca);
        free(vel);
        free(div);
        free(solid);
    }
    for (int i = 0; i < fs->count; i++) {
        const Light* l = fs->fires[i].light;
        if (!l)
            continue;
        printf("fire-probe light index=%d type=%d intensity=%.9g x=%.9g y=%.9g z=%.9g "
               "area=%.9g\n",
               i, (int)l->type, (double)l->intensity, (double)l->global_position[0],
               (double)l->global_position[1], (double)l->global_position[2],
               (double)(l->size[0] * l->size[1]));
    }
    gl_pass_end(&pass);
}
