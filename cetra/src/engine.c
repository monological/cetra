#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include <cglm/ray.h>
#include <cglm/box.h>

#include "scene.h"
#include "camera.h"
#include "shader.h"
#include "program.h"
#include "util.h"
#include "engine.h"
#include "transform.h"
#include "intersect.h"

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
#include "ext/log.h"

/*
 * Private functions
 */
static int _create_default_shaders_for_engine(Engine* engine);
static int _setup_engine_glfw(Engine* engine);
static int _setup_engine_msaa(Engine* engine);
static int _setup_engine_gui(Engine* engine);
static void _engine_cursor_position_callback(GLFWwindow* window, double xpos, double ypos);
static void _engine_mouse_button_callback(GLFWwindow* window, int button, int action, int mods);
static void _engine_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
static SceneNode* _perform_engine_ray_picking(Engine* engine, double mouse_fb_x, double mouse_fb_y);

/*
 * Engine
 */
Engine* create_engine(const char* window_title, int width, int height) {
    Engine* engine = malloc(sizeof(Engine));
    if (!engine) {
        log_error("Failed to allocate memory for engine");
        return NULL;
    }

    engine->window = NULL;

    if (window_title != NULL) {
        engine->window_title = safe_strdup(window_title);
    } else {
        engine->window_title = NULL;
    }

    if (window_title && !engine->window_title) {
        log_error("Failed to allocate memory for window title");
        free(engine);
        return NULL;
    }

    engine->win_width = width;
    engine->win_height = height;
    engine->fb_width = 0;
    engine->fb_height = 0;

    engine->error_callback = NULL;
    engine->mouse_button_callback = NULL;
    engine->cursor_position_callback = NULL;

    engine->framebuffer = 0;
    engine->multisample_texture = 0;
    engine->depth_renderbuffer = 0;

    engine->camera = NULL;
    engine->camera_mode = CAMERA_MODE_ORBIT;

    engine->scenes = NULL;
    engine->scene_count = 0;
    engine->current_scene_index = 0;

    engine->programs = NULL;
    engine->program_count = 0;
    engine->program_map = NULL;

    engine->current_render_mode = RENDER_MODE_PBR;

    glm_mat4_identity(engine->model_matrix);
    glm_mat4_identity(engine->view_matrix);
    glm_mat4_identity(engine->projection_matrix);

    engine->show_gui = false;
    engine->show_wireframe = false;
    engine->show_xyz = false;
    engine->show_fps = false;

    init_input_state(&engine->input);

    // FPS tracking initialization
    engine->last_frame_time = 0.0;
    engine->delta_time = 0.0;
    engine->fps = 0.0f;
    engine->fps_update_timer = 0.0f;
    engine->frame_count = 0;

    return engine;
}

void free_engine(Engine* engine) {
    if (!engine)
        return;

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

/*
 * Setup GLFW
 */
static int _setup_engine_glfw(Engine* engine) {
    if (!engine) {
        return -1;
    }

    if (engine->error_callback) {
        glfwSetErrorCallback(engine->error_callback);
    }

    if (!glfwInit()) {
        log_error("Failed to initialize GLFW");
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

    engine->window =
        glfwCreateWindow(engine->win_width, engine->win_height, engine->window_title, NULL, NULL);
    if (engine->window == NULL) {
        log_error("Failed to create GLFW window");
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(engine->window);

    // Enable V-Sync
    glfwSwapInterval(1);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glfwSwapInterval(1);

    if (glewInit() != GLEW_OK) {
        log_error("Failed to initialize GLEW");
        glfwTerminate();
        return -1;
    }

    glfwGetFramebufferSize(engine->window, &(engine->fb_width), &(engine->fb_height));
    glViewport(1, 0, engine->fb_width, engine->fb_height);

    return 0;
}

/*
 * MSAA anti-aliasing
 *
 */
static int _setup_engine_msaa(Engine* engine) {
    if (!engine || !engine->window)
        return -1;

    // Set up MSAA anti-aliasing
    int samples = 4; // Number of samples for MSAA, adjust as needed

    // Create and set up the multisample framebuffer
    glGenFramebuffers(1, &engine->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);

    glGenTextures(1, &engine->multisample_texture);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, engine->multisample_texture);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGB, engine->fb_width,
                            engine->fb_height, GL_TRUE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE,
                           engine->multisample_texture, 0);

    glGenRenderbuffers(1, &engine->depth_renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, engine->depth_renderbuffer);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8,
                                     engine->fb_width, engine->fb_height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                              engine->depth_renderbuffer);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Error: MSAA Framebuffer is not complete!");
        // Cleanup in case of framebuffer setup failure
        glDeleteFramebuffers(1, &engine->framebuffer);
        glDeleteTextures(1, &engine->multisample_texture);
        glDeleteRenderbuffers(1, &engine->depth_renderbuffer);
        engine->framebuffer = 0;
        engine->multisample_texture = 0;
        engine->depth_renderbuffer = 0;
        return -1;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0); // Bind back to the default framebuffer

    return 0;
}

