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
 * SCALE. The one object in this app with a known real-world size is the grass: blades are
 * authored at 5.5 units and grass stands about 0.25 m, which puts the world near 22 units per
 * metre -- and squares with the tree, 250 units for a mature tree's 11 m. That fixes the eye
 * height at 1.7 m, and it is worth keeping physical because eye height is what decides how the
 * world LOOKS: how far down the grass is, where the waterline crosses you.
 */
#define PLAYER_EYE_HEIGHT 37.0f

/*
 * SPEED is NOT physical, and pretending otherwise was the mistake. At 22 units/m a real 1.4 m/s
 * walk is 31 units/s, which is 10 seconds from the trunk to the waterline and a minute and a
 * half out to the seabed's edge -- correct, and far too slow to look around with.
 *
 * So this is a game speed chosen from TRAVERSAL TIME, which is the thing that actually decides
 * how the mode feels: 80 units/s puts the shore about 4 seconds away and the far seabed about
 * 35, with sprint cutting both to under half. It reads as roughly a jog at this scale.
 * --walk-speed overrides it, because feel is not something to settle by rebuild.
 */
#define PLAYER_WALK_SPEED  80.0f
#define PLAYER_SPRINT_MULT 2.5f
// 9.81 m/s^2 at 22 units/m. Jump apex is (v^2)/(2g) -- 100 units/s clears about 23, which is
// half a body height, i.e. a jump rather than a leap.
#define PLAYER_GRAVITY    216.0f
#define PLAYER_JUMP_SPEED 100.0f

typedef struct Player {
    vec3 feet; // world position of the soles; the camera sits PLAYER_EYE_HEIGHT above
    /*
     * Tunables. player_init seeds them and the caller overwrites what a flag asked for, rather
     * than each one becoming another init parameter -- they are plain data with no invariant
     * between them, and the alternative was a six-argument constructor.
     */
    // Units per second on the flat, before the sprint multiplier.
    float walk_speed;
    // true = the up arrow looks DOWN. Default, and it applies to the ARROWS ONLY: the mouse is
    // a direct pointing device where inversion is a minority taste, while an arrow key is a
    // pitch lever and up-is-down is the older convention for those.
    bool invert_arrow_pitch;
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

// Spawn standing on the ground at (x, z), looking at `yaw`. Captures the cursor and seeds the
// tunables above to their defaults; overwrite them afterwards to change them.
void player_init(Player* p, struct Engine* engine, float x, float z, float yaw);

// Advance one frame and write the result to the engine camera. `dt` is the frame delta.
void player_update(Player* p, struct Engine* engine, float dt);

// Tab toggles cursor capture. Returns true if the key was consumed.
bool player_on_key(Player* p, struct Engine* engine, int key, int action);

#endif // _PLAYER_H_
