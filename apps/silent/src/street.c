#include <math.h>

#include "cetra/ies.h"
#include "cetra/light.h"

#include "home.h"
#include "houses.h"
#include "land.h"
#include "layout.h"
#include "mats.h"
#include "street.h"

// Relative to the repository root, where every app in this tree is run from.
#define STREET_LAMP_IES "assets/ies/silent_street_lamp.ies"

/*
 * The fog. Thick enough that the far side of the street is a suggestion at
 * night -- lit windows and lamp halos, the houses themselves mostly gone --
 * and the road's ends vanish. Extinction per metre.
 */
#define FOG_NIGHT   0.09f
#define FOG_DAY     0.14f
#define FOG_FEATHER 3.0f

void street_ground(Kit* kit, int mat, float x0, float x1, float z0, float z1, float top) {
    kit_frame_box(kit, &KIT_WORLD, mat, x0, x1, top - GROUND_DEPTH, top, z0, z1, true);
}

// A strip of it the length of the street.
static void ground_strip(Kit* kit, int mat, float z0, float z1, float top) {
    street_ground(kit, mat, -STREET_HALF_LEN, STREET_HALF_LEN, z0, z1, top);
}

/*
 * A street lamp at (x, z) whose arm reaches over the road: a rusted post, the
 * arm and its head, the lens glowing under it, and at night a light there. A
 * dead lamp keeps its shape and loses its light.
 *
 * A POINT light shaped by `profile`, not the spot a lamp is: the fog scatters
 * point lights and only the scene's first spot (the flashlight), so a spot lamp
 * lights its pool and leaves the air round it dark. The profile is what makes
 * it a lamp rather than a bare bulb -- it sends the light down, so the fog
 * shows a bell of lit air under the head instead of a ball round it, and the
 * road gets the pool the air implies. No shadows: a point light's map is six
 * layers of a pool of eight.
 */
Light* street_lamp(Kit* kit, Scene* scene, float x, float y, float z, float yaw, bool night,
                   bool dead, int profile) {
    const KitFrame f = {{x, y, z}, yaw};
    kit_frame_prism(kit, &f, MAT_LAMP_POST, 0.0f, 0.0f, 0.0f, 0.5f, 0.12f, 8);
    kit_frame_prism(kit, &f, MAT_LAMP_POST, 0.0f, 0.0f, 0.5f, 5.2f, 0.07f, 8);
    kit_frame_box(kit, &f, MAT_LAMP_POST, -0.04f, 0.04f, 5.0f, 5.08f, 0.0f, 1.3f, false);
    kit_frame_box(kit, &f, MAT_LAMP_POST, -0.18f, 0.18f, 4.92f, 5.06f, 1.1f, 1.6f, false);
    kit_frame_box(kit, &f, dead ? MAT_BLACK : MAT_LAMP_GLOW, -0.14f, 0.14f, 4.9f, 4.92f, 1.15f,
                  1.55f, false);
    kit_collider(kit, (vec3){x, y + 1.5f, z}, (vec3){0.12f, 1.5f, 0.12f}, 0.0f);
    if (!night || dead)
        return NULL;
    vec3 pos = {0.0f, 0.0f, 0.0f};
    kit_frame_point(&f, 0.0f, 4.8f, 1.35f, pos);
    LightDesc desc = {.name = "street_lamp",
                      .type = LIGHT_POINT,
                      .position = {pos[0], pos[1], pos[2]},
                      .color = {0.80f, 0.95f, 0.90f},
                      .intensity = 3000.0f, // candela: an old mercury lamp
                      .range = 12.0f};
    Light* light = create_light(&desc);
    light->ies_profile = profile;
    scene_add_light(scene, light);
    return light;
}

FailingLamp failing_lamp(Light* light, unsigned int salt) {
    return (FailingLamp){light, light ? light->intensity : 0.0f, salt};
}

