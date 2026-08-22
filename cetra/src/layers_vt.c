#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "layers_vt.h"
#include "scene.h"
#include "engine.h"
#include "draw_list.h"
#include "intersect.h"
#include "mesh.h"
#include "material_texture_array.h"
#include "texture.h"
#include "program.h"
#include "ubo.h"
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
        // Once: this runs per frame from the ensure's key build, and a clamped
        // override is a healthy steady state, not a condition to re-report.
        static bool warned;
        if (!warned) {
            log_warn("layers_vt: %d exceeds GL_MAX_TEXTURE_SIZE (%d); clamped", res,
                     engine->max_texture_size);
            warned = true;
        }
        res = engine->max_texture_size;
    }
    return res;
}

static void vt_make_key(const Material* m, int res, const MaterialTextureArray* arr,
                        MaterialLayersVtKey* key) {
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
    // The array's canonical size, because the bake read THROUGH the array: a
    // rebuild that changes it while ids and indices survive resamples every
    // tap and every top-mip mean -- the same hole ids-beside-indices closes
    // one level down.
    key->arr_width = arr->width;
    key->arr_height = arr->height;
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

/*
 * The paged near-field atlas (spec 11.67).
 *
 * Page texel density is the fallback's times VT_PAGE_DENSITY_RATIO -- a ratio,
 * never an absolute span, which is what bounds the virtual grid at
 * VT_PAGE_GRID_MAX for every domain size: the fallback resolution clamps at
 * 2048, so a larger domain gets coarser pages exactly as it gets a coarser
 * fallback, and the table never overflows.
 */
static void vt_page_config(MaterialLayersVt* vt, const Material* m, int res) {
    float size = fmaxf(m->splat_size[0], m->splat_size[1]);
    if (size <= 0.0f || res <= 0) {
        vt->page_grid = 0;
        return;
    }
    vt->page_texel = (size / (float)res) / (float)VT_PAGE_DENSITY_RATIO;
    vt->page_span = (float)VT_PAGE_USABLE * vt->page_texel;
    int grid = (int)ceilf(size / vt->page_span);
    if (grid < 1)
        grid = 1;
    // Reachable only through the diagnostic res override, which skips the
    // derived rule's clamps on purpose; the table must not follow it past
    // capacity.
    if (grid > VT_PAGE_GRID_MAX)
        grid = VT_PAGE_GRID_MAX;
    vt->page_grid = grid;
}

static void vt_pages_reset(MaterialLayersVt* vt) {
    for (int i = 0; i < VT_PAGE_TABLE_MAX; i++)
        vt->page_table[i] = -1;
    for (int i = 0; i < VT_PAGE_SLOTS; i++)
        vt->slot_page[i] = -1;
    vt->pages_dirty = true;
}

static GLuint vt_alloc_page_atlas(void) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, VT_ATLAS_TEXELS, VT_ATLAS_TEXELS, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Mips CAPPED, not absent: page mip VT_PAGE_MIP_CAP is the same macro at
    // the same effective density as fallback mip 0, so the handoff band blends
    // near-identical signals -- and the 4-texel gutters bound trilinear reach
    // at that level. Mip-free was reviewed and rejected (unmipped minification
    // shimmers on exactly the content pages carry). No anisotropy: the paged
    // zone is the magnified near field; grazing minification belongs to the
    // fallback, which keeps its full chain and its aniso.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, VT_PAGE_MIP_CAP);
    return tex;
}

// The world rect one page TILE covers, gutter included -- the rect the bake
// draws and the inset the shader's read undoes.
static void vt_page_rect(const MaterialLayersVt* vt, const Material* m, int vpage,
                         float rect[4]) {
    int vx = vpage % vt->page_grid;
    int vz = vpage / vt->page_grid;
    rect[0] = m->splat_origin[0] + (float)vx * vt->page_span
              - (float)VT_PAGE_GUTTER * vt->page_texel;
    rect[1] = m->splat_origin[1] + (float)vz * vt->page_span
              - (float)VT_PAGE_GUTTER * vt->page_texel;
    rect[2] = (float)VT_PAGE_TEXELS * vt->page_texel;
    rect[3] = rect[2];
}

