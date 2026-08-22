#ifndef _MATERIAL_H_
#define _MATERIAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <cglm/cglm.h>

#include "texture.h"
#include "program.h"
// For STOCHASTIC_LUT_SIZE, the width of the inverse table a material carries.
#include "procedural/stochastic_tex.h"
// For MaterialRoad and the road caps, which a layered material carries inline.
#include "roads.h"

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

// Which coordinate addresses a layered material's splat map. See Material.
// Zero is UV1 so a calloc'd or partially-authored material keeps the mesh-local
// reading, which is the one that cannot silently sample a single texel.
typedef enum MaterialSplatSpace {
    SPLAT_SPACE_UV1 = 0,
    SPLAT_SPACE_WORLD_XZ,
} MaterialSplatSpace;

// How many material layers a layered surface can blend between.
//
// Four because the splat map carries three weights and the first layer is the
// remainder -- a fourth weight would be redundant with 1 - sum, and a fifth
// needs a second splat texture, which is a different feature rather than a
// larger number here.
#define MATERIAL_MAX_LAYERS 4

// One layer of a layered surface (spec 11.60).
//
// Two textures, not three, and the packing is the reason a layer is affordable:
// every layer is a tenant of the material texture array, which is RGBA8, so the
// alpha channel of each map is free storage that would otherwise be spent on a
// third tenant. `albedo_tex.a` carries the layer's HEIGHT, which is what lets
// gravel interlock with sand instead of averaging into mud, and `surface_tex.a`
// carries ambient occlusion.
//
// The albedo is stored NON-sRGB and decoded in the shader. That is not a
// workaround for the array's format -- it is the same arrangement the stochastic
// path already uses (pbr_frag.glsl), because a transform applied to stored codes
// has to be undone in the space it was applied in.
typedef struct MaterialLayer {
    Texture* albedo_tex;  // rgb albedo (linear-stored, shader-decoded), a = height
    Texture* surface_tex; // rg = normal xy, b = roughness, a = ambient occlusion

    // World units per texture tile. Per LAYER rather than per material because
    // the whole point of a layer set is that its members have different natural
    // scales -- gravel repeats over centimetres where a cliff face repeats over
    // metres, and one shared scale makes one of them wrong.
    float uv_scale;

    // Resolved when the material texture array is (re)built; -1 = absent, which
    // makes the layer contribute its scalar fallback rather than dropping out.
    int albedo_layer;
    int surface_layer;
} MaterialLayer;

