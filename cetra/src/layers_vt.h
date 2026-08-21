#ifndef _LAYERS_VT_H_
#define _LAYERS_VT_H_

#include <stdbool.h>
#include <GL/glew.h>

#include "material.h"

struct Scene;
struct Engine;
struct UniformManager;

/*
 * The layered-surface composite cache (spec 11.66, roadmap D10 stage 1).
 *
 * A world-XZ-splat layered material's blend is baked once into two 2D textures
 * covering its splat domain -- the MACRO: which layer wins where, at per-layer
 * tile means -- and the shading path samples those plus one triplanar detail
 * tap of the dominant layer's own maps, instead of running the full per-texel
 * blend. The per-texel path stays as the general mechanism: UV1-splat
 * materials, and anything vertical, never take the cache.
 *
 * The two textures ride pbr_frag's albedoTex/normalTex declarations (units 0
 * and 1), which are provably unread whenever layerCount > 0 -- so the cache
 * costs the 16/16 sampler ledger nothing. They are deliberately NOT tenants of
 * the material texture array: its canonical-size rule would promote every
 * other layer in the scene to the cache's resolution.
 */
typedef struct MaterialLayersVtKey {
    int res;
    int layer_count;
    int splat_layer;
    GLuint splat_id;
    GLuint albedo_ids[MATERIAL_MAX_LAYERS];
    GLuint surface_ids[MATERIAL_MAX_LAYERS];
    int albedo_layers[MATERIAL_MAX_LAYERS];
    int surface_layers[MATERIAL_MAX_LAYERS];
    float uv_scale[MATERIAL_MAX_LAYERS];
    float blend_sharpness;
    float triplanar_sharpness;
    float domain[4];
    int arr_width, arr_height; // the array's canonical size the bake read through
} MaterialLayersVtKey;

typedef struct MaterialLayersVt {
    GLuint albedo_tex;  // rgb = blended albedo in STORED codes, a = dominant/3
    GLuint surface_tex; // rg = tangent normal xy, b = roughness, a = AO

    // Per-layer means of the SAME resampled data the taps read -- the material
    // texture array's top mip -- which is what makes the runtime detail ratio
    // exactly one on a flat map, and the flat case byte-exact.
    float mean_albedo[MATERIAL_MAX_LAYERS][3];
    float mean_rough[MATERIAL_MAX_LAYERS];
    float mean_ao[MATERIAL_MAX_LAYERS];

    // Everything the bake read, by VALUE (the water-seed idiom): a dirty flag
    // would need every writer to set it, and the writers are a scene file, the
    // CLI and the GUI. Compared each frame; a mismatch re-bakes. Carries the
    // array LAYER indices beside the GL ids, because an array rebuild can
    // reshuffle indices without any source changing.
    MaterialLayersVtKey key;
    bool baked;
} MaterialLayersVt;

// (Re)bake the cache for every world-XZ-splat layered material whose key no
// longer matches, and DISARM the cache of any material that stops qualifying
// -- this function is the one owner of the armed state, so the bind site's
// `layers_vt && baked` is exactly the qualification predicate. Called each
// frame after material_texture_array_ensure_built, because the bake samples
// the array; a no-op while the array is dirty.
void material_layers_vt_ensure(struct Scene* scene, struct Engine* engine);

void free_material_layers_vt(MaterialLayersVt* vt);

// The layered-surface uniform set, shared by the shading path and the bake
// program so the two cannot upload different values for the same material.
void material_upload_layer_uniforms(const Material* material, struct UniformManager* u);

#endif // _LAYERS_VT_H_
