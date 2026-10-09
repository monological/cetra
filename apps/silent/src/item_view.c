#include <math.h>
#include <stdio.h>
#include <string.h>

#include <cglm/cglm.h>

#include "cetra/common.h"
#include "cetra/material.h"
#include "cetra/mesh.h"

#include "item_view.h"
#include "silent_shaders.h"

#define SAMPLES   4
#define FOV       glm_rad(28.0f)
#define FILL      0.82f // of the frame the item's sphere spans, in perspective
#define FILL_FLAT 0.9f  // of the frame its box spans, flat

ShaderProgram* create_item_view_program(void) {
    ShaderProgram* program = create_program_from_source("item_view", item_view_vert_shader_str,
                                                        item_view_frag_shader_str, NULL);
    if (!program)
        fprintf(stderr, "silent: the backpack's pictures will not build; no models shown\n");
    return program;
}

bool item_view_ready(const ItemView* view, int width, int height) {
    return view->fbo && view->width == width && view->height == height;
}

static void free_targets(ItemView* view) {
    const GLuint fbos[2] = {view->ms_fbo, view->fbo};
    const GLuint renderbuffers[2] = {view->ms_colour, view->ms_depth};
    glDeleteFramebuffers(2, fbos);
    glDeleteRenderbuffers(2, renderbuffers);
    glDeleteTextures(1, &view->colour);
    view->ms_fbo = view->fbo = view->ms_colour = view->ms_depth = view->colour = 0;
    view->width = view->height = 0;
}

// The two targets at this size, made again only when it changes.
static bool targets(ItemView* view, int width, int height) {
    if (item_view_ready(view, width, height))
        return true;
    free_targets(view);
    glGenFramebuffers(1, &view->ms_fbo);
    glGenRenderbuffers(1, &view->ms_colour);
    glGenRenderbuffers(1, &view->ms_depth);
    glBindFramebuffer(GL_FRAMEBUFFER, view->ms_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, view->ms_colour);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, SAMPLES, GL_RGBA8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              view->ms_colour);
    glBindRenderbuffer(GL_RENDERBUFFER, view->ms_depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, SAMPLES, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, view->ms_depth);
    const bool ms_ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenTextures(1, &view->colour);
    glBindTexture(GL_TEXTURE_2D, view->colour);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &view->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, view->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, view->colour, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ms_ok || !ok) {
        fprintf(stderr, "silent: the backpack's turntable cannot make a %dx%d picture\n", width,
                height);
        free_targets(view);
        return false;
    }
    view->width = width;
    view->height = height;
    view->texture.id = view->colour;
    view->texture.width = width;
    view->texture.height = height;
    return true;
}

// What the draw changes, to be put back: it runs inside someone else's frame.
typedef struct GLSaved {
    GLint draw_fbo, read_fbo, viewport[4], program, vao, active, texture, renderbuffer;
    GLboolean depth_test, cull, blend, scissor, depth_mask, colour_mask[4];
    GLint depth_func;
    GLfloat colour[4], clear[4];
    GLfloat clear_depth;
} GLSaved;

static void save(GLSaved* s) {
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read_fbo);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->texture);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &s->renderbuffer);
    s->depth_test = glIsEnabled(GL_DEPTH_TEST);
    s->cull = glIsEnabled(GL_CULL_FACE);
    s->blend = glIsEnabled(GL_BLEND);
    s->scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->colour_mask);
    glGetIntegerv(GL_DEPTH_FUNC, &s->depth_func);
    glGetVertexAttribfv(GL_ATTR_COLOR, GL_CURRENT_VERTEX_ATTRIB, s->colour);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &s->clear_depth);
}

static void enable(GLenum cap, GLboolean on) {
    if (on)
        glEnable(cap);
    else
        glDisable(cap);
}

static void restore(const GLSaved* s) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)s->read_fbo);
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    glUseProgram((GLuint)s->program);
    glBindVertexArray((GLuint)s->vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)s->texture);
    glActiveTexture((GLenum)s->active);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)s->renderbuffer);
    enable(GL_DEPTH_TEST, s->depth_test);
    enable(GL_CULL_FACE, s->cull);
    enable(GL_BLEND, s->blend);
    enable(GL_SCISSOR_TEST, s->scissor);
    glDepthMask(s->depth_mask);
    glColorMask(s->colour_mask[0], s->colour_mask[1], s->colour_mask[2], s->colour_mask[3]);
    glDepthFunc((GLenum)s->depth_func);
    glVertexAttrib4fv(GL_ATTR_COLOR, s->colour);
    glClearColor(s->clear[0], s->clear[1], s->clear[2], s->clear[3]);
    glClearDepth(s->clear_depth);
}

// The model's meshes' box, in its own frame; false for a model with nothing drawn.
static bool bounds(const SceneNode* model, AABB* box) {
    aabb_empty(box);
    for (size_t i = 0; i < model->mesh_count; i++) {
        const Mesh* m = model->meshes[i];
        if (m->gpu_vertex_count == 0 || m->shadow_role == MESH_SHADOW_ONLY)
            continue;
        aabb_union(box, &m->aabb);
    }
    return !aabb_is_empty(box);
}

static void uniform_int(GLuint program, const char* name, int value) {
    glUniform1i(glGetUniformLocation(program, name), value);
}

