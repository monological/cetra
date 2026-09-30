#include <assert.h>
#include <stdio.h>

#include "cetra/material.h"
#include "cetra/program.h"
#include "cetra/texture.h"
#include "cetra/util.h"

#include "mats.h"

/*
 * The photo sets under assets/textures/silent/, as tools/fetch_textures.py
 * leaves them: CC0 scans from Poly Haven and ambientCG cut down to 256 square and stored
 * bottom row first, so V runs up a wall. Relative to the repository root,
 * which is where every app in this tree is run from.
 */
#define TEXTURE_DIR "assets/textures/silent"

// One surface, at its MatId's index in SPECS.
typedef struct MatSpec {
    const char* name; // the material's own name, for the GUI's editor
    const char* set;  // the photo set's base name; NULL is a flat colour
    float tint[3];    // the albedo factor over the map
    float roughness;  // a factor over the map: under 1 is wetter
    float metallic;
    float repeat_m;    // metres one repeat covers
    bool surface_only; // take the set's normal and roughness, keep the tint as the colour
} MatSpec;

/*
 * Glass: what it transmits, how thick a path through it is, and the colour
 * that path leaves. The containers' contents are opaque meshes inside them,
 * which the refraction pass sees through the shell.
 */
typedef struct GlassSpec {
    MatId id;
    float transmission;
    float thickness;      // metres: about a jar's diameter
    float attenuation[3]; // what survives `distance` of glass
    float distance;
} GlassSpec;

static const GlassSpec GLASS[] = {
    {MAT_GLASS_AMBER, 0.95f, 0.1f, {0.80f, 0.50f, 0.22f}, 0.2f},
    {MAT_GLASS_CLEAR, 0.97f, 0.08f, {0.90f, 0.97f, 0.90f}, 0.5f},
    // A window pane is THIN glass: no volume, so no bend and no absorption,
    // only the tint and the smear.
    {MAT_WINDOW_GLASS, 0.9f, 0.0f, {1.0f, 1.0f, 1.0f}, 0.0f},
};

/*
 * Emitters that are decoration and not lamps: a lit window and a street lamp's
 * lens glow, but the light they throw is authored separately (or not at all),
 * so they are kept out of the engine's derived area panels -- which would
 * otherwise try to fit a rectangle to every lit window on the street at once.
 */
typedef struct GlowSpec {
    MatId id;
    float colour[3];
    float nits;
} GlowSpec;

static const GlowSpec GLOWS[] = {
    {MAT_WINDOW_LIT, {1.0f, 0.70f, 0.40f}, 30.0f},
    {MAT_LAMP_GLOW, {0.85f, 1.0f, 0.90f}, 4000.0f},
};

/*
 * Dirt round the edges (spec 13.8), for the surfaces that are furniture: every
 * box of these is a real object, so every edge the kit darkens is a real edge.
 * Walls are not here, and cannot be -- a wall is cut into slabs round its
 * openings, and the seams between slabs are edges nobody built.
 */
typedef struct GrimeSpec {
    MatId id;
    float strength;
} GrimeSpec;

static const GrimeSpec GRIME[] = {
    {MAT_ENAMEL, 0.75f}, {MAT_APPLIANCE, 0.6f}, {MAT_TRIM, 0.55f},
    {MAT_WOOD, 0.45f},   {MAT_TABLE, 0.5f},
};

static float grime_of(int id) {
    for (size_t g = 0; g < sizeof(GRIME) / sizeof(GRIME[0]); g++)
        if ((int)GRIME[g].id == id)
            return GRIME[g].strength;
    return 0.0f;
}

/*
 * The repeats follow each scan's real size where that reads right and depart
 * from it where the reference asks: the kitchen's 30 cm floor tiles are the
 * scan's 50 cm ones at a shorter repeat, and the backsplash is the SAME floor
 * set at 0.6 m, which makes its 15 cm glazed squares.
 */
