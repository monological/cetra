#include "fire_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "fire.h"
#include "fire_internal.h"
#include "light.h"
#include "profiler.h"
#include "scene.h"
#include "texture.h"
#include "util.h"
#include "ext/log.h"

/*
 * A GRID fire stepped on the GPU (spec 13.14), after GPU Gems 3's rasterised fluid passes: every
 * field a 2D atlas of the grid's z slices, every pass a fullscreen draw into one, and what the
 * fire casts summed on the GPU and read back through a fixed-latency ring. The simulation runs
 * in the box's LOCAL frame: nothing in it depends on where the fire's origin is, so a fire
 * carried along keeps its gas, and only what is drawn and cast is placed in the world.
 */

// Each simulation pass's own unit ledger: inputs from 0, the solids on FIRE_OBSTACLE_UNIT.
#define FIRE_OBSTACLE_UNIT 7

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

void fire_grid_gpu_free(FireGridGPU* g) {
    for (int i = 0; i < 4; i++) {
        gl_delete_texture(&g->velocity[i]);
        gl_delete_texture(&g->scalars[i]);
    }
    gl_delete_texture(&g->pressure[0]);
    gl_delete_texture(&g->pressure[1]);
    gl_delete_texture(&g->divergence);
    gl_delete_texture(&g->curl);
    gl_delete_texture(&g->obstacle);
    for (int i = 0; i < 2; i++) {
        gl_delete_texture(&g->rows[i]);
        gl_delete_texture(&g->slices[i]);
    }
    memset(g, 0, sizeof(*g));
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

// To zero, without touching the clear colour anyone else relies on.
static void _clear(FireRenderer* r, GLuint tex, int w, int h) {
    static const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    _target(r, tex, 0, 0, 0, w, h);
    glClearBufferfv(GL_COLOR, 0, zero);
}

static void _draw(const FireRenderer* r) {
    draw_fullscreen_quad(r->quad_vao);
}

static void _swap(GLuint* a, GLuint* b) {
    const GLuint t = *a;
    *a = *b;
    *b = t;
}

// A grid's targets, allocated for `dims` cells and cleared; kept while the cells do not change.
static bool _ensure_grid(FireRenderer* r, FireGridGPU* g, const int dims[3], const char* name) {
    if (g->velocity[0] && g->dims[0] == dims[0] && g->dims[1] == dims[1] && g->dims[2] == dims[2])
        return true;
    if (g->failed)
        return false;
    fire_grid_gpu_free(g);
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
    for (int i = 0; i < 2; i++) {
        g->rows[i] = _atlas_texture(dims[1], dims[2], GL_RGBA32F);
        g->slices[i] = _atlas_texture(dims[2], 1, GL_RGBA32F);
    }
    glGenTextures(1, &g->obstacle);
    glBindTexture(GL_TEXTURE_2D, g->obstacle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    _target(r, g->velocity[0], 0, 0, 0, w, h);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Fire: '%s''s grid framebuffer is incomplete; it will not simulate", name);
        fire_grid_gpu_free(g);
        g->failed = true;
        return false;
    }
    // Freshly allocated memory is undefined, and the state starts still, cold and empty.
    for (int i = 0; i < 4; i++) {
        _clear(r, g->velocity[i], w, h);
        _clear(r, g->scalars[i], w, h);
    }
    _clear(r, g->pressure[0], w, h);
    _clear(r, g->pressure[1], w, h);
    _clear(r, g->divergence, w, h);
    _clear(r, g->curl, w, h);
    return true;
}

static uint32_t _obstacle_key(const Fire* fire) {
    const FireGrid* grid = &fire->grid;
    uint32_t h = fnv1a_bytes(grid->obstacles, sizeof(FireBox) * (size_t)fire_obstacle_count(fire));
    h ^= fnv1a_bytes(grid->center, sizeof(vec3)) * 31u;
    h ^= fnv1a_bytes(grid->size, sizeof(vec3)) * 131u;
    h ^= fnv1a_bytes(&grid->cell, sizeof(float)) * 1031u;
    // Never 0, which is what a grid that has not been voxelised holds.
    return h | 1u;
}

// The atlas texel of cell (x, y, z): its slice's tile, then the cell within it.
static size_t _atlas_index(const FireGridGPU* g, int x, int y, int z) {
    const int tx = (z % g->tiles[0]) * g->dims[0];
    const int ty = (z / g->tiles[0]) * g->dims[1];
    return (size_t)(ty + y) * (size_t)g->atlas[0] + (size_t)(tx + x);
}

// The solids, a cell solid when its centre is inside any obstacle box.
static void _build_obstacles(FireGridGPU* g, const Fire* fire) {
    unsigned char* data = calloc((size_t)g->atlas[0] * (size_t)g->atlas[1], 1);
    if (!data)
        return;
    for (int z = 0; z < g->dims[2]; z++) {
        for (int y = 0; y < g->dims[1]; y++) {
            for (int x = 0; x < g->dims[0]; x++) {
                const vec3 p = {g->lo[0] + ((float)x + 0.5f) * g->cell,
                                g->lo[1] + ((float)y + 0.5f) * g->cell,
                                g->lo[2] + ((float)z + 0.5f) * g->cell};
                for (int o = 0; o < fire_obstacle_count(fire); o++) {
                    const FireBox* b = &fire->grid.obstacles[o];
                    if (p[0] >= b->min[0] && p[0] <= b->max[0] && p[1] >= b->min[1] &&
                        p[1] <= b->max[1] && p[2] >= b->min[2] && p[2] <= b->max[2]) {
                        data[_atlas_index(g, x, y, z)] = 255;
                        break;
                    }
                }
            }
        }
    }
    glBindTexture(GL_TEXTURE_2D, g->obstacle);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g->atlas[0], g->atlas[1], GL_RED, GL_UNSIGNED_BYTE,
                    data);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(data);
    g->obstacle_key = _obstacle_key(fire);
}

