#include <float.h>
#include <string.h>

#include "backpack.h"
#include "bedroom.h"
#include "door.h"
#include "kit.h"
#include "mats.h"

const ItemSpec ITEMS[ITEM_COUNT] = {
    [ITEM_FLASHLIGHT] = {"flashlight",
                         "Flashlight",
                         "A heavy metal flashlight, its black paint worn through at the grip. "
                         "F turns it on and off.",
                         {3, 1}},
};

int item_by_id(const char* id) {
    for (int i = 0; i < ITEM_COUNT; i++)
        if (id && !strcmp(ITEMS[i].id, id))
            return i;
    return -1;
}

// How the bag lies on the quilt: its top toward the bed's head, turned a little off square,
// rolled onto one side and sunk into the quilt, as something soft dropped on something soft.
#define BAG_YAW  (0.5f * GLM_PIf + 0.35f)
#define BAG_ROLL 0.12f
#define BAG_SINK 0.012f

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

/*
 * The flashlight along its own d, 0.3 m from its tail cap to its bezel, centred on its origin:
 * a knurled barrel holding the cells, a collar with a rubber switch, a head flaring to the bezel,
 * and in it a polished reflector behind the lens.
 */
#define TORCH_HALF 0.15f
static void flashlight(Kit* kit) {
    const KitFrame* f = &KIT_WORLD;
    vec2 body[KIT_MAX_POINTS];
    int n = 0;
    const vec2 tail[] = {
        {0.0f, 0.0f}, {0.017f, 0.0f}, {0.0195f, 0.003f}, {0.0195f, 0.028f}, {0.0182f, 0.031f}};
    for (int i = 0; i < KIT_COUNT(tail); i++)
        glm_vec2_copy((float*)tail[i], body[n++]);
    for (int i = 0; i < 6; i++) {
        const float z = 0.045f + 0.018f * (float)i;
        glm_vec2_copy((vec2){0.0182f, z}, body[n++]);
        glm_vec2_copy((vec2){0.0172f, z + 0.009f}, body[n++]);
    }
    const vec2 head[] = {{0.0182f, 0.16f}, {0.019f, 0.168f}, {0.019f, 0.2f},  {0.023f, 0.215f},
                         {0.029f, 0.25f},  {0.031f, 0.26f},  {0.031f, 0.29f}, {0.0285f, 0.294f},
                         {0.026f, 0.294f}, {0.026f, 0.288f}};
    for (int i = 0; i < KIT_COUNT(head); i++)
        glm_vec2_copy((float*)head[i], body[n++]);
    const vec3 axis = {0.0f, 0.0f, 1.0f};
    kit_frame_lathe_on(kit, f, MAT_ANODISED, (vec3){0.0f, 0.0f, -TORCH_HALF}, axis, body, n, 24);
    // The reflector, its inside toward the lens, and the lens.
    const vec2 cup[] = {{0.026f, 0.02f}, {0.006f, 0.0f}};
    kit_frame_lathe_on(kit, f, MAT_STAINLESS, (vec3){0.0f, 0.0f, TORCH_HALF - 0.05f}, axis, cup,
                       KIT_COUNT(cup), 24);
    const vec2 lens[] = {{0.0f, 0.0f}, {0.026f, 0.0f}, {0.026f, 0.002f}, {0.0f, 0.002f}};
    kit_frame_lathe_on(kit, f, MAT_GLASS_CLEAR, (vec3){0.0f, 0.0f, TORCH_HALF - 0.032f}, axis, lens,
                       KIT_COUNT(lens), 24);
    kit_frame_soft_box(kit, f, MAT_BLACK, -0.006f, 0.006f, 0.016f, 0.0235f, 0.025f, 0.045f, 0.003f,
                       0.0f);
}

// What the bag holds when it is found.
static const ItemId CONTENTS[] = {ITEM_FLASHLIGHT};

