#include <stdlib.h>
#include <string.h>

#include "gi_volume.h"
#include "engine.h"
#include "async_loader.h"
#include "ibl.h"
#include "lighting_atlas.h"
#include "render.h"
#include "scene.h"
#include "shadow.h"
#include "stream.h"
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

GIVolume* create_gi_volume(int nx, int ny, int nz) {
    if (nx < 1 || ny < 1 || nz < 1)
        return NULL;

    GIVolume* gi = malloc(sizeof(GIVolume));
    if (!gi) {
        log_error("Failed to allocate GI volume");
        return NULL;
    }
    memset(gi, 0, sizeof(GIVolume));

    gi->counts[0] = nx;
    gi->counts[1] = ny;
    gi->counts[2] = nz;
    gi->irradiance_rows = ny * nz * IRR_PITCH;
    gi->first_pass = true;
    gi->resident_slot = -1;
    gi->far_clip = 1.0f;
    glm_vec3_one(gi->spacing);

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
    GIVolume* gi = create_gi_volume(counts[0], counts[1], counts[2]);
    if (!gi)
        return NULL;
    gi->classify = true;
    gi_volume_fit(gi, lo, hi);
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
    free(gi->kept);
    free(gi);
}

void gi_volume_fit(GIVolume* gi, const vec3 aabb_min, const vec3 aabb_max) {
    if (!gi)
        return;
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
    gi_volume_mark_dirty(gi);
}

void gi_volume_mark_dirty(GIVolume* gi) {
    if (!gi)
        return;
    gi->dirty_count = gi_probe_count(gi);
    gi->next_probe = 0;
}

bool gi_volume_active(const GIVolume* gi) {
    // Not merely allocated: a volume mid-first-sweep holds tiles that were never
    // written, and sampling those would show as black blotches that resolve over
    // the next few frames. Withhold it until the opening sweep completes.
    return gi && !gi->failed && gi->targets_ready && !gi->first_pass && !gi->upload_pending;
}

