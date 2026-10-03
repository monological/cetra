#include "cat_places.h"

#include <stdio.h>
#include <string.h>

#include "layout.h"

// A flight's link runs from half a tread before its first riser to half a tread past its
// last, which is how make_cat_blender.py lays the stair clips' treads under their paws.
#define STAIR_TOP_Z    (GALLERY_Z1 - 0.5f * STAIR_GOING)
#define STAIR_BOTTOM_Z (STAIR_FOOT_Z + 0.5f * STAIR_GOING)
#define STAIR_MID_X    4.4f

// The chair's leather cushion, the counter's top and the kitchen chair's seat.
#define STUDY_SEAT   (FLOOR2_Y + 0.505f)
#define COUNTER_TOP  (FLOOR_Y + 0.9f)
#define KITCHEN_SEAT (FLOOR_Y + 0.516f)
// The gallery's hand rail: the top of its wood, and the line along its middle; the stair's
// head newel's cap.
#define RAIL_TOP  4.07f
#define RAIL_Z    15.0f
#define NEWEL_TOP 4.38f

const CatPlace CAT_PLACES[CAT_PLACE_COUNT] = {
    // The study: home is the master's chair at the desk in the bay, under the stained glass.
    [CAT_AT_STUDY_CHAIR] = {"study_chair", {-5.02f, STUDY_SEAT, 9.0f}, 0.12f, CAT_REST_CURL, 0.52f},
    [CAT_AT_CHAIR_JUMP] = {"chair_jump", {-4.35f, FLOOR2_Y, 8.95f}, 0.2f, CAT_REST_NONE, 0.0f},
    [CAT_AT_STUDY_BAY] = {"study_bay", {-3.55f, FLOOR2_Y, 9.3f}, 0.3f, CAT_REST_NONE, 0.0f},
    [CAT_AT_STUDY_WINDOW] = {"study_window", {-5.0f, FLOOR2_Y, 8.3f}, 0.2f, CAT_REST_SIT, GLM_PIf},
    [CAT_AT_STUDY_BAY_W] = {"study_bay_w", {-6.35f, FLOOR2_Y, 9.1f}, 0.3f, CAT_REST_LIE, 0.8f},
    [CAT_AT_STUDY_MID] = {"study_mid", {-3.2f, FLOOR2_Y, 12.0f}, 0.35f, CAT_REST_SIT, 2.6f},
    [CAT_AT_STUDY_DOOR] = {"study_door", {-2.55f, FLOOR2_Y, 13.55f}, 0.2f, CAT_REST_NONE, 0.0f},
    // The gallery, along the great hall's front at the upper floor, with a place a hop down
    // from each stretch of the rail.
    [CAT_AT_GAL_STUDY] = {"gal_study", {-2.55f, FLOOR2_Y, 14.4f}, 0.25f, CAT_REST_NONE, 0.0f},
    [CAT_AT_GAL_W] = {"gal_w", {-4.35f, FLOOR2_Y, 14.5f}, 0.25f, CAT_REST_SIT, 0.0f},
    [CAT_AT_GAL_1] = {"gal_1", {-1.5f, FLOOR2_Y, 14.45f}, 0.3f, CAT_REST_NONE, 0.0f},
    [CAT_AT_GALLERY] = {"gallery", {0.0f, FLOOR2_Y, 14.45f}, 0.3f, CAT_REST_LIE, 0.5f * GLM_PIf},
    [CAT_AT_GAL_2] = {"gal_2", {0.8f, FLOOR2_Y, 14.45f}, 0.3f, CAT_REST_NONE, 0.0f},
    [CAT_AT_GAL_3] = {"gal_3", {2.4f, FLOOR2_Y, 14.45f}, 0.3f, CAT_REST_NONE, 0.0f},
    [CAT_AT_GAL_4] = {"gal_4", {2.9f, FLOOR2_Y, 14.45f}, 0.2f, CAT_REST_NONE, 0.0f},
    [CAT_AT_GAL_E] = {"gal_e", {3.3f, FLOOR2_Y, 14.45f}, 0.25f, CAT_REST_NONE, 0.0f},
    // The hand rail over the drop into the great hall, walked one way, east, on the wood rather
    // than the collider standing 4 cm above it, to the stair's head newel at its end.
    [CAT_AT_RAIL_W] =
        {"rail_w", {-4.15f, RAIL_TOP, RAIL_Z}, 0.0f, CAT_REST_NONE, 0.0f, CAT_TAG_BEAM},
    [CAT_AT_RAIL_1] =
        {"rail_1", {-2.0f, RAIL_TOP, RAIL_Z}, 0.0f, CAT_REST_NONE, 0.0f, CAT_TAG_BEAM},
    [CAT_AT_RAIL_2] = {"rail_2", {0.3f, RAIL_TOP, RAIL_Z}, 0.0f, CAT_REST_NONE, 0.0f, CAT_TAG_BEAM},
    [CAT_AT_RAIL_3] = {"rail_3", {2.4f, RAIL_TOP, RAIL_Z}, 0.0f, CAT_REST_NONE, 0.0f, CAT_TAG_BEAM},
    [CAT_AT_RAIL_E] = {"rail_e", {3.7f, RAIL_TOP, RAIL_Z}, 0.0f, CAT_REST_NONE, 0.0f, CAT_TAG_BEAM},
    [CAT_AT_NEWEL_CAP] = {"newel_cap", {3.95f, NEWEL_TOP, RAIL_Z}, 0.05f, CAT_REST_SIT, 0.0f},
    // The stair: a level approach at either end, straight into the flight.
    [CAT_AT_STAIR_HEAD] =
        {"stair_head", {STAIR_MID_X, FLOOR2_Y, 14.55f}, 0.15f, CAT_REST_NONE, 0.0f},
    [CAT_AT_STAIR_TOP] =
        {"stair_top", {STAIR_MID_X, FLOOR2_Y, STAIR_TOP_Z}, 0.1f, CAT_REST_NONE, 0.0f},
    [CAT_AT_STAIR_BOTTOM] =
        {"stair_bottom", {STAIR_MID_X, FLOOR_Y, STAIR_BOTTOM_Z}, 0.1f, CAT_REST_NONE, 0.0f},
    [CAT_AT_STAIR_FOOT] = {"stair_foot", {STAIR_MID_X, FLOOR_Y, 18.95f}, 0.2f, CAT_REST_NONE, 0.0f},
    // The great hall.
    [CAT_AT_GREAT_STAIR] = {"great_stair", {3.2f, FLOOR_Y, 18.95f}, 0.35f, CAT_REST_NONE, 0.0f},
    [CAT_AT_GREAT_E] = {"great_e", {2.8f, FLOOR_Y, 16.3f}, 0.4f, CAT_REST_NONE, 0.0f},
    [CAT_AT_GREAT_W] = {"great_w", {-3.7f, FLOOR_Y, 16.6f}, 0.4f, CAT_REST_NONE, 0.0f},
    [CAT_AT_GREAT_S] = {"great_s", {-0.75f, FLOOR_Y, 14.4f}, 0.35f, CAT_REST_NONE, 0.0f},
    [CAT_AT_RUG] = {"rug", {-0.75f, FLOOR_Y + 0.008f, 16.4f}, 0.4f, CAT_REST_LIE, 0.4f},
    [CAT_AT_HEARTH] = {"hearth", {-0.75f, FLOOR_Y + 0.06f, 18.62f}, 0.25f, CAT_REST_LIE, GLM_PIf},
    // The hall and the kitchen.
    [CAT_AT_ARCH] = {"arch", {-0.75f, FLOOR_Y, 13.8f}, 0.25f, CAT_REST_NONE, 0.0f},
    [CAT_AT_HALL_CLOCK] =
        {"hall_clock", {-0.8f, FLOOR_Y, 13.15f}, 0.2f, CAT_REST_SIT, -0.5f * GLM_PIf},
    [CAT_AT_HALL_FRONT] = {"hall_front", {-0.75f, FLOOR_Y, 11.3f}, 0.3f, CAT_REST_NONE, 0.0f},
    [CAT_AT_KITCHEN_DOOR] = {"kitchen_door", {0.0f, FLOOR_Y, 13.15f}, 0.2f, CAT_REST_NONE, 0.0f},
    [CAT_AT_KITCHEN_MID] = {"kitchen_mid", {1.8f, FLOOR_Y, 12.1f}, 0.4f, CAT_REST_NONE, 0.0f},
    [CAT_AT_STOVE_MAT] =
        {"stove_mat", {1.12f, FLOOR_Y + 0.004f, 11.72f}, 0.2f, CAT_REST_CURL, -0.3f},
    [CAT_AT_COUNTER_JUMP] = {"counter_jump", {1.95f, FLOOR_Y, 11.1f}, 0.15f, CAT_REST_NONE, 0.0f},
    // On the counter between the bowls and the sink, looking out at the rain.
    [CAT_AT_KITCHEN_WINDOW] =
        {"kitchen_window", {1.95f, COUNTER_TOP, 10.42f}, 0.1f, CAT_REST_SIT, GLM_PIf},
    [CAT_AT_KCHAIR_JUMP] = {"kchair_jump", {2.75f, FLOOR_Y, 11.95f}, 0.15f, CAT_REST_NONE, 0.0f},
    [CAT_AT_KITCHEN_CHAIR] =
        {"kitchen_chair", {3.35f, KITCHEN_SEAT, 12.12f}, 0.1f, CAT_REST_CURL, -1.2f},
};

