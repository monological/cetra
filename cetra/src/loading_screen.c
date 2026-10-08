#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "crt.h"
#include "engine_internal.h"
#include "ext/log.h"
#include "loading_screen.h"
#include "program.h"
#include "uniform.h"
#include "util.h"

// The ident's length in seconds: the rule beneath ENGINE is drawn by then
// (loading_logo_frag.glsl). Hiding waits for it and for one sweep of the rule's light after it.
#define LOADING_IDENT_SECONDS 3.0
#define LOADING_HOLD_SECONDS  0.6
// The switch-off, in seconds (loading_tape_frag.glsl's `off` over it).
#define LOADING_OFF_SECONDS 0.45
// The most the clock moves in one draw: a long stall pauses the ident rather than skipping it.
#define LOADING_MAX_STEP 0.1
// The fewest seconds between two draws outside a frame.
#define LOADING_MIN_INTERVAL (1.0 / 60.0)

// The set: fewer, thicker lines than the game's, a deeper mask and a rounder tube. 360 lines keep
// a stroke's stripes at three lines or more, and the bleed stays near the game's so they keep
// their colours side by side.
static const CrtLook LOADING_TUBE = {.lines = 360.0f,
                                     .scanlines = 0.7f,
                                     .mask = 0.6f,
                                     .curvature = 0.6f,
                                     .bleed = 0.5f,
                                     .dither = true,
                                     .dither_strength = 1.0f};

// Each palette as display colours: the ground, the five stripes from the core of a stroke out,
// and the block shadow.
static const uint32_t PALETTES[LOADING_PALETTE_COUNT][7] = {
    [LOADING_PALETTE_SUNSET] = {0x140A06, 0xF4E4BC, 0xF0B23A, 0xE5702A, 0xB83A26, 0x5C2E1C,
                                0x2A140C},
    [LOADING_PALETTE_HARVEST] = {0x10120A, 0xEFE3C0, 0xDDA837, 0xC0612B, 0x6B7A2A, 0x4B3020,
                                 0x1E1A0E},
    [LOADING_PALETTE_PHOSPHOR] = {0x020803, 0xE8FFE8, 0x9CFF9C, 0x3FD45A, 0x1F8A35, 0x0E4019,
                                  0x031208},
    [LOADING_PALETTE_BROADCAST] = {0x06061A, 0xF2EEFF, 0xB8C4FF, 0x6E7BFF, 0x7A3FD0, 0x2A1A6A,
                                   0x0A0828},
};

struct LoadingScreen {
    ShaderProgram* logo;
    ShaderProgram* tape;
    Crt* crt;
    GLColorTarget mark;    // the mark, linear
    GLColorTarget picture; // the tape's, display-encoded
    GLuint quad_vao, quad_vbo;
    bool shown;
    double clock;     // seconds of the ident shown
    double last_draw; // the wall clock at the last draw; < 0 before the first
    bool lift_asked;
    double lift; // seconds into the switch-off; < 0 before it starts
    int frame;   // draws so far, for the tape's noise
};

static LoadingScreen* _loading_screen_make(void) {
    LoadingScreen* ls = calloc(1, sizeof(LoadingScreen));
    if (!ls) {
        log_error("Loading screen: out of memory");
        return NULL;
    }
    ls->logo = create_loading_logo_program();
    ls->tape = create_loading_tape_program();
    ls->crt = create_crt();
    if (!ls->logo || !ls->tape || !ls->crt) {
        log_error("Loading screen: its programs are unavailable, so it will not show");
        free_loading_screen(ls);
        return NULL;
    }
    create_fullscreen_quad_vao(&ls->quad_vao, &ls->quad_vbo);
    return ls;
}

void free_loading_screen(LoadingScreen* ls) {
    if (!ls)
        return;
    free_program(ls->logo);
    free_program(ls->tape);
    free_crt(ls->crt);
    gl_color_target_free(&ls->mark);
    gl_color_target_free(&ls->picture);
    glDeleteVertexArrays(1, &ls->quad_vao);
    glDeleteBuffers(1, &ls->quad_vbo);
    free(ls);
}

void engine_show_loading_screen(Engine* engine) {
    if (!engine) {
        log_error("engine_show_loading_screen: NULL engine");
        return;
    }
    if (!engine->loading_screen)
        engine->loading_screen = _loading_screen_make();
    LoadingScreen* ls = engine->loading_screen;
    if (!ls)
        return;
    ls->shown = true;
    ls->clock = 0.0;
    ls->last_draw = -1.0;
    ls->lift_asked = false;
    ls->lift = -1.0;
}

void engine_hide_loading_screen(Engine* engine) {
    if (!engine) {
        log_error("engine_hide_loading_screen: NULL engine");
        return;
    }
    if (engine->loading_screen && engine->loading_screen->shown)
        engine->loading_screen->lift_asked = true;
}

