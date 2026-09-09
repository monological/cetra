// sprites: the 2023 spinning particle globe sketch on cetra, behaving as it
// did. 540 points placed on a sphere of radius 0.2, seen from five units away, each
// with its own vertical jitter that picks a new target every second, coloured
// at random every frame, sized by depth, on black. The sketch's rotation is
// kept as written: a slowly growing angle applied to the positions again each
// frame, so the globe turns about the vertical axis and speeds up over time,
// with the same angle also tilting the whole thing about X.
//
// The sketch drew GL_POINTS; here the points are one particle emitter on the
// billboard renderer, with the placement and the motion in modules of our own.
//
// Flags: -x hides the window, -f N exits after N frames, -S path writes the
// last frame as a binary PPM.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/light.h"
#include "cetra/particle_emitter.h"
#include "cetra/particle_module.h"
#include "cetra/internal/particle_pool.h"
#include "cetra/particle_renderer.h"
#include "cetra/particle_sim.h"
#include "cetra/particle_system.h"
#include "cetra/postfx.h"
#include "cetra/program.h"
#include "cetra/scene.h"
#include "cetra/texture.h"

// The sketch's constants.
#define NUM_PARTICLES    540
#define GLOBE_RADIUS     0.2f
#define SPIN_SPEED       0.001f
#define JITTER_AMOUNT    0.9f
#define DEAD_ZONE_HEIGHT 0.01f
#define CAMERA_DISTANCE  5.0f
#define FOV_DEG          45.0f
#define MIN_SIZE_PX      1.0f
#define MAX_SIZE_PX      7.0f
#define WINDOW_W         640
#define WINDOW_H         480

// Per-particle state the pool does not carry. Nothing ever dies, so a pool
// slot keeps its index and these arrays can be indexed by it.
typedef struct {
    float x[NUM_PARTICLES]; // the rotating position, as the sketch stored it
    float z[NUM_PARTICLES];
    float original_y[NUM_PARTICLES];
    float jitter[NUM_PARTICLES];
    float target[NUM_PARTICLES];
    float jitter_time[NUM_PARTICLES];
    bool skipped[NUM_PARTICLES]; // the sketch left these zeroed at the origin
    float angle;                 // the sketch's accumulating angle
    float unit_per_px;           // world units of one pixel at the globe
    bool spawned;
} GlobeState;

static GlobeState globe;

static float rand_jitter(ParticleEmitter* e) {
    return particle_emitter_rand01(e) * JITTER_AMOUNT - JITTER_AMOUNT / 2.0f;
}

// SPAWN: every particle at once, on the first step, and never again.
static void spawn_once(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end, float dt,
                       float t) {
    (void)m;
    (void)begin;
    (void)end;
    (void)dt;
    (void)t;
    if (!globe.spawned) {
        e->spawn_request = NUM_PARTICLES;
        globe.spawned = true;
    }
}

// INIT: the sketch's setupParticles, including the particles it skipped when
// their jittered height fell in the dead zone, which stayed at the origin.
static void init_sphere(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end, float dt,
                        float t) {
    (void)m;
    (void)dt;
    (void)t;
    ParticlePool* pool = e->pool;
    for (size_t i = begin; i < end && i < NUM_PARTICLES; i++) {
        float theta = acosf(2.0f * particle_emitter_rand01(e) - 1.0f) - GLM_PIf / 2.0f;
        float phi = particle_emitter_rand01(e) * 2.0f * GLM_PIf;
        float x = GLOBE_RADIUS * cosf(theta) * cosf(phi);
        float y = GLOBE_RADIUS * cosf(theta) * sinf(phi);
        float z = GLOBE_RADIUS * sinf(theta);
        float jitter = rand_jitter(e);

        globe.skipped[i] = fabsf(y + jitter) < DEAD_ZONE_HEIGHT / 2.0f;
        if (!globe.skipped[i]) {
            globe.x[i] = x;
            globe.z[i] = z;
            globe.original_y[i] = y;
            globe.jitter[i] = jitter;
            globe.target[i] = rand_jitter(e);
        }
        glm_vec3_zero(pool->position[i]);
        // The renderer fades a particle in over the first tenth of its life
        // and out over the last third, so one that must never fade sits in the
        // middle of a very long one.
        pool->lifetime[i] = 1.0e6f;
        pool->age[i] = 0.5e6f;
        pool->size[i] = globe.unit_per_px * MIN_SIZE_PX;
        glm_vec4_copy((vec4){0.0f, 0.0f, 0.0f, 1.0f}, pool->color[i]);
    }
}

