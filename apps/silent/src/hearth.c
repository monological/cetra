#include <math.h>

#include "gothic.h"
#include "hearth.h"
#include "house.h"
#include "layout.h"
#include "mats.h"

/*
 * The hearth's own frame: a along the back wall from its middle, increasing to the right as
 * the room sees it, and d out of the wall's inner face into the room.
 */
static const KitFrame HEARTH = {{HEARTH_X, 0.0f, GREAT_Z1}, GLM_PIf};

#define HEARTH_TOP (FLOOR_Y + 0.06f) // the raised hearth's stone
#define JAMB_W     0.5f
#define MOUTH      (HEARTH_HALF - JAMB_W) // half the firebox's mouth
#define PIER_D     0.72f                  // the jambs' cores; their shafts stand on the fronts
#define DEPTH      0.9f                   // the lintel's projection, and the main shafts' fronts
#define BACK_HALF  0.95f                  // half the firebox's back, its cheeks splaying out
#define PLINTH_H   0.2f
#define ABACUS_H   0.07f
#define LINTEL_Y0  (HEARTH_TOP + 1.75f)
#define LINTEL_Y1  (LINTEL_Y0 + 0.55f)
#define CORNICE_Y  (LINTEL_Y1 + 0.16f) // the cornice's top, where the hood stands

/*
 * The hood slopes back from behind the lower battlements to its own cornice; the breast above
 * it stands BREAST_D out, behind the back truss's timbers, which come nearest this wall at
 * GREAT_Z1 - TRUSS_Z1 - TRUSS_HALF.
 */
#define HOOD_Y      4.75f
#define HOOD_D0     (DEPTH - 0.06f)
#define HOOD_D1     0.55f
#define HOOD_HALF0  1.62f
#define HOOD_HALF1  1.15f
#define HOOD_TOP    (HOOD_Y + 0.17f) // its cornice's top, where the breast stands
#define BREAST_HALF 0.8f
#define BREAST_D    0.45f
#define ARMS_SCALE  1.4f // the carved arms, larger than the atlas's card

#define MERLON_W     0.16f
#define MERLON_H     0.18f
#define MERLON_D     0.12f
#define MERLON_PITCH 0.35f

#define ARCADE      8 // pointed arches carved across the lintel's face
#define ARCADE_HALF 1.6f

// A flat face through four (a, y, d) corners, facing `out`.
static void face(Kit* kit, int mat, const vec3 p[4], const vec3 out) {
    vec3 w[4] = {{0.0f}}, o = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 4; i++)
        kit_frame_point(&HEARTH, p[i][0], p[i][1], p[i][2], w[i]);
    kit_frame_dir(&HEARTH, out[0], out[1], out[2], o);
    kit_quad_facing(kit, mat, w[0], w[1], w[2], w[3], o);
}

/*
 * One of a jamb's shafts, from the plinth to the abacus, turned: a base of two rolls, the
 * shaft, a ring at its neck, and a bell capital flaring out under the abacus. Radii are in the
 * shaft's own, heights in metres.
 */
static void shaft(Kit* kit, float a, float d, float r) {
    const float y0 = HEARTH_TOP + PLINTH_H, h = LINTEL_Y0 - ABACUS_H - y0;
    const vec2 profile[] = {{0.0f, 0.0f},
                            {1.45f * r, 0.0f},
                            {1.45f * r, 0.035f},
                            {1.2f * r, 0.07f},
                            {1.05f * r, 0.09f},
                            {1.25f * r, 0.12f},
                            {1.0f * r, 0.15f},
                            {1.0f * r, h - 0.22f},
                            {1.14f * r, h - 0.205f},
                            {1.14f * r, h - 0.185f},
                            {1.0f * r, h - 0.17f},
                            {1.15f * r, h - 0.12f},
                            {1.4f * r, h - 0.06f},
                            {1.62f * r, h - 0.02f},
                            {1.62f * r, h},
                            {0.0f, h}};
    kit_frame_lathe(kit, &HEARTH, MAT_STONE, a, d, y0, profile, KIT_COUNT(profile), 14);
}

/*
 * A jamb on side s: a square plinth, a core, three shafts clustered on its front -- a main one
 * between two slighter -- an abacus over their capitals, and a quarter-round corbel at the
 * mouth's top corner carrying the lintel's end. One body for all of it.
 */
