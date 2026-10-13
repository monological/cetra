#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gi_volume.h"
#include "capture_key.h"
#include "cook.h"
#include "draw_list.h"
#include "engine.h"
#include "ibl.h"
#include "intersect.h"
#include "lighting_atlas.h"
#include "profiler.h"
#include "render.h"
#include "scene.h"
#include "uniform.h"
#include "util.h"
#include "ext/log.h"

// Tile pitch including the gutter on both sides.
#define IRR_PITCH (GI_IRRADIANCE_RES + 2 * GI_TILE_BORDER)
#define VIS_PITCH (GI_VISIBILITY_RES + 2 * GI_TILE_BORDER)

// How much nearer a non-resident volume must be than a resident one to take its slot, in
// metres. A volume is a building's worth of grid, so a camera between two buildings is
// the case this guards: without it the last slot changes hands on every step.
#define GI_STREAM_MARGIN 2.0f

static int gi_probe_count(const GIVolume* gi) {
    return gi->counts[0] * gi->counts[1] * gi->counts[2];
}

// Atlas layout within the volume's slot: the volume's X axis runs along the atlas X, and
// (y,z) is folded into the atlas Y. Probe index is ix + nx*(iy + ny*iz), so a row of the
// atlas is a row of the grid -- which is the axis the 8-probe gather steps along fastest.
// The irradiance block sits above the visibility block; the two have different tile
// pitches, so they cannot share rows.
static void gi_tile_origin(const GIVolume* gi, int probe, bool visibility, int* out_x, int* out_y) {
    const int col = probe % gi->counts[0];
    const int row = probe / gi->counts[0];
    if (visibility) {
        *out_x = col * VIS_PITCH;
        *out_y = gi->irradiance_rows + row * VIS_PITCH;
    } else {
        *out_x = col * IRR_PITCH;
        *out_y = row * IRR_PITCH;
    }
}

// Every probe to capture again, from the first.
static void gi_arm(GIVolume* gi) {
    gi->dirty_count = gi_probe_count(gi);
    gi->next_probe = 0;
}

// The grid over a box: cell centres, a far plane the diagonal clears, and a full sweep armed.
static void gi_fit(GIVolume* gi, const vec3 aabb_min, const vec3 aabb_max) {
    for (int c = 0; c < 3; ++c) {
        float extent = aabb_max[c] - aabb_min[c];
        if (extent < 1e-5f)
            extent = 1e-5f;
        gi->spacing[c] = extent / (float)gi->counts[c];
        gi->grid_min[c] = aabb_min[c];
    }
    // A probe sees the whole volume, so the diagonal plus headroom -- anything
    // past this is "nothing hit" as far as the visibility moments are concerned.
    vec3 extent = {0};
    glm_vec3_sub((float*)aabb_max, (float*)aabb_min, extent);
    gi->far_clip = glm_vec3_norm(extent) * 1.5f + 1.0f;
    gi_arm(gi);
}

GIVolume* create_gi_volume(int nx, int ny, int nz, const vec3 box_min, const vec3 box_max) {
    if (nx < 1 || ny < 1 || nz < 1)
        return NULL;

    GIVolume* gi = calloc(1, sizeof(GIVolume));
    if (!gi) {
        log_error("Failed to allocate GI volume");
        return NULL;
    }
    gi->counts[0] = nx;
    gi->counts[1] = ny;
    gi->counts[2] = nz;
    gi->irradiance_rows = ny * nz * IRR_PITCH;
    gi_fit(gi, box_min, box_max);
    return gi;
}

GIVolume* create_gi_volume_spaced(const vec3 box_min, const vec3 box_max, float spacing) {
    if (spacing <= 0.0f)
        return NULL;
    int counts[3];
    vec3 lo, hi;
    for (int c = 0; c < 3; ++c) {
        const float extent = box_max[c] - box_min[c];
        counts[c] = (int)ceilf(extent / spacing - 1e-4f);
        if (counts[c] < 1)
            counts[c] = 1;
        // Centred, so the overhang a whole number of cells leaves is shared by both faces.
        const float centre = 0.5f * (box_min[c] + box_max[c]);
        lo[c] = centre - 0.5f * spacing * (float)counts[c];
        hi[c] = centre + 0.5f * spacing * (float)counts[c];
    }
    GIVolume* gi = create_gi_volume(counts[0], counts[1], counts[2], lo, hi);
    if (gi)
        gi->classify = true;
    return gi;
}

