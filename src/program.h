#ifndef _PROGRAM_H_
#define _PROGRAM_H_

#include "shader.h"

typedef struct {
    GLuint id;

    Shader** shaders;
    size_t shader_count;

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

} ShaderProgram;

ShaderProgram* create_program();
void attach_shader(ShaderProgram* program, Shader* shader);
GLboolean link_program(ShaderProgram* program);
void setup_program_uniforms(ShaderProgram* program);
GLboolean init_shader_program(ShaderProgram* program, const char* vert_path, const char* frag_path, const char* geom_path);
GLboolean validate_program(ShaderProgram* program);
void free_program(ShaderProgram* program);


#endif // _PROGRAM_H_


