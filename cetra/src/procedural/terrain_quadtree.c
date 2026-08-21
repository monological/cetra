#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terrain_quadtree.h"
#include "../ext/uthash.h"
#include "../ext/log.h"
#include "../util.h"

/*
 * How far a level reaches, in patch widths, and it is a proof rather than a
 * taste.
 *
 * Descent rule: a node at level L with edge s splits when the camera is closer
 * than SPLIT_FACTOR * s to its box. Two things have to follow from that, and both
 * bound the factor from below.
 *
 * ADJACENT PATCHES MUST NOT DIFFER BY MORE THAN ONE LEVEL, or the morph has
 * nothing to close: a patch becomes its PARENT's surface and no further. If A at
 * level L did not split then d(A) >= f*s. Any point of an adjacent B is within
 * s*sqrt(2) of A, so d(B) >= s*(f - sqrt(2)); B's children have edge s/2 and split
 * only under f*s/2, so they cannot split while f - sqrt(2) >= f/2, i.e. f >= 2*sqrt(2).
 *
 * AND THE MORPH MUST BE COMPLETE BY THE SEAM. Level L is in use over roughly
 * [f*s, 2f*s] -- its own threshold to its parent's -- so the window ends at 2f*s
 * and begins MORPH_FRACTION of the way back. A vertex on a seam with a coarser
 * neighbour needs factor 1 from this side and 0 from that one, which holds while
 * 2f*s + 2*sqrt(2)*s <= 2f*s*(2 - MORPH_FRACTION), i.e. f*(1 - MORPH_FRACTION) >= sqrt(2).
 *
 * At 3.5 and 0.5 both hold with about a quarter to spare. Raising the factor
 * costs patches quadratically -- a level's ring is 3*pi*f^2 of them -- so it is
 * not a free safety margin, and lowering it below 2.83 does not degrade, it
 * cracks: at 2.0 the coarse side of a seam has already morphed 17% of the way to
 * ITS parent while the fine side is still on the surface they were supposed to
 * share.
 */
#define TERRAIN_SPLIT_FACTOR   3.5f
#define TERRAIN_MORPH_FRACTION 0.5f

typedef struct TerrainPatch {
    uint64_t key; // level, ix, iz packed; the hash key
    int level;
    int ix, iz;
    Mesh* mesh;
    SceneNode* node;
    unsigned seen;  // the update tick that last selected this patch
    bool attached;  // currently a child of the root
    UT_hash_handle hh;
} TerrainPatch;

struct TerrainQuadtree {
    const TerrainParams* params; // borrowed
    Material* material;          // borrowed
    int levels;
    int segments;

    // The domain's vertical span, used to make the split test a distance to a
    // BOX rather than to a square on the ground. One range for every node rather
    // than a per-node one: too tall only ever splits earlier, and the balance
    // proof above is stated over XZ, where an inflated Y changes nothing.
    float y_lo, y_hi;

    TerrainPatch* cache; // uthash
    unsigned tick;

    TerrainPatch** selected;
    size_t selected_count, selected_cap;
    vec3 last_eye; // what the current selection was descended against

    size_t built;
};

// Patch edge at a level, in world units. Level 0 is the finest, so the root's
// edge is the whole domain.
static float level_span(const TerrainQuadtree* qt, int level) {
    return (2.0f * qt->params->extent) / (float)(1 << (qt->levels - 1 - level));
}

// The distance window a level morphs over: its own surface at `start`, exactly
// its parent's at `end`, which is the parent's own split threshold.
//
// The COARSEST level has no parent to become, and says so with an EMPTY window
// rather than by a branch at the read: end <= start is the same off state a mesh
// carrying no morph attributes already reads. Written down once because the
// probe reports the same window the builder bakes, and a probe that recomputes it
// can agree with a bug.
static void level_morph_window(const TerrainQuadtree* qt, int level, float* start, float* end) {
    if (level + 1 >= qt->levels) {
        *start = 0.0f;
        *end = 0.0f;
        return;
    }
    float span = level_span(qt, level);
    *end = 2.0f * TERRAIN_SPLIT_FACTOR * span;
    *start = *end - TERRAIN_MORPH_FRACTION * TERRAIN_SPLIT_FACTOR * span;
}

