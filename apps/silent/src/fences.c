#include <math.h>

#include "fences.h"
#include "land.h"
#include "layout.h"
#include "mats.h"

/*
 * The yards' fences (spec 13.35). Four kinds, each built along a straight RUN in a frame whose a
 * runs from the run's start to its end and whose d is square to it: a privacy fence of boards, a
 * picket fence, chain link on galvanised posts, and a cinder-block wall. A run is cut into
 * sections between posts, no longer than its kind's spacing, and either side of its gate if it
 * has one. Every one of them is old: boards are missing, broken off or slipped in their nails, a
 * section sags, pickets are gone, top blocks are off their wall, and here and there a whole length
 * has come down.
 *
 * One body runs the length of each fence, its gate included unless that stands open. A fallen
 * section keeps it: the gap it leaves is reported, for something to fill.
 */

#define POST_MAX 512 // posts placed

#define BOARD_HEIGHT  1.8f
#define BOARD_SPACING 2.4f   // metres between posts at most
#define BOARD_POST    0.045f // half a post's side
#define BOARD_WIDTH   0.14f
#define BOARD_PITCH   0.152f
#define BOARD_THICK   0.02f
#define RAIL_THICK    0.04f // a rail is a 2x4 on edge, between the posts' faces
#define RAIL_DEEP     0.09f
#define RAIL_LOW      0.25f // its bottom rail's foot, and the top rail's below the fence's top
#define RAIL_HIGH     0.4f

#define PICKET_HEIGHT 1.0f
#define PICKET_WIDTH  0.075f
#define PICKET_PITCH  0.14f
#define PICKET_RAIL   0.07f

#define CHAIN_HEIGHT  1.8f
#define CHAIN_SPACING 3.0f
#define CHAIN_POST    0.03f  // a line post's radius
#define CHAIN_END     0.042f // a terminal post's, at a run's ends and its gate
#define CHAIN_RAIL    0.021f

#define BLOCK_HEIGHT 1.6f
#define BLOCK_HALF   0.1f // half the wall's thickness
#define BLOCK_LEN    0.4f
#define BLOCK_COURSE 0.2f
#define BLOCK_PIER   0.2f // half a pier's side
#define BLOCK_PIERS  3.0f // metres between piers at most

#define GATE_WIDTH   1.0f
#define COLLIDE_HALF 0.08f

typedef enum FenceKind {
    FENCE_NONE,
    FENCE_BOARD,
    FENCE_PICKET,
    FENCE_CHAIN,
    FENCE_BLOCK
} FenceKind;

typedef enum GateState {
    GATE_NONE,    // no gate in it
    GATE_GAP,     // an opening with nothing hung in it
    GATE_OPEN,    // a leaf swung back into the yard
    GATE_SHUT,    // a leaf shut
    GATE_LATCHED, // shut, and padlocked
} GateState;

typedef struct Fence {
    FenceKind kind;
    vec2 a, b;    // its run in plan, (x, z)
    float y;      // the ground it stands on
    float height; // its top above y
    int face;     // +1 when its good face looks along +d, -1 along -d; the rails are behind it
    GateState gate;
    float gate_at;   // along the run, the gate's middle
    float fallen_at; // along it, in the section that has come down; < 0 none
    float torn_at;   // along it, in a chain-link panel with a corner torn out; < 0 none
} Fence;

typedef struct Build {
    Kit* kit;
    KitRng rng;
    FenceBreaches* breaches;
    struct {
        FenceKind kind;
        float x, z;
    } posts[POST_MAX];
    int post_count;
} Build;

static Fence fence(FenceKind kind, float ax, float az, float bx, float bz, float y, int face) {
    static const float HEIGHTS[] = {
        [FENCE_BOARD] = BOARD_HEIGHT,
        [FENCE_PICKET] = PICKET_HEIGHT,
        [FENCE_CHAIN] = CHAIN_HEIGHT,
        [FENCE_BLOCK] = BLOCK_HEIGHT,
    };
    return (Fence){kind, {ax, az}, {bx, bz}, y, HEIGHTS[kind], face, GATE_NONE, 0.0f, -1.0f, -1.0f};
}

// The run's frame, and its length.
static KitFrame run_frame(const Fence* fe, float* len) {
    const float dx = fe->b[0] - fe->a[0], dz = fe->b[1] - fe->a[1];
    *len = hypotf(dx, dz);
    return (KitFrame){{fe->a[0], fe->y, fe->a[1]}, atan2f(-dz, dx)};
}

