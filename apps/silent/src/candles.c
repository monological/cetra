#include <stdio.h>

#include "cetra/fire.h"
#include "cetra/light.h"

#include "candles.h"
#include "mats.h"

/*
 * Each holder is a lathe, one turned profile from its foot up to the cup of its socket, with
 * what is not round about it added: a chamberstick's ring, a sconce's backplate and arm. A
 * profile is {radius, height} in metres, listed up the outside, so a pan's inside is the profile
 * coming back in and down. A candle stands in its socket's cup, below the lip.
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

// Where a candle's light is windowed to nothing. A taper is about a candela, so this is a sixth
// of a lux: past it a candle lights nothing the night does not, and a wider reach only spends
// the cluster pool.
#define CANDLE_RANGE 2.5f

// A table candlestick's height to its socket's lip.
#define STICK_H 0.24f
// How far a sconce's pan stands out from the wall, and up from its backplate's middle.
#define SCONCE_OUT  0.17f
#define SCONCE_LIFT 0.06f

/*
 * The candle standing on y, burnt down to `wax`: its top melted into a shallow pool, the wick
 * out of the pool, and the wick's tip -- the foot of a flame `size` times a taper's -- left on
 * the kit.
 */
static void candle(Kit* kit, const KitFrame* f, float a, float y, float d, float r, float wax,
                   float size) {
    const vec2 profile[] = {
        {0.0f, 0.0f}, {r, 0.0f}, {r, wax}, {0.8f * r, wax - 0.12f * r}, {0.0f, wax - 0.15f * r}};
    kit_frame_lathe(kit, f, MAT_WAX, a, d, y, profile, KIT_COUNT(profile), r > 0.02f ? 16 : 10);
    const float top = y + wax;
    kit_frame_prism(kit, f, MAT_BLACK, a, d, top - 0.15f * r, top + WICK_OUT, WICK_R * size, 4);
    kit_wick(kit, f, a, top + FLAME_FOOT, d, size);
}

// A turned stem on a round foot, a knop under the socket's drip ring. Returns where the candle
// stands.
static float stick(Kit* kit, const KitFrame* f, float a, float y, float d) {
    const float h = STICK_H;
    const vec2 profile[] = {{0.0f, 0.0f},        {0.055f, 0.0f},       {0.055f, 0.01f},
                            {0.035f, 0.025f},    {0.012f, 0.04f},      {0.01f, 0.08f},
                            {0.02f, 0.095f},     {0.01f, 0.11f},       {0.009f, h - 0.03f},
                            {0.03f, h - 0.02f},  {0.032f, h - 0.004f}, {0.026f, h},
                            {0.014f, h - 0.01f}, {0.0f, h - 0.01f}};
    kit_frame_lathe(kit, f, MAT_BRASS_BRIGHT, a, d, y, profile, KIT_COUNT(profile), 14);
    return y + h - 0.01f;
}

// A wide pan with a raised rim, a short column to the socket, and a ring for a finger at +a.
static float chamber(Kit* kit, const KitFrame* f, float a, float y, float d) {
    const vec2 profile[] = {{0.0f, 0.0f},     {0.07f, 0.0f},    {0.072f, 0.004f}, {0.074f, 0.016f},
                            {0.068f, 0.018f}, {0.062f, 0.008f}, {0.02f, 0.006f},  {0.014f, 0.012f},
                            {0.011f, 0.035f}, {0.018f, 0.042f}, {0.02f, 0.06f},   {0.022f, 0.066f},
                            {0.016f, 0.068f}, {0.013f, 0.058f}, {0.0f, 0.058f}};
    kit_frame_lathe(kit, f, MAT_BRASS_BRIGHT, a, d, y, profile, KIT_COUNT(profile), 16);
    enum { RING = 17 };
    vec3 ring[RING] = {{0.0f}};
    const float r = 0.013f, ca = a + 0.074f + 0.011f, cy = y + 0.014f;
    for (int i = 0; i < RING; i++) {
        const float t = 2.0f * GLM_PIf * (float)i / (float)(RING - 1);
        glm_vec3_copy((vec3){ca + r * cosf(t), cy + r * sinf(t), d}, ring[i]);
    }
    kit_frame_pipe(kit, f, MAT_BRASS_BRIGHT, ring, RING, 0.003f, 6);
    return y + 0.058f;
}

/*
 * A floor stand a little under a metre and a third: a stepped foot, a stem with a knop at its
 * middle and one under the pan, a wide drip pan and a socket for a pillar. It collides.
 */