bool gi_volume_pending(const GIVolume* gi) {
    return gi && !gi->failed && gi->first_pass && gi->dirty_count > 0;
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

// The capture scratch. The tiles themselves live in the scene's lighting atlas, which a
// slot's first sweep overwrites whole, so nothing here clears them.
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
static void gi_project_tile(GIVolume* gi, const LightingAtlas* atlas, const int region[2],
                            int probe, int mode, float hysteresis) {
    const bool visibility = mode == GI_PROJECT_VISIBILITY;
    const int res = visibility ? GI_VISIBILITY_RES : GI_IRRADIANCE_RES;
    int ox, oy;
    gi_tile_origin(gi, probe, visibility, &ox, &oy);
    ox += region[0];
    oy += region[1];

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
    if (mode == GI_PROJECT_CLASSIFY)
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); // VAO bound once by the caller
    if (mode == GI_PROJECT_CLASSIFY)
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

// Capture up to `rate` probes of a resident volume into its slot (`stream_rate` in an opening
// sweep begun after load). True when this call finished converging it.
static bool gi_volume_capture(GIVolume* gi, struct Engine* engine, struct Scene* scene,
                              const LightingAtlas* atlas, const int region[2], int rate,
                              int stream_rate) {
    if (gi->failed || gi->dirty_count <= 0)
        return false; // converged: the steady state, and it costs nothing

    // Capturing before the textures land would bake placeholder materials into
    // the atlas. probe.c blocks on the loader because it captures once at load;
    // here the dirty count is untouched, so skipping simply retries next frame.
    if (engine->async_loader && async_loader_is_busy(engine->async_loader))
        return false;

    if (!gi_ensure_targets(gi, engine))
        return false;

    // The opening sweep runs in one frame, ignoring `rate`, and takes each
    // capture outright rather than blending. Amortising it would buy nothing:
    // gi_volume_active withholds a half-swept slot, so spreading the first
    // sweep over `probes/rate` frames only delays GI appearing at all -- and a
    // headless run short enough to be a golden would finish before it ever did.
    // `rate` governs RE-convergence, where the previous tiles are still valid to
    // sample and the cost genuinely wants spreading. This mirrors the sky's LUTs
    // and the reflection probe: both bake once, at load, in full.
    //
    // A volume first swept LATER, as the camera comes within reach of it, is the exception
    // (spec 13.24): it is someone else's building, its slot is withheld until it is done, and
    // a frame spent sweeping all of it at once is a hitch in the middle of a walk.
    const int probes = gi_probe_count(gi);
    int budget = probes;
    if (gi->first_pass && gi->streamed && stream_rate > 0)
        budget = stream_rate;
    else if (!gi->first_pass && rate > 0)
        budget = rate;
    if (budget > gi->dirty_count)
        budget = gi->dirty_count;

    const float hysteresis = gi->first_pass ? 0.0f : 0.97f;

    GLint saved_fbo;
    GLint saved_viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);
    glGetIntegerv(GL_VIEWPORT, saved_viewport);

    // Shared burst policy (see render.h). Nothing it bakes needs undoing beyond
    // the restore below: the frame's own shadow pass runs after this and
    // overwrites the maps with the camera-fit cascades it needs.
    SceneCaptureState saved_capture;
    // IRRADIANCE: what this bakes is added to the analytic direct term, so a
    // derived emissive panel's own surface must not appear in it (render.h).
    scene_capture_begin(engine, scene, SCENE_CAPTURE_IRRADIANCE, &saved_capture);

    for (int n = 0; n < budget; ++n) {
        int probe = gi->next_probe;
        gi->next_probe = (gi->next_probe + 1) % probes;
        gi->dirty_count--;

        vec3 pos = {0};
        gi_probe_position(gi, probe, pos);
        scene_capture_faces(engine, scene, scene->ibl, pos, gi->capture_color, gi->capture_depth,
                            GI_CAPTURE_FACE, GI_NEAR_CLIP, gi->far_clip);

        // Projection is a fullscreen-quad pass; depth and culling would only get
        // in its way. Both go back as found: this runs inside the frame, after
        // the frame top has set the culling the rest of the frame draws with.
        GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glBindVertexArray(gi->quad_vao);
        gi_project_tile(gi, atlas, region, probe, GI_PROJECT_IRRADIANCE, hysteresis);
        gi_project_tile(gi, atlas, region, probe, GI_PROJECT_VISIBILITY, hysteresis);
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
        if (cull_was)
            glEnable(GL_CULL_FACE);

        // The same probe again with only back faces drawn, against the front faces' depth
        // still held in capture_depth.
        if (gi->classify) {
            engine->capturing_back_faces = true;
            scene_capture_faces(engine, scene, scene->ibl, pos, gi->capture_color,
                                gi->classify_depth, GI_CAPTURE_FACE, GI_NEAR_CLIP, gi->far_clip);
            engine->capturing_back_faces = false;
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glBindVertexArray(gi->quad_vao);
            gi_project_tile(gi, atlas, region, probe, GI_PROJECT_CLASSIFY, hysteresis);
            glBindVertexArray(0);
            glEnable(GL_DEPTH_TEST);
            if (cull_was)
                glEnable(GL_CULL_FACE);
        }
    }

    gi->captures_total += budget;
    const bool converged = gi->dirty_count <= 0;
    if (converged) {
        gi->first_pass = false;
        gi->streamed = false;
        log_info("GI volume converged: %d captures total", gi->captures_total);
    }

    scene_capture_end(engine, scene, &saved_capture);

    // Blend ENABLED is the engine's baseline (set once at init; the G-buffer
    // relies on it and disables per attachment). The tile blend above turned it
    // off, so restoring means putting it back on -- an earlier version of this
    // line disabled it and called that the baseline, which handed every frame
    // after a sweep a context the rest of the renderer does not expect.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)saved_fbo);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    check_gl_error("gi volume update");
    return converged;
}

// Copy the slot's texels to the CPU, so leaving residency costs nothing to come back from.
// Once per convergence: a converged volume is never captured again until its light changes.
static void gi_volume_keep(GIVolume* gi, const LightingAtlas* atlas, const int region[2]) {
    int w = 0, h = 0;
    gi_volume_atlas_extent(gi, &w, &h);
    if (!gi->kept)
        gi->kept = malloc(sizeof(uint16_t) * 4 * (size_t)w * (size_t)h);
    if (!gi->kept) {
        log_warn("GI volume: no memory to keep its tiles; it will sweep again if it leaves");
        return;
    }
    lighting_atlas_read_rect(atlas, region[0], region[1], w, h, gi->kept);
}

// Put kept texels back into the slot the volume has been admitted to.
static void gi_volume_restore(GIVolume* gi, const LightingAtlas* atlas, const int region[2]) {
    int w = 0, h = 0;
    gi_volume_atlas_extent(gi, &w, &h);
    lighting_atlas_write_rect(atlas, region[0], region[1], w, h, gi->kept);
    gi->upload_pending = false;
    gi->first_pass = false;
    gi->dirty_count = 0;
}