int cat_place_find(const char* name) {
    for (int i = 0; name && i < CAT_PLACE_COUNT; i++)
        if (!strcmp(CAT_PLACES[i].name, name))
            return i;
    return -1;
}

typedef struct CatLinkDef {
    CatPlaceId a, b;
    int kind;
    float apex;   // a jump's arc above its higher end
    bool one_way; // from a to b only
} CatLinkDef;

// Every link goes both ways but the rail's: on along it, up onto the newel, and down off
// either. Nothing turns round on a hand rail.
static const CatLinkDef LINKS[] = {
    {CAT_AT_GAL_W, CAT_AT_RAIL_W, CAT_LINK_JUMP, 0.1f, true},
    {CAT_AT_RAIL_W, CAT_AT_RAIL_1, CAT_LINK_RAIL, 0.0f, true},
    {CAT_AT_RAIL_1, CAT_AT_RAIL_2, CAT_LINK_RAIL, 0.0f, true},
    {CAT_AT_RAIL_2, CAT_AT_RAIL_3, CAT_LINK_RAIL, 0.0f, true},
    {CAT_AT_RAIL_3, CAT_AT_RAIL_E, CAT_LINK_RAIL, 0.0f, true},
    {CAT_AT_RAIL_E, CAT_AT_NEWEL_CAP, CAT_LINK_JUMP, 0.08f, true},
    {CAT_AT_NEWEL_CAP, CAT_AT_STAIR_HEAD, CAT_LINK_JUMP, 0.06f, true},
    {CAT_AT_RAIL_1, CAT_AT_GAL_1, CAT_LINK_JUMP, 0.04f, true},
    {CAT_AT_RAIL_2, CAT_AT_GAL_2, CAT_LINK_JUMP, 0.04f, true},
    {CAT_AT_RAIL_3, CAT_AT_GAL_4, CAT_LINK_JUMP, 0.04f, true},
    {CAT_AT_STUDY_CHAIR, CAT_AT_CHAIR_JUMP, CAT_LINK_JUMP, 0.12f},
    {CAT_AT_CHAIR_JUMP, CAT_AT_STUDY_BAY, CAT_LINK_WALK, 0.0f},
    {CAT_AT_CHAIR_JUMP, CAT_AT_STUDY_WINDOW, CAT_LINK_WALK, 0.0f},
    {CAT_AT_STUDY_WINDOW, CAT_AT_STUDY_BAY_W, CAT_LINK_WALK, 0.0f},
    {CAT_AT_STUDY_BAY, CAT_AT_STUDY_MID, CAT_LINK_WALK, 0.0f},
    {CAT_AT_STUDY_MID, CAT_AT_STUDY_DOOR, CAT_LINK_WALK, 0.0f},
    {CAT_AT_STUDY_DOOR, CAT_AT_GAL_STUDY, CAT_LINK_WALK, 0.0f},
    {CAT_AT_GAL_STUDY, CAT_AT_GAL_W, CAT_LINK_WALK, 0.0f},
    {CAT_AT_GAL_STUDY, CAT_AT_GAL_1, CAT_LINK_WALK, 0.0f},
    {CAT_AT_GAL_1, CAT_AT_GALLERY, CAT_LINK_WALK, 0.0f},
    {CAT_AT_GALLERY, CAT_AT_GAL_2, CAT_LINK_WALK, 0.0f},
    {CAT_AT_GAL_2, CAT_AT_GAL_3, CAT_LINK_WALK, 0.0f},
    {CAT_AT_GAL_3, CAT_AT_GAL_4, CAT_LINK_WALK, 0.0f},
    {CAT_AT_GAL_4, CAT_AT_GAL_E, CAT_LINK_WALK, 0.0f},
    {CAT_AT_GAL_E, CAT_AT_STAIR_HEAD, CAT_LINK_WALK, 0.0f},
    {CAT_AT_STAIR_HEAD, CAT_AT_STAIR_TOP, CAT_LINK_WALK, 0.0f},
    {CAT_AT_STAIR_TOP, CAT_AT_STAIR_BOTTOM, CAT_LINK_STAIR, 0.0f},
    {CAT_AT_STAIR_BOTTOM, CAT_AT_STAIR_FOOT, CAT_LINK_WALK, 0.0f},
    {CAT_AT_STAIR_FOOT, CAT_AT_GREAT_STAIR, CAT_LINK_WALK, 0.0f},
    {CAT_AT_GREAT_STAIR, CAT_AT_GREAT_E, CAT_LINK_WALK, 0.0f},
    {CAT_AT_GREAT_E, CAT_AT_RUG, CAT_LINK_WALK, 0.0f},
    {CAT_AT_GREAT_E, CAT_AT_GREAT_S, CAT_LINK_WALK, 0.0f},
    {CAT_AT_RUG, CAT_AT_GREAT_W, CAT_LINK_WALK, 0.0f},
    {CAT_AT_RUG, CAT_AT_GREAT_S, CAT_LINK_WALK, 0.0f},
    {CAT_AT_RUG, CAT_AT_HEARTH, CAT_LINK_WALK, 0.0f},
    {CAT_AT_GREAT_W, CAT_AT_GREAT_S, CAT_LINK_WALK, 0.0f},
    {CAT_AT_GREAT_S, CAT_AT_ARCH, CAT_LINK_WALK, 0.0f},
    {CAT_AT_ARCH, CAT_AT_HALL_CLOCK, CAT_LINK_WALK, 0.0f},
    {CAT_AT_ARCH, CAT_AT_KITCHEN_DOOR, CAT_LINK_WALK, 0.0f},
    {CAT_AT_HALL_CLOCK, CAT_AT_KITCHEN_DOOR, CAT_LINK_WALK, 0.0f},
    {CAT_AT_HALL_CLOCK, CAT_AT_HALL_FRONT, CAT_LINK_WALK, 0.0f},
    {CAT_AT_KITCHEN_DOOR, CAT_AT_KITCHEN_MID, CAT_LINK_WALK, 0.0f},
    {CAT_AT_KITCHEN_MID, CAT_AT_STOVE_MAT, CAT_LINK_WALK, 0.0f},
    {CAT_AT_KITCHEN_MID, CAT_AT_COUNTER_JUMP, CAT_LINK_WALK, 0.0f},
    {CAT_AT_COUNTER_JUMP, CAT_AT_KITCHEN_WINDOW, CAT_LINK_JUMP, 0.12f},
    {CAT_AT_KITCHEN_MID, CAT_AT_KCHAIR_JUMP, CAT_LINK_WALK, 0.0f},
    {CAT_AT_KCHAIR_JUMP, CAT_AT_KITCHEN_CHAIR, CAT_LINK_JUMP, 0.1f},
};

