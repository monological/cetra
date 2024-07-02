#ifndef _MATERIAL_H_
#define _MATERIAL_H_

#include <cglm/cglm.h>

#include "texture.h"
#include "program.h"


typedef struct Material {
    vec3 albedo;
    float metallic;
    float roughness;
    float ao;

    // Core PBR Textures
    Texture* albedoTex;           // Albedo (Diffuse) Map
    Texture* normalTex;           // Normal Map
    Texture* roughnessTex;        // Roughness Map
    Texture* metalnessTex;        // Metalness Map
    Texture* ambientOcclusionTex; // Ambient Occlusion Map
    Texture* emissiveTex;         // Emissive Map
    Texture* heightTex;           // Height Map (Displacement Map)

    // Additional Advanced PBR Textures
    Texture* opacityTex;             // Opacity Map
    Texture* microsurfaceTex;        // Microsurface (Detail) Map
    Texture* anisotropyTex;          // Anisotropy Map
    Texture* subsurfaceScatteringTex;// Subsurface Scattering Map
    Texture* sheenTex;               // Sheen Map (for fabrics)
    Texture* reflectanceTex;         // Reflectance Map

    ShaderProgram* shader_program;
} Material;

Material* create_material();
void free_material(Material* material);

void set_material_shader_program(Material* material, ShaderProgram* shader_program);

#endif // _MATERIAL_H_

