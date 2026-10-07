#include <math.h>
#include <stdlib.h>

#include "crt.h"
#include "ext/log.h"
#include "postfx.h"
#include "program.h"
#include "texture.h"
#include "uniform.h"
#include "util.h"

// The horizontal filter's exponent, CRTS's default "sharpness"; and the widest the composite
// colour runs, at bleed 1, where a texel's chroma reaches about two texels either side.
#define CRT_LUMA_BLUR       (-2.5f)
#define CRT_CHROMA_BLUR_MAX (-0.3f)
// The warp at curvature 1, CRTS's "more warping" doubled; and its corner rounding.
#define CRT_WARP_MAX 0.0625f
#define CRT_CORNER   3.0f
// Window pixels per signal line below which the scanlines fade to fused: a beam drawn across
// fewer than two pixels beats against the pixel grid rather than reading as a line.
#define CRT_SCAN_FADE_LO 1.5f
#define CRT_SCAN_FADE_HI 2.5f
// The mask's pixel is a window pixel up to this many lines, and a multiple of one past it, so a
// phosphor stays a size the eye can resolve on a high-density display.
#define CRT_MASK_LINES 1080.0f

struct Crt {
    ShaderProgram* resample;
    ShaderProgram* show;
    GLuint picture_tex, picture_fbo;
    int picture_w, picture_h;
    GLuint signal_tex, signal_fbo;
    int signal_w, signal_h;
};

Crt* create_crt(void) {
    Crt* crt = calloc(1, sizeof(Crt));
    if (!crt)
        return NULL;
    crt->resample = create_crt_resample_program();
    crt->show = create_crt_program();
    if (!crt->resample || !crt->show) {
        log_error("CRT: programs unavailable");
        free_crt(crt);
        return NULL;
    }
    return crt;
}

void free_crt(Crt* crt) {
    if (!crt)
        return;
    gl_delete_texture(&crt->picture_tex);
    gl_delete_fbo(&crt->picture_fbo);
    gl_delete_texture(&crt->signal_tex);
    gl_delete_fbo(&crt->signal_fbo);
    free_program(crt->resample);
    free_program(crt->show);
    free(crt);
}

// An RGBA16F target, `filter` both ways. False, with nothing left behind, when it is incomplete.
static bool _crt_target(int w, int h, GLint filter, GLuint* tex, GLuint* fbo) {
    *tex = create_texture_2d_float(w, h, GL_RGBA16F, GL_RGBA, NULL);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        return true;
    gl_delete_texture(tex);
    gl_delete_fbo(fbo);
    return false;
}

// The picture is window-sized and RGBA16F, so what the tonemap writes is not quantized before
// the CRT resamples it -- the dither belongs to the window's write -- and the overlay, whose
// scissor is in window pixels, lands where it would in the window.
GLuint crt_picture_fbo(Crt* crt, int width, int height) {
    if (!crt || width <= 0 || height <= 0)
        return 0;
    if (crt->picture_fbo && crt->picture_w == width && crt->picture_h == height)
        return crt->picture_fbo;
    gl_delete_texture(&crt->picture_tex);
    gl_delete_fbo(&crt->picture_fbo);
    crt->picture_w = crt->picture_h = 0;
    if (!_crt_target(width, height, GL_LINEAR, &crt->picture_tex, &crt->picture_fbo)) {
        log_error("CRT: the %dx%d picture target is incomplete", width, height);
        return 0;
    }
    crt->picture_w = width;
    crt->picture_h = height;
    return crt->picture_fbo;
}

// The signal: `lines` tall and as wide as the window's shape makes it.
static bool _crt_signal(Crt* crt, int width, int height, float lines) {
    const int h = (int)fmaxf(16.0f, fminf(roundf(lines), (float)height));
    const int w = (int)fmaxf(16.0f, roundf((float)h * (float)width / (float)height));
    if (crt->signal_fbo && crt->signal_w == w && crt->signal_h == h)
        return true;
    gl_delete_texture(&crt->signal_tex);
    gl_delete_fbo(&crt->signal_fbo);
    crt->signal_w = crt->signal_h = 0;
    if (!_crt_target(w, h, GL_NEAREST, &crt->signal_tex, &crt->signal_fbo)) {
        log_error("CRT: the %dx%d signal target is incomplete", w, h);
        return false;
    }
    crt->signal_w = w;
    crt->signal_h = h;
    return true;
}

