
#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "animation.h"
#include "ext/log.h"
#include "scene.h"
#include "sky.h"
#include "program.h"
#include "uniform.h"
#include "shader.h"
#include "mesh.h"
#include "material.h"
#include "mask_array.h"
#include "light.h"
#include "camera.h"
#include "common.h"
#include "engine.h"
#include "util.h"
#include "shadow.h"
#include "intersect.h"

// The 0-15 fragment texture-unit budget is one global resource whose slots are
// declared across common.h (material) + shadow.h + ibl.h (engine). Pin the whole
// ordered map here so a change to any one #define that would collide with its
// neighbour fails the build (the queried GL_MAX_TEXTURE_IMAGE_UNITS is 16, so the
// top unit must stay < 16 -- A3 relocated brdfLUT/skybox off units 16/17).
_Static_assert(TEXUNIT_MATERIAL_MAX < SHADOW_MAP_TEXTURE_UNIT,
               "material texture units overlap the shadow map array unit");
_Static_assert(SHADOW_MAP_TEXTURE_UNIT < IBL_IRRADIANCE_TEXTURE_UNIT,
               "shadow unit overlaps the IBL irradiance unit");
_Static_assert(IBL_IRRADIANCE_TEXTURE_UNIT < IBL_PREFILTER_TEXTURE_UNIT,
               "IBL irradiance overlaps prefilter unit");
_Static_assert(IBL_PREFILTER_TEXTURE_UNIT < IBL_BRDF_LUT_TEXTURE_UNIT,
               "IBL prefilter overlaps brdfLUT unit");
_Static_assert(IBL_BRDF_LUT_TEXTURE_UNIT < IBL_SKYBOX_TEXTURE_UNIT,
               "IBL brdfLUT overlaps skybox unit");
_Static_assert(IBL_SKYBOX_TEXTURE_UNIT < 16,
               "engine texture units exceed GL_MAX_TEXTURE_IMAGE_UNITS (16)");

// Global animation state for skinned mesh rendering (set via set_render_animation_state)
static AnimationState* g_current_animation_state = NULL;

void set_render_animation_state(AnimationState* state) {
    g_current_animation_state = state;
}

AnimationState* get_render_animation_state(void) {
    return g_current_animation_state;
}

