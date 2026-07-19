#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sky.h"
#include "engine.h"
#include "util.h"
#include "ext/log.h"

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

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    if (blend_was)
        glEnable(GL_BLEND);
    glUseProgram(0);
}
