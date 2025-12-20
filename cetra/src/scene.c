
#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "ext/log.h"
#include "scene.h"
#include "program.h"
#include "shader.h"
#include "mesh.h"
#include "material.h"
#include "light.h"
#include "camera.h"
#include "common.h"
#include "util.h"

/*
 * prototypes
 */
static void _set_xyz_program_for_nodes(SceneNode* node, ShaderProgram* program);

Scene* create_scene() {
    Scene* scene = malloc(sizeof(Scene));
    if (!scene) {
        log_error("Failed to allocate memory for Scene");
        return NULL;
    }
    memset(scene, 0, sizeof(Scene));

    // Initialize the Scene structure
    scene->root_node = NULL;
    scene->lights = NULL;
    scene->light_count = 0;
    scene->cameras = NULL;
    scene->camera_count = 0;

    scene->tex_pool = create_texture_pool();

    scene->xyz_shader_program = NULL;

    // Initialize light cache (will be allocated on first use)
    scene->light_cache_pairs = NULL;
    scene->light_cache_result = NULL;
    scene->light_cache_capacity = 0;

    return scene;
}

void free_scene(Scene* scene) {
    if (!scene) {
        return; // Nothing to free
    }

    // Free texture pool
    if (scene->tex_pool) {
        free_texture_pool(scene->tex_pool);
        scene->tex_pool = NULL;
    }

    // Free all lights
    if (scene->lights) {
        for (size_t i = 0; i < scene->light_count; i++) {
            if (scene->lights[i]) {
                free_light(scene->lights[i]);
            }
        }
        free(scene->lights);
        scene->lights = NULL;
    }

    // Free all cameras
    if (scene->cameras) {
        for (size_t i = 0; i < scene->camera_count; i++) {
            if (scene->cameras[i]) {
                free_camera(scene->cameras[i]);
            }
        }
        free(scene->cameras);
        scene->cameras = NULL;
    }

    if (scene->materials) {
        for (size_t i = 0; i < scene->material_count; ++i) {
            if (scene->materials[i]) {
                free_material(scene->materials[i]);
            }
        }
        free(scene->materials);
    }

    // Free the root node and its subtree
    if (scene->root_node) {
        free_node(scene->root_node);
    }

    // Free light cache
    if (scene->light_cache_pairs) {
        free(scene->light_cache_pairs);
    }
    if (scene->light_cache_result) {
        free(scene->light_cache_result);
    }

    // Finally, free the scene itself
    free(scene);

    return;
}

void set_scene_root_node(Scene* scene, SceneNode* root_node) {
    if (!scene)
        return;
    scene->root_node = root_node;
}

/*
 * Cameras
 */

void set_scene_cameras(Scene* scene, Camera** cameras, size_t camera_count) {
    if (!scene)
        return;
    scene->cameras = cameras;
    scene->camera_count = camera_count;
}

void set_scene_lights(Scene* scene, Light** lights, size_t light_count) {
    if (!scene)
        return;
    scene->lights = lights;
    scene->light_count = light_count;
}

Camera* find_camera_by_name(Scene* scene, const char* name) {
    for (size_t i = 0; i < scene->camera_count; ++i) {
        if (strcmp(scene->cameras[i]->name, name) == 0) {
            return scene->cameras[i];
        }
    }
    return NULL;
}

/*
 * Lights
 */
void add_light_to_scene(Scene* scene, Light* light) {
    if (!scene || !light)
        return;

    // Reallocate the lights array to accommodate the new light
    size_t new_count = scene->light_count + 1;
    Light** new_lights = realloc(scene->lights, new_count * sizeof(Light*));
    if (!new_lights) {
        log_error("Failed to reallocate memory for new light");
        free_light(light); // Assuming there's a function to free a Light
        return;
    }

    // Add the new light to the array and update the light count
    scene->lights = new_lights;
    scene->lights[scene->light_count] = light;
    scene->light_count = new_count;

    return;
}

Light* find_light_by_name(Scene* scene, const char* name) {
    for (size_t i = 0; i < scene->light_count; ++i) {
        if (strcmp(scene->lights[i]->name, name) == 0) {
            return scene->lights[i];
        }
    }
    return NULL;
}

static int _compare_light_distance(const void* a, const void* b) {
    LightDistancePair* pair_a = (LightDistancePair*)a;
    LightDistancePair* pair_b = (LightDistancePair*)b;
    return (pair_a->distance > pair_b->distance) - (pair_a->distance < pair_b->distance);
}

static void _collect_scene_lights(Scene* scene, LightDistancePair* pairs, size_t* count,
                                  SceneNode* target_node) {
    vec3 target_pos = {target_node->global_transform[3][0], target_node->global_transform[3][1],
                       target_node->global_transform[3][2]};

    for (size_t i = 0; i < scene->light_count; ++i) {
        vec3 light_pos = {scene->lights[i]->global_position[0],
                          scene->lights[i]->global_position[1],
                          scene->lights[i]->global_position[2]};
        float distance =
            sqrt(pow(light_pos[0] - target_pos[0], 2) + pow(light_pos[1] - target_pos[1], 2) +
                 pow(light_pos[2] - target_pos[2], 2));

        pairs[*count].light = scene->lights[i];
        pairs[*count].distance = distance;
        (*count)++;
    }
}

