#include "cat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cetra/animation.h"
#include "cetra/engine.h"
#include "cetra/import.h"
#include "cetra/probe_set.h"
#include "cetra/program.h"

#include "cetra/game/animator_component.h"

#include "cat_clips.h"
#include "layout.h"

#define CAT_MODEL "assets/models/cat.glb"

// The body the player bumps into: half extents across, up and along, standing. Its top, at
// twice the half height, is above the player's step, so the player cannot climb onto the cat.
static const vec3 CAT_BOX = {0.07f, 0.16f, 0.20f};

// Where a cat can be, and what it is doing there: the feet, the way it faces (radians about
// +y, 0 facing +z) and the clip it holds.
typedef struct CatPlace {
    const char* name;
    vec3 feet;
    float yaw;
    CatClipId clip;
} CatPlace;

static const CatPlace PLACES[] = {
    // Home: curled on the study chair's leather cushion, the stained glass behind it and the
    // desk's candles a metre off.
    {"study_chair", {-5.02f, FLOOR2_Y + 0.505f, 9.0f}, 0.52f, CAT_CLIP_SLEEP},
    // On the counter between the bowls and the sink, looking out at the rain.
    {"kitchen_window", {1.95f, FLOOR_Y + 0.9f, 10.42f}, GLM_PIf, CAT_CLIP_SIT},
    // In the hall, facing the clock on its west wall.
    {"hall_clock", {-0.8f, FLOOR_Y, 13.15f}, -0.5f * GLM_PIf, CAT_CLIP_SIT},
    {"rug", {-0.75f, FLOOR_Y + 0.008f, 16.4f}, 0.4f, CAT_CLIP_LIE},
    {"stove_mat", {1.12f, FLOOR_Y + 0.004f, 11.72f}, -0.3f, CAT_CLIP_SLEEP},
    {"gallery", {0.0f, FLOOR2_Y, 14.45f}, 0.5f * GLM_PIf, CAT_CLIP_LIE},
};
const char* const CAT_PLACE_LIST =
    "study_chair, kitchen_window, hall_clock, rug, stove_mat, gallery";

// The coat, the values assets/scenes/cat.cscn gives the render app: twenty shells over the
// body, combed back and down and lying close; a shorter fringe standing up inside the ears.
typedef struct Coat {
    int layers;
    float length, density, thickness, root_shade, clump, lie;
    vec3 comb;
} Coat;
static const Coat BODY_COAT = {20,    0.011f, 900.0f, 0.75f,
                               0.28f, 0.85f,  0.75f,  {0.0f, -0.5f, -1.0f}};
static const Coat EAR_COAT = {10, 0.007f, 1100.0f, 0.4f, 0.7f, 0.6f, 0.3f, {0.0f, 1.0f, 0.0f}};

// The eyeshine. What the flashlight puts on the eye, in lux, times this, is how bright the
// eye glows back, and no brighter than the cap; it rises and falls over EASE. The eye throws
// the light back the way it came, so only a viewer near the beam sees it, and the falloff
// with the angle between the cat's gaze and the viewer is this power of its cosine.
#define SHINE_GAIN  2.5f
#define SHINE_MAX   600.0f // nits
#define SHINE_EASE  0.08f  // seconds
#define SHINE_POWER 6.0f

// A fur this light or lighter, by luminance, has a pink nose and pink inside its ears.
#define PINK_ABOVE 0.15f

static float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static void linear(const vec3 srgb, vec3 out) {
    for (int i = 0; i < 3; i++)
        out[i] = srgb_to_linear(srgb[i]);
}

static void dress(Material* m, const Coat* coat) {
    m->fur_layers = coat->layers;
    m->fur_length = coat->length;
    m->fur_density = coat->density;
    m->fur_thickness = coat->thickness;
    m->fur_root_shade = coat->root_shade;
    m->fur_clump = coat->clump;
    m->fur_lie = coat->lie;
    glm_vec3_copy((float*)coat->comb, m->fur_comb);
}

