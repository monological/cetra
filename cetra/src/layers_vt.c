#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "layers_vt.h"
#include "scene.h"
#include "engine.h"
#include "material_texture_array.h"
#include "texture.h"
#include "program.h"
#include "uniform.h"
#include "util.h"
#include "ext/log.h"

// World units per composite texel the derived resolution aims for. The cache
// holds the MACRO -- the splat and the blend -- and forest's splat is ~2 units
// per texel, so half a unit resolves it four times over. Grain never lives
// here at any resolution; that is the runtime detail term's job.
#define LAYERS_VT_TARGET_UNITS_PER_TEXEL 0.5f
#define LAYERS_VT_RES_MIN 256
#define LAYERS_VT_RES_MAX 2048

void material_upload_layer_uniforms(const Material* material, struct UniformManager* u) {
    if (!material || !u)
        return;
    uniform_set_int(u, "layerCount", material->layer_count);
    if (material->layer_count > 0) {
        // ivec4/vec4, so these go through the CACHED setters -- which matters
        // because apps/forest draws 64 terrain tiles off one material, and an
        // uncached array upload would repeat per tile.
        int albedo_layers[4], surface_layers[4];
        float uv_scales[4];
        for (int i = 0; i < MATERIAL_MAX_LAYERS; i++) {
            albedo_layers[i] = material->layers[i].albedo_layer;
            surface_layers[i] = material->layers[i].surface_layer;
            uv_scales[i] = material->layers[i].uv_scale;
        }
        uniform_set_ivec4(u, "layerAlbedoLayer", albedo_layers);
        uniform_set_ivec4(u, "layerSurfaceLayer", surface_layers);
        uniform_set_vec4(u, "layerUvScale", uv_scales);
        uniform_set_int(u, "splatLayer", material->splat_layer);
        uniform_set_int(u, "splatSpace", (int)material->splat_space);
        const float domain[4] = {material->splat_origin[0], material->splat_origin[1],
                                 material->splat_size[0], material->splat_size[1]};
        uniform_set_vec4(u, "splatDomain", domain);
        uniform_set_float(u, "layerBlendSharpness", material->layer_blend_sharpness);
        uniform_set_float(u, "layerTriplanarSharpness", material->layer_triplanar_sharpness);
    }
}

static int vt_res_for(const Material* m, const Engine* engine) {
    int res;
    if (engine->layers_vt_res > 0) {
        // The diagnostic override skips the clamps on purpose: the coarse-atlas
        // gate arm needs resolutions the derived rule would refuse.
        res = engine->layers_vt_res;
    } else {
        float size = fmaxf(m->splat_size[0], m->splat_size[1]);
        float texels = size / LAYERS_VT_TARGET_UNITS_PER_TEXEL;
        res = LAYERS_VT_RES_MIN;
        while (res < texels && res < LAYERS_VT_RES_MAX)
            res *= 2;
    }
    if (engine->max_texture_size > 0 && res > engine->max_texture_size) {
        log_warn("layers_vt: %d exceeds GL_MAX_TEXTURE_SIZE (%d); clamped", res,
                 engine->max_texture_size);
        res = engine->max_texture_size;
    }
    return res;
}

static void vt_make_key(const Material* m, int res, MaterialLayersVtKey* key) {
    // memset first so struct padding compares equal under memcmp.
    memset(key, 0, sizeof(*key));
    key->res = res;
    key->layer_count = m->layer_count;
    key->splat_id = m->splat_tex ? m->splat_tex->id : 0;
    key->splat_layer = m->splat_layer;
    for (int i = 0; i < MATERIAL_MAX_LAYERS; i++) {
        key->albedo_ids[i] = m->layers[i].albedo_tex ? m->layers[i].albedo_tex->id : 0;
        key->surface_ids[i] = m->layers[i].surface_tex ? m->layers[i].surface_tex->id : 0;
        key->albedo_layers[i] = m->layers[i].albedo_layer;
        key->surface_layers[i] = m->layers[i].surface_layer;
        key->uv_scale[i] = m->layers[i].uv_scale;
    }
    key->blend_sharpness = m->layer_blend_sharpness;
    key->triplanar_sharpness = m->layer_triplanar_sharpness;
    key->domain[0] = m->splat_origin[0];
    key->domain[1] = m->splat_origin[1];
    key->domain[2] = m->splat_size[0];
    key->domain[3] = m->splat_size[1];
}

static GLuint vt_alloc_target(int res) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    // Mip 0 only; glGenerateMipmap fills the chain after the draw
    // (glTexStorage2D is GL 4.2, unavailable here). Linear internal format:
    // the albedo target holds STORED codes the shader decodes after the
    // detail ratio, exactly as the material texture array does.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, res, res, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // CLAMP, not REPEAT: the composite is not tiled, and clamping is what
    // makes the half-texel inset the splat read needs unnecessary here.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // The material texture array sets no anisotropy; this must. A kilometre of
    // ground at grazing incidence is the engine's worst moire case, and the
    // composite is the only texture on it once the cache is live.
    texture_set_max_anisotropy();
    return tex;
}

