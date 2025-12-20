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

#endif // _MATERIAL_H_