/*
 * Bake a batch of pages into their atlas slots: one GL-state bracket, one
 * viewport draw per page (the GI-volume idiom -- the gutter ring is WRITTEN by
 * the pass covering the bordered tile), one capped glGenerateMipmap for the
 * whole atlas after. Alignment makes the mip box filter bleed-free.
 */
static bool vt_bake_pages(Material* m, struct Scene* scene, struct Engine* engine,
                          const int* vpages, const int* slots, int count) {
    MaterialLayersVt* vt = m->layers_vt;
    MaterialTextureArray* arr = scene->material_textures;
    ShaderProgram* prog = get_engine_shader_program_by_name(engine, "layers_vt_bake");
    if (!prog || count <= 0)
        return false;

    glActiveTexture(GL_TEXTURE0);
    if (vt->page_albedo_tex == 0) {
        vt->page_albedo_tex = vt_alloc_page_atlas();
        vt->page_surface_tex = vt_alloc_page_atlas();
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

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           vt->page_albedo_tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           vt->page_surface_tex, 0);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);

    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glUseProgram(prog->id);
        material_upload_layer_uniforms(m, prog->uniforms);
        uniform_set_int(prog->uniforms, "materialArray", 0);
        uniform_set_int(prog->uniforms, "layerGrainFrozen", 1);
        material_texture_array_bind(arr, 0);
        for (int i = 0; i < count; i++) {
            int ox = (slots[i] % VT_ATLAS_PAGES_PER_ROW) * VT_PAGE_TEXELS;
            int oy = (slots[i] / VT_ATLAS_PAGES_PER_ROW) * VT_PAGE_TEXELS;
            float rect[4];
            vt_page_rect(vt, m, vpages[i], rect);
            glViewport(ox, oy, VT_PAGE_TEXELS, VT_PAGE_TEXELS);
            uniform_set_vec4(prog->uniforms, "vtBakeRect", rect);
            draw_fullscreen_quad(arr->quad_vao);
        }
        glBindTexture(GL_TEXTURE_2D, vt->page_albedo_tex);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, vt->page_surface_tex);
        glGenerateMipmap(GL_TEXTURE_2D);
    } else {
        log_error("layers_vt: page bake framebuffer incomplete");
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
    return ok;
}

static void vt_pages_upload(const MaterialLayersVt* vt, struct Engine* engine);

// Conservative vertical extent for a page's frustum box. The material knows
// its XZ rectangle and nothing about the terrain's heights, so the box is tall
// on purpose: the cost of the slack is a page the frustum would have rejected
// vertically staying a candidate, which the capacity clamp absorbs.
#define VT_PAGE_Y_SPAN 2048.0f

// Ceiling on bakes per frame, sizing the batch arrays; the budget flag clamps
// to it rather than growing them.
#define VT_PAGE_BAKE_MAX 8

// Eviction hysteresis, squared: a resident page yields its slot only to an
// incoming page meaningfully nearer, or the truncation edge of the wanted set
// re-decides its last slots every frame the camera breathes (the regions
// idiom's 1.2, applied where the capacity clamp bites).
#define VT_PAGE_EVICT_HYST_SQ 1.44f

static float vt_page_dist2(const MaterialLayersVt* vt, const Material* m,
                           const struct Scene* scene, int vpage, const float* cam) {
    float rect[4];
    vt_page_rect(vt, m, vpage, rect);
    float cx = rect[0] - scene->world_origin[0] + 0.5f * rect[2];
    float cz = rect[1] - scene->world_origin[2] + 0.5f * rect[3];
    float dx = cam[0] - cx;
    float dz = cam[2] - cz;
    return dx * dx + dz * dz;
}