Light** get_closest_lights(Scene* scene, SceneNode* target_node, size_t max_lights,
                           size_t* returned_light_count) {
    if (!scene || scene->light_count == 0) {
        *returned_light_count = 0;
        return NULL;
    }

    // Grow cache if needed (only reallocates when light count increases)
    if (scene->light_count > scene->light_cache_capacity) {
        free(scene->light_cache_pairs);
        free(scene->light_cache_result);

        scene->light_cache_pairs = malloc(scene->light_count * sizeof(LightDistancePair));
        scene->light_cache_result = malloc(scene->light_count * sizeof(Light*));
        scene->light_cache_capacity = scene->light_count;

        if (!scene->light_cache_pairs || !scene->light_cache_result) {
            free(scene->light_cache_pairs);
            free(scene->light_cache_result);
            scene->light_cache_pairs = NULL;
            scene->light_cache_result = NULL;
            scene->light_cache_capacity = 0;
            *returned_light_count = 0;
            return NULL;
        }
    }

    // Collect and sort lights using cached arrays
    size_t count = 0;
    _collect_scene_lights(scene, scene->light_cache_pairs, &count, target_node);
    qsort(scene->light_cache_pairs, count, sizeof(LightDistancePair), _compare_light_distance);

    size_t result_count = (count < max_lights) ? count : max_lights;
    for (size_t i = 0; i < result_count; i++) {
        scene->light_cache_result[i] = scene->light_cache_pairs[i].light;
    }

    *returned_light_count = result_count;
    return scene->light_cache_result;
}

void add_material_to_scene(Scene* scene, Material* material) {
    if (!scene || !material) {
        log_error("Invalid input to add_material_to_scene");
        return;
    }

    // check if material already added to scene and return if so
    for (size_t i = 0; i < scene->material_count; ++i) {
        if (scene->materials[i] == material) {
            return;
        }
    }

    // Resize the materials array to accommodate the new material
    size_t new_count = scene->material_count + 1;
    Material** new_materials = realloc(scene->materials, new_count * sizeof(Material*));

    if (!new_materials) {
        log_error("Failed to allocate memory for new material");
        return;
    }

    // Add the new material to the array and update the material count
    scene->materials = new_materials;
    scene->materials[scene->material_count] = material;
    scene->material_count = new_count;
}

GLboolean set_scene_xyz_shader_program(Scene* scene, ShaderProgram* xyz_shader_program) {
    if ((scene->xyz_shader_program = xyz_shader_program) == NULL) {
        return GL_FALSE;
    }
    _set_xyz_program_for_nodes(scene->root_node, scene->xyz_shader_program);
    return GL_TRUE;
}

/*
 * Scene Node
 *
 */

SceneNode* create_node() {
    SceneNode* node = malloc(sizeof(SceneNode));
    if (!node) {
        log_error("Failed to allocate memory for scene node");
        return NULL;
    }

    node->name = NULL;
    node->parent = NULL;
    node->children = NULL;
    node->children_count = 0;
    glm_mat4_identity(node->original_transform);
    glm_mat4_identity(node->global_transform);

    node->meshes = NULL;
    node->mesh_count = 0;
    node->light = NULL;
    node->camera = NULL;

    // xyz
    node->show_xyz = true;
    glGenVertexArrays(1, &node->xyz_vao);
    glGenBuffers(1, &node->xyz_vbo);
    node->xyz_shader_program = NULL;

    return node;
}

void free_node(SceneNode* node) {
    if (!node)
        return;

    for (size_t i = 0; i < node->children_count; i++) {
        free_node(node->children[i]);
    }

    free(node->children);

    for (size_t i = 0; i < node->mesh_count; i++) {
        if (node->meshes[i]) {
            free_mesh(node->meshes[i]);
        }
    }
    free(node->meshes);

    if (node->name) {
        free(node->name);
    }

    // Note: Do not free camera and light, as they are managed by the Scene.
    // Similarly, shaders and programs are usually shared and should be managed
    // separately.

    free(node);
}

void add_child_node(SceneNode* node, SceneNode* child) {
    if (!node || !child)
        return;
    node->children = realloc(node->children, (node->children_count + 1) * sizeof(SceneNode*));
    node->children[node->children_count] = child;
    child->parent = node;
    node->children_count++;
}

