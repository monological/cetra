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

#define KIT_MAX_MATERIALS 96
#define KIT_MAX_OPENINGS  8
#define KIT_COLLIDER_ONLY (-1) // a material slot that draws nothing and always collides
#define KIT_MAX_POINTS    32   // in a pipe's path or a lathe's profile
#define KIT_MAX_OUTLINE   128  // corners of a flat polygon or an extruded outline
#define KIT_ARCH_SEGMENTS 8    // segments in each half of an arch's head
#define KIT_ARCH_POINTS   (2 * KIT_ARCH_SEGMENTS + 1)

#define KIT_COUNT(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

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
    bool warned_nonfinite;

    // The edges water drips from in the rain (spec 13.12), in world space, for the rain to take.
    RainDripLine drips[RAIN_DRIP_MAX];
    int drip_count;
} Kit;

/*
 * The head of an arch: FLAT is a lintel; POINTED is two arcs meeting at an apex, an
 * equilateral arch when its rise is the span's sqrt(3)/2 and a lancet above that; TUDOR is the
 * four-centred arch of late Gothic, a tight haunch rolling into a long, low arc, for a rise up
 * to about 0.39 of the span (a higher one is drawn POINTED).
 */
typedef enum KitArchShape { KIT_ARCH_FLAT = 0, KIT_ARCH_POINTED, KIT_ARCH_TUDOR } KitArchShape;

// A hole in a wall: [from, to] along the wall's axis, [bottom, top] in world Y. An arched one
// springs at `top` and rises `rise` above it, so its head is cut into the wall over it.
typedef struct KitOpening {
    float from, to;
    float bottom, top;
    KitArchShape arch;
    float rise;
    bool door; // a doorway, walked through, rather than a window
} KitOpening;

// Whether the opening has a head above its springing line; a FLAT one, or one of no rise, has
// a lintel at `top`.
static inline bool kit_opening_arched(const KitOpening* o) {
    return o->arch != KIT_ARCH_FLAT && o->rise > 0.0f;
}

#define KIT_OPENING_POINTS (KIT_ARCH_POINTS + 2)

// The opening's outline in (a, y), counter-clockwise from its bottom-left corner: across the
// sill, up a jamb, round the head and down. Returns its corner count.
int kit_opening_outline(const KitOpening* o, vec2 out[KIT_OPENING_POINTS]);

// The opening grown by `w` at its jambs and its head, its sill where it was. A pointed head
// grows concentrically, so a frame round it keeps its width all the way over; a Tudor one
// only nearly.
KitOpening kit_opening_grow(const KitOpening* o, float w);

// An axis-aligned wall. It runs along X when `along_x`, at z = `at`, or along Z
// at x = `at`, from `from` to `to`. `inner` is the sign of the side the inner
// material faces (+1 toward +z/+x). Two layers of half the thickness each, so
// a room's plaster and a house's siding are one wall. In a frame (kit_frame_wall)
// it runs along a at d = `at`, and `inner` is the sign along d; `along_x` is unused.
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

