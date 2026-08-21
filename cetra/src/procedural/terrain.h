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

// One resolution of a field: the four planes at a level and the node count they
// share. Level 0 aliases the field's own arrays and owns nothing.
typedef struct TerrainFieldLevel {
    int res;
    float* height;
    float* flow;
    float* deposit;
    float* wear;
} TerrainFieldLevel;

// A stored heightfield, spanning the same square about `center` the analytic
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

    // Coarser copies of all four planes (spec 11.63), level 0 aliasing the
    // arrays above. Empty until terrain_field_build_pyramid runs; afterwards a
    // field whose resolution does not halve carries exactly one level, which is
    // not the same thing -- field_level distinguishes them.
    //
    // FILTERED, under a separable [1 2 1] tent rather than subsampled --
    // filter_plane records why, including what the subsample was measured to
    // deliver.
    int level_count;
    TerrainFieldLevel* levels;

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

// Node i's coordinate along either axis, running -extent..+extent about the
// terrain's own middle -- the exact inverse of the mapping the sampler uses,
// sharing terrain_field_cell so a seed and a sample cannot disagree about
// where a node is.
static inline float terrain_field_node(float extent, int res, int i) {
    return -extent + terrain_field_cell(extent, res) * (float)i;
}

// Allocate (zeroed) / release all four planes. False leaves the struct zeroed.
bool terrain_field_alloc(TerrainField* field, int res);
void terrain_field_free(TerrainField* field);

// Build the coarse levels, returning how many the field ends up with (1 = level
// 0 alone). Owed by whoever last WROTE the field: the levels are copies, so a
// sim or a load that runs afterwards leaves them describing the old surface.
//
// A node-centred grid halves only while res - 1 is even, so a 512-node field
// gets no levels at all and a 513-node one gets eight. That is the reason the
// erosion default is 513 rather than the power of two it looks like it should be.
int terrain_field_build_pyramid(TerrainField* field);

// Recompute min_y/max_y from the heights currently stored. Every path that fills
// or edits a field owes this call, since the tint reads the result.
void terrain_field_measure(TerrainField* field);

