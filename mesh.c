
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

    if (mesh->material) {
        free_material(mesh->material);
    }

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

    // In create_material function
    material->albedoTex = NULL;
    material->normalTex = NULL;
    material->roughnessTex = NULL;
    material->metalnessTex = NULL;
    material->ambientOcclusionTex = NULL;
    material->emissiveTex = NULL;
    material->heightTex = NULL;

    // Additional Advanced PBR Textures
    material->opacityTex = NULL;
    material->microsurfaceTex = NULL;
    material->anisotropyTex = NULL;
    material->subsurfaceScatteringTex = NULL;
    material->sheenTex = NULL;
    material->reflectanceTex = NULL;

    return material;
}

void free_material(Material* material) {
    if (material) {
        // Free each Texture struct
        free_texture(material->albedoTex);
        free_texture(material->normalTex);
        free_texture(material->roughnessTex);
        free_texture(material->metalnessTex);
        free_texture(material->ambientOcclusionTex);
        free_texture(material->emissiveTex);
        free_texture(material->heightTex);
        free_texture(material->opacityTex);
        free_texture(material->microsurfaceTex);
        free_texture(material->anisotropyTex);
        free_texture(material->subsurfaceScatteringTex);
        free_texture(material->sheenTex);
        free_texture(material->reflectanceTex);

        // Finally, free the material structure itself
        free(material);
    }
}