// Updates an unselected patch survives before its mesh is released. The cache
// exists so a camera oscillating across a split boundary does not rebuild the
// same patch every frame, and three seconds at 60 covers that with room to
// spare; past it the patch is behind you and its 30 kB is not worth holding.
//
// Without an eviction the cache is every patch the camera has ever selected,
// which on a walk across a 4 km island is the whole tree at every level. This is
// a WINDOW rather than a capacity cap because the working set is what the camera
// has touched RECENTLY, not how many patches happen to fit -- a cap has to
// choose a victim, and every ordering it could choose is a worse answer than the
// one the tick already records.
#define TERRAIN_PATCH_GRACE 180u

static uint64_t patch_key(int level, int ix, int iz) {
    return ((uint64_t)level << 48) | ((uint64_t)(unsigned)ix << 24) | (uint64_t)(unsigned)iz;
}

// Squared distance from `eye` to the patch's world box, which is its footprint
// over the domain's whole vertical range.
static float box_dist_sq(const TerrainQuadtree* qt, float x0, float z0, float span,
                         const vec3 eye) {
    AABB box = {{x0, qt->y_lo, z0}, {x0 + span, qt->y_hi, z0 + span}};
    return aabb_dist_sq(&box, eye);
}

// The world corner of the patch at (level, ix, iz).
static void patch_origin(const TerrainQuadtree* qt, int level, int ix, int iz, float* x0,
                         float* z0) {
    float span = level_span(qt, level);
    *x0 = terrain_world_x(qt->params, -qt->params->extent + span * (float)ix);
    *z0 = terrain_world_z(qt->params, -qt->params->extent + span * (float)iz);
}

static TerrainPatch* patch_get(TerrainQuadtree* qt, int level, int ix, int iz) {
    uint64_t key = patch_key(level, ix, iz);
    TerrainPatch* patch = NULL;
    HASH_FIND(hh, qt->cache, &key, sizeof(key), patch);
    if (patch)
        return patch;

    float span = level_span(qt, level);
    float x0, z0;
    patch_origin(qt, level, ix, iz, &x0, &z0);

    float start, end;
    level_morph_window(qt, level, &start, &end);

    Mesh* mesh = create_mesh();
    if (!mesh)
        return NULL;
    if (!terrain_build_patch(qt->params, x0, z0, span, qt->segments, start, end, mesh)) {
        free_mesh(mesh);
        return NULL;
    }
    mesh->material = qt->material;
    upload_mesh_buffers_to_gpu(mesh);

    patch = calloc(1, sizeof(TerrainPatch));
    if (!patch) {
        free_mesh(mesh);
        return NULL;
    }
    patch->key = key;
    patch->level = level;
    patch->ix = ix;
    patch->iz = iz;
    patch->mesh = mesh;
    patch->node = create_node();
    if (!patch->node) {
        free_mesh(mesh);
        free(patch);
        return NULL;
    }
    add_mesh_to_node(patch->node, mesh);
    HASH_ADD(hh, qt->cache, key, sizeof(key), patch);
    qt->built++;
    return patch;
}

static void select_patch(TerrainQuadtree* qt, int level, int ix, int iz) {
    TerrainPatch* patch = patch_get(qt, level, ix, iz);
    if (!patch)
        return;
    if (!grow_array((void**)&qt->selected, &qt->selected_cap, qt->selected_count + 1,
                    sizeof(TerrainPatch*), 64)) {
        // Dropping a patch here is a hole in the ground, so it is said out loud
        // rather than returned into silence.
        log_error("terrain quadtree: could not hold %zu selected patches",
                  qt->selected_count + 1);
        return;
    }
    patch->seen = qt->tick;
    qt->selected[qt->selected_count++] = patch;
}

