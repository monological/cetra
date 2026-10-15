#include <math.h>
#include <stdint.h>
#include <string.h>

#include "cetra/mesh.h"
#include "cetra/mesh_builder.h"
#include "cetra/program.h"
#include "cetra/util.h"

#include "arms_clips.h"
#include "fixtures.h"
#include "layout.h"
#include "lights.h"
#include "mats.h"

/*
 * Cool white with the green cast old fluorescent tubes get, and roughly a
 * bare tube's brightness. The strip is the tube's projection: 3.5 cm across,
 * the length of the tube.
 */
static const float TUBE_COLOUR[3] = {0.80f, 1.0f, 0.84f};
// Metres a tube reaches: the room's two across the kitchen and down the hall, and the hood's
// only round the stove under it. A light index is spent on every cluster a tube's reach
// touches, and three tubes at nine metres overflowed the cluster pool in the irradiance
// probes' opening sweep, whose views look out from inside the kitchen.
#define TUBE_RANGE      9.0f
#define TUBE_RANGE_HOOD 4.5f
// Metres from a strip's centre that nothing nearer casts its shadow: past the steel channel
// (tube_fixture), about 7 cm at its farthest corner. The channel is a box square to the world,
// so a tilted strip's emitting plane cuts through it, and inside the near plane a cached face
// draws the steel in front of the strip and shadows the ceiling round it.
#define TUBE_SHADOW_NEAR 0.1f

#define FLASHLIGHT_CANDELA 900.0f

/*
 * The hall's pendant (spec 13.52): a lantern of brass and clear glass on a chain from a rose on
 * the ceiling, a 25 W filament bulb in it, near the kitchen door, so the clock opposite the door
 * reads from the doorway and the hall stays dim. Warm, the way a filament is beside a fluorescent
 * tube. Short of the cased opening halfway down the hall, on a short chain, high enough to light
 * the clock's face from over it.
 */
#define PENDANT_X     (-0.75f)
#define PENDANT_Z     13.15f
#define PENDANT_R     0.09f
#define PENDANT_H     0.3f
#define PENDANT_ROSE  0.025f // under the ceiling, where the chain hangs from and the swing turns
#define PENDANT_CHAIN 0.3f   // from the rose to the crown's ring
#define PENDANT_TOP   (HOME_CEIL_Y - PENDANT_ROSE - PENDANT_CHAIN - FIXTURE_CROWN_H) // the cage's
static const vec3 PENDANT_PIVOT = {PENDANT_X, HOME_CEIL_Y - PENDANT_ROSE, PENDANT_Z};
#define BULB_CANDELA 30.0f
// Its swing goes round an ellipse SWING_ACROSS as wide across as along, and the bulb's kept shadow
// follows it while it carries the bulb further than SWING_FOLLOW.
#define SWING_ACROSS 0.3f
#define SWING_FOLLOW 0.015f

typedef struct Tube {
    const char* name; // the node's name, which the derived light takes
    vec3 centre;
    vec3 axis;   // along the tube
    vec3 facing; // the side the strip emits from
    float length, width;
    float nits;
    float range;
    bool flicker;
} Tube;

// Over the room, over the window, and under the hood.
static const Tube TUBES[] = {
    {"tube_ceiling",
     {2.3f, HOME_CEIL_Y - 0.075f, 12.0f},
     {1.0f, 0.0f, 0.0f},
     {0.0f, -1.0f, 0.0f},
     1.2f,
     0.035f,
     6000.0f,
     TUBE_RANGE,
     true},
    {"tube_window",
     {2.6f, KITCHEN_WIN_HEAD + 0.16f, KITCHEN_Z0 + 0.075f},
     {1.0f, 0.0f, 0.0f},
     {0.0f, -0.5f, 0.866f},
     1.2f,
     0.035f,
     6000.0f,
     TUBE_RANGE,
     false},
    {"tube_hood",
     // On the canopy's front lip, tipped toward the room: the underside is at
     // eye height, so a strip lying flat under it is never seen from the room.
     {KITCHEN_X0 + 0.46f, FLOOR_Y + 1.605f, STOVE_Z},
     {0.0f, 0.0f, 1.0f},
     {0.6f, -0.8f, 0.0f},
     0.55f,
     0.03f,
     3500.0f,
     TUBE_RANGE_HOOD,
     false},
};
#define TUBE_COUNT ((int)(sizeof(TUBES) / sizeof(TUBES[0])))
_Static_assert(TUBE_COUNT <= LIGHTS_MAX_TUBES, "every tube needs a buzz slot");

