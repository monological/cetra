#include <math.h>
#include <stdio.h>
#include <string.h>

#include <cglm/cglm.h>

#include "cetra/material.h"
#include "cetra/mesh.h"
#include "cetra/uniform.h"
#include "cetra/util.h"

#include "item_view.h"
#include "silent_shaders.h"

#define SAMPLES   4
#define FOV       glm_rad(28.0f)
#define FILL      0.82f // of the frame the item's sphere spans, in perspective
#define FILL_FLAT 0.9f  // of the frame its box spans, flat

bool item_stage_start(ItemStage* stage) {
    memset(stage, 0, sizeof(*stage));
    stage->program = create_program_from_source("item_view", item_view_vert_shader_str,
                                                item_view_frag_shader_str, NULL);
    if (!stage->program)
        fprintf(stderr, "silent: the backpack's pictures will not build; no models shown\n");
    return stage->program != NULL;
}

static void free_samples(ItemStage* stage) {
    const GLuint renderbuffers[2] = {stage->colour, stage->depth};
    gl_delete_fbo(&stage->fbo);
    glDeleteRenderbuffers(2, renderbuffers);
    stage->colour = stage->depth = 0;
    stage->width = stage->height = 0;
}

// The four samples at least this large, made again only when a picture is larger than any before.
static bool samples(ItemStage* stage, int width, int height) {
    if (stage->fbo && width <= stage->width && height <= stage->height)
        return true;
    width = width > stage->width ? width : stage->width;
    height = height > stage->height ? height : stage->height;
    free_samples(stage);
    glGenFramebuffers(1, &stage->fbo);
    glGenRenderbuffers(1, &stage->colour);
    glGenRenderbuffers(1, &stage->depth);
    glBindFramebuffer(GL_FRAMEBUFFER, stage->fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, stage->colour);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, SAMPLES, GL_RGBA8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, stage->colour);
    glBindRenderbuffer(GL_RENDERBUFFER, stage->depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, SAMPLES, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, stage->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "silent: the backpack's pictures cannot be drawn at %dx%d\n", width,
                height);
        free_samples(stage);
        return false;
    }
    stage->width = width;
    stage->height = height;
    return true;
}

void item_stage_free(ItemStage* stage) {
    free_samples(stage);
    // Never registered with the engine, so it is the stage's to free.
    free_program(stage->program);
    memset(stage, 0, sizeof(*stage));
}

// What the draw changes beyond what a pass keeps, to be put back: it runs inside someone else's
// frame.
typedef struct GLSaved {
    GLPassState pass;
    GLint read_fbo, program, vao, active, texture, renderbuffer, depth_func, front_face;
    GLboolean depth_mask, colour_mask[4];
    GLfloat clear[4];
    GLfloat clear_depth;
} GLSaved;

static void save(GLSaved* s) {
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->texture);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &s->renderbuffer);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->colour_mask);
    glGetIntegerv(GL_DEPTH_FUNC, &s->depth_func);
    glGetIntegerv(GL_FRONT_FACE, &s->front_face);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &s->clear_depth);
    s->pass = gl_pass_begin();
}

static void restore(const GLSaved* s) {
    gl_pass_end(&s->pass);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)s->read_fbo);
    glUseProgram((GLuint)s->program);
    glBindVertexArray((GLuint)s->vao);
    glBindTexture(GL_TEXTURE_2D, (GLuint)s->texture);
    glActiveTexture((GLenum)s->active);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)s->renderbuffer);
    glDepthMask(s->depth_mask);
    glColorMask(s->colour_mask[0], s->colour_mask[1], s->colour_mask[2], s->colour_mask[3]);
    glDepthFunc((GLenum)s->depth_func);
    glFrontFace((GLenum)s->front_face);
    glClearColor(s->clear[0], s->clear[1], s->clear[2], s->clear[3]);
    glClearDepth(s->clear_depth);
}

