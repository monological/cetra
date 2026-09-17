
#include "app.h"
#include "engine.h"
#include "scene.h"
#include "camera.h"
#include "camera_rig.h"
#include "light.h"
#include "ext/log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Camera Drag Adapter Implementation
 */

CameraDrag* create_camera_drag(Engine* engine, CameraRig* rig) {
    if (!engine || !rig) {
        log_error("create_camera_drag: NULL engine or rig");
        return NULL;
    }
    CameraDrag* drag = calloc(1, sizeof(CameraDrag));
    if (!drag) {
        log_error("create_camera_drag: out of memory");
        return NULL;
    }
    drag->engine = engine;
    drag->rig = rig;
    drag->sensitivity = 0.002f;
    drag->pan_per_unit = 0.0005f;
    drag->zoom_step = 0.9f;
    drag->auto_orbit_speed = 0.5f;
    drag->auto_orbit_min_dist = 2000.0f;
    drag->auto_orbit_max_dist = 3000.0f;
    return drag;
}

void free_camera_drag(CameraDrag* drag) {
    free(drag);
}

void camera_drag_on_button(CameraDrag* drag, int button, int action, int mods) {
    (void)button;
    (void)action;
    (void)mods;
    if (!drag || !drag->rig)
        return;

    // Latch on the frame the engine says a drag began. The engine owns the
    // button state, so this asks it rather than tracking the button itself --
    // which is what keeps a press the GUI swallowed from latching here.
    if (drag->engine->input.is_dragging) {
        drag->start_yaw = drag->rig->yaw;
        drag->start_pitch = drag->rig->pitch;
        glm_vec3_copy(drag->rig->anchor, drag->start_anchor);
    }
}

/*
 * The pointer into the rig, then the rig into a pose.
 *
 * The recompute is HERE and not the caller's, because three of the four apps
 * converted in spec 12.19 forgot it in the space of one edit and their drags
 * went inert -- no error, no warning, a camera that simply stops. "Deliver the
 * input and work out what it means" is one operation, so it is one call.
 *
 * Every input this adapter produces is ABSOLUTE -- a drag states an aim, the
 * wheel and the keys state a distance -- so the rate arguments are always zero:
 * there is nothing here for the per-second path to integrate.
 */
void camera_drag_update(CameraDrag* drag, float time) {
    if (!drag || !drag->rig)
        return;
    CameraRig* rig = drag->rig;
    const InputState* in = &drag->engine->input;
    const float dt = (float)drag->engine->delta_time;

    if (!in->is_dragging) {
        if (drag->auto_orbit_enabled) {
            // The auto-orbit is this adapter pretending to be a hand, which is
            // why it lives here and not on the rig: it is the one behaviour that
            // needs a wall CLOCK, and the rig is a pure function of what it is
            // told. Giving it one would have put a clock in every rig for the
            // sake of a viewer affordance.
            float amplitude = (drag->auto_orbit_max_dist - drag->auto_orbit_min_dist) / 2.0f;
            float mid = drag->auto_orbit_min_dist + amplitude;
            camera_rig_set_distance(rig, mid + amplitude * sinf(time * drag->auto_orbit_speed));
            camera_rig_aim(rig, rig->yaw + 0.001f, rig->pitch);
        }
        camera_rig_update(rig, dt, 0.0f, 0.0f);
        return;
    }

    if (in->shift_held) {
        // Pan the ANCHOR in the camera's own plane, as an absolute offset from
        // where it was latched -- so the events do not accumulate and a drag
        // returning to its start returns the camera to its start.
        vec3 forward, right_vec, up_vec;
        glm_vec3_sub(rig->pose.look, rig->pose.eye, forward);
        glm_vec3_crossn(drag->engine->camera ? drag->engine->camera->up_vector : (float*)GLM_YUP,
                        forward, right_vec);
        glm_vec3_normalize(right_vec);
        glm_vec3_cross(forward, right_vec, up_vec);
        glm_vec3_normalize(up_vec);

        const float pan = rig->dist * drag->pan_per_unit;
        vec3 offset, up_offset;
        glm_vec3_scale(right_vec, -in->drag_fb_x * pan, offset);
        glm_vec3_scale(up_vec, -in->drag_fb_y * pan, up_offset);
        glm_vec3_add(offset, up_offset, offset);
        glm_vec3_add(drag->start_anchor, offset, rig->anchor);
        camera_rig_update(rig, dt, 0.0f, 0.0f);
        return;
    }

    // Orbit: an offset in pixels from the latched aim is an ANGLE, not a rate,
    // so it goes through camera_rig_aim rather than the per-second path.
    //
    // Both signs are PLUS where the camera's own orbit parameters took minus,
    // and that is a conversion rather than a preference: a rig's yaw and a
    // Camera's phi run in opposite senses (phi = -yaw + k, since the rig places
    // the eye along -dir while phi measures the eye's own bearing), and so do
    // pitch and theta. Writing the old signs onto the new field silently
    // reverses every drag in the viewer -- which is what it did, until
    // cam-drag-orbit read -0.38 where it wanted +0.40.
    camera_rig_aim(rig, drag->start_yaw + in->drag_fb_x * drag->sensitivity,
                   drag->start_pitch - in->drag_fb_y * drag->sensitivity);
    camera_rig_update(rig, dt, 0.0f, 0.0f);
}

