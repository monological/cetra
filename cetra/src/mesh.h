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
typedef struct {
    vec3 min;
    vec3 max;
} AABB;

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
    float* tex_coords;     // Array of texture coordinates (UV0)
    float* tex_coords2;    // Array of texture coordinates (UV1) for lightmaps/AO
    float* colors;         // Array of vertex colors (RGBA)
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

    AABB aabb;

    // The vertex maxima windOffset()'s vegetation modes scale their
    // displacement by, so a wind-driven mesh can be given a conservative bound
    // and culled rather than exempted. Both are RAW attribute values: nothing in
    // the shader, the importer or the upload clamps UV1 to [0,1], so assuming
    // it would build a bound the geometry can leave.
    //
    // Measured in upload_mesh_buffers_to_gpu rather than beside the AABB,
    // because that is the one point every drawable mesh passes with its vertex
    // data final -- calculate_aabb is called by each builder in turn and a mesh
    // that gained UV1 afterwards would carry a zero here against a shader
    // reading real flex. Zero when there is no UV1, which is what the shader
    // sees too: a disabled attribute reads (0,0,0,1).
    float wind_flex_max; // max |uv1.y|
    float wind_leaf_max; // max |uv1.y * uv0.y|, the joint max rather than the
                         // product of the two, which is tighter and still an
                         // upper bound on the term that uses them together

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
    AABB bone_rest_aabb;
    bool has_bone_rest;

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
