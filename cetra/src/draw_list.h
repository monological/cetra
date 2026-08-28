#ifndef DRAW_LIST_H
#define DRAW_LIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "intersect.h"
#include "mesh.h"

struct Scene;
struct SceneNode;
struct Wind;
struct AnimationState;

// One frame's drawable meshes, flattened out of the scene graph once.
//
// Before this the graph was walked per pass: four times for the camera with OIT
// on, once per shadow cascade, once per punctual layer, twice more per cascade
// under TSM, and six times again per cube-capture face -- each walk re-deriving
// the same routing decision for the same meshes. Flattening moves that work to
// once per frame and gives the shadow pass, which walked its own way and could
// not cull, the same vocabulary the camera pass uses.
//
// Order is EXACTLY the order the recursive walks produced: depth-first,
// children left to right, a node's meshes before its gizmo. That is not a
// convenience -- alpha-to-coverage, the unsorted late pass and transmissive
// blending all make submission order visible, so the list has to reproduce it
// to be a refactor rather than a change.

// Which pass draws an item. Decided once at build from the material, where it
// used to be recomputed per mesh per pass.
typedef enum DrawLane {
    DRAW_LANE_OPAQUE = 0,   // everything not blend and not transmissive
    DRAW_LANE_BLEND,        // ALPHA_BLEND without transmission: the OIT sub-passes
    DRAW_LANE_TRANSMISSIVE, // transmission > 0: keeps the refraction path
    DRAW_LANE_COUNT
} DrawLane;

// Material facts a consumer would otherwise re-derive per pass. The depth pass
// classified every mesh on every layer to answer questions the material already
// settles; these are those answers. Anything derivable from `lane` is NOT here
// -- a second encoding of one fact is a second thing to keep in agreement.
enum {
    DRAW_ALPHA_MASKED = 1u << 0,
    DRAW_FOLIAGE = 1u << 1, // alpha-masked and opted back into casting
    DRAW_DOUBLE_SIDED = 1u << 2,
};

// How a view picks a level out of a mesh's LOD chain.
typedef struct LodSelect {
    vec3 eye;     // where projected size is measured from
    float bias;   // > 1 holds detail longer, < 1 drops it sooner
    bool enabled; // false pins every item to level 0
} LodSelect;

typedef struct DrawItem {
    Mesh* mesh;             // never NULL: an item exists because a mesh does
    struct SceneNode* node; // transform; never the geometry
    uint8_t lane;
    uint8_t flags;
    // Level from this mesh's chain, chosen from the camera and used by every
    // pass. A per-light level would let a caster's depth-map silhouette
    // disagree with the surface it is shaded with, which reads as acne.
    uint8_t lod;
} DrawItem;

typedef struct DrawList {
    DrawItem* items;
    size_t count;
    size_t capacity;

    // Population per lane, known at build. Two of the three late-pass gate
    // counts are exactly these, so nothing has to scan for them.
    size_t lane_count[DRAW_LANE_COUNT];

    // Nodes wanting an axis gizmo. A second, much smaller output of the same
    // walk rather than a lane: a gizmo has no mesh, so as a lane it would put a
    // NULL mesh in a list whose whole contract is that every item is drawable,
    // and every consumer -- including the culler and the batcher still to come
    // -- would carry a skip for it.
    struct SceneNode** gizmos;
    size_t gizmo_count;
    size_t gizmo_capacity;

    // Frame index and graph epoch, folded. The epoch is what makes reuse safe:
    // an app that rebuilds geometry mid-frame invalidates a list already built
    // and held by an earlier pass, and reusing it would draw freed meshes. The
    // frame index on top means a graph nothing touched still refreshes once a
    // frame, so nothing can go stale by more than that.
    uint64_t stamp;
    bool valid;
} DrawList;

void draw_list_free(DrawList* list);

// Bumped by every mutation the list would have to see: a node or mesh added or
// freed, a mesh uploaded (the list refuses vao == 0), a material's alpha mode or
// caster-relevant textures changed.
//
// Global rather than a Scene field because the mutators that matter have no way
// back to a Scene -- free_mesh and add_mesh_to_node take a Mesh and a SceneNode.
// That is the same reason materials_dirty needs an explicit marker, and the
// reason this one is bumped by the mutators themselves rather than by callers:
// a rule an app has to remember is a rule an app forgets, and forgetting here
// means drawing freed geometry.
uint64_t scene_graph_epoch(void);
void scene_graph_touched(void);

