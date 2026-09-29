#include <stdio.h>

#include "cetra/material.h"
#include "cetra/program.h"

#include "mats.h"

typedef struct MatSpec {
    MatId id;
    float albedo[3];
    float roughness;
    float metallic;
    float repeat_m;
} MatSpec;

// Flat colours for now; each row gains its procedural maps in a later phase.
// The albedos are dirty and dark on purpose: nothing in this house is clean.
static const MatSpec SPECS[MAT_COUNT] = {
    {MAT_PLASTER, {0.46f, 0.47f, 0.40f}, 0.85f, 0.0f, 2.0f},
    {MAT_CEILING, {0.42f, 0.42f, 0.37f}, 0.9f, 0.0f, 2.0f},
    {MAT_KITCHEN_FLOOR, {0.38f, 0.40f, 0.38f}, 0.35f, 0.0f, 1.2f},
    {MAT_WOOD_FLOOR, {0.25f, 0.17f, 0.11f}, 0.6f, 0.0f, 1.5f},
    {MAT_SIDING, {0.40f, 0.42f, 0.40f}, 0.8f, 0.0f, 2.0f},
    {MAT_TRIM, {0.55f, 0.55f, 0.50f}, 0.6f, 0.0f, 1.0f},
    {MAT_PORCH, {0.30f, 0.26f, 0.22f}, 0.8f, 0.0f, 1.5f},
    {MAT_DIRT, {0.18f, 0.16f, 0.13f}, 0.95f, 0.0f, 3.0f},
    {MAT_ASPHALT, {0.11f, 0.11f, 0.11f}, 0.8f, 0.0f, 4.0f},
    {MAT_CONCRETE, {0.36f, 0.36f, 0.34f}, 0.9f, 0.0f, 2.0f},
    {MAT_ROOF, {0.16f, 0.15f, 0.15f}, 0.85f, 0.0f, 2.0f},
};

bool mats_register(Kit* kit, Engine* engine) {
    ShaderProgram* pbr = engine_get_program(engine, CETRA_PROGRAM_PBR);
    for (int i = 0; i < MAT_COUNT; i++) {
        const MatSpec* s = &SPECS[i];
        Material* m = create_material();
        glm_vec3_copy((float*)s->albedo, m->albedo);
        m->roughness = s->roughness;
        m->metallic = s->metallic;
        material_set_program(m, pbr);
        if (kit_material(kit, m, s->repeat_m) != (int)s->id) {
            fprintf(stderr, "silent: material %d landed in the wrong kit slot\n", i);
            return false;
        }
    }
    return true;
}
