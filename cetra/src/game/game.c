#include "game.h"
#include "entity.h"
#include "physics.h"
#include "character.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GameConfig game_default_config(void) {
    GameConfig config = {.title = "Game",
                         .width = 1280,
                         .height = 720,
                         .fixed_timestep = ENGINE_FIXED_FRAME_DT,
                         .max_frame_time = 0.25,
                         .vsync = true};
    return config;
}

Game* create_game(const GameConfig* config) {
    if (!config) {
        return NULL;
    }

    Game* game = calloc(1, sizeof(Game));
    if (!game) {
        return NULL;
    }

    // Create engine
    game->engine = create_engine(config->title, config->width, config->height);
    if (!game->engine) {
        free(game);
        return NULL;
    }

    // Headless must be set before init_engine (read during GLFW window setup)
    set_engine_headless(game->engine, config->headless);
    // Same reason: init_engine is where the profiler is built.
    set_engine_profiler(game->engine, config->profiler);

    // Initialize engine
    if (init_engine(game->engine) != 0) {
        free_engine(game->engine);
        free(game);
        return NULL;
    }

    // Set vsync (always off in headless so frame-count runs don't block on refresh)
    glfwSwapInterval((config->vsync && !config->headless) ? 1 : 0);

    // The engine now owns the frame loop, so route the game's CI/headless + GUI
    // config onto it (screenshot capture, frame-limit exit, debug panel).
    set_engine_screenshot_path(game->engine, config->screenshot_path);
    set_engine_screenshot_every(game->engine, config->screenshot_every);
    set_engine_exit_after_frames(game->engine, config->exit_after_frames);
    set_engine_show_gui(game->engine, config->show_debug_gui);

    // Initialize input
    input_init(&game->input, game->engine->window);

    // Timing (fixed-timestep sim; accumulator/time are calloc-zeroed)
    game->fixed_timestep = config->fixed_timestep;
    game->max_frame_time = config->max_frame_time;
    game->paused = false;

    return game;
}

void free_game(Game* game) {
    if (!game) {
        return;
    }

    // Call shutdown callback
    if (game->on_shutdown) {
        game->on_shutdown(game);
    }

    // Free entity manager FIRST (character controllers need physics system for inner body cleanup)
    if (game->entity_manager) {
        free_entity_manager(game->entity_manager);
        game->entity_manager = NULL;
    }

    // Free physics world (destroys Jolt resources)
    if (game->physics_world) {
        free_physics_world(game->physics_world);
        game->physics_world = NULL;
    }

    // Engine owns scenes, so don't free scene separately
    if (game->engine) {
        free_engine(game->engine);
    }

    free(game);
}

void game_set_init(Game* game, GameInitFunc func) {
    if (game)
        game->on_init = func;
}

void game_set_update(Game* game, GameUpdateFunc func) {
    if (game)
        game->on_update = func;
}

void game_set_render(Game* game, GameRenderFunc func) {
    if (game)
        game->on_render = func;
}

void game_set_shutdown(Game* game, GameShutdownFunc func) {
    if (game)
        game->on_shutdown = func;
}

void game_set_user_data(Game* game, void* data) {
    if (game)
        game->user_data = data;
}

void* game_get_user_data(const Game* game) {
    return game ? game->user_data : NULL;
}

void game_quit(Game* game) {
    if (game && game->engine)
        glfwSetWindowShouldClose(game->engine->window, GLFW_TRUE);
}

void game_pause(Game* game) {
    if (game)
        game->paused = true;
}

void game_unpause(Game* game) {
    if (game)
        game->paused = false;
}

void game_toggle_pause(Game* game) {
    if (game)
        game->paused = !game->paused;
}

bool game_is_paused(const Game* game) {
    return game ? game->paused : false;
}

void game_on_origin_shift_default(const vec3 delta, void* ctx) {
    Game* game = (Game*)ctx;
    if (!game)
        return;
    if (game->physics_world)
        physics_world_shift_origin(game->physics_world, delta);
    if (game->entity_manager)
        shift_all_character_controllers(game->entity_manager, delta);
}

void game_set_scene(Game* game, Scene* scene) {
    if (!game)
        return;

    game->scene = scene;
    if (scene) {
        add_scene_to_engine(game->engine, scene);
        // An app that needs more than this replaces it and calls back here; the
        // hook is a single slot, and the framework's own state is what an app
        // has no business remembering.
        scene_set_origin_callback(scene, game_on_origin_shift_default, game);
    }
}

Scene* game_get_scene(const Game* game) {
    return game ? game->scene : NULL;
}

