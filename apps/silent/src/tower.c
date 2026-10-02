#include <math.h>

#include "layout.h"
#include "mats.h"
#include "tower.h"

/*
 * The faces are numbered by the way they look: face k's outward normal points 45 k degrees
 * round from +x toward +z, so 0 looks east, 2 into the house (+z), 4 west and 6 at the
 * street. Each is built in a frame whose a runs along it and whose d points out of the tower,
 * centred on the face: a = +TOWER_HALF is its corner clockwise of it seen from above.
 */
#define FACES 8

// Past each corner, so two faces' outer skins meet at 135 degrees with no notch between them:
// half a wall's thickness times tan 22.5.
#define CORNER_LAP (0.5f * EXT_WALL * 0.41421356f)

// The spire's eave, out past the walls' outer faces.
#define SPIRE_EAVE 0.35f

static KitFrame face_at(int k) {
    const float phi = 0.25f * GLM_PIf * (float)k, nx = cosf(phi), nz = sinf(phi);
    return (KitFrame){{TOWER_X + TOWER_APOTHEM * nx, 0.0f, TOWER_Z + TOWER_APOTHEM * nz},
                      atan2f(nx, nz)};
}

void tower_octagon(float apothem, vec2 out[8]) {
    const float r = apothem / cosf(GLM_PIf / 8.0f);
    for (int k = 0; k < FACES; k++) {
        const float t = GLM_PIf / 8.0f + 0.25f * GLM_PIf * (float)k;
        out[k][0] = TOWER_X + r * cosf(t);
        out[k][1] = TOWER_Z + r * sinf(t);
    }
}

/*
 * Which of each face is outside the house, along a: the east and north faces are cut where the
 * house's front and west walls meet their middles, and the north-east face stands inside it.
 * Above the upper ceiling the inner part closes the study's tall bay off from the attic.
 */
typedef struct FaceSpan {
    float out0, out1; // the outer part; equal when there is none
    float in0, in1;   // the part inside the house
} FaceSpan;

static FaceSpan face_span(int k) {
    const float h = TOWER_HALF;
    switch (k) {
        case 0:
            return (FaceSpan){0.0f, h, -h, 0.0f};
        case 1:
            return (FaceSpan){0.0f, 0.0f, -h, h};
        case 2:
            return (FaceSpan){-h, 0.0f, 0.0f, h};
        default:
            return (FaceSpan){-h, h, 0.0f, 0.0f};
    }
}

// The lancets: a plain one into the parlour on the faces toward the street, and a tall one
// into the study on every face that looks out, which takes the stained glass.
static bool parlour_window(int k) {
    return k >= 4;
}

static bool study_window(int k) {
    return k >= 3;
}

static const KitOpening PARLOUR_LANCET = {
    -0.3f, 0.3f, FLOOR_Y + 0.9f, FLOOR_Y + 2.0f, KIT_ARCH_POINTED, 0.55f};
static const KitOpening STUDY_LANCET = {-0.35f,           0.35f, FLOOR2_Y + 0.8f, FLOOR2_Y + 2.6f,
                                        KIT_ARCH_POINTED, 0.7f};

// A face's wall from `a0` to `a1`, from y0 to y1, lapped past any end that is a corner.
static void face_wall(Kit* kit, const KitFrame* f, float a0, float a1, float y0, float y1,
                      const KitOpening* openings, int count) {
    const float h = TOWER_HALF;
    KitWall w = {.at = 0.0f,
                 .from = a0 <= -h + 1e-4f ? a0 - CORNER_LAP : a0,
                 .to = a1 >= h - 1e-4f ? a1 + CORNER_LAP : a1,
                 .y0 = y0,
                 .y1 = y1,
                 .thick = EXT_WALL,
                 .inner = -1,
                 .mat_inner = MAT_PLASTER,
                 .mat_outer = MAT_SIDING,
                 .opening_count = count};
    for (int i = 0; i < count; i++)
        w.openings[i] = openings[i];
    kit_frame_wall(kit, f, &w);
}

static void walls(Kit* kit) {
    for (int k = 0; k < FACES; k++) {
        const KitFrame f = face_at(k);
        const FaceSpan s = face_span(k);
        if (s.out1 > s.out0) {
            KitOpening open[2];
            int n = 0;
            if (parlour_window(k))
                open[n++] = PARLOUR_LANCET;
            if (study_window(k))
                open[n++] = STUDY_LANCET;
            face_wall(kit, &f, s.out0, s.out1, 0.0f, TOWER_TOP, open, n);
            for (int i = 0; i < n; i++)
                kit_frame_pane(kit, &f,
                               open[i].bottom < FLOOR2_Y ? MAT_DARK_GLASS : MAT_WINDOW_GLASS,
                               &open[i], 0.0f, EXT_WALL);
        }
        if (s.in1 > s.in0)
            face_wall(kit, &f, s.in0, s.in1, CEIL2_Y, TOWER_TOP, NULL, 0);
    }
}

// The parlour's floor, its ceiling and the study's floor over it, and the study's own high
// ceiling, each across the whole octagon: the house's slabs leave the tower out.
static void floors(Kit* kit) {
    vec2 oct[FACES];
    tower_octagon(TOWER_APOTHEM, oct);
    kit_slab(kit, MAT_WOOD_FLOOR, oct, FACES, 0.0f, FLOOR_Y, true);
    kit_slab(kit, MAT_CEILING, oct, FACES, CEIL_Y, CEIL_Y + SLAB, false);
    kit_slab(kit, MAT_WOOD_FLOOR, oct, FACES, CEIL_Y + SLAB, FLOOR2_Y, true);
    kit_slab(kit, MAT_CEILING, oct, FACES, TOWER_CEIL_Y, TOWER_CEIL_Y + SLAB, false);
}

// The spire: an eave all round, out past the walls, and eight slopes up to its point.
static void spire(Kit* kit) {
    const float out = TOWER_APOTHEM + 0.5f * EXT_WALL + SPIRE_EAVE;
    const float base = TOWER_TOP + 0.12f;
    vec2 eave[FACES];
    tower_octagon(out, eave);
    kit_slab(kit, MAT_TRIM, eave, FACES, TOWER_TOP, base, false);
    const vec3 apex = {TOWER_X, TOWER_SPIRE_Y, TOWER_Z};
    for (int k = 0; k < FACES; k++) {
        const float* p = eave[k];
        const float* q = eave[(k + 1) % FACES];
        const vec3 a = {p[0], base, p[1]}, b = {q[0], base, q[1]};
        // Corners k and k + 1 bound the face looking 45 (k + 1) degrees round.
        const float phi = 0.25f * GLM_PIf * (float)(k + 1);
        const vec3 outward = {cosf(phi), 0.5f, sinf(phi)};
        kit_tri_facing(kit, MAT_ROOF, a, b, apex, outward);
    }
}

void tower_build(Kit* kit) {
    walls(kit);
    floors(kit);
    spire(kit);
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
    tower_octagon(apothem, oct);
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

float tower_outside_distance(const vec3 p) {
    const float outer = TOWER_APOTHEM + 0.5f * EXT_WALL;
    float d = -1e9f;
    for (int k = 0; k < FACES; k++) {
        const float phi = 0.25f * GLM_PIf * (float)k;
        d = fmaxf(d, (p[0] - TOWER_X) * cosf(phi) + (p[2] - TOWER_Z) * sinf(phi) - outer);
    }
    return d;
}
