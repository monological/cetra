
#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "animation.h"
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
#include "shadow.h"
#include "intersect.h"

// Global animation state for skinned mesh rendering (set via set_render_animation_state)
static AnimationState* g_current_animation_state = NULL;

void set_render_animation_state(AnimationState* state) {
    g_current_animation_state = state;
}

AnimationState* get_render_animation_state(void) {
    return g_current_animation_state;
}

static void _update_skinning_uniforms(ShaderProgram* program, const Mesh* mesh) {
    if (!program || !program->uniforms)
        return;

    UniformManager* u = program->uniforms;

    if (mesh && mesh->is_skinned && g_current_animation_state &&
        g_current_animation_state->active_bone_count > 0) {
        uniform_set_int(u, "skinned", 1);

        // Upload bone matrices
        GLint loc = glGetUniformLocation(program->id, "boneMatrices[0]");
        if (loc >= 0) {
            glUniformMatrix4fv(loc, (GLsizei)g_current_animation_state->active_bone_count, GL_FALSE,
                               (const GLfloat*)g_current_animation_state->bone_matrices);
        }
    } else {
        uniform_set_int(u, "skinned", 0);
    }
}

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
    vec3 emissive_hdr;
    glm_vec3_scale(material->emissive, material->emissive_strength, emissive_hdr);
    // The shader treats a black factor with an emissive texture as "use the
    // texture as-is"; substitute the bare strength so it still applies there
    if (material->emissive_tex && glm_vec3_norm2(material->emissive) < 1e-8f) {
        glm_vec3_fill(emissive_hdr, material->emissive_strength);
    }
    uniform_set_vec3(u, "emissiveFactor", (const float*)&emissive_hdr);
    uniform_set_float(u, "metallic", material->metallic);
    uniform_set_float(u, "roughness", material->roughness);
    uniform_set_float(u, "ao", material->ao);
    uniform_set_float(u, "materialOpacity", material->opacity);
    uniform_set_float(u, "alphaCutoff", material->alphaCutoff);
    uniform_set_int(u, "alphaToCoverage", material->alpha_mode == ALPHA_MASK ? 1 : 0);
    uniform_set_float(u, "normalScale", material->normalScale);
    uniform_set_float(u, "aoStrength", material->aoStrength);
    uniform_set_float(u, "ior", material->ior);
    uniform_set_float(u, "filmThickness", material->filmThickness);
    uniform_set_vec2(u, "uvOffset", (const float*)&material->uvOffset);
    uniform_set_vec2(u, "uvScale", (const float*)&material->uvScale);
    uniform_set_float(u, "uvRotation", material->uvRotation);

    // Always set sampler uniforms to correct texture units (prevents stale values)
    uniform_set_int(u, "albedoTex", 0);
    uniform_set_int(u, "normalTex", 1);
    uniform_set_int(u, "roughnessTex", 2);
    uniform_set_int(u, "metalnessTex", 3);
    uniform_set_int(u, "aoTex", 4);
    uniform_set_int(u, "emissiveTex", 5);
    uniform_set_int(u, "heightTex", 6);
    uniform_set_int(u, "opacityTex", 7);
    uniform_set_int(u, "sheenTex", 8);
    uniform_set_int(u, "reflectanceTex", 9);
    uniform_set_int(u, "microsurfaceTex", 10);
    uniform_set_int(u, "anisotropyTex", 11);
    uniform_set_int(u, "subsurfaceTex", 12);

    if (material->albedo_tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, material->albedo_tex->id);
    }

    if (material->normal_tex) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, material->normal_tex->id);
    }

    if (material->roughness_tex) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, material->roughness_tex->id);
    }

    if (material->metalness_tex) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, material->metalness_tex->id);
    }

    if (material->ambient_occlusion_tex) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, material->ambient_occlusion_tex->id);
    }

    if (material->emissive_tex) {
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, material->emissive_tex->id);
    }

    if (material->height_tex) {
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, material->height_tex->id);
    }

    if (material->opacity_tex) {
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, material->opacity_tex->id);
    }

    if (material->sheen_tex) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, material->sheen_tex->id);
    }

    if (material->reflectance_tex) {
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_2D, material->reflectance_tex->id);
    }

    if (material->microsurface_tex) {
        glActiveTexture(GL_TEXTURE10);
        glBindTexture(GL_TEXTURE_2D, material->microsurface_tex->id);
    }

    if (material->anisotropy_tex) {
        glActiveTexture(GL_TEXTURE11);
        glBindTexture(GL_TEXTURE_2D, material->anisotropy_tex->id);
    }

    if (material->subsurface_scattering_tex) {
        glActiveTexture(GL_TEXTURE12);
        glBindTexture(GL_TEXTURE_2D, material->subsurface_scattering_tex->id);
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
    uniform_set_int(u, "microsurfaceTexExists", material->microsurface_tex ? 1 : 0);
    uniform_set_int(u, "anisotropyTexExists", material->anisotropy_tex ? 1 : 0);
    uniform_set_int(u, "subsurfaceTexExists", material->subsurface_scattering_tex ? 1 : 0);

    // Reset active texture unit
    glActiveTexture(GL_TEXTURE0);
}