/*
 * Each tube's ballast hums, louder for a brighter tube, and the failing one's
 * hum drops out and catches with its light. One recording serves every tube,
 * so each starts BUZZ_STAGGER after the last, or three copies of one loop
 * would play in step and sound like one source smeared across the room.
 */
#define BUZZ_VOLUME  0.25f // for a tube of BUZZ_NITS
#define BUZZ_NITS    6000.0f
#define BUZZ_STAGGER 1.9 // seconds

// The strip itself: one quad, alone in its mesh so the engine can fit a
// panel to it, facing where the tube throws its light. Returns its material.
static Material* tube_strip(Scene* scene, ShaderProgram* pbr, const Tube* t) {
    vec3 across, a, b, c, d, half_len, half_w;
    glm_vec3_cross((float*)t->facing, (float*)t->axis, across);
    glm_vec3_normalize(across);
    glm_vec3_scale((float*)t->axis, 0.5f * t->length, half_len);
    glm_vec3_scale(across, 0.5f * t->width, half_w);
    glm_vec3_sub((float*)t->centre, half_len, a);
    glm_vec3_sub(a, half_w, a);
    glm_vec3_add((float*)t->centre, half_len, b);
    glm_vec3_sub(b, half_w, b);
    glm_vec3_add((float*)t->centre, half_len, c);
    glm_vec3_add(c, half_w, c);
    glm_vec3_sub((float*)t->centre, half_len, d);
    glm_vec3_add(d, half_w, d);

    vec3 n;
    glm_vec3_normalize_to((float*)t->facing, n);
    MeshBuilder mb;
    mb_init(&mb, 4, 6, false);
    const unsigned int ia = mb_vertex(&mb, a, n, (float*)t->axis, 0, 0, 0, 0, NULL);
    const unsigned int ib = mb_vertex(&mb, b, n, (float*)t->axis, 1, 0, 1, 0, NULL);
    const unsigned int ic = mb_vertex(&mb, c, n, (float*)t->axis, 1, 1, 1, 1, NULL);
    const unsigned int id = mb_vertex(&mb, d, n, (float*)t->axis, 0, 1, 0, 1, NULL);
    // Faces `facing` by construction: a->b runs along the axis and b->c along
    // facing x axis, so the winding's normal is facing's part across the axis.
    mb_tri(&mb, ia, ib, ic);
    mb_tri(&mb, ia, ic, id);

    Material* m = create_material();
    m->name = safe_strdup(t->name);
    glm_vec3_copy((vec3){0.9f, 0.95f, 0.9f}, m->albedo);
    glm_vec3_copy((float*)TUBE_COLOUR, m->emissive);
    m->emissive_strength = t->nits;
    m->roughness = 0.3f;
    material_set_program(m, pbr);

    Mesh* mesh = create_mesh();
    if (!mb_transfer(&mb, mesh)) {
        free_mesh(mesh);
        free_material(m);
        return NULL;
    }
    mesh->material = m;
    SceneNode* node = create_node();
    node_set_name(node, t->name);
    node_add_mesh(node, mesh);
    node_add_child(scene->root_node, node);
    return m;
}

