#ifndef _NAV_GRAPH_H_
#define _NAV_GRAPH_H_

/*
 * Where an agent can go, and how (spec 13.17).
 *
 * A PLACE GRAPH: named places an agent can stand at, and the links between them it can take.
 * A link is a shape -- a straight walk, a flight of steps, a jump's arc -- and an app-defined
 * KIND, which says what a route may use, what it costs, and what moves an agent along it.
 * There is no navmesh: a house's cat goes from the chair to the floor to the door, and the
 * places it goes between are worth naming, which a mesh's triangles are not.
 *
 * A route is A* over the links a query allows. A FOLLOWER walks one: the app advances it by
 * what its animation laid down -- metres for a walk, a fraction for a jump, whose clip, not
 * the distance covered, says how far through it is -- and the follower says where the agent
 * is and which way it faces. Corners between links of a smoothed kind are rounded inside the
 * place's radius, the clear ground its author promises round it, so the agent does not stop
 * and turn on the spot at every place it passes.
 *
 * A link's profile is what the agent's FEET follow: a line, steps (or the line through their
 * nosings, by `hop`), or an arc topping out `apex` above the higher end. Heights come from the
 * profile and not from the world, so a graph is only right where its author measured it, and
 * `nav_graph_check` walks every profile through a probe the app supplies to say where it is
 * not.
 *
 * NO GL, NO PHYSICS, NO CLOCK and no game type reach this file.
 */

#include <stdbool.h>
#include <stdint.h>

#include <cglm/cglm.h>

#define NAV_KIND_MAX  16
#define NAV_NAME_MAX  32
#define NAV_ROUTE_MAX 96

typedef enum NavShape {
    NAV_SHAPE_LINE,  // straight from place to place
    NAV_SHAPE_STEPS, // a straight flight: level, risers `going` apart, level again
    NAV_SHAPE_ARC,   // a jump: up from one end, over, down to the other
} NavShape;

typedef enum NavDrive {
    NAV_DRIVE_DISTANCE, // the app advances a follower by metres along the profile
    NAV_DRIVE_PROGRESS, // the app sets how far through the link a follower is, 0..1
} NavDrive;

// A kind of link, which the app numbers 0..NAV_KIND_MAX-1 and describes once.
typedef struct NavKind {
    const char* name; // borrowed; for logs
    float cost_scale; // a link's cost is its length times this; 0 is 1
    NavDrive drive;
    bool smooth; // the corner between two links of smoothed kinds is rounded
} NavKind;

typedef struct NavNode {
    char name[NAV_NAME_MAX];
    vec3 position; // where the agent's feet are when it stands here
    float radius;  // clear ground round it, which a corner is rounded inside
    uint32_t tags; // the app's
} NavNode;

// A link's shape, as an app states it. Unused fields are ignored.
typedef struct NavShapeDesc {
    NavShape shape;
    float rise;   // STEPS: one riser's height
    float going;  // STEPS: one tread's depth
    float nosing; // STEPS: how far along the first riser is from the start
    float hop;    // STEPS: 1 follows the line through the nosings, 0 the treads themselves
    float apex;   // ARC: how far the top of the arc is above the higher end
} NavShapeDesc;

typedef struct NavLink {
    int from, to; // nodes
    int kind;
    NavShapeDesc shape;
    float length; // the profile's length: horizontal for steps, chord for a line or an arc
} NavLink;

// ENGINE-OWNED, every field: read them freely; places, links and kinds go in through the
// functions below.
typedef struct NavGraph {
    NavNode* nodes;
    int node_count, node_cap;
    NavLink* links;
    int link_count, link_cap;
    NavKind kinds[NAV_KIND_MAX];
} NavGraph;

NavGraph* create_nav_graph(void);
void free_nav_graph(NavGraph* graph);

// Describe kind `kind`. Links of a kind never described route at cost 1, by distance.
void nav_graph_set_kind(NavGraph* graph, int kind, const NavKind* desc);

// A place; its index, or -1 refused (a name in use, or out of memory).
int nav_graph_add_node(NavGraph* graph, const char* name, const vec3 position, float radius,
                       uint32_t tags);
// A place's index by name, or -1.
int nav_graph_find(const NavGraph* graph, const char* name);

