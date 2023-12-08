
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
static void set_axes_program_for_nodes(SceneNode* node, ShaderProgram* program);

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

    // Finally, free the scene itnode
    free(scene);

    return;
}


void set_scene_lights(Scene* scene, Light** lights, size_t light_count) {
    if (!scene) return;
    scene->lights = lights;
    scene->light_count = light_count;
}

void set_scene_cameras(Scene* scene, Camera** cameras, size_t camera_count) {
    if (!scene) return;
    scene->cameras = cameras;
    scene->camera_count = camera_count;
}

Camera* find_camera_by_name(Scene* scene, const char* name) {
    for (size_t i = 0; i < scene->camera_count; ++i) {
        if (strcmp(scene->cameras[i]->name, name) == 0) {
            return scene->cameras[i];
        }
    }
    return NULL;
}

Light* find_light_by_name(Scene* scene, const char* name) {
    for (size_t i = 0; i < scene->light_count; ++i) {
        if (strcmp(scene->lights[i]->name, name) == 0) {
            return scene->lights[i];
        }
    }
    return NULL;
}

GLboolean setup_scene_axes(Scene* scene){
    GLboolean success = GL_TRUE;

    ShaderProgram* axes_shader_program = create_program();
    if(!axes_shader_program){
        fprintf(stderr, "Failed to create axes shader program\n");
        return;
    }

    Shader* axes_vert_shader = create_shader(VERTEX_SHADER, axes_vert_src);
    if(axes_vert_shader && compile_shader(axes_vert_shader)){
        attach_program_shader(axes_shader_program, axes_vert_shader);
    }else {
        fprintf(stderr, "Axes vertex shader compilation failed\n");
        success = GL_FALSE;
    }

    Shader* axes_frag_shader = create_shader(FRAGMENT_SHADER, axes_frag_src);
    if(axes_frag_shader && compile_shader(axes_frag_shader)){
        attach_program_shader(axes_shader_program, axes_frag_shader);
    } else {
        fprintf(stderr, "Axes fragment shader compilation failed\n");
        success = GL_FALSE;
    }

    if (success && !link_program(axes_shader_program)) {
        fprintf(stderr, "Axes shader program linking failed\n");
        success = GL_FALSE;
    }


    if(success){
        scene->axes_shader_program = axes_shader_program;
        set_axes_program_for_nodes(scene->root_node, axes_shader_program);

        setup_program_uniforms(scene->axes_shader_program);
    }

    return success;
}

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
    node->show_axes = true;

    glGenVertexArrays(1, &node->axesVAO);
    check_gl_error("create node gen vertex failed");

    glGenBuffers(1, &node->axesVBO);
    check_gl_error("create node gen buffers failed");

    node->axes_shader_program = NULL;

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

static void set_axes_program_for_nodes(SceneNode* node, ShaderProgram* program) {
    if (!node) {
        return;
    }

    node->axes_shader_program = program;

    for (size_t i = 0; i < node->children_count; ++i) {
        set_axes_program_for_nodes(node->children[i], program);
    }
}

void set_show_axes_for_nodes(SceneNode* node, bool show_axes){
    if (!node) return;
    
    node->show_axes = show_axes;

    for (size_t i = 0; i < node->children_count; ++i) {
        set_show_axes_for_nodes(node->children[i], show_axes);
    }

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
    
    /*
     * Setup and upload axes buffers.
     */

    // Bind the Vertex Array Object (VAO)
    glBindVertexArray(node->axesVAO);

    // Bind and set up the Vertex Buffer Object (VBO)
    glBindBuffer(GL_ARRAY_BUFFER, node->axesVBO);
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

    // Unbind VAO
    glBindVertexArray(0);

    for (size_t i = 0; i < node->children_count; i++) {
        upload_buffers_to_gpu_for_nodes(node->children[i]);
    }
}

void transform_node(SceneNode* node, Transform* transform) {
    if (!node || !transform) return;

    mat4 localTransform;
    glm_mat4_identity(localTransform);
    glm_translate(localTransform, transform->position);
    glm_rotate(localTransform, transform->rotation[0], (vec3){1.0f, 0.0f, 0.0f});
    glm_rotate(localTransform, transform->rotation[1], (vec3){0.0f, 1.0f, 0.0f});
    glm_rotate(localTransform, transform->rotation[2], (vec3){0.0f, 0.0f, 1.0f});
    glm_scale(localTransform, transform->scale);

    glm_mat4_mul(node->original_transform, localTransform, node->local_transform);
}

void apply_transform_to_nodes(SceneNode* node, mat4 parentTransform) {
    if (!node) return;

    mat4 localTransform;
    glm_mat4_mul(node->original_transform, node->local_transform, localTransform);
    glm_mat4_mul(parentTransform, localTransform, node->global_transform);

    for (size_t i = 0; i < node->children_count; i++) {
        if (node->children[i]) {
            apply_transform_to_nodes(node->children[i], node->global_transform);
        }
    }
}

