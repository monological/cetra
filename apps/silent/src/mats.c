#include <stdio.h>

#include "cetra/material.h"
#include "cetra/program.h"
#include "cetra/texture.h"
#include "cetra/util.h"

#include "mats.h"

/*
 * The photo sets under assets/textures/silent/, as tools/fetch_textures.py
 * leaves them: CC0 scans from Poly Haven cut down to 256 square and stored
 * bottom row first, so V runs up a wall. Relative to the repository root,
 * which is where every app in this tree is run from.
 */
#define TEXTURE_DIR "assets/textures/silent"

typedef struct MatSpec {
    MatId id;
    const char* name; // the material's own name, for the GUI's editor
    const char* set;  // the photo set's base name
    float tint[3];    // the albedo factor over the map
    float roughness;  // a factor over the map: under 1 is wetter
    float metallic;
    float repeat_m; // metres one repeat covers
} MatSpec;

/*
 * The repeats follow each scan's real size where that reads right and depart
 * from it where the reference asks: the kitchen's 30 cm floor tiles are the
 * scan's 50 cm ones at a shorter repeat, and the backsplash is the SAME floor
 * set at 0.6 m, which makes its 15 cm glazed squares.
 */
static const MatSpec SPECS[MAT_COUNT] = {
    {MAT_PLASTER, "plaster", "grey_plaster_02", {0.80f, 0.90f, 0.82f}, 1.0f, 0.0f, 1.4f},
    {MAT_CEILING, "ceiling", "white_plaster_rough_02", {0.75f, 0.75f, 0.72f}, 1.0f, 0.0f, 1.5f},
    {MAT_KITCHEN_FLOOR,
     "kitchen_floor",
     "worn_tile_floor",
     {0.95f, 1.0f, 0.97f},
     0.55f,
     0.0f,
     1.25f},
    {MAT_WOOD_FLOOR, "wood_floor", "old_wood_floor", {1, 1, 1}, 0.8f, 0.0f, 2.0f},
    {MAT_BACKSPLASH, "backsplash", "worn_tile_floor", {0.88f, 0.98f, 1.02f}, 0.45f, 0.0f, 0.6f},
    {MAT_ENAMEL, "enamel", "rusty_metal_02", {0.88f, 0.9f, 0.84f}, 0.8f, 0.0f, 1.0f},
    {MAT_TRIM, "trim", "concrete_wall_003", {0.82f, 0.86f, 0.80f}, 1.0f, 0.0f, 1.5f},
    {MAT_STEEL, "steel", "metal_plate_02", {1.4f, 1.4f, 1.4f}, 0.8f, 1.0f, 1.0f},
    {MAT_WOOD, "wood", "wood_table_worn", {1, 1, 1}, 1.0f, 0.0f, 0.8f},
    {MAT_SIDING, "siding", "white_planks_clean", {0.78f, 0.84f, 0.82f}, 1.0f, 0.0f, 1.8f},
    {MAT_SIDING_B, "siding_blue", "blue_painted_planks", {1, 1, 1}, 1.0f, 0.0f, 1.2f},
    {MAT_SIDING_C, "siding_ochre", "white_planks_clean", {0.86f, 0.76f, 0.56f}, 1.0f, 0.0f, 1.8f},
    {MAT_PORCH, "porch", "old_wood_floor", {0.75f, 0.78f, 0.8f}, 1.0f, 0.0f, 2.0f},
    {MAT_DIRT, "yard", "grass_ground", {1, 1, 1}, 1.0f, 0.0f, 2.5f},
    {MAT_ASPHALT, "asphalt", "asphalt_02", {1, 1, 1}, 1.0f, 0.0f, 3.0f},
    {MAT_CONCRETE, "sidewalk", "concrete_pavement", {1, 1, 1}, 1.0f, 0.0f, 1.8f},
    {MAT_ROOF, "roof", "roof_slates_02", {1, 1, 1}, 1.0f, 0.0f, 3.0f},
    {MAT_BRICK, "brick", "brick_wall_006", {1, 1, 1}, 1.0f, 0.0f, 3.0f},
    {MAT_RUG, "rug", "dirty_carpet", {1, 1, 1}, 1.0f, 0.0f, 0.6f},
    {MAT_TOWEL, "towel", "fabric_pattern_05", {1, 1, 1}, 1.0f, 0.0f, 0.5f},
};

static Texture* load(TexturePool* pool, const char* set, const char* map, TextureDesc desc) {
    char file[128];
    snprintf(file, sizeof(file), "%s_%s.png", set, map);
    return texture_load_file(pool, file, desc);
}

bool mats_register(Kit* kit, Engine* engine, Scene* scene) {
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
        material_set_albedo_tex(m, load(scene->tex_pool, s->set, "albedo", texture_desc(true)));
        material_set_normal_tex(m, load(scene->tex_pool, s->set, "normal", normal_desc));
        material_set_roughness_tex(m, load(scene->tex_pool, s->set, "rough", texture_desc(false)));
        if (kit_material(kit, m, s->repeat_m) != (int)s->id) {
            fprintf(stderr, "silent: material %s landed in the wrong kit slot\n", s->name);
            return false;
        }
    }
    return true;
}
