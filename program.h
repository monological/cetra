#ifndef _PROGRAM_H_
#define _PROGRAM_H_

#include "shader.h"

typedef struct {
    GLuint programID; // ID for the shader program
} Program;

Program* create_program();
void attach_shader(Program* program, Shader* shader);
GLboolean link_program(Program* program);
void free_program(Program* program);

#endif // _PROGRAM_H_