// What the tube hangs from: a steel channel behind it, and end caps.
static void tube_fixture(Kit* kit, const Tube* t) {
    vec3 back, centre;
    glm_vec3_normalize_to((float*)t->facing, back);
    glm_vec3_scale(back, -0.03f, back);
    glm_vec3_add((float*)t->centre, back, centre);
    const bool along_x = fabsf(t->axis[0]) > 0.5f;
    const float hl = 0.5f * t->length + 0.04f;
    const vec3 half = {along_x ? hl : 0.035f, 0.025f, along_x ? 0.035f : hl};
    kit_box(kit, MAT_STEEL, centre, half, 0.0f, false);
}

// A chain of oval brass links from `y0` down to `y1` on the upright through the pendant's pivot,
// in its own frame, each link turned a quarter from the last and hung through it.
#define LINK_LEN  0.034f // outside, end to end
#define LINK_WIDE 0.016f
#define LINK_WIRE 0.0022f
static void chain(Kit* kit, float y0, float y1) {
    const float hy = 0.5f * LINK_LEN - LINK_WIRE, hx = 0.5f * LINK_WIDE - LINK_WIRE;
    const float pitch = LINK_LEN - 4.0f * LINK_WIRE;
    const int links = (int)ceilf((y0 - y1 - LINK_LEN) / pitch) + 1;
    for (int i = 0; i < links; i++) {
        const float yc = y0 - 0.5f * LINK_LEN - (float)i * pitch;
        vec3 loop[13];
        for (int k = 0; k < 13; k++) {
            const float t = (float)k * GLM_PIf / 6.0f, across = hx * cosf(t);
            glm_vec3_copy(
                (vec3){(i & 1) ? 0.0f : across, yc + hy * sinf(t), (i & 1) ? across : 0.0f},
                loop[k]);
        }
        kit_frame_pipe(kit, &KIT_WORLD, MAT_LANTERN_BRASS, loop, 13, LINK_WIRE, 5);
    }
}

// The pendant: a brass rose on the ceiling, and under it, in a kit of its own that turns about
// the rose, the chain and the lantern. That kit casts nothing: swinging, it would be a mover in
// its own light's kept views every frame. Its light is the bulb's, cached (spec 13.16) from the
// filament, softened by the filament's size; the clock's pendulum and hands swing their shadows
// with them.
static void hall_pendant(Lights* lights, Kit* kit, Scene* scene) {
    const vec2 rose[] = {{0.0f, -PENDANT_ROSE},
                         {0.012f, -PENDANT_ROSE},
                         {0.05f, -0.012f},
                         {0.06f, -0.002f},
                         {0.06f, 0.0f}};
    kit_frame_lathe(kit, &KIT_WORLD, MAT_LANTERN_BRASS, PENDANT_X, PENDANT_Z, HOME_CEIL_Y, rose,
                    KIT_COUNT(rose), 16);
    Kit hang;
    kit_init_beside(&hang, kit, GLM_VEC3_ZERO);
    hang.casts_nothing = true;
    const float top = PENDANT_TOP - PENDANT_PIVOT[1];
    chain(&hang, 0.0f, top + FIXTURE_CROWN_H);
    fixture_lantern(&hang, &KIT_WORLD, 0.0f, top, 0.0f, PENDANT_R, PENDANT_H);
    lights->pendant = kit_finish(&hang, "hall_pendant");
    kit_keep_beside(kit, &hang);
    // A capture is kept for good, and a swinging lantern is somewhere else a moment later.
    lights->pendant->capture_hidden = true;
    glm_translate_make(lights->pendant->original_transform, (float*)PENDANT_PIVOT);
    lights->pendant_bulb = top - FIXTURE_BULB_DROP;
    LightDesc desc = {.name = "hall_bulb",
                      .type = LIGHT_POINT,
                      .position = {PENDANT_X, PENDANT_PIVOT[1] + lights->pendant_bulb, PENDANT_Z},
                      .color = {1.0f, 0.72f, 0.42f},
                      .intensity = BULB_CANDELA,
                      .range = 7.0f,
                      .cast_shadows = true,
                      .shadow_cache = true,
                      .source_radius = FIXTURE_BULB_BODY,
                      .shadow_near = FIXTURE_BULB_NEAR};
    lights->hall = create_light(&desc);
    scene_add_light(scene, lights->hall);
    glm_vec3_copy(desc.position, lights->hall_at);
}

