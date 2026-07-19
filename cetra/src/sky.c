#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sky.h"
#include "engine.h"
#include "ibl.h"
#include "util.h"
#include "ext/log.h"

// Unit cube (position only) for the environment-face and background draws
static const float SKY_CUBE_VERTS[] = {
    -1, -1, -1, 1,  1,  -1, 1,  -1, -1, 1,  1,  -1, -1, -1, -1, -1, 1,  -1,
    -1, -1, 1,  1,  -1, 1,  1,  1,  1,  1,  1,  1,  -1, 1,  1,  -1, -1, 1,
    -1, 1,  1,  -1, 1,  -1, -1, -1, -1, -1, -1, -1, -1, -1, 1,  -1, 1,  1,
    1,  1,  1,  1,  -1, -1, 1,  1,  -1, 1,  -1, -1, 1,  1,  1,  1,  1,  -1,
    -1, -1, -1, 1,  -1, -1, 1,  -1, 1,  1,  -1, 1,  -1, -1, 1,  -1, -1, -1,
    -1, 1,  -1, 1,  1,  1,  1,  1,  -1, 1,  1,  -1, 1,  1,  -1, 1,  -1};

static void sky_init_cube(SkyAtmosphere* sky) {
    if (sky->cube_vao)
        return;
    glGenVertexArrays(1, &sky->cube_vao);
    glGenBuffers(1, &sky->cube_vbo);
    glBindVertexArray(sky->cube_vao);
    glBindBuffer(GL_ARRAY_BUFFER, sky->cube_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(SKY_CUBE_VERTS), SKY_CUBE_VERTS, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);
}

SkyAtmosphere* create_sky_atmosphere(void) {
    SkyAtmosphere* sky = malloc(sizeof(SkyAtmosphere));
    if (!sky) {
        log_error("Failed to allocate sky atmosphere");
        return NULL;
    }
    memset(sky, 0, sizeof(SkyAtmosphere));

    sky->enabled = true;
    sky->sun_elevation_deg = 35.0f;
    sky->sun_azimuth_deg = 135.0f;
    sky->sun_disc_deg = 0.53f;
    sky_update_sun_dir(sky);

    return sky;
}

void free_sky_atmosphere(SkyAtmosphere* sky) {
    if (!sky)
        return;

    if (sky->transmittance_lut)
        glDeleteTextures(1, &sky->transmittance_lut);
    if (sky->multiscatter_lut)
        glDeleteTextures(1, &sky->multiscatter_lut);
    if (sky->sky_view_lut)
        glDeleteTextures(1, &sky->sky_view_lut);
    if (sky->quad_vao)
        glDeleteVertexArrays(1, &sky->quad_vao);
    if (sky->quad_vbo)
        glDeleteBuffers(1, &sky->quad_vbo);
    if (sky->cube_vao)
        glDeleteVertexArrays(1, &sky->cube_vao);
    if (sky->cube_vbo)
        glDeleteBuffers(1, &sky->cube_vbo);
    if (sky->capture_fbo)
        glDeleteFramebuffers(1, &sky->capture_fbo);
    if (sky->capture_rbo)
        glDeleteRenderbuffers(1, &sky->capture_rbo);

    free(sky);
}

void sky_update_sun_dir(SkyAtmosphere* sky) {
    if (!sky)
        return;

    float el = glm_rad(sky->sun_elevation_deg);
    float az = glm_rad(sky->sun_azimuth_deg);
    sky->sun_dir[0] = cosf(el) * sinf(az);
    sky->sun_dir[1] = sinf(el);
    sky->sun_dir[2] = cosf(el) * cosf(az);
}

// Bake one 2D LUT with a fullscreen pass into a fresh RGBA16F texture
// (delete-before-gen; the render_brdf_lut local-FBO shape). The destination
// texture is created on a SCRATCH unit so it never clobbers a source LUT the
// caller bound on unit 0 (the multiscatter pass samples the transmittance
// LUT there).
#define SKY_BAKE_SCRATCH_UNIT 6
static void bake_lut_2d(SkyAtmosphere* sky, ShaderProgram* program, GLuint* texture, int width,
                        int height) {
    if (*texture)
        glDeleteTextures(1, texture);
    glActiveTexture(GL_TEXTURE0 + SKY_BAKE_SCRATCH_UNIT);
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glActiveTexture(GL_TEXTURE0);

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);

    glViewport(0, 0, width, height);
    glUseProgram(program->id);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_fullscreen_quad(sky->quad_vao);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
}

