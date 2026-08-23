#include <math.h>
#include <stdlib.h>

#include "material_texture_array.h"
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
#define MATERIAL_TEXTURE_ARRAY_CAP 2048

static void material_texture_array_alloc_dummy(MaterialTextureArray* arr) {
    // A 1x1 single-layer array so material_texture_array_bind always points the unit at a
    // COMPLETE texture. The shader gates every read on layer >= 0 (so a scene
    // with no masks never samples it), but a sampler2DArray bound to a unit
    // should still reference a complete texture at draw time.
    glGenTextures(1, &arr->texture);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr->texture);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    arr->width = 1;
    arr->height = 1;
    arr->layer_count = 0;
}

MaterialTextureArray* create_material_texture_array(void) {
    MaterialTextureArray* arr = calloc(1, sizeof(MaterialTextureArray));
    if (!arr) {
        log_error("Failed to allocate material texture array");
        return NULL;
    }
    material_texture_array_alloc_dummy(arr);
    return arr;
}

void free_material_texture_array(MaterialTextureArray* arr) {
    if (!arr)
        return;
    if (arr->texture)
        glDeleteTextures(1, &arr->texture);
    if (arr->quad_vao)
        glDeleteVertexArrays(1, &arr->quad_vao);
    if (arr->quad_vbo)
        glDeleteBuffers(1, &arr->quad_vbo);
    free(arr);
}

// Most layers ONE material can contribute: six masks (roughness/metallic/ao/
// opacity/microsurface/anisotropy), two maps for each of its surface layers,
// and the splat. Bounds the unique-texture buffer, which is why it is an upper
// bound rather than a count -- dedup only ever takes the real total lower.
#define TEXTURES_PER_MATERIAL (6 + MATERIAL_MAX_LAYERS * 2 + 1)

// Find texture t's layer in the dedup list (by GL id, so a shared glTF ORM
// texture yields one layer), appending it if new. Returns -1 for an absent map.
// The list is pre-sized to the upper bound, so no growth is needed.
static int material_texture_layer_for(GLuint* ids, Texture** texs, int* count, Texture* t) {
    if (!t || t->id == 0)
        return -1;
    for (int i = 0; i < *count; i++)
        if (ids[i] == t->id)
            return i;
    ids[*count] = t->id;
    texs[*count] = t;
    return (*count)++;
}

