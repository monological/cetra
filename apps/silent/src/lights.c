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
#define TUBE_RANGE 9.0f // metres: across the kitchen and down the hall

typedef struct Tube {
    const char* name; // the node's name, which the derived light takes
    vec3 centre;
    vec3 axis;   // along the tube
    vec3 facing; // the side the strip emits from
    float length, width;
    float nits;
    bool shadows;
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
     true,
     true},
    {"tube_window",
     {2.6f, KITCHEN_WIN_HEAD + 0.16f, KITCHEN_Z0 + 0.075f},
     {1.0f, 0.0f, 0.0f},
     {0.0f, -0.5f, 0.866f},
     1.2f,
     0.035f,
     6000.0f,
     true,
     false},
    {"tube_hood",
     {KITCHEN_X0 + 0.3f, FLOOR_Y + 1.605f, STOVE_Z},
     {0.0f, 0.0f, 1.0f},
     {0.0f, -1.0f, 0.0f},
     0.55f,
     0.03f,
     3500.0f,
     false,
     false},
};
#define TUBE_COUNT ((int)(sizeof(TUBES) / sizeof(TUBES[0])))

// The strip itself: one quad, alone in its mesh so the engine can fit a
// panel to it, facing where the tube throws its light.
static void tube_strip(Scene* scene, ShaderProgram* pbr, const Tube* t, Lights* lights, int i) {
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
    // Wound to face `facing`: a->b runs along the tube, b->c across it.
    vec3 e1, e2, w;
    glm_vec3_sub(b, a, e1);
    glm_vec3_sub(c, a, e2);
    glm_vec3_cross(e1, e2, w);
    if (glm_vec3_dot(w, n) >= 0.0f) {
        mb_tri(&mb, ia, ib, ic);
        mb_tri(&mb, ia, ic, id);
    } else {
        mb_tri(&mb, ia, ic, ib);
        mb_tri(&mb, ia, id, ic);
    }

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
        return;
    }
    mesh->material = m;
    SceneNode* node = create_node();
    node_set_name(node, t->name);
    node_add_mesh(node, mesh);
    node_add_child(scene->root_node, node);

    lights->tube_materials[i] = m;
    lights->tube_nits[i] = t->nits;
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

void lights_build(Lights* lights, Kit* kit, Engine* engine, Scene* scene, unsigned int seed,
                  bool flicker, bool flashlight_on) {
    memset(lights, 0, sizeof(*lights));
    lights->seed = seed;
    lights->flicker_tube = -1;
    ShaderProgram* pbr = engine_get_program(engine, CETRA_PROGRAM_PBR);
    // The panels come from the strips: this is what turns them into light.
    engine->emissive_lights_enabled = true;

    // FIRST among the spots, before any street lamp: the fog scatters the
    // scene's first spot into a visible beam, and that should be this one.
    lights->flashlight_candela = 900.0f;
    LightDesc desc = {.name = "flashlight",
                      .type = LIGHT_SPOT,
                      .color = {1.0f, 0.93f, 0.80f},
                      .intensity = lights->flashlight_candela,
                      .range = 25.0f,
                      .inner_cutoff = glm_rad(9.0f),
                      .outer_cutoff = glm_rad(21.0f),
                      .cast_shadows = true};
    lights->flashlight = create_light(&desc);
    scene_add_light(scene, lights->flashlight);
    lights->flashlight_on = flashlight_on;
    glm_vec3_copy((vec3){0.0f, 0.0f, -1.0f}, lights->flashlight_dir);

    for (int i = 0; i < TUBE_COUNT && i < LIGHTS_MAX_TUBES; i++) {
        tube_fixture(kit, &TUBES[i]);
        tube_strip(scene, pbr, &TUBES[i], lights, i);
        if (flicker && TUBES[i].flicker)
            lights->flicker_tube = i;
    }
    lights->tube_count = TUBE_COUNT < LIGHTS_MAX_TUBES ? TUBE_COUNT : LIGHTS_MAX_TUBES;
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

void lights_update(Lights* lights, Scene* scene, double time, float dt, const vec3 eye,
                   const vec3 forward) {
    if (lights->flicker_tube >= 0) {
        Material* m = lights->tube_materials[lights->flicker_tube];
        if (m)
            m->emissive_strength =
                lights->tube_nits[lights->flicker_tube] * flicker_level(time, lights->seed);
    }

    // The derived panels are the engine's, created on the first frame and kept
    // after; their reach and their casting are ours to state. Left derived, a
    // 6000-nit panel claims every cluster in the frustum. By name each frame
    // rather than by a pointer kept from the first, so a panel the engine
    // re-derives is never one this holds dangling.
    for (size_t i = 0; i < scene->light_count; i++) {
        Light* l = scene->lights[i];
        if (!l || !l->name || l->type != LIGHT_AREA)
            continue;
        for (int t = 0; t < TUBE_COUNT; t++) {
            if (strcmp(l->name, TUBES[t].name) != 0)
                continue;
            l->range = TUBE_RANGE;
            l->cast_shadows = TUBES[t].shadows;
        }
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
    f->intensity = lights->flashlight_on ? lights->flashlight_candela : 0.0f;
    f->cast_shadows = lights->flashlight_on;
}

void lights_toggle_flashlight(Lights* lights) {
    lights->flashlight_on = !lights->flashlight_on;
}
