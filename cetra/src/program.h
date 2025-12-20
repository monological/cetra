#ifndef _PROGRAM_H_
#define _PROGRAM_H_

#include "shader.h"
#include "uniform.h"

#include "ext/uthash.h"

typedef struct {
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
ShaderProgram* create_shape_program();
ShaderProgram* create_xyz_program();

size_t calculate_max_lights();

#endif // _PROGRAM_H_
