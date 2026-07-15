#include <stdlib.h>

#include "postfx.h"
#include "uniform.h"
#include "util.h"

#include "ext/log.h"

// Creates a single-sample color-only FBO; returns false on failure
static bool create_color_fbo(int width, int height, GLenum internal_format, GLuint* out_fbo,
                             GLuint* out_texture) {
    GLenum format = internal_format == GL_RGBA16F ? GL_RGBA : GL_RGB;

    glGenTextures(1, out_texture);
    glBindTexture(GL_TEXTURE_2D, *out_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal_format, width, height, 0, format, GL_FLOAT,
                 NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Clamp so bloom sampling doesn't wrap around screen edges
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, out_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *out_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *out_texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("PostFX framebuffer is not complete (%dx%d)", width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

PostFX* create_postfx(int width, int height) {
    if (width <= 0 || height <= 0) {
        log_error("create_postfx: invalid size %dx%d", width, height);
        return NULL;
    }

    PostFX* fx = calloc(1, sizeof(PostFX));
    if (!fx) {
        log_error("Failed to allocate memory for PostFX");
        return NULL;
    }

    fx->width = width;
    fx->height = height;
    fx->bloom_width = width / 2 > 0 ? width / 2 : 1;
    fx->bloom_height = height / 2 > 0 ? height / 2 : 1;

    fx->exposure = 1.0f;
    fx->bloom_threshold = 1.0f;
    fx->bloom_knee = 0.5f;
    fx->bloom_strength = 0.08f;
    fx->bloom_enabled = true;
    fx->blur_iterations = 2;
    fx->tonemap_mode = POSTFX_TONEMAP_NEUTRAL;

    // The HDR resolve target must be RGBA16F to match the MSAA source
    // (multisample blits require identical formats); the bloom chain never
    // reads alpha, so the cheaper packed-float format halves its bandwidth
    if (!create_color_fbo(fx->width, fx->height, GL_RGBA16F, &fx->hdr_fbo, &fx->hdr_texture)) {
        free_postfx(fx);
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (!create_color_fbo(fx->bloom_width, fx->bloom_height, GL_R11F_G11F_B10F,
                              &fx->bloom_fbo[i], &fx->bloom_texture[i])) {
            free_postfx(fx);
            return NULL;
        }
    }

    fx->bright_program = create_bloom_bright_program();
    fx->blur_program = create_bloom_blur_program();
    fx->tonemap_program = create_tonemap_program();
    if (!fx->bright_program || !fx->blur_program || !fx->tonemap_program) {
        free_postfx(fx);
        return NULL;
    }

    // Sampler bindings never change; set them once on the program objects
    glUseProgram(fx->bright_program->id);
    uniform_set_int(fx->bright_program->uniforms, "hdrTex", 0);
    glUseProgram(fx->blur_program->id);
    uniform_set_int(fx->blur_program->uniforms, "image", 0);
    glUseProgram(fx->tonemap_program->id);
    uniform_set_int(fx->tonemap_program->uniforms, "hdrTex", 0);
    uniform_set_int(fx->tonemap_program->uniforms, "bloomTex", 1);
    glUseProgram(0);

    create_fullscreen_quad_vao(&fx->quad_vao, &fx->quad_vbo);

    check_gl_error("create_postfx");
    return fx;
}

void free_postfx(PostFX* fx) {
    if (!fx)
        return;

    glDeleteFramebuffers(1, &fx->hdr_fbo);
    glDeleteTextures(1, &fx->hdr_texture);
    glDeleteFramebuffers(2, fx->bloom_fbo);
    glDeleteTextures(2, fx->bloom_texture);

    free_program(fx->bright_program);
    free_program(fx->blur_program);
    free_program(fx->tonemap_program);

    glDeleteVertexArrays(1, &fx->quad_vao);
    glDeleteBuffers(1, &fx->quad_vbo);

    free(fx);
}

void postfx_run(PostFX* fx, GLuint msaa_fbo, GLuint target_fbo, bool frame_is_hdr) {
    if (!fx)
        return;

    PostFXTonemapMode mode = frame_is_hdr ? fx->tonemap_mode : POSTFX_TONEMAP_PASSTHROUGH;

    // Fullscreen composite passes need blending and depth testing off
    GLboolean depth_was_on = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend_was_on = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // Resolve MSAA HDR into the single-sample HDR texture (formats must
    // match exactly for a multisample blit, both are RGBA16F)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fx->hdr_fbo);
    glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->width, fx->height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    if (mode == POSTFX_TONEMAP_PASSTHROUGH) {
        // Display-ready frame: plain copy to the target (single-sample
        // blits may convert formats), skipping bloom and tone mapping
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fx->hdr_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);
        glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->width, fx->height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    } else {
        if (fx->bloom_enabled) {
            // Bright pass into half-res buffer 0 (linear sampling downsamples)
            glBindFramebuffer(GL_FRAMEBUFFER, fx->bloom_fbo[0]);
            glViewport(0, 0, fx->bloom_width, fx->bloom_height);
            glUseProgram(fx->bright_program->id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, fx->hdr_texture);
            uniform_set_float(fx->bright_program->uniforms, "threshold", fx->bloom_threshold);
            uniform_set_float(fx->bright_program->uniforms, "knee", fx->bloom_knee);
            draw_fullscreen_quad(fx->quad_vao);

            // Separable Gaussian ping-pong; result ends in bloom_texture[0]
            glUseProgram(fx->blur_program->id);
            const float horizontal[2] = {1.0f / (float)fx->bloom_width, 0.0f};
            const float vertical[2] = {0.0f, 1.0f / (float)fx->bloom_height};
            for (int i = 0; i < fx->blur_iterations; i++) {
                glBindFramebuffer(GL_FRAMEBUFFER, fx->bloom_fbo[1]);
                glBindTexture(GL_TEXTURE_2D, fx->bloom_texture[0]);
                uniform_set_vec2(fx->blur_program->uniforms, "direction", horizontal);
                draw_fullscreen_quad(fx->quad_vao);

                glBindFramebuffer(GL_FRAMEBUFFER, fx->bloom_fbo[0]);
                glBindTexture(GL_TEXTURE_2D, fx->bloom_texture[1]);
                uniform_set_vec2(fx->blur_program->uniforms, "direction", vertical);
                draw_fullscreen_quad(fx->quad_vao);
            }
        }

        // Composite + tone map into the target framebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
        glViewport(0, 0, fx->width, fx->height);
        glUseProgram(fx->tonemap_program->id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fx->hdr_texture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, fx->bloom_texture[0]);
        uniform_set_float(fx->tonemap_program->uniforms, "exposure", fx->exposure);
        uniform_set_float(fx->tonemap_program->uniforms, "bloomStrength", fx->bloom_strength);
        uniform_set_int(fx->tonemap_program->uniforms, "bloomEnabled", fx->bloom_enabled ? 1 : 0);
        uniform_set_int(fx->tonemap_program->uniforms, "tonemapMode", (int)mode);
        draw_fullscreen_quad(fx->quad_vao);

        glUseProgram(0);
        glActiveTexture(GL_TEXTURE0);
    }

    if (depth_was_on)
        glEnable(GL_DEPTH_TEST);
    if (blend_was_on)
        glEnable(GL_BLEND);
}
