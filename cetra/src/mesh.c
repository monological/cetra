
#include <float.h>
#include <math.h>
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

// Never reset and never reused, so an id identifies one mesh for the life of the
// process. Wrapping after 4 billion creations would only cost a sort tie, and
// nothing here allocates at a rate that reaches it.
static unsigned g_next_mesh_id = 1;

Mesh* create_mesh() {
    // Zeroed, not malloc'd, for the reason create_engine states: every field
    // below is still set explicitly, but a struct this wide cannot rely on each
    // new one being remembered here. Spec 11.63 added five fields and remembered
    // two, which left a garbage cull margin on every mesh in the engine
    // (draw_list.c reads morph_max_offset unconditionally) and a glDeleteBuffers
    // on an arbitrary GL name in free_mesh.
    Mesh* mesh = calloc(1, sizeof(Mesh));
    if (!mesh) {
        log_error("Failed to allocate memory for Mesh");
        return NULL;
    }

    mesh->id = g_next_mesh_id++;

    mesh->draw_mode = GL_TRIANGLES;

    mesh->line_width = 1.0f;

    mesh->vertices = NULL;
    mesh->normals = NULL;
    mesh->tangents = NULL;
    mesh->tex_coords = NULL;
    mesh->tex_coords2 = NULL;
    mesh->colors = NULL;
    mesh->morph = NULL;
    mesh->morph_normals = NULL;
    mesh->indices = NULL;

    mesh->vertex_count = 0;
    mesh->index_count = 0;

    // No chain until one is built; mesh_lod_range reads the whole mesh at every
    // level while this holds.
    memset(mesh->lod_offset, 0, sizeof(mesh->lod_offset));
    memset(mesh->lod_count, 0, sizeof(mesh->lod_count));
    memset(mesh->lod_error, 0, sizeof(mesh->lod_error));
    mesh->lod_levels = 1;
    mesh->cluster_count = 0;
    mesh->cluster_groups = 0;
    mesh->cluster_levels = 0;

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

    mesh->wind_flex_max = 0.0f;
    mesh->wind_leaf_max = 0.0f;

    // Initialize skinning data
    mesh->bone_ids = NULL;
    mesh->bone_weights = NULL;
    mesh->bone_id_vbo = 0;
    mesh->bone_weight_vbo = 0;
    mesh->skeleton = NULL;
    mesh->is_skinned = false;
    mesh->bone_aabb = NULL;
    mesh->bone_aabb_count = 0;
    // Seeded EMPTY, not left to the allocator: this box is read whenever it is
    // not empty, so garbage here is a bound the geometry can leave.
    aabb_empty(&mesh->bone_rest_aabb);

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
    if (mesh->morph_vbo)
        glDeleteBuffers(1, &mesh->morph_vbo);
    if (mesh->morph_normal_vbo)
        glDeleteBuffers(1, &mesh->morph_normal_vbo);

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
    if (mesh->morph)
        free(mesh->morph);
    if (mesh->morph_normals)
        free(mesh->morph_normals);
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
    if (mesh->bone_aabb)
        free(mesh->bone_aabb);
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

        aabb_add_point(aabb, vertex);
    }
}

// The vertex maxima the wind bound needs (see mesh.h). Here rather than beside
// calculate_aabb because this runs once per mesh with every attribute final,
// which is what makes the answer a description of what the shader will read.
static void measure_wind_extremes(Mesh* mesh) {
    mesh->wind_flex_max = 0.0f;
    mesh->wind_leaf_max = 0.0f;
    if (!mesh->tex_coords2)
        return;

    for (size_t i = 0; i < mesh->vertex_count; ++i) {
        float flex = fabsf(mesh->tex_coords2[i * 2 + 1]);
        if (flex > mesh->wind_flex_max)
            mesh->wind_flex_max = flex;
        // The leaf term is flex * uv0.y in one product, so the max OF the
        // product bounds it more tightly than the product of the maxima.
        float leaf = mesh->tex_coords ? flex * fabsf(mesh->tex_coords[i * 2 + 1]) : 0.0f;
        if (leaf > mesh->wind_leaf_max)
            mesh->wind_leaf_max = leaf;
    }
}

// Each bone's own vertices, in bind space (see mesh.h). Runs beside the wind
// maxima and for the same reason: this is where the vertex data is final.
static void measure_bone_bounds(Mesh* mesh) {
    free(mesh->bone_aabb);
    mesh->bone_aabb = NULL;
    mesh->bone_aabb_count = 0;
    aabb_empty(&mesh->bone_rest_aabb);

    if (!mesh->is_skinned || !mesh->bone_ids || !mesh->bone_weights || !mesh->skeleton)
        return;
    size_t bones = mesh->skeleton->bone_count;
    if (bones == 0 || bones > MAX_BONES)
        return;

    mesh->bone_aabb = malloc(bones * sizeof(AABB));
    if (!mesh->bone_aabb)
        return;
    mesh->bone_aabb_count = bones;

    // An empty box per bone, marked by min > max, so a bone this mesh does not
    // bind is skipped at cull rather than contributing a box around the origin.
    for (size_t b = 0; b < bones; ++b)
        aabb_empty(&mesh->bone_aabb[b]);

    for (size_t i = 0; i < mesh->vertex_count; ++i) {
        vec3 v = {mesh->vertices[i * 3 + 0], mesh->vertices[i * 3 + 1], mesh->vertices[i * 3 + 2]};
        float total = 0.0f;
        for (int j = 0; j < BONES_PER_VERTEX; ++j) {
            int id = mesh->bone_ids[i * BONES_PER_VERTEX + j];
            float w = mesh->bone_weights[i * BONES_PER_VERTEX + j];
            if (id < 0 || (size_t)id >= bones || w <= 0.0f)
                continue;
            total += w;
            aabb_add_point(&mesh->bone_aabb[id], v);
        }
        // The shader's own threshold, mirrored: below it skinMatrix hands back
        // identity and this vertex draws where it sits.
        if (total < 0.001f) {
            aabb_add_point(&mesh->bone_rest_aabb, v);
        }
    }
}