// Flatten the graph, unless the stamp says the last flattening still describes
// it. Returns false only if it could not grow.
//
// `lod` may be NULL, which selects level 0 throughout -- what a caller that has
// no camera to measure against should pass rather than inventing one.
bool draw_list_build(DrawList* list, struct Scene* scene, uint64_t stamp, const LodSelect* lod);

// What a pass culls against: its frustum, plus the two things that move
// geometry off the import bounds a frustum test would otherwise use.
//
// Carried as a struct for two reasons, and neither is that the members vary by
// pass -- the wind field never does. First, it keeps this file out of the
// scene's wind field and out of render.c's animation-state global, so the
// culler depends on what it is handed rather than on what it can reach.
// Second, and the load-bearing one: replacing the `const Frustum*` this
// REPLACED is what makes the compiler find every cull site. A site left on the
// old signature does not build, where a site left on the old BOUND would
// silently pop geometry.
//
// Build one with render_cull_view, at the pass. The pose is written by the app
// from inside its own render callback, so the shadow pass and the camera pass
// legitimately see different ones -- and a view built anywhere but the pass can
// describe a pose that pass is not about to upload.
//
// A NULL view, or a NULL frustum inside one, accepts everything -- which is how
// a pass says it does not cull.
typedef struct CullView {
    const Frustum* frustum;
    const struct Wind* wind;           // the scene's field; NULL = nothing sways
    const struct AnimationState* pose; // the live pose; NULL = every mesh is at bind
} CullView;

// Whether this item survives the frustum.
//
// One function rather than the expression, because every pass has to reach the
// same answer twice -- once deciding whether to draw an item and once deciding
// whether the item can join the run in front of it -- and those two answers
// disagreeing is not a missed cull but a wrong picture: the batch submits
// whatever the chunk holds, so an item accepted by one test and rejected by
// the other shifts every instance behind it. Four hand-written copies of this
// had already drifted on the null guard.
bool draw_item_visible(const DrawItem* item, const CullView* view);

// Whether two items are the same DRAW: same geometry at the same level.
//
// Split from the visibility half below because a pass that settles visibility
// once, up front, still has to ask this -- and asking it by spelling the
// comparison out is how the copies above drifted. The level joins the key
// because one draw submits one index range, so two instances of a mesh at
// different distances cannot share a draw.
bool draw_run_key_equal(const DrawItem* head, const DrawItem* next);

// Whether `next` can ride in the same draw as the run that `head` started:
// the key above, and visible under the same frustum. The caller adds its own
// "does this pass want it" test -- that part differs per pass, this part does
// not.
bool draw_run_can_join(const DrawItem* head, const DrawItem* next, const CullView* view);

// Copy one lane out of `src` into `dst`, ordered coarsely front-to-back so early
// depth rejection has something to reject with, then by material and mesh so the
// batcher still finds its runs. `dst` is a scratch list owned by the caller;
// `src` is left untouched.
//
// A COPY rather than a permutation of `src`, because `src` is the stamp-guarded
// list every other pass reads and a camera-relative order means nothing in a
// light's frustum. This used to add "and the shadow pass forms its runs from
// adjacency too" -- it no longer does, it builds its own order, and it would not
// care what this one did either way.
//
// Depth is BUCKETED, and that is the whole design rather than a shortcut. An
// exact front-to-back sort puts every item at a distinct key, so the batcher --
// which joins only CONSECUTIVE items sharing (mesh, lod) -- finds runs of one
// and instancing collapses. Quantising to DRAW_SORT_DEPTH_BUCKETS leaves
// identical meshes adjacent inside a bucket, so ordering and batching both get
// most of what they want. The bucket count is the knob between them: more
// buckets order better and batch worse.
//
// eye is where distance is measured from, and it must be THIS pass's camera --
// a cube-capture face re-enters with a different one, and sorting a face against
// the main camera would order it backwards.
bool draw_list_sort_lane(DrawList* dst, const DrawList* src, uint8_t lane, const vec3 eye);

// Buckets spanning the drawn depth range. A starting value, not a measured one:
// the arms that would settle it compare draw count against depth complexity, and
// both move with it in opposite directions.
#define DRAW_SORT_DEPTH_BUCKETS 32

#endif // DRAW_LIST_H