void free_gi_volume(GIVolume* gi) {
    if (!gi)
        return;
    if (gi->capture_color)
        glDeleteTextures(1, &gi->capture_color);
    if (gi->capture_depth)
        glDeleteTextures(1, &gi->capture_depth);
    if (gi->classify_depth)
        glDeleteTextures(1, &gi->classify_depth);
    if (gi->quad_vao)
        glDeleteVertexArrays(1, &gi->quad_vao);
    if (gi->quad_vbo)
        glDeleteBuffers(1, &gi->quad_vbo);
    free(gi);
}

// Cubemap with no mips, LINEAR, clamped -- the capture scratch, reused per probe.
static GLuint gi_make_cubemap(int size, GLenum internal_format, GLenum format, GLenum type) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    for (int f = 0; f < 6; ++f) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, (GLint)internal_format, size, size, 0,
                     format, type, NULL);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    return tex;
}

void gi_volume_atlas_extent(const GIVolume* gi, int* out_w, int* out_h) {
    if (!gi) {
        if (out_w)
            *out_w = 0;
        if (out_h)
            *out_h = 0;
        return;
    }
    const int rows = gi->counts[1] * gi->counts[2];
    if (out_w)
        *out_w = gi->counts[0] * (IRR_PITCH > VIS_PITCH ? IRR_PITCH : VIS_PITCH);
    if (out_h)
        *out_h = rows * IRR_PITCH + rows * VIS_PITCH;
}

// The capture scratch. The tiles themselves live in the scene's lighting atlas, whose slot an
// opening sweep clears, so nothing here clears them.
static bool gi_ensure_targets(GIVolume* gi, struct Engine* engine) {
    // Allocate-once on its own flag: this is called on every frame with a dirty
    // probe, so a re-convergence sweep would otherwise re-create two cubemaps and
    // a VAO per frame and orphan the previous ones.
    if (gi->targets_ready)
        return true;
    if (gi->failed)
        return false;

    gi->capture_color = gi_make_cubemap(GI_CAPTURE_FACE, GL_RGB16F, GL_RGB, GL_FLOAT);
    gi->capture_depth =
        gi_make_cubemap(GI_CAPTURE_FACE, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT);
    if (gi->classify)
        gi->classify_depth =
            gi_make_cubemap(GI_CAPTURE_FACE, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT);
    if (!gi->capture_color || !gi->capture_depth || (gi->classify && !gi->classify_depth)) {
        log_error("GI volume capture targets incomplete; disabling this volume");
        gi->failed = true;
        return false;
    }

    create_fullscreen_quad_vao(&gi->quad_vao, &gi->quad_vbo);

    gi->project_program = engine_get_program(engine, "gi_project");
    if (!gi->project_program) {
        log_error("GI volume projection program missing; disabling this volume");
        gi->failed = true;
        return false;
    }

    gi->targets_ready = true;
    log_info("GI volume: %dx%dx%d probes", gi->counts[0], gi->counts[1], gi->counts[2]);
    return true;
}

static void gi_probe_position(const GIVolume* gi, int index, vec3 out) {
    int nx = gi->counts[0], ny = gi->counts[1];
    int ix = index % nx;
    int iy = (index / nx) % ny;
    int iz = index / (nx * ny);
    // Cell centres, not corners: a probe on the AABB face would sit inside the
    // wall it is supposed to sample away from.
    out[0] = gi->grid_min[0] + ((float)ix + 0.5f) * gi->spacing[0];
    out[1] = gi->grid_min[1] + ((float)iy + 0.5f) * gi->spacing[1];
    out[2] = gi->grid_min[2] + ((float)iz + 0.5f) * gi->spacing[2];
}

// Capture near plane. Fixed rather than scene-scaled: a probe sits in open space
// by construction (cell centres), and the depth it writes is only ever read back
// as a distance, so the near plane's only job is to not clip the room it is in.
#define GI_NEAR_CLIP 0.05f

// What a projection writes: gi_project_frag's three modes.
enum { GI_PROJECT_IRRADIANCE = 0, GI_PROJECT_VISIBILITY = 1, GI_PROJECT_CLASSIFY = 2 };

