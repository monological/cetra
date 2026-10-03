#include "cat_body.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cetra/cscene.h"
#include "cetra/engine.h"
#include "cetra/import.h"
#include "cetra/look_at.h"
#include "cetra/program.h"

#include "cetra/game/animator_component.h"

#define CAT_MODEL "assets/models/cat.glb"
// The coat: the render app's scene for the cat, whose material block is the one statement of
// it, read here for the same materials.
#define CAT_COAT "assets/scenes/cat.cscn"

// The body the player bumps into: half extents across, up and along, standing. Its top, at
// twice the half height, is above the player's step, so the player cannot climb onto the cat.
static const vec3 CAT_BOX = {0.07f, CAT_HALF_HEIGHT, 0.20f};

// The eyeshine. What the flashlight puts on the eye, in lux, times this, is how bright the
// eye glows back, and no brighter than the cap. The eye throws the light back the way it
// came, so only a viewer near the beam sees it, and the falloff with the angle between the
// cat's gaze and the viewer is this power of its cosine.
#define SHINE_GAIN  2.5f
#define SHINE_MAX   600.0f // nits
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

// The coat the scene file gives a material of this name, if it gives one.
static void dress(Material* m, const CetraSceneDesc* coat) {
    for (int i = 0; coat && m->name && i < coat->material_count; i++) {
        const CSceneMaterialOverride* o = &coat->materials[i];
        if (strcmp(o->material, m->name))
            continue;
        for (int k = 0; k < o->param_count; k++) {
            const MaterialParam* p = material_param_find(o->params[k].key);
            if (p && material_param_components(p) == o->params[k].components)
                material_param_set(m, p, o->params[k].value);
        }
    }
}

// The import's materials, moved from the model's scene -- which is never drawn -- to the
// game's, each with the cat's coat on; its colours are cat_set_fur's and cat_set_eyes'.
static void take_materials(SceneNode* node, Scene* library, Scene* game, Cat* cat,
                           const CetraSceneDesc* coat) {
    for (size_t i = 0; i < node->mesh_count; i++) {
        Mesh* mesh = node->meshes[i];
        Material* m = mesh->material;
        if (!m)
            continue;
        const char* name = m->name ? m->name : "";
        // Meshes may share one: it is moved and dressed once.
        if (scene_release_material(library, m)) {
            scene_add_material(game, m);
            dress(m, coat);
            if (!strcmp(name, "cat_eye")) {
                // Its glow is the eyeshine, which lights nothing: no panel is derived from it.
                m->emissive_strength = 0.0f;
                m->emissive_light = 1;
            }
        }
        if (!strcmp(name, "cat_eye")) {
            cat->eye = m;
        } else if (!strcmp(name, "cat_fur")) {
            cat->fur = m;
        } else if (!strcmp(name, "cat_ear")) {
            cat->ear = m;
            glm_vec3_copy(m->albedo, cat->ear_dark);
        } else if (!strcmp(name, "cat_nose")) {
            cat->nose = m;
            glm_vec3_copy(m->albedo, cat->nose_dark);
        }
        if (mesh->is_skinned && !cat->skin)
            cat->skin = node;
    }
    for (size_t i = 0; i < node->children_count; i++)
        take_materials(node->children[i], library, game, cat, coat);
}

void cat_set_fur(Cat* cat, const vec3 srgb) {
    glm_vec3_copy((float*)srgb, cat->fur_srgb);
    vec3 fur = {0.0f, 0.0f, 0.0f};
    linear(srgb, fur);
    if (cat->fur)
        glm_vec3_copy(fur, cat->fur->albedo);
    const bool pink = glm_luminance(fur) > PINK_ABOVE;
    if (cat->ear) {
        if (pink)
            linear((vec3){0.82f, 0.6f, 0.6f}, cat->ear->albedo);
        else
            glm_vec3_copy(cat->ear_dark, cat->ear->albedo);
    }
    if (cat->nose) {
        if (pink)
            linear((vec3){0.79f, 0.55f, 0.55f}, cat->nose->albedo);
        else
            glm_vec3_copy(cat->nose_dark, cat->nose->albedo);
    }
}

