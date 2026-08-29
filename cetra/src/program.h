#ifndef _PROGRAM_H_
#define _PROGRAM_H_

#include <stdbool.h>

#include "shader.h"
#include "uniform.h"

#include "ext/uthash.h"

// Which VERTEX stage a lit-surface variant is built on (spec 11.95). Up here
// only because ShaderProgram carries one; the family's rationale and the rest of
// the variant API are together further down.
typedef enum PbrFamily {
    PBR_FAMILY_RIGID = 0, // pbr_vert: instanced, the default for every app
    PBR_FAMILY_SKINNED,   // pbr_skinned_vert: bone_matrices, one draw per mesh
} PbrFamily;

typedef struct ShaderProgram {
    GLuint id;
    char* name;
    Shader** shaders;
    size_t shader_count;
    UniformManager* uniforms;
    // Whether this program reads its per-object transforms from InstanceBlock,
    // resolved from the linked program (ubo_wire_blocks). False means a draw
    // must carry exactly one object: the transform arrives as a plain uniform,
    // so every instance of a batched draw would land on the first one's.
    bool instanced;
    // Whether the depth prepass may draw this program's meshes. TWO
    // requirements, and they are separate even though today's two flagged
    // programs happen to satisfy both:
    //
    //   1. gl_Position comes from object_position.glsl, so the lean
    //      depth_prepass_vert lands on the same value and the shading pass
    //      survives GL_LEQUAL against it. This is what the flag was named for.
    //   2. The FRAGMENT stage implements passMode == SUBMIT_PASS_DEPTH_ONLY,
    //      because masked meshes are prepassed through this program rather than
    //      the lean one (spec 11.31). A program satisfying (1) but not (2) gets
    //      its passMode upload silently swallowed -- uniform writes are
    //      location-guarded -- and the prepass then stamps depth across the whole
    //      quad with no coverage test, punching a hole in whatever is behind.
    //
    // Both hold only because `pbr` and `pbr_skinned` are the sole setters and
    // both use pbr_frag. Anything setting this for reason (1) alone must either
    // carry pbr_frag or be kept out of the masked sweep.
    //
    // Opt-IN, because the failure is silent and ugly. `shape` is the standing
    // counter-example: it is GL_LINES plus a geometry shader that expands each
    // segment into a screen-facing quad, and its vertex stage emits a WORLD
    // position for the geometry stage to re-project. Prepassing it would stamp
    // depth along unexpanded lines that nothing ever shades. Its meshes are
    // ALPHA_OPAQUE, so the lane alone cannot tell them apart.
    bool depth_prepass_safe;
    // Which lit-surface features this program was compiled with, or -1 for a
    // program that is not a variant at all (spec 11.93). A third derived fact
    // beside the two above, and for their reason: it is settled at build time,
    // and every reader wants the answer rather than the derivation.
    //
    // It replaced a string test -- the resolver formatted "pbr-<mask>" and
    // strcmp'd it against this program's name every frame, per material. That
    // made the program NAMESPACE the membership rule, so any future program
    // called "pbr-anything" would have been silently swapped for a variant.
    int pbr_features;
    // How many sampler uniforms the LINKED program kept, which is not how many
    // its source declares. GL bounds the first against GL_MAX_TEXTURE_IMAGE_UNITS
    // (16 on this platform's guarantee) and says nothing about the second, so a
    // declaration whose every read was compiled away may or may not still be
    // spending a unit -- that is a question about the driver, and this field is
    // how it gets asked.
    //
    // -1 when the count could not be taken, so a reader cannot mistake "not
    // measured" for "declares nothing".
    int sampler_count;
    // Which vertex stage this variant was built on, so the resolver can swap a
    // material within its own family (spec 11.95).
    //
    // Its zero IS meaningful here, unlike pbr_features' -- but harmlessly, since
    // every read is already behind `pbr_features >= 0` and a program that is not
    // a variant never reaches one.
    PbrFamily pbr_family;
    UT_hash_handle hh;
} ShaderProgram;

/*
 * Shader Program Functions
 */
ShaderProgram* create_program(const char* name);
ShaderProgram* create_program_from_paths(const char* name, const char* vert_path,
                                         const char* frag_path, const char* geo_path);
ShaderProgram* create_program_from_source(const char* name, const char* vert_source,
                                          const char* frag_source, const char* geo_source);
void free_program(ShaderProgram* program);

// Hot-reload: recompile shaders from paths and relink program
GLboolean reload_program_from_paths(ShaderProgram* program, const char* vert_path,
                                    const char* frag_path, const char* geo_path);

/*
 * Shader Program Setup Functions
 */
