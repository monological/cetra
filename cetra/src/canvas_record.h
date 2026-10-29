#ifndef _CANVAS_RECORD_H_
#define _CANVAS_RECORD_H_

/*
 * What canvas_edit.c makes and canvas.c keeps: the GPU record of one shape,
 * and an edit's steps. Internal to the canvas.
 *
 * A record is one instance of a unit quad, read by canvas_vert.glsl:
 *   p  ivec4  points, as int32 offsets from the origin of the record's tile
 *   f  vec4   the kind's measures, in units
 *   u  uvec4  kind (6 bits) | flags (10) | style (16); net; object; tile
 * The tile in u.w is the index of the record's tile COORDINATE in the canvas's
 * tile table, the same for every layer, so the table holds one offset a tile.
 *
 * By kind:
 *   SEGMENT  p = a, b              f = half width
 *   ARC      p = a, b              f = half width, centre minus the chord's
 *                                      midpoint (x, y), radius
 *   CIRCLE   p = centre, radius,   f unused
 *              stroke
 *   BOX      p = centre, stroke    f = half size x, half size y, corner, angle
 *   EDGE     p = a, b              f unused: the outer half of a polygon edge's
 *                                    ramp, its polygon left of a to b when
 *                                    FLAG_INSIDE_LEFT is set
 * A cut end (FLAG_CUT_*) is where an edit split a shape reaching too far from
 * its tile: it has no cap and no edge ramp, so the pieces meet with no seam.
 */

#include <stdint.h>

#include "../shaders/include/canvas_constants.glsl"
#include "canvas.h"

// The largest radius, stroke or size a record holds: past it the int32 fields
// overflow and the float ones lose units.
#define CANVAS_LENGTH_MAX (INT64_C(1) << 30)

typedef struct CanvasRecord {
    int32_t p[4];
    float f[4];
    uint32_t u[4];
} CanvasRecord;

_Static_assert(sizeof(CanvasRecord) == 48, "canvas_vert.glsl reads a 48-byte record");

static inline uint32_t canvas_record_head(uint32_t kind, uint32_t flags, uint16_t style) {
    return (kind & 0x3fu) | (flags & 0x3ffu) << 6 | (uint32_t)style << 16;
}

/*
 * A corner of a fill triangle, read by canvas_fill_vert.glsl from the layer's
 * fill buffer, three to a triangle:
 *   p     ivec2      this corner, an int32 offset from the tile's origin
 *   near  ivec2 x 9  the outline points a pixel's inner ramp is measured
 *                    against, in the same frame: the three corners of the
 *                    triangle the polygon was cut into, then each one's
 *                    neighbour before and after it on its outline. Six edges,
 *                    every outline edge that meets a corner, which is what
 *                    makes the ramp the same however the polygon was cut:
 *                    an ear thinner than half a pixel leaves most of the
 *                    band inside its outline to the triangle next to it.
 *   u     uvec4      kind FILL and the style, as a record's head; net; object;
 *                    tile
 * The three corners of a triangle carry the same near and u, and a piece
 * clipped from a triangle carries the whole triangle's near.
 */
typedef struct CanvasFillVertex {
    int32_t p[2];
    int32_t near[18]; // corner 0, 1, 2, then corner 0's before and after, 1's, 2's
    uint32_t u[4];
} CanvasFillVertex;

_Static_assert(sizeof(CanvasFillVertex) == 96, "canvas_fill_vert.glsl reads a 96-byte corner");

typedef struct CanvasTriangle {
    CanvasFillVertex v[3];
} CanvasTriangle;

// A record as an edit holds it: what tile it belongs to, with the tile's index
// filled in when the edit is applied.
typedef struct CanvasPiece {
    CanvasLayer* layer;
    int64_t tx, ty;         // its tile
    int64_t min[2], max[2]; // its bound in units
    CanvasRecord record;
} CanvasPiece;

// A fill triangle as an edit holds it, likewise.
typedef struct CanvasFillPiece {
    CanvasLayer* layer;
    int64_t tx, ty;
    int64_t min[2], max[2];
    CanvasTriangle triangle;
} CanvasFillPiece;

typedef enum CanvasStepKind {
    CANVAS_STEP_ADD,       // a run of pieces
    CANVAS_STEP_ADD_FILLS, // a run of fill pieces
    CANVAS_STEP_REMOVE_OBJECT,
    CANVAS_STEP_CLEAR_LAYER,
} CanvasStepKind;

typedef struct CanvasStep {
    CanvasStepKind kind;
    CanvasLayer* layer;  // CLEAR_LAYER
    uint32_t object;     // REMOVE_OBJECT
    size_t first, count; // ADD and ADD_FILLS: the run
} CanvasStep;

struct CanvasEdit {
    int tile_shift;
    CanvasPiece* pieces;
    size_t piece_count, piece_cap;
    CanvasFillPiece* fills;
    size_t fill_count, fill_cap;
    CanvasStep* steps;
    size_t step_count, step_cap;
};

// log2 of a tile's side, fixed when the canvas was made, so safe to read from
// any thread.
int canvas_tile_shift(const Canvas* canvas);

#endif
