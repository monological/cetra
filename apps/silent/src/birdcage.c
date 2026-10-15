#include <stdio.h>

#include "basement.h"
#include "birdcage.h"
#include "library.h"

#define CAGE_MODEL "assets/models/birdcage.glb"
// Where it stands on the workbench, in the bench's frame (BASEMENT_BENCH): in the clear stretch
// between the vise and the jar where the key lay, turned a little off square to the bench's front.
#define CAGE_BENCH_A    0.85f
#define CAGE_BENCH_D    0.3f
#define CAGE_BENCH_TURN 0.15f
#define CAGE_MIDDLE     0.12f // its middle over its foot, what a hand reaches for

bool birdcage_build(Birdcage* cage, Engine* engine, Scene* scene) {
    *cage = (Birdcage){0};
    glm_mat4_identity(cage->door_rest);
    glm_mat4_identity(cage->hang);
    cage->node = library_take(engine, scene, CAGE_MODEL, "birdcage", NULL);
    if (!cage->node)
        return false;
    cage->door = library_find(cage->node, "cage_door", NULL);
    if (!cage->door || !library_find(cage->node, "key_hang", cage->hang)) {
        fprintf(stderr, "silent: %s has no door or no hook\n", CAGE_MODEL);
        free_node(cage->node);
        *cage = (Birdcage){0};
        return false;
    }
    glm_mat4_copy(cage->door->original_transform, cage->door_rest);
    library_programs(engine, cage->node);
    // It is moved while the game runs, so no capture keeps it.
    cage->node->capture_hidden = true;

    vec3 foot = {0.0f, 0.0f, 0.0f};
    kit_frame_point(&BASEMENT_BENCH, CAGE_BENCH_A, BASEMENT_BENCH_TOP, CAGE_BENCH_D, foot);
    glm_translate_make(cage->bench, foot);
    glm_rotate_y(cage->bench, BASEMENT_BENCH.yaw + CAGE_BENCH_TURN, cage->bench);
    glm_vec3_copy(foot, cage->at);
    cage->at[1] += CAGE_MIDDLE;
    birdcage_place(cage, cage->bench);
    node_add_child(scene->root_node, cage->node);
    return true;
}

void birdcage_free(Birdcage* cage) {
    free_node(cage->node);
    *cage = (Birdcage){0};
}

void birdcage_place(Birdcage* cage, const mat4 world) {
    if (cage->node)
        glm_mat4_copy((vec4*)world, cage->node->original_transform);
}

void birdcage_door(Birdcage* cage, float angle) {
    if (cage->door)
        glm_rotate_y(cage->door_rest, angle, cage->door->original_transform);
}

void birdcage_key(const Birdcage* cage, mat4 out) {
    glm_mat4_mul(cage->node->original_transform, (vec4*)cage->hang, out);
}
