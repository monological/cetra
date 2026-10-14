#include <stdio.h>
#include <string.h>

#include "cetra/import.h"

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

// `name` under `node`, `above` the frame `node` is in; the frame of what was found in `frame`.
static SceneNode* find_from(SceneNode* node, const char* name, const mat4 above, mat4 frame) {
    mat4 here;
    glm_mat4_mul((vec4*)above, node->original_transform, here);
    if (node->name && !strcmp(node->name, name)) {
        glm_mat4_copy(here, frame);
        return node;
    }
    for (size_t i = 0; i < node->children_count; i++) {
        SceneNode* found = find_from(node->children[i], name, here, frame);
        if (found)
            return found;
    }
    return NULL;
}

SceneNode* library_find(SceneNode* root, const char* name, mat4 frame) {
    for (size_t i = 0; i < root->children_count; i++) {
        SceneNode* found = find_from(root->children[i], name, GLM_MAT4_IDENTITY, frame);
        if (found)
            return found;
    }
    return NULL;
}
