#include <stdlib.h>
#include <string.h>

#include "probe.h"
#include "engine.h"
#include "render.h"
#include "shadow.h"
#include "postfx.h"
#include "util.h" // gl_delete_texture
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

static bool probe_capture_valid(const ReflectionProbe* probe, const struct Engine* engine,
                                const Scene* scene) {
    if (!probe || !engine || !scene || !engine->camera) {
        log_error("Invalid state for probe capture");
        return false;
    }
    if (!scene->ibl || !scene->ibl->precomputed) {
        log_error("Probe capture requires precomputed IBL");
        return false;
    }
    return true;
}

int reflection_probe_prefilter_environment(ReflectionProbe* probe, const struct Engine* engine,
                                           Scene* scene) {
    if (!probe_capture_valid(probe, engine, scene))
        return -1;
    IBLResources* ibl = scene->ibl;
    probe->max_lod = (float)(PROBE_PREFILTER_MIP_LEVELS - 1);

    GLint saved_env_viewport[4];
    GLint saved_env_fbo;
    glGetIntegerv(GL_VIEWPORT, saved_env_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_env_fbo);

    ibl_prefilter_cubemap(ibl, ibl->prefilter_program, ibl->environment_cubemap,
                          &probe->prefiltered, PROBE_PREFILTER_SIZE, PROBE_PREFILTER_MIP_LEVELS,
                          true);

    glBindFramebuffer(GL_FRAMEBUFFER, saved_env_fbo);
    glViewport(saved_env_viewport[0], saved_env_viewport[1], saved_env_viewport[2],
               saved_env_viewport[3]);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    log_info("Reflection probe grounded the environment at (%.2f, %.2f, %.2f)", probe->position[0],
             probe->position[1], probe->position[2]);
    return 0;
}

// The scene path reuses the full pipeline (engine_render_scene) with substituted per-face
// view/projection and the camera moved to the probe position. Everything touched is saved and
// restored so the frame the capture runs in draws as it would have without it.
int reflection_probe_capture_face(ReflectionProbe* probe, struct Engine* engine, Scene* scene) {
    if (!probe_capture_valid(probe, engine, scene))
        return -1;
    if (probe->environment_only)
        return reflection_probe_prefilter_environment(probe, engine, scene) == 0 ? 1 : -1;
    IBLResources* ibl = scene->ibl;
    probe->max_lod = (float)(PROBE_PREFILTER_MIP_LEVELS - 1);

    if (probe->faces_captured == 0) {
        if (probe->cubemap)
            glDeleteTextures(1, &probe->cubemap);
        ibl_create_cubemap_texture(&probe->cubemap, PROBE_CUBEMAP_SIZE, true);
    }

    // Supersampled 2x: the capture has no MSAA, and single-sample grazing-angle
    // aliasing at its horizon bakes in as stripe moire that mirror reflections
    // then magnify into banded streaks.
    scene_capture_faces(engine, scene, ibl, probe->position, probe->cubemap, 0, PROBE_CUBEMAP_SIZE,
                        probe->near_clip, probe->far_clip, SCENE_FACES_SHADED,
                        probe->faces_captured, 1);
    const bool whole = ++probe->faces_captured == 6;
    if (whole) {
        glBindTexture(GL_TEXTURE_CUBE_MAP, probe->cubemap);
        glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

        // No medium: the capture is the scene round the probe, metres away, not the distant
        // sky the fog stands in front of.
        ibl_prefilter_cubemap(ibl, ibl->prefilter_program, probe->cubemap, &probe->prefiltered,
                              PROBE_PREFILTER_SIZE, PROBE_PREFILTER_MIP_LEVELS, false);
        probe->faces_captured = 0;
    }

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    if (whole)
        log_info("Reflection probe captured at (%.2f, %.2f, %.2f)", probe->position[0],
                 probe->position[1], probe->position[2]);
    return whole ? 1 : 0;
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

void reflection_probe_shift_origin(ReflectionProbe* probe, const vec3 delta) {
    if (!probe)
        return;
    glm_vec3_sub(probe->position, (float*)delta, probe->position);
    glm_vec3_sub(probe->box_min, (float*)delta, probe->box_min);
    glm_vec3_sub(probe->box_max, (float*)delta, probe->box_max);
}

void probe_release_capture_scratch(ReflectionProbe* probe) {
    if (!probe)
        return;
    gl_delete_texture(&probe->cubemap);
    gl_delete_texture(&probe->prefiltered);
}

// Flatten the probe (or its absence) into postfx's per-frame uniform block.
// Deliberately NO environment tier on probe-less scenes: publishing the IBL
// prefilter as the miss fallback was built and rejected -- SSR only shades
// the shadow catcher, and an invisible catcher that mirrors the sky over a
// darker background prints as a glowing pool the size of its quad. The
// environment answer is only right when a probe's parallax box re-grounds
// it; a probe-less miss stays empty and the march's own fades hide the
// reach limits.
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
