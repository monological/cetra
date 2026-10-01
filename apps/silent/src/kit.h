#ifndef _SILENT_KIT_H_
#define _SILENT_KIT_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/material.h"
#include "cetra/mesh_builder.h"
#include "cetra/rain.h"
#include "cetra/scene.h"
#include "cetra/game/entity.h"
#include "cetra/game/physics.h"

/*
 * The prop kit: flat-shaded geometry batched into one mesh per material, and
 * the static colliders that go with it.
 *
 * Every solid is described ONCE. A box that collides hands the same centre,
 * half-extents and yaw to the mesh and to the Jolt body, so what is drawn and
 * what is bumped into cannot drift apart.
 *
 * UVs are a planar projection in WORLD metres divided by the material's
 * repeat length, so every surface carries the same texel density whatever
 * its size, and neighbouring pieces continue one another's pattern. That
 * even, low density is most of what makes low-poly geometry read as one
 * console-era world rather than a set of stretched decals.
 *
 * A material registered with grime has its boxes' faces cut into a grid and
 * the dirt baked into the vertex colours, darkest along each face's edges --
 * the gaps round a door, where a cupboard meets the floor -- which is how the
 * consoles this imitates shaded a room, and costs the renderer nothing new.
 * Its smooth solids take the dirt where they are fixed on: a lathe at its
 * base, a pipe at both ends.
 */

#define KIT_MAX_MATERIALS 64
#define KIT_MAX_OPENINGS  6
#define KIT_COLLIDER_ONLY (-1) // a material slot that draws nothing and always collides
#define KIT_MAX_POINTS    32   // in a pipe's path or a lathe's profile

typedef struct Kit {
    Material* materials[KIT_MAX_MATERIALS];
    float repeat_m[KIT_MAX_MATERIALS]; // metres one texture repeat covers
    float grime[KIT_MAX_MATERIALS];    // 0..1 edge dirt baked into its boxes; 0 = none
    MeshBuilder builders[KIT_MAX_MATERIALS];
    int material_count;

    Scene* scene;
    EntityManager* em;
    PhysicsWorld* physics;
    int collider_count;
    int vertex_count; // everything kit_finish handed over

    // The edges water drips from in the rain (spec 13.12), in world space, for the rain to take.
    RainDripLine drips[RAIN_DRIP_MAX];
    int drip_count;
} Kit;

// A hole in a wall: [from, to] along the wall's axis, [bottom, top] in world Y.
typedef struct KitOpening {
    float from, to;
    float bottom, top;
} KitOpening;

// An axis-aligned wall. It runs along X when `along_x`, at z = `at`, or along Z
// at x = `at`, from `from` to `to`. `inner` is the sign of the side the inner
// material faces (+1 toward +z/+x). Two layers of half the thickness each, so
// a room's plaster and a house's siding are one wall.
typedef struct KitWall {
    bool along_x;
    float at, from, to;
    float y0, y1;
    float thick;
    int inner;
    int mat_inner, mat_outer;
    KitOpening openings[KIT_MAX_OPENINGS];
    int opening_count;
} KitWall;

// Repeatable placement: a seeded LCG, so one seed is always one world.
typedef struct KitRng {
    uint32_t s;
} KitRng;

static inline float kit_rnd(KitRng* r) {
    r->s = r->s * 1664525u + 1013904223u;
    return (float)(r->s >> 8) * (1.0f / 16777216.0f);
}

static inline float kit_rrange(KitRng* r, float lo, float hi) {
    return lo + (hi - lo) * kit_rnd(r);
}

void kit_init(Kit* kit, Scene* scene, EntityManager* em, PhysicsWorld* physics);

// Registers a material and returns its slot. The kit borrows the material
// until kit_finish attaches it to a mesh.
int kit_material(Kit* kit, Material* material, float repeat_m, float grime);

// A flat quad or triangle, wound to face `outward` whatever order the corners
// come in, and fanned from the first corner.
void kit_quad_facing(Kit* kit, int mat, const vec3 a, const vec3 b, const vec3 c, const vec3 d,
                     const vec3 outward);
void kit_tri_facing(Kit* kit, int mat, const vec3 a, const vec3 b, const vec3 c,
                    const vec3 outward);

