#include "rain_render.h"

#include <math.h>
#include <stdlib.h>

#include "engine.h"
#include "postfx.h"
#include "profiler.h"
#include "program.h"
#include "rain.h"
#include "scene.h"
#include "shadow.h"
#include "uniform.h"
#include "util.h"
#include "water.h"
#include "ext/log.h"

// RAIN_DROP_MIN_MM: the smallest drop drawn, which is what the density a streak stands for
// is counted above. RAIN_STREAK_BOXES and the splash's numbers: rain_vert.glsl reads which
// kind of drop an instance is from its index, so both sides count the same instances.
#include "../shaders/include/rain_constants.glsl"

// This program's units, its own ledger: 0 the scene depth, 1 the frame copy, 2 the fog volume,
// 3 the punctual array the cover is a layer of.
#define RAIN_BEHIND_UNIT 1
#define RAIN_FOG_UNIT    2
#define RAIN_COVER_UNIT  3

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
    gl_delete_texture(&rr->behind_tex);
    gl_delete_fbo(&rr->behind_fbo);
    free(rr);
}

// The mipped copy of the canvas, rebuilt when the canvas changes size.
static bool _ensure_behind(RainRenderer* rr, int w, int h) {
    if (rr->behind_tex && rr->behind_w == w && rr->behind_h == h)
        return true;
    gl_delete_texture(&rr->behind_tex);
    gl_delete_fbo(&rr->behind_fbo);
    glGenTextures(1, &rr->behind_tex);
    glBindTexture(GL_TEXTURE_2D, rr->behind_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_2D);
    glGenFramebuffers(1, &rr->behind_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rr->behind_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rr->behind_tex, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ok) {
        log_error("Rain: the frame copy's framebuffer is incomplete");
        gl_delete_texture(&rr->behind_tex);
        gl_delete_fbo(&rr->behind_fbo);
        return false;
    }
    rr->behind_w = w;
    rr->behind_h = h;
    return true;
}

/*
 * The drip lines as the vertex stage reads them, one vec4 of each kind a line: its ends, with
 * the running end of its slots and the height its drops land at in the fourth components, and
 * its cycle and the chance a slot drips in one. A slot drips at most once a cycle, so a line
 * whose rate outruns its slots drips from every one and no faster.
 *
 * The cycle is reckoned onto the water when it lies above the line's ground, which only ever
 * shortens it.
 */
static void _upload_drips(const Rain* rain, UniformManager* u, const int* slots, int total,
                          float water_level) {
    if (total <= 0)
        return;
    const float flow = rain_drip_flow(rain);
    vec4 from[RAIN_DRIP_MAX], to[RAIN_DRIP_MAX], cycle[RAIN_DRIP_MAX];
    int end = 0;
    for (int i = 0; i < rain->drip_line_count; i++) {
        const RainDripLine* l = &rain->drips[i];
        end += slots[i];
        const float land = fmaxf(l->ground, water_level);
        const float period = rain_drip_period(l, land, fmaxf(rain->fall_scale, 0.0f));
        glm_vec4((float*)l->from, (float)end, from[i]);
        glm_vec4((float*)l->to, land, to[i]);
        const float chance = slots[i] > 0 ? l->rate * flow * period / (float)slots[i] : 0.0f;
        glm_vec4_copy((vec4){period, fminf(chance, 1.0f), 0.0f, 0.0f}, cycle[i]);
    }
    uniform_set_int(u, "dripLines", rain->drip_line_count);
    uniform_set_vec4_array(u, "dripFrom", (const float*)from, rain->drip_line_count);
    uniform_set_vec4_array(u, "dripTo", (const float*)to, rain->drip_line_count);
    uniform_set_vec4_array(u, "dripCycle", (const float*)cycle, rain->drip_line_count);
    uniform_set_float(u, "dripTerminal",
                      rain_terminal_velocity(RAIN_DRIP_MM) * fmaxf(rain->fall_scale, 0.0f));
    uniform_set_float(u, "dripBrightness", fmaxf(rain->drip_brightness, 0.0f));
}

