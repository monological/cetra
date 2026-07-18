#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "probe.h"
#include "engine.h"
#include "render.h"
#include "shadow.h"
#include "postfx.h"
#include "ext/log.h"

ReflectionProbe* create_reflection_probe(void) {
    ReflectionProbe* probe = malloc(sizeof(ReflectionProbe));
    if (!probe) {
        log_error("Failed to allocate reflection probe");
        return NULL;
    }
    memset(probe, 0, sizeof(ReflectionProbe));

    probe->intensity = 1.0f;
    probe->box_fade = 0.2f;
    probe->enabled = true;

    return probe;
}

void free_reflection_probe(ReflectionProbe* probe) {
    if (!probe)
        return;

    if (probe->cubemap)
        glDeleteTextures(1, &probe->cubemap);
    if (probe->prefiltered)
        glDeleteTextures(1, &probe->prefiltered);

    free(probe);
}

// Render the scene once into the probe cubemap and GGX-prefilter it — or,
// for environment_only, prefilter the global environment straight into the
// probe (see probe.h).
//
// The scene path reuses the full pipeline (render_current_scene) with
// substituted per-face view/projection and the camera moved to the probe
// position, into the shared ibl capture FBO. Everything touched is saved
// and restored so a capture at load leaves the first real frame
// bit-identical. Skinned meshes capture at bind pose (no animation state
// has been evaluated at load).
int reflection_probe_capture(ReflectionProbe* probe, struct Engine* engine, Scene* scene,
                             float near_clip, float far_clip, bool environment_only) {
    if (!probe || !engine || !scene || !engine->camera) {
        log_error("Invalid state for probe capture");
        return -1;
    }
    IBLResources* ibl = scene->ibl;
    if (!ibl || !ibl->precomputed) {
        log_error("Probe capture requires precomputed IBL");
        return -1;
    }

    probe->max_lod = (float)(PROBE_PREFILTER_MIP_LEVELS - 1);

    if (environment_only) {
        // No scene render: the probe is the global environment, re-prefiltered
        // into a probe-owned chain so the parallax box can ground it
        GLint saved_env_viewport[4];
        GLint saved_env_fbo;
        glGetIntegerv(GL_VIEWPORT, saved_env_viewport);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_env_fbo);

        ibl_prefilter_cubemap(ibl, ibl->environment_cubemap, &probe->prefiltered,
                              PROBE_PREFILTER_SIZE, PROBE_PREFILTER_MIP_LEVELS);

        glBindFramebuffer(GL_FRAMEBUFFER, saved_env_fbo);
        glViewport(saved_env_viewport[0], saved_env_viewport[1], saved_env_viewport[2],
                   saved_env_viewport[3]);
        glUseProgram(0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

        log_info("Reflection probe grounded the environment at (%.2f, %.2f, %.2f)",
                 probe->position[0], probe->position[1], probe->position[2]);
        return 0;
    }

    // Textures may still be streaming in from the async loader; a capture
    // taken now would bake placeholder materials into the cubemap forever.
    // Sleep between drains — process_pending returns immediately while the
    // workers are still decoding.
    if (engine->async_loader) {
        while (async_loader_is_busy(engine->async_loader)) {
            if (async_loader_process_pending(engine->async_loader, scene->tex_pool, 64) == 0) {
                struct timespec ms = {0, 1000000};
                nanosleep(&ms, NULL);
            }
        }
    }

    // Shadow maps have not been rendered at load time; bake them so the
    // capture contains shadowed direct light and catcher darkening.
    render_shadow_depth_pass(engine, scene);

    // Save everything the capture substitutes
    mat4 saved_view, saved_projection, saved_view_proj, saved_prev_view_proj;
    glm_mat4_copy(engine->view_matrix, saved_view);
    glm_mat4_copy(engine->projection_matrix, saved_projection);
    glm_mat4_copy(engine->view_proj, saved_view_proj);
    glm_mat4_copy(engine->prev_view_proj, saved_prev_view_proj);

    Camera* camera = engine->camera;
    vec3 saved_cam_pos;
    glm_vec3_copy(camera->position, saved_cam_pos);
    float saved_near = camera->near_clip;
    float saved_far = camera->far_clip;

    bool saved_normals = engine->normals_this_frame;
    bool saved_aux = engine->aux_this_frame;
    bool saved_albedo = engine->albedo_this_frame;
    bool saved_taa = engine->postfx ? engine->postfx->taa_enabled : false;
    bool saved_refraction = engine->refraction_enabled;

    GLint saved_viewport[4];
    GLint saved_fbo;
    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fbo);

    // Color-only target: keep the G-buffer draw-buffer list at attachment 0,
    // and make sure the TAA branch can never jitter the capture projection.
    // Refraction must sit out too: its mid-frame resolve reads and rebinds
    // engine->framebuffer, which is NOT the capture target -- it would blit
    // a stale main frame and hijack the rest of the capture pass.
    engine->normals_this_frame = false;
    engine->aux_this_frame = false;
    engine->albedo_this_frame = false;
    engine->refraction_enabled = false;
    if (engine->postfx)
        engine->postfx->taa_enabled = false;

    // Scene GL state: capture may run before the render loop's per-frame
    // preamble has ever executed, so establish it explicitly
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (probe->cubemap)
        glDeleteTextures(1, &probe->cubemap);
    ibl_create_cubemap_texture(&probe->cubemap, PROBE_CUBEMAP_SIZE, true);

    // Supersampled capture: render each face at 2x into a temporary target
    // and box-downsample into the cube face (an exact 4-tap average at a 2:1
    // blit). The capture has no MSAA, and single-sample grazing-angle
    // aliasing at its horizon bakes in as stripe moire that mirror
    // reflections then magnify into banded streaks.
    const int ss_size = 2 * PROBE_CUBEMAP_SIZE;
    GLuint ss_tex = 0, face_fbo = 0;
    glGenTextures(1, &ss_tex);
    glBindTexture(GL_TEXTURE_2D, ss_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, ss_size, ss_size, 0, GL_RGB, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &face_fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, ibl->capture_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ss_tex, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, ibl->capture_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, ss_size, ss_size);

    mat4 views[6];
    ibl_capture_views(probe->position, views);

    // Per-face shading is evaluated from the probe point
    glm_vec3_copy(probe->position, camera->position);
    camera->near_clip = near_clip;
    camera->far_clip = far_clip;
    glm_perspective(glm_rad(90.0f), 1.0f, near_clip, far_clip, engine->projection_matrix);

    for (int i = 0; i < 6; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, ibl->capture_fbo);
        glViewport(0, 0, ss_size, ss_size);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glm_mat4_copy(views[i], engine->view_matrix);
        render_current_scene(engine, 0.0f);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, ibl->capture_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, face_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, probe->cubemap, 0);
        glBlitFramebuffer(0, 0, ss_size, ss_size, 0, 0, PROBE_CUBEMAP_SIZE, PROBE_CUBEMAP_SIZE,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    glDeleteFramebuffers(1, &face_fbo);
    glDeleteTextures(1, &ss_tex);

    glBindTexture(GL_TEXTURE_CUBE_MAP, probe->cubemap);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

    ibl_prefilter_cubemap(ibl, probe->cubemap, &probe->prefiltered, PROBE_PREFILTER_SIZE,
                          PROBE_PREFILTER_MIP_LEVELS);

    // Restore
    glm_mat4_copy(saved_view, engine->view_matrix);
    glm_mat4_copy(saved_projection, engine->projection_matrix);
    glm_mat4_copy(saved_view_proj, engine->view_proj);
    glm_mat4_copy(saved_prev_view_proj, engine->prev_view_proj);
    glm_vec3_copy(saved_cam_pos, camera->position);
    camera->near_clip = saved_near;
    camera->far_clip = saved_far;
    engine->normals_this_frame = saved_normals;
    engine->aux_this_frame = saved_aux;
    engine->albedo_this_frame = saved_albedo;
    engine->refraction_enabled = saved_refraction;
    if (engine->postfx)
        engine->postfx->taa_enabled = saved_taa;

    glBindFramebuffer(GL_FRAMEBUFFER, saved_fbo);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    log_info("Reflection probe captured at (%.2f, %.2f, %.2f)", probe->position[0],
             probe->position[1], probe->position[2]);

    return 0;
}