// Project the capture into one tile of the volume's slot, gutter included. Classification
// writes the irradiance tile's alpha and nothing else.
static void gi_project_tile(GIVolume* gi, const LightingAtlas* atlas, AtlasRect slot, int probe,
                            int mode, float hysteresis) {
    const bool visibility = mode == GI_PROJECT_VISIBILITY;
    const int res = visibility ? GI_VISIBILITY_RES : GI_IRRADIANCE_RES;
    int ox, oy;
    gi_tile_origin(gi, probe, visibility, &ox, &oy);
    ox += slot.x;
    oy += slot.y;

    // Blended in place against the previous value with a constant alpha: legal
    // because the atlas is never bound for reading while it is the render target
    // -- this pass reads only the capture cube, and the scene pass that reads the
    // atlas runs separately. That is the shadow-map sequencing the codebase
    // already relies on, and it avoids a ping-pong that would have to copy every
    // untouched tile to update one.
    glBindFramebuffer(GL_FRAMEBUFFER, atlas->fbo);
    glViewport(ox, oy, res + 2 * GI_TILE_BORDER, res + 2 * GI_TILE_BORDER);
    if (hysteresis > 0.0f) {
        glEnable(GL_BLEND);
        glBlendColor(0.0f, 0.0f, 0.0f, hysteresis);
        glBlendFunc(GL_ONE_MINUS_CONSTANT_ALPHA, GL_CONSTANT_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }

    glUseProgram(gi->project_program->id);
    UniformManager* u = gi->project_program->uniforms;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, gi->capture_color);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, gi->capture_depth);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_CUBE_MAP, gi->classify_depth);
    glActiveTexture(GL_TEXTURE0);
    uniform_set_int(u, "captureColor", 0);
    uniform_set_int(u, "captureDepth", 1);
    uniform_set_int(u, "backDepth", 2);
    uniform_set_vec2(u, "tileOrigin", (const float[]){(float)ox, (float)oy});
    uniform_set_float(u, "tileRes", (float)res);
    uniform_set_float(u, "nearZ", GI_NEAR_CLIP);
    uniform_set_float(u, "farZ", gi->far_clip);
    uniform_set_int(u, "mode", mode);
    // The irradiance tile's alpha is the classification's: written by the opening sweep, never
    // by a re-convergence, whose blend would carry an inside-a-wall 0 back toward 1.
    const bool rgb_only = mode == GI_PROJECT_IRRADIANCE && gi->classify && hysteresis > 0.0f;
    if (mode == GI_PROJECT_CLASSIFY)
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    else if (rgb_only)
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); // VAO bound once by the caller
    if (mode == GI_PROJECT_CLASSIFY || rgb_only)
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

// A part of a sweep, timed when `timing` is: from a GPU drained of whatever came before, the CPU
// time to submit the part, and the wall time until the GPU has drawn it.
static double gi_clock_start(const GISweepTiming* timing) {
    if (!timing)
        return 0.0;
    glFinish();
    return glfwGetTime();
}

static void gi_clock_stop(GISweepTiming* timing, double t0, GISweepPart part) {
    if (!timing)
        return;
    const double submitted = glfwGetTime();
    glFinish();
    timing->cpu[part] += submitted - t0;
    timing->wall[part] += glfwGetTime() - t0;
}

// The sweep's cost, as it converges, and cleared for the next.
static void gi_timing_print(GIVolume* gi, size_t index) {
    const GISweepTiming* t = &gi->timing;
    double total = 0.0;
    for (int p = 0; p < GI_PART_COUNT; ++p)
        total += t->wall[p];
    static const char* const names[GI_PART_COUNT] = {"setup", "shaded", "classify", "project"};
    printf("gi-timing volume=%zu probes=%d frames=%d ms=%.1f", index, gi_probe_count(gi), t->frames,
           total * 1000.0);
    for (int p = 0; p < GI_PART_COUNT; ++p)
        printf(" %s-ms=%.1f/%.1f", names[p], t->cpu[p] * 1000.0, t->wall[p] * 1000.0);
    printf(" draws=%zu tris=%zu\n", t->submit.draws, t->submit.triangles);
    fflush(stdout);
    gi->timing = (GISweepTiming){0};
}

// The tiles of `probe` projected from its capture, each of `modes` in turn -- a fullscreen-quad
// pass, which depth and culling would only get in the way of. Both go back as found: this runs
// inside the frame, after the frame top has set the culling the rest of the frame draws with.
static void gi_project(GIVolume* gi, const LightingAtlas* atlas, AtlasRect slot, int probe,
                       const int* modes, int count, float hysteresis, GISweepTiming* timing) {
    const double t0 = gi_clock_start(timing);
    const GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(gi->quad_vao);
    for (int m = 0; m < count; ++m)
        gi_project_tile(gi, atlas, slot, probe, modes[m], hysteresis);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    if (cull_was)
        glEnable(GL_CULL_FACE);
    gi_clock_stop(timing, t0, GI_PART_PROJECT);
}