// The pendant's light at `at`, its kept shadow following it at any move or not.
static void pendant_light(Lights* lights, const vec3 at, bool follow) {
    if (!lights->hall)
        return;
    light_set_position(lights->hall, (float*)at);
    lights->hall->shadow_follow = follow;
}

// The pendant swung for `time`, its swing eased `dt` toward what it is asked: a pendulum from the
// rose to the bulb, round an ellipse.
static void pendant_update(Lights* lights, double time, float dt) {
    if (!lights->pendant)
        return;
    const float ease = fmaxf(lights->swing_ease, 1e-3f);
    lights->swing += (lights->swing_want - lights->swing) * (1.0f - expf(-dt / ease));
    const float drop = -lights->pendant_bulb, w = sqrtf(9.81f / drop), t = (float)time;
    mat4 m = GLM_MAT4_IDENTITY_INIT;
    glm_translate_make(m, (float*)PENDANT_PIVOT);
    glm_rotate_z(m, lights->swing * sinf(w * t), m);
    glm_rotate_x(m, SWING_ACROSS * lights->swing * sinf(w * t + 1.1f), m);
    glm_mat4_copy(m, lights->pendant->original_transform);
    glm_mat4_mulv3(m, (vec3){0.0f, lights->pendant_bulb, 0.0f}, 1.0f, lights->hall_at);
    lights->hall_follows = lights->swing * drop > SWING_FOLLOW;
    pendant_light(lights, lights->hall_at, lights->hall_follows);
}

void lights_build(Lights* lights, Kit* kit, Engine* engine, Scene* scene, unsigned int seed,
                  bool flicker, bool flashlight_on) {
    memset(lights, 0, sizeof(*lights));
    lights->flicker_tube = -1;
    lights->level = 1.0f;
    lights->mains = 1.0f;
    lights->seed = seed;
    ShaderProgram* pbr = engine_get_program(engine, CETRA_PROGRAM_PBR);
    // The panels come from the strips: this is what turns them into light.
    engine->emissive_lights_enabled = true;

    // The fog scatters the scene's first spot light into a visible beam, and
    // this is the app's only one.
    LightDesc desc = {.name = "flashlight",
                      .type = LIGHT_SPOT,
                      .color = {1.0f, 0.93f, 0.80f},
                      .intensity = FLASHLIGHT_CANDELA,
                      .range = 18.0f,
                      .inner_cutoff = glm_rad(9.0f),
                      .outer_cutoff = glm_rad(21.0f),
                      .cast_shadows = true};
    lights->flashlight = create_light(&desc);
    scene_add_light(scene, lights->flashlight);
    lights->flashlight_on = flashlight_on;
    glm_vec3_copy((vec3){0.0f, 0.0f, -1.0f}, lights->flashlight_dir);

    hall_pendant(lights, kit, scene);
    for (int i = 0; i < TUBE_COUNT; i++) {
        tube_fixture(kit, &TUBES[i]);
        Material* m = tube_strip(scene, pbr, &TUBES[i]);
        lights->strip[i] = m;
        if (flicker && TUBES[i].flicker) {
            lights->flicker = m;
            lights->flicker_nits = TUBES[i].nits;
            lights->flicker_tube = i;
        }
    }
}

/*
 * A failing starter: steady for seconds at a time, then a burst of stutter --
 * dropping out, catching, half-lighting -- and steady again. A pure function
 * of the sim clock, so a headless run shows the same frame every time.
 */