GIWorld* create_gi_world(void) {
    GIWorld* world = calloc(1, sizeof(GIWorld));
    if (!world) {
        log_error("Failed to allocate the GI world");
        return NULL;
    }
    world->enabled = true;
    world->rate = 2;
    world->stream_rate = 32;
    for (int s = 0; s < GI_RESIDENT_MAX; ++s)
        world->holder[s] = -1;
    return world;
}

void free_gi_world(GIWorld* world) {
    if (!world)
        return;
    for (int i = 0; i < world->count; ++i)
        free_gi_volume(world->volumes[i]);
    free(world->volumes);
    free(world->slot_of);
    free(world->home);
    free(world->distance);
    free(world);
}

bool gi_world_add(GIWorld* world, GIVolume* gi) {
    if (!world || !gi) {
        free_gi_volume(gi);
        return false;
    }
    if (world->count == world->capacity) {
        int capacity = world->capacity ? world->capacity * 2 : 4;
        GIVolume** volumes = realloc(world->volumes, sizeof(GIVolume*) * (size_t)capacity);
        if (volumes)
            world->volumes = volumes;
        int* slot_of = realloc(world->slot_of, sizeof(int) * (size_t)capacity);
        if (slot_of)
            world->slot_of = slot_of;
        float* distance = realloc(world->distance, sizeof(float) * (size_t)capacity);
        if (distance)
            world->distance = distance;
        int* home = realloc(world->home, sizeof(int) * (size_t)capacity);
        if (home)
            world->home = home;
        if (!volumes || !slot_of || !distance || !home) {
            log_error("Failed to grow the GI world");
            free_gi_volume(gi);
            return false;
        }
        world->capacity = capacity;
    }
    world->volumes[world->count] = gi;
    world->slot_of[world->count] = -1;
    world->home[world->count] = -1;
    world->distance[world->count] = 0.0f;
    world->count++;
    return true;
}

void gi_world_rank(GIWorld* world, const struct Engine* engine) {
    if (!world || !world->enabled || world->count == 0 || !engine)
        return;

    vec3 eye = {0.0f, 0.0f, 0.0f};
    if (engine->camera)
        glm_vec3_copy(engine->camera->position, eye);
    for (int i = 0; i < world->count; ++i) {
        const GIVolume* gi = world->volumes[i];
        if (gi->failed) {
            world->distance[i] = -1.0f;
            continue;
        }
        vec3 box_max;
        for (int c = 0; c < 3; ++c)
            box_max[c] = gi->grid_min[c] + gi->spacing[c] * (float)gi->counts[c];
        world->distance[i] = stream_box_distance(eye, gi->grid_min, box_max);
    }
    stream_assign(world->distance, world->count, GI_RESIDENT_MAX, GI_STREAM_MARGIN, world->slot_of,
                  world->holder, world->home);

    for (int i = 0; i < world->count; ++i) {
        GIVolume* gi = world->volumes[i];
        const int s = world->slot_of[i];
        if (s == gi->resident_slot)
            continue;
        gi->resident_slot = s;
        gi->upload_pending = false;
        if (gi->kept) {
            // Evicted, its texels wait on the CPU; admitted, the update puts them back.
            gi->upload_pending = s >= 0;
            continue;
        }
        // Evicted with nothing kept, or admitted with nothing to put back: the slot it held is
        // another's, so it sweeps from the start when it next holds one. At load in one frame;
        // after it at the stream rate, unless the camera is already inside, where it is the
        // light on screen.
        if (gi->failed)
            continue;
        gi->first_pass = true;
        gi->streamed = world->opened && world->distance[i] > 0.0f;
        gi_volume_mark_dirty(gi);
    }
}

void gi_world_update(GIWorld* world, struct Engine* engine, struct Scene* scene) {
    if (!world || !world->enabled || !engine || !scene)
        return;
    bool work = false;
    for (int s = 0; s < GI_RESIDENT_MAX; ++s) {
        const int v = world->holder[s];
        if (v >= 0 && (world->volumes[v]->upload_pending ||
                       (!world->volumes[v]->failed && world->volumes[v]->dirty_count > 0)))
            work = true;
    }
    if (!work)
        return;
    const LightingAtlas* atlas = lighting_atlas_sync(scene, engine);
    if (!atlas)
        return;
    for (int s = 0; s < GI_RESIDENT_MAX; ++s) {
        const int v = world->holder[s];
        if (v < 0)
            continue;
        GIVolume* gi = world->volumes[v];
        int region[2];
        lighting_atlas_gi_region(atlas, s, &region[0], &region[1]);
        if (gi->upload_pending) {
            gi_volume_restore(gi, atlas, region);
            continue;
        }
        if (gi_volume_capture(gi, engine, scene, atlas, region, world->rate, world->stream_rate)) {
            gi_volume_keep(gi, atlas, region);
            world->opened = true;
        }
    }
}

