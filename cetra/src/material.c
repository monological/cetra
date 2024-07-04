#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <cglm/cglm.h>
#include <GL/glew.h>

#include "common.h"
#include "material.h"
#include "program.h"

Material* create_material() {
    Material* material = (Material*)malloc(sizeof(Material));
    if (!material) {
        fprintf(stderr, "Failed to allocate memory for material\n");
        return NULL;
    }

    // Initialize with default values
    glm_vec3_fill(material->albedo, 1.0f);
    material->metallic = 0.0f;
    material->roughness = 1.0f;
    material->ao = 1.0f;

    // In create_material function
    material->albedo_tex = NULL;
    material->normal_tex = NULL;
    material->roughness_tex = NULL;
    material->metalness_tex = NULL;
    material->ambient_occlusion_tex = NULL;
    material->emissive_tex = NULL;
    material->height_tex = NULL;

    // Additional Advanced PBR Textures
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
        // Texures are managed by pool. Do not free them here.

        // shader program managed by engine. Do not free here.

        // Finally, free the material structure itself
        free(material);
    }
}

void set_material_shader_program(Material* material, ShaderProgram* shader_program) {
    if (!material) {
        fprintf(stderr, "Cannot set shader program for NULL material\n");
        return;
    }

    if (!shader_program) {
        fprintf(stderr, "Cannot set NULL shader program for material\n");
        return;
    }
    material->shader_program = shader_program;
}
