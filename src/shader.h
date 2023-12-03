#ifndef _SHADER_H_
#define _SHADER_H_

#include <GL/glew.h>

typedef enum {
    VERTEX,
    GEOMETRY,
    FRAGMENT
} ShaderType;

typedef struct {
    GLuint shaderID;
    ShaderType type;
    char* source;
} Shader;

Shader* create_shader_from_path(ShaderType type, const char* file_path);
Shader* create_shader(ShaderType type, const char* source);
GLboolean compile_shader(Shader* shader);
void free_shader(Shader* shader);

#endif // _SHADER_H_

