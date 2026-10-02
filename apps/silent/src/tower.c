#include <math.h>

#include "gargoyle.h"
#include "gothic.h"
#include "layout.h"
#include "mats.h"
#include "ornament.h"
#include "tower.h"

/*
 * The faces are numbered by the way they look: face k's outward normal points 45 k degrees
 * round from +x toward +z, so 0 looks east, 2 into the house (+z), 4 west and 6 at the
 * street. Each is built in a frame whose a runs along it and whose d points out of the tower,
 * centred on the face: a = +TOWER_HALF is its corner clockwise of it seen from above.
 */
#define FACES 8

// The spire's eave, out past the walls' outer faces.
#define SPIRE_EAVE 0.35f

// The cornice under the eave, and the brackets it stands on.
#define CORNICE_H    0.36f
#define CORBEL_PITCH 0.32f

// The gargoyles crouch on the outer corners between the study's lancets and the cornice, their
// folded wings clear of it and their jaws over the yard, each pouring what the spire sheds: a
// stream, at the reference rain.
#define GARGOYLE_Y     (TOWER_TOP - CORNICE_H - 1.0f)
#define GARGOYLE_DRIPS 25.0f

/*
 * What each face is, in half faces along its a from -1 to +1: where it is outside the house,
 * where inside, and what it holds. The east and north faces are cut where the house's front and
 * west walls meet their middles, and the north-east face stands inside the house; above the
 * upper ceiling the inner parts close the study's tall bay off from the attic. A plain lancet
 * lights the parlour on the faces toward the street, and a tall one of stained glass the study
 * on every face that looks out -- one design to a face round from the north-west, the rose on
 * the one looking at the street's corner.
 */
typedef struct Face {
    signed char out0, out1; // the outer part; equal when there is none
    signed char in0, in1;   // the part inside the house
    bool parlour;
    GothicId glass; // the study's lancet; GOTHIC_COUNT for none
} Face;

static const Face FACE[FACES] = {
    {0, 1, -1, 0, false, GOTHIC_COUNT},     {0, 0, -1, 1, false, GOTHIC_COUNT},
    {-1, 0, 0, 1, false, GOTHIC_COUNT},     {-1, 1, 0, 0, false, GOTHIC_GLASS_STAR},
    {-1, 1, 0, 0, true, GOTHIC_GLASS_LILY}, {-1, 1, 0, 0, true, GOTHIC_GLASS_ROSE},
    {-1, 1, 0, 0, true, GOTHIC_GLASS_LILY}, {-1, 1, 0, 0, true, GOTHIC_GLASS_STAR},
};

static const KitOpening PARLOUR_LANCET = {
    -0.3f, 0.3f, FLOOR_Y + 0.9f, FLOOR_Y + 2.0f, KIT_ARCH_POINTED, 0.55f};
static const KitOpening STUDY_LANCET = {-0.35f,           0.35f, FLOOR2_Y + 0.8f, FLOOR2_Y + 2.6f,
                                        KIT_ARCH_POINTED, 0.7f};

// How far past a corner a face's wall, or its ornament `depth` proud of its outer skin, runs
// so the two faces' meet at 135 degrees with no notch.
static float corner_lap(float depth) {
    return (0.5f * EXT_WALL + depth) * TOWER_TAN;
}

// Half faces h0..h1 along a, lapped past whichever ends are corners.
static void lapped(int h0, int h1, float depth, float* from, float* to) {
    *from = (float)h0 * TOWER_HALF - (h0 == -1 ? corner_lap(depth) : 0.0f);
    *to = (float)h1 * TOWER_HALF + (h1 == 1 ? corner_lap(depth) : 0.0f);
}

// Face k's outward normal in plan, 45 k degrees round from +x toward +z.
static void face_normal(int k, float* nx, float* nz) {
    const float phi = 0.25f * GLM_PIf * (float)k;
    *nx = cosf(phi);
    *nz = sinf(phi);
}

