#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "crt.h"
#include "engine_internal.h"
#include "ext/log.h"
#include "loading_screen.h"
#include "profiler.h"
#include "program.h"
#include "shader_params.h"
#include "uniform.h"
#include "util.h"

// The ident's timeline, shared with the shaders that play it. The switch-off waits for
// LOADING_IDENT_END, past its last glitch, and a beat of the picture held still after it.
#include "../shaders/include/loading_constants.glsl"
#define LOADING_HOLD_SECONDS 0.4
// The switch-off, in seconds (loading_tape_frag.glsl's `off` over it).
#define LOADING_OFF_SECONDS 0.45
// The most the clock moves in one draw: a long stall pauses the ident rather than skipping it.
#define LOADING_MAX_STEP 0.1
// The fewest seconds between two draws between a frame's pieces or outside one.
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
    ShaderProgram* blur;
    ShaderProgram* tape;
    Crt* crt;
    GLColorTarget mark;     // the mark, linear, with mips to the bloom's size
    GLColorTarget bloom[2]; // the mark blurred across, then down, at a quarter its size
    GLColorTarget picture;  // the tape's, display-encoded
    GLuint quad_vao, quad_vbo;
    bool shown; // from show until the switch-off has finished or a draw could not be made
    // What the frame's own draw last found: the switch-off not begun. Read for the frame after, so
    // the frame's two questions of it agree whatever moves the clock between them.
    bool covering;
    double clock;        // seconds of the screen shown
    double last_draw;    // the wall clock at the last draw; < 0 before the first
    double play_at;      // the clock at which PLAY shows; INFINITY until the game is ready
    double lift_at;      // the clock at which the switch-off starts; INFINITY until it is hidden
    uint64_t frame;      // draws so far, for the tape's noise
    double last_chance;  // the wall clock at its last chance to draw, or at show
    double longest_wait; // the longest between two chances since last asked, seconds
};