// Whether a post of `kind` at `a` along frame `f` is still to be placed; one two fences meeting
// at a corner share is placed once, by the first, or the two would be the same box twice.
static bool post_claim(Build* b, FenceKind kind, const KitFrame* f, float a) {
    vec3 p = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, a, 0.0f, 0.0f, p);
    for (int i = 0; i < b->post_count; i++)
        if (b->posts[i].kind == kind && fabsf(b->posts[i].x - p[0]) < 0.1f &&
            fabsf(b->posts[i].z - p[2]) < 0.1f)
            return false;
    if (b->post_count < POST_MAX) {
        b->posts[b->post_count].kind = kind;
        b->posts[b->post_count].x = p[0];
        b->posts[b->post_count].z = p[2];
        b->post_count++;
    }
    return true;
}

// A board or bar lying in the frame's (a, y) plane from (a0, y0) to (a1, y1), `half` either side
// of that line and from d0 to d1: a gate's brace.
static void brace(Kit* kit, const KitFrame* f, int mat, float a0, float y0, float a1, float y1,
                  float half, float d0, float d1) {
    const float len = hypotf(a1 - a0, y1 - y0);
    const float ta = (a1 - a0) / len, ty = (y1 - y0) / len;
    const float na = -ty * half, ny = ta * half;
    const vec2 c[4] = {
        {a0 + na, y0 + ny}, {a0 - na, y0 - ny}, {a1 - na, y1 - ny}, {a1 + na, y1 + ny}};
    const float ds[2] = {d0, d1};
    for (int s = 0; s < 2; s++) {
        const vec3 q[4] = {{c[0][0], c[0][1], ds[s]},
                           {c[1][0], c[1][1], ds[s]},
                           {c[2][0], c[2][1], ds[s]},
                           {c[3][0], c[3][1], ds[s]}};
        kit_frame_quad(kit, f, mat, q, (vec3){0.0f, 0.0f, s ? 1.0f : -1.0f});
    }
    // Its four edges, each between corner i and i + 1.
    const vec2 out[4] = {{-ta, -ty}, {-na, -ny}, {ta, ty}, {na, ny}};
    for (int i = 0; i < 4; i++) {
        const int j = (i + 1) % 4;
        const vec3 q[4] = {{c[i][0], c[i][1], d0},
                           {c[j][0], c[j][1], d0},
                           {c[j][0], c[j][1], d1},
                           {c[i][0], c[i][1], d1}};
        kit_frame_quad(kit, f, mat, q, (vec3){out[i][0], out[i][1], 0.0f});
    }
}

// The face side's d for a fence's boards, mesh or pickets: from `near` to `far` out from the
// post line on its good side.
static float face_d(const Fence* fe, float d) {
    return (float)fe->face * d;
}

/*
 * Boards from a0 to a1 on the face side, each `top` high less a sag across the section, or
 * missing, broken off or slipped down in its nails. A board stops short of the ground, which
 * rots it, unless it has slipped.
 */
static void boards(Build* b, const Fence* fe, const KitFrame* f, float a0, float a1, float top,
                   float sag) {
    const int n = (int)floorf((a1 - a0) / BOARD_PITCH);
    const float start = a0 + 0.5f * ((a1 - a0) - (float)n * BOARD_PITCH);
    const float d0 = face_d(fe, BOARD_POST), d1 = face_d(fe, BOARD_POST + BOARD_THICK);
    for (int i = 0; i < n; i++) {
        const float a = start + (float)i * BOARD_PITCH;
        const float t = (a + 0.5f * BOARD_WIDTH - a0) / (a1 - a0);
        float y1 = top - sag * 4.0f * t * (1.0f - t) + kit_rrange(&b->rng, -0.012f, 0.012f);
        float y0 = 0.03f;
        const float r = kit_rnd(&b->rng);
        if (r < 0.035f)
            continue;
        if (r < 0.08f) {
            y1 -= kit_rrange(&b->rng, 0.25f, 0.7f);
        } else if (r < 0.12f) {
            const float slip = kit_rrange(&b->rng, 0.04f, 0.12f);
            y1 -= slip;
            y0 = 0.0f;
        }
        kit_frame_box_faces(b->kit, f, MAT_FENCE_BOARD, a, a + BOARD_WIDTH, y0, y1, d0, d1,
                            KIT_FACES_ALL & ~KIT_FACE_DOWN);
    }
}