void camera_drag_on_scroll(CameraDrag* drag, double xoffset, double yoffset) {
    (void)xoffset;
    if (!drag || !drag->rig || yoffset == 0.0)
        return;
    camera_rig_set_distance(drag->rig, drag->rig->dist * powf(drag->zoom_step, (float)yoffset));
}

bool camera_drag_on_key(CameraDrag* drag, int key, int action, int mods) {
    if (!drag || !drag->rig)
        return false;
    if (action != GLFW_PRESS && action != GLFW_REPEAT)
        return false;

    CameraRig* rig = drag->rig;
    // Scaled by the arm so the feel carries across models of any size.
    const float base = fmaxf(rig->dist, 1.0f);
    const float move_speed = base * 0.1f;
    const float pan_speed = base * 0.05f;
    static const float ORBIT_STEP = 0.1f;

    vec3 dir;
    glm_vec3_sub(rig->pose.look, rig->pose.eye, dir);
    glm_vec3_normalize(dir);
    vec3 right;
    glm_vec3_crossn(dir, (float*)GLM_YUP, right);

    switch (key) {
        case GLFW_KEY_W:
            glm_vec3_muladds(dir, move_speed, rig->anchor);
            return true;
        case GLFW_KEY_S:
            glm_vec3_mulsubs(dir, move_speed, rig->anchor);
            return true;
        case GLFW_KEY_A:
            glm_vec3_mulsubs(right, move_speed, rig->anchor);
            return true;
        case GLFW_KEY_D:
            glm_vec3_muladds(right, move_speed, rig->anchor);
            return true;
        // The arrows PITCH, as they did before a camera mode decided between
        // pitching and zooming; the wheel zooms, which this viewer never had.
        // Each input now does the thing it conventionally does, where the mode
        // made one input mean two things depending on state.
        case GLFW_KEY_UP:
            if (mods & GLFW_MOD_SHIFT)
                rig->anchor[1] += pan_speed;
            else
                camera_rig_aim(rig, rig->yaw, rig->pitch - ORBIT_STEP);
            return true;
        case GLFW_KEY_DOWN:
            if (mods & GLFW_MOD_SHIFT)
                rig->anchor[1] -= pan_speed;
            else
                camera_rig_aim(rig, rig->yaw, rig->pitch + ORBIT_STEP);
            return true;
        case GLFW_KEY_LEFT:
            if (mods & GLFW_MOD_SHIFT)
                glm_vec3_mulsubs(right, pan_speed, rig->anchor);
            else
                camera_rig_aim(rig, rig->yaw - ORBIT_STEP, rig->pitch);
            return true;
        case GLFW_KEY_RIGHT:
            if (mods & GLFW_MOD_SHIFT)
                glm_vec3_muladds(right, pan_speed, rig->anchor);
            else
                camera_rig_aim(rig, rig->yaw + ORBIT_STEP, rig->pitch);
            return true;
        default:
            return false;
    }
}

