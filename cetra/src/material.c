#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cglm/cglm.h>
#include <GL/glew.h>

#include "common.h"
#include "ext/log.h"
#include "material.h"
#include "program.h"

#define MP(field, ty, lo, hi)                                                                      \
    .offset = offsetof(Material, field), .type = ty, .min = lo, .max = hi

// Names, not indices: the vegetation modes also redefine what UV1 MEANS on a
// material, which is not a thing to discover by dragging an integer.
static const char* const WIND_MODE_NAMES[] = {"cloth", "vegetation branch", "vegetation leaf"};

// Group order here is the order an editor shows them in, and it is deliberate:
// the handful of properties that describe every surface come first, and the
// ones that only matter to a material that opted into a feature follow. A flat
// list of all of them is technically complete and useless to tune against.
//
// Rows of a group must stay CONSECUTIVE -- an editor walks runs, so a row
// separated from its group would open a second section under the same name.
const MaterialParam MATERIAL_PARAMS[] = {
    {"albedo", "Base", MP(albedo, MATERIAL_PARAM_COLOR, 0.0f, 1.0f)},
    {"roughness", "Base", MP(roughness, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"metallic", "Base", MP(metallic, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"ao", "Base", MP(ao, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"opacity", "Base", MP(opacity, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},

    // Capped below 1: the anisotropic NDF divides by the cross-strand roughness,
    // which a strength of 1 drives to zero.
    {"anisotropy", "Anisotropy", MP(anisotropy, MATERIAL_PARAM_FLOAT, 0.0f, 0.95f)},
    {"anisotropyMap", "Anisotropy", .type = MATERIAL_PARAM_TEXTURE,
     .offset = offsetof(Material, anisotropy_tex), .set_tex = set_material_anisotropy_tex},

    {"emissive", "Emissive", MP(emissive, MATERIAL_PARAM_COLOR, 0.0f, 1.0f)},
    {"emissiveStrength", "Emissive", MP(emissive_strength, MATERIAL_PARAM_FLOAT, 0.0f, 20.0f)},

    {"clearcoat", "Coat and sheen", MP(clearcoat, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"clearcoatRoughness", "Coat and sheen",
     MP(clearcoat_roughness, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"sheenColor", "Coat and sheen", MP(sheen_color_factor, MATERIAL_PARAM_COLOR, 0.0f, 1.0f)},
    {"sheenRoughness", "Coat and sheen",
     MP(sheen_roughness_factor, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    // -1 means the KHR extension was absent, which is NOT the same as 0 (an
    // explicit zero weight), so the range has to reach below zero to express it.
    {"specularFactor", "Coat and sheen", MP(specular_factor, MATERIAL_PARAM_FLOAT, -1.0f, 2.0f)},
    {"specularColor", "Coat and sheen",
     MP(specular_color_factor, MATERIAL_PARAM_COLOR, 0.0f, 1.0f)},

    {"ior", "Transmission", MP(ior, MATERIAL_PARAM_FLOAT, 1.0f, 3.0f)},
    {"transmission", "Transmission", MP(transmission, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"thickness", "Transmission", MP(thickness, MATERIAL_PARAM_FLOAT, 0.0f, 5.0f)},
    {"filmThickness", "Transmission", MP(filmThickness, MATERIAL_PARAM_FLOAT, 0.0f, 1000.0f)},

    {"curvatureScale", "Skin", MP(curvature_scale, MATERIAL_PARAM_FLOAT, 0.0f, 2.0f)},

    {"normalScale", "Maps and wind", MP(normalScale, MATERIAL_PARAM_FLOAT, 0.0f, 2.0f)},
    {"aoStrength", "Maps and wind", MP(aoStrength, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"parallaxScale", "Maps and wind", MP(parallax_scale, MATERIAL_PARAM_FLOAT, 0.0f, 0.1f)},
    {"windResponse", "Maps and wind", MP(wind_response, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"windMode", "Maps and wind", .offset = offsetof(Material, wind_mode),
     .type = MATERIAL_PARAM_INT, .enum_labels = WIND_MODE_NAMES,
     .enum_count = (int)(sizeof(WIND_MODE_NAMES) / sizeof(WIND_MODE_NAMES[0]))},
};

#undef MP

const size_t MATERIAL_PARAM_COUNT = sizeof(MATERIAL_PARAMS) / sizeof(MATERIAL_PARAMS[0]);

const MaterialParam* material_param_find(const char* key) {
    if (!key)
        return NULL;
    for (size_t i = 0; i < MATERIAL_PARAM_COUNT; i++) {
        if (strcmp(MATERIAL_PARAMS[i].key, key) == 0)
            return &MATERIAL_PARAMS[i];
    }
    return NULL;
}

// The void* hop is not decoration: cppcheck's invalidPointerCast rejects a
// direct char* -> float* cast. offsetof already guarantees the alignment.
//
// Two helpers rather than one that casts constness away, because a single
// const-taking version makes cppcheck's constParameterPointer fire on the
// setter -- it cannot see the write through the void*, and that check is not
// suppressed here. memcpy rather than glm_vec3_copy for the same reason: cglm
// takes a non-const vec3, so copying through it would put the cast back.
static const void* material_param_field_const(const Material* material,
                                              const MaterialParam* param) {
    return (const void*)((const char*)material + param->offset);
}

static void* material_param_field(Material* material, const MaterialParam* param) {
    return (void*)((char*)material + param->offset);
}

void material_param_get(const Material* material, const MaterialParam* param, float* values) {
    if (!material || !param || !values || param->type == MATERIAL_PARAM_TEXTURE)
        return;
    const void* field = material_param_field_const(material, param);
    switch (param->type) {
    case MATERIAL_PARAM_COLOR:
        memcpy(values, field, sizeof(vec3));
        break;
    case MATERIAL_PARAM_FLOAT:
        values[0] = *(const float*)field;
        break;
    case MATERIAL_PARAM_INT:
        values[0] = (float)*(const int*)field;
        break;
    case MATERIAL_PARAM_TEXTURE:
        break; // unreachable; guarded above
    }
}

const Texture* material_param_texture(const Material* material, const MaterialParam* param) {
    if (!material || !param || param->type != MATERIAL_PARAM_TEXTURE)
        return NULL;
    const Texture* const* slot = material_param_field_const(material, param);
    return *slot;
}

void material_param_set(Material* material, const MaterialParam* param, const float* values) {
    if (!material || !param || !values || param->type == MATERIAL_PARAM_TEXTURE)
        return;
    void* field = material_param_field(material, param);
    switch (param->type) {
    case MATERIAL_PARAM_COLOR:
        memcpy(field, values, sizeof(vec3));
        break;
    case MATERIAL_PARAM_FLOAT:
        *(float*)field = values[0];
        break;
    case MATERIAL_PARAM_INT:
        *(int*)field = (int)values[0];
        break;
    case MATERIAL_PARAM_TEXTURE:
        break; // unreachable; guarded above
    }
}

// See Mesh.id: creation order, because a sort keyed on the pointer would order
// draws by allocator address and differ run to run.
static unsigned g_next_material_id = 1;

Material* create_material() {
    Material* material = (Material*)malloc(sizeof(Material));
    if (!material) {
        log_error("Failed to allocate memory for material");
        return NULL;
    }

    material->id = g_next_material_id++;
    material->name = NULL;
    glm_vec3_fill(material->albedo, 1.0f);
    glm_vec3_zero(material->emissive);
    material->emissive_strength = 1.0f;
    material->metallic = 0.0f;
    material->roughness = 1.0f;
    material->ao = 1.0f;
    material->opacity = 1.0f;
    material->alpha_mode = ALPHA_OPAQUE;
    material->alphaCutoff = 0.0f; // Disabled by default
    material->normalScale = 1.0f;
    material->aoStrength = 1.0f;
    material->ior = 1.5f;
    material->transmission = 0.0f;
    material->thickness = 0.0f;
    material->filmThickness = 0.0f;
    material->clearcoat = 0.0f;
    material->clearcoat_roughness = 0.0f;
    material->specular_factor = -1.0f; // KHR_materials_specular absent until imported
    glm_vec3_one(material->specular_color_factor);
    glm_vec3_zero(material->sheen_color_factor); // (0,0,0) = no sheen until imported
    material->sheen_roughness_factor = 0.0f;
    material->parallax_scale = 0.0f; // POM off until a material opts in (§4.11)
    material->subsurface = 0.0f;     // SSS off until a material opts in
    glm_vec3_copy((vec3){1.0f, 0.3f, 0.2f}, material->subsurface_color); // skin-ish default tint
    material->subsurface_profile = -1; // no scatter profile until configured
    material->curvature_scale = 0.0f;  // pre-integrated skin off until a material opts in
    material->anisotropy = 0.0f; // isotropic until a material opts in
    glm_vec2_zero(material->uvOffset);
    glm_vec2_one(material->uvScale);
    material->uvRotation = 0.0f;
    material->doubleSided = false;
    material->foliage_shadows = false; // masked surfaces stay out of the shadow map
    material->wind_response = 0.0f;    // rigid until a material opts into wind
    material->wind_mode = 0;           // cloth displacement unless a material asks for vegetation

    material->albedo_tex = NULL;
    material->normal_tex = NULL;
    material->roughness_tex = NULL;
    material->metalness_tex = NULL;
    material->ambient_occlusion_tex = NULL;
    material->emissive_tex = NULL;
    material->height_tex = NULL;

    material->opacity_tex = NULL;
    material->microsurface_tex = NULL;
    material->anisotropy_tex = NULL;
    material->sheen_tex = NULL;
    material->reflectance_tex = NULL;
    material->clearcoat_normal_tex = NULL;

    // No mask array layers until the array is built from loaded textures
    material->roughness_layer = -1;
    material->metallic_layer = -1;
    material->ao_layer = -1;
    material->opacity_layer = -1;
    material->microsurface_layer = -1;
    material->anisotropy_layer = -1;

    material->shader_program = NULL;

    return material;
}

void free_material(Material* material) {
    if (material) {
        if (material->name)
            free(material->name);
        // Release all texture references
        if (material->albedo_tex)
            texture_release(material->albedo_tex);
        if (material->normal_tex)
            texture_release(material->normal_tex);
        if (material->roughness_tex)
            texture_release(material->roughness_tex);
        if (material->metalness_tex)
            texture_release(material->metalness_tex);
        if (material->ambient_occlusion_tex)
            texture_release(material->ambient_occlusion_tex);
        if (material->emissive_tex)
            texture_release(material->emissive_tex);
        if (material->height_tex)
            texture_release(material->height_tex);
        if (material->opacity_tex)
            texture_release(material->opacity_tex);
        if (material->microsurface_tex)
            texture_release(material->microsurface_tex);
        if (material->anisotropy_tex)
            texture_release(material->anisotropy_tex);
        if (material->sheen_tex)
            texture_release(material->sheen_tex);
        if (material->reflectance_tex)
            texture_release(material->reflectance_tex);
        if (material->clearcoat_normal_tex)
            texture_release(material->clearcoat_normal_tex);

        // Shader program managed by engine. Do not free here.
        free(material);
    }
}

void set_material_shader_program(Material* material, ShaderProgram* shader_program) {
    if (!material) {
        log_error("Cannot set shader program for NULL material");
        return;
    }

    if (!shader_program) {
        log_error("Cannot set NULL shader program for material");
        return;
    }
    material->shader_program = shader_program;
}

void set_material_albedo_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->albedo_tex)
        texture_release(material->albedo_tex);
    material->albedo_tex = texture_retain(texture);
}

void set_material_normal_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->normal_tex)
        texture_release(material->normal_tex);
    material->normal_tex = texture_retain(texture);
}

void set_material_roughness_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->roughness_tex)
        texture_release(material->roughness_tex);
    material->roughness_tex = texture_retain(texture);
}

void set_material_metalness_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->metalness_tex)
        texture_release(material->metalness_tex);
    material->metalness_tex = texture_retain(texture);
}

void set_material_ambient_occlusion_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->ambient_occlusion_tex)
        texture_release(material->ambient_occlusion_tex);
    material->ambient_occlusion_tex = texture_retain(texture);
}

void set_material_emissive_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->emissive_tex)
        texture_release(material->emissive_tex);
    material->emissive_tex = texture_retain(texture);
}

void set_material_height_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->height_tex)
        texture_release(material->height_tex);
    material->height_tex = texture_retain(texture);
}

void set_material_opacity_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->opacity_tex)
        texture_release(material->opacity_tex);
    material->opacity_tex = texture_retain(texture);
}

void set_material_sheen_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->sheen_tex)
        texture_release(material->sheen_tex);
    material->sheen_tex = texture_retain(texture);
}

void set_material_reflectance_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->reflectance_tex)
        texture_release(material->reflectance_tex);
    material->reflectance_tex = texture_retain(texture);
}

void set_material_clearcoat_normal_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->clearcoat_normal_tex)
        texture_release(material->clearcoat_normal_tex);
    material->clearcoat_normal_tex = texture_retain(texture);
}

void set_material_microsurface_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->microsurface_tex)
        texture_release(material->microsurface_tex);
    material->microsurface_tex = texture_retain(texture);
}

void set_material_anisotropy_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->anisotropy_tex)
        texture_release(material->anisotropy_tex);
    material->anisotropy_tex = texture_retain(texture);
}