static void _grid_dims(UniformManager* u, const FireGridGPU* g) {
    const int dims[4] = {g->dims[0], g->dims[1], g->dims[2], g->tiles[0]};
    uniform_set_ivec4(u, "gridDims", dims);
}

// What every simulation pass reads about the grid it runs over.
static void _grid_uniforms(UniformManager* u, const FireGridGPU* g, const Fire* fire,
                           const vec3 wind, float dt) {
    _grid_dims(u, g);
    uniform_set_float(u, "dt", dt);
    uniform_set_float(u, "cell", g->cell);
    uniform_set_vec3(u, "wind", (float*)wind);
    uniform_set_int(u, "floorSolid", fire->grid.floor ? 1 : 0);
    fire_bind(u, FIRE_OBSTACLE_UNIT, g->obstacle, "obstacleTex");
}

/*
 * One fixed step of a GRID fire: advect, then react and force, then project. The state is in
 * index 0 of each pair before and after; every pass writes a texture none of its samplers is
 * bound to, which is why each pass binds all of them.
 */
static void _step(FireRenderer* r, FireGridGPU* g, const Fire* fire, const FireSystem* fs,
                  const vec3 wind, float dt) {
    const int w = g->atlas[0], h = g->atlas[1];
    GLuint* V = g->velocity;
    GLuint* S = g->scalars;
    const FireGrid* grid = &fire->grid;

    // Advection: a semi-Lagrangian trace into the final pair, or MacCormack's forward trace, the
    // trace back from it, and their correction.
    UniformManager* u = fire_use(r, FIRE_PROGRAM_ADVECT);
    _grid_uniforms(u, g, fire, wind, dt);
    fire_bind(u, 0, V[0], "velocityTex");
    fire_bind(u, 1, V[0], "fromVelocity");
    fire_bind(u, 2, S[0], "fromScalars");
    const int first = fs->maccormack ? 1 : 3;
    _target(r, V[first], S[first], 0, 0, w, h);
    _draw(r);
    if (fs->maccormack) {
        uniform_set_float(u, "dt", -dt);
        fire_bind(u, 1, V[1], "fromVelocity");
        fire_bind(u, 2, S[1], "fromScalars");
        _target(r, V[2], S[2], 0, 0, w, h);
        _draw(r);
        u = fire_use(r, FIRE_PROGRAM_CORRECT);
        _grid_uniforms(u, g, fire, wind, dt);
        fire_bind(u, 0, V[0], "velocityTex");
        fire_bind(u, 1, V[1], "fromVelocity");
        fire_bind(u, 2, S[1], "fromScalars");
        fire_bind(u, 3, V[0], "baseVelocity");
        fire_bind(u, 4, S[0], "baseScalars");
        fire_bind(u, 5, V[2], "backVelocity");
        fire_bind(u, 6, S[2], "backScalars");
        _target(r, V[3], S[3], 0, 0, w, h);
        _draw(r);
    }

    u = fire_use(r, FIRE_PROGRAM_CURL);
    _grid_uniforms(u, g, fire, wind, dt);
    _target(r, g->curl, 0, 0, 0, w, h);
    fire_bind(u, 0, V[3], "velocityTex");
    _draw(r);

    const FireParams* p = &fire->params;
    u = fire_use(r, FIRE_PROGRAM_REACT);
    _grid_uniforms(u, g, fire, wind, dt);
    _target(r, V[0], S[0], 0, 0, w, h);
    fire_bind(u, 0, V[3], "velocityTex");
    fire_bind(u, 1, S[3], "scalarTex");
    fire_bind(u, 2, g->curl, "curlTex");
    uniform_set_vec3(u, "boxMin", g->lo);
    // The fire's own clock, from its step count, so a headless run's noise is its own.
    uniform_set_float(u, "time", (float)(fire->start_step + fire->steps) * dt);
    uniform_set_float(u, "ambient", p->ambient);
    uniform_set_float(u, "peakRise", fmaxf(p->temperature - p->ambient, 1.0f));
    uniform_set_float(u, "reactionRate", p->reaction_rate);
    uniform_set_float(u, "coolingRate", fire_cooling_rate(p));
    uniform_set_float(u, "entrainment", p->entrainment);
    uniform_set_float(u, "core", p->core);
    uniform_set_float(u, "sootYield", p->soot_yield);
    uniform_set_float(u, "sootBurnout", p->soot_burnout);
    uniform_set_float(u, "sootBurnoutAt", p->soot_burnout_at);
    uniform_set_float(u, "smokeFade", p->smoke_fade);
    uniform_set_float(u, "buoyancy", p->buoyancy);
    uniform_set_float(u, "vorticity", p->vorticity);
    uniform_set_vec3(u, "draftMin", (float*)grid->draft.min);
    uniform_set_vec3(u, "draftMax", (float*)grid->draft.max);
    uniform_set_float(u, "draftSpeed", fmaxf(grid->draft_speed, 0.0f));
    vec4 sa[FIRE_MAX_SOURCES], sb[FIRE_MAX_SOURCES], sp[FIRE_MAX_SOURCES];
    const int sources =
        grid->source_count < FIRE_MAX_SOURCES ? grid->source_count : FIRE_MAX_SOURCES;
    for (int i = 0; i < sources; i++) {
        const FireSource* s = &grid->sources[i];
        glm_vec4_copy((vec4){s->a[0], s->a[1], s->a[2], (float)s->shape}, sa[i]);
        glm_vec4_copy((vec4){s->b[0], s->b[1], s->b[2], s->radius}, sb[i]);
        glm_vec4_copy((vec4){glm_clamp(s->coverage, 0.0f, 1.0f), s->lift, 0.0f, 0.0f}, sp[i]);
    }
    uniform_set_int(u, "sourceCount", sources);
    if (sources > 0) {
        uniform_set_vec4_array(u, "sourceA", (const float*)sa, sources);
        uniform_set_vec4_array(u, "sourceB", (const float*)sb, sources);
        uniform_set_vec4_array(u, "sourceParams", (const float*)sp, sources);
    }
    _draw(r);

    u = fire_use(r, FIRE_PROGRAM_DIVERGENCE);
    _grid_uniforms(u, g, fire, wind, dt);
    _target(r, g->divergence, 0, 0, 0, w, h);
    fire_bind(u, 0, V[0], "velocityTex");
    fire_bind(u, 1, S[0], "scalarTex");
    uniform_set_float(u, "expansion", p->expansion);
    _draw(r);

    u = fire_use(r, FIRE_PROGRAM_JACOBI);
    _grid_uniforms(u, g, fire, wind, dt);
    fire_bind(u, 1, g->divergence, "divergenceTex");
    for (int i = 0; i < fs->jacobi_iterations; i++) {
        _target(r, g->pressure[1], 0, 0, 0, w, h);
        fire_bind(u, 0, g->pressure[0], "pressureTex");
        _draw(r);
        _swap(&g->pressure[0], &g->pressure[1]);
    }

    u = fire_use(r, FIRE_PROGRAM_PROJECT);
    _grid_uniforms(u, g, fire, wind, dt);
    _target(r, V[1], 0, 0, 0, w, h);
    fire_bind(u, 0, V[0], "velocityTex");
    fire_bind(u, 1, g->pressure[0], "pressureTex");
    _draw(r);
    _swap(&V[0], &V[1]);
}

