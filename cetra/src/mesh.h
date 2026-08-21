#ifndef _MESH_H_
#define _MESH_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>

#include "material.h"
#include "util.h"
#include "common.h"

// Forward declaration
struct Skeleton;

// Levels in a mesh's LOD chain, level 0 (the original indices) included.
//
// A budget, not a derived bound: the builder's own floor would allow more rungs
// on a dense enough mesh, and four is where the index memory (which grows with
// the chain) stops being worth the triangles the next rung would save. Raising
// it means extending LOD_SWITCH in draw_list.c, which a static assert enforces.
#define CETRA_LOD_MAX 4

// Axis-Aligned Bounding Box
//
// TWO CONVENTIONS FOR "NOTHING HERE", and the difference is load-bearing.
//
// An ACCUMULATOR starts EMPTY -- min at +FLT_MAX, max at -FLT_MAX -- so the
// first point unions into it correctly and an untouched one is recognisable.
// That is what aabb_empty/aabb_is_empty below are for, and what the per-bone
// boxes and the posed-bounds accumulator use.
//
// A STORED GEOMETRY BOUND does not. `Mesh.aabb` starts at the ZERO box and stays
// there when nothing computed it, because five readers consume it with no
// emptiness guard at all -- the culler copies it straight into a cull bound, ray
// picking slabs against it, two passes upload its Y extent as the wind height
// mask, and scene.c unions its corners into the whole-scene box. Seeded with the
// sentinel instead, aabb_transform computes extents of -inf and that box poisons
// the scene bounds, the GI volume fit, the probe proxy and the camera framing.
// The zero box degrades to a point at the origin, which geometry.c and
// apps/tree/src/ground.c both record as the understood failure. Do not migrate
// it.
typedef struct {
    vec3 min;
    vec3 max;
} AABB;

// The five operations every consumer used to hand-roll. Read a box's emptiness
// through aabb_is_empty rather than testing a component, and build one through
// aabb_empty + aabb_add_point rather than seeding from the first element: the
// sentinel is a convention two files used to share by hand, and doing that once
// already cost a redundant flag beside an uninitialised box.
//
// Named after cglm's glm_aabb_invalidate/isvalid, which is the same convention
// and is compiled in here but used nowhere -- its box.h is not adopted because
// it has neither add_point nor expand, and its transform and frustum test are
// different arithmetic from ours at the frustum edge, where wind_cull_fixture
// deliberately parks meshes.
static inline void aabb_empty(AABB* box) {
    glm_vec3_fill(box->min, FLT_MAX);
    glm_vec3_fill(box->max, -FLT_MAX);
}

static inline bool aabb_is_empty(const AABB* box) {
    return box->min[0] > box->max[0];
}

static inline void aabb_add_point(AABB* box, const vec3 p) {
    glm_vec3_minv(box->min, (float*)p, box->min);
    glm_vec3_maxv(box->max, (float*)p, box->max);
}

// An empty `src` unions to a no-op, which is what lets a caller fold in a box
// that may hold nothing without asking whether it does.
static inline void aabb_union(AABB* box, const AABB* src) {
    glm_vec3_minv(box->min, (float*)src->min, box->min);
    glm_vec3_maxv(box->max, (float*)src->max, box->max);
}

static inline void aabb_expand(AABB* box, float margin) {
    glm_vec3_subs(box->min, margin, box->min);
    glm_vec3_adds(box->max, margin, box->max);
}

// Squared distance from `p` to the box, 0 inside it. Squared because every
// caller compares against a squared threshold and none of them wants the root.
//
// Here rather than at either caller because there were two, written differently
// -- a sign-branch walk in the quadtree's descent and a clamp-and-subtract in
// forest's residency -- and they answer the same question about the same shape.
// Two spellings of one predicate is where a fix lands on one of them.
static inline float aabb_dist_sq(const AABB* box, const vec3 p) {
    float d = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float v = glm_clamp(p[i], box->min[i], box->max[i]) - p[i];
        d += v * v;
    }
    return d;
}

typedef enum {
    MESH_POINTS = GL_POINTS,
    MESH_LINES = GL_LINES,
    MESH_LINE_LOOP = GL_LINE_LOOP,
    MESH_LINE_STRIP = GL_LINE_STRIP,
    MESH_TRIANGLES = GL_TRIANGLES,
    MESH_TRIANGLE_STRIP = GL_TRIANGLE_STRIP,
    MESH_TRIANGLE_FAN = GL_TRIANGLE_FAN,
} MeshDrawMode;