static float stand(Kit* kit, const KitFrame* f, float a, float y, float d) {
    const vec2 profile[] = {
        {0.0f, 0.0f},     {0.16f, 0.0f},   {0.16f, 0.025f},  {0.14f, 0.035f},  {0.13f, 0.05f},
        {0.09f, 0.07f},   {0.05f, 0.1f},   {0.04f, 0.13f},   {0.055f, 0.15f},  {0.04f, 0.17f},
        {0.022f, 0.2f},   {0.018f, 0.6f},  {0.034f, 0.63f},  {0.04f, 0.66f},   {0.034f, 0.69f},
        {0.018f, 0.72f},  {0.016f, 1.18f}, {0.03f, 1.2f},    {0.022f, 1.22f},  {0.1f, 1.24f},
        {0.105f, 1.255f}, {0.098f, 1.26f}, {0.09f, 1.25f},   {0.05f, 1.25f},   {0.046f, 1.26f},
        {0.044f, 1.3f},   {0.048f, 1.31f}, {0.042f, 1.315f}, {0.037f, 1.305f}, {0.0f, 1.305f}};
    kit_frame_lathe(kit, f, MAT_BRASS_BRIGHT, a, d, y, profile, KIT_COUNT(profile), 18);
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, a - 0.16f, a + 0.16f, y, y + 1.32f, d - 0.16f,
                  d + 0.16f, true);
    return y + 1.305f;
}

/*
 * A sconce: a lancet-shaped backplate on the wall, centred at y, a boss on it, and an arm
 * curving down, out and up to a small pan with its socket. Returns where the candle stands; it
 * stands SCONCE_OUT from the wall.
 */
static float sconce(Kit* kit, const KitFrame* f, float a, float y, float d) {
    const KitOpening plate = {a - 0.055f, a + 0.055f,       y - 0.12f,
                              y + 0.08f,  KIT_ARCH_POINTED, 0.09f};
    vec2 outline[KIT_OPENING_POINTS];
    const int n = kit_opening_outline(&plate, outline);
    kit_frame_extrude(kit, f, MAT_BRASS_BRIGHT, outline, n, d, d + 0.012f);
    const vec2 boss[] = {
        {0.0f, 0.0f}, {0.022f, 0.0f}, {0.02f, 0.008f}, {0.012f, 0.014f}, {0.0f, 0.016f}};
    kit_frame_lathe_on(kit, f, MAT_BRASS_BRIGHT, (vec3){a, y - 0.05f, d + 0.012f},
                       (vec3){0.0f, 0.0f, 1.0f}, boss, KIT_COUNT(boss), 10);

    // The arm, a cubic from the boss out and round to straight up under the pan.
    enum { ARM = 12 };
    const float pan_y = y + SCONCE_LIFT, out = d + SCONCE_OUT;
    const vec2 c[4] = {
        {y - 0.05f, d + 0.02f}, {y - 0.09f, d + 0.13f}, {pan_y - 0.07f, out}, {pan_y, out}};
    vec3 arm[ARM] = {{0.0f}};
    for (int i = 0; i < ARM; i++) {
        const float t = (float)i / (float)(ARM - 1), s = 1.0f - t;
        const float w[4] = {s * s * s, 3.0f * s * s * t, 3.0f * s * t * t, t * t * t};
        float ay = 0.0f, ad = 0.0f;
        for (int k = 0; k < 4; k++) {
            ay += w[k] * c[k][0];
            ad += w[k] * c[k][1];
        }
        glm_vec3_copy((vec3){a, ay, ad}, arm[i]);
    }
    kit_frame_pipe(kit, f, MAT_BRASS_BRIGHT, arm, ARM, 0.007f, 8);

    const vec2 pan[] = {{0.0f, 0.0f},     {0.012f, 0.0f},   {0.05f, 0.012f},  {0.054f, 0.02f},
                        {0.048f, 0.022f}, {0.044f, 0.014f}, {0.016f, 0.014f}, {0.013f, 0.02f},
                        {0.011f, 0.04f},  {0.016f, 0.048f}, {0.017f, 0.054f}, {0.013f, 0.055f},
                        {0.012f, 0.046f}, {0.0f, 0.046f}};
    kit_frame_lathe(kit, f, MAT_BRASS_BRIGHT, a, out, pan_y, pan, KIT_COUNT(pan), 14);
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, a - 0.07f, a + 0.07f, y - 0.13f, y + 0.32f, d,
                  d + 0.23f, true);
    return pan_y + 0.046f;
}

void candle_build(Kit* kit, const KitFrame* f, CandleHolder kind, float a, float y, float d,
                  float wax) {
    switch (kind) {
        case CANDLE_STICK:
            candle(kit, f, a, stick(kit, f, a, y, d), d, TAPER_R, wax, 1.0f);
            break;
        case CANDLE_CHAMBER:
            candle(kit, f, a, chamber(kit, f, a, y, d), d, TAPER_R, wax, 1.0f);
            break;
        case CANDLE_STAND:
            candle(kit, f, a, stand(kit, f, a, y, d), d, PILLAR_R, wax, PILLAR_FLAME);
            break;
        case CANDLE_SCONCE:
            candle(kit, f, a, sconce(kit, f, a, y, d), d + SCONCE_OUT, TAPER_R, wax, 1.0f);
            break;
    }
}

void candles_light(Scene* scene, const Kit* kit) {
    if (kit->wick_count == 0)
        return;
    FireSystem* fs = create_fire_system();
    if (!fs)
        return;
    scene->fire = fs;
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
        const LightDesc desc = {.name = name,
                                .type = LIGHT_POINT,
                                .position = {w->tip[0], w->tip[1], w->tip[2]},
                                .range = CANDLE_RANGE};
        Light* light = create_light(&desc);
        if (!light)
            continue;
        scene_add_light(scene, light);
        fire->light = light;
    }
}