static void _update_camera_uniforms(ShaderProgram* program, Camera* camera) {
    if (!program || !program->uniforms || !camera)
        return;

    UniformManager* u = program->uniforms;
    uniform_set_vec3(u, "camPos", (const float*)&camera->position);
    uniform_set_float(u, "nearClip", camera->near_clip);
    uniform_set_float(u, "farClip", camera->far_clip);
}

static void _render_node(const Engine* engine, Scene* scene, SceneNode* node, Camera* camera,
                         mat4 view, mat4 projection, float time_value, RenderMode render_mode,
                         Light** closest_lights, size_t returned_light_count,
                         GLuint* current_program, Material** current_material,
                         const Frustum* frustum, bool alpha_pass) {

    if (!node->meshes || node->mesh_count == 0)
        return;

    for (size_t i = 0; i < node->mesh_count; ++i) {
        Mesh* mesh = node->meshes[i];
        if (!mesh || !mesh->material)
            continue;

        // Blend materials render in the transparent pass after the skybox so
        // they composite against the real background; everything else
        // (including alpha-masked hair) renders in the opaque pass
        bool is_transparent = mesh->material->alpha_mode == ALPHA_BLEND;
        if (is_transparent != alpha_pass) {
            if (is_transparent)
                scene->transparent_mesh_count++;
            continue;
        }

        // Frustum culling: skip mesh if its AABB is completely outside the view frustum
        if (frustum && !frustum_test_aabb_transformed(frustum, mesh->aabb.min, mesh->aabb.max,
                                                      node->global_transform)) {
            continue;
        }

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
            uniform_set_float(u, "specularAAStrength", engine->specular_aa_strength);
            _update_camera_uniforms(program, camera);

            // Update lights once per program switch for this node
            for (size_t j = 0; j < returned_light_count; ++j) {
                _update_program_light_uniforms(program, closest_lights[j], returned_light_count, j);
            }

            // Bind shadow maps (always bind texture to satisfy sampler2DArray)
            if (scene && scene->shadow_system) {
                if (scene->shadow_system->active_count > 0 && scene->shadow_system->enabled) {
                    int shadow_indices[MAX_SHADOW_LIGHTS] = {-1, -1, -1};
                    for (size_t k = 0; k < returned_light_count && k < MAX_SHADOW_LIGHTS; ++k) {
                        shadow_indices[k] = closest_lights[k]->shadow_map_index;
                    }
                    bind_shadow_maps_to_program(scene->shadow_system, program, shadow_indices);
                } else {
                    // No active shadows, but still bind texture for sampler2DArray
                    glActiveTexture(GL_TEXTURE0 + SHADOW_MAP_TEXTURE_UNIT);
                    glBindTexture(GL_TEXTURE_2D_ARRAY, scene->shadow_system->shadow_map_array);
                    uniform_set_int(u, "shadowMaps", SHADOW_MAP_TEXTURE_UNIT);
                    uniform_set_int(u, "numShadowLights", 0);
                }
            } else {
                uniform_set_int(u, "numShadowLights", 0);
            }

            // Bind IBL textures if available
            if (scene && scene->ibl && scene->ibl->precomputed) {
                bind_ibl_textures(scene->ibl, program);
            } else {
                // Set IBL sampler uniforms to their designated texture units even when disabled
                // This prevents type mismatch when samplerCube defaults to unit 0 (which has 2D
                // textures)
                uniform_set_int(u, "irradianceMap", 14);
                uniform_set_int(u, "prefilteredMap", 15);
                uniform_set_int(u, "brdfLUT", 16);
                uniform_set_int(u, "iblEnabled", 0);
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

        // Update skinning uniforms for skinned meshes
        _update_skinning_uniforms(program, mesh);

        // Set mesh-specific uniforms for vertex colors and UV1
        uniform_set_int(u, "vertexColorExists", mesh->colors ? 1 : 0);
        uniform_set_int(u, "texCoords2Exists", mesh->tex_coords2 ? 1 : 0);

        // Alpha-masked materials (hair/foliage) render with alpha-to-coverage:
        // MSAA converts fractional alpha into sample coverage for soft,
        // order-independent edges, and uncovered samples stay open for the
        // skybox drawn later
        bool use_a2c = mat->alpha_mode == ALPHA_MASK;
        if (use_a2c) {
            glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        }

        // Handle double-sided materials
        if (mat->doubleSided) {
            glDisable(GL_CULL_FACE);
        }

        glBindVertexArray(mesh->vao);
        glDrawElements(mesh->draw_mode, mesh->index_count, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);

        if (mat->doubleSided) {
            glEnable(GL_CULL_FACE);
        }
        if (use_a2c) {
            glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        }
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

// Helper to ensure scene's traversal stack has enough capacity
static int _ensure_traversal_stack_capacity(Scene* scene, size_t required) {
    if (scene->traversal_stack_capacity >= required)
        return 0;

    size_t new_capacity = scene->traversal_stack_capacity;
    while (new_capacity < required)
        new_capacity *= 2;

    // Realloc each array separately to avoid dangling pointers on partial failure
    SceneNode** new_stack = realloc(scene->traversal_stack, new_capacity * sizeof(SceneNode*));
    if (!new_stack) {
        log_error("Failed to grow traversal stack");
        return -1;
    }
    scene->traversal_stack = new_stack;

    mat4* new_transforms = realloc(scene->traversal_transforms, new_capacity * sizeof(mat4));
    if (!new_transforms) {
        log_error("Failed to grow traversal transforms");
        return -1;
    }
    scene->traversal_transforms = new_transforms;

    scene->traversal_stack_capacity = new_capacity;
    return 0;
}

static void _render_scene_iterative(const Engine* engine, Scene* scene, SceneNode* root,
                                    Camera* camera, mat4 view, mat4 projection, float time_value,
                                    RenderMode render_mode, GLuint* current_program,
                                    Material** current_material, const Frustum* frustum,
                                    bool alpha_pass) {
    if (!scene) {
        log_error("error: render called with NULL scene");
        return;
    }

    if (!root) {
        log_error("error: render called with NULL root node");
        return;
    }

    // Use scene's pre-allocated traversal stack
    if (!scene->traversal_stack) {
        log_error("Scene traversal stack not initialized");
        return;
    }

    size_t stack_size = 0;
    size_t max_lights = get_gl_max_lights();

    // Push root node
    scene->traversal_stack[stack_size++] = root;

    while (stack_size > 0) {
        // Pop from stack
        SceneNode* node = scene->traversal_stack[--stack_size];

        // Get closest lights for this node
        size_t returned_light_count;
        Light** closest_lights = get_closest_lights(scene, node, max_lights, &returned_light_count);

        // Render this node's meshes
        _render_node(engine, scene, node, camera, view, projection, time_value, render_mode,
                     closest_lights, returned_light_count, current_program, current_material,
                     frustum, alpha_pass);

        // Render xyz axes if enabled (opaque pass only, to avoid duplicates)
        if (!alpha_pass && node->show_xyz && node->xyz_shader_program) {
            _render_xyz(node, view, projection, current_program);
        }

        // Push children in reverse order to maintain left-to-right traversal
        for (size_t i = node->children_count; i > 0; i--) {
            SceneNode* child = node->children[i - 1];
            if (!child)
                continue;

            // Grow stack if needed
            if (stack_size >= scene->traversal_stack_capacity) {
                if (_ensure_traversal_stack_capacity(scene, stack_size + 1) != 0) {
                    return;
                }
            }

            scene->traversal_stack[stack_size++] = child;
        }
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

    mat4* view = &engine->view_matrix;
    mat4* projection = &engine->projection_matrix;

    RenderMode render_mode = engine->current_render_mode;

    // Extract frustum from view-projection matrix for culling
    mat4 vp;
    glm_mat4_mul(*projection, *view, vp);
    Frustum frustum;
    frustum_extract_from_vp(vp, &frustum);

    // Track current program and material to avoid redundant state changes
    GLuint current_program = 0;
    Material* current_material = NULL;

    // Pass 1: opaque and alpha-masked meshes (counts skipped blend meshes).
    // The only pass that publishes the normals G-buffer; every later pass
    // (skybox, translucents, catcher, overlays) writes color only.
    engine_set_scene_draw_buffers(engine, true);
    scene->transparent_mesh_count = 0;
    _render_scene_iterative(engine, scene, root_node, camera, *view, *projection, time_value,
                            render_mode, &current_program, &current_material, &frustum, false);
    engine_set_scene_draw_buffers(engine, false);

    // Skybox after opaques (depth-tested against them at the far plane).
    // Skipped in debug render modes: those frames bypass tone mapping, and
    // the skybox shader emits linear HDR that would display uncorrected.
    if (scene->render_skybox && scene->ibl && scene->ibl->precomputed &&
        render_mode == RENDER_MODE_PBR) {
        render_skybox(scene->ibl, *view, *projection, scene->skybox_brightness,
                      scene->skybox_ground_projection, scene->skybox_gp_radius,
                      scene->skybox_gp_height);
    }

    // Pass 2: blend-mode (translucent) meshes, composited over the real
    // background. Depth writes off; not sorted back-to-front (typical models
    // have few translucent meshes, e.g. a visor). Skipped entirely when
    // pass 1 saw none.
    if (scene->transparent_mesh_count > 0) {
        current_program = 0;
        current_material = NULL;
        glDepthMask(GL_FALSE);
        _render_scene_iterative(engine, scene, root_node, camera, *view, *projection, time_value,
                                render_mode, &current_program, &current_material, &frustum, true);
        glDepthMask(GL_TRUE);
    }

    // Shadow catcher: darken the environment floor where the model blocks
    // the shadow-casting lights (drawn over the skybox, blended)
    if (scene->shadow_catcher && scene->shadow_system && scene->shadow_system->enabled &&
        scene->shadow_system->active_count > 0 && engine->shadow_catcher_program &&
        engine->catcher_vao) {
        ShaderProgram* catcher = engine->shadow_catcher_program;
        ShadowSystem* ss = scene->shadow_system;

        glUseProgram(catcher->id);
        uniform_set_mat4(catcher->uniforms, "view", (const float*)*view);
        uniform_set_mat4(catcher->uniforms, "projection", (const float*)*projection);
        uniform_set_float(catcher->uniforms, "catcherStrength", scene->shadow_catcher_strength);
        uniform_set_float(catcher->uniforms, "planeRadius", scene->skybox_gp_radius);
        uniform_set_int(catcher->uniforms, "numShadowLights", (int)ss->active_count);
        uniform_set_float(catcher->uniforms, "shadowBias", ss->casters[0].bias);

        // Weight each caster's shadow by its light's share of analytic light
        float weights[MAX_SHADOW_LIGHTS] = {0};
        float weight_total = 0.0f;
        for (size_t i = 0; i < scene->light_count; i++) {
            const Light* light = scene->lights[i];
            if (light && light->shadow_map_index >= 0 &&
                light->shadow_map_index < MAX_SHADOW_LIGHTS) {
                weights[light->shadow_map_index] = light->intensity;
                weight_total += light->intensity;
            }
        }
        for (size_t i = 0; i < ss->active_count && i < MAX_SHADOW_LIGHTS; i++) {
            char name[64];
            snprintf(name, sizeof(name), "lightSpaceMatrix[%zu]", i);
            GLint mloc = uniform_location(catcher->uniforms, name);
            if (mloc >= 0)
                glUniformMatrix4fv(mloc, 1, GL_FALSE,
                                   (const GLfloat*)ss->casters[i].light_space_matrix);

            snprintf(name, sizeof(name), "shadowLightWeight[%zu]", i);
            uniform_set_float(catcher->uniforms, name,
                              weight_total > 0.0f ? weights[i] / weight_total : 0.0f);
        }

        float texel = 1.0f / (float)ss->default_map_size;
        GLint loc = uniform_location(catcher->uniforms, "shadowTexelSize");
        if (loc >= 0)
            glUniform2f(loc, texel, texel);

        glActiveTexture(GL_TEXTURE0 + SHADOW_MAP_TEXTURE_UNIT);
        glBindTexture(GL_TEXTURE_2D_ARRAY, ss->shadow_map_array);
        uniform_set_int(catcher->uniforms, "shadowMaps", SHADOW_MAP_TEXTURE_UNIT);

        // Explicit state: blended, visible from both sides. Depth writes
        // stay ON: this is the last geometry of the frame, so its depth only
        // feeds the postfx SSAO resolve, giving the model a contact shadow
        // on the projected floor (which otherwise writes no depth)
        GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glBindVertexArray(engine->catcher_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        if (cull_was_enabled)
            glEnable(GL_CULL_FACE);
    }

    // Reset program state at end of frame
    glUseProgram(0);
}

void render_skeleton_bones(Engine* engine, Skeleton* skeleton, AnimationState* anim_state) {
    if (!engine || !engine->bone_program) {
        return;
    }

    // Need at least skeleton or anim_state
    Skeleton* skel = skeleton ? skeleton : (anim_state ? anim_state->skeleton : NULL);
    if (!skel) return;

    // Allocate for both bind pose (green) and animated pose (red)
    // Max 2 sets of bones * 2 vertices per bone * 6 floats per vertex
    float* vertices = malloc(skel->bone_count * 2 * 2 * 6 * sizeof(float));
    if (!vertices) return;

    size_t vertex_count = 0;

    // First pass: Draw BIND POSE in GREEN (from skeleton's inverse_bind_pose)
    if (skeleton) {
        // Compute bind pose global transforms by inverting inverse_bind_pose
        mat4* bind_globals = malloc(skel->bone_count * sizeof(mat4));
        if (bind_globals) {
            for (size_t i = 0; i < skel->bone_count; i++) {
                glm_mat4_inv(skel->bones[i].inverse_bind_pose, bind_globals[i]);
            }

            for (size_t i = 0; i < skel->bone_count; i++) {
                const Bone* bone = &skel->bones[i];
                if (bone->parent_index < 0) continue;

                float child_x = bind_globals[i][3][0];
                float child_y = bind_globals[i][3][1];
                float child_z = bind_globals[i][3][2];

                float parent_x = bind_globals[bone->parent_index][3][0];
                float parent_y = bind_globals[bone->parent_index][3][1];
                float parent_z = bind_globals[bone->parent_index][3][2];

                // GREEN for bind pose
                float r = 0.0f, g = 1.0f, b = 0.0f;

                vertices[vertex_count++] = parent_x;
                vertices[vertex_count++] = parent_y;
                vertices[vertex_count++] = parent_z;
                vertices[vertex_count++] = r;
                vertices[vertex_count++] = g;
                vertices[vertex_count++] = b;

                vertices[vertex_count++] = child_x;
                vertices[vertex_count++] = child_y;
                vertices[vertex_count++] = child_z;
                vertices[vertex_count++] = r;
                vertices[vertex_count++] = g;
                vertices[vertex_count++] = b;
            }
            free(bind_globals);
        }
    }

    // Second pass: Draw ANIMATED POSE in RED (when animation has been played)
    if (anim_state && anim_state->global_transforms && anim_state->current_time > 0.0f) {
        for (size_t i = 0; i < skel->bone_count; i++) {
            const Bone* bone = &skel->bones[i];
            if (bone->parent_index < 0) continue;

            float child_x = anim_state->global_transforms[i][3][0];
            float child_y = anim_state->global_transforms[i][3][1];
            float child_z = anim_state->global_transforms[i][3][2];

            float parent_x = anim_state->global_transforms[bone->parent_index][3][0];
            float parent_y = anim_state->global_transforms[bone->parent_index][3][1];
            float parent_z = anim_state->global_transforms[bone->parent_index][3][2];

            // RED for animated pose
            float r = 1.0f, g = 0.0f, b = 0.0f;

            vertices[vertex_count++] = parent_x;
            vertices[vertex_count++] = parent_y;
            vertices[vertex_count++] = parent_z;
            vertices[vertex_count++] = r;
            vertices[vertex_count++] = g;
            vertices[vertex_count++] = b;

            vertices[vertex_count++] = child_x;
            vertices[vertex_count++] = child_y;
            vertices[vertex_count++] = child_z;
            vertices[vertex_count++] = r;
            vertices[vertex_count++] = g;
            vertices[vertex_count++] = b;
        }
    }

    if (vertex_count == 0) {
        free(vertices);
        return;
    }

    // Upload to GPU and draw
    glBindVertexArray(engine->bone_line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, engine->bone_line_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_count * sizeof(float), vertices, GL_DYNAMIC_DRAW);

    // Use bone program
    glUseProgram(engine->bone_program->id);
    glUniformMatrix4fv(glGetUniformLocation(engine->bone_program->id, "view"),
                       1, GL_FALSE, (float*)engine->view_matrix);
    glUniformMatrix4fv(glGetUniformLocation(engine->bone_program->id, "projection"),
                       1, GL_FALSE, (float*)engine->projection_matrix);

    // Disable depth test for X-ray effect (bones always visible)
    glDisable(GL_DEPTH_TEST);
    glLineWidth(3.0f);
    glDrawArrays(GL_LINES, 0, (GLsizei)(vertex_count / 6));
    glEnable(GL_DEPTH_TEST);
    glLineWidth(1.0f);

    free(vertices);
}
