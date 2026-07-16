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
#include "shadow.h"
#include "render.h"
#include "springbone.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_KEYSTATE_BASED_INPUT
#define NK_NO_STB_RECT_PACK_IMPLEMENTATION
#define NK_NO_STB_TRUETYPE_IMPLEMENTATION
#define NK_IMPLEMENTATION
#define NK_GLFW_GL3_IMPLEMENTATION

#include "ext/nuklear.h"
#include "ext/nuklear_glfw_gl3.h"
#include "ext/log.h"

// Dear ImGui via cimgui (migrating off Nuklear). CIMGUI_DEFINE_ENUMS_AND_STRUCTS
// gives the full struct/enum definitions to C; CIMGUI_USE_GLFW/OPENGL3 (compile
// defs from CMake) expose the backend bindings in cimgui_impl.h.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include "cimgui_impl.h"

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
static void _engine_char_callback(GLFWwindow* window, unsigned int codepoint);
static void _engine_scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
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
    engine->ss_scale = 1; // Supersampling off by default (4x fragment cost);
                          // opt in with --ssaa 2 for beauty shots

    engine->error_callback = NULL;
    engine->mouse_button_callback = NULL;
    engine->cursor_position_callback = NULL;
    engine->key_callback = NULL;
    engine->scroll_callback = NULL;

    engine->framebuffer = 0;
    engine->multisample_texture = 0;
    engine->normal_multisample_texture = 0;
    engine->depth_renderbuffer = 0;
    engine->normals_this_frame = false;

    engine->camera = NULL;
    engine->camera_mode = CAMERA_MODE_ORBIT;

    engine->scenes = NULL;
    engine->scene_count = 0;
    engine->current_scene_index = 0;

    engine->programs = NULL;
    engine->program_count = 0;
    engine->program_map = NULL;

    engine->current_render_mode = RENDER_MODE_PBR;
    engine->specular_aa_strength = 1.0f;

    glm_mat4_identity(engine->model_matrix);
    glm_mat4_identity(engine->view_matrix);
    glm_mat4_identity(engine->projection_matrix);

    engine->show_gui = false;
    engine->show_wireframe = false;
    engine->show_xyz = false;
    engine->show_fps = false;
    engine->show_bones = false;
    engine->headless = false;
    engine->screenshot_path = NULL;
    engine->screenshot_every = 0;
    engine->total_frames = 0;

    engine->bone_program = NULL;
    engine->bone_line_vao = 0;
    engine->bone_line_vbo = 0;

    engine->shadow_catcher_program = NULL;
    engine->catcher_vao = 0;
    engine->catcher_vbo = 0;

    engine->postfx = NULL;

    init_input_state(&engine->input);

    // FPS tracking initialization
    engine->last_frame_time = 0.0;
    engine->delta_time = 0.0;
    engine->fps = 0.0f;
    engine->fps_update_timer = 0.0f;
    engine->frame_count = 0;

    engine->async_loader = NULL;

    return engine;
}

