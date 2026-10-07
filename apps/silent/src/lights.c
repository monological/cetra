#include <math.h>
#include <stdint.h>
#include <string.h>

#include "cetra/mesh.h"
#include "cetra/mesh_builder.h"
#include "cetra/program.h"
#include "cetra/util.h"

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
// (tube_fixture), whose box the strip sits inside, so that would otherwise shadow the ceiling
// round every tube from within.
#define TUBE_SHADOW_NEAR 0.1f

#define FLASHLIGHT_CANDELA 900.0f

/*
 * The hall's lantern: a 25 W bulb behind frosted glass, near the kitchen door,
 * so the clock opposite the door reads from the doorway and the hall stays
 * dim. Warm, the way a filament is beside a fluorescent tube.
 */
// Opposite the clock, short of the cased opening halfway down the hall.
static const vec3 BULB_AT = {-0.75f, CEIL_Y - 0.4f, 13.15f};
#define BULB_CANDELA 30.0f

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
     {2.3f, CEIL_Y - 0.075f, 12.0f},
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

// The pendant lantern (spec 13.25, after the hall in P.T.): a chain from a ceiling rose to an
// iron cage of four frosted panes under a pyramid cap, a bulb's point light inside. Its shadow
// is cached (spec 13.16), with a near plane past the glass round the bulb -- which would
// otherwise shadow everything -- and softened by the bulb's size. The clock's pendulum and
// hands swing their shadows with them.
#define LANTERN_R         0.09f // half the cage's width
#define LANTERN_H         0.3f
#define BULB_SHADOW_NEAR  (LANTERN_R + 0.03f) // past the panes; the cage's corner posts still cast
#define BULB_GLASS_RADIUS 0.03f
static void hall_bulb(Kit* kit, Scene* scene) {
    const float x = BULB_AT[0], y = BULB_AT[1], z = BULB_AT[2];
    const float top = y + 0.5f * LANTERN_H, bottom = y - 0.5f * LANTERN_H;
    const vec2 rose[] = {{0.0f, 0.0f}, {0.06f, 0.0f}, {0.05f, -0.02f}, {0.0f, -0.025f}};
    kit_frame_lathe(kit, &KIT_WORLD, MAT_IRON, x, z, CEIL_Y, rose,
                    (int)(sizeof(rose) / sizeof(rose[0])), 12);
    kit_frame_pipe(kit, &KIT_WORLD, MAT_IRON, (vec3[]){{x, CEIL_Y, z}, {x, top + 0.09f, z}}, 2,
                   0.006f, 6);
    const vec2 cap[] = {{0.0f, 0.0f}, {LANTERN_R + 0.03f, 0.0f}, {0.02f, 0.08f}, {0.0f, 0.09f}};
    kit_frame_lathe(kit, &KIT_WORLD, MAT_IRON, x, z, top, cap, (int)(sizeof(cap) / sizeof(cap[0])),
                    4);
    for (int i = 0; i < 4; i++) {
        const float dx = (i & 1) ? LANTERN_R : -LANTERN_R, dz = (i & 2) ? LANTERN_R : -LANTERN_R;
        kit_box(kit, MAT_IRON, (vec3){x + dx, y, z + dz}, (vec3){0.008f, 0.5f * LANTERN_H, 0.008f},
                0.0f, false);
    }
    kit_box(kit, MAT_IRON, (vec3){x, bottom - 0.01f, z},
            (vec3){LANTERN_R + 0.015f, 0.012f, LANTERN_R + 0.015f}, 0.0f, false);
    kit_box(kit, MAT_FROSTED, (vec3){x, y, z},
            (vec3){LANTERN_R - 0.005f, 0.5f * LANTERN_H - 0.01f, LANTERN_R - 0.005f}, 0.0f, false);
    LightDesc desc = {.name = "hall_bulb",
                      .type = LIGHT_POINT,
                      .position = {x, y, z},
                      .color = {1.0f, 0.72f, 0.42f},
                      .intensity = BULB_CANDELA,
                      .range = 7.0f,
                      .cast_shadows = true,
                      .shadow_cache = true,
                      .source_radius = BULB_GLASS_RADIUS,
                      .shadow_near = BULB_SHADOW_NEAR};
    scene_add_light(scene, create_light(&desc));
}

