/*
 * The 2D canvas's record vocabulary (spec 14.0): the kinds of shape a record holds and the flags
 * on its ends.
 *
 * INCLUDED BY BOTH LANGUAGES, shore_constants.glsl's technique: canvas_edit.c writes these into
 * the records canvas_vert.glsl and canvas_frag.glsl read, and a kind or a flag numbered apart
 * draws one shape as another with nothing to say so. Numbers only; the `u` suffix makes each an
 * unsigned value in both, which is what a record's fields are.
 */

#define CANVAS_KIND_NONE 0u // a dead slot: draws nothing
#define CANVAS_KIND_SEGMENT 1u
#define CANVAS_KIND_ARC 2u
#define CANVAS_KIND_CIRCLE 3u
#define CANVAS_KIND_BOX 4u
#define CANVAS_KIND_EDGE 5u // the outer half of a filled polygon's edge ramp
#define CANVAS_KIND_FILL 6u // a triangle of a filled polygon, in the layer's fill buffer
#define CANVAS_KIND_GLYPH 7u // one character of a string

#define CANVAS_FLAG_FLAT_A 1u  // end a is flat
#define CANVAS_FLAG_FLAT_B 2u  // end b is flat
#define CANVAS_FLAG_CUT_A 4u   // end a is where a longer shape was split: no cap, no ramp
#define CANVAS_FLAG_CUT_B 8u   // end b likewise
#define CANVAS_FLAG_MIRROR 16u // a glyph of mirrored text, flipped across its anchor's line
#define CANVAS_FLAG_DASHED 32u // the record carries a dash pattern after it
#define CANVAS_FLAG_DASH_BACK 64u // the pattern runs from b: a clockwise arc, its ends swapped
#define CANVAS_FLAG_SQUARE_A 128u // end a is square: flat half the width past it
#define CANVAS_FLAG_SQUARE_B 256u // end b likewise

// A segment's joins, in its fourth measure (canvas_record.h): the join at a, at b, and which ends
// are joined; a box's join, in its fourth point; a dashed record's cap, in its pattern's fourth.
// The codes are canvas.h's CanvasCap and CanvasJoin.
#define CANVAS_JOIN_AT_A 3u   // the join at a
#define CANVAS_JOIN_AT_B 12u  // at b, shifted two
#define CANVAS_JOINED_A 16u   // a is a corner, the segment before it in the second measure
#define CANVAS_JOINED_B 32u   // b likewise, the segment after it in the third
#define CANVAS_DASHED_A 64u   // corner a lies inside a dash, which runs on round it
#define CANVAS_DASHED_B 128u  // corner b likewise
#define CANVAS_STROKE_ROUND 0u
#define CANVAS_STROKE_BUTT 1u   // a cap
#define CANVAS_STROKE_SQUARE 2u // a cap
#define CANVAS_STROKE_MITRE 1u  // a join
#define CANVAS_STROKE_BEVEL 2u  // a join

// The glyph table (canvas_vert.glsl's uGlyphs): CANVAS_GLYPH_CODEPOINTS entries a font, two
// texels each, for each of the CANVAS_FONT_MAX fonts a canvas draws with. Codepoint 0 draws
// nothing, so its first texel holds the font's own numbers. Counts, so plain ints in both
// languages.
#define CANVAS_FONT_MAX 4
#define CANVAS_GLYPH_CODEPOINTS 128
