
#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "ext/log.h"
#include "scene.h"
#include "program.h"
#include "uniform.h"
#include "shader.h"
#include "mesh.h"
#include "material.h"
#include "light.h"
#include "camera.h"
#include "common.h"
#include "engine.h"
#include "util.h"

static void _update_program_light_uniforms(ShaderProgram* program, Light* light, size_t light_count,
                                           size_t index) {
    if (!program || !program->uniforms || !light)
        return;

    UniformManager* u = program->uniforms;

    GLint loc;

    loc = uniform_array_location(u, "lights", index, "position");
    if (loc >= 0)
        glUniform3fv(loc, 1, (const GLfloat*)&light->global_position);

    loc = uniform_array_location(u, "lights", index, "direction");
    if (loc >= 0)
        glUniform3fv(loc, 1, (const GLfloat*)&light->direction);

    loc = uniform_array_location(u, "lights", index, "color");
    if (loc >= 0)
        glUniform3fv(loc, 1, (const GLfloat*)&light->color);

    loc = uniform_array_location(u, "lights", index, "specular");
    if (loc >= 0)
        glUniform3fv(loc, 1, (const GLfloat*)&light->specular);

    loc = uniform_array_location(u, "lights", index, "ambient");
    if (loc >= 0)
        glUniform3fv(loc, 1, (const GLfloat*)&light->ambient);

    loc = uniform_array_location(u, "lights", index, "intensity");
    if (loc >= 0)
        glUniform1f(loc, light->intensity);

    loc = uniform_array_location(u, "lights", index, "constant");
    if (loc >= 0)
        glUniform1f(loc, light->constant);

    loc = uniform_array_location(u, "lights", index, "linear");
    if (loc >= 0)
        glUniform1f(loc, light->linear);

    loc = uniform_array_location(u, "lights", index, "quadratic");
    if (loc >= 0)
        glUniform1f(loc, light->quadratic);

    loc = uniform_array_location(u, "lights", index, "cutOff");
    if (loc >= 0)
        glUniform1f(loc, light->cutOff);

    loc = uniform_array_location(u, "lights", index, "outerCutOff");
    if (loc >= 0)
        glUniform1f(loc, light->outerCutOff);

    loc = uniform_array_location(u, "lights", index, "type");
    if (loc >= 0)
        glUniform1i(loc, light->type);

    loc = uniform_array_location(u, "lights", index, "size");
    if (loc >= 0)
        glUniform2f(loc, light->size[0], light->size[1]);

    uniform_set_int(u, "numLights", (int)light_count);
}

void _update_program_material_uniforms(ShaderProgram* program, Material* material) {
    if (!program || !program->uniforms || !material)
        return;

    UniformManager* u = program->uniforms;

    uniform_set_vec3(u, "albedo", (const float*)&material->albedo);
    uniform_set_float(u, "metallic", material->metallic);
    uniform_set_float(u, "roughness", material->roughness);
    uniform_set_float(u, "ao", material->ao);

    if (material->albedo_tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, material->albedo_tex->id);
        uniform_set_int(u, "albedoTex", 0);
    }

    if (material->normal_tex) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, material->normal_tex->id);
        uniform_set_int(u, "normalTex", 1);
    }

    if (material->roughness_tex) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, material->roughness_tex->id);
        uniform_set_int(u, "roughnessTex", 2);
    }

    if (material->metalness_tex) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, material->metalness_tex->id);
        uniform_set_int(u, "metalnessTex", 3);
    }

    if (material->ambient_occlusion_tex) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, material->ambient_occlusion_tex->id);
        uniform_set_int(u, "aoTex", 4);
    }

    if (material->emissive_tex) {
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, material->emissive_tex->id);
        uniform_set_int(u, "emissiveTex", 5);
    }

    if (material->height_tex) {
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, material->height_tex->id);
        uniform_set_int(u, "heightTex", 6);
    }

    if (material->opacity_tex) {
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, material->opacity_tex->id);
        uniform_set_int(u, "opacityTex", 7);
    }

    if (material->sheen_tex) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, material->sheen_tex->id);
        uniform_set_int(u, "sheenTex", 8);
    }

    if (material->reflectance_tex) {
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_2D, material->reflectance_tex->id);
        uniform_set_int(u, "reflectanceTex", 9);
    }

    uniform_set_int(u, "albedoTexExists", material->albedo_tex ? 1 : 0);
    uniform_set_int(u, "normalTexExists", material->normal_tex ? 1 : 0);
    uniform_set_int(u, "roughnessTexExists", material->roughness_tex ? 1 : 0);
    uniform_set_int(u, "metalnessTexExists", material->metalness_tex ? 1 : 0);
    uniform_set_int(u, "aoTexExists", material->ambient_occlusion_tex ? 1 : 0);
    uniform_set_int(u, "emissiveTexExists", material->emissive_tex ? 1 : 0);
    uniform_set_int(u, "heightTexExists", material->height_tex ? 1 : 0);
    uniform_set_int(u, "opacityTexExists", material->opacity_tex ? 1 : 0);
    uniform_set_int(u, "sheenTexExists", material->sheen_tex ? 1 : 0);
    uniform_set_int(u, "reflectanceTexExists", material->reflectance_tex ? 1 : 0);
}