int sky_bake_static_luts(SkyAtmosphere* sky, struct Engine* engine) {
    if (!sky || !engine) {
        log_error("Invalid state for sky LUT bake");
        return -1;
    }

    sky->transmittance_program = get_engine_shader_program_by_name(engine, "sky_transmittance");
    sky->multiscatter_program = get_engine_shader_program_by_name(engine, "sky_multiscatter");
    sky->debug_program = get_engine_shader_program_by_name(engine, "sky_debug");
    if (!sky->transmittance_program || !sky->multiscatter_program || !sky->debug_program) {
        log_error("Failed to get sky LUT shader programs");
        return -1;
    }

    if (sky->quad_vao == 0)
        create_fullscreen_quad_vao(&sky->quad_vao, &sky->quad_vbo);

    GLint prev_viewport[4];
    GLint prev_framebuffer;
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_framebuffer);

    // Partial-channel bake outputs must not feed the blend equation (the
    // BRDF LUT lesson); disable for the whole bake block
    GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);

    double t0 = glfwGetTime();
    bake_lut_2d(sky, sky->transmittance_program, &sky->transmittance_lut, SKY_TRANSMITTANCE_W,
                SKY_TRANSMITTANCE_H);

    // The multiple-scattering integral samples the transmittance LUT
    double t1 = glfwGetTime();
    glUseProgram(sky->multiscatter_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sky->transmittance_lut);
    uniform_set_int(sky->multiscatter_program->uniforms, "transmittanceLut", 0);
    bake_lut_2d(sky, sky->multiscatter_program, &sky->multiscatter_lut, SKY_MULTISCATTER_SIZE,
                SKY_MULTISCATTER_SIZE);
    double t2 = glfwGetTime();

    if (blend_was_enabled)
        glEnable(GL_BLEND);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_framebuffer);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    sky->luts_baked = true;
    log_info("Sky static LUTs baked: transmittance %.1f ms, multiscatter %.1f ms",
             (t1 - t0) * 1000.0, (t2 - t1) * 1000.0);
    return 0;
}

// Draw one LUT into a screen-corner viewport rectangle with the debug
// program (a textured-quad DRAW, not a blit: the default framebuffer is
// multisample and single-sample blits into it are illegal on core profile)
static void sky_debug_draw(SkyAtmosphere* sky, GLuint lut, int x, int y, int w, int h,
                           float scale) {
    glViewport(x, y, w, h);
    glUseProgram(sky->debug_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, lut);
    uniform_set_int(sky->debug_program->uniforms, "lut", 0);
    uniform_set_float(sky->debug_program->uniforms, "scale", scale);
    draw_fullscreen_quad(sky->quad_vao);
}

// Bake the sky-view LUT from the current sun (samples transmittance +
// multiscatter on units 0/1). Small fullscreen RGBA16F pass.
static void sky_bake_view_lut(SkyAtmosphere* sky) {
    if (sky->sky_view_lut)
        glDeleteTextures(1, &sky->sky_view_lut);
    glActiveTexture(GL_TEXTURE0 + SKY_BAKE_SCRATCH_UNIT);
    glGenTextures(1, &sky->sky_view_lut);
    glBindTexture(GL_TEXTURE_2D, sky->sky_view_lut);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, SKY_VIEW_W, SKY_VIEW_H, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glActiveTexture(GL_TEXTURE0);

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sky->sky_view_lut,
                           0);
    glViewport(0, 0, SKY_VIEW_W, SKY_VIEW_H);
    glUseProgram(sky->view_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sky->transmittance_lut);
    uniform_set_int(sky->view_program->uniforms, "transmittanceLut", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sky->multiscatter_lut);
    uniform_set_int(sky->view_program->uniforms, "multiscatterLut", 1);
    uniform_set_float(sky->view_program->uniforms, "sunCosZenith", sky->sun_dir[1]);
    draw_fullscreen_quad(sky->quad_vao);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glActiveTexture(GL_TEXTURE0);
}

