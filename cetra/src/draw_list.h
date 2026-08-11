#ifndef DRAW_LIST_H
#define DRAW_LIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
};

typedef struct DrawItem {
    Mesh* mesh;             // never NULL: an item exists because a mesh does
    struct SceneNode* node; // transform; never the geometry
    uint8_t lane;
    uint8_t flags;
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
bool draw_list_build(DrawList* list, struct Scene* scene, uint64_t stamp);

#endif // DRAW_LIST_H