static void uniform_float(GLuint program, const char* name, float value) {
    glUniform1f(glGetUniformLocation(program, name), value);
}

// How far across and up the box reaches from its middle, seen through `model_m` and `view_m`.
static void seen_extent(const AABB* box, const vec3 centre, mat4 model_m, mat4 view_m,
                        float* across, float* up) {
    mat4 mv;
    glm_mat4_mul(view_m, model_m, mv);
    vec3 middle = {0.0f, 0.0f, 0.0f};
    glm_mat4_mulv3(mv, (float*)centre, 1.0f, middle);
    *across = *up = 0.0f;
    for (int i = 0; i < 8; i++) {
        const vec3 corner = {(i & 1) ? box->max[0] : box->min[0],
                             (i & 2) ? box->max[1] : box->min[1],
                             (i & 4) ? box->max[2] : box->min[2]};
        vec3 at = {0.0f, 0.0f, 0.0f};
        glm_mat4_mulv3(mv, (float*)corner, 1.0f, at);
        *across = fmaxf(*across, fabsf(at[0] - middle[0]));
        *up = fmaxf(*up, fabsf(at[1] - middle[1]));
    }
}

const Texture* item_view_draw(ItemView* view, ShaderProgram* program, const SceneNode* model,
                              const ItemShot* shot, int width, int height) {
    AABB box;
    if (!program || !model || !shot || width <= 0 || height <= 0 || !bounds(model, &box))
        return NULL;
    GLSaved saved;
    save(&saved);
    if (!targets(view, width, height)) {
        restore(&saved);
        return NULL;
    }

    vec3 centre = {0.0f, 0.0f, 0.0f}, span = {0.0f, 0.0f, 0.0f};
    glm_vec3_add(box.min, box.max, centre);
    glm_vec3_scale(centre, 0.5f, centre);
    glm_vec3_sub(box.max, box.min, span);
    const float radius = 0.5f * glm_vec3_norm(span);
    const float aspect = (float)width / (float)height;
    // In perspective, framed from the sphere round its box, so a long thing turning end-on stays
    // in the frame; flat, from as far off as the sphere needs to clear the near plane.
    const float half = 0.5f * FOV * fminf(aspect, 1.0f);
    const float distance = shot->flat ? 3.0f * radius : radius / (FILL * sinf(half));
    const vec3 eye = {0.0f, distance * sinf(shot->tilt), distance * cosf(shot->tilt)};
    mat4 model_m, view_m, proj, view_proj;
    glm_rotate_make(model_m, shot->angle, (vec3){0.0f, 1.0f, 0.0f});
    glm_translate(model_m, (vec3){-centre[0], -centre[1], -centre[2]});
    glm_lookat((float*)eye, (vec3){0.0f, 0.0f, 0.0f}, (vec3){0.0f, 1.0f, 0.0f}, view_m);
    if (shot->flat) {
        float across = 0.0f, up = 0.0f;
        seen_extent(&box, centre, model_m, view_m, &across, &up);
        const float h = fmaxf(up, across / aspect) / FILL_FLAT;
        glm_ortho(-h * aspect, h * aspect, -h, h, distance - 2.0f * radius,
                  distance + 2.0f * radius, proj);
    } else {
        glm_perspective(FOV, aspect, 0.25f * distance, 4.0f * distance, proj);
    }
    // Upside down: the UI reads a texture's first row as its top, and GL writes the bottom first.
    proj[1][1] = -proj[1][1];
    glm_mat4_mul(proj, view_m, view_proj);

    glBindFramebuffer(GL_FRAMEBUFFER, view->ms_fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const GLuint p = program->id;
    glUseProgram(p);
    glUniformMatrix4fv(glGetUniformLocation(p, "model"), 1, GL_FALSE, (float*)model_m);
    glUniformMatrix4fv(glGetUniformLocation(p, "viewProj"), 1, GL_FALSE, (float*)view_proj);
    glUniform3fv(glGetUniformLocation(p, "eye"), 1, eye);
    uniform_int(p, "albedoTex", 0);
    glActiveTexture(GL_TEXTURE0);
    for (size_t i = 0; i < model->mesh_count; i++) {
        const Mesh* m = model->meshes[i];
        const Material* mat = m->material;
        if (m->gpu_vertex_count == 0 || m->shadow_role == MESH_SHADOW_ONLY || !mat)
            continue;
        glUniform3fv(glGetUniformLocation(p, "albedo"), 1, mat->albedo);
        uniform_float(p, "roughness", mat->roughness);
        uniform_float(p, "metallic", mat->metallic);
        uniform_int(p, "hasAlbedoTex", mat->albedo_tex ? 1 : 0);
        glBindTexture(GL_TEXTURE_2D, mat->albedo_tex ? mat->albedo_tex->id : 0);
        // A mesh with no colours has the attribute switched off, and reads this.
        glVertexAttrib4f(GL_ATTR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);
        glBindVertexArray(m->vao);
        glDrawElements(GL_TRIANGLES, (GLsizei)m->index_count, GL_UNSIGNED_INT, NULL);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, view->ms_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, view->fbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    restore(&saved);
    return &view->texture;
}

void item_view_free(ItemView* view) {
    free_targets(view);
    memset(view, 0, sizeof(*view));
}
