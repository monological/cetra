#include <stdlib.h>
#include <string.h>

#include "lod.h"

#include "ext/log.h"
#include "ext/meshoptimizer/src/meshoptimizer.h"

// Below this a chain is not worth the selection: the draw call dominates, and
// halving a handful of triangles saves nothing measurable.
#define LOD_MIN_TRIANGLES 256

// Each level AIMS for this fraction of the level above it. What the chain
// actually costs is bounded by LOD_MIN_SHRINK, not by this: a level that only
// reaches 0.85 is still kept, so the worst case is 1 + 0.85 + 0.85^2 = 2.57x
// the original index memory rather than the 1.875x halving would suggest.
#define LOD_DECIMATION 0.5f

// A level below this many triangles is not worth a rung of its own. Separate
// from the entry floor above: a mesh has to be reasonably large before a chain
// pays for itself at all, but once it has one the bottom rung can go finer.
#define LOD_MIN_LEVEL_TRIANGLES 64

// How far a level may deviate from the original surface, as a fraction of the
// mesh's own extent. meshopt treats this as a ceiling and returns early when it
// cannot reach the target index count without exceeding it.
#define LOD_TARGET_ERROR 0.05f

// A level has to be meaningfully smaller than the one above or it is not worth
// a chain entry: the selector would switch to it and draw almost the same
// triangles. This is what ends the chain on boundary-dominated geometry.
#define LOD_MIN_SHRINK 0.85f

bool mesh_lod_eligible(const Mesh* mesh, size_t min_triangles) {
    if (!mesh || !mesh->indices || !mesh->vertices)
        return false;
    if (mesh->draw_mode != MESH_TRIANGLES || mesh->index_count % 3 != 0)
        return false;
    // See the header for why skinned meshes are refused rather than approximated.
    if (mesh->is_skinned)
        return false;
    if (mesh->index_count / 3 < min_triangles)
        return false;
    for (size_t i = 0; i < mesh->index_count; ++i) {
        if (mesh->indices[i] >= mesh->vertex_count)
            return false;
    }
    return true;
}

int mesh_build_lod_chain(Mesh* mesh) {
    if (!mesh)
        return 1;

    mesh->lod_levels = 1;
    mesh->lod_offset[0] = 0;
    mesh->lod_count[0] = mesh->index_count;
    mesh->lod_error[0] = 0.0f;

    if (!mesh_lod_eligible(mesh, LOD_MIN_TRIANGLES))
        return 1;

    // Worst case every level is the size of the one above, which the shrink
    // test stops long before -- but the buffer has to exist before the test can
    // run, so it is sized for the bound rather than the expectation.
    size_t capacity = mesh->index_count * CETRA_LOD_MAX;
    unsigned int* chain = malloc(capacity * sizeof(unsigned int));
    if (!chain) {
        log_error("LOD: could not allocate %zu indices", capacity);
        return 1;
    }
    memcpy(chain, mesh->indices, mesh->index_count * sizeof(unsigned int));

    size_t total = mesh->index_count;      // indices written into the chain so far
    size_t previous = mesh->lod_count[0];  // the level this one has to beat
    for (int level = 1; level < CETRA_LOD_MAX; ++level) {
        size_t target = (size_t)((float)previous * LOD_DECIMATION);
        target -= target % 3;
        if (target / 3 < LOD_MIN_LEVEL_TRIANGLES)
            break;

        float error = 0.0f;
        // Simplify from the ORIGINAL indices every time, not from the level
        // above. Simplifying a simplification compounds its error and locks in
        // its choices; meshopt reaches a better result at the same target when
        // it can still see the full surface.
        size_t count = meshopt_simplify(chain + total, mesh->indices, mesh->index_count,
                                        mesh->vertices, mesh->vertex_count, 3 * sizeof(float),
                                        target, LOD_TARGET_ERROR, 0, &error);
        if (count == 0 || count % 3 != 0)
            break;
        // Boundary-dominated geometry lands here: collapsing a boundary edge
        // costs more error than LOD_TARGET_ERROR allows, so it returns something
        // close to what it was given, and a level that saves nothing is worse
        // than no level.
        if ((float)count > (float)previous * LOD_MIN_SHRINK)
            break;

        mesh->lod_offset[level] = total * sizeof(unsigned int);
        mesh->lod_count[level] = count;
        mesh->lod_error[level] = error;
        total += count;
        previous = count;
        mesh->lod_levels = level + 1;
    }

    if (mesh->lod_levels <= 1) {
        free(chain);
        return 1;
    }

    // Hand the concatenated buffer back to the mesh. index_count still describes
    // level 0 alone, so everything that means "the whole mesh" keeps meaning it;
    // index_total is what the EBO upload needs.
    //
    // The original array is freed only here: every level simplified FROM it, so
    // it had to outlive the loop.
    unsigned int* shrunk = realloc(chain, total * sizeof(unsigned int));
    free(mesh->indices);
    mesh->indices = shrunk ? shrunk : chain;
    return mesh->lod_levels;
}
