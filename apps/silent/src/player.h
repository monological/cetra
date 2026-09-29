#ifndef _SILENT_PLAYER_H_
#define _SILENT_PLAYER_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/camera_rig.h"
#include "cetra/game/entity.h"
#include "cetra/game/game.h"
#include "cetra/game/input.h"
#include "cetra/game/physics.h"

/*
 * A first-person walker: a Jolt CharacterVirtual capsule, and a camera rig at
 * distance zero on its head. apps/forest's walker, cut down to what a person
 * creeping through a house does: walk, sprint, look. No jump.
 *
 * Movement is read in the camera's frame (the rig steers the controls). The
 * mouse is captured on a click and released on Tab; the arrows and the right
 * stick look too, so a headless or pad run reaches every control.
 *
 * It reads the actions "move_x", "move_y", "sprint", "look_x", "look_y" and
 * "release_cursor" by name, from whatever table the app binds.
 */

#define PLAYER_EYE_HEIGHT 1.62f // feet to eye, metres

typedef struct Player {
    Entity* entity;
    CameraRig* rig;
    bool cursor_captured;
    bool skip_first_delta; // the first delta after a capture is a jump, not a movement
} Player;

// Stands the capsule on the floor at `feet`, looking along `yaw` (radians,
// the rig's convention), and installs the rig on the engine.
void player_init(Player* p, Game* game, PhysicsWorld* physics, EntityManager* em, const vec3 feet,
                 float yaw);

// Fixed step: turns the move actions into the capsule's velocity.
void player_update(Player* p, Game* game, double dt);

// Per rendered frame: the look, the cursor, and the rig onto the head. Under
// a pin, the camera is held at eye/target instead.
void player_pre_render(Player* p, Game* game, const vec3* pin_eye, const vec3* pin_target);

// Where the eye is this frame, and which way it looks.
void player_eye(const Player* p, vec3 eye, vec3 forward);

#endif // _SILENT_PLAYER_H_
