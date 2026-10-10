#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "gait.h"

// The phase is wound back by this many steps, an even number so the feet keep their turns, once
// past it: a float counting steps for an hour would be counting in thousandths.
#define PHASE_WRAP 1024.0f

// How far into a step, from the last heel down, a foot still on its way down is set down when
// the body stops: earlier than this, the heel that just struck is the one it stops on.
#define SETTLE_FROM 0.2f

// The speed a standing body must pass to be walking, as a share past `stop_speed`, so a speed
// hovering at the threshold does not stop and start.
#define START_MARGIN 1.5f

Gait* create_gait(void) {
    Gait* g = calloc(1, sizeof(Gait));
    if (!g) {
        log_error("create_gait: out of memory");
        return NULL;
    }
    // 120 steps a minute at a walk of 1.6 m/s and 163 at a jog of 3.4: the cadences people walk
    // and jog at.
    g->step_base = 0.40f;
    g->step_per_speed = 0.25f;
    // Blondel's 2R + G, about a level pace: what a flight is built to.
    g->flight_step = 0.63f;
    // A quarter is a steep street; a half, ground nobody would build to walk.
    g->stairs_from = 0.25f;
    g->stairs_to = 0.5f;
    g->grade_reach = 0.5f; // two treads
    // Up a flight at a walk of 1.6 m/s a person climbs about 0.35 m/s, 1.8 treads of 19 cm a
    // second, and comes down a little faster; as shares of the speed asked, a jog climbs faster.
    g->climb_up = 0.22f;
    g->climb_down = 0.28f;
    g->pace_floor = 0.6f;
    g->speed_time = 0.15f;
    g->stop_speed = 0.15f;
    // Most of a kerb taken up in a fifth of a second, and the eye a hair behind in a fall.
    g->eye_rate = 20.0f;
    g->eye_air_rate = 60.0f;
    g->lag_below = 0.25f;
    g->lag_above = 0.10f;
    // A drop off a kerb is a step down, not a landing; a fall of a third of a metre is.
    g->land_air = 0.25f;
    g->dip_per_speed = 0.03f;
    g->dip_max = 0.08f;
    g->dip_rate = 12.0f;
    g->shuffle_turn = glm_rad(70.0f);
    g->shuffle_gap = 0.35f;
    gait_reset(g);
    return g;
}

void free_gait(Gait* g) {
    free(g);
}

void gait_reset(Gait* g) {
    if (!g) {
        log_error("gait_reset: NULL gait");
        return;
    }
    memset(g, 0, offsetof(Gait, step_base));
    g->pace = 1.0f;
}

void gait_step(Gait* g, const vec3 move, bool grounded, float speed_asked, float dt) {
    if (!g) {
        log_error("gait_step: NULL gait");
        return;
    }
    if (!grounded) {
        g->air += dt;
        if (dt > 0.0f)
            g->fall = fmaxf(-move[1] / dt, 0.0f);
        return;
    }
    if (g->air >= g->land_air && g->fall > 0.0f)
        g->landing = g->fall;
    g->air = 0.0f;
    g->fall = 0.0f;

    // Rise over run, each sum weighted down by the ground walked since, so the grade is of the
    // last `grade_reach` of ground whatever the speed: a walker slowed by it does not change it.
    const float run = hypotf(move[0], move[2]);
    if (run > 0.0f) {
        const float keep = expf(-run / g->grade_reach);
        g->rise = g->rise * keep + move[1];
        g->run = g->run * keep + run;
        g->grade = g->run > 1e-4f ? g->rise / g->run : 0.0f;
    }
    const float steep = fabsf(g->grade);
    g->stairs = glm_smoothstep(g->stairs_from, g->stairs_to, steep);
    float pace = 1.0f;
    if (steep > 1e-3f)
        pace = fminf(1.0f, (g->grade > 0.0f ? g->climb_up : g->climb_down) / steep);
    if (speed_asked > 1e-4f)
        pace = fmaxf(pace, fminf(1.0f, g->pace_floor / speed_asked));
    g->pace = pace;
}