static void jamb(Kit* kit, float s) {
    const KitFrame* f = &HEARTH;
    kit_frame_box(kit, f, MAT_STONE, s * (MOUTH - 0.05f), s * (HEARTH_HALF + 0.03f), HEARTH_TOP,
                  HEARTH_TOP + PLINTH_H, 0.0f, DEPTH + 0.05f, false);
    kit_frame_box(kit, f, MAT_STONE, s * MOUTH, s * HEARTH_HALF, HEARTH_TOP + PLINTH_H,
                  LINTEL_Y0 - ABACUS_H, 0.0f, PIER_D, false);
    const float mid = s * 0.5f * (MOUTH + HEARTH_HALF);
    shaft(kit, mid, DEPTH - 0.11f, 0.11f);
    shaft(kit, mid - 0.17f, PIER_D + 0.035f, 0.065f);
    shaft(kit, mid + 0.17f, PIER_D + 0.035f, 0.065f);
    kit_frame_box(kit, f, MAT_STONE, s * (MOUTH - 0.04f), s * (HEARTH_HALF + 0.04f),
                  LINTEL_Y0 - ABACUS_H, LINTEL_Y0, 0.0f, DEPTH + 0.08f, false);
    kit_frame_box(kit, f, KIT_COLLIDER_ONLY, s * (MOUTH - 0.05f), s * (HEARTH_HALF + 0.04f),
                  HEARTH_TOP, LINTEL_Y0, 0.0f, DEPTH + 0.08f, true);

    enum { SEG = 8 };
    vec2 corbel[SEG + 2];
    for (int i = 0; i <= SEG; i++) {
        const float t = 0.5f * GLM_PIf * (float)i / SEG;
        glm_vec2_copy((vec2){s * (MOUTH - 0.24f * sinf(t)), LINTEL_Y0 - 0.34f * cosf(t)},
                      corbel[i]);
    }
    glm_vec2_copy((vec2){s * MOUTH, LINTEL_Y0}, corbel[SEG + 1]);
    kit_frame_extrude(kit, &HEARTH, MAT_STONE, corbel, SEG + 2, PIER_D - 0.3f, DEPTH - 0.02f);
}

/*
 * The lintel across the jambs, carved on its face: a bead along its foot, and over a ledge a
 * blind arcade of pointed arches with a boss in every spandrel. Then the cornice, in two steps.
 */
static void lintel(Kit* kit) {
    kit_frame_box(kit, &HEARTH, MAT_STONE, -HEARTH_HALF, HEARTH_HALF, LINTEL_Y0, LINTEL_Y1, 0.0f,
                  DEPTH, true);
    const float y = LINTEL_Y0;
    const vec2 bead[] = {{DEPTH, y},
                         {DEPTH + 0.02f, y + 0.01f},
                         {DEPTH + 0.03f, y + 0.035f},
                         {DEPTH + 0.02f, y + 0.06f},
                         {DEPTH, y + 0.07f}};
    kit_frame_run(kit, &HEARTH, MAT_STONE, bead, KIT_COUNT(bead), -HEARTH_HALF, HEARTH_HALF);
    kit_frame_box(kit, &HEARTH, MAT_STONE, -ARCADE_HALF - 0.02f, ARCADE_HALF + 0.02f, y + 0.095f,
                  y + 0.12f, DEPTH, DEPTH + 0.03f, false);

    const float span = 2.0f * ARCADE_HALF / ARCADE;
    for (int i = 0; i < ARCADE; i++) {
        const float a0 = -ARCADE_HALF + span * (float)i;
        const KitOpening arch = {a0 + 0.06f, a0 + span - 0.06f, y + 0.12f,
                                 y + 0.28f,  KIT_ARCH_POINTED,  0.2f};
        kit_frame_surround(kit, &HEARTH, MAT_STONE, &arch, 0.035f, false, DEPTH, DEPTH + 0.025f);
    }
    const vec2 boss[] = {
        {0.0f, 0.0f}, {0.038f, 0.0f}, {0.034f, 0.014f}, {0.016f, 0.024f}, {0.0f, 0.027f}};
    for (int i = 0; i <= ARCADE; i++)
        kit_frame_lathe_on(kit, &HEARTH, MAT_STONE,
                           (vec3){-ARCADE_HALF + span * (float)i, y + 0.47f, DEPTH},
                           (vec3){0.0f, 0.0f, 1.0f}, boss, KIT_COUNT(boss), 10);

    kit_frame_box(kit, &HEARTH, MAT_STONE, -HEARTH_HALF - 0.03f, HEARTH_HALF + 0.03f, LINTEL_Y1,
                  LINTEL_Y1 + 0.05f, 0.0f, DEPTH + 0.03f, false);
    kit_frame_box(kit, &HEARTH, MAT_STONE, -HEARTH_HALF - 0.08f, HEARTH_HALF + 0.08f,
                  LINTEL_Y1 + 0.05f, CORNICE_Y, 0.0f, DEPTH + 0.08f, false);
}