// The `count` texels along each row of `from` summed into `to`, `rows` of them from texel `x`.
static void _sum(FireRenderer* r, UniformManager* u, GLuint from, int count, GLuint to, int x,
                 int rows) {
    _target(r, to, 0, x, 0, rows, 1);
    fire_bind(u, 0, from, "sumTex");
    uniform_set_int(u, "count", count);
    uniform_set_int(u, "outBase", x);
    _draw(r);
}

// A GRID fire's two sums, into its two texels of the result row: what the ring reads back and
// what the probe compares against the CPU. Each row of cells, then each slice's rows, then the
// slices -- spread across fragments where one fragment a slice left the GPU nearly idle.
static void _reduce(FireRenderer* r, FireGridGPU* g, const Fire* fire, int index) {
    UniformManager* u = fire_use(r, FIRE_PROGRAM_REDUCE);
    _grid_dims(u, g);
    uniform_set_vec3(u, "boxMin", g->lo);
    uniform_set_float(u, "cell", g->cell);
    uniform_set_int(u, "floorSolid", fire->grid.floor ? 1 : 0);
    uniform_set_float(u, "coolingRate", fire_cooling_rate(&fire->params));
    fire_emission_uniforms(r, u, fire, 1);
    fire_bind(u, 0, g->scalars[0], "scalarTex");
    _target(r, g->rows[0], g->rows[1], 0, 0, g->dims[1], g->dims[2]);
    _draw(r);

    u = fire_use(r, FIRE_PROGRAM_SUM);
    for (int i = 0; i < 2; i++)
        _sum(r, u, g->rows[i], g->dims[1], g->slices[i], 0, g->dims[2]);
    for (int i = 0; i < 2; i++)
        _sum(r, u, g->slices[i], g->dims[2], r->result_tex, 2 * index + i, 1);
}