// A critically damped spring's exact step toward 0 at rate w (rad/s), from x moving at v.
static void spring(float* x, float* v, float w, float dt) {
    const float e = expf(-w * dt);
    const float c = *v + w * *x;
    *x = (*x + c * dt) * e;
    *v = (*v - w * c * dt) * e;
}

void gait_frame(Gait* g, const vec3 move, bool grounded, float yaw, float dt) {
    if (!g) {
        log_error("gait_frame: NULL gait");
        return;
    }
    g->fell = false;
    if (dt <= 0.0f)
        return;
    g->since += dt;
    const float run = hypotf(move[0], move[2]);
    g->speed += (run / dt - g->speed) * (1.0f - expf(-dt / g->speed_time));

    // The first foot to come down this frame is the one heard: a frame long enough for two is a
    // hitch, and two footsteps at once would say so.
    GaitFootfall fall = {GAIT_FALL_STEP, 0, 0.0f};
    bool falls = false;
    if (g->landing > 0.0f) {
        fall = (GaitFootfall){GAIT_FALL_LAND, 0, g->landing};
        falls = true;
        // Pushed down at a speed that bottoms out at `depth` a 1/rate later.
        const float depth = fminf(g->dip_per_speed * g->landing, g->dip_max);
        g->dip_v -= depth * g->dip_rate * GLM_Ef;
        g->phase = roundf(g->phase); // on both feet
        g->landing = 0.0f;
    }
    if (grounded) {
        const float w = g->stairs;
        const float step = g->step_base + g->step_per_speed * g->speed;
        const float before = floorf(g->phase - 0.5f);
        g->phase += (1.0f - w) * run / step + w * (2.0f * fabsf(move[1]) + run) / g->flight_step;
        const float after = floorf(g->phase - 0.5f);
        if (after > before && !falls) {
            fall = (GaitFootfall){GAIT_FALL_STEP, (int)after & 1, g->speed};
            falls = true;
        }
    }

    const bool moving = g->speed > g->stop_speed * (g->moving ? 1.0f : START_MARGIN);
    if (g->moving && !moving && grounded) {
        const float through = g->phase - 0.5f - floorf(g->phase - 0.5f);
        if (through > SETTLE_FROM) {
            g->phase += 1.0f - through;
            if (!falls) {
                fall = (GaitFootfall){GAIT_FALL_SETTLE, (int)floorf(g->phase - 0.5f) & 1, g->speed};
                falls = true;
            }
        }
    }
    g->moving = moving;
    if (g->phase > PHASE_WRAP)
        g->phase -= PHASE_WRAP;

    // Turning on the spot moves the feet too.
    if (g->yawed && grounded && !moving) {
        g->turned += fabsf(remainderf(yaw - g->yaw, 2.0f * GLM_PIf));
        if (g->turned >= g->shuffle_turn && g->since >= g->shuffle_gap && !falls) {
            fall = (GaitFootfall){GAIT_FALL_SHUFFLE, g->shuffles++ & 1, 0.0f};
            falls = true;
            g->turned = 0.0f;
        }
    } else {
        g->turned = 0.0f;
    }
    g->yaw = yaw;
    g->yawed = true;

    if (falls) {
        g->footfall = fall;
        g->fell = true;
        g->since = 0.0f;
    }

    // The body's rise is the eye's lag until the spring takes it up.
    g->lag = glm_clamp(g->lag - move[1], -g->lag_below, g->lag_above);
    spring(&g->lag, &g->lag_v, grounded ? g->eye_rate : g->eye_air_rate, dt);
    spring(&g->dip, &g->dip_v, g->dip_rate, dt);
    glm_vec3_copy((vec3){0.0f, g->lag + g->dip, 0.0f}, g->smooth_offset);
}
