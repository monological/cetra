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
    POINTS = GL_POINTS,
    LINES = GL_LINES,
    LINE_LOOP = GL_LINE_LOOP,
    LINE_STRIP = GL_LINE_STRIP,
    TRIANGLES = GL_TRIANGLES,
    TRIANGLE_STRIP = GL_TRIANGLE_STRIP,
    TRIANGLE_FAN = GL_TRIANGLE_FAN,
} MeshDrawMode;

typedef struct Mesh {
    MeshDrawMode draw_mode;

    // if we are drawing lines
    float line_width;

    float* vertices;       // Array of vertex positions
    float* normals;        // Array of normals
    float* tangents;       // Array of tangents
    float* bitangents;     // Array of bitangents
    float* tex_coords;     // Array of texture coordinates (UV0)
    float* tex_coords2;    // Array of texture coordinates (UV1) for lightmaps/AO
    float* colors;         // Array of vertex colors (RGBA)
    unsigned int* indices; // Array of indices

    size_t vertex_count; // Number of vertices
    size_t index_count;  // Number of indices

    Material* material;

    GLuint vao;           // Vertex Array Object
    GLuint vbo;           // Vertex Buffer Object
    GLuint ebo;           // Element Buffer Object (for indices)
    GLuint nbo;           // Normal Buffer Object
    GLuint tbo;           // Texture Buffer Object (for UV0)
    GLuint tbo2;          // Texture Buffer Object (for UV1)
    GLuint color_vbo;     // Vertex Color Buffer Object
    GLuint tangent_vbo;   // Tangent Buffer Object
    GLuint bitangent_vbo; // Bitangent Buffer Object

    AABB aabb;

    // Skinning data (NULL if not skinned)
    int* bone_ids;             // BONES_PER_VERTEX ints per vertex (ivec4)
    float* bone_weights;       // BONES_PER_VERTEX floats per vertex (vec4)
    GLuint bone_id_vbo;        // VBO for bone IDs
    GLuint bone_weight_vbo;    // VBO for bone weights
    struct Skeleton* skeleton; // Shared skeleton pointer (not owned)
    bool is_skinned;

} Mesh;

/*
 * Mesh
 */
Mesh* create_mesh();
void free_mesh(Mesh* mesh);

void set_mesh_draw_mode(Mesh* mesh, MeshDrawMode draw_mode);
void calculate_aabb(Mesh* mesh);

/*
 * Mesh buffers
 */
void upload_mesh_buffers_to_gpu(Mesh* mesh);

#endif // _MESH_H_