// Capture up to `most` probes of a resident volume into its slot, 0 = every one left, while the
// frame's capture budget allows: an `opening` sweep, into a slot nothing was captured in, or a
// re-convergence over the texels there. Inside a capture burst the caller holds open, with
// `held` whether a unit of the budget is already in hand, asked for and not yet spent, and
// `timing` where its parts are priced, or NULL. Returns the probes captured.
static int gi_volume_sweep(GIVolume* gi, struct Engine* engine, struct Scene* scene,
                           const LightingAtlas* atlas, AtlasRect slot, int most, bool opening,
                           CaptureBudget* budget, bool* held, GISweepTiming* timing) {
    if (gi->failed || gi->dirty_count <= 0 || !gi_ensure_targets(gi, engine))
        return 0;

    const int probes = gi_probe_count(gi);
    if (most <= 0 || most > gi->dirty_count)
        most = gi->dirty_count;
    // An opening capture is taken outright; a re-convergence blends over what is there.
    const float hysteresis = opening ? 0.0f : 0.97f;
    if (timing)
        engine->capture_submit = &timing->submit;

    int taken = 0;
    for (; taken < most; ++taken) {
        if (!*held && !capture_budget_take(budget, false))
            break;
        *held = false;
        int probe = gi->next_probe;
        gi->next_probe = (gi->next_probe + 1) % probes;
        gi->dirty_count--;

        // An opening sweep starts from a cleared slot: its tiles do not cover the whole of it --
        // an irradiance tile is narrower than the column it shares with a visibility tile -- and
        // between them the slot would keep whatever its last holder left, laid out by that
        // holder's grid.
        if (opening && probe == 0)
            lighting_atlas_clear(atlas, slot);

        vec3 pos = {0};
        gi_probe_position(gi, probe, pos);
        double t0 = gi_clock_start(timing);
        scene_capture_faces(engine, scene, scene->ibl, pos, gi->capture_color, gi->capture_depth,
                            GI_CAPTURE_FACE, GI_NEAR_CLIP, gi->far_clip, SCENE_FACES_SHADED, 0, 6);
        gi_clock_stop(timing, t0, GI_PART_SHADED);
        static const int lit[] = {GI_PROJECT_IRRADIANCE, GI_PROJECT_VISIBILITY};
        gi_project(gi, atlas, slot, probe, lit, 2, hysteresis, timing);

        // The same probe again with only back faces drawn, against the front faces' depth
        // still held in capture_depth: a back face nearer than every front face in a direction
        // means the probe sits inside something, and one that sees that in more than a quarter
        // of its directions is inside a wall. Whether a probe is in a wall is geometry, which a
        // change of light does not move, so only the opening sweep asks -- and depth is all it
        // reads, so depth is all this capture draws.
        if (gi->classify && opening) {
            t0 = gi_clock_start(timing);
            scene_capture_faces(engine, scene, scene->ibl, pos, gi->capture_color,
                                gi->classify_depth, GI_CAPTURE_FACE, GI_NEAR_CLIP, gi->far_clip,
                                SCENE_FACES_BACK_DEPTH, 0, 6);
            gi_clock_stop(timing, t0, GI_PART_CLASSIFY);
            static const int wall[] = {GI_PROJECT_CLASSIFY};
            gi_project(gi, atlas, slot, probe, wall, 1, hysteresis, timing);
        }
    }
    engine->capture_submit = NULL;
    if (timing && taken > 0)
        timing->frames++;

    gi->captures_total += taken;
    if (gi->dirty_count <= 0)
        log_info("GI volume converged: %d captures total", gi->captures_total);
    return taken;
}

// The rectangle of slot `slot` a volume's tiles take.
static AtlasRect gi_volume_rect(const GIVolume* gi, const LightingAtlas* atlas, int slot) {
    AtlasRect r = {lighting_atlas_gi_x(atlas, slot), 0, 0, 0};
    gi_volume_atlas_extent(gi, &r.w, &r.h);
    return r;
}

// What a swept volume leaving residency keeps: its tiles, read out of the slot it is giving up.
// A volume left mid-re-convergence keeps them as they stand and carries on when it returns.
typedef struct GIKeep {
    const GIWorld* world;
    const LightingAtlas* atlas;
} GIKeep;

static uint16_t* gi_keep_slot(void* user, size_t i, int slot) {
    const GIKeep* k = user;
    uint16_t* kept =
        lighting_atlas_keep(k->atlas, gi_volume_rect(k->world->volumes[i], k->atlas, slot));
    if (!kept)
        log_warn("GI volume %zu: its tiles were not kept; it sweeps again when it returns", i);
    return kept;
}

GIWorld* create_gi_world(void) {
    GIWorld* world = calloc(1, sizeof(GIWorld));
    if (!world) {
        log_error("Failed to allocate the GI world");
        return NULL;
    }
    world->enabled = true;
    world->rate = 2;
    residency_init(&world->residency, GI_RESIDENT_MAX, GI_STREAM_MARGIN);
    return world;
}