static float flicker_level(double t, unsigned int seed) {
    const double period = 6.0;
    const uint32_t cycle = (uint32_t)floor(t / period);
    const float in = (float)(t - (double)cycle * period);
    const float start = 1.0f + 3.5f * kit_hash01(cycle, 0u, seed);
    const float len = 0.3f + 0.9f * kit_hash01(cycle, 1u, seed);
    if (in < start || in > start + len)
        return 1.0f;
    const float h = kit_hash01(cycle, 100u + (uint32_t)((in - start) / 0.045f), seed);
    return h < 0.45f ? 0.04f : (h < 0.6f ? 0.5f : 1.0f);
}

float lights_brownout(double t, unsigned int seed) {
    const double period = 11.0;
    const uint32_t cycle = (uint32_t)floor(t / period);
    if (kit_hash01(cycle, 2u, seed) < 0.35f)
        return 1.0f;
    const float in = (float)(t - (double)cycle * period);
    const float start = 1.0f + 7.0f * kit_hash01(cycle, 3u, seed);
    const float len = 0.35f + 1.1f * kit_hash01(cycle, 4u, seed);
    const float depth = 0.35f + 0.35f * kit_hash01(cycle, 5u, seed);
    if (in < start || in > start + len)
        return 1.0f;
    // Down quickly, back slowly, as a filament cools and heats.
    const float u = (in - start) / len, fall = 0.15f;
    const float dip =
        u < fall ? glm_smoothstep(0.0f, fall, u) : 1.0f - glm_smoothstep(fall, 1.0f, u);
    return 1.0f - depth * dip;
}

void lights_start_audio(Lights* lights, AudioSystem* audio) {
    if (!audio)
        return;
    for (int t = 0; t < TUBE_COUNT; t++) {
        Sound* s =
            audio_sound_from_file(audio, "assets/audio/silent/tube_buzz.flac", AUDIO_BUS_SFX);
        if (!s)
            continue;
        audio_sound_set_looping(s, true);
        audio_sound_set_position(s, (float*)TUBES[t].centre);
        lights->buzz[t] = s;
    }
}

// The tubes on `mains` with the failing one at `level`, and the flashlight's beam on or off: what
// an update drives, and what the captures' rest drives in its place.
static void lights_apply(Lights* lights, float mains, float level, bool beam) {
    for (int t = 0; t < TUBE_COUNT; t++)
        if (lights->strip[t])
            lights->strip[t]->emissive_strength =
                TUBES[t].nits * mains * (t == lights->flicker_tube ? level : 1.0f);
    Light* f = lights->flashlight;
    if (f) {
        f->intensity = beam ? FLASHLIGHT_CANDELA : 0.0f;
        f->cast_shadows = beam;
    }
}

