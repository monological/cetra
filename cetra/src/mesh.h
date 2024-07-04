#ifndef _MESH_H_
#define _MESH_H_

#include <GL/glew.h>
#include <cglm/cglm.h>

#include "material.h"
#include "util.h"
#include "common.h"


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

    float* vertices;        // Array of vertex positions
    float* normals;         // Array of normals
    float* tangents;        // Array of tangents
    float* bitangents;      // Array of bitangents
    float* tex_coords;      // Array of texture coordinates
    unsigned int* indices;  // Array of indices

    size_t vertex_count;    // Number of vertices
    size_t index_count;     // Number of indices

    Material* material;

    GLuint vao;             // Vertex Array Object
    GLuint vbo;             // Vertex Buffer Object
    GLuint ebo;             // Element Buffer Object (for indices)
    GLuint nbo;             // Normal Buffer Object
    GLuint tbo;             // Texture Buffer Object (for UVs)
    GLuint tangent_vbo;     // Tangent Buffer Object
    GLuint bitangent_vbo;   // Bitangent Buffer Object

} Mesh;

/*
 * Mesh
 */
Mesh* create_mesh();
void free_mesh(Mesh* mesh);

/*
 * Mesh setters
 */
void set_mesh_draw_mode(Mesh* mesh, MeshDrawMode draw_mode);

/*
 * Mesh buffers
 */
void upload_mesh_buffers_to_gpu(Mesh* mesh);

#endif // _MESH_H_

