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

    glm_vec3_fill(material->albedo, 1.0f);
    glm_vec3_zero(material->emissive);
    material->metallic = 0.0f;
    material->roughness = 1.0f;
    material->ao = 1.0f;
    material->opacity = 1.0f;
    material->alphaCutoff = 0.0f;  // Disabled by default
    material->normalScale = 1.0f;
    material->aoStrength = 1.0f;
    material->ior = 1.5f;
    material->filmThickness = 0.0f;
    material->doubleSided = false;

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
    material->subsurface_scattering_tex = NULL;
    material->sheen_tex = NULL;
    material->reflectance_tex = NULL;

    material->shader_program = NULL;

    return material;
}

void free_material(Material* material) {
    if (material) {
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
        if (material->subsurface_scattering_tex)
            texture_release(material->subsurface_scattering_tex);
        if (material->sheen_tex)
            texture_release(material->sheen_tex);
        if (material->reflectance_tex)
            texture_release(material->reflectance_tex);

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

void set_material_subsurface_scattering_tex(Material* material, Texture* texture) {
    if (!material)
        return;
    if (material->subsurface_scattering_tex)
        texture_release(material->subsurface_scattering_tex);
    material->subsurface_scattering_tex = texture_retain(texture);
}