// Out for a beat now and then, and buzzing between, in a pattern hashed from the time so it never
// settles into a rhythm.
static float failing_level(double time, unsigned int salt) {
    const unsigned int beat = (unsigned int)(time * 9.0) + salt * 7919u;
    unsigned int h = beat * 2654435761u;
    h ^= h >> 15;
    const unsigned int stretch = (unsigned int)(time * 0.6) + salt * 104729u;
    unsigned int s = stretch * 2246822519u;
    s ^= s >> 13;
    const bool out = (h & 0xffu) < 70u || (s & 0xffu) < 50u;
    return out ? 0.04f : 1.0f;
}

void failing_lamp_update(const FailingLamp* lamp, double time) {
    if (lamp->light)
        lamp->light->intensity = failing_level(time, lamp->salt) * lamp->intensity;
}

int street_lamp_profile(Scene* scene, bool night) {
    // A missing profile logs by name and gives -1, which leaves the lamps bare bulbs rather
    // than dark.
    if (!night)
        return -1;
    if (!scene->ies_library)
        scene->ies_library = create_ies_library();
    return ies_library_load(scene->ies_library, STREET_LAMP_IES);
}

// Wooden utility poles down the far side, strung with two wires: on the sidewalk, in front of
// the terrace's wall, the first on the corner of the cross street, from where the crossroads
// carries the wires on.
static void poles(Kit* kit) {
    const float xs[] = {POLE_WEST_X, -30.0f, -10.0f, 10.0f, 30.0f};
    const int n = (int)(sizeof(xs) / sizeof(xs[0]));
    for (int i = 0; i < n; i++) {
        kit_prism(kit, MAT_POLE, xs[i], POLE_Z, 0.0f, 8.5f, 0.12f, 8, true);
        kit_box(kit, MAT_POLE, (vec3){xs[i], 8.0f, POLE_Z}, (vec3){0.06f, 0.05f, 0.8f}, 0.0f,
                false);
    }
    // Pole to pole, and on to the street's east end.
    for (int i = 0; i < n; i++) {
        const float x0 = xs[i];
        const float x1 = i + 1 < n ? xs[i + 1] : STREET_HALF_LEN;
        for (int w = -1; w <= 1; w += 2)
            kit_prism_lying(
                kit, MAT_BLACK,
                (vec3){0.5f * (x0 + x1), POLE_WIRE_Y, POLE_Z + POLE_WIRE_OFF * (float)w},
                0.5f * (x1 - x0), 0.012f, 4, true);
    }
}

void street_car(Kit* kit, float x, float z, bool police) {
    const float y = ROAD_Y;
    const int body = police ? MAT_BLACK : MAT_CAR;
    kit_box(kit, body, (vec3){x, y + 0.62f, z}, (vec3){2.15f, 0.3f, 0.86f}, 0.0f, true);
    kit_box(kit, MAT_DARK_GLASS, (vec3){x - 0.25f, y + 1.13f, z}, (vec3){1.05f, 0.22f, 0.8f}, 0.0f,
            false);
    kit_box(kit, body, (vec3){x - 0.25f, y + 1.37f, z}, (vec3){1.0f, 0.03f, 0.78f}, 0.0f, false);
    if (police) {
        // White doors either side, and the bar of lamps across the roof, out.
        for (int s = -1; s <= 1; s += 2)
            kit_box(kit, MAT_TRIM, (vec3){x - 0.1f, y + 0.66f, z + 0.865f * (float)s},
                    (vec3){1.0f, 0.22f, 0.006f}, 0.0f, false);
        kit_box(kit, MAT_DARK_GLASS, (vec3){x - 0.25f, y + 1.47f, z}, (vec3){0.12f, 0.07f, 0.62f},
                0.0f, false);
    }
    for (int i = 0; i < 4; i++) {
        const float wx = x + ((i & 1) ? 1.35f : -1.35f);
        const float wz = z + ((i & 2) ? 0.8f : -0.8f);
        kit_prism_lying(kit, MAT_BLACK, (vec3){wx, y + 0.33f, wz}, 0.11f, 0.33f, 10, false);
    }
    // Headlamps and tail lamps, unlit.
    kit_box(kit, MAT_CERAMIC, (vec3){x + 2.16f, y + 0.72f, z + 0.6f}, (vec3){0.02f, 0.07f, 0.14f},
            0.0f, false);
    kit_box(kit, MAT_CERAMIC, (vec3){x + 2.16f, y + 0.72f, z - 0.6f}, (vec3){0.02f, 0.07f, 0.14f},
            0.0f, false);
}

