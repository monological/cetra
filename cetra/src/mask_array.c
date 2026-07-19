#include <math.h>
#include <stdlib.h>

#include "mask_array.h"
#include "scene.h"
#include "material.h"
#include "texture.h"
#include "engine.h"
#include "program.h"
#include "uniform.h"
#include "util.h"
#include "ext/log.h"

// Largest canonical layer size; masks bigger than this are downsampled. Keeps
// the array's memory bounded (layer_count * size^2 * 4 bytes).
#define MASK_ARRAY_CAP 2048

static void mask_array_alloc_dummy(MaterialMaskArray* arr) {
    // A 1x1 single-layer array so mask_array_bind is always valid (the shader
    // gates every read on layer >= 0, but binding a live array avoids sampling
    // an unbound unit on drivers that evaluate both ternary branches).
    glGenTextures(1, &arr->texture);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr->texture);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    arr->size = 1;
    arr->layer_count = 0;
}

MaterialMaskArray* create_material_mask_array(void) {
    MaterialMaskArray* arr = calloc(1, sizeof(MaterialMaskArray));
    if (!arr) {
        log_error("Failed to allocate material mask array");
        return NULL;
    }
    mask_array_alloc_dummy(arr);
    return arr;
}

void free_material_mask_array(MaterialMaskArray* arr) {
    if (!arr)
        return;
    if (arr->texture)
        glDeleteTextures(1, &arr->texture);
    free(arr);
}

// Find texture t's layer in the dedup list (by GL id, so a shared glTF ORM
// texture yields one layer), appending it if new. Returns -1 for an absent map.
static int mask_layer_for(GLuint** ids, Texture*** texs, int* count, int* cap, Texture* t) {
    if (!t || t->id == 0)
        return -1;
    for (int i = 0; i < *count; i++)
        if ((*ids)[i] == t->id)
            return i;
    if (*count == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        GLuint* ni = realloc(*ids, (size_t)nc * sizeof(GLuint));
        Texture** nt = realloc(*texs, (size_t)nc * sizeof(Texture*));
        if (!ni || !nt) {
            free(ni ? ni : *ids);
            free(nt ? nt : *texs);
            *ids = NULL;
            *texs = NULL;
            return -1;
        }
        *ids = ni;
        *texs = nt;
        *cap = nc;
    }
    (*ids)[*count] = t->id;
    (*texs)[*count] = t;
    return (*count)++;
}

int mask_array_build(MaterialMaskArray* arr, struct Scene* scene, struct Engine* engine) {
    if (!arr || !scene || !engine)
        return -1;

    // 1. Dedup the scene's unique mask textures and assign each material's
    //    per-mask layer indices in one pass.
    GLuint* ids = NULL;
    Texture** texs = NULL;
    int count = 0, cap = 0;
    for (size_t m = 0; m < scene->material_count; m++) {
        Material* mat = scene->materials[m];
        if (!mat)
            continue;
        mat->roughness_layer = mask_layer_for(&ids, &texs, &count, &cap, mat->roughness_tex);
        mat->metallic_layer = mask_layer_for(&ids, &texs, &count, &cap, mat->metalness_tex);
        mat->ao_layer = mask_layer_for(&ids, &texs, &count, &cap, mat->ambient_occlusion_tex);
        mat->opacity_layer = mask_layer_for(&ids, &texs, &count, &cap, mat->opacity_tex);
        mat->microsurface_layer = mask_layer_for(&ids, &texs, &count, &cap, mat->microsurface_tex);
        mat->anisotropy_layer = mask_layer_for(&ids, &texs, &count, &cap, mat->anisotropy_tex);
        mat->subsurface_layer =
            mask_layer_for(&ids, &texs, &count, &cap, mat->subsurface_scattering_tex);
    }

    if (count == 0) {
        free(ids);
        free(texs);
        arr->layer_count = 0; // keep the dummy array
        return 0;
    }

    // 2. Canonical size = the largest present mask dimension, capped. Smaller
    //    masks upsample; nothing downsamples below the largest (no detail loss
    //    unless a mask exceeds the cap).
    int size = 1;
    for (int i = 0; i < count; i++) {
        if (texs[i]->width > size)
            size = texs[i]->width;
        if (texs[i]->height > size)
            size = texs[i]->height;
    }
    if (size > MASK_ARRAY_CAP)
        size = MASK_ARRAY_CAP;

    // 3. Allocate the array (mip 0; glGenerateMipmap fills the chain after the
    //    layers are drawn -- glTexStorage3D is GL 4.2, unavailable here).
    if (arr->texture)
        glDeleteTextures(1, &arr->texture);
    glGenTextures(1, &arr->texture);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr->texture);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, size, size, count, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 NULL);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // 4. Draw each source mask into its layer through the mask_copy program;
    //    the source sampler's linear filter does the resample. A source already
    //    at the canonical size copies 1:1 (byte-exact).
    ShaderProgram* copy = get_engine_shader_program_by_name(engine, "mask_copy");
    if (!copy) {
        log_error("mask_array_build: mask_copy program missing");
        free(ids);
        free(texs);
        return -1;
    }

    GLint prev_fbo = 0, prev_viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
    GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);

    GLuint quad_vao = 0, quad_vbo = 0;
    create_fullscreen_quad_vao(&quad_vao, &quad_vbo);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, size, size);
    glUseProgram(copy->id);
    uniform_set_int(copy->uniforms, "src", 0);
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < count; i++) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arr->texture, 0, i);
        glBindTexture(GL_TEXTURE_2D, ids[i]);
        draw_fullscreen_quad(quad_vao);
    }

    glBindTexture(GL_TEXTURE_2D_ARRAY, arr->texture);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

    // Restore
    glDeleteFramebuffers(1, &fbo);
    glDeleteVertexArrays(1, &quad_vao);
    glDeleteBuffers(1, &quad_vbo);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (blend_was)
        glEnable(GL_BLEND);
    if (cull_was)
        glEnable(GL_CULL_FACE);
    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    arr->size = size;
    arr->layer_count = count;
    log_info("Material mask array: %d layers at %dx%d", count, size, size);

    free(ids);
    free(texs);
    return 0;
}

void mask_array_bind(const MaterialMaskArray* arr, int unit) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr ? arr->texture : 0);
}