/*
 * Nuklear GUI
 *
 */
static int _setup_engine_gui(Engine* engine) {
    if (!engine || !engine->window)
        return -1;

    engine->nk_ctx = nk_glfw3_init(&engine->nk_glfw, engine->window, NK_GLFW3_INSTALL_CALLBACKS);

    if (!engine->nk_ctx) {
        log_error("Failed to initialize Nuklear context");
        return -1; // or handle the error appropriately
    }

    struct nk_font_atlas* atlas;
    nk_glfw3_font_stash_begin(&engine->nk_glfw, &atlas);
    nk_glfw3_font_stash_end(&engine->nk_glfw);

    nk_style_load_all_cursors(engine->nk_ctx, atlas->cursors);

    // Initialize default background color
    engine->bg = nk_rgb(28, 48, 62);

    // save engine to window so we can use it in callbacks
    glfwSetWindowUserPointer(engine->window, engine);

    glfwSetMouseButtonCallback(engine->window, _engine_mouse_button_callback);
    glfwSetCursorPosCallback(engine->window, _engine_cursor_position_callback);
    glfwSetKeyCallback(engine->window, _engine_key_callback);

    return 0;
}

/*
 * Initialize the Engine
 *
 */
int init_engine(Engine* engine) {
    printf("┏┓┏┓┏┳┓┳┓┏┓\n");
    printf("┃ ┣  ┃ ┣┫┣┫\n");
    printf("┗┛┗┛ ┻ ┛┗┛┗\n");

    printf("\nInitializing Cetra Graphics Engine...\n");

    if (_setup_engine_glfw(engine) != 0) {
        log_error("Failed to initialize engine GLFW");
        return -1;
    }
    if (_setup_engine_msaa(engine) != 0) {
        log_error("Failed to initialize engine MSAA");
        return -1;
    }
    if (_setup_engine_gui(engine) != 0) {
        log_error("Failed to initialize engine GUI");
        return -1;
    }

    if (_create_default_shaders_for_engine(engine) != 0) {
        log_error("Failed to create default shaders for engine");
        return -1;
    }

    return 0;
}

/*
 * Callbacks
 */
void set_engine_error_callback(Engine* engine, GLFWerrorfun error_callback) {
    if (!engine)
        return;
    engine->error_callback = error_callback;
    if (engine->window) {
        glfwSetErrorCallback(error_callback);
    }
}

void set_engine_cursor_position_callback(Engine* engine,
                                         CursorPositionCallback cursor_position_callback) {
    if (!engine)
        return;
    engine->cursor_position_callback = cursor_position_callback;
}

void set_engine_mouse_button_callback(Engine* engine, MouseButtonCallback mouse_button_callback) {
    if (!engine)
        return;
    engine->mouse_button_callback = mouse_button_callback;
}

void set_engine_key_callback(Engine* engine, KeyCallback key_callback) {
    if (!engine)
        return;
    engine->key_callback = key_callback;
}

/*
 * Mouse and Keyboard Callbacks
 */

