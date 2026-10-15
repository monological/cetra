#ifndef _SILENT_PLAYER_H_
#define _SILENT_PLAYER_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/camera_rig.h"
#include "cetra/gait.h"
#include "cetra/game/entity.h"
#include "cetra/game/game.h"
#include "cetra/game/input.h"
#include "cetra/game/physics.h"

/*
 * A first-person walker: a Jolt CharacterVirtual capsule, and a camera rig at
 * distance zero on its head. apps/forest's walker, cut down to what a person
 * creeping through a house does: walk, sprint, crouch, look. No jump.
 *
 * Movement is read in the camera's frame (the rig steers the controls), and ramps up and down
 * rather than starting and stopping dead. The body moves in fixed steps and is drawn between the
 * last two, so the eye moves every frame however the frames fall against the steps. A crouch
 * (spec 13.48) shortens the capsule from the top and eases the eye down after it; it stands again
 * only where there is room over the head, and sprinting asks it to. The arrows and the right stick
 * look as well as the mouse, so a headless or pad run reaches every control.
 *
 * It owns the window's cursor. In play the mouse looks: in exclusive fullscreen at once, in a
 * window once a click takes it. While the game's input is held -- a screen over the game, the
 * pause menu, the title -- the cursor is shown only while the mouse is what drives the screen, and
 * hidden from the moment a key or a pad does; under the loading screen it is hidden.
 *
 * It reads the actions "move_x", "move_y", "sprint", "crouch" (held), "crouch_toggle", "look_x"
 * and "look_y" by name, from whatever table the app binds, and the screens' "menu_up",
 * "menu_down", "menu_left", "menu_right", "menu_accept", "menu_back", "map_pan_x", "map_pan_y"
 * and "map_zoom".
 */

#define PLAYER_EYE_HEIGHT 1.62f // feet to eye standing, metres

typedef struct Player {
    Entity* entity;
    CameraRig* rig;
    bool cursor_wanted;    // in a window, the player's own choice: a click takes the cursor
    int cursor_mode;       // the window's GLFW_CURSOR_* mode; 0 until the first frame sets one
    bool cursor_captured;  // the mode is GLFW_CURSOR_DISABLED: the mouse looks
    bool pointing;         // the mouse, not a key or a pad, drives the screen over the game
    bool skip_first_delta; // the first delta after the mode changes is a jump, not a movement
    vec3 step_from;      // where the capsule was when this fixed step began: the last step's result
    float speed;         // the horizontal speed asked of the body, m/s, as it ramps
    float heading;       // and the way it goes, radians about +Y, facing (sin, 0, cos)
    vec3 view;           // where the capsule is drawn this frame, between the last two steps
    vec3 reach_from;     // the eye a hand reaches from: the body's, which no lag or bob moves
    bool interpolate;    // draw the body between steps; false draws it at the last
    Gait* gait;          // the stride, the footfalls, the eye's lag and the pace on a climb
    bool use_gait;       // the gait moves the eye and paces a climb
    bool crouch_toggled; // the toggle asks for a crouch; the held key asks besides
    float crouch_eased;  // 0..1, how far the eye has gone down toward a crouch's
    // Both hands hold something (spec 13.55): the body stops with its stance kept, nothing turns
    // the view, which what is held poses, and the cursor stays captured, where a screen holding the
    // game's input would free it.
    bool holding;
    bool trace; // print every step's move and every frame's eye
} Player;

// Stands the capsule on the floor at `feet`, looking along `yaw` and `pitch` (radians, the rig's
// convention: pitch up positive), and installs the rig on the engine.
void player_init(Player* p, Game* game, PhysicsWorld* physics, EntityManager* em, const vec3 feet,
                 float yaw, float pitch);

// Frees what player_init made; the entity and the rig are the game's and the engine's.
void player_free(Player* p);

// The body where its controller now stands, looking along its rig's yaw and pitch as just written
// over (spec 13.49), still, its gait begun again, and the cursor not wanted until a click. A
// crouch is not saved: it stands where there is room over its head, and stays crouched where there
// is none until it walks out.
void player_restore(Player* p);

// Fixed step: turns the move actions into the capsule's velocity.
void player_update(Player* p, Game* game, double dt);

// Per rendered frame: the body drawn between the last two steps, the gait, the look, the cursor,
// and the rig onto the head. Under a pin, the camera is held at eye/target instead.
void player_pre_render(Player* p, Game* game, const vec3* pin_eye, const vec3* pin_target);

// Where the eye is this frame, and which way it looks.
void player_eye(const Player* p, vec3 eye, vec3 forward);

// How high the body's eye is over its feet, metres: lower as it crouches, and not moved by the
// stride or a pinned camera.
float player_eye_height(const Player* p);

// Whether the body is crouched: its capsule a crouch's, which it stays until there is room to
// stand.
bool player_crouched(const Player* p);

// How far the eye is from `at` when a hand can go to it -- near enough and looked at -- and
// FLT_MAX when it cannot.
float player_reach_distance(const Player* p, const vec3 at);

// Where the body's feet are, and how fast it moved over the last step: the capsule's own, which
// a camera pinned elsewhere does not move.
void player_feet(const Player* p, vec3 out);
void player_velocity(const Player* p, vec3 out);
// Where the feet are drawn this frame, between the last two steps: for what is drawn or heard
// each frame, where player_feet is for what the simulation decides.
void player_view_feet(const Player* p, vec3 out);

#endif // _SILENT_PLAYER_H_
