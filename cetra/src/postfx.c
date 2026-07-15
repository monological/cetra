#include <stdlib.h>

#include "postfx.h"
#include "uniform.h"
#include "util.h"

#include "ext/log.h"

// Fullscreen quad (GL_TRIANGLE_STRIP), loc0 = vec3 position, loc1 = vec2 uv
static const float quad_vertices[] = {
    -1.0f, 1.0f, 0.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
    1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 1.0f,  -1.0f, 0.0f, 1.0f, 0.0f,
};

// Creates a single-sample RGBA16F color-only FBO; returns false on failure
static bool create_color_fbo(int width, int height, GLuint* out_fbo, GLuint* out_texture) {
    glGenTextures(1, out_texture);
    glBindTexture(GL_TEXTURE_2D, *out_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
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
    fx->tonemap_mode = POSTFX_TONEMAP_ACES;

    if (!create_color_fbo(fx->width, fx->height, &fx->hdr_fbo, &fx->hdr_texture)) {
        free_postfx(fx);
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (!create_color_fbo(fx->bloom_width, fx->bloom_height, &fx->bloom_fbo[i],
                              &fx->bloom_texture[i])) {
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

    glGenVertexArrays(1, &fx->quad_vao);
    glGenBuffers(1, &fx->quad_vbo);
    glBindVertexArray(fx->quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, fx->quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);

    check_gl_error("create_postfx");
    return fx;
}

void free_postfx(PostFX* fx) {
    if (!fx)
        return;

    if (fx->hdr_fbo)
        glDeleteFramebuffers(1, &fx->hdr_fbo);
    if (fx->hdr_texture)
        glDeleteTextures(1, &fx->hdr_texture);
    for (int i = 0; i < 2; i++) {
        if (fx->bloom_fbo[i])
            glDeleteFramebuffers(1, &fx->bloom_fbo[i]);
        if (fx->bloom_texture[i])
            glDeleteTextures(1, &fx->bloom_texture[i]);
    }

    if (fx->bright_program)
        free_program(fx->bright_program);
    if (fx->blur_program)
        free_program(fx->blur_program);
    if (fx->tonemap_program)
        free_program(fx->tonemap_program);

    if (fx->quad_vao)
        glDeleteVertexArrays(1, &fx->quad_vao);
    if (fx->quad_vbo)
        glDeleteBuffers(1, &fx->quad_vbo);

    free(fx);
}

static void draw_quad(PostFX* fx) {
    glBindVertexArray(fx->quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void postfx_run(PostFX* fx, GLuint msaa_fbo, GLuint target_fbo, PostFXTonemapMode mode) {
    if (!fx)
        return;

    // The engine leaves blending and depth testing enabled for the whole
    // frame; both must be off for fullscreen composite passes
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // Resolve MSAA HDR into the single-sample HDR texture (formats must
    // match exactly for a multisample blit, both are RGBA16F)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fx->hdr_fbo);
    glBlitFramebuffer(0, 0, fx->width, fx->height, 0, 0, fx->width, fx->height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    if (fx->bloom_enabled) {
        // Bright pass into half-res buffer 0 (linear sampling downsamples)
        glBindFramebuffer(GL_FRAMEBUFFER, fx->bloom_fbo[0]);
        glViewport(0, 0, fx->bloom_width, fx->bloom_height);
        glUseProgram(fx->bright_program->id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fx->hdr_texture);
        uniform_set_int(fx->bright_program->uniforms, "hdrTex", 0);
        uniform_set_float(fx->bright_program->uniforms, "threshold", fx->bloom_threshold);
        uniform_set_float(fx->bright_program->uniforms, "knee", fx->bloom_knee);
        draw_quad(fx);

        // Separable Gaussian ping-pong; result ends in bloom_texture[0]
        glUseProgram(fx->blur_program->id);
        uniform_set_int(fx->blur_program->uniforms, "image", 0);
        const float horizontal[2] = {1.0f / (float)fx->bloom_width, 0.0f};
        const float vertical[2] = {0.0f, 1.0f / (float)fx->bloom_height};
        for (int i = 0; i < fx->blur_iterations; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, fx->bloom_fbo[1]);
            glBindTexture(GL_TEXTURE_2D, fx->bloom_texture[0]);
            uniform_set_vec2(fx->blur_program->uniforms, "direction", horizontal);
            draw_quad(fx);

            glBindFramebuffer(GL_FRAMEBUFFER, fx->bloom_fbo[0]);
            glBindTexture(GL_TEXTURE_2D, fx->bloom_texture[1]);
            uniform_set_vec2(fx->blur_program->uniforms, "direction", vertical);
            draw_quad(fx);
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
    uniform_set_int(fx->tonemap_program->uniforms, "hdrTex", 0);
    uniform_set_int(fx->tonemap_program->uniforms, "bloomTex", 1);
    uniform_set_float(fx->tonemap_program->uniforms, "exposure", fx->exposure);
    uniform_set_float(fx->tonemap_program->uniforms, "bloomStrength", fx->bloom_strength);
    uniform_set_int(fx->tonemap_program->uniforms, "bloomEnabled", fx->bloom_enabled ? 1 : 0);
    uniform_set_int(fx->tonemap_program->uniforms, "tonemapMode", (int)mode);
    draw_quad(fx);

    // Restore the engine's standing GL state
    glBindVertexArray(0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
}