// A material of silent's own standing in for an imported one: every value the import gave it,
// then the cat's colours and coat. Its own because a material belongs to the scene that first
// registers it, and the import's belong to the model's scene, which is never drawn.
static Material* own_material(const Material* imported, const CatDesc* desc) {
    Material* m = create_material();
    if (!m)
        return NULL;
    for (size_t i = 0; i < MATERIAL_PARAM_COUNT; i++) {
        const MaterialParam* p = &MATERIAL_PARAMS[i];
        if (p->type == MATERIAL_PARAM_TEXTURE)
            continue;
        float v[3];
        material_param_get(imported, p, v);
        material_param_set(m, p, v);
    }
    m->alpha_mode = imported->alpha_mode;
    m->alphaCutoff = imported->alphaCutoff;
    m->doubleSided = imported->doubleSided;
    const char* name = imported->name ? imported->name : "";
    m->name = strdup(name);

    vec3 fur = {0.0f, 0.0f, 0.0f};
    linear(desc->fur, fur);
    const float luminance = 0.2126f * fur[0] + 0.7152f * fur[1] + 0.0722f * fur[2];
    const bool pink = luminance > PINK_ABOVE;
    if (!strcmp(name, "cat_fur")) {
        glm_vec3_copy(fur, m->albedo);
        dress(m, &BODY_COAT);
    } else if (!strcmp(name, "cat_eye")) {
        linear(desc->eyes, m->albedo);
        // Its glow is the eyeshine, which lights nothing: no panel is derived from it.
        glm_vec3_copy(m->albedo, m->emissive);
        m->emissive_strength = 0.0f;
        m->emissive_light = 1;
    } else if (!strcmp(name, "cat_ear")) {
        if (pink)
            linear((vec3){0.82f, 0.6f, 0.6f}, m->albedo);
        dress(m, &EAR_COAT);
    } else if (!strcmp(name, "cat_nose") && pink) {
        linear((vec3){0.79f, 0.55f, 0.55f}, m->albedo);
    }
    return m;
}

static void own_materials(SceneNode* node, const CatDesc* desc, Cat* cat) {
    for (size_t i = 0; i < node->mesh_count; i++) {
        Mesh* mesh = node->meshes[i];
        Material* m = mesh->material ? own_material(mesh->material, desc) : NULL;
        if (!m)
            continue;
        mesh->material = m;
        if (m->name && !strcmp(m->name, "cat_eye"))
            cat->eye = m;
        if (mesh->is_skinned && !cat->skin)
            cat->skin = node;
    }
    for (size_t i = 0; i < node->children_count; i++)
        own_materials(node->children[i], desc, cat);
}

// The model as a node that is not its scene's root, which cannot be moved under one of its
// own descendants: the root's children go into a wrapper carrying the root's own transform.
static SceneNode* take_model(const Scene* library) {
    SceneNode* root = library->root_node;
    SceneNode* wrapper = create_node();
    if (!wrapper)
        return NULL;
    node_set_name(wrapper, "cat_model");
    glm_mat4_copy(root->original_transform, wrapper->original_transform);
    // Bounded rather than drained: node_add_child refuses a cycle, and a while on a refusal
    // would spin forever.
    for (size_t guard = root->children_count; guard > 0 && root->children_count > 0; guard--)
        node_add_child(wrapper, root->children[0]);
    return wrapper;
}

static const CatPlace* find_place(const char* name) {
    for (size_t i = 0; i < sizeof(PLACES) / sizeof(PLACES[0]); i++)
        if (!strcmp(PLACES[i].name, name))
            return &PLACES[i];
    return NULL;
}

static int find_clip(const char* name) {
    for (int i = 0; i < CAT_CLIP_COUNT; i++)
        if (!strcmp(CAT_CLIPS[i].name, name))
            return i;
    return -1;
}

