#ifndef _SILENT_BOAT_H_
#define _SILENT_BOAT_H_

#include <stdbool.h>
#include <cglm/cglm.h>

#include "cetra/engine.h"
#include "cetra/game/audio.h"
#include "cetra/game/game.h"
#include "cetra/scene.h"
#include "cetra/water.h"

#include "hud.h"
#include "kit.h"
#include "player.h"
#include "pull_cord.h"

/*
 * The motorboat on the lake (spec 13.56): the user's wooden rowboat with an old outboard on its
 * transom (tools/make_boat_blender.py), moored off the dock's end under the dock lamp.
 *
 * E from the dock gets in, onto the stern seat beside the tiller. E starts the motor by its pull
 * cord (pull_cord.h); running, W and S work the throttle, A and D the tiller, and E stops it. The
 * first time on a trip that the boat reaches the spot circled on the cabin's lake map the motor
 * dies, and has to be started again. Alongside the dock RMB gets out, and the boat is brought
 * round onto its mooring and tied.
 *
 * The boat moves in fixed steps in plan -- where it is, which way it points, how fast it goes and
 * how it slides -- and is drawn between the last two with a swell on it. It bumps off the bodies
 * that stop a wader and off the dock, analytically, against its rim in plan (boat_shape.h). It
 * keeps the lake's water out of its hull with the water's dry shapes, and is no capture's, since
 * it moves.
 */

typedef enum BoatMotor {
    BOAT_MOTOR_OFF,
    BOAT_MOTOR_PULLING, // the cord being pulled to start it
    BOAT_MOTOR_RUNNING,
    BOAT_MOTOR_DYING, // stumbling to a stop
} BoatMotor;

typedef struct BoatSounds {
    AudioSystem* audio;
    Sound* run;  // the motor, a loop re-pitched by how fast it turns
    Sound* slap; // water against the hull, a loop louder as the boat goes faster
    Sound* pull; // and one-shots
    Sound* cough;
    Sound* caught;
    Sound* dies;
    Sound* stop;
    Sound* knock;
} BoatSounds;

typedef struct Boat {
    Scene* scene;
    Water* water;    // the lake's, which its dry shapes keep out of the hull; NULL for none
    SceneNode* node; // the boat, on the graph
    SceneNode* motor;
    SceneNode* prop;
    SceneNode* cord;
    SceneNode* rope;
    mat4 motor_rest, prop_rest, cord_rest, rope_rest; // as the file has them
    vec3 seat;                                        // the seated eye in the boat's frame
    vec3 prop_at;        // the propeller's hub in the boat's frame, where the wake leaves it
    vec3 motor_at;       // the motor's swivel in the boat's frame, where its sound comes from
    SceneNode* tie;      // the rope to the dock's post, on the graph while moored
    SceneNode* upturned; // its twin turned over on the bank
    // In plan each fixed step, and the step before, the frame drawn between them: the middle's x
    // and z, and the heading, radians about +Y with the bow toward (sin, 0, cos).
    float x, z, heading;
    float prev_x, prev_z, prev_heading;
    float speed;    // m/s along the bow
    float slip;     // m/s across it, to the left
    float yaw_rate; // rad/s, to the left
    float throttle; // -1..1, eased to what is asked
    float tiller;   // -1..1, eased: positive turns the bow left
    float rpm;      // 0 stopped .. 1 flat out
    double clock;   // seconds of the boat's own, for its swell
    BoatMotor motor_state;
    float motor_time; // seconds in this state
    float dying_from; // the rpm a stumble began at
    bool moored;      // tied at the dock, where it does not move
    bool docking;     // being brought round onto its mooring
    bool riding;      // the player in it
    bool cut_armed;   // the motor dies at the marked spot this trip
    bool cut_out;     // it just died there: thought about once the motor is still
    bool knocked;     // it just bumped something, heard in the frame
    bool told;        // the controls have been hinted
    float knock_speed;
    float cord_yank; // seconds since the handle was last pulled
    float prop_angle;
    float drawn_heading; // the heading drawn last frame, so the view turns with the boat
    WaterWake wake;
    PullCord pull;
    BoatSounds sounds;
    bool trace; // print where it is and how it goes every half second of steps
} Boat;

// The boat moored off the dock's end and its twin on the bank, both drawn to `reach` of the eye,
// 0 for always; the mooring rope in a kit beside `lake_kit`. False, said by name, when the model
// cannot be loaded: the lake has no boat.
bool boat_build(Boat* b, Engine* engine, Scene* scene, Water* water, Kit* lake_kit, float reach);
// Its motor's, its hull's and its bumps' sounds, decoded in the audio's loading phase.
void boat_start_audio(Boat* b, AudioSystem* audio);

// How far the player's eye is from the boat when they can get in -- on the dock, the moored boat
// near and looked at -- and FLT_MAX when they cannot.
float boat_reach(const Boat* b, const Player* p);
// Into the boat from the dock: the view eases to the seat.
void boat_board(Boat* b, Player* p, Hud* hud);
// Whether the player is in the boat.
bool boat_riding(const Boat* b);
// What E and RMB do in the boat now, for the prompt; NULL while riding with nothing to do, and
// while not riding.
const char* boat_prompt(const Boat* b);

// Fixed step: the throttle, the tiller and the motor; the boat moved and bumped off what it meets,
// and the rider carried in it.
void boat_step(Boat* b, Game* game, Player* p, float dt);
// Per frame, before player_pre_render: E and RMB in the boat, the boat drawn between its last two
// steps with its swell, its motor turned, its propeller spun, the cord, the dry shapes, the
// rider's seat, the wake and the sounds; `time` and `dt` the clock things are drawn by.
void boat_frame(Boat* b, Game* game, Player* p, Hud* hud, double time, float dt);

// The boat at (x, z) facing `heading`, its motor off: moored if that is its mooring, else on the
// water, with the player in it when `riding` (spec 13.56's saves and start flags).
void boat_place(Boat* b, Player* p, float x, float z, float heading, bool riding);
// The motor running at once, for a start that wants it so.
void boat_start_motor(Boat* b);
// Its pull cord up, for a picture of it.
void boat_open_cord(Boat* b, Player* p, Hud* hud);

void boat_free(Boat* b);

#endif // _SILENT_BOAT_H_