typedef struct Material {
    // Creation order. Same contract and same reason as Mesh.id: a stable key for
    // grouping draws that share a material, where the pointer would order them
    // by allocator address.
    unsigned id;
    char* name; // authored material name (glTF/FBX); scene files match on it
    vec3 albedo;
    vec3 emissive;           // Emissive color factor (multiplied with emissive texture)
    float emissive_strength; // HDR multiplier (KHR_materials_emissive_strength), feeds bloom
    float metallic;
    float roughness;
    float ao;
    float opacity;
    AlphaMode alpha_mode;
    float alphaCutoff;      // Alpha cutoff threshold for hair/foliage (0 = disabled, 0.5 typical)
    float normalScale;      // Normal map intensity scale (1.0 = full strength)
    float aoStrength;       // Occlusion texture strength (1.0 = full effect)
    float ior;              // Index of refraction (1.5 for plastic/glass, 1.33 for water)
    float transmission;     // KHR_materials_transmission factor (0 = opaque; > 0 joins the
                            // late pass and samples the resolved opaque scene color)
    float thickness;        // KHR_materials_volume thickness in world units (0 = thin: no
                            // refraction bend, only tint/blur)
    vec3 attenuation_color; // KHR_materials_volume: the colour a path of exactly
                            // attenuation_distance leaves unabsorbed (white = none)
    float attenuation_distance; // KHR_materials_volume distance in world units. 0 stands for the
                                // glTF default of infinity -- no absorption at any thickness --
                                // because a real distance is always positive.
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
    float curvature_scale;  // Pre-integrated skin (§11.13): 0 = off, 1 = the full authored width.
                            // NOT a physical constant -- the width it delivers is whatever the
                            // scatter profile's radius says, which is an artistic number here.
                            // Scales the ANGULAR scatter only, so it trades against that radius
                            // rather than duplicating it: fix one and tune the other, or they
                            // multiply and you chase yourself. Inert unless subsurface > 0 and a
                            // profile is assigned.
    // Anisotropic specular: stretches the highlight ALONG a direction rather
    // than leaving it round. Brushed metal, vinyl, and hair cards (roadmap B8,
    // spec 11.20), which is the case that drove the per-texel direction below.
    //
    // 0 = off, and the GGX highlight the rest of the engine uses stands
    // unchanged. Inert without anisotropy_tex: the strength scales a direction,
    // and without a map there is no direction that is not the whole card's.
    float anisotropy;

    vec2 uvOffset;    // Texture coordinate offset (KHR_texture_transform)
    vec2 uvScale;     // Texture coordinate scale (KHR_texture_transform)
    float uvRotation; // Texture coordinate rotation in radians (KHR_texture_transform)
    bool doubleSided; // Disable backface culling for this material

    // ALPHA_MASK opt-in to the shadow map: the depth pass draws this material
    // with an alpha test (casts) and the shading pass samples the map for it
    // (receives). Default 0 keeps the blanket exclusion documented above,
    // which is what hair cards need. Effective only when the material is
    // ALPHA_MASK with a positive alphaCutoff and an albedo texture to test.
    //
    // An int rather than a bool because it rides MATERIAL_PARAMS, which stores
    // through an offset and knows FLOAT/COLOR/INT/TEXTURE -- an INT row against
    // a bool would write four bytes into one.
    int foliage_shadows;

    // Wind response (World-Position Offset cloth; see wind.h). The per-material
    // half of the wind split: the Scene owns the wind field, a material opts in
    // here. 0 = rigid (the shader early-outs -> no motion). The height-mask
    // bounds that pin the top and free the hem are per-mesh geometry, uploaded
    // per draw from the mesh's AABB -- not stored here (a material is shared).
    float wind_response;

    // Shore wetness (see shore.glsl). The per-material half of the swash split, in the same
    // shape as wind_response above: the Water owns the run-up, a material opts in here.
    // 0 = the surface never darkens, whatever the sea does.
    float shore_wetness;

    /*
     * Stochastic albedo sampling (see include/stochastic.glsl), in UV units per lattice cell.
     * 0 = a plain lookup, which is every material that has not asked.
     *
     * Opt-in because the albedo map has to have been through stochastic_gaussianize first --
     * the shader samples a transformed texture and undoes the transform with a table, so a
     * material that sets this without a baked map renders its own histogram wrong. Costs three
     * texture fetches in place of one where it is on.
     */
    float stochastic_scale;
    // The inverse CDF that undoes the transform, as stochastic_gaussianize filled it: one
    // vec3 per entry, so it uploads in a single call. Meaningless unless stochastic_scale > 0.
    float stochastic_lut[STOCHASTIC_LUT_SIZE * 3];

    // Which displacement model wind_response drives (pbr_vert.glsl windOffset):
    //   0 = cloth: the AABB height gradient that pins the top and swings the hem
    //   1 = vegetation branch: whole-trunk lean + per-branch de-phased sway
    //   2 = vegetation leaf: the branch ride plus high-frequency flutter
    // The vegetation modes read UV1 per vertex (x = branch phase, y = flex
    // weight), so they only work on geometry authored with that data.
    //
    // This field therefore also declares what UV1 MEANS on a material: modes
    // >= 1 redefine it as wind data rather than a second texture coordinate
    // set. pbr_frag reads wind_mode for exactly that reason and falls the AO
    // map back to UV0 for those materials, so binding an occlusion texture on
    // vegetation is safe rather than silently wrong.
    int wind_mode;

    // Whether this material's emissive surface derives an LTC area light
    // (spec 11.49): 0 lights, 1 is decorative. Zero is "lights" so a calloc'd
    // material opts in, and the feature is gated separately at the app -- this
    // says what the SURFACE is, not whether the run wants any panels.
    //
    // A material is shared by every mesh that uses it, so this is a statement
    // about a KIND of surface. That is right for "decorative" and would be wrong
    // for anything per-instance, which is why casting lives on the scene file's
    // light_overrides rather than here.
    int emissive_light;

    // Core PBR Textures
    Texture* albedo_tex;            // Albedo (Diffuse) Map
    Texture* normal_tex;            // Normal Map
    Texture* roughness_tex;         // Roughness Map
    Texture* metalness_tex;         // Metalness Map
    Texture* ambient_occlusion_tex; // Ambient Occlusion Map
    Texture* emissive_tex;          // Emissive Map
    Texture* height_tex;            // Height Map (Displacement Map); refused when layer_count > 0

    // Additional Advanced PBR Textures
    Texture* opacity_tex;      // Opacity Map
    Texture* microsurface_tex; // Microsurface (Detail) Map
    // Per-texel strand/grain direction, as a coherence-weighted DOUBLED-ANGLE
    // vector in .rg plus a strand identity in .b. LINEAR data, not colour.
    //
    // Doubled-angle because a grain direction has no sign and this rides the
    // mask array, which resamples and mips every layer -- averaging a raw
    // direction with its own negation gives zero, exactly where the surface is
    // minified. The vector's LENGTH is the coherence, so a neighbourhood of
    // disagreeing directions shortens toward zero on its own and the shader
    // falls back to an isotropic highlight rather than trusting an average of
    // nothing.
    //
    // Produced by tools/gen_hair_flow.py from a hair atlas, or baked from a
    // groom or a brush pattern in a DCC; the engine cannot tell the difference.
    Texture* anisotropy_tex;
    Texture* sheen_tex;            // Sheen Map (for fabrics)
    Texture* reflectance_tex;      // Reflectance Map
    Texture* clearcoat_normal_tex; // Clearcoat normal map (orange-peel / weave); refused when
                                   // layer_count > 0

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

    /*
     * Layered surface (spec 11.60). 0 = an ordinary material, and the shader
     * skips the whole path -- which is what keeps every frame authored before
     * this byte-identical.
     *
     * The layers are WORLD-ALIGNED, sampled triplanar from world position and
     * world normal rather than from UV0. One coordinate model rather than two,
     * because the alternative is a UV projection that stretches by 1/cos(slope)
     * exactly where terrain is most interesting -- 5.8x on an 80-degree face.
     */
    MaterialLayer layers[MATERIAL_MAX_LAYERS];
    // > 0 makes this a LAYERED material, which REFUSES height_tex and
    // clearcoat_normal_tex: their sampler units carry the composite cache's
    // page pair on layered surfaces (spec 11.67), the same structural
    // exclusivity as units 0/1.
    int layer_count;

    // Per-texel layer weights: .r/.g/.b select layers 1..3 and layer 0 takes the
    // remainder, so the weights always sum to one and no normalisation can drift.
    Texture* splat_tex;
    int splat_layer;

    /*
     * WHICH COORDINATE ADDRESSES THE SPLAT, and it has to be a choice rather
     * than a policy.
     *
     * This shipped as UV1-only and the feature was inert in the app it was
     * written for: `build_grid` writes UV1 as a literal (0,0) at every terrain
     * vertex, so a kilometre of ground sampled one splat texel and resolved to
     * layer 0 everywhere. The bake, the erosion mapping and three of four
     * grounds did nothing, through a green suite -- the fixture authors UV1 by
     * hand and no forest arm reads a pixel.
     *
     * The two spaces answer different meshes and neither generalises:
     *   UV1       a prop, an instanced mesh, anything that MOVES -- world
     *             position would make the weights swim across the surface as it
     *             travelled. Costs the material its UV1, so it is mutually
     *             exclusive with wind_mode >= 1, which claims the same slot.
     *   WORLD_XZ  terrain, and anything else whose splat is baked as a function
     *             of world position. Needs no vertex data at all, and survives a
     *             mesh being rebuilt or retessellated.
     *
     * WORLD_XZ cannot address a vertical surface -- z is constant up a wall --
     * which is exactly why this is not simply "always world".
     */
    MaterialSplatSpace splat_space;
    // World-space domain the splat covers, for SPLAT_WORLD_XZ: uv = (xz - origin) / size.
    float splat_origin[2];
    float splat_size[2];

    // Height-blend contrast, in weight units. 0 = a plain linear blend, which is
    // reachable on purpose: it is what the height blend has to be measured
    // against, and a gate arm that cannot compare against the naive form is not
    // measuring the feature.
    float layer_blend_sharpness;

    // How sharply the triplanar projection favours the dominant axis. Higher is
    // a narrower seam between projections and a harder transition across it.
    float layer_triplanar_sharpness;

    // Roads over the layer set (spec 11.68): polylines that override the splat
    // weights toward one of the layers above. Inline rather than allocated --
    // the whole-struct default copy zero-fills them, and road_count 0 is the
    // off state every consumer already reads.
    MaterialRoad roads[MATERIAL_MAX_ROADS];
    int road_count;
    // Whether this material is the scene's road bearer. Owned by
    // material_roads_ensure, which is what a second road-bearing material is
    // disarmed by; the shader gates its override on it.
    bool roads_armed;

    // Composite cache for a WORLD_XZ splat (spec 11.66); NULL until the first
    // bake, and never set for a UV1 splat.
    struct MaterialLayersVt* layers_vt;

    ShaderProgram* shader_program;
} Material;

