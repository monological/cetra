#include <math.h>

#include "basement.h"
#include "house_key.h"
#include "mats.h"

// The key: KEY_LEN from its bow to the end of its bit, its shank KEY_SHANK about its middle, its
// bow a ring of KEY_WIRE KEY_BOW across, and its bit KEY_BIT thick either side of its middle.
#define KEY_LEN   0.1f
#define KEY_SHANK 0.004f
#define KEY_BOW   0.03f
#define KEY_WIRE  0.0035f
#define KEY_BIT   0.0025f
// Where it lies on the basement's workbench, in the bench's frame (BASEMENT_BENCH): in the clear
// stretch between the vise and the jar, turned off square as if it had been put down in a hurry.
#define KEY_BENCH_A    0.85f
#define KEY_BENCH_D    0.3f
#define KEY_BENCH_TURN 1.2f

// A point of the key, `u` along it from its bow, `v` up across it toward its bit, `w` through its
// thickness.
static void key_point(float u, float v, float w, vec3 out) {
    glm_vec3_copy((vec3){w, v, u - 0.5f * KEY_LEN}, out);
}

// A piece of the bit, u0..u1 along the key and v0..v1 across it, its thickness KEY_BIT either
// side.
static void key_plate(Kit* kit, float u0, float u1, float v0, float v1) {
    vec3 lo = GLM_VEC3_ZERO_INIT, hi = GLM_VEC3_ZERO_INIT;
    key_point(u0, v0, -KEY_BIT, lo);
    key_point(u1, v1, KEY_BIT, hi);
    kit_frame_box(kit, &KIT_WORLD, MAT_BRASS, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], false);
}

void house_key_model(Kit* kit) {
    // The bow, a ring of the wire, the shank coming out of its far side.
    const float rc = 0.5f * KEY_BOW - KEY_WIRE;
    vec3 ring[13];
    for (int i = 0; i < 13; i++) {
        const float t = (float)i * GLM_PIf / 6.0f;
        key_point(0.5f * KEY_BOW + rc * cosf(t), rc * sinf(t), 0.0f, ring[i]);
    }
    kit_frame_pipe(kit, &KIT_WORLD, MAT_BRASS, ring, KIT_COUNT(ring), KEY_WIRE, 6);
    // The shank, round, to the end of the bit, with a collar where it leaves the bow.
    const float from = KEY_BOW - KEY_WIRE, len = KEY_LEN - from, collar = 1.5f * KEY_SHANK;
    const vec2 shank[] = {{0.0f, 0.0f},     {KEY_SHANK, 0.0f}, {KEY_SHANK, 0.004f},
                          {collar, 0.005f}, {collar, 0.009f},  {KEY_SHANK, 0.01f},
                          {KEY_SHANK, len}, {0.0f, len}};
    vec3 base = GLM_VEC3_ZERO_INIT;
    key_point(from, 0.0f, 0.0f, base);
    kit_frame_lathe_on(kit, &KIT_WORLD, MAT_BRASS, base, (vec3){0.0f, 0.0f, 1.0f}, shank,
                       KIT_COUNT(shank), 10);
    // The bit across the shank's end, a ward cut into its middle.
    key_plate(kit, KEY_LEN - 0.018f, KEY_LEN - 0.012f, 0.0f, 0.016f);
    key_plate(kit, KEY_LEN - 0.012f, KEY_LEN - 0.008f, 0.0f, 0.009f);
    key_plate(kit, KEY_LEN - 0.008f, KEY_LEN, 0.0f, 0.016f);
}

void house_key_build(HouseKey* key, Engine* engine, Scene* scene) {
    *key = (HouseKey){0};
    kit_frame_point(&BASEMENT_BENCH, KEY_BENCH_A, BASEMENT_BENCH_TOP, KEY_BENCH_D, key->at);
    Kit kit;
    mats_kit(&kit, engine, scene);
    house_key_model(&kit);
    key->node = kit_finish(&kit, "front_door_key");
    // No capture keeps it: it is taken while the game runs.
    key->node->capture_hidden = true;
    // Laid on its side, its shank on the bench's top.
    glm_translate_make(key->node->original_transform,
                       (vec3){key->at[0], key->at[1] + KEY_SHANK, key->at[2]});
    glm_rotate_y(key->node->original_transform, BASEMENT_BENCH.yaw + KEY_BENCH_TURN,
                 key->node->original_transform);
    glm_rotate_z(key->node->original_transform, -0.5f * GLM_PIf, key->node->original_transform);
}

void house_key_free(HouseKey* key) {
    free_node(key->node);
    *key = (HouseKey){0};
}
