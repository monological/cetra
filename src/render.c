
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

void _render_node(SceneNode* node, Camera *camera, 
        mat4 model, mat4 view, mat4 projection, 
        float time_value, RenderMode render_mode, Light** closest_lights, size_t returned_light_count){
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
            glBindVertexArray(mesh->vao);
            glDrawElements(mesh->draw_mode, mesh->index_count, GL_UNSIGNED_INT, 0);
        }
    }

    // Optional cleanup
    glBindVertexArray(0);
    glUseProgram(0);
}

void _render_axes(SceneNode* node, mat4 view, mat4 projection){
    if(!node || !node->axes_shader_program){
        return;
    }
    ShaderProgram* program = node->axes_shader_program;

    glUseProgram(program->id);

    // Pass the model, view, and projection matrices to the shader
    glUniformMatrix4fv(program->model_loc, 1, GL_FALSE, (const GLfloat*)node->global_transform);
    glUniformMatrix4fv(program->view_loc, 1, GL_FALSE, (const GLfloat*)view);
    glUniformMatrix4fv(program->proj_loc, 1, GL_FALSE, (const GLfloat*)projection);

    glBindVertexArray(node->axes_vao);
    glDrawArrays(GL_LINES, 0,  axes_vertices_size / (6 * sizeof(float)));

    glBindVertexArray(0);
    glUseProgram(0);
}

void _render_outlines(SceneNode* node, Camera *camera, 
        mat4 model, mat4 view, mat4 projection, 
        float time_value, RenderMode render_mode, Light** closest_lights, size_t returned_light_count){
    if(!node || !node->outlines_shader_program){
        return;
    }
    ShaderProgram* program = node->outlines_shader_program;

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

    glBindVertexArray(node->outlines_vao);
    glDrawArrays(GL_POINTS, 0, 2);

    glBindVertexArray(0);
    glUseProgram(0);
}

void render_scene(Scene* scene, SceneNode *node, Camera *camera, 
        mat4 model, mat4 view, mat4 projection, 
        float time_value, RenderMode render_mode) {

    if (!scene) {
        printf("error: render called with NULL scene\n");
        return;
    }

    if (!node) {
        printf("error: render called with NULL root node\n");
        return;
    }

    size_t returned_light_count;
    size_t max_lights = calculate_max_lights();
    Light** closest_lights = get_closest_lights(scene, node, max_lights, 
            &returned_light_count);

    if (node->shader_program) {
        _render_node(node, camera, model, view, projection, 
                time_value, render_mode, closest_lights, returned_light_count);
    }

    if (node->show_axes && node->axes_shader_program) {
        _render_axes(node, view, projection);
    }

    if (node->show_outlines && node->outlines_shader_program) {
        _render_outlines(node, camera, model, view, projection, 
                time_value, render_mode, closest_lights, returned_light_count);
    }

    free(closest_lights);

    // Render children
    for (size_t i = 0; i < node->children_count; i++) {
        render_scene(scene, node->children[i], camera, 
            node->global_transform, view, 
            projection, time_value, render_mode);
    }
}

