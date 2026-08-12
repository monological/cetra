#ifndef DRAW_LIST_H
#define DRAW_LIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "intersect.h"
#include "mesh.h"

struct Scene;
struct SceneNode;

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
    // The AABB does not bound where this mesh actually draws, so a frustum test
    // on it would reject geometry that is really inside. Skinned meshes carry
    // their BIND-POSE bounds (calculate_aabb runs once at import, never per
    // animated frame) and wind displaces vertices the shader computes.
    //
    // The camera pass has always culled both anyway, and the visible artefact
    // is the same either way -- but on the camera the wrong answer costs a
    // popped object at the screen edge, where in a shadow map it costs a
    // missing shadow in the middle of a lit scene. Not worth inheriting.
    DRAW_UNBOUNDED = 1u << 3,
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
    // Level from this mesh's chain, chosen once per frame from the CAMERA, and
    // used by every pass including the shadow layers.
    //
    // Not per view, deliberately. A caster drawn into the depth map at a coarser
    // level than the camera shades it with gets a silhouette that disagrees with
    // its own surface, which reads as self-shadow acne along the seam -- so the
    // saving of a per-light level would be paid for in exactly the artefact
    // shadows are hardest to debug. The cube-capture faces inherit the main
    // camera's levels for the same reason, though they see the scene from
    // somewhere else entirely.
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

// Whether this item survives the frustum. A NULL frustum accepts everything,
// which is how a pass says it does not cull.
//
// One function rather than the expression, because every pass has to reach the
// same answer twice -- once deciding whether to draw an item and once deciding
// whether the item can join the run in front of it -- and those two answers
// disagreeing is not a missed cull but a wrong picture: the batch submits
// whatever the chunk holds, so an item accepted by one test and rejected by
// the other shifts every instance behind it. Four hand-written copies of this
// had already drifted on the null guard.
bool draw_item_visible(const DrawItem* item, const Frustum* frustum);

// Whether `next` can ride in the same draw as the run that `head` started:
// same geometry AT THE SAME LEVEL, and visible under the same frustum. The
// caller adds its own "does this pass want it" test -- that part differs per
// pass, this part does not.
//
// The level joins the key because one draw submits one index range. Two
// instances of a mesh at different distances are the same geometry and still
// cannot share a draw, which is not a limitation: grouping by level is what
// makes a hundred distant copies collapse into one cheap draw.
bool draw_run_can_join(const DrawItem* head, const DrawItem* next, const Frustum* frustum);

#endif // DRAW_LIST_H