static uint32_t vt_pages_digest(const MaterialLayersVt* vt) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < VT_PAGE_TABLE_MAX; i++) {
        h ^= (uint32_t)(uint16_t)vt->page_table[i];
        h *= 16777619u;
    }
    return h;
}

typedef struct VtPageCand {
    float d2;
    int vpage;
    int seen; // 1 = the feedback pass rasterized this page (frame N-RING)
} VtPageCand;

static int vt_cand_cmp(const void* a, const void* b) {
    const VtPageCand* ca = a;
    const VtPageCand* cb = b;
    // Feedback first: a page actually SEEN outranks one the frustum merely
    // admits -- which is occlusion-awareness, since a page behind a hill
    // rasterizes nothing and casts no vote. Then distance, then id, so the
    // order is total and two runs sort identically.
    if (ca->seen != cb->seen)
        return cb->seen - ca->seen;
    if (ca->d2 != cb->d2)
        return ca->d2 < cb->d2 ? -1 : 1;
    return ca->vpage - cb->vpage;
}

/*
 * Residency (spec 11.67): the wanted set is CPU frustum PREDICTION -- pages
 * whose rect intersects the camera frustum, nearest first -- later unioned
 * with GPU feedback. Runs on frame N-1's pose, deliberately: the camera
 * finalizes in the app's render hook, after this. One frame of lag, absorbed
 * by the eviction hysteresis and by the fallback, which renders a missing page
 * softer, never wrong.
 *
 * Page rects are AUTHORED space and the frustum is STORAGE; the conversion
 * (authored - world_origin) happens here and only here, and ensure runs after
 * engine_apply_origin_shift, so a shift frame sees the post-shift camera
 * against fixed authored rects.
 */
