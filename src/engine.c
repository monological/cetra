#include <stdio.h>
#include <stdint.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "scene.h"
#include "camera.h"
#include "shader.h"
#include "program.h"
#include "util.h"
#include "engine.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_KEYSTATE_BASED_INPUT
#define NK_IMPLEMENTATION
#define NK_GLFW_GL3_IMPLEMENTATION
#include "ext/nuklear.h"
#include "ext/nuklear_glfw_gl3.h"

// Function to create and initialize the engine
Engine* create_engine(const char* window_title, int width, int height) {
    Engine* engine = malloc(sizeof(Engine));
    if (!engine) {
        fprintf(stderr, "Failed to allocate memory for engine\n");
        return NULL;
    }

    engine->window = NULL;

    engine->window_title = window_title ? strdup(window_title) : NULL;
    if (window_title && !engine->window_title) {
        fprintf(stderr, "Failed to allocate memory for window title\n");
        free(engine);
        return NULL;
    }

    engine->window_width = width;
    engine->window_height = height;

    engine->error_callback = NULL;
    engine->mouse_button_callback = NULL;
    engine->cursor_position_callback = NULL;

    engine->framebuffer = 0;
    engine->framebuffer_width = 0;
    engine->framebuffer_height = 0;
    engine->multisample_texture = 0;
    engine->depth_renderbuffer = 0;

    engine->camera = NULL;

    engine->scenes = NULL;
    engine->scene_count = 0;
    engine->current_scene_index = 0;
    
    engine->programs = NULL;
    engine->program_count = 0;

    engine->current_render_mode = RENDER_MODE_PBR;

    glm_mat4_identity(engine->model_matrix);
    glm_mat4_identity(engine->view_matrix);
    glm_mat4_identity(engine->projection_matrix);

    engine->show_wireframe = false;
    engine->show_axes = false;

    return engine;
}

void free_engine(Engine* engine) {
    if (!engine) return;

    if (engine->scenes) {
        for (size_t i = 0; i < engine->scene_count; ++i) {
            if (engine->scenes[i]) {
                free_scene(engine->scenes[i]);
            }
        }
        free(engine->scenes);
    }

    if (engine->programs) {
        for (size_t i = 0; i < engine->program_count; ++i) {
            if (engine->programs[i]) {
                free_program(engine->programs[i]);
            }
        }
        free(engine->programs);
    }

    if (engine->camera) {
        free_camera(engine->camera);
    }

    // Destroy GLFW window
    if (engine->window) {
        glfwDestroyWindow(engine->window);
    }

    glDeleteFramebuffers(1, &engine->framebuffer);
    glDeleteTextures(1, &engine->multisample_texture);
    glDeleteRenderbuffers(1, &engine->depth_renderbuffer);

    nk_glfw3_shutdown(&engine->nk_glfw);

    // Terminate GLFW
    glfwTerminate();

    free(engine);
}


void set_engine_error_callback(Engine* engine, GLFWerrorfun error_callback) {
    if (!engine) return;
    engine->error_callback = error_callback;
    if (engine->window) {
        glfwSetErrorCallback(error_callback);
    }
}

void set_engine_mouse_button_callback(Engine* engine, GLFWmousebuttonfun mouse_button_callback) {
    if (!engine) return;
    engine->mouse_button_callback = mouse_button_callback;
    if (engine->window) {
        glfwSetMouseButtonCallback(engine->window, mouse_button_callback);
    }
}

void set_engine_cursor_position_callback(Engine* engine, GLFWcursorposfun cursor_position_callback) {
    engine->cursor_position_callback = cursor_position_callback;
    if (engine->window) {
        glfwSetCursorPosCallback(engine->window, cursor_position_callback);
    }
}

int setup_engine_glfw(Engine* engine) {
    if(!engine){
        return -1;
    }

    if (engine->error_callback) {
        glfwSetErrorCallback(engine->error_callback);
    }

    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 4); // Enable 4x MSAA
    glfwWindowHint(GLFW_DEPTH_BITS, 32);

    engine->window = glfwCreateWindow(engine->window_width, engine->window_height, engine->window_title, NULL, NULL);
    if (engine->window == NULL) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(engine->window);

    // Enable V-Sync
    glfwSwapInterval(1);

    if (engine->mouse_button_callback) {
        glfwSetMouseButtonCallback(engine->window, engine->mouse_button_callback);
    }
    if (engine->cursor_position_callback) {
        glfwSetCursorPosCallback(engine->window, engine->cursor_position_callback);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glfwSwapInterval(1);

    if (glewInit() != GLEW_OK) {
        fprintf(stderr, "Failed to initialize GLEW\n");
        glfwTerminate();
        return -1;
    }

    glfwGetFramebufferSize(engine->window, &(engine->framebuffer_width), &(engine->framebuffer_height));
    glViewport(0, 0, engine->framebuffer_width, engine->framebuffer_height);
    check_gl_error("view port");

    

    return 0; // Success
}