// A link one way, from `from` to `to`; its index, or -1 refused.
int nav_graph_link(NavGraph* graph, int from, int to, int kind, const NavShapeDesc* shape);
// A link each way: the second is the first walked backwards, its steps' first riser measured
// from the other end. The first's index, or -1.
int nav_graph_link_both(NavGraph* graph, int a, int b, int kind, const NavShapeDesc* shape);

// Where a link's profile is a fraction u (0..1) along it.
void nav_link_point(const NavGraph* graph, int link, float u, vec3 out);
// Which way a link runs over the ground: yaw about +y from its start to its end, 0 facing +z.
float nav_link_heading(const NavGraph* graph, int link);
// How long a body takes to fly an ARC link's arc under `gravity`, up from one end to its top and
// down to the other: the time a jump's clip has to spend in the air.
float nav_link_flight_time(const NavGraph* graph, int link, float gravity);

// What a route may use. Zero is everything.
typedef struct NavQuery {
    uint32_t kinds; // bit k allows kind k; 0 allows all
} NavQuery;

typedef struct NavRoute {
    int links[NAV_ROUTE_MAX];
    int count;
    float cost;
} NavRoute;

// The cheapest route from `from` to `to` under `query` (NULL allows everything), ties broken
// by node index so a route is the same on every run. False when there is none, or it is
// longer than NAV_ROUTE_MAX links.
bool nav_graph_route(const NavGraph* graph, int from, int to, const NavQuery* query, NavRoute* out);

// Where a follower is.
typedef struct NavSample {
    int link;       // the link it is on, -1 once arrived
    int kind;       // that link's kind
    float progress; // 0..1 along it
    vec3 position;  // the feet, after the profile and the corner rounding
    vec3 tangent;   // unit, the way it is going
    float heading;  // yaw about +y of the tangent's level part, 0 facing +z
    float turn;     // the turn at the end of this link, radians, left positive
    bool arrived;
} NavSample;

// ENGINE-OWNED, every field: a follower is moved by the functions below and read freely.
typedef struct NavFollower {
    const NavGraph* graph;
    NavRoute route;
    NavQuery query; // what its route may use
    int leg;        // index into route.links
    float along;    // metres along the current link, or 0..1 on a PROGRESS link
    int at;         // the node it stands at once arrived, or last passed
    int goal;
    bool arrived;
} NavFollower;

// Start a route from `from` to `to`. False when there is none, and the follower stands at
// `from`, arrived.
bool nav_follower_start(NavFollower* f, const NavGraph* graph, int from, int to,
                        const NavQuery* query);

// Along a DISTANCE link by `metres`, on into the next while the route goes on through
// DISTANCE links. A follower that reaches a PROGRESS link waits at its start for
// nav_follower_set_progress; what is left over is spent.
void nav_follower_advance(NavFollower* f, float metres);

// On a PROGRESS link, how far through it the follower is; 1 or more finishes the link.
void nav_follower_set_progress(NavFollower* f, float progress);

void nav_follower_sample(const NavFollower* f, NavSample* out);

// The route a replan to `to` under `query` would take, without taking it: from a place the
// follower has arrived at, the route from there; from part way along a link, the rest of that
// link and on -- or, part way along a level link driven by distance that runs both ways, back
// along it, when that is the cheaper way. A flight or a jump is always finished first. `out`'s
// first link is the one the follower is on or turns back along, and its cost is counted from
// where the follower stands. False when there is no route.
bool nav_follower_plan(const NavFollower* f, int to, const NavQuery* query, NavRoute* out);

// Go somewhere else instead, by nav_follower_plan's route, under `query` from now on (NULL
// keeps the follower's own). False when there is no route, and the follower keeps the one it
// had.
bool nav_follower_replan(NavFollower* f, int to, const NavQuery* query);

// The graph against a world. Every link's profile -- once for a link and its way back -- is
// walked in steps of `step` metres, `lift` above the feet, and each step is a segment `probe` is
// asked about: true is blocked. Each blocked link is logged by its places' names. Returns how
// many were blocked.
typedef bool (*NavProbeFn)(const vec3 a, const vec3 b, void* user);
int nav_graph_check(const NavGraph* graph, NavProbeFn probe, void* user, float lift, float step);

#endif // _NAV_GRAPH_H_