typedef enum MaterialParamType {
    MATERIAL_PARAM_FLOAT,
    MATERIAL_PARAM_COLOR, // a vec3 that is a colour; editors may pick accordingly
    MATERIAL_PARAM_INT,
    MATERIAL_PARAM_TEXTURE, // uses `set_tex`, not `offset`
} MaterialParamType;

// One tunable material property: the name a scene file authors it under, where
// it lives, and the range an editor should offer.
typedef struct MaterialParam {
    const char* key;
    const char* group; // editor grouping; CONSECUTIVE rows sharing a name group
    MaterialParamType type;
    size_t offset; // into Material; for TEXTURE rows, at the Texture* member
    // TEXTURE rows only. Writing one is not a store -- it releases what was
    // there and retains the new one -- so an offset can address the field for
    // reading but cannot set it.
    void (*set_tex)(Material*, Texture*);
    // The range an editor OFFERS, which is the useful band and not the
    // expressible one. A scene file may author outside it and nothing clamps
    // the stored value; only dragging the control brings it into the band. The
    // band is narrow on purpose -- a slider that reaches settings which destroy
    // the effect invites exactly that, and the result then looks like a broken
    // feature rather than an extreme setting (spec 11.20 records this
    // happening, on hair roughness and jitter specifically).
    //
    // INT rows with `enum_labels` derive `max` from the label count instead, so
    // the number of modes is stated once rather than twice.
    float min, max;
    // INT rows only: the names of the values, NULL when the value is a quantity
    // rather than an enum. Kept as an array rather than a widget library's
    // packed form -- nothing else in this layer knows what a GUI is.
    const char* const* enum_labels;
    int enum_count;
} MaterialParam;