bool cat_create(Cat* cat, const CatDesc* desc, Game* game, Scene* scene, PhysicsWorld* physics) {
    memset(cat, 0, sizeof(*cat));
    cat->eye_bones[0] = cat->eye_bones[1] = -1;
    Engine* engine = game->engine;

    const CatPlace* place = find_place(desc->at ? desc->at : PLACES[0].name);
    if (!place) {
        fprintf(stderr, "silent: no place called '%s' for the cat (%s); home instead\n", desc->at,
                CAT_PLACE_LIST);
        place = &PLACES[0];
    }
    int clip = place->clip;
    if (desc->clip) {
        const int named = find_clip(desc->clip);
        if (named < 0)
            fprintf(stderr, "silent: the cat has no clip '%s'; holding '%s'\n", desc->clip,
                    CAT_CLIPS[clip].name);
        else
            clip = named;
    }

    // The model's own scene, owned by the engine from here and never drawn: the game draws
    // only its own. The model carries no textures, so nothing is left in flight on the loader.
    Scene* library = create_scene_from_model_path(CAT_MODEL, NULL, engine->async_loader);
    if (!library || library->skeleton_count == 0) {
        fprintf(stderr, "silent: cannot load the cat from %s\n", CAT_MODEL);
        if (library)
            free_scene(library);
        return false;
    }
    engine_add_scene(engine, library);
    SceneNode* model = take_model(library);
    if (!model)
        return false;
    own_materials(model, desc, cat);
    ShaderProgram* skinned = engine_find_program(engine, CETRA_PROGRAM_PBR_SKINNED);
    if (!skinned) {
        skinned = create_pbr_skinned_program();
        if (skinned)
            engine_add_program(engine, skinned);
    }
    node_set_programs(model, engine_get_program(engine, CETRA_PROGRAM_PBR), skinned);

    // The entity stands at its body's middle, so the model hangs below it to the feet.
    cat->entity = create_entity(game->entity_manager, "cat");
    if (!cat->entity)
        return false;
    glm_vec3_copy((float*)place->feet, cat->entity->position);
    cat->entity->position[1] += CAT_BOX[1];
    glm_quatv(cat->entity->rotation, place->yaw, (vec3){0.0f, 1.0f, 0.0f});
    cat->holder = create_node();
    node_set_name(cat->holder, "cat");
    SceneNode* inner = create_node();
    node_set_name(inner, "cat_rig");
    node_add_child(cat->holder, inner);
    node_add_child(inner, model);
    glm_translate_make(inner->original_transform, (vec3){0.0f, -CAT_BOX[1], 0.0f});
    cat->entity->node = cat->holder;

    PhysicsShapeDesc box = {.type = SHAPE_BOX,
                            .box.half_extents = {CAT_BOX[0], CAT_BOX[1], CAT_BOX[2]},
                            .density = 0.0f};
    entity_add_rigid_body(cat->entity, physics, &box, MOTION_KINEMATIC, OBJ_LAYER_KINEMATIC);

    Skeleton* skeleton = library->skeletons[0];
    cat->animator = entity_add_animator(cat->entity, create_animator(skeleton));
    const Animation* animation = scene_find_animation(library, CAT_CLIPS[clip].name);
    if (cat->animator && animation) {
        animator_play(cat->animator, animation, 0.0f, CAT_CLIPS[clip].looping != 0);
        if (desc->clip_seconds >= 0.0f) {
            animator_update(cat->animator, desc->clip_seconds);
            cat->animator->speed = 0.0f;
        }
    } else {
        fprintf(stderr, "silent: the cat's model has no clip '%s'\n", CAT_CLIPS[clip].name);
    }
    cat->eyes_shut = clip == CAT_CLIP_SLEEP;
    cat->eye_bones[0] = get_bone_index_by_name(skeleton, "Eye.L");
    cat->eye_bones[1] = get_bone_index_by_name(skeleton, "Eye.R");
    cat->eyeshine = desc->eyeshine && cat->eye && cat->skin;
    printf("silent: the cat at %s, %s\n", place->name, CAT_CLIPS[clip].name);
    return true;
}