void rain_render_drops(RainRenderer* rr, Engine* engine, const Scene* scene,
                       const PostFXLateDraw* late) {
    if (!rr || !engine || !scene || !late || !engine->camera)
        return;
    const Rain* rain = scene->rain;
    // Streaks and splashes while rain falls; the drips for as long as there is water to drip.
    const bool falls = rain_falling(rain);
    const int per_box = falls && rain->streak_count > 0 ? rain->streak_count : 0;
    const int falling = RAIN_STREAK_BOXES * per_box;
    // The splash slots are a square grid, whose side the shader reads a slot's cell from.
    const int splash_side =
        falls && rain->splash_count > 0 ? (int)floorf(sqrtf((float)rain->splash_count)) : 0;
    const int droplets = splash_side * splash_side * RAIN_SPLASH_DROPLETS;
    int drip_slots[RAIN_DRIP_MAX];
    const int drips = rain_draws(rain) ? rain_drip_slots(rain, drip_slots) : 0;
    // A build failure was logged when the engine registered the program.
    ShaderProgram* program = engine_find_program(engine, CETRA_PROGRAM_RAIN);
    const int instances = falling + droplets + drips * (1 + RAIN_SPLASH_DROPLETS);
    if (!rain_draws(rain) || instances == 0 || !program || !late->scene_depth)
        return;

    // The camera's own motion over the frame: a streak is the drop's path relative to it,
    // so walking into the rain slants it toward the eye.
    vec3 cam_vel = GLM_VEC3_ZERO_INIT;
    const float dt = (float)engine->render_delta;
    if (dt > 0.0f) {
        glm_vec3_scale(engine->camera_travel, 1.0f / dt, cam_vel);
        if (glm_vec3_norm(cam_vel) > RAIN_MAX_CAMERA_SPEED)
            glm_vec3_zero(cam_vel);
    }

    // The canvas postfx bound, put back after the copy.
    GLint canvas = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &canvas);
    profiler_scope_begin(engine->profiler, "rain");
    // What the drops refract, taken before any of them draws.
    if (!_ensure_behind(rr, late->width, late->height)) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)canvas);
        profiler_scope_end(engine->profiler);
        return;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)canvas);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rr->behind_fbo);
    glBlitFramebuffer(0, 0, late->width, late->height, 0, 0, late->width, late->height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, rr->behind_tex);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)canvas);
    glUseProgram(program->id);
    UniformManager* u = program->uniforms;
    uniform_set_mat4(u, "view", (const float*)engine->view_matrix);
    uniform_set_mat4(u, "projection", (const float*)engine->projection_matrix);
    uniform_set_vec3(u, "cameraPos", engine->camera->position);
    uniform_set_vec3(u, "cameraVelocity", cam_vel);
    uniform_set_vec2(u, "viewport", (vec2){(float)late->width, (float)late->height});
    uniform_set_float(u, "time", (float)engine->render_time);
    uniform_set_vec3(u, "rainWind", rain->wind_now);
    // Infinite once nothing falls, which no instance then reads.
    uniform_set_float(u, "mpLambda", falls ? rain_mp_lambda(rain->rate_mmh) : 1.0f);
    uniform_set_float(u, "fallScale", fmaxf(rain->fall_scale, 0.0f));
    uniform_set_float(u, "shutter", rain->shutter_s);
    uniform_set_float(u, "boxHalf", rain->streak_radius);
    uniform_set_int(u, "dropsPerBox", per_box);
    // Each box holds the same count over a volume RAIN_STREAK_BOX_SCALE^3 the last's, which is
    // the shader's to scale by: what a streak stands for is counted against the innermost.
    const float side = 2.0f * rain->streak_radius;
    uniform_set_float(u, "dropsPerStreak",
                      per_box > 0 ? rain_drop_density(rain->rate_mmh, RAIN_DROP_MIN_MM) * side *
                                        side * side / (float)per_box
                                  : 0.0f);

    // How many drops big enough to splash land in one slot's cell over its life. A slot draws
    // at most one splash a life: it fires with that count as its chance when there are fewer,
    // and when there are more it fires every life and stands for all of them.
    const float cell = splash_side > 0 ? 2.0f * rain->splash_radius / (float)splash_side : 1.0f;
    const float per_life = rain_splash_flux(rain->rate_mmh) * cell * cell * RAIN_SPLASH_LIFE *
                           fmaxf(rain->splash_amount, 0.0f);
    uniform_set_int(u, "splashSide", splash_side);
    uniform_set_float(u, "splashCell", cell);
    uniform_set_float(u, "splashFire", fminf(per_life, 1.0f));
    uniform_set_float(u, "splashStandsFor", fmaxf(per_life, 1.0f));
    uniform_set_float(u, "splashSize", fmaxf(rain->splash_size, 0.0f));
    // Down the rain from above a cell to what the occlusion map says it lands on: the
    // direction the map was cast along. The map does not hold water, so a surface that draws
    // this frame is handed over as its still plane.
    uniform_set_vec3(u, "rainTravel", rain->travel);
    const bool water = water_will_draw(scene->water, engine, engine->current_render_mode);
    uniform_set_int(u, "rainWaterPresent", water ? 1 : 0);
    uniform_set_float(u, "rainWaterLevel", water ? scene->water->level : 0.0f);
    _upload_drips(rain, u, drip_slots, drips, water ? scene->water->level : -INFINITY);
    uniform_set_float(u, "streakWidth", rain->streak_width);
    uniform_set_float(u, "streakBrightness", rain->streak_brightness);
    uniform_set_float(u, "forwardG", rain->streak_forward_g);
    uniform_set_float(u, "glintShare", glm_clamp(rain->streak_glint, 0.0f, 1.0f));
    uniform_set_float(u, "sheen", fmaxf(rain->streak_sheen, 0.0f));

    shadow_bind_rain_cover(scene->shadow_system, program, RAIN_COVER_UNIT);

    // A drop refracts a wide field -- about 165 degrees -- so what it shows is an average of
    // the frame round it rather than the pixel behind it: a level whose texel is a
    // thirty-second of the frame's longer side.
    const int longest = late->width > late->height ? late->width : late->height;
    uniform_set_float(u, "behindLod", fmaxf(0.0f, log2f((float)longest) - 5.0f));
    glActiveTexture(GL_TEXTURE0 + RAIN_BEHIND_UNIT);
    glBindTexture(GL_TEXTURE_2D, rr->behind_tex);
    uniform_set_int(u, "behindTex", RAIN_BEHIND_UNIT);

    glActiveTexture(GL_TEXTURE0 + RAIN_FOG_UNIT);
    glBindTexture(GL_TEXTURE_3D, late->fog_volume);
    uniform_set_int(u, "fogVolume", RAIN_FOG_UNIT);
    uniform_set_int(u, "fogSlices", late->fog_slices);
    uniform_set_float(u, "fogNear", late->fog_near);
    uniform_set_float(u, "fogFar", late->fog_far);
    uniform_set_float(u, "fogDepthDist", late->fog_depth_dist);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, late->scene_depth);
    uniform_set_int(u, "sceneDepth", 0);

    const GLboolean depth_test = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cull = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(rr->vao);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, instances);
    glBindVertexArray(0);
    // Back to the chain's resting state, which every composite there restores to.
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (depth_test)
        glEnable(GL_DEPTH_TEST);
    if (cull)
        glEnable(GL_CULL_FACE);
    check_gl_error("rain drops");
    profiler_scope_end(engine->profiler);
}

