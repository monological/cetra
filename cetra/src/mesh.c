
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cglm/cglm.h>
#include <GL/glew.h>
#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "animation.h"
#include "common.h"
#include "ext/log.h"
#include "material.h"
#include "mesh.h"

#include "draw_list.h"
#include "util.h"

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
    mesh->tangents = NULL;
    mesh->tex_coords = NULL;
    mesh->tex_coords2 = NULL;
    mesh->colors = NULL;
    mesh->indices = NULL;

    mesh->vertex_count = 0;
    mesh->index_count = 0;

    // No chain until one is built; mesh_lod_range reads the whole mesh at every
    // level while this holds.
    memset(mesh->lod_offset, 0, sizeof(mesh->lod_offset));
    memset(mesh->lod_count, 0, sizeof(mesh->lod_count));
    memset(mesh->lod_error, 0, sizeof(mesh->lod_error));
    mesh->lod_levels = 1;
    mesh->index_total = 0;

    // One share, held by whoever asked for the mesh. That makes the single-node
    // case identical to the ownership this had before refcounting, including
    // leaking a mesh nobody ever attached to a node.
    mesh->refs = 1;

    // Generate and bind the Vertex Array Object (vao)
    glGenVertexArrays(1, &mesh->vao);
    glBindVertexArray(mesh->vao);

    // Generate Vertex Buffer Object (vbo)
    glGenBuffers(1, &mesh->vbo);
    // Generate Normal Buffer Object (nbo)
    glGenBuffers(1, &mesh->nbo);
    // Generate Texture Buffer Object (tbo)
    glGenBuffers(1, &mesh->tbo);
    // Generate Texture Buffer Object 2 (tbo2) for UV1
    glGenBuffers(1, &mesh->tbo2);
    // Generate Color Buffer Object
    glGenBuffers(1, &mesh->color_vbo);
    // Generate Element Buffer Object (ebo)
    glGenBuffers(1, &mesh->ebo);

    glGenBuffers(1, &mesh->tangent_vbo);

    // Unbind the vao
    glBindVertexArray(0);

    mesh->aabb.min[0] = 0.0f;
    mesh->aabb.min[1] = 0.0f;
    mesh->aabb.min[2] = 0.0f;
    mesh->aabb.max[0] = 0.0f;
    mesh->aabb.max[1] = 0.0f;
    mesh->aabb.max[2] = 0.0f;

    // Initialize skinning data
    mesh->bone_ids = NULL;
    mesh->bone_weights = NULL;
    mesh->bone_id_vbo = 0;
    mesh->bone_weight_vbo = 0;
    mesh->skeleton = NULL;
    mesh->is_skinned = false;

    return mesh;
}

Mesh* mesh_ref(Mesh* mesh) {
    if (mesh)
        mesh->refs++;
    return mesh;
}

void free_mesh(Mesh* mesh) {
    if (!mesh)
        return;
    if (--mesh->refs > 0)
        return; // another node still draws this geometry
    scene_graph_touched();

    // Free OpenGL buffers
    glDeleteBuffers(1, &mesh->vbo);
    glDeleteBuffers(1, &mesh->nbo);
    glDeleteBuffers(1, &mesh->tbo);
    glDeleteBuffers(1, &mesh->tbo2);
    glDeleteBuffers(1, &mesh->color_vbo);
    glDeleteBuffers(1, &mesh->ebo);
    glDeleteVertexArrays(1, &mesh->vao);
    glDeleteBuffers(1, &mesh->tangent_vbo);

    // Free the allocated memory
    if (mesh->vertices)
        free(mesh->vertices);
    if (mesh->normals)
        free(mesh->normals);
    if (mesh->tangents)
        free(mesh->tangents);
    if (mesh->tex_coords)
        free(mesh->tex_coords);
    if (mesh->tex_coords2)
        free(mesh->tex_coords2);
    if (mesh->colors)
        free(mesh->colors);
    if (mesh->indices)
        free(mesh->indices);

    // Free skinning data
    if (mesh->bone_ids)
        free(mesh->bone_ids);
    if (mesh->bone_weights)
        free(mesh->bone_weights);
    if (mesh->bone_id_vbo)
        glDeleteBuffers(1, &mesh->bone_id_vbo);
    if (mesh->bone_weight_vbo)
        glDeleteBuffers(1, &mesh->bone_weight_vbo);
    // Do not free skeleton - it's shared and managed by Scene

    // Do not free material. Same material can be shared by multiple meshes.
    // Managed by scene.

    free(mesh);
}

