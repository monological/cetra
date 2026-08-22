#ifndef _LAYERS_VT_H_
#define _LAYERS_VT_H_

#include <stdbool.h>
#include <GL/glew.h>

#include "layers_vt_pages.h"
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
    // The roads the bake drew (spec 11.68), as the struct itself rather than
    // flattened into parallel arrays. The layer fields above flatten because
    // they SUBSTITUTE -- a Texture* becomes a GL id -- where a road field is
    // already a plain value. Copying the struct whole is what makes this
    // struct's contract, everything the bake read BY VALUE, mechanically true:
    // a field added to MaterialRoad joins the key by construction instead of
    // waiting for someone to remember a sixth copy line.
    MaterialRoad roads[MATERIAL_MAX_ROADS];
    int road_count;
} MaterialLayersVtKey;

typedef struct MaterialLayersVt {
    GLuint albedo_tex;  // rgb = blended albedo in STORED codes, a = dominant/3
    GLuint surface_tex; // rg = tangent normal xy, b = roughness, a = AO

    /*
     * The paged near-field atlas (spec 11.67): fixed 256-texel tiles of the
     * same two-target packing at VT_PAGE_DENSITY_RATIO x the fallback's
     * density, guttered, mips capped at VT_PAGE_MIP_CAP so page mip 2 meets
     * fallback mip 0 and the handoff band blends near-identical signals.
     * Residency is a page table (virtual index -> atlas slot), uploaded to the
     * engine's VtPageBlock UBO when it changes.
     */
    GLuint page_albedo_tex, page_surface_tex; // VT_ATLAS_TEXELS^2 pair; 0 = pages off
    int page_grid;                            // virtual pages per axis for the current bake config
    float page_span;                          // world units one page's USABLE texels cover
    float page_texel;                         // world units per page texel (fallback texel / ratio)
    int16_t page_table[VT_PAGE_TABLE_MAX];    // virtual index -> atlas slot, -1 = absent
    int16_t slot_page[VT_PAGE_SLOTS];         // atlas slot -> virtual index, -1 = free
    bool pages_dirty;                         // table changed since the last UBO upload
    unsigned long long pages_loaded;          // lifetime page bakes, for the probe
    unsigned long long pages_evicted;         // lifetime evictions, for the probe

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

/*
 * The GPU feedback loop (spec 11.67): a small depth-tested pass where every
 * rasterized texel of a paged surface votes for its virtual page, read back
 * through a PBO ring consumed at FIXED latency -- always the slot from
 * VT_FEEDBACK_RING frames ago, never "whichever fence signalled", which is
 * what keeps residency a pure function of frame history (10.2's
 * nondeterminism belonged to the ready-order consume; a fixed-slot map costs
 * wall time at worst, never content). Prediction covers the frames before the
 * ring fills and every teleport; feedback adds what prediction cannot see --
 * occlusion today, non-enumerable VT consumers (decals, meshes) later.
 */
#define VT_FEEDBACK_RING 4
// Render size over this, clamped to a floor: page votes need page-sized
// blobs, not pixels.
#define VT_FEEDBACK_DIVISOR 8

typedef struct LayersVtFeedback {
    GLuint fbo, color_tex, depth_rb;
    int w, h;
    GLuint pbo[VT_FEEDBACK_RING];
    unsigned long long frames;                  // submits so far; ring is live past RING
    unsigned char requested[VT_PAGE_TABLE_MAX]; // frame N-RING's votes, parsed
} LayersVtFeedback;

// Render the vote pass for the scene's paged material and cycle the readback
// ring. Called once per frame after the scene render (it re-walks the draw
// list); a no-op without an armed paged material or with feedback disabled.
void layers_vt_feedback_pass(struct Engine* engine, struct Scene* scene);

void free_layers_vt_feedback(LayersVtFeedback* fb);

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