/*
 * The material vocabulary, and the ONLY place a name is tied to a Material
 * field. Adding a property is one row and every consumer picks it up: the scene
 * file parser resolves authored keys through it, and the GUI builds a control
 * per row. Two tables would drift the moment someone extended one of them,
 * which is why textures live here too rather than app-side -- they are the same
 * namespace and the same authored vocabulary, they just need a setter call
 * where the others need a store.
 *
 * alpha_mode, alphaCutoff and doubleSided are deliberately absent: they decide
 * which PASS a mesh draws in and whether it is culled, so a wrong value there
 * moves geometry between queues instead of merely misshading it. All three also
 * arrive from the GLB already, so a key here would be a second source of truth
 * for something the file has already said.
 *
 * foliage_shadows was in that list until 11.50 and is now a row, because neither
 * half applied to it. It cannot move the camera lane -- classify() derives that
 * from transmissive and blend alone, and DRAW_FOLIAGE reaches only the two
 * caster-set reads in shadow.c -- so the worst it can do is the streak-and-acne
 * aliasing the default protects hair from, which is an ugly surface and nothing
 * more. And no format can express it, so excluding it removed the capability
 * rather than preventing a conflict: masked foliage could shadow only when the
 * app that built it was compiled. It is also guarded four ways at the point of
 * use, so a value set on anything but alpha-masked textured geometry is inert.
 *
 * That is NOT the same as "nothing here changes the lane", and it used to be
 * stated as if it were. `transmission` has always routed to
 * DRAW_LANE_TRANSMISSIVE, and since 11.31 `opacity` decides the blend lane too --
 * classify() derives it rather than the importer writing it back, which is what
 * makes editing opacity here work at all. Before that the slider moved a number
 * nothing read. So the rule is: a param may imply a lane, as long as the lane is
 * DERIVED from it every frame rather than cached at load.
 *
 * Subsurface is absent for a different reason: its consumer is PostFX's
 * scatter-profile table rather than any field here, so no offset describes it.
 *
 * Keys must fit CSCENE_MAX_PARAM_KEY or a scene file truncates them and reports
 * the result as an unknown key.
 */