void render_nodes(SceneNode* node, Camera *camera, 
        mat4 model, mat4 view, mat4 projection, 
        float time_value, RenderMode render_mode) {
    if (!node) {
        printf("error: render_node called with NULL node\n");
        return;
    }

    if (node->shader_program) {
        // Use the shader program
        glUseProgram(node->shader_program->id);

        // PBR, NORMALS, etc...
        glUniform1i(node->shader_program->render_mode_loc, render_mode);
        glUniform1f(node->shader_program->near_clip_loc, camera->near_clip);
        glUniform1f(node->shader_program->far_clip_loc, camera->far_clip);


        // Set shader uniforms for model, view, and projection matrices
        glUniformMatrix4fv(node->shader_program->model_loc, 1, GL_FALSE, (const GLfloat*)node->global_transform);
        glUniformMatrix4fv(node->shader_program->view_loc, 1, GL_FALSE, (const GLfloat*)view);
        glUniformMatrix4fv(node->shader_program->proj_loc, 1, GL_FALSE, (const GLfloat*)projection);

        // cam position and time
        glUniform3fv(node->shader_program->cam_pos_loc, 1, (const GLfloat*)&camera->position);
        glUniform1f(node->shader_program->time_loc, time_value);

        // Set material properties if available
        if (node->meshes && node->mesh_count > 0) {
            for (size_t i = 0; i < node->mesh_count; ++i) {
                Mesh* mesh = node->meshes[i];
                if (mesh && mesh->material) {
                    Material* mat = mesh->material;
                    
                    // Set albedo, metallic, roughness, and ambient occlusion
                    glUniform3fv(node->shader_program->albedo_loc, 1, (const GLfloat*)&mat->albedo);
                    glUniform1f(node->shader_program->metallic_loc, mat->metallic);
                    glUniform1f(node->shader_program->roughness_loc, mat->roughness);
                    glUniform1f(node->shader_program->ao_loc, mat->ao);

                    if (mat->albedoTex) {
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, mat->albedoTex->id);
                        glUniform1i(node->shader_program->albedo_tex_loc, 0);
                    }

                    if (mat->normalTex) {
                        glActiveTexture(GL_TEXTURE1);
                        glBindTexture(GL_TEXTURE_2D, mat->normalTex->id);
                        glUniform1i(node->shader_program->normal_tex_loc, 1);
                    }

                    if (mat->roughnessTex) {
                        glActiveTexture(GL_TEXTURE2);
                        glBindTexture(GL_TEXTURE_2D, mat->roughnessTex->id);
                        glUniform1i(node->shader_program->roughness_tex_loc, 2);
                    }

                    if (mat->metalnessTex) {
                        glActiveTexture(GL_TEXTURE3);
                        glBindTexture(GL_TEXTURE_2D, mat->metalnessTex->id);
                        glUniform1i(node->shader_program->metalness_tex_loc, 3);
                    }

                    if (mat->ambientOcclusionTex) {
                        glActiveTexture(GL_TEXTURE4);
                        glBindTexture(GL_TEXTURE_2D, mat->ambientOcclusionTex->id);
                        glUniform1i(node->shader_program->ao_tex_loc, 4);
                    }

                    if (mat->emissiveTex) {
                        glActiveTexture(GL_TEXTURE5);
                        glBindTexture(GL_TEXTURE_2D, mat->emissiveTex->id);
                        glUniform1i(node->shader_program->emissive_tex_loc, 5);
                    }

                    if (mat->heightTex) {
                        glActiveTexture(GL_TEXTURE6);
                        glBindTexture(GL_TEXTURE_2D, mat->heightTex->id);
                        glUniform1i(node->shader_program->height_tex_loc, 6);
                    }

                    if (mat->opacityTex) {
                        glActiveTexture(GL_TEXTURE7);
                        glBindTexture(GL_TEXTURE_2D, mat->opacityTex->id);
                        glUniform1i(node->shader_program->opacity_tex_loc, 7);
                    }

                    if (mat->sheenTex) {
                        glActiveTexture(GL_TEXTURE8);
                        glBindTexture(GL_TEXTURE_2D, mat->sheenTex->id);
                        glUniform1i(node->shader_program->sheen_tex_loc, 8);
                    }

                    if (mat->reflectanceTex) {
                        glActiveTexture(GL_TEXTURE9);
                        glBindTexture(GL_TEXTURE_2D, mat->reflectanceTex->id);
                        glUniform1i(node->shader_program->reflectance_tex_loc, 9);
                    }

                    // Set uniform values for each texture type
                    glUniform1i(node->shader_program->albedo_tex_exists_loc, mat->albedoTex ? 1 : 0);
                    glUniform1i(node->shader_program->normal_tex_exists_loc, mat->normalTex ? 1 : 0);
                    glUniform1i(node->shader_program->roughness_tex_exists_loc, mat->roughnessTex ? 1 : 0);
                    glUniform1i(node->shader_program->metalness_tex_exists_loc, mat->metalnessTex ? 1 : 0);
                    glUniform1i(node->shader_program->ao_tex_exists_loc, mat->ambientOcclusionTex ? 1 : 0);
                    glUniform1i(node->shader_program->emissive_tex_exists_loc, mat->emissiveTex ? 1 : 0);
                    glUniform1i(node->shader_program->height_tex_exists_loc, mat->heightTex ? 1 : 0);
                    glUniform1i(node->shader_program->opacity_tex_exists_loc, mat->opacityTex ? 1 : 0);
                    glUniform1i(node->shader_program->sheen_tex_exists_loc, mat->sheenTex ? 1 : 0);
                    glUniform1i(node->shader_program->reflectance_tex_exists_loc, mat->reflectanceTex ? 1 : 0);

                }
                // Bind VAO and draw the mesh
                glBindVertexArray(mesh->VAO);
                glDrawElements(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, 0);
            }
        }

        // Check and pass light data to shader
        if (node->light) {
            GLint lightPosLoc = glGetUniformLocation(node->shader_program->id, "light.position");
            GLint lightDirLoc = glGetUniformLocation(node->shader_program->id, "light.direction");
            GLint lightColorLoc = glGetUniformLocation(node->shader_program->id, "light.color");
            GLint lightColorSpecular = glGetUniformLocation(node->shader_program->id, "light.specular");
            GLint lightColorAmbient = glGetUniformLocation(node->shader_program->id, "light.ambient");
            GLint lightIntensityLoc = glGetUniformLocation(node->shader_program->id, "light.intensity");
            GLint lightConstantLoc = glGetUniformLocation(node->shader_program->id, "light.constant");
            GLint lightLinearLoc = glGetUniformLocation(node->shader_program->id, "light.linear");
            GLint lightQuadraticLoc = glGetUniformLocation(node->shader_program->id, "light.quadratic");
            GLint lightCutOffLoc = glGetUniformLocation(node->shader_program->id, "light.cutOff");
            GLint lightOuterCutOffLoc = glGetUniformLocation(node->shader_program->id, "light.outerCutOff");
            GLint lightTypeLoc = glGetUniformLocation(node->shader_program->id, "light.type");
            

            glUniform3fv(lightPosLoc, 1, (const GLfloat*)&node->light->position);
            glUniform3fv(lightDirLoc, 1, (const GLfloat*)&node->light->direction);
            glUniform3fv(lightColorLoc, 1, (const GLfloat*)&node->light->color);
            glUniform3fv(lightColorSpecular, 1, (const GLfloat*)&node->light->specular);
            glUniform3fv(lightColorAmbient, 1, (const GLfloat*)&node->light->ambient);
            glUniform1f(lightIntensityLoc, node->light->intensity);
            glUniform1f(lightConstantLoc, node->light->constant);
            glUniform1f(lightLinearLoc, node->light->linear);
            glUniform1f(lightQuadraticLoc, node->light->quadratic);
            glUniform1f(lightCutOffLoc, node->light->cutOff);
            glUniform1f(lightOuterCutOffLoc, node->light->outerCutOff);
            glUniform1f(lightTypeLoc, node->light->type);

        }

        // Optional cleanup
        glBindVertexArray(0);
        glUseProgram(0);

    }

    if (node->show_axes && node->axes_shader_program) {
        glUseProgram(node->axes_shader_program->id);

        // Pass the model, view, and projection matrices to the shader
        glUniformMatrix4fv(node->axes_shader_program->model_loc, 1, GL_FALSE, (const GLfloat*)node->global_transform);
        glUniformMatrix4fv(node->axes_shader_program->view_loc, 1, GL_FALSE, (const GLfloat*)view);
        glUniformMatrix4fv(node->axes_shader_program->proj_loc, 1, GL_FALSE, (const GLfloat*)projection);

        glBindVertexArray(node->axesVAO);
        glDrawArrays(GL_LINES, 0,  sizeof(axesVertices) / (6 * sizeof(float)));
        glBindVertexArray(0);

        glUseProgram(0);
    }

    // Render children
    for (size_t i = 0; i < node->children_count; i++) {
        render_nodes(node->children[i], camera, 
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

void print_scene(const Scene* scene) {
    if (!scene) return;

    printf("Scene | Lights: %zu | Cameras: %zu | Texture Directory: %s\n",
           scene->light_count,
           scene->camera_count,
           scene->tex_pool ? (scene->tex_pool->directory ? scene->tex_pool->directory : "None") : "None");

    print_scene_node(scene->root_node, 0);
}