// One fire's two texels, decoded onto it: the centroid placed in the world.
static void _take(Fire* fire, const float* t) {
    const FireAnswer a = fire_answer_decode(t, fire->grid.center);
    fire->intensity = a.intensity;
    glm_vec3_add(fire->origin, (float*)a.centroid, fire->centroid);
    glm_vec3_copy((float*)a.color, fire->color);
    fire->heat_release = a.heat_release;
    fire->answered = true;
}

/*
 * The ring: retire the slot issued FIRE_READBACK_LATENCY frames ago, then issue this frame's
 * sums into it, for the fires that stepped. No fence: the slot is always that old, and a read
 * that has not landed stalls the map rather than answering early, so the latency changes only
 * how often a stall happens and never the answer -- which is what keeps a headless run equal to
 * itself. A slot that will not map leaves its fires unanswered rather than holding an old answer
 * as though it were this one.
 */
static void _readback(FireRenderer* r, FireSystem* fs, const bool stepped[FIRE_MAX]) {
    const int slot = r->ring_passes % FIRE_READBACK_LATENCY;
    const GLsizeiptr bytes = (GLsizeiptr)(2 * FIRE_MAX * 4 * sizeof(float));
    if (r->ring_passes >= FIRE_READBACK_LATENCY) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, r->pbo[slot]);
        const float* data = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        for (int i = 0; i < fs->count; i++) {
            if (!r->slot_issued[slot][i])
                continue;
            if (data)
                _take(&fs->fires[i], data + 8 * i);
            else
                fs->fires[i].answered = false;
        }
        if (data) {
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        } else if (!r->readback_failed) {
            log_error("Fire: the light readback could not be mapped");
            r->readback_failed = true;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    for (int i = 0; i < FIRE_MAX; i++) {
        r->slot_issued[slot][i] = i < fs->count && stepped[i];
        if (r->slot_issued[slot][i])
            _reduce(r, &r->grids[i], &fs->fires[i], i);
    }
    _target(r, r->result_tex, 0, 0, 0, 2 * FIRE_MAX, 1);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, r->pbo[slot]);
    glReadPixels(0, 0, 2 * FIRE_MAX, 1, GL_RGBA, GL_FLOAT, NULL);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    r->ring_passes++;
}

