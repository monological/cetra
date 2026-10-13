#ifndef _SILENT_BASEMENT_H_
#define _SILENT_BASEMENT_H_

#include "cetra/engine.h"
#include "cetra/light.h"
#include "cetra/scene.h"
#include "cetra/game/audio.h"
#include "cetra/game/entity.h"
#include "cetra/game/physics.h"

#include "door.h"
#include "kit.h"

/*
 * The basement under the player's house (spec 13.31): the whole footprint dug out below the
 * ground floor, its walls the house's foundation, its ceiling the floor's own framing on a beam
 * down the middle, reached by a steep flight from the door at the hall's end. Where it is, is
 * home.h's.
 */

// The bulb's light: where it hangs, its supply's level (1 is full), whether its cached shadow
// follows it, a filter over its colour (white is its own) and how many times its own light it
// gives.
typedef struct BulbState {
    vec3 at;
    float level;
    bool follow;
    vec3 tint;
    float gain;
} BulbState;

/*
 * The bare bulb hanging in the stairwell, which is what the door opens on: lit, set swinging by
 * the draft the first time the door opens, and now and then dimming on a failing supply. And the
 * tap over the laundry tub, dripping into the water standing in it.
 */
typedef struct Basement {
    SceneNode* bulb; // the cord, the socket and the glass, turned about the rose
    Material* glass; // its own, driven with the light
    Light* light;    // cached, its shadow following it while the swing is wide
    double drafted;  // when the door first opened, or -1 before
    float gust;      // that draft's swing over the door's own: 1, or basement_swing's
    unsigned int seed;
    // From the next update: the share of its supply the bulb gets, 1 by default, never quite 0 or
    // the light leaves its cached shadow undrawn; a filter over its colour, white by default; and
    // how many times its own light it gives, 1 by default. What the haunting (spec 13.52) drives.
    float mains;
    vec3 tint;
    float gain;
    BulbState live; // as the last update left the light; hanging still on a full supply before

    AudioSystem* audio;
    Sound* drip; // decoded once and played as voices; NULL without audio
    double next_drip;
    KitRng drips; // the gaps between them

    Entity* bar;  // across the way in at the flight's foot while the player has no light; or NULL
    bool at_foot; // the player has come down to the foot since last up in the hall
} Basement;

// The foundation, the slab, the framing overhead, the beam and its posts, the stair and its
// walls, what is kept down there and the damp standing on its floor, placed from `seed`, and the
// bulb's rose on the ceiling.
void basement_build(Kit* kit, unsigned int seed);

// The bulb, a node of its own with its light, and the drip from `audio`, which may be NULL.
void basement_start(Basement* b, Engine* engine, Scene* scene, AudioSystem* audio,
                    unsigned int seed);

// Per frame, before the frame draws: the swing, set going the first time `door` opens (NULL when
// it was not hung), the dimming, and the drip.
void basement_update(Basement* b, const Door* door, double time);

/*
 * The basement is dark past the bulb's reach at the flight's foot, and nobody goes into it
 * without a light (spec 13.40): the way in from the foot is barred by a body nobody sees while
 * the player has no `light`, and open once they have one. Per fixed step, with where the
 * player's `feet` are: true on the step they come down to the foot -- its last steps or the floor
 * below them -- without a light, once each time they come down from the hall.
 */
bool basement_hold(Basement* b, EntityManager* em, PhysicsWorld* physics, const vec3 feet,
                   bool light);
// Feet put somewhere rather than walked there (spec 13.49): already at the foot if they are there,
// so arriving there is not what the next step thinks about; the clock put at `time`, which the
// drip is timed again from (spec 13.50); and the draft's swing the door's own again.
void basement_settle(Basement* b, const vec3 feet, double time);

// For the light's captures (spec 13.42): `rest` hangs the bulb straight on a full supply in its
// own colour; false puts it back as the last update left it.
void basement_rest(Basement* b, bool rest);

// The bulb set swinging from `time` as a draft does, `scale` times as wide and dying away `scale`
// times as slowly (spec 13.52): what a door flung open does.
void basement_swing(Basement* b, double time, float scale);

#endif // _SILENT_BASEMENT_H_
