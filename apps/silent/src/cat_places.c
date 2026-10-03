#include "cat_places.h"

#include <stdio.h>

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

const CatPlace CAT_PLACES[] = {
    // The study: home is the master's chair at the desk in the bay, under the stained glass.
    {"study_chair", {-5.02f, STUDY_SEAT, 9.0f}, 0.12f, CAT_REST_CURL, 0.52f},
    {"chair_jump", {-4.35f, FLOOR2_Y, 8.95f}, 0.2f, CAT_REST_NONE, 0.0f},
    {"study_bay", {-3.55f, FLOOR2_Y, 9.3f}, 0.3f, CAT_REST_NONE, 0.0f},
    {"study_window", {-5.0f, FLOOR2_Y, 8.3f}, 0.2f, CAT_REST_SIT, GLM_PIf},
    {"study_bay_w", {-6.35f, FLOOR2_Y, 9.1f}, 0.3f, CAT_REST_LIE, 0.8f},
    {"study_mid", {-3.2f, FLOOR2_Y, 12.0f}, 0.35f, CAT_REST_SIT, 2.6f},
    {"study_door", {-2.55f, FLOOR2_Y, 13.55f}, 0.2f, CAT_REST_NONE, 0.0f},
    // The gallery, along the great hall's front at the upper floor, with a place a hop down
    // from each stretch of the rail.
    {"gal_study", {-2.55f, FLOOR2_Y, 14.4f}, 0.25f, CAT_REST_NONE, 0.0f},
    {"gal_w", {-4.35f, FLOOR2_Y, 14.5f}, 0.25f, CAT_REST_SIT, 0.0f},
    {"gal_1", {-1.5f, FLOOR2_Y, 14.45f}, 0.3f, CAT_REST_NONE, 0.0f},
    {"gallery", {0.0f, FLOOR2_Y, 14.45f}, 0.3f, CAT_REST_LIE, 0.5f * GLM_PIf},
    {"gal_2", {0.8f, FLOOR2_Y, 14.45f}, 0.3f, CAT_REST_NONE, 0.0f},
    {"gal_3", {2.4f, FLOOR2_Y, 14.45f}, 0.3f, CAT_REST_NONE, 0.0f},
    {"gal_4", {2.9f, FLOOR2_Y, 14.45f}, 0.2f, CAT_REST_NONE, 0.0f},
    {"gal_e", {3.3f, FLOOR2_Y, 14.45f}, 0.25f, CAT_REST_NONE, 0.0f},
    // The hand rail over the drop into the great hall, walked one way, east, on the wood rather
    // than the collider standing 4 cm above it, to the stair's head newel at its end.
    {"rail_w", {-4.15f, RAIL_TOP, RAIL_Z}, 0.0f, CAT_REST_NONE, 0.0f, CAT_TAG_BEAM},
    {"rail_1", {-2.0f, RAIL_TOP, RAIL_Z}, 0.0f, CAT_REST_NONE, 0.0f, CAT_TAG_BEAM},
    {"rail_2", {0.3f, RAIL_TOP, RAIL_Z}, 0.0f, CAT_REST_NONE, 0.0f, CAT_TAG_BEAM},
    {"rail_3", {2.4f, RAIL_TOP, RAIL_Z}, 0.0f, CAT_REST_NONE, 0.0f, CAT_TAG_BEAM},
    {"rail_e", {3.7f, RAIL_TOP, RAIL_Z}, 0.0f, CAT_REST_NONE, 0.0f, CAT_TAG_BEAM},
    {"newel_cap", {3.95f, NEWEL_TOP, RAIL_Z}, 0.05f, CAT_REST_SIT, 0.0f},
    // The stair: a level approach at either end, straight into the flight.
    {"stair_head", {STAIR_MID_X, FLOOR2_Y, 14.55f}, 0.15f, CAT_REST_NONE, 0.0f},
    {"stair_top", {STAIR_MID_X, FLOOR2_Y, STAIR_TOP_Z}, 0.1f, CAT_REST_NONE, 0.0f},
    {"stair_bottom", {STAIR_MID_X, FLOOR_Y, STAIR_BOTTOM_Z}, 0.1f, CAT_REST_NONE, 0.0f},
    {"stair_foot", {STAIR_MID_X, FLOOR_Y, 18.95f}, 0.2f, CAT_REST_NONE, 0.0f},
    // The great hall.
    {"great_stair", {3.2f, FLOOR_Y, 18.95f}, 0.35f, CAT_REST_NONE, 0.0f},
    {"great_e", {2.8f, FLOOR_Y, 16.3f}, 0.4f, CAT_REST_NONE, 0.0f},
    {"great_w", {-3.7f, FLOOR_Y, 16.6f}, 0.4f, CAT_REST_NONE, 0.0f},
    {"great_s", {-0.75f, FLOOR_Y, 14.4f}, 0.35f, CAT_REST_NONE, 0.0f},
    {"rug", {-0.75f, FLOOR_Y + 0.008f, 16.4f}, 0.4f, CAT_REST_LIE, 0.4f},
    {"hearth", {-0.75f, FLOOR_Y + 0.06f, 18.62f}, 0.25f, CAT_REST_LIE, GLM_PIf},
    // The hall and the kitchen.
    {"arch", {-0.75f, FLOOR_Y, 13.8f}, 0.25f, CAT_REST_NONE, 0.0f},
    {"hall_clock", {-0.8f, FLOOR_Y, 13.15f}, 0.2f, CAT_REST_SIT, -0.5f * GLM_PIf},
    {"hall_front", {-0.75f, FLOOR_Y, 11.3f}, 0.3f, CAT_REST_NONE, 0.0f},
    {"kitchen_door", {0.0f, FLOOR_Y, 13.15f}, 0.2f, CAT_REST_NONE, 0.0f},
    {"kitchen_mid", {1.8f, FLOOR_Y, 12.1f}, 0.4f, CAT_REST_NONE, 0.0f},
    {"stove_mat", {1.12f, FLOOR_Y + 0.004f, 11.72f}, 0.2f, CAT_REST_CURL, -0.3f},
    {"counter_jump", {1.95f, FLOOR_Y, 11.1f}, 0.15f, CAT_REST_NONE, 0.0f},
    // On the counter between the bowls and the sink, looking out at the rain.
    {"kitchen_window", {1.95f, COUNTER_TOP, 10.42f}, 0.1f, CAT_REST_SIT, GLM_PIf},
    {"kchair_jump", {2.75f, FLOOR_Y, 11.95f}, 0.15f, CAT_REST_NONE, 0.0f},
    {"kitchen_chair", {3.35f, KITCHEN_SEAT, 12.12f}, 0.1f, CAT_REST_CURL, -1.2f},
};
const int CAT_PLACE_COUNT = (int)(sizeof(CAT_PLACES) / sizeof(CAT_PLACES[0]));

