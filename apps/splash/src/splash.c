// Splash Screen Demo - Text Rendering Test
//
// Displays "C E T R A" text on screen using the text rendering system

#include <stdio.h>
#include <stdlib.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/text.h"

int main(int argc, const char* argv[]) {
    (void)argc;
    (void)argv;

    printf("=== CETRA Splash Demo ===\n\n");

    const char* font_path = "apps/splash/assets/Roboto-Bold.ttf";

    // Create engine
    Engine* engine = create_engine("CETRA", 1280, 720);
    if (!engine) {
        fprintf(stderr, "Failed to create engine\n");
        return -1;
    }

    if (init_engine(engine) != 0) {
        fprintf(stderr, "Failed to initialize engine\n");
        free_engine(engine);
        return -1;
    }

    // Load font
    Font* font = load_font(engine->text_renderer->font_pool, font_path, 128.0f, true);
    if (!font) {
        fprintf(stderr, "Failed to load font: %s\n", font_path);
        free_engine(engine);
        return -1;
    }
    printf("Font loaded successfully\n");

    // Create text mesh for "C E T R A"
    TextMesh* title = create_text_mesh(font, "C E T R A", 120.0f);
    if (!title) {
        fprintf(stderr, "Failed to create text mesh\n");
        free_engine(engine);
        return -1;
    }

    // Set white color
    vec4 white = {1.0f, 1.0f, 1.0f, 1.0f};
    text_mesh_set_color(title, white);

    // Center the text on screen
    float font_size = 120.0f;
    float bx0, by0, bx1, by1;
    text_measure_bounds(font, "C E T R A", font_size, &bx0, &by0, &bx1, &by1);

    float x = (engine->win_width - (bx0 + bx1)) / 2.0f;
    float y = (engine->win_height - (by0 + by1)) / 2.0f;
    vec3 pos = {x, y, 0.0f};
    text_mesh_set_position(title, pos);

    // Build and upload
    text_mesh_rebuild(title);
    text_mesh_upload(title);

    printf("Text mesh created: bounds=(%.1f,%.1f)-(%.1f,%.1f), pos=(%.1f, %.1f)\n", bx0, by0, bx1,
           by1, x, y);

    // Main loop
    while (!glfwWindowShouldClose(engine->window)) {
        glfwPollEvents();

        // Check for escape
        if (glfwGetKey(engine->window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            break;
        }

        // Clear screen with near-black background
        glClearColor(0.02f, 0.02f, 0.02f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Render text
        render_text_2d(engine->text_renderer, title);

        glfwSwapBuffers(engine->window);
    }

    // Cleanup
    free_text_mesh(title);
    free_engine(engine);

    printf("Goodbye!\n");
    return 0;
}
