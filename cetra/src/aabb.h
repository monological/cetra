#ifndef _AABB_H_
#define _AABB_H_

/*
 * The axis-aligned box and its operations, on cglm alone, so a module wanting a box -- a mesh's
 * bound, an audio zone's room -- need not take GL and the material stack with it.
 */

#include <cglm/cglm.h>
#include <float.h>
#include <stdbool.h>

// TWO CONVENTIONS FOR "NOTHING HERE", and the difference is load-bearing.
//
// An ACCUMULATOR starts EMPTY -- min at +FLT_MAX, max at -FLT_MAX -- so the
// first point unions into it correctly and an untouched one is recognisable.
// That is what aabb_empty/aabb_is_empty below are for, and what the per-bone
// boxes and the posed-bounds accumulator use.
//
// A STORED GEOMETRY BOUND does not. `Mesh.aabb` starts at the ZERO box and stays
// there when nothing computed it, because five readers consume it with no
// emptiness guard at all -- the culler copies it straight into a cull bound, ray
// picking slabs against it, two passes upload its Y extent as the wind height
// mask, and scene.c unions its corners into the whole-scene box. Seeded with the
// sentinel instead, aabb_transform computes extents of -inf and that box poisons
// the scene bounds, the GI volume fit, the probe proxy and the camera framing.
// The zero box degrades to a point at the origin, which geometry.c and
// apps/tree/src/ground.c both record as the understood failure. Do not migrate
// it.
typedef struct {
    vec3 min;
    vec3 max;
} AABB;

// The five operations every consumer used to hand-roll. Read a box's emptiness
// through aabb_is_empty rather than testing a component, and build one through
// aabb_empty + aabb_add_point rather than seeding from the first element: the
// sentinel is a convention two files used to share by hand, and doing that once
// already cost a redundant flag beside an uninitialised box.
//
// Named after cglm's glm_aabb_invalidate/isvalid, which is the same convention
// and is compiled in here but used nowhere -- its box.h is not adopted because
// it has neither add_point nor expand, and its transform and frustum test are
// different arithmetic from ours at the frustum edge, where wind_cull_fixture
// deliberately parks meshes.
static inline void aabb_empty(AABB* box) {
    glm_vec3_fill(box->min, FLT_MAX);
    glm_vec3_fill(box->max, -FLT_MAX);
}

static inline bool aabb_is_empty(const AABB* box) {
    return box->min[0] > box->max[0];
}

static inline void aabb_add_point(AABB* box, const vec3 p) {
    glm_vec3_minv(box->min, (float*)p, box->min);
    glm_vec3_maxv(box->max, (float*)p, box->max);
}

// An empty `src` unions to a no-op, which is what lets a caller fold in a box
// that may hold nothing without asking whether it does.
static inline void aabb_union(AABB* box, const AABB* src) {
    glm_vec3_minv(box->min, (float*)src->min, box->min);
    glm_vec3_maxv(box->max, (float*)src->max, box->max);
}

static inline void aabb_expand(AABB* box, float margin) {
    glm_vec3_subs(box->min, margin, box->min);
    glm_vec3_adds(box->max, margin, box->max);
}

// Squared distance from `p` to the box, 0 inside it. Squared because every
// caller compares against a squared threshold and none of them wants the root.
//
// Here rather than at either caller because there were two, written differently
// -- a sign-branch walk in the quadtree's descent and a clamp-and-subtract in
// forest's residency -- and they answer the same question about the same shape.
// Two spellings of one predicate is where a fix lands on one of them.
static inline float aabb_dist_sq(const AABB* box, const vec3 p) {
    float d = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float v = glm_clamp(p[i], box->min[i], box->max[i]) - p[i];
        d += v * v;
    }
    return d;
}

// Whether two boxes share a point; boxes that only touch at a face do.
static inline bool aabb_overlaps(const AABB* a, const AABB* b) {
    for (int i = 0; i < 3; ++i)
        if (a->min[i] > b->max[i] || a->max[i] < b->min[i])
            return false;
    return true;
}

#endif // _AABB_H_