typedef struct CatLinkDef {
    const char *a, *b;
    int kind;
    float apex;   // a jump's arc above its higher end
    bool one_way; // from a to b only
} CatLinkDef;

// Every link goes both ways but the rail's: on along it, up onto the newel, and down off
// either. Nothing turns round on a hand rail.
static const CatLinkDef LINKS[] = {
    {"gal_w", "rail_w", CAT_LINK_JUMP, 0.1f, true},
    {"rail_w", "rail_1", CAT_LINK_RAIL, 0.0f, true},
    {"rail_1", "rail_2", CAT_LINK_RAIL, 0.0f, true},
    {"rail_2", "rail_3", CAT_LINK_RAIL, 0.0f, true},
    {"rail_3", "rail_e", CAT_LINK_RAIL, 0.0f, true},
    {"rail_e", "newel_cap", CAT_LINK_JUMP, 0.08f, true},
    {"newel_cap", "stair_head", CAT_LINK_JUMP, 0.06f, true},
    {"rail_1", "gal_1", CAT_LINK_JUMP, 0.04f, true},
    {"rail_2", "gal_2", CAT_LINK_JUMP, 0.04f, true},
    {"rail_3", "gal_4", CAT_LINK_JUMP, 0.04f, true},
    {"study_chair", "chair_jump", CAT_LINK_JUMP, 0.12f},
    {"chair_jump", "study_bay", CAT_LINK_WALK, 0.0f},
    {"chair_jump", "study_window", CAT_LINK_WALK, 0.0f},
    {"study_window", "study_bay_w", CAT_LINK_WALK, 0.0f},
    {"study_bay", "study_mid", CAT_LINK_WALK, 0.0f},
    {"study_mid", "study_door", CAT_LINK_WALK, 0.0f},
    {"study_door", "gal_study", CAT_LINK_WALK, 0.0f},
    {"gal_study", "gal_w", CAT_LINK_WALK, 0.0f},
    {"gal_study", "gal_1", CAT_LINK_WALK, 0.0f},
    {"gal_1", "gallery", CAT_LINK_WALK, 0.0f},
    {"gallery", "gal_2", CAT_LINK_WALK, 0.0f},
    {"gal_2", "gal_3", CAT_LINK_WALK, 0.0f},
    {"gal_3", "gal_4", CAT_LINK_WALK, 0.0f},
    {"gal_4", "gal_e", CAT_LINK_WALK, 0.0f},
    {"gal_e", "stair_head", CAT_LINK_WALK, 0.0f},
    {"stair_head", "stair_top", CAT_LINK_WALK, 0.0f},
    {"stair_top", "stair_bottom", CAT_LINK_STAIR, 0.0f},
    {"stair_bottom", "stair_foot", CAT_LINK_WALK, 0.0f},
    {"stair_foot", "great_stair", CAT_LINK_WALK, 0.0f},
    {"great_stair", "great_e", CAT_LINK_WALK, 0.0f},
    {"great_e", "rug", CAT_LINK_WALK, 0.0f},
    {"great_e", "great_s", CAT_LINK_WALK, 0.0f},
    {"rug", "great_w", CAT_LINK_WALK, 0.0f},
    {"rug", "great_s", CAT_LINK_WALK, 0.0f},
    {"rug", "hearth", CAT_LINK_WALK, 0.0f},
    {"great_w", "great_s", CAT_LINK_WALK, 0.0f},
    {"great_s", "arch", CAT_LINK_WALK, 0.0f},
    {"arch", "hall_clock", CAT_LINK_WALK, 0.0f},
    {"arch", "kitchen_door", CAT_LINK_WALK, 0.0f},
    {"hall_clock", "kitchen_door", CAT_LINK_WALK, 0.0f},
    {"hall_clock", "hall_front", CAT_LINK_WALK, 0.0f},
    {"kitchen_door", "kitchen_mid", CAT_LINK_WALK, 0.0f},
    {"kitchen_mid", "stove_mat", CAT_LINK_WALK, 0.0f},
    {"kitchen_mid", "counter_jump", CAT_LINK_WALK, 0.0f},
    {"counter_jump", "kitchen_window", CAT_LINK_JUMP, 0.12f},
    {"kitchen_mid", "kchair_jump", CAT_LINK_WALK, 0.0f},
    {"kchair_jump", "kitchen_chair", CAT_LINK_JUMP, 0.1f},
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
        const int a = nav_graph_find(g, d->a), b = nav_graph_find(g, d->b);
        if (a < 0 || b < 0 ||
            (d->one_way ? nav_graph_link(g, a, b, d->kind, &shape)
                        : nav_graph_link_both(g, a, b, d->kind, &shape)) < 0)
            fprintf(stderr, "silent: the cat's link %s - %s is refused\n", d->a, d->b);
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
