// Cluster-DAG level of detail (spec 11.63). See cluster.h for what a DAG buys
// over lod.c's chain and why the cut is quantised by distance band.
//
// C++ because meshoptimizer's clusterlod.h implementation is C++ (<algorithm>,
// <vector>) while its interface is extern "C". The same split JoltC already uses:
// one translation unit compiles the library, everything else sees a C header.

#include <float.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "ext/meshoptimizer/src/meshoptimizer.h"

#define CLUSTERLOD_IMPLEMENTATION
#include "ext/meshoptimizer/demo/clusterlod.h"

extern "C" {
#include "cluster.h"
#include "ext/log.h"
}

// Triangles per cluster. 128 is the figure the meshlet literature settled on and
// what clodDefaultConfig is tuned around; it is also small enough that a cut has
// real granularity within one mesh, which is the whole point of a DAG over a chain.
#define CLUSTER_TRIANGLES 128

// Below this a DAG is not worth building: with fewer triangles than a couple of
// clusters there is nothing to group, so every level is the whole mesh and the
// cut degenerates to a chain with extra steps. Matches lod.c's own entry floor.
#define CLUSTER_MIN_TRIANGLES 256

// The error a cut may introduce, as `error / distance` -- an angular measure, so
// it is dimensionless for the reason LOD_SWITCH is: folding in FOV or resolution
// would make a zoom re-cut every mesh in the scene at once.
//
// Derived rather than dialled. clusterlod's own projection formula is
// `error / distance * (proj * 0.5)`, and proj = cot(fovy/2) is about 1.73 at 60
// degrees, so a screen fraction f corresponds to error/distance ~ 1.15 * f. Half
// a pixel of a 1080-line frame is f = 4.6e-4, hence this. Measured against a
// forest terrain tile, whose DAG groups carry errors 0.09 through 1.50 in mesh
// units: it puts band 1 in the middle of that range and bands 2-3 past the end,
// which is the intended shape -- graded where the geometry is still large on
// screen, coarsest where it is not.
#define CLUSTER_ERROR_LIMIT 2.0e-4f

// The band ladder, which is LOD_SWITCH read from the other side: band b covers
// projected sizes down to LOD_SWITCH[b-1], so its WORST case -- the finest cut it
// must serve -- is the size at which it begins. Band 0 begins at infinity, which
// is what makes it the untouched finest cut.
//
// Kept here rather than shared with draw_list.c deliberately: that file selects a
// band from a projected size and this one builds what a band contains, and the two
// meet through the number, not through a symbol. A shared constant would suggest
// they can be changed independently, which is exactly what they cannot be.
static const float CLUSTER_BAND_BEGIN[] = {FLT_MAX, 0.045f, 0.022f, 0.011f};

static_assert(sizeof(CLUSTER_BAND_BEGIN) / sizeof(CLUSTER_BAND_BEGIN[0]) == CETRA_LOD_MAX,
              "one band per LOD level, and band 0 must begin at infinity");

namespace {

struct BuiltCluster {
    std::vector<unsigned int> indices;
    int group;   // the group this cluster is IN
    int refined; // the group whose simplification produced it, -1 for original
};

struct BuildSink {
    std::vector<BuiltCluster> clusters;
    std::vector<clodGroup> groups;

    int take(const clodGroup& group, const clodCluster* items, size_t count) {
        int id = (int)groups.size();
        groups.push_back(group);
        for (size_t i = 0; i < count; ++i) {
            BuiltCluster c;
            // COPIED, not referenced: clodCluster::indices points into clodBuild's
            // own working storage and is only valid for the length of this call.
            c.indices.assign(items[i].indices, items[i].indices + items[i].index_count);
            c.group = id;
            c.refined = items[i].refined;
            clusters.push_back(std::move(c));
        }
        // Saved as clodCluster::refined on every cluster later produced by
        // simplifying THIS group, which is what rule 2 below tests.
        return id;
    }

    // The raw C callback rather than clodBuild's convenience template, which takes
    // its functor BY VALUE -- it would fill a copy and leave this one empty, and
    // report a healthy cluster count while doing it.
    static int thunk(void* ctx, clodGroup group, const clodCluster* items, size_t count) {
        return static_cast<BuildSink*>(ctx)->take(group, items, count);
    }
};

// The mesh's own radius, from the same import AABB draw_list.c measures a
// projected size against -- so the error limit and the band selection are in one
// scale rather than two that happen to agree.
float mesh_radius(Mesh* mesh) {
    vec3 extent;
    glm_vec3_sub(mesh->aabb.max, mesh->aabb.min, extent);
    float r = glm_vec3_norm(extent) * 0.5f;
    return r > 1e-6f ? r : 1.0f;
}

} // namespace