void free_gi_world(GIWorld* world) {
    if (!world)
        return;
    for (size_t i = 0; i < world->residency.count; ++i)
        free_gi_volume(world->volumes[i]);
    free(world->volumes);
    residency_free(&world->residency);
    free(world);
}

bool gi_world_add(GIWorld* world, GIVolume* gi) {
    if (!world || !gi) {
        free_gi_volume(gi);
        return false;
    }
    if (!grow_array((void**)&world->volumes, &world->volume_capacity, world->residency.count + 1,
                    sizeof(GIVolume*), 4) ||
        !residency_add(&world->residency)) {
        log_error("Failed to grow the GI world");
        free_gi_volume(gi);
        return false;
    }
    world->volumes[world->residency.count - 1] = gi;
    return true;
}

// A volume's grid, corner to corner.
static AABB gi_volume_box(const GIVolume* gi) {
    AABB box = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    for (int c = 0; c < 3; ++c) {
        box.min[c] = gi->grid_min[c];
        box.max[c] = gi->grid_min[c] + gi->spacing[c] * (float)gi->counts[c];
    }
    return box;
}

// The cook's key for volume `index`'s opening sweep (spec 13.42): the scene its probes can see,
// out to their far plane past the grid, the grid and how it is captured, and every other volume
// whose light a probe can sample there.
static CookKey gi_volume_key(const GIWorld* world, size_t index, struct Engine* engine,
                             struct Scene* scene) {
    const GIVolume* gi = world->volumes[index];
    CookKey key = cook_key("gi-volume/1");
    char label[COOK_NAME_MAX];
    snprintf(label, sizeof(label), "volume-%zu", index);
    cook_key_label(&key, label);
    AABB reach = gi_volume_box(gi);
    aabb_expand(&reach, gi->far_clip);
    scene_capture_fold(engine, scene, &reach, &key);
    for (int c = 0; c < 3; ++c) {
        cook_key_i32(&key, gi->counts[c]);
        cook_key_f32(&key, gi->grid_min[c]);
        cook_key_f32(&key, gi->spacing[c]);
    }
    cook_key_f32(&key, gi->far_clip);
    cook_key_bool(&key, gi->classify);
    const int sizes[] = {GI_CAPTURE_FACE, GI_IRRADIANCE_RES, GI_VISIBILITY_RES, GI_TILE_BORDER};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
        cook_key_i32(&key, sizes[s]);
    cook_key_f32(&key, world->cull_pixels);
    gi_world_fold(world, &reach, index, &key);
    return key;
}

void gi_world_fold(const GIWorld* world, const AABB* box, size_t skip, CookKey* key) {
    for (size_t j = 0; world && j < world->residency.count; ++j) {
        const ResidencyItem* item = &world->residency.items[j];
        const GIVolume* gi = world->volumes[j];
        const AABB grid = gi_volume_box(gi);
        if (j == skip || !aabb_overlaps(&grid, box))
            continue;
        const bool loaded = item->state == RESIDENCY_LOADED && !gi->failed;
        cook_key_u64(key, (uint64_t)j);
        cook_key_bool(key, loaded);
        if (!loaded)
            continue;
        // A volume swept with no key lit what it lit with nothing to name it by.
        if (!item->cook_hash)
            cook_key_refuse(key);
        cook_key_u64(key, item->cook_hash);
    }
}

// Volume `index`'s opening sweep from the cook, keyed as it begins and inside the burst, so the
// lights are folded at rest (spec 13.42): its tiles into its slot and the volume swept, true; or
// false, the key kept for the store, and the sweep runs live.
static bool gi_volume_fetch(GIWorld* world, size_t index, struct Engine* engine,
                            struct Scene* scene, const LightingAtlas* atlas) {
    GIVolume* gi = world->volumes[index];
    ResidencyItem* item = &world->residency.items[index];
    const CookKey key = gi_volume_key(world, index, engine, scene);
    item->cook_hash = key.valid ? key.hash : 0;
    if (!key.valid) {
        if (cook_enabled())
            log_info("gi-cook volume=%zu result=unkeyable: something it sees cannot say what it "
                     "is",
                     index);
        return false;
    }
    if (!lighting_atlas_cook_fetch(atlas, gi_volume_rect(gi, atlas, item->slot), &key))
        return false;
    gi->dirty_count = 0;
    gi->next_probe = 0;
    residency_loaded(item);
    return true;
}