void render_update_skinning_uniforms(ShaderProgram* program, const Mesh* mesh) {
    if (!program || !program->uniforms)
        return;

    UniformManager* u = program->uniforms;

    if (mesh && mesh->is_skinned && g_current_animation_state &&
        g_current_animation_state->active_bone_count > 0) {
        uniform_set_int(u, "skinned", 1);

        GLsizei count = (GLsizei)g_current_animation_state->active_bone_count;

        // Upload bone matrices
        GLint loc = uniform_location(u, "boneMatrices[0]");
        if (loc >= 0) {
            glUniformMatrix4fv(loc, count, GL_FALSE,
                               (const GLfloat*)g_current_animation_state->bone_matrices);
        }

        // Previous-frame bones for skinned motion vectors (TAA), packed once per
        // frame by animation_snapshot_prev_pose. Absent on programs without the
        // uniform (e.g. the shadow depth pass) -> skipped.
        GLint prevLoc = uniform_location(u, "uPrevBoneRows[0]");
        if (prevLoc >= 0) {
            glUniform4fv(prevLoc, count * 3, g_current_animation_state->prev_bone_rows);
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
    uniform_set_float(u, "transmission", material->transmission);
    uniform_set_float(u, "transmissionThickness", material->thickness);
    uniform_set_float(u, "filmThickness", material->filmThickness);
    uniform_set_float(u, "clearcoat", material->clearcoat);
    uniform_set_float(u, "clearcoatRoughness", material->clearcoat_roughness);
    uniform_set_float(u, "specularFactor", material->specular_factor);
    uniform_set_vec3(u, "specularColorFactor", (const float*)&material->specular_color_factor);
    uniform_set_vec2(u, "uvOffset", (const float*)&material->uvOffset);
    uniform_set_vec2(u, "uvScale", (const float*)&material->uvScale);
    uniform_set_float(u, "uvRotation", material->uvRotation);

    // Dedicated (native-resolution) sampler units. The scalar masks
    // (roughness/metallic/ao/opacity/microsurface/anisotropy/subsurface) are no
    // longer per-slot samplers -- they live in the mask sampler2DArray, bound
    // once per program in the draw loop and selected per material by layer.
    uniform_set_int(u, "albedoTex", TEXUNIT_ALBEDO);
    uniform_set_int(u, "normalTex", TEXUNIT_NORMAL);
    uniform_set_int(u, "emissiveTex", TEXUNIT_EMISSIVE);
    uniform_set_int(u, "sceneColorTex", TEXUNIT_SCENE_COLOR); // refraction source
    uniform_set_int(u, "sheenTex", TEXUNIT_SHEEN);            // reserved (unsampled)
    uniform_set_int(u, "reflectanceTex", TEXUNIT_REFLECTANCE); // reserved (unsampled)
    uniform_set_int(u, "clearcoatNormalTex", TEXUNIT_CLEARCOAT_NORMAL);

    // Per-mask layer into the mask array (-1 = no texture -> scalar factor)
    uniform_set_int(u, "roughnessLayer", material->roughness_layer);
    uniform_set_int(u, "metallicLayer", material->metallic_layer);
    uniform_set_int(u, "aoLayer", material->ao_layer);
    uniform_set_int(u, "opacityLayer", material->opacity_layer);
    uniform_set_int(u, "microsurfaceLayer", material->microsurface_layer);
    uniform_set_int(u, "anisotropyLayer", material->anisotropy_layer);
    uniform_set_int(u, "subsurfaceLayer", material->subsurface_layer);

    if (material->albedo_tex) {
        glActiveTexture(GL_TEXTURE0 + TEXUNIT_ALBEDO);
        glBindTexture(GL_TEXTURE_2D, material->albedo_tex->id);
    }

    if (material->normal_tex) {
        glActiveTexture(GL_TEXTURE0 + TEXUNIT_NORMAL);
        glBindTexture(GL_TEXTURE_2D, material->normal_tex->id);
    }

    if (material->emissive_tex) {
        glActiveTexture(GL_TEXTURE0 + TEXUNIT_EMISSIVE);
        glBindTexture(GL_TEXTURE_2D, material->emissive_tex->id);
    }

    // height_tex is deliberately NOT bound: pbr_frag declares heightTex but
    // never samples it (POM is unimplemented), and TEXUNIT_SCENE_COLOR carries
    // the refraction pass's scene-color texture. sheen/reflectance are reserved
    // (compiler-dropped until KHR sheen/specular land).
    if (material->sheen_tex) {
        glActiveTexture(GL_TEXTURE0 + TEXUNIT_SHEEN);
        glBindTexture(GL_TEXTURE_2D, material->sheen_tex->id);
    }

    if (material->reflectance_tex) {
        glActiveTexture(GL_TEXTURE0 + TEXUNIT_REFLECTANCE);
        glBindTexture(GL_TEXTURE_2D, material->reflectance_tex->id);
    }

    if (material->clearcoat_normal_tex) {
        glActiveTexture(GL_TEXTURE0 + TEXUNIT_CLEARCOAT_NORMAL);
        glBindTexture(GL_TEXTURE_2D, material->clearcoat_normal_tex->id);
    }

    uniform_set_int(u, "albedoTexExists", material->albedo_tex ? 1 : 0);
    uniform_set_int(u, "normalTexExists", material->normal_tex ? 1 : 0);
    uniform_set_int(u, "emissiveTexExists", material->emissive_tex ? 1 : 0);
    uniform_set_int(u, "heightTexExists", material->height_tex ? 1 : 0);
    uniform_set_int(u, "sheenTexExists", material->sheen_tex ? 1 : 0);
    uniform_set_int(u, "reflectanceTexExists", material->reflectance_tex ? 1 : 0);
    uniform_set_int(u, "clearcoatNormalExists", material->clearcoat_normal_tex ? 1 : 0);

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

        // Blend and transmissive materials render in the late pass after the
        // skybox so they composite against the real background; everything
        // else (including alpha-masked hair) renders in the opaque pass.
        // Transmissive meshes are counted separately: their count gates the
        // mid-frame opaque-color resolve refraction samples from.
        bool is_transmissive = mesh->material->transmission > 0.0f;
        bool is_late = mesh->material->alpha_mode == ALPHA_BLEND || is_transmissive;
        if (is_late != alpha_pass) {
            if (is_late) {
                scene->transparent_mesh_count++;
                // Frustum-gate the transmissive count: it triggers the
                // full-frame resolve, which off-screen glass must not pay
                // for (the late-pass re-traversal it shares with blend
                // meshes is cheap and keeps its pre-cull count)
                if (is_transmissive &&
                    (!frustum || frustum_test_aabb_transformed(frustum, mesh->aabb.min,
                                                               mesh->aabb.max,
                                                               node->global_transform)))
                    scene->transmissive_mesh_count++;
            }
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

            // Set view/projection/camera uniforms once per program switch.
            // projection is the jittered matrix (rasterization); the motion-vector
            // matrices are the engine's real un-jittered ones, computed once per
            // frame in render_current_scene.
            uniform_set_mat4(u, "view", (const float*)view);
            uniform_set_mat4(u, "projection", (const float*)projection);
            uniform_set_mat4(u, "uCurrViewProjNoJitter", (const float*)engine->view_proj);
            uniform_set_mat4(u, "uPrevViewProj", (const float*)engine->prev_view_proj);
            uniform_set_float(u, "time", time_value);
            uniform_set_int(u, "renderMode", render_mode);
            uniform_set_float(u, "specularAAStrength", engine->specular_aa_strength);
            uniform_set_int(u, "energyCompEnabled", engine->energy_comp_enabled ? 1 : 0);
            uniform_set_int(u, "clearcoatEnabled", engine->clearcoat_enabled ? 1 : 0);
            uniform_set_int(u, "specularEnabled", engine->specular_enabled ? 1 : 0);
            // Refraction source: valid only in the late pass, after the
            // mid-frame resolve ran (pass 2 forces a program re-switch by
            // resetting current_program, so this always re-uploads there)
            uniform_set_int(u, "sceneColorAvailable", engine->scene_color_this_frame ? 1 : 0);
            if (engine->scene_color_this_frame) {
                glActiveTexture(GL_TEXTURE6);
                glBindTexture(GL_TEXTURE_2D, engine->opaque_color_texture);
                glActiveTexture(GL_TEXTURE0);
            }
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

            // Bind the material mask array (roughness/metallic/ao/opacity/
            // microsurface/anisotropy/subsurface packed into one layered
            // texture). Always bind to satisfy the sampler2DArray; each
            // material's per-mask layer indices select or skip a layer.
            mask_array_bind(scene ? scene->mask_array : NULL, TEXUNIT_MASKS);
            uniform_set_int(u, "maskArray", TEXUNIT_MASKS);

            // Bind IBL textures if available
            if (scene && scene->ibl && scene->ibl->precomputed) {
                bind_ibl_textures(scene->ibl, program);
            } else {
                // Set IBL sampler uniforms to their designated texture units even when disabled
                // This prevents type mismatch when samplerCube defaults to unit 0 (which has 2D
                // textures)
                uniform_set_int(u, "irradianceMap", IBL_IRRADIANCE_TEXTURE_UNIT);
                uniform_set_int(u, "prefilteredMap", IBL_PREFILTER_TEXTURE_UNIT);
                uniform_set_int(u, "brdfLUT", IBL_BRDF_LUT_TEXTURE_UNIT);
                uniform_set_int(u, "iblEnabled", 0);
            }

            // Local reflection probe (parallax-corrected specular), rebinding
            // the IBL prefilter unit to the probe capture. The probe joins
            // the scene only after its capture, so the capture pass itself
            // never consumes it.
            if (scene && reflection_probe_active(scene->probe)) {
                bind_reflection_probe(scene->probe, program);
            } else {
                uniform_set_int(u, "probeEnabled", 0);
            }
        }

        // Per-mesh uniforms (model matrix is always per-mesh)
        uniform_set_mat4(u, "model", (const float*)node->global_transform);
        uniform_set_mat4(u, "uPrevModel", (const float*)node->prev_global_transform);
        uniform_set_float(u, "lineWidth", mesh->line_width);

        // Only update material uniforms if material changed
        if (*current_material != mat) {
            _update_program_material_uniforms(program, mat);
            *current_material = mat;
        }

        // Update skinning uniforms for skinned meshes
        render_update_skinning_uniforms(program, mesh);

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

// Halton low-discrepancy sequence element (1-based index), for sub-pixel jitter.
static float _halton(int index, int base) {
    float f = 1.0f;
    float r = 0.0f;
    for (int i = index; i > 0; i /= base) {
        f /= (float)base;
        r += f * (float)(i % base);
    }
    return r;
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
    mat4* projection = &engine->projection_matrix; // un-jittered

    RenderMode render_mode = engine->current_render_mode;

    // Un-jittered view-projection, computed once per frame for frustum culling
    // and motion vectors (and stashed as next frame's prev at the end). Held on
    // the engine so _render_node uploads it without recomputing per program.
    glm_mat4_mul(*projection, *view, engine->view_proj);
    Frustum frustum;
    frustum_extract_from_vp(engine->view_proj, &frustum);

    // Draw projection: the un-jittered projection, sub-pixel-jittered when TAA
    // runs so the temporal resolve accumulates coverage. Recomputed here every
    // call so every render loop — including apps that call
    // render_current_scene from their own loop — gets a correct projection.
    // Off in headless (jitter would break deterministic screenshots).
    mat4 draw_projection;
    glm_mat4_copy(*projection, draw_projection);
    if (render_mode == RENDER_MODE_PBR && postfx_taa_active(engine->postfx) && !engine->headless) {
        int j = (int)(engine->total_frames % 8) + 1;
        int rw = engine->fb_width * engine->ss_scale;
        int rh = engine->fb_height * engine->ss_scale;
        draw_projection[2][0] += (_halton(j, 2) - 0.5f) * 2.0f / (float)rw;
        draw_projection[2][1] += (_halton(j, 3) - 0.5f) * 2.0f / (float)rh;
    }
    // Published for postfx: depth-buffer reconstruction must invert the
    // projection the depth was actually rasterized with
    glm_mat4_copy(draw_projection, engine->draw_projection);

    // Track current program and material to avoid redundant state changes
    GLuint current_program = 0;
    Material* current_material = NULL;

    // Pass 1: opaque and alpha-masked meshes (counts skipped blend meshes).
    // The only pass that publishes the normals G-buffer; every later pass
    // (skybox, translucents, catcher, overlays) writes color only.
    engine_set_scene_draw_buffers(engine, true);
    scene->transparent_mesh_count = 0;
    scene->transmissive_mesh_count = 0;
    // False until this frame's resolve runs, so pass 1 never uploads or
    // binds a stale refraction source
    engine->scene_color_this_frame = false;
    _render_scene_iterative(engine, scene, root_node, camera, *view, draw_projection, time_value,
                            render_mode, &current_program, &current_material, &frustum, false);
    engine_set_scene_draw_buffers(engine, false);

    // Skybox after opaques (depth-tested against them at the far plane).
    // Skipped in debug render modes: those frames bypass tone mapping, and
    // the skybox shader emits linear HDR that would display uncorrected.
    // With the probe debug view on, the probe content replaces the skybox
    // (environment-only probes have no capture; show their prefilter source).
    if (scene->ibl && scene->ibl->precomputed && render_mode == RENDER_MODE_PBR) {
        if (scene->probe && scene->probe->debug_background) {
            render_skybox_cubemap(scene->ibl,
                                  scene->probe->cubemap ? scene->probe->cubemap
                                                        : scene->ibl->environment_cubemap,
                                  *view, draw_projection);
        } else if (scene->sky && scene->sky->enabled) {
            // Procedural sky owns the background (sky-view LUT + sun disc)
            // rather than the cubemap skybox
            sky_render_background(scene->sky, scene->ibl, *view, draw_projection);
        } else if (scene->render_skybox) {
            render_skybox(scene->ibl, *view, draw_projection, scene->skybox_brightness,
                          scene->skybox_ground_projection, scene->skybox_gp_radius,
                          scene->skybox_gp_height);
        }
    }

    // Refraction source: resolve the opaque scene (including the skybox
    // just drawn) into the mipped color texture transmissive surfaces
    // sample. Gated on this frame's count, the engine toggle, and PBR mode
    // (debug modes skip the skybox and never reach the shader branch).
    if (scene->transmissive_mesh_count > 0 && render_mode == RENDER_MODE_PBR &&
        engine->refraction_enabled) {
        engine->scene_color_this_frame = engine_resolve_opaque_color(engine);
    }

    // Pass 2: blend-mode (translucent) and transmissive meshes, composited
    // over the real background. Depth writes off; not sorted back-to-front
    // (typical models have few translucent meshes, e.g. a visor). Skipped
    // entirely when pass 1 saw none.
    if (scene->transparent_mesh_count > 0) {
        current_program = 0;
        current_material = NULL;
        glDepthMask(GL_FALSE);
        _render_scene_iterative(engine, scene, root_node, camera, *view, draw_projection, time_value,
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
        uniform_set_mat4(catcher->uniforms, "projection", (const float*)draw_projection);
        uniform_set_float(catcher->uniforms, "catcherStrength", scene->shadow_catcher_strength);
        uniform_set_float(catcher->uniforms, "planeRadius", scene->skybox_gp_radius);

        // With SSR active the floor publishes depth and the reflective
        // marker across the whole quad (surfaceMode 1 skips the unshadowed
        // discard) so the reflection march has a surface to start from
        bool ssr_floor = engine->postfx && postfx_ssr_active(engine->postfx,
                                                             engine->normals_this_frame);
        uniform_set_int(catcher->uniforms, "surfaceMode", ssr_floor ? 1 : 0);
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
        shadow_upload_cascade_uniforms(ss, catcher->uniforms);

        for (size_t i = 0; i < ss->active_count && i < MAX_SHADOW_LIGHTS; i++) {
            char name[64];
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
        // The floor writes the reflective marker only when SSR consumes it;
        // otherwise it draws color-only and leaves the normals buffer (and
        // SSAO's read of it) untouched. Must come after the blanket blend
        // enable above, which resets the indexed blend-off state this call
        // establishes for attachment 1.
        engine_set_scene_draw_buffers(engine, ssr_floor);

        glBindVertexArray(engine->catcher_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        engine_set_scene_draw_buffers(engine, false);
        if (cull_was_enabled)
            glEnable(GL_CULL_FACE);
    }

    // Reset program state at end of frame
    glUseProgram(0);

    // Remember this frame's un-jittered view-projection for next frame's motion
    // vectors. Done here (not in the engine loop) so every render path keeps it.
    glm_mat4_copy(engine->view_proj, engine->prev_view_proj);
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
