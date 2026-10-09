#include "fridge_map.h"
#include "kitchen.h"
#include "mats.h"
#include "town_map.h"

#define PANEL         0.15f   // a folded panel's side
#define THICK         0.003f  // a panel's thickness: the sheet folded several times over
#define SAG           0.35f   // radians the lower panel hangs out from the door
#define LIFT          0.0004f // a printed face over the body it is printed on
#define PAPER_UV_SPAN 0.375f  // a panel of MAT_PAPER's picture: its side over the paper's repeat

static const float PAPER_UV[4] = {0.0f, 0.0f, PAPER_UV_SPAN, PAPER_UV_SPAN};

/*
 * The panel hinged at the cover panel's foot, from `hinge` along `down` for its length, `out` its
 * printed face's normal: the print on that face, plain paper on the other, and the paper's edges
 * round the three sides that are not the hinge, so it has a thickness from any side.
 */
static void hanging_panel(Kit* kit, const KitFrame* f, const vec3 hinge, const vec3 down,
                          const vec3 out) {
    const MapArt* art = &MAP_ART[MAP_TOWN];
    vec3 front, back, foot;
    glm_vec3_copy((float*)hinge, front);
    glm_vec3_muladds((float*)out, 0.5f * THICK + LIFT, front);
    glm_vec3_copy((float*)hinge, back);
    glm_vec3_muladds((float*)out, -0.5f * THICK, back);
    const vec3 along = {PANEL * down[0], PANEL * down[1], PANEL * down[2]};
    const vec3 up = {-along[0], -along[1], -along[2]};
    const vec3 depth = {THICK * out[0], THICK * out[1], THICK * out[2]};
    const vec3 across = {PANEL, 0.0f, 0.0f}, reverse = {-PANEL, 0.0f, 0.0f};

    // The print toward the room, its top at the hinge.
    glm_vec3_add(front, (float*)along, foot);
    kit_frame_card(kit, f, MAT_TOWN_MAP, (vec3){foot[0] - 0.5f * PANEL, foot[1], foot[2]}, across,
                   up, art->folded_inside);
    // Plain paper toward the door.
    glm_vec3_add(back, (float*)along, foot);
    kit_frame_card(kit, f, MAT_PAPER, (vec3){foot[0] + 0.5f * PANEL, foot[1], foot[2]}, reverse, up,
                   PAPER_UV);
    // The edges: either side, and the foot.
    kit_frame_card(kit, f, MAT_PAPER, (vec3){back[0] - 0.5f * PANEL, back[1], back[2]}, along,
                   depth, PAPER_UV);
    kit_frame_card(kit, f, MAT_PAPER, (vec3){back[0] + 0.5f * PANEL, back[1], back[2]}, depth,
                   along, PAPER_UV);
    kit_frame_card(kit, f, MAT_PAPER, (vec3){foot[0] - 0.5f * PANEL, foot[1], foot[2]}, across,
                   depth, PAPER_UV);
}

/*
 * The map as it hangs, in the frame kitchen_fridge_map_frame gives: the cover panel flat on the
 * door from the magnet at its top edge down, and the next panel hinged at its foot, sagging out
 * into the room with the town's print toward it.
 */
static void hanging(Kit* kit, const KitFrame* f) {
    const MapArt* art = &MAP_ART[MAP_TOWN];
    const float h = 0.5f * PANEL;
    kit_frame_soft_box(kit, f, MAT_PAPER, -h, h, -PANEL, 0.0f, 0.0002f, THICK, 0.001f, 0.0f);
    kit_frame_card_rect(kit, f, MAT_TOWN_MAP, art->folded_cover, -h + 0.0005f, h - 0.0005f,
                        -PANEL + 0.0005f, -0.0005f, THICK + LIFT, 1.0f);
    // The fold between the two panels, rounded.
    kit_frame_pipe(kit, f, MAT_PAPER,
                   (vec3[]){{-h, -PANEL, 0.5f * THICK}, {h, -PANEL, 0.5f * THICK}}, 2, 0.5f * THICK,
                   8);
    const vec3 hinge = {0.0f, -PANEL, 0.5f * THICK};
    const vec3 down = {0.0f, -cosf(SAG), sinf(SAG)};
    const vec3 out = {0.0f, sinf(SAG), cosf(SAG)};
    hanging_panel(kit, f, hinge, down, out);
}

void fridge_map_closed(Kit* kit, const KitFrame* f) {
    const MapArt* art = &MAP_ART[MAP_TOWN];
    const float h = 0.5f * PANEL;
    kit_frame_soft_box(kit, f, MAT_PAPER, -h, h, -h, h, -THICK, THICK, 0.0012f, 0.0f);
    kit_frame_card_rect(kit, f, MAT_TOWN_MAP, art->folded_cover, -h + 0.0005f, h - 0.0005f,
                        -h + 0.0005f, h - 0.0005f, THICK + LIFT, 1.0f);
    kit_frame_card_rect(kit, f, MAT_TOWN_MAP, art->folded_inside, -h + 0.0005f, h - 0.0005f,
                        -h + 0.0005f, h - 0.0005f, -THICK - LIFT, -1.0f);
}

void fridge_map_build(FridgeMap* fm, Engine* engine, Scene* scene, bool taken) {
    *fm = (FridgeMap){.taken = taken};
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
    if (fm->taken)
        return;
    fm->taken = true;
    free_node(fm->node);
    fm->node = NULL;
}
