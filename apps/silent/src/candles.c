#include <math.h>
#include <stdio.h>

#include "cetra/light.h"

#include "candles.h"
#include "mats.h"

/*
 * Each holder is a lathe, one turned profile from its foot up to the cup of its socket, with
 * what is not round about it added: a chamberstick's ring, a sconce's backplate and arm. A
 * profile is {radius, height} in metres, listed up the outside, so a pan's inside is the profile
 * coming back in and down; it ends where it closes on its axis at the floor of the cup, which is
 * where the candle stands.
 */

// The candles: a taper, and a floor stand's pillar, which burns a thicker wick and a larger flame.
#define TAPER_R      0.011f
#define PILLAR_R     0.035f
#define PILLAR_FLAME 1.35f
// The wick stands this far out of the wax, and its flame's foot is this far above the wax, so
// the wick's black is inside the flame's dim base, as a burning candle's is.
#define WICK_OUT   0.009f
#define WICK_R     0.0012f
#define FLAME_FOOT 0.002f

/*
 * How the candles burn against a real one, which is taste, as the rain's constants are. The
 * night's exposure is pinned for the tubes and the street lamps, under which a real candle --
 * about a candela -- lights a wall to middle grey only a hand's breadth from it, so the light
 * each casts is scaled up; the flame as drawn is left as it is, being past white already. And
 * they are white-balanced less than a fire's default, so they read as a candle's orange.
 */
#define CANDLE_LIGHT_SCALE 20.0f
#define CANDLE_ADAPTATION  0.55f
// Where a candle's light is windowed to nothing: a budget on the cluster pool rather than a
// photometric cut, since what it drops there scales with CANDLE_LIGHT_SCALE.
#define CANDLE_RANGE 2.5f

// A table candlestick's height to its socket's lip.
#define STICK_H 0.24f
// How far a sconce's pan stands out from the wall, and up from its backplate's middle.
#define SCONCE_OUT  0.17f
#define SCONCE_LIFT 0.06f

// A holder's turned brass standing on y at (a, d). Returns where its candle stands.
static float turned(Kit* kit, const KitFrame* f, float a, float y, float d, const vec2* profile,
                    int count, int sides) {
    kit_frame_lathe(kit, f, MAT_BRASS_BRIGHT, a, d, y, profile, count, sides);
    return y + profile[count - 1][1];
}

/*
 * A candle standing on y: its top melted into a shallow pool, the wick out of the pool, and the
 * wick's tip -- the foot of its flame -- left on the kit.
 */
static void candle(Kit* kit, const KitFrame* f, float a, float y, float d, float wax, bool pillar) {
    const float r = pillar ? PILLAR_R : TAPER_R, size = pillar ? PILLAR_FLAME : 1.0f;
    const float pool = wax - 0.15f * r;
    const vec2 profile[] = {
        {0.0f, 0.0f}, {r, 0.0f}, {r, wax}, {0.8f * r, wax - 0.12f * r}, {0.0f, pool}};
    kit_frame_lathe(kit, f, MAT_WAX, a, d, y, profile, KIT_COUNT(profile), pillar ? 16 : 10);
    kit_frame_prism(kit, f, MAT_BLACK, a, d, y + pool, y + wax + WICK_OUT, WICK_R * size, 4);
    kit_wick(kit, f, a, y + wax + FLAME_FOOT, d, size);
}

// A turned stem on a round foot, a knop under the socket's drip ring.
void candle_stick(Kit* kit, const KitFrame* f, float a, float y, float d, float wax) {
    const float h = STICK_H;
    const vec2 profile[] = {{0.0f, 0.0f},        {0.055f, 0.0f},       {0.055f, 0.01f},
                            {0.035f, 0.025f},    {0.012f, 0.04f},      {0.01f, 0.08f},
                            {0.02f, 0.095f},     {0.01f, 0.11f},       {0.009f, h - 0.03f},
                            {0.03f, h - 0.02f},  {0.032f, h - 0.004f}, {0.026f, h},
                            {0.014f, h - 0.01f}, {0.0f, h - 0.01f}};
    candle(kit, f, a, turned(kit, f, a, y, d, profile, KIT_COUNT(profile), 14), d, wax, false);
}

void candle_chamber(Kit* kit, const KitFrame* f, float a, float y, float d, float wax) {
    const float rim = 0.074f;
    const vec2 profile[] = {{0.0f, 0.0f},     {0.07f, 0.0f},    {0.072f, 0.004f}, {rim, 0.016f},
                            {0.068f, 0.018f}, {0.062f, 0.008f}, {0.02f, 0.006f},  {0.014f, 0.012f},
                            {0.011f, 0.035f}, {0.018f, 0.042f}, {0.02f, 0.06f},   {0.022f, 0.066f},
                            {0.016f, 0.068f}, {0.013f, 0.058f}, {0.0f, 0.058f}};
    const float cup = turned(kit, f, a, y, d, profile, KIT_COUNT(profile), 16);
    // The ring stands on the rim's edge.
    enum { RING = 17 };
    vec3 ring[RING] = {{0.0f}};
    const float r = 0.013f, ca = a + rim + 0.011f, cy = y + 0.014f;
    for (int i = 0; i < RING; i++) {
        const float t = 2.0f * GLM_PIf * (float)i / (float)(RING - 1);
        glm_vec3_copy((vec3){ca + r * cosf(t), cy + r * sinf(t), d}, ring[i]);
    }
    kit_frame_pipe(kit, f, MAT_BRASS_BRIGHT, ring, RING, 0.003f, 6);
    candle(kit, f, a, cup, d, wax, false);
}

