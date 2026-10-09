#include "fridge_map.h"
#include "kitchen.h"
#include "mats.h"
#include "town_map.h"

#define PANEL 0.15f   // a folded panel's side
#define THICK 0.003f  // a panel's thickness: the sheet folded several times over
#define SAG   0.35f   // radians the lower panel hangs out from the door
#define LIFT  0.0004f // a printed face over the body it is printed on
#define INSET 0.0005f // a printed face in from its panel's edges

/*
 * The panel hinged at the cover panel's foot, sagging out into the room: the print on its room
 * side, plain paper on its door side, and the paper's edges round the three sides that are not the
 * hinge, so it has a thickness from any side.
 */
static void hanging_panel(Kit* kit, const KitFrame* f) {
    const float h = 0.5f * PANEL;
    const vec3 down = {0.0f, -cosf(SAG), sinf(SAG)}, out = {0.0f, sinf(SAG), cosf(SAG)};
    // Its corners: bit 0 across (-h, +h), bit 1 along (the hinge, the foot), bit 2 through (the
    // door side, the room side).
    vec3 c[8];
    for (int i = 0; i < 8; i++) {
        const float t = (i & 2) ? PANEL : 0.0f, s = (i & 4) ? 0.5f * THICK : -0.5f * THICK;
        c[i][0] = (i & 1) ? h : -h;
        c[i][1] = -PANEL + t * down[1] + s * out[1];
        c[i][2] = 0.5f * THICK + t * down[2] + s * out[2];
    }
    // The print, from its foot's left corner up to the hinge.
    vec3 corner;
    glm_vec3_copy(c[6], corner);
    glm_vec3_muladds((float*)out, LIFT, corner);
    kit_frame_card(kit, f, MAT_TOWN_MAP, corner, (vec3){PANEL, 0.0f, 0.0f},
                   (vec3){-PANEL * down[0], -PANEL * down[1], -PANEL * down[2]},
                   MAP_ART[MAP_TOWN].folded_inside);
    // Plain paper toward the door, and the edges: either side, and the foot.
    const int faces[4][4] = {{0, 1, 3, 2}, {0, 2, 6, 4}, {1, 3, 7, 5}, {2, 3, 7, 6}};
    const vec3 facing[4] = {{-out[0], -out[1], -out[2]},
                            {-1.0f, 0.0f, 0.0f},
                            {1.0f, 0.0f, 0.0f},
                            {down[0], down[1], down[2]}};
    for (int k = 0; k < 4; k++) {
        vec3 p[4];
        for (int j = 0; j < 4; j++)
            glm_vec3_copy(c[faces[k][j]], p[j]);
        kit_frame_quad(kit, f, MAT_PAPER, (const vec3*)p, facing[k]);
    }
}

/*
 * The map as it hangs, in the frame kitchen_fridge_map_frame gives: the cover panel flat on the
 * door from the magnet at its top edge down, and the next panel hinged at its foot, sagging out
 * into the room with the town's print toward it.
 */
static void hanging(Kit* kit, const KitFrame* f) {
    const float h = 0.5f * PANEL;
    kit_frame_soft_box(kit, f, MAT_PAPER, -h, h, -PANEL, 0.0f, 0.0002f, THICK, 0.001f, 0.0f);
    kit_frame_card_rect(kit, f, MAT_TOWN_MAP, MAP_ART[MAP_TOWN].folded_cover, -h + INSET, h - INSET,
                        -PANEL + INSET, -INSET, THICK + LIFT, 1.0f);
    // The fold between the two panels, rounded.
    kit_frame_pipe(kit, f, MAT_PAPER,
                   (vec3[]){{-h, -PANEL, 0.5f * THICK}, {h, -PANEL, 0.5f * THICK}}, 2, 0.5f * THICK,
                   8);
    hanging_panel(kit, f);
}

void fridge_map_magnet(Kit* kit, const KitFrame* f) {
    // In the cover's top corner, clear of the name, deep enough to stand proud of the map it holds.
    const float a = -0.5f * PANEL + 0.022f, y = -0.017f;
    kit_frame_pipe(kit, f, MAT_CERAMIC, (vec3[]){{a, y, 0.0f}, {a, y, 0.013f}}, 2, 0.013f, 16);
}

void fridge_map_closed(Kit* kit, const KitFrame* f) {
    const float h = 0.5f * PANEL;
    const MapArt* art = &MAP_ART[MAP_TOWN];
    kit_frame_soft_box(kit, f, MAT_PAPER, -h, h, -h, h, -THICK, THICK, 0.0012f, 0.0f);
    kit_frame_card_rect(kit, f, MAT_TOWN_MAP, art->folded_cover, -h + INSET, h - INSET, -h + INSET,
                        h - INSET, THICK + LIFT, 1.0f);
    kit_frame_card_rect(kit, f, MAT_TOWN_MAP, art->folded_inside, -h + INSET, h - INSET, -h + INSET,
                        h - INSET, -THICK - LIFT, -1.0f);
}

void fridge_map_build(FridgeMap* fm, Engine* engine, Scene* scene, bool taken) {
    *fm = (FridgeMap){0};
    KitFrame f;
    kitchen_fridge_map_frame(&f);
    kit_frame_point(&f, 0.0f, -0.5f * PANEL, THICK, fm->at);
    if (taken)
        return;
    Kit kit;
    mats_kit(&kit, engine, scene);
    hanging(&kit, &f);
    fm->node = kit_finish(&kit, "town_map");
    // No capture keeps it: it is taken while the game runs.
    fm->node->capture_hidden = true;
}

void fridge_map_take(FridgeMap* fm) {
    free_node(fm->node);
    fm->node = NULL;
}