typedef struct Mesh {
    MeshDrawMode draw_mode;

    // if we are drawing lines
    float line_width;

    float* vertices; // Array of vertex positions
    float* normals;  // Array of normals
    // Array of tangents, 4 floats per vertex (glTF/mikktspace convention): xyz
    // is the tangent, w is the bitangent handedness, +1 or -1. There is no
    // bitangent stream -- the shader derives B = cross(N, T) * w, which is how
    // the fragment stage always reconstructed it anyway. Storing a full
    // bitangent only ever contributed that sign, and only mirrored-UV imports
    // set it to anything but +1.
    float* tangents;
    float* tex_coords;  // Array of texture coordinates (UV0)
    float* tex_coords2; // Array of texture coordinates (UV1) for lightmaps/AO
    float* colors;      // Array of vertex colors (RGBA)

    // CDLOD morph targets, 3 floats per vertex each, NULL on almost every mesh
    // (spec 11.63). `morph` is (parent Y, window start, 1/(end - start)) and
    // `morph_normals` the parent surface's normal; terrain_morph.glsl reads both.
    //
    // The window is per PATCH and stored per vertex anyway, which is eight bytes
    // of redundancy against the alternative -- a baked level index, a uniform
    // array of windows, and a dynamic index into it in five programs. What the
    // redundancy buys is that a mesh without these arrays is an exact identity
    // with nothing switched off.
    float* morph;
    float* morph_normals;

    unsigned int* indices; // Array of indices

    size_t vertex_count; // Number of vertices
    size_t index_count;  // Number of indices

    Material* material;

    GLuint vao;         // Vertex Array Object
    GLuint vbo;         // Vertex Buffer Object
    GLuint ebo;         // Element Buffer Object (for indices)
    GLuint nbo;         // Normal Buffer Object
    GLuint tbo;         // Texture Buffer Object (for UV0)
    GLuint tbo2;        // Texture Buffer Object (for UV1)
    GLuint color_vbo;   // Vertex Color Buffer Object
    GLuint tangent_vbo; // Tangent Buffer Object (vec4: xyz tangent, w handedness)
    GLuint morph_vbo;   // CDLOD morph target; generated only when `morph` exists
    GLuint morph_normal_vbo;

    AABB aabb;

    // The vertex maxima windOffset()'s vegetation modes scale their
    // displacement by, over this mesh's own vertices. Both are RAW attribute
    // values -- nothing in the shader, the importer or the upload clamps UV1 to
    // [0,1] -- and both are 0 when there is no UV1, which is what the shader
    // sees too: a disabled attribute reads (0,0,0,1).
    float wind_flex_max; // max |uv1.y|
    float wind_leaf_max; // max |uv1.y * uv0.y|, the joint max rather than the
                         // product of the two, which is tighter and still an
                         // upper bound on the term that uses them together

    // The furthest the CDLOD morph can move a vertex, in object space: the
    // largest |parent Y - Y| this mesh carries. 0 with no morph arrays.
    //
    // A measurement rather than an envelope, unlike the wind bound -- the morph
    // is a lerp between two stored values, so this extreme is really reached. It
    // is exact in Y, which is the only axis the morph moves in; the culler
    // expands all three, so the box it builds is exact there and conservative in
    // X and Z. Read by draw_list.c's _item_bounds, for the same reason wind's is:
    // a mesh whose displacement is unbounded cannot be frustum-culled without
    // dropping geometry that is on screen.
    //
    // And NOTHING CHECKS IT. Wind has --wind-bound-probe, which drives the real
    // shader through transform feedback because a CPU port of the bound reads
    // straight through a term added to the GLSL. This has no equivalent.
    float morph_max_offset;

    // Skinning data (NULL if not skinned)
    int* bone_ids;             // BONES_PER_VERTEX ints per vertex (ivec4)
    float* bone_weights;       // BONES_PER_VERTEX floats per vertex (vec4)
    GLuint bone_id_vbo;        // VBO for bone IDs
    GLuint bone_weight_vbo;    // VBO for bone weights
    struct Skeleton* skeleton; // Shared skeleton pointer (not owned)
    bool is_skinned;

    // Where each bone's own vertices sit in BIND space, so a posed mesh can be
    // bounded and therefore culled. NULL until the upload measures them, and
    // only ever for a skinned mesh.
    //
    // Per bone rather than one box per mesh because skin weights are convex --
    // a posed vertex lies in the convex hull of its bones acting on it alone --
    // so the union of each bone's own box, transformed, bounds the pose. Asking
    // every bone to bound the WHOLE mesh instead needs no measurement and
    // inflates 2-4x, which is loose enough that a character never culls.
    AABB* bone_aabb;
    size_t bone_aabb_count;
    // Vertices whose weights sum to nothing. skinMatrix falls back to identity
    // for them, so they draw at bind position and no bone's box covers them.
    // EMPTY (min > max) when there are none -- the same sentinel the per-bone
    // boxes carry, rather than a second flag saying the same thing.
    AABB bone_rest_aabb;

    // This mesh's emissive surface is also an LTC area panel (spec 11.49), so a
    // capture whose output is irradiance must not see it emit -- the panel
    // already carries that light analytically.
    //
    // Per MESH and not per material, which is the whole reason it lives here: one
    // material can serve both a quad that became a panel and a shape the
    // planarity test rejected, and the second still has to emit.
    //
    // Owned by the emissive reconcile, which CLEARS it when the feature is off.
    // Leaving it set would suppress an emitter for a panel that no longer exists.
    bool emissive_derived;

    // Outstanding shares. One is the ordinary case and behaves exactly as the
    // pointer did when it was owned outright, including for a mesh no node ever
    // takes; more than one is geometry a file says is shared.
    int refs;

    // Creation order, for a sort key that has to be STABLE across runs. The
    // pointer identifies the same geometry and would sort correctly, but its
    // value comes from the allocator -- so a sort keyed on it produces a
    // different draw order run to run, and the 0 px bar every ordering arm rests
    // on could not be met. Assigned once and never reused.
    //
    // It does NOT follow that an id breaks every tie: draw_list.c truncates it
    // to 24 bits to pack the key, so two meshes 16M apart collide there. What
    // makes that sort a total order is its source index, not this.
    unsigned id;

    // LOD chain: simplified INDEX RANGES over the same vertices, concatenated
    // into the one EBO. No extra buffers, no extra VAO, and the offset works
    // just as well on glDrawElementsInstanced -- so a level composes with
    // batching rather than competing with it.
    //
    // Level 0 is always the original index data at offset 0, which is what makes
    // a mesh with no chain byte-identical to one built before chains existed.
    // Read these through mesh_lod_range and mesh_index_total, never directly:
    // they answer for a mesh that has no chain too, which is most of them.
    size_t lod_offset[CETRA_LOD_MAX]; // byte offset into the EBO
    size_t lod_count[CETRA_LOD_MAX];  // indices at this level
    float lod_error[CETRA_LOD_MAX];   // meshopt's deviation estimate, mesh units
    int lod_levels;                   // <= 1 means no chain

    // What built those levels (spec 11.63). Zero for a chain or for no chain at
    // all; non-zero when they are cuts across a cluster DAG, where a "level" is
    // a distance BAND rather than a whole-mesh simplification.
    //
    // Reporting only -- nothing in the draw path branches on it, which is the
    // point: a cut is a concatenated index range like any other, so selection,
    // batching and the sort key never learn that a DAG exists.
    int cluster_count;  // clusters across every DAG level
    int cluster_groups; // DAG groups
    int cluster_levels; // DAG depth, NOT the band count

} Mesh;