static void _engine_cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    if (!window)
        return;

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine || !engine->nk_ctx)
        return;

    // Check if mouse is over any Nuklear window
    if (nk_window_is_any_hovered(engine->nk_ctx)) {
        return;
    }

    glfwGetWindowSize(window, &engine->win_width, &engine->win_height);
    glfwGetFramebufferSize(window, &engine->fb_width, &engine->fb_height);

    xpos = ((xpos / engine->win_width) * engine->fb_width);
    ypos = (1.0 - (ypos / engine->win_height)) * engine->fb_height;

    if (engine->input.is_dragging) {
        // Calculate total offset from drag start position
        engine->input.drag_fb_x = xpos - engine->input.center_fb_x;
        engine->input.drag_fb_y = ypos - engine->input.center_fb_y;

        // Update previous position for next frame (for per-frame delta if needed)
        engine->input.prev_fb_x = xpos;
        engine->input.prev_fb_y = ypos;
    }

    if (engine->cursor_position_callback) {
        engine->cursor_position_callback(engine, xpos, ypos);
    }
}

static void _engine_mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    if (!window)
        return;

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine || !engine->nk_ctx)
        return;

    // Check if mouse is over any Nuklear window
    if (nk_window_is_any_hovered(engine->nk_ctx)) {
        return;
    }

    glfwGetWindowSize(window, &engine->win_width, &engine->win_height);
    glfwGetFramebufferSize(window, &engine->fb_width, &engine->fb_height);

    double mouse_fb_x, mouse_fb_y;
    glfwGetCursorPos(window, &mouse_fb_x, &mouse_fb_y);

    mouse_fb_x = ((mouse_fb_x / engine->win_width) * engine->fb_width);
    mouse_fb_y = ((1.0 - (mouse_fb_y / engine->win_height)) * engine->fb_height);

    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        engine->input.is_dragging = true;
        engine->input.center_fb_x = mouse_fb_x;
        engine->input.center_fb_y = mouse_fb_y;
        engine->input.prev_fb_x = mouse_fb_x;
        engine->input.prev_fb_y = mouse_fb_y;

        engine->input.selected_node = _perform_engine_ray_picking(engine, mouse_fb_x, mouse_fb_y);

    } else if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        engine->input.is_dragging = false;
        engine->input.center_fb_x = mouse_fb_x;
        engine->input.center_fb_y = mouse_fb_y;
    }

    if (engine->mouse_button_callback) {
        engine->mouse_button_callback(engine, button, action, mods);
    }
}