// How far `p` is in plan, (x, z), from the segment a..b.
static inline float kit_plan_distance(const vec3 p, const vec2 a, const vec2 b) {
    const float dx = b[0] - a[0], dz = b[1] - a[1], len2 = dx * dx + dz * dz;
    const float t = len2 > 0.0f
                        ? glm_clamp(((p[0] - a[0]) * dx + (p[2] - a[1]) * dz) / len2, 0.0f, 1.0f)
                        : 0.0f;
    return hypotf(p[0] - (a[0] + t * dx), p[2] - (a[1] + t * dz));
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
// A flat polygon of `count` coplanar corners, concave or not but never crossing itself, cut
// into triangles by ear clipping and wound to face `outward`.
void kit_polygon_facing(Kit* kit, int mat, const vec3* corners, int count, const vec3 outward);

// A horizontal slab whose outline is `count` (x, z) corners, from y0 to y1. `collide` adds a
// body for it, one box per edge reaching in to the corners' average, which is exact only where
// every corner is at least a right angle and that average lies inside every edge, as in a
// regular polygon.
void kit_slab(Kit* kit, int mat, const vec2* xz, int count, float y0, float y1, bool collide);

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
// The box a0..a1 along, y0..y1 up, d0..d1 out, each range in either order.
void kit_frame_box(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float y0, float y1,
                   float d0, float d1, bool collide);

// A box's faces in its frame, to leave out the ones nothing can see: against a wall, on a
// shelf, under a card.
typedef enum KitFace {
    KIT_FACE_A_POS = 1 << 0,
    KIT_FACE_A_NEG = 1 << 1,
    KIT_FACE_UP = 1 << 2,
    KIT_FACE_DOWN = 1 << 3,
    KIT_FACE_OUT = 1 << 4, // +d
    KIT_FACE_IN = 1 << 5,  // -d
    KIT_FACES_ALL = (1 << 6) - 1,
} KitFace;
// The same box with only the KitFace bits `shown`, and no body.
void kit_frame_box_faces(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float y0,
                         float y1, float d0, float d1, unsigned shown);
// A wall in frame `f`: see KitWall. A wall is never grimed, whatever its materials: the seams
// between the slabs it is cut into round its openings are edges nobody built.
void kit_frame_wall(Kit* kit, const KitFrame* f, const KitWall* wall);
// One layer of `mat` the wall's whole thickness through, cut round its openings, with no body: a
// board cut to an arch, a lining on a wall that already stops the player. The wall's materials
// and inner side are unused.
void kit_frame_panel(Kit* kit, const KitFrame* f, int mat, const KitWall* wall);

// The frame a wall is drawn in, the d of its middle there, and which way along d (+1 or -1)
// its inner side lies.
typedef struct KitWallFrame {
    KitFrame f;
    float at;
    int inner;
} KitWallFrame;

// An axis-aligned wall's: what puts a pane, a door or a dressing on it.
KitWallFrame kit_wall_frame(const KitWall* wall);
// A body filling an opening of a wall `thick` through, its middle at d, so nobody walks or
// climbs through what fills it; an arch's head is solid to its crown.
void kit_frame_plug(Kit* kit, const KitFrame* f, const KitOpening* o, float d, float thick);
// A pane of `mat` 6 mm thick at d, filling an opening of a wall `thick` through, and its plug.
void kit_frame_pane(Kit* kit, const KitFrame* f, int mat, const KitOpening* o, float d,
                    float thick);
// A flat polygon of (a, y) corners at distance d, facing out (+d).
void kit_frame_polygon(Kit* kit, const KitFrame* f, int mat, const vec2* outline, int count,
                       float d);
// The same polygon showing `uv` = {u0, v0, u1, v1} of its material's picture stretched over
// the outline's bounds, as kit_frame_card does a rectangle -- facing +d, or -d when `back`, in
// which case the picture reads mirrored, as glass does from behind.
void kit_frame_card_polygon(Kit* kit, const KitFrame* f, int mat, const vec2* outline, int count,
                            float d, const float uv[4], bool back);
// The (a, y) outline extruded from d0 to d1: both faces and its edges, which are flat.
void kit_frame_extrude(Kit* kit, const KitFrame* f, int mat, const vec2* outline, int count,
                       float d0, float d1);
// A (d, y) profile swept straight along a from a0 to a1: a moulding, a sill, a cornice.
void kit_frame_run(Kit* kit, const KitFrame* f, int mat, const vec2* profile, int count, float a0,
                   float a1);
/*
 * The band `w` wide round an opening, from d0 to d1: its casing. `head_only` stops it at the
 * springing line, which is a hood over the opening or a bar of tracery across it; otherwise it
 * runs down both jambs to the sill. Exactly as wide all round on a pointed head.
 */
void kit_frame_surround(Kit* kit, const KitFrame* f, int mat, const KitOpening* o, float w,
                        bool head_only, float d0, float d1);
/*
 * A solid flight of `risers` steps, each `rise` high and `going` deep, climbing along +d from
 * d0 at floor height y0 between a0 and a1. The last riser lands on the floor above, so there
 * are risers - 1 treads; each step is a box down to y0, and collides.
 */
void kit_frame_stair(Kit* kit, const KitFrame* f, int mat, float a0, float a1, float y0, float d0,
                     float rise, float going, int risers);
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
// A card of `uv` filling a0..a1 and y0..y1 at d, facing +d, or -d when `toward` is negative --
// the right way up and round seen from the side it faces.
void kit_frame_card_rect(Kit* kit, const KitFrame* f, int mat, const float uv[4], float a0,
                         float a1, float y0, float y1, float d, float toward);
// Cards of `uv` end to end across a0..a1 the same way, as many as come nearest `width` each.
void kit_frame_card_row(Kit* kit, const KitFrame* f, int mat, const float uv[4], float a0, float a1,
                        float y0, float y1, float d, float toward, float width);

// Builds one mesh per used material under a node on the scene root.
SceneNode* kit_finish(Kit* kit, const char* name);

#endif // _SILENT_KIT_H_
