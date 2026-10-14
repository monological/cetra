#include <stdio.h>

#include "cetra/animation.h"
#include "cetra/program.h"

#include "arms.h"
#include "kit.h"
#include "library.h"

#define ARMS_MODEL "assets/models/arms.glb"
// A node of the skinned meshes, all of which hang alike from the rig, so one stands for the rest.
#define ARMS_SKIN "hand_R"

static const char* const AT_BONES[ARMS_AT_COUNT] = {"View", "Cage", "Lamp", ARMS_KEY_BONE};

// Every mesh under `node` drawn and casting nothing: so near the eye a sleeve would put the room in
// its shadow, and the flashlight's own beam with it.
static void cast_nothing(SceneNode* node) {
    for (size_t i = 0; i < node->mesh_count; i++)
        node->meshes[i]->shadow_role = MESH_SHADOW_NONE;
    for (size_t i = 0; i < node->children_count; i++)
        cast_nothing(node->children[i]);
}

bool arms_load(Arms* arms, Engine* engine, Scene* scene) {
    *arms = (Arms){.root = scene->root_node};
    glm_mat4_identity(arms->skin);
    glm_mat4_identity(arms->body);
    Scene* library = NULL;
    arms->node = library_take(engine, scene, ARMS_MODEL, "arms", &library);
    if (!arms->node)
        return false;
    if (library->skeleton_count == 0) {
        fprintf(stderr, "silent: %s has no rig\n", ARMS_MODEL);
        return false;
    }
    if (!library_find(arms->node, ARMS_SKIN, arms->skin))
        fprintf(stderr, "silent: %s has no '%s'; its bones are read in its own frame\n", ARMS_MODEL,
                ARMS_SKIN);
    ShaderProgram* skinned = engine_find_program(engine, CETRA_PROGRAM_PBR_SKINNED);
    if (!skinned) {
        skinned = create_pbr_skinned_program();
        if (skinned)
            engine_add_program(engine, skinned);
    }
    node_set_programs(arms->node, engine_get_program(engine, CETRA_PROGRAM_PBR), skinned);
    cast_nothing(arms->node);
    // It moves with the player, so no capture keeps it.
    arms->node->capture_hidden = true;

    Skeleton* skeleton = library->skeletons[0];
    arms->animator = create_animator(skeleton);
    if (!arms->animator)
        return false;
    node_set_pose(arms->node, arms->animator->state);
    for (int i = 0; i < ARMS_CLIP_COUNT; i++) {
        Animation* a = scene_find_animation(library, ARMS_CLIPS[i].name);
        arms->clips[i] = a;
        if (!a) {
            fprintf(stderr, "silent: %s has no clip '%s'\n", ARMS_MODEL, ARMS_CLIPS[i].name);
            continue;
        }
        // The events the build wrote down in seconds, onto the clip in its own ticks.
        for (int e = 0; e < ARMS_CLIPS[i].event_count; e++)
            animation_add_event(a, ARMS_CLIPS[i].events[e].seconds * a->ticks_per_second,
                                ARMS_CLIPS[i].events[e].name);
    }
    for (int i = 0; i < ARMS_AT_COUNT; i++) {
        arms->bones[i] = get_bone_index_by_name(skeleton, AT_BONES[i]);
        if (arms->bones[i] < 0) {
            fprintf(stderr, "silent: %s has no bone '%s'\n", ARMS_MODEL, AT_BONES[i]);
            return false;
        }
        glm_mat4_inv(skeleton->bones[arms->bones[i]].inverse_bind_pose, arms->bind[i]);
    }
    return true;
}

void arms_free(Arms* arms) {
    arms_show(arms, false);
    free_node(arms->node);
    free_animator(arms->animator);
    *arms = (Arms){0};
}

void arms_show(Arms* arms, bool shown) {
    if (!arms->node || shown == arms->shown)
        return;
    kit_node_shown(arms->root, arms->node, shown);
    arms->shown = shown;
}

void arms_play(Arms* arms, ArmsClipId clip, float fade) {
    if (!arms->animator || !arms->clips[clip])
        return;
    arms->playing = clip;
    animator_play(arms->animator, arms->clips[clip], fade, ARMS_CLIPS[clip].looping);
}

void arms_frame(Arms* arms, const mat4 body, float dt, AnimatorEventFn events, void* user) {
    if (!arms->animator)
        return;
    glm_mat4_copy((vec4*)body, arms->body);
    glm_mat4_copy((vec4*)body, arms->node->original_transform);
    animator_set_event_callback(arms->animator, events, user);
    animator_update(arms->animator, dt);
}

float arms_seconds(const Arms* arms) {
    const Animation* a = arms->animator ? arms->clips[arms->playing] : NULL;
    return a && a->ticks_per_second > 0.0 ? arms->animator->base.time / (float)a->ticks_per_second
                                          : 0.0f;
}

bool arms_done(const Arms* arms) {
    return arms->animator && arms->animator->base.finished;
}

void arms_at(const Arms* arms, ArmsAt at, mat4 out) {
    // As the skin is drawn: the pose carries a bone from where it was bound to where it is, in the
    // skin's frame, which the body carries into the world.
    mat4 posed;
    glm_mat4_mul(arms->animator->state->bone_matrices[arms->bones[at]], (vec4*)arms->bind[at],
                 posed);
    glm_mat4_mul((vec4*)arms->body, (vec4*)arms->skin, out);
    glm_mat4_mul(out, posed, out);
    if (at == ARMS_AT_KEY) {
        mat4 key;
        glm_mat4_make(ARMS_KEY_ON_BONE, key);
        glm_mat4_mul(out, key, out);
    }
}
