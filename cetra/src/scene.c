
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

    // Pre-allocate traversal stack (avoids per-frame malloc)
    scene->traversal_stack_capacity = 64;
    scene->traversal_stack = malloc(scene->traversal_stack_capacity * sizeof(SceneNode*));
    scene->traversal_transforms = malloc(scene->traversal_stack_capacity * sizeof(mat4));
    if (!scene->traversal_stack || !scene->traversal_transforms) {
        log_error("Failed to allocate traversal stack");
        free(scene->traversal_stack);
        free(scene->traversal_transforms);
        scene->traversal_stack = NULL;
        scene->traversal_transforms = NULL;
        scene->traversal_stack_capacity = 0;
    }

    // Initialize shadow system
    scene->shadow_system = create_shadow_system(DEFAULT_SHADOW_MAP_SIZE);

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

    // Free traversal stack
    if (scene->traversal_stack) {
        free(scene->traversal_stack);
    }
    if (scene->traversal_transforms) {
        free(scene->traversal_transforms);
    }

    // Free shadow system
    if (scene->shadow_system) {
        free_shadow_system(scene->shadow_system);
        scene->shadow_system = NULL;
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

int add_camera_to_scene(Scene* scene, Camera* camera) {
    if (!scene || !camera)
        return -1;

    size_t new_count = scene->camera_count + 1;
    Camera** new_cameras = realloc(scene->cameras, new_count * sizeof(Camera*));
    if (!new_cameras) {
        log_error("Failed to reallocate memory for new camera");
        return -1;
    }

    scene->cameras = new_cameras;
    scene->cameras[scene->camera_count] = camera;
    scene->camera_count = new_count;
    return 0;
}

Camera* find_camera_by_name(Scene* scene, const char* name) {
    if (!scene || !name)
        return NULL;

    for (size_t i = 0; i < scene->camera_count; ++i) {
        Camera* cam = scene->cameras[i];
        if (cam && cam->name && strcmp(cam->name, name) == 0) {
            return cam;
        }
    }
    return NULL;
}

/*
 * Lights
 */
int add_light_to_scene(Scene* scene, Light* light) {
    if (!scene || !light)
        return -1;

    size_t new_count = scene->light_count + 1;
    Light** new_lights = realloc(scene->lights, new_count * sizeof(Light*));
    if (!new_lights) {
        log_error("Failed to reallocate memory for new light");
        return -1;
    }

    scene->lights = new_lights;
    scene->lights[scene->light_count] = light;
    scene->light_count = new_count;
    return 0;
}

Light* find_light_by_name(Scene* scene, const char* name) {
    if (!scene || !name)
        return NULL;

    for (size_t i = 0; i < scene->light_count; ++i) {
        Light* light = scene->lights[i];
        if (light && light->name && strcmp(light->name, name) == 0) {
            return light;
        }
    }
    return NULL;
}

// Max-heap helpers for O(n log k) light selection
// Heap is ordered by distance with maximum at root (index 0)
static void _heap_sift_up(LightDistancePair* heap, size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
        if (heap[idx].distance > heap[parent].distance) {
            LightDistancePair tmp = heap[idx];
            heap[idx] = heap[parent];
            heap[parent] = tmp;
            idx = parent;
        } else {
            break;
        }
    }
}

