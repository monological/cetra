#ifndef _MESH_BUILDER_H_
#define _MESH_BUILDER_H_

#include <stdbool.h>
#include <stddef.h>
#include <cglm/cglm.h>

#include "mesh.h"

// Growable parallel vertex arrays that become a Mesh's arrays outright.
//
// For geometry whose size is not known until it has been built. The engine's
// geometry.c primitives all compute an exact vertex count up front and hand-roll
// their own allocation, which works for a box or a sphere but not for a tree or
// a grass field.
//
// Colours are optional because most geometry has none, and an unused RGBA
// channel on a six-figure vertex count is not free. Everything else is always
// present.
typedef struct MeshBuilder {
    // `tan` is 4 floats per vertex: xyz tangent, w bitangent handedness. The
    // builder always writes w = +1 -- procedurally generated geometry has no
    // mirrored UV islands, so its bitangent is always cross(N, T), which is
    // what the shader derives. Callers therefore never supply a bitangent.
    float *pos, *nrm, *tan, *uv0, *uv1, *col;
    unsigned int* idx;
    size_t vcount, vcap, icount, icap;
    bool want_colors;
    bool ok; // cleared on allocation failure; every later call then no-ops
} MeshBuilder;

// `vres`/`ires` are reservations, not limits -- the builder grows past them.
// Reserving the real count avoids the copies, so census first where you can.
bool mb_init(MeshBuilder* mb, size_t vres, size_t ires, bool want_colors);
void mb_free(MeshBuilder* mb);

// `rgba` is read only when the builder was initialised with colours.
unsigned int mb_vertex(MeshBuilder* mb, const vec3 p, const vec3 n, const vec3 t, float u0,
                       float v0, float u1, float v1, const float* rgba);
void mb_tri(MeshBuilder* mb, unsigned int a, unsigned int b, unsigned int c);

// Hand the arrays to the mesh rather than copying them; the builder is emptied
// so nothing double-frees. Computes the AABB. False if nothing was built, in
// which case the builder is freed and the mesh left untouched.
bool mb_transfer(MeshBuilder* mb, Mesh* mesh);

#endif // _MESH_BUILDER_H_