// The programs a GRID fire steps and is summed with, built; false when any will not.
static bool _sim_ready(FireRenderer* r) {
    static const FireProgram SIM[] = {
        FIRE_PROGRAM_ADVECT,  FIRE_PROGRAM_CORRECT,    FIRE_PROGRAM_CURL,
        FIRE_PROGRAM_REACT,   FIRE_PROGRAM_DIVERGENCE, FIRE_PROGRAM_JACOBI,
        FIRE_PROGRAM_PROJECT, FIRE_PROGRAM_REDUCE,     FIRE_PROGRAM_SUM};
    for (size_t i = 0; i < sizeof(SIM) / sizeof(SIM[0]); i++)
        if (!fire_use(r, SIM[i]))
            return false;
    return true;
}

void fire_simulate(FireRenderer* r, Engine* engine, Scene* scene) {
    FireSystem* fs = scene ? scene->fire : NULL;
    if (!r || !fs)
        return;
    if (r->system != fs) {
        // Another scene's fires, or a new system: none of the gas or the answers in flight are
        // theirs.
        for (int i = 0; i < FIRE_MAX; i++)
            fire_grid_gpu_free(&r->grids[i]);
        memset(r->slot_issued, 0, sizeof(r->slot_issued));
        r->system = fs;
    }
    bool any_grid = false;
    for (int i = 0; i < fs->count; i++)
        any_grid |= fs->fires[i].kind == FIRE_GRID && fs->fires[i].enabled;
    if (!any_grid)
        return;
    profiler_scope_begin(engine->profiler, "fire sim");
    const GLPassState pass = gl_pass_begin();
    if (_sim_ready(r)) {
        const float dt = 1.0f / fs->sim_hz;
        bool stepped[FIRE_MAX] = {false};
        for (int i = 0; i < fs->count; i++) {
            Fire* fire = &fs->fires[i];
            if (fire->kind != FIRE_GRID || !fire->enabled)
                continue;
            FireGridGPU* g = &r->grids[i];
            int cells[3];
            fire_grid_cells(fire, cells);
            if (!_ensure_grid(r, g, cells, fire->name))
                continue;
            vec3 hi = {0.0f, 0.0f, 0.0f}, wind = {0.0f, 0.0f, 0.0f};
            fire_grid_bounds(fire, g->lo, hi);
            g->cell = fire_grid_cell(fire);
            if (g->obstacle_key != _obstacle_key(fire))
                _build_obstacles(g, fire);
            glm_vec3_scale(fs->air, fire->params.wind_response, wind);
            for (int s = 0; s < fire->pending; s++) {
                _step(r, g, fire, fs, wind, dt);
                fire->steps++;
            }
            stepped[i] = fire->pending > 0;
            fire->pending = 0;
        }
        _readback(r, fs, stepped);
    }
    gl_pass_end(&pass);
    check_gl_error("fire sim");
    profiler_scope_end(engine->profiler);
}