/*
 * MSAA anti-aliasing
 *
 */
void setup_engine_msaa(Engine *engine) {
    if (!engine || !engine->window) return;
    
    // Set up MSAA anti-aliasing
    int samples = 4; // Number of samples for MSAA, adjust as needed

    // Create and set up the multisample framebuffer
    glGenFramebuffers(1, &engine->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);

    glGenTextures(1, &engine->multisample_texture);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, engine->multisample_texture);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGB, engine->framebuffer_width, engine->framebuffer_height, GL_TRUE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, engine->multisample_texture, 0);

    glGenRenderbuffers(1, &engine->depth_renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, engine->depth_renderbuffer);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, engine->framebuffer_width, engine->framebuffer_height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, engine->depth_renderbuffer);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "Error: MSAA Framebuffer is not complete!\n");
        // Cleanup in case of framebuffer setup failure
        glDeleteFramebuffers(1, &engine->framebuffer);
        glDeleteTextures(1, &engine->multisample_texture);
        glDeleteRenderbuffers(1, &engine->depth_renderbuffer);
        engine->framebuffer = 0;
        engine->multisample_texture = 0;
        engine->depth_renderbuffer = 0;
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0); // Bind back to the default framebuffer
}

/*
 * Camera
 */

void set_engine_camera(Engine* engine, Camera* camera){
    if (!engine || !camera) return;

    engine->camera = camera;
}

void update_engine_camera_lookat(Engine* engine){
    if (!engine) return;

    Camera* camera = engine->camera;
    if (!camera) return;

    glm_lookat(camera->position, camera->look_at, camera->up_vector, engine->view_matrix);
}

void update_engine_camera_perspective(Engine* engine){
    if (!engine) return;

    Camera* camera = engine->camera;
    if (!camera) return;

    camera->aspect_ratio = (float)engine->framebuffer_width / (float)engine->framebuffer_height;
    glm_perspective(camera->fov_radians, camera->aspect_ratio, camera->near_clip, 
            camera->far_clip, engine->projection_matrix);
}

/*
 * Scene
 *
 */

Scene* add_scene_from_fbx(Engine* engine, const char* fbx_file_path, const char* texture_directory) {
    if (!engine || !fbx_file_path || !texture_directory) return NULL;

    // Import the scene from the FBX file with the texture directory
    Scene* new_scene = import_fbx(fbx_file_path, texture_directory);
    if (!new_scene) {
        fprintf(stderr, "Failed to import FBX file: %s\n", fbx_file_path);
        return NULL;
    }

    // Reallocate the scenes array to accommodate the new scene
    size_t new_count = engine->scene_count + 1;
    Scene** new_scenes = realloc(engine->scenes, new_count * sizeof(Scene*));
    if (!new_scenes) {
        fprintf(stderr, "Failed to reallocate memory for new scene\n");
        free_scene(new_scene); // Assuming there's a function to free a Scene
        return NULL;
    }

    // Add the new scene to the array and update the scene count
    engine->scenes = new_scenes;
    engine->scenes[engine->scene_count] = new_scene;
    engine->scene_count = new_count;

    return new_scene;
}


void set_active_scene_by_index(Engine* engine, size_t scene_index) {
    if (!engine) return;

    if (scene_index < engine->scene_count) {
        engine->current_scene_index = scene_index;
    } else {
        fprintf(stderr, "Scene index %zu is out of bounds. The engine has %zu scenes.\n", scene_index, engine->scene_count);
    }
}

void set_active_scene_by_name(Engine* engine, const char* scene_name) {
    if (!engine || !scene_name) return;

    for (size_t i = 0; i < engine->scene_count; ++i) {
        if (engine->scenes[i] && strcmp(engine->scenes[i]->root_node->name, scene_name) == 0) {
            engine->current_scene_index = i;
            return;
        }
    }

    fprintf(stderr, "Scene named '%s' not found.\n", scene_name);
}