int sky_bake(SkyAtmosphere* sky, struct IBLResources* ibl, struct Engine* engine) {
    if (!sky || !ibl || !engine) {
        log_error("Invalid state for sky bake");
        return -1;
    }
    sky->view_program = get_engine_shader_program_by_name(engine, "sky_view");
    sky->env_program = get_engine_shader_program_by_name(engine, "sky_env");
    sky->background_program = get_engine_shader_program_by_name(engine, "sky_background");
    if (!sky->view_program || !sky->env_program || !sky->background_program) {
        log_error("Failed to get sky render programs");
        return -1;
    }
    if (sky->quad_vao == 0)
        create_fullscreen_quad_vao(&sky->quad_vao, &sky->quad_vbo);
    sky_init_cube(sky);

    GLint prev_viewport[4];
    GLint prev_framebuffer;
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_framebuffer);
    GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    double t0 = glfwGetTime();
    sky_bake_view_lut(sky);

    // Render the sky into the environment cubemap (6 faces), then mip it so
    // the IBL convolutions can pre-average
    if (ibl->environment_cubemap)
        glDeleteTextures(1, &ibl->environment_cubemap);
    ibl_create_cubemap_texture(&ibl->environment_cubemap, SKY_ENV_SIZE, true);

    if (sky->capture_fbo == 0)
        glGenFramebuffers(1, &sky->capture_fbo);
    if (sky->capture_rbo == 0)
        glGenRenderbuffers(1, &sky->capture_rbo);
    glBindFramebuffer(GL_FRAMEBUFFER, sky->capture_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, sky->capture_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, SKY_ENV_SIZE, SKY_ENV_SIZE);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              sky->capture_rbo);
    glViewport(0, 0, SKY_ENV_SIZE, SKY_ENV_SIZE);

    mat4 views[6];
    mat4 projection;
    vec3 origin = {0.0f, 0.0f, 0.0f};
    ibl_capture_views(origin, views);
    glm_perspective(glm_rad(90.0f), 1.0f, 0.1f, 10.0f, projection);

    glUseProgram(sky->env_program->id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sky->sky_view_lut);
    uniform_set_int(sky->env_program->uniforms, "skyViewLut", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sky->transmittance_lut);
    uniform_set_int(sky->env_program->uniforms, "transmittanceLut", 1);
    uniform_set_vec3(sky->env_program->uniforms, "sunDir", sky->sun_dir);
    uniform_set_mat4(sky->env_program->uniforms, "projection", (float*)projection);
    for (int i = 0; i < 6; i++) {
        uniform_set_mat4(sky->env_program->uniforms, "view", (float*)views[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, ibl->environment_cubemap, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBindVertexArray(sky->cube_vao);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, ibl->environment_cubemap);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    double t1 = glfwGetTime();

    if (cull_was)
        glEnable(GL_CULL_FACE);
    if (blend_was)
        glEnable(GL_BLEND);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_framebuffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    // The rest of the IBL chain (irradiance + prefilter) from the sky cube
    if (ibl_bake_from_cubemap(ibl, engine, SKY_ENV_SIZE, SKY_PREFILTER_SIZE, SKY_PREFILTER_MIPS) !=
        0)
        return -1;
    double t2 = glfwGetTime();

    log_info("Sky bake: view+env %.1f ms, IBL %.1f ms", (t1 - t0) * 1000.0, (t2 - t1) * 1000.0);
    return 0;
}

void sky_render_background(SkyAtmosphere* sky, mat4 view, mat4 projection) {
    if (!sky || !sky->background_program || !sky->sky_view_lut)
        return;

    // Strip translation so the background sits at infinity
    mat4 rot_view;
    glm_mat4_copy(view, rot_view);
    rot_view[3][0] = 0.0f;
    rot_view[3][1] = 0.0f;
    rot_view[3][2] = 0.0f;

    glUseProgram(sky->background_program->id);
    UniformManager* u = sky->background_program->uniforms;
    uniform_set_mat4(u, "view", (float*)rot_view);
    uniform_set_mat4(u, "projection", (float*)projection);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sky->sky_view_lut);
    uniform_set_int(u, "skyViewLut", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, sky->transmittance_lut);
    uniform_set_int(u, "transmittanceLut", 1);
    uniform_set_vec3(u, "sunDir", sky->sun_dir);
    float cos_radius = cosf(glm_rad(sky->sun_disc_deg * 0.5f));
    uniform_set_float(u, "sunCosRadius", cos_radius);
    uniform_set_float(u, "sunIntensity", 20.0f);

    GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glBindVertexArray(sky->cube_vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    if (cull_was)
        glEnable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
}

void sky_debug_blit_luts(SkyAtmosphere* sky, int screen_w, int screen_h) {
    if (!sky || !sky->luts_baked || !sky->debug_program)
        return;
    (void)screen_w;

    GLint prev_viewport[4];
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // Transmittance (wide strip, [0,1] so unscaled) above the multiscatter
    // square. Psi is a normalized per-sun-illuminance transfer factor with
    // very small stored values, so the multiscatter view is heavily boosted.
    int tw = SKY_TRANSMITTANCE_W * 2, th = SKY_TRANSMITTANCE_H * 2;
    sky_debug_draw(sky, sky->transmittance_lut, 10, screen_h - 10 - th, tw, th, 1.0f);
    int mw = SKY_MULTISCATTER_SIZE * 4;
    sky_debug_draw(sky, sky->multiscatter_lut, 10, screen_h - 20 - th - mw, mw, mw, 200.0f);
    // Sky-view LUT below (mild boost to reveal the HDR horizon/zenith range)
    if (sky->sky_view_lut) {
        int sw = SKY_VIEW_W * 2, sh = SKY_VIEW_H * 2;
        sky_debug_draw(sky, sky->sky_view_lut, 10, screen_h - 30 - th - mw - sh, sw, sh, 0.3f);
    }

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    if (blend_was)
        glEnable(GL_BLEND);
    glUseProgram(0);
}
