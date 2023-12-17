
#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "scene.h"
#include "program.h"
#include "shader.h"
#include "mesh.h"
#include "light.h"
#include "camera.h"
#include "common.h"
#include "util.h"

/*
 * prototypes
 */
static void _set_axes_program_for_nodes(SceneNode* node, ShaderProgram* program);
static void _set_light_outlines_program_for_nodes(SceneNode* node, ShaderProgram* program);

// Axis vertices: 6 vertices, 2 for each line (origin and end)
float axesVertices[] = {
    // X-axis (Red)
    0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, // Origin to Red
    1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, // Red end
    // Y-axis (Green)
    0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, // Origin to Green
    0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, // Green end
    // Z-axis (Blue)
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, // Origin to Blue
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f  // Blue end
};

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
    glBufferData(GL_ARRAY_BUFFER, sizeof(axesVertices), axesVertices, GL_STATIC_DRAW);

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


    // Print the position and color
    //printf("Light Position: (%f, %f, %f)\n", buffer_data[0][0], buffer_data[0][1], buffer_data[0][2]);
    //printf("Light Color: (%f, %f, %f)\n\n\n", buffer_data[1][0], buffer_data[1][1], buffer_data[1][2]);


    /*for (size_t i = 0; i < 2; i++) {
        for (size_t j = 0; j < 3; j++) {
           printf("%f\n", buffer_data[i][j]);
        }
    }*/

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
        glm_mat4_identity(*result_matrix); // Reset to identity if inputs are invalid
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

        //print_light(node->light);
        _upload_light_outline_buffers_to_gpu_for_node(node, false);
        
    }

    for (size_t i = 0; i < node->children_count; i++) {
        if (node->children[i]) {
            apply_transform_to_nodes(node->children[i], node->global_transform);
        }
    }
}

