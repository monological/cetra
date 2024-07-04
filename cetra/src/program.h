#ifndef _PROGRAM_H_
#define _PROGRAM_H_

#include "shader.h"

#include "ext/uthash.h"

typedef struct {
    GLuint id;

    char* name;

    Shader** shaders;
    size_t shader_count;

    GLint render_mode_loc;
    GLint near_clip_loc;
    GLint far_clip_loc;

    GLint model_loc;
    GLint view_loc;
    GLint proj_loc; 
    GLint cam_pos_loc;  
    GLint time_loc;     

    GLint albedo_loc;
    GLint metallic_loc;
    GLint roughness_loc;
    GLint ao_loc;

    // Texture uniforms
    GLint albedo_tex_loc;
    GLint normal_tex_loc;
    GLint roughness_tex_loc;
    GLint metalness_tex_loc;
    GLint ao_tex_loc;
    GLint emissive_tex_loc;
    GLint height_tex_loc;
    GLint opacity_tex_loc;
    GLint sheen_tex_loc;
    GLint reflectance_tex_loc;

    // Texture exists uniforms
    GLint albedo_tex_exists_loc;
    GLint normal_tex_exists_loc;
    GLint roughness_tex_exists_loc;
    GLint metalness_tex_exists_loc;
    GLint ao_tex_exists_loc;
    GLint emissive_tex_exists_loc;
    GLint height_tex_exists_loc;
    GLint opacity_tex_exists_loc;
    GLint sheen_tex_exists_loc;
    GLint reflectance_tex_exists_loc;

    // Dynamic arrays for light uniform locations
    GLint* light_position_loc;
    GLint* light_direction_loc;
    GLint* light_color_loc;
    GLint* light_specular_loc;
    GLint* light_ambient_loc;
    GLint* light_intensity_loc;
    GLint* light_constant_loc;
    GLint* light_linear_loc;
    GLint* light_quadratic_loc;
    GLint* light_cutOff_loc;
    GLint* light_outerCutOff_loc;
    GLint* light_type_loc;
    GLint* light_size_loc;

    GLint num_lights_loc; // Location for the uniform indicating the number of active lights

    size_t max_lights; // Maximum number of lights the shader program can handle

    // only if drawing LINES, LINE_LOOP, LINE_STRIP
    GLint line_width_loc;

    UT_hash_handle hh;   // Makes this structure hashable
} ShaderProgram;

/*
 * Shader Program Functions
 */
ShaderProgram* create_program(const char* name);
void free_program(ShaderProgram* program);

/*
 * Shader Program Setup
 */ 
void attach_program_shader(ShaderProgram* program, Shader* shader);
GLboolean link_program(ShaderProgram* program);

/*
 * Shader Program Uniforms
 */
void setup_program_uniforms(ShaderProgram* program);

/*
 * Setup Program Shaders
 */
ShaderProgram* setup_program_shader_from_paths(const char* name, const char* vert_path,
        const char* frag_path, const char* geo_path);
ShaderProgram* setup_program_shader_from_source(const char* name, const char* vert_source,
        const char* frag_source, const char* geo_source);
/*
 * Preset Programs
 */
ShaderProgram* create_pbr_program();
ShaderProgram* create_shape_program();
ShaderProgram* create_outline_program();
ShaderProgram* create_xyz_program();

/*
 * Program Validation
 */
GLboolean validate_program(ShaderProgram* program);

size_t calculate_max_lights();


#endif // _PROGRAM_H_