// A board fence's section that has fallen out flat on its face side, its rails on top of it, on
// whatever ground is there; and the gap it leaves, reported.
static void board_fallen(Build* b, const Fence* fe, const KitFrame* f, float a0, float a1) {
    const float s = (float)fe->face, reach = BOARD_POST + 0.05f;
    const int n = (int)floorf((a1 - a0) / BOARD_PITCH);
    const float start = a0 + 0.5f * ((a1 - a0) - (float)n * BOARD_PITCH);
    // The ground under the panel, from its middle.
    vec3 mid = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, 0.5f * (a0 + a1), 0.0f, s * (reach + 0.5f * fe->height), mid);
    const float ground = land_height(mid[0], mid[2]) - fe->y;
    for (int i = 0; i < n; i++) {
        if (kit_rnd(&b->rng) < 0.2f)
            continue;
        const float a = start + (float)i * BOARD_PITCH + kit_rrange(&b->rng, -0.02f, 0.02f);
        const float len =
            fe->height - (kit_rnd(&b->rng) < 0.15f ? kit_rrange(&b->rng, 0.2f, 0.6f) : 0.0f);
        kit_frame_box(b->kit, f, MAT_FENCE_BOARD, a, a + BOARD_WIDTH, ground, ground + BOARD_THICK,
                      s * reach, s * (reach + len), false);
    }
    const float rails[2] = {RAIL_LOW, fe->height - RAIL_HIGH};
    for (int r = 0; r < 2; r++)
        kit_frame_box(b->kit, f, MAT_FENCE_BOARD, a0 + BOARD_POST, a1 - BOARD_POST,
                      ground + BOARD_THICK, ground + BOARD_THICK + RAIL_THICK,
                      s * (reach + rails[r]), s * (reach + rails[r] + RAIL_DEEP), false);
    if (b->breaches->count < FENCE_BREACH_MAX) {
        FenceBreach* br = &b->breaches->at[b->breaches->count++];
        vec3 p0 = {0.0f, 0.0f, 0.0f}, p1 = {0.0f, 0.0f, 0.0f}, out = {0.0f, 0.0f, 0.0f};
        kit_frame_point(f, a0, 0.0f, 0.0f, p0);
        kit_frame_point(f, a1, 0.0f, 0.0f, p1);
        kit_frame_dir(f, 0.0f, 0.0f, s, out);
        br->a[0] = p0[0];
        br->a[1] = p0[2];
        br->b[0] = p1[0];
        br->b[1] = p1[2];
        br->out[0] = out[0];
        br->out[1] = out[2];
        br->y = fe->y;
    }
}

// The two rails between a section's posts, flush with the posts' face side, a 2x4 each.
static void rails(Build* b, const Fence* fe, const KitFrame* f, int mat, float a0, float a1,
                  float low, float high, float deep) {
    const float d0 = face_d(fe, 0.005f), d1 = face_d(fe, BOARD_POST);
    kit_frame_box(b->kit, f, mat, a0, a1, low, low + deep, d0, d1, false);
    kit_frame_box(b->kit, f, mat, a0, a1, high - deep, high, d0, d1, false);
}

static void board_section(Build* b, const Fence* fe, const KitFrame* f, float a0, float a1) {
    if (fe->fallen_at >= a0 && fe->fallen_at < a1) {
        board_fallen(b, fe, f, a0, a1);
        return;
    }
    const float sag = kit_rnd(&b->rng) < 0.3f ? kit_rrange(&b->rng, 0.02f, 0.1f) : 0.0f;
    rails(b, fe, f, MAT_FENCE_BOARD, a0 + BOARD_POST, a1 - BOARD_POST, RAIL_LOW,
          fe->height - RAIL_HIGH + RAIL_DEEP, RAIL_DEEP);
    boards(b, fe, f, a0, a1, fe->height, sag);
}

// One picket, its point up, from (a, y0) to a top `top`; a broken one is cut off square lower.
static void picket(Kit* kit, const Fence* fe, const KitFrame* f, float a, float y0, float top,
                   bool broken) {
    const float w = PICKET_WIDTH, d0 = face_d(fe, BOARD_POST),
                d1 = face_d(fe, BOARD_POST + BOARD_THICK);
    if (broken) {
        const vec2 square[4] = {{a, y0}, {a + w, y0}, {a + w, top}, {a, top}};
        kit_frame_extrude(kit, f, MAT_PICKET, square, 4, d0, d1);
        return;
    }
    const vec2 pointed[5] = {
        {a, y0}, {a + w, y0}, {a + w, top - 0.5f * w}, {a + 0.5f * w, top}, {a, top - 0.5f * w}};
    kit_frame_extrude(kit, f, MAT_PICKET, pointed, 5, d0, d1);
}

