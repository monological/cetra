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
// [min_y, max_y] and recording it on the field. Accepts `.r16` and 16-bit PNG,
// chosen by extension. Refuses, with a log line naming the path and the reason,
// on: a non-square byte count, a resolution under 2, a PNG that is not square, an
// 8-bit PNG, or an unreadable file.
//
// SIBLING MASKS ARE PICKED UP AUTOMATICALLY when they sit beside the height as
// `<stem>_flow.r8`, `_deposit.r8` and `_wear.r8`. Missing ones stay zero, which
// is the truth about a Gaea or World Machine export -- it carries geometry and no
// erosion history. What it must NOT be is the truth about this module's own save,
// which is why heightmap_save writes them.
bool heightmap_load(TerrainField* field, const char* path, float min_y, float max_y);

// Write the field as `.r16` plus its three `_flow` / `_deposit` / `_wear` 8-bit
// siblings, mapping [min_y, max_y] onto the full 16-bit range and clamping
// outside it. Bytes are little-endian explicitly, so a file written on either
// endianness reads the same on both.
//
// The masks are written because without them the round trip is not the one this
// module exists to provide. A shipping load would get the eroded GEOMETRY and
// then shade it with the slope-and-altitude guess erosion was built to replace --
// which is the exact failure the feature opens by describing, arrived at through
// its own save path. Eight bits because a mask is a blend weight read through a
// smoothstep, where 1/255 is far below what any threshold resolves.
bool heightmap_save(const TerrainField* field, const char* path, float min_y, float max_y);

#endif // _HEIGHTMAP_H_
