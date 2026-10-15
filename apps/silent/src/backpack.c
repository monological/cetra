#include <stdio.h>
#include <string.h>

#include "backpack.h"
#include "bedroom.h"
#include "fridge_map.h"
#include "house_key.h"
#include "kit.h"
#include "library.h"
#include "mats.h"

// How the bag lies on the quilt: its top toward the bed's head, turned a little off square,
// rolled onto one side and sunk into the quilt, as something soft dropped on something soft.
#define BAG_YAW  (0.5f * GLM_PIf + 0.35f)
#define BAG_ROLL 0.12f
#define BAG_SINK 0.012f
// Its middle over the quilt's top, which is what the player reaches for.
#define BAG_REACH_Y 0.08f

#define STRAP_POINTS 16

// A point `t` of the way along the quadratic Bezier p0, p1, p2.
static void bezier(const vec3 p0, const vec3 p1, const vec3 p2, float t, vec3 out) {
    const float u = 1.0f - t;
    for (int k = 0; k < 3; k++)
        out[k] = u * u * p0[k] + 2.0f * u * t * p1[k] + t * t * p2[k];
}

/*
 * The bag in its own frame, lying on its back as it was dropped: a along its width, y up out of
 * its front, d along its length, its top at +d. A slack olive canvas body, a pocket on the lower
 * half of its front, a flap over the upper half buckled down onto the pocket by two leather
 * straps, a leather haul loop at the top, and one shoulder strap fallen loose beside it.
 */
static void bag(Kit* kit) {
    const KitFrame* f = &KIT_WORLD;
    kit_frame_soft_box(kit, f, MAT_CANVAS, -0.15f, 0.15f, 0.0f, 0.12f, -0.21f, 0.21f, 0.055f,
                       0.02f);
    kit_frame_soft_box(kit, f, MAT_CANVAS, -0.11f, 0.11f, 0.09f, 0.15f, -0.19f, 0.0f, 0.025f,
                       0.01f);
    kit_frame_soft_box(kit, f, MAT_CANVAS, -0.14f, 0.14f, 0.115f, 0.14f, -0.02f, 0.22f, 0.012f,
                       0.006f);
    for (int s = -1; s <= 1; s += 2) {
        const float a = (float)s * 0.065f;
        kit_frame_box(kit, f, MAT_LEATHER, a - 0.012f, a + 0.012f, 0.152f, 0.157f, -0.09f, 0.02f,
                      false);
        kit_frame_box(kit, f, MAT_BRASS, a - 0.017f, a + 0.017f, 0.156f, 0.161f, -0.07f, -0.047f,
                      false);
    }
    const vec3 loop[] = {{-0.035f, 0.06f, 0.205f},
                         {-0.03f, 0.07f, 0.245f},
                         {0.0f, 0.075f, 0.262f},
                         {0.03f, 0.07f, 0.245f},
                         {0.035f, 0.06f, 0.205f}};
    kit_frame_pipe(kit, f, MAT_LEATHER, loop, KIT_COUNT(loop), 0.006f, 8);
    // The strap out from under the bag's top, round on the quilt in a lazy curve, and back under
    // its foot.
    const vec3 s0 = {0.1f, 0.012f, 0.19f}, s1 = {0.42f, 0.006f, 0.02f}, s2 = {0.11f, 0.012f, -0.2f};
    vec3 strap[STRAP_POINTS];
    for (int i = 0; i < STRAP_POINTS; i++)
        bezier(s0, s1, s2, (float)i / (float)(STRAP_POINTS - 1), strap[i]);
    kit_frame_pipe(kit, f, MAT_CANVAS, strap, STRAP_POINTS, 0.007f, 8);
    // The zip's pull, at the pocket's top edge.
    kit_frame_box(kit, f, MAT_BRASS, 0.05f, 0.058f, 0.142f, 0.148f, -0.005f, 0.02f, false);
}

// The town map folded, turned a quarter so the grid's side-on picture of it is its cover.
static void town_map(Kit* kit) {
    fridge_map_closed(kit, &KIT_WORLD_Z);
}

