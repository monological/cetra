#ifndef _TERRAIN_STREAM_H_
#define _TERRAIN_STREAM_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "terrain.h"

/*
 * Terrain streaming (spec 11.69), owned here: the on-disk tiled pyramid and the
 * resident windows over it.
 *
 * A field big enough to be worth eroding is too big to hold -- 291 MB at 4097
 * square, 1.16 GB at 8193 -- so the stored surface moves to a file and only a
 * rectangle of it stays in memory per level, anchored near the camera and the
 * player. The coarse tail of the pyramid stays whole, which is what makes a
 * miss ANSWERABLE rather than fatal: a query the fine window cannot serve falls
 * to a level that covers twice the world, and keeps falling until it reaches a
 * level that covers all of it.
 *
 * WINDOWS rather than a page table with an indirection per lookup, because the
 * access pattern here is not the one virtual texturing is shaped for. Every
 * heavy consumer -- a patch mesh, a collider, the scatter's rejection sampling
 * -- walks a contiguous square, and the sampler is called on the order of
 * thirty thousand times per patch build. A window costs one containment test
 * per sample and a subtraction per tap; a page table costs a lookup per corner
 * of a 4x4 footprint that straddles up to four tiles. The second thing windows
 * buy is determinism: a window's position is a pure function of the anchor
 * path, so there is no eviction order to get wrong and no hysteresis to tune.
 *
 * What a caller has to know is in terrain_stream_ensure_rect's comment: the
 * miss policy is safe but it is not exact, so anything building a PERSISTENT
 * artifact ensures first.
 */

// Nodes on a tile's edge. The unit of I/O and of window movement: a window
// slides by whole tile columns so its overlap can be kept rather than re-read.
//
// 64 puts a level-0 tile at 64 KB with all four planes, which is one read; it
// also sits inside every consumer's rect at forest's scale, where a 250-unit
// region spans two to three tiles and a patch spans well under one.
#define TERRAIN_STREAM_TILE_NODES 64

// Window edge, in tiles, for the levels that stream. Level 0 gets its own,
// derived from the coverage the caller asks for -- it is the level region loads
// need out to their whole radius, where a coarse level is only ever drawn.
#define TERRAIN_STREAM_WINDOW_TILES 8

// Levels at or under this node count stay WHOLE and resident for the life of
// the stream. This is the miss policy's floor, so it is sized for what strays
// and teleports read rather than for what is cheap: 1025 square is 4.2 MB and
// covers the domain at a cell coarse enough that no window can be missing.
//
// A field whose finest level is already under it -- forest's 513 -- is
// therefore resident in its entirety, and streaming quiesces into an exact
// identity with no special case anywhere.
#define TERRAIN_STREAM_RESIDENT_RES 1025

// Nodes of slack added around an ensured rect. The bicubic footprint reaches
// [i-1, i+2] about the sample, and terrain_normal_at widens that again by its
// own step -- so the two together are covered here rather than restated at
// every call site.
#define TERRAIN_STREAM_APRON_NODES 4

// Tiles the budgeted advance may read per update, and the ceiling the flag
// clamps to. Sixteen level-0 tiles is a megabyte a frame against a walking
// demand of about two tiles a second, so the budget is a bound on a spike
// rather than a throttle on the common case.
#define TERRAIN_STREAM_BUDGET     16
#define TERRAIN_STREAM_BUDGET_MAX 64

// Deep enough for any field terrain_field_build_pyramid can produce: 8193
// squared halves to twelve levels, 32769 to fourteen.
#define TERRAIN_STREAM_MAX_LEVELS 16

// File magic and version. A format change bumps the version rather than
// widening the header, so a stale file is refused by name instead of being read
// with the wrong field offsets.
#define TERRAIN_STREAM_MAGIC   0x52455443u // 'CTER' little-endian
#define TERRAIN_STREAM_VERSION 1u

/*
 * One pyramid level's residency.
 *
 * `win_*` is where the window sits in this level's node space and how wide it
 * is; `resident` says which of its tiles have actually been read. Both, rather
 * than one derived from the other, because a window that has moved but not yet
 * filled is the normal state under a budget.
 *
 * A BITMAP and not a filled rectangle, which is what this held first: an
 * ensure covers the square a caller named, so it fills tiles the budgeted
 * advance had not reached and leaves the read set a shape no rectangle
 * describes. Testing it costs at most four byte loads, since a 4x4 node
 * footprint straddles at most two 64-node tiles per axis.
 */
typedef struct TerrainStreamLevel {
    int res;   // nodes per side at this level
    int tiles; // tiles per side, covering res nodes with the last one padded
    uint64_t offset;
    float* mem[4]; // height, flow, deposit, wear -- masks on level 0 only

    int win_x0, win_z0; // window origin, node coords, a multiple of the tile
    int win_nodes;      // window edge in nodes; equals res when whole
    int win_tiles;      // tiles per side covering the window

    unsigned char* resident; // win_tiles squared; 1 = read

    bool whole; // res <= resident_res: window is the level and never moves
} TerrainStreamLevel;