int material_texture_array_build(MaterialTextureArray* arr, struct Scene* scene, struct Engine* engine) {
    if (!arr || !scene || !engine)
        return -1;

    // 1. Dedup the scene's unique source textures and assign each material's
    //    layer indices in one pass. The unique count is bounded by
    //    TEXTURES_PER_MATERIAL per material, so one up-front allocation suffices.
    // Decals are tenants of this array too (spec 11.73): what it holds is the
    // scene's unique per-texel material IMAGES, and a mark projected onto a
    // surface is one of those. Two apiece, an albedo and an optional surface.
    size_t bound = scene->material_count * TEXTURES_PER_MATERIAL + (size_t)DECAL_MAX * 2;
    GLuint* ids = malloc(bound * sizeof(GLuint));
    Texture** texs = malloc(bound * sizeof(Texture*));
    if (bound && (!ids || !texs)) {
        free(ids);
        free(texs);
        return -1;
    }
    int count = 0;
    for (size_t m = 0; m < scene->material_count; m++) {
        Material* mat = scene->materials[m];
        if (!mat)
            continue;
        mat->roughness_layer = material_texture_layer_for(ids, texs, &count, mat->roughness_tex);
        mat->metallic_layer = material_texture_layer_for(ids, texs, &count, mat->metalness_tex);
        mat->ao_layer = material_texture_layer_for(ids, texs, &count, mat->ambient_occlusion_tex);
        mat->opacity_layer = material_texture_layer_for(ids, texs, &count, mat->opacity_tex);
        mat->microsurface_layer = material_texture_layer_for(ids, texs, &count, mat->microsurface_tex);
        mat->anisotropy_layer = material_texture_layer_for(ids, texs, &count, mat->anisotropy_tex);

        // The layered-surface tenants. Every slot is cleared before the live
        // prefix is filled, because lowering layer_count leaves the slots above
        // it still holding their textures: walking only the prefix would leave
        // stale indices pointing into a rebuilt array, and walking all of them
        // would spend layers on maps nothing samples.
        mat->splat_layer = material_texture_layer_for(ids, texs, &count, mat->splat_tex);
        for (int i = 0; i < MATERIAL_MAX_LAYERS; i++) {
            mat->layers[i].albedo_layer = -1;
            mat->layers[i].surface_layer = -1;
        }
        int live = mat->layer_count < MATERIAL_MAX_LAYERS ? mat->layer_count : MATERIAL_MAX_LAYERS;
        for (int i = 0; i < live; i++) {
            mat->layers[i].albedo_layer =
                material_texture_layer_for(ids, texs, &count, mat->layers[i].albedo_tex);
            mat->layers[i].surface_layer =
                material_texture_layer_for(ids, texs, &count, mat->layers[i].surface_tex);
        }
    }

    // Where the materials' own textures end, so the canonical-size warning below
    // can tell whether a decal is what raised it.
    const int material_layer_count = count;

    // The decal images, after the materials and by the same dedup. Assigned even
    // for a disabled decal: `enabled` is a per-frame question the descriptor pack
    // asks, where a layer index is a property of the built array, and dropping it
    // here would make switching a decal back on need a rebuild.
    for (int d = 0; d < scene->decal_count; d++) {
        Decal* dec = &scene->decals[d];
        dec->albedo_layer = material_texture_layer_for(ids, texs, &count, dec->albedo_tex);
        dec->surface_layer = material_texture_layer_for(ids, texs, &count, dec->surface_tex);
    }

    if (count == 0) {
        free(ids);
        free(texs);
        arr->layer_count = 0; // keep the dummy array
        return 0;
    }

    if (count > engine->max_array_texture_layers) {
        log_error("material_texture_array_build: %d unique textures exceeds GL_MAX_ARRAY_TEXTURE_LAYERS (%d)",
                  count, engine->max_array_texture_layers);
        free(ids);
        free(texs);
        return -1;
    }

    // 2. Canonical size = the largest present source WIDTH by the largest source
    //    HEIGHT, capped. Smaller sources upsample; nothing downsamples below the
    //    largest (no detail loss unless a source exceeds the cap). Every layer
    //    shares these dimensions, so a mix of a large and several small sources
    //    over-allocates the small ones (memory = layers * w * h * 4 bytes);
    //    lowering MATERIAL_TEXTURE_ARRAY_CAP trades detail for VRAM if that ever bites.
    //
    //    Two dimensions rather than one square, and it is worth stating why: a
    //    non-square source forced a square at its LONGEST side, so apps/forest's
    //    leaf atlas -- 8 cells of 256 laid out along u, 2048 x 256 -- promoted
    //    every layer to 2048 x 2048 and was itself stretched eightfold vertically
    //    into a layer it had no detail for. 234.7 MB became 117.4 MB with no
    //    source downsampled and no read changed.
    //
    //    This is the one real cost of putting layered-surface maps in here, and
    //    it is why the build LOGS its size and total: a scene whose masks are
    //    256 and whose surface layers are 1024 still pays 1024 for the masks. The
    //    number is reported rather than assumed, because the coupling is
    //    invisible from either material on its own.
    int width = 1, height = 1;
    for (int i = 0; i < count; i++) {
        if (texs[i]->width > width)
            width = texs[i]->width;
        if (texs[i]->height > height)
            height = texs[i]->height;
    }
    if (width > MATERIAL_TEXTURE_ARRAY_CAP)
        width = MATERIAL_TEXTURE_ARRAY_CAP;
    if (height > MATERIAL_TEXTURE_ARRAY_CAP)
        height = MATERIAL_TEXTURE_ARRAY_CAP;

    // A decal that RAISED the canonical size is worth naming, because the cost
    // does not land on it: every other layer in the scene is promoted to hold one
    // large poster, and that is invisible from either the decal or the material
    // it inflated. Warned rather than refused -- the price is VRAM and it is
    // reported right below, where a refusal would reject the ordinary case of one
    // detailed mark in a scene of small masks.
    int mat_w = 1, mat_h = 1;
    for (int i = 0; i < material_layer_count; i++) {
        if (texs[i]->width > mat_w)
            mat_w = texs[i]->width;
        if (texs[i]->height > mat_h)
            mat_h = texs[i]->height;
    }
    if (material_layer_count > 0 && (width > mat_w || height > mat_h)) {
        for (int i = material_layer_count; i < count; i++) {
            if (texs[i]->width > mat_w || texs[i]->height > mat_h)
                log_warn("material_texture_array: decal image '%s' (%dx%d) raises every layer "
                         "from %dx%d to %dx%d",
                         texs[i]->filepath ? texs[i]->filepath : "(unnamed)", texs[i]->width,
                         texs[i]->height, mat_w, mat_h, width, height);
        }
    }

    // 3. Allocate the array (mip 0; glGenerateMipmap fills the chain after the
    //    layers are drawn -- glTexStorage3D is GL 4.2, unavailable here).
    if (arr->texture)
        glDeleteTextures(1, &arr->texture);
    glGenTextures(1, &arr->texture);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr->texture);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, width, height, count, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // 4. Draw each source mask into its layer through the mask_copy program;
    //    the source sampler's linear filter does the resample. A source already
    //    at the canonical size copies 1:1 (byte-exact).
    ShaderProgram* copy = get_engine_shader_program_by_name(engine, "mask_copy");
    if (!copy) {
        log_error("material_texture_array_build: mask_copy program missing");
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

    if (arr->quad_vao == 0)
        create_fullscreen_quad_vao(&arr->quad_vao, &arr->quad_vbo);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glUseProgram(copy->id);
    uniform_set_int(copy->uniforms, "src", 0);
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < count; i++) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arr->texture, 0, i);
        glBindTexture(GL_TEXTURE_2D, ids[i]);
        draw_fullscreen_quad(arr->quad_vao);
    }

    glBindTexture(GL_TEXTURE_2D_ARRAY, arr->texture);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

    // Restore (the quad VAO is cached on the array for the next rebuild)
    glDeleteFramebuffers(1, &fbo);
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

    arr->width = width;
    arr->height = height;
    arr->layer_count = count;
    // The 4/3 is the mip chain, which glGenerateMipmap has just filled.
    double bytes = (double)count * (double)width * (double)height * 4.0 * (4.0 / 3.0);
    log_info("Material texture array: %d layers at %dx%d, %.1f MB%s", count, width, height,
             bytes / (1024.0 * 1024.0),
             count > material_layer_count ? " (including decals)" : "");

    free(ids);
    free(texs);
    return 0;
}

void material_texture_array_bind(const MaterialTextureArray* arr, int unit) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr ? arr->texture : 0);
}

void material_texture_array_ensure_built(struct Scene* scene, struct Engine* engine) {
    if (!scene || !engine || !scene->material_textures_dirty)
        return;
    // Defer until streaming finishes so every source mask is present; the
    // materials render with their scalar factors in the meantime. Covers the
    // sync path too (loader never busy -> builds the first frame).
    if (engine->async_loader && async_loader_is_busy(engine->async_loader))
        return;
    if (!scene->material_textures)
        scene->material_textures = create_material_texture_array();
    if (scene->material_textures && material_texture_array_build(scene->material_textures, scene, engine) == 0)
        scene->material_textures_dirty = false;
}
