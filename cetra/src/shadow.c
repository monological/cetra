
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <GL/glew.h>
#include <cglm/cglm.h>

#include "program.h"
#include "uniform.h"
#include "scene.h"
#include "light.h"
#include "mesh.h"
#include "engine.h"
#include "shadow.h"
#include "render.h"
#include "animation.h"
#include "ext/log.h"

ShadowSystem* create_shadow_system(int default_map_size) {
    ShadowSystem* system = malloc(sizeof(ShadowSystem));
    if (!system) {
        log_error("Failed to allocate shadow system");
        return NULL;
    }
    memset(system, 0, sizeof(ShadowSystem));

    system->default_map_size = default_map_size;
    system->active_count = 0;
    system->ortho_size = 2000.0f;
    system->near_plane = 1.0f;
    system->far_plane = 7500.0f;
    system->shadow_map_array = 0;
    system->depth_program = NULL;
    system->initialized = false;
    system->enabled = true;
    system->pcss_enabled = false; // library default off; the app opts in
    system->pcss_softness = 1.0f;
    system->cascade_count = 1; // library default = classic single map; the app opts in
    system->allocated_cascades = 0;
    system->csm_debug = false;
    for (int i = 0; i < MAX_SHADOW_LIGHTS * SHADOW_CASCADES; i++) {
        glm_mat4_identity(system->cascade_matrices[i]);
        glm_vec4_copy((vec4){1.0f, 0.0f, 1.0f, 1.0f}, system->cascade_params[i]);
    }
    for (int i = 0; i < SHADOW_CASCADES; i++) {
        system->cascade_splits[i] = 0.0f;
    }

    for (int i = 0; i < MAX_SHADOW_LIGHTS; i++) {
        system->casters[i].initialized = false;
        system->casters[i].fbo = 0;
        system->casters[i].depth_texture = 0;
        system->casters[i].map_size = default_map_size;
        system->casters[i].bias = 0.005f;
        system->casters[i].normal_bias = 0.02f;
    }

    return system;
}

void free_shadow_system(ShadowSystem* system) {
    if (!system)
        return;

    free_shadow_map_array(system);

    for (int i = 0; i < MAX_SHADOW_LIGHTS; i++) {
        free_shadow_caster(&system->casters[i]);
    }

    free(system);
}

