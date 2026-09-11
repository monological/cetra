#ifndef _GAME_H_
#define _GAME_H_

/*
 * The game framework: a fixed-timestep loop over the Engine, with an optional
 * Jolt physics world, an entity manager and character controllers, and the
 * particle tick. An app that wants a simulation step rather than a frame
 * hook builds on this; a viewer or a sketch uses engine_run directly. The
 * physics world and the entity manager are installed by the app, owned here
 * and freed before the engine.
 */

#include <stdbool.h>
#include <cglm/types.h>

#include "input.h"
#include "../engine.h"
#include "../scene.h"

// Forward declarations
struct Game;
struct PhysicsWorld;
struct EntityManager;
struct AudioSystem;

// Game callbacks - implement these in your game
typedef void (*GameInitFunc)(struct Game* game);
/*
 * Once per FRAME, immediately after the input poll and BEFORE the fixed steps.
 *
 * This is the only window in which a UI can take input away from the game
 * without losing a frame in each direction: on_update runs inside the step loop
 * (zero to N times a frame) and pre-render runs after it, so a menu opened
 * there would let the same frame also walk the character, and closing it would
 * drop a step of real input. Anything that decides what the sim is allowed to
 * read belongs here; anything that moves the world does not.
 */
typedef void (*GameFrameInputFunc)(struct Game* game);
typedef void (*GameUpdateFunc)(struct Game* game, double dt);
// Everything that must be settled before the frame reads the geometry: the
// camera, and any node added, removed or moved. The engine propagates the graph
// as soon as it returns, so the shadow pass and the LOD selection see this
// frame's positions (spec 11.96). `alpha` is the same interpolant on_render
// gets. Nothing here may draw.
//
// on_update is equally early and equally fine for moving a node -- it runs well
// before the walk. What this hook is FOR is the work that needs the frame's
// final camera, which the fixed step does not have.
typedef void (*GamePreRenderFunc)(struct Game* game, double alpha);
// Draws the scene. Unset, the loop draws it itself (engine_render_scene on
// game->scene); set one for what an app does around that draw.
typedef void (*GameRenderFunc)(struct Game* game, double alpha);
typedef void (*GameShutdownFunc)(struct Game* game);

// What a game is created from: the engine's own config, plus the loop and the
// cook, which are the two things this layer owns that the engine does not.
// Zero is the default throughout, so a designated initialiser naming only the
// title is a complete config. Anything about the RUN rather than the creation
// -- the frame limit, the screenshot path -- is set on game->engine afterwards.
typedef struct GameConfig {
    EngineConfig engine;
    double fixed_timestep; // Physics/logic update rate (0 = 1/60)
    double max_frame_time; // Max frame time before clamping (0 = 0.25)
    // The derived-data cook (spec 11.99). Config fields because cook_init must
    // precede on_init, whose bakes are the fetch sites, and create_game owns
    // that call.
    const char* cook_dir; // NULL = CETRA_COOK_DIR, then the repo default
    bool no_cook;         // true = every fetch misses and nothing is stored
} GameConfig;

// Main game structure
typedef struct Game {
    // Cetra engine (owns window, rendering)
    Engine* engine;

    // Current scene
    Scene* scene;

    // Input state (polled)
    GameInputState input;

    // Timing (fixed-timestep sim only; frame dt / FPS / screenshot / frame-limit
    // all live on the Engine now that it owns the loop)
    double fixed_timestep;
    double accumulator;
    double time;           // Total game (sim) time
    double max_frame_time; // Frame-time clamp (spiral-of-death guard)

    // The sim clock, published for the engine to sample as the frame's animation
    // clock (engine_set_render_clock, wired once in game_run). `.time` mirrors
    // `time` above; `.delta` is how far the sim actually advanced this frame --
    // a whole number of fixed steps, so 0 on a frame that did not step and 0
    // while paused. Wind then holds still when the sim does, and its motion
    // vectors describe the step that really happened rather than a wall-clock
    // interval the sim never took.
    EngineFrameClock sim_clock;

    // State
    bool paused;

    // Callbacks
    GameInitFunc on_init;
    GameFrameInputFunc on_frame_input;
    GameUpdateFunc on_update;
    GamePreRenderFunc on_pre_render;
    GameRenderFunc on_render;
    GameShutdownFunc on_shutdown;

    // User data pointer
    void* user_data;

    // Physics (optional)
    struct PhysicsWorld* physics_world;

    // Entity management (optional)
    struct EntityManager* entity_manager;

    // Audio (optional). Freed after the entity manager, whose AUDIO_SOURCE
    // components hold sounds that live in this engine.
    struct AudioSystem* audio;
} Game;

// Creates and initialises the engine from config->engine; NULL when that fails.
Game* create_game(const GameConfig* config);

// Free game resources
void free_game(Game* game);

// Set callbacks before running
void game_set_init(Game* game, GameInitFunc func);
void game_set_frame_input(Game* game, GameFrameInputFunc func);
void game_set_update(Game* game, GameUpdateFunc func);
void game_set_pre_render(Game* game, GamePreRenderFunc func);
void game_set_render(Game* game, GameRenderFunc func);
void game_set_shutdown(Game* game, GameShutdownFunc func);

// Set user data
void game_set_user_data(Game* game, void* data);
void* game_get_user_data(const Game* game);

// Run the game loop (blocking)
void game_run(Game* game);

// Request game exit
void game_quit(Game* game);

// Pause/unpause
void game_pause(Game* game);
void game_unpause(Game* game);
void game_toggle_pause(Game* game);
bool game_is_paused(const Game* game);

// Scene management. Installs the default origin-shift callback below, so a game
// gets large-world shifting correct without writing any of it.
void game_set_scene(Game* game, Scene* scene);

// Move the FRAMEWORK's world-space state after an origin shift (spec 11.62):
// physics bodies, character controllers, and the entity positions cached from
// them. Installed by game_set_scene; exposed so an app that needs to move
// something of its OWN can replace the callback and still chain to this rather
// than reimplement it. `ctx` is the Game.
void game_on_origin_shift_default(const vec3 delta, void* ctx);
Scene* game_get_scene(const Game* game);

// Get fixed timestep (for physics calculations)
double game_get_fixed_timestep(const Game* game);

// Get total game time
double game_get_time(const Game* game);

// Get FPS
double game_get_fps(const Game* game);

// The two owned subsystems. Each install takes ownership and frees the one it
// replaces; free_game frees both, the entity manager first.
void game_set_physics_world(Game* game, struct PhysicsWorld* world);
struct PhysicsWorld* game_get_physics_world(const Game* game);
void game_set_entity_manager(Game* game, struct EntityManager* em);
struct EntityManager* game_get_entity_manager(const Game* game);

// The audio subsystem (spec 12.0). Install takes ownership and frees the one it
// replaces; free_game frees it after the entity manager. The loop points the
// listener along the camera and syncs AUDIO_SOURCE components each frame.
void game_set_audio_system(Game* game, struct AudioSystem* audio);
struct AudioSystem* game_get_audio_system(const Game* game);

#endif // _GAME_H_
