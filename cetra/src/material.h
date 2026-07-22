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
//     card strands alias into streaks/acne at that scale. Foliage opts back
//     in per material (foliage_shadows below): leaf cards are centimeters, so
//     an alpha-tested depth pass resolves them cleanly and the canopy shadow
//     it casts is the whole point of the surface.
//   - Normals G-buffer: writes a zero-normal marker (see above), which GTAO
//     reads to skip these texels (gtao_frag.glsl) — screen-space AO on the
//     strand tangle is depth-derivative noise that flickers under TAA. This
//     holds for foliage too; its occlusion comes from the shadow map instead.
//   - Occlusion for these surfaces comes from the baked AO texture only.
typedef enum AlphaMode {
    ALPHA_OPAQUE = 0, // Single opaque pass
    ALPHA_MASK,       // Opaque pass with alpha-to-coverage (hair/foliage)
    ALPHA_BLEND,      // Translucent pass after the skybox, no depth writes
} AlphaMode;

typedef struct Material {
    char* name; // authored material name (glTF/FBX); scene files match on it
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
    float clearcoat;     // KHR_materials_clearcoat weight (0 = no coat lobe)
    float clearcoat_roughness;  // Clearcoat lobe roughness (glTF default 0 = mirror-smooth)
    float specular_factor;      // KHR_materials_specular weight (-1 = extension absent -> base BRDF
                                // unchanged; >= 0 tints/weights the dielectric specular)
    vec3 specular_color_factor; // KHR_materials_specular F0 tint (glTF default white = no tint)
    vec3 sheen_color_factor;    // KHR_materials_sheen color ((0,0,0) = no sheen lobe)
    float sheen_roughness_factor; // KHR_materials_sheen roughness (glTF default 0)
    float parallax_scale;         // POM height-march depth in UV units (0 = off, §4.11)
    float subsurface;       // Separable SSS strength (0 = off); blends the diffuse sharp<->blurred,
                            // so it is also the per-material skin flag
    vec3 subsurface_color;  // Back-light transmission tint (skin ~(1.0,0.3,0.2)). The screen-space
                            // blur's per-channel profile + radius live in subsurface_profile.
    int subsurface_profile; // Index into PostFX's per-material scatter profiles (color + radius);
                            // -1 = unassigned. pbr_frag writes it into the skin-diffuse alpha so
                            // the blur picks this material's profile per pixel.
    vec2 uvOffset;          // Texture coordinate offset (KHR_texture_transform)
    vec2 uvScale;           // Texture coordinate scale (KHR_texture_transform)
    float uvRotation;       // Texture coordinate rotation in radians (KHR_texture_transform)
    bool doubleSided;       // Disable backface culling for this material

    // ALPHA_MASK opt-in to the shadow map: the depth pass draws this material
    // with an alpha test (casts) and the shading pass samples the map for it
    // (receives). Default false keeps the blanket exclusion documented above,
    // which is what hair cards need. Effective only when the material is
    // ALPHA_MASK with a positive alphaCutoff and an albedo texture to test.
    bool foliage_shadows;

    // Wind response (World-Position Offset cloth; see wind.h). The per-material
    // half of the wind split: the Scene owns the wind field, a material opts in
    // here. 0 = rigid (the shader early-outs -> no motion). The height-mask
    // bounds that pin the top and free the hem are per-mesh geometry, uploaded
    // per draw from the mesh's AABB -- not stored here (a material is shared).
    float wind_response;

    // Which displacement model wind_response drives (pbr_vert.glsl windOffset):
    //   0 = cloth: the AABB height gradient that pins the top and swings the hem
    //   1 = vegetation branch: whole-trunk lean + per-branch de-phased sway
    //   2 = vegetation leaf: the branch ride plus high-frequency flutter
    // The vegetation modes read UV1 per vertex (x = branch phase, y = flex
    // weight), so they only work on geometry authored with that data.
    int wind_mode;

    // Core PBR Textures
    Texture* albedo_tex;            // Albedo (Diffuse) Map
    Texture* normal_tex;            // Normal Map
    Texture* roughness_tex;         // Roughness Map
    Texture* metalness_tex;         // Metalness Map
    Texture* ambient_occlusion_tex; // Ambient Occlusion Map
    Texture* emissive_tex;          // Emissive Map
    Texture* height_tex;            // Height Map (Displacement Map)

    // Additional Advanced PBR Textures
    Texture* opacity_tex;          // Opacity Map
    Texture* microsurface_tex;     // Microsurface (Detail) Map
    Texture* anisotropy_tex;       // Anisotropy Map
    Texture* sheen_tex;            // Sheen Map (for fabrics)
    Texture* reflectance_tex;      // Reflectance Map
    Texture* clearcoat_normal_tex; // Clearcoat normal map (orange-peel / weave)

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
void set_material_clearcoat_normal_tex(Material* material, Texture* texture);
void set_material_microsurface_tex(Material* material, Texture* texture);
void set_material_anisotropy_tex(Material* material, Texture* texture);

#endif // _MATERIAL_H_