// A swept volume's tiles into the cook, under the key its sweep began with -- and only if the
// scene still folds to it, since a door opened or a light switched mid-sweep leaves tiles that
// are no one scene's.
static void gi_volume_store(const GIWorld* world, size_t index, struct Engine* engine,
                            struct Scene* scene, const LightingAtlas* atlas) {
    const ResidencyItem* item = &world->residency.items[index];
    if (!item->cook_hash)
        return;
    const CookKey key = gi_volume_key(world, index, engine, scene);
    if (!key.valid || key.hash != item->cook_hash) {
        log_info("gi-cook volume=%zu result=unstable: the scene changed while it was swept", index);
        return;
    }
    lighting_atlas_cook_store(atlas, gi_volume_rect(world->volumes[index], atlas, item->slot),
                              &key);
}

// Which volumes are resident, from the camera. A swept volume that leaves keeps its tiles, read
// out of `atlas` before its slot changes hands; one that leaves mid-sweep sweeps again when next
// admitted.
static void gi_world_rank(GIWorld* world, const struct Engine* engine, const LightingAtlas* atlas) {
    vec3 eye = {0.0f, 0.0f, 0.0f};
    if (engine->camera)
        glm_vec3_copy(engine->camera->position, eye);
    for (size_t i = 0; i < world->residency.count; ++i) {
        const GIVolume* gi = world->volumes[i];
        const AABB box = gi_volume_box(gi);
        world->residency.items[i].distance = gi->failed ? -1.0f : sqrtf(aabb_dist_sq(&box, eye));
    }
    GIKeep keep = {world, atlas};
    residency_assign(&world->residency, atlas ? gi_keep_slot : NULL, &keep);
    // Admitted with nothing kept, a volume's slot holds another's tiles: it sweeps from the start.
    for (size_t i = 0; i < world->residency.count; ++i) {
        const ResidencyItem* item = &world->residency.items[i];
        if (item->moved && item->state == RESIDENCY_CAPTURE)
            gi_arm(world->volumes[i]);
    }
}

void gi_world_update(GIWorld* world, struct Engine* engine, struct Scene* scene,
                     CaptureBudget* budget) {
    if (!world || !world->enabled || world->residency.count == 0 || !engine || !scene)
        return;
    gi_world_rank(world, engine, scene->lighting_atlas);
    Residency* res = &world->residency;
    bool work = false;
    for (size_t i = 0; i < res->count && !work; ++i) {
        const GIVolume* gi = world->volumes[i];
        work = res->items[i].slot >= 0 &&
               (res->items[i].state == RESIDENCY_UPLOAD || (!gi->failed && gi->dirty_count > 0));
    }
    if (!work)
        return;
    const LightingAtlas* atlas = scene_lighting_atlas(scene, engine);
    if (!atlas)
        return;
    // Kept tiles back first: they cost an upload and no capture.
    for (size_t i = 0; i < res->count; ++i) {
        ResidencyItem* item = &res->items[i];
        if (item->state != RESIDENCY_UPLOAD)
            continue;
        lighting_atlas_restore(atlas, gi_volume_rect(world->volumes[i], atlas, item->slot),
                               item->kept);
        residency_loaded(item);
    }

    // The volumes with probes to capture that may capture now, nearest first, ties to the lower
    // index: the camera's own building first, and an order that does not depend on which slot
    // each holds.
    int due[GI_RESIDENT_MAX];
    int n = 0;
    for (size_t i = 0; i < res->count; ++i) {
        const GIVolume* gi = world->volumes[i];
        if (res->items[i].slot < 0 || gi->failed || gi->dirty_count <= 0)
            continue;
        const AABB box = gi_volume_box(gi);
        if (!scene_capture_ready(engine, scene, SCENE_CAPTURE_IRRADIANCE, &box))
            continue;
        int at = n++;
        while (at > 0 && res->items[due[at - 1]].distance > res->items[i].distance) {
            due[at] = due[at - 1];
            at--;
        }
        due[at] = (int)i;
    }
    // Timed only on a frame that captures: a converged world is the steady state, and a scope
    // opened every frame would file a 0.000 ms row on nearly all of them. The world's first unit
    // is asked for here, before the burst opens, so the burst's own shadow pass is counted too.
    if (n == 0 || !capture_budget_take(budget, true))
        return;
    profiler_scope_begin(engine->profiler, "gi capture");
    GLint saved_fbo;
    GLint saved_viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);
    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    // One burst for every volume the frame captures in: a burst bakes the shadow maps its
    // captures read, the same maps for each, and nothing it bakes needs undoing beyond the
    // restore below -- the frame's own shadow pass runs after this and overwrites them with the
    // camera-fit cascades it needs. IRRADIANCE: what this bakes is added to the analytic direct
    // term, so a derived emissive panel's own surface must not appear in it (render.h).
    SceneCaptureState saved_capture;
    // The burst's setup is the nearest volume's to account for: the one that asked for it.
    GISweepTiming* first = world->timing ? &world->volumes[due[0]]->timing : NULL;
    const double t0 = gi_clock_start(first);
    scene_capture_begin(engine, scene, SCENE_CAPTURE_IRRADIANCE, &saved_capture);
    gi_clock_stop(first, t0, GI_PART_SETUP);

    // Every sweep is paced by the frame's capture budget (spec 13.32), the opening one too: a
    // frame spent sweeping a whole volume froze the window for seconds, at load and walking into
    // a building alike. A half-swept slot is withheld, so a volume's light appears whole, the
    // frames after its last probe. Re-convergences, over tiles still valid to sample, also share
    // the world's rate; 0 is no limit.
    int rate_left = world->rate;
    bool held = true; // the unit asked for above
    for (int k = 0; k < n; ++k) {
        ResidencyItem* item = &res->items[due[k]];
        GIVolume* gi = world->volumes[due[k]];
        const bool opening = item->state == RESIDENCY_CAPTURE;
        const bool limited = !opening && world->rate > 0;
        if (limited && rate_left <= 0)
            continue;
        // A sweep about to begin asks the cook first (spec 13.42), from inside the burst, where
        // the lights are held at rest -- once the budget lets it begin, so that what it is keyed
        // by is the scene its first probe sees. A hit spends the unit, as an upload would; a miss
        // leaves it in hand for the first probe.
        const bool cook = scene->cook_lighting;
        if (opening && cook && gi->next_probe == 0 && gi->dirty_count == gi_probe_count(gi)) {
            if (!held && !capture_budget_take(budget, false))
                continue;
            held = !gi_volume_fetch(world, (size_t)due[k], engine, scene, atlas);
            if (!held)
                continue;
        }
        const int swept = gi_volume_sweep(
            gi, engine, scene, atlas, gi_volume_rect(gi, atlas, item->slot),
            limited ? rate_left : 0, opening, budget, &held, world->timing ? &gi->timing : NULL);
        if (limited)
            rate_left -= swept;
        if (gi->dirty_count <= 0 && opening) {
            if (cook)
                gi_volume_store(world, (size_t)due[k], engine, scene, atlas);
            residency_loaded(item);
        }
        if (swept > 0 && gi->dirty_count <= 0 && world->timing)
            gi_timing_print(gi, (size_t)due[k]);
    }

    scene_capture_end(engine, scene, &saved_capture);
    // Blend ENABLED is the engine's baseline (set once at init; the G-buffer
    // relies on it and disables per attachment). The tile blend turned it
    // off, so restoring means putting it back on -- an earlier version of this
    // line disabled it and called that the baseline, which handed every frame
    // after a sweep a context the rest of the renderer does not expect.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    check_gl_error("gi world update");
    profiler_scope_end(engine->profiler);
}