const ItemSpec ITEMS[ITEM_COUNT] = {
    // tools/make_flashlight_blender.py makes it, its lamp the node named as the item is.
    [ITEM_FLASHLIGHT] = {"flashlight",
                         "FLASHLIGHT",
                         "An army flashlight. It clips to the jacket. F turns it on and off.",
                         {1, 2},
                         NULL,
                         "assets/models/flashlight.glb"},
    [ITEM_TOWN_MAP] = {"map",
                       "TOWN MAP",
                       "A folded street map of Pale Ridge, taken off the fridge door. "
                       "M opens it.",
                       {2, 2},
                       town_map},
    [ITEM_KEY] = {"key",
                  "KEY",
                  "A heavy old brass key, left on the workbench in the basement. It might fit "
                  "the front door.",
                  {2, 1},
                  house_key_model},
};

ItemId item_by_id(const char* id) {
    for (int i = 0; i < ITEM_COUNT; i++)
        if (id && !strcmp(ITEMS[i].id, id))
            return (ItemId)i;
    return ITEM_NONE;
}

// What the bag holds when it is found.
static const ItemId CONTENTS[] = {ITEM_FLASHLIGHT};

// Whether a `w` x `h` footprint at column `c`, row `r` is clear of what lies in `used`.
static bool fits(const bool used[BAG_MAX_ROWS][BAG_COLS], int c, int r, int w, int h) {
    for (int y = r; y < r + h; y++)
        for (int x = c; x < c + w; x++)
            if (used[y][x])
                return false;
    return true;
}

// `item` where it first fits, reading the grid a row at a time, into `cell`; false when it fits
// nowhere in BAG_MAX_ROWS.
static bool place_one(bool used[BAG_MAX_ROWS][BAG_COLS], ItemId item, int cell[2]) {
    const int w = ITEMS[item].cells[0], h = ITEMS[item].cells[1];
    for (int r = 0; r + h <= BAG_MAX_ROWS; r++)
        for (int c = 0; c + w <= BAG_COLS; c++) {
            if (!fits(used, c, r, w, h))
                continue;
            for (int y = r; y < r + h; y++)
                for (int x = c; x < c + w; x++)
                    used[y][x] = true;
            cell[0] = c;
            cell[1] = r;
            return true;
        }
    return false;
}

// Each of `order`'s footprints where it first fits, in that order, into `cells`: the rows they
// take, or past BAG_MAX_ROWS when one does not fit. First fit read a row at a time only ever adds
// places after the ones it had, so a grid of any more rows packs them the same.
static int place(const ItemId* order, int n, int cells[ITEM_COUNT][2]) {
    bool used[BAG_MAX_ROWS][BAG_COLS];
    memset(used, 0, sizeof(used));
    int rows = 0;
    for (int k = 0; k < n; k++) {
        if (!place_one(used, order[k], cells[order[k]]))
            return BAG_MAX_ROWS + 1;
        const int end = cells[order[k]][1] + ITEMS[order[k]].cells[1];
        rows = end > rows ? end : rows;
    }
    return rows;
}

static int area(ItemId item) {
    return ITEMS[item].cells[0] * ITEMS[item].cells[1];
}

// Where everything carried lies, as the header says -- in the order it was had, unless that will
// not fit the rows the grid has at the least and largest first takes fewer -- and the grid's
// rows: down to the last thing and BAG_SPARE more.
static void pack(Backpack* bp) {
    // Largest first, each size in the order its things were had in.
    ItemId largest[ITEM_COUNT] = {0};
    int most = 0, n = 0;
    for (int k = 0; k < bp->held_count; k++)
        most = area(bp->held[k]) > most ? area(bp->held[k]) : most;
    for (int a = most; a > 0; a--)
        for (int k = 0; k < bp->held_count; k++)
            if (area(bp->held[k]) == a)
                largest[n++] = bp->held[k];
    int by_held[ITEM_COUNT][2] = {{0}}, by_size[ITEM_COUNT][2] = {{0}};
    const int held_rows = place(bp->held, bp->held_count, by_held);
    const int size_rows = place(largest, bp->held_count, by_size);
    const bool by_size_wins = held_rows > BAG_MIN_ROWS && size_rows < held_rows;
    memcpy(bp->cell, by_size_wins ? by_size : by_held, sizeof(bp->cell));
    const int rows = (by_size_wins ? size_rows : held_rows) + BAG_SPARE;
    bp->rows = rows < BAG_MIN_ROWS ? BAG_MIN_ROWS : rows > BAG_MAX_ROWS ? BAG_MAX_ROWS : rows;
}

