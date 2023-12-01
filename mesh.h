#ifndef _MESH_H_
#define _MESH_H_

#include <cglm/cglm.h>
#include <GL/glew.h>
#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>


typedef struct Material {
    vec3 albedo;
    float metallic;
    float roughness;
    float ao;

    // Core PBR Texture IDs
    GLuint albedoTexID;           // Albedo (Diffuse) Map
    GLuint normalTexID;           // Normal Map
    GLuint roughnessTexID;        // Roughness Map
    GLuint metalnessTexID;        // Metalness Map
    GLuint ambientOcclusionTexID; // Ambient Occlusion Map
    GLuint emissiveTexID;         // Emissive Map
    GLuint heightTexID;           // Height Map (Displacement Map)

    // Additional Advanced PBR Textures
    GLuint opacityTexID;             // Opacity Map
    GLuint microsurfaceTexID;        // Microsurface (Detail) Map
    GLuint anisotropyTexID;          // Anisotropy Map
    GLuint subsurfaceScatteringTexID;// Subsurface Scattering Map
    GLuint sheenTexID;               // Sheen Map (for fabrics)
    GLuint reflectanceTexID;         // Reflectance Map
} Material;

Material* create_material();
void free_material(Material* material);

typedef struct Mesh {
    float* vertices;       // Array of vertex positions
    float* normals;        // Array of normals
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
} Mesh;

Mesh* create_mesh();
void setup_mesh_buffers(Mesh* mesh);
void free_mesh(Mesh* mesh);

#endif // _MESH_H_

