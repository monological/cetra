#ifndef _LOD_H_
#define _LOD_H_

#include <stdbool.h>

#include "mesh.h"

// Builds a mesh's LOD chain: progressively simplified index buffers over the
// SAME vertices, concatenated onto the end of mesh->indices so one EBO carries
// every level (see the chain fields on Mesh).
//
// Call after the index and vertex arrays are final and before
// upload_mesh_buffers_to_gpu -- it rewrites mesh->indices, so an upload before
// it would send only level 0 and the offsets would point past the buffer.
//
// Returns the number of levels the mesh ended up with, 1 meaning none was built.
// Refused, and why:
//   - SKINNED meshes. Simplification drops vertices, and the bone weights that
//     went with them do not transfer; the seams show as tearing that only
//     appears once the mesh animates, which is the worst way to find it.
//   - non-triangle draw modes, since meshopt_simplify indexes triangles.
//   - anything under a triangle floor, where a level costs more to select than
//     the triangles it saves.
//   - a level that fails to get meaningfully smaller, which ends the chain.
//     This is what a mesh that is nearly all boundary (a leaf card, a grass
//     blade) does: meshopt weights boundary and seam edges heavily, so
//     collapsing one costs far more error than the target allows and it returns
//     approximately what it was given. Weighted, not locked -- chains are built
//     with options 0, and only meshopt_SimplifyLockBorder makes a border
//     genuinely uncollapsible.
int mesh_build_lod_chain(Mesh* mesh);

#endif // _LOD_H_
