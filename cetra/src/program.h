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
ShaderProgram* create_shape_program();
ShaderProgram* create_xyz_program();
ShaderProgram* create_shadow_depth_program();

// IBL Programs
ShaderProgram* create_skybox_program();
ShaderProgram* create_ibl_equirect_to_cube_program();
ShaderProgram* create_ibl_irradiance_program();
ShaderProgram* create_ibl_prefilter_program();
ShaderProgram* create_ibl_brdf_program();

// Sky atmosphere LUT programs
ShaderProgram* create_sky_transmittance_program();
ShaderProgram* create_sky_multiscatter_program();
ShaderProgram* create_sky_debug_program();
ShaderProgram* create_sky_view_program();
ShaderProgram* create_sky_env_program();
ShaderProgram* create_sky_background_program();

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
ShaderProgram* create_gtao_program();
ShaderProgram* create_ssao_blur_program();
ShaderProgram* create_ssr_program();
ShaderProgram* create_ssr_hiz_program();
ShaderProgram* create_upsample_tent_program();
ShaderProgram* create_taa_resolve_program();
ShaderProgram* create_temporal_accum_program();
ShaderProgram* create_ssgi_composite_program();
ShaderProgram* create_ssgi_accum_program();
ShaderProgram* create_ssgi_atrous_program();
ShaderProgram* create_fog_program();
ShaderProgram* create_lum_measure_program();
ShaderProgram* create_lum_adapt_program();
ShaderProgram* create_dof_coc_program();
ShaderProgram* create_dof_blur_program();
ShaderProgram* create_dof_composite_program();

size_t calculate_max_lights();

#endif // _PROGRAM_H_
