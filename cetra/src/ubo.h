#ifndef _UBO_H_
#define _UBO_H_

#include <GL/glew.h>
#include <stdbool.h>

// Uniform-buffer plumbing for std140 blocks (first used by clustered forward
// lighting, spec 9.1). GLSL 330 cannot write layout(binding=N) -- that
// qualifier is 4.2 -- so C code owns the block-index -> binding-point wiring:
// each program's named block is bound once after link (ubo_bind_program_block)
// and the buffer sits on the matching indexed GL_UNIFORM_BUFFER binding point
// for the context's lifetime (glBindBufferBase at create).

// Global binding-point registry. Indexed UBO binding points are context state
// shared by every program; keep every assignment here so collisions are
// impossible by construction.
#define UBO_BINDING_LIGHTS          0
#define UBO_BINDING_CLUSTERS        1
#define UBO_BINDING_CLUSTER_INDICES 2

// std140 byte sizes of the engine's blocks, asserted against the C mirror
// structs (light_cluster.h) and validated against the driver's
// GL_UNIFORM_BLOCK_DATA_SIZE by ubo_validate_program_block. All under the
// GL 4.1 guaranteed GL_MAX_UNIFORM_BLOCK_SIZE minimum of 16384.
#define UBO_LIGHTS_BLOCK_SIZE          12512
#define UBO_CLUSTERS_BLOCK_SIZE        12288
#define UBO_CLUSTER_INDICES_BLOCK_SIZE 8192

typedef struct Ubo {
    GLuint id;
    GLsizeiptr size; // fixed allocation size, set at create
    GLuint binding;  // indexed GL_UNIFORM_BUFFER binding point
} Ubo;

// Create a UBO of the given fixed size, zero-initialized (a shader reading
// before the first upload sees zero counts, not garbage), and bind it to the
// indexed binding point. Returns NULL on allocation failure.
Ubo* create_ubo(GLsizeiptr size, GLuint binding);
void free_ubo(Ubo* ubo);

// Replace the buffer contents. Orphans the old storage first
// (glBufferData(NULL), then glBufferSubData) so the driver never stalls on a
// store the previous frame may still be reading. size must be <= create size.
void ubo_upload(Ubo* ubo, const void* data, GLsizeiptr size);

// Wire a program's named block to a binding point; call once after link. A
// program without the block (absent, or stripped as unreferenced) is a no-op:
// glGetUniformBlockIndex returns GL_INVALID_INDEX.
void ubo_bind_program_block(GLuint program_id, const char* block_name, GLuint binding);

// Compare the driver's std140 size of a program's block against the C-side
// expectation; logs and returns false on mismatch -- the guard against the
// silent-garbage failure mode of a C/GLSL layout drift. A program without the
// block returns true.
bool ubo_validate_program_block(GLuint program_id, const char* block_name, GLsizeiptr expected);

#endif // _UBO_H_