extern "C" bool mesh_build_cluster_lod(Mesh* mesh) {
    if (!mesh || !mesh->indices || !mesh->vertices)
        return false;
    if (mesh->draw_mode != MESH_TRIANGLES || mesh->index_count % 3 != 0)
        return false;
    // Skinned meshes are refused for the reason lod.c refuses them: weights do
    // not transfer to surviving vertices, so a simplified level animates wrong.
    // The DAG inherits that unchanged -- clustering does not make it false.
    if (mesh->is_skinned)
        return false;
    if (mesh->index_count / 3 < CLUSTER_MIN_TRIANGLES)
        return false;
    // Every index must name a real vertex before the clusteriser dereferences
    // it, for the reason lod.c states: an untriangulated face leaves the third
    // slot of its triple at whatever malloc returned, and only the GPU clamps.
    for (size_t i = 0; i < mesh->index_count; ++i) {
        if (mesh->indices[i] >= mesh->vertex_count)
            return false;
    }

    clodConfig config = clodDefaultConfig(CLUSTER_TRIANGLES);
    // OFF, and this is the guarantee rather than a tuning choice: permissive
    // simplification rewrites vertex POSITIONS, and every level indexing one
    // unmoved vertex buffer is what makes a crack between levels impossible.
    config.simplify_permissive = false;
    config.simplify_fallback_permissive = false;

    clodMesh input = {};
    input.indices = mesh->indices;
    input.index_count = mesh->index_count;
    input.vertex_count = mesh->vertex_count;
    input.vertex_positions = mesh->vertices;
    input.vertex_positions_stride = 3 * sizeof(float);

    BuildSink sink;
    size_t produced = clodBuild(config, input, &sink, &BuildSink::thunk);
    if (produced == 0 || sink.clusters.empty() || sink.groups.empty())
        return false;

    // A DAG that never simplified anything is a chain of one level; it would cost
    // the index memory of a cut set and draw the same triangles at every band.
    int depth = 0;
    for (size_t i = 0; i < sink.groups.size(); ++i) {
        if (sink.groups[i].depth > depth)
            depth = sink.groups[i].depth;
    }
    if (depth == 0)
        return false;

    const float radius = mesh_radius(mesh);

    // Build every band's cut up front. A cut depends only on (mesh, band), so
    // there is nothing to recompute per frame or per instance -- which is what
    // lets the bands live in the EBO exactly as a chain's levels do.
    std::vector<unsigned int> packed;
    size_t offset[CETRA_LOD_MAX] = {0};
    size_t count[CETRA_LOD_MAX] = {0};
    float band_error[CETRA_LOD_MAX] = {0.0f};

    for (int band = 0; band < CETRA_LOD_MAX; ++band) {
        // error <= LIMIT * radius / projected, from screen_error ~ error *
        // projected / radius. Band 0 begins at infinity, so its limit is 0 and
        // nothing is acceptable: the finest cut, every original cluster.
        const float begin = CLUSTER_BAND_BEGIN[band];
        const float limit = begin >= FLT_MAX ? 0.0f : CLUSTER_ERROR_LIMIT * radius / begin;

        size_t start = packed.size();
        for (const BuiltCluster& c : sink.clusters) {
            // The rule clusterlod.h documents, and it is what makes the cut a
            // consistent front across the DAG: draw this cluster when its own
            // group's simplification is NOT good enough, and the group it
            // replaced IS -- so exactly one cluster along every path is drawn.
            bool own_too_coarse = sink.groups[c.group].simplified.error > limit;
            bool child_acceptable =
                c.refined < 0 || sink.groups[c.refined].simplified.error <= limit;
            if (own_too_coarse && child_acceptable)
                packed.insert(packed.end(), c.indices.begin(), c.indices.end());
        }
        if (packed.size() == start) {
            // Nothing selected means the ladder ran past the DAG's coarsest
            // level. Stop rather than emit an empty range: mesh_lod_range clamps
            // a too-high level to the last one, so the bands below still serve.
            break;
        }
        offset[band] = start * sizeof(unsigned int);
        count[band] = packed.size() - start;
        band_error[band] = limit;
        mesh->lod_levels = band + 1;
    }

    if (mesh->lod_levels <= 1 || packed.empty())
        return false;

    unsigned int* buffer =
        static_cast<unsigned int*>(malloc(packed.size() * sizeof(unsigned int)));
    if (!buffer) {
        log_error("cluster: could not allocate %zu indices", packed.size());
        mesh->lod_levels = 1;
        return false;
    }
    memcpy(buffer, packed.data(), packed.size() * sizeof(unsigned int));

    for (int band = 0; band < CETRA_LOD_MAX; ++band) {
        mesh->lod_offset[band] = offset[band];
        mesh->lod_count[band] = count[band];
        mesh->lod_error[band] = band_error[band];
    }

    // index_count still describes band 0 alone, so everything meaning "the whole
    // mesh" keeps meaning it and mesh_index_total answers for the buffer. Band 0
    // is every original triangle, REORDERED into cluster order -- the same
    // surface, not the same byte sequence as the unclustered mesh.
    mesh->index_count = mesh->lod_count[0];
    free(mesh->indices);
    mesh->indices = buffer;

    mesh->cluster_count = (int)sink.clusters.size();
    mesh->cluster_groups = (int)sink.groups.size();
    mesh->cluster_levels = depth + 1;
    return true;
}

extern "C" void mesh_cluster_stats(const Mesh* mesh, MeshClusterStats* out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!mesh || mesh->cluster_count <= 0)
        return;
    out->clusters = mesh->cluster_count;
    out->groups = mesh->cluster_groups;
    out->levels = mesh->cluster_levels;
    out->permissive = false;
    unsigned int high = 0;
    size_t total = mesh_index_total(mesh);
    for (size_t i = 0; i < total; ++i) {
        if (mesh->indices[i] > high)
            high = mesh->indices[i];
    }
    out->max_index = (int)high;
}