static void picket_section(Build* b, const Fence* fe, const KitFrame* f, float a0, float a1) {
    rails(b, fe, f, MAT_PICKET, a0 + BOARD_POST, a1 - BOARD_POST, 0.2f, fe->height - 0.22f,
          PICKET_RAIL);
    const int n = (int)floorf((a1 - a0) / PICKET_PITCH);
    const float start = a0 + 0.5f * ((a1 - a0) - (float)n * PICKET_PITCH);
    for (int i = 0; i < n; i++) {
        const float a = start + (float)i * PICKET_PITCH + 0.5f * (PICKET_PITCH - PICKET_WIDTH);
        const float r = kit_rnd(&b->rng);
        float top = fe->height + kit_rrange(&b->rng, -0.015f, 0.015f);
        if (r < 0.05f)
            continue;
        const bool broken = r < 0.11f;
        if (broken)
            top -= kit_rrange(&b->rng, 0.2f, 0.5f);
        picket(b->kit, fe, f, a, 0.04f, top, broken);
    }
}

// Rodrigues: `p` turned `angle` about the axis through `o` along unit `u`.
static void turn_about(const vec3 p, const vec3 o, const vec3 u, float angle, vec3 out) {
    vec3 v, c;
    glm_vec3_sub((float*)p, (float*)o, v);
    glm_vec3_cross((float*)u, v, c);
    const float k = glm_vec3_dot((float*)u, v), cs = cosf(angle), sn = sinf(angle);
    for (int i = 0; i < 3; i++)
        out[i] = o[i] + v[i] * cs + c[i] * sn + u[i] * k * (1.0f - cs);
}

static void chain_section(Build* b, const Fence* fe, const KitFrame* f, float a0, float a1) {
    Kit* kit = b->kit;
    const float top = fe->height - 0.02f;
    kit_frame_bar(kit, f, MAT_GALVANISED, a0, a1, top, 0.0f, CHAIN_RAIL);
    kit_frame_bar(kit, f, MAT_GALVANISED, a0, a1, 0.08f, 0.0f, 0.004f);
    const float d = face_d(fe, CHAIN_POST + 0.006f), y0 = 0.04f, y1 = top - CHAIN_RAIL;
    const vec3 out = {0.0f, 0.0f, (float)fe->face};
    if (!(fe->torn_at >= a0 && fe->torn_at < a1)) {
        const vec3 q[4] = {{a0, y0, d}, {a1, y0, d}, {a1, y1, d}, {a0, y1, d}};
        kit_frame_quad(kit, f, MAT_CHAINLINK, q, out);
        return;
    }
    // Torn: a corner cut free where somebody went under, and peeled back up off the ground nearly
    // flat against the mesh above it, which leaves the hole open below.
    const float cut_a = a1 - kit_rrange(&b->rng, 0.8f, 1.1f);
    const float cut_y = y0 + kit_rrange(&b->rng, 0.8f, 1.1f);
    const vec2 rest[5] = {{a0, y0}, {cut_a, y0}, {a1, cut_y}, {a1, y1}, {a0, y1}};
    kit_frame_polygon(kit, f, MAT_CHAINLINK, rest, 5, d);
    vec3 hinge = {cut_a, y0, d}, axis = {a1 - cut_a, cut_y - y0, 0.0f}, corner = {a1, y0, d};
    glm_vec3_normalize(axis);
    vec3 lifted = {0.0f, 0.0f, 0.0f};
    turn_about(corner, hinge, axis, -(float)fe->face * kit_rrange(&b->rng, 2.4f, 2.8f), lifted);
    vec3 w[3] = {{0.0f}}, up = {0.0f, 0.0f, 0.0f};
    kit_frame_point(f, hinge[0], hinge[1], hinge[2], w[0]);
    kit_frame_point(f, a1, cut_y, d, w[1]);
    kit_frame_point(f, lifted[0], lifted[1], lifted[2], w[2]);
    kit_frame_dir(f, 0.0f, 0.0f, (float)fe->face, up);
    kit_tri_facing(kit, MAT_CHAINLINK, w[0], w[1], w[2], up);
}

static void block_section(Build* b, const Fence* fe, const KitFrame* f, float a0, float a1) {
    Kit* kit = b->kit;
    const float course = fe->height - BLOCK_COURSE;
    kit_frame_box(kit, f, MAT_CINDER, a0, a1, 0.0f, course, -BLOCK_HALF, BLOCK_HALF, false);
    // The top course block by block, between the piers: some gone, some knocked askew.
    const float lo = a0 + BLOCK_PIER, hi = a1 - BLOCK_PIER;
    for (float a = lo; a < hi - 0.05f; a += BLOCK_LEN) {
        const float r = kit_rnd(&b->rng);
        if (r < 0.07f)
            continue;
        const float shift = r < 0.15f ? kit_rrange(&b->rng, -0.04f, 0.04f) : 0.0f;
        kit_frame_box(kit, f, MAT_CINDER, a + 0.005f, fminf(a + BLOCK_LEN, hi) - 0.005f, course,
                      fe->height, -BLOCK_HALF + shift, BLOCK_HALF + shift, false);
    }
}