static KitFrame face_at(int k) {
    float nx = 0.0f, nz = 0.0f;
    face_normal(k, &nx, &nz);
    return (KitFrame){{TOWER_X + TOWER_APOTHEM * nx, 0.0f, TOWER_Z + TOWER_APOTHEM * nz},
                      atan2f(nx, nz)};
}

// The octagon's corners in (x, z) for faces `apothem` from its middle: corner k between face k
// and face k + 1, counter-clockwise from the east face's north end.
static void octagon(float apothem, vec2 out[FACES]) {
    const float r = apothem / cosf(GLM_PIf / 8.0f);
    for (int k = 0; k < FACES; k++) {
        const float t = GLM_PIf / 8.0f + 0.25f * GLM_PIf * (float)k;
        out[k][0] = TOWER_X + r * cosf(t);
        out[k][1] = TOWER_Z + r * sinf(t);
    }
}

// A face's wall over half faces h0..h1, from y0 to y1, lapped past any end that is a corner.
static KitWall face_wall(int h0, int h1, float y0, float y1, const KitOpening* openings,
                         int count) {
    KitWall w = {.at = 0.0f,
                 .y0 = y0,
                 .y1 = y1,
                 .thick = EXT_WALL,
                 .inner = -1,
                 .mat_inner = MAT_PLASTER,
                 .mat_outer = MAT_SIDING_DARK,
                 .opening_count = count};
    lapped(h0, h1, 0.0f, &w.from, &w.to);
    for (int i = 0; i < count; i++)
        w.openings[i] = openings[i];
    return w;
}

/*
 * A face's dressing outside over half faces h0..h1: the base, boards and battens up to the
 * cornice, its windows cased and hooded, a string course at the study's floor, and the cornice
 * on its brackets.
 */
static void dress(Kit* kit, const KitFrame* f, const KitWall* w, int h0, int h1) {
    const Facade s = facade_of_frame(f, w);
    const float a0 = (float)h0 * TOWER_HALF, a1 = (float)h1 * TOWER_HALF;
    float from = 0.0f, to = 0.0f;
    lapped(h0, h1, 0.08f, &from, &to);
    ornament_base(kit, &s, from, to, NULL, 0);
    ornament_battens(kit, &s, a0, a1, BOARDS_Y, TOWER_TOP - CORNICE_H, w->openings,
                     w->opening_count);
    for (int i = 0; i < w->opening_count; i++)
        ornament_window(kit, &s, &w->openings[i]);
    const float sf = s.face, y = FLOOR2_Y - 0.1f;
    lapped(h0, h1, 0.06f, &from, &to);
    const vec2 course[4] = {
        {sf, y}, {sf + 0.06f, y + 0.03f}, {sf + 0.06f, y + 0.09f}, {sf, y + 0.12f}};
    kit_frame_run(kit, f, MAT_SIDING_DARK, course, 4, from, to);
    const float cy = TOWER_TOP - CORNICE_H;
    lapped(a0, a1, 0.14f, &from, &to);
    const vec2 cornice[5] = {{sf, cy + 0.16f},
                             {sf + 0.1f, cy + 0.2f},
                             {sf + 0.14f, cy + 0.28f},
                             {sf + 0.14f, TOWER_TOP},
                             {sf, TOWER_TOP}};
    kit_frame_run(kit, f, MAT_SIDING_DARK, cornice, 5, from, to);
    const int brackets = (int)floorf((a1 - a0) / CORBEL_PITCH);
    for (int i = 0; i < brackets; i++) {
        const float a = a0 + (a1 - a0) * ((float)i + 0.5f) / (float)brackets;
        kit_frame_box(kit, f, MAT_SIDING_DARK, a - 0.035f, a + 0.035f, cy, cy + 0.17f, sf,
                      sf + 0.09f, false);
    }
}