static void descend(TerrainQuadtree* qt, int level, int ix, int iz, const vec3 eye) {
    if (level > 0) {
        float span = level_span(qt, level);
        float x0, z0;
        patch_origin(qt, level, ix, iz, &x0, &z0);
        float threshold = TERRAIN_SPLIT_FACTOR * span;
        if (box_dist_sq(qt, x0, z0, span, eye) < threshold * threshold) {
            descend(qt, level - 1, ix * 2, iz * 2, eye);
            descend(qt, level - 1, ix * 2 + 1, iz * 2, eye);
            descend(qt, level - 1, ix * 2, iz * 2 + 1, eye);
            descend(qt, level - 1, ix * 2 + 1, iz * 2 + 1, eye);
            return;
        }
    }
    select_patch(qt, level, ix, iz);
}

TerrainQuadtree* create_terrain_quadtree(const TerrainParams* params, int levels, int segments,
                                         Material* material) {
    if (!params || levels < 1 || levels > 16 || segments < 2 || (segments & 1)) {
        log_error("terrain quadtree: levels 1-16 and an even segment count >= 2");
        return NULL;
    }
    TerrainQuadtree* qt = calloc(1, sizeof(TerrainQuadtree));
    if (!qt)
        return NULL;
    qt->params = params;
    qt->material = material;
    qt->levels = levels;
    qt->segments = segments;

    // An installed field states its own range; the analytic fbm's amplitude is
    // symmetric about zero. Either way this only has to CONTAIN the terrain --
    // and a box that does NOT is the one direction that costs correctness, since
    // an eye below the box is further from it than from the ground and the patch
    // fails to split. Hence the island floor: it reaches island_depth down,
    // which is unrelated to height and on the island is four times it.
    if (params->field && params->field->max_y > params->field->min_y) {
        qt->y_lo = params->field->min_y;
        qt->y_hi = params->field->max_y;
    } else {
        qt->y_lo = -params->height;
        qt->y_hi = params->height;
    }
    if (params->island_start > 0.0f && params->island_start < 1.0f &&
        -params->island_depth < qt->y_lo)
        qt->y_lo = -params->island_depth;
    return qt;
}

static void patch_free(TerrainPatch* patch) {
    // The node owns the mesh, so freeing it releases both, and free_node unlinks
    // it from the root on the way out -- so a patch can be dropped whether or not
    // the selection currently holds it.
    free_node(patch->node);
    free(patch);
}

// Release every cached patch, leaving the tree empty.
static void drop_all(TerrainQuadtree* qt) {
    TerrainPatch *patch, *tmp;
    HASH_ITER(hh, qt->cache, patch, tmp) {
        HASH_DEL(qt->cache, patch);
        patch_free(patch);
    }
    qt->selected_count = 0;
}

void free_terrain_quadtree(TerrainQuadtree* qt) {
    if (!qt)
        return;
    drop_all(qt);
    free(qt->selected);
    free(qt);
}

void terrain_quadtree_rebuild(TerrainQuadtree* qt, SceneNode* root, const vec3 eye) {
    if (!qt)
        return;
    drop_all(qt);
    qt->built = 0;
    // The shift that brought us here has already translated every root child by
    // -delta, this node among them, which is exactly right for a subtree whose
    // vertices did not move. A patch's did: it is rebuilt in the new storage
    // frame, so a translation on top of it moves it twice. Cleared here rather
    // than at the app, because "patches are in storage space under an untouched
    // node" is this tree's own invariant and the shift is the one thing that
    // breaks it.
    if (root)
        glm_mat4_identity(root->original_transform);
    // Re-selected HERE rather than left to the next descent, and that is the
    // whole reason this is one call. An origin shift lands before the shadow
    // pass and the GI captures, which are the first things in a frame to read
    // world positions -- so a tree left empty until the app's render callback
    // builds its shadow map out of a world with no ground in it.
    terrain_quadtree_update(qt, root, eye);
}