double game_get_fixed_timestep(const Game* game) {
    return game ? game->fixed_timestep : ENGINE_FIXED_FRAME_DT;
}

double game_get_time(const Game* game) {
    return game ? game->time : 0.0;
}

double game_get_fps(const Game* game) {
    return (game && game->engine) ? (double)game->engine->fps : 0.0;
}

// engine_run's per-frame update hook: poll input, quit on escape, advance the
// fixed-timestep sim (physics/entities/particles), and stash the interpolation
// alpha for the render callback. Runs once per frame, before the render.
static void game_frame_update(Engine* engine, float dt) {
    Game* game = engine_get_user_data(engine);

    // No steps taken yet this frame. Set before the early return below so a
    // frame that bails still reports an honest zero advance rather than last
    // frame's.
    game->sim_clock.delta = 0.0;

    input_update(&game->input);
    if (input_key_pressed(&game->input, GLFW_KEY_ESCAPE)) {
        glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
        return;
    }

    // The engine passes raw dt; apply the game's own spiral-of-death clamp.
    double frame_time = dt;
    if (frame_time > game->max_frame_time) {
        frame_time = game->max_frame_time;
    }

    if (!game->paused) {
        game->accumulator += frame_time;

        while (game->accumulator >= game->fixed_timestep) {
            // Sync kinematic bodies from entity transforms before physics
            if (game->entity_manager) {
                sync_entities_to_physics(game->entity_manager, (float)game->fixed_timestep);
            }

            // User update callback
            if (game->on_update) {
                game->on_update(game, game->fixed_timestep);
            }

            // Tick scene-attached particle systems (framework-driven, like physics)
            scene_update_particle_systems(game->scene, (float)game->fixed_timestep,
                                          (float)game->time);

            // Update character controllers (after user sets velocities, before physics)
            if (game->entity_manager && game->physics_world) {
                vec3 gravity = {0.0f, -9.81f, 0.0f};
                update_all_character_controllers(game->entity_manager, game->physics_world,
                                                 (float)game->fixed_timestep, gravity);
            }

            // Step physics simulation (4 collision sub-steps for stable constraints)
            if (game->physics_world) {
                physics_world_update(game->physics_world, (float)game->fixed_timestep, 4);

                // Process collision events
                physics_world_process_collisions(game->physics_world);

                // Sync physics results back to entities
                if (game->entity_manager) {
                    sync_physics_to_entities(game->physics_world, game->entity_manager);
                }
            }

            // Sync character controller positions to entities
            if (game->entity_manager) {
                sync_character_controllers_to_entities(game->entity_manager);
            }

            // Sync entity transforms to scene nodes
            if (game->entity_manager) {
                sync_entity_transforms(game->entity_manager);
            }

            game->time += game->fixed_timestep;
            game->accumulator -= game->fixed_timestep;
            game->sim_clock.delta += game->fixed_timestep;
        }
    }

    // Publish the sim clock. Outside the paused check on purpose: a paused game
    // holds its time steady and takes no steps, so wind holds still and reports
    // no motion. engine_run samples this after we return.
    game->sim_clock.time = game->time;
}

// engine_run's render hook: hand the app its on_render with the interpolation
// alpha. The engine owns the framebuffer / G-buffer / present around it.
static void game_scene_render(Engine* engine, Scene* scene) {
    (void)scene;
    Game* game = engine_get_user_data(engine);
    if (game->on_render) {
        // Interpolation alpha, derived (not stored) so an escape-key early-return
        // in game_frame_update can't leave it stale.
        game->on_render(game, game->accumulator / game->fixed_timestep);
    }
}

void run_game(Game* game) {
    if (!game || !game->engine) {
        return;
    }
    // on_init builds the scene (needs the live GL context create_game set up).
    if (game->on_init) {
        game->on_init(game);
    }
    // The engine owns the frame loop; the game plugs in its per-frame sim + render,
    // and stashes itself as the engine's user data so the callbacks can find it.
    engine_set_user_data(game->engine, game);
    // Animate from the sim clock, not the wall clock, for the whole loop.
    engine_set_render_clock(game->engine, &game->sim_clock);
    engine_run(game->engine, game_frame_update, game_scene_render);
}

void game_set_physics_world(Game* game, PhysicsWorld* world) {
    if (game)
        game->physics_world = world;
}

PhysicsWorld* game_get_physics_world(const Game* game) {
    return game ? game->physics_world : NULL;
}

void game_set_entity_manager(Game* game, EntityManager* em) {
    if (game)
        game->entity_manager = em;
}

EntityManager* game_get_entity_manager(const Game* game) {
    return game ? game->entity_manager : NULL;
}