// The picture's own target at this size, made again only when it changes; false, with nothing
// held, when it cannot be.
static bool target(ItemView* view, int width, int height) {
    if (view->fbo && view->texture.width == width && view->texture.height == height)
        return true;
    gl_delete_fbo(&view->fbo);
    gl_delete_texture(&view->texture.id);
    if (!gl_color_fbo_create(width, height, GL_RGBA8, &view->fbo, &view->texture.id)) {
        gl_delete_fbo(&view->fbo);
        gl_delete_texture(&view->texture.id);
        return false;
    }
    return true;
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

static bool same_shot(const ItemShot* a, const ItemShot* b) {
    return a->angle == b->angle && a->tilt == b->tilt && a->flat == b->flat;
}

// `model` drawn into the stage's four samples at `width` x `height` from `shot`.
static void draw(ItemStage* stage, const SceneNode* model, const AABB* box, const ItemShot* shot,
                 int width, int height) {
    vec3 centre = {0.0f, 0.0f, 0.0f}, span = {0.0f, 0.0f, 0.0f};
    glm_vec3_add((float*)box->min, (float*)box->max, centre);
    glm_vec3_scale(centre, 0.5f, centre);
    glm_vec3_sub((float*)box->max, (float*)box->min, span);
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
        seen_extent(box, centre, model_m, view_m, &across, &up);
        const float h = fmaxf(up, across / aspect) / FILL_FLAT;
        glm_ortho(-h * aspect, h * aspect, -h, h, distance - 2.0f * radius,
                  distance + 2.0f * radius, proj);
    } else {
        glm_perspective(FOV, aspect, 0.25f * distance, 4.0f * distance, proj);
    }
    // Upside down: the UI reads a texture's first row as its top, and GL writes the bottom first.
    // That mirrors the picture, so a face turned toward the eye winds clockwise on it: the face
    // the shader is told is the front is the one it sees.
    proj[1][1] = -proj[1][1];
    glm_mat4_mul(proj, view_m, view_proj);

    glBindFramebuffer(GL_FRAMEBUFFER, stage->fbo);
    glViewport(0, 0, width, height);
    glFrontFace(GL_CW);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    UniformManager* u = stage->program->uniforms;
    glUseProgram(stage->program->id);
    uniform_set_mat4(u, "model", (float*)model_m);
    uniform_set_mat4(u, "viewProj", (float*)view_proj);
    uniform_set_vec3(u, "eye", eye);
    uniform_set_int(u, "albedoTex", 0);
    for (size_t i = 0; i < model->mesh_count; i++) {
        const Mesh* m = model->meshes[i];
        const Material* mat = m->material;
        if (m->gpu_vertex_count == 0 || m->shadow_role == MESH_SHADOW_ONLY || !mat)
            continue;
        uniform_set_vec3(u, "albedo", mat->albedo);
        uniform_set_float(u, "roughness", mat->roughness);
        uniform_set_float(u, "metallic", mat->metallic);
        uniform_set_int(u, "hasAlbedoTex", mat->albedo_tex ? 1 : 0);
        uniform_set_int(u, "hasColors", m->colors ? 1 : 0);
        glBindTexture(GL_TEXTURE_2D, mat->albedo_tex ? mat->albedo_tex->id : 0);
        GLsizei count = 0;
        const void* offset = NULL;
        mesh_lod_range(m, 0, &count, &offset);
        glBindVertexArray(m->vao);
        glDrawElements(m->draw_mode, count, GL_UNSIGNED_INT, offset);
    }
}

const Texture* item_view_draw(ItemView* view, ItemStage* stage, const SceneNode* model,
                              const ItemShot* shot, int width, int height) {
    if (view->model == model && same_shot(&view->shot, shot) && view->texture.width == width &&
        view->texture.height == height)
        return view->texture.id ? &view->texture : NULL;
    view->model = model;
    view->shot = *shot;
    view->texture.width = width;
    view->texture.height = height;
    AABB box;
    if (!stage->program || !model || width <= 0 || height <= 0 || !bounds(model, &box)) {
        gl_delete_fbo(&view->fbo);
        gl_delete_texture(&view->texture.id);
        return NULL;
    }
    GLSaved saved;
    save(&saved);
    const bool ok = samples(stage, width, height) && target(view, width, height);
    if (ok) {
        draw(stage, model, &box, shot, width, height);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, stage->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, view->fbo);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
    }
    restore(&saved);
    return ok ? &view->texture : NULL;
}

void item_view_free(ItemView* view) {
    gl_delete_fbo(&view->fbo);
    gl_delete_texture(&view->texture.id);
    memset(view, 0, sizeof(*view));
}