/*
 * The fog as boxes that together fill the world but stop at the two houses (spec 13.25): the
 * world's rectangle, cut into columns at each house's sides, a column holding a house split
 * round it. Two houses side by side along X make seven boxes, under the scene's eight.
 *
 * Each box's density ramps in over the feather from every face, so where two meet they overlap
 * by exactly one feather: one ramps down as the other ramps up and the sum stays level, where
 * boxes merely touching would leave a trough of clear air along the seam. A face against a house
 * is not grown: the hole is where the house is, and the ramp runs inside the box toward it.
 */
typedef struct FogHole {
    float x0, x1, z0, z1;
} FogHole;

static void fog_box(Scene* scene, float density, float x0, float x1, float z0, float z1) {
    const float top = 40.0f;     // high enough that the sky is fogged out too
    const float bottom = -60.0f; // and low enough to fill the chasm (spec 13.35)
    FogVolume v = {
        .center = {0.5f * (x0 + x1), 0.5f * (top + bottom), 0.5f * (z0 + z1)},
        .half_extent = {0.5f * (x1 - x0), 0.5f * (top - bottom) + 1.0f, 0.5f * (z1 - z0)},
        .density = density,
        .feather = FOG_FEATHER,
        .tint = {1.0f, 1.0f, 1.0f}};
    scene_add_fog_volume(scene, &v);
}

// The Gothic plan's footprint with the fog kept off it, the tower included -- it stands out past
// the front and the west side, and a fog volume does not stop at a wall: inside one the study
// would be full of it.
static FogHole mansion_hole(void) {
    const float gap = -(TOWER_X - TOWER_APOTHEM) + 0.7f;
    return (FogHole){-gap + MANSION_X, gap + MANSION_X, TOWER_Z - TOWER_APOTHEM - 0.7f + MANSION_Z,
                     HOUSE_BACK_Z + 1.0f + MANSION_Z};
}

// The home's, which has no tower: the walls with a margin, and the porch left in the fog.
static FogHole home_hole(void) {
    return (FogHole){HOUSE_X0 - 0.7f, HOUSE_X1 + 0.7f, HOUSE_FRONT_Z - 0.4f, HOUSE_BACK_Z + 0.7f};
}

static void fog(Scene* scene, bool night) {
    const float density = night ? FOG_NIGHT : FOG_DAY;
    const float F = 0.5f * FOG_FEATHER;
    // West across the chasm, so its far side is never clear air, and on past the lake valley
    // (spec 13.41).
    const float wx0 = VALLEY_X0 - 30.0f, wx1 = WORLD_X1 + 20.0f, wz0 = WORLD_Z0 - 20.0f,
                wz1 = VALLEY_Z1 + 20.0f;
    // West to east, which is the order the columns are cut in.
    const FogHole holes[2] = {home_hole(), mansion_hole()};
    float x = wx0;
    for (int h = 0; h <= 2; h++) {
        const float next = h < 2 ? holes[h].x0 : wx1;
        // The open column up to the next house, and then the house's own, split round it.
        fog_box(scene, density, x - (h > 0 ? F : 0.0f), next + (h < 2 ? F : 0.0f), wz0, wz1);
        if (h == 2)
            break;
        const FogHole* o = &holes[h];
        fog_box(scene, density, o->x0 - F, o->x1 + F, wz0, o->z0);
        fog_box(scene, density, o->x0 - F, o->x1 + F, o->z1, wz1);
        x = o->x1;
    }
}

