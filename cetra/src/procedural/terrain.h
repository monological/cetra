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

    // The height range this field actually spans, maintained by whoever fills it.
    //
    // It is here because a field is not obliged to cover the range the fbm would
    // have: an imported heightmap declares its own, and erosion moves the
    // extremes. Without it terrain_tint normalised altitude against
    // TerrainParams.height -- the AMPLITUDE of a noise field that is inert once a
    // field is installed -- so a file loaded over 0..1200 into an app whose params
    // say 95 put the whole terrain twelve times over the snow line. max_y <= min_y
    // means "not stated" and the tint falls back to the params.
    float min_y;
    float max_y;
} TerrainField;

// Distance between adjacent field nodes, in world units.
//
// One definition because it is a contract rather than a convenience: the node
// convention above puts texel res-1 ON +extent, so the divisor is res-1 and not
// res, and a consumer that gets that wrong is off by one cell across the whole
// terrain. It was hand-written in seven places before this existed.
static inline float terrain_field_cell(float extent, int res) {
    return res > 1 ? (2.0f * extent) / (float)(res - 1) : 0.0f;
}

// World coordinate of node i along either axis -- the exact inverse of the
// mapping the sampler uses, sharing terrain_field_cell so a seed and a sample
// cannot disagree about where a node is.
static inline float terrain_field_node(float extent, int res, int i) {
    return -extent + terrain_field_cell(extent, res) * (float)i;
}

// Allocate (zeroed) / release all four planes. False leaves the struct zeroed.
bool terrain_field_alloc(TerrainField* field, int res);
void terrain_field_free(TerrainField* field);

// Recompute min_y/max_y from the heights currently stored. Every path that fills
// or edits a field owes this call, since the tint reads the result.
void terrain_field_measure(TerrainField* field);

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

    // Whether a tile's vertex colours carry MACRO VARIATION or the ground's
    // colour itself (spec 11.60).
    //
    // false is every frame built before layers existed: the tint IS the terrain's
    // colour, blended from slope, altitude and the erosion masks. true is for a
    // layered material, where the layers already carry what the ground is made
    // of -- so a tint here would multiply one colour by another and the ground
    // comes out near black. What vertex colour is still good for at 2.6 units is
    // the thing layers cannot do: the slow drift that stops a kilometre of one
    // layer reading as one flat colour.
    bool layered;
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

// Bake the erosion masks into a splat map -- `res` square, row-major, RGB,
// `res * res * 3` bytes the caller allocates -- where .r/.g/.b are the weights
// of a layered material's layers 1, 2 and 3 and layer 0 takes the remainder.
//
// The mapping is rock / silt / gravel over a grass remainder, and it is HERE
// rather than in the app because it is the statement of what the erosion masks
// MEAN as materials. An app choosing four different grounds re-authors the
// layers; it does not re-derive which mask implies which.
//
// Sampled at the field's own nodes through terrain_mask_at, so a splat baked at
// the field's resolution is an exact resample rather than an interpolation of an
// interpolation. Slope joins the masks here because a cliff is bare rock whether
// or not any water ever ran down it, and no erosion mask says so.
bool terrain_bake_splat(const TerrainParams* p, int res, unsigned char* out_rgb);

// Print sampled heights, normals and masks to stdout, in the --water-fft-probe
// idiom. The only way to see a field wired to the wrong world scale, read with
// its axes transposed, or clamped to zero outside its domain -- all of which
// render as perfectly plausible terrain.
void terrain_height_probe(const TerrainParams* p);

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
