#ifndef _LUT_H_
#define _LUT_H_

#include <stdbool.h>

// Adobe .cube colour-grading LUTs (spec 11.58): a table of answers mapping RGB
// to RGB, which is what a colourist exports and what lift/gamma/gain -- a
// three-parameter per-channel curve -- structurally cannot express.
//
// DISPLAY-REFERRED. The table is applied to the frame AFTER tone mapping and
// after the gamma encode, which is the space a .cube is authored in: a
// colourist grades what a monitor is showing and exports the transformation
// from that. Applying one to the LDR-linear values a tonemap curve returns
// produces a plausible image that is not the look the artist made.
//
// Nothing in the format declares that, which is the one failure this reader
// cannot check for -- a log/show LUT loads cleanly and renders washed out. The
// mid-grey heuristic in lut.c turns the common case of that into a named
// warning; it is a suspicion, not a test.

// Bounds on LUT_3D_SIZE. The format allows 2..256; 64 is the ceiling here
// because a 64-cubed table is already 3 MB of transient float and no look LUT
// ships above 65. Refused by name rather than clamped -- a table resampled to a
// size it was not written for is a different table.
#define LUT_MIN_SIZE 2
#define LUT_MAX_SIZE 64

// A loaded table, CPU-side and transient: the consumer uploads it and frees it.
typedef struct ColorLut {
    int size; // LUT_3D_SIZE; the table is size^3 entries

    // size^3 * 3 floats, RED VARYING FASTEST -- the format's own order, kept
    // rather than converted because it is also what glTexImage3D wants when red
    // maps to width and blue to depth. Index is ((ib*size + ig)*size + ir)*3.
    float* data;

    char title[64]; // TITLE if the file carried one, else empty
} ColorLut;

// Parse a .cube into `out`, allocating `out->data`. Returns false and leaves
// `out` untouched on any refusal, each of which log_warns naming the path: a
// 1D LUT, a size outside the bounds above, a domain other than 0..1, a short or
// non-numeric data block, or a non-finite value.
//
// Refuse rather than repair, throughout. Every one of these has a "reasonable"
// coercion available and every coercion produces a table that grades a frame
// into something nobody authored.
bool lut_load_cube(const char* path, ColorLut* out);

void lut_free(ColorLut* lut);

// Trilinear sample, for callers that need a value without a GPU. Used by the
// loader's own mid-grey heuristic; the shading path samples on the GPU and this
// is deliberately not the reference for it.
void lut_sample(const ColorLut* lut, const float in[3], float out[3]);

#endif // _LUT_H_
