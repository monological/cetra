#ifndef _MESH_H_
#define _MESH_H_

#include <GL/glew.h>

#include "material.h"

typedef struct Mesh {
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
void upload_mesh_buffers_to_gpu(Mesh* mesh);
void free_mesh(Mesh* mesh);

#endif // _MESH_H_

