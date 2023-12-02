#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "common.h"
#include "mesh.h"
#include "shader.h"
#include "program.h"
#include "scene.h"
#include "util.h"
#include "import.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_GLFW_GL3_IMPLEMENTATION
#define NK_KEYSTATE_BASED_INPUT
#include "nuklear.h"
#include "nuklear_glfw_gl3.h"

#define MAX_VERTEX_BUFFER 512 * 1024
#define MAX_ELEMENT_BUFFER 128 * 1024

#define PBR_VERT_SHADER_PATH "/Users/neo/Desktop/graphics/engine/src/pbr_vert.glsl"
#define PBR_FRAG_SHADER_PATH "/Users/neo/Desktop/graphics/engine/src/pbr_frag.glsl"
#define AXES_VERT_SHADER_PATH "/Users/neo/Desktop/graphics/engine/src/axes_vert.glsl"
#define AXES_FRAG_SHADER_PATH "/Users/neo/Desktop/graphics/engine/src/axes_frag.glsl"

const unsigned int WIDTH = 800;
const unsigned int HEIGHT = 600;
const float ROTATE_SPEED = 0.4f; // Speed of rotation
const float RADIUS = 2.0f;
const unsigned int RINGS = 20;
const unsigned int SECTORS = 20;
const float rotationSensitivity = 0.005f;

float minDistance = 2000.0f;
float maxDistance = 3000.0f;
float oscillationSpeed = 0.5f; // Adjust this value as needed

float cameraDistance = 2000.0f;
float cameraHeight = 0.0f; // Base height of the camera
float cameraTheta = 0.3f;
float cameraPhi = 0.0f;
float zoomSpeed = 0.005f;
double orbitSpeed = 0.001f;


void generateSphere(Mesh* mesh, float radius, unsigned int rings, unsigned int sectors) {
    float const R = 1.0f / (float)(rings - 1);
    float const S = 1.0f / (float)(sectors - 1);

    mesh->vertexCount = rings * sectors * 3;
    mesh->indexCount = (rings - 1) * (sectors - 1) * 6;

    mesh->vertices = malloc(mesh->vertexCount * sizeof(float));
    mesh->indices = malloc(mesh->indexCount * sizeof(unsigned int));

    unsigned int v = 0, i = 0;
    for (unsigned int r = 0; r < rings; ++r) {
        for (unsigned int s = 0; s < sectors; ++s) {
            float const y = sinf(-M_PI_2 + M_PI * r * R);
            float const x = cosf(2 * M_PI * s * S) * sinf(M_PI * r * R);
            float const z = sinf(2 * M_PI * s * S) * sinf(M_PI * r * R);

            mesh->vertices[v++] = x * radius;
            mesh->vertices[v++] = y * radius;
            mesh->vertices[v++] = z * radius;
        }
    }

    for (unsigned int r = 0; r < rings - 1; ++r) {
        for (unsigned int s = 0; s < sectors - 1; ++s) {
            mesh->indices[i++] = r * sectors + s;
            mesh->indices[i++] = r * sectors + (s + 1);
            mesh->indices[i++] = (r + 1) * sectors + (s + 1);

            mesh->indices[i++] = (r + 1) * sectors + (s + 1);
            mesh->indices[i++] = (r + 1) * sectors + s;
            mesh->indices[i++] = r * sectors + s;
        }
    }
}


void error_callback(int error, const char* description) {
    fprintf(stderr, "Error: %s\n", description);
}



bool dragging = false;
float centerX, centerY;
float dx, dy;

void cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    int windowWidth, windowHeight, framebufferWidth, framebufferHeight;

    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    xpos = ((xpos / windowWidth) * framebufferWidth);
    ypos = ((1 - ypos / windowHeight) * framebufferHeight);


    if (dragging) {
        dx = xpos - centerX;
        dy = ypos - centerY;
    }
}

