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
 * every level indexes the ORIGINAL vertex buffer -- meshoptimizer's collapse model
 * only ever drops vertices, it never introduces one -- so two clusters that share
 * an edge share the literal same vertices whatever levels they came from.
 *
 * That property does NOT come from `simplify_permissive`, which an earlier version
 * of this comment claimed. Permissive is "allow collapses across attribute
 * discontinuities" -- a UV/normal seam permission. It is off because a prop with UV
 * seams would have them collapsed, which is a quality decision, not the seal.
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

// What the build produced. Only meaningful when mesh_build_cluster_lod returned
// true, which is the same condition under which there is a DAG to describe.
typedef struct MeshClusterStats {
    int clusters;  // total across every level
    int groups;    // DAG groups
    int levels;    // DAG depth (max group depth + 1), NOT the band count
    int max_index; // largest vertex index any cluster references
    // Indices in a COARSE band that band 0 never used. This is the seal as a
    // number, and it is the only one of these a broken build can move: an index
    // merely being in range says nothing, since the builder refuses an
    // out-of-range input up front and meshoptimizer promises the output
    // references the input buffer. A simplifier that REMAPPED to a compacted
    // buffer would keep every index in range and land here instead.
    int foreign_indices;
} MeshClusterStats;

// Build the cluster DAG for `mesh` and install its band cuts as the mesh's LOD
// levels, replacing any chain lod.c built. Returns false and leaves the mesh
// untouched when the DAG cannot be built (too few triangles, allocation failure),
// so a caller may attempt this on anything and fall back to the chain.
//
// Call BEFORE upload_mesh_buffers_to_gpu, like mesh_build_lod_chain: it rewrites
// mesh->indices.
//
// `out` is optional and is filled ONLY on success -- what the build produced,
// for the probes and gate arms. It is an out-param rather than three fields on
// Mesh because nothing in the engine reads these to draw with: they described
// the build, on a struct shared by every mesh in every app, at 12 bytes each.
bool mesh_build_cluster_lod(Mesh* mesh, MeshClusterStats* out);

// MESHOPTIMIZER_VERSION, evaluated where that header is visible -- the cook's
// version axis for anything the simplifier produced (spec 11.99): its collapse
// decisions move between releases, so a vendored upgrade must orphan cooked
// DAGs rather than serve them against a builder that would now disagree.
unsigned cluster_builder_version(void);

#ifdef __cplusplus
}
#endif

#endif // _CLUSTER_H_