int terrain_quadtree_update(TerrainQuadtree* qt, SceneNode* root, const vec3 eye) {
    if (!qt || !root)
        return 0;

    qt->tick++;
    qt->selected_count = 0;
    glm_vec3_copy((float*)eye, qt->last_eye);
    descend(qt, qt->levels - 1, 0, 0, eye);

    // Detach what the descent did not reach, and evict what has been detached
    // long enough. Walking the CACHE rather than the root's children is what lets
    // a patch carry its own attachment state and saves the node needing a
    // back-reference to it -- and it is why eviction costs nothing extra: this
    // pass already visits every patch and already knows both facts it needs.
    TerrainPatch *patch, *tmp;
    HASH_ITER(hh, qt->cache, patch, tmp) {
        if (patch->attached && patch->seen != qt->tick) {
            remove_child_node(root, patch->node);
            patch->attached = false;
        }
        if (!patch->attached && qt->tick - patch->seen > TERRAIN_PATCH_GRACE) {
            HASH_DEL(qt->cache, patch);
            patch_free(patch);
        }
    }
    for (size_t i = 0; i < qt->selected_count; ++i) {
        if (qt->selected[i]->attached)
            continue;
        add_child_node(root, qt->selected[i]->node);
        qt->selected[i]->attached = true;
    }
    return (int)qt->selected_count;
}

void terrain_quadtree_stats(const TerrainQuadtree* qt, TerrainQuadtreeStats* out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!qt)
        return;
    out->levels = qt->levels;
    out->segments = qt->segments;
    out->selected = (int)qt->selected_count;
    out->resident = (int)HASH_COUNT(qt->cache);
    out->built = qt->built;
    out->split_factor = TERRAIN_SPLIT_FACTOR;
    for (size_t i = 0; i < qt->selected_count; ++i) {
        const TerrainPatch* patch = qt->selected[i];
        out->triangles += patch->mesh->index_count / 3u;
        // Cannot be false -- create_terrain_quadtree caps levels at 16, which is
        // what sizes level_count -- and kept as the bound's second statement, so
        // raising that cap fails here loudly rather than writing past the array.
        if (patch->level >= 0 && patch->level < 16)
            out->level_count[patch->level]++;
    }
}

/*
 * Whether the selection can crack, measured rather than argued.
 *
 * The proof at the top of this file says two things, and neither is visible in a
 * frame: adjacent patches never differ by more than one level, and at a seam the
 * fine side is fully morphed while the coarse side has not started. A crack from
 * either opens for the few frames the camera spends crossing a band, on ground
 * that is by construction far away -- so a golden would have to catch a handful
 * of pixels in a handful of frames at a camera position nothing pins.
 *
 * The gap is the number that subsumes both: how far the fine patch's morphed edge
 * sits from the surface the coarse patch actually draws there, in world units. It
 * is measured against the coarse patch's own VERTICES rather than against the
 * height function, because what has to agree is what gets rasterized.
 */
typedef struct SeamStat {
    int seams;       // fine-against-coarse adjacencies examined
    int unbalanced;  // adjacencies more than one level apart -- must be 0
    float fine_min;  // smallest morph factor on a fine side at a seam; want 1
    float coarse_max; // largest morph factor on a coarse side at a seam; want 0
    float gap;       // largest |fine morphed Y - coarse drawn Y|, world units
} SeamStat;

static const TerrainPatch* patch_selected(const TerrainQuadtree* qt, int level, int ix, int iz) {
    if (level < 0 || level >= qt->levels)
        return NULL;
    int side = 1 << (qt->levels - 1 - level);
    if (ix < 0 || iz < 0 || ix >= side || iz >= side)
        return NULL;
    uint64_t key = patch_key(level, ix, iz);
    TerrainPatch* patch = NULL;
    HASH_FIND(hh, qt->cache, &key, sizeof(key), patch);
    return patch && patch->seen == qt->tick ? patch : NULL;
}