void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    int windowWidth, windowHeight, framebufferWidth, framebufferHeight;

    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);

    xpos = ((xpos / windowWidth) * framebufferWidth);
    ypos = ((1 - ypos / windowHeight) * framebufferHeight);


    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        dragging = true;
        centerX = xpos;
        centerY = ypos;
        printf("dragging %i %f %f\n", dragging, xpos, ypos);
    } else if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        dragging = false;
        printf("dragging %i %f %f\n", dragging, xpos, ypos);
    }
}


int main() {
    glfwSetErrorCallback(error_callback);

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


    GLFWwindow* window = glfwCreateWindow(WIDTH, HEIGHT, "Wireframe Sphere", NULL, NULL);
    if (window == NULL) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glfwSwapInterval(1);

    if (glewInit() != GLEW_OK) {
        fprintf(stderr, "Failed to initialize GLEW\n");
        return -1;
    }

    struct nk_glfw glfw = {0};
    struct nk_context *ctx;
    struct nk_colorf bg;
    struct nk_image img;

    ctx = nk_glfw3_init(&glfw, window, NK_GLFW3_INSTALL_CALLBACKS);
    {
        struct nk_font_atlas *atlas;
        nk_glfw3_font_stash_begin(&glfw, &atlas);
        nk_glfw3_font_stash_end(&glfw);
        nk_style_load_all_cursors(ctx, atlas->cursors);
    }

    int framebufferWidth, framebufferHeight;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    glViewport(0, 0, framebufferWidth, framebufferHeight);
    CheckOpenGLError("view port");

    /*
     * MSAA anti-aliasing
     *
     */
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    glViewport(0, 0, framebufferWidth, framebufferHeight);
    CheckOpenGLError("view port");

    // Set up MSAA anti-aliasing
    GLuint framebuffer, multisampleTexture, depthRbo;
    int samples = 4; // Number of samples for MSAA, adjust as needed

    // Create and set up the multisample framebuffer
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    glGenTextures(1, &multisampleTexture);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, multisampleTexture);
    glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGB, framebufferWidth, framebufferHeight, GL_TRUE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, multisampleTexture, 0);

    glGenRenderbuffers(1, &depthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, framebufferWidth, framebufferHeight);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthRbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "Error: MSAA Framebuffer is not complete!\n");
        return -1;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0); // Bind back to the default framebuffer

    /*
     * Set up shaders.
     *
     */
    Program* pbr_shader_program = create_program();

    Shader* pbr_vertex_shader = create_shader_from_path(VERTEX, PBR_VERT_SHADER_PATH);
    Shader *pbr_geometry_shader = NULL;
    Shader* pbr_fragment_shader = create_shader_from_path(FRAGMENT, PBR_FRAG_SHADER_PATH);

    if (pbr_vertex_shader) {
        if (compile_shader(pbr_vertex_shader)) {
            attach_shader(pbr_shader_program, pbr_vertex_shader);
        } else {
            fprintf(stderr, "Vertex shader compilation failed\n");
            return -1;
        }
    }

    if (pbr_geometry_shader) {
        if (compile_shader(pbr_geometry_shader)) {
            attach_shader(pbr_shader_program, pbr_geometry_shader);
        } else {
            fprintf(stderr, "Geometry shader compilation failed\n");
            return -1;
        }
    }

    if (pbr_fragment_shader) {
        if (compile_shader(pbr_fragment_shader)) {
            attach_shader(pbr_shader_program, pbr_fragment_shader);
        } else {
            fprintf(stderr, "Fragment shader compilation failed\n");
            return -1;
        }
    }

    // Link the shader program
    if (!link_program(pbr_shader_program)) {
        fprintf(stderr, "Shader program linking failed\n");
        return -1;
    }


    Program* axes_shader_program = create_program();
    Shader* axes_vertex_shader = create_shader_from_path(VERTEX, AXES_VERT_SHADER_PATH);
    Shader* axes_fragment_shader = create_shader_from_path(FRAGMENT, AXES_FRAG_SHADER_PATH);

    if (axes_vertex_shader) {
        if (compile_shader(axes_vertex_shader)) {
            attach_shader(axes_shader_program, axes_vertex_shader);
        } else {
            fprintf(stderr, "Vertex shader compilation failed\n");
            return -1;
        }
    }

    if (axes_fragment_shader) {
        if (compile_shader(axes_fragment_shader)) {
            attach_shader(axes_shader_program, axes_fragment_shader);
        } else {
            fprintf(stderr, "Fragment shader compilation failed\n");
            return -1;
        }
    }

    // Link the shader program
    if (!link_program(axes_shader_program)) {
        fprintf(stderr, "Shader program linking failed\n");
        return -1;
    }

    
    mat4 view;
    mat4 projection;
    glm_mat4_identity(view);
    glm_mat4_identity(projection);

    vec3 cameraPosition = {0.0f, 2.0f, 300.0f};
    vec3 lookAtPoint = {0.0f, 0.0f, 0.0f};
    vec3 upVector = {0.0f, 1.0f, 0.0f};

    glm_lookat(cameraPosition, lookAtPoint, upVector, view);

    float fovRadians = glm_rad(35.0f);
    float aspectRatio = (float)framebufferWidth / (float)framebufferHeight;
    float nearClip = 0.1f;
    float farClip = 10000.0f;

    float cameraAmplitude = (maxDistance - minDistance) / 2.0f; // Half the range of motion
    float midPoint = minDistance + cameraAmplitude; // Middle point of the motion

    glm_perspective(fovRadians, aspectRatio, nearClip, farClip, projection);

    Scene *scene = import_fbx("/Users/neo/Desktop/graphics/engine/src/room.fbx", \
            "/Users/neo/Desktop/graphics/engine/src/room.fbm");

    // Create scene node for the sphere
    SceneNode* root_node = scene->root_node;
    assert(root_node != NULL);

    vec3 lightPosition = {0.0, 0.0, 2000.00};
    if(!root_node->light){
        Light *light = create_light();
        set_light_name(light, "root");
        set_light_type(light, LIGHT_POINT);
        set_light_position(light, lightPosition);
        set_light_intensity(light, 200.0f);
        set_light_color(light, (vec3){1.0f, 0.0f, 0.0f});
        set_node_light(root_node, light);
    }

    setup_node_meshes(root_node);
    set_shaders_for_node(root_node, pbr_vertex_shader, pbr_geometry_shader, pbr_fragment_shader);
    set_program_for_node(root_node, pbr_shader_program);

    setup_node_axes_buffers(root_node);
    set_axes_program_for_node(root_node, axes_shader_program);
    set_axes_shaders_for_node(root_node, axes_vertex_shader, axes_fragment_shader);

    print_scene(scene);

    //glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glEnable(GL_DEPTH_TEST);

    GLint camPosLoc = glGetUniformLocation(pbr_shader_program->programID, "camPos");
    glUniform3fv(camPosLoc, 1, (const GLfloat*)&cameraPosition);

    GLint timeUniformLocation = glGetUniformLocation(pbr_shader_program->programID, "time");

    static bool should_draw_axes = true;

    while (!glfwWindowShouldClose(window)) {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer); // Bind multisample framebuffer
        
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);


        float timeValue = glfwGetTime();
        float angle = timeValue * ROTATE_SPEED;

        glUniform1f(timeUniformLocation, timeValue);

        Transform transform = {
            .position = {0.0f, -200.0f, 0.0f},
            .rotation = {0.0f, 0.0f, 0.0f},
            .scale = {1.0f, 1.0f, 1.0f}
        };

        if (!dragging) {
            cameraDistance = midPoint + cameraAmplitude * sin(timeValue * oscillationSpeed);
            cameraPhi += orbitSpeed; // Rotate around the center

            // Calculate vertical movement
            //float cameraHeight = cameraHeightBase + sin(cameraTheta) * cameraDistance;

            // Calculate the new camera position
            vec3 newCameraPosition = {
                cos(cameraPhi) * cameraDistance * cos(cameraTheta),
                cameraHeight,
                sin(cameraPhi) * cameraDistance * cos(cameraTheta)
            };

            // Update camera view matrix
            glm_lookat(newCameraPosition, lookAtPoint, upVector, view);
            glm_perspective(fovRadians, aspectRatio, nearClip, farClip, projection);
            glUniform3fv(camPosLoc, 1, (const GLfloat*)&newCameraPosition);


        } else {
            float zpos = cosf(angle);
            float ypos = sinf(angle);

            transform.position[0] = 0.0f;
            transform.position[1] = -25.0f;
            transform.position[2] = 0.0f;
            transform.rotation[0] = dy * rotationSensitivity;
            transform.rotation[1] = dx * rotationSensitivity;
            transform.rotation[2] = 0.0f;
        }


        transform_node(root_node, &transform);

        mat4 identityMatrix;
        glm_mat4_identity(identityMatrix);
        update_child_nodes(root_node, identityMatrix);

        render_node(root_node, root_node->local_transform, view, projection);

        GLint previousViewport[4];
        glGetIntegerv(GL_VIEWPORT, previousViewport);
        GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
        GLboolean blend = glIsEnabled(GL_BLEND);

        nk_glfw3_new_frame(&glfw);

        if (nk_begin(ctx, "Camera", nk_rect(15, 15, 230, 500),
            NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|
            NK_WINDOW_MINIMIZABLE|NK_WINDOW_TITLE))
        {

            nk_layout_row_static(ctx, 30, 80, 1);
            if (nk_button_label(ctx, "Show Axes")){
                if(should_draw_axes){
                    should_draw_axes = false;
                }else{
                    should_draw_axes = true;
                }
                set_should_draw_axes(root_node, should_draw_axes);
            }

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "Theta:", 0.0f, &cameraTheta, M_PI_2, 0.1f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "Phi:", 0.0f, &cameraPhi, M_PI_2, 0.1f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "Distance:", 0.0f, &cameraDistance, 3000.0f, 100.0f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "Height:", -2000.0f, &cameraHeight, 2000.0f, 100.0f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "LookAt X:", -25.0f, &lookAtPoint[0], 25.0f, 1.0f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "LookAt Y:", -25.0f, &lookAtPoint[1], 25.0f, 1.0f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "LookAt Z:", -25.0f, &lookAtPoint[2], 25.0f, 1.0f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "Up X:", -25.0f, &upVector[0], 25.0f, 1.0f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "Up Y:", -25.0f, &upVector[1], 25.0f, 1.0f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "Up Z:", -25.0f, &upVector[2], 25.0f, 1.0f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "Zoom:", 0.0f, &zoomSpeed, 2.0f, 0.01f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_double(ctx, "Orbit:", 0.0f, &orbitSpeed, 0.1f, 0.001f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "Amplitude:", 0.0f, &cameraAmplitude, 50.0f, 1.0f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "Fov:", 0.0f, &fovRadians, M_PI_2, 0.1f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "Near Clip:", 0.0f, &nearClip, 5.0f, 0.1f, 1);

            nk_layout_row_dynamic(ctx, 25, 1);
            nk_property_float(ctx, "Far Clip:", 0.0f, &farClip, 10000.0f, 500.0f, 1);
        }
        nk_end(ctx);

        nk_glfw3_render(&glfw, NK_ANTI_ALIASING_ON, MAX_VERTEX_BUFFER, MAX_ELEMENT_BUFFER);

        // Restore OpenGL state
        glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
        if (depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); // Default framebuffer
        glBlitFramebuffer(0, 0, framebufferWidth, framebufferHeight, 0, 0, framebufferWidth, framebufferHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    free_node(root_node);
    free_shader(pbr_vertex_shader);
    free_shader(pbr_geometry_shader);
    free_shader(pbr_fragment_shader);
    free_program(pbr_shader_program);
    free_program(axes_shader_program);
    free_shader(axes_vertex_shader);
    free_shader(axes_fragment_shader);

    // Cleanup MSAA resources
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &multisampleTexture);
    glDeleteRenderbuffers(1, &depthRbo);

    nk_glfw3_shutdown(&glfw);
    glfwTerminate();

    return 0;
}