// UPDATE: the sketch's frame, in its order. The angle grows by dt * SPIN_SPEED
// and the positions are rotated by the whole of it every frame (the sketch's
// updateParticlePositions), so the spin accelerates. The jitter interpolates
// toward a target that is redrawn each second. The same angle then tilts
// everything about X, which was the sketch's glRotatef.
static void update_globe(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end, float dt,
                         float t) {
    (void)m;
    (void)t;
    ParticlePool* pool = e->pool;

    globe.angle -= dt * SPIN_SPEED;
    globe.angle = fmodf(globe.angle, 360.0f);
    float c = cosf(globe.angle);
    float s = sinf(globe.angle);

    for (size_t i = begin; i < end && i < NUM_PARTICLES; i++) {
        if (globe.skipped[i]) {
            // At the origin, black, sized as a point at z = 0 was.
            pool->size[i] = globe.unit_per_px * (MIN_SIZE_PX + (MAX_SIZE_PX - MIN_SIZE_PX) * 0.5f);
            continue;
        }

        float x = globe.x[i];
        float z = globe.z[i];
        globe.x[i] = c * x - s * z;
        globe.z[i] = s * x + c * z;

        globe.jitter_time[i] += dt;
        if (globe.jitter_time[i] >= 1.0f) {
            globe.jitter_time[i] -= 1.0f;
            globe.jitter[i] = globe.target[i];
            globe.target[i] = rand_jitter(e);
        }
        float k = globe.jitter_time[i];
        float y = globe.original_y[i] + globe.jitter[i] * (1.0f - k) + globe.target[i] * k;

        // Point size from the depth before the tilt, as the sketch read it.
        float depth = (globe.z[i] + GLOBE_RADIUS) / (2.0f * GLOBE_RADIUS);
        pool->size[i] = globe.unit_per_px * (MIN_SIZE_PX + (MAX_SIZE_PX - MIN_SIZE_PX) * depth);

        // The modelview tilt about X by the same angle.
        pool->position[i][0] = globe.x[i];
        pool->position[i][1] = c * y - s * globe.z[i];
        pool->position[i][2] = s * y + c * globe.z[i];

        if (fabsf(y) < DEAD_ZONE_HEIGHT / 2.0f) {
            glm_vec4_copy((vec4){0.0f, 0.0f, 0.0f, 1.0f}, pool->color[i]);
        } else {
            glm_vec4_copy((vec4){particle_emitter_rand01(e), particle_emitter_rand01(e),
                                 particle_emitter_rand01(e), 1.0f},
                          pool->color[i]);
        }
    }
}

int main(int argc, char** argv) {
    bool headless = false;
    int frames = 0;
    const char* screenshot = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-x") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
            screenshot = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [-x] [-f frames] [-S out.ppm]\n", argv[0]);
            return 2;
        }
    }

    EngineConfig cfg = {
        .title = "sprites", .width = WINDOW_W, .height = WINDOW_H, .headless = headless};
    Engine* engine = create_engine(&cfg);
    if (!engine)
        return 1;
    engine->exit_after_frames = frames;
    engine_set_screenshot_path(engine, screenshot);
    engine->show_gui = false;
    engine->show_fps = !headless;

    // The sketch's camera: 45 degrees, five units back along Z.
    CameraDesc camera = {.position = {0.0f, 0.0f, CAMERA_DISTANCE},
                         .fov = glm_rad(FOV_DEG),
                         .near = 0.1f,
                         .far = 100.0f};
    engine_set_camera(engine, create_camera(&camera));

    // The sketch's point sizes are pixels across, and glPointSize counts
    // FRAMEBUFFER pixels, which on a Retina display are half a window pixel.
    // A billboard's size is world units from centre to edge, so: half a
    // framebuffer pixel at the globe's distance.
    float view_height = 2.0f * CAMERA_DISTANCE * tanf(glm_rad(FOV_DEG) / 2.0f);
    globe.unit_per_px = 0.5f * view_height / (float)engine->fb_height;

    Scene* scene = create_scene();
    engine_add_scene(engine, scene);
    SceneNode* root = scene->root_node;
    // No post effects, and the raw values out: the sketch drew its colours
    // straight into the framebuffer with no display encode, so passthrough
    // rather than the preset's linear curve, which encodes.
    engine_set_2d_preset(engine, scene);
    engine->postfx->tonemap_mode = POSTFX_TONEMAP_PASSTHROUGH;

    // The sketch cleared to black.
    glm_vec3_zero(engine->clear_color);

    // The engine registers no particle program of its own.
    ShaderProgram* particle_program = create_particle_program();
    engine_add_program(engine, particle_program);

    // The CPU backend, because modules written here run only there.
    ParticleSystem* sys = create_particle_system("sprites");
    particle_system_set_backend(sys, create_cpu_particle_sim_backend());

    // GL_POINTS are hard squares. The renderer's default is a soft disc, and a
    // sprite replaces that with the texture's own shape, so a single white
    // pixel makes each point the square it was. Gain 1 and unlit: the colour
    // as picked, with no key light to tint it and none in the scene.
    static const unsigned char white[4] = {255, 255, 255, 255};
    Texture* square =
        texture_load_memory(scene->tex_pool, "point", white, 1, 1, 4, texture_desc(false));

    ParticleEmitter* em = create_particle_emitter("points", NUM_PARTICLES);
    ParticleRenderer* renderer = create_billboard_particle_renderer(particle_program);
    billboard_renderer_set_sprite(renderer, square, 1.0f);
    billboard_renderer_set_lit(renderer, false);
    particle_emitter_set_renderer(em, renderer);
    // The state is the file static above, so the modules carry no params.
    particle_emitter_add_module(
        em, create_particle_module("spawn_once", PARTICLE_PHASE_SPAWN, spawn_once, NULL));
    particle_emitter_add_module(
        em, create_particle_module("init_sphere", PARTICLE_PHASE_INIT, init_sphere, NULL));
    particle_emitter_add_module(
        em, create_particle_module("update_globe", PARTICLE_PHASE_UPDATE, update_globe, NULL));
    particle_system_add_emitter(sys, em);
    scene_add_particle_system(scene, sys); // the scene owns it, ticks it and draws it

    SceneNode* node = create_node();
    node_set_name(node, "sprites");
    node_set_particle_system(node, sys);
    node_add_child(root, node);

    engine_run(engine, NULL, NULL, NULL);
    free_engine(engine);
    return 0;
}