/*
 * An open streamed field.
 *
 * `field` is the seam: it is what gets installed on TerrainParams, and its
 * `stream` member points back here so the samplers can tell the two residencies
 * apart. Its plane pointers address WINDOW memory, so the res-by-res indexing
 * every other field admits is invalid on this one -- see terrain.h's note on
 * TerrainField.stream.
 */
typedef struct TerrainStream {
    TerrainField field;
    FILE* f;
    char path[512];

    int level_count;
    TerrainStreamLevel lv[TERRAIN_STREAM_MAX_LEVELS];

    int tile_nodes;
    int resident_res;   // the whole-level threshold in force for this stream
    int window_tiles;   // window edge for the streaming levels
    int budget;         // tiles per update
    int coarsest_whole; // the level the miss policy terminates at

    /*
     * The masks a query outside level 0's window falls to, resident whole for
     * the life of the stream.
     *
     * They exist because zero is the one answer that is not available: an
     * absent flow mask reads as "no water has run here", which is a sentence
     * the scatter believes. Unlike the height fall this one is an
     * APPROXIMATION -- the masks have no in-memory pyramid for it to agree
     * with, so no unstreamed run reproduces these values. Every consumer that
     * would care ensures its rect first and reads the window instead.
     */
    float* coarse_mask[3];
    int mask_res; // 0 = the file carried none

    // Lifetime counters, for the probe. `loaded` counts every tile read,
    // including the whole-level fill at open, so it is never zero on a stream
    // that opened -- which is what an identity arm needs to tell a working
    // stream from a feature that never armed. `misses` counts reads the fine
    // level could not serve, counted at the read rather than at the fall, since
    // one query can fall more than once.
    unsigned long long loaded;
    unsigned long long ensured;
    unsigned long long misses;

    // The level the most recent height read settled on. Instrumentation, like
    // the counters, and the probe's whole leverage: asking for that level
    // explicitly re-enters the resolver at a level that CONTAINS, so the answer
    // is the fall's own without going through it, and the two can be held
    // against each other from outside the process.
    int last_level;

    bool warned_capacity; // the once-warn for a rect wider than its window
} TerrainStream;

/*
 * Write `field` and its pyramid to a tiled file.
 *
 * The levels written ARE the levels in memory, never re-filtered on the way in
 * or out: a stream's coarse answer has to be the same float the unstreamed
 * pyramid would have returned, and a second filtering pass is a second place
 * for that to stop being true. Refuses a field with no coarse levels, because
 * such a file has no miss policy -- a node-centred grid halves only while
 * res - 1 is even, so this is the (res = 2^k + 1) rule arriving as a refusal.
 */
bool terrain_stream_save(const TerrainField* field, const char* path);

/*
 * Open a tiled file. Returns NULL having warned by name on anything it does not
 * recognise, so a caller falls back to its analytic terrain whole rather than
 * running on half a field.
 *
 * `l0_coverage` is the world radius level 0 must be able to serve without
 * falling -- region loads need exact heights out to their own radius, and it is
 * that, not the camera, that sizes the finest window. Reads the header, the
 * level table and every whole level; the streaming levels are filled by
 * terrain_stream_update and terrain_stream_ensure_rect.
 */
TerrainStream* terrain_stream_open(const char* path, float extent, float l0_coverage,
                                   int resident_res, int window_tiles, int budget);

void terrain_stream_free(TerrainStream* s);

/*
 * Re-target the windows on this frame's anchors and read up to `budget` tiles
 * toward them.
 *
 * Two anchors for the same reason residency has two: the camera decides what is
 * DRAWN, so it aims the coarse levels, and the player decides what is STOOD ON,
 * so it aims level 0. Windows snap to tile columns and re-target only when the
 * anchor has left a dead-band, or a camera breathing across a tile boundary
 * would re-read the same column every frame.
 */
void terrain_stream_update(TerrainStream* s, const TerrainParams* p, const vec3 eye,
                           const vec3 focus);

/*
 * Make a world-space square exactly readable at `level`, synchronously and
 * without a budget, padding by the apron the sampler needs.
 *
 * This is the other half of the miss policy, and the rule for when to call it
 * is: anything that builds a PERSISTENT artifact -- a collider Jolt will copy
 * into a BVH, a scatter placement, a patch mesh the quadtree caches -- ensures
 * first, because a coarse answer there is not a softer picture but a wrong one
 * that outlives the residency state that produced it. Anything transient reads
 * straight through and takes the fall.
 *
 * May re-anchor a window, which is what makes a teleport work. Returns false
 * having warned once if the rect is wider than the window can hold; the caller
 * gets coarse data rather than a failure, since refusing to build a collider is
 * worse than building a smooth one.
 */
bool terrain_stream_ensure_rect(TerrainStream* s, const TerrainParams* p, float x0, float z0,
                                float span, int level);

// One residency row, and at `final` the per-level and fallback blocks with it.
// Printed by the app on its own cadence, like every other probe here.
void terrain_stream_probe(const TerrainStream* s, const TerrainParams* p, size_t frame, bool final);

#endif // _TERRAIN_STREAM_H_