bool engine_loading_screen_shown(const Engine* engine) {
    return engine && engine->loading_screen && engine->loading_screen->shown;
}

// Each colour from its display code to light, through the display's own 2.2 (display.glsl).
static void _palette_linear(int which, float out[7][3]) {
    const int p = which >= 0 && which < LOADING_PALETTE_COUNT ? which : LOADING_PALETTE_SUNSET;
    for (int i = 0; i < 7; i++)
        for (int c = 0; c < 3; c++) {
            const uint32_t code = (PALETTES[p][i] >> (16 - 8 * c)) & 0xFFu;
            out[i][c] = powf((float)code / 255.0f, 2.2f);
        }
}

// The clock moves by the time since the last draw, or a frame's fixed step headless, and the
// switch-off starts once it is asked for and the ident has played.
static void _loading_advance(const Engine* engine, LoadingScreen* ls) {
    const double now = glfwGetTime();
    double step = engine->headless ? ENGINE_FIXED_FRAME_DT
                                   : (ls->last_draw < 0.0 ? 0.0 : now - ls->last_draw);
    if (step > LOADING_MAX_STEP)
        step = LOADING_MAX_STEP;
    ls->last_draw = now;
    ls->clock += step;
    if (ls->lift >= 0.0)
        ls->lift += step;
    else if (ls->lift_asked && ls->clock >= LOADING_IDENT_SECONDS + LOADING_HOLD_SECONDS)
        ls->lift = 0.0;
    ls->frame = (ls->frame + 1) & 0xFFFFFF;
}

// The mark, then the tape over it, then the set: into the window, at its size.
static void _loading_draw(const Engine* engine, LoadingScreen* ls) {
    const int w = engine->fb_width, h = engine->fb_height;
    if (w <= 0 || h <= 0)
        return;
    const int pw = w > 1 ? w / 2 : 1, ph = h > 1 ? h / 2 : 1;
    if (!gl_color_target_ensure(&ls->mark, pw, ph, "loading mark") ||
        !gl_color_target_ensure(&ls->picture, pw, ph, "loading picture"))
        return;
    const float resolution[2] = {(float)pw, (float)ph};
    const float off = ls->lift < 0.0 ? 0.0f : (float)(ls->lift / LOADING_OFF_SECONDS);
    float palette[7][3];
    _palette_linear(engine->loading_palette, palette);

    const GLPassState pass = gl_pass_begin();
    glViewport(0, 0, pw, ph);

    glBindFramebuffer(GL_FRAMEBUFFER, ls->mark.fbo);
    glUseProgram(ls->logo->id);
    UniformManager* m = ls->logo->uniforms;
    uniform_set_float(m, "time", (float)ls->clock);
    uniform_set_vec2(m, "resolution", resolution);
    uniform_set_vec3_array(m, "palette", &palette[0][0], 7);
    draw_fullscreen_quad(ls->quad_vao);

    glBindFramebuffer(GL_FRAMEBUFFER, ls->picture.fbo);
    glUseProgram(ls->tape->id);
    UniformManager* t = ls->tape->uniforms;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ls->mark.tex);
    uniform_set_int(t, "markTex", 0);
    uniform_set_float(t, "time", (float)ls->clock);
    uniform_set_float(t, "off", off);
    uniform_set_int(t, "frame", ls->frame);
    uniform_set_vec2(t, "resolution", resolution);
    draw_fullscreen_quad(ls->quad_vao);

    glUseProgram(0);
    gl_pass_end(&pass);
    crt_present(ls->crt, ls->picture.tex, w, h, ls->quad_vao, &LOADING_TUBE);

    if (off >= 1.0f)
        ls->shown = false;
}

void loading_screen_frame(Engine* engine) {
    LoadingScreen* ls = engine ? engine->loading_screen : NULL;
    if (!ls || !ls->shown)
        return;
    _loading_advance(engine, ls);
    _loading_draw(engine, ls);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void engine_draw_loading_screen(Engine* engine) {
    LoadingScreen* ls = engine ? engine->loading_screen : NULL;
    if (!ls || !ls->shown || engine->headless || !engine->window)
        return;
    if (ls->last_draw >= 0.0 && glfwGetTime() - ls->last_draw < LOADING_MIN_INTERVAL)
        return;
    glfwPollEvents();

    // Whatever was bound goes back as it was: this may be called from anywhere on the main thread.
    GLint program = 0, vao = 0, unit = 0, texture = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);

    _loading_advance(engine, ls);
    _loading_draw(engine, ls);

    // A draw here never waits for the display: the time is the loading's.
    glfwSwapInterval(0);
    glfwSwapBuffers(engine->window);
    glfwSwapInterval(engine->vsync ? 1 : 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
    glActiveTexture((GLenum)unit);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)program);
}