static void _engine_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (!window)
        return;

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine || !engine->camera)
        return;

    Camera* camera = engine->camera;

    float cameraSpeed = 300.0f;
    float orbit_step = 0.1f;
    float max_theta = (float)M_PI_2 - 0.1f;
    vec3 new_position = {0.0f, 0.0f, 0.0f};
    vec3 movement = {0.0f, 0.0f, 0.0f};

    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        vec3 forward, right;
        switch (key) {
            case GLFW_KEY_W:
                glm_vec3_sub(camera->look_at, camera->position, forward);
                glm_vec3_normalize(forward);
                glm_vec3_scale(forward, cameraSpeed, movement);
                glm_vec3_add(camera->position, movement, new_position);
                set_camera_position(camera, new_position);
                glm_vec3_add(camera->look_at, movement, camera->look_at);
                break;

            case GLFW_KEY_S:
                glm_vec3_sub(camera->look_at, camera->position, forward);
                glm_vec3_normalize(forward);
                glm_vec3_scale(forward, cameraSpeed, movement);
                glm_vec3_sub(camera->position, movement, new_position);
                set_camera_position(camera, new_position);
                glm_vec3_sub(camera->look_at, movement, camera->look_at);
                break;

            case GLFW_KEY_A:
                glm_vec3_sub(camera->look_at, camera->position, forward);
                glm_vec3_crossn(camera->up_vector, forward, right);
                glm_vec3_normalize(right);
                glm_vec3_scale(right, cameraSpeed, movement);
                glm_vec3_add(camera->position, movement, new_position);
                set_camera_position(camera, new_position);
                glm_vec3_add(camera->look_at, movement, camera->look_at);
                break;

            case GLFW_KEY_D:
                glm_vec3_sub(camera->look_at, camera->position, forward);
                glm_vec3_crossn(camera->up_vector, forward, right);
                glm_vec3_normalize(right);
                glm_vec3_scale(right, cameraSpeed, movement);
                glm_vec3_sub(camera->position, movement, new_position);
                set_camera_position(camera, new_position);
                glm_vec3_sub(camera->look_at, movement, camera->look_at);
                break;

            case GLFW_KEY_UP:
                if (engine->camera_mode == CAMERA_MODE_ORBIT) {
                    camera->theta += orbit_step;
                    if (camera->theta > max_theta)
                        camera->theta = max_theta;
                    float cos_theta = cosf(camera->theta);
                    vec3 offset = {camera->distance * cos_theta * cosf(camera->phi),
                                   camera->distance * sinf(camera->theta),
                                   camera->distance * cos_theta * sinf(camera->phi)};
                    glm_vec3_add(camera->look_at, offset, new_position);
                    set_camera_position(camera, new_position);
                } else {
                    glm_vec3_sub(camera->look_at, camera->position, forward);
                    glm_vec3_normalize(forward);
                    glm_vec3_scale(forward, cameraSpeed, movement);
                    glm_vec3_add(camera->position, movement, new_position);
                    set_camera_position(camera, new_position);
                    glm_vec3_add(camera->look_at, movement, camera->look_at);
                }
                break;

            case GLFW_KEY_DOWN:
                if (engine->camera_mode == CAMERA_MODE_ORBIT) {
                    camera->theta -= orbit_step;
                    if (camera->theta < -max_theta)
                        camera->theta = -max_theta;
                    float cos_theta = cosf(camera->theta);
                    vec3 offset = {camera->distance * cos_theta * cosf(camera->phi),
                                   camera->distance * sinf(camera->theta),
                                   camera->distance * cos_theta * sinf(camera->phi)};
                    glm_vec3_add(camera->look_at, offset, new_position);
                    set_camera_position(camera, new_position);
                } else {
                    glm_vec3_sub(camera->look_at, camera->position, forward);
                    glm_vec3_normalize(forward);
                    glm_vec3_scale(forward, cameraSpeed, movement);
                    glm_vec3_sub(camera->position, movement, new_position);
                    set_camera_position(camera, new_position);
                    glm_vec3_sub(camera->look_at, movement, camera->look_at);
                }
                break;

            case GLFW_KEY_LEFT:
                if (engine->camera_mode == CAMERA_MODE_ORBIT) {
                    camera->phi += orbit_step;
                    float cos_theta = cosf(camera->theta);
                    vec3 offset = {camera->distance * cos_theta * cosf(camera->phi),
                                   camera->distance * sinf(camera->theta),
                                   camera->distance * cos_theta * sinf(camera->phi)};
                    glm_vec3_add(camera->look_at, offset, new_position);
                    set_camera_position(camera, new_position);
                } else {
                    glm_vec3_sub(camera->look_at, camera->position, forward);
                    glm_vec3_crossn(camera->up_vector, forward, right);
                    glm_vec3_normalize(right);
                    glm_vec3_scale(right, cameraSpeed, movement);
                    glm_vec3_add(camera->position, movement, new_position);
                    set_camera_position(camera, new_position);
                    glm_vec3_add(camera->look_at, movement, camera->look_at);
                }
                break;

            case GLFW_KEY_RIGHT:
                if (engine->camera_mode == CAMERA_MODE_ORBIT) {
                    camera->phi -= orbit_step;
                    float cos_theta = cosf(camera->theta);
                    vec3 offset = {camera->distance * cos_theta * cosf(camera->phi),
                                   camera->distance * sinf(camera->theta),
                                   camera->distance * cos_theta * sinf(camera->phi)};
                    glm_vec3_add(camera->look_at, offset, new_position);
                    set_camera_position(camera, new_position);
                } else {
                    glm_vec3_sub(camera->look_at, camera->position, forward);
                    glm_vec3_crossn(camera->up_vector, forward, right);
                    glm_vec3_normalize(right);
                    glm_vec3_scale(right, cameraSpeed, movement);
                    glm_vec3_sub(camera->position, movement, new_position);
                    set_camera_position(camera, new_position);
                    glm_vec3_sub(camera->look_at, movement, camera->look_at);
                }
                break;
        }
    }

    if (engine->key_callback) {
        engine->key_callback(engine, key, scancode, action, mods);
    }
}

/*
 * Camera
 */
void set_engine_camera(Engine* engine, Camera* camera) {
    if (!engine || !camera)
        return;

    engine->camera = camera;
}