// Each of `order`'s footprints where it first fits in `rows` rows, in that order, reading the grid
// a row at a time; false when one will not fit.
static bool place(Backpack* bp, const ItemId* order, int n, int rows) {
    bool used[BAG_MAX_ROWS][BAG_COLS];
    memset(used, 0, sizeof(used));
    for (int k = 0; k < n; k++) {
        const int w = ITEMS[order[k]].cells[0], h = ITEMS[order[k]].cells[1];
        bool placed = false;
        for (int r = 0; r + h <= rows && !placed; r++)
            for (int c = 0; c + w <= BAG_COLS && !placed; c++) {
                bool free = true;
                for (int y = r; y < r + h && free; y++)
                    for (int x = c; x < c + w && free; x++)
                        free = !used[y][x];
                if (!free)
                    continue;
                for (int y = r; y < r + h; y++)
                    for (int x = c; x < c + w; x++)
                        used[y][x] = true;
                bp->cell[order[k]][0] = c;
                bp->cell[order[k]][1] = r;
                placed = true;
            }
        if (!placed)
            return false;
    }
    return true;
}

static int area(ItemId item) {
    return ITEMS[item].cells[0] * ITEMS[item].cells[1];
}

// Where everything carried lies, as the header says, and the grid's rows: down to the last thing
// and BAG_SPARE more.
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
    int rows = BAG_MIN_ROWS;
    while (rows < BAG_MAX_ROWS && !place(bp, bp->held, bp->held_count, rows) &&
           !place(bp, largest, bp->held_count, rows))
        rows++;
    int bottom = 0;
    for (int k = 0; k < bp->held_count; k++) {
        const ItemId item = bp->held[k];
        const int end = bp->cell[item][1] + ITEMS[item].cells[1];
        bottom = end > bottom ? end : bottom;
    }
    rows = bottom + BAG_SPARE;
    bp->rows = rows < BAG_MIN_ROWS ? BAG_MIN_ROWS : rows > BAG_MAX_ROWS ? BAG_MAX_ROWS : rows;
}

// The bag's contents into the grid.
static void unpack(Backpack* bp) {
    for (int i = 0; i < KIT_COUNT(CONTENTS); i++)
        if (!backpack_holds(bp, CONTENTS[i]))
            bp->held[bp->held_count++] = CONTENTS[i];
    pack(bp);
}

// A model as a kit of its own, built round its origin, its materials its own.
static SceneNode* model(Engine* engine, Scene* scene, const char* name, void (*build)(Kit*),
                        bool casts) {
    Kit kit;
    kit_init(&kit, scene, NULL, NULL);
    mats_register(&kit, engine, scene);
    kit.casts_nothing = !casts;
    build(&kit);
    return kit_finish(&kit, name);
}

void backpack_build(Backpack* bp, Engine* engine, Scene* scene, bool taken) {
    *bp = (Backpack){.taken = taken, .rows = BAG_MIN_ROWS};
    if (taken)
        unpack(bp);
    bedroom_bed_top(bp->at);
    if (!taken) {
        // No capture keeps it: it is taken while the game runs.
        bp->bag = model(engine, scene, "backpack", bag, true);
        bp->bag->capture_hidden = true;
        const vec3 rest = {bp->at[0], bp->at[1] - BAG_SINK, bp->at[2]};
        glm_translate_make(bp->bag->original_transform, (float*)rest);
        glm_rotate_y(bp->bag->original_transform, BAG_YAW, bp->bag->original_transform);
        glm_rotate_z(bp->bag->original_transform, BAG_ROLL, bp->bag->original_transform);
    }
    bp->at[1] += 0.08f;
    // Only ever drawn alone, on the backpack's screen. Off the graph, no draw registers its
    // materials, which the scene then would not free: they are registered here.
    SceneNode* torch = model(engine, scene, "flashlight", flashlight, false);
    node_remove_child(scene->root_node, torch);
    for (size_t i = 0; i < torch->mesh_count; i++)
        scene_add_material(scene, torch->meshes[i]->material);
    bp->models[ITEM_FLASHLIGHT] = torch;
}

float backpack_reach_distance(const Backpack* bp, const vec3 eye, const vec3 forward, float reach,
                              float cone) {
    return bp->taken ? FLT_MAX : reach_distance(bp->at, eye, forward, reach, cone);
}

void backpack_take(Backpack* bp) {
    if (bp->taken)
        return;
    bp->taken = true;
    unpack(bp);
    free_node(bp->bag);
    bp->bag = NULL;
}

bool backpack_holds(const Backpack* bp, ItemId item) {
    for (int k = 0; k < bp->held_count; k++)
        if (bp->held[k] == item)
            return true;
    return false;
}

void backpack_free(Backpack* bp) {
    for (int i = 0; i < ITEM_COUNT; i++)
        free_node(bp->models[i]);
    *bp = (Backpack){0};
}
