#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <cglm/cglm.h>
#include <GL/glew.h>

#include "common.h"
#include "ext/log.h"
#include "material.h"
#include "program.h"

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
    material->hair_jitter = 0.06f;
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