static const MatSpec SPECS[MAT_COUNT] = {
    [MAT_PLASTER] = {"plaster", "grey_plaster_02", {0.80f, 0.90f, 0.82f}, 1.0f, 0.0f, 1.4f},
    [MAT_CEILING] = {"ceiling", "white_plaster_rough_02", {0.75f, 0.75f, 0.72f}, 1.0f, 0.0f, 1.5f},
    [MAT_KITCHEN_FLOOR] =
        {"kitchen_floor", "worn_tile_floor", {0.95f, 1.0f, 0.97f}, 0.32f, 0.0f, 1.25f},
    [MAT_WOOD_FLOOR] = {"wood_floor", "old_wood_floor", {1, 1, 1}, 0.8f, 0.0f, 2.0f},
    [MAT_BACKSPLASH] = {"backsplash", "worn_tile_floor", {0.88f, 0.98f, 1.02f}, 0.45f, 0.0f, 0.6f},
    [MAT_ENAMEL] = {"enamel", "rusty_metal_02", {0.88f, 0.9f, 0.84f}, 0.8f, 0.0f, 1.0f},
    // The dirty-white wall scan again, yellowed and matte: old enamel that has
    // gone grey with grime rather than to rust.
    [MAT_APPLIANCE] = {"appliance", "concrete_wall_003", {0.82f, 0.80f, 0.70f}, 1.0f, 0.0f, 1.5f},
    [MAT_TRIM] = {"trim", "concrete_wall_003", {0.82f, 0.86f, 0.80f}, 1.0f, 0.0f, 1.5f},
    [MAT_STEEL] = {"steel", "Metal009", {1, 1, 1}, 1.0f, 1.0f, 0.6f},
    [MAT_WOOD] = {"wood", "wood_table_worn", {1, 1, 1}, 1.0f, 0.0f, 0.8f},
    [MAT_SIDING] = {"siding", "white_planks_clean", {0.78f, 0.84f, 0.82f}, 1.0f, 0.0f, 1.8f},
    [MAT_SIDING_B] = {"siding_blue", "blue_painted_planks", {1, 1, 1}, 1.0f, 0.0f, 1.2f},
    [MAT_SIDING_C] =
        {"siding_ochre", "white_planks_clean", {0.86f, 0.76f, 0.56f}, 1.0f, 0.0f, 1.8f},
    [MAT_PORCH] = {"porch", "old_wood_floor", {0.75f, 0.78f, 0.8f}, 1.0f, 0.0f, 2.0f},
    [MAT_DIRT] = {"yard", "grass_ground", {1, 1, 1}, 1.0f, 0.0f, 2.5f},
    [MAT_ASPHALT] = {"asphalt", "asphalt_02", {1, 1, 1}, 1.0f, 0.0f, 3.0f},
    [MAT_CONCRETE] = {"sidewalk", "concrete_pavement", {1, 1, 1}, 1.0f, 0.0f, 1.8f},
    [MAT_ROOF] = {"roof", "roof_slates_02", {1, 1, 1}, 1.0f, 0.0f, 3.0f},
    [MAT_BRICK] = {"brick", "brick_wall_006", {1, 1, 1}, 1.0f, 0.0f, 3.0f},
    [MAT_RUG] = {"rug", "dirty_carpet", {1, 1, 1}, 1.0f, 0.0f, 0.6f},
    [MAT_TOWEL] = {"towel", "fabric_pattern_05", {1, 1, 1}, 1.0f, 0.0f, 0.5f},
    [MAT_PAPER] = {"paper", "Paper003", {0.86f, 0.80f, 0.64f}, 1.0f, 0.0f, 0.4f},
    // Glass takes the kitchen smear's roughness and relief, so it is smudged
    // rather than perfect, and its colour from the tint; see GLASS above.
    [MAT_GLASS_AMBER] = {"amber_glass", "Smear008", {0.90f, 0.62f, 0.36f}, 0.12f, 0.0f, 0.3f, true},
    [MAT_GLASS_CLEAR] = {"clear_glass", "Smear008", {0.94f, 1.0f, 0.95f}, 0.12f, 0.0f, 0.3f, true},
    // What is in the jars, off one granular scan: a red-brown sauce or spice,
    // pale grain, and something pickled.
    [MAT_CONTENTS] = {"contents_red", "grass_ground", {0.46f, 0.15f, 0.07f}, 0.5f, 0.0f, 0.25f},
    [MAT_CONTENTS_PALE] =
        {"contents_pale", "grass_ground", {1.0f, 0.86f, 0.62f}, 0.8f, 0.0f, 0.15f},
    [MAT_CONTENTS_GREEN] =
        {"contents_green", "grass_ground", {0.42f, 0.50f, 0.20f}, 0.4f, 0.0f, 0.3f},
    // Glazed and stained: the dirty-white wall scan at a small repeat, glossy.
    [MAT_CERAMIC] = {"ceramic", "concrete_wall_003", {1.0f, 1.0f, 0.97f}, 0.3f, 0.0f, 0.5f},
    [MAT_BLACK] = {"black_enamel", NULL, {0.03f, 0.03f, 0.03f}, 0.35f, 0.0f, 1.0f},
    [MAT_TABLE] = {"table_enamel", "PaintedMetal001", {0.50f, 0.58f, 0.66f}, 0.7f, 0.0f, 1.0f},
    [MAT_WINDOW_GLASS] =
        {"window_glass", "Smear008", {0.86f, 0.92f, 0.88f}, 0.25f, 0.0f, 0.6f, true},
    [MAT_DARK_GLASS] = {"dark_glass", "Smear008", {0.02f, 0.025f, 0.03f}, 0.15f, 0.0f, 0.6f, true},
    [MAT_WINDOW_LIT] = {"window_lit", "fabric_pattern_05", {0.9f, 0.85f, 0.7f}, 1.0f, 0.0f, 0.5f},
    [MAT_LAMP_GLOW] = {"lamp_glow", NULL, {0.9f, 0.95f, 0.9f}, 0.5f, 0.0f, 1.0f},
    [MAT_LAMP_POST] = {"lamp_post", "metal_plate_02", {0.7f, 0.72f, 0.7f}, 1.0f, 0.6f, 1.0f},
    [MAT_POLE] = {"utility_pole", "old_wood_floor", {0.55f, 0.52f, 0.5f}, 1.0f, 0.0f, 1.5f},
    [MAT_CAR] = {"car_paint", "rusty_metal_02", {0.45f, 0.14f, 0.11f}, 0.6f, 0.0f, 1.2f},
    [MAT_CARDBOARD] = {"cardboard", "Cardboard003", {0.85f, 0.80f, 0.70f}, 1.0f, 0.0f, 0.6f},
    // A washed-out green, glossy: the one saturated thing by the sink.
    [MAT_PLASTIC] = {"plastic", NULL, {0.30f, 0.42f, 0.26f}, 0.35f, 0.0f, 1.0f},
};

