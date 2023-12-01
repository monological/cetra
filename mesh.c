
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <cglm/cglm.h>
#include <GL/glew.h>
#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "common.h"
#include "mesh.h"

Mesh* create_mesh() {
    Mesh* mesh = malloc(sizeof(Mesh));
    if (!mesh) {
        fprintf(stderr, "Failed to allocate memory for Mesh\n");
        return NULL;
    }

    mesh->vertices = NULL;
    mesh->normals = NULL;
    mesh->texCoords = NULL;
    mesh->indices = NULL;
    mesh->vertexCount = 0;
    mesh->indexCount = 0;

    // Generate and bind the Vertex Array Object (VAO)
    glGenVertexArrays(1, &mesh->VAO);
    glBindVertexArray(mesh->VAO);

    // Generate Vertex Buffer Object (VBO)
    glGenBuffers(1, &mesh->VBO);
    // Generate Normal Buffer Object (NBO)
    glGenBuffers(1, &mesh->NBO);
    // Generate Texture Buffer Object (TBO)
    glGenBuffers(1, &mesh->TBO);
    // Generate Element Buffer Object (EBO)
    glGenBuffers(1, &mesh->EBO);

    // Unbind the VAO
    glBindVertexArray(0);

    return mesh;
}

void setup_mesh_buffers(Mesh* mesh) {
    if (!mesh) return;

    // Bind the Vertex Array Object (VAO)
    glBindVertexArray(mesh->VAO);

    // Vertex positions
    glBindBuffer(GL_ARRAY_BUFFER, mesh->VBO);
    glBufferData(GL_ARRAY_BUFFER, mesh->vertexCount * 3 * sizeof(float), mesh->vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(GL_ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(GL_ATTR_POSITION);

    // Normals
    if (mesh->normals) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh->NBO);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertexCount * 3 * sizeof(float), mesh->normals, GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_NORMAL, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_NORMAL);
    }

    // Texture coordinates
    if (mesh->texCoords) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh->TBO);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertexCount * 2 * sizeof(float), mesh->texCoords, GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_TEXCOORD);
    }

    // Indices
    if (mesh->indices) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh->indexCount * sizeof(unsigned int), mesh->indices, GL_STATIC_DRAW);
    }

    // Unbind VAO
    glBindVertexArray(0);
}

void free_mesh(Mesh* mesh) {
    if (!mesh) return;

    // Free OpenGL buffers
    glDeleteBuffers(1, &mesh->VBO);
    glDeleteBuffers(1, &mesh->NBO);
    glDeleteBuffers(1, &mesh->TBO);
    glDeleteBuffers(1, &mesh->EBO);
    glDeleteVertexArrays(1, &mesh->VAO);

    // Free the allocated memory
    if (mesh->vertices) free(mesh->vertices);
    if (mesh->normals) free(mesh->normals);
    if (mesh->texCoords) free(mesh->texCoords);
    if (mesh->indices) free(mesh->indices);

    free_material(mesh->material);

    free(mesh);
}


Material* create_material() {
    Material* material = (Material*)malloc(sizeof(Material));
    if (!material) {
        fprintf(stderr, "Failed to allocate memory for material\n");
        return NULL;
    }

    // Initialize with default values
    glm_vec3_fill(material->albedo, 1.0f);
    material->metallic = 0.0f;
    material->roughness = 1.0f;
    material->ao = 1.0f;

    // Initialize texture IDs to 0 (no texture)
    material->albedoTexID = 0;
    material->normalTexID = 0;
    material->roughnessTexID = 0;
    material->metalnessTexID = 0;
    material->ambientOcclusionTexID = 0;
    material->emissiveTexID = 0;
    material->heightTexID = 0;

    // Additional Advanced PBR Textures
    material->opacityTexID = 0;
    material->microsurfaceTexID = 0;
    material->anisotropyTexID = 0;
    material->subsurfaceScatteringTexID = 0;
    material->sheenTexID = 0;
    material->reflectanceTexID = 0;

    return material;
}

void free_material(Material* material) {
    if (material) {
        // Free the material structure itself
        free(material);
    }
}
