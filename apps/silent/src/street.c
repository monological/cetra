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
static void lamp(Kit* kit, Scene* scene, float x, float z, bool night, bool dead, int profile) {
    const KitFrame f = {{x, 0.0f, z}, z > 0.0f ? GLM_PIf : 0.0f};
    kit_frame_prism(kit, &f, MAT_LAMP_POST, 0.0f, 0.0f, 0.0f, 0.5f, 0.12f, 8);
    kit_frame_prism(kit, &f, MAT_LAMP_POST, 0.0f, 0.0f, 0.5f, 5.2f, 0.07f, 8);
    kit_frame_box(kit, &f, MAT_LAMP_POST, -0.04f, 0.04f, 5.0f, 5.08f, 0.0f, 1.3f, false);
    kit_frame_box(kit, &f, MAT_LAMP_POST, -0.18f, 0.18f, 4.92f, 5.06f, 1.1f, 1.6f, false);
    kit_frame_box(kit, &f, dead ? MAT_BLACK : MAT_LAMP_GLOW, -0.14f, 0.14f, 4.9f, 4.92f, 1.15f,
                  1.55f, false);
    kit_collider(kit, (vec3){x, 1.5f, z}, (vec3){0.12f, 1.5f, 0.12f}, 0.0f);
    if (!night || dead)
        return;
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
 * The fog as four overlapping boxes that together fill the street and every
 * yard but stop at the player's house. Each box's density ramps in over the
 * feather from every face, so where two meet they overlap by exactly one
 * feather: one ramps down as the other ramps up and the sum stays level,
 * where boxes merely touching would leave a trough of clear air along the seam.
 */
static void fog(Scene* scene, bool night) {
    const float density = night ? FOG_NIGHT : FOG_DAY;
    const float F = FOG_FEATHER, top = 40.0f; // high enough that the sky is fogged out too
    const float house_gap = 6.0f, front = HOUSE_FRONT_Z - 0.5f, back = HOUSE_BACK_Z + 1.0f;
    const float far = 45.0f, wide = 60.0f;
    const float boxes[4][4] = {
        // x0, x1, z0, z1
        {-wide, wide, -far, front},                 // the street and the far side
        {-wide, -house_gap, front - F, far},        // the yards to the left
        {house_gap, wide, front - F, far},          // and to the right
        {-house_gap - F, house_gap + F, back, far}, // behind the house
    };
    for (int i = 0; i < 4; i++) {
        const float* b = boxes[i];
        FogVolume v = {
            .center = {0.5f * (b[0] + b[1]), 0.5f * top, 0.5f * (b[2] + b[3])},
            .half_extent = {0.5f * (b[1] - b[0]), 0.5f * top + 1.0f, 0.5f * (b[3] - b[2])},
            .density = density,
            .feather = F,
            .tint = {1.0f, 1.0f, 1.0f}};
        scene_add_fog_volume(scene, &v);
    }
}

void street_build(Kit* kit, Scene* scene, unsigned int seed, bool night, Drips* drips) {
    const float kerb = ROAD_HALF_WIDTH;
    const float walk = ROAD_HALF_WIDTH + SIDEWALK_WIDTH;
    ground(kit, MAT_ASPHALT, -kerb, kerb, ROAD_Y);
    ground(kit, MAT_CONCRETE, kerb, walk, 0.0f);
    ground(kit, MAT_CONCRETE, -walk, -kerb, 0.0f);
    ground(kit, MAT_DIRT, walk, walk + YARD_DEPTH, 0.0f);
    ground(kit, MAT_DIRT, -walk - YARD_DEPTH, -walk, 0.0f);

    // Our path from the sidewalk to the porch steps.
    kit_box(kit, MAT_CONCRETE, (vec3){-0.8f, 0.01f, 0.5f * (walk + PORCH_Z0 - 0.32f)},
            (vec3){0.55f, 0.01f, 0.5f * (PORCH_Z0 - 0.32f - walk)}, 0.0f, false);

    // The neighbours: this side of the street either side of us, fronts in
    // line with ours, and the far side facing back.
    KitRng rng = {seed * 2246822519u + 3266489917u};
    const float near_side[] = {-28.0f, -14.0f, 14.0f, 28.0f};
    for (int i = 0; i < 4; i++) {
        const KitFrame f = {{near_side[i], 0.0f, HOUSE_FRONT_Z}, GLM_PIf};
        house_neighbour(kit, &f, &rng, night, drips);
    }
    const float far_side[] = {-35.0f, -21.0f, -7.0f, 7.0f, 21.0f, 35.0f};
    for (int i = 0; i < 6; i++) {
        const KitFrame f = {{far_side[i], 0.0f, -HOUSE_FRONT_Z}, 0.0f};
        house_neighbour(kit, &f, &rng, night, drips);
    }
    fence(kit, -34.5f, -8.0f, walk + 1.6f);
    fence(kit, 8.0f, 34.5f, walk + 1.6f);

    // A missing profile logs by name and gives -1, which leaves the lamps bare
    // bulbs rather than dark.
    int profile = -1;
    if (night) {
        if (!scene->ies_library)
            scene->ies_library = create_ies_library();
        profile = ies_library_load(scene->ies_library, STREET_LAMP_IES);
    }
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
        lamp(kit, scene, LAMPS[i].x, LAMPS[i].side * lamp_z, night, LAMPS[i].dead, profile);

    poles(kit);
    car(kit, 5.0f, -(kerb - 1.1f));
    // The mailbox at the end of our path.
    kit_box(kit, MAT_POLE, (vec3){-1.8f, 0.55f, walk + 0.4f}, (vec3){0.04f, 0.55f, 0.04f}, 0.0f,
            false);
    kit_box(kit, MAT_LAMP_POST, (vec3){-1.8f, 1.18f, walk + 0.4f}, (vec3){0.1f, 0.1f, 0.24f}, 0.0f,
            false);

    // The world's edge: walls the fog hides, so a walk down the street ends in
    // grey rather than off the end of the ground.
    const float h = 3.0f;
    const float zmax = walk + YARD_DEPTH;
    kit_collider(kit, (vec3){-STREET_HALF_LEN + 1.0f, h, 0.0f}, (vec3){0.5f, h, zmax}, 0.0f);
    kit_collider(kit, (vec3){STREET_HALF_LEN - 1.0f, h, 0.0f}, (vec3){0.5f, h, zmax}, 0.0f);
    kit_collider(kit, (vec3){0.0f, h, zmax - 1.0f}, (vec3){STREET_HALF_LEN, h, 0.5f}, 0.0f);
    kit_collider(kit, (vec3){0.0f, h, -zmax + 1.0f}, (vec3){STREET_HALF_LEN, h, 0.5f}, 0.0f);

    fog(scene, night);
    if (!night)
        mats_lamps_out(kit);
}
