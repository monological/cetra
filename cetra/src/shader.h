#ifndef _SHADER_H_
#define _SHADER_H_

#include <GL/glew.h>

#include "shader_strings.h"

typedef enum {
    VERTEX_SHADER,
    GEOMETRY_SHADER,
    FRAGMENT_SHADER
} ShaderType;

typedef struct {
    GLuint shaderID;
    ShaderType type;
    char* source;
} Shader;

Shader* create_shader(ShaderType type, const char* source);
Shader* create_shader_from_path(ShaderType type, const char* file_path);
void free_shader(Shader* shader);

GLboolean compile_shader(Shader* shader);

#endif // _SHADER_H_