typedef struct GIBoxQuery {
    const GIWorld* world;
    const AABB* box;
} GIBoxQuery;

static bool gi_bears_on(const void* user, size_t i) {
    const GIBoxQuery* q = user;
    const GIVolume* gi = q->world->volumes[i];
    const AABB grid = gi_volume_box(gi);
    return !gi->failed && aabb_overlaps(&grid, q->box);
}

bool gi_world_ready_in(const GIWorld* world, const AABB* box) {
    if (!world || !world->enabled)
        return true;
    const GIBoxQuery q = {world, box};
    return residency_loaded_where(&world->residency, gi_bears_on, &q);
}

bool gi_world_pending(const GIWorld* world) {
    if (!world || !world->enabled)
        return false;
    for (size_t i = 0; i < world->residency.count; ++i) {
        const GIVolume* gi = world->volumes[i];
        if (world->residency.items[i].state == RESIDENCY_CAPTURE && !gi->failed &&
            gi->dirty_count > 0)
            return true;
    }
    return false;
}

void gi_world_mark_dirty(GIWorld* world) {
    if (!world)
        return;
    // What every volume kept was lit by the light that just changed. A resident one
    // re-converges over its old tiles at `rate`; one out of residency has none, and sweeps from
    // the start when it is next admitted.
    residency_forget(&world->residency, false);
    for (size_t i = 0; i < world->residency.count; ++i)
        gi_arm(world->volumes[i]);
}

void gi_world_shift_origin(GIWorld* world, const vec3 delta) {
    if (!world)
        return;
    for (size_t i = 0; i < world->residency.count; ++i)
        glm_vec3_sub(world->volumes[i]->grid_min, (float*)delta, world->volumes[i]->grid_min);
}

