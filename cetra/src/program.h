#ifndef _PROGRAM_H_
#define _PROGRAM_H_

#include <stdbool.h>

#include "shader.h"
#include "uniform.h"

#include "ext/uthash.h"

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
ShaderProgram* create_pbr_program();
ShaderProgram* create_pbr_skinned_program();

// Particle Program (instanced camera-facing billboards)
ShaderProgram* create_particle_program();
// Particle GPU-sim UPDATE program (transform feedback; vertex-only, spec 5.2)
ShaderProgram* create_particle_sim_program();
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
ShaderProgram* create_lum_measure_program();
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
