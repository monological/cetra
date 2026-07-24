#ifndef _LIGHT_CLUSTER_H_
#define _LIGHT_CLUSTER_H_

#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>

#include "ubo.h"

struct Engine;
struct Scene;
struct Light;

// CPU side of clustered forward lighting (spec 9.1): frustum-cull the scene's
// punctual lights, pack them into the std140 LightsBlock, assign each to the
// screen-tile x exponential-Z cluster grid, and upload the three UBOs. Runs
// once per render_current_scene invocation -- including each reflection-probe
// capture face, whose view/projection differ (the grid is view-dependent).
//
// Grid dimensions and the slice mapping must match lights_ubo.glsl:
//   slice(z) = floor(log2(z) * sliceScale + sliceBias)
//   z(s)     = near * pow(far/near, s / LC_CLUSTER_Z)

#define LC_CLUSTER_X           16
#define LC_CLUSTER_Y           8
#define LC_CLUSTER_Z           24
#define LC_CLUSTER_COUNT       (LC_CLUSTER_X * LC_CLUSTER_Y * LC_CLUSTER_Z)
#define LC_MAX_DIR_LIGHTS      4
#define LC_MAX_CLUSTER_LIGHTS  128
#define LC_MAX_CLUSTER_INDICES 6144

// C mirrors of the std140 blocks (lights_ubo.glsl). Every member is a 16-byte
// row (float[4] / int32_t[4]) or packs to whole rows, so the C layout equals
// the std140 layout by construction; the _Static_asserts pin the contract.
typedef struct GpuDirLight {
    float dir_shadow[4];      // xyz = direction (world), w = float(CSM slot), -1 = none
    float color_intensity[4]; // xyz = color * intensity (premultiplied)
    float size_misc[4];       // xy = emitter size (PCSS penumbra width)
} GpuDirLight;

typedef struct GpuPackedLight {
    // w = cull radius (0 = unbounded). Not sampled by any shader today -- the
    // GPU never needs the radius, since cluster membership already answers
    // "does this light reach me". Kept because it costs nothing (the vec4 row
    // exists for the position regardless) and it is what a range-windowed
    // falloff would read.
    float pos_range[4];
    float dir_type[4];         // xyz = direction, w = 1 point / 2 spot / 3 area
    float color_intensity[4];  // xyz = color * intensity (premultiplied)
    float atten_cutoff[4];     // constant, linear, quadratic, cos inner cone
    float spot_shadow_size[4]; // cos outer cone, shadow slot (spare), size.xy
    float up_area[4];          // AREA only: panel height axis, orthonormal to dir (spec 9.2)
} GpuPackedLight;

typedef struct GpuLightsBlock {
    int32_t light_counts[4]; // x = dir count, y = clustered count,
                             // z = area count (gates the LTC LUT fetches), w unused
    float cluster_params[4]; // sliceScale, sliceBias, LC_CLUSTER_X/fbW, LC_CLUSTER_Y/fbH
    GpuDirLight dir_lights[LC_MAX_DIR_LIGHTS];
    GpuPackedLight cluster_lights[LC_MAX_CLUSTER_LIGHTS];
} GpuLightsBlock;

typedef struct GpuClusterBlock {
    // (index-pool offset << 12) | count per cluster; the GPU reads the same
    // bytes as uvec4 clusters[LC_CLUSTER_COUNT / 4]
    uint32_t clusters[LC_CLUSTER_COUNT];
} GpuClusterBlock;

typedef struct GpuClusterIndexBlock {
    // Two 16-bit light indices per uint32 on the GPU; a little-endian uint16
    // array IS that layout (even index = low halfword)
    uint16_t indices[LC_MAX_CLUSTER_INDICES];
} GpuClusterIndexBlock;

_Static_assert(sizeof(GpuLightsBlock) == UBO_LIGHTS_BLOCK_SIZE,
               "GpuLightsBlock must match the std140 LightsBlock layout");
_Static_assert(sizeof(GpuClusterBlock) == UBO_CLUSTERS_BLOCK_SIZE,
               "GpuClusterBlock must match the std140 ClusterBlock layout");
_Static_assert(sizeof(GpuClusterIndexBlock) == UBO_CLUSTER_INDICES_BLOCK_SIZE,
               "GpuClusterIndexBlock must match the std140 ClusterIndexBlock layout");

// Per-light cluster coverage, computed once per build (scratch)
typedef struct LightClusterRange {
    int x0, x1, y0, y1, z0, z1; // inclusive tile/slice ranges
} LightClusterRange;

#define LC_TOUCH_STRIDE (LC_CLUSTER_COUNT / 8) // bytes per light in the touch bitset

// Fixed-size working set, allocated once (no per-frame allocation). Owns the
// three GPU buffers too: nothing outside this module touches them, so they
// live here rather than as loose fields on Engine (mirrors how PostFX owns its
// own targets).
typedef struct LightClusterContext {
    struct Ubo* lights_ubo;          // LightsBlock       (UBO_BINDING_LIGHTS)
    struct Ubo* clusters_ubo;        // ClusterBlock      (UBO_BINDING_CLUSTERS)
    struct Ubo* cluster_indices_ubo; // ClusterIndexBlock (UBO_BINDING_CLUSTER_INDICES)

    GpuLightsBlock lights;
    GpuClusterBlock grid;
    GpuClusterIndexBlock index_pool;
    LightClusterRange ranges[LC_MAX_CLUSTER_LIGHTS];
    // View-space bounding sphere per packed light (xyz center, w radius;
    // w < 0 = uncullable) for the per-cluster AABB refinement in the fill
    float view_spheres[LC_MAX_CLUSTER_LIGHTS][4];
    // One bit per (light, cluster): set by the counting pass, replayed by the
    // writing pass. Without it both passes would re-run the same ~40-flop
    // sphere-vs-AABB test over the same cells -- up to ~390k redundant tests a
    // frame at the light cap -- and the two loops would have to be kept in
    // lockstep by hand.
    uint8_t touched[LC_MAX_CLUSTER_LIGHTS][LC_TOUCH_STRIDE];
    uint16_t counts[LC_CLUSTER_COUNT];  // per-cluster light counts (fill pass 1)
    uint32_t offsets[LC_CLUSTER_COUNT]; // per-cluster index-pool offsets (prefix sum)
    bool warned_dir_overflow;
    bool warned_packed_overflow;
    bool warned_index_overflow;
    bool logged_first_build;
} LightClusterContext;

LightClusterContext* create_light_cluster_context(void);
void free_light_cluster_context(LightClusterContext* ctx);

// Cull radius for a light: the authored range if set, else the distance where
// the light falls under ~1/256 (LDR LSB at the project-standard -E 1.0).
// Punctual lights solve that against their attenuation coefficients; area
// panels ignore those entirely (the LTC form factor carries the falloff) and
// instead invert the head-on far-field irradiance, plus half the panel
// diagonal to cover its own extent. Returns 0 for a light that never reaches
// the epsilon (drop it) and a negative value for an uncullable light
// (constant-only attenuation: assign everywhere).
float light_cull_radius(const struct Light* light);

// Build the three blocks from scene->lights for this invocation's camera and
// upload them. fb_width/fb_height are the render-target dimensions
// gl_FragCoord is measured in (the current viewport).
void light_cluster_build_and_upload(LightClusterContext* ctx, struct Scene* scene, mat4 view,
                                    mat4 projection, int fb_width, int fb_height, float near_clip,
                                    float far_clip);

#endif // _LIGHT_CLUSTER_H_