void free_engine(Engine* engine) {
    if (!engine)
        return;

    // Free text renderer
    if (engine->text_renderer) {
        free_text_renderer(engine->text_renderer);
        engine->text_renderer = NULL;
    }

    // Free async loader before scenes (may have pending work)
    if (engine->async_loader) {
        free_async_loader(engine->async_loader);
        engine->async_loader = NULL;
    }

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

    if (engine->screenshot_path) {
        free(engine->screenshot_path);
    }

    // GL objects must be released while the context still exists,
    // i.e. before the window is destroyed
    if (engine->postfx) {
        free_postfx(engine->postfx);
    }

    glDeleteFramebuffers(1, &engine->framebuffer);
    glDeleteTextures(1, &engine->multisample_texture);
    glDeleteTextures(1, &engine->normal_multisample_texture);
    glDeleteRenderbuffers(1, &engine->depth_renderbuffer);

    if (engine->catcher_vao)
        glDeleteVertexArrays(1, &engine->catcher_vao);
    if (engine->catcher_vbo)
        glDeleteBuffers(1, &engine->catcher_vbo);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    igDestroyContext(NULL);

    nk_glfw3_shutdown(&engine->nk_glfw);

    // Destroy GLFW window
    if (engine->window) {
        glfwDestroyWindow(engine->window);
    }

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

    if (engine->headless) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    engine->window =
        glfwCreateWindow(engine->win_width, engine->win_height, engine->window_title, NULL, NULL);
    if (engine->window == NULL) {
        log_error("Failed to create GLFW window");
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(engine->window);

    // V-Sync on for normal runs, off for headless so frames run at full speed
    glfwSwapInterval(engine->headless ? 0 : 1);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (glewInit() != GLEW_OK) {
        log_error("Failed to initialize GLEW");
        glfwTerminate();
        return -1;
    }

    glfwGetFramebufferSize(engine->window, &(engine->fb_width), &(engine->fb_height));
    glViewport(0, 0, engine->fb_width, engine->fb_height);

    return 0;
}

/*
 * MSAA anti-aliasing
 *
 */
// The scene renders into a target supersampled by ss_scale; the post chain
// box-downsamples to display size at tone map. This render (not display)
// resolution is shared by the MSAA allocation and the scene-pass viewport.
static void engine_render_size(const Engine* engine, int* w, int* h) {
    *w = engine->fb_width * engine->ss_scale;
    *h = engine->fb_height * engine->ss_scale;
}

static int _setup_engine_msaa(Engine* engine) {
    if (!engine || !engine->window)
        return -1;

    int rw, rh;
    engine_render_size(engine, &rw, &rh);

    // Set up MSAA anti-aliasing
    int samples = 4; // Number of samples for MSAA, adjust as needed
    GLint max_color_samples = 0;
    GLint max_samples = 0;
    glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &max_color_samples);
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    if (max_samples > 0 && max_samples < max_color_samples)
        max_color_samples = max_samples;
    if (max_color_samples > 0 && samples > max_color_samples)
        samples = max_color_samples;

    // Create and set up the multisample framebuffer. The color target is
    // float (RGBA16F) so the scene accumulates in linear HDR; the post
    // stack tone maps it down to the display
    glGenFramebuffers(1, &engine->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);

    glGenTextures(1, &engine->multisample_texture);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, engine->multisample_texture);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGBA16F, rw, rh, GL_TRUE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE,
                           engine->multisample_texture, 0);

    // Attachment 1: view-space normals (xyz) + roughness (a) for SSAO/SSR.
    // Always allocated, only written when a consumer is active (see
    // engine_set_scene_draw_buffers). RGBA16F so the MSAA resolve blit's
    // format-match rule is satisfied against the float resolve target.
    glGenTextures(1, &engine->normal_multisample_texture);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, engine->normal_multisample_texture);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGBA16F, rw, rh, GL_TRUE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D_MULTISAMPLE,
                           engine->normal_multisample_texture, 0);

    glGenRenderbuffers(1, &engine->depth_renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, engine->depth_renderbuffer);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, rw, rh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                              engine->depth_renderbuffer);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        log_error("Error: MSAA Framebuffer is not complete!");
        // Cleanup in case of framebuffer setup failure
        glDeleteFramebuffers(1, &engine->framebuffer);
        glDeleteTextures(1, &engine->multisample_texture);
        glDeleteTextures(1, &engine->normal_multisample_texture);
        glDeleteRenderbuffers(1, &engine->depth_renderbuffer);
        engine->framebuffer = 0;
        engine->multisample_texture = 0;
        engine->normal_multisample_texture = 0;
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

    engine->nk_ctx = nk_glfw3_init(&engine->nk_glfw, engine->window, NK_GLFW3_DEFAULT);

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

    // Dear ImGui, sharing the same GLFW window + GL context. install_callbacks
    // = false: the engine owns the GLFW callbacks and forwards events to the
    // ImGui backend itself (see the input callbacks). "#version 150" is the
    // core-profile GLSL the OpenGL3 backend needs on macOS.
    igCreateContext(NULL);
    ImGui_ImplGlfw_InitForOpenGL(engine->window, false);
    ImGui_ImplOpenGL3_Init("#version 150");
    igStyleColorsDark(NULL);

    // save engine to window so we can use it in callbacks
    glfwSetWindowUserPointer(engine->window, engine);

    glfwSetMouseButtonCallback(engine->window, _engine_mouse_button_callback);
    glfwSetCursorPosCallback(engine->window, _engine_cursor_position_callback);
    glfwSetKeyCallback(engine->window, _engine_key_callback);
    glfwSetCharCallback(engine->window, _engine_char_callback);
    glfwSetScrollCallback(engine->window, _engine_scroll_callback);

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

    engine->async_loader = create_async_loader();
    if (!engine->async_loader) {
        log_error("Failed to create async loader");
        return -1;
    }

    // Initialize text renderer
    engine->text_renderer = create_text_renderer();
    if (engine->text_renderer) {
        init_text_renderer(engine->text_renderer, engine->win_width, engine->win_height);
        // Create and cache text shader program
        ShaderProgram* text_prog = create_text_program();
        if (text_prog) {
            add_shader_program_to_engine(engine, text_prog);
            engine->text_renderer->text_program = text_prog;
        }
    }

    // Initialize bone visualization
    glGenVertexArrays(1, &engine->bone_line_vao);
    glGenBuffers(1, &engine->bone_line_vbo);
    glBindVertexArray(engine->bone_line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, engine->bone_line_vbo);
    // Position attribute (location 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // Color attribute (location 1)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    engine->bone_program = create_bone_program();
    if (engine->bone_program) {
        add_shader_program_to_engine(engine, engine->bone_program);
    }

    // Shadow catcher: unit quad at y=0 (scaled by planeRadius in the shader)
    engine->shadow_catcher_program = create_shadow_catcher_program();
    if (engine->shadow_catcher_program) {
        add_shader_program_to_engine(engine, engine->shadow_catcher_program);

        // CCW as seen from above (+y) so the upward face is the front face
        const float catcher_quad[] = {
            -1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f,
            -1.0f, 0.0f, -1.0f, 1.0f,  0.0f, 1.0f, 1.0f, 0.0f, -1.0f,
        };
        glGenVertexArrays(1, &engine->catcher_vao);
        glGenBuffers(1, &engine->catcher_vbo);
        glBindVertexArray(engine->catcher_vao);
        glBindBuffer(GL_ARRAY_BUFFER, engine->catcher_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(catcher_quad), catcher_quad, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
    }

    // HDR post-processing (resolve + bloom + tone map). Sized at the display
    // resolution; the supersample factor enlarges the internal chain to match
    // the enlarged MSAA scene target.
    engine->postfx = create_postfx(engine->fb_width, engine->fb_height, engine->ss_scale);
    if (!engine->postfx) {
        log_error("Failed to initialize engine post-processing");
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

void set_engine_scroll_callback(Engine* engine, ScrollCallback scroll_callback) {
    if (!engine)
        return;
    engine->scroll_callback = scroll_callback;
}

/*
 * Mouse and Keyboard Callbacks
 */

// True when the GUI wants the pointer/keyboard this frame, so 3D input is
// suppressed. ImGui's io flags are authoritative; the Nuklear hover check is
// kept only while both GUIs coexist (removed in the Nuklear-removal stage).
bool engine_gui_wants_mouse(const Engine* engine) {
    if (igGetIO_Nil()->WantCaptureMouse)
        return true;
    return engine->nk_ctx && nk_window_is_any_hovered(engine->nk_ctx);
}

static bool engine_gui_wants_keyboard(void) {
    return igGetIO_Nil()->WantCaptureKeyboard;
}

static void _engine_cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    if (!window)
        return;

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine || !engine->nk_ctx)
        return;

    ImGui_ImplGlfw_CursorPosCallback(window, xpos, ypos);

    if (engine_gui_wants_mouse(engine)) {
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

    ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);

    glfwGetWindowSize(window, &engine->win_width, &engine->win_height);
    glfwGetFramebufferSize(window, &engine->fb_width, &engine->fb_height);

    double mouse_fb_x, mouse_fb_y;
    glfwGetCursorPos(window, &mouse_fb_x, &mouse_fb_y);

    mouse_fb_x = ((mouse_fb_x / engine->win_width) * engine->fb_width);
    mouse_fb_y = ((1.0 - (mouse_fb_y / engine->win_height)) * engine->fb_height);

    // A release ALWAYS ends the drag, even over the GUI — otherwise a button-up
    // that lands on a panel leaves the camera stuck orbiting.
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        engine->input.is_dragging = false;
        engine->input.shift_held = false;
        engine->input.center_fb_x = mouse_fb_x;
        engine->input.center_fb_y = mouse_fb_y;
        if (engine->mouse_button_callback) {
            engine->mouse_button_callback(engine, button, action, mods);
        }
        return;
    }

    // Presses (drag-start, picking) only when the GUI doesn't want the pointer.
    if (engine_gui_wants_mouse(engine)) {
        return;
    }

    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        engine->input.is_dragging = true;
        engine->input.shift_held = (mods & GLFW_MOD_SHIFT) != 0;
        engine->input.center_fb_x = mouse_fb_x;
        engine->input.center_fb_y = mouse_fb_y;
        engine->input.prev_fb_x = mouse_fb_x;
        engine->input.prev_fb_y = mouse_fb_y;

        engine->input.selected_node = _perform_engine_ray_picking(engine, mouse_fb_x, mouse_fb_y);
    }

    if (engine->mouse_button_callback) {
        engine->mouse_button_callback(engine, button, action, mods);
    }
}