// A post at `a`: `end` is one at a run's end or its gate, which takes the strain.
static void post(Build* b, const Fence* fe, const KitFrame* f, float a, bool end) {
    if (!post_claim(b, fe->kind, f, a))
        return;
    Kit* kit = b->kit;
    switch (fe->kind) {
        case FENCE_BOARD:
            kit_frame_box(kit, f, MAT_FENCE_BOARD, a - BOARD_POST, a + BOARD_POST, 0.0f,
                          fe->height + 0.04f, -BOARD_POST, BOARD_POST, false);
            break;
        case FENCE_PICKET:
            kit_frame_box(kit, f, MAT_PICKET, a - BOARD_POST, a + BOARD_POST, 0.0f,
                          fe->height - 0.06f, -BOARD_POST, BOARD_POST, false);
            break;
        case FENCE_CHAIN:
            kit_frame_prism(kit, f, MAT_GALVANISED, a, 0.0f, 0.0f,
                            fe->height + (end ? 0.06f : 0.02f), end ? CHAIN_END : CHAIN_POST, 8);
            break;
        case FENCE_BLOCK:
            kit_frame_box(kit, f, MAT_CINDER, a - BLOCK_PIER, a + BLOCK_PIER, 0.0f,
                          fe->height + 0.1f, -BLOCK_PIER, BLOCK_PIER, false);
            break;
        case FENCE_NONE:
            break;
    }
}

// The fence from s0 to s1 along its run: posts, and the sections between them.
static void span(Build* b, const Fence* fe, const KitFrame* f, float s0, float s1) {
    static const float SPACING[] = {
        [FENCE_BOARD] = BOARD_SPACING,
        [FENCE_PICKET] = BOARD_SPACING,
        [FENCE_CHAIN] = CHAIN_SPACING,
        [FENCE_BLOCK] = BLOCK_PIERS,
    };
    if (s1 - s0 < 0.2f)
        return;
    const int n = (int)ceilf((s1 - s0) / SPACING[fe->kind]);
    const float step = (s1 - s0) / (float)n;
    for (int i = 0; i <= n; i++)
        post(b, fe, f, s0 + step * (float)i, i == 0 || i == n);
    for (int i = 0; i < n; i++) {
        const float a0 = s0 + step * (float)i, a1 = a0 + step;
        switch (fe->kind) {
            case FENCE_BOARD:
                board_section(b, fe, f, a0, a1);
                break;
            case FENCE_PICKET:
                picket_section(b, fe, f, a0, a1);
                break;
            case FENCE_CHAIN:
                chain_section(b, fe, f, a0, a1);
                break;
            case FENCE_BLOCK:
                block_section(b, fe, f, a0, a1);
                break;
            case FENCE_NONE:
                break;
        }
    }
}

// A shut leaf's padlock at its free edge (a, y), on the yard side, where whoever keeps it locked
// stands: a latch bar out to the post, a padlock hanging off it; on chain link, a chain round the
// leaf's end and the post as well.
static void padlock(Kit* kit, const Fence* fe, const KitFrame* f, float a, float y) {
    const float d = face_d(fe, -0.054f);
    kit_frame_box(kit, f, MAT_IRON, a - 0.1f, a + 0.07f, y - 0.025f, y + 0.025f, 0.0f, d, false);
    kit_frame_box(kit, f, MAT_BRASS, a + 0.01f, a + 0.06f, y - 0.12f, y - 0.06f, d,
                  d + face_d(fe, -0.025f), false);
    kit_frame_prism(kit, f, MAT_STEEL, a + 0.035f, d + face_d(fe, -0.012f), y - 0.06f, y - 0.025f,
                    0.005f, 4);
    if (fe->kind != FENCE_CHAIN)
        return;
    vec3 ring[13];
    for (int i = 0; i <= 12; i++) {
        const float t = 2.0f * GLM_PIf * (float)i / 12.0f;
        ring[i][0] = a + 0.053f + 0.11f * cosf(t);
        ring[i][1] = y + 0.04f;
        ring[i][2] = 0.065f * sinf(t);
    }
    kit_frame_pipe(kit, f, MAT_STEEL, ring, 13, 0.006f, 4);
}

