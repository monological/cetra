#ifndef _TERRAIN_QUADTREE_H_
#define _TERRAIN_QUADTREE_H_

#include <stdbool.h>
#include <stddef.h>
#include <cglm/cglm.h>

#include "terrain.h"
#include "../material.h"
#include "../scene.h"

// A CDLOD quadtree over a terrain's domain (spec 11.63), replacing the fixed
// tiles x tiles grid.
//
// What a fixed grid cannot do is stay the same size as the world grows: 64 tiles
// at a kilometre is 1,024 at four and 6,400 at ten, all resident, all drawn, all
// at one resolution. A quadtree draws a RING structure instead -- fine patches
// near the camera and coarse ones away -- so the patch count is a function of how
// many levels there are and not of how much ground they cover, and the world can
// grow by adding a level.
//
// Patches are built on the CPU off terrain_height_at and cached. That is a
// decision rather than an implementation detail: the alternative -- one shared
// unit-square grid displaced in the vertex shader by a height TEXTURE -- makes
// the drawn surface a different function from the one the collider, the scatter
// and the camera's floor clamp agree through, and there is nowhere in it to put
// the per-vertex tint.

typedef struct TerrainQuadtree TerrainQuadtree;

// `levels` counts the tree's depth, with level 0 the finest; the root is one
// node at level levels-1 covering the whole domain. `segments` is quads along a
// patch edge and must be even (terrain.h says why).
//
// `params` and `material` are BORROWED and must outlive the quadtree. params in
// particular is read on every patch build, so an app that shifts its terrain's
// centre has to invalidate -- see terrain_quadtree_invalidate.
TerrainQuadtree* create_terrain_quadtree(const TerrainParams* params, int levels, int segments,
                                         Material* material);

// `root` is the node the selection was attached to, so its children can be
// detached before they are freed. NULL if nothing was ever attached.
void free_terrain_quadtree(TerrainQuadtree* qt, SceneNode* root);

// Descend against `eye` -- a position in the same space patch vertices are
// stored in -- and make `root`'s children exactly the selected patches. Patches
// that leave the selection are detached and kept; nothing is rebuilt to bring one
// back. Returns how many are selected.
//
// Deliberately NOT frustum-aware. The shadow pass draws from the same graph and
// needs the patches behind the camera, and the draw list already culls per item.
int terrain_quadtree_update(TerrainQuadtree* qt, SceneNode* root, const vec3 eye);

// There is no "pin every patch to the finest level" knob, and --no-lod does not
// reach here. --no-lod is a per-mesh statement -- draw this mesh at level 0 --
// and a patch IS at level 0; it has no chain to hold back. Pinning the DESCENT
// instead is a different thing wearing the same name, it costs 4^levels patches
// (16,384 at four kilometres against 706), and what it would be for is answered
// by --no-quadtree, which draws the fixed grid at its own finest cell.

// Drop every cached patch and immediately re-select against `eye`. For an origin
// shift: patch vertices are baked in storage space, so a shift makes all of them
// wrong at once and rebuilding from the moved params is the only repair.
//
// A fresh build rather than subtracting the delta from the stored vertices,
// which would be cheaper and is what every OTHER node in the scene gets. A patch
// carries absolute coordinates rather than a transform, and translating them
// would leave the low bits that the old origin's magnitude rounded away -- which
// is the precision the shift exists to recover. It costs one rebuild of the whole
// selection, on a frame that is already reconciling physics.
void terrain_quadtree_rebuild(TerrainQuadtree* qt, SceneNode* root, const vec3 eye);

typedef struct TerrainQuadtreeStats {
    int levels;
    int segments;
    int selected;        // patches in the current selection
    int resident;        // patches held in the cache
    size_t built;        // patches built since creation
    size_t triangles;    // triangles in the current selection
    float split_factor;  // how many patch widths from the camera a level survives
    int level_count[16]; // selection, by level
} TerrainQuadtreeStats;

void terrain_quadtree_stats(const TerrainQuadtree* qt, TerrainQuadtreeStats* out);

// Print the selection and the morph windows, in the --water-fft-probe idiom.
//
// The instrument exists because the two things that can be wrong here are both
// invisible in a frame: a selection that descends further than it needs looks
// exactly like a correct one but costs, and a morph window off by a level makes
// a crack that opens for a few frames as the camera crosses a band.
//
// NOT const: answering whether a patch morphs onto its parent's surface means
// building that parent, which the selection has no reason to have asked for.
void terrain_quadtree_probe(TerrainQuadtree* qt);

#endif // _TERRAIN_QUADTREE_H_