void street_build(Kit* kit, Scene* scene, unsigned int seed, bool night, bool fogged,
                  StreetPlots* plots) {
    const float kerb = ROAD_HALF_WIDTH;
    const float walk = STREET_HALF_WIDTH;
    ground_strip(kit, MAT_ASPHALT, -kerb, kerb, ROAD_Y);
    ground_strip(kit, MAT_CONCRETE, kerb, walk, 0.0f);
    ground_strip(kit, MAT_CONCRETE, -walk, -kerb, 0.0f);
    // Our side's yards, cut away round the basement under our house (spec 13.31), to the back
    // fences. The far side's lots are the terrace's.
    ground_strip(kit, MAT_DIRT, walk, DIG_Z0, 0.0f);
    ground_strip(kit, MAT_DIRT, DIG_Z1, BACK_FENCE_Z, 0.0f);
    street_ground(kit, MAT_DIRT, -STREET_HALF_LEN, DIG_X0, DIG_Z0, DIG_Z1, 0.0f);
    street_ground(kit, MAT_DIRT, DIG_X1, STREET_HALF_LEN, DIG_Z0, DIG_Z1, 0.0f);

    // Our path from the sidewalk to the porch steps.
    kit_box(kit, MAT_CONCRETE,
            (vec3){0.5f * (PATH_X0 + PATH_X1), 0.01f, 0.5f * (walk + PORCH_Z0 - 0.32f)},
            (vec3){0.5f * (PATH_X1 - PATH_X0), 0.01f, 0.5f * (PORCH_Z0 - 0.32f - walk)}, 0.0f,
            false);

    // The neighbours: this side of the street either side of us, fronts in line with ours, with
    // a vacant lot at each end, and the far side facing back from its terrace's lots, one a lot.
    KitRng rng = {seed * 2246822519u + 3266489917u};
    *plots = (StreetPlots){0};
    for (int lot = 1; lot < NEAR_LOTS - 1; lot++) {
        if (lot == HOME_LOT)
            continue;
        const KitFrame f = {{near_lot_house_x(lot), 0.0f, HOUSE_FRONT_Z}, GLM_PIf};
        house_neighbour(kit, &f, &rng, night, &plots->near[lot]);
        // A path from the sidewalk to the door, running on under its porch or up to its step.
        const float* door = plots->near[lot].door;
        kit_box(kit, MAT_CONCRETE, (vec3){door[0], 0.01f, 0.5f * (walk + door[2])},
                (vec3){0.55f, 0.01f, 0.5f * (door[2] - walk)}, 0.0f, false);
    }
    plots->near[HOME_LOT] = (HousePlot){
        {0.0f, 0.0f, HOUSE_FRONT_Z}, HOUSE_OUT_X0, HOUSE_OUT_X1, HOUSE_OUT_Z0, HOUSE_OUT_Z1};
    for (int lot = 0; lot < TERRACE_LOTS; lot++) {
        const KitFrame f = {{far_lot_house_x(lot), land_terrace_lot_height(lot), FAR_HOUSE_FRONT_Z},
                            0.0f};
        house_neighbour(kit, &f, &rng, night, &plots->far[lot]);
    }

    const int profile = street_lamp_profile(scene, night);
    // Four down our side and three down the far side, one of them dead.
    static const struct {
        float x;
        float side; // +1 our side of the road, -1 the far side
        bool dead;
    } LAMPS[] = {{-24.0f, 1.0f, false}, {-8.0f, 1.0f, false},   {8.0f, 1.0f, false},
                 {24.0f, 1.0f, false},  {-16.0f, -1.0f, false}, {0.0f, -1.0f, false},
                 {16.0f, -1.0f, true}};
    const float lamp_z = kerb + 1.4f;
    for (size_t i = 0; i < sizeof(LAMPS) / sizeof(LAMPS[0]); i++)
        street_lamp(kit, scene, LAMPS[i].x, 0.0f, LAMPS[i].side * lamp_z,
                    LAMPS[i].side > 0.0f ? GLM_PIf : 0.0f, night, LAMPS[i].dead, profile);

    poles(kit);
    street_car(kit, 5.0f, -(kerb - 1.1f), false);
    // The mailbox at the end of our path.
    kit_box(kit, MAT_POLE, (vec3){-1.8f, 0.55f, walk + 0.4f}, (vec3){0.04f, 0.55f, 0.04f}, 0.0f,
            false);
    kit_box(kit, MAT_LAMP_POST, (vec3){-1.8f, 1.18f, walk + 0.4f}, (vec3){0.1f, 0.1f, 0.24f}, 0.0f,
            false);

    // The world's edge: walls the fog hides, so a walk ends in grey rather than off the end of
    // the ground. Round the street, the woods behind both sides and the hill the drive climbs,
    // tall enough for the grounds up there and the woods' climb. Behind the street it stands well
    // inside the woods (spec 13.35), so the trees go on past it.
    const float h = 14.0f, y = 9.0f;
    const float x0 = -STREET_HALF_LEN + 1.0f, x1 = WORLD_X1 - 1.0f;
    const float z0 = WORLD_Z0 + 1.0f, z1 = WORLD_Z1 - 1.0f;
    struct {
        float ax, az, bx, bz;
    } const edges[] = {
        // Behind the far lots and behind ours, where the woods run on west past the crossroads;
        // the crossroads itself is closed by its barricades and the chasm's lip.
        {x0, WOODS_EDGE_Z0, x0, TERRACE_BACK_Z},
        {x0, BACK_FENCE_Z, x0, WOODS_EDGE_Z1},
        {x0, WOODS_EDGE_Z1, WOODS_EAST_X, WOODS_EDGE_Z1}, // in the woods behind our side
        {WOODS_EAST_X, WOODS_EDGE_Z1, WOODS_EAST_X, z1},  // and their east side
        {WOODS_EAST_X, z1, x1, z1},                       // behind the mansion
        {x1, z1, x1, z0},                                 // the hill's east side
        {x1, z0, WOODS_EAST_X, z0},                       // the hill's north side
        {WOODS_EAST_X, z0, WOODS_EAST_X, WOODS_EDGE_Z0},  // the far woods' east side
        {WOODS_EAST_X, WOODS_EDGE_Z0, x0, WOODS_EDGE_Z0}, // in the woods behind them
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        const float cx = 0.5f * (edges[i].ax + edges[i].bx),
                    cz = 0.5f * (edges[i].az + edges[i].bz);
        const float hx = 0.5f * fabsf(edges[i].bx - edges[i].ax) + 0.5f;
        const float hz = 0.5f * fabsf(edges[i].bz - edges[i].az) + 0.5f;
        kit_collider(kit, (vec3){cx, y, cz}, (vec3){hx, h, hz}, 0.0f);
    }
    // Round the lake valley (spec 13.41), down past its water: on south behind the woods behind
    // ours, across its far end, and up its west side to the chasm's south lip.
    const float vh = 16.0f, vy = -4.0f;
    struct {
        float ax, az, bx, bz;
    } const valley[] = {
        {x0, WOODS_EDGE_Z1, x0, VALLEY_WALL_Z1},
        {x0, VALLEY_WALL_Z1, VALLEY_WALL_X0, VALLEY_WALL_Z1},
        {VALLEY_WALL_X0, VALLEY_WALL_Z1, VALLEY_WALL_X0, RIDGE_Z - 2.0f},
    };
    for (size_t i = 0; i < sizeof(valley) / sizeof(valley[0]); i++) {
        const float cx = 0.5f * (valley[i].ax + valley[i].bx),
                    cz = 0.5f * (valley[i].az + valley[i].bz);
        const float hx = 0.5f * fabsf(valley[i].bx - valley[i].ax) + 0.5f;
        const float hz = 0.5f * fabsf(valley[i].bz - valley[i].az) + 0.5f;
        kit_collider(kit, (vec3){cx, vy, cz}, (vec3){hx, vh, hz}, 0.0f);
    }

    if (fogged)
        fog(scene, night);
    if (!night)
        mats_daytime(kit);
}
