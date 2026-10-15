#include <stdio.h>

#include "cetra/animation.h"

#include "arms.h"
#include "kit.h"
#include "library.h"

#define ARMS_MODEL "assets/models/arms.glb"
// A node of the skinned meshes, all of which hang alike from the rig, so one stands for the rest.
#define ARMS_SKIN "hand_R"

static const char* const AT_BONES[ARMS_AT_COUNT] = {"View", "Cage", "Lamp", ARMS_KEY_BONE};

// Nothing kept: what was made is let go, so nothing reads a rig that is not whole.
static bool refused(Arms* arms) {
    arms_free(arms);
    return false;
}

bool arms_load(Arms* arms, Engine* engine, Scene* scene) {
    *arms = (Arms){.root = scene->root_node};
    glm_mat4_identity(arms->skin);
    glm_mat4_make(ARMS_KEY_ON_BONE, arms->key_on_bone);
    Scene* library = NULL;
    arms->node = library_take(engine, scene, ARMS_MODEL, "arms", &library);
    if (!arms->node)
        return false;
    if (library->skeleton_count == 0) {
        fprintf(stderr, "silent: %s has no rig\n", ARMS_MODEL);
        return refused(arms);
    }
    if (!library_find(arms->node, ARMS_SKIN, arms->skin))
        fprintf(stderr, "silent: %s has no '%s'; its bones are read in its own frame\n", ARMS_MODEL,
                ARMS_SKIN);
    library_programs(engine, arms->node);
    // So near the eye a sleeve would put the room in its shadow, and the flashlight's own beam
    // with it.
    kit_node_casts(arms->node, false);
    // It moves with the player, so no capture keeps it.
    arms->node->capture_hidden = true;

    Skeleton* skeleton = library->skeletons[0];
    for (int i = 0; i < ARMS_AT_COUNT; i++) {
        arms->bones[i] = get_bone_index_by_name(skeleton, AT_BONES[i]);
        if (arms->bones[i] < 0) {
            fprintf(stderr, "silent: %s has no bone '%s'\n", ARMS_MODEL, AT_BONES[i]);
            return refused(arms);
        }
    }
    arms->animator = create_animator(skeleton);
    if (!arms->animator)
        return refused(arms);
    node_set_pose(arms->node, arms->animator->state);
    for (int i = 0; i < ARMS_CLIP_COUNT; i++) {
        Animation* a = scene_find_animation(library, ARMS_CLIPS[i].name);
        arms->clips[i] = a;
        if (!a) {
            fprintf(stderr, "silent: %s has no clip '%s'\n", ARMS_MODEL, ARMS_CLIPS[i].name);
            return refused(arms);
        }
        // The events the build wrote down in seconds, onto the clip in its own ticks.
        for (int e = 0; e < ARMS_CLIPS[i].event_count; e++)
            animation_add_event(a, ARMS_CLIPS[i].events[e].seconds * a->ticks_per_second,
                                ARMS_CLIPS[i].events[e].name);
    }
    return true;
}

void arms_free(Arms* arms) {
    arms_show(arms, false);
    if (arms->node)
        free_node(arms->node);
    if (arms->animator)
        free_animator(arms->animator);
    *arms = (Arms){0};
}

void arms_show(Arms* arms, bool shown) {
    kit_node_shown(arms->root, arms->node, shown);
}

void arms_place(Arms* arms, const mat4 body) {
    glm_mat4_copy((vec4*)body, arms->node->original_transform);
}

void arms_play(Arms* arms, ArmsClipId clip, float fade) {
    animator_play(arms->animator, arms->clips[clip], fade, ARMS_CLIPS[clip].looping);
}

void arms_frame(Arms* arms, float dt) {
    animator_update(arms->animator, dt);
}

float arms_seconds(const Arms* arms) {
    const AnimatorSpace* s = &arms->animator->base;
    const Animation* a = s->count > 0 ? s->entries[s->ref].clip : NULL;
    return a && a->ticks_per_second > 0.0 ? s->time / (float)a->ticks_per_second : 0.0f;
}

bool arms_done(const Arms* arms) {
    return arms->animator->base.finished;
}

void arms_at(const Arms* arms, ArmsAt at, mat4 out) {
    // The bone as it is posed, in the skin's frame, which the body carries into the world.
    glm_mat4_mul(arms->node->original_transform, (vec4*)arms->skin, out);
    glm_mat4_mul(out, arms->animator->state->global_transforms[arms->bones[at]], out);
    if (at == ARMS_AT_KEY)
        glm_mat4_mul(out, (vec4*)arms->key_on_bone, out);
}