NavGraph* cat_places_build(void) {
    NavGraph* g = create_nav_graph();
    if (!g)
        return NULL;
    // A walk is what a cat would rather do; the stair costs it a little more, and a jump
    // more again, so it climbs onto nothing it can walk round.
    nav_graph_set_kind(g, CAT_LINK_WALK,
                       &(NavKind){.name = "walk", .cost_scale = 1.0f, .smooth = true});
    nav_graph_set_kind(g, CAT_LINK_STAIR, &(NavKind){.name = "stair", .cost_scale = 1.5f});
    nav_graph_set_kind(g, CAT_LINK_JUMP,
                       &(NavKind){.name = "jump", .cost_scale = 3.0f, .drive = NAV_DRIVE_PROGRESS});
    nav_graph_set_kind(g, CAT_LINK_RAIL, &(NavKind){.name = "rail", .cost_scale = 1.2f});
    for (int i = 0; i < CAT_PLACE_COUNT; i++)
        nav_graph_add_node(g, CAT_PLACES[i].name, CAT_PLACES[i].feet, CAT_PLACES[i].radius,
                           CAT_PLACES[i].tags);
    for (size_t i = 0; i < sizeof(LINKS) / sizeof(LINKS[0]); i++) {
        const CatLinkDef* d = &LINKS[i];
        NavShapeDesc shape = {.shape = NAV_SHAPE_LINE};
        if (d->kind == CAT_LINK_STAIR)
            shape = (NavShapeDesc){.shape = NAV_SHAPE_STEPS,
                                   .rise = STAIR_RISE,
                                   .going = STAIR_GOING,
                                   .nosing = 0.5f * STAIR_GOING,
                                   .hop = 1.0f};
        else if (d->kind == CAT_LINK_JUMP)
            shape = (NavShapeDesc){.shape = NAV_SHAPE_ARC, .apex = d->apex};
        if ((d->one_way ? nav_graph_link(g, d->a, d->b, d->kind, &shape)
                        : nav_graph_link_both(g, d->a, d->b, d->kind, &shape)) < 0)
            fprintf(stderr, "silent: the cat's link %s - %s is refused\n", CAT_PLACES[d->a].name,
                    CAT_PLACES[d->b].name);
    }
    return g;
}

