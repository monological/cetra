#ifndef _TERRAIN_H_
#define _TERRAIN_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "../mesh.h"

// Procedural heightfield terrain, as fbm over a private Perlin table -- or, since
// spec 11.59, as a filtered sample of a stored grid.
//
// The height is a pure function of (params, x, z), which is what lets the visual
// tiles, the physics collider and the scatter placement all agree without any of
// them storing a grid of their own. That property is what the SOURCE has to
// preserve, not the analytic form: a filtered grid sample is equally a pure
// function of position, so installing a field changes where the number comes from
// and nothing else.
//
// Tiles rather than one mesh because a tile is the unit of BOTH frustum culling
// and LOD selection -- a single kilometre-wide mesh is always visible, always at
// level 0, and is exactly the "mega-mesh with extra steps" the roadmap's D4 entry
// warns about.

// Which erosion byproduct terrain_mask_at reads.
//
// These are the reason the erosion bake exists, more than the silhouette is: a
// sim knows where water FLOWED, so it can put gravel in stream beds and bare rock
// on scoured ridges. Deriving the same thing from slope and altitude cannot, which
// is why terrain shaded that way reads as procedural however good the palette.
typedef enum TerrainMask {
    TERRAIN_MASK_FLOW,    // water throughput -- stream beds, drainage lines
    TERRAIN_MASK_DEPOSIT, // sediment dropped -- valley silt, outwash fans
    TERRAIN_MASK_WEAR,    // material removed -- exposed bedrock
} TerrainMask;

// A stored heightfield, spanning the same [-extent, +extent] square the analytic
// terrain does, sampled at NODES: texel 0 sits exactly on -extent and texel res-1
// exactly on +extent, so a field's corners are the terrain's corners. This is the
// convention every heightmap tool uses and the one that keeps an imported field
// agreeing with the mesh built from it.
//
// All four planes are allocated together and zero-filled. The masks are not
// optional-and-absent: an import that carries no erosion data leaves them zero,
// which reads as "no water has run here" and is true. Making them nullable would
// buy a few megabytes and cost a branch at every read.
typedef struct TerrainField {
    int res;       // res x res, row-major, res >= 2
    float* height; // world Y
    float* flow;   // the three masks, each nominally [0,1]
    float* deposit;
    float* wear;
} TerrainField;

// Allocate (zeroed) / release all four planes. False leaves the struct zeroed.
bool terrain_field_alloc(TerrainField* field, int res);
void terrain_field_free(TerrainField* field);

typedef struct TerrainParams {
    float extent; // half-width; the terrain spans [-extent, +extent] on X and Z
    float height; // peak displacement, in the same units as extent

    // Lowest octave, in cycles per world unit. Each further octave multiplies
    // frequency by `lacunarity` and amplitude by `gain`.
    float base_freq;
    float lacunarity;
    float gain;
    int octaves;

    unsigned seed;

    int tiles;         // tiles per side; the grid is tiles x tiles
    int tile_segments; // quads along one tile edge

    // NULL = evaluate the fbm above. Non-NULL = sample this instead, and every
    // noise field in this struct becomes inert for HEIGHT (the tint's own two
    // still run). Borrowed, never owned: whoever baked or loaded it frees it.
    const TerrainField* field;
} TerrainParams;

// Centred on the origin deliberately: the outermost shadow cascade is fitted
// around a hardcoded {0,0,0}, so terrain placed anywhere else loses its far
// shadows with no diagnostic.
TerrainParams terrain_default_params(void);

// Fill field->height by evaluating the ANALYTIC fbm at every node, ignoring any
// field already installed on params. This is how a field is seeded before a sim
// runs over it, and it is why erosion does not replace the noise: the landscape
// still comes from the fbm, and the sim only decides what water did to it.
bool terrain_field_seed(TerrainField* field, const TerrainParams* params);

// Surface height at a world XZ. A pure function of (params, x, z): the same
// arguments always give the same answer, and nothing has to be initialised first.
//
// OUTSIDE [-extent, +extent] the two sources answer differently in form but not in
// kind. The fbm continues, because it is defined everywhere. A field CLAMPS to its
// edge row -- callers do query outside the domain (the third-person camera eye
// trails the player and leaves it), so returning zero would drop the camera through
// the world and extrapolating a cubic off the end of the table would launch it.
// Same policy the water bed already documents for its own bounded field.
//
// NOT thread-safe, and installing a field does not change that: the ANALYTIC path
// memoizes its permutation table in file statics because a build evaluates this
// around a million times. The field path happens to touch no statics, but the
// function's contract is the weaker of the two and callers may not depend on which
// branch they took.
float terrain_height_at(const TerrainParams* p, float x, float z);

// An erosion byproduct at a world XZ, in [0,1]. Zero everywhere with no field
// installed, so a caller blending by these degrades to the un-eroded look rather
// than to a special case. Same clamp policy as the height.
float terrain_mask_at(const TerrainParams* p, TerrainMask mask, float x, float z);

// Surface normal at a world XZ, by central difference on the height function.
// Used for slope-aware scatter as well as for shading.
void terrain_normal_at(const TerrainParams* p, float x, float z, vec3 out);

// One tile of the visual grid, at (tx, tz) in [0, tiles). Fills positions,
// normals, tangents, UV0 (world-scaled, for tiling detail) and vertex COLOURS,
// which carry the slope and altitude tint -- there is no splat-map system, and a
// per-vertex blend is what stops a kilometre of terrain reading as one flat hue.
//
// Adjacent tiles share their edge vertices exactly at level 0, because both
// sample the same analytic height at the same world coordinate.
//
// That does NOT survive simplification. lod.c builds chains with options 0, and
// meshoptimizer only promotes a border vertex to locked under
// meshopt_SimplifyLockBorder -- without it a border is merely weighted, so a
// tile's perimeter can collapse and two neighbours at different levels can
// T-junction. No crack has been observed at the framings tried so far, and the
// fix if one appears is that flag rather than anything here.
bool terrain_build_tile(const TerrainParams* p, int tx, int tz, Mesh* mesh);

// One mesh spanning the whole terrain at `segments` quads per side, for physics.
//
// Separate from the visual tiles because collision wants one body rather than
// tiles x tiles of them, and because it can afford to be coarser. It cannot be
// MUCH coarser: the character stands on this while the eye sees the tiles, and
// the two diverge by roughly the height change across one collider quad.
bool terrain_build_collider(const TerrainParams* p, int segments, Mesh* mesh);

#endif // _TERRAIN_H_