// A gate's leaf, `w` wide, in its hinge's frame: its a runs from the hinge to its free edge.
static void leaf(Build* b, const Fence* fe, const KitFrame* h, float w) {
    Kit* kit = b->kit;
    const float top = fe->height - 0.05f;
    switch (fe->kind) {
        case FENCE_BOARD:
        case FENCE_PICKET: {
            const int mat = fe->kind == FENCE_BOARD ? MAT_FENCE_BOARD : MAT_PICKET;
            const float low = 0.2f, high = top - 0.3f;
            rails(b, fe, h, mat, 0.02f, w - 0.02f, low, high, RAIL_DEEP);
            brace(kit, h, mat, 0.08f, low + RAIL_DEEP, w - 0.08f, high - RAIL_DEEP, 0.045f,
                  face_d(fe, 0.005f), face_d(fe, BOARD_POST));
            if (fe->kind == FENCE_BOARD) {
                boards(b, fe, h, 0.0f, w, top, 0.0f);
            } else {
                for (float a = 0.03f; a < w - PICKET_WIDTH; a += PICKET_PITCH)
                    picket(kit, fe, h, a, 0.06f, top, false);
            }
            // Strap hinges.
            for (int i = 0; i < 2; i++) {
                const float y = i ? high - 0.5f * RAIL_DEEP : low + 0.5f * RAIL_DEEP;
                kit_frame_box(kit, h, MAT_IRON, -0.04f, 0.35f, y - 0.02f, y + 0.02f,
                              face_d(fe, BOARD_POST + BOARD_THICK),
                              face_d(fe, BOARD_POST + BOARD_THICK + 0.005f), false);
            }
            break;
        }
        case FENCE_CHAIN: {
            const vec3 frame[5] = {{0.03f, 0.08f, 0.0f},
                                   {w - 0.03f, 0.08f, 0.0f},
                                   {w - 0.03f, top, 0.0f},
                                   {0.03f, top, 0.0f},
                                   {0.03f, 0.08f, 0.0f}};
            kit_frame_pipe(kit, h, MAT_GALVANISED, frame, 5, CHAIN_RAIL, 6);
            kit_frame_bar(kit, h, MAT_GALVANISED, 0.03f, w - 0.03f, 0.5f * top, 0.0f, CHAIN_RAIL);
            const float d = face_d(fe, CHAIN_RAIL + 0.004f);
            const vec3 q[4] = {
                {0.03f, 0.08f, d}, {w - 0.03f, 0.08f, d}, {w - 0.03f, top, d}, {0.03f, top, d}};
            kit_frame_quad(kit, h, MAT_CHAINLINK, q, (vec3){0.0f, 0.0f, (float)fe->face});
            break;
        }
        case FENCE_BLOCK:
        case FENCE_NONE:
            break;
    }
    if (fe->gate == GATE_LATCHED)
        padlock(kit, fe, h, w - 0.04f, 0.55f * top + 0.2f);
}

static void build(Build* b, const Fence* fe) {
    if (fe->kind == FENCE_NONE)
        return;
    float len = 0.0f;
    const KitFrame f = run_frame(fe, &len);
    float g0 = len, g1 = len;
    if (fe->gate != GATE_NONE) {
        g0 = glm_clamp(fe->gate_at - 0.5f * GATE_WIDTH, 0.3f, len - 0.3f - GATE_WIDTH);
        g1 = g0 + GATE_WIDTH;
    }
    span(b, fe, &f, 0.0f, g0);
    span(b, fe, &f, g1, len);

    const bool through = fe->gate == GATE_GAP || fe->gate == GATE_OPEN;
    const float top = fe->height + 0.1f;
    if (through) {
        kit_frame_box(b->kit, &f, KIT_COLLIDER_ONLY, 0.0f, g0, 0.0f, top, -COLLIDE_HALF,
                      COLLIDE_HALF, true);
        kit_frame_box(b->kit, &f, KIT_COLLIDER_ONLY, g1, len, 0.0f, top, -COLLIDE_HALF,
                      COLLIDE_HALF, true);
    } else {
        kit_frame_box(b->kit, &f, KIT_COLLIDER_ONLY, 0.0f, len, 0.0f, top, -COLLIDE_HALF,
                      COLLIDE_HALF, true);
    }
    if (fe->gate == GATE_NONE || fe->gate == GATE_GAP)
        return;

    // The leaf hangs off the post at g0, swung back into the yard -- away from the good face --
    // when it stands open.
    const float jamb = fe->kind == FENCE_CHAIN ? CHAIN_END : BOARD_POST;
    const float w = GATE_WIDTH - 2.0f * jamb - 0.03f;
    const float swing = fe->gate == GATE_OPEN
                            ? (float)fe->face * glm_rad(kit_rrange(&b->rng, 55.0f, 100.0f))
                            : 0.0f;
    KitFrame h = f;
    kit_frame_point(&f, g0 + jamb + 0.015f, 0.0f, 0.0f, h.origin);
    h.yaw = f.yaw + swing;
    leaf(b, fe, &h, w);
    if (fe->gate == GATE_OPEN)
        kit_frame_box(b->kit, &h, KIT_COLLIDER_ONLY, 0.0f, w, 0.0f, top, -0.05f, 0.05f, true);
}