// A study lancet's stained glass: the picture twice, facing out and facing in, so from inside
// it reads mirrored as glass does.
static void stained_pane(Kit* kit, const KitFrame* f, const KitOpening* o, GothicId design) {
    vec2 outline[KIT_OPENING_POINTS];
    const int n = kit_opening_outline(o, outline);
    kit_frame_card_polygon(kit, f, MAT_STAINED, outline, n, 0.003f, GOTHICS[design].uv, false);
    kit_frame_card_polygon(kit, f, MAT_STAINED, outline, n, -0.003f, GOTHICS[design].uv, true);
    kit_frame_plug(kit, f, o, 0.0f, EXT_WALL);
}

static void walls(Kit* kit) {
    for (int k = 0; k < FACES; k++) {
        const Face* face = &FACE[k];
        const KitFrame f = face_at(k);
        if (face->out1 > face->out0) {
            KitOpening open[2];
            int n = 0;
            if (face->parlour)
                open[n++] = PARLOUR_LANCET;
            if (face->glass != GOTHIC_COUNT)
                open[n++] = STUDY_LANCET;
            const KitWall w = face_wall(face->out0, face->out1, 0.0f, TOWER_TOP, open, n);
            kit_frame_wall(kit, &f, &w);
            if (face->parlour)
                kit_frame_pane(kit, &f, MAT_DARK_GLASS, &PARLOUR_LANCET, 0.0f, EXT_WALL);
            if (face->glass != GOTHIC_COUNT)
                stained_pane(kit, &f, &STUDY_LANCET, face->glass);
            dress(kit, &f, &w, face->out0, face->out1);
        }
        // Above the upper ceiling, standing on it rather than level with its underside.
        if (face->in1 > face->in0) {
            const KitWall w = face_wall(face->in0, face->in1, CEIL2_Y + SLAB, TOWER_TOP, NULL, 0);
            kit_frame_wall(kit, &f, &w);
        }
    }
}

// On the outer corners, between two faces wholly outside the house, under the cornice, each
// with its drip.
static void gargoyles(Kit* kit) {
    vec2 corner[FACES];
    octagon(TOWER_OUTER, corner);
    for (int k = 0; k + 1 < FACES; k++) {
        const Face* a = &FACE[k];
        const Face* b = &FACE[k + 1];
        if (a->out0 != -1 || a->out1 != 1 || b->out0 != -1 || b->out1 != 1)
            continue;
        const float nx = corner[k][0] - TOWER_X, nz = corner[k][1] - TOWER_Z;
        const KitFrame f = {{corner[k][0], GARGOYLE_Y, corner[k][1]}, atan2f(nx, nz)};
        vec3 mouth = {0.0f, 0.0f, 0.0f};
        gargoyle_build(kit, &f, mouth);
        kit_drip(kit, &KIT_WORLD, mouth, mouth, GARGOYLE_DRIPS, 0.0f);
    }
}

// The parlour's floor, its ceiling and the study's floor over it, and the study's own high
// ceiling, each across the whole octagon: the house's slabs leave the tower out.
static void floors(Kit* kit) {
    vec2 oct[FACES];
    octagon(TOWER_APOTHEM, oct);
    kit_slab(kit, MAT_WOOD_FLOOR, oct, FACES, 0.0f, FLOOR_Y, true);
    kit_slab(kit, MAT_CEILING, oct, FACES, CEIL_Y, CEIL_Y + SLAB, false);
    kit_slab(kit, MAT_WOOD_FLOOR, oct, FACES, CEIL_Y + SLAB, FLOOR2_Y, true);
    kit_slab(kit, MAT_CEILING, oct, FACES, TOWER_CEIL_Y, TOWER_CEIL_Y + SLAB, false);
}

// The spire: an eave all round, out past the walls, and eight slopes up to its point.
static void spire(Kit* kit) {
    const float base = TOWER_TOP + 0.12f;
    vec2 eave[FACES];
    octagon(TOWER_OUTER + SPIRE_EAVE, eave);
    kit_slab(kit, MAT_SIDING_DARK, eave, FACES, TOWER_TOP, base, false);
    const vec3 apex = {TOWER_X, TOWER_SPIRE_Y, TOWER_Z};
    for (int k = 0; k < FACES; k++) {
        const float* p = eave[k];
        const float* q = eave[(k + 1) % FACES];
        const vec3 a = {p[0], base, p[1]}, b = {q[0], base, q[1]};
        // Corners k and k + 1 bound face k + 1.
        float nx = 0.0f, nz = 0.0f;
        face_normal((k + 1) % FACES, &nx, &nz);
        const vec3 outward = {nx, 0.5f, nz};
        kit_tri_facing(kit, MAT_SLATE, a, b, apex, outward);
    }
    ornament_finial(kit, &KIT_WORLD, MAT_IRON, TOWER_X, TOWER_SPIRE_Y - 0.1f, TOWER_Z, 1.3f, 0.0f);
}

