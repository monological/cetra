#include <stdlib.h>
#include <string.h>

#include "gi_volume.h"
#include "engine.h"
#include "async_loader.h"
#include "ibl.h"
#include "mask_array.h"
#include "render.h"
#include "scene.h"
#include "shadow.h"
#include "uniform.h"
#include "util.h"
#include "ext/log.h"

// Tile pitch including the gutter on both sides.
#define IRR_PITCH (GI_IRRADIANCE_RES + 2 * GI_TILE_BORDER)
#define VIS_PITCH (GI_VISIBILITY_RES + 2 * GI_TILE_BORDER)

static int gi_probe_count(const GIVolume* gi) {
    return gi->counts[0] * gi->counts[1] * gi->counts[2];
}

// Atlas layout: the volume's X axis runs along the atlas X, and (y,z) is folded
// into the atlas Y. Probe index is ix + nx*(iy + ny*iz), so a row of the atlas is
// a row of the grid -- which is the axis the 8-probe gather steps along fastest.
// The irradiance block sits above the visibility block; the two have different
// tile pitches, so they cannot share rows.
static void gi_tile_origin(const GIVolume* gi, int probe, bool visibility, int* out_x,
                           int* out_y) {
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

    gi->enabled = true;
    gi->counts[0] = nx;
    gi->counts[1] = ny;
    gi->counts[2] = nz;
    gi->rate = 2;
    gi->first_pass = true;
    gi->far_clip = 1.0f;
    glm_vec3_one(gi->spacing);

    return gi;
}