void attach_shader_to_program(ShaderProgram* program, Shader* shader);
GLboolean link_program(ShaderProgram* program);
GLboolean validate_program(ShaderProgram* program);
void setup_program_uniforms(ShaderProgram* program);

/*
 * Preset Programs
 */
/*
 * Lit-surface variants (spec 11.93).
 *
 * A feature a material cannot use still costs it, and the cost is OCCUPANCY
 * rather than work: every gate in pbr_frag is a dynamically uniform branch that
 * an unusing scene already skips, so what is left is live values and declared
 * samplers that every fragment pays for. Measured on forest, whose materials use
 * none of these five: removing them at compile time took the opaque pass from
 * 149.2 to 125.1 ms and the frame from 203.7 to 179.0. Guarding one of the same
 * code paths at RUNTIME instead was worth 0.05 ms, which is the same fact from
 * the other side.
 *
 * The polarity is SUBTRACTIVE -- the defines turn features OFF, and no defines
 * is exactly today's shader. So a resolver that fails to run, or a mask that is
 * never assembled, yields the slow program rather than a fast one missing a
 * feature the material needed. Wrong-and-slow is recoverable; wrong-and-pretty
 * is what ships.
 *
 * SCENE-scoped and MATERIAL-scoped bits both live here, and they are not the
 * same kind of thing. Decals and area lights are properties of the scene -- a
 * material cannot know whether either exists -- while sheen, anisotropy and
 * parallax are the material's own. The mask is the union, so a scene that gains
 * its first decal changes every material's variant.
 */
// The bits, from the file the shader reads them out of. Included rather than
// restated so a mask emitted here cannot mean something else there.
#include "../shaders/include/pbr_features.glsl"

// DECALS and AREA are the scene's, SHEEN, ANISO and PARALLAX the material's --
// which is the split _scene_pbr_features and _material_pbr_features are named
// for. A material cannot know whether the scene has a decal, so the mask is the
// union of the two and a scene gaining its first decal changes every material.

// PbrFamily (declared above, beside the struct that carries one) is which VERTEX
// stage a variant is built on. The two families share pbr_frag EXACTLY, so a
// mask means the same thing in both and the bits above need no per-family
// reading -- which is the whole reason a second family costs a builder argument
// rather than a second set of gates.
//
// It is a family and not a flag because the vertex source, the cache-key prefix
// and the resulting `instanced` answer all follow from it together: a skinned
// program links without an InstanceBlock, which is what keeps 11.28's rule that
// such a program may never carry more than one instance.
//
// Longest "pbr_skinned-<mask>" plus its terminator, with room to spare.
#define PBR_VARIANT_NAME_MAX 32

// The cache key for a variant, and the ONE place the family-and-mask-to-name
// rule lives. Two sites spelling it differently would miss the lookup forever,
// compiling and leaking a fresh program every frame while rendering correctly
// throughout.
void pbr_variant_name(PbrFamily family, unsigned features, char* out, size_t n);

// Compile the variant carrying exactly `features` on `family`'s vertex stage.
// Does not register it -- see engine_pbr_variant, which owns the cache and is
// where callers should go.
ShaderProgram* create_pbr_program_variant(PbrFamily family, unsigned features);

// The full variant of each family, which is the uber-shader and what an app
// hands to set_shader_programs_for_nodes before the resolver narrows it.
ShaderProgram* create_pbr_program();
ShaderProgram* create_pbr_skinned_program();

// Particle Program (instanced camera-facing billboards)
ShaderProgram* create_particle_program();
// Particle GPU-sim UPDATE program (transform feedback; vertex-only, spec 5.2)
ShaderProgram* create_particle_sim_program();
// Captures windOffset itself (transform feedback; vertex-only, spec 11.54), so
// the bound that lets a swaying mesh be culled is checked against the shader
// rather than against a second copy of it.
ShaderProgram* create_wind_probe_program();
ShaderProgram* create_shape_program();
ShaderProgram* create_xyz_program();
ShaderProgram* create_shadow_depth_program();
// Position only, for the depth prepass (spec 11.30). Shares the object-position
// chunk with pbr_vert so the two agree to the bit, which GL_LEQUAL against its
// output depends on.
ShaderProgram* create_depth_prepass_program();
// Water surface (spec 11.32). Its own program rather than a pbr_frag feature
// because it samples the resolved scene depth, and pbr_frag has declared all
// sixteen fragment samplers the driver allows.
ShaderProgram* create_water_program();
// The spectral cascade passes (--water-waves fft): evolve the spectrum, then one
// Stockham inverse-FFT stage per draw. Both write two MRT targets.
ShaderProgram* create_water_spectrum_program();
ShaderProgram* create_water_fft_program();
// Foam accumulation (spec 11.42): one pass a frame over the transformed cascades, holding
// whitewater on the surface after the crest that made it has passed.
ShaderProgram* create_water_foam_program();
// Resolves the depth cascades into the filterable moment cascades (--msm)
ShaderProgram* create_msm_resolve_program();
ShaderProgram* create_shadow_absorb_program();
ShaderProgram* create_tsm_resolve_program();

