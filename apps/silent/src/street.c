#include <math.h>

#include "cetra/ies.h"
#include "cetra/light.h"

#include "houses.h"
#include "layout.h"
#include "mats.h"
#include "street.h"

// How far the yards reach behind the houses on either side. Past this is fog.
#define YARD_DEPTH   40.0f
#define GROUND_DEPTH 0.4f // how thick the ground boxes are, below their tops

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

// A strip of ground the length of the street, from z0 to z1, its top at `top`.
static void ground(Kit* kit, int mat, float z0, float z1, float top) {
    kit_frame_box(kit, &KIT_WORLD, mat, -STREET_HALF_LEN, STREET_HALF_LEN, top - GROUND_DEPTH, top,
                  z0, z1, true);
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

int street_lamp_profile(Scene* scene, bool night) {
    // A missing profile logs by name and gives -1, which leaves the lamps bare bulbs rather
    // than dark.
    if (!night)
        return -1;
    if (!scene->ies_library)
        scene->ies_library = create_ies_library();
    return ies_library_load(scene->ies_library, STREET_LAMP_IES);
}

// Wooden utility poles down the far side, strung with two wires.
static void poles(Kit* kit) {
    const float z = -(ROAD_HALF_WIDTH + SIDEWALK_WIDTH + 0.6f);
    const float xs[] = {-30.0f, -10.0f, 10.0f, 30.0f};
    const int n = (int)(sizeof(xs) / sizeof(xs[0]));
    for (int i = 0; i < n; i++) {
        kit_prism(kit, MAT_POLE, xs[i], z, 0.0f, 8.5f, 0.12f, 8, true);
        kit_box(kit, MAT_POLE, (vec3){xs[i], 8.0f, z}, (vec3){0.06f, 0.05f, 0.8f}, 0.0f, false);
    }
    // From the world's edge, pole to pole, to the other edge.
    for (int i = -1; i < n; i++) {
        const float x0 = i < 0 ? -STREET_HALF_LEN : xs[i];
        const float x1 = i + 1 < n ? xs[i + 1] : STREET_HALF_LEN;
        for (int w = -1; w <= 1; w += 2)
            kit_prism_lying(kit, MAT_BLACK, (vec3){0.5f * (x0 + x1), 7.95f, z + 0.65f * (float)w},
                            0.5f * (x1 - x0), 0.012f, 4, true);
    }
}

// A boxy sedan parked at the far kerb, its windows dark.
static void car(Kit* kit, float x, float z) {
    const float y = ROAD_Y;
    kit_box(kit, MAT_CAR, (vec3){x, y + 0.62f, z}, (vec3){2.15f, 0.3f, 0.86f}, 0.0f, true);
    kit_box(kit, MAT_DARK_GLASS, (vec3){x - 0.25f, y + 1.13f, z}, (vec3){1.05f, 0.22f, 0.8f}, 0.0f,
            false);
    kit_box(kit, MAT_CAR, (vec3){x - 0.25f, y + 1.37f, z}, (vec3){1.0f, 0.03f, 0.78f}, 0.0f, false);
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

// A picket-less fence along a yard's front: posts and two rails.
static void fence(Kit* kit, float x0, float x1, float z) {
    for (float x = x0; x <= x1 + 0.01f; x += 2.0f)
        kit_box(kit, MAT_PORCH, (vec3){x, 0.5f, z}, (vec3){0.05f, 0.5f, 0.05f}, 0.0f, false);
    const float mid = 0.5f * (x0 + x1), half = 0.5f * (x1 - x0);
    kit_box(kit, MAT_PORCH, (vec3){mid, 0.75f, z}, (vec3){half, 0.04f, 0.02f}, 0.0f, false);
    kit_box(kit, MAT_PORCH, (vec3){mid, 0.4f, z}, (vec3){half, 0.04f, 0.02f}, 0.0f, false);
    kit_collider(kit, (vec3){mid, 0.5f, z}, (vec3){half, 0.5f, 0.06f}, 0.0f);
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
    const float top = 40.0f; // high enough that the sky is fogged out too
    FogVolume v = {.center = {0.5f * (x0 + x1), 0.5f * top, 0.5f * (z0 + z1)},
                   .half_extent = {0.5f * (x1 - x0), 0.5f * top + 1.0f, 0.5f * (z1 - z0)},
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
    const float wx0 = -60.0f, wx1 = WORLD_X1 + 20.0f, wz0 = -60.0f, wz1 = WORLD_Z1 + 20.0f;
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

void street_build(Kit* kit, Scene* scene, unsigned int seed, bool night, bool fogged) {
    const float kerb = ROAD_HALF_WIDTH;
    const float walk = ROAD_HALF_WIDTH + SIDEWALK_WIDTH;
    ground(kit, MAT_ASPHALT, -kerb, kerb, ROAD_Y);
    ground(kit, MAT_CONCRETE, kerb, walk, 0.0f);
    ground(kit, MAT_CONCRETE, -walk, -kerb, 0.0f);
    ground(kit, MAT_DIRT, walk, walk + YARD_DEPTH, 0.0f);
    ground(kit, MAT_DIRT, -walk - YARD_DEPTH, -walk, 0.0f);

    // Our path from the sidewalk to the porch steps.
    kit_box(kit, MAT_CONCRETE,
            (vec3){0.5f * (PATH_X0 + PATH_X1), 0.01f, 0.5f * (walk + PORCH_Z0 - 0.32f)},
            (vec3){0.5f * (PATH_X1 - PATH_X0), 0.01f, 0.5f * (PORCH_Z0 - 0.32f - walk)}, 0.0f,
            false);

    // The neighbours: this side of the street either side of us, fronts in
    // line with ours, and the far side facing back.
    KitRng rng = {seed * 2246822519u + 3266489917u};
    const float near_side[] = {-28.0f, -14.0f, 14.0f, 28.0f};
    for (int i = 0; i < 4; i++) {
        const KitFrame f = {{near_side[i], 0.0f, HOUSE_FRONT_Z}, GLM_PIf};
        house_neighbour(kit, &f, &rng, night);
    }
    const float far_side[] = {-35.0f, -21.0f, -7.0f, 7.0f, 21.0f, 35.0f};
    for (int i = 0; i < 6; i++) {
        const KitFrame f = {{far_side[i], 0.0f, -HOUSE_FRONT_Z}, 0.0f};
        house_neighbour(kit, &f, &rng, night);
    }
    fence(kit, -34.5f, -8.0f, walk + 1.6f);
    fence(kit, 8.0f, 34.5f, walk + 1.6f);

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
    car(kit, 5.0f, -(kerb - 1.1f));
    // The mailbox at the end of our path.
    kit_box(kit, MAT_POLE, (vec3){-1.8f, 0.55f, walk + 0.4f}, (vec3){0.04f, 0.55f, 0.04f}, 0.0f,
            false);
    kit_box(kit, MAT_LAMP_POST, (vec3){-1.8f, 1.18f, walk + 0.4f}, (vec3){0.1f, 0.1f, 0.24f}, 0.0f,
            false);

    // The world's edge: walls the fog hides, so a walk ends in grey rather than off the end of
    // the ground. Round the street and, past its east end, round the hill the drive climbs
    // (spec 13.25), so they stand tall enough for the grounds up there.
    const float h = 11.0f, y = 9.0f;
    const float zmax = walk + YARD_DEPTH;
    const float x0 = -STREET_HALF_LEN + 1.0f, x1 = WORLD_X1 - 1.0f;
    const float zs = zmax - 1.0f, z0 = -zmax + 1.0f, z1 = WORLD_Z1 - 1.0f;
    struct {
        float ax, az, bx, bz;
    } const edges[] = {
        {x0, z0, x0, zs},                           // the street's west end
        {x0, zs, STREET_HALF_LEN, zs},              // behind our side's yards
        {STREET_HALF_LEN, zs, STREET_HALF_LEN, z1}, // the hill's west side, past them
        {STREET_HALF_LEN, z1, x1, z1},              // behind the mansion
        {x1, z1, x1, z0},                           // the hill's east side
        {x1, z0, x0, z0},                           // behind the far side's yards, and on
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        const float cx = 0.5f * (edges[i].ax + edges[i].bx),
                    cz = 0.5f * (edges[i].az + edges[i].bz);
        const float hx = 0.5f * fabsf(edges[i].bx - edges[i].ax) + 0.5f;
        const float hz = 0.5f * fabsf(edges[i].bz - edges[i].az) + 0.5f;
        kit_collider(kit, (vec3){cx, y, cz}, (vec3){hx, h, hz}, 0.0f);
    }

    if (fogged)
        fog(scene, night);
    if (!night)
        mats_daytime(kit);
}
