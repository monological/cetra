#ifndef _UBO_H_
#define _UBO_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <stddef.h>

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
#define UBO_BINDING_VIEW            3
#define UBO_BINDING_INSTANCES       4

// std140 byte sizes of the engine's blocks, asserted against the C mirror
// structs (light_cluster.h) and validated against the driver's
// GL_UNIFORM_BLOCK_DATA_SIZE by ubo_validate_program_block. All under the
// GL 4.1 guaranteed GL_MAX_UNIFORM_BLOCK_SIZE minimum of 16384.
#define UBO_LIGHTS_BLOCK_SIZE          12512
#define UBO_CLUSTERS_BLOCK_SIZE        12288
#define UBO_CLUSTER_INDICES_BLOCK_SIZE 12288
// Four floats, rounded up to std140's 16-byte block granularity.
#define UBO_VIEW_BLOCK_SIZE 16

// Per-instance transforms: model, prevModel and the normal matrix, the three
// values a draw needs per object. 64 instances x 3 mat4 x 64 B = 12288, which
// is the same size the cluster blocks already use and comfortably inside the
// 16 KB floor -- so a chunk is 64 instances and a longer run submits in
// several draws.
//
// The normal matrix is stored as a mat4 rather than std140's padded mat3. It
// costs 16 bytes an instance and removes the whole class of hand-packed-column
// bugs that ubo_validate_program_block exists to catch.
#define UBO_INSTANCE_MAX         64
#define UBO_INSTANCES_BLOCK_SIZE 12288

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
//
// Always the WHOLE buffer, even where the consumer reads a prefix of it. A
// partial write after the orphan measured 4.3 ms/frame SLOWER than sending all
// 12 KB on the instance block (spec 11.28): replacing the whole allocation
// lets the driver hand back a fresh block, where a prefix leaves it owing an
// answer for the rest, and that costs more than the copy it saves.
void ubo_upload(Ubo* ubo, const void* data, GLsizeiptr size);

// Wire a program's named block to a binding point and check the driver's
// std140 size against the C-side expectation (the guard against the
// silent-garbage failure mode of a C/GLSL layout drift, which logs on
// mismatch). Call once after link. A program without the block -- absent, or
// stripped as unreferenced -- is a no-op: glGetUniformBlockIndex returns
// GL_INVALID_INDEX, so one lookup answers both questions.
//
// Returns whether the program can actually read this block: present AND the
// driver's layout agrees with ours. A mismatch returns false rather than
// wiring it anyway, so a caller that gates on the result reads no floats
// through a layout it does not understand.
bool ubo_wire_program_block(GLuint program_id, const char* block_name, GLuint binding,
                            GLsizeiptr expected_size);

// Wire every engine-owned block a program might declare: the three
// clustered-forward light blocks (spec 9.1), ViewParams (spec 10.1) and
// InstanceBlock (spec 11.28). Programs that don't sample a block are
// unaffected.
//
// Returns whether the program declares a usable InstanceBlock. Every other
// block is data a shader reads or ignores; this one alone changes what the
// SUBMITTER may do, because a program without it takes its transform from a
// per-draw uniform and a batch would draw every instance at the first one's.
// Resolving it from the linked program is what makes that structural rather
// than a hand-kept list of which programs are safe to batch.
//
// ViewParams goes through here rather than a per-program upload on purpose. It
// carries the working-space contract, which EVERY pass writing scene radiance
// must honour -- skybox, sky background, particles and the fog composite each
// do their own glUseProgram and are missed by render.c's scene-traversal
// uniform block. Eight hand-maintained upload sites is how a ninth pass gets
// added silently wrong.
bool ubo_wire_blocks(GLuint program_id);

#endif // _UBO_H_