// A SECOND IMPLEMENTATION of cetraMorphFactor, in C, and it can only ever agree
// with the shader by being kept in step by hand. Everything the probe says about
// seams and morph targets is therefore a statement about this arithmetic and not
// about the surface that gets drawn: a morph that never reached a single geometry
// program -- include missing, attributes unbound, uniform never uploaded -- leaves
// every probe row correct.
//
// What closes that is --no-morph and the frame it moves, which is the same answer
// --wind-bound-probe reached by driving the real shader through transform
// feedback. Transform feedback is not available to this one: the morph is a
// property of a SELECTION, and capturing it would mean a TF pass over every
// selected patch every frame rather than over one fixture mesh.
static float vertex_morph_factor(const TerrainQuadtree* qt, const TerrainPatch* patch, int v) {
    const float* pos = &patch->mesh->vertices[(size_t)v * 3];
    const float* m = &patch->mesh->morph[(size_t)v * 3];
    float d = glm_vec3_distance((float*)pos, (float*)qt->last_eye);
    float k = (d - m[1]) * m[2];
    return k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
}

// Where the coarse patch's own triangulation puts the surface at `coord` along
// the shared boundary. Linear over its edge segment, which is what a rasterized
// triangle interpolates between two of its vertices.
//
// Its edge is found by matching the boundary COORDINATE rather than by index
// arithmetic, deliberately: the index correspondence between a patch and its
// parent is the thing under test, so a check that assumed it would agree with a
// bug in it.
static bool coarse_edge_y(const TerrainPatch* coarse, int segments, int axis, float boundary,
                          float coord, float* out) {
    int side = segments + 1;
    const float* verts = coarse->mesh->vertices;
    float best_lo = -FLT_MAX, best_hi = FLT_MAX, y_lo = 0.0f, y_hi = 0.0f;
    bool have_lo = false, have_hi = false;
    for (int v = 0; v < side * side; ++v) {
        const float* p = &verts[(size_t)v * 3];
        if (fabsf(p[axis] - boundary) > 1e-3f)
            continue;
        float c = p[axis == 0 ? 2 : 0];
        if (c <= coord && c >= best_lo) {
            best_lo = c;
            y_lo = p[1];
            have_lo = true;
        }
        if (c >= coord && c <= best_hi) {
            best_hi = c;
            y_hi = p[1];
            have_hi = true;
        }
    }
    if (!have_lo || !have_hi)
        return false;
    float t = best_hi > best_lo ? (coord - best_lo) / (best_hi - best_lo) : 0.0f;
    *out = y_lo + t * (y_hi - y_lo);
    return true;
}

// The index of the t-th vertex along the boundary in direction (dx, dz): the
// far row when the direction is positive, the near one when negative, and t
// itself along the free axis. Written once because both arms of seam_measure walk
// the same edge and a disagreement between them would compare a fine vertex
// against a coarse edge it does not lie on.
static int seam_vertex(int dx, int dz, int segments, int t) {
    int i = dx > 0 ? segments : (dx < 0 ? 0 : t);
    int j = dz > 0 ? segments : (dz < 0 ? 0 : t);
    return j * (segments + 1) + i;
}