void free_gi_volume(GIVolume* gi) {
    if (!gi)
        return;
    if (gi->atlas)
        glDeleteTextures(1, &gi->atlas);
    if (gi->capture_color)
        glDeleteTextures(1, &gi->capture_color);
    if (gi->capture_depth)
        glDeleteTextures(1, &gi->capture_depth);
    if (gi->tile_fbo)
        glDeleteFramebuffers(1, &gi->tile_fbo);
    if (gi->quad_vao)
        glDeleteVertexArrays(1, &gi->quad_vao);
    if (gi->quad_vbo)
        glDeleteBuffers(1, &gi->quad_vbo);
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
    return gi && gi->enabled && gi->atlas && !gi->failed && !gi->first_pass;
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

static bool gi_ensure_targets(GIVolume* gi, struct Engine* engine) {
    if (gi->atlas)
        return true;
    if (gi->failed)
        return false;

    const int rows = gi->counts[1] * gi->counts[2];
    gi->irradiance_rows = rows * IRR_PITCH;
    gi->atlas_w = gi->counts[0] * (IRR_PITCH > VIS_PITCH ? IRR_PITCH : VIS_PITCH);
    gi->atlas_h = gi->irradiance_rows + rows * VIS_PITCH;

    glGenTextures(1, &gi->atlas);
    glBindTexture(GL_TEXTURE_2D, gi->atlas);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, gi->atlas_w, gi->atlas_h, 0, GL_RGBA, GL_FLOAT,
                 NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    gi->capture_color = gi_make_cubemap(GI_CAPTURE_FACE, GL_RGB16F, GL_RGB, GL_FLOAT);
    gi->capture_depth =
        gi_make_cubemap(GI_CAPTURE_FACE, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_FLOAT);

    glGenFramebuffers(1, &gi->tile_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, gi->tile_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gi->atlas, 0);
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (!gi->atlas || !gi->capture_color || !gi->capture_depth || !complete) {
        log_error("GI volume targets incomplete; disabling irradiance probes");
        gi->failed = true;
        return false;
    }

    create_fullscreen_quad_vao(&gi->quad_vao, &gi->quad_vbo);

    gi->project_program = get_engine_shader_program_by_name(engine, "gi_project");
    if (!gi->project_program) {
        log_error("GI volume projection program missing; disabling irradiance probes");
        gi->failed = true;
        return false;
    }

    // The atlas starts undefined; a probe that is never reached would otherwise
    // blend against garbage forever.
    glBindFramebuffer(GL_FRAMEBUFFER, gi->tile_fbo);
    glViewport(0, 0, gi->atlas_w, gi->atlas_h);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    log_info("GI volume: %dx%dx%d probes, %dx%d atlas", gi->counts[0], gi->counts[1],
             gi->counts[2], gi->atlas_w, gi->atlas_h);
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

// Project the capture into one tile, gutter included.
static void gi_project_tile(GIVolume* gi, int probe, bool visibility, float hysteresis) {
    const int res = visibility ? GI_VISIBILITY_RES : GI_IRRADIANCE_RES;
    int ox, oy;
    gi_tile_origin(gi, probe, visibility, &ox, &oy);

    // Blended in place against the previous value with a constant alpha: legal
    // because the atlas is never bound for reading while it is the render target
    // -- this pass reads only the capture cube, and the scene pass that reads the
    // atlas runs separately. That is the shadow-map sequencing the codebase
    // already relies on, and it avoids a ping-pong that would have to copy every
    // untouched tile to update one.
    glBindFramebuffer(GL_FRAMEBUFFER, gi->tile_fbo);
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
    glActiveTexture(GL_TEXTURE0);
    uniform_set_int(u, "captureColor", 0);
    uniform_set_int(u, "captureDepth", 1);
    uniform_set_vec2(u, "tileOrigin", (const float[]){(float)ox, (float)oy});
    uniform_set_float(u, "tileRes", (float)res);
    uniform_set_float(u, "nearZ", GI_NEAR_CLIP);
    uniform_set_float(u, "farZ", gi->far_clip);
    uniform_set_int(u, "mode", visibility ? 1 : 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); // VAO bound once by the caller
}

void gi_volume_update(GIVolume* gi, struct Engine* engine, struct Scene* scene) {
    if (!gi || !gi->enabled || gi->failed || !engine || !scene)
        return;
    if (gi->dirty_count <= 0)
        return; // converged: the steady state, and it costs nothing

    // Capturing before the textures land would bake placeholder materials into
    // the atlas. probe.c blocks on the loader because it captures once at load;
    // here the dirty count is untouched, so skipping simply retries next frame.
    if (engine->async_loader && async_loader_is_busy(engine->async_loader))
        return;

    if (!gi_ensure_targets(gi, engine))
        return;

    // The opening sweep runs in one frame, ignoring `rate`, and takes each
    // capture outright rather than blending. Amortising it would buy nothing:
    // gi_volume_active withholds a half-swept atlas, so spreading the first
    // sweep over `probes/rate` frames only delays GI appearing at all -- and a
    // headless run short enough to be a golden would finish before it ever did.
    // `rate` governs RE-convergence, where the previous atlas is still valid to
    // sample and the cost genuinely wants spreading. This mirrors the sky's LUTs
    // and the reflection probe: both bake once, at load, in full.
    const int probes = gi_probe_count(gi);
    int budget = (gi->first_pass || gi->rate <= 0) ? probes : gi->rate;
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
    scene_capture_begin(engine, scene, &saved_capture);

    // What this volume bakes is IRRADIANCE, added to the analytic direct term
    // rather than shown to an eye -- so an emissive surface that also exists as a
    // derived area panel must sit this out, or its light reaches a receiver twice.
    // Raised here and not in scene_capture_faces because the reflection probe
    // goes through the same call and wants the opposite (engine.h).
    //
    // Around the whole burst rather than each face: the only other thing between
    // them is the projection pass, a fullscreen quad with no emissive in it.
    const bool saved_irradiance = engine->capturing_irradiance;
    engine->capturing_irradiance = true;

    for (int n = 0; n < budget; ++n) {
        int probe = gi->next_probe;
        gi->next_probe = (gi->next_probe + 1) % probes;
        gi->dirty_count--;

        vec3 pos = {0};
        gi_probe_position(gi, probe, pos);
        scene_capture_faces(engine, scene->ibl, pos, gi->capture_color, gi->capture_depth,
                            GI_CAPTURE_FACE, GI_NEAR_CLIP, gi->far_clip);

        // Projection is a fullscreen-quad pass; depth and culling would only get
        // in its way, and the capture left both enabled.
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glBindVertexArray(gi->quad_vao);
        gi_project_tile(gi, probe, false, hysteresis);
        gi_project_tile(gi, probe, true, hysteresis);
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
    }

    gi->captures_total += budget;
    if (gi->dirty_count <= 0) {
        gi->first_pass = false;
        log_info("GI volume converged: %d captures total", gi->captures_total);
    }

    engine->capturing_irradiance = saved_irradiance;
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
}

void gi_volume_bind(const GIVolume* gi, ShaderProgram* program) {
    if (!program || !program->uniforms)
        return;
    UniformManager* u = program->uniforms;

    // Pointed at its own unit even when off, the way the IBL samplers are: a
    // sampler left on its default unit 0 shares a slot with the material
    // textures, and that is only ever safe by accident.
    uniform_set_int(u, "giAtlasTex", GI_ATLAS_TEXTURE_UNIT);

    if (!gi_volume_active(gi)) {
        uniform_set_int(u, "giEnabled", 0);
        return;
    }

    glActiveTexture(GL_TEXTURE0 + GI_ATLAS_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, gi->atlas);
    glActiveTexture(GL_TEXTURE0);

    uniform_set_int(u, "giEnabled", 1);
    uniform_set_vec3(u, "giGridMin", (const float*)gi->grid_min);
    uniform_set_vec3(u, "giSpacing", (const float*)gi->spacing);
    uniform_set_vec3(u, "giCounts", (const float[]){(float)gi->counts[0], (float)gi->counts[1],
                                                    (float)gi->counts[2]});
    uniform_set_vec2(u, "giAtlasSize", (const float[]){(float)gi->atlas_w, (float)gi->atlas_h});
    uniform_set_float(u, "giIrradianceRows", (float)gi->irradiance_rows);
    uniform_set_float(u, "giFarClip", gi->far_clip);
    uniform_set_float(u, "giTileBorder", (float)GI_TILE_BORDER);
    uniform_set_vec2(u, "giTileRes",
                     (const float[]){(float)GI_IRRADIANCE_RES, (float)GI_VISIBILITY_RES});
}

void gi_volume_debug_blit(const GIVolume* gi, struct Engine* engine, int screen_w, int screen_h) {
    if (!gi || !gi->atlas || !engine)
        return;
    // Reuses the shared 2D-texture debug overlay (sky_debug_frag), which is a
    // scaled textured-quad DRAW rather than a blit -- the default framebuffer is
    // multisample and a single-sample blit into it is illegal on core profile.
    ShaderProgram* prog = get_engine_shader_program_by_name(engine, "sky_debug");
    if (!prog)
        return;

    GLint prev_viewport[4];
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // Scaled to fill most of the screen height at the atlas's own aspect: the
    // tiles are 8 and 16 texels across and nothing is legible at 1:1.
    int h = screen_h - 20;
    int w = (int)((float)h * (float)gi->atlas_w / (float)gi->atlas_h);
    if (w > screen_w - 20) {
        w = screen_w - 20;
        h = (int)((float)w * (float)gi->atlas_h / (float)gi->atlas_w);
    }
    glViewport(10, screen_h - 10 - h, w, h);
    glUseProgram(prog->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gi->atlas);
    // Point-sample for the overlay only: the atlas is magnified ~20x here and
    // bilinear smears the tile grid and its gutter into an unreadable wash --
    // which is exactly the structure the overlay exists to check.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    uniform_set_int(prog->uniforms, "lut", 0);
    // One exposure for two blocks that hold different quantities: bounced
    // radiance (small, a fraction of the direct light) above, normalised
    // distances (0..1) below. 4x puts both in range at once.
    uniform_set_float(prog->uniforms, "scale", 4.0f);
    draw_fullscreen_quad(gi->quad_vao);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    if (blend_was)
        glEnable(GL_BLEND);
    glUseProgram(0);
}