int init_shadow_caster(ShadowCaster* caster, int map_size) {
    if (!caster)
        return -1;

    if (caster->initialized)
        return 0;

    caster->map_size = map_size;

    glGenFramebuffers(1, &caster->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, caster->fbo);

    glGenTextures(1, &caster->depth_texture);
    glBindTexture(GL_TEXTURE_2D, caster->depth_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, map_size, map_size, 0, GL_DEPTH_COMPONENT,
                 GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float border_color[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           caster->depth_texture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Shadow framebuffer incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return -1;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    caster->initialized = true;

    return 0;
}

void free_shadow_caster(ShadowCaster* caster) {
    if (!caster || !caster->initialized)
        return;

    if (caster->fbo) {
        glDeleteFramebuffers(1, &caster->fbo);
        caster->fbo = 0;
    }
    if (caster->depth_texture) {
        glDeleteTextures(1, &caster->depth_texture);
        caster->depth_texture = 0;
    }
    caster->initialized = false;
}

int init_shadow_map_array(ShadowSystem* system) {
    if (!system)
        return -1;

    if (system->shadow_map_array != 0)
        return 0;

    int size = system->default_map_size;

    // Layers are count-strided (slot * cascade_count + cascade), sized to the
    // ACTIVE cascade count so the classic single-map default never pays the
    // 3x VRAM of a full 9-layer array; a count change rebuilds the array
    int layers = MAX_SHADOW_LIGHTS * system->cascade_count;
    glGenTextures(1, &system->shadow_map_array);
    glBindTexture(GL_TEXTURE_2D_ARRAY, system->shadow_map_array);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, size, size, layers, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float border_color[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border_color);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    glGenFramebuffers(1, &system->casters[0].fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, system->casters[0].fbo);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    system->allocated_cascades = system->cascade_count;
    system->initialized = true;
    return 0;
}

void free_shadow_map_array(ShadowSystem* system) {
    if (!system)
        return;

    if (system->shadow_map_array) {
        glDeleteTextures(1, &system->shadow_map_array);
        system->shadow_map_array = 0;
    }

    if (system->casters[0].fbo) {
        glDeleteFramebuffers(1, &system->casters[0].fbo);
        system->casters[0].fbo = 0;
    }

    system->initialized = false;
}

// caster_index is a LAYER index (slot * cascade_count + cascade)
void begin_shadow_pass(ShadowSystem* system, size_t caster_index) {
    if (!system || caster_index >= (size_t)(MAX_SHADOW_LIGHTS * SHADOW_CASCADES))
        return;

    if (!system->initialized) {
        if (init_shadow_map_array(system) != 0)
            return;
    }

    int size = system->default_map_size;

    glBindFramebuffer(GL_FRAMEBUFFER, system->casters[0].fbo);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, system->shadow_map_array, 0,
                              (GLint)caster_index);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Shadow framebuffer incomplete: 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    glViewport(0, 0, size, size);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void end_shadow_pass(ShadowSystem* system) {
    (void)system;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void compute_directional_light_space_matrix(vec3 direction, vec3 scene_center, float ortho_size,
                                            float near_plane, float far_plane, mat4 dest) {
    vec3 light_dir;
    glm_vec3_normalize_to(direction, light_dir);

    vec3 light_pos;
    glm_vec3_scale(light_dir, -far_plane * 0.5f, light_pos);
    glm_vec3_add(light_pos, scene_center, light_pos);

    vec3 up = {0.0f, 1.0f, 0.0f};
    if (fabsf(glm_vec3_dot(light_dir, up)) > 0.99f) {
        up[0] = 1.0f;
        up[1] = 0.0f;
        up[2] = 0.0f;
    }

    mat4 light_view;
    glm_lookat(light_pos, scene_center, up, light_view);

    mat4 light_projection;
    glm_ortho(-ortho_size, ortho_size, -ortho_size, ortho_size, near_plane, far_plane,
              light_projection);

    glm_mat4_mul(light_projection, light_view, dest);
}

void compute_cascade_light_space_matrix(vec3 direction, const CascadeCamera* cam,
                                        float slice_near, float slice_far, float scene_pad,
                                        int map_size, mat4 dest, vec4 out_params) {
    vec3 light_dir;
    glm_vec3_normalize_to(direction, light_dir);

    // Bounding sphere of the view slice: center on the view axis at the
    // radius-minimizing depth, radius from the far corners. Depends only on
    // fov/aspect and the split depths, never on camera pose -> the box size
    // is constant per cascade and cannot breathe as the camera moves.
    float k = tanf(cam->fov_radians * 0.5f);
    float k2 = k * k * (1.0f + cam->aspect_ratio * cam->aspect_ratio);
    float zc = 0.5f * (slice_near + slice_far) * (1.0f + k2);
    if (zc > slice_far)
        zc = slice_far;
    float dz = slice_far - zc;
    float radius = sqrtf(dz * dz + slice_far * slice_far * k2);

    vec3 center;
    glm_vec3_scale((float*)cam->forward, zc, center);
    glm_vec3_add(center, (float*)cam->position, center);

    vec3 up = {0.0f, 1.0f, 0.0f};
    if (fabsf(glm_vec3_dot(light_dir, up)) > 0.99f) {
        up[0] = 1.0f;
        up[1] = 0.0f;
        up[2] = 0.0f;
    }

    // Snap the sphere center to shadow-texel increments in light view space:
    // with the diameter constant, the box then slides in whole texels and
    // shadow edges stay put while the camera translates (Valient)
    mat4 snap_view;
    vec3 origin = {0.0f, 0.0f, 0.0f};
    glm_lookat(origin, light_dir, up, snap_view);
    vec3 center_ls;
    glm_mat4_mulv3(snap_view, center, 1.0f, center_ls);
    float texel = (2.0f * radius) / (float)map_size;
    center_ls[0] = floorf(center_ls[0] / texel) * texel;
    center_ls[1] = floorf(center_ls[1] / texel) * texel;
    mat4 inv_snap;
    glm_mat4_inv(snap_view, inv_snap);
    glm_mat4_mulv3(inv_snap, center_ls, 1.0f, center);

    // Eye pushed back past the slice by the scene pad so tall geometry
    // OUTSIDE the slice but toward the light still casts into it
    float back = radius + scene_pad;
    vec3 eye;
    glm_vec3_scale(light_dir, -back, eye);
    glm_vec3_add(eye, center, eye);
    mat4 light_view;
    glm_lookat(eye, center, up, light_view);

    float ortho_near = 0.1f;
    float ortho_far = back + radius;
    mat4 light_projection;
    glm_ortho(-radius, radius, -radius, radius, ortho_near, ortho_far, light_projection);
    glm_mat4_mul(light_projection, light_view, dest);

    out_params[0] = 2.0f * radius;
    out_params[1] = ortho_near;
    out_params[2] = ortho_far;
    out_params[3] = 1.0f; // bias scale; caller fills per cascade
}

void bind_shadow_maps_to_program(ShadowSystem* system, ShaderProgram* program,
                                 const int* shadow_light_indices) {
    if (!system || !program || !program->uniforms)
        return;

    UniformManager* u = program->uniforms;

    glActiveTexture(GL_TEXTURE0 + SHADOW_MAP_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, system->shadow_map_array);
    uniform_set_int(u, "shadowMaps", SHADOW_MAP_TEXTURE_UNIT);

    uniform_set_int(u, "numShadowLights", (int)system->active_count);

    float texel_size = 1.0f / (float)system->default_map_size;
    GLint loc = uniform_location(u, "shadowTexelSize");
    if (loc >= 0)
        glUniform2f(loc, texel_size, texel_size);

    // Scalar shadow uniforms are shared across casters; set once (the bias
    // was previously written inside the loop, where the last caster won)
    uniform_set_float(u, "shadowBias", system->casters[0].bias);

    // PCSS controls; the per-cascade ortho geometry rides in cascadeParams
    uniform_set_int(u, "pcssEnabled", system->pcss_enabled ? 1 : 0);
    uniform_set_float(u, "pcssSoftness", system->pcss_softness);

    // Cascade state. At count 1 the layer indices and matrices match the
    // classic path exactly (the byte-identity bridge)
    int cc = system->cascade_count;
    uniform_set_int(u, "cascadeCount", cc);
    uniform_set_int(u, "csmDebug", system->csm_debug ? 1 : 0);
    loc = uniform_location(u, "cascadeSplits");
    if (loc >= 0)
        glUniform4f(loc, system->cascade_splits[0], system->cascade_splits[1],
                    system->cascade_splits[2], 0.0f);

    for (size_t i = 0; i < system->active_count && i < MAX_SHADOW_LIGHTS; i++) {
        char name[64];

        for (int c = 0; c < cc; c++) {
            size_t layer = i * (size_t)cc + (size_t)c;
            snprintf(name, sizeof(name), "lightSpaceMatrix[%zu]", layer);
            loc = uniform_location(u, name);
            if (loc >= 0)
                glUniformMatrix4fv(loc, 1, GL_FALSE,
                                   (const GLfloat*)system->cascade_matrices[layer]);

            snprintf(name, sizeof(name), "cascadeParams[%zu]", layer);
            loc = uniform_location(u, name);
            if (loc >= 0)
                glUniform4fv(loc, 1, (const GLfloat*)system->cascade_params[layer]);
        }

        snprintf(name, sizeof(name), "shadowLightIndex[%zu]", i);
        uniform_set_int(u, name, shadow_light_indices ? shadow_light_indices[i] : (int)i);
    }
}

static void _render_shadow_node(SceneNode* node, ShaderProgram* program, GLuint* current_program) {
    if (!node)
        return;

    if (node->meshes && node->mesh_count > 0) {
        if (*current_program != program->id) {
            glUseProgram(program->id);
            *current_program = program->id;
        }

        uniform_set_mat4(program->uniforms, "model", (const float*)node->global_transform);

        for (size_t i = 0; i < node->mesh_count; ++i) {
            Mesh* mesh = node->meshes[i];
            if (!mesh || mesh->vao == 0)
                continue;

            // Alpha-masked materials (hair cards) are excluded from the
            // shadow map entirely: at map-texel scale (millimeters) their
            // strands can neither cast nor receive cleanly — solid-quad
            // casting draws card-shaped streaks, alpha-tested casting
            // draws strand-scale acne on whatever is beneath. Their
            // occlusion comes from the AO texture and SSAO instead.
            if (mesh->material && mesh->material->alpha_mode == ALPHA_MASK)
                continue;

            // Skin animated meshes so they cast animated shadows
            render_update_skinning_uniforms(program, mesh);

            glBindVertexArray(mesh->vao);
            glDrawElements(mesh->draw_mode, mesh->index_count, GL_UNSIGNED_INT, 0);
            glBindVertexArray(0);
        }
    }

    for (size_t i = 0; i < node->children_count; i++) {
        _render_shadow_node(node->children[i], program, current_program);
    }
}

void render_shadow_depth_pass(Engine* engine, Scene* scene) {
    if (!engine || !scene || !scene->shadow_system)
        return;

    ShadowSystem* ss = scene->shadow_system;

    // First count shadow-casting lights before doing any GL operations
    ss->active_count = 0;
    vec3 scene_center = {0.0f, 0.0f, 0.0f};

    for (size_t i = 0; i < scene->light_count && ss->active_count < MAX_SHADOW_LIGHTS; ++i) {
        Light* light = scene->lights[i];
        if (!light)
            continue;

        if (light->type == LIGHT_DIRECTIONAL && light->cast_shadows) {
            ss->active_count++;
        } else {
            light->shadow_map_index = -1;
        }
    }

    // Rebuild the array when the cascade count changed (layer count differs)
    if (ss->initialized && ss->allocated_cascades != ss->cascade_count) {
        free_shadow_map_array(ss);
    }

    // Always initialize the shadow map array texture (needed for sampler2DArray in shader)
    if (!ss->initialized) {
        if (init_shadow_map_array(ss) != 0)
            return;
    }

    // Early exit if no shadow-casting lights - but texture is already initialized
    if (ss->active_count == 0)
        return;

    // Now get the depth program for shadow rendering
    if (!ss->depth_program) {
        ss->depth_program = get_engine_shader_program_by_name(engine, "shadow_depth");
        if (!ss->depth_program) {
            return;
        }
    }

    // Cascade split depths (count > 1): the practical lambda mix of
    // logarithmic (resolution where the eye is) and uniform (coverage)
    // splits over [camera near, shadow distance]
    int cc = ss->cascade_count;
    CascadeCamera cam = {0};
    if (cc > 1) {
        const Camera* camera = engine->camera;
        glm_vec3_copy((float*)camera->position, cam.position);
        // World-space view forward from the view matrix's third row
        cam.forward[0] = -engine->view_matrix[0][2];
        cam.forward[1] = -engine->view_matrix[1][2];
        cam.forward[2] = -engine->view_matrix[2][2];
        glm_vec3_normalize(cam.forward);
        cam.fov_radians = camera->fov_radians;
        cam.aspect_ratio = camera->aspect_ratio;

        const float lambda = 0.75f;
        float cam_near = camera->near_clip;
        float shadow_dist = fminf(ss->far_plane, camera->far_clip);
        for (int c = 0; c < cc; c++) {
            float t = (float)(c + 1) / (float)cc;
            float uniform_split = cam_near + (shadow_dist - cam_near) * t;
            float log_split = cam_near * powf(shadow_dist / cam_near, t);
            ss->cascade_splits[c] = lambda * log_split + (1.0f - lambda) * uniform_split;
        }
    }

    // Fit each caster's cascades. Count 1 takes the classic scene-fit path
    // VERBATIM (the byte-identity bridge); count > 1 fits each view slice's
    // bounding sphere with texel snapping.
    size_t slot = 0;
    for (size_t i = 0; i < scene->light_count && slot < MAX_SHADOW_LIGHTS; ++i) {
        Light* light = scene->lights[i];
        if (!light)
            continue;

        if (light->type == LIGHT_DIRECTIONAL && light->cast_shadows) {
            if (cc == 1) {
                compute_directional_light_space_matrix(light->direction, scene_center,
                                                       ss->ortho_size, ss->near_plane,
                                                       ss->far_plane,
                                                       ss->cascade_matrices[slot]);
                ss->cascade_params[slot][0] = 2.0f * ss->ortho_size;
                ss->cascade_params[slot][1] = ss->near_plane;
                ss->cascade_params[slot][2] = ss->far_plane;
                ss->cascade_params[slot][3] = 1.0f;
            } else {
                float slice_near = engine->camera->near_clip;
                // The scene pad covers casters toward the light outside the
                // slice; the legacy fit's eye sat at far/2, reuse that scale
                float scene_pad = ss->far_plane * 0.5f;
                for (int c = 0; c < cc; c++) {
                    int layer = (int)slot * cc + c;
                    compute_cascade_light_space_matrix(
                        light->direction, &cam, slice_near, ss->cascade_splits[c], scene_pad,
                        ss->default_map_size, ss->cascade_matrices[layer],
                        ss->cascade_params[layer]);
                    slice_near = ss->cascade_splits[c];
                }
                // shadowBias is a 0..1-depth value tuned by apps against the
                // scene-fit map. Each cascade reinterprets 0..1 over its own
                // [near, far] and width, so normalize: undo the depth-range
                // stretch (a long-range cascade turns the same 0..1 bias into
                // more world units) and grow with the real texel size ratio
                // vs the scene-fit reference (the acne guard).
                float legacy_range = ss->far_plane - ss->near_plane;
                float legacy_width = 2.0f * ss->ortho_size;
                for (int c = 0; c < cc; c++) {
                    int layer = (int)slot * cc + c;
                    float range = ss->cascade_params[layer][2] - ss->cascade_params[layer][1];
                    ss->cascade_params[layer][3] = (legacy_range / range) *
                                                   (ss->cascade_params[layer][0] / legacy_width);
                }
            }
            light->shadow_map_index = (int)slot;
            slot++;
        }
    }

    GLint prev_viewport[4];
    glGetIntegerv(GL_VIEWPORT, prev_viewport);

    glCullFace(GL_FRONT);

    GLuint current_program = 0;
    glUseProgram(ss->depth_program->id);
    current_program = ss->depth_program->id;

    for (size_t i = 0; i < ss->active_count; ++i) {
        for (int c = 0; c < cc; ++c) {
            size_t layer = i * (size_t)cc + (size_t)c;
            begin_shadow_pass(ss, layer);

            uniform_set_mat4(ss->depth_program->uniforms, "lightSpaceMatrix",
                             (const float*)ss->cascade_matrices[layer]);

            _render_shadow_node(scene->root_node, ss->depth_program, &current_program);

            end_shadow_pass(ss);
        }
    }

    glCullFace(GL_BACK);
    glUseProgram(0);

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}

// Flatten this frame's shadow casters + their lights into postfx's fog block.
// The consumer indexes its POSTFX_FOG_MAX_LIGHTS-sized arrays by the count
// published here, so the two slot capacities must never diverge.
_Static_assert(POSTFX_FOG_MAX_LIGHTS == MAX_SHADOW_LIGHTS,
               "postfx caster mirror must match the shadow slot count");
_Static_assert(POSTFX_FOG_CASCADES == SHADOW_CASCADES,
               "postfx cascade mirror must match the shadow cascade count");

void shadow_publish_to_postfx(const Scene* scene, PostFX* fx) {
    if (!fx)
        return;

    // Publishing count 0 with a zero array handle is the single "no
    // shadowed in-scatter" state consumers rely on: a nonzero count
    // guarantees the map array and every slot below it are valid.
    ShadowSystem* ss = scene ? scene->shadow_system : NULL;
    if (!ss || !ss->enabled || ss->active_count == 0 || !ss->shadow_map_array ||
        !scene->lights) {
        fx->fog_light_count = 0;
        fx->fog_cascade_count = 1;
        fx->fog_shadow_map_array = 0;
        return;
    }

    int cc = ss->cascade_count;
    for (size_t i = 0; i < scene->light_count; i++) {
        Light* light = scene->lights[i];
        if (!light || light->shadow_map_index < 0 ||
            light->shadow_map_index >= POSTFX_FOG_MAX_LIGHTS) {
            continue;
        }
        int slot = light->shadow_map_index;
        // Layers stride by the runtime count (slot indices at count 1)
        for (int c = 0; c < cc; c++) {
            int layer = slot * cc + c;
            glm_mat4_copy(ss->cascade_matrices[layer], fx->fog_light_space[layer]);
        }
        glm_vec3_normalize_to(light->direction, fx->fog_light_dir[slot]);
        glm_vec3_scale(light->color, light->intensity, fx->fog_light_color[slot]);
    }
    fx->fog_light_count = (int)ss->active_count;
    fx->fog_cascade_count = cc;
    fx->fog_shadow_map_array = ss->shadow_map_array;
    fx->fog_shadow_bias = ss->casters[0].bias;
}
