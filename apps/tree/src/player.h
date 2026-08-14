#ifndef _PLAYER_H_
#define _PLAYER_H_

#include <stdbool.h>
#include <cglm/cglm.h>

struct Engine;

/*
 * First-person walker on the island (--player).
 *
 * WHY NOT THE GAME FRAMEWORK. `cetra/src/game/` exists for exactly this and `apps/forest`
 * uses it -- a Jolt CharacterVirtual on a triangle-mesh collider. It is the wrong trade here:
 * this island's surface is `ground_height_at`, a closed form, so a mesh collider would be a
 * strictly WORSE approximation of a surface we can evaluate exactly, and buying it would mean
 * restructuring an engine-loop app with ImGui sliders onto run_game. Standing on the analytic
 * surface is both simpler and more accurate. forest remains the physics-character demo.
 *
 * What that costs, stated rather than discovered later: no collision with the trunk, so you
 * can walk through it; and no buoyancy or swimming, which is an explicit non-goal of spec
 * 11.32 -- walking off the island walks you DOWN the flank and under the surface on foot.
 * The submerged look needs nothing here: the froxel volume already becomes a second medium
 * when the camera goes under (11.33 phase 2).
 */

/*
 * SCALE, anchored rather than picked. The one object in this app with a known real-world size
 * is the grass: blades are authored at 5.5 units and grass stands about 0.25 m, which puts the
 * world at roughly 22 units per metre -- and squares with the tree, 250 units for something a
 * mature tree's 11 m. So a 1.7 m eye is 37 units and a 1.4 m/s walk is 31 units/s.
 */
#define PLAYER_EYE_HEIGHT  37.0f
#define PLAYER_WALK_SPEED  31.0f
#define PLAYER_SPRINT_MULT 2.6f
// 9.81 m/s^2 at 22 units/m. Jump apex is (v^2)/(2g) -- 100 units/s clears about 23, which is
// half a body height, i.e. a jump rather than a leap.
#define PLAYER_GRAVITY    216.0f
#define PLAYER_JUMP_SPEED 100.0f

typedef struct Player {
    vec3 feet; // world position of the soles; the camera sits PLAYER_EYE_HEIGHT above
    // Where the head is pointing. Driven by the mouse and by the arrow keys, additively: one is
    // a displacement per pixel and the other a rate per second, which is why only the second is
    // scaled by the frame delta.
    float yaw;   // radians, 0 = -Z
    float pitch; // radians, clamped short of straight up/down
    float vertical_velocity;
    bool grounded;
    // Mouse look is only live while the cursor is captured, so the ImGui sliders this app is
    // built around stay reachable. Toggled with Tab.
    bool mouse_captured;
    bool warp_pending;  // skip one frame's delta after a capture, or the view snaps
    bool look_was_live; // so a focus regain re-seeds too, not only a capture
    double last_mouse_x, last_mouse_y;
} Player;

// Spawn standing on the ground at (x, z), looking at `yaw`. Captures the cursor.
void player_init(Player* p, struct Engine* engine, float x, float z, float yaw);

// Advance one frame and write the result to the engine camera. `dt` is the frame delta.
void player_update(Player* p, struct Engine* engine, float dt);

// Tab toggles cursor capture. Returns true if the key was consumed.
bool player_on_key(Player* p, struct Engine* engine, int key, int action);

#endif // _PLAYER_H_
