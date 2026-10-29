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

#define CANVAS_FLAG_FLAT_A 1u     // end a is flat
#define CANVAS_FLAG_FLAT_B 2u     // end b is flat
#define CANVAS_FLAG_CUT_A 4u      // end a is where a longer shape was split: no cap, no ramp
#define CANVAS_FLAG_CUT_B 8u      // end b likewise
#define CANVAS_FLAG_CLOCKWISE 16u // an arc runs clockwise from a to b
