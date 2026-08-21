#ifndef _CLUSTER_H_
#define _CLUSTER_H_

#include <stdbool.h>
#include <stddef.h>

#include "mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cluster-DAG level of detail (spec 11.63).
 *
 * The alternative to lod.c's chain, and the difference is the DAG rather than the
 * clustering. A chain simplifies the WHOLE mesh N times, so a level is all-or-
 * nothing and two neighbouring meshes at different levels can crack. A DAG splits
 * the mesh into clusters, groups adjacent clusters, simplifies each group with its
 * own boundary LOCKED, then re-splits -- and re-groups differently at the next
 * level, so the locked seams move instead of accumulating. What you draw is a CUT
 * across that DAG: fine clusters where the geometry needs them, coarse ones where
 * it does not, in one mesh.
 *
 * CRACKS ARE IMPOSSIBLE HERE, and not because a seam is stitched. Every cluster at
 * every level indexes the ORIGINAL vertex buffer -- simplification only drops
 * vertices, it never moves them -- so two clusters that share an edge share the
 * literal same vertices whatever levels they came from. That is why
 * `simplify_permissive` must stay off: it is the one setting that would
 * destructively rewrite positions and turn this guarantee into an approximation.
 *
 * The cut is quantised by DISTANCE BAND, which is what keeps instancing alive.
 * A per-instance cut would mean two trees at different distances could never share
 * a draw call, un-shipping spec 11.29's batching. Selection depends only on
 * projected size, so every instance in one band shares one cut -- and since a cut
 * is then a pure function of (mesh, band), all of them are built ONCE at import and
 * concatenated into the same EBO the LOD chain already uses. The draw path does not
 * branch: `mesh_lod_range` and the (mesh, lod) batch key work unchanged, and a
 * "level" is a band.
 */

// Build the cluster DAG for `mesh` and install its band cuts as the mesh's LOD
// levels, replacing any chain lod.c built. Returns false and leaves the mesh
// untouched when the DAG cannot be built (too few triangles, allocation failure),
// so a caller may attempt this on anything and fall back to the chain.
//
// Call BEFORE upload_mesh_buffers_to_gpu, like mesh_build_lod_chain: it rewrites
// mesh->indices.
bool mesh_build_cluster_lod(Mesh* mesh);

// What the build produced, for the probes and gate arms. Zeroed for a mesh that
// has no DAG, so a caller needs no guard.
typedef struct MeshClusterStats {
    int clusters;    // total across every level
    int groups;      // DAG groups
    int levels;      // DAG depth (max group depth + 1), NOT the band count
    int max_index;   // largest vertex index any cluster references
    bool permissive; // whether destructive simplification ran -- must be false
} MeshClusterStats;

void mesh_cluster_stats(const Mesh* mesh, MeshClusterStats* out);

#ifdef __cplusplus
}
#endif

#endif // _CLUSTER_H_