bool gi_world_pending(const GIWorld* world) {
    if (!world || !world->enabled)
        return false;
    for (int s = 0; s < GI_RESIDENT_MAX; ++s)
        if (world->holder[s] >= 0 && gi_volume_pending(world->volumes[world->holder[s]]))
            return true;
    return false;
}

bool gi_world_dirty(const GIWorld* world) {
    if (!world || !world->enabled)
        return false;
    for (int s = 0; s < GI_RESIDENT_MAX; ++s) {
        const int v = world->holder[s];
        if (v >= 0 && !world->volumes[v]->failed && world->volumes[v]->dirty_count > 0)
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
    for (int i = 0; i < world->count; ++i) {
        GIVolume* gi = world->volumes[i];
        free(gi->kept);
        gi->kept = NULL;
        if (gi->resident_slot < 0 || gi->upload_pending) {
            gi->upload_pending = false;
            gi->first_pass = true;
        }
        gi_volume_mark_dirty(gi);
    }
}

void gi_world_shift_origin(GIWorld* world, const vec3 delta) {
    if (!world)
        return;
    for (int i = 0; i < world->count; ++i)
        glm_vec3_sub(world->volumes[i]->grid_min, (float*)delta, world->volumes[i]->grid_min);
}

void gi_world_bind(const GIWorld* world, const LightingAtlas* atlas, ShaderProgram* program) {
    if (!program || !program->uniforms)
        return;
    UniformManager* u = program->uniforms;

    // Pointed at its own unit even when off, the way the IBL samplers are: a
    // sampler left on its default unit 0 shares a slot with the material
    // textures, and that is only ever safe by accident.
    uniform_set_int(u, "giAtlasTex", GI_ATLAS_TEXTURE_UNIT);

    // Every resident volume is published, ready or not: a fragment inside one still sweeping
    // takes the environment's answer rather than a neighbour's edge (gi_volume.glsl).
    float slots[GI_RESIDENT_MAX * 4][4];
    int published = 0;
    bool any_ready = false;
    if (world && world->enabled && atlas && atlas->texture) {
        for (int s = 0; s < GI_RESIDENT_MAX; ++s) {
            const int v = world->holder[s];
            if (v < 0)
                continue;
            const GIVolume* gi = world->volumes[v];
            const bool ready = gi_volume_active(gi);
            any_ready = any_ready || ready;
            int rx = 0, ry = 0;
            lighting_atlas_gi_region(atlas, s, &rx, &ry);
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
            row[0] = (float)rx;
            row[1] = (float)ry;
            row[2] = 0.0f;
            row[3] = 0.0f;
            published++;
        }
    }

    if (!any_ready) {
        uniform_set_int(u, "giEnabled", 0);
        return;
    }

    glActiveTexture(GL_TEXTURE0 + GI_ATLAS_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, atlas->texture);
    glActiveTexture(GL_TEXTURE0);

    uniform_set_int(u, "giEnabled", 1);
    uniform_set_int(u, "giSlotCount", published);
    uniform_set_vec4_array(u, "giSlot", &slots[0][0], published * 4);
    uniform_set_vec2(u, "giAtlasSize", (const float[]){(float)atlas->width, (float)atlas->height});
    uniform_set_float(u, "giTileBorder", (float)GI_TILE_BORDER);
    uniform_set_vec2(u, "giTileRes",
                     (const float[]){(float)GI_IRRADIANCE_RES, (float)GI_VISIBILITY_RES});
}

void gi_world_probe_print(const GIWorld* world, int frame) {
    if (!world)
        return;
    printf("stream gi frame=%d volumes=%d slots=", frame, world->count);
    for (int s = 0; s < GI_RESIDENT_MAX; ++s)
        printf(s ? ",%d" : "%d", world->holder[s]);
    printf("\n");
    for (int i = 0; i < world->count; ++i) {
        const GIVolume* gi = world->volumes[i];
        const char* state = gi->failed            ? "failed"
                            : gi->upload_pending  ? "uploading"
                            : gi->first_pass      ? (gi->dirty_count > 0 ? "sweeping" : "unswept")
                            : gi->dirty_count > 0 ? "converging"
                                                  : "swept";
        printf("stream gi idx=%d slot=%d dist=%.2f captures=%d state=%s kept=%d\n", i,
               world->slot_of[i], (double)world->distance[i], gi->captures_total, state,
               gi->kept ? 1 : 0);
    }
    fflush(stdout);
}