static void _read_texture(GLuint tex, GLenum format, GLenum type, void* out) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, format, type, out);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// One GRID fire's fields read back whole and checked against the GPU, as a probe row.
static void _probe_grid(FireRenderer* r, Fire* fire, int index) {
    FireGridGPU* g = &r->grids[index];
    const size_t texels = (size_t)g->atlas[0] * (size_t)g->atlas[1];
    float* sca = malloc(texels * 4 * sizeof(float));
    float* vel = malloc(texels * 4 * sizeof(float));
    float* div = malloc(texels * sizeof(float));
    unsigned char* solid = malloc(texels);
    if (!sca || !vel || !div || !solid) {
        free(sca);
        free(vel);
        free(div);
        free(solid);
        return;
    }
    _read_texture(g->scalars[0], GL_RGBA, GL_FLOAT, sca);
    _read_texture(g->velocity[0], GL_RGBA, GL_FLOAT, vel);
    _read_texture(g->divergence, GL_RED, GL_FLOAT, div);
    _read_texture(g->obstacle, GL_RED, GL_UNSIGNED_BYTE, solid);

    const float volume = g->cell * g->cell * g->cell;
    const FireParams* p = &fire->params;
    const int X = g->dims[0], Y = g->dims[1], Z = g->dims[2];
    double peak = 0.0, reaction = 0.0, soot = 0.0, solid_heat = 0.0, solid_soot = 0.0;
    double pre_max = 0.0, pre_sq = 0.0, post_max = 0.0, post_sq = 0.0, intensity = 0.0;
    double m[3] = {0.0, 0.0, 0.0};
    int fluid = 0;
    for (int z = 0; z < Z; z++) {
        for (int y = 0; y < Y; y++) {
            for (int x = 0; x < X; x++) {
                const size_t t = _atlas_index(g, x, y, z);
                const float* s = &sca[4 * t];
                if (solid[t]) {
                    solid_heat = fmax(solid_heat, (double)s[0]);
                    solid_soot = fmax(solid_soot, (double)s[2]);
                    continue;
                }
                peak = fmax(peak, (double)s[0]);
                reaction += (double)s[1];
                soot += (double)s[2];
                const vec3 at = {(float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f};
                const float fade = fire_edge_fade(fire, g->dims, at);
                vec3 rgb = {0.0f, 0.0f, 0.0f};
                const double e = (double)fire_emission(fire, s[0], s[2] * fade, s[3] * fade, rgb) *
                                 (double)volume;
                intensity += e;
                m[0] += e * (double)(g->lo[0] + ((float)x + 0.5f) * g->cell);
                m[1] += e * (double)(g->lo[1] + ((float)y + 0.5f) * g->cell);
                m[2] += e * (double)(g->lo[2] + ((float)z + 0.5f) * g->cell);
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
                        if (q[k][1] < 0 && fire->grid.floor) {
                            v[k] = 0.0;
                            continue;
                        }
                        const int qx = q[k][0] < 0 ? 0 : (q[k][0] >= X ? X - 1 : q[k][0]);
                        const int qy = q[k][1] < 0 ? 0 : (q[k][1] >= Y ? Y - 1 : q[k][1]);
                        const int qz = q[k][2] < 0 ? 0 : (q[k][2] >= Z ? Z - 1 : q[k][2]);
                        const size_t qt = _atlas_index(g, qx, qy, qz);
                        const bool inside = qx == q[k][0] && qy == q[k][1] && qz == q[k][2];
                        v[k] = inside && solid[qt] ? 0.0 : (double)vel[4 * qt + (size_t)a];
                    }
                    d += v[0] - v[1];
                }
                // Less the expansion the core asks for, which is what the solve aims at.
                d = d / (2.0 * (double)g->cell) - (double)(p->expansion * fmaxf(s[3], 0.0f));
                post_max = fmax(post_max, fabs(d));
                post_sq += d * d;
                pre_max = fmax(pre_max, fabs((double)div[t]));
                pre_sq += (double)div[t] * (double)div[t];
                fluid++;
            }
        }
    }
    // The same sums on the GPU, this frame, rather than the ring's three frames ago.
    _reduce(r, g, fire, index);
    float res[2 * FIRE_MAX * 4];
    _read_texture(r->result_tex, GL_RGBA, GL_FLOAT, res);
    const FireAnswer got = fire_answer_decode(&res[8 * index], fire->grid.center);
    const double inv = intensity > 0.0 ? 1.0 / intensity : 0.0;
    printf("fire-probe grid index=%d nx=%d ny=%d nz=%d tiles=%dx%d cell=%.9g fluid=%d "
           "peak_kelvin=%.9g reaction=%.9g soot=%.9g solid_heat=%.9g solid_soot=%.9g "
           "div_pre_max=%.9g div_pre_rms=%.9g div_post_max=%.9g div_post_rms=%.9g "
           "cpu_intensity=%.9g gpu_intensity=%.9g cpu_cx=%.9g cpu_cy=%.9g cpu_cz=%.9g "
           "gpu_cx=%.9g gpu_cy=%.9g gpu_cz=%.9g heat_release=%.9g\n",
           index, X, Y, Z, g->tiles[0], g->tiles[1], (double)g->cell, fluid,
           (double)p->ambient + peak, reaction, soot, solid_heat, solid_soot, pre_max,
           fluid ? sqrt(pre_sq / fluid) : 0.0, post_max, fluid ? sqrt(post_sq / fluid) : 0.0,
           intensity, (double)got.intensity, m[0] * inv, m[1] * inv, m[2] * inv,
           (double)got.centroid[0], (double)got.centroid[1], (double)got.centroid[2],
           (double)got.heat_release);
    free(sca);
    free(vel);
    free(div);
    free(solid);
}

void fire_render_probe(FireRenderer* r, const Scene* scene) {
    FireSystem* fs = scene ? scene->fire : NULL;
    if (!r || !fs)
        return;
    const GLPassState pass = gl_pass_begin();
    for (int i = 0; i < fs->count; i++)
        if (fs->fires[i].kind == FIRE_GRID && r->grids[i].velocity[0] &&
            fire_use(r, FIRE_PROGRAM_REDUCE))
            _probe_grid(r, &fs->fires[i], i);
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
