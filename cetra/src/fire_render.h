#ifndef _FIRE_RENDER_H_
#define _FIRE_RENDER_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <stdint.h>

#include <cglm/cglm.h>

#include "program.h"
#include "uniform.h"

#include "../shaders/include/fire_constants.glsl"

struct Engine;
struct Fire;
struct FireSystem;
struct Scene;
struct PostFXLateDraw;

/*
 * The GL half of fire (spec 13.14): each GRID fire's simulation stepped on the GPU and what it
 * casts read back (fire_sim.c), and every fire composited from the post chain's late draw after
 * the temporal seam (fire_render.c). fire.h owns what a fire IS and stays free of GL; this owns
 * how it is stepped and drawn. Engine-owned, created the first frame a scene has fires.
 */

// Frames between a fire's state and the light read back from it: past what a driver keeps in
// flight, so mapping the oldest buffer does not stall, and fixed, so a headless run repeats.
#define FIRE_READBACK_LATENCY 3

typedef enum FireProgram {
    FIRE_PROGRAM_ADVECT,
    FIRE_PROGRAM_CORRECT,
    FIRE_PROGRAM_CURL,
    FIRE_PROGRAM_REACT,
    FIRE_PROGRAM_DIVERGENCE,
    FIRE_PROGRAM_JACOBI,
    FIRE_PROGRAM_PROJECT,
    FIRE_PROGRAM_REDUCE,
    FIRE_PROGRAM_SUM,
    FIRE_PROGRAM_SLICE,
    FIRE_PROGRAM_MARCH,
    FIRE_PROGRAM_CARD,
    FIRE_PROGRAM_COUNT
} FireProgram;

// One GRID fire's fields, each a 2D atlas of its z slices (fire_grid.glsl).
typedef struct FireGridGPU {
    int dims[3];  // cells
    int tiles[2]; // slices across and down the atlas
    int atlas[2]; // texels
    float cell;   // metres
    vec3 lo;      // the box's low corner, local to the fire's origin
    // Velocity (m/s) and scalars (temperature above ambient, reaction, soot, core), RGBA16F:
    // [0] the state, the rest MacCormack's intermediates and the projection's output.
    GLuint velocity[4];
    GLuint scalars[4];
    GLuint pressure[2]; // R32F ping-pong, kept between steps as the next solve's first guess
    GLuint divergence;  // R32F
    GLuint curl;        // RGBA16F, vorticity and its magnitude
    GLuint obstacle;    // R8, 1 inside a solid
    // RGBA32F, the light's two sums staged: a texel per row of cells (y across, z down), then
    // a texel per slice.
    GLuint rows[2];
    GLuint slices[2];
    uint32_t obstacle_key; // what the obstacle atlas was voxelised from
    bool failed;           // its targets would not attach: this fire does not simulate, said once
} FireGridGPU;

typedef struct FireRenderer {
    // Built the first time a fire needs them; one that will not build is tried once, said once,
    // and leaves only what needs it undrawn.
    ShaderProgram* programs[FIRE_PROGRAM_COUNT];
    bool program_failed[FIRE_PROGRAM_COUNT];
    GLuint fbo;
    GLuint vao;      // empty: the march and the cards read no attributes
    GLuint quad_vao; // the fullscreen quad every simulation pass draws
    GLuint quad_vbo;
    GLuint blackbody_lut; // fire_blackbody_table, RGBA32F
    // Each GRID fire's fields, by its index in `system`: the fire system they were kept for,
    // since a fire at the same index in another system has nothing to do with this one's gas.
    const struct FireSystem* system;
    FireGridGPU grids[FIRE_MAX];

    // What every GRID fire casts, read back through a ring: two texels a fire (FireAnswer),
    // mapped FIRE_READBACK_LATENCY frames after issue.
    GLuint result_tex;
    GLuint pbo[FIRE_READBACK_LATENCY];
    int ring_passes;
    bool slot_issued[FIRE_READBACK_LATENCY][FIRE_MAX];
    bool readback_failed; // a slot would not map, said once
} FireRenderer;

FireRenderer* create_fire_renderer(void);
void free_fire_renderer(FireRenderer* renderer);

// The program for `which` in use, its uniforms returned; NULL when it will not build.
UniformManager* fire_use(FireRenderer* renderer, FireProgram which);
// `tex` on texture unit `unit` as `name`.
void fire_bind(UniformManager* u, int unit, GLuint tex, const char* name);
// fire_emission.glsl's uniforms for `fire`, its blackbody table on `blackbody_unit`.
void fire_emission_uniforms(FireRenderer* renderer, UniformManager* u, const struct Fire* fire,
                            int blackbody_unit);

// Take each GRID fire's pending steps on the GPU and read back what the fires cast. Every frame
// there are fires, after fire_update and before fire_system_drive.
void fire_simulate(FireRenderer* renderer, struct Engine* engine, struct Scene* scene);
// A grid's textures, released.
void fire_grid_gpu_free(FireGridGPU* grid);

// Draw the scene's fires onto the bound canvas, from the late draw.
void fire_render_draw(FireRenderer* renderer, struct Engine* engine, const struct Scene* scene,
                      const struct PostFXLateDraw* late);

// --fire-slice (FireSystem.debug_field): the slice, opaque, into the lower-left corner of a
// finished frame `height` pixels tall -- after the tone map, so its ramp is the colour shown.
void fire_render_slice(FireRenderer* renderer, const struct FireSystem* fs, int height);

// --fire-probe's GPU half: each GRID fire's fields read back whole -- the peak temperature, the
// heat and soot inside solids, the divergence before and after the last projection, and what
// it casts reduced on the GPU against the same sum on the CPU -- and the lights the fires
// drive. Needs a live GL context.
void fire_render_probe(FireRenderer* renderer, const struct Scene* scene);

#endif // _FIRE_RENDER_H_
