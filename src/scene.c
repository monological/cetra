
#include <stdlib.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "scene.h"
#include "program.h"
#include "shader.h"
#include "mesh.h"
#include "light.h"
#include "camera.h"

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
    scene->texture_directory = NULL;
    return scene;
}

void set_scene_texture_directory(Scene* scene, const char* directory) {
    if (scene) {
        if (scene->texture_directory) {
            free(scene->texture_directory);
        }
        scene->texture_directory = strdup(directory);
    }
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

void free_scene(Scene* scene) {
    if (!scene) {
        return; // Nothing to free
    }

    // Free texture directory string
    if (scene->texture_directory) {
        free(scene->texture_directory);
        scene->texture_directory = NULL;
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

    // Finally, free the scene itself
    free(scene);
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
    node->camera_name = NULL;

    node->shader_program = NULL;

    node->show_axes = true;
    glGenVertexArrays(1, &node->axesVAO);
    glGenBuffers(1, &node->axesVBO);
    node->axes_shader_program = NULL;

    return node;
}

void set_node_name(SceneNode* node, const char* name) {
    if (!node || !name) return;
    node->name = strdup(name);
}

void set_node_light(SceneNode* node, Light* light) {
    if (!node) return;
    node->light = light;
}

void set_node_camera(SceneNode* node, Camera* camera) {
    if (!node) return;
    node->camera = camera;
}

void set_node_camera_name(SceneNode* node, const char* name) {
    if (!node || !name) return;
    node->camera_name = strdup(name);
}


void set_program_for_node(SceneNode* node, ShaderProgram* program) {
    if (!node) {
        return; // If the node is NULL, exit the function
    }

    // Set the shader program for the current node
    node->shader_program = program;

    // Recursively set the shader program for each child node
    for (size_t i = 0; i < node->children_count; ++i) {
        set_program_for_node(node->children[i], program);
    }
}

void set_axes_program_for_node(SceneNode* node, ShaderProgram* program) {
    if (!node) {
        return; // If the node is NULL, exit the function
    }

    // Set the shader program for the current node
    node->axes_shader_program = program;

    // Recursively set the shader program for each child node
    for (size_t i = 0; i < node->children_count; ++i) {
        set_axes_program_for_node(node->children[i], program);
    }
}

void add_child_node(SceneNode* self, SceneNode* child) {
    self->children = realloc(self->children, (self->children_count + 1) * sizeof(SceneNode*));
    self->children[self->children_count] = child;
    child->parent = self;
    self->children_count++;
}

void setup_node_axes_buffers(SceneNode* node) {
    if (!node) return;

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

    // Unbind VAO
    glBindVertexArray(0);

    for (size_t i = 0; i < node->children_count; ++i) {
        setup_node_axes_buffers(node->children[i]);
    }
}

void set_node_show_axes(SceneNode* node, bool show_axes){
    if (!node) return;
    
    node->show_axes = show_axes;

    for (size_t i = 0; i < node->children_count; ++i) {
        set_node_show_axes(node->children[i], show_axes);
    }

}

void draw_node_axes(SceneNode* node, const mat4 model, const mat4 view, const mat4 projection) {
    if (!node || !node->show_axes || !node->axes_shader_program) return;

    glUseProgram(node->axes_shader_program->id);

    // Pass the model, view, and projection matrices to the shader
    GLint modelLoc = glGetUniformLocation(node->axes_shader_program->id, "model");
    GLint viewLoc = glGetUniformLocation(node->axes_shader_program->id, "view");
    GLint projLoc = glGetUniformLocation(node->axes_shader_program->id, "projection");

    glUniformMatrix4fv(modelLoc, 1, GL_FALSE, (const GLfloat*)model);
    glUniformMatrix4fv(viewLoc, 1, GL_FALSE, (const GLfloat*)view);
    glUniformMatrix4fv(projLoc, 1, GL_FALSE, (const GLfloat*)projection);

    glBindVertexArray(node->axesVAO);
    glDrawArrays(GL_LINES, 0,  sizeof(axesVertices) / (6 * sizeof(float)));
    glBindVertexArray(0);

    glUseProgram(0);
}


void render_node(SceneNode* self, mat4 model, mat4 view, mat4 projection, 
        float time_value, vec3 cam_pos) {
    if (!self) {
        printf("error: render_node called with NULL node\n");
        return;
    }

    if (self->shader_program) {
        // Use the shader program
        glUseProgram(self->shader_program->id);

        // Set shader uniforms for model, view, and projection matrices
        glUniformMatrix4fv(self->shader_program->model_loc, 1, GL_FALSE, (const GLfloat*)self->global_transform);
        glUniformMatrix4fv(self->shader_program->view_loc, 1, GL_FALSE, (const GLfloat*)view);
        glUniformMatrix4fv(self->shader_program->proj_loc, 1, GL_FALSE, (const GLfloat*)projection);

        // cam position and time
        glUniform3fv(self->shader_program->cam_pos_loc, 1, (const GLfloat*)&cam_pos);
        glUniform1f(self->shader_program->time_loc, time_value);

        // Set material properties if available
        if (self->meshes && self->mesh_count > 0) {
            for (size_t i = 0; i < self->mesh_count; ++i) {
                Mesh* mesh = self->meshes[i];
                if (mesh && mesh->material) {
                    Material* mat = mesh->material;
                    
                    // Set albedo, metallic, roughness, and ambient occlusion
                    glUniform3fv(self->shader_program->albedo_loc, 1, (const GLfloat*)&mat->albedo);
                    glUniform1f(self->shader_program->metallic_loc, mat->metallic);
                    glUniform1f(self->shader_program->roughness_loc, mat->roughness);
                    glUniform1f(self->shader_program->ao_loc, mat->ao);

                    if (mat->albedoTex) {
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, mat->albedoTex->id);
                        glUniform1i(self->shader_program->albedo_tex_loc, 0);
                    }

                    if (mat->normalTex) {
                        glActiveTexture(GL_TEXTURE1);
                        glBindTexture(GL_TEXTURE_2D, mat->normalTex->id);
                        glUniform1i(self->shader_program->normal_tex_loc, 1);
                    }

                    if (mat->roughnessTex) {
                        glActiveTexture(GL_TEXTURE2);
                        glBindTexture(GL_TEXTURE_2D, mat->roughnessTex->id);
                        glUniform1i(self->shader_program->roughness_tex_loc, 2);
                    }

                    if (mat->metalnessTex) {
                        glActiveTexture(GL_TEXTURE3);
                        glBindTexture(GL_TEXTURE_2D, mat->metalnessTex->id);
                        glUniform1i(self->shader_program->metalness_tex_loc, 3);
                    }

                    if (mat->ambientOcclusionTex) {
                        glActiveTexture(GL_TEXTURE4);
                        glBindTexture(GL_TEXTURE_2D, mat->ambientOcclusionTex->id);
                        glUniform1i(self->shader_program->ao_tex_loc, 4);
                    }

                    if (mat->emissiveTex) {
                        glActiveTexture(GL_TEXTURE5);
                        glBindTexture(GL_TEXTURE_2D, mat->emissiveTex->id);
                        glUniform1i(self->shader_program->emissive_tex_loc, 5);
                    }

                    if (mat->heightTex) {
                        glActiveTexture(GL_TEXTURE6);
                        glBindTexture(GL_TEXTURE_2D, mat->heightTex->id);
                        glUniform1i(self->shader_program->height_tex_loc, 6);
                    }

                    if (mat->opacityTex) {
                        glActiveTexture(GL_TEXTURE7);
                        glBindTexture(GL_TEXTURE_2D, mat->opacityTex->id);
                        glUniform1i(self->shader_program->opacity_tex_loc, 7);
                    }

                    if (mat->sheenTex) {
                        glActiveTexture(GL_TEXTURE8);
                        glBindTexture(GL_TEXTURE_2D, mat->sheenTex->id);
                        glUniform1i(self->shader_program->sheen_tex_loc, 8);
                    }

                    if (mat->reflectanceTex) {
                        glActiveTexture(GL_TEXTURE9);
                        glBindTexture(GL_TEXTURE_2D, mat->reflectanceTex->id);
                        glUniform1i(self->shader_program->reflectance_tex_loc, 9);
                    }

                    // Set uniform values for each texture type
                    glUniform1i(self->shader_program->albedo_tex_exists_loc, mat->albedoTex ? 1 : 0);
                    glUniform1i(self->shader_program->normal_tex_exists_loc, mat->normalTex ? 1 : 0);
                    glUniform1i(self->shader_program->roughness_tex_exists_loc, mat->roughnessTex ? 1 : 0);
                    glUniform1i(self->shader_program->metalness_tex_exists_loc, mat->metalnessTex ? 1 : 0);
                    glUniform1i(self->shader_program->ao_tex_exists_loc, mat->ambientOcclusionTex ? 1 : 0);
                    glUniform1i(self->shader_program->emissive_tex_exists_loc, mat->emissiveTex ? 1 : 0);
                    glUniform1i(self->shader_program->height_tex_exists_loc, mat->heightTex ? 1 : 0);
                    glUniform1i(self->shader_program->opacity_tex_exists_loc, mat->opacityTex ? 1 : 0);
                    glUniform1i(self->shader_program->sheen_tex_exists_loc, mat->sheenTex ? 1 : 0);
                    glUniform1i(self->shader_program->reflectance_tex_exists_loc, mat->reflectanceTex ? 1 : 0);

                }
                // Bind VAO and draw the mesh
                glBindVertexArray(mesh->VAO);
                glDrawElements(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, 0);
            }
        }

        // Check and pass light data to shader
        if (self->light) {
            GLint lightPosLoc = glGetUniformLocation(self->shader_program->id, "light.position");
            GLint lightDirLoc = glGetUniformLocation(self->shader_program->id, "light.direction");
            GLint lightColorLoc = glGetUniformLocation(self->shader_program->id, "light.color");
            GLint lightColorSpecular = glGetUniformLocation(self->shader_program->id, "light.specular");
            GLint lightColorAmbient = glGetUniformLocation(self->shader_program->id, "light.ambient");
            GLint lightIntensityLoc = glGetUniformLocation(self->shader_program->id, "light.intensity");
            GLint lightConstantLoc = glGetUniformLocation(self->shader_program->id, "light.constant");
            GLint lightLinearLoc = glGetUniformLocation(self->shader_program->id, "light.linear");
            GLint lightQuadraticLoc = glGetUniformLocation(self->shader_program->id, "light.quadratic");
            GLint lightCutOffLoc = glGetUniformLocation(self->shader_program->id, "light.cutOff");
            GLint lightOuterCutOffLoc = glGetUniformLocation(self->shader_program->id, "light.outerCutOff");
            GLint lightTypeLoc = glGetUniformLocation(self->shader_program->id, "light.type");
            

            glUniform3fv(lightPosLoc, 1, (const GLfloat*)&self->light->position);
            glUniform3fv(lightDirLoc, 1, (const GLfloat*)&self->light->direction);
            glUniform3fv(lightColorLoc, 1, (const GLfloat*)&self->light->color);
            glUniform3fv(lightColorSpecular, 1, (const GLfloat*)&self->light->specular);
            glUniform3fv(lightColorAmbient, 1, (const GLfloat*)&self->light->ambient);
            glUniform1f(lightIntensityLoc, self->light->intensity);
            glUniform1f(lightConstantLoc, self->light->constant);
            glUniform1f(lightLinearLoc, self->light->linear);
            glUniform1f(lightQuadraticLoc, self->light->quadratic);
            glUniform1f(lightCutOffLoc, self->light->cutOff);
            glUniform1f(lightOuterCutOffLoc, self->light->outerCutOff);
            glUniform1f(lightTypeLoc, self->light->type);

        }

        // Optional cleanup
        glUseProgram(0);
        glBindVertexArray(0);

    }

    if (self->show_axes && self->axes_shader_program) {
        draw_node_axes(self, self->global_transform, view, projection);
    }

    // Render children
    for (size_t i = 0; i < self->children_count; i++) {
        render_node(self->children[i], self->global_transform, view, projection, time_value, cam_pos);
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

void update_child_nodes(SceneNode* node, mat4 parentTransform) {
    if (!node) return;

    mat4 localTransform;
    glm_mat4_mul(node->original_transform, node->local_transform, localTransform);
    glm_mat4_mul(parentTransform, localTransform, node->global_transform);

    for (size_t i = 0; i < node->children_count; i++) {
        if (node->children[i]) {
            update_child_nodes(node->children[i], node->global_transform);
        }
    }
}

void setup_node_meshes(SceneNode* node) {
    if (!node) return;

    // Setup mesh buffers for all meshes in this node
    for (size_t i = 0; i < node->mesh_count; i++) {
        if (node->meshes[i]) {
            setup_mesh_buffers(node->meshes[i]);
        }
    }

    // Recursively setup mesh buffers for all children
    for (size_t i = 0; i < node->children_count; i++) {
        setup_node_meshes(node->children[i]);
    }
}



void free_node(SceneNode* node) {
    if (!node) return;

    // Free all child nodes recursively
    for (size_t i = 0; i < node->children_count; i++) {
        free_node(node->children[i]);
    }

    // Free the array of children pointers
    free(node->children);

    // Free the meshes array and each mesh
    for (size_t i = 0; i < node->mesh_count; i++) {
        if (node->meshes[i]) {
            free_mesh(node->meshes[i]); // Assuming free_mesh is a function to properly free a Mesh
        }
    }
    free(node->meshes);

    // If the node has a name, free it
    if (node->name) {
        free(node->name);
    }

    if (node->camera_name) {
        free(node->camera_name);
    }

    // Note: Do not free camera and light, as they are managed by the Scene.
    // Similarly, shaders and programs are usually shared and should be managed separately.

    free(node);
}

void print_indentation(int depth) {
    for (int i = 0; i < depth; i++) {
        printf("    ");
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
           node->camera_name ? node->camera_name : "None");

    // Recursively print children nodes
    for (size_t i = 0; i < node->children_count; i++) {
        print_scene_node(node->children[i], depth + 1);
    }
}


void print_scene(const Scene* scene) {
    if (!scene) return;

    printf("Scene | Lights: %zu | Cameras: %zu | Texture Directory: %s\n",
           scene->light_count,
           scene->camera_count,
           scene->texture_directory ? scene->texture_directory : "None");

    print_scene_node(scene->root_node, 0);
}