/*
 * Merlons along the front and both returns of a cornice whose top is at y, `half` either side
 * of the middle and `front` out from the wall: a castle's battlements, in miniature.
 */
static void battlements(Kit* kit, float y, float half, float front) {
    const int n = (int)roundf((2.0f * half - MERLON_W) / MERLON_PITCH) + 1;
    const float pitch = (2.0f * half - MERLON_W) / (float)(n - 1);
    for (int i = 0; i < n; i++) {
        const float a = -half + 0.5f * MERLON_W + pitch * (float)i;
        kit_frame_box(kit, &HEARTH, MAT_STONE, a - 0.5f * MERLON_W, a + 0.5f * MERLON_W, y,
                      y + MERLON_H, front - MERLON_D, front, false);
    }
    for (int s = -1; s <= 1; s += 2)
        for (float d = front - 0.5f * MERLON_W - pitch; d - 0.5f * MERLON_W > 0.05f; d -= pitch)
            kit_frame_box(kit, &HEARTH, MAT_STONE, (float)s * (half - MERLON_D), (float)s * half, y,
                          y + MERLON_H, d - 0.5f * MERLON_W, d + 0.5f * MERLON_W, false);
}

// The point on the hood's face at a, `s` up its slope from the cornice, `lift` off it.
static void on_hood(float a, float s, float lift, vec3 out) {
    const float rise = HOOD_Y - CORNICE_Y, back = HOOD_D0 - HOOD_D1;
    const float len = sqrtf(rise * rise + back * back);
    glm_vec3_copy((vec3){a, CORNICE_Y + (s * rise + lift * back) / len,
                         HOOD_D0 + (lift * rise - s * back) / len},
                  out);
}

/*
 * The hood: a front and two sides leaning back from the lower cornice to its own, a roll down
 * each of its front corners, the arms carved halfway up its face in a panel framed by a roll,
 * and its cornice and battlements; then the breast, from there to the roof's underside, cut to
 * the slope. Its back is the wall.
 */
static void hood(Kit* kit) {
    const float y0 = CORNICE_Y, y1 = HOOD_Y;
    const vec3 front[4] = {{-HOOD_HALF0, y0, HOOD_D0},
                           {HOOD_HALF0, y0, HOOD_D0},
                           {HOOD_HALF1, y1, HOOD_D1},
                           {-HOOD_HALF1, y1, HOOD_D1}};
    const float rise = y1 - y0, back = HOOD_D0 - HOOD_D1, len = sqrtf(rise * rise + back * back);
    face(kit, MAT_STONE, front, (vec3){0.0f, back, rise});
    for (int s = -1; s <= 1; s += 2) {
        const float sf = (float)s;
        const vec3 side[4] = {{sf * HOOD_HALF0, y0, 0.0f},
                              {sf * HOOD_HALF0, y0, HOOD_D0},
                              {sf * HOOD_HALF1, y1, HOOD_D1},
                              {sf * HOOD_HALF1, y1, 0.0f}};
        face(kit, MAT_STONE, side, (vec3){sf * rise, HOOD_HALF0 - HOOD_HALF1, 0.0f});
        kit_frame_pipe(kit, &HEARTH, MAT_STONE,
                       (vec3[]){{sf * HOOD_HALF0, y0, HOOD_D0}, {sf * HOOD_HALF1, y1, HOOD_D1}}, 2,
                       0.045f, 10);
    }

    // The arms, lying on the face and lifted off it by a few millimetres, and the roll round
    // them, one straight length a side.
    const GothicSpec* arms = &GOTHICS[GOTHIC_COAT_OF_ARMS];
    const float w = ARMS_SCALE * arms->size[0], h = ARMS_SCALE * arms->size[1];
    const float s0 = 0.5f * (len - h);
    vec3 corner = {0.0f, 0.0f, 0.0f};
    on_hood(-0.5f * w, s0, 0.004f, corner);
    kit_frame_card(kit, &HEARTH, MAT_CARVED, corner, (vec3){w, 0.0f, 0.0f},
                   (vec3){0.0f, rise / len * h, -back / len * h}, arms->uv);
    const float m = 0.035f, r = 0.03f;
    const float ring[5][2] = {{-0.5f * w - m, s0 - m},
                              {0.5f * w + m, s0 - m},
                              {0.5f * w + m, s0 + h + m},
                              {-0.5f * w - m, s0 + h + m},
                              {-0.5f * w - m, s0 - m}};
    for (int i = 0; i < 4; i++) {
        vec3 run[2] = {{0.0f}};
        on_hood(ring[i][0], ring[i][1], 0.5f * r, run[0]);
        on_hood(ring[i + 1][0], ring[i + 1][1], 0.5f * r, run[1]);
        kit_frame_pipe(kit, &HEARTH, MAT_STONE, run, 2, r, 10);
    }

    kit_frame_box(kit, &HEARTH, MAT_STONE, -HOOD_HALF1 - 0.05f, HOOD_HALF1 + 0.05f, y1, y1 + 0.08f,
                  0.0f, HOOD_D1 + 0.05f, false);
    kit_frame_box(kit, &HEARTH, MAT_STONE, -HOOD_HALF1 - 0.1f, HOOD_HALF1 + 0.1f, y1 + 0.08f,
                  HOOD_TOP, 0.0f, HOOD_D1 + 0.1f, false);
    battlements(kit, HOOD_TOP, HOOD_HALF1 + 0.1f, HOOD_D1 + 0.1f);

    // The breast's top follows the roof's underside, over the ridge where it crosses it. In
    // this frame a runs against the world's x.
    const float ridge_a = HEARTH_X;
    vec2 breast[5];
    int n = 0;
    glm_vec2_copy((vec2){-BREAST_HALF, HOOD_TOP}, breast[n++]);
    glm_vec2_copy((vec2){BREAST_HALF, HOOD_TOP}, breast[n++]);
    glm_vec2_copy((vec2){BREAST_HALF, house_roof_under_y(HEARTH_X - BREAST_HALF)}, breast[n++]);
    if (fabsf(ridge_a) < BREAST_HALF)
        glm_vec2_copy((vec2){ridge_a, house_roof_under_y(0.0f)}, breast[n++]);
    glm_vec2_copy((vec2){-BREAST_HALF, house_roof_under_y(HEARTH_X + BREAST_HALF)}, breast[n++]);
    kit_frame_extrude(kit, &HEARTH, MAT_STONE, breast, n, 0.0f, BREAST_D);
}