void set_engine_camera_mode(Engine* engine, CameraMode mode) {
    if (engine) {
        engine->camera_mode = mode;
    }
}

void update_engine_camera_lookat(Engine* engine) {
    if (!engine)
        return;

    Camera* camera = engine->camera;
    if (!camera)
        return;

    glm_lookat(camera->position, camera->look_at, camera->up_vector, engine->view_matrix);
}

void update_engine_camera_perspective(Engine* engine) {
    if (!engine)
        return;

    Camera* camera = engine->camera;
    if (!camera)
        return;

    camera->aspect_ratio = (float)engine->fb_width / (float)engine->fb_height;
    glm_perspective(camera->fov_radians, camera->aspect_ratio, camera->near_clip, camera->far_clip,
                    engine->projection_matrix);
}

/*
 * Scene
 *
 */
int add_scene_to_engine(Engine* engine, Scene* scene) {
    if (!engine || !scene)
        return -1;

    // check if scene already added to engine and return if so
    for (size_t i = 0; i < engine->scene_count; ++i) {
        if (engine->scenes[i] == scene) {
            return 0; // already added, success
        }
    }

    // Reallocate the scenes array to accommodate the new scene
    size_t new_count = engine->scene_count + 1;
    Scene** new_scenes = realloc(engine->scenes, new_count * sizeof(Scene*));
    if (!new_scenes) {
        log_error("Failed to reallocate memory for new scene");
        return -1;
    }

    // Add the new scene to the array and update the scene count
    engine->scenes = new_scenes;
    engine->scenes[engine->scene_count] = scene;
    engine->scene_count = new_count;

    return 0;
}

void set_active_scene_by_index(Engine* engine, size_t scene_index) {
    if (!engine)
        return;

    if (scene_index < engine->scene_count) {
        engine->current_scene_index = scene_index;
    } else {
        log_error("Scene index %zu is out of bounds. The engine has %zu scenes.", scene_index,
                  engine->scene_count);
    }
}

void set_active_scene_by_name(Engine* engine, const char* scene_name) {
    if (!engine || !scene_name)
        return;

    for (size_t i = 0; i < engine->scene_count; ++i) {
        if (engine->scenes[i] && strcmp(engine->scenes[i]->root_node->name, scene_name) == 0) {
            engine->current_scene_index = i;
            return;
        }
    }

    log_error("Scene named '%s' not found.", scene_name);
}

Scene* get_current_scene(const Engine* engine) {
    // Validate the engine pointer and scenes array
    if (!engine || !engine->scenes) {
        log_error("Engine or scenes array is NULL.");
        return NULL;
    }

    // Validate the current scene index
    if (engine->current_scene_index >= engine->scene_count) {
        log_error("Current scene index (%zu) is out of bounds. Total scenes: %zu.",
                  engine->current_scene_index, engine->scene_count);
        return NULL;
    }

    // Return the current scene
    return engine->scenes[engine->current_scene_index];
}

/*
 * Shaders
 *
 */

static int _create_default_shaders_for_engine(Engine* engine) {
    if (!engine)
        return -1;

    ShaderProgram* pbr_shader_program = NULL;

    if ((pbr_shader_program = create_pbr_program()) == NULL) {
        log_error("Failed to create PBR shader program");
        return -1;
    }

    add_shader_program_to_engine(engine, pbr_shader_program);

    ShaderProgram* shape_shader_program = NULL;

    if ((shape_shader_program = create_shape_program()) == NULL) {
        log_error("Failed to create shape shader program");
        return -1;
    }

    add_shader_program_to_engine(engine, shape_shader_program);

    ShaderProgram* xyz_shader_program = NULL;

    if ((xyz_shader_program = create_xyz_program()) == NULL) {
        log_error("Failed to create xyz shader program");
        return -1;
    }

    add_shader_program_to_engine(engine, xyz_shader_program);

    return 0;
}