NavQuery cat_places_query(bool rail) {
    NavQuery q = {0};
    if (!rail)
        q.kinds = ((1u << CAT_LINK_KINDS) - 1u) & ~(1u << CAT_LINK_RAIL);
    return q;
}

static bool probe_static(const vec3 a, const vec3 b, void* user) {
    PhysicsWorld* physics = user;
    vec3 d;
    glm_vec3_sub((float*)b, (float*)a, d);
    const float len = glm_vec3_norm(d);
    if (len < 1e-5f)
        return false;
    glm_vec3_scale(d, 1.0f / len, d);
    RaycastHit hit;
    return physics_world_raycast_filtered(physics, (float*)a, d, len, 1u << OBJ_LAYER_STATIC,
                                          &hit) &&
           hit.hit;
}

// A hand above the feet: a link that grazes a tread's nosing or a rug's edge is not blocked,
// one through a table leg or a wall is.
#define CHECK_LIFT 0.1f
#define CHECK_STEP 0.05f

int cat_places_check(const NavGraph* graph, PhysicsWorld* physics) {
    if (!graph || !physics)
        return 0;
    const int blocked = nav_graph_check(graph, probe_static, physics, CHECK_LIFT, CHECK_STEP);
    printf("silent: the cat's %d places and %d links, %d of them blocked\n", graph->node_count,
           graph->link_count, blocked);
    return blocked;
}