/*
 * What each lot has, west to east. A lot's YARD kind is its returns' and its back fence's; a lot
 * with no house has no returns. Gates and the fallen section are placed by world x.
 */
typedef struct Lot {
    FenceKind front;
    GateState front_gate;
    FenceKind yard;
    GateState side_gate; // in the return across the wider side yard, the east one on a tie
    GateState back_gate;
    float back_gate_x;
    float fallen_x; // in the back fence's section that is down; NAN none
    float torn_x;   // in the front's chain-link panel that is torn; NAN none
} Lot;

// The fence on each line between the lots, west to east, and whether it runs from the front of
// the lots rather than from the houses' returns.
typedef struct Line {
    FenceKind kind;
    bool full;
} Line;

// Our side: a vacant lot at each end, the four neighbours, and ours in the middle, its front open.
static const Lot NEAR[NEAR_LOTS] = {
    {FENCE_CHAIN, GATE_LATCHED, FENCE_CHAIN, GATE_NONE, GATE_NONE, 0.0f, NAN, NAN},
    {FENCE_PICKET, GATE_OPEN, FENCE_BOARD, GATE_SHUT, GATE_NONE, 0.0f, NAN, NAN},
    {FENCE_BLOCK, GATE_GAP, FENCE_BOARD, GATE_LATCHED, GATE_NONE, 0.0f, NAN, NAN},
    {FENCE_NONE, GATE_NONE, FENCE_BOARD, GATE_OPEN, GATE_LATCHED, -0.75f, 3.2f, NAN},
    {FENCE_CHAIN, GATE_OPEN, FENCE_CHAIN, GATE_SHUT, GATE_NONE, 0.0f, NAN, NAN},
    {FENCE_PICKET, GATE_SHUT, FENCE_BOARD, GATE_SHUT, GATE_NONE, 0.0f, NAN, NAN},
    {FENCE_CHAIN, GATE_NONE, FENCE_CHAIN, GATE_NONE, GATE_NONE, 0.0f, NAN, 40.0f},
};
static const Line NEAR_LINES[NEAR_LOTS + 1] = {
    {FENCE_CHAIN, true},  {FENCE_CHAIN, true},  {FENCE_BOARD, false}, {FENCE_BOARD, false},
    {FENCE_BOARD, false}, {FENCE_BLOCK, false}, {FENCE_CHAIN, true},  {FENCE_CHAIN, true},
};

// The far side, a lot of the terrace's each, every one with a house; one yard is open, and its
// back fence is down.
static const Lot FAR[TERRACE_LOTS] = {
    {FENCE_NONE, GATE_NONE, FENCE_BOARD, GATE_SHUT, GATE_NONE, 0.0f, NAN, NAN},
    {FENCE_NONE, GATE_NONE, FENCE_BOARD, GATE_LATCHED, GATE_NONE, 0.0f, NAN, NAN},
    {FENCE_NONE, GATE_NONE, FENCE_BOARD, GATE_OPEN, GATE_NONE, 0.0f, -9.0f, NAN},
    {FENCE_NONE, GATE_NONE, FENCE_CHAIN, GATE_SHUT, GATE_NONE, 0.0f, NAN, NAN},
    {FENCE_NONE, GATE_NONE, FENCE_BOARD, GATE_SHUT, GATE_NONE, 0.0f, NAN, NAN},
    {FENCE_NONE, GATE_NONE, FENCE_BOARD, GATE_LATCHED, GATE_NONE, 0.0f, NAN, NAN},
};
static const Line FAR_LINES[TERRACE_LOTS + 1] = {
    {FENCE_CHAIN, true},  {FENCE_BOARD, false}, {FENCE_BOARD, false}, {FENCE_CHAIN, false},
    {FENCE_BOARD, false}, {FENCE_BLOCK, false}, {FENCE_CHAIN, true},
};

#define NEAR_FRONT_Z  (STREET_HALF_WIDTH + 1.6f)
#define NEAR_RETURN_Z 15.0f // behind our chimney, and every neighbour's front
// Just behind the guard rail on the terrace wall's coping, so nobody gets between the two.
#define FAR_FRONT_Z  (TERRACE_WALL_Z - 0.5f * TERRACE_WALL_THICK - 0.15f)
#define FAR_RETURN_Z (FAR_HOUSE_FRONT_Z - 5.0f)