// Bind the probe for PBR consumption. The fragment stage is already at the
// driver's sampler limit, so the probe reuses the prefilteredMap slot: its
// prefiltered cubemap replaces the global environment on the IBL prefilter
// unit (call after bind_ibl_textures), and the shader switches the lookup
// with probeEnabled.
void bind_reflection_probe(const ReflectionProbe* probe, ShaderProgram* program) {
    if (!reflection_probe_active(probe) || !program || !program->uniforms)
        return;

    UniformManager* u = program->uniforms;

    glActiveTexture(GL_TEXTURE0 + IBL_PREFILTER_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_CUBE_MAP, probe->prefiltered);

    uniform_set_int(u, "probeEnabled", 1);
    uniform_set_vec3(u, "probePos", probe->position);
    uniform_set_vec3(u, "probeBoxMin", probe->box_min);
    uniform_set_vec3(u, "probeBoxMax", probe->box_max);
    uniform_set_float(u, "probeIntensity", probe->intensity);
    uniform_set_float(u, "probeMaxLOD", probe->max_lod);
    uniform_set_float(u, "probeBoxFade", probe->box_fade);

    glActiveTexture(GL_TEXTURE0);
}

// Flatten the probe (or its absence) into postfx's per-frame uniform block
void reflection_probe_publish_to_postfx(const ReflectionProbe* probe, PostFX* fx) {
    if (!fx)
        return;

    if (reflection_probe_active(probe)) {
        fx->probe_enabled = true;
        fx->probe_cubemap = probe->prefiltered;
        memcpy(fx->probe_pos, probe->position, sizeof(vec3));
        memcpy(fx->probe_box_min, probe->box_min, sizeof(vec3));
        memcpy(fx->probe_box_max, probe->box_max, sizeof(vec3));
        fx->probe_max_lod = probe->max_lod;
        fx->probe_intensity = probe->intensity;
    } else {
        fx->probe_enabled = false;
        fx->probe_cubemap = 0;
    }
}
