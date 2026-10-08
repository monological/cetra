#ifndef _TREE_GEN_H_
#define _TREE_GEN_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "../mesh.h"

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

// A conifer's cards address the same TG_LEAF_VARIANTS cells of a needle-spray atlas: the live
// sprays first, then the browning ones, then a twig bare of needles.
#define TG_SPRAY_LIVE_CELLS 5
#define TG_SPRAY_DEAD_CELLS 2
#define TG_SPRAY_BARE_CELL  (TG_SPRAY_LIVE_CELLS + TG_SPRAY_DEAD_CELLS)
// A leaf card's indices: a quad creased down its mid-rib, two triangles each side.
#define TG_CARD_INDICES 12
// A spray's indices, its two crossed cards: what a conifer's needles thin by as one card, so a
// spray goes whole.
#define TG_SPRAY_INDICES (2 * TG_CARD_INDICES)
// The share of a conifer's sprays each level of detail keeps of the one nearer.
#define TG_SPRAY_KEEP 0.45f

// How a tree grows. The zero is the recursive form, so a zeroed TreeParams keeps it.
typedef enum TreeForm {
    TREE_FORM_RECURSIVE = 0, // a trunk that splits at its tip, and every child again
    TREE_FORM_EXCURRENT = 1, // a conifer: one trunk to the top, whorls of branches up it
} TreeForm;

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

    // The growth form, a TreeForm. The excurrent form reads the fields below, and these above
    // under its own meaning: trunk_length and trunk_radius, twist (degrees each whorl turns
    // from the last), angle_variance, droop and phototropism (a branch's sag and the turn up
    // of its tip), curve_noise (the wander of trunk and branches), and the leaf fields (its
    // needle sprays). The rest above are the recursive form's alone.
    int form;
    float crown_base;    // the bare trunk under the lowest whorl, a fraction of its length
    float crown_width;   // the longest branch, a fraction of the trunk's length
    float crown_shape;   // exponent on the fall of branch length up the crown: 1 a cone
    float whorl_spacing; // trunk length between whorls
    int whorl_size;      // branches a whorl, give or take one
    float branch_pitch;  // degrees below level the lowest branches leave the trunk at
    float spray_angle;   // degrees a needle spray leaves its branch at, in the branch's plane
    float dead_lower;    // the fraction of the crown, from the bottom, whose branches are dead
    float dead_fraction; // the chance a branch lower in the crown has died, bare or browned
    float snag;          // 0 a live leader; toward 1, the top broken off further down
    // 0 a tidy cone; toward 1 the knocks a tree takes in a wood: a kinked trunk, a crown fuller
    // on one side, uneven and missing tiers and branches, gaps, and now and then a dead leader
    // that the branches under it have turned up to replace
    float irregularity;
} TreeParams;

// One sample along a branch spine.
typedef struct BranchPoint {
    vec3 pos;
    vec3 tangent; // normalized spine direction here
    float radius;
    float arc;       // distance along this branch from its base
    float root_dist; // distance from the trunk base, through the hierarchy
} BranchPoint;

// What an excurrent branch's sprays are: green, browned, or gone to bare twigs.
typedef enum BranchHealth {
    BRANCH_LIVE = 0,
    BRANCH_BROWNING,
    BRANCH_BARE,
} BranchHealth;

typedef struct Branch {
    int parent;                  // index into branches[], -1 for the trunk
    int depth;                   // generations from the trunk
    int first_point, num_points; // slice of the shared point pool
    float base_radius, tip_radius, length;
    float phase;        // per-branch wind phase in [0,1)
    float parent_phase; // blended from over the first stretch of arc
    float uv_v0;        // bark v at the base, inherited for continuity
    int uv_tiles_u;     // whole bark tiles around the circumference
    bool is_terminal;   // no children: gets a pointed tip
    bool bears_leaves;
    int bark_segs; // ring segments; 0 = as many as the radius asks for
    BranchHealth health;
} Branch;

typedef struct TreeSkeleton {
    Branch* branches;
    int branch_count, branch_cap;
    BranchPoint* points;
    int point_count, point_cap;
    float max_root_dist; // normalizes the flex weight
} TreeSkeleton;

// Named trees: one statement of each, so every app that names one grows the same tree.
typedef enum TreePreset {
    TREE_PRESET_BROADLEAF = 0, // tall, upright, a narrow crown in leaf
    TREE_PRESET_DEAD,          // leafless, sagging and wandering: wood dead a long time
    TREE_PRESET_SPRUCE,        // a narrow cone of drooping branches, dense to the ground
    TREE_PRESET_FIR,           // flatter tiers held nearer level, a little wider
    TREE_PRESET_SNAG,          // a spruce gone half dead, its top broken off
    TREE_PRESET_COUNT
} TreePreset;

// Overwrites all of `p`, so it starts from zero and nothing a caller set before survives.
void tree_params_preset(TreeParams* p, TreePreset preset, int seed);
// The preset's name as the command line spells it, and back; false for a name it does not know.
const char* tree_preset_name(TreePreset preset);
bool tree_preset_from_name(const char* name, TreePreset* out);

// Grow the skeleton for `p`. Deterministic in p->seed. Safe to call on a
// zeroed struct; call tree_skeleton_free when done.
void tree_skeleton_build(TreeSkeleton* skel, const TreeParams* p);
void tree_skeleton_free(TreeSkeleton* skel);

// Sweep the skeleton into meshes. Each fills a fresh Mesh's arrays (handing
// over ownership), sets the counts and draw mode, and computes the AABB. A
// conifer's meshes also carry their levels of detail: its bark decimated, its
// sprays thinned whole, TG_SPRAY_KEEP of a level to the next. The leaf builder
// may produce nothing, in which case it returns false and the caller should
// discard the mesh.
bool tree_mesh_bark(const TreeSkeleton* skel, const TreeParams* p, Mesh* mesh);
bool tree_mesh_leaves(const TreeSkeleton* skel, const TreeParams* p, Mesh* mesh);

// The foliage atlas the leaf cards of a tree of `form` address: leaf clusters, or a conifer's
// needle sprays. Three buffers the caller owns, all NULL on failure.
void tree_foliage_maps(TreeForm form, int width, int height, unsigned char** out_albedo,
                       unsigned char** out_normal, unsigned char** out_rough);

#endif // _TREE_GEN_H_