/*
 * One side of the street: what each of its `n` lots has, the fences on the lines between them,
 * where those lines stand -- the ends `end_inset` in from the street's ends -- and the ground each
 * lot stands on, each lot's house, and the lines across the lots the fronts, the returns and the
 * backs run along.
 */
typedef struct Side {
    const Lot* lots;
    const Line* lines;
    int n;
    float (*line_x)(int k);
    float end_inset;
    float (*lot_y)(int lot);
    const HousePlot* houses;
    float front_z, return_z, back_z;
} Side;

// Our side's lots, every one on the street's level.
static float street_level(int lot) {
    (void)lot;
    return 0.0f;
}

static float side_line_x(const Side* s, int k) {
    return s->line_x(k) + (k == 0 ? s->end_inset : (k == s->n ? -s->end_inset : 0.0f));
}

// One side's fences. A line stands on the lot east of it, and as much taller as the step up to
// the lot west of it.
static void side(Build* b, const Side* s) {
    const float toward = s->back_z > s->front_z ? 1.0f : -1.0f;
    // A run along +x has +d toward +z: the good face of a front or a return looks back to the
    // street, of a back fence out to the woods.
    const int to_street = toward > 0.0f ? -1 : 1;
    for (int i = 0; i <= s->n; i++) {
        const Line* l = &s->lines[i];
        const float x = side_line_x(s, i), z0 = l->full ? s->front_z : s->return_z;
        const float y = s->lot_y(i < s->n ? i : s->n - 1);
        Fence fe = fence(l->kind, x, z0, x, s->back_z, y, i % 2 ? 1 : -1);
        fe.height += i > 0 && i < s->n ? s->lot_y(i - 1) - y : 0.0f;
        build(b, &fe);
    }
    for (int i = 0; i < s->n; i++) {
        const Lot* lot = &s->lots[i];
        const float x0 = side_line_x(s, i), x1 = side_line_x(s, i + 1);
        const float y = s->lot_y(i);
        const HousePlot* h = &s->houses[i];
        const bool house = !street_lot_vacant(h);
        if (lot->front != FENCE_NONE) {
            Fence fe = fence(lot->front, x0, s->front_z, x1, s->front_z, y, to_street);
            if (lot->front == FENCE_BLOCK)
                fe.height = 0.9f;
            else if (lot->front == FENCE_CHAIN && house)
                fe.height = 1.2f;
            fe.gate = lot->front_gate;
            fe.gate_at = (house ? h->door[0] : 0.5f * (x0 + x1)) - x0;
            fe.torn_at = isnan(lot->torn_x) ? -1.0f : lot->torn_x - x0;
            build(b, &fe);
        }
        if (house) {
            const bool east = x1 - h->x1 >= h->x0 - x0;
            Fence w = fence(lot->yard, x0, s->return_z, h->x0, s->return_z, y, to_street);
            Fence e = fence(lot->yard, h->x1, s->return_z, x1, s->return_z, y, to_street);
            Fence* g = east ? &e : &w;
            g->gate = lot->side_gate;
            g->gate_at = 0.5f * (g->b[0] - g->a[0]);
            build(b, &w);
            build(b, &e);
        }
        Fence back = fence(lot->yard, x0, s->back_z, x1, s->back_z, y, -to_street);
        back.gate = lot->back_gate;
        back.gate_at = lot->back_gate_x - x0;
        back.fallen_at = isnan(lot->fallen_x) ? -1.0f : lot->fallen_x - x0;
        build(b, &back);
    }
}

void fences_build(Kit* kit, unsigned int seed, const StreetPlots* plots, FenceBreaches* breaches) {
    Build b = {.kit = kit, .rng = {seed * 2654435761u + 1335u}, .breaches = breaches};
    breaches->count = 0;
    const Side near = {
        NEAR,         NEAR_LINES,  NEAR_LOTS,    near_lot_line_x, NEAR_END_FENCE_INSET,
        street_level, plots->near, NEAR_FRONT_Z, NEAR_RETURN_Z,   BACK_FENCE_Z};
    const Side far = {FAR,
                      FAR_LINES,
                      TERRACE_LOTS,
                      far_lot_line_x,
                      FAR_END_FENCE_INSET,
                      land_terrace_lot_height,
                      plots->far,
                      FAR_FRONT_Z,
                      FAR_RETURN_Z,
                      FAR_BACK_FENCE_Z};
    side(&b, &near);
    side(&b, &far);
}
