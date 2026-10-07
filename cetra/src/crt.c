#include <cglm/cglm.h>
#include <math.h>
#include <stdlib.h>

#include "crt.h"
#include "ext/log.h"
#include "program.h"
#include "uniform.h"
#include "util.h"

// Window pixels per signal line below which the scanlines fade to fused: a beam drawn across
// fewer than two pixels beats against the pixel grid rather than reading as a line.
#define CRT_SCAN_FADE_LO 1.5f
#define CRT_SCAN_FADE_HI 2.5f

struct Crt {
    ShaderProgram* resample;
    ShaderProgram* show;
    GLColorTarget signal;
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
    gl_color_target_free(&crt->signal);
    free_program(crt->resample);
    free_program(crt->show);
    free(crt);
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

void crt_present(Crt* crt, GLuint picture, int width, int height, GLuint quad_vao,
                 const CrtLook* look) {
    if (!crt || !look || !picture || width <= 0 || height <= 0)
        return;
    const int lines = (int)fmaxf(16.0f, fminf(roundf(look->lines), (float)height));
    const int across = (int)fmaxf(16.0f, roundf((float)lines * (float)width / (float)height));
    if (!gl_color_target_ensure(&crt->signal, across, lines, "CRT signal"))
        return;
    const GLPassState pass = gl_pass_begin();

    // The signal, each texel the window pixels it covers.
    glBindFramebuffer(GL_FRAMEBUFFER, crt->signal.fbo);
    glViewport(0, 0, across, lines);
    glUseProgram(crt->resample->id);
    UniformManager* r = crt->resample->uniforms;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, picture);
    uniform_set_int(r, "pictureTex", 0);
    const float footprint[2] = {1.0f / (float)across, 1.0f / (float)lines};
    uniform_set_vec2(r, "footprint", footprint);
    draw_fullscreen_quad(quad_vao);

    // The television, in the window.
    const float fade = glm_smoothstep(CRT_SCAN_FADE_LO, CRT_SCAN_FADE_HI, (float)height / lines);
    const float thin = 0.5f + 0.5f * glm_clamp_zo(look->scanlines) * fade;
    const float dark = 1.0f - glm_clamp_zo(look->mask);
    float tone[2];
    _crt_tone(thin, dark, tone);
    const float signal_size[2] = {(float)across, (float)lines};
    const float output_size[2] = {(float)width, (float)height};

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glUseProgram(crt->show->id);
    UniformManager* s = crt->show->uniforms;
    glBindTexture(GL_TEXTURE_2D, crt->signal.tex);
    uniform_set_int(s, "signalTex", 0);
    uniform_set_vec2(s, "signalSize", signal_size);
    uniform_set_vec2(s, "outputSize", output_size);
    uniform_set_float(s, "curvature", glm_clamp_zo(look->curvature));
    uniform_set_float(s, "bleed", glm_clamp_zo(look->bleed));
    uniform_set_float(s, "thin", thin);
    uniform_set_float(s, "maskDark", dark);
    uniform_set_vec2(s, "tone", tone);
    uniform_set_int(s, "ditherEnabled", look->dither ? 1 : 0);
    uniform_set_float(s, "ditherStrength", look->dither_strength);
    draw_fullscreen_quad(quad_vao);

    glUseProgram(0);
    gl_pass_end(&pass);
}