static void vt_pages_update(Material* m, struct Scene* scene, struct Engine* engine) {
    MaterialLayersVt* vt = m->layers_vt;
    if (!engine->layers_vt_pages_enabled || vt->page_grid <= 0 || !engine->camera)
        return;
    int slots = VT_PAGE_SLOTS;
    if (engine->layers_vt_page_slots > 0 && engine->layers_vt_page_slots < VT_PAGE_SLOTS)
        slots = engine->layers_vt_page_slots;
    int budget = engine->layers_vt_page_budget > 0 ? engine->layers_vt_page_budget : 2;
    if (budget > VT_PAGE_BAKE_MAX)
        budget = VT_PAGE_BAKE_MAX;

    Frustum fr;
    frustum_extract_from_vp(engine->view_proj, &fr);
    const float cam[3] = {engine->camera->position[0], engine->camera->position[1],
                          engine->camera->position[2]};

    VtPageCand cands[VT_PAGE_TABLE_MAX] = {{0.0f, 0}};
    int nc = 0;
    int total = vt->page_grid * vt->page_grid;
    mat4 ident = GLM_MAT4_IDENTITY_INIT;
    for (int v = 0; v < total; v++) {
        float rect[4];
        vt_page_rect(vt, m, v, rect);
        float ox = rect[0] - scene->world_origin[0];
        float oz = rect[1] - scene->world_origin[2];
        vec3 mn = {ox, -VT_PAGE_Y_SPAN, oz};
        vec3 mx = {ox + rect[2], VT_PAGE_Y_SPAN, oz + rect[3]};
        if (!frustum_test_aabb_transformed(&fr, mn, mx, ident))
            continue;
        cands[nc].d2 = vt_page_dist2(vt, m, scene, v, cam);
        cands[nc].vpage = v;
        cands[nc].seen = engine->vt_feedback && engine->vt_feedback->have &&
                                 engine->vt_feedback->requested[v]
                             ? 1
                             : 0;
        nc++;
    }
    qsort(cands, (size_t)nc, sizeof(VtPageCand), vt_cand_cmp);

    // The CAPACITY CLAMP is the governor, not a screen-density bound: the
    // frustum admits far more pages than the atlas holds, so the first
    // `slots` by (distance, id) are the want.
    int want_n = nc < slots ? nc : slots;
    bool in_want[VT_PAGE_TABLE_MAX] = {false};
    for (int i = 0; i < want_n; i++)
        in_want[cands[i].vpage] = true;

    int bake_v[VT_PAGE_BAKE_MAX], bake_s[VT_PAGE_BAKE_MAX];
    int nb = 0;
    for (int i = 0; i < want_n && nb < budget; i++) {
        int v = cands[i].vpage;
        if (vt->page_table[v] >= 0)
            continue; // already resident
        int slot = -1;
        for (int s2 = 0; s2 < slots; s2++) {
            if (vt->slot_page[s2] < 0) {
                slot = s2;
                break;
            }
        }
        if (slot < 0) {
            // Evict the farthest resident page outside the want, and only for
            // an incoming page nearer by the hysteresis margin.
            float worst = cands[i].d2 * VT_PAGE_EVICT_HYST_SQ;
            int worst_slot = -1;
            for (int s2 = 0; s2 < slots; s2++) {
                int rv = vt->slot_page[s2];
                if (rv < 0 || in_want[rv])
                    continue;
                float d2 = vt_page_dist2(vt, m, scene, rv, cam);
                if (d2 > worst) {
                    worst = d2;
                    worst_slot = s2;
                }
            }
            if (worst_slot < 0)
                break; // nothing yields this frame; nearer wants retry later
            vt->page_table[vt->slot_page[worst_slot]] = -1;
            vt->slot_page[worst_slot] = -1;
            vt->pages_evicted++;
            slot = worst_slot; // pages_dirty follows with the assignment below
        }
        vt->slot_page[slot] = (int16_t)v;
        vt->page_table[v] = (int16_t)slot;
        vt->pages_dirty = true;
        bake_v[nb] = v;
        bake_s[nb] = slot;
        nb++;
    }

    if (nb > 0) {
        if (vt_bake_pages(m, scene, engine, bake_v, bake_s, nb)) {
            vt->pages_loaded += (unsigned long long)nb;
        } else {
            // Roll the table back: it must never point at a slot the bake did
            // not fill. The fallback covers those pixels next frame.
            for (int i = 0; i < nb; i++) {
                vt->page_table[bake_v[i]] = -1;
                vt->slot_page[bake_s[i]] = -1;
            }
        }
    }

    if (vt->pages_dirty) {
        vt_pages_upload(vt, engine);
        vt->pages_dirty = false;
    }

    if (engine->layers_vt_probe_interval > 0 &&
        engine->total_frames % (size_t)engine->layers_vt_probe_interval == 0) {
        int resident = 0;
        for (int s2 = 0; s2 < VT_PAGE_SLOTS; s2++)
            if (vt->slot_page[s2] >= 0)
                resident++;
        int requested = 0;
        if (engine->vt_feedback && engine->vt_feedback->have)
            for (int v2 = 0; v2 < VT_PAGE_TABLE_MAX; v2++)
                requested += engine->vt_feedback->requested[v2];
        printf("layers-vt-probe frame=%zu grid=%d slots=%d resident=%d wanted=%d "
               "requested=%d loaded=%llu evicted=%llu digest=%08x\n",
               engine->total_frames, vt->page_grid, slots, resident, want_n, requested,
               vt->pages_loaded, vt->pages_evicted, vt_pages_digest(vt));
    }
}

// Pack and upload the page table. Called at the end of ensure when residency
// changed; residency has one writer, so a plain dirty flag suffices where the
// multi-writer IES table needed a revision.
static void vt_pages_upload(const MaterialLayersVt* vt, struct Engine* engine) {
    if (!engine->vt_pages_ubo)
        return;
    GpuVtPageBlock block;
    memset(&block, 0, sizeof(block));
    block.info[0] = vt->page_grid;
    block.info[1] = VT_ATLAS_PAGES_PER_ROW;
    block.info[2] = VT_PAGE_TEXELS;
    block.info[3] = VT_PAGE_GUTTER;
    block.params[0] = (float)VT_ATLAS_TEXELS;
    block.params[1] = vt->page_span;
    for (int i = 0; i < VT_PAGE_TABLE_MAX; i++)
        block.entries[i >> 2][i & 3] = vt->page_table[i];
    ubo_upload(engine->vt_pages_ubo, &block, sizeof(block));
}