void lights_build(Lights* lights, Kit* kit, Engine* engine, Scene* scene, unsigned int seed,
                  bool flicker, bool flashlight_on) {
    memset(lights, 0, sizeof(*lights));
    lights->flicker_tube = -1;
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

    hall_bulb(kit, scene);
    for (int i = 0; i < TUBE_COUNT; i++) {
        tube_fixture(kit, &TUBES[i]);
        Material* m = tube_strip(scene, pbr, &TUBES[i]);
        if (flicker && TUBES[i].flicker) {
            lights->flicker = m;
            lights->flicker_nits = TUBES[i].nits;
            lights->flicker_tube = i;
        }
    }
}

static float hash01(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x8da6b343u ^ b * 0xd8163841u ^ c * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return (float)(h >> 8) * (1.0f / 16777216.0f);
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
    const float start = 1.0f + 3.5f * hash01(cycle, 0u, seed);
    const float len = 0.3f + 0.9f * hash01(cycle, 1u, seed);
    if (in < start || in > start + len)
        return 1.0f;
    const float h = hash01(cycle, 100u + (uint32_t)((in - start) / 0.045f), seed);
    return h < 0.45f ? 0.04f : (h < 0.6f ? 0.5f : 1.0f);
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

void lights_update(Lights* lights, Scene* scene, double time, float dt, const vec3 eye,
                   const vec3 forward, float hearing) {
    const float level = lights->flicker ? flicker_level(time, lights->seed) : 1.0f;
    if (lights->flicker)
        lights->flicker->emissive_strength = lights->flicker_nits * level;

    for (int t = 0; t < TUBE_COUNT; t++) {
        Sound* s = lights->buzz[t];
        if (!s)
            continue;
        if (!lights->buzzing[t] && time >= BUZZ_STAGGER * (double)t) {
            audio_sound_play(s);
            lights->buzzing[t] = true;
        }
        const float lit = t == lights->flicker_tube ? level : 1.0f;
        audio_sound_set_volume(s, BUZZ_VOLUME * (TUBES[t].nits / BUZZ_NITS) * lit * hearing);
    }

    // The derived panels are the engine's, created on the first frame and kept
    // after, and named for their strips' nodes; their reach and their casting
    // are ours to state. Left derived, a 6000-nit panel claims every cluster in
    // the frustum. Looked up each frame rather than held from the first, so a
    // panel the engine re-derives is never one this holds dangling. Every tube
    // casts, and keeps its shadow, since none of them moves: the hood's lights
    // the hall's floor through the wall without one.
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
    if (!f)
        return;
    // The beam trails the head a little, as a hand-held torch does.
    const float k = 1.0f - expf(-dt * 14.0f);
    glm_vec3_lerp(lights->flashlight_dir, (float*)forward, k, lights->flashlight_dir);
    glm_vec3_normalize(lights->flashlight_dir);
    vec3 right, up = {0.0f, 1.0f, 0.0f}, pos;
    glm_vec3_cross((float*)forward, up, right);
    glm_vec3_normalize(right);
    glm_vec3_copy((float*)eye, pos);
    glm_vec3_muladds(right, 0.16f, pos);
    pos[1] -= 0.22f;
    glm_vec3_muladds((float*)forward, 0.1f, pos);
    light_set_position(f, pos);
    light_set_direction(f, lights->flashlight_dir);
    f->intensity = lights->flashlight_on ? FLASHLIGHT_CANDELA : 0.0f;
    f->cast_shadows = lights->flashlight_on;
}

void lights_toggle_flashlight(Lights* lights) {
    lights->flashlight_on = !lights->flashlight_on;
}