int add_shader_program_to_engine(Engine* engine, ShaderProgram* program) {
    if (!engine || !program) {
        log_error("Invalid input to add_shader_program_to_engine");
        return -1;
    }

    // check if program already added to engine and return if so
    for (size_t i = 0; i < engine->program_count; ++i) {
        if (engine->programs[i] == program) {
            return 0; // already added, success
        }
    }

    // Resize the programs array to accommodate the new program
    size_t new_count = engine->program_count + 1;
    ShaderProgram** new_programs = realloc(engine->programs, new_count * sizeof(ShaderProgram*));

    if (!new_programs) {
        log_error("Failed to allocate memory for new program");
        return -1;
    }

    // Add the new program to the array and update the program count
    engine->programs = new_programs;
    engine->programs[engine->program_count] = program;
    engine->program_count = new_count;

    ShaderProgram* existing;
    HASH_FIND_STR(engine->program_map, program->name, existing);
    if (!existing) {
        HASH_ADD_KEYPTR(hh, engine->program_map, program->name, strlen(program->name), program);
    }

    return 0;
}

ShaderProgram* get_engine_shader_program_by_name(Engine* engine, const char* program_name) {
    if (!engine || !program_name) {
        log_error("Invalid input to get_program_from_engine");
        return NULL;
    }

    ShaderProgram* existing;
    HASH_FIND_STR(engine->program_map, program_name, existing);
    return existing;
}

/*
 * GUI
 *
 */
void set_engine_show_gui(Engine* engine, bool show_gui) {
    if (!engine)
        return;
    engine->show_gui = show_gui;
}

void set_engine_show_fps(Engine* engine, bool show_fps) {
    if (!engine)
        return;
    engine->show_fps = show_fps;
}