void tower_build(Kit* kit) {
    walls(kit);
    floors(kit);
    spire(kit);
    gargoyles(kit);
}

// Where the octagon's outline crosses the line at `value` along axis `ax` (0 = x, 1 = z),
// taking the crossing furthest along the other axis. False if it never does.
static bool crossing(const vec2 oct[FACES], int ax, float value, vec2 out) {
    const int other = 1 - ax;
    bool found = false;
    for (int k = 0; k < FACES; k++) {
        const float* p = oct[k];
        const float* q = oct[(k + 1) % FACES];
        if ((p[ax] - value) * (q[ax] - value) > 0.0f || p[ax] == q[ax])
            continue;
        const float t = (value - p[ax]) / (q[ax] - p[ax]);
        vec2 c = {0.0f, 0.0f};
        c[ax] = value;
        c[other] = p[other] + t * (q[other] - p[other]);
        if (!found || c[other] > out[other]) {
            glm_vec2_copy(c, out);
            found = true;
        }
    }
    return found;
}

int tower_notch(float x0, float z0, float x1, float z1, float apothem, vec2* out) {
    vec2 oct[FACES], bottom = {0.0f, 0.0f}, left = {0.0f, 0.0f};
    octagon(apothem, oct);
    if (!crossing(oct, 1, z0, bottom) || !crossing(oct, 0, x0, left))
        return 0;
    int n = 0;
    glm_vec2_copy(bottom, out[n++]);
    glm_vec2_copy((vec2){x1, z0}, out[n++]);
    glm_vec2_copy((vec2){x1, z1}, out[n++]);
    glm_vec2_copy((vec2){x0, z1}, out[n++]);
    glm_vec2_copy(left, out[n++]);
    // Then back round the tower's outline to where it began: the corners inside the
    // rectangle, clockwise seen from above, which is falling angle about the middle.
    int inside[FACES], count = 0;
    for (int k = 0; k < FACES; k++)
        if (oct[k][0] > x0 && oct[k][0] < x1 && oct[k][1] > z0 && oct[k][1] < z1)
            inside[count++] = k;
    for (int i = 1; i < count; i++)
        for (int j = i; j > 0; j--) {
            const float* a = oct[inside[j - 1]];
            const float* b = oct[inside[j]];
            if (atan2f(a[1] - TOWER_Z, a[0] - TOWER_X) >= atan2f(b[1] - TOWER_Z, b[0] - TOWER_X))
                break;
            const int t = inside[j];
            inside[j] = inside[j - 1];
            inside[j - 1] = t;
        }
    for (int i = 0; i < count; i++)
        glm_vec2_copy(oct[inside[i]], out[n++]);
    return n;
}

float tower_wall_distance(const vec3 p) {
    vec2 c[FACES];
    octagon(TOWER_APOTHEM, c);
    float d = 1e9f;
    for (int k = 0; k < FACES; k++)
        d = fminf(d, kit_plan_distance(p, c[(k + FACES - 1) % FACES], c[k]));
    return d - 0.5f * EXT_WALL;
}

float tower_outside_distance(const vec3 p) {
    float d = -1e9f;
    for (int k = 0; k < FACES; k++) {
        float nx = 0.0f, nz = 0.0f;
        face_normal(k, &nx, &nz);
        d = fmaxf(d, (p[0] - TOWER_X) * nx + (p[2] - TOWER_Z) * nz - TOWER_OUTER);
    }
    return d;
}