void cat_set_eyes(Cat* cat, const vec3 srgb) {
    glm_vec3_copy((float*)srgb, cat->eyes_srgb);
    if (!cat->eye)
        return;
    linear(srgb, cat->eye->albedo);
    glm_vec3_copy(cat->eye->albedo, cat->eye->emissive);
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

bool cat_body_load(Cat* cat, const CatDesc* desc, Game* game, PhysicsWorld* physics,
                   const vec3 feet, float yaw) {
    Engine* engine = game->engine;
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
    CetraSceneDesc* coat = cscene_load(CAT_COAT);
    if (!coat)
        fprintf(stderr, "silent: no coat for the cat in %s\n", CAT_COAT);
    take_materials(model, library, game->scene, cat, coat);
    cscene_free(coat);
    cat_set_fur(cat, desc->fur);
    cat_set_eyes(cat, desc->eyes);
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
    glm_vec3_copy((float*)feet, cat->entity->position);
    cat->entity->position[1] += CAT_HALF_HEIGHT;
    glm_quatv(cat->entity->rotation, yaw, (vec3){0.0f, 1.0f, 0.0f});
    SceneNode* holder = create_node();
    node_set_name(holder, "cat");
    SceneNode* inner = create_node();
    node_set_name(inner, "cat_rig");
    node_add_child(holder, inner);
    node_add_child(inner, model);
    glm_translate_make(inner->original_transform, (vec3){0.0f, -CAT_HALF_HEIGHT, 0.0f});
    cat->entity->node = holder;

    PhysicsShapeDesc box = {.type = SHAPE_BOX,
                            .box.half_extents = {CAT_BOX[0], CAT_BOX[1], CAT_BOX[2]},
                            .density = 0.0f};
    entity_add_rigid_body(cat->entity, physics, &box, MOTION_KINEMATIC, OBJ_LAYER_KINEMATIC);

    Skeleton* skeleton = library->skeletons[0];
    cat->animator = entity_add_animator(cat->entity, create_animator(skeleton));
    if (!cat->animator) {
        destroy_entity(game->entity_manager, cat->entity);
        cat->entity = NULL;
        return false;
    }
    // The walks, the flights and the turns state where they take the body, and it goes there.
    cat->animator->root_motion = true;
    for (int i = 0; i < CAT_CLIP_COUNT; i++) {
        Animation* a = scene_find_animation(library, CAT_CLIPS[i].name);
        cat->clips[i] = a;
        if (!a) {
            fprintf(stderr, "silent: the cat's model has no clip '%s'\n", CAT_CLIPS[i].name);
            continue;
        }
        // The events the build wrote down in seconds, onto the clip in its own ticks.
        for (int e = 0; e < CAT_CLIPS[i].event_count; e++)
            animation_add_event(a, CAT_CLIPS[i].events[e].seconds * a->ticks_per_second,
                                CAT_CLIPS[i].events[e].name);
    }
    cat->eye_bones[0] = get_bone_index_by_name(skeleton, "Eye.L");
    cat->eye_bones[1] = get_bone_index_by_name(skeleton, "Eye.R");
    cat->eyeshine = desc->eyeshine && cat->eye && cat->skin;

    // The head turns on the neck: the upper neck takes a share and the head the rest, so a
    // glance over the shoulder bends the neck rather than screwing the head round on it.
    LookAtSystem* look = create_look_at_system(skeleton);
    if (look && look_at_add_bone(look, "Neck2", 0.4f) && look_at_add_bone(look, "Head", 0.6f)) {
        glm_vec3_copy((vec3){0.0f, CAT_EYE_Y, CAT_EYE_Z}, look->eye);
        look->blend_rate = 3.0f;
        cat->animator->state->look_at = look;
    } else {
        free_look_at_system(look);
    }
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

bool cat_eye(const Cat* cat, vec3 at, vec3 forward) {
    if (!cat->entity || !cat->skin)
        return false;
    glm_vec3_zero(at);
    glm_vec3_zero(forward);
    for (int side = 0; side < 2; side++) {
        vec3 a = {0.0f, 0.0f, 0.0f}, g = {0.0f, 0.0f, 0.0f};
        if (!eye_world(cat, side, a, g))
            return false;
        glm_vec3_muladds(a, 0.5f, at);
        glm_vec3_muladds(g, 0.5f, forward);
    }
    glm_vec3_normalize(forward);
    return true;
}

void cat_feet(const Cat* cat, vec3 out) {
    glm_vec3_copy((float*)cat->entity->position, out);
    out[1] -= CAT_HALF_HEIGHT;
}

// What the flashlight throws on the eyes, seen back along the beam, unless a wall stands
// between them or the lids are shut.
float cat_body_shine(const Cat* cat, Game* game, const Lights* lights, const vec3 viewer) {
    if (!cat->eyeshine || !cat->attached || cat_eyes_shut(cat) || !lights || !lights->flashlight ||
        !lights->flashlight_on)
        return 0.0f;
    vec3 at = {0.0f, 0.0f, 0.0f}, gaze = {0.0f, 0.0f, 0.0f};
    if (!cat_eye(cat, at, gaze))
        return 0.0f;
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

// The entity's pose this step, carried down the fixed chain of nodes between it and the skin --
// not the skin's global, which is the last frame's until the walk after this hook.
void cat_body_world(const Cat* cat, mat4 out) {
    mat4 chain = GLM_MAT4_IDENTITY_INIT;
    for (const SceneNode* n = cat->skin; n && n != cat->entity->node; n = n->parent)
        glm_mat4_mul((vec4*)n->original_transform, chain, chain);
    mat4 entity = GLM_MAT4_IDENTITY_INIT;
    entity_get_transform_matrix(cat->entity, entity);
    glm_mat4_mul(entity, chain, out);
}
