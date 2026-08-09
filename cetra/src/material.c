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

#define MP(field, type, lo, hi) offsetof(Material, field), type, lo, hi

// Group order here is the order an editor shows them in, and it is deliberate:
// the handful of properties that describe every surface come first, and the
// ones that only matter to a material that opted into a feature follow. A flat
// list of all of them is technically complete and useless to tune against.
const MaterialParam MATERIAL_PARAMS[] = {
    {"albedo", "Base", MP(albedo, MATERIAL_PARAM_VEC3, 0.0f, 1.0f)},
    {"roughness", "Base", MP(roughness, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"metallic", "Base", MP(metallic, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"ao", "Base", MP(ao, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"opacity", "Base", MP(opacity, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},

    {"hairShading", "Hair", MP(hair_shading, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"hairRoughness", "Hair", MP(hair_roughness, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"hairShift", "Hair", MP(hair_shift, MATERIAL_PARAM_FLOAT, 0.0f, 0.3f)},
    {"hairTint", "Hair", MP(hair_tint, MATERIAL_PARAM_VEC3, 0.0f, 1.0f)},
    {"hairBacklit", "Hair", MP(hair_backlit, MATERIAL_PARAM_FLOAT, 0.0f, 2.0f)},
    {"hairJitter", "Hair", MP(hair_jitter, MATERIAL_PARAM_FLOAT, 0.0f, 4.0f)},

    {"emissive", "Emissive", MP(emissive, MATERIAL_PARAM_VEC3, 0.0f, 1.0f)},
    {"emissiveStrength", "Emissive", MP(emissive_strength, MATERIAL_PARAM_FLOAT, 0.0f, 20.0f)},

    {"clearcoat", "Coat and sheen", MP(clearcoat, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"clearcoatRoughness", "Coat and sheen",
     MP(clearcoat_roughness, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"sheenColor", "Coat and sheen", MP(sheen_color_factor, MATERIAL_PARAM_VEC3, 0.0f, 1.0f)},
    {"sheenRoughness", "Coat and sheen",
     MP(sheen_roughness_factor, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    // -1 means the KHR extension was absent, which is NOT the same as 0 (an
    // explicit zero weight), so the range has to reach below zero to express it.
    {"specularFactor", "Coat and sheen", MP(specular_factor, MATERIAL_PARAM_FLOAT, -1.0f, 2.0f)},
    {"specularColor", "Coat and sheen",
     MP(specular_color_factor, MATERIAL_PARAM_VEC3, 0.0f, 1.0f)},

    {"ior", "Transmission", MP(ior, MATERIAL_PARAM_FLOAT, 1.0f, 3.0f)},
    {"transmission", "Transmission", MP(transmission, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"thickness", "Transmission", MP(thickness, MATERIAL_PARAM_FLOAT, 0.0f, 5.0f)},
    {"filmThickness", "Transmission", MP(filmThickness, MATERIAL_PARAM_FLOAT, 0.0f, 1000.0f)},

    {"curvatureScale", "Skin", MP(curvature_scale, MATERIAL_PARAM_FLOAT, 0.0f, 2.0f)},

    {"normalScale", "Maps and wind", MP(normalScale, MATERIAL_PARAM_FLOAT, 0.0f, 4.0f)},
    {"aoStrength", "Maps and wind", MP(aoStrength, MATERIAL_PARAM_FLOAT, 0.0f, 1.0f)},
    {"parallaxScale", "Maps and wind", MP(parallax_scale, MATERIAL_PARAM_FLOAT, 0.0f, 0.2f)},
    {"windResponse", "Maps and wind", MP(wind_response, MATERIAL_PARAM_FLOAT, 0.0f, 2.0f)},
    {"windMode", "Maps and wind", MP(wind_mode, MATERIAL_PARAM_INT, 0.0f, 2.0f)},
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
// Split by constness rather than casting it away, so the read path cannot
// silently gain the right to write.
static const void* material_param_field_const(const Material* material,
                                              const MaterialParam* param) {
    return (const void*)((const char*)material + param->offset);
}

static void* material_param_field(Material* material, const MaterialParam* param) {
    return (void*)((char*)material + param->offset);
}

void material_param_get(const Material* material, const MaterialParam* param, float* values) {
    if (!material || !param || !values)
        return;
    const void* field = material_param_field_const(material, param);
    switch (param->type) {
    case MATERIAL_PARAM_VEC3:
        glm_vec3_copy((float*)field, values);
        break;
    case MATERIAL_PARAM_FLOAT:
        values[0] = *(const float*)field;
        break;
    case MATERIAL_PARAM_INT:
        values[0] = (float)*(const int*)field;
        break;
    }
}

void material_param_set(Material* material, const MaterialParam* param, const float* values) {
    if (!material || !param || !values)
        return;
    void* field = material_param_field(material, param);
    switch (param->type) {
    case MATERIAL_PARAM_VEC3:
        glm_vec3_copy((float*)values, field);
        break;
    case MATERIAL_PARAM_FLOAT:
        *(float*)field = values[0];
        break;
    case MATERIAL_PARAM_INT:
        *(int*)field = (int)values[0];
        break;
    }
}

Material* create_material() {
    Material* material = (Material*)malloc(sizeof(Material));
    if (!material) {
        log_error("Failed to allocate memory for material");
        return NULL;
    }

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
    // Hair lobes off until a material opts in; the rest are the shape they take
    // WHEN it does, so enabling is one key rather than five.
    material->hair_shading = 0.0f;
    material->hair_roughness = 0.35f;
    material->hair_shift = 0.04f;
    glm_vec3_copy((vec3){0.55f, 0.34f, 0.22f}, material->hair_tint); // warm brown absorption
    material->hair_backlit = 0.35f;
    material->hair_jitter = 1.0f; // one lobe half-width; see material.h
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
    material->hair_flow_tex = NULL;

    // No mask array layers until the array is built from loaded textures
    material->roughness_layer = -1;
    material->metallic_layer = -1;
    material->ao_layer = -1;
    material->opacity_layer = -1;
    material->microsurface_layer = -1;
    material->anisotropy_layer = -1;
    material->hair_flow_layer = -1;

    material->shader_program = NULL;

    return material;
}

void material_finalize_alpha_mode(Material* material) {
    if (!material)
        return;
    // Formats without an explicit alpha mode: translucency is implied by a
    // fractional opacity or a dedicated opacity map
    if (material->alpha_mode == ALPHA_OPAQUE &&
        (material->opacity < 1.0f || material->opacity_tex)) {
        material->alpha_mode = ALPHA_BLEND;
    }
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
        if (material->hair_flow_tex)
            texture_release(material->hair_flow_tex);

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

void set_material_hair_flow_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->hair_flow_tex)
        texture_release(material->hair_flow_tex);
    material->hair_flow_tex = texture_retain(texture);
}
