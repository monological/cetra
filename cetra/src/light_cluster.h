#ifndef _LIGHT_CLUSTER_H_
#define _LIGHT_CLUSTER_H_

#include <cglm/cglm.h>
#include <stdbool.h>
#include <stdint.h>

#include "ubo.h"
// PROBE_SET_MAX, which sizes the probe descriptor array below. probe_set.h owns
// the cap; this file only lays it out for std140, the shore_chain relationship.
#include "probe_set.h"
// And again: decal.h owns the decal cap the mask array below is sized from.
#include "decal.h"

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
    float dir_type[4];        // xyz = direction (world, UNIT), w = 1 point / 2 spot / 3 area
    float color_intensity[4]; // xyz = color * intensity (premultiplied)
    // The attenuation triple this used to carry is gone -- punctual falloff is
    // inverse-square windowed by `range`, and slot [1] now holds a live index.
    float atten_cutoff[4]; // 1/range^2 (0 = unbounded), IES profile index
                           // (-1 = none), reserved, cos inner cone
    float shadow_misc[4];  // cos outer cone, punctual shadow layer, size.xy
    // Roll reference, unit and orthonormal to dir: a panel's height axis
    // (spec 9.2), an asymmetric IES profile's azimuth zero (spec 11.57).
    float up_area[4];
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

/*
 * C mirror of ProbeBlock (include/probe_specular.glsl), spec 11.70. Here rather
 * than in probe_set.h because half of it is the froxel masks, which this file's
 * grid sizes and this file's build pass fills.
 *
 * ONE block for the descriptors AND the masks, where the lights spend three.
 * Not thrift: pbr_frag's fragment stage declares eight uniform blocks against
 * GL 4.1's guaranteed twelve, so a second block would spend a tenth declaration
 * to save re-sending 3.6 KB -- under a third of what ONE light block sends
 * every frame.
 *
 * Uploaded whole, per build, on the grid's cadence rather than the IES table's:
 * the masks are VIEW-space and a probe capture face re-enters the build with
 * its own view, so a once-a-frame upload would leave every capture face reading
 * another view's masks. Writing it whole also leaves no undefined tail, which
 * is the trap the IES table was placed after the lights to avoid.
 */
typedef struct GpuProbeDesc {
    float pos_intensity[4]; // xyz capture position (world), w intensity
    float box_min_fade[4];  // xyz parallax box min,         w box_fade
    float box_max_pad[4];   // xyz parallax box max,         w unused
    float column[4];        // x column left edge (texels),  yzw unused
} GpuProbeDesc;

typedef struct GpuProbeBlock {
    int32_t info[4];       // x count, yzw unused
    float atlas_params[4]; // 1/atlas_w, 1/atlas_h, atlas_w, atlas_h
    // Atlas-wide rather than per probe: xy unused, z gutter, w last row index.
    float atlas_column[4];
    // Row r's y origin (texels, gutter included) and interior edge. Published
    // by the CPU because every probe's column has the SAME row geometry and it
    // never changes after the atlas is built -- which is what keeps the halving
    // rule out of the shader, and with it the requirement that the row size be
    // a power of two.
    float rows[PROBE_ATLAS_ROWS_MAX][4];
    GpuProbeDesc descs[PROBE_SET_MAX];
    // One 8-bit mask per froxel, four to a word. A uint per froxel would be
    // four times the bytes for the same bits, and std140 would then give each
    // one a vec4 stride and make it sixteen.
    uint32_t cluster_masks[LC_CLUSTER_COUNT / 4];
} GpuProbeBlock;

/*
 * Clustered decals (spec 11.73). Same two halves as the probe block above and
 * for the same reasons: world-space descriptors that are a function of the
 * authored scene, and VIEW-space froxel masks that are a function of the camera
 * and so must be rebuilt per invocation, capture faces included.
 *
 * The transform is stored as three ROWS of world->local rather than a mat4 with
 * a separate extent: folding 1/half_extent into the rotation rows makes the
 * shader's local coordinate land in [-1,1] with three dots and no divide, and
 * costs a row less than the alternative. The fourth column is the translation,
 * so there is no separate origin to keep in step with it.
 */