void render_nuklear_gui(Engine* engine) {
    if (!engine || !engine->nk_ctx)
        return;

    // Skip if nothing to show
    if (!engine->show_gui && !engine->show_fps)
        return;

    // Save OpenGL state
    GLint previousViewport[4];
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean blend = glIsEnabled(GL_BLEND);

    // Start Nuklear frame
    nk_glfw3_new_frame(&engine->nk_glfw);

    Camera* camera = engine->camera;

    // Camera controls window (only if show_gui and camera exists)
    if (engine->show_gui && camera) {
        if (nk_begin(engine->nk_ctx, "Camera", nk_rect(15, 15, 250, 500),
                     NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                         NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE)) {

            // Button for toggling xyz
            nk_layout_row_dynamic(engine->nk_ctx, 30, 2);
            if (nk_button_label(engine->nk_ctx, "Show XYZ")) {
                set_engine_show_xyz(engine, !engine->show_xyz);
            }

            // Button for toggling wireframe
            if (nk_button_label(engine->nk_ctx, "Show Wireframe")) {
                set_engine_show_wireframe(engine, !engine->show_wireframe);
            }

            // cam modes
            nk_layout_row_dynamic(engine->nk_ctx, 30, 2);

            // Radio button for CAMERA_MODE_FREE
            if (nk_option_label(engine->nk_ctx, "Free Mode",
                                engine->camera_mode == CAMERA_MODE_FREE)) {
                engine->camera_mode = CAMERA_MODE_FREE;
            }

            // Radio button for CAMERA_MODE_ORBIT
            if (nk_option_label(engine->nk_ctx, "Orbit Mode",
                                engine->camera_mode == CAMERA_MODE_ORBIT)) {
                engine->camera_mode = CAMERA_MODE_ORBIT;
            }

            // top margin
            nk_layout_row_dynamic(engine->nk_ctx, 10, 1); // 10 pixels of vertical space
            nk_spacing(engine->nk_ctx, 1);                // Creates a dummy widget for spacing

            nk_layout_row_dynamic(engine->nk_ctx, 30, 1);
            const char* render_modes[] = {
                "PBR",        "Normals",         "World Pos",
                "Tex Coords", "Tangent Space",   "Flat Color",
                "Albedo",     "Simple Lighting", "Metallic and Roughness",
            };
            int selected_render_mode = engine->current_render_mode;
            if (nk_combo_begin_label(engine->nk_ctx, render_modes[selected_render_mode],
                                     nk_vec2(nk_widget_width(engine->nk_ctx), 200))) {
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
            nk_spacing(engine->nk_ctx, 1);                // Creates a dummy widget for spacing

            // Camera properties
            nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
            nk_property_float(engine->nk_ctx, "Theta:", 0.0f, &camera->theta, GLM_PI_2, 0.1f, 1);
            nk_property_float(engine->nk_ctx, "Phi:", 0.0f, &camera->phi, GLM_PI_2, 0.1f, 1);
            nk_property_float(engine->nk_ctx, "Distance:", 0.0f, &camera->distance, 3000.0f, 100.0f,
                              1);
            nk_property_float(engine->nk_ctx, "Height:", -2000.0f, &camera->height, 2000.0f, 100.0f,
                              1);
            nk_property_float(engine->nk_ctx, "Fov:", 0.0f, &camera->fov_radians, GLM_PI, 0.01f,
                              0.01f);

            // LookAt Point properties
            nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
            nk_property_float(engine->nk_ctx, "LookAt X:", -25.0f, &camera->look_at[0], 25.0f, 1.0f,
                              1);
            nk_property_float(engine->nk_ctx, "LookAt Y:", -25.0f, &camera->look_at[1], 25.0f, 1.0f,
                              1);
            nk_property_float(engine->nk_ctx, "LookAt Z:", -25.0f, &camera->look_at[2], 25.0f, 1.0f,
                              1);

            // Up Vector properties
            nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
            nk_property_float(engine->nk_ctx, "up X:", -25.0f, &camera->up_vector[0], 25.0f, 1.0f,
                              1);
            nk_property_float(engine->nk_ctx, "up Y:", -25.0f, &camera->up_vector[1], 25.0f, 1.0f,
                              1);
            nk_property_float(engine->nk_ctx, "up Z:", -25.0f, &camera->up_vector[2], 25.0f, 1.0f,
                              1);

            // Additional camera control properties
            nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
            nk_property_float(engine->nk_ctx, "Zoom:", 0.0f, &camera->zoom_speed, 2.0f, 0.01f, 1);
            nk_property_float(engine->nk_ctx, "Orbit:", 0.0f, &camera->orbit_speed, 0.1f, 0.001f,
                              1);
            nk_property_float(engine->nk_ctx, "Amplitude:", 0.0f, &camera->amplitude, 50.0f, 1.0f,
                              1);
            nk_property_float(engine->nk_ctx, "Near Clip:", 5.0f, &camera->near_clip, 100.0f, 1.0f,
                              1.0f);
            nk_property_float(engine->nk_ctx, "Far Clip:", 0.1f, &camera->far_clip, 10000.0f,
                              100.0f, 10.0f);
        }
        nk_end(engine->nk_ctx);
    }

    // FPS display - raw text overlay, no window chrome
    if (engine->show_fps) {
        // Make window completely transparent
        struct nk_color transparent = nk_rgba(0, 0, 0, 0);
        nk_style_push_color(engine->nk_ctx, &engine->nk_ctx->style.window.background, transparent);
        nk_style_push_style_item(engine->nk_ctx, &engine->nk_ctx->style.window.fixed_background,
                                 nk_style_item_color(transparent));

        // Position in top-right, using window coords
        struct nk_rect fps_rect = nk_rect(engine->win_width - 100, 10, 90, 25);

        if (nk_begin(engine->nk_ctx, "##fps", fps_rect,
                     NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT)) {
            char fps_text[16];
            snprintf(fps_text, sizeof(fps_text), "%.1f FPS", engine->fps);
            nk_layout_row_dynamic(engine->nk_ctx, 20, 1);
            nk_text_colored(engine->nk_ctx, fps_text, strlen(fps_text), NK_TEXT_RIGHT,
                            nk_rgb(255, 255, 255));
        }
        nk_end(engine->nk_ctx);

        // Restore styles
        nk_style_pop_style_item(engine->nk_ctx);
        nk_style_pop_color(engine->nk_ctx);
    }

    // Render Nuklear GUI
    nk_glfw3_render(&engine->nk_glfw, NK_ANTI_ALIASING_ON, MAX_VERTEX_BUFFER, MAX_ELEMENT_BUFFER);

    // Restore OpenGL state
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    if (depthTest)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    if (blend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
}

/*
 * Render
 */
void set_engine_show_wireframe(Engine* engine, bool show_wireframe) {
    if (!engine)
        return;

    engine->show_wireframe = show_wireframe;

    if (show_wireframe) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
    }
}

void set_engine_show_xyz(Engine* engine, bool show_xyz) {
    if (!engine)
        return;

    engine->show_xyz = show_xyz;

    for (size_t i = 0; i < engine->scene_count; ++i) {
        if (engine->scenes[i]) {
            Scene* scene = engine->scenes[i];
            if (!scene)
                continue;
            SceneNode* root_node = scene->root_node;
            if (!root_node)
                continue;
            set_show_xyz_for_nodes(root_node, show_xyz);
        }
    }
}

void run_engine_render_loop(Engine* engine, RenderSceneFunc render_func) {
    if (!engine)
        return;

    glEnable(GL_DEPTH_TEST);

    glCullFace(GL_BACK); // Cull back faces
    glFrontFace(GL_CCW); // Front faces are defined in counter-clockwise order

    engine->last_frame_time = glfwGetTime();

    while (!glfwWindowShouldClose(engine->window)) {
        // Calculate delta time and FPS
        double current_time = glfwGetTime();
        engine->delta_time = current_time - engine->last_frame_time;
        engine->last_frame_time = current_time;

        engine->frame_count++;
        engine->fps_update_timer += (float)engine->delta_time;

        if (engine->fps_update_timer >= 0.5f) {
            engine->fps = (float)engine->frame_count / engine->fps_update_timer;
            engine->frame_count = 0;
            engine->fps_update_timer = 0.0f;
        }

        if (engine->show_wireframe) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        } else {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        Scene* current_scene = get_current_scene(engine);

        if (render_func != NULL && current_scene != NULL) {
            render_func(engine, current_scene);
        }

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        render_nuklear_gui(engine);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, engine->framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); // Default framebuffer
        glBlitFramebuffer(0, 0, engine->fb_width, engine->fb_height, 0, 0, engine->fb_width,
                          engine->fb_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        glfwSwapBuffers(engine->window);
        glfwPollEvents();
    }
}

void get_mouse_world_position_on_drag_plane(Engine* engine, double mouse_fb_x, double mouse_fb_y,
                                            vec3 out_world_pos) {
    if (!engine || !engine->camera)
        return;

    // Build projection and view matrices
    mat4 projection, view;
    glm_perspective(engine->camera->fov_radians, engine->camera->aspect_ratio,
                    engine->camera->near_clip, engine->camera->far_clip, projection);
    glm_lookat(engine->camera->position, engine->camera->look_at, engine->camera->up_vector, view);

    // Compute ray from screen coordinates
    vec3 ray_dir;
    compute_ray_from_screen((float)mouse_fb_x, (float)mouse_fb_y, engine->fb_width,
                            engine->fb_height, projection, view, engine->camera->position, ray_dir);

    // Project ray to the stored drag plane distance
    ray_point_at_distance(engine->camera->position, ray_dir, engine->input.drag_plane_distance,
                          out_world_pos);
}

static SceneNode* _perform_engine_ray_picking(Engine* engine, double mouse_fb_x,
                                              double mouse_fb_y) {
    // Build projection and view matrices
    mat4 projection, view;
    glm_perspective(engine->camera->fov_radians, engine->camera->aspect_ratio,
                    engine->camera->near_clip, engine->camera->far_clip, projection);
    glm_lookat(engine->camera->position, engine->camera->look_at, engine->camera->up_vector, view);

    // Compute ray from screen coordinates
    vec3 ray_origin, ray_dir;
    glm_vec3_copy(engine->camera->position, ray_origin);
    compute_ray_from_screen((float)mouse_fb_x, (float)mouse_fb_y, engine->fb_width,
                            engine->fb_height, projection, view, ray_origin, ray_dir);

    // Perform scene graph picking
    Scene* current_scene = get_current_scene(engine);
    if (!current_scene || !current_scene->root_node)
        return NULL;

    RayPickResult result = pick_scene_node(current_scene->root_node, ray_origin, ray_dir);

    if (result.hit) {
        // Store hit information for drag operations
        glm_vec3_copy(result.hit_point, engine->input.drag_start_world_pos);
        engine->input.drag_plane_distance = result.distance;

        // Store object's current position (extract from transform)
        glm_vec3_copy((vec3){result.node->global_transform[3][0],
                             result.node->global_transform[3][1],
                             result.node->global_transform[3][2]},
                      engine->input.drag_object_start_pos);
    }

    return result.node;
}