// IBL Programs
ShaderProgram* create_skybox_program();
ShaderProgram* create_ibl_equirect_to_cube_program();
ShaderProgram* create_ibl_irradiance_program();
ShaderProgram* create_ibl_prefilter_program();
ShaderProgram* create_ibl_charlie_prefilter_program();
ShaderProgram* create_ibl_brdf_program();

// Sky atmosphere LUT programs
ShaderProgram* create_sky_transmittance_program();
ShaderProgram* create_sky_multiscatter_program();
ShaderProgram* create_sky_debug_program();
ShaderProgram* create_sky_view_program();
ShaderProgram* create_sky_env_program();
ShaderProgram* create_sky_background_program();
// Aerial-perspective volume, one draw per slice (spec 9.6)
ShaderProgram* create_sky_aerial_program();
// Cloud-noise volume slice inspector (spec 11.0)
ShaderProgram* create_cloud_noise_debug_program();
// Half-res cloud shell march (spec 11.0)
ShaderProgram* create_cloud_march_program();
ShaderProgram* create_cloud_shadow_program();
// Sky background with the cloud composite (bound only when clouds are on)
ShaderProgram* create_sky_background_clouds_program();
// Env-cubemap face render with the low-quality cloud march (release bakes)
ShaderProgram* create_sky_env_clouds_program();

// Copies/resamples a 2D mask texture into a material-mask-array layer
ShaderProgram* create_mask_copy_program();
ShaderProgram* create_layers_vt_bake_program();
ShaderProgram* create_layers_vt_feedback_program();

// Text Program
ShaderProgram* create_text_program();

// Bone Visualization Program
ShaderProgram* create_bone_program();

// Shadow Catcher Program
ShaderProgram* create_shadow_catcher_program();

// Post-Processing Programs
ShaderProgram* create_bloom_bright_program();
ShaderProgram* create_bloom_down_program();
ShaderProgram* create_bloom_up_program();
ShaderProgram* create_lens_flare_program();
ShaderProgram* create_tonemap_program();
ShaderProgram* create_spec_occ_composite_program();
ShaderProgram* create_gtao_program();
ShaderProgram* create_ssao_blur_program();
ShaderProgram* create_ssr_program();
ShaderProgram* create_ssr_hiz_program();
ShaderProgram* create_upsample_tent_program();
ShaderProgram* create_taa_resolve_program();
// TAAU render-to-post upscaling resolve (render_scale < 1 only)
ShaderProgram* create_taau_resolve_program();
ShaderProgram* create_temporal_accum_program();
ShaderProgram* create_ssgi_composite_program();
ShaderProgram* create_ssgi_accum_program();
ShaderProgram* create_ssgi_atrous_program();
ShaderProgram* create_ssr_atrous_program();
ShaderProgram* create_ssr_accum_program();
ShaderProgram* create_froxel_inject_program();
ShaderProgram* create_froxel_integrate_program();
ShaderProgram* create_froxel_composite_program();
// Downsamples the scene's depth cascades into the fog's own filterable
// exponential representation (spec 11.12); one pass per layer per axis.
ShaderProgram* create_fog_esm_program();
ShaderProgram* create_gi_project_program();
ShaderProgram* create_probe_project_program();
ShaderProgram* create_lum_measure_program();
ShaderProgram* create_lum_histogram_program();
ShaderProgram* create_lum_reduce_program();
ShaderProgram* create_dof_coc_program();
ShaderProgram* create_dof_tile_program();
ShaderProgram* create_dof_dilate_program();
ShaderProgram* create_dof_gather_program();
ShaderProgram* create_dof_composite_program();
ShaderProgram* create_motion_blur_program();
ShaderProgram* create_motion_blur_tilemax_program();
ShaderProgram* create_motion_blur_neighbormax_program();
ShaderProgram* create_sss_gather_program();
ShaderProgram* create_sss_pyr_seed_program();
ShaderProgram* create_sss_pyr_down_program();
ShaderProgram* create_contact_shadow_program();
ShaderProgram* create_oit_resolve_program();

#endif // _PROGRAM_H_
