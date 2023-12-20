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

    float* vertices;       // Array of vertex positions
    float* normals;        // Array of normals
    float* tangents;   // Array of tangents
    float* bitangents; // Array of bitangents
    float* texCoords;      // Array of texture coordinates
    unsigned int* indices; // Array of indices

    size_t vertexCount;    // Number of vertices
    size_t indexCount;     // Number of indices

    Material* material;

    // OpenGL buffer objects
    GLuint VAO; // Vertex Array Object
    GLuint VBO; // Vertex Buffer Object
    GLuint EBO; // Element Buffer Object (for indices)
    GLuint NBO; // Normal Buffer Object
    GLuint TBO; // Texture Buffer Object (for UVs)
    GLuint TangentVBO;     // Tangent Buffer Object
    GLuint BitangentVBO;   // Bitangent Buffer Object

} Mesh;

Mesh* create_mesh();
void free_mesh(Mesh* mesh);

void set_mesh_draw_mode(Mesh* mesh, MeshDrawMode draw_mode);

void upload_mesh_buffers_to_gpu(Mesh* mesh);

#endif // _MESH_H_

