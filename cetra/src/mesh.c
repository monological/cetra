
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <cglm/cglm.h>
#include <GL/glew.h>
#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "common.h"
#include "ext/log.h"
#include "material.h"
#include "mesh.h"

Mesh* create_mesh() {
    Mesh* mesh = malloc(sizeof(Mesh));
    if (!mesh) {
        log_error("Failed to allocate memory for Mesh");
        return NULL;
    }

    mesh->draw_mode = GL_TRIANGLES;

    mesh->line_width = 1.0f;

    mesh->vertices = NULL;
    mesh->normals = NULL;
    mesh->tex_coords = NULL;
    mesh->indices = NULL;
    mesh->vertex_count = 0;
    mesh->index_count = 0;

    // Generate and bind the Vertex Array Object (vao)
    glGenVertexArrays(1, &mesh->vao);
    glBindVertexArray(mesh->vao);

    // Generate Vertex Buffer Object (vbo)
    glGenBuffers(1, &mesh->vbo);
    // Generate Normal Buffer Object (nbo)
    glGenBuffers(1, &mesh->nbo);
    // Generate Texture Buffer Object (tbo)
    glGenBuffers(1, &mesh->tbo);
    // Generate Element Buffer Object (ebo)
    glGenBuffers(1, &mesh->ebo);

    glGenBuffers(1, &mesh->tangent_vbo);
    glGenBuffers(1, &mesh->bitangent_vbo);

    // Unbind the vao
    glBindVertexArray(0);

    return mesh;
}

void free_mesh(Mesh* mesh) {
    if (!mesh) return;

    // Free OpenGL buffers
    glDeleteBuffers(1, &mesh->vbo);
    glDeleteBuffers(1, &mesh->nbo);
    glDeleteBuffers(1, &mesh->tbo);
    glDeleteBuffers(1, &mesh->ebo);
    glDeleteVertexArrays(1, &mesh->vao);
    glDeleteBuffers(1, &mesh->tangent_vbo);
    glDeleteBuffers(1, &mesh->bitangent_vbo);

    // Free the allocated memory
    if (mesh->vertices) free(mesh->vertices);
    if (mesh->normals) free(mesh->normals);
    if (mesh->tangents) free(mesh->tangents);
    if (mesh->bitangents) free(mesh->bitangents);
    if (mesh->tex_coords) free(mesh->tex_coords);
    if (mesh->indices) free(mesh->indices);

    // Do not free material. Same material can be shared by multiple meshes.
    // Managed by scene.

    free(mesh);
}

void set_mesh_draw_mode(Mesh* mesh, MeshDrawMode draw_mode) {
    if (!mesh) return;
    mesh->draw_mode = draw_mode;
}

void upload_mesh_buffers_to_gpu(Mesh* mesh) {
    if (!mesh) return;

    // Bind the Vertex Array Object (vao)
    glBindVertexArray(mesh->vao);

    // Vertex positions
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * 3 * sizeof(float), mesh->vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(GL_ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(GL_ATTR_POSITION);

    // Indices
    if (mesh->indices) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh->index_count * sizeof(unsigned int), mesh->indices, GL_STATIC_DRAW);
    }

    // Normals
    if (mesh->normals) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh->nbo);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * 3 * sizeof(float), mesh->normals, GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_NORMAL, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_NORMAL);
    }

    // Tangents
    if (mesh->tangents) {
        glGenBuffers(1, &(mesh->tangent_vbo));
        glBindBuffer(GL_ARRAY_BUFFER, mesh->tangent_vbo);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * 3 * sizeof(float), mesh->tangents, GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_TANGENT, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_TANGENT);
    }

    // Bitangents
    if (mesh->bitangents) {
        glGenBuffers(1, &(mesh->bitangent_vbo));
        glBindBuffer(GL_ARRAY_BUFFER, mesh->bitangent_vbo);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * 3 * sizeof(float), mesh->bitangents, GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_BITANGENT, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_BITANGENT);
    }
    

    // Texture coordinates
    if (mesh->tex_coords) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh->tbo);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * 2 * sizeof(float), mesh->tex_coords, GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_TEXCOORD);
    }

    // Unbind vao
    glBindVertexArray(0);
}