static bool vt_bake(Material* m, struct Scene* scene, struct Engine* engine, int res) {
    MaterialLayersVt* vt = m->layers_vt;
    MaterialTextureArray* arr = scene->material_textures;
    ShaderProgram* prog = get_engine_shader_program_by_name(engine, "layers_vt_bake");
    if (!prog) {
        log_error("layers_vt: bake program missing");
        return false;
    }

    // Unit 0 first: glBindTexture binds on whatever unit the shadow pass or
    // the async uploads left active, and the trailing cleanup resets only
    // unit 0 -- the water.c data-texture lesson.
    glActiveTexture(GL_TEXTURE0);
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
        const float full_rect[4] = {m->splat_origin[0], m->splat_origin[1], m->splat_size[0],
                                    m->splat_size[1]};
        uniform_set_vec4(prog->uniforms, "vtBakeRect", full_rect);
        material_texture_array_bind(arr, 0);
        // The array's own quad: non-zero on every path here, since the ensure
        // gates on a successfully built array and the build creates it.
        draw_fullscreen_quad(arr->quad_vao);

        glBindTexture(GL_TEXTURE_2D, vt->albedo_tex);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, vt->surface_tex);
        glGenerateMipmap(GL_TEXTURE_2D);

        vt_read_means(vt, m, arr);

        // A fallback re-bake invalidates every page: their content was a
        // function of the same key. Config re-derives with the new res, and
        // the reset marks the table for its next UBO upload.
        vt_page_config(vt, m, res);
        vt_pages_reset(vt);
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
        // The 4/3 is the mip chain; two targets, so the pair is 8 bytes a texel.
        double bytes = 2.0 * (double)res * (double)res * 4.0 * (4.0 / 3.0);
        log_info("Layers VT: material '%s' composite %dx%d pair, %.1f MB",
                 m->name ? m->name : "(unnamed)", res, res, bytes / (1024.0 * 1024.0));
    }
    return ok;
}

void material_layers_vt_ensure(struct Scene* scene, struct Engine* engine) {
    if (!scene || !engine)
        return;
    // The bake samples the material texture array, so it waits for the array
    // to be current -- one frame behind it at worst, never ahead of it.
    if (scene->material_textures_dirty || !scene->material_textures ||
        scene->material_textures->layer_count == 0)
        return;
    // v1 pages ONE material per scene: the engine owns a single VtPageBlock,
    // and a buffer per binding point is ubo.h's lifetime contract. The first
    // qualifying material is the page owner; any further one keeps its
    // fallback atlas and is warned about, once.
    bool page_owner_seen = false;
    for (size_t i = 0; i < scene->material_count; i++) {
        Material* m = scene->materials[i];
        if (!m)
            continue;
        // This function is the ONE owner of the armed state: a material that
        // stops qualifying is DISARMED, not merely skipped, so the bind site's
        // `layers_vt && baked` is exactly this predicate, one frame behind at
        // worst. Skipping instead leaves a stale cache bound to units 0/1 for
        // a material whose layered gate no longer skips the albedo read.
        bool want = engine->layers_vt_enabled && m->layer_count > 0 &&
                    m->splat_space == SPLAT_SPACE_WORLD_XZ;
        if (!want) {
            if (m->layers_vt)
                m->layers_vt->baked = false;
            continue;
        }
        int res = vt_res_for(m, engine);
        MaterialLayersVtKey key;
        vt_make_key(m, res, scene->material_textures, &key);
        bool current = m->layers_vt && m->layers_vt->baked &&
                       memcmp(&key, &m->layers_vt->key, sizeof(key)) == 0;
        if (!current) {
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
            // A failed bake keeps baked false and retries next frame; the
            // shading path falls back to the per-texel blend meanwhile, which
            // is a correct frame rather than a black one.
        }
        if (m->layers_vt && m->layers_vt->baked) {
            if (!page_owner_seen) {
                page_owner_seen = true;
                vt_pages_update(m, scene, engine);
            } else if (m->layers_vt->page_grid > 0) {
                static bool warned_second_owner;
                if (!warned_second_owner) {
                    log_warn("layers_vt: material '%s' also qualifies for pages; v1 pages "
                             "one material per scene, this one keeps its fallback atlas",
                             m->name ? m->name : "(unnamed)");
                    warned_second_owner = true;
                }
                m->layers_vt->page_grid = 0;
            }
        }
    }
}