void set_mesh_draw_mode(Mesh* mesh, MeshDrawMode draw_mode) {
    if (!mesh)
        return;
    mesh->draw_mode = draw_mode;
}

void calculate_aabb(Mesh* mesh) {
    AABB* aabb = &mesh->aabb;

    if (mesh->vertex_count == 0) {
        glm_vec3_zero(aabb->min);
        glm_vec3_zero(aabb->max);
        return;
    }

    // Initialize min and max to the first vertex
    glm_vec3_copy((vec3){mesh->vertices[0], mesh->vertices[1], mesh->vertices[2]}, aabb->min);
    glm_vec3_copy((vec3){mesh->vertices[0], mesh->vertices[1], mesh->vertices[2]}, aabb->max);

    // Iterate over each vertex to update the min and max vectors
    for (size_t i = 1; i < mesh->vertex_count; ++i) {
        vec3 vertex = {mesh->vertices[i * 3 + 0], mesh->vertices[i * 3 + 1],
                       mesh->vertices[i * 3 + 2]};

        glm_vec3_minv(aabb->min, vertex, aabb->min);
        glm_vec3_maxv(aabb->max, vertex, aabb->max);
    }
}

void upload_mesh_buffers_to_gpu(Mesh* mesh) {
    if (!mesh)
        return;
    // A mesh with no VAO is not drawable and the list refuses it, so the upload
    // that makes it drawable has to invalidate.
    scene_graph_touched();

    // Bind the Vertex Array Object (vao)
    glBindVertexArray(mesh->vao);

    // Vertex positions
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * 3 * sizeof(float), mesh->vertices,
                 GL_STATIC_DRAW);
    glVertexAttribPointer(GL_ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(GL_ATTR_POSITION);

    // Indices. index_total covers every LOD level end to end and equals
    // index_count when no chain was built, so this is the whole EBO either way.
    if (mesh->indices) {
        size_t total = mesh->index_total ? mesh->index_total : mesh->index_count;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, total * sizeof(unsigned int), mesh->indices,
                     GL_STATIC_DRAW);
    }

    // Normals
    if (mesh->normals) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh->nbo);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * 3 * sizeof(float), mesh->normals,
                     GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_NORMAL, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_NORMAL);
    }

    // Tangents (vec4: xyz tangent, w bitangent handedness)
    if (mesh->tangents) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh->tangent_vbo);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * 4 * sizeof(float), mesh->tangents,
                     GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_TANGENT, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_TANGENT);
    }

    // Texture coordinates (UV0)
    if (mesh->tex_coords) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh->tbo);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * 2 * sizeof(float), mesh->tex_coords,
                     GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_TEXCOORD, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_TEXCOORD);
    }

    // Texture coordinates (UV1) for lightmaps/AO
    if (mesh->tex_coords2) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh->tbo2);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * 2 * sizeof(float), mesh->tex_coords2,
                     GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_TEXCOORD2, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                              (void*)0);
        glEnableVertexAttribArray(GL_ATTR_TEXCOORD2);
    }

    // Vertex colors (RGBA)
    if (mesh->colors) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh->color_vbo);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * 4 * sizeof(float), mesh->colors,
                     GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_COLOR, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_COLOR);
    }

    // Bone IDs (ivec4 per vertex)
    if (mesh->is_skinned && mesh->bone_ids) {
        if (mesh->bone_id_vbo == 0)
            glGenBuffers(1, &mesh->bone_id_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh->bone_id_vbo);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * BONES_PER_VERTEX * sizeof(int),
                     mesh->bone_ids, GL_STATIC_DRAW);
        glVertexAttribIPointer(GL_ATTR_BONE_IDS, BONES_PER_VERTEX, GL_INT,
                               BONES_PER_VERTEX * sizeof(int), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_BONE_IDS);
    }

    // Bone Weights (vec4 per vertex)
    if (mesh->is_skinned && mesh->bone_weights) {
        if (mesh->bone_weight_vbo == 0)
            glGenBuffers(1, &mesh->bone_weight_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, mesh->bone_weight_vbo);
        glBufferData(GL_ARRAY_BUFFER, mesh->vertex_count * BONES_PER_VERTEX * sizeof(float),
                     mesh->bone_weights, GL_STATIC_DRAW);
        glVertexAttribPointer(GL_ATTR_BONE_WEIGHTS, BONES_PER_VERTEX, GL_FLOAT, GL_FALSE,
                              BONES_PER_VERTEX * sizeof(float), (void*)0);
        glEnableVertexAttribArray(GL_ATTR_BONE_WEIGHTS);
    }

    check_gl_error("mesh buffer upload");

    // Unbind vao
    glBindVertexArray(0);
}
