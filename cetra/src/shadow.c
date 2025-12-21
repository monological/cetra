
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

    for (int i = 0; i < MAX_SHADOW_LIGHTS; i++) {
        system->casters[i].initialized = false;
        system->casters[i].fbo = 0;
        system->casters[i].depth_texture = 0;
        system->casters[i].map_size = default_map_size;
        system->casters[i].bias = 0.005f;
        system->casters[i].normal_bias = 0.02f;
        glm_mat4_identity(system->casters[i].light_space_matrix);
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
    glm_mat4_identity(caster->light_space_matrix);

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

    glGenTextures(1, &system->shadow_map_array);
    glBindTexture(GL_TEXTURE_2D_ARRAY, system->shadow_map_array);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, size, size, MAX_SHADOW_LIGHTS, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float border_color[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border_color);

    glGenFramebuffers(1, &system->casters[0].fbo);

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

void begin_shadow_pass(ShadowSystem* system, size_t caster_index) {
    if (!system || caster_index >= MAX_SHADOW_LIGHTS)
        return;

    if (!system->initialized) {
        if (init_shadow_map_array(system) != 0)
            return;
    }

    int size = system->default_map_size;

    glBindFramebuffer(GL_FRAMEBUFFER, system->casters[0].fbo);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, system->shadow_map_array, 0,
                              (GLint)caster_index);

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

    for (size_t i = 0; i < system->active_count && i < MAX_SHADOW_LIGHTS; i++) {
        ShadowCaster* caster = &system->casters[i];

        char name[64];

        snprintf(name, sizeof(name), "lightSpaceMatrix[%zu]", i);
        loc = uniform_location(u, name);
        if (loc >= 0)
            glUniformMatrix4fv(loc, 1, GL_FALSE, (const GLfloat*)caster->light_space_matrix);

        snprintf(name, sizeof(name), "shadowLightIndex[%zu]", i);
        uniform_set_int(u, name, shadow_light_indices ? shadow_light_indices[i] : (int)i);

        uniform_set_float(u, "shadowBias", caster->bias);
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

    if (!ss->depth_program) {
        ss->depth_program = get_engine_shader_program_by_name(engine, "shadow_depth");
        if (!ss->depth_program) {
            return;
        }
    }

    if (!ss->initialized) {
        if (init_shadow_map_array(ss) != 0)
            return;
    }

    ss->active_count = 0;
    vec3 scene_center = {0.0f, 0.0f, 0.0f};

    for (size_t i = 0; i < scene->light_count && ss->active_count < MAX_SHADOW_LIGHTS; ++i) {
        Light* light = scene->lights[i];
        if (!light)
            continue;

        if (light->type == LIGHT_DIRECTIONAL && light->cast_shadows) {
            size_t slot = ss->active_count;

            compute_directional_light_space_matrix(light->direction, scene_center, ss->ortho_size,
                                                   ss->near_plane, ss->far_plane,
                                                   ss->casters[slot].light_space_matrix);

            light->shadow_map_index = (int)slot;
            ss->active_count++;
        } else {
            light->shadow_map_index = -1;
        }
    }

    if (ss->active_count == 0)
        return;

    GLint prev_viewport[4];
    glGetIntegerv(GL_VIEWPORT, prev_viewport);

    glCullFace(GL_FRONT);

    GLuint current_program = 0;
    glUseProgram(ss->depth_program->id);
    current_program = ss->depth_program->id;

    for (size_t i = 0; i < ss->active_count; ++i) {
        begin_shadow_pass(ss, i);

        uniform_set_mat4(ss->depth_program->uniforms, "lightSpaceMatrix",
                         (const float*)ss->casters[i].light_space_matrix);

        _render_shadow_node(scene->root_node, ss->depth_program, &current_program);

        end_shadow_pass(ss);
    }

    glCullFace(GL_BACK);

    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}
