#include <math.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "cetra/engine.h"

#include "ground.h"
#include "player.h"

// Radians per pixel of mouse travel. Independent of resolution and of frame rate, because a
// mouse delta is already a distance and scaling it by dt would make the same hand movement turn
// a different amount at a different frame rate.
#define PLAYER_LOOK_SENSITIVITY 0.0022f
// Arrow-key look, radians per SECOND -- the opposite of the mouse above, and deliberately so: a
// held key is a rate, not a displacement, so this one has to be scaled by dt or the turn speed
// becomes whatever the frame rate is.
//
// SLOWER than it looks like it should be, and lower than the first attempt's 2.2. A key has no
// way to modulate its own rate the way a hand on a mouse does, so it is stuck at one speed and
// that speed has to be comfortable for the whole turn rather than quick for a glance. 1.0 rad/s
// is a bit under 60 degrees a second, so a full turn takes about six.
#define PLAYER_KEY_LOOK_RATE 1.0f
// Ceiling on ONE frame's mouse travel, in pixels. Not a sensitivity: at 60 Hz this is already a
// faster hand movement than anyone makes, so it never touches real input and only bounds the
// jumps that come from the cursor being moved by something other than a hand.
#define PLAYER_MAX_LOOK_PIXELS 200.0
// Just short of straight up and down, so the forward vector never degenerates against the up
// axis and the look-at matrix stays defined.
#define PLAYER_PITCH_LIMIT 1.5533f // 89 degrees