typedef struct TerrainParams {
    float extent; // half-width on X and Z, measured from `center`
    // World XZ the domain is centred on, so the terrain spans
    // [center - extent, center + extent]. Zero is every terrain built before
    // this field existed.
    //
    // The height function answers in WORLD coordinates because that is what its
    // callers hold, so a terrain that does not know where it sits can only be at
    // the origin -- which is what it was.
    vec2 center;
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

    // Island shaping (spec 11.63): where the ground starts falling toward the
    // sea, as a fraction of the half-extent, and how far below zero it has got
    // by the domain's edge. `island_start` outside (0,1) is OFF, which is every
    // terrain built before this existed.
    //
    // Applied to the ANALYTIC height only, which is what makes it a shape rather
    // than a post-process: terrain_field_seed writes the fbm into a field, so a
    // baked or eroded island is eroded AS an island -- water cuts its valleys
    // down to a shoreline that exists. A field loaded from a file is left alone,
    // because a file is a statement about what the terrain is.
    //
    // The radius is Euclidean and normalised by the half-extent, so it passes 1
    // at the edge MIDPOINTS and reaches sqrt(2) at the corners: a square domain
    // with a round island in it, and the corners are open sea.
    float island_start;
    float island_depth;

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

// World XZ -> the domain's own coordinate, running 0 .. 2*extent from the
// -X,-Z corner. Every field lookup and every noise lattice in here is indexed
// from that corner, so this is the ONE place a terrain's placement is applied.
//
// The subtraction comes FIRST and the order is load-bearing far from the
// origin: (x - center) is exact while the two are within a factor of two of
// each other, so the add that follows rounds at the DIFFERENCE's magnitude
// rather than at x's. Adding extent to x first rounds at x's magnitude and
// then subtracts, which loses low bits the caller still has.
static inline void terrain_to_domain(const TerrainParams* p, float x, float z, float* out_x,
                                     float* out_z) {
    *out_x = (x - p->center[0]) + p->extent;
    *out_z = (z - p->center[1]) + p->extent;
}

// A CENTRED coordinate (-extent .. +extent about the terrain's middle) to world.
// The generators walk the domain and have to say where in the world each step
// landed.
//
// NOT the inverse of terrain_to_domain, which answers in CORNER coordinates
// (0 .. 2*extent). The two differ by `extent`, so composing them is a silent
// half-domain error -- terrain_field_node is what produces the form these take.
static inline float terrain_world_x(const TerrainParams* p, float local_x) {
    return local_x + p->center[0];
}
static inline float terrain_world_z(const TerrainParams* p, float local_z) {
    return local_z + p->center[1];
}

TerrainParams terrain_default_params(void);

// Fill field->height by evaluating the ANALYTIC fbm at every node, ignoring any
// field already installed on params. This is how a field is seeded before a sim
// runs over it, and it is why erosion does not replace the noise: the landscape
// still comes from the fbm, and the sim only decides what water did to it.
bool terrain_field_seed(TerrainField* field, const TerrainParams* params);

// Surface height at a world XZ. A pure function of (params, x, z): the same
// arguments always give the same answer, and nothing has to be initialised first.
//
// OUTSIDE the domain the two sources answer differently in form but not in
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

// The same surface with everything below `level`'s own cell removed, for
// geometry that will be sampled too coarsely to carry it (spec 11.63).
//
// LEVEL 0 IS terrain_height_at, bit for bit. Above it the two sources drop detail
// the same way and for the same reason -- the field reads a filtered mip, the fbm
// drops the octaves whose wavelength is under the cell. A coarse mesh that
// instead point-samples the full surface does not draw a smoother version of it,
// it draws an arbitrary one, and the silhouette changes for no reason every time
// a patch switches level.
//
// The fbm's dropped octaves are removed from the SUM and left in the
// normalisation, so a level is the fine surface minus what it cannot resolve
// rather than the remainder scaled back up to full amplitude.
float terrain_height_at_level(const TerrainParams* p, float x, float z, int level);
void terrain_normal_at_level(const TerrainParams* p, float x, float z, int level, vec3 out);

// How many levels this terrain offers, at least 1.
int terrain_level_count(const TerrainParams* p);

// The coarsest level whose own cell is no larger than `cell`, clamped into
// range. This is how a consumer says how finely it intends to sample rather than
// picking a level index, and it is what keeps a patch and its parent on adjacent
// levels without either of them knowing about the other.
int terrain_level_for_cell(const TerrainParams* p, float cell);

// The cell a level describes, in world units: the field's node spacing at that
// level, or -- with no field -- the Nyquist limit of the fbm's finest surviving
// octave, doubling per level.
float terrain_level_cell(const TerrainParams* p, int level);

// An erosion byproduct at a world XZ, in [0,1]. Zero everywhere with no field
// installed, so a caller blending by these degrades to the un-eroded look rather
// than to a special case. Same clamp policy as the height.
float terrain_mask_at(const TerrainParams* p, TerrainMask mask, float x, float z);

// The drainage band over which the ground becomes a channel bed, as
// terrain_bake_splat paints it. Exported because a consumer deciding what may
// stand on that ground has to agree with what the ground looks like, and the
// first version of the scatter's threshold stated the coupling in a comment.
//
// Flow is a LOG of drainage normalised to the catchment peak, so its mean sits
// near 0.4 -- a threshold anywhere near that paints most of the map as riverbed.
#define TERRAIN_CHANNEL_FLOW_LO 0.58f
#define TERRAIN_CHANNEL_FLOW_HI 0.88f

// Bake the erosion masks into a splat map -- `res` square, row-major, RGB,
// `res * res * 3` bytes the caller allocates -- where .r/.g/.b are the weights
// of a layered material's layers 1, 2 and 3 and layer 0 takes the remainder.
//
// The mapping is rock / silt / gravel over a grass remainder, and it is HERE
// rather than in the app because it is the statement of what the erosion masks
// MEAN as materials. An app choosing four different grounds re-authors the
// layers; it does not re-derive which mask implies which.
//
// Sampled at TEXEL CENTRES -- (i + 0.5) / res across the domain -- because
// that is where a texture read lands. Sampling on field NODES instead, which the
// first version did, offsets the whole map by half a texel and stretches it by
// (res-1)/res against the terrain it describes.
//
// Slope joins the masks here because a cliff is bare rock whether or not any
// water ever ran down it, and no erosion mask says so.
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

// One quadtree patch (spec 11.63): the same grid a tile is, over an arbitrary
// square, plus the CDLOD morph targets that let it become its parent's surface.
//
// `segments` must be EVEN, and that is a contract rather than a check that could
// be relaxed. The parent quadtree node covers four times the area at the same
// segment count, so its lattice restricted to this patch is this patch's
// even-indexed subset -- which is only true when the odd/even parity survives the
// half-patch offset an odd child index introduces. At an odd count the two
// lattices interleave and there is no parent vertex to morph to.
//
// The morph window is a distance band on the camera: the patch is its own
// surface at `morph_start` and exactly its parent's at `morph_end`. It is
// written into every vertex rather than uploaded, and `morph_end <= morph_start`
// is the off state, which is what a mesh with no morph attributes already reads
// as.
bool terrain_build_patch(const TerrainParams* p, float x0, float z0, float span, int segments,
                         float morph_start, float morph_end, Mesh* mesh);

// One mesh spanning the whole terrain at `segments` quads per side, for physics.
//
// Separate from the visual tiles because collision wants one body rather than
// tiles x tiles of them, and because it can afford to be coarser. It cannot be
// MUCH coarser: the character stands on this while the eye sees the tiles, and
// the two diverge by roughly the height change across one collider quad.
bool terrain_build_collider(const TerrainParams* p, int segments, Mesh* mesh);

// The same, over one square of it, for a world whose collision is resident in
// pieces (spec 11.63). Adjacent squares share their edge vertices exactly, since
// both evaluate the same height function at the same world coordinates -- which
// is what stops a character finding a seam between two bodies.
//
// Level 0 always, whatever a patch overhead is drawing: the character stands on
// this while the eye sees the mesh, and letting collision coarsen with distance
// would move the ground under a player who walked toward it.
bool terrain_build_collider_region(const TerrainParams* p, float x0, float z0, float span,
                                   int segments, Mesh* mesh);

#endif // _TERRAIN_H_
