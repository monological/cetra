#ifndef _MATERIAL_H_
#define _MATERIAL_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "texture.h"
#include "program.h"

// How a material's alpha is rendered (glTF alphaMode semantics).
//
// The normals G-buffer's alpha channel (color attachment 1) is a shared
// contract across producers/consumers; its meaning is the SIGN, not the
// magnitude:
//   - Opaque model surfaces write .a = 0 (non-reflective; SSAO reads .xyz).
//   - The shadow catcher writes .a = -1 to mark the reflective floor, the
//     only surface SSR traces (its roughness is a scalar uniform, not
//     carried per-texel). Only stamped when SSR is active.
//   - ALPHA_MASK surfaces write .a = FragColor's alpha (A2C coverage), a
//     non-negative value: Apple's driver takes A2C coverage from the LAST
//     color output's alpha, so it cannot carry anything else.
//
// ALPHA_MASK carries renderer-wide special cases beyond alpha-to-coverage
// itself; the full contract, each enforced where it must live:
//   - Shadow map: excluded entirely, neither casts (shadow.c mesh skip) nor
//     receives (pbr_frag.glsl shadow loop) — map texels are millimeters,
//     card strands alias into streaks/acne at that scale.
//   - Normals G-buffer: writes a zero-normal marker (see above), which GTAO
//     reads to skip these texels (gtao_frag.glsl) — screen-space AO on the
//     strand tangle is depth-derivative noise that flickers under TAA.
//   - Occlusion for these surfaces comes from the baked AO texture only.
typedef enum AlphaMode {
    ALPHA_OPAQUE = 0, // Single opaque pass
    ALPHA_MASK,       // Opaque pass with alpha-to-coverage (hair/foliage)
    ALPHA_BLEND,      // Translucent pass after the skybox, no depth writes
} AlphaMode;

typedef struct Material {
    vec3 albedo;
    vec3 emissive;           // Emissive color factor (multiplied with emissive texture)
    float emissive_strength; // HDR multiplier (KHR_materials_emissive_strength), feeds bloom
    float metallic;
    float roughness;
    float ao;
    float opacity;
    AlphaMode alpha_mode;
    float alphaCutoff;   // Alpha cutoff threshold for hair/foliage (0 = disabled, 0.5 typical)
    float normalScale;   // Normal map intensity scale (1.0 = full strength)
    float aoStrength;    // Occlusion texture strength (1.0 = full effect)
    float ior;           // Index of refraction (1.5 for plastic/glass, 1.33 for water)
    float transmission;  // KHR_materials_transmission factor (0 = opaque; > 0 joins the
                         // late pass and samples the resolved opaque scene color)
    float thickness;     // KHR_materials_volume thickness in world units (0 = thin: no
                         // refraction bend, only tint/blur)
    float filmThickness; // Thin-film thickness in nanometers (0 = disabled, 200-600nm typical)
    vec2 uvOffset;       // Texture coordinate offset (KHR_texture_transform)
    vec2 uvScale;        // Texture coordinate scale (KHR_texture_transform)
    float uvRotation;    // Texture coordinate rotation in radians (KHR_texture_transform)
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

    // Per-mask layer indices into the scene's material mask sampler2DArray
    // (-1 = no texture -> the shader falls back to the scalar factor). The
    // *_tex pointers above are the load-time source; these are resolved when
    // the mask array is (re)built. roughness/metallic/ao read .g/.b/.r of
    // their layer (glTF ORM), the rest read .r.
    int roughness_layer;
    int metallic_layer;
    int ao_layer;
    int opacity_layer;
    int microsurface_layer;
    int anisotropy_layer;
    int subsurface_layer;

    ShaderProgram* shader_program;
} Material;

Material* create_material();
void free_material(Material* material);

// Derive ALPHA_BLEND from opacity/opacity_tex when no explicit mode was set
// (call after all material properties and textures are assigned)
void material_finalize_alpha_mode(Material* material);

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
