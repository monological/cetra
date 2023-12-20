
#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

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
static void _set_axes_program_for_nodes(SceneNode* node, ShaderProgram* program);
static void _set_light_outlines_program_for_nodes(SceneNode* node, ShaderProgram* program);



const char* axes_vert_src =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec3 aColor;\n"
    "out vec3 vertexColor;\n"
    "uniform mat4 model;\n"
    "uniform mat4 view;\n"
    "uniform mat4 projection;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = projection * view * model * vec4(aPos, 1.0);\n"
    "    vertexColor = aColor;\n"
    "}";

const char* axes_frag_src =
    "#version 330 core\n"
    "in vec3 vertexColor;\n"
    "out vec4 FragColor;\n"
    "uniform mat4 view;\n"
    "uniform mat4 model;\n"
    "uniform mat4 projection;\n"
    "void main()\n"
    "{\n"
    "    FragColor = vec4(vertexColor, 1.0);\n"
    "}";


Scene* create_scene() {
    Scene* scene = malloc(sizeof(Scene));
    if (!scene) {
        fprintf(stderr, "Failed to allocate memory for Scene\n");
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

    scene->axes_shader_program = NULL;
    scene->light_outlines_shader_program = NULL;

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

    // Free the root node and its subtree
    if (scene->root_node) {
        free_node(scene->root_node);
    }

    if(scene->axes_shader_program){
        free_program(scene->axes_shader_program);
    }

    if(scene->light_outlines_shader_program){
        free_program(scene->light_outlines_shader_program);
    }

    // Finally, free the scene itnode
    free(scene);

    return;
}

void set_scene_root_node(Scene* scene, SceneNode* root_node) {
    if (!scene) return;
    scene->root_node = root_node;
}

/*
 * Cameras
 */

void set_scene_cameras(Scene* scene, Camera** cameras, size_t camera_count) {
    if (!scene) return;
    scene->cameras = cameras;
    scene->camera_count = camera_count;
}

void set_scene_lights(Scene* scene, Light** lights, size_t light_count) {
    if (!scene) return;
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
    if (!scene || ! light) return;

    // Reallocate the lights array to accommodate the new light
    size_t new_count = scene->light_count + 1;
    Light** new_lights = realloc(scene->lights, new_count * sizeof(Light*));
    if (!new_lights) {
        fprintf(stderr, "Failed to reallocate memory for new light\n");
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

static void _collect_scene_lights(Scene* scene, LightDistancePair* pairs, size_t* count, SceneNode* target_node) {
    vec3 target_pos = {
        target_node->global_transform[3][0],
        target_node->global_transform[3][1],
        target_node->global_transform[3][2]
    };

    for (size_t i = 0; i < scene->light_count; ++i) {
        vec3 light_pos = {
            scene->lights[i]->global_position[0],
            scene->lights[i]->global_position[1],
            scene->lights[i]->global_position[2]
        };
        float distance = sqrt(pow(light_pos[0] - target_pos[0], 2) 
            + pow(light_pos[1] - target_pos[1], 2) 
            + pow(light_pos[2] - target_pos[2], 2));

        pairs[*count].light = scene->lights[i];
        pairs[*count].distance = distance;
        (*count)++;
    }
}

Light** get_closest_lights(Scene* scene, SceneNode* target_node, 
        size_t max_lights, size_t* returned_light_count) {

    LightDistancePair* pairs = malloc(scene->light_count * sizeof(LightDistancePair));
    if (!pairs) {
        // Handle memory allocation failure
        *returned_light_count = 0;
        return NULL;
    }

    size_t count = 0;
    _collect_scene_lights(scene, pairs, &count, target_node);
    qsort(pairs, count, sizeof(LightDistancePair), _compare_light_distance);

    size_t result_count = (count < max_lights) ? count : max_lights;
    Light** closest_lights = malloc(result_count * sizeof(Light*));
    if (!closest_lights) {
        free(pairs);
        *returned_light_count = 0;
        return NULL;
    }

    for (size_t i = 0; i < result_count; i++) {
        closest_lights[i] = pairs[i].light;
    }

    free(pairs);
    *returned_light_count = result_count;
    return closest_lights;
}

GLboolean setup_scene_axes(Scene* scene) {
    if (!setup_program_shader_from_source(&scene->axes_shader_program, 
            axes_vert_src, axes_frag_src, NULL)) {
        fprintf(stderr, "Failed to set up scene axes program");
        return GL_FALSE;
    }
    _set_axes_program_for_nodes(scene->root_node, scene->axes_shader_program);
    return GL_TRUE;
}

GLboolean setup_scene_light_outlines(Scene* scene) {
    if (!setup_program_shader_from_paths(&scene->light_outlines_shader_program,
            OUTLINES_VERT_SHADER_PATH, OUTLINES_FRAG_SHADER_PATH, OUTLINES_GEO_SHADER_PATH)) {
        fprintf(stderr, "Failed to set up scene light program");
        return GL_FALSE;
    }
    _set_light_outlines_program_for_nodes(scene->root_node, scene->light_outlines_shader_program);
    return GL_TRUE;
}

/*
 * Scene Node
 *
 */

SceneNode* create_node() {
    SceneNode* node = malloc(sizeof(SceneNode));
    if (!node) {
        fprintf(stderr, "Failed to allocate memory for scene node\n");
        return NULL;
    }
    
    node->name = NULL;
    node->parent = NULL;
    node->children = NULL;
    node->children_count = 0;
    glm_mat4_identity(node->original_transform); 
    glm_mat4_identity(node->local_transform); 
    glm_mat4_identity(node->global_transform); 

    node->meshes = NULL;
    node->mesh_count = 0;
    node->light = NULL;
    node->camera = NULL;
    node->shader_program = NULL;

    // axes
    node->show_axes = true;
    glGenVertexArrays(1, &node->axes_vao);
    check_gl_error("create node axes gen vertex failed");
    glGenBuffers(1, &node->axes_vbo);
    check_gl_error("create node axes gen buffers failed");
    node->axes_shader_program = NULL;

    // light outlines
    node->show_light_outlines = true;
    glGenVertexArrays(1, &node->light_outlines_vao);
    check_gl_error("create node outlines gen vertex failed");
    glGenBuffers(1, &node->light_outlines_vbo);
    check_gl_error("create node outlines gen buffers failed");
    node->light_outlines_shader_program = NULL;

    return node;
}

void free_node(SceneNode* node) {
    if (!node) return;

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
    // Similarly, shaders and programs are usually shared and should be managed separately.

    free(node);
}

void add_child_node(SceneNode* node, SceneNode* child) {
    if (!node || !child) return;
    node->children = realloc(node->children, (node->children_count + 1) * sizeof(SceneNode*));
    node->children[node->children_count] = child;
    child->parent = node;
    node->children_count++;
}


void add_mesh_to_node(SceneNode* node, Mesh* mesh) {
    if (!node || !mesh) return;

    // Reallocate the meshes array to accommodate the new mesh
    size_t new_count = node->mesh_count + 1;
    Mesh** new_meshes = realloc(node->meshes, new_count * sizeof(Mesh*));
    if (!new_meshes) {
        fprintf(stderr, "Failed to reallocate memory for new mesh\n");
        // Handle error, such as freeing the mesh if it's dynamically allocated
        return;
    }

    // Add the new mesh to the array and update the mesh count
    node->meshes = new_meshes;
    node->meshes[node->mesh_count] = mesh;
    node->mesh_count = new_count;
}

void set_node_name(SceneNode* node, const char* name) {
    if (!node || !name) return;
    node->name = safe_strdup(name);
}

void set_node_light(SceneNode* node, Light* light) {
    if (!node) return;
    node->light = light;
}

void set_node_camera(SceneNode* node, Camera* camera) {
    if (!node) return;
    node->camera = camera;
}

void set_program_for_nodes(SceneNode* node, ShaderProgram* program) {
    if (!node) {
        return;
    }

    node->shader_program = program;

    for (size_t i = 0; i < node->children_count; ++i) {
        set_program_for_nodes(node->children[i], program);
    }
}

static void _set_axes_program_for_nodes(SceneNode* node, ShaderProgram* program) {
    if (!node) {
        return;
    }

    node->axes_shader_program = program;

    for (size_t i = 0; i < node->children_count; ++i) {
        _set_axes_program_for_nodes(node->children[i], program);
    }
}

void set_show_axes_for_nodes(SceneNode* node, bool show_axes){
    if (!node) return;
    
    node->show_axes = show_axes;

    for (size_t i = 0; i < node->children_count; ++i) {
        set_show_axes_for_nodes(node->children[i], show_axes);
    }
}

static void _set_light_outlines_program_for_nodes(SceneNode* node, ShaderProgram* program) {
    if (!node) {
        return;
    }

    node->light_outlines_shader_program = program;

    for (size_t i = 0; i < node->children_count; ++i) {
        _set_light_outlines_program_for_nodes(node->children[i], program);
    }
}

void set_show_light_outlines_for_nodes(SceneNode* node, bool show_light_outlines){
    if (!node) return;
    
    node->show_light_outlines = show_light_outlines;

    for (size_t i = 0; i < node->children_count; ++i) {
        set_show_light_outlines_for_nodes(node->children[i], show_light_outlines);
    }
}

static void _upload_axes_buffers_to_gpu_for_node(SceneNode* node){
    // Bind the Vertex Array Object (VAO)
    glBindVertexArray(node->axes_vao);

    // Bind and set up the Vertex Buffer Object (VBO)
    glBindBuffer(GL_ARRAY_BUFFER, node->axes_vbo);
    glBufferData(GL_ARRAY_BUFFER, axes_vertices_size, axes_vertices, GL_STATIC_DRAW);

    // Position attribute
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Color attribute
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // only validate if VAO is bound
    if(node->axes_shader_program && !validate_program(node->axes_shader_program)){
        fprintf(stderr, "Axes shader program validation failed\n");
    }

    glBindVertexArray(0);
}

static void _upload_light_outline_buffers_to_gpu_for_node(SceneNode* node, bool val_prog){
    if(!node || !node->light) return;

    size_t buffer_size = sizeof(vec3) * 2;
    vec3* buffer_data = (vec3*)malloc(buffer_size);
    if (!buffer_data) {
        return;
    }

    glm_vec3_copy(node->light->global_position, buffer_data[0]);
    glm_vec3_copy(node->light->color, buffer_data[1]);


    glBindVertexArray(node->light_outlines_vao);
    glBindBuffer(GL_ARRAY_BUFFER, node->light_outlines_vbo);


    glBufferData(GL_ARRAY_BUFFER, buffer_size, buffer_data, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // only validate if VAO is bound
    if(val_prog && node->light_outlines_shader_program && !validate_program(node->light_outlines_shader_program)){
        fprintf(stderr, "Light outline shader program validation failed\n");
    }

    glBindVertexArray(0);

    free(buffer_data);
}

void upload_buffers_to_gpu_for_nodes(SceneNode* node) {
    if (!node) return;

    /*
     * Setup and upload mesh buffers.
     */
    for (size_t i = 0; i < node->mesh_count; i++) {
        if (node->meshes[i]) {
            upload_mesh_buffers_to_gpu(node->meshes[i]);
        }
    }
    
    _upload_axes_buffers_to_gpu_for_node(node);

    if(node->light){
        _upload_light_outline_buffers_to_gpu_for_node(node, true);
    }

    for (size_t i = 0; i < node->children_count; i++) {
        upload_buffers_to_gpu_for_nodes(node->children[i]);
    }
}

void transform_node(SceneNode* node, Transform* transform, mat4* result_matrix) {
    if (!node || !transform || !result_matrix) {
        glm_mat4_identity(*result_matrix);
        return;
    }

    glm_mat4_identity(*result_matrix);
    glm_translate(*result_matrix, transform->position);
    glm_rotate(*result_matrix, transform->rotation[0], (vec3){1.0f, 0.0f, 0.0f});
    glm_rotate(*result_matrix, transform->rotation[1], (vec3){0.0f, 1.0f, 0.0f});
    glm_rotate(*result_matrix, transform->rotation[2], (vec3){0.0f, 0.0f, 1.0f});
    glm_scale(*result_matrix, transform->scale);

    glm_mat4_mul(node->original_transform, *result_matrix, node->local_transform);
}


void apply_transform_to_nodes(SceneNode* node, mat4 transform) {
    if (!node) return;

    mat4 localTransform;
    glm_mat4_identity(localTransform);

    glm_mat4_mul(node->original_transform, node->local_transform, localTransform);
    glm_mat4_mul(transform, localTransform, node->global_transform);

    if (node->light) {
        vec3 light_position;
        glm_mat4_mulv3(node->global_transform, node->light->original_position, 1.0f, light_position);
        glm_vec3_copy(light_position, node->light->global_position);

        _upload_light_outline_buffers_to_gpu_for_node(node, false);
    }

    for (size_t i = 0; i < node->children_count; i++) {
        if (node->children[i]) {
            apply_transform_to_nodes(node->children[i], node->global_transform);
        }
    }
}


void print_scene_node(const SceneNode* node, int depth) {
    if (!node) return;

    print_indentation(depth);
    printf("Node: %s | Children: %zu | Meshes: %zu | Light: %s | Camera: %s\n",
           node->name ? node->name : "Unnamed",
           node->children_count,
           node->mesh_count,
           node->light ? (node->light->name ? node->light->name : "Unnamed Light") : "None",
           node->camera ? (node->camera->name ? node->camera->name : "Unnamed Camera") : "None");

    for (size_t i = 0; i < node->children_count; i++) {
        print_scene_node(node->children[i], depth + 1);
    }
}

void print_scene_lights(const Scene* scene) {
    if (!scene) return;
    for (size_t i = 0; i < scene->light_count; i++) {
        print_light(scene->lights[i]);
    }
}

void print_scene(const Scene* scene) {
    if (!scene) return;

    printf("Scene | Lights: %zu | Cameras: %zu | Texture Directory: %s\n",
           scene->light_count,
           scene->camera_count,
           scene->tex_pool ? (scene->tex_pool->directory ? scene->tex_pool->directory : "None") : "None");

    print_scene_lights(scene);
    print_scene_node(scene->root_node, 0);
}