static Texture* load(TexturePool* pool, const char* set, const char* map, TextureDesc desc) {
    char file[128];
    snprintf(file, sizeof(file), "%s_%s.png", set, map);
    return texture_load_file(pool, file, desc);
}

_Static_assert(MAT_COUNT <= KIT_MAX_MATERIALS, "every MatId needs a kit slot");

void mats_register(Kit* kit, Engine* engine, Scene* scene) {
    // Registered in order into an empty kit, so the slot IS the MatId.
    assert(kit->material_count == 0);
    ShaderProgram* pbr = engine_get_program(engine, CETRA_PROGRAM_PBR);
    set_texture_pool_directory(scene->tex_pool, TEXTURE_DIR);
    const TextureDesc normal_desc = {
        .is_srgb = false, .alpha = TEXTURE_ALPHA_DATA, .use = TEXTURE_USE_NORMAL};
    for (int i = 0; i < MAT_COUNT; i++) {
        const MatSpec* s = &SPECS[i];
        Material* m = create_material();
        m->name = safe_strdup(s->name);
        glm_vec3_copy((float*)s->tint, m->albedo);
        m->roughness = s->roughness;
        m->metallic = s->metallic;
        material_set_program(m, pbr);
        // The pool caches by path, so a set two materials share loads once.
        if (s->set) {
            if (!s->surface_only)
                material_set_albedo_tex(
                    m, load(scene->tex_pool, s->set, "albedo", texture_desc(true)));
            material_set_normal_tex(m, load(scene->tex_pool, s->set, "normal", normal_desc));
            material_set_roughness_tex(m,
                                       load(scene->tex_pool, s->set, "rough", texture_desc(false)));
        }
        kit_material(kit, m, s->repeat_m, grime_of(i));
    }
    for (size_t g = 0; g < sizeof(GLASS) / sizeof(GLASS[0]); g++) {
        Material* m = kit->materials[GLASS[g].id];
        m->transmission = GLASS[g].transmission;
        m->ior = 1.5f;
        m->thickness = GLASS[g].thickness;
        glm_vec3_copy((float*)GLASS[g].attenuation, m->attenuation_color);
        m->attenuation_distance = GLASS[g].distance;
    }
    for (size_t g = 0; g < sizeof(GLOWS) / sizeof(GLOWS[0]); g++) {
        Material* m = kit->materials[GLOWS[g].id];
        glm_vec3_copy((float*)GLOWS[g].colour, m->emissive);
        m->emissive_strength = GLOWS[g].nits;
        m->emissive_light = 1; // decoration: never a derived panel
    }
}

void mats_lamps_out(Kit* kit) {
    kit->materials[MAT_LAMP_GLOW]->emissive_strength = 0.0f;
}