void render_nodes(Scene* scene, SceneNode* node, Camera *camera, 
        mat4 model, mat4 view, mat4 projection, 
        float time_value, RenderMode render_mode) {
    if (!node) {
        printf("error: render_node called with NULL node\n");
        return;
    }

    size_t returned_light_count;
    size_t max_lights = calculate_max_lights();
    Light** closest_lights = get_closest_lights(scene, node, max_lights, 
            &returned_light_count);


    if (node->shader_program) {

        ShaderProgram* program = node->shader_program;

        // Use the shader program
        glUseProgram(program->id);

        for (size_t i = 0; i < returned_light_count; ++i) {

            Light* closest_light = closest_lights[i];

            if(!closest_light){
                continue;
            }

            if (program->light_position_loc[i] != -1) {
                glUniform3fv(program->light_position_loc[i], 1, (const GLfloat*)&closest_light->global_position);
            }
            if (program->light_direction_loc[i] != -1) {
                glUniform3fv(program->light_direction_loc[i], 1, (const GLfloat*)&closest_light->direction);
            }
            if (program->light_color_loc[i] != -1) {
                glUniform3fv(program->light_color_loc[i], 1, (const GLfloat*)&closest_light->color);
            }
            if (program->light_specular_loc[i] != -1) {
                glUniform3fv(program->light_specular_loc[i], 1, (const GLfloat*)&closest_light->specular);
            }
            if (program->light_ambient_loc[i] != -1) {
                glUniform3fv(program->light_ambient_loc[i], 1, (const GLfloat*)&closest_light->ambient);
            }
            if (program->light_intensity_loc[i] != -1) {
                glUniform1f(program->light_intensity_loc[i], closest_light->intensity);
            }
            if (program->light_constant_loc[i] != -1) {
                glUniform1f(program->light_constant_loc[i], closest_light->constant);
            }
            if (program->light_linear_loc[i] != -1) {
                glUniform1f(program->light_linear_loc[i], closest_light->linear);
            }
            if (program->light_quadratic_loc[i] != -1) {
                glUniform1f(program->light_quadratic_loc[i], closest_light->quadratic);
            }
            if (program->light_cutOff_loc[i] != -1) {
                glUniform1f(program->light_cutOff_loc[i], closest_light->cutOff);
            }
            if (program->light_outerCutOff_loc[i] != -1) {
                glUniform1f(program->light_outerCutOff_loc[i], closest_light->outerCutOff);
            }
            if (program->light_type_loc[i] != -1) {
                glUniform1i(program->light_type_loc[i], closest_light->type);
            }

            if (program->light_size_loc[i] != -1) {
                glUniform2f(program->light_size_loc[i], closest_light->size[0], closest_light->size[1]);
            }

            glUniform1i(program->num_lights_loc, (GLint)returned_light_count);
        }


        // PBR, NORMALS, etc...
        glUniform1i(program->render_mode_loc, render_mode);
        glUniform1f(program->near_clip_loc, camera->near_clip);
        glUniform1f(program->far_clip_loc, camera->far_clip);


        // Set shader uniforms for model, view, and projection matrices
        glUniformMatrix4fv(program->model_loc, 1, GL_FALSE, (const GLfloat*)node->global_transform);
        glUniformMatrix4fv(program->view_loc, 1, GL_FALSE, (const GLfloat*)view);
        glUniformMatrix4fv(program->proj_loc, 1, GL_FALSE, (const GLfloat*)projection);

        // cam position and time
        glUniform3fv(program->cam_pos_loc, 1, (const GLfloat*)&camera->position);
        glUniform1f(program->time_loc, time_value);

        // Set material properties if available
        if (node->meshes && node->mesh_count > 0) {
            for (size_t i = 0; i < node->mesh_count; ++i) {
                Mesh* mesh = node->meshes[i];
                if (mesh && mesh->material) {
                    Material* mat = mesh->material;
                    
                    // Set albedo, metallic, roughness, and ambient occlusion
                    glUniform3fv(program->albedo_loc, 1, (const GLfloat*)&mat->albedo);
                    glUniform1f(program->metallic_loc, mat->metallic);
                    glUniform1f(program->roughness_loc, mat->roughness);
                    glUniform1f(program->ao_loc, mat->ao);

                    if (mat->albedoTex) {
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, mat->albedoTex->id);
                        glUniform1i(program->albedo_tex_loc, 0);
                    }

                    if (mat->normalTex) {
                        glActiveTexture(GL_TEXTURE1);
                        glBindTexture(GL_TEXTURE_2D, mat->normalTex->id);
                        glUniform1i(program->normal_tex_loc, 1);
                    }

                    if (mat->roughnessTex) {
                        glActiveTexture(GL_TEXTURE2);
                        glBindTexture(GL_TEXTURE_2D, mat->roughnessTex->id);
                        glUniform1i(program->roughness_tex_loc, 2);
                    }

                    if (mat->metalnessTex) {
                        glActiveTexture(GL_TEXTURE3);
                        glBindTexture(GL_TEXTURE_2D, mat->metalnessTex->id);
                        glUniform1i(program->metalness_tex_loc, 3);
                    }

                    if (mat->ambientOcclusionTex) {
                        glActiveTexture(GL_TEXTURE4);
                        glBindTexture(GL_TEXTURE_2D, mat->ambientOcclusionTex->id);
                        glUniform1i(program->ao_tex_loc, 4);
                    }

                    if (mat->emissiveTex) {
                        glActiveTexture(GL_TEXTURE5);
                        glBindTexture(GL_TEXTURE_2D, mat->emissiveTex->id);
                        glUniform1i(program->emissive_tex_loc, 5);
                    }

                    if (mat->heightTex) {
                        glActiveTexture(GL_TEXTURE6);
                        glBindTexture(GL_TEXTURE_2D, mat->heightTex->id);
                        glUniform1i(program->height_tex_loc, 6);
                    }

                    if (mat->opacityTex) {
                        glActiveTexture(GL_TEXTURE7);
                        glBindTexture(GL_TEXTURE_2D, mat->opacityTex->id);
                        glUniform1i(program->opacity_tex_loc, 7);
                    }

                    if (mat->sheenTex) {
                        glActiveTexture(GL_TEXTURE8);
                        glBindTexture(GL_TEXTURE_2D, mat->sheenTex->id);
                        glUniform1i(program->sheen_tex_loc, 8);
                    }

                    if (mat->reflectanceTex) {
                        glActiveTexture(GL_TEXTURE9);
                        glBindTexture(GL_TEXTURE_2D, mat->reflectanceTex->id);
                        glUniform1i(program->reflectance_tex_loc, 9);
                    }

                    // Set uniform values for each texture type
                    glUniform1i(program->albedo_tex_exists_loc, mat->albedoTex ? 1 : 0);
                    glUniform1i(program->normal_tex_exists_loc, mat->normalTex ? 1 : 0);
                    glUniform1i(program->roughness_tex_exists_loc, mat->roughnessTex ? 1 : 0);
                    glUniform1i(program->metalness_tex_exists_loc, mat->metalnessTex ? 1 : 0);
                    glUniform1i(program->ao_tex_exists_loc, mat->ambientOcclusionTex ? 1 : 0);
                    glUniform1i(program->emissive_tex_exists_loc, mat->emissiveTex ? 1 : 0);
                    glUniform1i(program->height_tex_exists_loc, mat->heightTex ? 1 : 0);
                    glUniform1i(program->opacity_tex_exists_loc, mat->opacityTex ? 1 : 0);
                    glUniform1i(program->sheen_tex_exists_loc, mat->sheenTex ? 1 : 0);
                    glUniform1i(program->reflectance_tex_exists_loc, mat->reflectanceTex ? 1 : 0);

                }
                // Bind VAO and draw the mesh
                glBindVertexArray(mesh->VAO);
                glDrawElements(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, 0);
            }
        }

        // Optional cleanup
        glBindVertexArray(0);
        glUseProgram(0);

    }

    if (node->show_axes && node->axes_shader_program) {
        ShaderProgram* program = node->axes_shader_program;

        glUseProgram(program->id);

        // Pass the model, view, and projection matrices to the shader
        glUniformMatrix4fv(program->model_loc, 1, GL_FALSE, (const GLfloat*)node->global_transform);
        glUniformMatrix4fv(program->view_loc, 1, GL_FALSE, (const GLfloat*)view);
        glUniformMatrix4fv(program->proj_loc, 1, GL_FALSE, (const GLfloat*)projection);

        glBindVertexArray(node->axes_vao);
        glDrawArrays(GL_LINES, 0,  sizeof(axesVertices) / (6 * sizeof(float)));

        glBindVertexArray(0);
        glUseProgram(0);
    }

    if (node->show_light_outlines && node->light_outlines_shader_program) {
        ShaderProgram* program = node->light_outlines_shader_program;

        glUseProgram(program->id);

        for (size_t i = 0; i < returned_light_count; ++i) {

            Light* closest_light = closest_lights[i];

            if(!closest_light){
                continue;
            }

            if (program->light_position_loc[i] != -1) {
                glUniform3fv(program->light_position_loc[i], 1, (const GLfloat*)&closest_light->global_position);
            }
            if (program->light_direction_loc[i] != -1) {
                glUniform3fv(program->light_direction_loc[i], 1, (const GLfloat*)&closest_light->direction);
            }
            if (program->light_color_loc[i] != -1) {
                glUniform3fv(program->light_color_loc[i], 1, (const GLfloat*)&closest_light->color);
            }
            if (program->light_specular_loc[i] != -1) {
                glUniform3fv(program->light_specular_loc[i], 1, (const GLfloat*)&closest_light->specular);
            }
            if (program->light_ambient_loc[i] != -1) {
                glUniform3fv(program->light_ambient_loc[i], 1, (const GLfloat*)&closest_light->ambient);
            }
            if (program->light_intensity_loc[i] != -1) {
                glUniform1f(program->light_intensity_loc[i], closest_light->intensity);
            }
            if (program->light_constant_loc[i] != -1) {
                glUniform1f(program->light_constant_loc[i], closest_light->constant);
            }
            if (program->light_linear_loc[i] != -1) {
                glUniform1f(program->light_linear_loc[i], closest_light->linear);
            }
            if (program->light_quadratic_loc[i] != -1) {
                glUniform1f(program->light_quadratic_loc[i], closest_light->quadratic);
            }
            if (program->light_cutOff_loc[i] != -1) {
                glUniform1f(program->light_cutOff_loc[i], closest_light->cutOff);
            }
            if (program->light_outerCutOff_loc[i] != -1) {
                glUniform1f(program->light_outerCutOff_loc[i], closest_light->outerCutOff);
            }
            if (program->light_type_loc[i] != -1) {
                glUniform1i(program->light_type_loc[i], closest_light->type);
            }

            if (program->light_size_loc[i] != -1) {
                glUniform2f(program->light_size_loc[i], closest_light->size[0], closest_light->size[1]);
            }

            glUniform1i(program->num_lights_loc, (GLint)returned_light_count);
        }

        glUniformMatrix4fv(program->model_loc, 1, GL_FALSE, (const GLfloat*)node->global_transform);
        glUniformMatrix4fv(program->view_loc, 1, GL_FALSE, (const GLfloat*)view);
        glUniformMatrix4fv(program->proj_loc, 1, GL_FALSE, (const GLfloat*)projection);

        glBindVertexArray(node->light_outlines_vao);
        glDrawArrays(GL_POINTS, 0, 2);

        glBindVertexArray(0);
        glUseProgram(0);
    }

    free(closest_lights);

    // Render children
    for (size_t i = 0; i < node->children_count; i++) {
        render_nodes(scene, node->children[i], camera, 
            node->global_transform, view, 
            projection, time_value, render_mode);
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




