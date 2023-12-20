
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <cglm/cglm.h>
#include <GL/glew.h>
#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "common.h"
#include "material.h"
#include "mesh.h"

Mesh* create_mesh() {
    Mesh* mesh = malloc(sizeof(Mesh));
    if (!mesh) {
        fprintf(stderr, "Failed to allocate memory for Mesh\n");
        return NULL;
    }

    mesh->draw_mode = GL_TRIANGLES;

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

    glGenBuffers(1, &mesh->TangentVBO);
    glGenBuffers(1, &mesh->BitangentVBO);

    // Unbind the VAO
    glBindVertexArray(0);

    return mesh;
}

void free_mesh(Mesh* mesh) {
    if (!mesh) return;

    // Free OpenGL buffers
    glDeleteBuffers(1, &mesh->VBO);
    glDeleteBuffers(1, &mesh->NBO);
    glDeleteBuffers(1, &mesh->TBO);
    glDeleteBuffers(1, &mesh->EBO);
    glDeleteVertexArrays(1, &mesh->VAO);
    glDeleteBuffers(1, &mesh->TangentVBO);
    glDeleteBuffers(1, &mesh->BitangentVBO);

    // Free the allocated memory
    if (mesh->vertices) free(mesh->vertices);
    if (mesh->normals) free(mesh->normals);
    if (mesh->tangents) free(mesh->tangents);
    if (mesh->bitangents) free(mesh->bitangents);
    if (mesh->texCoords) free(mesh->texCoords);
    if (mesh->indices) free(mesh->indices);

    // same material can be shared by multiple meshes
    //if (mesh->material) {
    //    free_material(mesh->material);
    //}

    free(mesh);
}

void set_mesh_draw_mode(Mesh* mesh, MeshDrawMode draw_mode) {
    if (!mesh) return;
    mesh->draw_mode = draw_mode;
}

void upload_mesh_buffers_to_gpu(Mesh* mesh) {
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

    // Tangents
    if (mesh->tangents) {
        glGenBuffers(1, &(mesh->TangentVBO));
        glBindBuffer(GL_ARRAY_BUFFER, mesh->TangentVBO);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertexCount * 3 * sizeof(float), mesh->tangents, GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_TANGENT, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_TANGENT);
    }

    // Bitangents
    if (mesh->bitangents) {
        glGenBuffers(1, &(mesh->BitangentVBO));
        glBindBuffer(GL_ARRAY_BUFFER, mesh->BitangentVBO);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertexCount * 3 * sizeof(float), mesh->bitangents, GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_BITANGENT, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_BITANGENT);
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


