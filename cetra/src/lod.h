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

// Whether `mesh` can have ANY level-of-detail built over it, chain or cluster
// DAG, at a builder's own triangle floor.
//
// Shared because the two builders refuse the same six things for the same
// reasons and one of them is not a style choice: every index must name a real
// vertex before a simplifier dereferences it. The import fills indices per FACE
// and aiProcess_Triangulate leaves line and point primitives alone, so a 2-index
// face leaves the third slot of its triple at whatever malloc returned. That
// garbage used to reach only the GPU, which clamps; a simplifier reads
// vertex_positions + idx * stride and does not.
//
// `min_triangles` is the caller's, because the floors differ in kind: a chain
// level costs more to select than it saves below its floor, while a cluster DAG
// below its floor has nothing to GROUP and degenerates to a chain with extra
// steps.
//
// NOTE neither builder can cheaply refuse an already-uploaded mesh, which would
// be worth doing -- building after the upload rewrites mesh->indices without
// touching the EBO, so every level past 0 points past what the GPU holds and
// draws wrong in silence. create_mesh generates the VAO and EBO names up front,
// so neither handle distinguishes "created" from "uploaded"; catching it needs a
// flag on Mesh that nothing else wants yet.
bool mesh_lod_eligible(const Mesh* mesh, size_t min_triangles);

#endif // _LOD_H_
