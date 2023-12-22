
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

void _update_program_light_uniforms(ShaderProgram * program, Light* light, 
        size_t light_count, size_t index){
    if(!program || !light){
        return;
    }

    if (program->light_position_loc[index] != -1) {
        glUniform3fv(program->light_position_loc[index], 1, (const GLfloat*)&light->global_position);
    }
    if (program->light_direction_loc[index] != -1) {
        glUniform3fv(program->light_direction_loc[index], 1, (const GLfloat*)&light->direction);
    }
    if (program->light_color_loc[index] != -1) {
        glUniform3fv(program->light_color_loc[index], 1, (const GLfloat*)&light->color);
    }
    if (program->light_specular_loc[index] != -1) {
        glUniform3fv(program->light_specular_loc[index], 1, (const GLfloat*)&light->specular);
    }
    if (program->light_ambient_loc[index] != -1) {
        glUniform3fv(program->light_ambient_loc[index], 1, (const GLfloat*)&light->ambient);
    }
    if (program->light_intensity_loc[index] != -1) {
        glUniform1f(program->light_intensity_loc[index], light->intensity);
    }
    if (program->light_constant_loc[index] != -1) {
        glUniform1f(program->light_constant_loc[index], light->constant);
    }
    if (program->light_linear_loc[index] != -1) {
        glUniform1f(program->light_linear_loc[index], light->linear);
    }
    if (program->light_quadratic_loc[index] != -1) {
        glUniform1f(program->light_quadratic_loc[index], light->quadratic);
    }
    if (program->light_cutOff_loc[index] != -1) {
        glUniform1f(program->light_cutOff_loc[index], light->cutOff);
    }
    if (program->light_outerCutOff_loc[index] != -1) {
        glUniform1f(program->light_outerCutOff_loc[index], light->outerCutOff);
    }
    if (program->light_type_loc[index] != -1) {
        glUniform1i(program->light_type_loc[index], light->type);
    }

    if (program->light_size_loc[index] != -1) {
        glUniform2f(program->light_size_loc[index], light->size[0], light->size[1]);
    }

    glUniform1i(program->num_lights_loc, (GLint)light_count);
}

void _update_program_material_uniforms(ShaderProgram * program, Material* material){
    if(!program || !material){
        return;
    }

    // Set albedo, metallic, roughness, and ambient occlusion
    glUniform3fv(program->albedo_loc, 1, (const GLfloat*)&material->albedo);
    glUniform1f(program->metallic_loc, material->metallic);
    glUniform1f(program->roughness_loc, material->roughness);
    glUniform1f(program->ao_loc, material->ao);

    if (material->albedoTex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, material->albedoTex->id);
        glUniform1i(program->albedo_tex_loc, 0);
    }

    if (material->normalTex) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, material->normalTex->id);
        glUniform1i(program->normal_tex_loc, 1);
    }

    if (material->roughnessTex) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, material->roughnessTex->id);
        glUniform1i(program->roughness_tex_loc, 2);
    }

    if (material->metalnessTex) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, material->metalnessTex->id);
        glUniform1i(program->metalness_tex_loc, 3);
    }

    if (material->ambientOcclusionTex) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, material->ambientOcclusionTex->id);
        glUniform1i(program->ao_tex_loc, 4);
    }

    if (material->emissiveTex) {
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, material->emissiveTex->id);
        glUniform1i(program->emissive_tex_loc, 5);
    }

    if (material->heightTex) {
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, material->heightTex->id);
        glUniform1i(program->height_tex_loc, 6);
    }

    if (material->opacityTex) {
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, material->opacityTex->id);
        glUniform1i(program->opacity_tex_loc, 7);
    }

    if (material->sheenTex) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, material->sheenTex->id);
        glUniform1i(program->sheen_tex_loc, 8);
    }

    if (material->reflectanceTex) {
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_2D, material->reflectanceTex->id);
        glUniform1i(program->reflectance_tex_loc, 9);
    }

    // Set uniform values for each texture type
    glUniform1i(program->albedo_tex_exists_loc, material->albedoTex ? 1 : 0);
    glUniform1i(program->normal_tex_exists_loc, material->normalTex ? 1 : 0);
    glUniform1i(program->roughness_tex_exists_loc, material->roughnessTex ? 1 : 0);
    glUniform1i(program->metalness_tex_exists_loc, material->metalnessTex ? 1 : 0);
    glUniform1i(program->ao_tex_exists_loc, material->ambientOcclusionTex ? 1 : 0);
    glUniform1i(program->emissive_tex_exists_loc, material->emissiveTex ? 1 : 0);
    glUniform1i(program->height_tex_exists_loc, material->heightTex ? 1 : 0);
    glUniform1i(program->opacity_tex_exists_loc, material->opacityTex ? 1 : 0);
    glUniform1i(program->sheen_tex_exists_loc, material->sheenTex ? 1 : 0);
    glUniform1i(program->reflectance_tex_exists_loc, material->reflectanceTex ? 1 : 0);

}

void _update_camera_uniforms(ShaderProgram * program, Camera *camera){
    if(!program || !camera){
        return;
    }

    glUniform3fv(program->cam_pos_loc, 1, (const GLfloat*)&camera->position);
    glUniform1f(program->near_clip_loc, camera->near_clip);
    glUniform1f(program->far_clip_loc, camera->far_clip);

}

void _render_node(SceneNode* node, Camera *camera,
        mat4 model, mat4 view, mat4 projection,
        float time_value, RenderMode render_mode, Light** closest_lights, 
        size_t returned_light_count){

    // Process each mesh and its material
    if (node->meshes && node->mesh_count > 0) {
        for (size_t i = 0; i < node->mesh_count; ++i) {
            Mesh* mesh = node->meshes[i];
            if (mesh && mesh->material) {
                Material* mat = mesh->material;

                // Use the shader program associated with the material
                ShaderProgram* program = mat->shader_program;
                glUseProgram(program->id);

                // Update light uniforms per material
                for (size_t j = 0; j < returned_light_count; ++j) {
                    Light* closest_light = closest_lights[j];
                    _update_program_light_uniforms(program, closest_light, returned_light_count, j);
                }

                // Set shader uniforms for model, view, projection matrices, and other properties
                glUniformMatrix4fv(program->view_loc, 1, GL_FALSE, (const GLfloat*)view);
                glUniformMatrix4fv(program->proj_loc, 1, GL_FALSE, (const GLfloat*)projection);
                glUniformMatrix4fv(program->model_loc, 1, GL_FALSE, (const GLfloat*)node->global_transform);

                glUniform1f(program->time_loc, time_value);
                glUniform1i(program->render_mode_loc, render_mode);
                glUniform1f(program->line_width_loc, mesh->line_width);

                _update_camera_uniforms(program, camera);
                _update_program_material_uniforms(program, mat);

                // Bind VAO and draw the mesh
                glBindVertexArray(mesh->vao);
                glDrawElements(mesh->draw_mode, mesh->index_count, GL_UNSIGNED_INT, 0);

                // Cleanup after rendering the mesh
                glBindVertexArray(0);
                glUseProgram(0);
            }
        }
    }
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

        _update_program_light_uniforms(program, closest_light, returned_light_count, i);
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

    _render_node(node, camera, model, view, projection, 
            time_value, render_mode, closest_lights, returned_light_count);

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

