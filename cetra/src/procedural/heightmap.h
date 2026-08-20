#ifndef _HEIGHTMAP_H_
#define _HEIGHTMAP_H_

#include <stdbool.h>

#include "terrain.h"

// Heightmap import and export, so a field can come from somewhere other than this
// process and go somewhere other than this frame.
//
// The bake and the importer meet HERE, and that is the point of the module: a
// dev-time run erodes and saves, a shipping run loads with no sim, and a Gaea or
// World Machine export drops into the same slot. Without that the erosion bake is
// a demo and there is no answer to "an artist made this terrain".
//
// `.r16` is headerless little-endian 16-bit unsigned, square, which is what UE,
// World Machine and Gaea all exchange. Headerless means the resolution comes from
// the file SIZE, so a truncated file is refused by name rather than loading as a
// smaller terrain -- and it means the world height range has to arrive separately,
// from whoever knows what the numbers mean.
//
// Deliberately not routed through texture.c. Its texture_gl_formats picks UNSIZED
// internal formats from a channel count, so no path through it can ask for 16-bit
// or float at all; and this is CPU-side height data that the GL never sees.

// Load into a freshly allocated field, mapping the file's full range onto
// [min_y, max_y]. Accepts `.r16` and 16-bit PNG, chosen by extension. Refuses,
// with a log line naming the path and the reason, on: a non-square byte count, a
// resolution under 2, a PNG that is not square, or an unreadable file. The masks
// are allocated and left zero -- an import carries no erosion history, which is
// the truth about it.
bool heightmap_load(TerrainField* field, const char* path, float min_y, float max_y);

// Write field->height as `.r16`, mapping [min_y, max_y] onto the full 16-bit
// range and clamping outside it. Bytes are written little-endian explicitly, so a
// file written on either endianness reads the same on both.
bool heightmap_save(const TerrainField* field, const char* path, float min_y, float max_y);

// The tightest [min_y, max_y] that loses nothing to clamping. Separate from the
// save so a CALLER can choose a range shared across several fields -- two
// terrains saved against their own peaks are not comparable, which is a mistake
// worth having to make on purpose.
void heightmap_height_range(const TerrainField* field, float* min_y, float* max_y);

#endif // _HEIGHTMAP_H_