static void _heap_sift_down(LightDistancePair* heap, size_t size) {
    size_t idx = 0;
    while (1) {
        size_t left = 2 * idx + 1;
        size_t right = 2 * idx + 2;
        size_t largest = idx;

        if (left < size && heap[left].distance > heap[largest].distance) {
            largest = left;
        }
        if (right < size && heap[right].distance > heap[largest].distance) {
            largest = right;
        }

        if (largest != idx) {
            LightDistancePair tmp = heap[idx];
            heap[idx] = heap[largest];
            heap[largest] = tmp;
            idx = largest;
        } else {
            break;
        }
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

    // Extract target position from node's global transform
    vec3 target_pos;
    glm_vec3_copy((vec3){target_node->global_transform[3][0], target_node->global_transform[3][1],
                         target_node->global_transform[3][2]},
                  target_pos);

    // Use max-heap for O(n log k) selection of k closest lights
    // Heap stored in light_cache_pairs[0..heap_size-1], max distance at root
    size_t heap_size = 0;

    for (size_t i = 0; i < scene->light_count; i++) {
        float distance = glm_vec3_distance(scene->lights[i]->global_position, target_pos);

        if (heap_size < max_lights) {
            // Heap not full, add this light
            scene->light_cache_pairs[heap_size].light = scene->lights[i];
            scene->light_cache_pairs[heap_size].distance = distance;
            _heap_sift_up(scene->light_cache_pairs, heap_size);
            heap_size++;
        } else if (distance < scene->light_cache_pairs[0].distance) {
            // New light is closer than the farthest in heap, replace root
            scene->light_cache_pairs[0].light = scene->lights[i];
            scene->light_cache_pairs[0].distance = distance;
            _heap_sift_down(scene->light_cache_pairs, heap_size);
        }
    }

    // Copy heap contents to result array
    for (size_t i = 0; i < heap_size; i++) {
        scene->light_cache_result[i] = scene->light_cache_pairs[i].light;
    }

    *returned_light_count = heap_size;
    return scene->light_cache_result;
}

int add_material_to_scene(Scene* scene, Material* material) {
    if (!scene || !material)
        return -1;

    for (size_t i = 0; i < scene->material_count; ++i) {
        if (scene->materials[i] == material)
            return 0;
    }

    size_t new_count = scene->material_count + 1;
    Material** new_materials = realloc(scene->materials, new_count * sizeof(Material*));
    if (!new_materials) {
        log_error("Failed to allocate memory for new material");
        return -1;
    }

    scene->materials = new_materials;
    scene->materials[scene->material_count] = material;
    scene->material_count = new_count;
    return 0;
}

GLboolean set_scene_xyz_shader_program(Scene* scene, ShaderProgram* xyz_shader_program) {
    if (!scene || !xyz_shader_program) {
        return GL_FALSE;
    }
    scene->xyz_shader_program = xyz_shader_program;
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

int add_child_node(SceneNode* node, SceneNode* child) {
    if (!node || !child)
        return -1;

    SceneNode** new_children =
        realloc(node->children, (node->children_count + 1) * sizeof(SceneNode*));
    if (!new_children) {
        log_error("Failed to reallocate memory for child node");
        return -1;
    }

    node->children = new_children;
    node->children[node->children_count] = child;
    child->parent = node;
    node->children_count++;
    return 0;
}

int add_mesh_to_node(SceneNode* node, Mesh* mesh) {
    if (!node || !mesh)
        return -1;

    size_t new_count = node->mesh_count + 1;
    Mesh** new_meshes = realloc(node->meshes, new_count * sizeof(Mesh*));
    if (!new_meshes) {
        log_error("Failed to reallocate memory for new mesh");
        return -1;
    }

    node->meshes = new_meshes;
    node->meshes[node->mesh_count] = mesh;
    node->mesh_count = new_count;
    return 0;
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

    // Clear any stale GL errors before validation
    while (glGetError() != GL_NO_ERROR) {
    }

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

typedef struct {
    SceneNode* node;
    mat4 parent_transform;
} TransformStackEntry;

void apply_transform_to_nodes(SceneNode* root, mat4 transform) {
    if (!root)
        return;

    // Iterative traversal using explicit stack
    size_t stack_capacity = 64;
    size_t stack_size = 0;
    TransformStackEntry* stack = malloc(stack_capacity * sizeof(TransformStackEntry));
    if (!stack) {
        log_error("Failed to allocate transform stack");
        return;
    }

    // Push root node
    stack[stack_size].node = root;
    glm_mat4_copy(transform, stack[stack_size].parent_transform);
    stack_size++;

    while (stack_size > 0) {
        // Pop from stack
        stack_size--;
        SceneNode* node = stack[stack_size].node;
        mat4 parent_transform;
        glm_mat4_copy(stack[stack_size].parent_transform, parent_transform);

        // Apply transform
        glm_mat4_mul(parent_transform, node->original_transform, node->global_transform);

        // Update light position if present
        if (node->light) {
            vec3 light_position;
            glm_mat4_mulv3(node->global_transform, node->light->original_position, 1.0f,
                           light_position);
            glm_vec3_copy(light_position, node->light->global_position);
        }

        // Push children (in reverse order to maintain left-to-right traversal)
        for (size_t i = node->children_count; i > 0; i--) {
            if (node->children[i - 1]) {
                // Grow stack if needed
                if (stack_size >= stack_capacity) {
                    stack_capacity *= 2;
                    TransformStackEntry* new_stack =
                        realloc(stack, stack_capacity * sizeof(TransformStackEntry));
                    if (!new_stack) {
                        log_error("Failed to grow transform stack");
                        free(stack);
                        return;
                    }
                    stack = new_stack;
                }

                stack[stack_size].node = node->children[i - 1];
                glm_mat4_copy(node->global_transform, stack[stack_size].parent_transform);
                stack_size++;
            }
        }
    }

    free(stack);
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
