#include <math.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "cetra/engine.h"

#include "ground.h"
#include "player.h"

// Just short of straight up and down, so the forward vector never degenerates against the up
// axis and the look-at matrix stays defined.
#define PLAYER_PITCH_LIMIT 1.5533f // 89 degrees

void player_init(Player* p, struct Engine* engine, float x, float z, float yaw) {
    (void)engine; // no cursor to capture and no input mode to set: see player.h
    p->feet[0] = x;
    p->feet[1] = ground_height_at(x, z);
    p->feet[2] = z;
    p->yaw = yaw;
    p->pitch = 0.0f;
    p->vertical_velocity = 0.0f;
    p->grounded = true;
    p->walk_speed = PLAYER_WALK_SPEED;
    p->look_rate = PLAYER_LOOK_RATE;
    p->invert_pitch = true;
}

// Held-key state for one axis: 1, -1 or 0.
static float axis(GLFWwindow* win, int positive, int negative) {
    float v = 0.0f;
    if (glfwGetKey(win, positive) == GLFW_PRESS)
        v += 1.0f;
    if (glfwGetKey(win, negative) == GLFW_PRESS)
        v -= 1.0f;
    return v;
}

void player_update(Player* p, struct Engine* engine, float dt) {
    if (!p || !engine || !engine->window || !engine->camera)
        return;
    GLFWwindow* win = engine->window;

    // Clamped, not trusted: a frame that stalled (a tree rebuild, a window drag) would
    // otherwise integrate gravity over the whole pause and fire the walker through the ground.
    if (dt > 0.1f)
        dt = 0.1f;

    /*
     * Two gates, and both are needed.
     *
     * FOCUS, because a held key belongs to whichever window has the keyboard, and GLFW keeps
     * answering glfwGetKey for one that does not. And the GUI's own keyboard capture, because
     * the cursor is never grabbed here -- so a slider can take focus at any moment, and without
     * this, typing a number into one also walks the player and turns their head.
     */
    const bool input_live =
        glfwGetWindowAttrib(win, GLFW_FOCUSED) == GLFW_TRUE && !engine_gui_wants_keyboard();

    // Head first, so the step below travels along the direction the frame will be drawn from.
    if (input_live) {
        p->yaw += axis(win, GLFW_KEY_LEFT, GLFW_KEY_RIGHT) * p->look_rate * dt;
        float dpitch = axis(win, GLFW_KEY_UP, GLFW_KEY_DOWN);
        if (p->invert_pitch)
            dpitch = -dpitch;
        p->pitch = glm_clamp(p->pitch + dpitch * p->look_rate * dt, -PLAYER_PITCH_LIMIT,
                             PLAYER_PITCH_LIMIT);
    }

    // Ground-plane basis from the yaw alone. Taking it from the look direction instead would
    // make walking forward while looking down a descent through the surface.
    const float sy = sinf(p->yaw), cy = cosf(p->yaw);
    vec3 forward = {-sy, 0.0f, -cy};
    vec3 right = {cy, 0.0f, -sy};

    vec3 wish = GLM_VEC3_ZERO_INIT;
    float speed = p->walk_speed;
    if (input_live) {
        glm_vec3_scale(forward, axis(win, GLFW_KEY_W, GLFW_KEY_S), wish);
        vec3 strafe;
        glm_vec3_scale(right, axis(win, GLFW_KEY_D, GLFW_KEY_A), strafe);
        glm_vec3_add(wish, strafe, wish);
        // Normalised, so pressing two keys does not walk faster diagonally.
        if (glm_vec3_norm(wish) > 1e-4f)
            glm_vec3_normalize(wish);
        if (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
            glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS)
            speed *= PLAYER_SPRINT_MULT;
    }

    p->feet[0] += wish[0] * speed * dt;
    p->feet[2] += wish[2] * speed * dt;

    if (input_live && p->grounded && glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS) {
        p->vertical_velocity = PLAYER_JUMP_SPEED;
        p->grounded = false;
    }

    p->vertical_velocity -= PLAYER_GRAVITY * dt;
    p->feet[1] += p->vertical_velocity * dt;

    /*
     * Stand on the surface, evaluated at the position just walked to.
     *
     * This is the whole controller, and it is exact rather than approximate because
     * ground_height_at is the same closed form the island mesh, the grass and the water's
     * shoaling bed are built from -- so the soles cannot be on a different surface than the
     * one being drawn. It also means walking off the rim continues down the seabed with no
     * special case: the function is continuous across it.
     */
    const float ground = ground_height_at(p->feet[0], p->feet[2]);
    if (p->feet[1] <= ground) {
        p->feet[1] = ground;
        p->vertical_velocity = 0.0f;
        p->grounded = true;
    } else {
        p->grounded = false;
    }

    // Drive the camera as a look-AT pair rather than a direction: that is the only camera the
    // engine has, and its `distance` is what the GUI's DoF autofocus reads.
    vec3 eye = {p->feet[0], p->feet[1] + PLAYER_EYE_HEIGHT, p->feet[2]};
    const float cp = cosf(p->pitch);
    vec3 look = {eye[0] + forward[0] * cp, eye[1] + sinf(p->pitch), eye[2] + forward[2] * cp};
    set_camera_position(engine->camera, eye);
    set_camera_look_at(engine->camera, look);
    set_camera_up_vector(engine->camera, (vec3){0.0f, 1.0f, 0.0f});
    engine->camera->distance = glm_vec3_distance(eye, look);
    // Writing the camera is not enough: the view matrix is built from it here, and nothing
    // else in the frame does it. The orbit path got this via mouse_drag_update, which this
    // walker replaces -- so without these two the camera moved and the picture did not.
    update_engine_camera_lookat(engine);
    update_engine_camera_perspective(engine);
}
