#include "rain_render.h"

#include <math.h>
#include <stdlib.h>

#include "engine.h"
#include "engine_internal.h"
#include "ibl.h"
#include "postfx.h"
#include "profiler.h"
#include "program.h"
#include "rain.h"
#include "scene.h"
#include "shadow.h"
#include "uniform.h"
#include "util.h"
#include "ext/log.h"

// RAIN_DROP_MIN_MM: the smallest drop drawn, which is what the density a streak stands for
// is counted above.
#include "../shaders/include/rain_constants.glsl"

// Nested boxes around the camera, each three times the last: rain_vert.glsl reads the box
// from the instance index, so the count lives only in the draw.
#define RAIN_STREAK_BOXES 3

// The fog volume's unit in this program. Its own ledger, so any unit whose other tenants in
// this program are not sampler3D: 0 is the scene depth, 11 the irradiance cube and 10 and 15
// the shadow arrays bind_shadow_maps_to_program fills.
#define RAIN_FOG_UNIT 2

// Faster than this the camera did not move, it was placed: a cut or a respawn is not a streak
// across the whole frame.
#define RAIN_MAX_CAMERA_SPEED 50.0f

RainRenderer* create_rain_renderer(void) {
    RainRenderer* rr = calloc(1, sizeof(RainRenderer));
    if (!rr) {
        log_error("Failed to allocate RainRenderer");
        return NULL;
    }
    glGenVertexArrays(1, &rr->vao);
    return rr;
}

void free_rain_renderer(RainRenderer* rr) {
    if (!rr)
        return;
    if (rr->vao)
        glDeleteVertexArrays(1, &rr->vao);
    free(rr);
}

// The program, registered the first time it is asked for. A failed build is not retried:
// the log line is the answer, once.
static ShaderProgram* _rain_program(RainRenderer* rr, Engine* engine) {
    ShaderProgram* program = engine_find_program(engine, CETRA_PROGRAM_RAIN);
    if (program || rr->program_failed)
        return program;
    program = create_rain_program();
    if (!program) {
        rr->program_failed = true;
        return NULL;
    }
    engine_add_program(engine, program);
    return program;
}

void rain_render_streaks(RainRenderer* rr, Engine* engine, Scene* scene,
                         const PostFXLateDraw* late) {
    if (!rr || !engine || !scene || !late)
        return;
    const Rain* rain = scene->rain;
    if (!rain || !(rain->rate_mmh > 0.0f) || rain->streak_count <= 0 || !engine->camera) {
        rr->prev_valid = false;
        return;
    }
    ShaderProgram* program = _rain_program(rr, engine);
    if (!program)
        return;

    // The camera's own motion over the frame: a streak is the drop's path relative to it,
    // so walking into the rain slants it toward the eye.
    const float* eye = engine->camera->position;
    vec3 cam_vel = GLM_VEC3_ZERO_INIT;
    const float dt = (float)engine->render_delta;
    if (rr->prev_valid && dt > 0.0f) {
        glm_vec3_sub((float*)eye, rr->prev_eye, cam_vel);
        glm_vec3_scale(cam_vel, 1.0f / dt, cam_vel);
        if (glm_vec3_norm(cam_vel) > RAIN_MAX_CAMERA_SPEED)
            glm_vec3_zero(cam_vel);
    }
    glm_vec3_copy((float*)eye, rr->prev_eye);
    rr->prev_valid = true;

    // The resolve rebinds the scene framebuffer, so the canvas postfx bound is put back.
    GLint canvas = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &canvas);
    const GLuint depth = engine_resolve_scene_depth(engine);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)canvas);
    glViewport(0, 0, late->width, late->height);
    if (!depth)
        return;

    profiler_scope_begin(engine->profiler, "rain");
    glUseProgram(program->id);
    UniformManager* u = program->uniforms;
    uniform_set_mat4(u, "view", (const float*)engine->view_matrix);
    uniform_set_mat4(u, "projection", (const float*)engine->projection_matrix);
    uniform_set_vec3(u, "cameraPos", eye);
    uniform_set_vec3(u, "cameraVelocity", cam_vel);
    uniform_set_vec2(u, "viewport", (vec2){(float)late->width, (float)late->height});
    uniform_set_float(u, "rainTime", rain->time);
    uniform_set_vec3(u, "rainWind", rain->wind);
    uniform_set_float(u, "mpLambda", rain_mp_lambda(rain->rate_mmh));
    uniform_set_float(u, "shutter", rain->shutter_s);
    uniform_set_float(u, "boxHalf", rain->streak_radius);
    uniform_set_int(u, "dropsPerBox", rain->streak_count);
    const float side = 2.0f * rain->streak_radius;
    uniform_set_float(u, "dropsPerStreak",
                      rain_drop_density(rain->rate_mmh, RAIN_DROP_MIN_MM) * side * side * side /
                          (float)rain->streak_count);
    uniform_set_float(u, "streakWidth", rain->streak_width);
    uniform_set_float(u, "streakBrightness", rain->streak_brightness);
    uniform_set_float(u, "forwardG", rain->streak_forward_g);
    uniform_set_float(u, "glintShare", glm_clamp(rain->streak_glint, 0.0f, 1.0f));

    // The cover first: the binder leaves another unit active.
    bind_shadow_maps_to_program(scene->shadow_system, program);

    const IBLResources* ibl = scene->ibl;
    const bool ibl_on = ibl && ibl->precomputed;
    uniform_set_int(u, "iblEnabled", ibl_on ? 1 : 0);
    // Pointed at its unit even with no environment: left at its default of 0 it would sit on
    // the scene depth's unit as a second sampler type, which is an INVALID_OPERATION at draw.
    uniform_set_int(u, "irradianceMap", IBL_IRRADIANCE_TEXTURE_UNIT);
    if (ibl_on) {
        glActiveTexture(GL_TEXTURE0 + IBL_IRRADIANCE_TEXTURE_UNIT);
        glBindTexture(GL_TEXTURE_CUBE_MAP, ibl->irradiance_map);
        uniform_set_float(u, "iblIntensity", ibl->intensity);
    }
    uniform_set_vec3(u, "ambientRadiance", scene->ambient_radiance);

    glActiveTexture(GL_TEXTURE0 + RAIN_FOG_UNIT);
    glBindTexture(GL_TEXTURE_3D, late->fog_volume);
    uniform_set_int(u, "fogVolume", RAIN_FOG_UNIT);
    uniform_set_int(u, "fogSlices", late->fog_slices);
    uniform_set_float(u, "fogNear", late->fog_near);
    uniform_set_float(u, "fogFar", late->fog_far);
    uniform_set_float(u, "fogDepthDist", late->fog_depth_dist);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depth);
    uniform_set_int(u, "sceneDepth", 0);

    const GLboolean depth_test = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cull = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(rr->vao);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, RAIN_STREAK_BOXES * rain->streak_count);
    glBindVertexArray(0);
    // Back to the chain's resting state, which every composite there restores to.
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (depth_test)
        glEnable(GL_DEPTH_TEST);
    if (cull)
        glEnable(GL_CULL_FACE);
    check_gl_error("rain streaks");
    profiler_scope_end(engine->profiler);
}
