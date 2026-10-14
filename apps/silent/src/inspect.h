#ifndef _SILENT_INSPECT_H_
#define _SILENT_INSPECT_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/camera_rig.h"
#include "cetra/game/game.h"

#include "arms.h"
#include "birdcage.h"
#include "house_key.h"
#include "hud.h"
#include "lights.h"
#include "player.h"

/*
 * The birdcage picked up and looked at (spec 13.55): the view settles to the pickup's framing,
 * the arms come up and lift the cage, and the player turns it on the palm while two thoughts
 * pass; E opens its face, E again takes the key, and the hands set it down on the bench and drop
 * away. A put-down sets it back shut.
 *
 * Through all of it the view is the arms' own: a camera rig of this module's, posed each frame
 * from the clip, glided into from the player's and back. The flashlight is lit and held on the
 * cage. The player is held still, and their look is kept where the inspection left it.
 */

typedef enum {
    INSPECT_NONE,      // nothing held
    INSPECT_SETTLING,  // the view going to the pickup's framing, the beam onto the cage
    INSPECT_RAISING,   // the reach, the grip, the lift and the look
    INSPECT_HOLDING,   // turning it in the hands
    INSPECT_OPENING,   // the face swung open
    INSPECT_OPEN,      // the hand waiting before the open door
    INSPECT_TAKING,    // the key pinched, drawn out and put away
    INSPECT_LOWERING,  // set back on the bench
    INSPECT_RETURNING, // the view going back to the player's own
} InspectState;

// The key into the backpack, its thought and the haunting, at sim time `time`.
typedef void (*InspectTakeFn)(double time);

typedef struct Inspect {
    InspectState state;
    float t; // seconds in the state
    Engine* engine;
    Arms* arms;
    Birdcage* cage;
    HouseKey* key;
    Lights* lights;
    Hud* hud;
    Player* player;
    InspectTakeFn take;
    CameraRig* rig; // the view while it lasts, this module's
    mat4 body;      // the pickup's framing in the world: the standing eye before the cage
    vec3 from_eye;  // the view a glide starts from
    vec3 from_dir;
    float spin;      // the player's turn of the cage on the palm, radians
    float spin_from; // what it was as it began to be turned back
    float door;      // the door's angle, radians
    float beat;      // seconds since the look, below 0 before it
    int thoughts;    // the beat's lines had so far
    bool carried;    // the key is in the pinch
    double time;     // the sim time of this frame, for the take
    bool trace;      // print every state and event
} Inspect;

// Nothing held. All but the rig are borrowed.
void inspect_init(Inspect* in, Engine* engine, Arms* arms, Birdcage* cage, HouseKey* key,
                  Lights* lights, Hud* hud, Player* player, InspectTakeFn take);
void inspect_free(Inspect* in);

// The cage picked up from the bench: the view starts settling onto it.
void inspect_pick_up(Inspect* in);
// Already holding it, for a picture: shut and being turned, or open with the hand waiting.
void inspect_start_held(Inspect* in, bool open);
// Per rendered frame, after the player's own and before anything reads the view or the beam.
void inspect_frame(Inspect* in, Game* game);
// Whether something is held, or the view is still going to or from it.
bool inspect_active(const Inspect* in);
// What the action keys would do now, or NULL.
const char* inspect_prompt(const Inspect* in);
// Everything put back at once, as a load or a return to the title needs: the cage on the bench,
// open exactly when the key is had and the key on its hook until then, no arms, and the view, the
// beam and the player given back.
void inspect_drop(Inspect* in, bool key_had);

#endif // _SILENT_INSPECT_H_