Scene* get_current_scene(const Engine* engine) {
    // Validate the engine pointer and scenes array
    if (!engine || !engine->scenes) {
        fprintf(stderr, "Engine or scenes array is NULL.\n");
        return NULL;
    }

    // Validate the current scene index
    if (engine->current_scene_index >= engine->scene_count) {
        fprintf(stderr, "Current scene index (%zu) is out of bounds. Total scenes: %zu.\n", engine->current_scene_index, engine->scene_count);
        return NULL;
    }

    // Return the current scene
    return engine->scenes[engine->current_scene_index];
}

void add_program_to_engine(Engine* engine, ShaderProgram* program) {
    if (!engine || !program) {
        fprintf(stderr, "Invalid input to add_program_to_engine\n");
        return;
    }

    // Resize the programs array to accommodate the new program
    size_t new_count = engine->program_count + 1;
    ShaderProgram** new_programs = realloc(engine->programs, new_count * sizeof(ShaderProgram*));

    if (!new_programs) {
        fprintf(stderr, "Failed to allocate memory for new program\n");
        return;
    }

    // Add the new program to the array and update the program count
    engine->programs = new_programs;
    engine->programs[engine->program_count] = program;
    engine->program_count = new_count;
}


/*
 * GUI
 *
 */
int setup_engine_gui(Engine* engine) {
    if (!engine || !engine->window) return -1;

    engine->nk_ctx = nk_glfw3_init(&engine->nk_glfw, engine->window, NK_GLFW3_INSTALL_CALLBACKS);

    if (!engine->nk_ctx) {
        printf("Failed to initialize Nuklear context\n");
        return -1; // or handle the error appropriately
    }

    struct nk_font_atlas *atlas;
    nk_glfw3_font_stash_begin(&engine->nk_glfw, &atlas);
    nk_glfw3_font_stash_end(&engine->nk_glfw);

    nk_style_load_all_cursors(engine->nk_ctx, atlas->cursors);

    // Initialize default background color
    engine->bg = nk_rgb(28, 48, 62);

    // save engine to window so we can use it in callbacks
    glfwSetWindowUserPointer(engine->window, engine);

    return 0; // Success
}

