#include "game.h"
#include "entity.h"
#include "physics.h"
#include "character.h"
#include "../render.h"
#include "../transform.h"
#include "../shadow.h"
#include "../async_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GameConfig game_default_config(void) {
    GameConfig config = {.title = "Game",
                         .width = 1280,
                         .height = 720,
                         .fixed_timestep = 1.0 / 60.0,
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

    // Initialize engine
    if (init_engine(game->engine) != 0) {
        free_engine(game->engine);
        free(game);
        return NULL;
    }

    // Set vsync
    glfwSwapInterval(config->vsync ? 1 : 0);

    // Initialize input
    input_init(&game->input, game->engine->window);

    // Set timing
    game->fixed_timestep = config->fixed_timestep;
    game->max_frame_time = config->max_frame_time;
    game->accumulator = 0.0;
    game->time = 0.0;
    game->last_time = glfwGetTime();

    game->running = true;
    game->paused = false;
    game->show_debug_gui = config->show_debug_gui;

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
    if (game)
        game->running = false;
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

void game_set_scene(Game* game, Scene* scene) {
    if (!game)
        return;

    game->scene = scene;
    if (scene) {
        add_scene_to_engine(game->engine, scene);
    }
}

Scene* game_get_scene(const Game* game) {
    return game ? game->scene : NULL;
}

double game_get_fixed_timestep(const Game* game) {
    return game ? game->fixed_timestep : 1.0 / 60.0;
}

double game_get_time(const Game* game) {
    return game ? game->time : 0.0;
}

double game_get_fps(const Game* game) {
    return game ? game->fps : 0.0;
}

// Internal: process async texture loading
static void process_async_loading(Game* game) {
    if (game->engine->async_loader && game->scene && game->scene->tex_pool) {
        // Process up to 5 completed textures per frame
        async_loader_process_pending(game->engine->async_loader, game->scene->tex_pool, 5);
    }
}

void run_game(Game* game) {
    if (!game || !game->engine) {
        return;
    }

    Engine* engine = game->engine;

    // OpenGL state setup (same as engine render loop)
    glEnable(GL_DEPTH_TEST);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // Call init callback
    if (game->on_init) {
        game->on_init(game);
    }

    // Main loop
    while (game->running && !glfwWindowShouldClose(engine->window)) {
        // Calculate frame time first
        double current_time = glfwGetTime();
        double frame_time = current_time - game->last_time;
        game->last_time = current_time;
        game->delta_time = frame_time;

        // Clamp frame time to avoid spiral of death
        if (frame_time > game->max_frame_time) {
            frame_time = game->max_frame_time;
        }

        // FPS calculation
        game->frame_count++;
        game->fps_timer += frame_time;
        if (game->fps_timer >= 0.5) {
            game->fps = (double)game->frame_count / game->fps_timer;
            game->frame_count = 0;
            game->fps_timer = 0.0;
        }

        // Update input state
        input_update(&game->input);

        // Check for escape to quit
        if (input_key_pressed(&game->input, GLFW_KEY_ESCAPE)) {
            game->running = false;
            continue;
        }

        // Fixed timestep update loop
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
            }
        }

        // Calculate interpolation alpha for smooth rendering
        double alpha = game->accumulator / game->fixed_timestep;

        // Shadow pass (if applicable)
        if (game->scene && game->scene->shadow_system) {
            render_shadow_depth_pass(engine, game->scene);
        }

        // Bind main framebuffer and clear
        glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        // Process async loading
        process_async_loading(game);

        // Call render callback (user handles camera, transforms, render_current_scene)
        if (game->on_render) {
            game->on_render(game, alpha);
        }

        // Render Nuklear GUI (only if enabled)
        if (game->show_debug_gui) {
            render_nuklear_gui(engine);
        }

        // Blit MSAA framebuffer to screen
        glBindFramebuffer(GL_READ_FRAMEBUFFER, engine->framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, engine->fb_width, engine->fb_height, 0, 0, engine->fb_width,
                          engine->fb_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        glfwSwapBuffers(engine->window);
        glfwPollEvents();
    }
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
