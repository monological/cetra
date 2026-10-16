#include <math.h>

#include "cetra/wind.h"

#include "dock_lamp.h"
#include "fixtures.h"
#include "mats.h"

#define POST    0.022f // the post's radius
#define TALL    2.4f   // from the deck to the crook's top
#define REACH   0.6f   // how far out the crook carries the lantern
#define CHAIN   0.16f  // from the crook's hook to the lantern's ring
#define GLOBE_R 0.075f // the globe's radius
#define GLOBE_H 0.24f  // and its height
#define CANDELA 60.0f
#define RANGE   10.0f

// The lantern's swing in the breeze: how far it leans downwind at a gust's peak, how hard the
// gust's turbulence pushes it about (rad/s/s at the peak, a share of that across the wind), and
// its damping. Its own swing is 1.24 s.
#define SWING_LEAN   0.08f
#define SWING_PUSH   0.9f
#define SWING_ACROSS 0.6f
#define SWING_DAMP   0.1f
#define SWING_STEP   (1.0f / 240.0f) // seconds the pendulum is integrated in

void dock_lamp_build(DockLamp* lamp, Kit* kit, Scene* scene, const KitFrame* at) {
    *lamp = (DockLamp){.scene = scene};
    const vec2 flange[] = {{0.0f, 0.0f},    {0.07f, 0.0f},   {0.07f, 0.014f}, {0.045f, 0.026f},
                           {0.034f, 0.07f}, {0.04f, 0.085f}, {0.034f, 0.1f},  {POST, 0.11f}};
    kit_frame_lathe(kit, at, MAT_LANTERN_BRASS, 0.0f, 0.0f, 0.0f, flange, KIT_COUNT(flange), 20);
    // Up the post, then over in a cubic, its end turned down to the hook: (d, y) of its controls.
    const vec2 c[4] = {
        {0.0f, TALL - 0.25f}, {0.0f, TALL + 0.08f}, {REACH, TALL + 0.1f}, {REACH, TALL - 0.12f}};
    enum { CROOK = 16 };
    vec3 path[CROOK + 1];
    glm_vec3_copy((vec3){0.0f, 0.1f, 0.0f}, path[0]);
    for (int i = 0; i < CROOK; i++) {
        const float t = (float)i / (CROOK - 1);
        glm_vec3_copy((vec3){0.0f, glm_bezier(t, c[0][1], c[1][1], c[2][1], c[3][1]),
                             glm_bezier(t, c[0][0], c[1][0], c[2][0], c[3][0])},
                      path[i + 1]);
    }
    kit_frame_pipe(kit, at, MAT_LANTERN_BRASS, path, CROOK + 1, POST, 10);
    // A ball on the crook's end; the chain hangs from under it.
    const float hook = c[3][1];
    const vec2 knob[] = {
        {0.0f, -0.032f}, {0.022f, -0.026f}, {0.03f, -0.01f}, {0.022f, 0.006f}, {POST, 0.01f}};
    kit_frame_lathe(kit, at, MAT_LANTERN_BRASS, 0.0f, REACH, hook, knob, KIT_COUNT(knob), 14);
    vec3 middle = {0.0f, 0.0f, 0.0f};
    kit_frame_point(at, 0.0f, 0.5f * TALL, 0.0f, middle);
    kit_collider(kit, middle, (vec3){0.05f, 0.5f * TALL, 0.05f}, 0.0f);

    // The chain and the lantern, round the pivot under the ball.
    kit_frame_point(at, 0.0f, hook - 0.03f, REACH, lamp->pivot);
    const float top = -CHAIN - FIXTURE_CROWN_H;
    Kit hang;
    kit_init_beside(&hang, kit, GLM_VEC3_ZERO);
    hang.casts_nothing = true;
    hang.draw_distance = kit->draw_distance;
    fixtures_chain(&hang, 0.0f, -CHAIN);
    fixture_lantern_round(&hang, &KIT_WORLD, 0.0f, top, 0.0f, GLOBE_R, GLOBE_H, MAT_LAMP_GLASS,
                          MAT_LAMP_FILAMENT);
    lamp->lantern = kit_finish(&hang, "dock_lantern");
    kit_keep_beside(kit, &hang);
    if (lamp->lantern) {
        lamp->lantern->capture_hidden = true;
        glm_translate_make(lamp->lantern->original_transform, lamp->pivot);
    }
    lamp->drop = FIXTURE_BULB_DROP - top;
    LightDesc desc = {.name = "dock_lamp",
                      .type = LIGHT_POINT,
                      .position = {lamp->pivot[0], lamp->pivot[1] - lamp->drop, lamp->pivot[2]},
                      .color = {1.0f, 0.70f, 0.40f},
                      .intensity = CANDELA,
                      .range = RANGE,
                      .cast_shadows = true,
                      .shadow_cache = true,
                      .shadow_near = FIXTURE_BULB_NEAR};
    lamp->light = create_light(&desc);
    scene_add_light(scene, lamp->light);
}