/*
 * CRTS's exposure match (CrtsTone, contrast 1), for this beam and this mask. The beams pass about
 * (1.5 - thin) of the light, CRTS's own estimate; the slot mask passes a third of its stripes
 * whole and two thirds at `dark`, and alternate rows at `dark`, so (1 + 2 dark) / 3 times
 * (1 + dark) / 2. The curve returned maps mid-grey 0.18 to 0.18 over that loss and 1 to 1.
 */
static void _crt_tone(float thin, float dark, float out[2]) {
    const float mask = (1.0f + 2.0f * dark) / 3.0f * (1.0f + dark) / 2.0f;
    const float mid_out = 0.18f / ((1.5f - thin) * mask);
    const float mid_in = 0.18f;
    out[0] = (mid_out - mid_in) / ((1.0f - mid_in) * mid_out);
    out[1] = (mid_in - mid_in * mid_out) / ((1.0f - mid_in) * mid_out);
}

void crt_present(Crt* crt, const PostFX* fx, GLuint target_fbo, int width, int height) {
    if (!crt || !fx || !crt->picture_tex || width <= 0 || height <= 0)
        return;
    const float lines = fx->crt_lines > 0.0f ? fx->crt_lines : 480.0f;
    if (!_crt_signal(crt, width, height, lines))
        return;

    const GLboolean depth_was_on = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean blend_was_on = glIsEnabled(GL_BLEND);
    const GLboolean scissor_was_on = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    // The signal, each texel the window pixels it covers.
    glBindFramebuffer(GL_FRAMEBUFFER, crt->signal_fbo);
    glViewport(0, 0, crt->signal_w, crt->signal_h);
    glUseProgram(crt->resample->id);
    UniformManager* r = crt->resample->uniforms;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, crt->picture_tex);
    uniform_set_int(r, "pictureTex", 0);
    const float footprint[2] = {1.0f / (float)crt->signal_w, 1.0f / (float)crt->signal_h};
    uniform_set_vec2(r, "footprint", footprint);
    draw_fullscreen_quad(fx->quad_vao);

    // The television.
    const float clamp01_scan = fminf(fmaxf(fx->crt_scanlines, 0.0f), 1.0f);
    const float per_line = (float)height / (float)crt->signal_h;
    const float t = fminf(
        fmaxf((per_line - CRT_SCAN_FADE_LO) / (CRT_SCAN_FADE_HI - CRT_SCAN_FADE_LO), 0.0f), 1.0f);
    const float thin = 0.5f + 0.5f * clamp01_scan * t * t * (3.0f - 2.0f * t);
    const float dark = 1.0f - fminf(fmaxf(fx->crt_mask, 0.0f), 1.0f);
    const float bleed = fminf(fmaxf(fx->crt_bleed, 0.0f), 1.0f);
    const float bow = fminf(fmaxf(fx->crt_curvature, 0.0f), 1.0f) * CRT_WARP_MAX;
    const float warp[2] = {bow, bow * (float)height / (float)width};
    const float signal_size[2] = {(float)crt->signal_w, (float)crt->signal_h};
    const float output_size[2] = {(float)width, (float)height};
    float tone[2];
    _crt_tone(thin, dark, tone);

    glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
    glViewport(0, 0, width, height);
    glUseProgram(crt->show->id);
    UniformManager* s = crt->show->uniforms;
    glBindTexture(GL_TEXTURE_2D, crt->signal_tex);
    uniform_set_int(s, "signalTex", 0);
    uniform_set_vec2(s, "signalSize", signal_size);
    uniform_set_vec2(s, "outputSize", output_size);
    uniform_set_vec2(s, "warp", warp);
    uniform_set_float(s, "corner", CRT_CORNER);
    uniform_set_float(s, "thin", thin);
    uniform_set_float(s, "blur", CRT_LUMA_BLUR);
    uniform_set_float(s, "chromaBlur",
                      CRT_LUMA_BLUR + (CRT_CHROMA_BLUR_MAX - CRT_LUMA_BLUR) * bleed);
    uniform_set_float(s, "maskDark", dark);
    uniform_set_float(s, "maskScale", fmaxf(1.0f, roundf((float)height / CRT_MASK_LINES)));
    uniform_set_vec2(s, "tone", tone);
    uniform_set_int(s, "ditherEnabled", fx->dither_enabled ? 1 : 0);
    uniform_set_float(s, "ditherStrength", fx->dither_strength);
    draw_fullscreen_quad(fx->quad_vao);

    glUseProgram(0);
    if (depth_was_on)
        glEnable(GL_DEPTH_TEST);
    if (blend_was_on)
        glEnable(GL_BLEND);
    if (scissor_was_on)
        glEnable(GL_SCISSOR_TEST);
}