void render_nuklear_gui(Engine* engine) {
    if (!engine || !engine->nk_ctx) return;

    Camera *camera = engine->camera;

    if(!camera) return;

    // Save OpenGL state
    GLint previousViewport[4];
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend = glIsEnabled(GL_BLEND);

    // Start Nuklear frame
    nk_glfw3_new_frame(&engine->nk_glfw);

    // Begin Nuklear GUI window
    if (nk_begin(engine->nk_ctx, "Camera", nk_rect(15, 15, 250, 500),
        NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
        NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE)) {

        // Button for toggling axes
        nk_layout_row_dynamic(engine->nk_ctx, 30, 2);
        if (nk_button_label(engine->nk_ctx, "Show Axes")) {
            set_engine_show_axes(engine, !engine->show_axes);
        }

        // Button for toggling wireframe
        if (nk_button_label(engine->nk_ctx, "Show Wireframe")) {
            set_engine_show_wireframe(engine, !engine->show_wireframe);
        }

        // top margin
        nk_layout_row_dynamic(engine->nk_ctx, 10, 1); // 10 pixels of vertical space
        nk_spacing(engine->nk_ctx, 1); // Creates a dummy widget for spacing

        nk_layout_row_dynamic(engine->nk_ctx, 30, 1);
        const char *render_modes[] = {
            "PBR",
            "Normals",
            "World Pos",
            "Tex Coords",
            "Tangent Space",
            "Flat Color",
        };
        int selected_render_mode = engine->current_render_mode;
        if (nk_combo_begin_label(engine->nk_ctx, render_modes[selected_render_mode], nk_vec2(nk_widget_width(engine->nk_ctx), 200))) {
            nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
            for (int i = 0; i < sizeof(render_modes) / sizeof(render_modes[0]); i++) {
                if (nk_combo_item_label(engine->nk_ctx, render_modes[i], NK_TEXT_ALIGN_LEFT)) {
                    selected_render_mode = i;
                }
            }
            nk_combo_end(engine->nk_ctx);
        }
        engine->current_render_mode = selected_render_mode;

        // bot margin
        nk_layout_row_dynamic(engine->nk_ctx, 10, 1); // 10 pixels of vertical space
        nk_spacing(engine->nk_ctx, 1); // Creates a dummy widget for spacing

        // Camera properties
        nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
        nk_property_float(engine->nk_ctx, "Theta:", 0.0f, &camera->theta, M_PI_2, 0.1f, 1);
        nk_property_float(engine->nk_ctx, "Phi:", 0.0f, &camera->phi, M_PI_2, 0.1f, 1);
        nk_property_float(engine->nk_ctx, "Distance:", 0.0f, &camera->distance, 3000.0f, 100.0f, 1);
        nk_property_float(engine->nk_ctx, "Height:", -2000.0f, &camera->height, 2000.0f, 100.0f, 1);
        nk_property_float(engine->nk_ctx, "Fov:", 0.0f, &camera->fov_radians, M_PI, 0.01f, 0.01f);

        // LookAt Point properties
        nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
        nk_property_float(engine->nk_ctx, "LookAt X:", -25.0f, &camera->look_at[0], 25.0f, 1.0f, 1);
        nk_property_float(engine->nk_ctx, "LookAt Y:", -25.0f, &camera->look_at[1], 25.0f, 1.0f, 1);
        nk_property_float(engine->nk_ctx, "LookAt Z:", -25.0f, &camera->look_at[2], 25.0f, 1.0f, 1);

        // Up Vector properties
        nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
        nk_property_float(engine->nk_ctx, "up X:", -25.0f, &camera->up_vector[0], 25.0f, 1.0f, 1);
        nk_property_float(engine->nk_ctx, "up Y:", -25.0f, &camera->up_vector[1], 25.0f, 1.0f, 1);
        nk_property_float(engine->nk_ctx, "up Z:", -25.0f, &camera->up_vector[2], 25.0f, 1.0f, 1);

        // Additional camera control properties
        nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
        nk_property_float(engine->nk_ctx, "Zoom:", 0.0f, &camera->zoom_speed, 2.0f, 0.01f, 1);
        nk_property_float(engine->nk_ctx, "Orbit:", 0.0f, &camera->orbit_speed, 0.1f, 0.001f, 1);
        nk_property_float(engine->nk_ctx, "Amplitude:", 0.0f, &camera->amplitude, 50.0f, 1.0f, 1);
        nk_property_float(engine->nk_ctx, "Near Clip:", 5.0f, &camera->near_clip, 100.0f, 1.0f, 1.0f);
        nk_property_float(engine->nk_ctx, "Far Clip:", 0.1f, &camera->far_clip, 10000.0f, 100.0f, 10.0f);
    }
    nk_end(engine->nk_ctx);

    // Render Nuklear GUI
    nk_glfw3_render(&engine->nk_glfw, NK_ANTI_ALIASING_ON, MAX_VERTEX_BUFFER, MAX_ELEMENT_BUFFER);

    // Restore OpenGL state
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    if (depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
}

/*
 * Render
 */
void set_engine_show_wireframe(Engine* engine, bool show_wireframe){
    if (!engine) return;

    engine->show_wireframe = show_wireframe;

    if(show_wireframe){
        glDisable(GL_CULL_FACE);
    }else{
        glEnable(GL_CULL_FACE);
    }
}

void set_engine_show_axes(Engine* engine, bool show_axes){
    if (!engine) return;

    engine->show_axes = show_axes;

    for (size_t i = 0; i < engine->scene_count; ++i) {
        if (engine->scenes[i]) {
            Scene* scene = engine->scenes[i];
            if(!scene) continue;
            SceneNode* root_node = scene->root_node;
            if(!root_node) continue;
            set_show_axes_for_nodes(root_node, show_axes);
        }
    }
}

void run_engine_render_loop(Engine* engine, RenderSceneFunc render_func) {
    if (!engine) return;

    glEnable(GL_DEPTH_TEST);

    glCullFace(GL_BACK); // Cull back faces
    glFrontFace(GL_CCW); // Front faces are defined in counter-clockwise order

    while (!glfwWindowShouldClose(engine->window)) {
        if(engine->show_wireframe){
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        }else{
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        Scene* current_scene = get_current_scene(engine);

        if(render_func != NULL && current_scene != NULL) {
            render_func(engine, current_scene);
        }

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        render_nuklear_gui(engine);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, engine->framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); // Default framebuffer
        glBlitFramebuffer(0, 0, engine->framebuffer_width, engine->framebuffer_height,
                          0, 0, engine->framebuffer_width, engine->framebuffer_height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);

        glfwSwapBuffers(engine->window);
        glfwPollEvents();
    }
}


