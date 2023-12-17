#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <cglm/cglm.h>
#include <GL/glew.h>

#include "common.h"
#include "material.h"

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
    material->albedoTex = NULL;
    material->normalTex = NULL;
    material->roughnessTex = NULL;
    material->metalnessTex = NULL;
    material->ambientOcclusionTex = NULL;
    material->emissiveTex = NULL;
    material->heightTex = NULL;

    // Additional Advanced PBR Textures
    material->opacityTex = NULL;
    material->microsurfaceTex = NULL;
    material->anisotropyTex = NULL;
    material->subsurfaceScatteringTex = NULL;
    material->sheenTex = NULL;
    material->reflectanceTex = NULL;

    return material;
}

void free_material(Material* material) {
    if (material) {
        // Texures are managed by pool. Do not free them here.

        // Finally, free the material structure itself
        free(material);
    }
}
