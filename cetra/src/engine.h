#ifndef _ENGINE_H_
#define _ENGINE_H_

#include <stdint.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "scene.h"
#include "camera.h"
#include "shader.h"
#include "program.h"
#include "import.h"
#include "common.h"
#include "input.h"
#include "async_loader.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_KEYSTATE_BASED_INPUT
#include "ext/nuklear.h"
#include "ext/nuklear_glfw_gl3.h"

typedef enum CameraMode {
    CAMERA_MODE_FREE,  // Free movement mode
    CAMERA_MODE_ORBIT, // Orbit around a point
} CameraMode;

struct Engine;

typedef void (*CursorPositionCallback)(struct Engine* engine, double xpos, double ypos);
typedef void (*MouseButtonCallback)(struct Engine* engine, int button, int action, int mods);
typedef void (*KeyCallback)(struct Engine* engine, int key, int scancode, int action, int mods);

typedef struct Engine {
    GLFWwindow* window;
    char* window_title; // Title of the GLFW window

    int win_width;
    int win_height;
    int fb_width;
    int fb_height;

    GLFWerrorfun error_callback;

    CursorPositionCallback cursor_position_callback;
    MouseButtonCallback mouse_button_callback;
    KeyCallback key_callback;

    GLuint framebuffer;         // Framebuffer object
    GLuint multisample_texture; // Multisample texture for MSAA
    GLuint depth_renderbuffer;  // Depth renderbuffer

    Camera* camera;         // main camera
    CameraMode camera_mode; // Current camera mode

    Scene** scenes;             // Array of scenes managed by the engine
    size_t scene_count;         // Number of scenes
    size_t current_scene_index; // Index of the currently active scene

    ShaderProgram** programs;   // Global shader programs used across scenes
    size_t program_count;       // Count of global programs
    ShaderProgram* program_map; // name to program cache

    Material** materials;  // Global materials used across meshes
    size_t material_count; // Count of global materials

    RenderMode current_render_mode; // default is PBR

    mat4 model_matrix;
    mat4 view_matrix;
    mat4 projection_matrix;

    // nuklear gui
    struct nk_context* nk_ctx;
    struct nk_glfw nk_glfw;
    struct nk_color bg;

    bool show_gui;
    bool show_wireframe;
    bool show_xyz;
    bool show_fps;

    InputState input;

    // FPS tracking
    double last_frame_time;
    double delta_time;
    float fps;
    float fps_update_timer;
    int frame_count;

    // Async loading
    AsyncLoader* async_loader;
} Engine;

typedef void (*RenderSceneFunc)(Engine*, Scene*);

Engine* create_engine(const char* window_title, int width, int height);
void free_engine(Engine* engine);

int init_engine(Engine* engine);

// GLFW callbacks
void set_engine_error_callback(Engine* engine, GLFWerrorfun error_callback);
void set_engine_mouse_button_callback(Engine* engine, MouseButtonCallback mouse_button_callback);
void set_engine_cursor_position_callback(Engine* engine,
                                         CursorPositionCallback cursor_position_callback);
void set_engine_key_callback(Engine* engine, KeyCallback key_callback);

// Camera
void set_engine_camera(Engine* engine, Camera* camera);
void set_engine_camera_mode(Engine* engine, CameraMode mode);
void update_engine_camera_lookat(Engine* engine);
void update_engine_camera_perspective(Engine* engine);

// Scene
int add_scene_to_engine(Engine* engine, Scene* scene);
void set_active_scene_by_index(Engine* engine, size_t scene_index);
void set_active_scene_by_name(Engine* engine, const char* scene_name);
Scene* get_current_scene(const Engine* engine);

// Shader Programs
int add_shader_program_to_engine(Engine* engine, ShaderProgram* program);
ShaderProgram* get_engine_shader_program_by_name(Engine* engine, const char* program_name);

// GUI
void set_engine_show_gui(Engine* engine, bool show_gui);
void set_engine_show_fps(Engine* engine, bool show_fps);
void render_nuklear_gui(Engine* engine);

// Render
void set_engine_show_wireframe(Engine* engine, bool show_wireframe);
void set_engine_show_xyz(Engine* engine, bool show_xyz);
void run_engine_render_loop(Engine* engine, RenderSceneFunc render_func);

// Drag/pick helpers
void get_mouse_world_position_on_drag_plane(Engine* engine, double mouse_fb_x, double mouse_fb_y,
                                            vec3 out_world_pos);

#endif // _ENGINE_H_
