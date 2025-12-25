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

    const char* font_path = "apps/splash/assets/Silkscreen-Regular.ttf";

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

    // Create text meshes
    float title_size = 120.0f;
    float subtitle_size = 24.0f;
    float gap = 10.0f;

    TextMesh* title = create_text_mesh(font, "C E T R A", title_size);
    if (!title) {
        fprintf(stderr, "Failed to create text mesh\n");
        free_engine(engine);
        return -1;
    }

    // Set white color
    vec4 white = {1.0f, 1.0f, 1.0f, 1.0f};
    text_mesh_set_color(title, white);

    // Measure title
    float tx0, ty0, tx1, ty1;
    text_measure_bounds(font, "C E T R A", title_size, &tx0, &ty0, &tx1, &ty1);
    float title_height = ty1 - ty0;

    // Measure subtitle height (single char for height reference)
    float sx0, sy0, sx1, sy1;
    text_measure_bounds(font, "E", subtitle_size, &sx0, &sy0, &sx1, &sy1);
    float subtitle_height = sy1 - sy0;
    float total_height = title_height + gap + subtitle_height;

    // Center both texts vertically as a group
    float group_top = (engine->win_height - total_height) / 2.0f;

    // Position title centered
    float title_x = (engine->win_width - (tx0 + tx1)) / 2.0f;
    float title_y = group_top - ty0;
    text_mesh_set_position(title, (vec3){title_x, title_y, 0.0f});

    // Position subtitle below title
    float subtitle_y = group_top + title_height + gap - sy0;

    // Create individual letter meshes for ENGINE, spaced to match title width
    const char* letters = "ENGINE";
    int num_letters = 6;
    TextMesh* letter_meshes[6];
    float letter_bounds[6][4]; // lx0, ly0, lx1, ly1 for each

    for (int i = 0; i < num_letters; i++) {
        char buf[2] = {letters[i], '\0'};
        letter_meshes[i] = create_text_mesh(font, buf, subtitle_size);
        text_mesh_set_color(letter_meshes[i], white);

        text_measure_bounds(font, buf, subtitle_size, &letter_bounds[i][0], &letter_bounds[i][1],
                            &letter_bounds[i][2], &letter_bounds[i][3]);
    }

    // SDF padding is 8px at base size (128), scale it for each font size
    float sdf_spread = 8.0f;
    float title_sdf_pad = sdf_spread * (title_size / font->base_size);
    float sub_sdf_pad = sdf_spread * (subtitle_size / font->base_size);

    // The measured bounds include SDF padding. Actual visible edges are inset by the padding.
    // Title visual edges (compensating for SDF padding)
    float visual_left = title_x + tx0 + title_sdf_pad;
    float visual_right = title_x + tx1 - title_sdf_pad;

    // Subtitle letter bounds also include SDF padding
    float first_e_lx0 = letter_bounds[0][0] + sub_sdf_pad;
    float last_e_lx1 = letter_bounds[num_letters - 1][2] - sub_sdf_pad;

    float first_pos = visual_left - first_e_lx0;
    float last_pos = visual_right - last_e_lx1;

    // Linearly interpolate positions for all letters
    for (int i = 0; i < num_letters; i++) {
        float t = (float)i / (num_letters - 1);
        float pos_x = first_pos + t * (last_pos - first_pos);
        text_mesh_set_position(letter_meshes[i], (vec3){pos_x, subtitle_y, 0.0f});
        text_mesh_rebuild(letter_meshes[i]);
        text_mesh_upload(letter_meshes[i]);
    }

    // Build and upload title
    text_mesh_rebuild(title);
    text_mesh_upload(title);

    // Plasma effect config
    TextEffectConfig fx = {
        .type = TEXT_EFFECT_PLASMA, .time = 0.0f, .plasma = {.speed = 1.0f, .intensity = 1.0f}};

    // Main loop
    while (!glfwWindowShouldClose(engine->window)) {
        glfwPollEvents();

        // Check for escape
        if (glfwGetKey(engine->window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            break;
        }

        fx.time = (float)glfwGetTime();

        // Clear screen with near-black background
        glClearColor(0.02f, 0.02f, 0.02f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Render text with plasma effect
        render_text_2d_effect(engine->text_renderer, title, &fx);
        for (int i = 0; i < num_letters; i++) {
            render_text_2d_effect(engine->text_renderer, letter_meshes[i], &fx);
        }

        glfwSwapBuffers(engine->window);
    }

    // Cleanup
    free_text_mesh(title);
    for (int i = 0; i < num_letters; i++) {
        free_text_mesh(letter_meshes[i]);
    }
    free_engine(engine);

    printf("Goodbye!\n");
    return 0;
}