/*
 * Canvas Controller Implementation
 */

CanvasController* create_canvas_controller(Engine* engine) {
    if (!engine) {
        return NULL;
    }
    CanvasController* ctrl = calloc(1, sizeof(CanvasController));
    if (!ctrl) {
        return NULL;
    }
    ctrl->engine = engine;
    ctrl->zoom_step = 0.9f;
    /*
     * A rig of its own, and the arithmetic above it is UNCHANGED (spec 12.19).
     *
     * A 2D camera is not a 3D camera with the angles zeroed: zoom is
     * ortho_height, which is a projection field where a 3D zoom is a pose field;
     * a 2D zoom is anchored on the cursor and has no 3D analogue; and rotation
     * is not merely unused but forbidden, since any camera rotation shears flat
     * geometry under a parallel projection. So this keeps its own maths.
     *
     * What it joins is the OWNERSHIP rule: it writes its pose through a rig like
     * every other camera, so "one thing moves the camera, and the engine knows
     * which" has no exception on the day it ships.
     */
    ctrl->rig = create_camera_rig();
    if (ctrl->rig && engine->camera)
        camera_rig_set_pose(ctrl->rig, engine->camera->position, engine->camera->look_at);
    engine_set_camera_rig(engine, ctrl->rig);
    return ctrl;
}

void free_canvas_controller(CanvasController* ctrl) {
    if (ctrl)
        free_camera_rig(ctrl->rig);
    free(ctrl);
}

void canvas_world_under_cursor(const Engine* engine, double fb_x, double fb_y, vec3 out) {
    glm_vec3_zero(out);
    if (!engine || !engine->camera || engine->fb_height <= 0) {
        return;
    }
    const Camera* camera = engine->camera;
    float units_per_pixel = camera->ortho_height / (float)engine->fb_height;
    out[0] = camera->look_at[0] + (float)(fb_x - engine->fb_width * 0.5) * units_per_pixel;
    out[1] = camera->look_at[1] + (float)(fb_y - engine->fb_height * 0.5) * units_per_pixel;
    out[2] = camera->look_at[2];
}

void canvas_on_button(CanvasController* ctrl, int button, int action, int mods) {
    (void)mods;
    if (!ctrl || !ctrl->engine || button != GLFW_MOUSE_BUTTON_LEFT) {
        return;
    }
    Engine* engine = ctrl->engine;
    if (action == GLFW_RELEASE) {
        ctrl->panning = false;
        return;
    }
    // The engine has already picked the node under the press, if any.
    ctrl->panning = engine->input.selected_node == NULL && engine->camera != NULL;
    if (ctrl->panning) {
        glm_vec3_copy(engine->camera->look_at, ctrl->pan_start_look_at);
    }
}

// Slide the whole view with the cursor: the world point that was under the
// press stays under it. The eye and the target move together, so the view
// direction never changes.
static void _canvas_pan(CanvasController* ctrl) {
    Engine* engine = ctrl->engine;
    Camera* camera = engine->camera;
    if (!camera) {
        return;
    }
    vec3 press = GLM_VEC3_ZERO_INIT, now = GLM_VEC3_ZERO_INIT, offset, delta;
    canvas_world_under_cursor(engine, engine->input.center_fb_x, engine->input.center_fb_y, press);
    canvas_world_under_cursor(engine, engine->input.center_fb_x + engine->input.drag_fb_x,
                              engine->input.center_fb_y + engine->input.drag_fb_y, now);
    glm_vec3_sub(press, now, offset);
    // As a delta from the latched target rather than from the current one,
    // so a sequence of motion events does not accumulate rounding.
    glm_vec3_add(ctrl->pan_start_look_at, offset, delta);
    glm_vec3_sub(delta, camera->look_at, delta);
    camera_translate(camera, delta);
    camera_rig_set_pose(ctrl->rig, camera->position, camera->look_at);
}

