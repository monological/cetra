#ifndef _OCCLUSION_H_
#define _OCCLUSION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cglm/cglm.h>

// CPU masked software occlusion culling (spec 11.98): authored occluder proxies
// rasterised into a small fixed-point depth buffer from THIS frame's camera,
// before the draw list is culled -- so there is no one-frame latency and no
// popping, the property a last-frame HZB cannot have. No GL anywhere in this
// module.
//
// Conservative in both directions, and the two halves are different kinds of
// promise. The ARITHMETIC half lives here: occluder depth rounds toward far and
// coverage rounds down, occludee depth rounds toward near and footprint rounds
// out, so "hidden" can never be claimed for anything the rasterised proxies do
// not provably cover. The ASSET half is the author's: a proxy must sit entirely
// inside opaque geometry, because the raster faithfully hides what stands
// behind the BOX -- a box poking out of its mesh is the one path to a wrong
// pixel, and the probe's box-against-triangles check exists to catch it.

enum {
    OCCLUSION_W = 256,
    OCCLUSION_H = 144,
    OCCLUSION_TILE_W = 8,
    OCCLUSION_TILE_H = 4,
    OCCLUSION_TILES_X = OCCLUSION_W / OCCLUSION_TILE_W, // 32
    OCCLUSION_TILES_Y = OCCLUSION_H / OCCLUSION_TILE_H, // 36
    OCCLUSION_MAX_OCCLUDERS = 256,
};

// An uncovered pixel. The sentinel IS the coverage mask: a tile's aggregate is
// the max over its pixels, so one uncovered pixel drives the tile to EMPTY and
// no in-frustum depth can ever test >= it -- "full coverage required" costs no
// second buffer.
#define OCCLUSION_DEPTH_EMPTY 0xFFFFu
#define OCCLUSION_DEPTH_MAX   0xFFFEu

// Fixed-size working set, allocated once -- the LightClusterContext charter.
// The buffer is fixed-resolution in NDC, so render scale, TAAU, supersampling
// and window size never touch it. Depth is FIXED-POINT because -O2 moves CPU
// float results and integer state is what a probe can digest and a gate can
// assert on.
//
// Consumers go through the accessors below rather than reaching into the
// struct; it is declared here so the engine can own one by pointer.
typedef struct OcclusionContext {
    mat4 view_proj; // latched at begin(); the UNJITTERED camera matrix
    vec3 eye;       // the camera's world position, for the inside-a-box skip

    // Nearest occluder front-face depth per pixel (GL_LESS semantics), EMPTY
    // where nothing landed. Overlapping occluders merge by min for free.
    uint16_t depth[OCCLUSION_H][OCCLUSION_W];
    // Farthest pixel per tile after finish() -- what the test compares against.
    uint16_t tile_zmax[OCCLUSION_TILES_Y][OCCLUSION_TILES_X];

    int occluder_count;    // boxes accepted this frame (skipped ones excluded)
    size_t tested, culled; // occlusion_cull_list's tallies, for the probe
    bool active;           // finish() found at least one fully covered tile
    bool warned_overflow;  // boxes past the cap are dropped and said once
} OcclusionContext;

OcclusionContext* create_occlusion_context(void);
void free_occlusion_context(OcclusionContext* context);

// Frame protocol: begin -> add_box xN -> finish -> test/cull_list. begin clears
// the buffer to EMPTY and latches the matrix; finish folds pixels into
// tile_zmax and decides `active` -- a frame whose occluders all clipped away or
// sat behind the camera covers no tile and the per-item walk is skipped.
// `eye` is the camera's world position: a box the eye sits inside is SKIPPED,
// because a proxy proves nothing from inside itself and the asset contract
// makes "camera inside a proxy" an authoring situation, not a rendering one.
void occlusion_begin(OcclusionContext* context, mat4 view_proj, const vec3 eye);
// transform may be NULL for a box already in world space (the authored kind).
void occlusion_add_box(OcclusionContext* context, const vec3 box_min, const vec3 box_max,
                       mat4 transform);
void occlusion_finish(OcclusionContext* context);
bool occlusion_active(const OcclusionContext* context);

// True = provably hidden: every tile the box's outward-rounded footprint
// touches is fully covered at a depth nearer than the box's conservatively
// nearest corner. Never true for anything that might be visible; a box
// touching the near plane or reaching behind the camera is never culled.
bool occlusion_test_aabb(const OcclusionContext* context, const vec3 world_min,
                         const vec3 world_max);

// Walk `list` once and set item->occluded on every boundable, provably hidden
// item. `view` supplies wind and pose for the bounds; its frustum and
// occlusion fields are not read.
struct DrawList;
struct CullView;
void occlusion_cull_list(OcclusionContext* context, struct DrawList* list,
                         const struct CullView* view);

// The same clip/round/fill path into a caller's buffer at any size -- the
// probe's reference twin rasterises through THIS at full resolution, so the
// hierarchy and the quantisation are the only things it does not share. The
// inside-a-box skip is part of the path, which is why `eye` is here too.
void occlusion_rasterize_box_into(uint16_t* depth, int w, int h, mat4 view_proj, const vec3 eye,
                                  const vec3 box_min, const vec3 box_max, mat4 transform);
// Triangle-exact raster of a mesh's CPU arrays (they survive upload) under the
// same rounding. Validation only: the probe compares a flagged mesh's box
// raster against its triangles to check the interior contract.
struct Mesh;
void occlusion_rasterize_mesh_into(uint16_t* depth, int w, int h, mat4 view_proj,
                                   const struct Mesh* mesh, mat4 transform);

// Integer state reads for the probe. Buffers are hashed by the caller when
// asked rather than digested every frame.
const uint16_t* occlusion_depth_pixels(const OcclusionContext* context, int* w, int* h);
const uint16_t* occlusion_tiles(const OcclusionContext* context, int* tx, int* ty);
int occlusion_occluder_count(const OcclusionContext* context);
size_t occlusion_tested_count(const OcclusionContext* context);
size_t occlusion_culled_count(const OcclusionContext* context);

#endif // _OCCLUSION_H_
