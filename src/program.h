#ifndef _PROGRAM_H_
#define _PROGRAM_H_

#include "shader.h"

typedef struct {
    GLuint id;

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


} ShaderProgram;

// malloc
ShaderProgram* create_program();
void free_program(ShaderProgram* program);

void attach_program_shader(ShaderProgram* program, Shader* shader);
GLboolean link_program(ShaderProgram* program);
void setup_program_uniforms(ShaderProgram* program);
GLboolean setup_program_shader_from_paths(ShaderProgram** program, const char* vert_path,
        const char* frag_path, const char* geo_path);
GLboolean setup_program_shader_from_source(ShaderProgram** program, const char* vert_source,
        const char* frag_source, const char* geo_source);
GLboolean validate_program(ShaderProgram* program);
size_t calculate_max_lights();


#endif // _PROGRAM_H_


