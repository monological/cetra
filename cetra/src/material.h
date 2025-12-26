#ifndef _MATERIAL_H_
#define _MATERIAL_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "texture.h"
#include "program.h"

typedef struct Material {
    vec3 albedo;
    vec3 emissive; // Emissive color factor (multiplied with emissive texture)
    float metallic;
    float roughness;
    float ao;
    float opacity;
    float alphaCutoff;   // Alpha cutoff threshold for hair/foliage (0 = disabled, 0.5 typical)
    float ior;           // Index of refraction (1.5 for plastic/glass, 1.33 for water)
    float filmThickness; // Thin-film thickness in nanometers (0 = disabled, 200-600nm typical)
    bool doubleSided;    // Disable backface culling for this material

    // Core PBR Textures
    Texture* albedo_tex;            // Albedo (Diffuse) Map
    Texture* normal_tex;            // Normal Map
    Texture* roughness_tex;         // Roughness Map
    Texture* metalness_tex;         // Metalness Map
    Texture* ambient_occlusion_tex; // Ambient Occlusion Map
    Texture* emissive_tex;          // Emissive Map
    Texture* height_tex;            // Height Map (Displacement Map)

    // Additional Advanced PBR Textures
    Texture* opacity_tex;               // Opacity Map
    Texture* microsurface_tex;          // Microsurface (Detail) Map
    Texture* anisotropy_tex;            // Anisotropy Map
    Texture* subsurface_scattering_tex; // Subsurface Scattering Map
    Texture* sheen_tex;                 // Sheen Map (for fabrics)
    Texture* reflectance_tex;           // Reflectance Map

    ShaderProgram* shader_program;
} Material;

Material* create_material();
void free_material(Material* material);

void set_material_shader_program(Material* material, ShaderProgram* shader_program);

void set_material_albedo_tex(Material* material, Texture* texture);
void set_material_normal_tex(Material* material, Texture* texture);
void set_material_roughness_tex(Material* material, Texture* texture);
void set_material_metalness_tex(Material* material, Texture* texture);
void set_material_ambient_occlusion_tex(Material* material, Texture* texture);
void set_material_emissive_tex(Material* material, Texture* texture);
void set_material_height_tex(Material* material, Texture* texture);
void set_material_opacity_tex(Material* material, Texture* texture);
void set_material_sheen_tex(Material* material, Texture* texture);
void set_material_reflectance_tex(Material* material, Texture* texture);
void set_material_microsurface_tex(Material* material, Texture* texture);
void set_material_anisotropy_tex(Material* material, Texture* texture);
void set_material_subsurface_scattering_tex(Material* material, Texture* texture);

#endif // _MATERIAL_H_
