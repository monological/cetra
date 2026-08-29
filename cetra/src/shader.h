#ifndef _SHADER_H_
#define _SHADER_H_

#include <GL/glew.h>

#include "shader_strings.h"

typedef enum { VERTEX_SHADER, GEOMETRY_SHADER, FRAGMENT_SHADER } ShaderType;

typedef struct {
    GLuint shaderID;
    ShaderType type;
    char* source;
} Shader;

// `source` with a block of preprocessor lines spliced in after its `#version`
// directive. Caller owns the result; a NULL source, or NULL/empty defines,
// returns a plain copy.
//
// Splices rather than prepends because `#version` must be the first thing in a
// translation unit, and returns ONE string rather than feeding glShaderSource a
// second element so a compile error prints the text the driver actually saw.
//
// Follows the block with `#line 2` so the body's reported line numbers do not
// move -- see the note at the implementation for why that matters more here than
// it looks.
//
// A free function rather than a parameter on create_shader, which was the first
// shape and the wrong one: only the lit surface is ever built as a variant, so
// threading it through create_shader and create_program_from_source would have
// put a `NULL` at 39 call sites that cannot want one. A variant is a property of
// the SOURCE, so the caller that knows it is building one splices, and every
// other path is untouched.
char* shader_source_with_defines(const char* source, const char* defines);

Shader* create_shader(ShaderType type, const char* source);
Shader* create_shader_from_path(ShaderType type, const char* file_path);
void free_shader(Shader* shader);

GLboolean compile_shader(Shader* shader);

#endif // _SHADER_H_