void gi_world_atlas_needs(const GIWorld* world, LightingAtlasLayout* layout) {
    if (!world)
        return;
    const int count = (int)world->residency.count;
    layout->gi_slots = count < GI_RESIDENT_MAX ? count : GI_RESIDENT_MAX;
    for (int i = 0; i < count; ++i) {
        int w = 0, h = 0;
        gi_volume_atlas_extent(world->volumes[i], &w, &h);
        if (w > layout->gi_w)
            layout->gi_w = w;
        if (h > layout->gi_h)
            layout->gi_h = h;
    }
}

void gi_world_bind(const GIWorld* world, const LightingAtlas* atlas, ShaderProgram* program) {
    if (!program || !program->uniforms)
        return;
    UniformManager* u = program->uniforms;

    // Every resident volume is published, ready or not: a fragment inside one still sweeping
    // takes the environment's answer rather than a neighbour's edge (gi_volume.glsl).
    // In the volumes' own order, so which of two overlapping volumes answers does not depend on
    // which slot each happens to hold.
    float slots[GI_RESIDENT_MAX * 4][4];
    int published = 0;
    bool any_ready = false;
    if (world && world->enabled && atlas && atlas->texture) {
        for (size_t i = 0; i < world->residency.count; ++i) {
            const ResidencyItem* item = &world->residency.items[i];
            if (item->slot < 0)
                continue;
            const GIVolume* gi = world->volumes[i];
            const bool ready = item->state == RESIDENCY_LOADED && !gi->failed;
            any_ready = any_ready || ready;
            float* row = slots[published * 4];
            glm_vec3_copy((float*)gi->grid_min, row);
            row[3] = ready ? 1.0f : 0.0f;
            row = slots[published * 4 + 1];
            glm_vec3_copy((float*)gi->spacing, row);
            row[3] = gi->far_clip;
            row = slots[published * 4 + 2];
            row[0] = (float)gi->counts[0];
            row[1] = (float)gi->counts[1];
            row[2] = (float)gi->counts[2];
            row[3] = (float)gi->irradiance_rows;
            row = slots[published * 4 + 3];
            row[0] = (float)lighting_atlas_gi_x(atlas, item->slot);
            row[1] = 0.0f;
            row[2] = 0.0f;
            row[3] = 0.0f;
            published++;
        }
    }

    if (!any_ready) {
        uniform_set_int(u, "giEnabled", 0);
        return;
    }

    uniform_set_int(u, "giEnabled", 1);
    uniform_set_int(u, "giSlotCount", published);
    uniform_set_vec4_array(u, "giSlot", &slots[0][0], published * 4);
    uniform_set_vec2(u, "giAtlasSize", (const float[]){(float)atlas->width, (float)atlas->height});
    uniform_set_float(u, "giTileBorder", (float)GI_TILE_BORDER);
    uniform_set_vec2(u, "giTileRes",
                     (const float[]){(float)GI_IRRADIANCE_RES, (float)GI_VISIBILITY_RES});
}

// What a volume is doing, in the words --stream-probe prints.
static const char* gi_state_name(const GIVolume* gi, ResidencyState state) {
    if (gi->failed)
        return "failed";
    switch (state) {
        case RESIDENCY_OUT:
            return "out";
        case RESIDENCY_UPLOAD:
            return "uploading";
        case RESIDENCY_CAPTURE:
            return gi->dirty_count > 0 ? "sweeping" : "unswept";
        case RESIDENCY_LOADED:
            break;
    }
    return gi->dirty_count > 0 ? "converging" : "swept";
}

void gi_world_probe_print(const GIWorld* world, const LightingAtlas* atlas, int frame) {
    if (!world)
        return;
    const Residency* res = &world->residency;
    printf("stream gi frame=%d volumes=%zu slots=", frame, res->count);
    for (int s = 0; s < res->slots; ++s)
        printf(s ? ",%d" : "%d", res->holder[s]);
    printf("\n");
    for (size_t i = 0; i < res->count; ++i) {
        const ResidencyItem* item = &res->items[i];
        const GIVolume* gi = world->volumes[i];
        // Of the tiles wherever they are -- kept on the CPU or in the slot -- so two runs that
        // photographed one room alike agree on it whatever each did since.
        const uint32_t digest =
            item->kept || item->state == RESIDENCY_LOADED
                ? lighting_atlas_digest(atlas, gi_volume_rect(gi, atlas, item->slot), item->kept)
                : 0u;
        printf("stream gi idx=%zu slot=%d dist=%.2f captures=%d state=%s kept=%d digest=%08x\n", i,
               item->slot, (double)item->distance, gi->captures_total,
               gi_state_name(gi, item->state), item->kept ? 1 : 0, digest);
    }
    fflush(stdout);
}