// The emissive factor exactly as pbr_frag receives it: colour x strength, with
// the glTF fallback where a BLACK factor beside an emissive texture means "the
// texture is the colour".
//
// One place, because there were three. render.c uploaded it, emissive_light.c
// re-derived it to size a derived area panel -- justifying the copy in a header
// comment that claimed the two read the fallback differently, which they did
// not -- and pbr_frag spells the same rule a third way, on a different threshold
// (component sum > 0.001 of the scaled factor, against squared norm < 1e-8 of the
// unscaled colour). The panel's whole premise is that it carries EXACTLY the
// radiance the surface would have emitted, so a drift between the first two is a
// lamp that silently stops matching its own pixels, and no arm can see it.
void material_emissive_factor(const Material* material, vec3 out);

extern const MaterialParam MATERIAL_PARAMS[];
extern const size_t MATERIAL_PARAM_COUNT;

// Look a key up in the table above; NULL if the vocabulary has no such name.
const MaterialParam* material_param_find(const char* key);

// Read/write a parameter generically. `values` is 3 floats for COLOR and one
// for the other value types; TEXTURE rows are not addressable this way.
void material_param_get(const Material* material, const MaterialParam* param, float* values);

// What a TEXTURE row currently points at; NULL for an unbound slot or a row of
// any other type.
const Texture* material_param_texture(const Material* material, const MaterialParam* param);
void material_param_set(Material* material, const MaterialParam* param, const float* values);

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
void set_material_clearcoat_normal_tex(Material* material, Texture* texture);
void set_material_microsurface_tex(Material* material, Texture* texture);
void set_material_anisotropy_tex(Material* material, Texture* texture);

void set_material_splat_tex(Material* material, Texture* texture);
// `index` outside [0, MATERIAL_MAX_LAYERS) is dropped with a warning, not
// clamped -- a clamp would silently write one layer's map over another's.
void set_material_layer_albedo_tex(Material* material, int index, Texture* texture);
void set_material_layer_surface_tex(Material* material, int index, Texture* texture);

#endif // _MATERIAL_H_