static void _update_camera_uniforms(ShaderProgram* program, Camera* camera) {
    if (!program || !program->uniforms || !camera)
        return;

    UniformManager* u = program->uniforms;
    uniform_set_vec3(u, "camPos", (const float*)&camera->position);
    uniform_set_float(u, "nearClip", camera->near_clip);
    uniform_set_float(u, "farClip", camera->far_clip);
}

static void _render_node(SceneNode* node, Camera* camera, mat4 model, mat4 view, mat4 projection,
                         float time_value, RenderMode render_mode, Light** closest_lights,
                         size_t returned_light_count, GLuint* current_program,
                         Material** current_material) {

    if (!node->meshes || node->mesh_count == 0)
        return;

    for (size_t i = 0; i < node->mesh_count; ++i) {
        Mesh* mesh = node->meshes[i];
        if (!mesh || !mesh->material)
            continue;

        Material* mat = mesh->material;
        ShaderProgram* program = mat->shader_program;
        if (!program || !program->uniforms)
            continue;

        UniformManager* u = program->uniforms;

        // Only switch program if different from current
        if (*current_program != program->id) {
            glUseProgram(program->id);
            *current_program = program->id;
            // Force material update when program changes
            *current_material = NULL;

            // Set view/projection/camera uniforms once per program switch
            uniform_set_mat4(u, "view", (const float*)view);
            uniform_set_mat4(u, "projection", (const float*)projection);
            uniform_set_float(u, "time", time_value);
            uniform_set_int(u, "renderMode", render_mode);
            _update_camera_uniforms(program, camera);

            // Update lights once per program switch for this node
            for (size_t j = 0; j < returned_light_count; ++j) {
                _update_program_light_uniforms(program, closest_lights[j], returned_light_count, j);
            }
        }

        // Per-mesh uniforms (model matrix is always per-mesh)
        uniform_set_mat4(u, "model", (const float*)node->global_transform);
        uniform_set_float(u, "lineWidth", mesh->line_width);

        // Only update material uniforms if material changed
        if (*current_material != mat) {
            _update_program_material_uniforms(program, mat);
            *current_material = mat;
        }

        glBindVertexArray(mesh->vao);
        glDrawElements(mesh->draw_mode, mesh->index_count, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }
}

static void _render_xyz(SceneNode* node, mat4 view, mat4 projection, GLuint* current_program) {
    if (!node || !node->xyz_shader_program || !node->xyz_shader_program->uniforms)
        return;

    ShaderProgram* program = node->xyz_shader_program;
    UniformManager* u = program->uniforms;

    if (*current_program != program->id) {
        glUseProgram(program->id);
        *current_program = program->id;
    }

    uniform_set_mat4(u, "model", (const float*)node->global_transform);
    uniform_set_mat4(u, "view", (const float*)view);
    uniform_set_mat4(u, "projection", (const float*)projection);

    glBindVertexArray(node->xyz_vao);
    glDrawArrays(GL_LINES, 0, xyz_vertices_size / (6 * sizeof(float)));
    glBindVertexArray(0);
}

static void _render_scene(Scene* scene, SceneNode* node, Camera* camera, mat4 model, mat4 view,
                          mat4 projection, float time_value, RenderMode render_mode,
                          GLuint* current_program, Material** current_material) {

    if (!scene) {
        log_error("error: render called with NULL scene");
        return;
    }

    if (!node) {
        log_error("error: render called with NULL root node");
        return;
    }

    size_t returned_light_count;
    size_t max_lights = get_gl_max_lights();
    Light** closest_lights = get_closest_lights(scene, node, max_lights, &returned_light_count);

    _render_node(node, camera, model, view, projection, time_value, render_mode, closest_lights,
                 returned_light_count, current_program, current_material);

    if (node->show_xyz && node->xyz_shader_program) {
        _render_xyz(node, view, projection, current_program);
    }

    // Note: closest_lights points to scene's cached array, do not free

    // Render children
    for (size_t i = 0; i < node->children_count; i++) {
        _render_scene(scene, node->children[i], camera, node->global_transform, view, projection,
                      time_value, render_mode, current_program, current_material);
    }
}

void render_current_scene(Engine* engine, float time_value) {
    if (!engine) {
        log_error("error: render called with NULL engine");
        return;
    }

    Scene* scene = get_current_scene(engine);
    if (!scene) {
        log_error("error: render called with NULL scene");
        return;
    }

    SceneNode* root_node = scene->root_node;
    if (!root_node) {
        log_error("error: render called with NULL root node");
        return;
    }

    Camera* camera = engine->camera;
    if (!camera) {
        log_error("error: render called with NULL camera");
        return;
    }

    mat4* model = &engine->model_matrix;
    mat4* view = &engine->view_matrix;
    mat4* projection = &engine->projection_matrix;

    RenderMode render_mode = engine->current_render_mode;

    // Track current program and material to avoid redundant state changes
    GLuint current_program = 0;
    Material* current_material = NULL;

    _render_scene(scene, root_node, camera, *model, *view, *projection, time_value, render_mode,
                  &current_program, &current_material);

    // Reset program state at end of frame
    glUseProgram(0);
}