/*
 * The firebox: its cheeks splaying in from the mouth to the back, which with them, the floor
 * and the lintel's underside is black with soot, and the cast-iron fireback standing against
 * the back.
 */
static void firebox(Kit* kit) {
    const float front = PIER_D - 0.02f, back = 0.04f;
    kit_frame_box(kit, &HEARTH, MAT_SOOT, -BACK_HALF, BACK_HALF, HEARTH_TOP, LINTEL_Y0, 0.0f, back,
                  false);
    for (int s = -1; s <= 1; s += 2) {
        const float sf = (float)s;
        const vec3 cheek[4] = {{sf * MOUTH, HEARTH_TOP, front},
                               {sf * BACK_HALF, HEARTH_TOP, back},
                               {sf * BACK_HALF, LINTEL_Y0, back},
                               {sf * MOUTH, LINTEL_Y0, front}};
        face(kit, MAT_SOOT, cheek, (vec3){-sf * (front - back), 0.0f, MOUTH - BACK_HALF});
    }
    const float floor_y = HEARTH_TOP + 0.002f, under = LINTEL_Y0 - 0.002f;
    const vec3 floor[4] = {{-MOUTH, floor_y, front},
                           {MOUTH, floor_y, front},
                           {BACK_HALF, floor_y, back},
                           {-BACK_HALF, floor_y, back}};
    face(kit, MAT_SOOT, floor, (vec3){0.0f, 1.0f, 0.0f});
    const vec3 soffit[4] = {
        {-MOUTH, under, front}, {MOUTH, under, front}, {MOUTH, under, back}, {-MOUTH, under, back}};
    face(kit, MAT_SOOT, soffit, (vec3){0.0f, -1.0f, 0.0f});

    const GothicSpec* plate = &GOTHICS[GOTHIC_FIREBACK];
    const float w = plate->size[0], h = plate->size[1];
    kit_frame_box(kit, &HEARTH, MAT_IRON, -0.5f * w - 0.01f, 0.5f * w + 0.01f, HEARTH_TOP,
                  HEARTH_TOP + h + 0.01f, back, back + 0.015f, false);
    kit_frame_card_rect(kit, &HEARTH, MAT_CARVED, plate->uv, -0.5f * w, 0.5f * w,
                        HEARTH_TOP + 0.005f, HEARTH_TOP + 0.005f + h, back + 0.016f, 1.0f);
}

void hearth_build(Kit* kit) {
    kit_frame_box(kit, &HEARTH, MAT_STONE, -HEARTH_HALF - 0.1f, HEARTH_HALF + 0.1f, FLOOR_Y,
                  HEARTH_TOP, 0.0f, HEARTH_FRONT, true);
    jamb(kit, -1.0f);
    jamb(kit, 1.0f);
    lintel(kit);
    battlements(kit, CORNICE_Y, HEARTH_HALF + 0.08f, DEPTH + 0.08f);
    hood(kit);
    firebox(kit);
}