// A box turned `yaw` radians about +Y. `collide` adds a static body for it.
void kit_box(Kit* kit, int mat, const vec3 centre, const vec3 half, float yaw, bool collide);
// A box that only collides: an invisible wall, a pane of glass.
void kit_collider(Kit* kit, const vec3 centre, const vec3 half, float yaw);

// An upright prism of `sides` faces (a bottle, a pipe, a post): radius r, from
// y0 to y1 at (x, z). Faceted on purpose. Capped both ends.
void kit_prism(Kit* kit, int mat, float x, float z, float y0, float y1, float r, int sides,
               bool collide);

// A prism lying along X or Z, for a tube, a rail or a handle.
void kit_prism_lying(Kit* kit, int mat, const vec3 centre, float half_len, float r, int sides,
                     bool along_x);

void kit_wall(Kit* kit, const KitWall* wall);

/*
 * A frame against a wall, for furnishing it: an origin on the floor at the
 * wall's face, and a yaw that turns local +X ALONG the wall and local +Z OUT
 * of it into the room. Local coordinates are (a, y, d): along, up, out.
 * Yaw 0 is a wall you face looking -Z; the kitchen's walls are all multiples
 * of a quarter turn.
 */
typedef struct KitFrame {
    vec3 origin;
    float yaw;
} KitFrame;

// The world itself as a frame: (a, y, d) is (x, y, z).
static const KitFrame KIT_WORLD = {{0.0f, 0.0f, 0.0f}, 0.0f};

void kit_frame_point(const KitFrame* f, float a, float y, float d, vec3 out);
// A direction turned by the frame's yaw, without its origin.
void kit_frame_dir(const KitFrame* f, float a, float y, float d, vec3 out);
// A drip line from `from` to `to`, (a, y, d) in frame `f` -- the same point twice for a single
// source -- dripping `rate` drops a second at the reference rain onto world Y `ground`.
void kit_drip(Kit* kit, const KitFrame* f, const vec3 from, const vec3 to, float rate,
              float ground);
// The box a0..a1 along, y0..y1 up, d0..d1 out.
void kit_frame_box(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float y0, float y1,
                   float d0, float d1, bool collide);
// An upright prism standing at (a, d).
void kit_frame_prism(Kit* kit, const KitFrame* f, int mat, float a, float d, float y0, float y1,
                     float r, int sides);
// A bar lying along the wall, from a0 to a1 at height y and distance d.
void kit_frame_bar(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float y, float d,
                   float r);
/*
 * The two SMOOTH solids, for what must read as round and polished rather than
 * faceted -- a pot, a tap. Points are (a, y, d); at most KIT_MAX_POINTS.
 */
// A round pipe of radius r along a polyline, capped both ends. A bend is a run
// of close points; the pipe stays round through it.
void kit_frame_pipe(Kit* kit, const KitFrame* f, int mat, const vec3* path, int count, float r,
                    int sides);
// A surface of revolution about the upright through (a, d), from {radius,
// height above y} points listed bottom to top up the outside. A radius of 0
// closes it on the axis; a turn sharper than 60 degrees is a crease.
void kit_frame_lathe(Kit* kit, const KitFrame* f, int mat, float a, float d, float y,
                     const vec2* profile, int count, int sides);
// The same about any axis, from `base` along `axis`, both (a, y, d): a disc
// facing the room, a finial on its side.
void kit_frame_lathe_on(Kit* kit, const KitFrame* f, int mat, const vec3 base, const vec3 axis,
                        const vec2* profile, int count, int sides);

// A flat card from `corner` along `across` and `up`, all (a, y, d), facing
// cross(across, up). It shows `uv` = {u0, v0, u1, v1} of its material's
// picture, where every other face is mapped from the world: a photograph is
// one picture, not a pattern.
void kit_frame_card(Kit* kit, const KitFrame* f, int mat, const vec3 corner, const vec3 across,
                    const vec3 up, const float uv[4]);

// Builds one mesh per used material under a node on the scene root.
SceneNode* kit_finish(Kit* kit, const char* name);

#endif // _SILENT_KIT_H_
