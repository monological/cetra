#ifndef _PLAYER_H_
#define _PLAYER_H_

#include <stdbool.h>
#include <cglm/cglm.h>

struct Engine;

/*
 * First-person walker on the island (--player).
 *
 * KEYBOARD ONLY, by choice. WASD moves and the arrow keys turn the head; the mouse does not
 * touch the camera at all, and the cursor is never captured. That means the GUI sliders this
 * app is built around stay clickable at all times with no mode to toggle -- and it removes a
 * whole failure class with them: a mouse-look delta is a difference of two ABSOLUTE cursor
 * readings, so anything that moves the cursor other than a hand (a warp, a focus round trip)
 * arrives as camera motion, and a persistent one is a spin with no way out but quitting.
 * Nothing here reads the cursor, so nothing can do that.
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
 * 1.7 m at this world's scale, which `GROUND_UNITS_PER_METRE` states and derives. Worth
 * keeping physical because eye height is what decides how the world LOOKS: how far down the
 * grass is, where the waterline crosses you.
 *
 * Not written as the product. 1.7 x 22 is 37.4, and rounding it here rather than there keeps
 * every frame ever captured in this mode comparable.
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
 */
#define PLAYER_WALK_SPEED  80.0f
#define PLAYER_SPRINT_MULT 2.5f
// 9.81 m/s^2 at 22 units/m. Jump apex is (v^2)/(2g) -- 100 units/s clears about 23, which is
// half a body height, i.e. a jump rather than a leap.
#define PLAYER_GRAVITY    216.0f
#define PLAYER_JUMP_SPEED 100.0f
/*
 * Arrow-key look, radians per SECOND. A rate, not a displacement, so it is scaled by the frame
 * delta -- otherwise the turn speed is whatever the frame rate is.
 *
 * Slower than it looks like it should be. A key cannot modulate its own rate the way a hand on
 * a mouse can, so it is stuck at one speed and that speed has to be comfortable for a whole
 * turn rather than quick for a glance. 1.0 rad/s is a bit under 60 degrees a second.
 */
#define PLAYER_LOOK_RATE 1.0f

typedef struct Player {
    vec3 feet;   // world position of the soles; the camera sits PLAYER_EYE_HEIGHT above
    float yaw;   // radians, 0 = -Z
    float pitch; // radians, clamped short of straight up/down
    float vertical_velocity;
    bool grounded;

    /*
     * Tunables. player_init seeds them and the caller overwrites what a flag asked for, rather
     * than each one becoming another init parameter -- they are plain data with no invariant
     * between them, and the alternative was a six-argument constructor.
     */
    float walk_speed; // units/s on the flat, before the sprint multiplier
    float look_rate;  // radians/s of head turn
    // true = the up arrow looks DOWN. Default. PITCH only: yaw is never inverted, because left
    // means left in every convention there is.
    bool invert_pitch;
} Player;

// Spawn standing on the ground at (x, z), looking at `yaw`, with the tunables above at their
// defaults; overwrite them afterwards to change them.
void player_init(Player* p, struct Engine* engine, float x, float z, float yaw);

// Advance one frame and write the result to the engine camera. `dt` is the frame delta.
void player_update(Player* p, struct Engine* engine, float dt);

#endif // _PLAYER_H_