// The index range to draw for `level`, clamped to what this mesh actually has.
// A mesh with no chain answers with the whole mesh whatever the level, so a
// caller never has to ask whether a chain exists.
static inline void mesh_lod_range(const Mesh* mesh, int level, GLsizei* count,
                                  const void** offset) {
    if (mesh->lod_levels <= 1) {
        *count = (GLsizei)mesh->index_count;
        *offset = NULL;
        return;
    }
    if (level < 0)
        level = 0;
    if (level >= mesh->lod_levels)
        level = mesh->lod_levels - 1;
    *count = (GLsizei)mesh->lod_count[level];
    *offset = (const void*)mesh->lod_offset[level];
}

// Indices in the EBO: every level end to end, since the chain is appended to
// the original. Derived rather than stored so there is one answer to "how many
// indices does this mesh have" per level and one for the buffer, and no third
// field that a future chain builder could forget to set.
static inline size_t mesh_index_total(const Mesh* mesh) {
    if (mesh->lod_levels <= 1)
        return mesh->index_count;
    int last = mesh->lod_levels - 1;
    return mesh->lod_offset[last] / sizeof(unsigned int) + mesh->lod_count[last];
}

/*
 * Mesh
 */
Mesh* create_mesh();

// Claim a share of an existing mesh, for a second holder that draws the same
// geometry. Returns the mesh, so it can be used in place.
Mesh* mesh_ref(Mesh* mesh);

// Release one share. The mesh and its GL buffers go only when the last holder
// lets go, so freeing one node of a shared mesh leaves the others intact.
void free_mesh(Mesh* mesh);

void set_mesh_draw_mode(Mesh* mesh, MeshDrawMode draw_mode);
void calculate_aabb(Mesh* mesh);

/*
 * Mesh buffers
 */
void upload_mesh_buffers_to_gpu(Mesh* mesh);

#endif // _MESH_H_
