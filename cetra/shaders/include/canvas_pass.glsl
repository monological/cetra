// How both of the 2D canvas's fragment stages write a layer (spec 14.0). A layer resolves in the
// view's target in passes over the same geometry, the target's alpha its scratch:
//
//   0  coverage, the LARGEST any shape gives a pixel, times the layer's opacity, into alpha
//      alone. The largest and not the union, because a shape cut at a tile edge, and a polygon's
//      outline fringe at a corner, cover one pixel twice with the same coverage: max is
//      idempotent there, and over shapes of one layer it is the coverage of the nearest edge.
//   1  the colour of shapes a tag raises,
//   2  then of the rest where they cover half a pixel or more,
//   3  then of the rest where less,
//   4  then outline rings, drawn only when a tag has one: each paints by the pixel's alpha and
//      zeroes it, so the first shape to reach a pixel spends the whole coverage and overlaps add
//      nothing.
//
// The order is what decides which colour a pixel shared by two shapes takes: a raised one, else
// one that covers it well rather than an edge's fringe, and a ring last of all. A ring is not a
// distance that falls away from its shape, so where it crosses another shape of the layer -- the
// next segment of a picked trace at a joint, a polygon's own inside at a reflex corner -- that
// shape must win, and it has painted and spent the pixel by then.

uniform int uPass;
uniform float uOpacity; // the layer's

const int CANVAS_RANK_RAISED = 1;
const int CANVAS_RANK_SHAPE = 2; // 3 where it covers less than half the pixel
const int CANVAS_RANK_RING = 4;

vec4 canvasOutput(vec4 colour, int rank, float coverage) {
    if (coverage <= 0.0)
        discard;
    if (uPass == 0)
        return vec4(0.0, 0.0, 0.0, coverage * colour.a * uOpacity);
    if (rank == CANVAS_RANK_SHAPE && coverage < 0.5)
        rank = CANVAS_RANK_SHAPE + 1;
    if (rank != uPass)
        discard;
    return vec4(colour.rgb, 0.0);
}
