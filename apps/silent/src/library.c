#include <stdio.h>

#include "cetra/import.h"
#include "cetra/program.h"

#include "library.h"

// Every material under `node`, moved from the library to the game's scene: meshes may share one,
// which is moved once.
static void take_materials(SceneNode* node, Scene* library, Scene* game) {
    for (size_t i = 0; i < node->mesh_count; i++) {
        Material* m = node->meshes[i]->material;
        if (m && scene_release_material(library, m))
            scene_add_material(game, m);
    }
    for (size_t i = 0; i < node->children_count; i++)
        take_materials(node->children[i], library, game);
}

SceneNode* library_take(Engine* engine, Scene* game, const char* path, const char* name,
                        Scene** library) {
    Scene* lib = create_scene_from_model_path(path, NULL, engine->async_loader);
    if (!lib) {
        fprintf(stderr, "silent: cannot load %s\n", path);
        return NULL;
    }
    engine_add_scene(engine, lib);
    // Its maps in before anything draws it: a picture of it drawn once and kept, as the
    // backpack's are, would keep a model with none.
    engine_finish_texture_loads(engine);
    // The root's children into a node carrying the root's own transform: a root cannot be moved
    // under a node of another graph. Bounded rather than drained, since node_add_child refuses a
    // cycle and a while on a refusal would spin forever.
    SceneNode* root = lib->root_node;
    SceneNode* model = create_node();
    if (!model)
        return NULL;
    node_set_name(model, name);
    glm_mat4_copy(root->original_transform, model->original_transform);
    for (size_t guard = root->children_count; guard > 0 && root->children_count > 0; guard--)
        node_add_child(model, root->children[0]);
    take_materials(model, lib, game);
    if (library)
        *library = lib;
    return model;
}

void library_programs(Engine* engine, SceneNode* model) {
    ShaderProgram* skinned = engine_find_program(engine, CETRA_PROGRAM_PBR_SKINNED);
    if (!skinned) {
        skinned = create_pbr_skinned_program();
        if (skinned)
            engine_add_program(engine, skinned);
    }
    node_set_programs(model, engine_get_program(engine, CETRA_PROGRAM_PBR), skinned);
}

SceneNode* library_find(SceneNode* root, const char* name, mat4 frame) {
    SceneNode* found = NULL;
    for (size_t i = 0; i < root->children_count && !found; i++)
        found = node_find(root->children[i], name);
    if (found && frame) {
        glm_mat4_copy(found->original_transform, frame);
        for (const SceneNode* up = found->parent; up && up != root; up = up->parent)
            glm_mat4_mul((vec4*)up->original_transform, frame, frame);
    }
    return found;
}
