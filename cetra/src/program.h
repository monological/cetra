#ifndef _PROGRAM_H_
#define _PROGRAM_H_

#include "shader.h"
#include "uniform.h"

#include "ext/uthash.h"

typedef struct ShaderProgram {
    GLuint id;
    char* name;
    Shader** shaders;
    size_t shader_count;
    UniformManager* uniforms;
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