void lights_update(Lights* lights, Scene* scene, double time, float dt, const vec3 eye,
                   const vec3 forward, float reach) {
    const float level = lights->flicker ? flicker_level(time, lights->seed) : 1.0f;
    lights->level = level;

    for (int t = 0; t < TUBE_COUNT; t++) {
        Sound* s = lights->buzz[t];
        if (!s)
            continue;
        if (!lights->buzzing[t] && time >= BUZZ_STAGGER * (double)t) {
            audio_sound_play(s);
            lights->buzzing[t] = true;
        }
        const float lit = lights->mains * (t == lights->flicker_tube ? level : 1.0f);
        audio_sound_set_volume(s, BUZZ_VOLUME * (TUBES[t].nits / BUZZ_NITS) * lit);
    }

    // The derived panels are the engine's, created on the first frame and kept
    // after, and named for their strips' nodes; their reach and their casting
    // are ours to state. Left derived, a 6000-nit panel claims every cluster in
    // the frustum. Looked up each frame rather than held from the first, so a
    // panel the engine re-derives is never one this holds dangling. Every tube
    // casts, and keeps its shadow, since none of them moves: without one, the
    // hood's tube lights the hall's floor through the wall.
    for (int t = 0; t < TUBE_COUNT; t++) {
        Light* l = scene_find_light(scene, TUBES[t].name);
        if (!l || l->type != LIGHT_AREA)
            continue;
        l->range = TUBES[t].range;
        l->cast_shadows = true;
        l->shadow_cache = true;
        l->shadow_near = TUBE_SHADOW_NEAR;
    }

    Light* f = lights->flashlight;
    if (f) {
        // The beam trails the head a little, as the body turns after it, and the distance it
        // meets the look at eases after the look's own, so it does not jump past an edge.
        const float k = 1.0f - expf(-dt * 14.0f);
        glm_vec3_lerp(lights->flashlight_dir, (float*)forward, k, lights->flashlight_dir);
        glm_vec3_normalize(lights->flashlight_dir);
        lights->flashlight_reach =
            lights->flashlight_reach > 0.0f ? glm_lerp(lights->flashlight_reach, reach, k) : reach;
        // Clipped to the jacket's chest: under the eye by the body's heading, which the head's
        // pitch does not move.
        vec3 ahead = {forward[0], 0.0f, forward[2]}, left, up = {0.0f, 1.0f, 0.0f}, pos, dir;
        if (glm_vec3_norm2(ahead) < 1e-8f)
            glm_vec3_copy(lights->flashlight_dir, ahead);
        ahead[1] = 0.0f;
        glm_vec3_normalize(ahead);
        glm_vec3_cross(up, ahead, left);
        glm_vec3_copy((float*)eye, pos);
        glm_vec3_muladds(left, ARMS_LAMP[0], pos);
        pos[1] += ARMS_LAMP[1];
        glm_vec3_muladds(ahead, ARMS_LAMP[2], pos);
        // Aimed at what the look meets: along the look from the chest, it would pass under it up
        // close and light a wall a hand's breadth below where the player looks.
        vec3 meets;
        glm_vec3_copy((float*)eye, meets);
        glm_vec3_muladds(lights->flashlight_dir, lights->flashlight_reach, meets);
        glm_vec3_sub(meets, pos, dir);
        glm_vec3_normalize(dir);
        glm_vec3_lerp(pos, lights->hold_at, lights->hold, pos);
        glm_vec3_lerp(dir, lights->hold_dir, lights->hold, dir);
        glm_vec3_normalize(dir);
        light_set_position(f, pos);
        light_set_direction(f, dir);
    }
    lights_apply(lights, lights->mains, level, lights_flashlight_lit(lights));
    pendant_update(lights, time, dt);
}

void lights_toggle_flashlight(Lights* lights) {
    lights->flashlight_on = !lights->flashlight_on;
}

void lights_flashlight_hold(Lights* lights, const vec3 at, const vec3 dir, float weight) {
    glm_vec3_copy((float*)at, lights->hold_at);
    glm_vec3_normalize_to((float*)dir, lights->hold_dir);
    lights->hold = glm_clamp(weight, 0.0f, 1.0f);
}

bool lights_flashlight_lit(const Lights* lights) {
    return lights->flashlight_on || lights->hold > 0.0f;
}

void lights_swing(Lights* lights, float radians, float seconds) {
    lights->swing_want = radians;
    lights->swing_ease = seconds;
}

void lights_set_mains(Lights* lights, float mains) {
    lights->mains = mains;
    lights_apply(lights, mains, lights->level, lights_flashlight_lit(lights));
}

void lights_rest(Lights* lights, bool rest) {
    if (rest) {
        lights_apply(lights, 1.0f, 1.0f, false);
        pendant_light(lights, (vec3){PENDANT_X, PENDANT_PIVOT[1] + lights->pendant_bulb, PENDANT_Z},
                      false);
    } else {
        lights_apply(lights, lights->mains, lights->level, lights_flashlight_lit(lights));
        pendant_light(lights, lights->hall_at, lights->hall_follows);
    }
}