// A stepped foot, a stem with a knop at its middle and one under the pan, a wide drip pan and a
// socket for a pillar.
void candle_stand(Kit* kit, const KitFrame* f, float a, float y, float d, float wax) {
    const vec2 profile[] = {
        {0.0f, 0.0f},     {0.16f, 0.0f},   {0.16f, 0.025f},  {0.14f, 0.035f},  {0.13f, 0.05f},
        {0.09f, 0.07f},   {0.05f, 0.1f},   {0.04f, 0.13f},   {0.055f, 0.15f},  {0.04f, 0.17f},
        {0.022f, 0.2f},   {0.018f, 0.6f},  {0.034f, 0.63f},  {0.04f, 0.66f},   {0.034f, 0.69f},
        {0.018f, 0.72f},  {0.016f, 1.18f}, {0.03f, 1.2f},    {0.022f, 1.22f},  {0.1f, 1.24f},
        {0.105f, 1.255f}, {0.098f, 1.26f}, {0.09f, 1.25f},   {0.05f, 1.25f},   {0.046f, 1.26f},
        {0.044f, 1.3f},   {0.048f, 1.31f}, {0.042f, 1.315f}, {0.037f, 1.305f}, {0.0f, 1.305f}};
    const float cup = turned(kit, f, a, y, d, profile, KIT_COUNT(profile), 18);
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, a - 0.16f, a + 0.16f, y, cup, d - 0.16f, d + 0.16f,
                  true);
    candle(kit, f, a, cup, d, wax, true);
}

// A lancet backplate with a boss on it, and an arm from the boss down, out and up to a small pan
// and its socket.
void candle_sconce(Kit* kit, const Facade* wall, float a, float y, float wax) {
    const KitFrame* f = &wall->f;
    const float face = wall->face, o = wall->out;
    const KitOpening plate = {a - 0.055f, a + 0.055f,       y - 0.12f,
                              y + 0.08f,  KIT_ARCH_POINTED, 0.09f};
    vec2 outline[KIT_OPENING_POINTS];
    const int n = kit_opening_outline(&plate, outline);
    kit_frame_extrude(kit, f, MAT_BRASS_BRIGHT, outline, n, face, face + o * 0.012f);
    const vec2 boss[] = {
        {0.0f, 0.0f}, {0.022f, 0.0f}, {0.02f, 0.008f}, {0.012f, 0.014f}, {0.0f, 0.016f}};
    kit_frame_lathe_on(kit, f, MAT_BRASS_BRIGHT, (vec3){a, y - 0.05f, face + o * 0.012f},
                       (vec3){0.0f, 0.0f, o}, boss, KIT_COUNT(boss), 10);

    // The arm, a cubic from the boss out and round to straight up under the pan.
    enum { ARM = 12 };
    const float pan_y = y + SCONCE_LIFT, out = face + o * SCONCE_OUT;
    vec3 arm[ARM] = {{0.0f}};
    for (int i = 0; i < ARM; i++) {
        const float t = (float)i / (float)(ARM - 1);
        glm_vec3_copy((vec3){a, glm_bezier(t, y - 0.05f, y - 0.09f, pan_y - 0.07f, pan_y),
                             glm_bezier(t, face + o * 0.02f, face + o * 0.13f, out, out)},
                      arm[i]);
    }
    kit_frame_pipe(kit, f, MAT_BRASS_BRIGHT, arm, ARM, 0.007f, 8);

    const vec2 pan[] = {{0.0f, 0.0f},     {0.012f, 0.0f},   {0.05f, 0.012f},  {0.054f, 0.02f},
                        {0.048f, 0.022f}, {0.044f, 0.014f}, {0.016f, 0.014f}, {0.013f, 0.02f},
                        {0.011f, 0.04f},  {0.016f, 0.048f}, {0.017f, 0.054f}, {0.013f, 0.055f},
                        {0.012f, 0.046f}, {0.0f, 0.046f}};
    candle(kit, f, a, turned(kit, f, a, pan_y, out, pan, KIT_COUNT(pan), 14), out, wax, false);
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, a - 0.07f, a + 0.07f, y - 0.13f, y + 0.32f, face,
                  face + o * 0.23f, true);
}

void candles_light(FireSystem* fs, Scene* scene, const Kit* kit, bool shadows) {
    if (!fs)
        return;
    for (int i = 0; i < kit->wick_count; i++) {
        const KitWick* w = &kit->wicks[i];
        char name[32];
        snprintf(name, sizeof(name), "candle_%d", i);
        Fire* fire = fire_system_add(fs, FIRE_FLAME, name);
        if (!fire)
            return;
        glm_vec3_copy((float*)w->tip, fire->flame.wick);
        fire->flame.width *= w->size;
        fire->flame.height *= w->size;
        // The street's breeze does not come into the house.
        fire->params.wind_response = 0.0f;
        fire->params.adaptation = CANDLE_ADAPTATION;
        fire->light_scale = CANDLE_LIGHT_SCALE;
        // A candle stands still, so its shadow is drawn once and kept (spec 13.16), softened
        // by the flame's own body, which the fire keeps up to date as it burns.
        const LightDesc desc = {.name = name,
                                .type = LIGHT_POINT,
                                .position = {w->tip[0], w->tip[1], w->tip[2]},
                                .range = CANDLE_RANGE,
                                .cast_shadows = shadows,
                                .shadow_cache = shadows,
                                .source_radius = 0.5f * fire->flame.width,
                                .source_length =
                                    fmaxf(fire->flame.height - fire->flame.width, 0.0f)};
        Light* light = create_light(&desc);
        if (!light)
            continue;
        scene_add_light(scene, light);
        fire->light = light;
    }
}
