#ifndef _FIRE_RENDER_H_
#define _FIRE_RENDER_H_

#include <GL/glew.h>
#include <stdbool.h>
#include <stdint.h>

#include "program.h"

#include "../shaders/include/fire_constants.glsl"

struct Engine;
struct Scene;
struct PostFXLateDraw;

/*
 * The GL half of fire (spec 13.14): each GRID fire's simulation stepped on the GPU, what every
 * fire casts read back, and every fire drawn from the post chain's late draw after the temporal
 * seam. fire.h owns what a fire IS and stays free of GL; this owns how it is stepped and drawn.
 * Engine-owned, created the first frame a scene has fires.
 */

// Frames between a fire's state and the light read back from it: past what a driver keeps in
// flight, so mapping the oldest buffer does not stall, and fixed, so a headless run repeats.
#define FIRE_READBACK_LATENCY 3

// One GRID fire's fields, each a 2D atlas of its z slices (fire_grid.glsl).
typedef struct FireGridGPU {
    int dims[3];  // cells
    int tiles[2]; // slices across and down the atlas
    int atlas[2]; // texels
    // Velocity (m/s) and scalars (temperature above ambient, fuel, soot, reaction), RGBA16F:
    // [0] the state, the rest MacCormack's intermediates and the projection's output.
    GLuint velocity[4];
    GLuint scalars[4];
    GLuint pressure[2];    // R32F ping-pong, kept between steps as the next solve's first guess
    GLuint divergence;     // R32F
    GLuint curl;           // RGBA16F, vorticity and its magnitude
    GLuint obstacle;       // R8, 1 inside a solid
    GLuint partial[2];     // RGBA32F, one texel a slice: the reduction's first pass
    uint32_t obstacle_key; // what the obstacle atlas was voxelised from
    bool checked;          // the framebuffer has been found complete over these targets
} FireGridGPU;

// A FLIPBOOK fire's sheets on the GPU, loaded from the path the fire names and kept while it does.
typedef struct FireBookGPU {
    GLuint sheet;   // SRGB8_ALPHA8, mipped; 0 = the sheet would not load, said once for this path
    GLuint motion;  // RGBA8, linear; 0 = the flipbook has none
    char path[256]; // what the sheets were loaded from
} FireBookGPU;

typedef struct FireRenderer {
    ShaderProgram* programs[FIRE_PROGRAM_COUNT];
    FireBookGPU books[FIRE_MAX];
    bool failed; // a program would not build, or a target would not attach: nothing runs
    GLuint fbo;
    GLuint vao;      // empty: the march reads no attributes
    GLuint quad_vao; // the fullscreen quad every simulation pass draws
    GLuint quad_vbo;
    GLuint blackbody_lut; // fire_blackbody_table, RGBA32F
    FireGridGPU grids[FIRE_MAX];

    // What every fire casts, read back through a ring: two texels a fire, (intensity, its first
    // moments) and (emitted rgb, heat release), mapped FIRE_READBACK_LATENCY frames after issue.
    GLuint result_tex;
    GLuint pbo[FIRE_READBACK_LATENCY];
    int ring_passes;
    bool slot_issued[FIRE_READBACK_LATENCY][FIRE_MAX];

    // --fire-bake's targets: a frame's colour and motion, RGBA32F, and a 1x1 depth of 1, which is
    // a scene with nothing in front of the fire.
    GLuint bake_fbo, bake_color, bake_motion, bake_depth;
    int bake_w, bake_h;
} FireRenderer;

FireRenderer* create_fire_renderer(void);
void free_fire_renderer(FireRenderer* renderer);

// Take each GRID fire's pending steps on the GPU, read back what the fires cast, and drive the
// lights they hold. Every frame there are fires, before the shadow pass: a driven light has to be
// final before it is rendered from.
void fire_simulate(FireRenderer* renderer, struct Engine* engine, struct Scene* scene);

// Draw the scene's fires onto the bound canvas, from the late draw.
void fire_render_draw(FireRenderer* renderer, struct Engine* engine, const struct Scene* scene,
                      const struct PostFXLateDraw* late);

// --fire-probe's GPU half: each GRID fire's fields read back whole -- the peak temperature, the
// heat and soot inside solids, the divergence before and after the last projection, and what
// it casts reduced on the GPU against the same sum on the CPU. Needs a live GL context.
void fire_render_probe(FireRenderer* renderer, struct Engine* engine, const struct Scene* scene);

#endif // _FIRE_RENDER_H_
