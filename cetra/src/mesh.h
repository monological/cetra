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

    // Skinning data (NULL if not skinned)
    int* bone_ids;             // BONES_PER_VERTEX ints per vertex (ivec4)
    float* bone_weights;       // BONES_PER_VERTEX floats per vertex (vec4)
    GLuint bone_id_vbo;        // VBO for bone IDs
    GLuint bone_weight_vbo;    // VBO for bone weights
    struct Skeleton* skeleton; // Shared skeleton pointer (not owned)
    bool is_skinned;

    // Outstanding shares. One is the ordinary case and behaves exactly as the
    // pointer did when it was owned outright, including for a mesh no node ever
    // takes; more than one is geometry a file says is shared.
    int refs;

} Mesh;

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