typedef struct GpuDecalDesc {
    float row0[4]; // world->local rows, each scaled by 1/half_extent[i] with the
    float row1[4]; // translation folded into .w -- so xyz local is in [-1,1]^3
    float row2[4]; // inside the box, and outside it exactly where |local| > 1
    // x albedo layer (-1 none), y surface layer (-1 none), z opacity,
    // w cos(angle_fade) -- the cosine rather than the angle because that is what
    // the shader compares a dot product against, the spot-cutoff lesson.
    float params0[4];
    float params1[4]; // x edge feather (local units), y normal strength, zw unused
} GpuDecalDesc;

typedef struct GpuDecalBlock {
    int32_t info[4]; // x count, yzw unused
    GpuDecalDesc descs[DECAL_MAX];
    // One 16-bit mask per froxel, two to a word. Sixteen because the cap is
    // sixteen; the packing is the light index pool's, not the probe mask's.
    uint32_t cluster_masks[LC_CLUSTER_COUNT / 2];
} GpuDecalBlock;

_Static_assert(sizeof(GpuLightsBlock) == UBO_LIGHTS_BLOCK_SIZE,
               "GpuLightsBlock must match the std140 LightsBlock layout");
_Static_assert(sizeof(GpuProbeBlock) == UBO_PROBES_BLOCK_SIZE,
               "GpuProbeBlock must match the std140 ProbeBlock layout");
_Static_assert(sizeof(GpuDecalBlock) == UBO_DECALS_BLOCK_SIZE,
               "GpuDecalBlock must match the std140 DecalBlock layout");
// Two froxels to a word, so an odd froxel count would drop the last one.
_Static_assert(LC_CLUSTER_COUNT % 2 == 0, "the decal mask packs two froxels per word");
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
    // IesBlock (UBO_BINDING_IES). Lives here because it is light data and the
    // binding registry keeps the light blocks together, but it is uploaded from
    // the SCENE's library when that changes, never by the per-frame build.
    struct Ubo* ies_ubo;
    uint64_t ies_uploaded_revision; // library+set last packed; 0 = nothing uploaded yet
    // ProbeBlock (UBO_BINDING_PROBES). Here for the ies_ubo reason -- the masks
    // are this module's grid -- but built per invocation like the light blocks,
    // because they are view-space where an IES table is not.
    struct Ubo* probes_ubo;
    bool probes_armed; // last build wrote a live block; drives the disarming zero write
    // DecalBlock (UBO_BINDING_DECALS), on the probe block's terms exactly: the
    // masks are view-space, so it is built per invocation and not per frame.
    struct Ubo* decals_ubo;
    bool decals_armed;
    // How many (decal, froxel) pairs the last build marked, for --decal-probe.
    // Kept here rather than reported back to a decal subsystem the way the
    // probes' bit count is, because there is no such object: a Decal is a plain
    // struct on the Scene with no module owning a set of them.
    //
    // There is no digest field beside it. The masks survive the build in
    // `decals` below, so the accessor hashes them WHEN ASKED -- 6 KB of
    // byte-wise FNV-1a every build, seven times over on a probe-capture frame,
    // is a lot of work for a flag nobody passed.
    int decal_mask_bits;

    GpuLightsBlock lights;
    GpuProbeBlock probes;
    GpuDecalBlock decals;
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
    bool area_lights_enabled; // false = LIGHT_AREA lights are skipped at gather
} LightClusterContext;

LightClusterContext* create_light_cluster_context(void);
void free_light_cluster_context(LightClusterContext* ctx);

// Build the three blocks from scene->lights for this invocation's camera and
// upload them. fb_width/fb_height are the render-target dimensions
// gl_FragCoord is measured in (the current viewport).
void light_cluster_build_and_upload(LightClusterContext* ctx, struct Scene* scene, mat4 view,
                                    mat4 projection, int fb_width, int fb_height, float near_clip,
                                    float far_clip);

// What the last build's decal masks came to, for --decal-probe. Accessors rather
// than a reach into the struct, because the digest is the one thing about this
// module an app has any business reading.
uint32_t light_cluster_decal_mask_digest(const LightClusterContext* ctx);
int light_cluster_decal_mask_bits(const LightClusterContext* ctx);

#endif // _LIGHT_CLUSTER_H_
