// How both of the 2D canvas's fragment stages write a layer (spec 14.0). A layer resolves in the
// view's target in passes over the same geometry, the target's alpha its scratch:
//
//   0  coverage, the LARGEST any shape gives a pixel, times the layer's opacity, into alpha
//      alone. The largest and not the union, because a shape cut at a tile edge, and a polygon's
//      outline fringe at a corner, cover one pixel twice with the same coverage: max is
//      idempotent there, and over shapes of one layer it is the coverage of the nearest edge.
//   1  the colour of shapes a tag raises,
//   2  then of the rest where they cover half a pixel or more,
//   3  then of the rest where less: each paints by the pixel's alpha and zeroes it, so the first
//      shape to reach a pixel spends the whole coverage and overlaps add nothing.
//
// The order of 1 to 3 is what decides which colour a pixel shared by two shapes takes: a raised
// one, else one that covers it well rather than an edge's fringe.

uniform int uPass;
uniform float uOpacity; // the layer's

vec4 canvasOutput(vec4 colour, bool raised, float coverage) {
    if (coverage <= 0.0)
        discard;
    if (uPass == 0)
        return vec4(0.0, 0.0, 0.0, coverage * colour.a * uOpacity);
    bool solid = coverage >= 0.5;
    if ((uPass == 1) != raised || (uPass == 2 && !solid) || (uPass == 3 && solid))
        discard;
    return vec4(colour.rgb, 0.0);
}