/*
 * One optional per-vertex stream, or nothing when the mesh does not carry it --
 * leaving the attribute unbound, which is the off state every consumer of an
 * optional attribute is written against (terrain_morph.glsl says why for its
 * two).
 *
 * `vbo` is generated on first use rather than in create_mesh. That was the morph
 * streams' rule for a reason that generalises: almost no mesh has every stream,
 * and a GL name per stream per mesh across a scene is not free.
 *
 * TWO functions and not one with a type flag, because integer attributes take
 * glVertexAttribIPointer and a float that happens to hold an index is a
 * different thing from an index. The alternative -- one function branching on a
 * GLenum -- puts the branch at every call site's expense to save four lines.
 *
 * Everything above these was six near-identical lines per stream: bind, buffer,
 * pointer, enable, differing only in the component count. That is the shape
 * spec 11.63's uninitialised-field bug grew in, so the duplication is the defect
 * and not merely the symptom.
 */
static void _upload_float_stream(const Mesh* mesh, const float* data, GLuint* vbo, GLuint attr,
                                 GLint components) {
    if (!data)
        return;
    if (*vbo == 0)
        glGenBuffers(1, vbo);
    const size_t stride = (size_t)components * sizeof(float);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(mesh->vertex_count * stride), data,
                 GL_STATIC_DRAW);
    glVertexAttribPointer(attr, components, GL_FLOAT, GL_FALSE, (GLsizei)stride, (void*)0);
    glEnableVertexAttribArray(attr);
}

static void _upload_int_stream(const Mesh* mesh, const int* data, GLuint* vbo, GLuint attr,
                               GLint components) {
    if (!data)
        return;
    if (*vbo == 0)
        glGenBuffers(1, vbo);
    const size_t stride = (size_t)components * sizeof(int);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(mesh->vertex_count * stride), data,
                 GL_STATIC_DRAW);
    glVertexAttribIPointer(attr, components, GL_INT, (GLsizei)stride, (void*)0);
    glEnableVertexAttribArray(attr);
}

void upload_mesh_buffers_to_gpu(Mesh* mesh) {
    if (!mesh)
        return;
    // A mesh with no VAO is not drawable and the list refuses it, so the upload
    // that makes it drawable has to invalidate.
    scene_graph_touched();

    measure_wind_extremes(mesh);
    measure_bone_bounds(mesh);

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
        size_t total = mesh_index_total(mesh);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, total * sizeof(unsigned int), mesh->indices,
                     GL_STATIC_DRAW);
    }

    _upload_float_stream(mesh, mesh->normals, &mesh->nbo, GL_ATTR_NORMAL, 3);
    // Tangents are vec4: xyz tangent, w bitangent handedness.
    _upload_float_stream(mesh, mesh->tangents, &mesh->tangent_vbo, GL_ATTR_TANGENT, 4);
    _upload_float_stream(mesh, mesh->tex_coords, &mesh->tbo, GL_ATTR_TEXCOORD, 2);
    // UV1 carries lightmap/AO coordinates, and on a wind material the branch
    // phase and flex weight instead (spec 11.51).
    _upload_float_stream(mesh, mesh->tex_coords2, &mesh->tbo2, GL_ATTR_TEXCOORD2, 2);
    _upload_float_stream(mesh, mesh->colors, &mesh->color_vbo, GL_ATTR_COLOR, 4);

    _upload_float_stream(mesh, mesh->morph, &mesh->morph_vbo, GL_ATTR_MORPH, 3);
    _upload_float_stream(mesh, mesh->morph_normals, &mesh->morph_normal_vbo,
                         GL_ATTR_MORPH_NORMAL, 3);

    // The skinning pair is gated on is_skinned as well as on the array, because a
    // mesh can carry weights it does not use and binding them would put a
    // skinned program's attributes on an unskinned draw.
    if (mesh->is_skinned) {
        _upload_int_stream(mesh, mesh->bone_ids, &mesh->bone_id_vbo, GL_ATTR_BONE_IDS,
                           BONES_PER_VERTEX);
        _upload_float_stream(mesh, mesh->bone_weights, &mesh->bone_weight_vbo,
                             GL_ATTR_BONE_WEIGHTS, BONES_PER_VERTEX);
    }

    check_gl_error("mesh buffer upload");

    // Unbind vao
    glBindVertexArray(0);
}
