#ifndef _TERRAIN_H_
#define _TERRAIN_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "../mesh.h"

// Procedural heightfield terrain, as fbm over a private Perlin table.
//
// The height is a pure function of (params, x, z) with no global state, which is
// what lets the visual tiles, the physics collider and the scatter placement all
// agree without any of them storing a grid. Nothing here allocates a heightmap.
//
// Tiles rather than one mesh because a tile is the unit of BOTH frustum culling
// and LOD selection -- a single kilometre-wide mesh is always visible, always at
// level 0, and is exactly the "mega-mesh with extra steps" the roadmap's D4 entry
// warns about.

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
} TerrainParams;

// Centred on the origin deliberately: the outermost shadow cascade is fitted
// around a hardcoded {0,0,0}, so terrain placed anywhere else loses its far
// shadows with no diagnostic.
TerrainParams terrain_default_params(void);

// Surface height at a world XZ. A pure function of (params, x, z): the same
// arguments always give the same answer, and nothing has to be initialised
// first. NOT thread-safe -- it memoizes its permutation table in file statics,
// because a build evaluates this around a million times.
float terrain_height_at(const TerrainParams* p, float x, float z);

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