// Per-layer means from the material texture array's top mip -- the SAME
// resampled data the bake's frozen-grain taps read, so the runtime ratio's
// denominator matches its numerator's mip limit exactly.
static void vt_read_means(MaterialLayersVt* vt, const Material* m,
                          const MaterialTextureArray* arr) {
    for (int i = 0; i < MATERIAL_MAX_LAYERS; i++) {
        vt->mean_albedo[i][0] = 1.0f;
        vt->mean_albedo[i][1] = 1.0f;
        vt->mean_albedo[i][2] = 1.0f;
        vt->mean_rough[i] = 1.0f;
        vt->mean_ao[i] = 1.0f;
    }
    int maxdim = arr->width > arr->height ? arr->width : arr->height;
    int top = 0;
    while ((maxdim >> (top + 1)) > 0)
        top++;
    unsigned char* px = malloc((size_t)arr->layer_count * 4);
    if (!px)
        return;
    // One read for every layer at once, the water.c idiom. A bake-time stall,
    // never a per-frame one.
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr->texture);
    glGetTexImage(GL_TEXTURE_2D_ARRAY, top, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int i = 0; i < MATERIAL_MAX_LAYERS; i++) {
        int al = m->layers[i].albedo_layer;
        if (al >= 0 && al < arr->layer_count) {
            vt->mean_albedo[i][0] = (float)px[al * 4 + 0] / 255.0f;
            vt->mean_albedo[i][1] = (float)px[al * 4 + 1] / 255.0f;
            vt->mean_albedo[i][2] = (float)px[al * 4 + 2] / 255.0f;
        }
        int sl = m->layers[i].surface_layer;
        if (sl >= 0 && sl < arr->layer_count) {
            vt->mean_rough[i] = (float)px[sl * 4 + 2] / 255.0f;
            vt->mean_ao[i] = (float)px[sl * 4 + 3] / 255.0f;
        }
    }
    free(px);
}

static bool vt_bake(Material* m, struct Scene* scene, struct Engine* engine, int res) {
    MaterialLayersVt* vt = m->layers_vt;
    MaterialTextureArray* arr = scene->material_textures;
    ShaderProgram* prog = get_engine_shader_program_by_name(engine, "layers_vt_bake");
    if (!prog) {
        log_error("layers_vt: bake program missing");
        return false;
    }

    // Fresh targets before any source is bound, so the allocation cannot
    // clobber a unit a source sits on (the bake_lut_2d lesson).
    gl_delete_texture(&vt->albedo_tex);
    gl_delete_texture(&vt->surface_tex);
    vt->albedo_tex = vt_alloc_target(res);
    vt->surface_tex = vt_alloc_target(res);

    GLint prev_fbo = 0, prev_viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
    GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    // Blending must be off for the draw: the surface target's alpha is AO, and
    // an AO feeding the blend equation as source alpha makes the bake differ
    // run to run (the BRDF LUT lesson, ibl.c).
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);

    if (vt->quad_vao == 0)
        create_fullscreen_quad_vao(&vt->quad_vao, &vt->quad_vbo);

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vt->albedo_tex,
                           0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, vt->surface_tex,
                           0);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);

    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ok) {
        log_error("layers_vt: bake framebuffer incomplete at %dx%d", res, res);
    } else {
        glViewport(0, 0, res, res);
        glUseProgram(prog->id);
        material_upload_layer_uniforms(m, prog->uniforms);
        uniform_set_int(prog->uniforms, "materialArray", 0);
        uniform_set_int(prog->uniforms, "layerGrainFrozen", 1);
        material_texture_array_bind(arr, 0);
        draw_fullscreen_quad(vt->quad_vao);

        glBindTexture(GL_TEXTURE_2D, vt->albedo_tex);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, vt->surface_tex);
        glGenerateMipmap(GL_TEXTURE_2D);

        vt_read_means(vt, m, arr);
    }

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
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    if (ok) {
        vt->res = res;
        // The 4/3 is the mip chain; two targets, so the pair is 8 bytes a texel.
        double bytes = 2.0 * (double)res * (double)res * 4.0 * (4.0 / 3.0);
        log_info("Layers VT: material '%s' composite %dx%d pair, %.1f MB",
                 m->name ? m->name : "(unnamed)", res, res, bytes / (1024.0 * 1024.0));
    }
    return ok;
}

void material_layers_vt_ensure(struct Scene* scene, struct Engine* engine) {
    if (!scene || !engine || !engine->layers_vt_enabled)
        return;
    // The bake samples the material texture array, so it waits for the array
    // to be current -- one frame behind it at worst, never ahead of it.
    if (scene->material_textures_dirty || !scene->material_textures ||
        scene->material_textures->layer_count == 0)
        return;
    for (size_t i = 0; i < scene->material_count; i++) {
        Material* m = scene->materials[i];
        if (!m || m->layer_count <= 0 || m->splat_space != SPLAT_SPACE_WORLD_XZ)
            continue;
        int res = vt_res_for(m, engine);
        MaterialLayersVtKey key;
        vt_make_key(m, res, &key);
        if (m->layers_vt && m->layers_vt->baked &&
            memcmp(&key, &m->layers_vt->key, sizeof(key)) == 0)
            continue;
        if (!m->layers_vt) {
            m->layers_vt = calloc(1, sizeof(MaterialLayersVt));
            if (!m->layers_vt) {
                log_error("layers_vt: allocation failed for material '%s'",
                          m->name ? m->name : "(unnamed)");
                continue;
            }
        }
        if (vt_bake(m, scene, engine, res)) {
            m->layers_vt->key = key;
            m->layers_vt->baked = true;
        }
        // A failed bake keeps baked false and retries next frame; the shading
        // path falls back to the per-texel blend meanwhile, which is a correct
        // frame rather than a black one.
    }
}

void free_material_layers_vt(MaterialLayersVt* vt) {
    if (!vt)
        return;
    gl_delete_texture(&vt->albedo_tex);
    gl_delete_texture(&vt->surface_tex);
    if (vt->quad_vao)
        glDeleteVertexArrays(1, &vt->quad_vao);
    if (vt->quad_vbo)
        glDeleteBuffers(1, &vt->quad_vbo);
    free(vt);
}