// Where one eye is in the world, and which way it looks, from the pose being drawn.
static bool eye_world(const Cat* cat, int side, vec3 at, vec3 gaze) {
    const int bone = cat->eye_bones[side];
    if (bone < 0 || !cat->animator)
        return false;
    mat4 m;
    glm_mat4_mul(cat->skin->global_transform, cat->animator->state->bone_matrices[bone], m);
    const float x = side == 0 ? CAT_EYE_X : -CAT_EYE_X;
    vec3 ahead;
    glm_mat4_mulv3(m, (vec3){x, CAT_EYE_Y, CAT_EYE_Z}, 1.0f, at);
    glm_mat4_mulv3(m, (vec3){x, CAT_EYE_Y, CAT_EYE_Z + 0.02f}, 1.0f, ahead);
    glm_vec3_sub(ahead, at, gaze);
    glm_vec3_normalize(gaze);
    return true;
}

// How bright the eyes glow now, in nits: what the flashlight throws on them, seen back along
// the beam, unless a wall stands between them or the lids are shut.
static float shine_now(const Cat* cat, Game* game, const Lights* lights, const vec3 viewer) {
    if (!cat->eyeshine || !cat->attached || cat->eyes_shut || !lights || !lights->flashlight ||
        !lights->flashlight_on)
        return 0.0f;
    vec3 at = {0.0f, 0.0f, 0.0f}, gaze = {0.0f, 0.0f, 0.0f};
    for (int side = 0; side < 2; side++) {
        vec3 a = {0.0f, 0.0f, 0.0f}, g = {0.0f, 0.0f, 0.0f};
        if (!eye_world(cat, side, a, g))
            return 0.0f;
        glm_vec3_muladds(a, 0.5f, at);
        glm_vec3_muladds(g, 0.5f, gaze);
    }
    glm_vec3_normalize(gaze);
    const Light* f = lights->flashlight;
    vec3 beam;
    glm_vec3_sub(at, (float*)f->global_position, beam);
    const float d = glm_vec3_norm(beam);
    if (d < 1e-3f)
        return 0.0f;
    glm_vec3_scale(beam, 1.0f / d, beam);
    vec3 aim;
    glm_vec3_copy((float*)f->direction, aim);
    const float cone = glm_smoothstep(f->outerCutOff, f->cutOff, glm_vec3_dot(beam, aim));
    if (cone <= 0.0f)
        return 0.0f;
    RaycastHit hit;
    if (game->physics_world &&
        physics_world_raycast_filtered(game->physics_world, (float*)f->global_position, beam,
                                       d - 0.05f, 1u << OBJ_LAYER_STATIC, &hit) &&
        hit.hit)
        return 0.0f;
    vec3 back;
    glm_vec3_sub((float*)viewer, at, back);
    glm_vec3_normalize(back);
    const float facing = powf(fmaxf(0.0f, glm_vec3_dot(gaze, back)), SHINE_POWER);
    const float lux = f->intensity * cone / (d * d);
    return fminf(SHINE_MAX, SHINE_GAIN * lux * facing);
}

void cat_update(Cat* cat, Game* game, Scene* scene, const Lights* lights, const vec3 viewer,
                float dt) {
    if (!cat->entity)
        return;
    // Into the scene once the reflection probes have their pictures. They capture once, and a
    // cat in a room then would be in that room's reflections for good, asleep on a chair it
    // has long since left.
    if (!cat->attached && game->engine->total_frames > 2 &&
        (!scene->probe_set || scene->probe_set->ready)) {
        node_add_child(scene->root_node, cat->holder);
        cat->attached = true;
    }
    const float target = shine_now(cat, game, lights, viewer);
    cat->shine += (target - cat->shine) * (1.0f - expf(-dt / SHINE_EASE));
    if (cat->eye)
        cat->eye->emissive_strength = cat->shine;
}
