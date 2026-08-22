#ifndef _ROADS_H_
#define _ROADS_H_

#include <stdint.h>
#include <stdbool.h>

struct Scene;
struct Engine;

/*
 * Roads on layered surfaces (spec 11.68), owned here and only laid out by
 * ubo.h -- the shore film's duplicated-constant lesson, in the shape ies.h and
 * layers_vt_pages.h use.
 *
 * A road is a world-XZ polyline that OVERRIDES the splat weights toward one of
 * the material's own layers. It is not a decal and not a mesh: the override
 * happens inside sampleLayeredSurface before the height blend, so the shoulder
 * interlocks exactly as a painted layer boundary does, and the composite cache
 * inherits roads because its bake calls that same function unchanged. What
 * reaches the GPU is therefore geometry, not pixels -- segments in a uniform
 * block, which costs the sampler ledger nothing.
 *
 * The caps are deliberately small. A road is authored content describing a
 * COURSE, not a mesh: forest's trail is eleven segments across a kilometre, and
 * a scene wanting a road network wants the paged content era's decals rather
 * than a hundred polylines evaluated per fragment on the per-texel path.
 */
#define MATERIAL_MAX_ROADS       4
#define MATERIAL_MAX_ROAD_POINTS 16
// One segment between each adjacent pair, for every road at the ceiling.
#define ROAD_SEGS_MAX (MATERIAL_MAX_ROADS * (MATERIAL_MAX_ROAD_POINTS - 1))

/*
 * One road on a material. Inline in Material rather than behind a pointer --
 * the stochastic_lut precedent: a fixed table small enough to live in the
 * object needs no allocation, no free and no ownership rule.
 *
 * Points are AUTHORED world XZ, the same frame splat_origin is in: a road is an
 * identity, not a location, so it may not slide when the engine re-centres.
 */
typedef struct MaterialRoad {
    float points[MATERIAL_MAX_ROAD_POINTS][2];
    int point_count; // < 2 is not a road; the authoring paths refuse it
    float width;     // full width in world units; the mask is half this either side
    float feather;   // shoulder beyond the half-width; 0 = a hard edge
    int layer;       // which of the material's OWN layers the road is made of
} MaterialRoad;

/*
 * C mirror of RoadsBlock (roads_ubo.glsl).
 *
 * Two descriptor rows per road and one shared segment pool -- the IES
 * descriptor idiom, because a road spends only its own segments and sizing
 * every road for the ceiling would quadruple the block for nothing.
 *
 * Segment endpoints are floats rather than an indexed point list: the shader
 * wants (a, b) per iteration, and storing the pair costs one vec4 where a point
 * list would cost two fetches and an index. A zero-filled buffer reads count 0,
 * which every consumer treats as "no roads".
 */
typedef struct GpuRoadsBlock {
    // roadCount, unused x3 (std140 pads a lone int to a full vec4 regardless)
    int32_t info[4];
    // [2r + 0] firstSeg, segCount, halfWidth, feather
    // [2r + 1] layer, unused x3
    float desc[MATERIAL_MAX_ROADS * 2][4];
    // ax, az, bx, bz -- authored world XZ
    float segs[ROAD_SEGS_MAX][4];
} GpuRoadsBlock;

// Arm the scene's one road-bearing material, pack its roads and upload them
// when they changed. This function is the ONE owner of Material.roads_armed, so
// a material that stops qualifying is DISARMED rather than merely skipped.
// Called each frame BEFORE material_layers_vt_ensure -- the composite bake
// samples this block, so a road edit must reach the GPU before the key change
// it causes triggers the re-bake that reads it.
void material_roads_ensure(struct Scene* scene, struct Engine* engine);

/*
 * Shortest distance from (x, z) to a polyline, in the polyline's own frame.
 *
 * The CPU twin of roadReshapedWeights' inner loop (layers.glsl). The two cannot
 * share a token -- one is C and one is GLSL -- so they are the same four-line
 * clamped projection written twice, and what holds them together is that the
 * fixture arms pin the shader against closed forms while the scatter arm pins
 * this one. Returns a large value for a degenerate polyline, so a caller
 * comparing against a width rejects nothing.
 */
float roads_polyline_distance_xz(const float (*points)[2], int count, float x, float z);

#endif // _ROADS_H_