/*
 * The GPU feedback loop (spec 11.67). See layers_vt.h for the fixed-latency
 * contract; here is the mechanics: parse the OLDEST ring slot first (its
 * glReadPixels was issued VT_FEEDBACK_RING frames ago, so the map is a formality
 * by now), then render this frame's votes and queue their readback into the
 * slot just freed.
 */
static void vt_feedback_alloc(LayersVtFeedback* fb, int w, int h) {
    glActiveTexture(GL_TEXTURE0);
    gl_delete_texture(&fb->color_tex);
    if (fb->depth_rb)
        glDeleteRenderbuffers(1, &fb->depth_rb);
    gl_delete_fbo(&fb->fbo);
    for (int i = 0; i < VT_FEEDBACK_RING; i++)
        if (fb->pbo[i])
            glDeleteBuffers(1, &fb->pbo[i]);

    glGenTextures(1, &fb->color_tex);
    glBindTexture(GL_TEXTURE_2D, fb->color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenRenderbuffers(1, &fb->depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, fb->depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glGenFramebuffers(1, &fb->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb->color_tex,
                           0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              fb->depth_rb);
    for (int i = 0; i < VT_FEEDBACK_RING; i++) {
        glGenBuffers(1, &fb->pbo[i]);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, fb->pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 4, NULL, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    fb->w = w;
    fb->h = h;
    fb->frames = 0;
    fb->have = false;
    memset(fb->requested, 0, sizeof(fb->requested));
}

static void vt_feedback_parse(LayersVtFeedback* fb, GLuint pbo, int grid) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    const unsigned char* px =
        glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)fb->w * fb->h * 4, GL_MAP_READ_BIT);
    memset(fb->requested, 0, sizeof(fb->requested));
    if (px) {
        int count = fb->w * fb->h;
        for (int i = 0; i < count; i++) {
            const unsigned char* p = px + (size_t)i * 4;
            if (p[2] == 0)
                continue; // no vote
            // Decoded against the CURRENT grid: a vote from before a config
            // change lands out of range and is dropped, which is the safe
            // reading of stale feedback.
            if ((int)p[0] >= grid || (int)p[1] >= grid)
                continue;
            fb->requested[(int)p[1] * grid + (int)p[0]] = 1;
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        fb->have = true;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

void layers_vt_feedback_pass(struct Engine* engine, struct Scene* scene) {
    if (!engine || !scene || !engine->layers_vt_feedback_enabled)
        return;
    // The scene's one paged material; without it there is nothing to vote for.
    const Material* paged = NULL;
    for (size_t i = 0; i < scene->material_count; i++) {
        const Material* m = scene->materials[i];
        if (m && m->layers_vt && m->layers_vt->baked && m->layers_vt->page_grid > 0) {
            paged = m;
            break;
        }
    }
    if (!paged)
        return;
    ShaderProgram* prog = get_engine_shader_program_by_name(engine, "layers_vt_feedback");
    if (!prog)
        return;

    if (!engine->vt_feedback) {
        engine->vt_feedback = calloc(1, sizeof(LayersVtFeedback));
        if (!engine->vt_feedback)
            return;
    }
    LayersVtFeedback* fb = engine->vt_feedback;
    int rw = 0, rh = 0;
    engine_render_size(engine, &rw, &rh);
    int w = rw / VT_FEEDBACK_DIVISOR;
    int h = rh / VT_FEEDBACK_DIVISOR;
    if (w < 16)
        w = 16;
    if (h < 16)
        h = 16;
    if (fb->fbo == 0 || fb->w != w || fb->h != h)
        vt_feedback_alloc(fb, w, h);

    // Fixed-latency consume BEFORE this frame's submit: the slot about to be
    // reused holds the readback issued VT_FEEDBACK_RING frames ago.
    int slot = (int)(fb->frames % VT_FEEDBACK_RING);
    if (fb->frames >= VT_FEEDBACK_RING)
        vt_feedback_parse(fb, fb->pbo[slot], paged->layers_vt->page_grid);

    GLint prev_fbo = 0, prev_viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    GLboolean blend_was = glIsEnabled(GL_BLEND);
    GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
    GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    glViewport(0, 0, fb->w, fb->h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog->id);
    uniform_set_mat4(prog->uniforms, "viewProj", (const float*)engine->view_proj);
    const float domain[4] = {paged->splat_origin[0], paged->splat_origin[1],
                             paged->splat_size[0], paged->splat_size[1]};
    uniform_set_vec4(prog->uniforms, "splatDomain", domain);

    // Only the paged surfaces vote, depth-tested against EACH OTHER: terrain
    // self-occlusion (a hillside hiding the valley behind it) is the occlusion
    // that matters for ground pages. Other geometry as occluders is deliberate
    // v1 scope-out -- omitting a tree canopy makes the vote conservative, never
    // wrong -- and skinned meshes are skipped outright (no paged material is
    // skinned, and the minimal vertex stage has no bones).
    const DrawList* list = &scene->draw_list;
    for (size_t i = 0; i < list->count; i++) {
        const DrawItem* item = &list->items[i];
        Mesh* mesh = item->mesh;
        if (!mesh || mesh->is_skinned || !mesh->material || mesh->material != paged)
            continue;
        uniform_set_mat4(prog->uniforms, "model",
                         (const float*)item->node->global_transform);
        GLsizei count = 0;
        const void* offset = NULL;
        mesh_lod_range(mesh, 0, &count, &offset);
        glBindVertexArray(mesh->vao);
        glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, offset);
    }
    glBindVertexArray(0);

    // Queue this frame's readback into the slot just consumed; the map that
    // retires it happens VT_FEEDBACK_RING frames from now.
    glBindBuffer(GL_PIXEL_PACK_BUFFER, fb->pbo[slot]);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fb->w, fb->h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    fb->frames++;

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    if (blend_was)
        glEnable(GL_BLEND);
    if (cull_was)
        glEnable(GL_CULL_FACE);
    if (!depth_was)
        glDisable(GL_DEPTH_TEST);
}

void free_layers_vt_feedback(LayersVtFeedback* fb) {
    if (!fb)
        return;
    gl_delete_texture(&fb->color_tex);
    if (fb->depth_rb)
        glDeleteRenderbuffers(1, &fb->depth_rb);
    gl_delete_fbo(&fb->fbo);
    for (int i = 0; i < VT_FEEDBACK_RING; i++)
        if (fb->pbo[i])
            glDeleteBuffers(1, &fb->pbo[i]);
    free(fb);
}

void free_material_layers_vt(MaterialLayersVt* vt) {
    if (!vt)
        return;
    gl_delete_texture(&vt->albedo_tex);
    gl_delete_texture(&vt->surface_tex);
    gl_delete_texture(&vt->page_albedo_tex);
    gl_delete_texture(&vt->page_surface_tex);
    free(vt);
}