// What lies in the world, each on the graph exactly while the bag does not hold it.
static void lay_out(Backpack* bp) {
    for (int i = 0; i < ITEM_COUNT; i++)
        kit_node_shown(bp->root, bp->lying[i], !backpack_holds(bp, (ItemId)i));
}

// `item`'s model from its file: the node named as it is, alone, its materials the game's; NULL,
// said by name, when there is none.
static SceneNode* model_from_file(Engine* engine, Scene* scene, const ItemSpec* item) {
    SceneNode* file = library_take(engine, scene, item->file, item->id, NULL);
    if (!file)
        return NULL;
    SceneNode* model = library_find(file, item->id, NULL);
    if (model)
        node_remove_child(model->parent, model);
    else
        fprintf(stderr, "silent: %s has no '%s'\n", item->file, item->id);
    free_node(file);
    return model;
}

// The bag's contents into the grid.
static void unpack(Backpack* bp) {
    for (int i = 0; i < KIT_COUNT(CONTENTS); i++)
        backpack_add(bp, CONTENTS[i]);
}

void backpack_build(Backpack* bp, Engine* engine, Scene* scene) {
    *bp = (Backpack){.root = scene->root_node, .rows = BAG_MIN_ROWS};
    vec3 rest = {0.0f, 0.0f, 0.0f};
    bedroom_bed_top(rest);
    glm_vec3_copy(rest, bp->at);
    bp->at[1] += BAG_REACH_Y;
    // The bag on the bed and every model built here, each only ever drawn alone: one kit apiece,
    // beside the bag's, so the materials are made once and those none of them used freed after.
    // Every kit is made before any is finished, which would free what it did not use. A model from
    // a file has no kit.
    Kit kits[ITEM_COUNT + 1];
    Kit* finished[ITEM_COUNT + 1];
    int count = 0;
    Kit* kit = finished[count++] = &kits[0];
    mats_kit(kit, engine, scene);
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (ITEMS[i].file)
            continue;
        Kit* item = finished[count] = &kits[count];
        count++;
        kit_init_beside(item, kit, GLM_VEC3_ZERO);
        item->casts_nothing = true;
        ITEMS[i].model(item);
    }
    for (int i = 0, k = 1; i < ITEM_COUNT; i++)
        bp->models[i] = ITEMS[i].file ? model_from_file(engine, scene, &ITEMS[i])
                                      : kit_finish_alone(finished[k++], ITEMS[i].id);
    bag(kit);
    bp->bag = kit_finish(kit, "backpack");
    // No capture keeps it: it is taken while the game runs.
    bp->bag->capture_hidden = true;
    glm_translate_make(bp->bag->original_transform, (vec3){rest[0], rest[1] - BAG_SINK, rest[2]});
    glm_rotate_y(bp->bag->original_transform, BAG_YAW, bp->bag->original_transform);
    glm_rotate_z(bp->bag->original_transform, BAG_ROLL, bp->bag->original_transform);
    kit_free_unused(finished, count);
}

void backpack_take(Backpack* bp) {
    if (bp->taken)
        return;
    bp->taken = true;
    unpack(bp);
    kit_node_shown(bp->root, bp->bag, false);
}

bool backpack_add(Backpack* bp, ItemId item) {
    if (!bp->taken || backpack_holds(bp, item))
        return false;
    bp->held[bp->held_count++] = item;
    pack(bp);
    lay_out(bp);
    return true;
}

void backpack_lay(Backpack* bp, ItemId item, SceneNode* node) {
    bp->lying[item] = node;
    lay_out(bp);
}

bool backpack_holds(const Backpack* bp, ItemId item) {
    for (int k = 0; k < bp->held_count; k++)
        if (bp->held[k] == item)
            return true;
    return false;
}

void backpack_restore(Backpack* bp, bool taken, const ItemId* held, int count) {
    // On the bed or not, then holding what it is said to hold rather than what it came with.
    kit_node_shown(bp->root, bp->bag, !taken);
    bp->taken = taken;
    bp->held_count = 0;
    for (int k = 0; k < count; k++)
        if (held[k] > ITEM_NONE && held[k] < ITEM_COUNT)
            backpack_add(bp, held[k]);
    pack(bp);
    lay_out(bp);
}

void backpack_free(Backpack* bp) {
    // The bag is the backpack's wherever it lies.
    free_node(bp->bag);
    for (int i = 0; i < ITEM_COUNT; i++)
        free_node(bp->models[i]);
    *bp = (Backpack){0};
}
