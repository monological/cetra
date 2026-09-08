#ifndef _GAME_H_
#define _GAME_H_

#include <stdbool.h>
#include <cglm/types.h>

#include "input.h"
#include "../engine.h"
#include "../scene.h"

// Forward declarations
struct Game;
struct PhysicsWorld;
struct EntityManager;

// Game callbacks - implement these in your game
typedef void (*GameInitFunc)(struct Game* game);
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
typedef void (*GameRenderFunc)(struct Game* game, double alpha);
typedef void (*GameShutdownFunc)(struct Game* game);

// What a game is created from: the engine's own config, plus the loop and the
// cook, which are the two things this layer owns that the engine does not.
// Everything a game app used to route through here to reach the engine before
// init -- headless, the profiler, the sample count -- is in `engine` now, and
// what it set afterwards (the screenshot path, the frame limit, the debug GUI)
// is set on game->engine after create_game, where it always could be.
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
} Game;

// Default configuration
GameConfig game_default_config(void);

// Create game with configuration
Game* create_game(const GameConfig* config);

// Free game resources
void free_game(Game* game);

// Set callbacks before running
void game_set_init(Game* game, GameInitFunc func);
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

// Physics management
void game_set_physics_world(Game* game, struct PhysicsWorld* world);
struct PhysicsWorld* game_get_physics_world(const Game* game);

// Entity management
void game_set_entity_manager(Game* game, struct EntityManager* em);
struct EntityManager* game_get_entity_manager(const Game* game);

#endif // _GAME_H_