static void seam_measure(const TerrainQuadtree* qt, SeamStat* st) {
    memset(st, 0, sizeof(*st));
    st->fine_min = 1.0f;
    st->coarse_max = 0.0f;
    st->gap = 0.0f;

    static const int DX[4] = {1, -1, 0, 0};
    static const int DZ[4] = {0, 0, 1, -1};
    int segments = qt->segments;
    int side = segments + 1;

    for (size_t s = 0; s < qt->selected_count; ++s) {
        const TerrainPatch* fine = qt->selected[s];
        for (int d = 0; d < 4; ++d) {
            int nx = fine->ix + DX[d], nz = fine->iz + DZ[d];
            int level_side = 1 << (qt->levels - 1 - fine->level);
            if (nx < 0 || nz < 0 || nx >= level_side || nz >= level_side)
                continue; // the domain's own edge, not a seam
            if (patch_selected(qt, fine->level, nx, nz))
                continue; // same level; the vertices are literally shared

            const TerrainPatch* coarse = patch_selected(qt, fine->level + 1, nx >> 1, nz >> 1);
            if (!coarse) {
                // Finer neighbour, so THIS patch is the coarse side and must not
                // have started morphing. Any one selected child settles it.
                bool finer = false;
                for (int c = 0; c < 4 && !finer; ++c)
                    finer = patch_selected(qt, fine->level - 1, nx * 2 + (c & 1),
                                           nz * 2 + (c >> 1)) != NULL;
                if (!finer) {
                    st->unbalanced++;
                    continue;
                }
                st->seams++;
                for (int t = 0; t < side; ++t) {
                    float k = vertex_morph_factor(qt, fine,
                                                  seam_vertex(DX[d], DZ[d], segments, t));
                    if (k > st->coarse_max)
                        st->coarse_max = k;
                }
                continue;
            }

            st->seams++;
            // Which axis the shared boundary is constant along; the coordinate
            // itself is taken off each edge vertex, so it is the value the raster
            // used rather than one recomputed from the level's span.
            int axis = DX[d] != 0 ? 0 : 2;
            for (int t = 0; t < side; ++t) {
                int v = seam_vertex(DX[d], DZ[d], segments, t);
                const float* pos = &fine->mesh->vertices[(size_t)v * 3];
                float k = vertex_morph_factor(qt, fine, v);
                if (k < st->fine_min)
                    st->fine_min = k;

                float own = pos[1];
                float parent = fine->mesh->morph[(size_t)v * 3];
                float morphed = own + k * (parent - own);

                float drawn;
                if (coarse_edge_y(coarse, segments, axis, pos[axis], pos[axis == 0 ? 2 : 0],
                                  &drawn)) {
                    float gap = fabsf(morphed - drawn);
                    if (gap > st->gap)
                        st->gap = gap;
                }
            }
        }
    }
}

/*
 * Whether a fully morphed patch really is its parent, measured against the
 * parent's ACTUAL triangles.
 *
 * The seam number above cannot see this. Every boundary vertex of a patch is
 * even-indexed, and an even vertex is its own morph target by construction, so
 * the seam check passes whatever the interior does -- flipping the coarse quad's
 * diagonal in fill_morph_targets moves it by zero. What the interior decides is
 * the POP: a patch that morphs onto a surface its parent does not draw jumps at
 * the moment the parent replaces it, which is exactly the artefact the morph was
 * added to remove.
 *
 * The parent is BUILT to ask, rather than derived from the same index arithmetic
 * the target came from -- a check written from that arithmetic would agree with a
 * mistake in it. The cost is a cache entry per selected patch's parent, which is
 * why this is a probe and not something the update path does.
 */
static float interior_gap(TerrainQuadtree* qt) {
    float worst = 0.0f;
    int segments = qt->segments;
    int side = segments + 1;
    size_t count = qt->selected_count;

    for (size_t s = 0; s < count; ++s) {
        const TerrainPatch* fine = qt->selected[s];
        if (fine->level + 1 >= qt->levels)
            continue;
        const TerrainPatch* parent = patch_get(qt, fine->level + 1, fine->ix >> 1, fine->iz >> 1);
        if (!parent)
            continue;
        float pspan = level_span(qt, fine->level + 1);
        float px0, pz0;
        patch_origin(qt, fine->level + 1, fine->ix >> 1, fine->iz >> 1, &px0, &pz0);
        float h = pspan / (float)segments;

        for (int v = 0; v < side * side; ++v) {
            const float* pos = &fine->mesh->vertices[(size_t)v * 3];
            float fi = (pos[0] - px0) / h;
            float fj = (pos[2] - pz0) / h;
            int ci = (int)fi, cj = (int)fj;
            if (ci < 0)
                ci = 0;
            if (cj < 0)
                cj = 0;
            if (ci > segments - 1)
                ci = segments - 1;
            if (cj > segments - 1)
                cj = segments - 1;
            float u = fi - (float)ci, w = fj - (float)cj;

            // build_grid splits a quad (a, c, b) and (b, c, d), so the shared
            // edge runs from (ci+1, cj) to (ci, cj+1) -- the line u + w = 1.
            const float* verts = parent->mesh->vertices;
            float ya = verts[((size_t)cj * side + ci) * 3 + 1];
            float yb = verts[((size_t)cj * side + ci + 1) * 3 + 1];
            float yc = verts[((size_t)(cj + 1) * side + ci) * 3 + 1];
            float yd = verts[((size_t)(cj + 1) * side + ci + 1) * 3 + 1];
            float drawn = u + w <= 1.0f ? ya + u * (yb - ya) + w * (yc - ya)
                                        : yd + (1.0f - u) * (yc - yd) + (1.0f - w) * (yb - yd);

            float gap = fabsf(fine->mesh->morph[(size_t)v * 3] - drawn);
            if (gap > worst)
                worst = gap;
        }
    }
    return worst;
}

