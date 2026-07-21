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
typedef void (*GameRenderFunc)(struct Game* game, double alpha);
typedef void (*GameShutdownFunc)(struct Game* game);

// Game configuration
typedef struct GameConfig {
    const char* title;
    int width;
    int height;
    double fixed_timestep; // Physics/logic update rate (default: 1/60)
    double max_frame_time; // Max frame time before clamping (default: 0.25)
    bool vsync;            // Enable vsync (default: true)
    bool show_debug_gui;   // Show debug GUI (default: false)
    // Headless / CI verification (routed onto the engine, which owns the loop):
    // hidden window + no vsync, deterministic frame-count exit, and a final-frame
    // PPM screenshot.
    bool headless;               // Hidden window, no vsync (set before init_engine)
    int exit_after_frames;       // Exit cleanly after N rendered frames (0 = run forever)
    const char* screenshot_path; // Save the final frame here as PPM (NULL = off)
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

    // State
    bool paused;

    // Callbacks
    GameInitFunc on_init;
    GameUpdateFunc on_update;
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
void game_set_render(Game* game, GameRenderFunc func);
void game_set_shutdown(Game* game, GameShutdownFunc func);

// Set user data
void game_set_user_data(Game* game, void* data);
void* game_get_user_data(const Game* game);

// Run the game loop (blocking)
void run_game(Game* game);

// Request game exit
void game_quit(Game* game);

// Pause/unpause
void game_pause(Game* game);
void game_unpause(Game* game);
void game_toggle_pause(Game* game);
bool game_is_paused(const Game* game);

// Scene management
void game_set_scene(Game* game, Scene* scene);
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