static void player_set_capture(Player* p, struct Engine* engine, bool capture) {
    // Never in headless: the window is hidden, there is no cursor to capture, and what
    // glfwGetCursorPos reports about one is not a mouse movement. Left unguarded it spun the
    // view off the spawn bearing within a few frames, which is also what an unfocused window
    // would do to a live session.
    if (engine && engine->headless)
        capture = false;
    p->mouse_captured = capture;
    if (!engine || !engine->window)
        return;
    glfwSetInputMode(engine->window, GLFW_CURSOR,
                     capture ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (capture) {
        // The cursor jumps when it is captured, so the first delta after it would be a wild
        // spin. Re-seed the reference on the next frame instead of trusting this one.
        p->warp_pending = true;
    }
}

void player_init(Player* p, struct Engine* engine, float x, float z, float yaw) {
    p->feet[0] = x;
    p->feet[1] = ground_height_at(x, z);
    p->feet[2] = z;
    p->yaw = yaw;
    p->pitch = 0.0f;
    p->vertical_velocity = 0.0f;
    p->grounded = true;
    p->warp_pending = true;
    p->look_was_live = false;
    p->last_mouse_x = 0.0;
    p->last_mouse_y = 0.0;
    player_set_capture(p, engine, true);
}

bool player_on_key(Player* p, struct Engine* engine, int key, int action) {
    if (!p || action != GLFW_PRESS)
        return false;
    if (key == GLFW_KEY_TAB) {
        player_set_capture(p, engine, !p->mouse_captured);
        return true;
    }
    return false;
}

void player_update(Player* p, struct Engine* engine, float dt) {
    if (!p || !engine || !engine->window || !engine->camera)
        return;
    GLFWwindow* win = engine->window;

    // Clamped, not trusted: a frame that stalled (a tree rebuild, a window drag) would
    // otherwise integrate gravity over the whole pause and fire the walker through the ground.
    if (dt > 0.1f)
        dt = 0.1f;

    // Focus, not just capture: an unfocused window still answers glfwGetCursorPos, with the
    // cursor wherever the user took it, and the whole excursion arrives as one frame's delta.
    const bool look_live =
        p->mouse_captured && glfwGetWindowAttrib(win, GLFW_FOCUSED) == GLFW_TRUE;
    // Any transition INTO look-live re-seeds the reference, not just a capture. Focus regain is
    // the case that was missing and it is the one that bites: the cursor moved while the window
    // was away, `last_mouse` is from before it left, and the difference arrives as a single
    // enormous delta. Re-seeding costs one frame of look and cannot be wrong.
    if (look_live && !p->look_was_live)
        p->warp_pending = true;
    p->look_was_live = look_live;

    if (look_live) {
        // The mouse: a DISPLACEMENT already, so it must not be scaled by dt or the same hand
        // movement would turn a different amount at a different frame rate.
        double mx, my;
        glfwGetCursorPos(win, &mx, &my);
        if (p->warp_pending) {
            p->warp_pending = false;
        } else {
            /*
             * Bounded per frame, which is a guard rather than a feel adjustment.
             *
             * The delta is a difference of two absolute cursor readings, so anything that moves
             * the cursor other than the user's hand -- a warp, a focus round trip, a mode change
             * under us -- arrives here as a jump, and an unbounded jump is an instant spin with
             * no way back except quitting. A hand cannot travel this far in one frame, so
             * clamping costs nothing real and turns any such event into a nudge.
             */
            const double lim = PLAYER_MAX_LOOK_PIXELS;
            const double dx = glm_clamp((double)(mx - p->last_mouse_x), -lim, lim);
            const double dy = glm_clamp((double)(my - p->last_mouse_y), -lim, lim);
            p->yaw -= (float)dx * PLAYER_LOOK_SENSITIVITY;
            p->pitch -= (float)dy * PLAYER_LOOK_SENSITIVITY;
        }
        p->last_mouse_x = mx;
        p->last_mouse_y = my;

        // The arrow keys: a held RATE, so this one is scaled by dt. Additive with the mouse
        // rather than instead of it, so there is no mode to be in the wrong one of. Same sign
        // convention: right and down look right and down.
        float dyaw = 0.0f, dpitch = 0.0f;
        if (glfwGetKey(win, GLFW_KEY_LEFT) == GLFW_PRESS)
            dyaw += 1.0f;
        if (glfwGetKey(win, GLFW_KEY_RIGHT) == GLFW_PRESS)
            dyaw -= 1.0f;
        if (glfwGetKey(win, GLFW_KEY_UP) == GLFW_PRESS)
            dpitch += 1.0f;
        if (glfwGetKey(win, GLFW_KEY_DOWN) == GLFW_PRESS)
            dpitch -= 1.0f;
        p->yaw += dyaw * PLAYER_KEY_LOOK_RATE * dt;
        p->pitch += dpitch * PLAYER_KEY_LOOK_RATE * dt;

        // Clamped once, after both sources, rather than after each.
        p->pitch = glm_clamp(p->pitch, -PLAYER_PITCH_LIMIT, PLAYER_PITCH_LIMIT);
    }

    // Ground-plane basis from the yaw alone. Taking it from the look direction instead would
    // make walking forward while looking down a descent through the surface.
    const float sy = sinf(p->yaw), cy = cosf(p->yaw);
    vec3 forward = {-sy, 0.0f, -cy};
    vec3 right = {cy, 0.0f, -sy};

    vec3 wish = GLM_VEC3_ZERO_INIT;
    if (look_live) {
        if (glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS)
            glm_vec3_add(wish, forward, wish);
        if (glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS)
            glm_vec3_sub(wish, forward, wish);
        if (glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS)
            glm_vec3_add(wish, right, wish);
        if (glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS)
            glm_vec3_sub(wish, right, wish);
    }
    // Normalised, so pressing two keys does not walk faster diagonally.
    if (glm_vec3_norm(wish) > 1e-4f)
        glm_vec3_normalize(wish);

    float speed = PLAYER_WALK_SPEED;
    if (look_live && (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                              glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS))
        speed *= PLAYER_SPRINT_MULT;

    p->feet[0] += wish[0] * speed * dt;
    p->feet[2] += wish[2] * speed * dt;

    if (look_live && p->grounded && glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS) {
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