void terrain_quadtree_probe(TerrainQuadtree* qt) {
    if (!qt)
        return;
    TerrainQuadtreeStats st;
    terrain_quadtree_stats(qt, &st);
    // Taken before interior_gap, which builds parents the selection did not ask
    // for and would otherwise be reported as residency the renderer paid for.
    printf("terrain-quadtree-probe levels=%d segments=%d split_factor=%.4f selected=%d "
           "resident=%d built=%zu triangles=%zu\n",
           st.levels, st.segments, (double)st.split_factor, st.selected, st.resident, st.built,
           st.triangles);
    for (int level = 0; level < qt->levels; ++level) {
        float span = level_span(qt, level);
        float start, end;
        level_morph_window(qt, level, &start, &end);
        float cell = span / (float)qt->segments;
        // How much detail this level's patches actually gave up, as the largest
        // gap between a vertex and the FULL surface at the same XZ. The source
        // level alone says only which level was asked for; this says whether
        // asking changed anything, and it reads 0 on a terrain with no pyramid
        // and no octaves to drop -- which is the honest answer there.
        float dropped = 0.0f;
        const TerrainPatch *patch, *tmp;
        HASH_ITER(hh, qt->cache, patch, tmp) {
            if (patch->seen != qt->tick || patch->level != level)
                continue;
            const Mesh* mesh = patch->mesh;
            for (size_t v = 0; v < mesh->vertex_count; ++v) {
                const float* pos = &mesh->vertices[v * 3];
                float d = fabsf(pos[1] - terrain_height_at(qt->params, pos[0], pos[2]));
                if (d > dropped)
                    dropped = d;
            }
        }
        printf("terrain-quadtree-probe level=%d span=%.4f cell=%.4f split_at=%.4f "
               "morph_start=%.4f morph_end=%.4f selected=%d source_level=%d source_cell=%.4f "
               "dropped=%.6f\n",
               level, (double)span, (double)cell, (double)(TERRAIN_SPLIT_FACTOR * span),
               (double)start, (double)end, st.level_count[level],
               terrain_level_for_cell(qt->params, cell),
               (double)terrain_level_cell(qt->params, terrain_level_for_cell(qt->params, cell)),
               (double)dropped);
    }
    SeamStat seam;
    seam_measure(qt, &seam);
    printf("terrain-quadtree-probe seams=%d unbalanced=%d fine_min=%.6f coarse_max=%.6f "
           "gap=%.9f interior_gap=%.9f\n",
           seam.seams, seam.unbalanced, (double)seam.fine_min, (double)seam.coarse_max,
           (double)seam.gap, (double)interior_gap(qt));

    // The morph offset a patch actually carries, which is the only number here
    // that comes from the terrain rather than from the schedule.
    const TerrainPatch *patch, *tmp;
    HASH_ITER(hh, qt->cache, patch, tmp) {
        if (patch->seen != qt->tick)
            continue;
        printf("terrain-quadtree-probe patch level=%d ix=%d iz=%d vertices=%zu triangles=%zu "
               "morph_max=%.6f\n",
               patch->level, patch->ix, patch->iz, patch->mesh->vertex_count,
               patch->mesh->index_count / 3u, (double)patch->mesh->morph_max_offset);
    }
}