static LoadingScreen* _loading_screen_make(void) {
    LoadingScreen* ls = calloc(1, sizeof(LoadingScreen));
    if (!ls) {
        log_error("Loading screen: out of memory");
        return NULL;
    }
    ls->logo = create_loading_logo_program();
    ls->blur = create_loading_blur_program();
    ls->tape = create_loading_tape_program();
    ls->crt = create_crt();
    if (!ls->logo || !ls->blur || !ls->tape || !ls->crt) {
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
    free_program(ls->blur);
    free_program(ls->tape);
    free_crt(ls->crt);
    gl_color_target_free(&ls->mark);
    gl_color_target_free(&ls->bloom[0]);
    gl_color_target_free(&ls->bloom[1]);
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
    ls->covering = true;
    ls->clock = 0.0;
    ls->last_draw = -1.0;
    ls->play_at = INFINITY;
    ls->lift_at = INFINITY;
    ls->last_chance = glfwGetTime();
    ls->longest_wait = 0.0;
}

void engine_loading_screen_ready(Engine* engine) {
    if (!engine) {
        log_error("engine_loading_screen_ready: NULL engine");
        return;
    }
    LoadingScreen* ls = engine->loading_screen;
    if (ls && ls->shown && isinf(ls->play_at))
        ls->play_at = fmax(ls->clock, LOADING_IDENT_END);
}

void engine_hide_loading_screen(Engine* engine) {
    if (!engine) {
        log_error("engine_hide_loading_screen: NULL engine");
        return;
    }
    LoadingScreen* ls = engine->loading_screen;
    if (ls && ls->shown && isinf(ls->lift_at))
        ls->lift_at = fmax(ls->clock, LOADING_IDENT_END + LOADING_HOLD_SECONDS);
}

bool engine_loading_screen_shown(const Engine* engine) {
    return engine && engine->loading_screen && engine->loading_screen->shown;
}

bool engine_loading_screen_prompting(const Engine* engine) {
    if (!engine_loading_screen_shown(engine))
        return false;
    const LoadingScreen* ls = engine->loading_screen;
    return ls->clock >= ls->play_at && isinf(ls->lift_at);
}

bool loading_screen_covers(const Engine* engine) {
    return engine_loading_screen_shown(engine) && engine->loading_screen->covering;
}

double engine_loading_screen_longest_wait(Engine* engine) {
    if (!engine_loading_screen_shown(engine))
        return 0.0;
    LoadingScreen* ls = engine->loading_screen;
    const double longest = ls->longest_wait;
    ls->longest_wait = 0.0;
    return longest;
}

// A chance to draw, drawn or not: the time since the last one is how long the screen stood still.
static void _loading_chance(LoadingScreen* ls) {
    const double now = glfwGetTime();
    ls->longest_wait = fmax(ls->longest_wait, now - ls->last_chance);
    ls->last_chance = now;
}

// The clock moves by the time since the last draw, or a frame's fixed step headless. False once
// the switch-off has finished, when there is nothing left to draw.
static bool _loading_advance(const Engine* engine, LoadingScreen* ls) {
    const double now = glfwGetTime();
    double step = ENGINE_FIXED_FRAME_DT;
    if (!engine->headless)
        step = ls->last_draw < 0.0 ? 0.0 : now - ls->last_draw;
    ls->last_draw = now;
    ls->clock += fmin(step, LOADING_MAX_STEP);
    ls->frame++;
    if (ls->clock >= ls->lift_at + LOADING_OFF_SECONDS)
        ls->shown = false;
    return ls->shown;
}

// One direction of the bloom's blur: `src` at mip `lod` into `dst`, a texel of `dst` a step.
static void _loading_blur(const LoadingScreen* ls, GLuint src, float lod, const GLColorTarget* dst,
                          bool across) {
    glBindFramebuffer(GL_FRAMEBUFFER, dst->fbo);
    glViewport(0, 0, dst->w, dst->h);
    glBindTexture(GL_TEXTURE_2D, src);
    UniformManager* b = ls->blur->uniforms;
    const float step[2] = {across ? 1.0f / (float)dst->w : 0.0f,
                           across ? 0.0f : 1.0f / (float)dst->h};
    uniform_set_int(b, "srcTex", 0);
    uniform_set_float(b, "srcLod", lod);
    uniform_set_vec2(b, "stepUv", step);
    draw_fullscreen_quad(ls->quad_vao);
}

// The targets at half the window, the bloom's at a quarter of that, which is the mark's mip level
// 2 exactly, texel for texel. A screen whose targets cannot be made is put away rather than left
// up over a window it cannot draw.
static bool _loading_targets(LoadingScreen* ls, int w, int h) {
    const int pw = w > 1 ? w / 2 : 1, ph = h > 1 ? h / 2 : 1;
    const int qw = pw >> 2 > 0 ? pw >> 2 : 1, qh = ph >> 2 > 0 ? ph >> 2 : 1;
    const bool remade = ls->mark.w != pw || ls->mark.h != ph;
    if (!gl_color_target_ensure(&ls->mark, pw, ph, "loading mark") ||
        !gl_color_target_ensure(&ls->bloom[0], qw, qh, "loading bloom") ||
        !gl_color_target_ensure(&ls->bloom[1], qw, qh, "loading bloom") ||
        !gl_color_target_ensure(&ls->picture, pw, ph, "loading picture")) {
        log_error("Loading screen: put away, since it cannot be drawn");
        ls->shown = false;
        return false;
    }
    if (remade) {
        glBindTexture(GL_TEXTURE_2D, ls->mark.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);
    }
    return true;
}

// The mark, its bloom, then the tape over both, then the set: into the window, at its size. Each
// stage a row of the profiler when `timed`, which only a frame's own draw is. The caller's
// framebuffer, viewport and fixed-function state are put back.
static void _loading_draw(const Engine* engine, LoadingScreen* ls, bool timed) {
    Profiler* prof = engine->profiler;
    const int w = engine->fb_width, h = engine->fb_height;
    if (w <= 0 || h <= 0)
        return;
    const GLPassState pass = gl_pass_begin();
    glActiveTexture(GL_TEXTURE0);
    if (!_loading_targets(ls, w, h)) {
        gl_pass_end(&pass);
        return;
    }
    const float resolution[2] = {(float)ls->mark.w, (float)ls->mark.h};
    const double since_lift = isinf(ls->lift_at) ? 0.0 : fmax(0.0, ls->clock - ls->lift_at);
    const float off = (float)(since_lift / LOADING_OFF_SECONDS);
    float palette[7][3];
    const int p = engine->loading_palette >= 0 && engine->loading_palette < LOADING_PALETTE_COUNT
                      ? engine->loading_palette
                      : LOADING_PALETTE_SUNSET;
    for (int i = 0; i < 7; i++)
        for (int c = 0; c < 3; c++)
            palette[i][c] = (float)((PALETTES[p][i] >> (16 - 8 * c)) & 0xFFu) / 255.0f;
    glViewport(0, 0, ls->mark.w, ls->mark.h);

    profiler_scope_begin_if(prof, timed, "loading mark");
    glBindFramebuffer(GL_FRAMEBUFFER, ls->mark.fbo);
    glUseProgram(ls->logo->id);
    UniformManager* m = ls->logo->uniforms;
    shader_clock_upload(m, (float)ls->clock, ls->frame);
    uniform_set_vec2(m, "resolution", resolution);
    uniform_set_vec3_array(m, "paletteCodes", &palette[0][0], 7);
    uniform_set_float(m, "playAt", isinf(ls->play_at) ? -1.0f : (float)ls->play_at);
    draw_fullscreen_quad(ls->quad_vao);
    profiler_scope_end(prof);

    // The tube's bloom, of the picture as it stands, a letter in mid-turn included: the mark
    // brought to a quarter its size by its own mips, then blurred across and down.
    profiler_scope_begin_if(prof, timed, "loading bloom");
    glBindTexture(GL_TEXTURE_2D, ls->mark.tex);
    glGenerateMipmap(GL_TEXTURE_2D);
    glUseProgram(ls->blur->id);
    _loading_blur(ls, ls->mark.tex, 2.0f, &ls->bloom[0], true);
    _loading_blur(ls, ls->bloom[0].tex, 0.0f, &ls->bloom[1], false);
    profiler_scope_end(prof);

    profiler_scope_begin_if(prof, timed, "loading tape");
    glBindFramebuffer(GL_FRAMEBUFFER, ls->picture.fbo);
    glViewport(0, 0, ls->picture.w, ls->picture.h);
    glUseProgram(ls->tape->id);
    UniformManager* t = ls->tape->uniforms;
    glBindTexture(GL_TEXTURE_2D, ls->mark.tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ls->bloom[1].tex);
    glActiveTexture(GL_TEXTURE0);
    uniform_set_int(t, "markTex", 0);
    uniform_set_int(t, "bloomTex", 1);
    shader_clock_upload(t, (float)ls->clock, ls->frame);
    uniform_set_float(t, "off", off);
    uniform_set_vec2(t, "resolution", resolution);
    draw_fullscreen_quad(ls->quad_vao);
    profiler_scope_end(prof);

    profiler_scope_begin_if(prof, timed, "loading crt");
    crt_present(ls->crt, ls->picture.tex, w, h, ls->quad_vao, &LOADING_TUBE);
    profiler_scope_end(prof);
    gl_pass_end(&pass);
}

void loading_screen_frame(Engine* engine) {
    LoadingScreen* ls = engine ? engine->loading_screen : NULL;
    if (!ls)
        return;
    if (ls->shown)
        _loading_chance(ls);
    if (ls->shown && _loading_advance(engine, ls)) {
        _loading_draw(engine, ls, true);
        ls->covering = ls->clock < ls->lift_at;
    }
    // Put away whole once it is done, its targets and programs with it: they are a few
    // window-sized pictures nothing else will draw.
    if (!ls->shown) {
        free_loading_screen(ls);
        engine->loading_screen = NULL;
    }
}

// A draw between frames or between a frame's pieces: the screen into the window and swapped,
// without waiting for the display, and whatever the caller had bound put back. `poll` answers the
// window's events too, which only a draw outside a frame may do: inside one, an event could resize
// the targets the frame is drawing into.
static void _loading_present(Engine* engine, bool poll) {
    LoadingScreen* ls = engine ? engine->loading_screen : NULL;
    if (!ls || !ls->shown)
        return;
    _loading_chance(ls);
    if (engine->headless || !engine->window)
        return;
    if (ls->last_draw >= 0.0 && glfwGetTime() - ls->last_draw < LOADING_MIN_INTERVAL)
        return;
    if (poll)
        glfwPollEvents();

    // Between capture units colour may be masked off, and the draw binds units 0 and 1.
    GLint program = 0, vao = 0, unit = 0, texture[2] = {0, 0};
    GLboolean colour_mask[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    glGetBooleanv(GL_COLOR_WRITEMASK, colour_mask);
    for (int u = 0; u < 2; u++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture[u]);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (_loading_advance(engine, ls)) {
        _loading_draw(engine, ls, false);
        glfwSwapInterval(0);
        glfwSwapBuffers(engine->window);
        engine_apply_swap_interval(engine);
    }

    for (int u = 0; u < 2; u++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)u);
        glBindTexture(GL_TEXTURE_2D, (GLuint)texture[u]);
    }
    glActiveTexture((GLenum)unit);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)program);
    glColorMask(colour_mask[0], colour_mask[1], colour_mask[2], colour_mask[3]);
}

void engine_draw_loading_screen(Engine* engine) {
    _loading_present(engine, true);
}

void loading_screen_tick(Engine* engine) {
    _loading_present(engine, false);
}