static void _engine_key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (!window)
        return;

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine)
        return;

    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);

    // Don't drive the camera / app hotkeys while the GUI has keyboard focus
    // (e.g. an active text field).
    if (engine_gui_wants_keyboard())
        return;

    if (engine->key_callback) {
        engine->key_callback(engine, key, scancode, action, mods);
    }
}

// Text input goes only to ImGui.
static void _engine_char_callback(GLFWwindow* window, unsigned int codepoint) {
    ImGui_ImplGlfw_CharCallback(window, codepoint);
}

// Scroll feeds ImGui first; if the GUI isn't using the pointer, it forwards to
// the app (e.g. camera zoom). This is the first scroll wiring in the engine.
static void _engine_scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    ImGui_ImplGlfw_ScrollCallback(window, xoffset, yoffset);

    Engine* engine = glfwGetWindowUserPointer(window);
    if (!engine || engine_gui_wants_mouse(engine))
        return;

    if (engine->scroll_callback) {
        engine->scroll_callback(engine, xoffset, yoffset);
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

    ShaderProgram* shadow_depth_program = NULL;

    if ((shadow_depth_program = create_shadow_depth_program()) == NULL) {
        log_error("Failed to create shadow depth shader program");
        return -1;
    }

    add_shader_program_to_engine(engine, shadow_depth_program);

    // IBL Programs
    ShaderProgram* skybox_program = create_skybox_program();
    if (skybox_program) {
        add_shader_program_to_engine(engine, skybox_program);
    }

    ShaderProgram* ibl_equirect_program = create_ibl_equirect_to_cube_program();
    if (ibl_equirect_program) {
        add_shader_program_to_engine(engine, ibl_equirect_program);
    }

    ShaderProgram* ibl_irradiance_program = create_ibl_irradiance_program();
    if (ibl_irradiance_program) {
        add_shader_program_to_engine(engine, ibl_irradiance_program);
    }

    ShaderProgram* ibl_prefilter_program = create_ibl_prefilter_program();
    if (ibl_prefilter_program) {
        add_shader_program_to_engine(engine, ibl_prefilter_program);
    }

    ShaderProgram* ibl_brdf_program = create_ibl_brdf_program();
    if (ibl_brdf_program) {
        add_shader_program_to_engine(engine, ibl_brdf_program);
    }

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

void set_engine_headless(Engine* engine, bool headless) {
    if (!engine)
        return;
    engine->headless = headless;
}

void set_engine_ss_scale(Engine* engine, int ss_scale) {
    if (!engine)
        return;
    if (ss_scale < 1)
        ss_scale = 1;
    // The tone-map downsample is an exact box filter only at 2x (GL linear
    // sampling averages one 2x2 texel block); higher factors under-resolve and
    // can also exceed GL_MAX_TEXTURE_SIZE. Cap until a proper mip/N-tap
    // downsample exists.
    if (ss_scale > 2) {
        log_warn("SSAA %dx unsupported (exact downsample only at 2x); using 2x", ss_scale);
        ss_scale = 2;
    }
    engine->ss_scale = ss_scale;
}

void set_engine_screenshot_path(Engine* engine, const char* path) {
    if (!engine)
        return;
    if (engine->screenshot_path) {
        free(engine->screenshot_path);
        engine->screenshot_path = NULL;
    }
    if (path) {
        engine->screenshot_path = strdup(path);
    }
}

void set_engine_screenshot_every(Engine* engine, int every) {
    if (!engine)
        return;
    engine->screenshot_every = every > 0 ? every : 0;
}

/*
 * Build a numbered variant of a screenshot path: /tmp/shot.ppm -> /tmp/shot_000042.ppm
 */
static void _numbered_screenshot_path(const char* path, size_t frame, char* out, size_t out_size) {
    const char* dot = strrchr(path, '.');
    if (dot && dot != path) {
        snprintf(out, out_size, "%.*s_%06zu%s", (int)(dot - path), path, frame, dot);
    } else {
        snprintf(out, out_size, "%s_%06zu", path, frame);
    }
}

/*
 * Save the default framebuffer (after the post pass and GUI) to a binary
 * PPM file.
 */
static void _save_framebuffer_ppm(const Engine* engine, const char* path) {
    int w = engine->fb_width;
    int h = engine->fb_height;
    if (w <= 0 || h <= 0)
        return;

    unsigned char* pixels = malloc((size_t)w * (size_t)h * 3);
    if (!pixels) {
        log_error("Failed to allocate screenshot buffer");
        return;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels);

    FILE* f = fopen(path, "wb");
    if (!f) {
        log_error("Failed to open screenshot file: %s", path);
        free(pixels);
        return;
    }

    fprintf(f, "P6\n%d %d\n255\n", w, h);
    // GL rows are bottom-up; PPM is top-down
    for (int y = h - 1; y >= 0; y--) {
        fwrite(pixels + (size_t)y * (size_t)w * 3, 1, (size_t)w * 3, f);
    }
    fclose(f);
    free(pixels);
    log_info("Saved screenshot: %s (%dx%d)", path, w, h);
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

            // Button for toggling bone X-ray
            nk_layout_row_dynamic(engine->nk_ctx, 30, 1);
            if (nk_button_label(engine->nk_ctx, engine->show_bones ? "Hide Bones" : "Show Bones")) {
                engine->show_bones = !engine->show_bones;
            }

            // Button for animation play/pause
            AnimationState* anim = get_render_animation_state();
            if (anim) {
                nk_layout_row_dynamic(engine->nk_ctx, 30, 1);
                if (nk_button_label(engine->nk_ctx, anim->playing ? "Pause Animation" : "Play Animation")) {
                    if (anim->playing) {
                        pause_animation(anim);
                    } else {
                        play_animation(anim);
                    }
                }

                // Button to recalculate bind poses from skeleton hierarchy
                if (anim->skeleton) {
                    nk_layout_row_dynamic(engine->nk_ctx, 30, 1);
                    if (nk_button_label(engine->nk_ctx, "Recalc Bind Pose")) {
                        recalculate_inverse_bind_poses(anim->skeleton);
                    }
                }

                // Spring-bone secondary motion controls
                if (anim->springs) {
                    SpringBoneSystem* sb = anim->springs;

                    nk_layout_row_dynamic(engine->nk_ctx, 10, 1);
                    nk_spacing(engine->nk_ctx, 1);

                    nk_layout_row_dynamic(engine->nk_ctx, 20, 1);
                    nk_label(engine->nk_ctx, "Spring Bones", NK_TEXT_LEFT);

                    nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
                    if (nk_button_label(engine->nk_ctx,
                                        sb->enabled ? "Springs: On" : "Springs: Off")) {
                        sb->enabled = !sb->enabled;
                        if (sb->enabled) {
                            spring_bone_reset(sb); // Re-enable snaps instead of lurching
                        }
                    }

                    nk_property_float(engine->nk_ctx, "Stiffness:", 0.0f, &sb->params.stiffness,
                                      1.0f, 0.05f, 0.005f);
                    nk_property_float(engine->nk_ctx, "Damping:", 0.0f, &sb->params.damping, 1.0f,
                                      0.05f, 0.005f);
                    nk_property_float(engine->nk_ctx, "Gravity:", 0.0f, &sb->params.gravity, 30.0f,
                                      0.5f, 0.1f);
                }
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

            // Lighting section
            Scene* current_scene = get_current_scene(engine);
            if (current_scene && current_scene->light_count > 0) {
                nk_layout_row_dynamic(engine->nk_ctx, 10, 1);
                nk_spacing(engine->nk_ctx, 1);

                nk_layout_row_dynamic(engine->nk_ctx, 20, 1);
                nk_label(engine->nk_ctx, "Lighting", NK_TEXT_LEFT);

                // Use first light's intensity as the master value
                static float light_intensity = 3.0f;
                float prev_intensity = light_intensity;

                nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
                nk_property_float(engine->nk_ctx, "Intensity:", 0.0f, &light_intensity, 20.0f, 0.1f,
                                  0.1f);

                // Apply to all lights if changed
                if (light_intensity != prev_intensity) {
                    for (size_t i = 0; i < current_scene->light_count; i++) {
                        if (current_scene->lights[i]) {
                            current_scene->lights[i]->intensity = light_intensity;
                        }
                    }
                }
            }

            // Environment/skybox section
            if (current_scene && current_scene->render_skybox && current_scene->ibl) {
                nk_layout_row_dynamic(engine->nk_ctx, 10, 1);
                nk_spacing(engine->nk_ctx, 1);

                nk_layout_row_dynamic(engine->nk_ctx, 20, 1);
                nk_label(engine->nk_ctx, "Environment", NK_TEXT_LEFT);

                nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
                if (nk_button_label(engine->nk_ctx, current_scene->skybox_ground_projection
                                                        ? "Ground Projection: On"
                                                        : "Ground Projection: Off")) {
                    current_scene->skybox_ground_projection =
                        !current_scene->skybox_ground_projection;
                }

                if (current_scene->skybox_ground_projection) {
                    nk_property_float(engine->nk_ctx, "Dome Radius:", 1.0f,
                                      &current_scene->skybox_gp_radius, 100.0f, 0.5f, 0.1f);
                    nk_property_float(engine->nk_ctx, "Capture Height:", 0.1f,
                                      &current_scene->skybox_gp_height, 10.0f, 0.1f, 0.05f);
                }

                nk_property_float(engine->nk_ctx, "IBL Intensity:", 0.0f,
                                  &current_scene->ibl->intensity, 4.0f, 0.1f, 0.05f);

                if (current_scene->shadow_system) {
                    ShadowSystem* ss = current_scene->shadow_system;
                    if (nk_button_label(engine->nk_ctx,
                                        ss->enabled ? "Shadows: On" : "Shadows: Off")) {
                        ss->enabled = !ss->enabled;
                    }
                    if (ss->enabled) {
                        if (nk_button_label(engine->nk_ctx,
                                            ss->pcss_enabled ? "PCSS: On" : "PCSS: Off")) {
                            ss->pcss_enabled = !ss->pcss_enabled;
                        }
                        if (ss->pcss_enabled) {
                            nk_property_float(engine->nk_ctx, "Shadow Softness:", 0.0f,
                                              &ss->pcss_softness, 4.0f, 0.1f, 0.02f);
                        }
                    }
                }

                if (nk_button_label(engine->nk_ctx, current_scene->shadow_catcher
                                                        ? "Shadow Catcher: On"
                                                        : "Shadow Catcher: Off")) {
                    current_scene->shadow_catcher = !current_scene->shadow_catcher;
                }

                if (current_scene->shadow_catcher) {
                    nk_property_float(engine->nk_ctx, "Shadow Strength:", 0.0f,
                                      &current_scene->shadow_catcher_strength, 1.0f, 0.05f,
                                      0.01f);
                }
            }

            // Post-processing controls
            if (engine->postfx) {
                PostFX* fx = engine->postfx;

                nk_layout_row_dynamic(engine->nk_ctx, 10, 1);
                nk_spacing(engine->nk_ctx, 1);

                nk_layout_row_dynamic(engine->nk_ctx, 20, 1);
                nk_label(engine->nk_ctx, "Post", NK_TEXT_LEFT);

                nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
                if (nk_button_label(engine->nk_ctx, fx->tonemap_mode == POSTFX_TONEMAP_ACES
                                                        ? "Tonemap: ACES"
                                                        : "Tonemap: Neutral")) {
                    fx->tonemap_mode = fx->tonemap_mode == POSTFX_TONEMAP_ACES
                                           ? POSTFX_TONEMAP_NEUTRAL
                                           : POSTFX_TONEMAP_ACES;
                }

                nk_property_float(engine->nk_ctx, "Exposure:", 0.05f, &fx->exposure, 8.0f, 0.05f,
                                  0.01f);

                if (nk_button_label(engine->nk_ctx,
                                    fx->bloom_enabled ? "Bloom: On" : "Bloom: Off")) {
                    fx->bloom_enabled = !fx->bloom_enabled;
                }

                if (fx->bloom_enabled) {
                    nk_property_float(engine->nk_ctx, "Bloom Strength:", 0.0f,
                                      &fx->bloom_strength, 1.0f, 0.02f, 0.005f);
                    nk_property_float(engine->nk_ctx, "Bloom Threshold:", 0.0f,
                                      &fx->bloom_threshold, 8.0f, 0.1f, 0.02f);
                }

                if (nk_button_label(engine->nk_ctx, fx->ssao_enabled ? "SSAO: On" : "SSAO: Off")) {
                    fx->ssao_enabled = !fx->ssao_enabled;
                }

                if (fx->ssao_enabled) {
                    nk_property_float(engine->nk_ctx, "SSAO Radius:", 0.05f, &fx->ssao_radius,
                                      5.0f, 0.05f, 0.01f);
                    nk_property_float(engine->nk_ctx, "SSAO Strength:", 0.0f, &fx->ssao_strength,
                                      1.0f, 0.05f, 0.01f);
                }

                if (nk_button_label(engine->nk_ctx,
                                    fx->ssr_enabled ? "SSR: On" : "SSR: Off")) {
                    fx->ssr_enabled = !fx->ssr_enabled;
                }

                if (fx->ssr_enabled) {
                    nk_property_float(engine->nk_ctx, "SSR Strength:", 0.0f, &fx->ssr_strength,
                                      2.0f, 0.05f, 0.01f);
                    nk_property_float(engine->nk_ctx, "SSR Distance:", 1.0f,
                                      &fx->ssr_max_distance, 50.0f, 0.5f, 0.1f);
                    nk_property_float(engine->nk_ctx, "SSR Floor Rough:", 0.0f,
                                      &fx->ssr_floor_roughness, 1.0f, 0.05f, 0.01f);
                }

                if (nk_button_label(engine->nk_ctx,
                                    fx->normals_enabled ? "Normals: On" : "Normals: Off")) {
                    fx->normals_enabled = !fx->normals_enabled;
                }

                nk_property_float(engine->nk_ctx, "Spec AA:", 0.0f,
                                  &engine->specular_aa_strength, 2.0f, 0.1f, 0.02f);

                // Finishing grade
                nk_layout_row_dynamic(engine->nk_ctx, 10, 1);
                nk_spacing(engine->nk_ctx, 1);
                nk_layout_row_dynamic(engine->nk_ctx, 20, 1);
                nk_label(engine->nk_ctx, "Finishing", NK_TEXT_LEFT);

                nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
                if (nk_button_label(engine->nk_ctx,
                                    fx->vignette_enabled ? "Vignette: On" : "Vignette: Off")) {
                    fx->vignette_enabled = !fx->vignette_enabled;
                }
                if (fx->vignette_enabled) {
                    nk_property_float(engine->nk_ctx, "Vig Strength:", 0.0f,
                                      &fx->vignette_strength, 1.0f, 0.05f, 0.01f);
                    nk_property_float(engine->nk_ctx, "Vig Radius:", 0.0f, &fx->vignette_radius,
                                      1.0f, 0.05f, 0.01f);
                }

                if (nk_button_label(engine->nk_ctx,
                                    fx->sharpen_enabled ? "Sharpen: On" : "Sharpen: Off")) {
                    fx->sharpen_enabled = !fx->sharpen_enabled;
                }
                if (fx->sharpen_enabled) {
                    nk_property_float(engine->nk_ctx, "Sharpen:", 0.0f, &fx->sharpen_strength,
                                      3.0f, 0.05f, 0.01f);
                }

                if (nk_button_label(engine->nk_ctx,
                                    fx->grain_enabled ? "Grain: On" : "Grain: Off")) {
                    fx->grain_enabled = !fx->grain_enabled;
                }
                if (fx->grain_enabled) {
                    nk_property_float(engine->nk_ctx, "Grain:", 0.0f, &fx->grain_strength, 0.3f,
                                      0.005f, 0.002f);
                }

                if (nk_button_label(engine->nk_ctx,
                                    fx->grade_enabled ? "Grade: On" : "Grade: Off")) {
                    fx->grade_enabled = !fx->grade_enabled;
                }
                if (fx->grade_enabled) {
                    // Three RGB rows: lift (shadows), gamma (mids), gain (highs).
                    // Labels must be unique — Nuklear keys widget state by name.
                    nk_layout_row_dynamic(engine->nk_ctx, 25, 3);
                    nk_property_float(engine->nk_ctx, "LiftR", -1.0f, &fx->grade_lift[0], 1.0f,
                                      0.02f, 0.005f);
                    nk_property_float(engine->nk_ctx, "LiftG", -1.0f, &fx->grade_lift[1], 1.0f,
                                      0.02f, 0.005f);
                    nk_property_float(engine->nk_ctx, "LiftB", -1.0f, &fx->grade_lift[2], 1.0f,
                                      0.02f, 0.005f);
                    nk_property_float(engine->nk_ctx, "GammaR", 0.1f, &fx->grade_gamma[0], 4.0f,
                                      0.02f, 0.005f);
                    nk_property_float(engine->nk_ctx, "GammaG", 0.1f, &fx->grade_gamma[1], 4.0f,
                                      0.02f, 0.005f);
                    nk_property_float(engine->nk_ctx, "GammaB", 0.1f, &fx->grade_gamma[2], 4.0f,
                                      0.02f, 0.005f);
                    nk_property_float(engine->nk_ctx, "GainR", 0.0f, &fx->grade_gain[0], 4.0f,
                                      0.02f, 0.005f);
                    nk_property_float(engine->nk_ctx, "GainG", 0.0f, &fx->grade_gain[1], 4.0f,
                                      0.02f, 0.005f);
                    nk_property_float(engine->nk_ctx, "GainB", 0.0f, &fx->grade_gain[2], 4.0f,
                                      0.02f, 0.005f);
                }

                // Grade uses a 3-column grid; restore single-column rows
                nk_layout_row_dynamic(engine->nk_ctx, 25, 1);
                if (nk_button_label(engine->nk_ctx, fx->dof_enabled ? "DoF: On" : "DoF: Off")) {
                    fx->dof_enabled = !fx->dof_enabled;
                }
                if (fx->dof_enabled) {
                    if (nk_button_label(engine->nk_ctx,
                                        fx->dof_autofocus ? "Autofocus: On" : "Autofocus: Off")) {
                        fx->dof_autofocus = !fx->dof_autofocus;
                    }
                    // With autofocus on, the engine drives Focus Dist each frame
                    nk_property_float(engine->nk_ctx, "Focus Dist:", 0.0f, &fx->dof_focus_distance,
                                      1000.0f, 0.1f, 0.05f);
                    nk_property_float(engine->nk_ctx, "Focus Range:", 0.1f, &fx->dof_focus_range,
                                      1000.0f, 0.1f, 0.05f);
                    nk_property_float(engine->nk_ctx, "Max CoC:", 0.0f, &fx->dof_max_coc, 40.0f,
                                      0.5f, 0.1f);
                }
            }

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

    // ImGui: NewFrame ran at the top of the loop; build the demo window (the
    // Stage A proof — replaced by the ported panel in Stage C) and render it
    // over Nuklear. Stage D removes Nuklear entirely.
    igShowDemoWindow(NULL);
    igRender();
    ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());

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
        Scene* scene = engine->scenes[i];
        if (scene) {
            SceneNode* root_node = scene->root_node;
            if (!root_node)
                continue;
            set_show_xyz_for_nodes(root_node, show_xyz);
        }
    }
}