void add_mesh_to_node(SceneNode* node, Mesh* mesh) {
    if (!node || !mesh)
        return;

    // Reallocate the meshes array to accommodate the new mesh
    size_t new_count = node->mesh_count + 1;
    Mesh** new_meshes = realloc(node->meshes, new_count * sizeof(Mesh*));
    if (!new_meshes) {
        log_error("Failed to reallocate memory for new mesh");
        // Handle error, such as freeing the mesh if it's dynamically allocated
        return;
    }

    // Add the new mesh to the array and update the mesh count
    node->meshes = new_meshes;
    node->meshes[node->mesh_count] = mesh;
    node->mesh_count = new_count;
}

void set_node_name(SceneNode* node, const char* name) {
    if (!node || !name)
        return;
    node->name = safe_strdup(name);
}

void set_node_light(SceneNode* node, Light* light) {
    if (!node)
        return;
    node->light = light;
}

void set_node_camera(SceneNode* node, Camera* camera) {
    if (!node)
        return;
    node->camera = camera;
}

void set_shader_program_for_nodes(SceneNode* node, ShaderProgram* program) {
    if (!node) {
        return;
    }

    for (size_t i = 0; i < node->mesh_count; ++i) {
        Mesh* mesh = node->meshes[i];

        if (mesh && mesh->material) {
            mesh->material->shader_program = program;
        }
    }

    for (size_t i = 0; i < node->children_count; ++i) {
        set_shader_program_for_nodes(node->children[i], program);
    }
}

static void _set_xyz_program_for_nodes(SceneNode* node, ShaderProgram* program) {
    if (!node) {
        return;
    }

    node->xyz_shader_program = program;

    for (size_t i = 0; i < node->children_count; ++i) {
        _set_xyz_program_for_nodes(node->children[i], program);
    }
}

void set_show_xyz_for_nodes(SceneNode* node, bool show_xyz) {
    if (!node)
        return;

    node->show_xyz = show_xyz;

    for (size_t i = 0; i < node->children_count; ++i) {
        set_show_xyz_for_nodes(node->children[i], show_xyz);
    }
}

static void _upload_xyz_buffers_to_gpu_for_node(SceneNode* node) {
    // Bind the Vertex Array Object (VAO)
    glBindVertexArray(node->xyz_vao);

    // Bind and set up the Vertex Buffer Object (VBO)
    glBindBuffer(GL_ARRAY_BUFFER, node->xyz_vbo);
    glBufferData(GL_ARRAY_BUFFER, xyz_vertices_size, xyz_vertices, GL_STATIC_DRAW);

    // Position attribute
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Color attribute
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // only validate if VAO is bound
    if (node->xyz_shader_program && !validate_program(node->xyz_shader_program)) {
        log_error("xyz shader program validation failed");
    }

    glBindVertexArray(0);
}

void upload_buffers_to_gpu_for_nodes(SceneNode* node) {
    if (!node)
        return;

    /*
     * Setup and upload mesh buffers.
     */
    for (size_t i = 0; i < node->mesh_count; i++) {
        if (node->meshes[i]) {
            upload_mesh_buffers_to_gpu(node->meshes[i]);
        }
    }

    _upload_xyz_buffers_to_gpu_for_node(node);

    for (size_t i = 0; i < node->children_count; i++) {
        upload_buffers_to_gpu_for_nodes(node->children[i]);
    }
}

void apply_transform_to_nodes(SceneNode* node, mat4 transform) {
    if (!node)
        return;

    glm_mat4_mul(transform, node->original_transform, node->global_transform);

    if (node->light) {
        vec3 light_position;
        glm_mat4_mulv3(node->global_transform, node->light->original_position, 1.0f,
                       light_position);
        glm_vec3_copy(light_position, node->light->global_position);
    }

    for (size_t i = 0; i < node->children_count; i++) {
        if (node->children[i]) {
            apply_transform_to_nodes(node->children[i], node->global_transform);
        }
    }
}

void print_scene_node(const SceneNode* node, int depth) {
    if (!node)
        return;

    print_indentation(depth);
    printf("Node: %s | Children: %zu | Meshes: %zu | Light: %s | Camera: %s\n",
           node->name ? node->name : "Unnamed", node->children_count, node->mesh_count,
           node->light ? (node->light->name ? node->light->name : "Unnamed Light") : "None",
           node->camera ? (node->camera->name ? node->camera->name : "Unnamed Camera") : "None");

    for (size_t i = 0; i < node->children_count; i++) {
        print_scene_node(node->children[i], depth + 1);
    }
}

void print_scene_lights(const Scene* scene) {
    if (!scene)
        return;
    for (size_t i = 0; i < scene->light_count; i++) {
        print_light(scene->lights[i]);
    }
}

void print_scene(const Scene* scene) {
    if (!scene)
        return;

    printf("Scene | Lights: %zu | Cameras: %zu | Textures: '%s'\n", scene->light_count,
           scene->camera_count,
           scene->tex_pool ? (scene->tex_pool->directory ? scene->tex_pool->directory : "None")
                           : "None");

    print_scene_lights(scene);
    print_scene_node(scene->root_node, 0);
}