void canvas_on_cursor(CanvasController* ctrl, double fb_x, double fb_y) {
    if (!ctrl || !ctrl->engine) {
        return;
    }
    Engine* engine = ctrl->engine;
    if (!engine->input.is_dragging) {
        return;
    }
    SceneNode* node = engine->input.selected_node;
    if (node) {
        // Where the cursor is on the drag plane, as a delta from where the
        // press was, applied to where the node was: x and y only, since the
        // plane is the board and the node's depth is its own. on_plane is
        // seeded because the unproject leaves it alone with no camera.
        vec3 on_plane = GLM_VEC3_ZERO_INIT, delta, target;
        engine_mouse_to_drag_plane(engine, fb_x, fb_y, on_plane);
        glm_vec3_sub(on_plane, engine->input.drag_start_world_pos, delta);
        glm_vec3_add(engine->input.drag_object_start_pos, delta, target);
        target[2] = node->original_transform[3][2];
        node_set_position(node, target);
    } else if (ctrl->panning) {
        _canvas_pan(ctrl);
    }
}

// Zoom about the cursor: the world point under it stays under it.
void canvas_on_scroll(CanvasController* ctrl, double xoffset, double yoffset) {
    (void)xoffset;
    if (!ctrl || !ctrl->engine || !ctrl->engine->camera) {
        return;
    }
    Engine* engine = ctrl->engine;
    Camera* camera = engine->camera;
    float h = camera->ortho_height;
    double fb_x = 0.0, fb_y = 0.0;
    if (h <= 0.0f || !engine_cursor_fb(engine, &fb_x, &fb_y)) {
        return;
    }
    vec3 anchor = GLM_VEC3_ZERO_INIT;
    canvas_world_under_cursor(engine, fb_x, fb_y, anchor);

    // Each bound applies on its own, so a range with one end set is one-sided.
    float h_new = h * powf(ctrl->zoom_step, (float)yoffset);
    if (ctrl->zoom_min > 0.0f) {
        h_new = fmaxf(h_new, ctrl->zoom_min);
    }
    if (ctrl->zoom_max > 0.0f) {
        h_new = fminf(h_new, ctrl->zoom_max);
    }
    float k = 1.0f - h_new / h;

    vec3 shift = {(anchor[0] - camera->look_at[0]) * k, (anchor[1] - camera->look_at[1]) * k, 0.0f};
    camera_translate(camera, shift);
    camera->ortho_height = h_new;
    camera_rig_set_pose(ctrl->rig, camera->position, camera->look_at);
}

/*
 * Light Rigs
 */

void scene_add_three_point_lights(Scene* scene, float intensity_scale) {
    if (!scene || !scene->root_node) {
        fprintf(stderr, "scene_add_three_point_lights: invalid scene\n");
        return;
    }

    // Key from front-right above, fill softer from front-left, rim from behind
    // and above for edge definition.
    const LightDesc rig[] = {
        {.name = "key_light",
         .direction = {-0.4f, -0.7f, -0.6f},
         .color = {1.0f, 0.95f, 0.9f},
         .intensity = 3.0f * intensity_scale},
        {.name = "fill_light",
         .direction = {0.5f, -0.4f, -0.5f},
         .color = {0.8f, 0.85f, 1.0f},
         .intensity = 1.5f * intensity_scale},
        {.name = "rim_light",
         .direction = {0.0f, -0.6f, 0.8f},
         .intensity = 2.0f * intensity_scale},
    };
    for (size_t i = 0; i < sizeof(rig) / sizeof(rig[0]); i++) {
        Light* light = create_light(&rig[i]);
        if (!light) {
            fprintf(stderr, "Failed to create %s.\n", rig[i].name);
            return;
        }
        scene_add_light(scene, light);
        char node_name[32];
        snprintf(node_name, sizeof(node_name), "%s_node", rig[i].name);
        SceneNode* node = create_node();
        node_set_light(node, light);
        node_set_name(node, node_name);
        node_add_child(scene->root_node, node);
    }
}

/*
 * GUI Helpers
 */

bool app_can_process_3d_input(const Engine* engine) {
    if (!engine) {
        return true;
    }
    return !engine_gui_wants_mouse();
}

/*
 * Common Callbacks
 */

void app_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}