void engine_present_frame(Engine* engine, RenderMode frame_mode, bool draw_gui) {
    if (!engine)
        return;

    // Only PBR frames are linear HDR and get SSAO + bloom + exposure + tone
    // mapping; debug render modes emit display-ready colors and are copied
    // unchanged. The GUI draws after so it is never tone mapped.
    // Seed the film grain off the frame counter so it animates live yet stays
    // deterministic across equal --frames runs.
    engine->postfx->frame_index = (int)engine->total_frames;
    // Depth-of-field autofocus: keep the subject sharp as the camera orbits or
    // zooms by refocusing on the point the camera looks at (the orbit target),
    // unless a manual focus distance was pinned.
    if (engine->postfx->dof_enabled && engine->postfx->dof_autofocus && engine->camera) {
        engine->postfx->dof_focus_distance =
            glm_vec3_distance(engine->camera->position, engine->camera->look_at);
    }
    postfx_run(engine->postfx, engine->framebuffer, 0, frame_mode == RENDER_MODE_PBR,
               engine->normals_this_frame, engine->projection_matrix);

    if (draw_gui) {
        render_nuklear_gui(engine);
    }
}

void engine_set_scene_draw_buffers(const Engine* engine, bool with_normals) {
    if (!engine)
        return;

    if (with_normals && engine->normals_this_frame) {
        const GLenum both[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, both);
        // Blending is enabled globally; keep the normals target opaque.
        // Indexed state is wiped by any blanket glEnable(GL_BLEND), so
        // re-issue it at every pass boundary rather than once at init.
        glDisablei(GL_BLEND, 1);
    } else {
        const GLenum color_only[] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, color_only);
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

        // Clamp delta time to prevent physics explosions on frame drops
        if (engine->delta_time > 0.1)
            engine->delta_time = 0.1;

        engine->frame_count++;
        engine->fps_update_timer += (float)engine->delta_time;

        if (engine->fps_update_timer >= 0.5f) {
            engine->fps = (float)engine->frame_count / engine->fps_update_timer;
            engine->frame_count = 0;
            engine->fps_update_timer = 0.0f;
        }

        // Begin the ImGui frame before the app's render_func, so both the app
        // (e.g. the tree demo) and the engine panel can add windows between
        // NewFrame and the Render/RenderDrawData at present time.
        if (engine->show_gui || engine->show_fps) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            igNewFrame();
        }

        // Wireframe mode: use albedo-only rendering for performance
        RenderMode saved_render_mode = engine->current_render_mode;
        if (engine->show_wireframe) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            engine->current_render_mode = RENDER_MODE_ALBEDO;
        } else {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }
        // The mode this frame actually renders with (wireframe substitutes
        // ALBEDO), used by the present pass after the mode is restored
        RenderMode frame_mode = engine->current_render_mode;

        // Shadow depth pass (before main render)
        Scene* shadow_scene = get_current_scene(engine);
        if (shadow_scene && shadow_scene->shadow_system && shadow_scene->shadow_system->enabled) {
            render_shadow_depth_pass(engine, shadow_scene);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, engine->framebuffer);
        // The scene target is supersampled; the present pass left the viewport
        // at display size, so set it to the full render size here.
        int rw, rh;
        engine_render_size(engine, &rw, &rh);
        glViewport(0, 0, rw, rh);
        engine->normals_this_frame =
            frame_mode == RENDER_MODE_PBR && postfx_wants_normals(engine->postfx);
        engine_set_scene_draw_buffers(engine, true);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        if (engine->normals_this_frame) {
            const GLfloat zero_normal[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            glClearBufferfv(GL_COLOR, 1, zero_normal);
        }

        Scene* current_scene = get_current_scene(engine);

        // Process pending async texture uploads (max 5 per frame to avoid stutter)
        if (current_scene && current_scene->tex_pool && engine->async_loader) {
            async_loader_process_pending(engine->async_loader, current_scene->tex_pool, 5);
        }

        if (render_func != NULL && current_scene != NULL) {
            render_func(engine, current_scene);
        }

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        engine->current_render_mode = saved_render_mode;

        engine_present_frame(engine, frame_mode, true);

        engine->total_frames++;

        // Periodic capture: numbered frames every N frames
        if (engine->screenshot_path && engine->screenshot_every > 0 &&
            engine->total_frames % (size_t)engine->screenshot_every == 0) {
            char numbered[1024];
            _numbered_screenshot_path(engine->screenshot_path, engine->total_frames, numbered,
                                      sizeof(numbered));
            _save_framebuffer_ppm(engine, numbered);
        }

        // Capture the final frame before exit if a screenshot was requested
        if (engine->screenshot_path && glfwWindowShouldClose(engine->window)) {
            _save_framebuffer_ppm(engine, engine->screenshot_path);
        }

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
    vec3 ray_dir = {0};
    compute_ray_from_screen((float)mouse_fb_x, (float)mouse_fb_y, engine->fb_width,
                            engine->fb_height, projection, view, engine->camera->position, ray_dir);

    // Project ray to the stored drag plane distance
    ray_point_at_distance(engine->camera->position, ray_dir, engine->input.drag_plane_distance,
                          out_world_pos);
}

static SceneNode* _perform_engine_ray_picking(Engine* engine, double mouse_fb_x,
                                              double mouse_fb_y) {
    // Need camera for ray picking
    if (!engine->camera)
        return NULL;

    // Build projection and view matrices
    mat4 projection, view;
    glm_perspective(engine->camera->fov_radians, engine->camera->aspect_ratio,
                    engine->camera->near_clip, engine->camera->far_clip, projection);
    glm_lookat(engine->camera->position, engine->camera->look_at, engine->camera->up_vector, view);

    // Compute ray from screen coordinates
    vec3 ray_origin, ray_dir = {0};
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
