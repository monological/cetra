#ifndef _TREE_GEN_H_
#define _TREE_GEN_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/mesh.h"

// Procedural tree generation, in two phases.
//
// Phase 1 grows a SKELETON: a pool of curved branch spines, pure math, no GL.
// Phase 2 MESHES that skeleton -- every branch swept into one bark mesh, every
// leaf card into one leaf mesh. Two draw calls for the whole tree, which is why
// the SceneNode hierarchy the old generator built (one node per branch) is gone;
// the curves already carry their own world placement.
//
// Wind data rides in UV1 on both meshes (see the vertex shader's vegetation
// modes): .x is a per-branch phase, .y a flex weight. Both are continuous
// across branch joints, so wind displacement cannot tear an attachment apart.
//
// The material's wind_mode is what declares that, and the PBR shader reads it
// before deciding whether UV1 is a usable texture coordinate set -- so an AO
// map on a vegetation material falls back to UV0 rather than sampling phase and
// flex. Nothing is required of the caller here.

// Leaf cluster variants in the foliage atlas. Each card picks one and may
// mirror it, for 2x this many distinct arrangements -- with only a handful the
// eye picks the repeated sprig out of the canopy immediately.
//
// The atlas tiles along U only: the wind shader reads UV0.y as the flutter
// weight so a card pivots about its stem, and rows would move that pivot.
#define TG_LEAF_VARIANTS 8

// Live-tunable shape. Compared with memcmp to decide when to rebuild, so it
// holds no pointers and must be zeroed before its first assignment.
typedef struct TreeParams {
    int seed;
    int max_depth; // generations of tip splitting
    float trunk_length;
    float trunk_radius;
    int branches_per_node; // children at each tip split
    float length_decay;    // child length as a fraction of its parent's
    float taper;           // tip radius as a fraction of the branch's base
    float branch_angle;    // degrees a child tilts off its parent
    float angle_variance;  // degrees of random tilt jitter
    float twist;           // degrees of azimuth advance between children
    float droop;           // 0..1 gravity bend, stronger on thin branches
    float curve_noise;     // 0..1 directional wander along a spine
    float phototropism;    // 0..1 upward re-straightening toward the tip
    float lateral_density; // side branches per 10 units of parent arc
    float twig_scale;      // length multiplier for the final generation
    int show_leaves;
    float leaf_size;
    float leaf_density; // leaves per 10 units of leaf-bearing arc
} TreeParams;

// One sample along a branch spine.
typedef struct BranchPoint {
    vec3 pos;
    vec3 tangent; // normalized spine direction here
    float radius;
    float arc;       // distance along this branch from its base
    float root_dist; // distance from the trunk base, through the hierarchy
} BranchPoint;

typedef struct Branch {
    int parent; // index into branches[], -1 for the trunk
    int depth;
    int first_point, num_points; // slice of the shared point pool
    float base_radius, tip_radius, length;
    float phase;        // per-branch wind phase in [0,1)
    float parent_phase; // blended from over the first stretch of arc
    float uv_v0;        // bark v at the base, inherited for continuity
    int uv_tiles_u;     // whole bark tiles around the circumference
    bool is_terminal;   // no children: gets a pointed tip
    bool bears_leaves;
} Branch;

typedef struct TreeSkeleton {
    Branch* branches;
    int branch_count, branch_cap;
    BranchPoint* points;
    int point_count, point_cap;
    float max_root_dist; // normalizes the flex weight
} TreeSkeleton;

// Grow the skeleton for `p`. Deterministic in p->seed. Safe to call on a
// zeroed struct; call tree_skeleton_free when done.
void tree_skeleton_build(TreeSkeleton* skel, const TreeParams* p);
void tree_skeleton_free(TreeSkeleton* skel);

// Sweep the skeleton into meshes. Each fills a fresh Mesh's arrays (handing
// over ownership), sets the counts and draw mode, and computes the AABB. The
// leaf builder may produce nothing, in which case it returns false and the
// caller should discard the mesh.
bool tree_mesh_bark(const TreeSkeleton* skel, const TreeParams* p, Mesh* mesh);
bool tree_mesh_leaves(const TreeSkeleton* skel, const TreeParams* p, Mesh* mesh);

#endif // _TREE_GEN_H_