// The breeze's way in plan, down the valley when there is no wind to say.
static void downwind(const Wind* wind, vec3 along) {
    glm_vec3_copy((vec3){0.0f, 0.0f, 1.0f}, along);
    if (wind && fabsf(wind->direction[0]) + fabsf(wind->direction[2]) > 1e-4f)
        glm_vec3_normalize_to((vec3){wind->direction[0], 0.0f, wind->direction[2]}, along);
}

// Swung `dt` on by the breeze at `time`: a pendulum from the hook to the bulb, leaning downwind as
// far as the gust blows and pushed about by its turbulence, along the wind and across it.
static void swing(DockLamp* lamp, double time, float dt) {
    const Wind* wind = lamp->scene ? lamp->scene->wind : NULL;
    if (!wind || dt <= 0.0f)
        return;
    const float w = sqrtf(9.81f / lamp->drop);
    const int steps = (int)ceilf(dt / SWING_STEP);
    const float h = dt / (float)steps;
    for (int i = 0; i < steps; i++) {
        const float t = (float)time - dt + h * (float)(i + 1);
        const float gust = wind_gust(wind, t);
        for (int k = 0; k < 2; k++) {
            const float p = 1.7f * (float)k;
            const float turb = sinf(1.3f * t + p) + 0.7f * sinf(2.9f * t + 2.0f * p) +
                               0.5f * sinf(4.7f * t + 3.0f * p) + 0.35f * sinf(6.1f * t + 5.0f * p);
            const float lean = k ? 0.0f : SWING_LEAN * gust;
            const float push = (k ? SWING_ACROSS : 1.0f) * SWING_PUSH * gust * turb;
            const float accel = -w * w * (lamp->swing[k] - lean) -
                                2.0f * SWING_DAMP * w * lamp->swing_rate[k] + push;
            lamp->swing_rate[k] += accel * h;
            lamp->swing[k] += lamp->swing_rate[k] * h;
        }
    }
}

// Where its swing has it, or hanging straight down at rest, and its light at the bulb, the kept
// shadow following while the bulb is further than FIXTURE_SWING_FOLLOW from straight down.
static void place(DockLamp* lamp, bool rest) {
    if (!lamp->lantern || !lamp->light)
        return;
    const float lean = rest ? 0.0f : lamp->swing[0], sway = rest ? 0.0f : lamp->swing[1];
    // Leaning downwind turns it about the level line across the wind; swaying across, about the
    // wind's own line.
    vec3 along = {0.0f, 0.0f, 0.0f}, across = {0.0f, 0.0f, 0.0f};
    downwind(lamp->scene ? lamp->scene->wind : NULL, along);
    glm_vec3_cross(along, GLM_YUP, across);
    mat4 turn = GLM_MAT4_IDENTITY_INIT;
    glm_rotate(turn, lean, across);
    glm_rotate(turn, sway, along);
    vec3 bulb = {0.0f, 0.0f, 0.0f};
    fixture_hang(lamp->lantern, lamp->pivot, turn, lamp->drop, bulb);
    light_set_position(lamp->light, bulb);
    lamp->light->shadow_follow =
        lamp->drop * sqrtf(lean * lean + sway * sway) > FIXTURE_SWING_FOLLOW;
}

void dock_lamp_update(DockLamp* lamp, double time, float dt) {
    swing(lamp, time, dt);
    place(lamp, false);
}

void dock_lamp_rest(DockLamp* lamp, bool rest) {
    place(lamp, rest);
}
