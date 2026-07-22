#include <stdlib.h>
#include <string.h>

#include "mesh_builder.h"
#include "cetra/util.h"

bool mb_init(MeshBuilder* mb, size_t vres, size_t ires, bool want_colors) {
    memset(mb, 0, sizeof(*mb));
    mb->vcap = vres > 64 ? vres : 64;
    mb->icap = ires > 64 ? ires : 64;
    mb->want_colors = want_colors;
    mb->pos = malloc(mb->vcap * 3 * sizeof(float));
    mb->nrm = malloc(mb->vcap * 3 * sizeof(float));
    mb->tan = malloc(mb->vcap * 4 * sizeof(float));
    mb->uv0 = malloc(mb->vcap * 2 * sizeof(float));
    mb->uv1 = malloc(mb->vcap * 2 * sizeof(float));
    mb->idx = malloc(mb->icap * sizeof(unsigned int));
    mb->ok = mb->pos && mb->nrm && mb->tan && mb->uv0 && mb->uv1 && mb->idx;
    if (want_colors) {
        mb->col = malloc(mb->vcap * 4 * sizeof(float));
        mb->ok = mb->ok && mb->col != NULL;
    }
    return mb->ok;
}

void mb_free(MeshBuilder* mb) {
    free(mb->pos);
    free(mb->nrm);
    free(mb->tan);
    free(mb->uv0);
    free(mb->uv1);
    free(mb->col);
    free(mb->idx);
    memset(mb, 0, sizeof(*mb));
}

// A NULL stream means "not in use", so the colour channel needs no special case.
static bool mb_grow_one(float** arr, size_t cap, int comps) {
    if (!*arr)
        return true;
    float* p = safe_realloc(*arr, cap * (size_t)comps * sizeof(float));
    if (!p)
        return false;
    *arr = p;
    return true;
}

static bool mb_grow_verts(MeshBuilder* mb) {
    size_t cap = mb->vcap * 2;
    if (!(mb_grow_one(&mb->pos, cap, 3) && mb_grow_one(&mb->nrm, cap, 3) &&
          mb_grow_one(&mb->tan, cap, 4) &&
          mb_grow_one(&mb->uv0, cap, 2) && mb_grow_one(&mb->uv1, cap, 2) &&
          mb_grow_one(&mb->col, cap, 4))) {
        mb->ok = false;
        return false;
    }
    mb->vcap = cap;
    return true;
}

unsigned int mb_vertex(MeshBuilder* mb, const vec3 p, const vec3 n, const vec3 t, float u0,
                       float v0, float u1, float v1, const float* rgba) {
    if (!mb->ok)
        return 0;
    if (mb->vcount == mb->vcap && !mb_grow_verts(mb))
        return 0;
    size_t i = mb->vcount++;
    memcpy(&mb->pos[i * 3], p, 3 * sizeof(float));
    memcpy(&mb->nrm[i * 3], n, 3 * sizeof(float));
    memcpy(&mb->tan[i * 4], t, 3 * sizeof(float));
    mb->tan[i * 4 + 3] = 1.0f; // right-handed: bitangent is cross(N, T)
    mb->uv0[i * 2 + 0] = u0;
    mb->uv0[i * 2 + 1] = v0;
    mb->uv1[i * 2 + 0] = u1;
    mb->uv1[i * 2 + 1] = v1;
    if (mb->want_colors)
        memcpy(&mb->col[i * 4], rgba, 4 * sizeof(float));
    return (unsigned int)i;
}

void mb_tri(MeshBuilder* mb, unsigned int a, unsigned int b, unsigned int c) {
    if (!mb->ok)
        return;
    if (mb->icount + 3 > mb->icap) {
        size_t cap = mb->icap * 2;
        unsigned int* p = safe_realloc(mb->idx, cap * sizeof(unsigned int));
        if (!p) {
            mb->ok = false;
            return;
        }
        mb->idx = p;
        mb->icap = cap;
    }
    mb->idx[mb->icount++] = a;
    mb->idx[mb->icount++] = b;
    mb->idx[mb->icount++] = c;
}

bool mb_transfer(MeshBuilder* mb, Mesh* mesh) {
    if (!mb->ok || mb->vcount == 0 || mb->icount == 0) {
        mb_free(mb);
        return false;
    }
    mesh->vertices = mb->pos;
    mesh->normals = mb->nrm;
    mesh->tangents = mb->tan;
    mesh->tex_coords = mb->uv0;
    mesh->tex_coords2 = mb->uv1;
    mesh->colors = mb->col;
    mesh->indices = mb->idx;
    mesh->vertex_count = mb->vcount;
    mesh->index_count = mb->icount;
    mesh->draw_mode = TRIANGLES;
    memset(mb, 0, sizeof(*mb));
    calculate_aabb(mesh);
    return true;
}