void rain_publish_to_postfx(const Rain* rain, PostFX* fx) {
    if (!fx)
        return;
    fx->rain_sigma = 0.0f;
    // After the rain stops as well as during it: what is still wet still carries the marker.
    fx->rain_wet = rain_active(rain);
    if (!rain_falling(rain))
        return;
    fx->rain_sigma = rain_extinction(rain->rate_mmh) * fmaxf(rain->mist, 0.0f);
    // Its own knob, though the drops are the same drops: a lobe that differs from the
    // streaks' scatters a lamp differently in the distance than in front of it, which is a
    // look rather than physics. The default is the streaks'.
    fx->rain_forward_g = rain->mist_forward_g;
    // The outermost streak box's half-width. Inside it the streaks already stand for every
    // drop, so a medium there too would count the rain twice.
    fx->rain_near =
        rain->streak_count > 0
            ? rain->streak_radius * powf(RAIN_STREAK_BOX_SCALE, (float)(RAIN_STREAK_BOXES - 1))
            : 0.0f;
}

// What a scene with no rain binds: dry, and every look at its neutral.
static const Rain RAIN_DRY = {
    .travel = {0.0f, -1.0f, 0.0f},
    .wet_darkening = 1.0f,
    .puddle_scale = 1.0f,
    .ripple_size = 1.0f,
};

void rain_bind_surface(const Rain* rain, ShaderProgram* program) {
    if (!program || !program->uniforms)
        return;
    if (!rain)
        rain = &RAIN_DRY;
    UniformManager* u = program->uniforms;
    uniform_set_float(u, "rainWetness", rain->wetness);
    uniform_set_float(u, "rainDarkening", fmaxf(rain->wet_darkening, 0.0f));
    uniform_set_float(u, "rainPuddleLevel", rain->puddle_level);
    uniform_set_float(u, "rainPuddleScale", fmaxf(rain->puddle_scale, 0.01f));
    uniform_set_float(u, "rainPuddleRelief", glm_clamp(rain->puddle_relief, 0.0f, 1.0f));
    uniform_set_float(u, "rainRippleActivity", rain_ripple_activity(rain));
    uniform_set_float(u, "rainRippleSize", fmaxf(rain->ripple_size, 0.01f));
    uniform_set_float(u, "rainRippleStrength", fmaxf(rain->ripple_strength, 0.0f));
    uniform_set_vec3(u, "rainTravel", rain->travel);
    uniform_set_float(u, "rainBeadClock", rain->bead_clock);
    uniform_set_float(u, "rainGlassLens", fmaxf(rain->glass_lens, 0.0f));
    uniform_set_float(u, "rainGlassSize", fmaxf(rain->glass_drop_size, 0.01f));
}
