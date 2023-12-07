#ifndef _MESH_H_
#define _MESH_H_

#include <cglm/cglm.h>
#include <GL/glew.h>
#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "texture.h"


typedef struct Material {
    vec3 albedo;
    float metallic;
    float roughness;
    float ao;

    // Core PBR Textures
    Texture* albedoTex;           // Albedo (Diffuse) Map
    Texture* normalTex;           // Normal Map
    Texture* roughnessTex;        // Roughness Map
    Texture* metalnessTex;        // Metalness Map
    Texture* ambientOcclusionTex; // Ambient Occlusion Map
    Texture* emissiveTex;         // Emissive Map
    Texture* heightTex;           // Height Map (Displacement Map)

    // Additional Advanced PBR Textures
    Texture* opacityTex;             // Opacity Map
    Texture* microsurfaceTex;        // Microsurface (Detail) Map
    Texture* anisotropyTex;          // Anisotropy Map
    Texture* subsurfaceScatteringTex;// Subsurface Scattering Map
    Texture* sheenTex;               // Sheen Map (for fabrics)
    Texture* reflectanceTex;         // Reflectance Map

} Material;

Material* create_material();
void free_material(Material* material);

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

