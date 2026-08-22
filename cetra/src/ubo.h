#ifndef _UBO_H_
#define _UBO_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <stddef.h>

// For SHORE_CHAIN_COLS / SHORE_CHAIN_HISTORY, which size the shore film block below. The
// solver owns those dimensions; this file only lays them out for std140.
#include "shore_chain.h"
// Same relationship: ies.h owns the profile ceiling, this file only lays it out.
#include "ies.h"
// And again: layers_vt_pages.h owns the page-table dimensions.
#include "layers_vt_pages.h"

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
#define UBO_BINDING_SHORE_FILM      5
#define UBO_BINDING_IES             6
#define UBO_BINDING_VT_PAGES        7

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

/*
 * The swash film's tips (spec 11.45), and the reason this feature costs no sampler.
 *
 * pbr_frag has been at 16/16 declared samplers since 4.10 and a simulated swash is state a
 * shader has to read, so on the face of it the film was blocked behind freeing a unit. It is
 * not: the ledger's constraint is on TEXTURE lookups, and clustered forward already
 * establishes that a table small enough for uniform space costs zero units. What a lit
 * surface needs is not the solver's nodes but the TIP per column at a few past times, and
 * that is a few hundred floats.
 *
 * 64 columns x 12 history slots is 768 floats = 192 vec4, plus one vec4 of parameters and 64
 * vec4 of column origins (x, z, nx, nz). 257 vec4 = 4112 bytes, against a cluster block already
 * carrying 12288 every frame.
 *
 * The DIMENSIONS ARE THE SOLVER'S, not this file's. They were declared here as their own 64 and
 * 12 while shore_chain.h declared its own pair, and the packing loop iterated one while indexing
 * an array sized by the other -- so raising the solver's column count alone was a silent
 * out-of-bounds read every frame, with no compiler diagnostic and no GL error. Deriving them
 * makes that unrepresentable. The GLSL copy in shore_film.glsl is still a copy (there is no
 * sharing a `layout(std140)` block's array size with C) and is asserted against these at wire
 * time by the driver's own reported block size.
 */
#define UBO_SHORE_FILM_COLS  SHORE_CHAIN_COLS
#define UBO_SHORE_FILM_SLOTS SHORE_CHAIN_HISTORY
// params(1) + origins(COLS) + tips(COLS*SLOTS/4)
#define UBO_SHORE_FILM_VEC4S \
    (1 + UBO_SHORE_FILM_COLS + (UBO_SHORE_FILM_COLS * UBO_SHORE_FILM_SLOTS) / 4)
#define UBO_SHORE_FILM_BLOCK_SIZE (UBO_SHORE_FILM_VEC4S * 16)
// The tips must fill whole vec4 rows, or the division above silently truncates the ring.
_Static_assert((UBO_SHORE_FILM_COLS * UBO_SHORE_FILM_SLOTS) % 4 == 0,
               "the shore film's tip array must be a whole number of vec4 rows");

/*
 * IES photometric profiles (spec 11.57), and why they cost no sampler either.
 *
 * The roadmap deferred asymmetric profiles "rather than paying a unit for the symmetric 90%",
 * which is a Wall 1 argument that assumes a 2D table means a TEXTURE. It does not: a block
 * costs BYTES, and this one is its own, so the symmetric and asymmetric cases differ only in
 * stride and both ship.
 *
 * ITS OWN BLOCK RATHER THAN ROOM IN LightsBlock, and that is not a preference. ubo_upload
 * orphans the whole allocation and light_cluster.c rewrites only the live prefix
 * (offsetof(cluster_lights) + N * sizeof(GpuPackedLight)), so a table appended after the
 * lights would be undefined every frame; one placed before them would be re-sent on every
 * render_current_scene invocation, which a probe-capture frame enters seven times. Uploaded
 * once at load, here, it is touched by neither.
 *
 * TWO INDEPENDENT LIMITS, because one would have to be sized for the worst case and waste the
 * budget on the common one. IES_MAX_PROFILES caps how many DESCRIPTORS there are;
 * IES_POOL_FLOATS caps the bytes they share. A profile spends only its own v_taps * h_taps, so
 * a symmetric one costs 32 floats against a fully asymmetric one's 512 -- sixteen to one. The
 * loader refuses by name when either runs out rather than truncating a table into a plausible
 * wrong shape.
 *
 * At the ceiling this holds seven fully asymmetric profiles, or a hundred-odd symmetric ones,
 * or any mix. The spec's planning estimate of "~8 asymmetric" was arithmetic done before this
 * assert existed; the assert is what corrected it.
 */
#define UBO_IES_TAPS (IES_MAX_VERT * IES_MAX_HORIZ)
// counts(1) + TWO descriptor rows per profile + the shared pool, 4 taps to a vec4
#define UBO_IES_VEC4S      (1 + IES_MAX_PROFILES * 2 + IES_POOL_FLOATS / 4)
#define UBO_IES_BLOCK_SIZE (UBO_IES_VEC4S * 16)
// The pool must fill whole vec4 rows, or the division above silently truncates its tail --
// the shore film's lesson, in the same shape.
_Static_assert(IES_POOL_FLOATS % 4 == 0, "the IES pool must be a whole number of vec4 rows");
_Static_assert(IES_POOL_FLOATS >= UBO_IES_TAPS,
               "the IES pool must hold at least one profile at the ceiling");
_Static_assert(UBO_IES_BLOCK_SIZE <= 16384,
               "the IES block must fit GL 4.1's guaranteed GL_MAX_UNIFORM_BLOCK_SIZE");
// ...and the C mirror against that size, the way light_cluster.h pins its three.
// ubo_upload rejects only an OVER-size write, so a struct that drifted SHORT
// would upload a partial pool and leave its tail at whatever the orphan gave it,
// with every check above still green.
_Static_assert(sizeof(GpuIesBlock) == UBO_IES_BLOCK_SIZE,
               "IesBlock's C mirror must match its std140 size");

/*
 * The composite cache's page table (spec 11.67), and why RVT costs no sampler
 * for its indirection either: a page table is a TABLE, and the IES block above
 * already established that a table small enough for uniform space costs zero
 * units. The physical pages themselves ride freed sampler units; only the
 * virtual-to-physical mapping lives here.
 *
 * Its own block for the IesBlock reason exactly -- the light blocks orphan and
 * rewrite per frame, where this uploads only when residency changes.
 */
// info(1) + params(1) + the packed entry grid
#define UBO_VT_PAGES_VEC4S      (2 + VT_PAGE_TABLE_VEC4S)
#define UBO_VT_PAGES_BLOCK_SIZE (UBO_VT_PAGES_VEC4S * 16)
_Static_assert(VT_PAGE_TABLE_MAX % 4 == 0, "the page table must be a whole number of ivec4 rows");
_Static_assert(UBO_VT_PAGES_BLOCK_SIZE <= 16384,
               "the page-table block must fit GL 4.1's guaranteed GL_MAX_UNIFORM_BLOCK_SIZE");
_Static_assert(sizeof(GpuVtPageBlock) == UBO_VT_PAGES_BLOCK_SIZE,
               "VtPageBlock's C mirror must match its std140 size");

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
