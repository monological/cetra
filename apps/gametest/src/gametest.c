// Game Test - Demonstrates the CharacterController integration
//
// Press WASD to move the player
// Press Space to jump (only when grounded)
// Press F to spawn a falling box
// Press P to pause/unpause
// Press G to print ground state
// Press R to raycast downward
// Press Escape for the pause menu

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>

#include "cetra/internal/async_loader.h"
#include "cetra/internal/ragdoll.h"
#include "cetra/internal/ragdoll_jolt.h"
#include "cetra/internal/rigging.h"
#include "cetra/common.h"
#include "cetra/mesh.h"
#include "cetra/scene.h"
#include "cetra/engine.h"
#include "cetra/geometry.h"
#include "cetra/ik.h"
#include "cetra/light.h"
#include "cetra/app.h"
#include "cetra/program.h"
#include "cetra/text.h"
#include "cetra/ui.h"
#include "ui_backdrop.h"
#include "cetra/game/game.h"
#include "cetra/game/entity.h"
#include "cetra/game/physics.h"
#include "cetra/game/character.h"
#include "cetra/game/audio.h"
#include "cetra/game/settings.h"
#include "cetra/game/save.h"
#include "cetra/game/animator_component.h"
#include "cetra/camera_rig.h"
#include "cetra/animator.h"
#include "cetra/import.h"
#include "cetra/ibl.h"
#include "cetra/sky.h"
#include "cetra/water.h"
#include "cetra/procedural/terrain.h"
#include "cetra/procedural/erosion.h"
#include "cetra/procedural/terrain_tex.h"
#include "cetra/cook.h"
#include "cetra/particle_system.h"
#include "cetra/particle_emitter.h"
#include "cetra/particle_module.h"
#include "cetra/particle_renderer.h"
#include "cetra/particle_sim.h"
#include "cetra/texture.h"

static MouseDragController* drag_controller = NULL;
static Entity* player_entity = NULL;
static Entity* door_entity = NULL;
static Constraint* door_hinge = NULL;
static ShaderProgram* pbr_shader = NULL;
static int box_count = 0;
static const char* hdr_path = NULL;
static SaveSystem* save_system = NULL;
/*
 * Edges taken once per FRAME and consumed by the next fixed step.
 *
 * Input is polled once a frame, outside the step loop, so every step of a
 * multi-step frame reads the same press -- one F5 on a frame that ran five
 * steps meant five whole saves, each with its own fsync. The flag is what makes
 * the edge happen once while the WORK still happens in the settled world a
 * fixed step is.
 */
static bool save_pending = false;
static bool load_pending = false;

// Animation (spec 12.1): the player is the procedural puppet on an ANIMATOR
// component -- idle, walk and run blended from the controller's post-solve
// speed, a jump one-shot, a wave on the masked override layer, footsteps from
// the clips' events. --no-puppet keeps the red box; --twin <clip> stands a
// second rig beside the player playing its own clip, which is the per-node
// pose seen from a game.
// The committed walk cycle: a real one, with a stance phase. The generated gait() cannot
// stand in for it -- that swings thighs about a straight leg, so the foot traces an arc and
// never dwells, and a contact phase is the thing locking holds.
#define WALK_CLIP "assets/models/strut_walk.fbx"
// The rig the shared set below was authored on, and the reason a rig that rests
// differently can wear those clips at all (spec 12.11). The loader derives its
// correction from the difference between two rest poses, so with only one of them it
// has nothing to reconcile and writes the clip's raw local rotations into bones whose
// rest frame is nothing like the source's -- which is not a lean but a collapse. The
// clips are exported without skin, so none of them carries the rig itself.
#define SHARED_CLIP_RIG "assets/models/t_pose.fbx"
// The clips a rig with no locomotion of its own is given, all on the `cetra_rig:` names.
// Each is looked up afterwards by its FILE STEM, which is what import.c names a clip
// holding one animation -- so the name to ask for is the basename here and never
// whatever take name the file was authored with.
static const char* const SHARED_CLIPS[] = {
    WALK_CLIP,
    "assets/models/steady_run.fbx",
    "assets/models/quiet_idle.fbx",
    "assets/models/swim_cycle.fbx",
    "assets/models/float_idle.fbx",
    "assets/models/fall_cycle.fbx",
    "assets/models/touch_down.fbx",
    "assets/models/jump_start.fbx",
};

// What full stick is worth when the clips say nothing about it -- the fallback, not the
// policy. When the locomotion clips can be measured (spec 12.10) `player_speed` is DERIVED
// from the fastest of them instead, so travel and animation cannot disagree by a factor of
// ten the way they did for the whole of 12.9.
#define PLAYER_SPEED 10.0f
// What the player actually walks at: the derived figure, PLAYER_SPEED where nothing could
// be measured, or whatever --speed says over either.
static float player_speed = PLAYER_SPEED;
// How far a clip may be stretched to meet the ground before the stretching is worse than
// the sliding. Outside this band the honest answer is that this clip cannot carry that
// speed, which is what the blend space's other entries are for.
#define ANIM_RATE_MIN 0.6f
#define ANIM_RATE_MAX 1.6f
// How big the player is. The VISUAL and the collision capsule both come from
// this: scaling one alone either sinks the feet through the floor or leaves the
// character standing on nothing.
#define PLAYER_SCALE  2.0f
#define PLAYER_RADIUS (0.5f * PLAYER_SCALE)
#define PLAYER_HALF_H (0.5f * PLAYER_SCALE)
// Capsule centre to feet, derived rather than written down twice. The yaw
// rebuilds the rig transform every step, so both sites read this.
#define PLAYER_RIG_DROP (-(PLAYER_RADIUS + PLAYER_HALF_H))
static bool no_puppet = false;
static const char* puppet_path = "assets/models/puppet.gltf";
static const char* twin_clip = NULL;
static SceneNode* player_rig = NULL; // the puppet under the entity's node: drop + yaw
static Animator* player_animator = NULL;

// Whether physics owns the player rather than the clips. Everything that STEERS is
// gated on it, and that is not thrift: with the controller disabled its velocity and
// its grounded bit are frozen at the instant of death, so a locomotion axis reading
// them holds one gear forever and the foot planter, whose weight eases toward that
// frozen bit, pins at 1 and keeps forming ground contacts under a body that is falling.
static bool player_ragdolled(void) {
    return player_animator && ragdoll_active(player_animator->state->ragdoll);
}

static AnimatorEntry locomotion[3];
static int locomotion_count = 3;
// What the locomotion knob MEANS, which is the one question three different answers
// hang off: the axis units, whether playback is rate-scaled, and which way round the app
// and the animation are. One value rather than two booleans, because two of the four
// combinations they spell cannot happen -- and the pair shipped with the root path
// setting both and the second one unreadable underneath the first.
typedef enum LocomotionAxis {
    LOCO_FRACTION, // no clip could be measured: the knob is a fraction of PLAYER_SPEED
    LOCO_STRIDE,   // the entries sit at the speeds their FEET imply (spec 12.10)
    LOCO_TRAVEL,   // the entries sit at the speeds they STATE, and carry the body (12.18)
} LocomotionAxis;
static LocomotionAxis locomotion_axis = LOCO_FRACTION;
// The chaser's own entries, and it needs its own because root motion is a property of
// the ANIMATOR while the clips are a property of an array. It is stick-driven -- nothing
// drains it -- so handing it clips that carry a displacement would leave that
// displacement in its pose, which is a body sliding a metre off its own capsule per loop
// and snapping back. It gets the clips that stay where they are.
static AnimatorEntry chaser_locomotion[3];
static bool speed_override = false;
static Animation* clip_jump = NULL;
static Animation* clip_wave = NULL;
static Animation* clip_lunge = NULL; // a one-shot that travels a stated distance
static Animation* clip_spin = NULL;  // and one that turns without travelling
static Animation* clip_swim = NULL;
static Animation* clip_float = NULL; // treading water: the swim space's standing end
static Animation* clip_fall = NULL;  // airborne, looping
static Animation* clip_land = NULL;  // the one-shot that ends a fall
// The swim space, built like the locomotion one and on the same axis: metres per second,
// entries at the speeds their clips imply.
static AnimatorEntry aquatic[2];
static int aquatic_count = 0;

// What a character is IN, which is what decides which SOURCE plays. One value rather than
// a flag per medium: the three are exclusive, and asking "did this change" once is what
// makes the crossfade fire on an EDGE. Re-issuing a play every step restarts the clip
// continuously and it never advances past its first tick.
typedef enum PlayerMedium {
    MEDIUM_GROUND = 0, // the locomotion space
    MEDIUM_WATER,      // the swim space, or the one swim clip on a rig without a float
    MEDIUM_AIR,        // falling, which before this was not handled at all: the jump
                       // one-shot ended after a second and handed the rig back to the
                       // locomotion space, so a character ran on the spot in mid-air
} PlayerMedium;
static PlayerMedium player_medium = MEDIUM_GROUND;
// HOW the ground was left, which is what picks the airborne pose: a jump rises and a step
// off a ledge does not, and the two read completely differently. Set on the press and
// cleared on the way back down, so a walk off the plate is a fall and not a leap.
static bool player_jumped = false;
static bool chaser_in_swim_clip = false;
// A file static because the flag is parsed in main and read in on_pre_render, long after.
// ON by default, with --no-follow-cam to opt out, which is the shape --no-puppet,
// --no-chaser and --no-ik already use. It was off while the camera was being settled; a
// demo whose subject is a drop into a cavern should not ship with the camera pinned to
// the world origin.
static bool follow_cam = true;

// An explicit camera pose, apps/render's --cam-eye/--cam-target shape carried over here.
//
// It exists because this app's camera is a FOLLOWER: it is derived from the player every
// frame, so there is no way to state a view, and a framing seen interactively cannot be
// photographed again -- which makes any question about what the frame looks like
// unanswerable except by the person holding the keyboard. Setting a pose stands the
// follower down, since both want to own the camera and the later writer would win
// silently.
static bool cam_eye_set = false;
static bool cam_target_set = false;
static vec3 cam_pose_eye = {0.0f, 0.0f, 0.0f};
static vec3 cam_pose_target = {0.0f, 0.0f, 0.0f};
static vec3 cam_pose_up = {0.0f, 1.0f, 0.0f};
static float cam_pose_fov_deg = 0.0f; // 0 = leave the camera's own default

static bool parse_vec3_arg(const char* s, vec3 out) {
    return s && sscanf(s, "%f,%f,%f", &out[0], &out[1], &out[2]) == 3;
}
// The camera's heading, moved by the arrow keys and by NOTHING else.
//
// Every automatic version of this was wrong, and in the same way each time: player_yaw
// follows the velocity, so a camera that chased it could never be in front of you.
// Pressing back turned the character round and the camera swung in behind -- both
// directions read as forward, and no sign change could fix it because there was no
// backward to invert. A camera that only moves when you move it has none of that.
static float cam_yaw = (float)M_PI;
// 10.2 degrees down, which with the near distance below is an authored framing rather than a
// number picked off a slider: eye 1.411 above the look point and 7.841 back from it.
static float cam_pitch = -0.178f;
// 0 = tight on the player, 1 = the wide establishing shot (spec 12.13), and the height of
// the last footing it is measured against. Seeded at the spawn height so the first frames
// do not read as a fall from the origin.
static float cam_wide = 0.0f;
static float cam_ground_y = 0.0f;
// forest's, in radians per second at full deflection, and the pitch clamped so the eye
// cannot roll under the floor or over the top.
#define LOOK_YAW_RATE   1.8f
#define LOOK_PITCH_RATE 1.2f
#define CAM_PITCH_MIN   -1.25f
#define CAM_PITCH_MAX   0.2f
static float wave_mask[MAX_BONES];
// Facing -Z at spawn, which is the direction W drives. Zero would be +Z, so the puppet
// would stand facing the way S goes and spin 180 the first time you pressed forward, in
// front of a camera whose own yaw starts at this same angle and never follows this one.
// The facing block below writes this from velocity once moving; this is only where it
// starts.
static float player_yaw = (float)M_PI;
static Sound* step_sound = NULL;

// The chaser (a second puppet that hunts the player) and the hearts it earns.
// It is its own entity with its own Animator, which is what per-node poses buy:
// two rigs over ONE set of meshes, each posed independently in the same frame.
// A FRACTION of whatever the player travels at, and that is what it always meant -- it
// was 6 against a player's 10, and the moment travel became a number derived from the
// clips (spec 12.10) a written-down 6 would have outrun the player instead.
#define CHASER_FRACTION 0.6f // slower than the player, so it is escapable
#define CATCH_RADIUS    (1.6f * PLAYER_SCALE)
// A few distinct hearts, not a cloud: RATE over SECONDS is the whole count,
// so a burst is about five. Raising either turns it back into a puff.
#define HEART_RATE    8.0f
#define HEART_SECONDS 0.6f
static Entity* chaser_entity = NULL;
static Animator* chaser_animator = NULL;
static SceneNode* chaser_rig = NULL;
static float chaser_yaw = 0.0f;
static bool no_chaser = false;

// Foot planting (spec 12.4). player_skel_root is the node the pose hangs under, and
// inverting its global transform is what turns a world-space raycast hit into the
// MODEL space the solver works in -- drop, facing yaw and scale all at once, so
// there is no scale factor written down here to get wrong.
static bool no_ik = false;
// Planting without locking -- the 12.4 behaviour, kept reachable because the two are only
// tellable apart by watching, and a walk that still slides is what the difference looks
// like.
static bool no_lock = false;
// Root motion off (spec 12.18): the locomotion pair goes back to the clips that stay
// where they are, the stick carries the body again, and the one-shots that travel are
// not offered. The A/B, and the only way to see what the feature is worth -- the two
// look the same in a still frame and quite different in motion.
static bool no_root_motion = false;
// Spawn no falling crates. For an arm that reads where the player ENDED UP: the crates
// land at rand() positions that differ per platform, so one of them on the character is a
// failure in one place and nowhere else.
static bool no_crates = false;
static IkSystem* player_ik = NULL;
static SceneNode* player_skel_root = NULL;
static int ik_foot_left = -1;
static int ik_foot_right = -1;
// Smoothed rather than switched: grounded is a bool, and planting a foot the instant
// it becomes true snaps the leg into place at the end of a jump.
static float ik_weight = 0.0f;
// Whether each character is in the water, settled once per fixed step in on_update and
// read by ik_update_targets in the pre-render hook. File statics because the two live in
// different hooks, the same reason ik_weight is one.
static bool player_swimming = false;
static bool chaser_swimming = false;

static SceneNode* heart_node = NULL;
static ParticleModule* heart_spawn = NULL;
static float heart_timer = 0.0f;    // > 0 while a burst is emitting
static float catch_cooldown = 0.0f; // so one catch is one burst, not sixty

// --trace-player: the player's pose and the input it acted on, printed every
// trace_every fixed steps, which is what the gate group reads.
static bool trace_player = false;
static int trace_every = 30;
static int trace_step = 0;

// Audio (spec 12.0): procedural tones, so the demo ships no audio files. A beep
// on jump and on spawn, and a looping tone carried by the door as an
// AUDIO_SOURCE component -- it pans and fades as the door swings and the camera
// moves. --mute silences the master bus.
static Sound* jump_sound = NULL;
static Sound* spawn_sound = NULL;
static bool audio_muted = false;

// The UI (spec 12.2). Declared up here rather than beside its functions because
// on_pre_render asks whether the menu owns the pointer, and that is well above
// them in this file.
static UISystem* ui_system = NULL;
static Font* ui_font = NULL;
// Four screens (spec 12.2, phase 7). The HUD sits at the BOTTOM of the stack
// for the whole run and the menus push above it, which works because capture is
// a property of the stack rather than of the top: ui_captures_input is true
// while any screen on it is modal, and the HUD is the one that is not.
static UIScreen* screen_main = NULL;
static UIScreen* screen_pause = NULL;
static UIScreen* screen_settings = NULL;
static UIScreen* screen_hud = NULL;
// The audio system is a local inside on_init; a menu callback takes only its
// own user pointer, so the handle it needs is a file static like the sounds.
static AudioSystem* ui_audio = NULL;
// The menu's own contribution to the pause state, so it can be applied on its
// edges and never stomp the pause the player asked for with P.
static bool ui_menu_paused = false;
// --ui-screen: which screen to open at startup, if any. A file static because
// the install runs from on_init, long after the flags were parsed.
static const char* ui_screen_at_start = NULL;
// --ui-focus: how many times to press "down" once a screen is up, so a golden
// can photograph the focus visual. Fed through the real navigation path rather
// than by setting focus directly, because a highlight proves nothing about a
// menu if navigation is the half that is broken.
static int ui_focus_steps = 0;
// The backdrop's own fragment stage. Owned by the engine's program cache once
// registered, like every other program in the tree.
static ShaderProgram* ui_backdrop_program = NULL;

/*
 * What the settings screen edits, and where it persists. The sliders bind
 * straight into this struct -- a control is a VIEW of the app's variable, so
 * there is nothing to read back -- and it is written out when the screen
 * closes.
 *
 * The bloom toggle is the exception that makes the point: it binds the
 * ENGINE's own field, because since 11.108 that is a plain bool and a copy to
 * mediate it would only be a second place for the answer to live.
 *
 * Every control here moves something observable, which is why the display mode
 * only joined them in 12.15: until settings_apply could make it take effect, a
 * control that changed when clicked and did nothing was the defect this app had
 * already shipped once.
 *
 * The monitor selector is the one control that is not a view of its own stored
 * value. The file holds a display NAME and a selector binds an int, so the
 * index below is a view of the LIVE monitor list and the name is copied across
 * on change -- an index in the file would renumber the day a display is
 * unplugged.
 */
static GameSettings ui_settings;
static char ui_settings_path[1024];
static bool ui_settings_have_path = false;
static bool ui_settings_dirty = false;
static int ui_tonemap = POSTFX_TONEMAP_NEUTRAL;
static int ui_monitor_index = 0;
// Borrowed from the engine, which owns them until a monitor disconnects. The
// selector borrows the array in turn, so it outlives ui_install.
static const char* const* ui_monitor_names = NULL;
static int ui_monitor_count = 0;

// The HUD's two labels, rewritten from live state each frame.
static UIElement* hud_speed_label = NULL;
static UIElement* hud_anim_label = NULL;
// The player's POST-SOLVE ground speed, latched where the animator's knob is
// derived from it, so the number on screen and the rig cannot disagree.
static float hud_ground_speed = 0.0f;

// What the game reads, and which key, pad button or pad axis each one is.
// The stick's Y is negated: GLFW reads it down-positive, and the move helper
// takes +y as forward.
static const InputAction actions[] = {
    {"move_x",
     {INPUT_KEY(D, 1), INPUT_KEY(A, -1), INPUT_AXIS(LEFT_X, 1), INPUT_PAD(DPAD_RIGHT, 1),
      INPUT_PAD(DPAD_LEFT, -1)}},
    {"move_y",
     {INPUT_KEY(W, 1), INPUT_KEY(S, -1), INPUT_AXIS(LEFT_Y, -1), INPUT_PAD(DPAD_UP, 1),
      INPUT_PAD(DPAD_DOWN, -1)}},
    {"jump", {INPUT_KEY(SPACE, 1), INPUT_PAD(A, 1)}},
    {"spawn", {INPUT_KEY(F, 1), INPUT_PAD(X, 1)}},
    // Kills the player. A key rather than a consequence because this app has no
    // damage, no health and no death -- inventing one to justify a ragdoll
    // would be the larger feature wagging the smaller one.
    {"ragdoll", {INPUT_KEY(K, 1), INPUT_PAD(LEFT_THUMB, 1)}},
    {"pause", {INPUT_KEY(P, 1), INPUT_PAD(START, 1)}},
    {"raycast", {INPUT_KEY(R, 1), INPUT_PAD(Y, 1)}},
    {"ground", {INPUT_KEY(G, 1), INPUT_PAD(B, 1)}},
    {"wave", {INPUT_KEY(E, 1), INPUT_PAD(LEFT_BUMPER, 1)}},

    /*
     * The two authored moves (spec 12.18): a step that carries a stated distance and a
     * turn that carries none. Both on TRIGGERS rather than buttons, because every one
     * of GLFW's fifteen is already spoken for above and the triggers are what is left
     * -- which is its own small argument for the rebinding UI this app does not have.
     */
    {"lunge", {INPUT_KEY(Q, 1), INPUT_AXIS(RIGHT_TRIGGER, 1)}},
    {"spin", {INPUT_KEY(C, 1), INPUT_AXIS(LEFT_TRIGGER, 1)}},

    /*
     * Quicksave and quickload, as ordinary game actions rather than keys read
     * somewhere by hand -- so they rebind like everything else and a pad can
     * reach them. NOT flagged `ui`, which means a menu suppresses them: saving
     * from inside a pause screen would capture the menu's own state as part of
     * the world, and the screen stack is not what a save is about.
     */
    {"quicksave", {INPUT_KEY(F5, 1), INPUT_PAD(RIGHT_BUMPER, 1)}},
    {"quickload", {INPUT_KEY(F9, 1), INPUT_PAD(RIGHT_THUMB, 1)}},

    /*
     * The follow camera's rotation, forest's bindings. Read unless --no-follow-cam.
     *
     * These share the arrow keys with ui_up/ui_down below and that is safe rather than
     * the bug the BACK note warns about: those carry `ui`, so a menu suppresses these
     * and the arrows navigate it, while with no menu open the UI ignores them and they
     * turn the camera. The hazard there is two readers acting at once, which the
     * suppression switch is exactly what prevents.
     */
    {"look_x", {INPUT_AXIS(RIGHT_X, 1), INPUT_KEY(RIGHT, 1), INPUT_KEY(LEFT, -1)}},
    {"look_y", {INPUT_AXIS(RIGHT_Y, -1), INPUT_KEY(UP, 1), INPUT_KEY(DOWN, -1)}},

    /*
     * The UI's own, flagged so they keep reading while the menu has taken input
     * away from the game -- the key that opens a menu has to be able to close
     * it. Escape rather than a quit: a menu is what Escape does in a game, and
     * quitting is an item inside it.
     *
     * BACK on the pad, not START: `pause` already holds START, and a button
     * doing two things is a bug waiting for whichever reader runs first.
     */
    {"menu", {INPUT_KEY(ESCAPE, 1), INPUT_PAD(BACK, 1)}, true},
    {"ui_up", {INPUT_KEY(UP, 1), INPUT_PAD(DPAD_UP, 1)}, true},
    {"ui_down", {INPUT_KEY(DOWN, 1), INPUT_PAD(DPAD_DOWN, 1)}, true},
    {"ui_left", {INPUT_KEY(LEFT, 1), INPUT_PAD(DPAD_LEFT, 1)}, true},
    {"ui_right", {INPUT_KEY(RIGHT, 1), INPUT_PAD(DPAD_RIGHT, 1)}, true},
    {"ui_accept", {INPUT_KEY(ENTER, 1), INPUT_PAD(A, 1)}, true},
};
#define ACTION_COUNT (sizeof(actions) / sizeof(actions[0]))

// Deferred door action (set in callback, applied in update)
static bool door_open_pending = false;
static float door_open_velocity = 0.0f;

// Uniform in [0, 1]. The division is in DOUBLE deliberately: RAND_MAX is 0x7fffffff,
// which float cannot represent, so dividing in float rounds the divisor up to 2^31 and
// the quotient is quietly wrong. double holds it exactly, and one rounding on the way
// out is the whole error.
static float rand01(void) {
    return (float)(rand() / (double)RAND_MAX);
}

// Create a visual mesh node for an entity
static SceneNode* create_box_node(Scene* scene, vec3 size, vec3 color, bool glass) {
    SceneNode* node = create_node();

    Mesh* mesh = create_mesh();
    Box box = {.position = {0, 0, 0}, .size = {size[0] * 2, size[1] * 2, size[2] * 2}};
    mesh_generate_box(mesh, &box);

    Material* mat = create_material();
    glm_vec3_copy(color, mat->albedo);
    if (glass) {
        mat->roughness = 0.05f;
        mat->metallic = 0.0f;
        mat->opacity = 0.2f;
        mat->ior = 1.5f;
    } else {
        mat->roughness = 0.4f;
        mat->metallic = 0.3f;
    }
    material_set_program(mat, pbr_shader);
    mesh->material = mat;

    node_add_mesh(node, mesh);
    node_add_child(scene->root_node, node);

    return node;
}

// Half the floor collider's thickness, and the one place it is written. The floor is a
// box CENTRED on its entity, so its walkable top is this far above that centre and the
// drawn plane has to be lifted by the same amount. Two independent literals is exactly
// how the visual and the collider drifted half a metre apart; the probe builds its own
// floor, so there are two construction sites and still only one number.
#define GAMETEST_FLOOR_HALF_Y 0.5f

// The ground a foot planting solver needs: a ramp of known slope and three steps.
//
// Both numbers the `ik` gate asserts are pure geometry and no contact slop can move
// them. Standing at the ramp's x = IK_RAMP_STAND the feet are one stance apart -- which
// the probe MEASURES and prints rather than restating as a constant here, so neither
// side holds the other's number -- and the ground under them differs by the slope times
// that stance. The steps give a difference of one riser instead.
//
// A rotated box rather than an authored wedge: entity_set_rotation_euler reaches
// settings.Rotation through entity_add_rigid_body AND the node local through
// sync_entity_transforms, so one rotation moves the collider and the visual together
// and the fixture needs no new asset. The rotation must be set BEFORE the body is
// created, which is the only ordering constraint here.
#define IK_RAMP_SLOPE  0.25f // rise over run; atan(0.25) = 14.036 degrees
#define IK_RAMP_FOOT_X 14.0f // where the ramp's top plane meets the floor
#define IK_RAMP_STAND  18.0f // the x the gate stands the rig at
#define IK_RAMP_HALF_X 6.0f
#define IK_RAMP_HALF_Y 0.5f
#define IK_RAMP_HALF_Z 4.0f
#define IK_STEP_RISER \
    0.5f // under gametest's step_height (0.4 * PLAYER_SCALE = 0.8),
         // but NOT under the engine default of 0.4
#define IK_STEP_HALF_Z 4.0f
#define IK_STEP_COUNT  3
// The shared edge of the first two steps: step 0 spans [-16,-14] and step 1 [-18,-16],
// so standing here puts one foot on each tread and the difference is one riser
// exactly -- with the UPHILL foot on the opposite side from the ramp's, which is what
// catches a solver that has hardcoded which leg bends.
#define IK_STEP_HALF_X  1.0f
#define IK_STEP_FIRST_X (-15.0f)
#define IK_STEP_PITCH   2.0f
// DERIVED, not asserted: the first two steps share this edge, so standing here puts one
// foot on each tread. Spelling it as a literal is what made the comment above have to
// explain the arithmetic the code could not.
#define IK_STEP_NOSING (IK_STEP_FIRST_X - IK_STEP_HALF_X)

// The grotto (spec 12.6): the 50x50 plate is a plateau in a walled basin with an ocean
// between it and the cliff. Walking off the edge was always possible and always
// unhandled -- a CharacterVirtual has no y limit, so you fell forever. This gives the
// fall a bottom.
//
// The platform hangs in the air and the cavern is a long way under it. The drop is the
// point, so the cavern must not be visible from up top: its rim sits far enough below
// that the follow camera, whose pitch stops at -1.25 rad, cannot get it into frame from
// the platform. You see sky past the edge, then you fall, and the walls close in on the
// way down.
//
// EVERY DEPTH BELOW IS RELATIVE TO THE WATER, which is what lets the whole basin be
// moved by editing one number. The clearances are the reason it has to stay that way.
// A floating character sits with its feet at GROTTO_WATER_Y + PLAYER_RIG_DROP, i.e. two
// units under the surface at this rig's scale; the IK foot ray starts a metre above that
// and reaches three down, so it probes to five under the surface and an eight-deep basin
// is out of its reach -- it cannot plant a swimmer's feet on the bottom even if the
// swimming gate were missed. And stick_to_floor_distance is 0.5, so a bed six metres
// under the feet is inert where a shallower one would pull a treading character down and
// report it grounded. Deepen the basin freely; SHALLOWING it past six breaks both.
#define GROTTO_WATER_Y    -180.0f // still-water plane, and a 180-unit fall to reach it
#define GROTTO_SEABED_Y   -190.0f // basin floor near the middle: ten under the water
#define GROTTO_SEABED_FAR -192.0f // and at the cliff, so the water deepens outward
#define GROTTO_BASIN_HALF 60.0f   // cliff ring, half extent
#define GROTTO_CLIFF_TOP  -50.0f  // 50 below the platform: out of sight until you drop
#define GROTTO_WALL_THICK 4.0f
// How far the platform's own edge hangs below it. A lip, not a wall -- the skirt used to
// run all the way to the seabed, which is the cavern the drop is supposed to hide.
#define GROTTO_SKIRT_DROP 3.0f
// Terminal velocity for the fall. Not flavour: uncapped, free fall over 180 units
// arrives at about 85 m/s, and the buoyancy below trades speed off over roughly half a
// unit of depth per m/s -- which at 85 is far through an eight-unit basin and into the
// seabed. At 25 the plunge is under 6, and that is what the basin depth is sized for.
//
// It is a CAP, so it does not scale with the drop: lengthening the fall costs more
// seconds, never more speed, and the plunge depth is the same as it was at 80 units.
#define GROTTO_TERMINAL_V    25.0f
#define GROTTO_SWIM_FRACTION 0.4f // a swimmer is not a runner, whatever a runner is

// The follow camera. forest's constants, scaled: this character is
// PLAYER_SCALE 2, so forest's 14 and 1 would sit it half as far back as intended.
// The shot sits CLOSE while the player has footing and opens out as it falls away from it
// (spec 12.13). One distance had to serve both walking a 50x50 plate and falling 180 units
// down the shaft; the plate lost, and the character sat small in a large empty frame.
//
// FAR is what the single pair used to be, so the wide end is the framing that was here.
// NEAR carries its whole rise in cam_pitch's default, so the height term is 0: the two are
// redundant -- the eye is always aimed AT the look point, so a height offset and a pitch
// offset move it the same way -- and putting it all in the angle leaves one number to read.
#define FOLLOW_CAM_NEAR_DISTANCE (4.0f * PLAYER_SCALE)
#define FOLLOW_CAM_NEAR_HEIGHT   0.0f
#define FOLLOW_CAM_FAR_DISTANCE  (9.0f * PLAYER_SCALE)
// Chosen so the wide shot keeps the elevation it had at the steeper pitch this replaced:
// 4.5*2 - sin(-0.178)*18 = 12.19, against the old 3.0*2 - sin(-0.35)*18 = 12.18.
#define FOLLOW_CAM_FAR_HEIGHT (4.5f * PLAYER_SCALE)
// Below the player's own origin, and that is what keeps the FEET in frame. A 1.8 m rig at
// PLAYER_SCALE spans y 0.5 to 4.1 about an origin at 2.0 -- the capsule's centre, which is
// ABOVE the body's -- so aiming at the origin puts the head mid-shot and drops the soles
// 0.135 below the bottom edge at the near distance and a 45.8 degree vertical fov. Aiming a
// metre under it centres the figure instead and leaves the feet about a metre clear.
#define FOLLOW_CAM_LOOK_Y (0.5f * PLAYER_SCALE)
// The fall band, in WORLD units and not scaled by the rig: it is measured against the
// world's own geometry, and the lower edge has to clear what the level legitimately steps
// down -- the demo staircase's risers are 0.5 and the ramp climbs 0.25 per unit -- or
// walking downstairs would pull the camera.
#define FOLLOW_CAM_DROP_START 3.0f
#define FOLLOW_CAM_DROP_FULL  18.0f
// Per second, and asymmetric on purpose: a fall wants the shot open before the drop reads
// as a mistake, while coming back in slowly is what keeps a landing from snapping.
#define FOLLOW_CAM_WIDEN_RATE   4.0f
#define FOLLOW_CAM_TIGHTEN_RATE 1.2f

// The foot ray starts above the ankle and reaches below it. Up has to clear the
// tallest thing a foot may already be standing on; down has to find ground the leg
// could actually reach. At this rig's scale the ankle rides about 0.16 above the sole,
// so a metre up and three down is ample -- and what stops a foot in mid-air seizing on
// the floor far below it is the WEIGHT, not the length of the ray.
#define IK_FOOT_RAY_UP  1.0f
#define IK_FOOT_RAY_LEN 3.0f
// 1/seconds the plant fades in and out. Grounded is a bool, and switching straight on
// it plants the leg the instant a jump lands, which reads as a snap.
#define IK_WEIGHT_RATE 10.0f

// The ramp's top plane, the one equation the gate restates: y = slope * (x - foot_x).
static float ik_ramp_height_at(float x) {
    return IK_RAMP_SLOPE * (x - IK_RAMP_FOOT_X);
}

static void build_ik_ground(Game* game) {
    EntityManager* em = game_get_entity_manager(game);
    PhysicsWorld* physics = game_get_physics_world(game);
    Scene* scene = game_get_scene(game);
    if (!em || !physics || !scene)
        return;

    const float angle = atanf(IK_RAMP_SLOPE);
    vec3 ramp_color = {0.35f, 0.30f, 0.28f};

    // The centre is DERIVED from the top plane rather than written down: rotating a box
    // by `angle` lifts its top face by half_y / cos(angle) above the centre, so the
    // centre sits that far below the plane at the stand point. Writing the answer as a
    // literal is how the ramp and the number the gate asserts would drift apart.
    vec3 ramp_size = {IK_RAMP_HALF_X, IK_RAMP_HALF_Y, IK_RAMP_HALF_Z};
    Entity* ramp = create_entity(em, "ik_ramp");
    glm_vec3_copy((vec3){IK_RAMP_STAND,
                         ik_ramp_height_at(IK_RAMP_STAND) - IK_RAMP_HALF_Y / cosf(angle), 0.0f},
                  ramp->position);
    entity_set_rotation_euler(ramp, (vec3){0.0f, 0.0f, angle});

    SceneNode* ramp_node = create_box_node(scene, ramp_size, ramp_color, false);
    node_set_name(ramp_node, "ik_ramp");
    ramp->node = ramp_node;

    // ramp_size, not a second triple. The drawn box and the collider have to be the
    // same box: this fixture exists so the ground a ray finds is the ground you see,
    // and writing its extents twice is how the floor's two halves drifted apart.
    PhysicsShapeDesc ramp_shape = {.type = SHAPE_BOX,
                                   .box.half_extents = {ramp_size[0], ramp_size[1], ramp_size[2]},
                                   .density = 0.0f};
    entity_add_rigid_body(ramp, physics, &ramp_shape, MOTION_STATIC, OBJ_LAYER_STATIC);

    // The steps march away along -X, each one riser taller and sitting ON the floor, so
    // a step's top is riser * (n + 1) and its half height is half of that. No rotation
    // and no trig: the whole point of the pair is that one fixture is analytic in a
    // direction the other is not.
    for (int i = 0; i < IK_STEP_COUNT; i++) {
        const float top = IK_STEP_RISER * (float)(i + 1);
        const float half_y = top * 0.5f;
        char name[32];
        snprintf(name, sizeof(name), "ik_step_%d", i);

        vec3 step_size = {IK_STEP_HALF_X, half_y, IK_STEP_HALF_Z};
        Entity* step = create_entity(em, name);
        glm_vec3_copy((vec3){IK_STEP_FIRST_X - IK_STEP_PITCH * (float)i, half_y, 0.0f},
                      step->position);

        SceneNode* step_node = create_box_node(scene, step_size, ramp_color, false);
        node_set_name(step_node, name);
        step->node = step_node;

        // step_size again, for the reason the ramp gives above.
        PhysicsShapeDesc step_shape = {
            .type = SHAPE_BOX,
            .box.half_extents = {step_size[0], step_size[1], step_size[2]},
            .density = 0.0f};
        entity_add_rigid_body(step, physics, &step_shape, MOTION_STATIC, OBJ_LAYER_STATIC);
    }

    printf("IK ground: a ramp rising %g in %g from x=%g, and %d steps of %g\n",
           (double)IK_RAMP_SLOPE, 1.0, (double)IK_RAMP_FOOT_X, IK_STEP_COUNT,
           (double)IK_STEP_RISER);
}

// One static box, the idiom build_ik_ground uses: the half-extent triple goes into the
// node and the shape once each, never written twice.
static void grotto_box(Scene* scene, EntityManager* em, PhysicsWorld* physics, const char* name,
                       vec3 centre, vec3 half, vec3 color, float tilt_z) {
    Entity* e = create_entity(em, name);
    if (!e)
        return;
    glm_vec3_copy(centre, e->position);
    // Before the body: entity_set_rotation_euler reaches settings.Rotation through
    // entity_add_rigid_body and the node local through sync_entity_transforms, so one
    // call moves collider and visual together -- but only in that order.
    if (tilt_z != 0.0f)
        entity_set_rotation_euler(e, (vec3){0.0f, 0.0f, tilt_z});

    SceneNode* node = create_box_node(scene, half, color, false);
    node_set_name(node, name);
    e->node = node;

    PhysicsShapeDesc shape = {
        .type = SHAPE_BOX, .box.half_extents = {half[0], half[1], half[2]}, .density = 0.0f};
    entity_add_rigid_body(e, physics, &shape, MOTION_STATIC, OBJ_LAYER_STATIC);
}

// The shaft: the basin walls and floor as one eroded heightfield rather than boxes.
//
// A roofless shaft is single-valued in Y, which is the whole reason this is a heightfield
// and not a voxel field -- so the procedural terrain subsystem applies unchanged and no
// engine code is added for it.
#define SHAFT_FIELD_RES     1025 // node-centred: res-1 must halve, so 1025 and not 1024
#define SHAFT_TILES         16
#define SHAFT_TILE_SEGS     48  // (2*800/16)/48 = 2.08 units a vertex
#define SHAFT_COLLIDER_SEGS 192 // coarser than the visual, as terrain.h intends
/*
 * How far the LAND runs, against GROTTO_BASIN_HALF, which is only how far the BASIN does.
 * The two are different questions and used to share one number: the terrain stopped at the
 * basin, so its square domain ended in mid-air and the edge of the world was a visible slab.
 *
 * 800 is 1600 units across for a crater 53 units wide, and that RATIO is the point rather
 * than the number. A crater has to be a hole in something, and "something" means ground
 * that runs past where the eye gives up. Two earlier attempts undershot it -- at 100 the
 * land read as an islet, at 300 the far slope was still a ridge against the sky from the
 * platform -- and each time the tell was the same: you could see where the world stopped.
 *
 * The field resolution rises WITH the extent and never after it. Those two set the cell
 * size between them, so moving one alone silently re-scales every noise frequency tuned
 * against it. 1025 over 1600 units is 1.56 units a cell; the crater spans about 34 of them
 * and keeps its shape, while the plain is sampled at whatever distance was going to hide
 * anyway. Erosion costs the square of this, so it is the number to look at first if
 * startup ever becomes the complaint.
 */
#define SHAFT_EXTENT  800.0f
#define SHAFT_FLOOR_R 0.30f // normalised radius the pool floor reaches out to
#define SHAFT_RIM_R   0.88f // and where the wall has finished climbing
/*
 * Where the land finally gives up, normalised on GROTTO_BASIN_HALF like the two above.
 *
 * The descent starts at the PLAIN radius, not at the rim. Starting it at the rim is what
 * made the first attempt an islet: the ground turned over and fell the moment the crater
 * wall topped out, so the whole landform was 90 units wide with sea on every side. 10.0 is
 * about 600 world units of open ground beyond a 53-unit crater before anything falls.
 *
 * And -216 is under the water at -180, so the descent finishes SUBMERGED: the domain's own
 * square boundary and its corners drown, and the only edge a player can find is a waterline
 * somewhere on that slope. Everything past 12.5 (about 750 units) is already under the sea,
 * which puts the corners at 1131 units both drowned and far outside anything in frame.
 */
#define SHAFT_PLAIN_R   10.00f
#define SHAFT_SHORE_R   12.50f
#define SHAFT_SHORE_Y   -216.0f
#define SHAFT_LAYER_TEX 512 // one ground layer map, square and periodic

// The field, and a SECOND params carrying no field at all.
//
// The second is a detail source, not a terrain: with `field` NULL and island shaping off,
// terrain_height_at is the analytic fbm and nothing else, which is how a caller outside
// terrain.c reaches that noise -- noise_perlin3_tiled is not exported, and reusing this
// costs nothing and inherits the tuned octave set.
static TerrainParams g_shaft;
static TerrainParams g_shaft_noise;
// Two more, and they are not decoration. g_shaft_warp displaces the SAMPLE POINT before
// any radius is taken, which is the only thing that stops features organising into rings;
// g_shaft_ridge is the high-frequency source the creasing transform below runs on.
static TerrainParams g_shaft_warp;
static TerrainParams g_shaft_ridge;
// The crag warp: the same idea as g_shaft_warp but at a fraction of the wavelength. On a
// wall this steep a horizontal displacement is worth about four times a vertical one, so
// this is where the rock detail has to live.
static TerrainParams g_shaft_crag;
static TerrainField g_shaft_field;
static bool g_shaft_ready = false;

static float shaft_smoothstep(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// Pool floor, walls climbing to the rim, and rock beyond it that fills the square
// domain's corners -- a round shaft in a square field leaves them, and filling them with
// solid rock is what a shaft wants anyway.
//
// THE FLOOR IS NEVER FLAT AND NEVER CLAMPED, which is a Jolt requirement before it is a
// look. A large run of exactly coplanar collider triangles fails Jolt's triangle splitter
// and takes JPH_ASSERT(false) through DummyTrace -- a debug-build death, and worse in
// release, where the same unsplittable BVH is built silently. Spec 11.63 shipped exactly
// that from a saturating island floor and fixed it at the geometry. So the fbm below
// keeps real weight at r = 0, and the profile is a lerp rather than a max.
// Crease a smooth noise into a sharp one.
//
// Perlin is smooth BY CONSTRUCTION, so any sum of it -- however many octaves -- gives
// dunes: rounded humps with no edges anywhere. 1 - |n| folds the field at every zero
// crossing, turning each one into a crease, and squaring sharpens the crease into an
// edge while flattening the basins between. That fold is the difference between a
// surface that reads as drifted and one that reads as fractured.
static float shaft_ridge(float v, float amp) {
    if (amp <= 0.0f)
        return 0.0f;
    float n = 1.0f - fabsf(v / amp);
    if (n < 0.0f)
        n = 0.0f;
    return n * n;
}

/*
 * The shaft's surface.
 *
 * TWO ARTIFACTS WERE SHIPPED HERE BEFORE THIS SHAPE, and they came from one mistake, so
 * the history is worth more than the formula. First a height detail f(x, z) on a
 * near-vertical wall, which cannot vary as you climb -- x and z do not change going up --
 * and rakes into vertical streaks. Then a terrace term added to fix that, which was a
 * sine over the RADIAL parameter and so drew seven concentric rings round the basin: a
 * contour map. An angular warp of sin(3a) + sin(5a) fluted it on top, because a pure
 * function of the angle is identical at every height.
 *
 * The common cause is that every one of those terms was a smooth analytic function of
 * (r, a), and such a function can only ever produce rings and lobes. Real rock is not a
 * function of anything: it is fracture and collapse, irregular at every scale. So the
 * regular terms are gone entirely rather than retuned, and what replaces them is noise
 * that is warped before it is measured and creased after.
 */
static float shaft_height(float x, float z) {
    /*
     * DOMAIN WARP FIRST, which is the single line that matters most here.
     *
     * Everything below keys off a radius, and a radius is precisely what turns any wobble
     * into a ring -- features line up into contours around the centre because the centre
     * is what they are measured from. Displacing the sample point by noise BEFORE the
     * radius is taken means there is no clean r left for anything to organise on. Two
     * independent taps, so the displacement is a genuine 2D vector rather than a radial
     * stretch wearing a new name.
     */
    // 12 and not 30. At 30 against a 60-unit basin the warp could drag a sample from the
    // middle of the pool out to most of the way up the wall, which is measurable rather
    // than theoretical: the pool floor came out at -174 when it is authored at -188, so
    // rock was breaching the water surface in the middle of the pool. The warp has to be
    // big enough to break the radial symmetry and small enough not to redraw the basin.
    // TWO warps, and the second is where the rock is.
    //
    // The first is the landform: low frequency, large amplitude, enough to take the basin
    // out of round so nothing organises into rings. The second is the crag detail, at a
    // tenth of the wavelength -- and it is horizontal for a measured reason. The wall
    // climbs ~6 units per 1.4 horizontal, so its slope is about 4; shoving the surface
    // sideways by 3 units therefore moves it vertically by ~12, where adding 3 units of
    // height to the same wall only bends the rate of climb and never carves a ledge. The
    // profile dump is what settled that: a vertical ridge term modulated the rise and
    // never once reversed it.
    // The crag is GATED on the unwarped radius, and that gate is what lets it be strong.
    //
    // Horizontal displacement drags whatever is at the sample point toward the centre, so
    // an ungated crag pulls WALL heights into the pool: measured, it lifted the pool floor
    // from -185.7 to -184.2 and breached the swimmer probe, the same failure the 30-unit
    // landform warp produced. Gating on the raw radius -- not on t, which is computed from
    // the warped position and would be circular -- makes the floor immune by construction
    // instead of by tuning, and with the floor safe the amplitude can go up where it is
    // wanted.
    const float r0 = sqrtf(x * x + z * z) / GROTTO_BASIN_HALF;
    const float crag = 2.0f * shaft_smoothstep(SHAFT_FLOOR_R * 0.8f, SHAFT_FLOOR_R * 1.7f, r0);

    // The LANDFORM warp is gated on the same unwarped radius as the crag, and for the same
    // reason: horizontal displacement drags whatever stands at the sample point toward the
    // centre, so over the pool it hauls wall height into the water. Ungated it lifted the
    // floor to -184 against a swimmer foot probe at -185. Gating both leaves the pool
    // computed from its true radius; the floor noise below is what keeps it from being a
    // surface of revolution, and from being a coplanar run Jolt cannot split.
    const float land = 12.0f * shaft_smoothstep(SHAFT_FLOOR_R * 0.8f, SHAFT_FLOOR_R * 1.7f, r0);

    const float wx = x + land * terrain_height_at(&g_shaft_warp, x, z) +
                     crag * terrain_height_at(&g_shaft_crag, x, z);
    const float wz = z + land * terrain_height_at(&g_shaft_warp, x + 918.0f, z + 517.0f) +
                     crag * terrain_height_at(&g_shaft_crag, x - 377.0f, z + 244.0f);

    const float r = sqrtf(wx * wx + wz * wz) / GROTTO_BASIN_HALF;
    const float t = shaft_smoothstep(SHAFT_FLOOR_R, SHAFT_RIM_R, r);
    float h = GROTTO_SEABED_Y + (GROTTO_CLIFF_TOP - GROTTO_SEABED_Y) * t;

    // The wall, as creased noise read at the WARPED position so it inherits the same
    // broken symmetry. The offset re-centres a 0..1 ridge about zero, so the term cuts
    // into the profile as well as standing off it -- gullies as well as buttresses.
    const float ridged =
        shaft_ridge(terrain_height_at(&g_shaft_ridge, wx, wz), g_shaft_ridge.height);
    // Amplitude comes DOWN as the frequency goes up, and the pair is the point: relief is
    // read as slope, so 5 units across a 7-unit wavelength bites far harder than 9 across
    // 33 did. Kept on t so the pool floor stays inside the swimmer clearance.
    h += (ridged - 0.35f) * 5.0f * t;

    /*
     * The floor term, which is doing a different job from the wall and is scaled for it.
     *
     * It is the anti-coplanar relief the Jolt BVH needs -- a large run of exactly planar
     * collider triangles fails the splitter -- so it must never reach zero. It is also
     * bounded by the swimmer clearance: a floating character's feet sit two units under
     * the surface and the IK foot ray probes three below that, so rock rising past that
     * line plants a swimmer on the bottom. Small, and weighted away from the wall where
     * the ridged term above has the relief covered.
     */
    // Weighted down from 1.0 at the floor to leave margin under the swimmer probe: the
    // rule is about the floor's HIGHEST point, so the full noise amplitude spent its whole
    // budget on one poke. 0.6 keeps well over a unit of relief, which is all Jolt needs.
    h += terrain_height_at(&g_shaft_noise, x, z) * (0.6f - 0.45f * t);

    /*
     * And beyond the rim the ground FALLS AWAY to the sea.
     *
     * Without this the crater is a square plateau: the height function flattens past the
     * rim, the field's domain ends, and the terrain simply stops in mid-air with the water
     * visible 130 units below. From the platform that reads as the edge of a slab, which
     * is the one thing that says "this is a fixture" rather than "this is a place".
     *
     * Falling to below the waterline makes the boundary DROWN instead. The shoreline the
     * player sees is then where this slope crosses the water, somewhere on the way down,
     * and the square itself is under the sea with no edge to find. The basin becomes a
     * crater on a rocky island -- and since the water is one infinite plane, the pool at
     * the bottom of the shaft and the sea around the island are the same surface, which
     * is what makes the reading hold together rather than needing two water levels.
     *
     * The ridged term above is still live out here (its weight is t, which saturates at
     * the rim), so the drowned flat is not a run of coplanar triangles and Jolt can still
     * split it.
     */
    // Open ground beyond the rim, rolling rather than flat: without this the plain is an
    // apron at one height and reads as a tabletop the crater was cut into. The warp noise
    // is already low frequency -- 83 units a cycle -- which is the scale that reads as
    // landform from the platform rather than as texture.
    h += terrain_height_at(&g_shaft_warp, x + 211.0f, z - 133.0f) * 9.0f * t;

    h += (SHAFT_SHORE_Y - GROTTO_CLIFF_TOP) * shaft_smoothstep(SHAFT_PLAIN_R, SHAFT_SHORE_R, r);
    return h;
}

static void build_shaft(Game* game) {
    EntityManager* em = game_get_entity_manager(game);
    PhysicsWorld* physics = game_get_physics_world(game);
    Scene* scene = game_get_scene(game);
    if (!em || !physics || !scene)
        return;

    g_shaft_noise = terrain_default_params();
    g_shaft_noise.extent = SHAFT_EXTENT;
    g_shaft_noise.height = 2.5f; // amplitude of the rock detail, not of the shaft
    g_shaft_noise.base_freq = 0.035f;
    // Four and not five. At base_freq 0.035 the fifth octave lands near 1.8 world units,
    // against a tile cell of 1.25 -- close enough to the mesh Nyquist that it aliases onto
    // the triangle grid and prints as a regular field of facets rather than as rock.
    // Dropping one octave puts the finest detail at about 3.6 units, clear of the lattice.
    g_shaft_noise.octaves = 4;
    g_shaft_noise.seed = 20260913u;
    g_shaft_noise.island_start = 0.0f; // island shaping is a DOME and has no inverse
    g_shaft_noise.field = NULL;

    // The warp. LOW frequency and large amplitude on purpose: this is not detail, it is
    // the term that decides where the wall is at all, and it has to move the sample point
    // by tens of units to break the basin out of round at a scale the eye reads as
    // landform rather than as texture.
    g_shaft_warp = g_shaft_noise;
    g_shaft_warp.height = 1.0f;
    g_shaft_warp.base_freq = 0.012f; // ~83 units a cycle
    g_shaft_warp.octaves = 3;
    /*
     * Every source below INHERITS the seed rather than setting its own, and that is a
     * performance contract, not tidiness.
     *
     * terrain.c memoises the Perlin permutation table ONE entry deep, keyed on the seed,
     * and rebuilds it with a 256-element Fisher-Yates whenever the seed differs from the
     * last call. Its own comment says a shuffle per call "would dominate load time
     * entirely" -- which is exactly what four seeds did here: shaft_height interleaves
     * seven taps across warp, crag, ridge and floor noise, so the memo missed on nearly
     * every one and the fill paid about seven million shuffles. Measured at 11.7 s of a
     * 15.7 s terrain build, against 1.7 s for the erosion sim it dwarfed.
     *
     * One seed means one table, built once and hit forever after. The four fields stay
     * decorrelated by what actually separates them -- base_freq spanning 0.012 to 0.14,
     * and sample offsets in the hundreds of units -- so sharing a table costs nothing
     * visible. Giving any of them its own seed again reinstates the thrash.
     */

    // The ridge source, read through shaft_ridge. Higher frequency than the warp, since
    // this one IS detail -- but still clear of the tile lattice at 1.25 units a vertex.
    g_shaft_ridge = g_shaft_noise;
    g_shaft_ridge.height = 1.0f;
    // SHORT wavelength, because what makes rock read as rock is local slope, not
    // amplitude. At 0.030 this ran a 33-unit wavelength against a wall whose base slope is
    // about 4, so even nine units of relief perturbed it by 0.27 -- seven percent, which
    // renders as a smooth funnel however jagged the generator is. At 0.14 the wavelength
    // is about 7 units, and a few units of relief across that is a local slope near 1.
    g_shaft_ridge.base_freq = 0.14f;
    // Two, not four, and it follows from the frequency above rather than from taste. Each
    // octave halves the wavelength, so four octaves from a 7-unit base reaches 0.89 units
    // -- against a 0.469-unit field cell that is under two samples per cycle, which is the
    // definition of aliasing, and it prints as the regular hatching the dumps show across
    // the pool floor. Two octaves stop at 3.6 units, about eight samples a cycle.
    g_shaft_ridge.octaves = 2;
    // Seed inherited from g_shaft_noise: see the permutation-table note above.

    // Short wavelength, modest amplitude. The measured wall climbs about 6 units per 1.4
    // horizontal -- a base slope near 4 -- so a 3-unit radial shove moves the surface some
    // 12 units vertically. That is the leverage a vertical term does not have, and it is
    // why detail belongs here rather than in the height.
    g_shaft_crag = g_shaft_noise;
    g_shaft_crag.height = 3.0f;
    g_shaft_crag.base_freq = 0.10f; // ~10 units a cycle, clear of the 0.47 field cell
    g_shaft_crag.octaves = 3;
    // Seed inherited from g_shaft_noise: see the permutation-table note above.

    g_shaft = terrain_default_params();
    g_shaft.extent = SHAFT_EXTENT;
    g_shaft.tiles = SHAFT_TILES;
    g_shaft.tile_segments = SHAFT_TILE_SEGS;
    g_shaft.island_start = 0.0f;
    g_shaft.field = NULL; // installed after the erode, never before

    if (!terrain_field_alloc(&g_shaft_field, SHAFT_FIELD_RES)) {
        fprintf(stderr, "Shaft: field allocation failed\n");
        return;
    }

    // Hand-filled, in terrain_field_seed's shape but NOT through it: that function nulls
    // the installed field and writes the analytic fbm over every node, which is the one
    // thing a shaped basin must not have done to it.
    for (int j = 0; j < SHAFT_FIELD_RES; ++j) {
        const float z =
            terrain_world_z(&g_shaft, terrain_field_node(g_shaft.extent, SHAFT_FIELD_RES, j));
        for (int i = 0; i < SHAFT_FIELD_RES; ++i) {
            const float x =
                terrain_world_x(&g_shaft, terrain_field_node(g_shaft.extent, SHAFT_FIELD_RES, i));
            g_shaft_field.height[(size_t)j * SHAFT_FIELD_RES + i] = shaft_height(x, z);
        }
    }
    terrain_field_measure(&g_shaft_field); // owed by whoever last wrote the field

    // Erosion over the shaped field. Thermal is what puts scree at the foot of the walls
    // and hydraulic is what carves channels down them, and the basin being CLOSED --
    // erosion zeroes flux across the domain boundary -- is an artifact for an island and
    // exactly right here: water ponds in the middle and the deposit mask becomes the silt.
    //
    // talus is a SLOPE and the threshold is talus * cell, so the default 0.62 over this
    // 0.47-unit cell shaves far harder than it does over forest's 1.96. The walls run at
    // about 2.4, so anything near the default collapses them into a saucer.
    ErosionParams ep = erosion_default_params();
    ep.talus = 3.0f;
    ep.thermal_every = 8;
    // Fewer passes. Thermal erosion flattens toward the angle of repose, which is exactly
    // the operation that smooths the short-wavelength relief above back out -- and the
    // dumps show it also sharpening the field's own lattice aliasing into diagonal stripes
    // across the pool floor. The sim is here to add scree and channels, not to sand the
    // rock down.
    ep.iterations = 40;

    /*
     * Cooked, and no longer optional.
     *
     * Erosion is O(res^2 * iterations), so taking the field from 257 to 1025 to carry the
     * wider land multiplied this sim by sixteen. A hit reloads the four worn planes off
     * disk and skips the sim outright, so only the first run after a shape change pays --
     * which is what lets the land be this large at all.
     *
     * The recipe name must fit COOK_RECIPE_MAX, which COUNTS THE NUL: 23 characters, not
     * 24. cook_key refuses a longer one and hands back an invalid key, and that refusal is
     * silent in every counter the summary prints -- fetch and store both return before
     * they reach the miss, the refusal or the store failure -- so an over-long name
     * subtracts this site from the report rather than failing anywhere in it. The name
     * here was one byte over when it was written, and cooked nothing at all.
     *
     * The key folds the SEEDED HEIGHT PLANE rather than the parameters that produced it.
     * The bytes capture the bowl profile, both warps, the ridge, the plain roll and the
     * shore descent transitively, so editing any of those invalidates the cache without
     * anyone remembering to add a field here -- which is the failure this pattern exists
     * to prevent. Every ErosionParams scalar folds EXCEPT workers: worker-invariance is
     * the erosion module's own proven contract, and folding it would fracture the cache
     * while contradicting the invariant. The extent folds too, since it decides the cell
     * size the talus threshold is measured against.
     */
    CookKey ek = cook_key("shaft-erosion/1");
    cook_key_i32(&ek, SHAFT_FIELD_RES);
    cook_key_i32(&ek, ep.iterations);
    cook_key_f32(&ek, ep.dt);
    cook_key_f32(&ek, ep.rain);
    cook_key_f32(&ek, ep.evaporation);
    cook_key_f32(&ek, ep.capacity);
    cook_key_f32(&ek, ep.dissolve);
    cook_key_f32(&ek, ep.deposit);
    cook_key_f32(&ek, ep.min_tilt);
    cook_key_f32(&ek, ep.talus);
    cook_key_f32(&ek, ep.thermal_rate);
    cook_key_i32(&ek, ep.thermal_every);
    cook_key_f32(&ek, g_shaft.extent);
    const size_t plane_bytes = (size_t)SHAFT_FIELD_RES * (size_t)SHAFT_FIELD_RES * sizeof(float);
    cook_key_bytes(&ek, g_shaft_field.height, plane_bytes);

    CookBlob sec[5];
    bool restored = false;
    if (cook_fetch(&ek, sec, 5)) {
        if (sec[0].size == 2 * sizeof(float) && sec[1].size == plane_bytes &&
            sec[2].size == plane_bytes && sec[3].size == plane_bytes &&
            sec[4].size == plane_bytes) {
            // Adopt the planes rather than copying into the field's own: each is an
            // independent allocation the field frees individually, and the pyramid built
            // below aliases whatever pointer it holds by then.
            memcpy(&g_shaft_field.min_y, sec[0].data, sizeof(float));
            memcpy(&g_shaft_field.max_y, (const char*)sec[0].data + sizeof(float), sizeof(float));
            free(g_shaft_field.height);
            free(g_shaft_field.flow);
            free(g_shaft_field.deposit);
            free(g_shaft_field.wear);
            g_shaft_field.height = sec[1].data;
            g_shaft_field.flow = sec[2].data;
            g_shaft_field.deposit = sec[3].data;
            g_shaft_field.wear = sec[4].data;
            free(sec[0].data);
            restored = true;
        } else {
            for (int s = 0; s < 5; ++s)
                free(sec[s].data);
            fprintf(stderr, "Shaft: the cooked field disagrees with the recipe; eroding live\n");
        }
    }

    if (!restored) {
        ErosionStats st;
        if (!terrain_erode(&g_shaft_field, &g_shaft, &ep, &st)) {
            fprintf(stderr, "Shaft: erosion refused, using the unworn field\n");
        } else {
            float range[2] = {g_shaft_field.min_y, g_shaft_field.max_y};
            CookBlob out[5] = {{range, sizeof(range)},
                               {g_shaft_field.height, plane_bytes},
                               {g_shaft_field.flow, plane_bytes},
                               {g_shaft_field.deposit, plane_bytes},
                               {g_shaft_field.wear, plane_bytes}};
            cook_store(&ek, out, 5);
        }
    }

    /*
     * The swimmer clearance, MEASURED after the erode rather than reasoned about before it.
     *
     * A floating character's feet sit at GROTTO_WATER_Y + PLAYER_RIG_DROP and the IK foot
     * ray reaches three units below that, so rock rising past that line lets a swimmer
     * plant on the bottom. The rule is about the floor's HIGHEST point, which is exactly
     * what a noisy floor makes easy to get wrong -- a mean says nothing here, because one
     * node poking up is enough to catch a foot.
     *
     * After the erode and not before, because this basin is CLOSED: the sim ponds water in
     * the middle and deposits there, so erosion raises this floor. Measuring the seeded
     * field would be measuring the wrong surface, in the wrong direction.
     *
     * The disc is the nominal floor, not an exact one -- the domain warp means world
     * radius and the profile's t no longer agree -- but it is the middle of the pool,
     * which is where a swimmer actually floats.
     */
    const float floor_radius = SHAFT_FLOOR_R * GROTTO_BASIN_HALF;
    float floor_max = GROTTO_SEABED_FAR;
    for (int j = 0; j < SHAFT_FIELD_RES; ++j) {
        const float z = terrain_field_node(g_shaft.extent, SHAFT_FIELD_RES, j);
        for (int i = 0; i < SHAFT_FIELD_RES; ++i) {
            const float x = terrain_field_node(g_shaft.extent, SHAFT_FIELD_RES, i);
            const float y = g_shaft_field.height[(size_t)j * SHAFT_FIELD_RES + i];
            if (y > floor_max && sqrtf(x * x + z * z) <= floor_radius)
                floor_max = y;
        }
    }
    const float swim_probe = GROTTO_WATER_Y + PLAYER_RIG_DROP - 3.0f;
    if (floor_max > swim_probe)
        fprintf(stderr,
                "Shaft: pool floor reaches %g, above the swimmer foot probe at %g -- a "
                "swimmer can plant on the bottom\n",
                (double)floor_max, (double)swim_probe);

    g_shaft.field = &g_shaft_field;
    terrain_field_build_pyramid(&g_shaft_field); // last: the levels are copies

    // The layered rock. Which layer shows at a texel is decided on the CPU, by the splat:
    // terrain_bake_splat writes rock from SLOPE ALONE into .r, silt from the deposit mask
    // into .g and gravel from flow into .b. So "bare rock up the walls, silt on the basin
    // floor" costs nothing to author -- it is what the erosion already knows, and rock
    // lands on a cliff whether or not water ever ran there. layers.glsl itself has no
    // slope or height input; it reads the splat and the layer heights and nothing else.
    const struct {
        TerrainLayerKind kind;
        const char* name;
        float uv_scale;
    } layers[] = {
        // Slot 0 is the REMAINDER -- what shows where all three splat channels are low.
        // forest puts grass there; a cave has none, so rock takes it at a broad scale and
        // slot 1 takes rock again at a tighter one with its own seed. Two rock layers
        // rather than one is what stops a 140-unit wall reading as a single tiling.
        // uv_scale is WORLD UNITS PER TILE, so small numbers repeat harder. These were
        // 9/4/5/3, tuned by eye against forest's ground, where the camera looks DOWN at
        // terrain from a distance. A shaft wall is 140 units of near-vertical rock read
        // from a few metres away, and at those numbers one map tiled thirty times up it
        // and printed as a field of scales. Larger tiles trade crispness for not
        // announcing the texture, which is the right trade on a wall you stand next to.
        // ONE layer, and the four that were here are the reason the basin had contour
        // rings painted on it.
        //
        // terrain_bake_splat picks the layer from SLOPE, through smoothstep(0.62, 0.88).
        // On a basin slope is a function of the radius, so every layer boundary it draws
        // is a circle round the centre -- and where slope sits near that threshold a hair
        // of noise flips the choice back and forth, which bands hard. The dumped splat
        // shows exactly that: a green silt ring hugging the pool and red/green filaments
        // across the outer field. No amount of retuning removes it, because a slope-keyed
        // splat on a radial landform can only draw contours.
        //
        // Slot 0 is the remainder and takes weight 1 everywhere when it is the only entry,
        // so with one layer the splat is never consulted and there is nothing to band.
        // What carries the rock now is the GEOMETRY and the triplanar detail on it, which
        // is where the variety should have been coming from all along.
        {TERRAIN_LAYER_ROCK, "base", 18.0f},
    };
    const int layer_count = (int)(sizeof(layers) / sizeof(layers[0]));

    Material* rock = create_material();
    material_set_program(rock, pbr_shader);
    rock->roughness = 0.9f;
    rock->metallic = 0.0f;
    // Brown, and it has to be applied HERE rather than in the layer maps. albedo
    // multiplies every layer the shader blends, so one warm value browns the bedrock, the
    // silt and the gravel together and keeps the relation between them -- where retuning
    // four procedural palettes would be four chances to break it. terrain_tex bakes a
    // GROUND set, grey-green bedrock and pale silt meant to sit under a sky; the default
    // white albedo passed that palette through unchanged, which is why the shaft read as
    // wet concrete instead of rock.
    glm_vec3_copy((vec3){0.52f, 0.34f, 0.22f}, rock->albedo);
    // Registered with the scene, which nothing in gametest has ever needed to do: the
    // layer INDICES are resolved by material_texture_array_build, which walks
    // scene->materials. An unregistered layered material keeps every index at -1 and
    // renders as layer 0 everywhere -- a plausible frame, and the wrong one.
    scene_add_material(scene, rock);

    for (int i = 0; i < layer_count; ++i) {
        unsigned char *albedo = NULL, *surface = NULL;
        terrain_layer_maps(layers[i].kind, SHAFT_LAYER_TEX, 20260913u + (unsigned)i * 977u, &albedo,
                           &surface);
        if (!albedo || !surface) {
            free(albedo);
            free(surface);
            fprintf(stderr, "Shaft: layer %s bake failed\n", layers[i].name);
            continue;
        }
        char key[64];
        snprintf(key, sizeof(key), "shaft_layer_%s_a", layers[i].name);
        material_set_layer_albedo_tex(rock, i,
                                      texture_load_memory_owned(scene->tex_pool, key, albedo,
                                                                SHAFT_LAYER_TEX, SHAFT_LAYER_TEX, 4,
                                                                texture_desc(false)));
        snprintf(key, sizeof(key), "shaft_layer_%s_s", layers[i].name);
        material_set_layer_surface_tex(rock, i,
                                       texture_load_memory_owned(scene->tex_pool, key, surface,
                                                                 SHAFT_LAYER_TEX, SHAFT_LAYER_TEX,
                                                                 4, texture_desc(false)));
        rock->layers[i].uv_scale = layers[i].uv_scale;
    }

    const int splat_res = g_shaft_field.res;
    unsigned char* splat = malloc((size_t)splat_res * (size_t)splat_res * 3u);
    if (splat && terrain_bake_splat(&g_shaft, splat_res, splat)) {
        material_set_splat_tex(rock, texture_load_memory_owned(scene->tex_pool, "shaft_splat",
                                                               splat, splat_res, splat_res, 3,
                                                               texture_desc(false)));
    } else {
        free(splat);
        fprintf(stderr, "Shaft: splat bake failed; the rock falls back to layer 0\n");
    }

    // WORLD_XZ and not UV1, which is not a preference: build_grid writes UV1 as a literal
    // zero, so a mesh-local splat samples one texel and the whole shaft resolves to layer
    // 0. That shipped broken once already, through a green suite.
    rock->splat_space = SPLAT_SPACE_WORLD_XZ;
    rock->splat_origin[0] = terrain_world_x(&g_shaft, -g_shaft.extent);
    rock->splat_origin[1] = terrain_world_z(&g_shaft, -g_shaft.extent);
    rock->splat_size[0] = rock->splat_size[1] = 2.0f * g_shaft.extent;
    rock->layer_count = layer_count; // LAST: it is what arms the shader

    // The mesh side of the same switch, and it has to precede the first tile built below.
    // The layers carry the rock's colour now, so the vertex tint must become macro
    // variation -- left as a colour the two multiply and the shaft comes out near black.
    g_shaft.layered = true;

    SceneNode* group = create_node();
    node_set_name(group, "shaft");
    node_add_child(scene->root_node, group);

    int built = 0;
    for (int tz = 0; tz < g_shaft.tiles; ++tz) {
        for (int tx = 0; tx < g_shaft.tiles; ++tx) {
            Mesh* mesh = create_mesh();
            if (!terrain_build_tile(&g_shaft, tx, tz, mesh)) {
                free_mesh(mesh);
                continue;
            }
            mesh->material = rock;
            SceneNode* node = create_node();
            node_add_mesh(node, mesh);
            node_add_child(group, node);
            built++;
        }
    }

    // One static mesh collider for the whole shaft. MOTION_STATIC is not a choice:
    // entity_add_rigid_body refuses a mesh shape on anything else by name.
    //
    // The triangle count is REPORTED rather than assumed, and that is about evidence
    // rather than tidiness. A silent false from terrain_build_collider leaves the basin
    // with no collision at all, which from outside looks exactly like a working build --
    // and it would also mean Jolt never indexed these triangles, so a run that did not
    // assert would prove nothing about the coplanar hazard this geometry is shaped to
    // avoid. A number here is what makes a quiet run meaningful.
    size_t collider_tris = 0;
    Entity* e = create_entity(em, "shaft");
    if (e) {
        Mesh* collider = create_mesh();
        if (terrain_build_collider(&g_shaft, SHAFT_COLLIDER_SEGS, collider)) {
            PhysicsShapeDesc desc = {.type = SHAPE_MESH, .density = 0.0f};
            desc.mesh.vertices = collider->vertices;
            desc.mesh.vertex_count = collider->vertex_count;
            desc.mesh.indices = collider->indices;
            desc.mesh.index_count = collider->index_count;
            entity_add_rigid_body(e, physics, &desc, MOTION_STATIC, OBJ_LAYER_STATIC);
            collider_tris = collider->index_count / 3;
        } else {
            fprintf(stderr, "Shaft: collider build refused; the basin has no collision\n");
        }
        free_mesh(collider); // borrowed for the create call only; Jolt has copied it
    }

    g_shaft_ready = true;
    printf("Shaft: %d tiles over %g units, field %d^2, collider %zu tris, y %g to %g, pool "
           "floor tops at %g (swimmer probe %g)\n",
           built, (double)(2.0f * g_shaft.extent), SHAFT_FIELD_RES, collider_tris,
           (double)g_shaft_field.min_y, (double)g_shaft_field.max_y, (double)floor_max,
           (double)swim_probe);
}

// The lip under the platform's own rim. Static, and before physics_world_optimize.
//
// This is all that is left of the box grotto: the seabed, the cliff ring and the inward
// overhang are now build_shaft's eroded heightfield. The skirt stays a box because it
// belongs to the PLATFORM rather than to the basin -- it is what stops a 50x50 plate
// reading as a plane with no thickness, and it deliberately does not reach the water.
static void build_platform_skirt(Game* game) {
    EntityManager* em = game_get_entity_manager(game);
    PhysicsWorld* physics = game_get_physics_world(game);
    Scene* scene = game_get_scene(game);
    if (!em || !physics || !scene)
        return;

    vec3 rock = {0.26f, 0.24f, 0.26f};

    const float platform_half = 25.0f;
    // A lip under the platform's rim, so it reads as a slab hanging in the air rather
    // than a plate with no thickness. It deliberately does NOT reach the water.
    const float skirt_half_y = GROTTO_SKIRT_DROP * 0.5f;
    const float skirt_mid_y = -skirt_half_y;
    // Four skirts. The signs walk the perimeter: x then z, each twice, so one loop body
    // serves four boxes and no lip is written out by hand.
    for (int i = 0; i < 4; i++) {
        const float s = (i & 1) ? -1.0f : 1.0f;
        const bool along_x = i < 2;
        char name[32];

        /*
         * OUTSIDE the platform, not centred on its edge, and that is a z-fighting fix
         * rather than tidiness. Centred on +/-25 with a half-extent of 0.5 the skirt
         * spanned 24.5 to 25.5, and its top face at y = 0 was coplanar with the floor
         * plane over that half-unit strip -- two surfaces in one plane, the depth test
         * picking a winner per pixel per frame, and the platform edge crawling with
         * stripes that changed as the camera moved.
         *
         * At 25.5 it spans 25 to 26: touching the floor along one edge, overlapping it
         * nowhere. The long axis runs slightly past the corner so the four do not leave
         * a notch where they meet.
         */
        const float skirt_t = 0.5f;
        vec3 skirt_c = {along_x ? s * (platform_half + skirt_t) : 0.0f, skirt_mid_y,
                        along_x ? 0.0f : s * (platform_half + skirt_t)};
        vec3 skirt_h = {along_x ? skirt_t : platform_half + 2.0f * skirt_t, skirt_half_y,
                        along_x ? platform_half + 2.0f * skirt_t : skirt_t};
        snprintf(name, sizeof(name), "grotto_skirt_%d", i);
        grotto_box(scene, em, physics, name, skirt_c, skirt_h, rock, 0.0f);
    }

    printf("Platform skirt: four lips at +/-%g, hanging %g below the plate\n",
           (double)platform_half, (double)GROTTO_SKIRT_DROP);
}

// Where the water's surface is, asked of the WATER rather than repeated from the constant
// that put it there -- so moving the ocean moves what counts as being in it, and the two
// cannot drift. The fallback is that constant, for a scene with no water at all.
//
// What this deliberately does NOT claim is the wave displacement. The surface is an
// inverse FFT evaluated in the shader into a texture, and `water.h` publishes the still
// plane and the height VARIANCE but no point sample, so there is nothing on this side to
// ask for the height under a given (x, z). `level` is the plane the waves ride on, which
// is their mean and so the best single answer available; at this sea state -- 2.5 m/s over
// 900 m of fetch -- they ride within a few centimetres of it, against a character four
// units tall. A surface that actually heaved would need a CPU evaluation of the spectrum
// or a readback, and neither exists yet.
static float grotto_surface_y(const Scene* scene) {
    return scene && scene->water ? scene->water->level : GROTTO_WATER_Y;
}

// Whether a capsule centre is under the surface. One predicate, so the player, the
// chaser and the IK gate cannot disagree about what "in the water" means.
static bool grotto_submerged(const Scene* scene, const vec3 p) {
    return p[1] < grotto_surface_y(scene);
}

// Tread water: drive the capsule toward the surface and damp it, rather than fall.
//
// This is newly authored rather than wired to anything. Jolt's buoyancy is not bound in
// this engine at all, gravity_factor exists only on RigidBody and is unreachable from a
// CharacterVirtual, and the gravity passed to character_controller_update is world-wide
// and shared by every character. The seam that IS available is the velocity the app sets
// each step, so that is where this lives.
//
// Critically damped toward the surface with the return clamped: a long fall arrives with
// a large downward velocity, and an undamped spring would fire the character back out of
// the water like a cork.
static float grotto_float_velocity(float centre_y, float vy, float dt) {
    const float depth = GROTTO_WATER_Y - centre_y; // positive when under
    const float buoyancy = 18.0f;                  // toward the surface, per metre under
    const float drag = 6.0f;                       // vertical damping, 1/s
    const float rise_max = 4.0f;                   // never breach

    vy += (buoyancy * depth - drag * vy) * dt;
    if (vy > rise_max)
        vy = rise_max;
    return vy;
}

// The bed adapter that used to live here is gone with the shore: build_ocean hands the
// water no height function now, so there is nothing for it to agree with. See the note
// there for why a crater rim should never have been a foreshore.

// The ocean. Placement is `level` alone: the surface is a projected grid in NDC and is
// effectively infinite, so `extent` bounds only the bed field and not what is drawn.
// The cliff ring is what gives the water a visible edge -- at this camera the ground
// plane's vanishing point sits above the top of the frame, so there is no horizon in
// shot to end it.
static void build_ocean(Scene* scene) {
    Water* water = create_water();
    if (!water) {
        fprintf(stderr, "Failed to create water\n");
        return;
    }
    water->level = GROTTO_WATER_Y;
    water->extent = SHAFT_EXTENT; // inert with no bed below; bounds the bed field only
    /*
     * NO BED, and that is what turns the beach off.
     *
     * A bed provider is what makes this a SHORE. Handing one over had the water tracing a
     * waterline around the crater -- 844 points over 4546 units, at a foreshore slope of
     * 1.4 -- and then doing what a shore does: swash, run-up, wetness, and foam. A 130-unit
     * cliff is not a foreshore, so every one of those was the engine correctly serving a
     * question nobody meant to ask.
     *
     * Switching the foam flags off could not fix it, because none of them is where the foam
     * came from. water_frag computes shore foam from `Shoal` and breaking crests from the
     * depth limit, and BOTH are functions of the bed: with none, the shoal factor is 1
     * everywhere and both terms are identically zero -- the shader says so itself, and
     * notes it needs no `bedAvailable` guard for exactly that reason. Breaking is also the
     * one whitecap source the Gerstner path carries, which is why the surf survived turning
     * the spectral model off.
     *
     * NULL is the documented normal case and not a degraded one: the surface takes its
     * water column from resolved scene depth per fragment, which works against arbitrary
     * geometry rather than only a heightfield. What it gives up is SHOALING, which a crater
     * rim has no use for. The turquoise is untouched -- that is the refraction resolve of
     * real lit geometry attenuated over a depth-buffer path, never the bed callback.
     */
    water->height_at = NULL;
    water->height_ctx = NULL;

    // apps/tree's lagoon rather than the library's open-ocean blue: greener, and about
    // four times brighter. Turquoise is a pale bed seen THROUGH water -- the water tints
    // a bed, it does not make the colour -- so this and the LIT basin floor are one
    // decision, which is why build_lights aims a panel down at that floor.
    glm_vec3_copy((vec3){0.03f, 0.13f, 0.14f}, water->scatter_albedo);
    /*
     * No longer zero, and the reversal is the point. apps/tree tried a glow floor and
     * reverted it as an authored constant wearing a new name -- correct for tree, where
     * the sun lit the sea and a glow was a second answer to a solved question. Here the
     * pool is the light source of the whole cavern and the glow IS the art direction.
     *
     * It emits NOTHING, and nothing downstream of it does either. The term is
     * inscatter = scatter_albedo*incident + scatter_glow, which feeds FragColor and
     * stops; the G-buffer albedo writes scatter_albedo alone and says in its own comment
     * that the glow is not reflectance, so SSGI cannot bounce it, and every capture path
     * skips water outright so no probe or DDGI sweep can see it either.
     *
     * What it buys is the LOOK: a surface bright enough to cross the bloom threshold.
     * The rock is lit by the panels in build_lights. Two mechanisms, one appearance, and
     * conflating them is how this ends up a glowing sheet in an unlit hole.
     */
    glm_vec3_copy((vec3){0.02f, 0.20f, 0.22f}, water->scatter_glow);

    // Absorption stays the clear-seawater default -- gametest is one unit to the metre,
    // so it needs no scaling, which is the conversion tree has to do and this does not.

    // A calm sea, not the default. create_water ships a fully-developed 11.5 m/s wind
    // over 120 km of fetch -- about two metres of significant height, which on a 120 m
    // basin is a storm in a bathtub.
    /*
     * Spectral, not Gerstner. The library defaults to the closed-form octaves, which are
     * lake scale; this is a sea, and the cascades are what give it a real spectrum.
     *
     * One consequence to know rather than discover: FFT is the path that reports a
     * JACOBIAN, and crest foam is selected from it with no uniform in the way. So whitecaps
     * are back on the table here even with no bed, and the only lever on them is sea state
     * -- a surface that does not fold hard enough to reach the onset compression grows no
     * foam. Hence the calm authoring below; raising the wind is what would bring them back.
     */
    water->wave_model = WATER_WAVES_FFT;

    /*
     * Calmer than the Gerstner authoring was, because FFT actually READS this and Gerstner
     * never did -- it has no sea state to ask.
     *
     * At 6 m/s over 15 km the seed reported Hs 1.92 m with a slope variance of 0.0996
     * against Cox-Munk's 0.0337: nearly three times the reference, on a pool about 100
     * units across. That is a surface folding hard enough to feed the Jacobian crest foam,
     * which has no switch -- so the sea state IS the switch, and leaving it at storm
     * numbers while claiming the calm suppresses whitecaps would have been a comment that
     * lied. A train is calmed by lower wind or shorter fetch; both, here.
     */
    water->sea.wind_sea.wind_speed = 2.5f;
    water->sea.wind_sea.fetch = 900.0f;

    scene->water = water; // the scene owns it and frees it
    printf("Ocean: level %g, bed %g to %g, lagoon scatter\n", (double)water->level,
           (double)GROTTO_SEABED_Y, (double)GROTTO_SEABED_FAR);
}

// A light, its node and its registration. Three lights want the same five lines, and the
// node is not optional: a light on no node is never placed by the transform walk.
static Light* add_scene_light(Scene* scene, const LightDesc* desc) {
    Light* light = create_light(desc);
    if (!light) {
        fprintf(stderr, "Failed to create light %s\n", desc->name ? desc->name : "(unnamed)");
        return NULL;
    }
    scene_add_light(scene, light);
    SceneNode* node = create_node();
    node_set_name(node, desc->name);
    node_set_light(node, light);
    node_add_child(scene->root_node, node);
    return light;
}

/*
 * The lights that are PLACED rather than inherited: a cone over the platform, and the
 * pool at the bottom of the shaft lighting the rock around it.
 *
 * This replaces app.c's three-point rig, which lit the basin harder than the sky did and
 * said nothing about it. Its three descs set no .type, so they are DIRECTIONAL, and no
 * .cast_shadows, so shadow_map_index stays -1 and pbr_frag reads that as "no shadow test"
 * and leaves visibility at 1.0. Three downward directionals therefore lit the basin floor
 * at full NdotL through 140 units of rock, with no occlusion of any kind. No amount of
 * cliff makes a pit dark while that is running, which is why this is the phase that
 * carries the look and not the geometry.
 */
static void build_lights(Scene* scene) {
    if (!scene || !scene->root_node)
        return;

    // The platform's cone. The cutoffs are HALF-ANGLES in radians, and the desc's zero
    // means 12.5 and 15 degrees -- a spotlight circle in the middle of a 50x50 plate.
    // That plate's half-diagonal is 35.4, so from 45 up its corner subtends 38 degrees;
    // the outer edge carries past that so the falloff finishes OFF the plate rather than
    // across it, which is the difference between a lit stage and a visible pool of light.
    const float spot_y = 45.0f;
    const float inner_deg = 38.0f, outer_deg = 47.0f;
    /*
     * RAKED off vertical, and the lamp is MOVED rather than merely turned.
     *
     * Straight down is the one direction that lights a floor fully and a standing figure
     * not at all: the plate's normal is +Y and takes N.L = 1, while a torso's normal is
     * horizontal and takes nearly zero. Only shoulders and scalp caught the key, so an
     * arbitrary --puppet read as a silhouette against its own brightly lit stage -- and
     * auto-exposure compounded it, metering the plate and mapping the character wherever
     * it fell. Nothing about that is specific to a metallic asset; a diffuse surface with
     * a horizontal normal receives the same nothing.
     *
     * TURNING the light is not enough. At 44.5 above the plate, rotating the direction
     * alone drags the pool `drop * tan(rake)` off centre -- 44.5 units here, on a plate
     * whose half-extent is 25. So the lamp moves to an offset position and aims back at
     * the plate's centre, and the throw grows from 44.5 to 62.9, which the intensity
     * compensates by its square or the plate loses a stop and the meter re-opens.
     *
     * The cone is deliberately UNCHANGED. An oblique footprint is an ellipse, so the
     * plate is no longer evenly lit -- 0.87 stops corner to corner against 0.10 hung
     * straight down -- but that falloff is a gradient rather than a cutoff and still
     * finishes off the plate, which is what the cutoffs above were sized for. The angle
     * is 45 and not halfway because the two costs run opposite ways: nearly all of that
     * spread is spent by 25 degrees, while the figure keeps gaining past 40. Past 55 the
     * figure turns back over, the lamp having gone oblique enough to lose its top.
     */
    const float rake_deg = 45.0f, azimuth_deg = 35.0f;
    const float drop = spot_y - GAMETEST_FLOOR_HALF_Y;
    const float reach = drop * tanf(glm_rad(rake_deg));
    const float key_x = reach * sinf(glm_rad(azimuth_deg));
    const float key_z = reach * cosf(glm_rad(azimuth_deg));
    LightDesc spot = {
        .name = "platform_key",
        .type = LIGHT_SPOT,
        .position = {key_x, spot_y, key_z},
        // Not a unit vector: light_set_direction stores what it is handed and the cluster
        // pack normalizes, which is why the rig in app.c passes non-unit vectors too.
        .direction = {-key_x, -drop, -key_z},
        .color = {1.0f, 0.96f, 0.90f},
        // CANDELA: a spot is punctual, where a panel is in nits. Scaled by the square of
        // the throw so raking the lamp back does not dim the plate it still has to light.
        .intensity = 100000.0f * (drop * drop + reach * reach) / (drop * drop),
        /*
         * Down past the rim, not just past the plate -- and unlike a panel's, a spot's
         * range genuinely windows the falloff, so this number is the look and not just a
         * culling bound.
         *
         * At spot_y + 30 it died at y = -30 while the shaft's upper rock sits near -47,
         * so that rock was outside the range and the panels are 130 units below it again:
         * with the fill rig retired, the band between plate and pool had no light in it
         * at all. Reaching the rim lets inverse-square do the work instead -- the wall at
         * -47 is twice as far from the lamp as the plate, so it lands near a quarter
         * brightness and keeps fading down the shaft. That gradient IS the fall.
         *
         * It does NOT fix the black wedge that used to swing across the plate; that was
         * the cluster index pool overflowing, and the panel ranges below are what fixed
         * it. Two separate faults that looked like one.
         */
        .range = spot_y + 120.0f,
        .inner_cutoff = glm_rad(inner_deg),
        .outer_cutoff = glm_rad(outer_deg),
        .cast_shadows = true, // a perspective map, so the ramp and steps cast on the plate
    };
    add_scene_light(scene, &spot);

    /*
     * The pool, as TWO one-sided panels back to back at the waterline.
     *
     * An LTC panel is the only mechanism here that puts light from a horizontal surface
     * onto a wall, and it suits this exactly: panels are single-sided, so one facing up
     * lights every wall above it and spends nothing on submerged rock.
     *
     * TWO, back to back at the waterline, doing different jobs at very different strengths.
     *
     * DOWN is the one that matters, and it exists for the water's COLOUR rather than for
     * the rock. The surface resolves as bed * exp(-absorption * path) + inscatter, where
     * the bed term is the refraction resolve of real geometry -- so an unlit basin floor
     * makes the shallows read black and leaves only deep water coloured. It lights the
     * floor that is seen through, which is what the turquoise actually is.
     *
     * UP is a wash and no longer a light source. It carried the walls at 55 nits when the
     * fill rig had just been retired and nothing else reached down here; the platform spot
     * reaches past the rim now and does that job properly, with inverse-square giving the
     * fade a single panel never could. At 1 nit this is the suggestion of bounce off the
     * pool onto the rock immediately above it -- the part the spot, coming from overhead,
     * cannot produce. Anything near the old value is the spot's job done twice.
     *
     * Neither lights the WATER: water_key_light goes through scene_key_directional, which
     * skips every non-directional light. So they cannot double-brighten the surface they
     * sit in, and its own look stays the glow plus the sky.
     */
    const float pool = 2.0f * GROTTO_BASIN_HALF * 0.75f; // a little wider than the pool

    LightDesc up = {
        .name = "water_glow_up",
        .type = LIGHT_AREA,
        .position = {0.0f, GROTTO_WATER_Y + 0.25f, 0.0f},
        // Not a default to inherit: a zero direction here is straight DOWN, which would
        // aim this at the seabed the other panel already covers and leave the walls unlit.
        .direction = {0.0f, 1.0f, 0.0f},
        .up = {0.0f, 0.0f, 1.0f}, // the panel then lies in the XZ plane
        // Pale rather than saturated: a strongly cyan light makes every surface cyan
        // whatever its albedo, and the vivid turquoise belongs to the water itself.
        .color = {0.62f, 0.93f, 0.89f},
        .intensity = 1.0f, // nits -- a wash, not a source; see the note above
        .size = {pool, pool},
        // Tightened WITH the intensity. A panel this dim delivers nothing at 220 units, so
        // claiming froxels out there is pure cost against a pool that averages two lights
        // per froxel -- and leaving range at 0 would derive a radius from the far-field
        // irradiance and put it in all 3072 of them, which is what starved the grid before.
        .range = 70.0f,
    };
    add_scene_light(scene, &up);

    LightDesc down = {
        .name = "water_glow_down",
        .type = LIGHT_AREA,
        .position = {0.0f, GROTTO_WATER_Y - 0.25f, 0.0f},
        // Stated, not inherited: a zero direction is already straight down, but a panel
        // whose whole purpose is which way it faces should not depend on a default.
        .direction = {0.0f, -1.0f, 0.0f},
        .up = {0.0f, 0.0f, 1.0f}, // the panel then lies in the XZ plane
        // Pale, not saturated. A strongly cyan light makes every surface cyan whatever its
        // albedo, and the vivid turquoise belongs to the WATER, where scatter_glow puts it.
        .color = {0.62f, 0.93f, 0.89f},
        .intensity = 22.0f,   // nits; it only has to make the bed readable through water
        .size = {pool, pool}, // a zero here would silently mean 50 by 50
        /*
         * BOUNDED, and leaving it derived is what put a black wedge in the frame.
         *
         * A panel's range shrinks only the CULL SPHERE -- the area branch returns before
         * the punctual falloff -- so this costs the look nothing and buys the culler
         * everything. Derived, light_cull_radius inverts a panel's far-field irradiance
         * against a 1/256 floor and lands in the THOUSANDS of units, so the two panels
         * that used to be here sat in all 3072 froxels of the grid. Three clusterable
         * lights then want 9216 index slots against a 6144 cap, and _assign_index_offsets
         * answered the overflow by zeroing whole clusters -- those froxels lost every
         * local light and rendered black. The grid is camera-aligned, so the dead region
         * swung with the view and read as a cutoff that followed the player around.
         *
         * 90 is all this one needs: a seabed a few units under it, across a basin 120
         * wide. Every froxel it does not claim is one the index pool keeps.
         */
        .range = 90.0f,
    };
    add_scene_light(scene, &down);

    printf("Lights: platform cone at y=%g (%g to %g deg), pool panels %gx%g at %g\n",
           (double)spot_y, (double)inner_deg, (double)outer_deg, (double)pool, (double)pool,
           (double)GROTTO_WATER_Y);
}

/*
 * The dark, as a box over the shaft rather than a dimmer on the scene.
 *
 * Three of the four terms that reach the basin floor are NOT occluded by geometry, so no
 * arrangement of rock darkens it: IBL diffuse is texture(irradianceMap, N), a cube lookup
 * on the normal alone, its specular twin reads the reflection vector, and the baked sky
 * lights its own virtual ground so even downward directions in the cube carry sun. The
 * fill rig was the fourth and build_lights already retired it. Of what is left, the only
 * handle is ibl->intensity, which dims the platform and the sky by exactly as much.
 *
 * A fog VOLUME is the one lever that is bounded in world space, so it can make the pit
 * dark BECAUSE it is a pit and leave everything above it alone. It sits under the rim and
 * not under the platform: the long fall stays clear air, which is what keeps the middle
 * of the drop empty rather than milky, and the walls fade in as you reach them.
 */
static void build_cavern_fog(Scene* scene, PostFX* fx) {
    if (!scene)
        return;

    // Top at the rim, floor below the basin, and wider than the walls so the ramp-in
    // happens inside rock rather than in open air at the edge of the box.
    const float top = GROTTO_CLIFF_TOP;
    const float bottom = GROTTO_SEABED_FAR - 6.0f;
    FogVolume vol = {
        .center = {0.0f, 0.5f * (top + bottom), 0.0f},
        .half_extent = {GROTTO_BASIN_HALF + 10.0f, 0.5f * (top - bottom),
                        GROTTO_BASIN_HALF + 10.0f},
        // Thin, because the box is tall. Optical depth is density times the distance
        // looked through, so 0.018 over the shaft's ~140 units left a transmittance near
        // 0.08 -- ninety percent of the rock's own light gone before it reached the eye,
        // which is most of why the pit read as a void rather than as a deep space. At
        // 0.012 the far wall stays visible and the haze still separates near from far.
        .density = 0.012f,
        .feather = 12.0f,
        // Only slightly toward the pool's turquoise. The panels already carry the colour
        // of this place, and a saturated medium on top of a saturated key light is how
        // the whole cavern collapses into one hue -- the fog should read as depth, not as
        // a second coat of paint. White here would leave the surrounding air alone.
        .tint = {0.38f, 0.64f, 0.62f},
    };
    if (!scene_add_fog_volume(scene, &vol))
        fprintf(stderr, "Cavern fog: volume refused\n");

    if (!fx)
        return;

    /*
     * The medium's own light, and it must not be zero.
     *
     * froxel_inject seeds in-scatter as `S = ambientColor` and then adds each directional
     * scaled by `sunBoost`. Zero both and S is exactly zero, which does not make a dim
     * medium -- it makes a PURELY ABSORBING one. The composite is
     * scene * transmittance + inscatter, so with no in-scatter every extra metre of fog
     * drives the pixel toward black, and the basin renders as a growing black wedge that
     * eats the rock behind it. Shipped exactly that, and the tint cannot rescue it: tint
     * colours the EXTINCTION, not the light scattered back toward the eye.
     *
     * So the ambient is a dim turquoise rather than nothing: it is the water's glow
     * hanging in the air of the shaft, and it is what makes depth read as depth instead
     * of as a hole. fog_ambient_from_sky still has to go, or the sky stamps its zenith
     * radiance over this every frame and the pit fills with daylight --
     * postfx_set_fog_ambient clears that flag as well as writing the value, which is why
     * it is a call and not a field write.
     *
     * fog_sun_boost stays at zero, and that one was right: it gathers shaft in-scatter
     * through the same visibility test the shadow map answers, and that test reports LIT
     * for anything outside the cascade, which down here is most of the basin. There are
     * no god rays in a cave lit from its floor.
     */
    postfx_set_fog_ambient(fx, (vec3){0.02f, 0.09f, 0.10f});
    fx->fog_sun_boost = 0.0f;

    /*
     * No GLOBAL air term: this scene wants fog in the cavern and nowhere else.
     *
     * A volume arms the froxel pass without fog_enabled, which is the documented way to
     * get local fog -- but arming the pass also brings up the global height fog, and its
     * defaults are density 0.02 with fog_floor_y at 0.0, which is exactly the platform's
     * own height. Density is MAXIMUM at that floor and decays over four units above it,
     * so the plate sits in the thickest part of a layer nobody asked for.
     *
     * Zeroed on its own merits and NOT as a fix: it was measured against the black wedge
     * across the plate and changed nothing there, which is how that suspicion died. The
     * wedge was the cluster index pool overflowing. What this removes is a ground haze
     * the scene never asked for, and it is recorded here because the next reader will
     * otherwise reach for fog_enabled and find it already false.
     */
    fx->fog_density = 0.0f;

    // The froxel volume is spent over the camera's depth range, and 60 is the default.
    // The shaft floor sits ~190 under the platform, so at the default every slice is
    // used up long before the fog is reached and the box renders as nothing at all.
    if (fx->fog_far < 400.0f)
        fx->fog_far = 400.0f;

    printf("Cavern fog: box y %g to %g, half %g, density %g, sun boost off\n", (double)bottom,
           (double)top, (double)(GROTTO_BASIN_HALF + 10.0f), (double)vol.density);
}

// Create a door with hinge constraint
static void create_door(Game* game, vec3 position) {
    EntityManager* em = game_get_entity_manager(game);
    PhysicsWorld* physics = game_get_physics_world(game);
    Scene* scene = game_get_scene(game);

    if (!em || !physics || !scene)
        return;

    // Door dimensions
    float door_width = 4.0f;
    float door_height = 6.0f;
    float door_thickness = 0.3f;

    // Create door frame (static anchor point)
    Entity* frame = create_entity(em, "door_frame");
    vec3 frame_pos;
    glm_vec3_copy(position, frame_pos);
    frame_pos[1] = door_height / 2.0f; // Center vertically
    glm_vec3_copy(frame_pos, frame->position);

    // Frame visual (post at hinge edge)
    vec3 frame_size = {0.2f, door_height / 2.0f, 0.2f};
    vec3 frame_color = {0.4f, 0.3f, 0.2f};
    SceneNode* frame_node = create_box_node(scene, frame_size, frame_color, false);
    node_set_name(frame_node, "door_frame");
    frame->node = frame_node;

    // Frame physics (static)
    PhysicsShapeDesc frame_shape = {
        .type = SHAPE_BOX, .box.half_extents = {0.2f, door_height / 2.0f, 0.2f}, .density = 0.0f};
    RigidBody* frame_body =
        entity_add_rigid_body(frame, physics, &frame_shape, MOTION_STATIC, OBJ_LAYER_STATIC);

    // Create door panel (dynamic)
    door_entity = create_entity(em, "door");
    vec3 door_pos;
    glm_vec3_copy(position, door_pos);
    // Position door so its left edge aligns with frame's right edge (no overlap)
    float frame_half_width = 0.2f;
    door_pos[0] += frame_half_width + door_width / 2.0f;
    door_pos[1] = door_height / 2.0f;
    glm_vec3_copy(door_pos, door_entity->position);

    // Door visual
    vec3 door_size = {door_width / 2.0f, door_height / 2.0f, door_thickness / 2.0f};
    vec3 door_color = {0.6f, 0.4f, 0.2f}; // Wood brown
    SceneNode* door_node = create_box_node(scene, door_size, door_color, false);
    node_set_name(door_node, "door");
    door_entity->node = door_node;

    // Door physics (dynamic) - wooden door ~20-30kg
    // Volume = 4m * 6m * 0.3m = 7.2m³, density = 4 gives ~29kg
    PhysicsShapeDesc door_shape = {
        .type = SHAPE_BOX,
        .box.half_extents = {door_width / 2.0f, door_height / 2.0f, door_thickness / 2.0f},
        .density = 4.0f};
    RigidBody* door_body =
        entity_add_rigid_body(door_entity, physics, &door_shape, MOTION_DYNAMIC, OBJ_LAYER_DYNAMIC);

    // Create hinge constraint
    // Both anchors meet at frame's right edge = door's left edge
    ConstraintDesc hinge_desc = {.type = CONSTRAINT_HINGE,
                                 .anchor_a = {frame_half_width, 0, 0},   // Right edge of frame
                                 .anchor_b = {-door_width / 2.0f, 0, 0}, // Left edge of door
                                 .num_velocity_steps = 10,               // Moderate rigidity
                                 .num_position_steps = 4,
                                 .hinge = {
                                     .axis = {0, 1, 0},           // Vertical hinge axis
                                     .min_angle = -GLM_PI * 0.6f, // Allow swing when pushed
                                     .max_angle = GLM_PI * 0.6f,
                                     .max_friction_torque = 0.5f // Low friction for easy swing
                                 }};

    door_hinge = create_constraint(physics, frame_body, door_body, &hinge_desc);
    if (door_hinge) {
        physics_world_add_constraint(physics, door_hinge);
        printf("Door created with hinge constraint at (%.1f, %.1f, %.1f)\n", position[0],
               position[1], position[2]);
    }
}

/*
 * One crate, from explicit arguments.
 *
 * BOTH ways in go through here -- the keypress rolls the arguments, the loader
 * reads them back from the file -- so a restored crate is built by the same
 * code that built the original rather than by a second copy free to drift from
 * it. Splitting this out is what makes the crates saveable at all: the size and
 * colour used to be rolled inline and then stored only inside a Material and a
 * Jolt shape, where nothing could read them back, and the rand01 sequence that
 * produced them cannot be replayed without a draw count nobody keeps.
 */
static Entity* spawn_box(Game* game, const char* name, vec3 pos, float half, vec3 color) {
    EntityManager* em = game_get_entity_manager(game);
    PhysicsWorld* physics = game_get_physics_world(game);
    Scene* scene = game_get_scene(game);

    if (!em || !physics || !scene)
        return NULL;

    Entity* box = create_entity(em, name);
    if (!box)
        return NULL;
    glm_vec3_copy(pos, box->position);

    vec3 half_extents = {half, half, half};
    SceneNode* node = create_box_node(scene, half_extents, color, false);
    node_set_name(node, name);
    box->node = node;

    // Add physics body (density ~50 kg/m3, like a light wooden crate)
    PhysicsShapeDesc shape = {
        .type = SHAPE_BOX, .box.half_extents = {half, half, half}, .density = 50.0f};
    entity_add_rigid_body(box, physics, &shape, MOTION_DYNAMIC, OBJ_LAYER_DYNAMIC);
    return box;
}

// The recipe's arguments, in the shape the save file carries them.
static cJSON* box_params(vec3 pos, float half, vec3 color) {
    cJSON* params = cJSON_CreateObject();
    if (!params)
        return NULL;
    const double p[3] = {pos[0], pos[1], pos[2]};
    const double c[3] = {color[0], color[1], color[2]};
    cJSON_AddItemToObject(params, "position", cJSON_CreateDoubleArray(p, 3));
    cJSON_AddNumberToObject(params, "half", half);
    cJSON_AddItemToObject(params, "color", cJSON_CreateDoubleArray(c, 3));
    return params;
}

static bool params_vec3(const cJSON* params, const char* key, vec3 out) {
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(params, key);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != 3)
        return false;
    for (int i = 0; i < 3; i++) {
        const cJSON* e = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsNumber(e))
            return false;
        out[i] = (float)e->valuedouble;
    }
    return true;
}

/*
 * The registered "box" recipe. The position it reads is only a starting point:
 * the entity's own saved pose is applied over it a moment later, which is what
 * puts a restored crate where it actually was rather than where it first fell.
 */
static Entity* spawn_box_from_save(EntityManager* em, const char* name, const cJSON* params,
                                   void* user) {
    (void)em;
    Game* game = (Game*)user;
    vec3 pos = {0.0f, 0.0f, 0.0f};
    vec3 color = {0.5f, 0.5f, 0.5f};
    if (!params || !params_vec3(params, "position", pos) || !params_vec3(params, "color", color)) {
        fprintf(stderr, "gametest: box params are malformed; '%s' is not restored\n", name);
        return NULL;
    }
    const cJSON* half = cJSON_GetObjectItemCaseSensitive(params, "half");
    if (!cJSON_IsNumber(half)) {
        fprintf(stderr, "gametest: box '%s' has no size; not restored\n", name);
        return NULL;
    }
    return spawn_box(game, name, pos, (float)half->valuedouble, color);
}

// Spawn a falling box at a random position above the scene
static void spawn_falling_box(Game* game) {
    // The counter advances only once the crate exists. It is SAVED state now,
    // so a refused spawn that burned a name would leave a permanent gap in what
    // the file carries.
    char name[32];
    snprintf(name, sizeof(name), "box_%d", box_count);

    // Random position above the scene, random colour, random size
    float x = (rand01() - 0.5f) * 20.0f;
    float z = (rand01() - 0.5f) * 20.0f;
    vec3 pos = {x, 15.0f + rand01() * 5.0f, z};
    vec3 color = {0.3f + rand01() * 0.7f, 0.3f + rand01() * 0.7f, 0.3f + rand01() * 0.7f};
    float half = 0.5f + rand01() * 1.0f;

    if (!spawn_box(game, name, pos, half, color))
        return;
    box_count++;

    // Recorded as it happens, by the only code that holds these numbers.
    if (save_system)
        save_note_spawn(save_system, name, "box", box_params(pos, half, color));

    printf("Spawned %s at (%.1f, %.1f, %.1f)\n", name, pos[0], pos[1], pos[2]);
}

// Track if player is touching door this frame (declared before on_update uses it)
static bool player_touching_door = false;

/*
 * The state an entity walk cannot reach.
 *
 * A save that carries every entity still restores a world whose player faces
 * the wrong way, whose next crate collides with an existing name, and whose
 * door forgets it was mid-swing: a facing angle, a spawn counter and two
 * deferred flags live in file statics here and nowhere the engine can see.
 *
 * Addressed directly, because they are plain variables and save.h has a row for
 * exactly that. The two alternatives are both worse: gathering them into a
 * struct for the serializer's benefit would rewrite door, yaw and catch logic
 * across a file the anim, audio, ui and gamepad groups and two goldens all
 * read, and a generated accessor pair per variable states each type twice -- in
 * the row and in the cast -- with nothing checking the two agree.
 */
static const SaveField SAVE_APP_FIELDS[] = {
    SAVE_ROW_AT(SAVE_INT, "box_count", &box_count),
    SAVE_ROW_AT(SAVE_FLOAT, "player_yaw", &player_yaw),
    SAVE_ROW_AT(SAVE_FLOAT, "chaser_yaw", &chaser_yaw),
    SAVE_ROW_AT(SAVE_FLOAT, "heart_timer", &heart_timer),
    SAVE_ROW_AT(SAVE_FLOAT, "catch_cooldown", &catch_cooldown),
    SAVE_ROW_AT(SAVE_BOOL, "door_open_pending", &door_open_pending),
    SAVE_ROW_AT(SAVE_FLOAT, "door_open_velocity", &door_open_velocity),
    SAVE_ROW_AT(SAVE_BOOL, "player_touching_door", &player_touching_door),
};
#define SAVE_APP_COUNT   ((int)(sizeof(SAVE_APP_FIELDS) / sizeof(SAVE_APP_FIELDS[0])))
#define SAVE_APP_VERSION 1

// Character contact callback for door interaction
static void on_player_contact(CharacterController* cc, Entity* hit_entity, vec3 contact_position,
                              vec3 contact_normal, void* user_data) {
    (void)contact_position;
    (void)user_data;

    // Check if we hit the door
    if (hit_entity == door_entity && door_hinge) {
        player_touching_door = true;

        RigidBody* door_rb = entity_get_rigid_body(door_entity);
        if (door_rb)
            rigid_body_activate(door_rb);

        // Get player velocity to determine push direction
        vec3 player_vel;
        character_controller_get_velocity(cc, player_vel);

        // Only push door if player is moving
        if (glm_vec3_norm(player_vel) > 0.1f) {
            // Determine push direction based on velocity and position relative to door
            vec3 to_door;
            glm_vec3_sub(door_entity->position, player_entity->position, to_door);
            float cross_y = to_door[0] * player_vel[2] - to_door[2] * player_vel[0];

            // Defer motor change to update (can't call from callback - threading)
            door_open_pending = true;
            door_open_velocity = (cross_y > 0) ? 6.0f : -6.0f;
        }
    }
}

// Collision callback
static void on_collision(const CollisionEvent* event, void* user_data) {
    (void)user_data;

    // Only handle BEGIN events for gameplay
    if (event->type != COLLISION_BEGIN)
        return;

    const char* name_a = event->entity_a ? event->entity_a->name : "?";
    const char* name_b = event->entity_b ? event->entity_b->name : "?";

    // Check if player hit the door
    bool player_hit_door = (event->entity_a == player_entity && event->entity_b == door_entity) ||
                           (event->entity_b == player_entity && event->entity_a == door_entity);

    if (player_hit_door) {
        // Don't apply explicit impulse - kinematic player contact forces push door naturally
        // This is gentler and less likely to overwhelm the constraint solver
        printf("Player touching door\n");
    }

    // Log player collisions
    bool player_involved = (event->entity_a == player_entity || event->entity_b == player_entity);
    if (player_involved) {
        printf("Player collision: %s <-> %s\n", name_a, name_b);
    }
}

// The footstep events the walk and run cycles carry: one per plant, at the
// two extremes of the cos-phased swing.
static void add_footsteps(Animation* clip) {
    if (!clip)
        return;
    animation_add_event(clip, 0.0f, "step_l");
    animation_add_event(clip, clip->duration * 0.5f, "step_r");
}

// The rate to play a clip at so the ground goes past its feet at `want`. This is the
// whole of stride matching and it is one division -- the difficulty is entirely in the
// denominator, which nothing outside the animator can compute.
//
// The bounds are not tuning. Below the lower one a walk reads as a stagger and above the
// upper one as a cartoon sprint, so past them the honest answer is that this clip cannot
// carry that speed and the caller should be blending to another; stretching further only
// trades a sliding foot for a worse-looking one. A source with no stride plays at 1.
static float locomotion_rate(const Animator* animator, float want) {
    const float implied = animator_stride_speed(animator);
    if (implied <= 1e-4f)
        return 1.0f;
    const float rate = want / implied;
    return rate < ANIM_RATE_MIN ? ANIM_RATE_MIN : (rate > ANIM_RATE_MAX ? ANIM_RATE_MAX : rate);
}

// The locomotion space, and its axis. When the clips can be measured the entries sit at
// the WORLD speeds they imply and the knob is metres per second; when they cannot, the
// knob is a fraction of PLAYER_SPEED as it was before spec 12.10.
//
// The absolute axis is worth the branch: on a fraction of a constant, the one speed foot
// locking works at is a knob position of 0.19, so the demo walked at its clip's own stride
// with 62 per cent of a T-pose blended over it. It also deletes a coupling -- the knob had
// to divide by the compile-time PLAYER_SPEED and never by the --speed cap, or the gear
// re-normalised and --speed stopped meaning anything. An absolute axis has no gear to
// re-normalise.
//
// A clip that REFUSES is only fatal for the moving entries. A standing clip lays down no
// ground and 0 is the right stride for it; a walk that could not be measured is the reason
// there is no axis, because only this function knows which entries were supposed to move.
// The WORLD speed a clip's own feet imply on this rig, or 0 when they imply none. The
// engine measures in model units and says so; the scale on the rig's node is the caller's
// to apply, and this is the one place that applies it.
static float clip_world_stride(Skeleton* skeleton, const Animation* clip) {
    if (!skeleton || !clip)
        return 0.0f;
    // Resolved, not looked up: this app names bones one way and a rig it is pointed at
    // may name them another. A model whose author called a foot `leg left ankle` measures
    // here exactly as one that spells it `cetra_rig:LeftFoot`.
    const int ankle[2] = {skeleton_resolve_bone(skeleton, "cetra_rig:LeftFoot"),
                          skeleton_resolve_bone(skeleton, "cetra_rig:RightFoot")};
    const int toe[2] = {skeleton_resolve_bone(skeleton, "cetra_rig:LeftToeBase"),
                        skeleton_resolve_bone(skeleton, "cetra_rig:RightToeBase")};
    float model = 0.0f;
    if (!animation_stride_speed(clip, skeleton, ankle, toe, &model, NULL))
        return 0.0f;
    return model * PLAYER_SCALE;
}

// The WORLD speed a clip STATES it travels at (spec 12.18), or 0 where it states
// nothing. Same units, same scale, same division of labour as the stride above -- and
// the opposite question: that one asks the feet what the ground must be doing, this
// one reads what the animator wrote for the body.
static float clip_world_travel(int root, const Animation* clip) {
    vec3 travel = {0.0f, 0.0f, 0.0f};
    if (!animation_root_travel(clip, root, travel, NULL))
        return 0.0f;
    const float seconds = clip->duration / clip->ticks_per_second;
    if (seconds <= 0.0f)
        return 0.0f;
    return hypotf(travel[0], travel[2]) / seconds * PLAYER_SCALE;
}

// The water's own space: a treading end and a stroking end, blended on the same knob the
// ground uses. Its axis is STATED and not measured, and that is the difference between
// the two media rather than an omission -- a stride is how fast the ground goes past a
// foot that is pressing it, and a swimmer's feet press nothing. `animation_stride_speed`
// refuses the stroke for exactly that reason, which is the right answer, so the top of
// this axis is the speed the swimmer actually travels at and the playback rate stays 1.
static void build_aquatic(Animation* float_clip, Animation* swim, float top_speed) {
    aquatic_count = 0;
    if (!float_clip || !swim || float_clip == swim || top_speed <= 0.0f)
        return;
    aquatic[0] = (AnimatorEntry){float_clip, 0.0f, 0.0f};
    aquatic[1] = (AnimatorEntry){swim, top_speed, 0.0f};
    aquatic_count = 2;
    printf("Swim axis is 0 to %.2f m/s, stated: a stroke has no stride to measure\n",
           (double)top_speed);
}

static void build_locomotion(Skeleton* skeleton, Animation* idle, Animation* walk, Animation* run) {
    locomotion[0] = (AnimatorEntry){idle, 0.0f, 0.0f};
    locomotion[1] = (AnimatorEntry){walk, 0.5f, 0.0f};
    locomotion[2] = (AnimatorEntry){run, 1.0f, 0.0f};
    locomotion_count = 3;
    if (!skeleton || !idle || !walk || !run)
        return;

    // What the clips STATE comes first, and where they state anything it is the whole
    // answer: the character travels exactly as far as the animation says, the axis is
    // the speeds the clips themselves reach, and playback stays at rate 1 because
    // scaling it to hit a speed the player asked for is stride matching wearing this
    // feature's coat. Nothing in the committed corpus states a travel except the four
    // clips authored for it, so every imported rig falls through to the measurement
    // below exactly as it did.
    const bool one_clip = run == walk;
    const int root = skeleton_root_bone(skeleton);
    const float walk_travel = clip_world_travel(root, walk);
    const float run_travel = one_clip ? walk_travel : clip_world_travel(root, run);
    if (walk_travel > 0.0f && (one_clip || run_travel > walk_travel)) {
        locomotion[1].position = walk_travel;
        locomotion[2].position = run_travel;
        if (one_clip)
            locomotion_count = 2;
        locomotion_axis = LOCO_TRAVEL;
        if (!speed_override)
            player_speed = locomotion[locomotion_count - 1].position;
        printf("Locomotion travels 0 to %.2f m/s, stated by the clips themselves; the body "
               "goes where the animation says\n",
               (double)player_speed);
        return;
    }

    // The standing entry anchors the axis at ZERO and keeps no stride of its own, even
    // though one can be measured off it: a breathing idle shifts its weight, which reads
    // as 0.13 m/s here, and an axis starting there plays the idle at the rate floor while
    // the character is standing perfectly still. What a standing clip lays down is no
    // ground, by definition rather than by measurement.
    locomotion[1].stride = clip_world_stride(skeleton, walk);
    const bool walks = locomotion[1].stride > 0.0f;
    // The two moving entries are often ONE clip -- a rig with no run gets the walk for
    // both -- and two entries at one position is not a blend space. Drop to two.
    const bool runs = run == walk ? walks : clip_world_stride(skeleton, run) > 0.0f;
    if (runs && run != walk)
        locomotion[2].stride = clip_world_stride(skeleton, run);

    if (!walks || !runs) {
        printf("Locomotion clips imply no stride (%s); travel stays a fraction of %.1f m/s "
               "and the feet will slide\n",
               walks ? run->name : walk->name, (double)PLAYER_SPEED);
        return;
    }
    if (run == walk)
        locomotion_count = 2;
    else if (locomotion[2].stride <= locomotion[1].stride) {
        printf("The run clip strides %.2f against the walk's %.2f; keeping the fractional "
               "axis\n",
               (double)locomotion[2].stride, (double)locomotion[1].stride);
        return;
    }
    // The axis becomes metres per second, so every entry sits where it belongs on it and
    // the knob is the speed itself.
    for (int i = 0; i < locomotion_count; i++)
        locomotion[i].position = locomotion[i].stride;
    locomotion_axis = LOCO_STRIDE;
    if (!speed_override)
        player_speed = locomotion[locomotion_count - 1].stride * ANIM_RATE_MAX;
    printf("Locomotion axis is %.2f to %.2f m/s from the clips themselves; full stick is "
           "%.2f\n",
           (double)locomotion[0].stride, (double)locomotion[locomotion_count - 1].stride,
           (double)player_speed);
}

static void on_anim_event(Animator* animator, const char* name, void* user) {
    (void)animator;
    (void)user;
    if (step_sound && (!strcmp(name, "step_l") || !strcmp(name, "step_r")))
        audio_sound_play(step_sound);
}

// Put `rig` under a holder that becomes the entity's node, and return the node
// the app may pose.
//
// TWO levels, and both are load-bearing. sync_entity_transforms overwrites the
// entity node's local every step, so the drop from capsule centre to feet and
// the facing yaw cannot live there -- they go on the inner node.
//
// `rig` must be a node of its own, never the scene root. It used to accept the
// root and move its CHILDREN, which was right only while the root held nothing
// but the model: the floor is added before the player, so that scooped up the
// floor and parented it to the character, and WASD drove the ground around with
// him. take_puppet_root does the wrapping at import instead, before anything
// else is in the scene.
static SceneNode* attach_rig(Scene* scene, Entity* entity, SceneNode* rig, float drop,
                             float scale) {
    SceneNode* holder = create_node();
    node_set_name(holder, entity->name);
    SceneNode* inner = create_node();
    node_set_name(inner, "rig");
    node_add_child(holder, inner);
    node_add_child(inner, rig);
    node_add_child(scene->root_node, holder);
    glm_translate_make(inner->original_transform, (vec3){0.0f, drop, 0.0f});
    glm_scale_uni(inner->original_transform, scale);
    entity->node = holder;
    return inner;
}

// Let every texture already in flight LAND, before the scene holding the materials
// they will be set on is freed, or before a second scene claims the loader.
//
// Two things need this and neither has an API. The async callback holds a raw
// Material* and there is no way to cancel one, so freeing underneath it writes into
// released memory -- a byte-write fault far away in whatever now owns that address,
// with a stack naming the texture and not the free. And a loader has one texture
// pool (async_loader.h), while embedded texture keys are "*0", "*1", ... per FILE --
// so two imported scenes in flight at once would answer each other's lookups.
static void drain_async_loader(AsyncLoader* loader, Scene* scene) {
    while (async_loader_is_busy(loader) || async_loader_pending_count(loader) > 0)
        async_loader_process_pending(loader, scene->tex_pool, 64);
}

// The imported model as a node that is NOT the scene root.
//
// create_scene_from_model_path makes the file's own node the root (spec 11.107),
// and a root cannot be re-parented under one of its own descendants -- that is a
// cycle, and every recursive walk in the engine runs until the stack is gone.
// So its children move into a wrapper HERE, at import, while the scene still
// contains nothing else. Doing it later is the bug this replaced: the root's
// children by then include the floor.
static SceneNode* take_puppet_root(Scene* scene) {
    // A node called "puppet" if the model names one, and the whole model if it does not.
    // Requiring the name meant --puppet only ever accepted the generated fixture: every
    // other rigged model in the tree was refused before a clip was read, and the app fell
    // back to the red box with a message about missing clips that was not the reason.
    SceneNode* found = node_find(scene->root_node, "puppet");
    if (!found)
        found = scene->root_node;
    if (found != scene->root_node)
        return found;
    SceneNode* wrapper = create_node();
    node_set_name(wrapper, "puppet_rig");
    // Bounded rather than drained: node_add_child refuses a cycle, and a
    // `while (children_count)` on a refusal spins forever.
    for (size_t guard = scene->root_node->children_count;
         guard > 0 && scene->root_node->children_count > 0; guard--)
        node_add_child(wrapper, scene->root_node->children[0]);
    node_add_child(scene->root_node, wrapper);
    return wrapper;
}

// Hearts weave as they rise. Written as a module rather than reached for from
// curl noise, because the ask is ONE axis with a phase, and curl noise wanders
// on three -- it reads as drift, not as a wobble.
//
// The phase comes from the particle's own `seed`, which is stable for its whole
// life, so five hearts from one burst weave independently instead of swaying in
// lockstep. Amplitude grows with age so they leave the burst tightly and spread
// as they climb.
typedef struct HeartWobble {
    float amplitude; // metres of sway at full age
    float cycles;    // full left-right sweeps over a lifetime
} HeartWobble;

static void heart_wobble_run(ParticleModule* m, ParticleEmitter* e, size_t begin, size_t end,
                             float dt, float t) {
    (void)t;
    const HeartWobble* w = (const HeartWobble*)m->params;
    ParticlePool* pool = e->pool;
    for (size_t i = begin; i < end; i++) {
        float life = pool->lifetime[i] > 0.0f ? pool->lifetime[i] : 1.0f;
        float u = pool->age[i] / life; // 0 at birth, 1 at death
        float phase = pool->seed[i] * 6.2831853f;
        // The DERIVATIVE of amplitude * sin(phase), so the sway composes with
        // the upward drift instead of fighting the integrator for position.
        float omega = w->cycles * 6.2831853f / life;
        float sway = w->amplitude * omega * cosf(phase + omega * pool->age[i]);
        pool->velocity[i][0] += sway * u * dt * 8.0f;
    }
}

static ParticleModule* particle_module_heart_wobble(float amplitude, float cycles) {
    HeartWobble* w = malloc(sizeof(HeartWobble));
    if (!w)
        return NULL;
    w->amplitude = amplitude;
    w->cycles = cycles;
    return create_particle_module("heart_wobble", PARTICLE_PHASE_UPDATE, heart_wobble_run, w);
}

// A heart, drawn into an RGBA sprite: the implicit curve
// (x^2 + y^2 - 1)^3 - x^2*y^3 <= 0, which is the standard one. Generated rather
// than shipped, like the puppet and the audio tones -- the demo carries no art.
static unsigned char* heart_pixels(int size) {
    unsigned char* px = malloc((size_t)size * size * 4);
    if (!px)
        return NULL;
    // The curve (x^2 + y^2 - 1)^3 - x^2*y^3 <= 0 spans about x in [-1.2, 1.2]
    // and y in [-1.35, 1.25]. The WINDOW has to contain all of that. The first
    // cut of this shifted y down by 0.25, which clipped the top off both lobes
    // and took the cleft with them -- and a heart without its cleft is a
    // pentagon, which is exactly what it drew.
    const float half = 1.45f;
    // Supersampled rather than distance-faded: the implicit function grows at
    // wildly different rates around the outline (fast at the point, slow at the
    // lobes), so a fixed f/0.35 falloff blurs one end while the other stays
    // hard. Coverage is uniform by construction.
    const int ss = 4;
    for (int j = 0; j < size; j++) {
        for (int i = 0; i < size; i++) {
            int hits = 0;
            for (int sy = 0; sy < ss; sy++) {
                for (int sx = 0; sx < ss; sx++) {
                    float u = ((float)i + ((float)sx + 0.5f) / ss) / (float)size;
                    float v = ((float)j + ((float)sy + 0.5f) / ss) / (float)size;
                    float x = (u * 2.0f - 1.0f) * half;
                    // Row 0 is uploaded first and lands at v = 0, which is the
                    // BOTTOM of the billboard -- so the first row has to be the
                    // heart's POINT. Writing +y up here draws it upside down.
                    float y = (v * 2.0f - 1.0f) * half;
                    float t = x * x + y * y - 1.0f;
                    if (t * t * t - x * x * y * y * y <= 0.0f)
                        hits++;
                }
            }
            unsigned char* p = &px[((size_t)j * size + i) * 4];
            p[0] = 255;
            p[1] = 58;
            p[2] = 96;
            p[3] = (unsigned char)((float)hits / (float)(ss * ss) * 255.0f + 0.5f);
        }
    }
    return px;
}

// One emitter, parked and silent until a catch moves it and turns it on.
static void create_hearts(Engine* engine, Scene* scene) {
    ShaderProgram* particle_prog = create_particle_program();
    if (!particle_prog)
        return;
    engine_add_program(engine, particle_prog);

    ParticleSystem* sys = create_particle_system("hearts");
    particle_system_set_backend(sys, create_cpu_particle_sim_backend());

    ParticleEmitter* em = create_particle_emitter("heart", 256);
    ParticleRenderer* r = create_billboard_particle_renderer(particle_prog);
    Texture* sprite = texture_load_memory_owned(scene->tex_pool, "heart_sprite", heart_pixels(96),
                                                96, 96, 4, texture_desc(true));
    if (sprite)
        billboard_renderer_set_sprite(r, sprite, 2.0f);
    // Unlit: a heart is an icon, not a surface, and lighting it makes it dim
    // whenever the character walks into shadow.
    billboard_renderer_set_lit(r, false);
    particle_emitter_set_renderer(em, r);

    heart_spawn = particle_module_spawn_rate(0.0f); // off until a catch
    particle_emitter_add_module(em, heart_spawn);
    particle_emitter_add_module(
        em, particle_module_init_box_location(
                (vec3){-1.1f * PLAYER_SCALE, 0.0f, -1.1f * PLAYER_SCALE},
                (vec3){1.1f * PLAYER_SCALE, 0.9f * PLAYER_SCALE, 1.1f * PLAYER_SCALE}));
    particle_emitter_add_module(em, particle_module_init_lifetime(1.1f, 1.9f));
    particle_emitter_add_module(em, particle_module_init_size(0.22f, 0.34f));
    particle_emitter_add_module(
        em, particle_module_init_color((vec4){1.0f, 0.25f, 0.45f, 1.0f}, 0.08f));
    // Up, and a little drag, so they rise and ease off rather than accelerate
    // out of frame.
    particle_emitter_add_module(em, particle_module_update_drift((vec3){0.0f, 1.4f, 0.0f}));
    particle_emitter_add_module(em, particle_module_heart_wobble(0.45f, 1.5f));
    particle_emitter_add_module(em, particle_module_update_rotation(-1.2f, 1.2f));
    particle_emitter_add_module(em, particle_module_update_integrate(0.96f));

    particle_system_add_emitter(sys, em);
    scene_add_particle_system(scene, sys); // the scene owns and ticks it

    heart_node = create_node();
    node_set_name(heart_node, "hearts");
    node_set_particle_system(heart_node, sys); // the node is the spawn frame
    node_add_child(scene->root_node, heart_node);
}

// A second node tree over the puppet's mesh, shared by reference: two nodes on
// one skinned mesh never batch, and each carries its own pose.
static SceneNode* clone_rig(SceneNode* puppet_root) {
    SceneNode* mesh_node = node_find(puppet_root, "puppet_mesh");
    if (!mesh_node || mesh_node->mesh_count == 0)
        return NULL;
    SceneNode* rig = create_node();
    node_set_name(rig, "twin_rig");
    node_add_mesh(rig, mesh_ref(mesh_node->meshes[0]));
    return rig;
}

// EVERY skinned mesh under a node, because a capsule's radius comes from the
// per-bone bind boxes and those are per mesh. The first one alone is what an
// imported character will not survive: raiden arrives as fifteen meshes, the
// first of them a 22-vertex accessory binding a handful of bones, so a capsule
// asked to measure anything else finds an empty box and takes the fallback.
// Eleven uniform sticks is a legal ragdoll and it melts.
#define RD_MAX_SKINNED 64
static size_t collect_skinned_meshes(const SceneNode* node, const Mesh** out, size_t cap) {
    if (!node) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < node->mesh_count && n < cap; i++) {
        if (node->meshes[i] && node->meshes[i]->is_skinned) {
            out[n++] = node->meshes[i];
        }
    }
    for (size_t i = 0; i < node->children_count && n < cap; i++) {
        n += collect_skinned_meshes(node->children[i], out + n, cap - n);
    }
    return n;
}

static void on_init(Game* game) {
    printf("Game initialized with physics!\n");

    Engine* engine = game->engine;

    // Get shaders
    pbr_shader = engine_get_program(engine, CETRA_PROGRAM_PBR);
    ShaderProgram* xyz = engine_get_program(engine, CETRA_PROGRAM_XYZ);

    // The scene IS the puppet's when there is one: a material belongs to the
    // scene that registered it, so a rig imported into a second scene cannot
    // be re-parented into this one. Without a puppet, or when it fails to
    // load, the scene is built from nothing as it always was.
    Scene* scene = NULL;
    SceneNode* puppet_root = NULL;
    if (!no_puppet) {
        scene = create_scene_from_model_path(puppet_path, NULL, engine->async_loader);
        if (scene) {
            puppet_root = take_puppet_root(scene);
            // A RIG is the requirement; clips are not. A model can ship a skeleton and no
            // animation at all -- most rigged characters do -- and the shared set below is
            // exactly what such a rig is for. Refusing it here meant the shared clips
            // could never reach the models that needed them most.
            if (!puppet_root || scene->skeleton_count == 0) {
                fprintf(stderr, "gametest: '%s' has no rig; keeping the box\n", puppet_path);
                drain_async_loader(engine->async_loader, scene);
                free_scene(scene);
                scene = NULL;
                puppet_root = NULL;
            }
        } else {
            fprintf(stderr, "gametest: could not load '%s'; keeping the box\n", puppet_path);
        }
    }
    if (!scene)
        scene = create_scene();
    SceneNode* root = scene->root_node;
    game_set_scene(game, scene);

    if (puppet_root) {
        ShaderProgram* pbr_skinned = create_pbr_skinned_program();
        if (pbr_skinned) {
            engine_add_program(engine, pbr_skinned);
            node_set_programs(puppet_root, pbr_shader, pbr_skinned);
        }
    }

    if (xyz) {
        scene_set_xyz_program(scene, xyz);
    }

    // Load IBL environment if HDR path provided
    if (hdr_path) {
        IBLResources* ibl = create_ibl_resources();
        if (ibl && load_hdr_environment(ibl, hdr_path) == 0) {
            if (precompute_ibl(ibl, engine) == 0) {
                scene->ibl = ibl;
                scene->render_skybox = true;
                scene->skybox_brightness = 1.0f;
                printf("Loaded HDR environment: %s\n", hdr_path);
            } else {
                fprintf(stderr, "Failed to precompute IBL\n");
                free_ibl_resources(ibl);
            }
        } else {
            fprintf(stderr, "Failed to load HDR: %s\n", hdr_path);
            if (ibl)
                free_ibl_resources(ibl);
        }
    } else {
        /*
         * No HDR: bake a physically-based sky and derive the IBL from it. The
         * ocean needs both. With iblEnabled 0 the water's reflection term is
         * identically zero and its horizon goes black, and the environment is
         * half of what its in-scatter multiplies -- so a sea under no sky is
         * legal, runs, and looks wrong.
         *
         * The library's 35 degree sun is taken as it comes rather than authored
         * down to apps/tree's 0.8. At that elevation sunDir.y is 0.014, so the
         * sun delivers about one per cent of the in-scatter: right for a sunset
         * seascape, and a dark hole for a basin walled in by cliffs.
         */
        SkyAtmosphere* sky = create_sky_atmosphere();
        IBLResources* sky_ibl = create_ibl_resources();
        if (sky && sky_ibl && sky_bake_static_luts(sky, engine) == 0 &&
            sky_bake(sky, sky_ibl, engine) == 0) {
            scene->sky = sky;
            scene->ibl = sky_ibl;
            scene->render_skybox = true;
            scene->skybox_brightness = 1.0f;

            /*
             * A real directional the sky owns and retints, not a second light
             * standing beside the rig. Water picks its glitter source through
             * scene_key_directional -- the directional delivering most to a
             * horizontal surface -- so an uncoupled sky would draw the disc in
             * one place and its reflection on the sea in another.
             */
            LightDesc sun_desc = {.name = "sun", .type = LIGHT_DIRECTIONAL, .cast_shadows = true};
            Light* sun = create_light(&sun_desc);
            if (sun) {
                sky->sun_light = sun;
                sky->sun_base_intensity = 6.0f;
                sky_apply_sun_to_light(sky); // owns direction, tint and intensity
                scene_add_light(scene, sun);

                SceneNode* sun_node = create_node();
                node_set_name(sun_node, "sun");
                node_set_light(sun_node, sun);
                node_add_child(scene->root_node, sun_node);
            }
            printf("Sky: sun at elevation %.1f azimuth %.1f\n", (double)sky->sun_elevation_deg,
                   (double)sky->sun_azimuth_deg);
        } else {
            fprintf(stderr, "Failed to bake the sky\n");
            if (sky)
                free_sky_atmosphere(sky);
            if (sky_ibl)
                free_ibl_resources(sky_ibl);
        }
    }

    /*
     * Fit the shadow map to the grotto, which the library default does not.
     *
     * ortho_size ships at 2000 with a far plane of 7500 -- one map over a 4000-unit box,
     * sized for a world far larger than this one. Nothing here was ever tall enough to
     * show what that costs: a 50x50 plate one unit thick has no surface that can shadow
     * itself. A 28-unit cliff wall does, and it came out carrying the shadow map's own
     * rasterization as a grid of triangles across the rock.
     *
     * 80 covers the 120-unit basin corner to corner with margin, which is about 25 times
     * the texel density. That is the fix -- not a bigger bias, which would clear the acne
     * by pushing every shadow off its caster and would peter-pan the ramp and steps the
     * IK fixture needs to read correctly.
     *
     * Per-app rather than in the library: cascade_count > 1 is the more principled answer
     * but takes a different fit path, and shadow.c records count 1 as a byte-identity
     * bridge other apps' goldens rest on.
     */
    if (scene->shadow_system) {
        scene->shadow_system->ortho_size = 80.0f;
        scene->shadow_system->far_plane = 400.0f;
    }

    // The placed lights, in place of app.c's three-point fill. That rig was the brightest
    // thing in the basin and nothing declared it: unshadowed directionals reach a fragment
    // whatever stands between, so the pit could not be made dark while it ran. See
    // build_lights for what replaces it and why there are two panels.
    build_lights(scene);
    build_cavern_fog(scene, engine->postfx);

    // Create physics world
    PhysicsConfig physics_config = physics_default_config();
    PhysicsWorld* physics = create_physics_world(&physics_config);
    if (!physics) {
        fprintf(stderr, "Failed to create physics world!\n");
        return;
    }
    game_set_physics_world(game, physics);
    printf("Physics world created\n");

    // Set up collision callback
    physics_world_set_collision_callback(physics, on_collision, game);

    // Create entity manager
    EntityManager* em = create_entity_manager(game);
    game_set_entity_manager(game, em);

    // The save system. Here rather than at startup because a spawner needs the
    // game it spawns into, and the app table's accessors need nothing at all --
    // the base is the game only because a table must address something.
    save_system = create_save_system(game);
    if (save_system) {
        save_register_table(save_system, "gametest", SAVE_APP_VERSION, SAVE_APP_FIELDS,
                            SAVE_APP_COUNT, game);
        save_register_spawner(save_system, "box", spawn_box_from_save, game);
    }

    // Audio: one device, two SFX beeps. Headless opens no device (offline).
    AudioSystem* audio = create_audio_system(engine->headless);
    if (audio) {
        game_set_audio_system(game, audio);
        ui_audio = audio;
        // The buses take the loaded settings here rather than at install: the
        // audio system is created in this callback, so at install there was
        // nothing to push them into.
        if (ui_system)
            settings_apply(&ui_settings, audio, NULL);
        if (audio_muted)
            audio_set_bus_volume(audio, AUDIO_BUS_MASTER, 0.0f);
        jump_sound = audio_sound_from_tone(audio, 660.0f, AUDIO_BUS_SFX);
        spawn_sound = audio_sound_from_tone(audio, 180.0f, AUDIO_BUS_SFX);
        step_sound = audio_sound_from_tone(audio, 110.0f, AUDIO_BUS_SFX);
        if (step_sound)
            audio_sound_set_volume(step_sound, 0.4f);
    }

    // Create floor entity (static physics body)
    //
    // The collider is a box CENTRED on the entity, so the surface a body rests on is
    // half an extent above that centre -- and the drawn plane has to be lifted to meet
    // it. One constant, read by the visual and the collider both, because these were
    // two independent literals and had drifted: the floor was drawn half a metre below
    // the surface everything stood on. Nothing caught it because no golden photographs
    // a body against the floor, and the anim probes compare bone matrices on the CPU.
    Entity* floor = create_entity(em, "floor");
    glm_vec3_copy((vec3){0, -GAMETEST_FLOOR_HALF_Y, 0}, floor->position);

    // Floor visual, lifted onto the collider's top by that same half-extent
    SceneNode* floor_node = create_node();
    node_set_name(floor_node, "floor");
    Mesh* floor_mesh = create_mesh();
    Plane floor_plane = {.position = {0, GAMETEST_FLOOR_HALF_Y, 0},
                         .width = 50.0f,
                         .depth = 50.0f,
                         .segments_w = 10,
                         .segments_d = 10};
    mesh_generate_plane(floor_mesh, &floor_plane);

    Material* floor_mat = create_material();
    floor_mat->albedo[0] = 0.3f;
    floor_mat->albedo[1] = 0.3f;
    floor_mat->albedo[2] = 0.35f;
    floor_mat->roughness = 0.8f;
    floor_mat->metallic = 0.0f;
    material_set_program(floor_mat, pbr_shader);
    floor_mesh->material = floor_mat;

    node_add_mesh(floor_node, floor_mesh);
    node_add_child(root, floor_node);
    floor->node = floor_node;

    // Floor physics (static box)
    PhysicsShapeDesc floor_shape = {
        .type = SHAPE_BOX,
        .box.half_extents = {25.0f, GAMETEST_FLOOR_HALF_Y, 25.0f},
        .density = 0.0f // Static body
    };
    entity_add_rigid_body(floor, physics, &floor_shape, MOTION_STATIC, OBJ_LAYER_STATIC);
    printf("Floor created with static physics\n");

    // Create player entity with CharacterController
    player_entity = create_entity(em, "player");
    glm_vec3_copy((vec3){0, 2.0f * PLAYER_SCALE, 0}, player_entity->position);
    // The follow camera measures its drop against the last footing, and the spawn is above
    // the floor -- so seed it here or the first frames read as a fall from y = 0.
    cam_ground_y = player_entity->position[1];

    if (puppet_root) {
        // The puppet, its feet a capsule's half-height plus radius below the
        // entity, on the locomotion space; the box's colour and size are the
        // capsule's, which stays the physics body either way.
        player_rig = attach_rig(scene, player_entity, puppet_root, PLAYER_RIG_DROP, PLAYER_SCALE);
        Skeleton* skeleton = scene->skeletons[0];
        Animation* idle = scene_find_animation(scene, "idle");
        Animation* walk = scene_find_animation(scene, "walk");
        Animation* run = scene_find_animation(scene, "run");
        // Where the rig carries clips that state their own travel, those are the pair
        // (spec 12.18). Not a different feature bolted beside locomotion -- the same
        // two entries, filled with clips that say how far they go, so everything
        // downstream of the space is untouched.
        Animation* in_place_walk = walk;
        Animation* in_place_run = run;
        if (!no_root_motion) {
            Animation* carried_walk = scene_find_animation(scene, "travel_walk");
            Animation* carried_run = scene_find_animation(scene, "travel_run");
            if (carried_walk && carried_run) {
                walk = carried_walk;
                run = carried_run;
            }
            clip_lunge = scene_find_animation(scene, "lunge");
            clip_spin = scene_find_animation(scene, "spin");
        }
        // A rig that is not the generated puppet carries none of those three, and before
        // this that meant the red box: the locomotion space could not be built, so no
        // animator was added and the IK that hangs off it never was either.
        //
        // Give it the committed walk cycle for the two moving entries and whatever the
        // model's OWN first clip is for the standing one -- a rigged model almost always
        // ships a rest or bind pose, and it is the difference between a character that
        // stands when you let go and one that walks on the spot forever. One clip filling
        // all three was tried and is exactly that bug.
        if (!idle || !walk || !run) {
            // Before there was a set to give it, this loaded the walk alone and handed it
            // to BOTH moving entries, standing the rig on whatever its own first clip
            // happened to be -- usually the bind pose, so the character crossed the world
            // with its arms out and had no gear above a walk.
            //
            // The source rig is imported as a whole scene because that is the only public
            // way to a Skeleton, and freed as soon as the loads are done: a channel copies
            // the source bone's index, its parent's and its local rest BY VALUE, and the
            // sampler rebuilds the source hierarchy from those alone, so nothing points
            // back here afterwards.
            drain_async_loader(engine->async_loader, scene);
            Scene* rig = create_scene_from_model_path(SHARED_CLIP_RIG, NULL, engine->async_loader);
            Skeleton* source = (rig && rig->skeleton_count > 0) ? rig->skeletons[0] : NULL;
            if (!source)
                fprintf(stderr,
                        "gametest: no rig in '%s'; the shared clips will play uncorrected\n",
                        SHARED_CLIP_RIG);
            for (size_t i = 0; i < sizeof SHARED_CLIPS / sizeof *SHARED_CLIPS; i++)
                load_animations_from_file(scene, skeleton, SHARED_CLIPS[i], true, source);
            if (rig) {
                drain_async_loader(engine->async_loader, rig);
                free_scene(rig);
            }
            idle = scene_find_animation(scene, "quiet_idle");
            walk = scene_find_animation(scene, "strut_walk");
            run = scene_find_animation(scene, "steady_run");
            if (idle && walk && run)
                printf("Rig carries no locomotion of its own: standing on '%s', moving on '%s' "
                       "and '%s'\n",
                       idle->name, walk->name, run->name);
        }
        // The generated rig's own tuck, or the rise from the shared set. One slot for
        // both, because which one a rig gets decides nothing else: what changes is
        // whether there is also a fall loop to hand over to.
        clip_jump = scene_find_animation(scene, "jump");
        if (!clip_jump)
            clip_jump = scene_find_animation(scene, "jump_start");
        clip_wave = scene_find_animation(scene, "wave");
        // The generated rig names its own stroke `swim`; a rig given the shared set gets
        // the pair, and the pair is what makes treading water a state rather than a
        // stroke performed on the spot.
        clip_swim = scene_find_animation(scene, "swim");
        if (!clip_swim)
            clip_swim = scene_find_animation(scene, "swim_cycle");
        clip_float = scene_find_animation(scene, "float_idle");
        clip_fall = scene_find_animation(scene, "fall_cycle");
        clip_land = scene_find_animation(scene, "touch_down");
        add_footsteps(walk);
        if (run != walk)
            add_footsteps(run);
        animator_mask_subtree(skeleton, "cetra_rig:RightArm", wave_mask);
        build_locomotion(skeleton, idle, walk, run);
        // The chaser's entries are the same shape on the clips that stay put, so its
        // positions come off the built space rather than being derived a second way.
        for (int i = 0; i < locomotion_count; i++)
            chaser_locomotion[i] = locomotion[i];
        chaser_locomotion[1].clip = in_place_walk;
        chaser_locomotion[2].clip = in_place_run;
        // After the locomotion axis, which is what sets player_speed: the water's top is
        // the fraction of it a swimmer actually moves at.
        build_aquatic(clip_float, clip_swim, player_speed * GROTTO_SWIM_FRACTION);
        player_animator = create_animator(skeleton);
        // Ask for the clips' travel, and ask for it from what the clips turned out to
        // STATE rather than from the flag: opt-in, because an animator whose caller does
        // not drain has the displacement taken out of its pose and applied nowhere --
        // which is what the chaser above would get, and why it plays its own entries.
        if (player_animator)
            player_animator->root_motion = locomotion_axis == LOCO_TRAVEL;
        if (player_animator && idle && walk && run) {
            animator_play_space(player_animator, "locomotion", locomotion, locomotion_count, 0.0f,
                                true);
            animator_set_event_callback(player_animator, on_anim_event, game);
            entity_add_animator(player_entity, player_animator);

            // Both legs, and Hips as the pelvis a foot that cannot reach asks down.
            // ik_set_pelvis refuses a bone that is not an ancestor of every foot, so
            // the check that this rig is shaped the way the solve assumes is the
            // call itself. The state owns the system and frees it.
            if (!no_ik) {
                player_ik = create_ik_system(skeleton);
                if (player_ik) {
                    ik_foot_left =
                        ik_add_foot(player_ik, "cetra_rig:LeftUpLeg", "cetra_rig:LeftLeg",
                                    "cetra_rig:LeftFoot", (vec3){0.0f, 0.0f, 1.0f});
                    ik_foot_right =
                        ik_add_foot(player_ik, "cetra_rig:RightUpLeg", "cetra_rig:RightLeg",
                                    "cetra_rig:RightFoot", (vec3){0.0f, 0.0f, 1.0f});
                    if (ik_foot_left < 0 || ik_foot_right < 0 ||
                        !ik_set_pelvis(player_ik, "cetra_rig:Hips")) {
                        free_ik_system(player_ik);
                        player_ik = NULL;
                    } else {
                        // After the chain is known good, or a rig missing a leg bone gets
                        // told it has no toes -- twice, by a helper handed -1 -- ahead of
                        // the refusal that actually applies. Both calls are made before
                        // either is reported, so one toe and not the other is not a foot
                        // silently left at the ankle under a message about both.
                        const bool lt =
                            ik_foot_set_toe(player_ik, ik_foot_left, "cetra_rig:LeftToeBase");
                        const bool rt =
                            ik_foot_set_toe(player_ik, ik_foot_right, "cetra_rig:RightToeBase");
                        // Not fatal: a foot with no toe is judged and held at its ankle,
                        // which is what --puppet on another rig may well want. Said out
                        // loud because the puppet has toes since 12.9, so on the default
                        // rig this line means something is wrong.
                        if (!lt || !rt)
                            printf("No toe bones on this rig; contacts are held at the ankle\n");
                        if (no_lock)
                            player_ik->params.lock_distance = 0.0f;
                        player_animator->state->ik = player_ik;
                        player_skel_root = puppet_root;
                    }
                }
            }
            // The ragdoll, built now and started only when something kills the
            // player. Built here because the measurements come from the BIND
            // pose, which does not change, and a build at the moment of death
            // would put a rig walk and a dozen allocations in the frame the
            // player most wants to be smooth.
            //
            // PLAYER_SCALE is the rig node's, which is the scale every capsule
            // is measured against.
            const Mesh* skinned[RD_MAX_SKINNED];
            const size_t skinned_count =
                collect_skinned_meshes(puppet_root, skinned, RD_MAX_SKINNED);
            player_animator->state->ragdoll =
                create_ragdoll(skeleton, skinned, skinned_count, PLAYER_SCALE);
        }
        printf("Player is the puppet: %zu bones, %zu clips\n", skeleton->bone_count,
               scene->animation_count);
    } else {
        // Player visual (capsule approximated as box for now)
        vec3 player_size = {PLAYER_RADIUS, PLAYER_RADIUS + PLAYER_HALF_H, PLAYER_RADIUS};
        vec3 player_color = {0.8f, 0.2f, 0.2f};
        SceneNode* player_node = create_box_node(scene, player_size, player_color, false);
        node_set_name(player_node, "player");
        player_entity->node = player_node;
    }

    // Player character controller
    CharacterControllerConfig player_config = character_controller_default_config();
    player_config.capsule_radius = PLAYER_RADIUS;
    player_config.capsule_half_height = PLAYER_HALF_H;
    player_config.step_height = 0.4f * PLAYER_SCALE;
    player_config.max_strength = 200.0f; // Strong enough to push door

    CharacterController* cc =
        entity_add_character_controller(player_entity, physics, &player_config);
    if (cc) {
        character_controller_set_contact_callback(cc, on_player_contact, game);
    }
    printf("Player created with CharacterController\n");

    // The chaser: the same meshes, its own rig, its own Animator, its own
    // character controller. It hunts the player in on_update.
    if (puppet_root && !no_chaser) {
        SceneNode* rig = clone_rig(puppet_root);
        if (rig) {
            chaser_entity = create_entity(em, "chaser");
            // The far corner at the TOP LEFT of the view: the camera sits at
            // +Z looking at the origin, so screen-right is +X and screen-up is
            // -Z. At 6 m/s against the player's 10 it is a chase, not an ambush.
            glm_vec3_copy((vec3){-20.0f, 2.0f * PLAYER_SCALE, -20.0f}, chaser_entity->position);
            chaser_rig = attach_rig(scene, chaser_entity, rig, PLAYER_RIG_DROP, PLAYER_SCALE);

            CharacterControllerConfig cfg = character_controller_default_config();
            cfg.capsule_radius = PLAYER_RADIUS;
            cfg.capsule_half_height = PLAYER_HALF_H;
            cfg.step_height = 0.4f * PLAYER_SCALE;
            entity_add_character_controller(chaser_entity, physics, &cfg);

            chaser_animator = create_animator(scene->skeletons[0]);
            if (chaser_animator) {
                animator_play_space(chaser_animator, "locomotion", chaser_locomotion,
                                    locomotion_count, 0.0f, true);
                entity_add_animator(chaser_entity, chaser_animator);
            }
            printf("Chaser created -- run!\n");
        }
    }

    create_hearts(engine, scene);

    // A second rig beside the player on its own clip: two poses in one frame.
    if (puppet_root && twin_clip) {
        Animation* clip = scene_find_animation(scene, twin_clip);
        SceneNode* rig = clip ? clone_rig(puppet_root) : NULL;
        if (!clip) {
            fprintf(stderr, "gametest: --twin names clip '%s', which the puppet lacks\n",
                    twin_clip);
        } else if (rig) {
            Entity* twin = create_entity(em, "twin");
            glm_vec3_copy((vec3){-6.0f, 0.0f, 0.0f}, twin->position);
            attach_rig(scene, twin, rig, 0.0f, PLAYER_SCALE);
            Animator* a = create_animator(scene->skeletons[0]);
            if (a) {
                animator_play(a, clip, 0.0f, true);
                entity_add_animator(twin, a);
            }
            printf("Twin rig playing '%s'\n", clip->name);
        }
    }

    // Create a door with hinge constraint
    create_door(game, (vec3){5.0f, 0.0f, 0.0f});

    // A looping tone carried by the door as an AUDIO_SOURCE component: its world
    // position is pushed from the entity each frame, so it pans and attenuates
    // as the door swings and as the camera orbits.
    if (audio && door_entity) {
        Sound* beacon = audio_sound_from_tone(audio, 440.0f, AUDIO_BUS_SFX);
        if (beacon) {
            audio_sound_set_looping(beacon, true);
            audio_sound_set_volume(beacon, 0.5f);
            entity_add_audio_source(door_entity, beacon);
            audio_sound_play(beacon);
        }
    }

    // The uneven ground, before the broad phase is optimised so its bodies are covered.
    // Always present: flat floor is the one case where foot planting is correctly a
    // no-op, so a demo without a slope demonstrates nothing. It stood behind a flag to
    // keep it out of the two menu goldens, which photograph the world through a
    // backdrop that is only 77 percent opaque -- those now include it.
    build_ik_ground(game);
    // The basin around it, and the platform's own lip. All before the optimize below,
    // which the comment there requires: every static body has to exist first. The shaft
    // goes first because build_ocean's bed callback reads the field it installs.
    build_shaft(game);
    build_platform_skirt(game);
    build_ocean(scene);

    // Optimize broad phase after adding initial bodies
    physics_world_optimize(physics);

    // Setup camera
    CameraDesc camera_desc = {
        .position = {0.0f, 20.0f, 35.0f}, .fov = 0.8f, .near = 0.1f, .far = 1000.0f};
    // The FOV rides the desc rather than the built camera, so the explicit pose and the
    // default go through one construction path instead of two.
    if (cam_pose_fov_deg > 0.0f)
        camera_desc.fov = glm_rad(cam_pose_fov_deg);
    Camera* camera = create_camera(&camera_desc);
    engine_set_camera(engine, camera);
    engine->camera_mode = CAMERA_MODE_ORBIT;

    // Both halves, or neither: an eye with no target is a direction nobody stated, and
    // silently keeping half of a pose is worse than ignoring it.
    if (cam_eye_set && cam_target_set) {
        glm_vec3_copy(cam_pose_up, camera->up_vector);
        camera_set_position(camera, cam_pose_eye);
        camera_set_look_at(camera, cam_pose_target);
        printf("Camera pose: eye %g,%g,%g target %g,%g,%g fov %g\n", (double)cam_pose_eye[0],
               (double)cam_pose_eye[1], (double)cam_pose_eye[2], (double)cam_pose_target[0],
               (double)cam_pose_target[1], (double)cam_pose_target[2], (double)cam_pose_fov_deg);
    } else if (cam_eye_set || cam_target_set) {
        fprintf(stderr, "--cam-eye and --cam-target must both be given; ignoring the pose\n");
    }

    // Create drag controller
    drag_controller = create_mouse_drag_controller(engine);

    // No GUI or FPS overlay headless, as the other apps: the FPS digits are
    // wall clock and land in the screenshot, which is what made two identical
    // runs differ by pixels while the sim beneath them did not.
    engine->show_gui = !engine->headless;
    engine->show_fps = !engine->headless;
    // A rig brings twenty-two joint nodes, each of which would wear a gizmo.
    engine->show_xyz = puppet_root == NULL;

    // Spawn a few initial boxes, unless somebody is about to measure where the player
    // ENDED UP. They fall at rand() positions in a 20-unit box centred on the spawn,
    // and rand() differs per platform's libc -- so a crate that lands beside the
    // player here lands on him elsewhere, and an arm reading his displacement fails on
    // one machine and nowhere else. That is why the gamepad group reads the commanded
    // move and never the position; the two arms that must read position ask for this.
    if (!no_crates) {
        for (int i = 0; i < 5; i++) {
            spawn_falling_box(game);
        }
    }
}

// Fixed timestep update - game logic and physics
static void on_update(Game* game, double dt) {
    if (!player_entity)
        return;

    PhysicsWorld* physics = game_get_physics_world(game);
    CharacterController* cc = entity_get_character_controller(player_entity);
    if (!cc)
        return;

    // Handle deferred door opening (from contact callback)
    if (door_open_pending && door_hinge) {
        constraint_hinge_set_motor_state(door_hinge, MOTOR_VELOCITY);
        constraint_hinge_set_target_velocity(door_hinge, door_open_velocity);
        RigidBody* door_rb = entity_get_rigid_body(door_entity);
        if (door_rb)
            rigid_body_activate(door_rb);
        door_open_pending = false;
    }
    // Close door if player wasn't touching it last frame
    else if (!player_touching_door && door_hinge) {
        // Use velocity mode for constant closing speed
        float current_angle = constraint_hinge_get_current_angle(door_hinge);
        if (fabsf(current_angle) > 0.05f) {
            // Door is open - close at constant velocity
            float close_velocity = (current_angle > 0) ? -3.0f : 3.0f;
            constraint_hinge_set_motor_state(door_hinge, MOTOR_VELOCITY);
            constraint_hinge_set_target_velocity(door_hinge, close_velocity);
        } else {
            // Door is nearly closed - switch to position to hold at 0
            constraint_hinge_set_motor_state(door_hinge, MOTOR_POSITION);
            constraint_hinge_set_target_angle(door_hinge, 0.0f);
        }
        // Wake up the door so motor can move it
        RigidBody* door_rb = entity_get_rigid_body(door_entity);
        if (door_rb)
            rigid_body_activate(door_rb);
    }
    // Reset for this frame - contact callbacks will set it if touching
    player_touching_door = false;

    vec3 input_dir;
    input_action_move(&game->input, "move_x", "move_y", input_dir);

    /*
     * What the stick is asking for, in the WORLD, computed once: the direction and how
     * hard. Camera-relative under the follow camera (spec 12.17) -- W goes away from the
     * lens, whichever way the arrows have aimed it -- and the argument for that, which
     * has been settled in both directions, is at the velocity below.
     *
     * Here rather than beside the velocity because three blocks read it: the knob (when
     * the clips carry the body, the knob is the stick's), the facing, and the velocity
     * itself. `lean` is a rotation-invariant magnitude, so computing it from either
     * vector gives the same number -- which is exactly why it should be computed from
     * one of them, once, rather than twice under one name in two scopes.
     */
    const float cam_sin = sinf(cam_yaw), cam_cos = cosf(cam_yaw);
    const vec3 want_dir = {-cam_cos * input_dir[0] - cam_sin * input_dir[2], 0.0f,
                           cam_sin * input_dir[0] - cam_cos * input_dir[2]};
    const float lean = hypotf(want_dir[0], want_dir[2]);

    // Get current velocity
    vec3 vel;
    character_controller_get_velocity(cc, vel);

    // The locomotion knob and the facing come from this velocity -- Jolt's
    // POST-SOLVE one, before the input below overwrites it -- so walking into
    // a wall stops the walk rather than running on the spot.
    float ground_speed = hypotf(vel[0], vel[2]);
    hud_ground_speed = ground_speed;
    if (player_animator && !player_ragdolled()) {
        if (locomotion_axis == LOCO_TRAVEL && player_medium == MEDIUM_GROUND) {
            // The knob is what the STICK asks for, not what the body achieved, and that
            // is forced rather than chosen: under root motion the travel comes from the
            // clip the knob selects, so a knob fed by the achieved speed would start at
            // zero, select the standing clip, travel nothing and stay there. The
            // feedback runs the other way now.
            //
            // What that costs is the wall: today's knob reads Jolt's post-solve speed,
            // so walking into one stops the walk. Here the clip keeps walking while the
            // sweep refuses to move the body, which is what inverting the ownership
            // means and is left visible rather than papered over.
            //
            // ON THE GROUND ONLY, because only the ground source carries the body. The
            // water and the air are stick-driven whatever the locomotion clips state,
            // and their axes are their own -- the swim space tops out at a fraction of
            // this one, so a stick reading meant for the ground reads as a full stroke
            // at 40 per cent of it.
            player_animator->param = (lean > 1.0f ? 1.0f : lean) * player_speed;
            player_animator->speed = 1.0f;
        } else if (locomotion_axis != LOCO_FRACTION) {
            // The axis IS metres per second, so the knob is the speed and the clamp is the
            // space's own (animator.h: param is clamped to its entries).
            player_animator->param = ground_speed;
            // And what the clips cannot reach by standing where they are, they reach by
            // playing faster or slower. Inside the covered range this is close to 1 and
            // does almost nothing, which is the point: past the last entry it is the only
            // thing keeping a foot on the ground.
            player_animator->speed = locomotion_rate(player_animator, ground_speed);
        } else {
            // No measured stride, so the knob is a fraction of the compile-time constant
            // and never of whatever --speed capped travel at: dividing by the cap
            // re-normalises the gear, so full stick would be the run clip at any speed.
            const float knob = ground_speed / PLAYER_SPEED;
            player_animator->param = knob > 1.0f ? 1.0f : knob;
        }

        // Water and air are their own SOURCES rather than more entries in the locomotion
        // space, and that is about shape and not about convenience: a swim is not a faster
        // run and a fall is not a slower one, so neither belongs on an axis whose whole
        // meaning is ground speed, and a fourth entry would blank the trace's three weight
        // columns besides. Each is crossfaded on the EDGE of the medium changing.
        // A rig with no airborne clip never enters that state, rather than entering it and
        // playing the ground source anyway: the second shape re-issues the locomotion
        // space twice in the first second, since the player spawns above the floor, and a
        // crossfade restarts the clock a settling arm is reading.
        const PlayerMedium want = player_swimming ? MEDIUM_WATER
                                  : (clip_fall && !character_controller_is_grounded(cc))
                                      ? MEDIUM_AIR
                                      : MEDIUM_GROUND;
        if (want != player_medium) {
            const PlayerMedium was = player_medium;
            player_medium = want;
            if (want == MEDIUM_AIR) {
                // The rise is a ONE-SHOT and the fall is a loop, which is the difference
                // between the two ways of being in the air: a jump ends, and what it ends
                // into is the fall. The hand-off is the finished edge just below.
                if (player_jumped && clip_jump)
                    animator_play(player_animator, clip_jump, 0.10f, false);
                else
                    animator_play(player_animator, clip_fall, 0.15f, true);
            } else if (want == MEDIUM_WATER && aquatic_count > 0) {
                animator_play_space(player_animator, "swim", aquatic, aquatic_count, 0.25f, true);
            } else if (want == MEDIUM_WATER && clip_swim) {
                animator_play(player_animator, clip_swim, 0.25f, true);
            } else {
                player_jumped = false;
                animator_play_space(player_animator, "locomotion", locomotion, locomotion_count,
                                    0.25f, true);
                // The landing goes on AFTER the space, so the space is what it resumes to
                // when it releases itself. Short fade: a landing that eases in has already
                // missed the moment it exists for.
                if (was == MEDIUM_AIR && clip_land)
                    animator_play_once(player_animator, clip_land, 0.08f);
            }
        } else if (player_medium == MEDIUM_AIR && clip_fall && animator_finished(player_animator)) {
            // The rise reached its end: from here the character is falling, whatever put
            // it up there. animator_finished is an EDGE, so this fires once and the loop
            // it starts does not re-trigger it.
            animator_play(player_animator, clip_fall, 0.15f, true);
        }
        // Off the ground there is no stride to match, so the clip plays at its own rate:
        // the swim space carries no strides by design and the fall is a single clip. The
        // knob still means metres per second, which is what lets the swim space read its
        // own axis off the same param the ground uses.
        if (player_medium != MEDIUM_GROUND)
            player_animator->speed = 1.0f;
    }
    /*
     * What the clip laid down, taken once per step (spec 12.18).
     *
     * A statement rather than a term in a condition. The drain has a SIDE EFFECT --
     * it hands the accumulator over and zeroes it -- so hiding it behind && makes
     * every guard in front of it a silent decision to stop draining while the
     * animator keeps accumulating, and the next successful drain then delivers the
     * hoard in one step. That is the lurch `switch_source` exists to prevent,
     * recreated at the call site by anyone who adds a fourth condition.
     *
     * WHERE it sits is load-bearing and is not obvious from reading it: BELOW the
     * medium machine above, which is what makes the step you walk off a ledge
     * stick-driven. Hoisted -- and it reads like input-gathering, so it invites
     * hoisting -- that step is driven by the walk clip the character has just left.
     *
     * The query is live rather than the startup axis, so a fall loop and a stroke
     * fall back to the stick with nothing here to switch.
     */
    vec3 root_travel = {0.0f, 0.0f, 0.0f};
    float root_yaw = 0.0f;
    bool rooted = false;
    if (player_animator && !player_ragdolled())
        rooted = animator_take_root_motion(player_animator, root_travel, &root_yaw);

    if (player_rig && !player_ragdolled()) {
        // The puppet faces +Z at yaw 0. Smoothed on sim time, so it is the
        // same turn headless and windowed.
        //
        // Where it turns TOWARD is the one thing root motion changes here: the body's
        // velocity is derived from the facing, so reading the facing back out of the
        // velocity would be a circle that never starts turning. The stick says where to
        // point; the clip says how fast that gets you there.
        player_yaw += root_yaw; // the clip's own turn, on top of the steering
        const bool steering = rooted ? lean > 0.05f : ground_speed > 0.1f;
        if (steering) {
            float target = rooted ? atan2f(want_dir[0], want_dir[2]) : atan2f(vel[0], vel[2]);
            float delta = target - player_yaw;
            while (delta > (float)M_PI)
                delta -= 2.0f * (float)M_PI;
            while (delta < -(float)M_PI)
                delta += 2.0f * (float)M_PI;
            float k = (float)dt * 12.0f;
            player_yaw += delta * (k > 1.0f ? 1.0f : k);
        }
        if (steering || root_yaw != 0.0f) {
            glm_mat4_identity(player_rig->original_transform);
            glm_translate(player_rig->original_transform, (vec3){0.0f, PLAYER_RIG_DROP, 0.0f});
            glm_rotate_y(player_rig->original_transform, player_yaw,
                         player_rig->original_transform);
            glm_scale_uni(player_rig->original_transform, PLAYER_SCALE);
        }
    }

    /*
     * Apply horizontal movement, CAMERA-RELATIVE under the follow camera (spec 12.17):
     * W goes away from the lens, whichever way the arrows have aimed it.
     *
     * This has now been both ways and the record matters more than the choice, because
     * each direction has a comment that sounds conclusive. 12.13 made it WORLD-ALIGNED,
     * arguing that forward-away-from-camera welds the character's facing to the camera's,
     * so you only ever see its back and can never cross the frame. That cost is real and
     * the conclusion still does not follow: `cam_yaw` moves ONLY when the player presses
     * an arrow -- the camera chasing facing was tried in 12.6 and abandoned -- so orbiting
     * round to see the character's front is a thing the player can simply do. World
     * alignment trades that momentary inconvenience for a permanent one, in which turning
     * the camera ninety degrees leaves W walking across the screen.
     *
     * What settled it is that 12.13 changed the code and neither document: `cli-reference`
     * and `AGENTS.md` both went on describing camera-relative movement for four specs. The
     * behaviour a player meets should be the one written down, and when they disagree the
     * documented one has at least been read by somebody.
     *
     * SCOPE, since `cam_yaw` is the FOLLOW camera's own state and nothing else writes it:
     * under `--no-follow-cam` the drag controller owns the camera and this stays at pi, so
     * movement there is world-aligned as before, and the same holds under the `--cam-eye`
     * pinning that exists to freeze a framing for A/B captures. Deriving the heading back
     * out of the live camera would cover every camera with one rule and is the wrong
     * trade: `--cam-eye` states a POSE, and letting it silently rotate the controls is a
     * worse surprise than the debug orbit keeping the scheme it has.
     *
     * At the default yaw of pi this is exactly the IDENTITY -- sin is 0 and cos is -1,
     * cancelling both the strafe sign and the forward negation -- which is what keeps
     * every gamepad script, trace displacement and menu golden byte-identical, none of
     * them touching an arrow. That property is also why the flip went unnoticed in 12.13,
     * so `pad-camera-relative` now asserts which scheme is live.
     *
     * UNDER ROOT MOTION the stick has already been spent, on the facing rather than on
     * the velocity (spec 12.18), and what moves the body is the distance the clip
     * states: model units into world by the rig's own scale, turned by the facing the
     * block above just set, and divided by this step's dt because a controller takes a
     * velocity and a clip states a distance. The air and the water keep the scheme
     * below, their clips stating no travel to spend.
     */
    if (rooted) {
        glm_vec3_scale(root_travel, PLAYER_SCALE, root_travel);
        const float face_sin = sinf(player_yaw), face_cos = cosf(player_yaw);
        vel[0] = (root_travel[0] * face_cos + root_travel[2] * face_sin) / (float)dt;
        vel[2] = (-root_travel[0] * face_sin + root_travel[2] * face_cos) / (float)dt;
    } else {
        vel[0] = want_dir[0] * player_speed;
        vel[2] = want_dir[2] * player_speed;
    }

    // Gravity, or buoyancy where the water is. Swimming is surface-only by design: you
    // float and cannot go under, which is impossible to get stuck in and reads clearly
    // at this camera distance.
    float gravity = 20.0f;
    player_swimming = grotto_submerged(game->scene, player_entity->position);
    if (player_swimming) {
        vel[1] = grotto_float_velocity(player_entity->position[1], vel[1], (float)dt);
        // A swimmer is slower than a runner, and the stroke should read as effort.
        vel[0] *= GROTTO_SWIM_FRACTION;
        vel[2] *= GROTTO_SWIM_FRACTION;
    } else {
        vel[1] -= gravity * (float)dt;
        // Terminal velocity, and it is load-bearing rather than flavour. An 80-unit drop
        // arrives at about 56 m/s, and the float below trades that against buoyancy over
        // roughly 13 units of depth -- through an 8-unit basin and into the seabed. At 25
        // the plunge is under 6 and the water catches you.
        if (vel[1] < -GROTTO_TERMINAL_V)
            vel[1] = -GROTTO_TERMINAL_V;
    }

    // Jump when on ground
    bool grounded = character_controller_is_grounded(cc);
    bool jump = input_action_pressed(&game->input, "jump");
    if (jump && grounded) {
        vel[1] = 10.0f; // Jump velocity
        printf("Jump!\n");
        if (jump_sound)
            audio_sound_play(jump_sound);
        player_jumped = true;
        // On a rig with no airborne loop the tuck is a one-shot that returns to the
        // locomotion space by itself, the airtime being one second at this velocity under
        // this gravity. With a loop it is the AIR state that plays the rise instead, a
        // frame later, and a one-shot fired here would be replaced before it read.
        if (player_animator && clip_jump && !clip_fall)
            animator_play_once(player_animator, clip_jump, 0.25f);
    }
    if (player_animator && clip_wave && input_action_pressed(&game->input, "wave"))
        animator_play_layer(player_animator, clip_wave, wave_mask, 0.1f, 0.1f, false);

    // The two authored moves (spec 12.18), and they are BASE one-shots where the wave is
    // an override layer -- a wave happens on an arm while the body carries on, a lunge is
    // what the body is doing. Each returns to the locomotion space by itself.
    //
    // Only on the ground, and only while the space they return to is the one that
    // travels: firing a lunge mid-air would hand the character a metre of ground it has
    // no contact with, and the medium machine above would take the source back on the
    // next edge anyway.
    if (player_animator && player_medium == MEDIUM_GROUND && !player_ragdolled()) {
        if (clip_lunge && input_action_pressed(&game->input, "lunge"))
            animator_play_once(player_animator, clip_lunge, 0.08f);
        else if (clip_spin && input_action_pressed(&game->input, "spin"))
            animator_play_once(player_animator, clip_spin, 0.08f);
    }

    // Set velocity (CharacterController will handle collision response)
    character_controller_set_velocity(cc, vel);

    // The chaser: steer flat toward the player at its own speed, blend its legs
    // from the speed it actually achieved, and face where it is going.
    CharacterController* chase_cc =
        chaser_entity ? entity_get_character_controller(chaser_entity) : NULL;
    if (chase_cc) {
        vec3 to_player;
        glm_vec3_sub(player_entity->position, chaser_entity->position, to_player);
        to_player[1] = 0.0f;
        float gap = glm_vec3_norm(to_player);

        vec3 chase_vel;
        character_controller_get_velocity(chase_cc, chase_vel);
        if (chaser_animator) {
            float speed = hypotf(chase_vel[0], chase_vel[2]) / player_speed;
            chaser_animator->param = speed > 1.0f ? 1.0f : speed;

            // The same edge for the chaser, which is what makes chaser_swimming more than
            // bookkeeping: it swims after you rather than walking along the bottom.
            if (clip_swim && chaser_swimming != chaser_in_swim_clip) {
                if (chaser_swimming)
                    animator_play(chaser_animator, clip_swim, 0.25f, true);
                else
                    animator_play_space(chaser_animator, "locomotion", chaser_locomotion,
                                        locomotion_count, 0.25f, true);
                chaser_in_swim_clip = chaser_swimming;
            }
        }
        if (chaser_rig && hypotf(chase_vel[0], chase_vel[2]) > 0.1f) {
            float target = atan2f(chase_vel[0], chase_vel[2]);
            float d = target - chaser_yaw;
            while (d > (float)M_PI)
                d -= 2.0f * (float)M_PI;
            while (d < -(float)M_PI)
                d += 2.0f * (float)M_PI;
            float k = (float)dt * 12.0f;
            chaser_yaw += d * (k > 1.0f ? 1.0f : k);
            glm_mat4_identity(chaser_rig->original_transform);
            glm_translate(chaser_rig->original_transform, (vec3){0.0f, PLAYER_RIG_DROP, 0.0f});
            glm_rotate_y(chaser_rig->original_transform, chaser_yaw,
                         chaser_rig->original_transform);
            glm_scale_uni(chaser_rig->original_transform, PLAYER_SCALE);
        }

        // Close in unless already on top of him, so it does not jitter against
        // the player's capsule once it arrives.
        if (gap > CATCH_RADIUS * 0.5f) {
            glm_vec3_scale(to_player, CHASER_FRACTION * player_speed / gap, to_player);
            chase_vel[0] = to_player[0];
            chase_vel[2] = to_player[2];
        } else {
            chase_vel[0] = 0.0f;
            chase_vel[2] = 0.0f;
        }
        // Its own handling, not the player's: this one never zeroes chase_vel[1] when
        // grounded, so it carries accumulated downward velocity into the water and would
        // sink through a clamp written for a character that does.
        chaser_swimming = grotto_submerged(game->scene, chaser_entity->position);
        if (chaser_swimming) {
            chase_vel[1] =
                grotto_float_velocity(chaser_entity->position[1], chase_vel[1], (float)dt);
            chase_vel[0] *= GROTTO_SWIM_FRACTION;
            chase_vel[2] *= GROTTO_SWIM_FRACTION;
        } else {
            chase_vel[1] -= gravity * (float)dt;
        }
        character_controller_set_velocity(chase_cc, chase_vel);

        // Caught: hearts, once. The cooldown is what makes it one burst rather
        // than one per step for as long as the two overlap.
        if (catch_cooldown > 0.0f)
            catch_cooldown -= (float)dt;
        if (gap < CATCH_RADIUS && catch_cooldown <= 0.0f) {
            catch_cooldown = 2.0f;
            heart_timer = HEART_SECONDS;
            if (heart_spawn)
                particle_module_spawn_rate_set(heart_spawn, HEART_RATE);
            if (heart_node) {
                // Burst from between the two of them, at chest height.
                vec3 mid;
                glm_vec3_add(player_entity->position, chaser_entity->position, mid);
                glm_vec3_scale(mid, 0.5f, mid);
                node_set_position(heart_node, (vec3){mid[0], mid[1], mid[2]});
            }
            printf("Caught!\n");
            if (jump_sound)
                audio_sound_play(jump_sound);
        }
    }

    // The burst is a WINDOW, not a one-shot: the emitter keeps spawning for
    // HEART_SECONDS and then stops, and the hearts already alive finish rising.
    if (heart_timer > 0.0f) {
        heart_timer -= (float)dt;
        if (heart_timer <= 0.0f && heart_spawn)
            particle_module_spawn_rate_set(heart_spawn, 0.0f);
    }

    // The position is the one BEFORE this step; move_x and move_y are the
    // action values the step acted on. With a rig the line continues after
    // `jump`: the knob, the locomotion space's three weights (zeros while a
    // one-shot has the base), the crossfade weight, the override weight and
    // the base source's name -- all as of the last rendered frame's tick.
    if (trace_player && trace_step % trace_every == 0) {
        printf("player step %d t=%5.2f pos %8.3f %8.3f %8.3f  vel %6.2f %6.2f %6.2f  "
               "grounded %d  move %5.2f %5.2f jump %d",
               trace_step, game->time, player_entity->position[0], player_entity->position[1],
               player_entity->position[2], vel[0], vel[1], vel[2], grounded ? 1 : 0,
               input_action_value(&game->input, "move_x"),
               input_action_value(&game->input, "move_y"), jump ? 1 : 0);
        if (player_animator) {
            const AnimatorSpace* base = &player_animator->base;
            // By NAME and not by a count of three: a rig with no run clip builds the same
            // space with two entries, and a count gate blanked its weights as though
            // something else were playing.
            const bool loco = !strcmp(animator_source_name(player_animator), "locomotion");
            printf(" anim %.3f %.3f %.3f %.3f %.3f %.3f %.3f %s", player_animator->param,
                   loco ? base->weights[0] : 0.0f, loco ? base->weights[1] : 0.0f,
                   loco ? base->weights[2] : 0.0f, player_animator->fade_weight,
                   player_animator->layer.weight, player_animator->speed,
                   animator_source_name(player_animator));
        }
        // The follow camera's heading, LAST, so appending it cannot disturb either
        // regex already reading this line -- neither anchors its end. It is here
        // because `vel` above is camera-relative since 12.17, and a reader with no
        // camera angle can see that the two disagree but not that they disagree by
        // exactly the amount the arrows asked for.
        printf(" cam %.4f", (double)cam_yaw);
        // And the character's own heading, after it (spec 12.18), for the same reason
        // and by the same rule: appended rather than inserted, since every regex reading
        // this line counts its groups from the left. A clip that turns the body turns
        // THIS and moves nothing else, so without it a spin and a stand are the same
        // three columns.
        printf(" yaw %.4f", (double)player_yaw);
        printf("\n");
    }
    trace_step++;

    // Door closing is now handled at START of next frame, after we know contact state
    // See beginning of on_update

    if (input_action_pressed(&game->input, "spawn")) {
        spawn_falling_box(game);
        if (spawn_sound)
            audio_sound_play(spawn_sound);
    }

    /*
     * Quicksave and quickload. The WORK is here, in the fixed step, because
     * on_update runs after the step's physics sync and writes a settled world
     * -- pre_render would catch the entity poses of one step against the bodies
     * of the next. The EDGE is not here: on_frame_input took it once for the
     * whole frame, and the flag is what carries it across.
     */
    if (save_pending) {
        save_pending = false;
        char path[1024];
        if (save_system && save_default_path(path, sizeof(path), "quick"))
            save_write(save_system, path);
    }
    if (load_pending) {
        load_pending = false;
        char path[1024];
        if (save_system && save_default_path(path, sizeof(path), "quick")) {
            const SaveLoadResult r = save_read(save_system, path);
            // The one teleport this app has. ik.h gives the caller exactly one duty
            // beyond setting targets, and this is where it falls due: without it the
            // feet ease through the world toward their new ground for a few frames.
            // teleport_distance catches the long jumps on its own, so what leaks
            // through is precisely a short load -- the case nobody would notice failing.
            if (r.ok && player_ik)
                ik_reset(player_ik);
            if (r.ok)
                printf("Loaded: %d entities, %d spawned, %d dropped\n", r.entities_restored,
                       r.entities_spawned,
                       r.dropped_missing_entity + r.dropped_unknown_spawner +
                           r.dropped_unknown_component);
        }
    }

    if (input_action_pressed(&game->input, "raycast") && physics) {
        vec3 down = {0, -1, 0};
        RaycastHit hit;
        if (physics_world_raycast(physics, player_entity->position, down, 50.0f, &hit)) {
            printf("Raycast hit: %s at distance %.2f (pos: %.1f, %.1f, %.1f)\n",
                   hit.entity ? hit.entity->name : "unknown", hit.distance, hit.position[0],
                   hit.position[1], hit.position[2]);
        } else {
            printf("Raycast: no hit\n");
        }
    }

    if (input_action_pressed(&game->input, "ground")) {
        CharacterGroundState state = character_controller_get_ground_state(cc);
        const char* state_str = "unknown";
        switch (state) {
            case CHAR_GROUND_ON_GROUND:
                state_str = "ON_GROUND";
                break;
            case CHAR_GROUND_ON_STEEP_GROUND:
                state_str = "ON_STEEP_GROUND";
                break;
            case CHAR_GROUND_NOT_SUPPORTED:
                state_str = "NOT_SUPPORTED";
                break;
            case CHAR_GROUND_IN_AIR:
                state_str = "IN_AIR";
                break;
        }
        printf("Ground state: %s\n", state_str);
    }
}

// Pre-render callback - the camera the frame's geometry is read against. The
// engine propagates the graph as soon as this returns.
// One foot's ground target: raycast under its current ankle and hand the hit back in
// MODEL space, which is the only space the solver speaks. Returns false when nothing
// is under it -- walked off an edge -- and the caller then lets that foot go.
//
// Filtered to the STATIC layer, and that is required rather than tidy: a character's
// inner body is created in OBJ_LAYER_DYNAMIC, so an unfiltered ray cast from inside
// the capsule hits the player and plants the foot on itself. The cost is that feet
// plant on the floor and the ramp but not on the crates, which are dynamic.
static bool ik_ground_under_foot(PhysicsWorld* physics, mat4 to_world, mat4 to_model,
                                 vec3 ankle_model, vec3 out_target, vec3 out_normal) {
    vec3 ankle_world, origin;
    glm_mat4_mulv3(to_world, ankle_model, 1.0f, ankle_world);
    glm_vec3_copy(ankle_world, origin);
    origin[1] += IK_FOOT_RAY_UP;

    RaycastHit hit;
    vec3 down = {0.0f, -1.0f, 0.0f};
    if (!physics_world_raycast_filtered(physics, origin, down, IK_FOOT_RAY_LEN,
                                        1u << OBJ_LAYER_STATIC, &hit))
        return false;

    glm_mat4_mulv3(to_model, hit.position, 1.0f, out_target);
    glm_mat4_mulv3(to_model, hit.normal, 0.0f, out_normal);
    glm_vec3_normalize(out_normal);
    return true;
}

// Targets are set HERE and not in on_update, and the difference is not stylistic.
// update_all_animators runs once per RENDERED frame, from game_pre_render, after this
// hook returns. on_update is the fixed step: it may run zero times in a frame or
// several, so a target set there reaches the single animator tick either stale or as
// the last of N.
static void ik_update_targets(Game* game) {
    // One test, not four: player_skel_root, player_animator and its state are assigned
    // in the same block as player_ik, which is NULLed on every failure path there, so
    // a non-null player_ik already implies the rest. Testing them separately would
    // suggest they can disagree and invite someone to set player_ik somewhere else.
    if (!player_ik || player_ragdolled())
        return;
    PhysicsWorld* physics = game_get_physics_world(game);
    if (!physics)
        return;

    // The node's global is propagated AFTER this hook, so it is one frame old. That
    // is the same frame of lag the target already carries by construction, and the
    // solver's easing absorbs it.
    //
    // On the FIRST frame it has never been propagated at all, so this would be the
    // identity and the targets would be computed in the wrong space. What prevents
    // that is the weight test below -- ik_weight starts at 0 and the early return
    // fires before to_world is used. That is deliberate, not luck: it used to be
    // luck, resting on the player also happening to spawn airborne.
    mat4 to_world, to_model;
    glm_mat4_copy(player_skel_root->global_transform, to_world);
    glm_mat4_inv(to_world, to_model);

    CharacterController* cc = entity_get_character_controller(player_entity);
    // Not-swimming as well as grounded, and the second half is load-bearing: the foot ray
    // filters on OBJ_LAYER_STATIC and nothing else, so the seabed is indistinguishable
    // from the floor to it. Grounded alone would plant a treading swimmer's feet on the
    // bottom the moment the bed came within reach.
    const float want =
        (cc && character_controller_is_grounded(cc) && !player_swimming) ? 1.0f : 0.0f;
    const float rate = (float)game->sim_clock.delta * IK_WEIGHT_RATE;
    ik_weight += (want - ik_weight) * (rate > 1.0f ? 1.0f : rate);
    // Snapped, because the decay is asymptotic and never actually arrives. Left alone,
    // ik_weight stays a hair above zero for the whole of a jump, so the solver's
    // "weight 0 is bit-identical to no IK" path -- which it documents -- never fires,
    // and two raycasts plus two full solves run every airborne frame to move an ankle
    // by an epsilon.
    if (want == 0.0f && ik_weight < 1e-3f)
        ik_weight = 0.0f;

    mat4* globals = player_animator->state->global_transforms;
    const int feet[2] = {ik_foot_left, ik_foot_right};
    // Zero is HANDED OVER rather than returned on, which is the whole point of snapping it
    // and was missing: returning here left IkFoot.weight at whatever the last call passed,
    // a hair above zero forever, so the bit-identical path never fired and -- since spec
    // 12.9 -- a lock could form in mid-air against a target and a world matrix both frozen
    // at the last grounded frame, to be released one frame after landing with the leg
    // reaching for where the character used to be.
    if (ik_weight == 0.0f) {
        for (int i = 0; i < 2; i++)
            if (feet[i] >= 0)
                ik_foot_set_target(player_ik, feet[i],
                                   globals[player_ik->feet[feet[i]].ankle_index][3],
                                   (vec3){0.0f, 1.0f, 0.0f}, 0.0f);
        return;
    }

    // Past the weight gate, where the transform has been propagated at least once: the
    // identity the first frame carries is exactly the wrong space for a WORLD contact, and
    // it is the same reason the targets below are computed here rather than above.
    ik_set_world(player_ik, to_world);

    for (int i = 0; i < 2; i++) {
        if (feet[i] < 0)
            continue;
        // Zeroed because cppcheck cannot see through an out-parameter and reads these
        // as used before set. The writes below are unconditional on the true path.
        vec3 ankle = {0.0f, 0.0f, 0.0f};
        vec3 target = {0.0f, 0.0f, 0.0f};
        vec3 normal = {0.0f, 0.0f, 0.0f};
        glm_vec3_copy(globals[player_ik->feet[feet[i]].ankle_index][3], ankle);
        if (ik_ground_under_foot(physics, to_world, to_model, ankle, target, normal)) {
            // The ray is vertical, so the hit shares the ankle's x and z and this is
            // exactly how high the CLIP is holding the foot. A foot near the ground is
            // planted on it; one the clip has swung up is released, or the solve would
            // overwrite the stride and the legs would stop moving.
            // The bare ground: ik_foot_set_ground adds the ankle's own clearance above
            // the sole. Whether the foot is near enough to be planted at all is also the
            // solver's to decide -- only it sees this frame's animated pose rather than
            // last frame's solved one, which is why the release lives there and not here.
            ik_foot_set_ground(player_ik, feet[i], target, normal, ik_weight);
        } else {
            ik_foot_set_target(player_ik, feet[i], ankle, (vec3){0.0f, 1.0f, 0.0f}, 0.0f);
        }
    }
}

// Trail the player at a fixed distance behind whichever way it is facing.
//
// HERE rather than in on_render, and that is not stylistic: the engine derives the view
// and projection matrices immediately after this hook returns, so a pose written from
// the render callback is a frame late.
//
// It follows the ENTITY position rather than the rig's global_transform, for the same
// reason ik_update_targets does: the graph walk runs after this hook, so a node's global
// is one frame old, while entity->position was settled by the last fixed step.
//
// Yaw comes from player_yaw, the smoothed facing the locomotion block already maintains,
// so there is nothing to drive and nothing to learn -- and world-aligned WASD stays
// coherent, because the camera ends up behind whatever direction you walked.
static void follow_camera_update(Game* game) {
    Engine* engine = game ? game->engine : NULL;
    if (!engine || !engine->camera || !player_entity)
        return;

    vec3 focus;
    glm_vec3_copy(player_entity->position, focus);
    focus[1] += FOLLOW_CAM_LOOK_Y;

    // On the sim clock rather than the frame's, so the turn is the same headless and
    // windowed -- this hook is handed an interpolant, not a delta.
    const float look_dt = (float)game->sim_clock.delta;
    cam_yaw -= input_action_value(&game->input, "look_x") * LOOK_YAW_RATE * look_dt;
    cam_pitch += input_action_value(&game->input, "look_y") * LOOK_PITCH_RATE * look_dt;
    cam_pitch = glm_clamp(cam_pitch, CAM_PITCH_MIN, CAM_PITCH_MAX);

    /*
     * How far back to sit: close while there is ground under the player, opening out as it
     * falls away from the last ground there was.
     *
     * The signal is that DROP and not whether the player is airborne. A jump is airborne
     * too, and it only ever goes up -- so measured against the last footing its drop is at
     * or below zero for the whole rise and small on the landing, and hopping on the spot
     * moves the shot not at all. Walking off the plate is the case this exists for, and it
     * is the only one that puts real distance between the player and the last solid thing
     * under it.
     *
     * Deliberately NOT player_medium, which looks made for this and is not: MEDIUM_AIR is
     * only ever entered on a rig that ships a fall clip, so a camera keyed to it would
     * frame the generated puppet differently from an imported one.
     *
     * Swimming counts as footing. Reaching the water at the bottom is the end of the fall,
     * so the shot comes back in rather than staying wide because a controller that is
     * swimming is not "grounded".
     */
    CharacterController* cam_cc = entity_get_character_controller(player_entity);
    if ((cam_cc && character_controller_is_grounded(cam_cc)) || player_swimming)
        cam_ground_y = player_entity->position[1];
    const float drop = cam_ground_y - player_entity->position[1];
    const float wide_target =
        glm_clamp((drop - FOLLOW_CAM_DROP_START) / (FOLLOW_CAM_DROP_FULL - FOLLOW_CAM_DROP_START),
                  0.0f, 1.0f);
    const float rate = wide_target > cam_wide ? FOLLOW_CAM_WIDEN_RATE : FOLLOW_CAM_TIGHTEN_RATE;
    cam_wide += (wide_target - cam_wide) * (1.0f - expf(-rate * look_dt));

    const float distance =
        FOLLOW_CAM_NEAR_DISTANCE + (FOLLOW_CAM_FAR_DISTANCE - FOLLOW_CAM_NEAR_DISTANCE) * cam_wide;
    const float height =
        FOLLOW_CAM_NEAR_HEIGHT + (FOLLOW_CAM_FAR_HEIGHT - FOLLOW_CAM_NEAR_HEIGHT) * cam_wide;

    // Orbit the player on that heading. The camera follows POSITION and never rotates on
    // its own: the arrows are the only thing that turns it.
    const float cp = cosf(cam_pitch);
    vec3 eye = {focus[0] - sinf(cam_yaw) * cp * distance,
                focus[1] + height - sinf(cam_pitch) * distance,
                focus[2] - cosf(cam_yaw) * cp * distance};

    /*
     * Keep the camera in front of the rock instead of inside it.
     *
     * The orbit above is a pure function of yaw, pitch and the player, so nothing in it
     * knows the basin exists: stand near a wall and the eye is simply placed behind it,
     * and the shot becomes the far side of the world seen through the near side. A ray
     * from the player to where the eye WANTS to be answers that directly -- if rock is in
     * the way the arm is shortened to just short of the hit, so the shot tightens rather
     * than breaking.
     *
     * Filtered to STATIC on purpose. Crates and the door are things you walk around, not
     * things the camera should be shoved by, and letting a physics prop drive the camera
     * is how a follow cam starts lurching for reasons the player cannot see.
     *
     * This is a RAY, and that is a stated limit rather than an oversight: a zero-radius
     * probe can pass beside an edge the frustum still straddles, so a corner can clip the
     * near plane even when the ray is clear. The skin below buys most of that back, and
     * physics_world_sweep_body is the honest fix if it turns out not to be enough.
     */
    PhysicsWorld* physics = game_get_physics_world(game);
    if (physics) {
        vec3 arm;
        glm_vec3_sub(eye, focus, arm);
        const float want = glm_vec3_norm(arm);
        if (want > 1e-4f) {
            vec3 dir;
            glm_vec3_divs(arm, want, dir);
            RaycastHit hit;
            if (physics_world_raycast_filtered(physics, focus, dir, want, 1u << OBJ_LAYER_STATIC,
                                               &hit) &&
                hit.hit) {
                // Never collapse onto the player: inside its own capsule the rig fills the
                // frame and the near plane starts clipping the puppet instead of the rock.
                const float min_arm = 2.0f * PLAYER_SCALE;
                const float skin = 0.6f;
                float len = hit.distance - skin;
                if (len < min_arm)
                    len = min_arm;
                glm_vec3_scale(dir, len, arm);
                glm_vec3_add(focus, arm, eye);
            }
        }
    }

    camera_set_position(engine->camera, eye);
    camera_set_look_at(engine->camera, focus);
}

/*
 * Hand the player over to physics. Three things move together and all three
 * matter: the controller stops (so it neither steers nor pins the entity to its
 * capsule), the bodies start from the pose the rig is in right now (so the heap
 * continues the character's motion rather than snapping to bind), and the
 * entity follows the hips from here (so the camera, which frames the entity,
 * keeps framing the body).
 */
static void ragdoll_kill_player(Game* game) {
    if (!player_animator || !player_skel_root || player_ragdolled()) {
        return;
    }
    PhysicsWorld* physics = game_get_physics_world(game);
    if (!physics) {
        return;
    }
    if (!ragdoll_start(player_animator->state->ragdoll, physics->physics_system, OBJ_LAYER_DYNAMIC,
                       player_animator->state->global_transforms,
                       player_skel_root->global_transform)) {
        return;
    }
    CharacterController* cc = entity_get_character_controller(player_entity);
    if (cc) {
        cc->enabled = false;
    }
    printf("Ragdoll.\n");
}

static void on_pre_render(Game* game, double alpha) {
    (void)alpha;

    // Killing the player. Here rather than in on_update for the same reason the
    // pause toggle is: the fixed step may run any number of times in a frame,
    // and this wants to happen once.
    if (input_action_pressed(&game->input, "ragdoll")) {
        ragdoll_kill_player(game);
    }
    // The rig node moves with the character, so the ragdoll's model-to-world has
    // to follow it -- while it is active nothing else writes the node, but the
    // matrix was captured a frame before the bodies started moving.
    if (player_ragdolled() && player_skel_root) {
        ragdoll_set_world(player_animator->state->ragdoll, player_skel_root->global_transform);
        // The entity follows the hips, because the camera frames the ENTITY and
        // a disabled controller stops writing it -- without this the shot stays
        // where the character died while the body slides out of it.
        //
        // It looks circular (the node follows the entity, the ragdoll reads the
        // node) and is not: the conversion is exact either way, so moving the
        // node changes which model-space numbers come out and not where the
        // bones land in the world.
        vec3 hips;
        if (ragdoll_hips_world(player_animator->state->ragdoll, hips)) {
            glm_vec3_copy(hips, player_entity->position);
        }
    }

    // Here and not in on_update: the fixed step does not run while paused, so
    // a toggle read there could pause and never unpause.
    if (input_action_pressed(&game->input, "pause")) {
        game_toggle_pause(game);
        printf("Game %s\n", game_is_paused(game) ? "PAUSED" : "RESUMED");
    }

    ik_update_targets(game);

    Engine* engine = game->engine;
    // A menu owns the pointer while it is up: without this, clicking a button
    // also orbits the camera behind it. Asked of the INPUT layer, which already
    // holds the answer -- the frame-input pass set it from ui_captures_input --
    // rather than of the UI, which is a layer this callback should not need to
    // know about. It also composes: anything that suppresses input, menu or
    // not, gates the camera for free.
    // Either/or, the way apps/tree resolves the same collision: both the follower and
    // the orbit want to own the camera, and mouse_drag_update rewrites the eye from the
    // orbit parameters every frame, so leaving both live means the follow pose is
    // overwritten the moment the pointer moves.
    if (follow_cam && !(cam_eye_set && cam_target_set)) {
        follow_camera_update(game);
    } else if (drag_controller && app_can_process_3d_input(engine) &&
               !input_is_suppressed(&game->input)) {
        mouse_drag_update(drag_controller, glfwGetTime());
    }
}

// Render callback - runs every frame
static void on_render(Game* game, double alpha) {
    (void)alpha;

    Engine* engine = game->engine;
    const Scene* scene = game->scene;

    if (!scene || !scene->root_node) {
        return;
    }

    // Disable backface culling for glass transparency
    glDisable(GL_CULL_FACE);

    // Render the scene
    engine_render_scene(engine, game->scene);

    // Re-enable culling
    glEnable(GL_CULL_FACE);
}

// Shutdown callback
static void on_shutdown(Game* game) {
    (void)game;
    printf("Game shutting down...\n");

    if (drag_controller) {
        free_mouse_drag_controller(drag_controller);
        drag_controller = NULL;
    }

    free_save_system(save_system);
    save_system = NULL;
}

// Mouse callback for camera control
static void mouse_button_callback(Engine* engine, int button, int action, int mods) {
    (void)engine;
    if (drag_controller) {
        mouse_drag_on_button(drag_controller, button, action, mods);
    }
}

// --audio-probe: a deterministic offline render that measures the mixed PCM, so
// the `audio` gate can assert onset, panning, distance falloff, bus routing and
// the file-decode path with no device and no committed audio. Headless only.
#define AUDIO_PROBE_WINDOW 9600 // frames per measurement, 0.2s at the offline 48 kHz

static void probe_measure(AudioSystem* audio, float* rms_l, float* rms_r) {
    float buf[1024]; // up to 512 interleaved stereo frames
    double sum_l = 0.0, sum_r = 0.0;
    long total = 0;
    int remaining = AUDIO_PROBE_WINDOW;
    while (remaining > 0) {
        size_t want = remaining < 512 ? (size_t)remaining : 512;
        size_t got = audio_system_read_pcm(audio, buf, want);
        if (got == 0)
            break;
        for (size_t i = 0; i < got; i++) {
            sum_l += (double)buf[i * 2] * buf[i * 2];
            sum_r += (double)buf[i * 2 + 1] * buf[i * 2 + 1];
        }
        total += (long)got;
        remaining -= (int)got;
    }
    *rms_l = total ? sqrtf((float)(sum_l / total)) : 0.0f;
    *rms_r = total ? sqrtf((float)(sum_r / total)) : 0.0f;
}

static int run_audio_probe(Game* game, const char* which, const char* file) {
    AudioSystem* audio = create_audio_system(game->engine->headless);
    if (!audio) {
        fprintf(stderr, "audio-probe: could not create audio system\n");
        return 1;
    }
    game_set_audio_system(game, audio); // owned; freed by free_game

    vec3 origin = {0, 0, 0}, fwd = {0, 0, -1}, up = {0, 1, 0};
    audio_system_update(audio, NULL, origin, fwd, up); // fix the listener at the origin

    float l = 0.0f, r = 0.0f;
    int rc = 0;

    if (!strcmp(which, "onset")) {
        // A centred 2D tone: silent until played, then energetic.
        Sound* t = audio_sound_from_tone(audio, 440.0f, AUDIO_BUS_SFX);
        audio_sound_set_looping(t, true);
        probe_measure(audio, &l, &r);
        printf("audio onset pre rms %.6f %.6f\n", l, r);
        audio_sound_play(t);
        probe_measure(audio, &l, &r);
        printf("audio onset post rms %.6f %.6f\n", l, r);
    } else if (!strcmp(which, "pan")) {
        // Same distance either side, so only the pan differs.
        Sound* t = audio_sound_from_tone(audio, 440.0f, AUDIO_BUS_SFX);
        audio_sound_set_looping(t, true);
        audio_sound_play(t);
        audio_sound_set_position(t, (vec3){10.0f, 0.0f, 0.0f});
        probe_measure(audio, &l, &r);
        printf("audio pan right rms %.6f %.6f\n", l, r);
        audio_sound_set_position(t, (vec3){-10.0f, 0.0f, 0.0f});
        probe_measure(audio, &l, &r);
        printf("audio pan left rms %.6f %.6f\n", l, r);
    } else if (!strcmp(which, "distance")) {
        // Straight ahead, so panning is even; only the distance differs.
        Sound* t = audio_sound_from_tone(audio, 440.0f, AUDIO_BUS_SFX);
        audio_sound_set_looping(t, true);
        audio_sound_play(t);
        audio_sound_set_position(t, (vec3){0.0f, 0.0f, -1.0f});
        probe_measure(audio, &l, &r);
        printf("audio distance near rms %.6f %.6f\n", l, r);
        audio_sound_set_position(t, (vec3){0.0f, 0.0f, -20.0f});
        probe_measure(audio, &l, &r);
        printf("audio distance far rms %.6f %.6f\n", l, r);
    } else if (!strcmp(which, "master")) {
        Sound* t = audio_sound_from_tone(audio, 440.0f, AUDIO_BUS_SFX);
        audio_sound_set_looping(t, true);
        audio_sound_play(t);
        audio_set_bus_volume(audio, AUDIO_BUS_MASTER, 1.0f);
        probe_measure(audio, &l, &r);
        printf("audio master on rms %.6f %.6f\n", l, r);
        audio_set_bus_volume(audio, AUDIO_BUS_MASTER, 0.0f);
        probe_measure(audio, &l, &r);
        printf("audio master off rms %.6f %.6f\n", l, r);
    } else if (!strcmp(which, "decode")) {
        if (!file) {
            fprintf(stderr, "audio-probe decode: needs --audio-file <wav>\n");
            rc = 1;
        } else {
            Sound* s = audio_sound_from_file(audio, file, AUDIO_BUS_SFX);
            if (!s) {
                fprintf(stderr, "audio-probe decode: could not load %s\n", file);
                rc = 1;
            } else {
                audio_sound_play(s);
                probe_measure(audio, &l, &r);
                printf("audio decode result rms %.6f %.6f\n", l, r);
            }
        }
    } else {
        fprintf(stderr, "audio-probe: unknown case '%s'\n", which);
        rc = 1;
    }
    return rc;
}

// --ik-probe: the two-bone solver as a pure function (spec 12.4) and the lock above it
// (spec 12.9) and stride matching above that (spec 12.10). The eleven rig-only cases here
// need no physics and no frame at all -- a solve is (hip, target, thigh, shin, pole)
// and nothing else, so giving them a world would make exact arithmetic depend on
// Jolt's contact slop for no gain. The cases that DO need a raycast are the ones that
// stand the rig on real ground, and they are separate for that reason.
//
// Prints `ik <case> <label> <key> <numbers...>` at %.6f, the grammar the audio, anim,
// ui and save probes already share. Note the gate's regex admits no letters and no
// exponent, so a nan or an inf prints a line it cannot match: the key vanishes from
// the dict and the arm fails on absence. NaN is caught for free, on every arm.
#define IK_PROBE_HIP   "cetra_rig:LeftUpLeg"
#define IK_PROBE_KNEE  "cetra_rig:LeftLeg"
#define IK_PROBE_ANKLE "cetra_rig:LeftFoot"

// The walk the `lock` case ticks, and the generated one cannot stand in for it: gait()
// swings thighs about a straight leg, so its foot traces an arc and never dwells. A
// contact phase is the thing locking holds, and only a real cycle has one.
#define WALK_CLIP "assets/models/strut_walk.fbx"
// A foot within this fraction of its own LIFT of its lowest point is in contact. The
// instrument states this itself rather than reading the solver's label, because an arm
// that took its window from the thing it measures could be fooled by a label wrong in
// the same direction as the solve.
#define IK_LOCK_BAND   0.15f
#define IK_LOCK_CYCLES 3
#define IK_LOCK_TICKS  720
// The third pass walks the body at three times the speed the clip's own feet imply,
// which is the mismatch foot locking exists in the presence of and also the one it must
// refuse to paper over. Every lock breaks its unlock distance almost at once; what the
// arm reads is that the feature LETS GO rather than straining the chain to hold on.
#define IK_LOCK_FAST 3.0f

// The travel speeds the `rate` case walks the body at, as multiples of the clip's own
// stride: the EDGES of the band `ik_rate_playback` will stretch a clip over, and the free
// operating point in the middle that the `lock` case already measures -- carried here so
// all three are read off one instrument and a regression at the easy speed cannot hide.
//
// Derived from the band rather than written beside it, so the case always tests exactly
// what the policy claims. Testing outside the band would measure the clamp instead: past
// it the honest answer is that this clip cannot carry that speed, and a game is meant to
// be blending to another entry rather than stretching further.
#define IK_RATE_COUNT 3
static const float IK_RATE_MULT[IK_RATE_COUNT] = {ANIM_RATE_MIN, 1.0f, ANIM_RATE_MAX};
static const char* IK_RATE_TAG[IK_RATE_COUNT] = {"slow", "one", "fast"};

// The bend the gate states in closed form: the angle at the knee for a hip-to-ankle
// distance c, which is acos((c^2 - a^2 - b^2) / 2ab) measured as the deviation from
// straight. Read off the solved globals rather than recomputed, so what is asserted is
// what the solver actually produced.
static float ik_probe_bend_deg(const mat4* g, const IkFoot* foot) {
    vec3 a, b, c, thigh, shin;
    glm_vec3_copy((float*)g[foot->hip_index][3], a);
    glm_vec3_copy((float*)g[foot->knee_index][3], b);
    glm_vec3_copy((float*)g[foot->ankle_index][3], c);
    glm_vec3_sub(b, a, thigh);
    glm_vec3_sub(c, b, shin);
    if (glm_vec3_norm(thigh) < 1e-8f || glm_vec3_norm(shin) < 1e-8f)
        return 0.0f;
    glm_vec3_normalize(thigh);
    glm_vec3_normalize(shin);
    return glm_deg(acosf(glm_clamp(glm_vec3_dot(thigh, shin), -1.0f, 1.0f)));
}

static float ik_probe_residual(const mat4* g, const IkFoot* foot, const vec3 target) {
    vec3 ankle;
    glm_vec3_copy((float*)g[foot->ankle_index][3], ankle);
    return glm_vec3_distance(ankle, (float*)target);
}

// The longest run of consecutive true ticks, and where it starts. Several cycles are
// recorded so at least one stance falls wholly inside the recording rather than straddling
// the loop point, where one run would read as two short ones.
// How many ticks are true, and how many separate runs of true there are. Written once
// rather than three times: the rising-edge test carries a `t == 0` boundary case that is
// three chances to get wrong, and two of the three counts feed an exact-equality arm.
static int ik_probe_count(const bool* flag, int n) {
    int hits = 0;
    for (int i = 0; i < n; i++)
        hits += flag[i] ? 1 : 0;
    return hits;
}

static int ik_probe_runs(const bool* flag, int n) {
    int runs = 0;
    for (int i = 0; i < n; i++)
        runs += (flag[i] && (i == 0 || !flag[i - 1])) ? 1 : 0;
    return runs;
}

static int ik_probe_longest_run(const bool* flag, int n, int* start) {
    int best = 0, best_at = 0, run = 0;
    for (int i = 0; i < n; i++) {
        if (flag[i]) {
            run++;
            if (run > best) {
                best = run;
                best_at = i - run + 1;
            }
        } else {
            run = 0;
        }
    }
    *start = best_at;
    return best;
}

// Where a foot stands still, and how fast the ground goes past it while it does.
typedef struct IkProbeStance {
    float low, lift; // the ankle's lowest point over the recording, and its total travel
    int start, run;  // the longest contact window, in ticks
    float stride;    // the ground speed that window implies, model units per second
    vec3 dir;        // the direction the body travels to make that so
} IkProbeStance;

// The stance window and the speed it implies, read off a RECORDING of one foot: `ankle`
// and `toe` are its model-space positions over `ticks` ticks at 60 Hz, and `inside` comes
// back as the window's own per-tick flag, which a caller can cross against a solver's
// label.
//
// The window is where the ankle sits within IK_LOCK_BAND of its own lift. The speed is
// the least-squares slope of the TOE's horizontal travel across the whole window rather
// than across its two endpoints: a foot that rocks at heel-strike and toe-off moves the
// endpoints without moving the average, and the endpoint form read a stride 2x short. The
// toe is what a lock holds, so the toe is what has to come out stationary.
//
// False, with the reason on stderr, for a recording with no window or a stance that does
// not travel -- which is a real answer about the clip and not a failure of the fit.
static bool ik_probe_stance(const vec3* ankle, const vec3* toe, int ticks, bool* inside,
                            IkProbeStance* out) {
    // The band is a fraction of the foot's OWN lift, not of the leg. A clip that barely
    // picks its feet up and one that marches both want "near the bottom of this foot's
    // travel", and a band in leg lengths read 77 per cent of the cycle as contact.
    float high = -1e9f;
    out->low = 1e9f;
    for (int t = 0; t < ticks; t++) {
        if (ankle[t][1] < out->low)
            out->low = ankle[t][1];
        if (ankle[t][1] > high)
            high = ankle[t][1];
    }
    out->lift = high - out->low;
    for (int t = 0; t < ticks; t++)
        inside[t] = ankle[t][1] <= out->low + IK_LOCK_BAND * out->lift;
    out->run = ik_probe_longest_run(inside, ticks, &out->start);
    if (out->run < 3 || out->lift < 1e-5f) {
        fprintf(stderr, "ik-probe: no contact window in %d ticks\n", ticks);
        return false;
    }

    const int s = out->start, e = out->start + out->run - 1;
    float mean_t = 0.0f, var_t = 0.0f;
    vec3 mean_p = {0.0f, 0.0f, 0.0f}, slope = {0.0f, 0.0f, 0.0f};
    for (int t = s; t <= e; t++) {
        mean_t += (float)t;
        mean_p[0] += toe[t][0];
        mean_p[2] += toe[t][2];
    }
    mean_t /= (float)out->run;
    mean_p[0] /= (float)out->run;
    mean_p[2] /= (float)out->run;
    for (int t = s; t <= e; t++) {
        const float dt = (float)t - mean_t;
        var_t += dt * dt;
        slope[0] += dt * (toe[t][0] - mean_p[0]);
        slope[2] += dt * (toe[t][2] - mean_p[2]);
    }
    if (var_t <= 0.0f) {
        fprintf(stderr, "ik-probe: the contact window has no extent\n");
        return false;
    }
    glm_vec3_scale(slope, 60.0f / var_t, slope); // per tick, then per second
    out->stride = glm_vec3_norm(slope);
    if (out->stride < 1e-5f) {
        fprintf(stderr, "ik-probe: the stance foot does not travel\n");
        return false;
    }
    // The body goes the way the stance foot does not.
    glm_vec3_scale(slope, -1.0f / out->stride, out->dir);
    return true;
}

// How far a held contact travels across the WORLD over `run` ticks from `start`: the
// toe's recorded model position with the body's own travel added back in, measured from
// where it stood on the first tick of the window. `step` comes back as the largest single
// tick of that travel, which is what a transition that pops looks like and what the total
// cannot see -- a foot that jumps once and then holds still reports the same drift as one
// that eases over the whole stance.
//
// Read at the TOE and flattened to the ground plane: a lock holds the toe and leaves the
// heel free, so an ankle that lifts at toe-off is the clip being preserved rather than a
// contact slipping.
// (`toe` and `dir` are read-only here; cglm's vector calls are not const-qualified.)
static float ik_probe_drift(vec3* toe, int start, int run, vec3 dir, float speed, float* step) {
    vec3 anchor, prev = {0.0f, 0.0f, 0.0f};
    float worst = 0.0f;
    *step = 0.0f;
    glm_vec3_scale(dir, speed * (float)start / 60.0f, anchor);
    glm_vec3_add(toe[start], anchor, anchor);
    anchor[1] = 0.0f;
    for (int t = start; t < start + run; t++) {
        vec3 body, world;
        glm_vec3_scale(dir, speed * (float)t / 60.0f, body);
        glm_vec3_add(toe[t], body, world);
        world[1] = 0.0f;
        const float drift = glm_vec3_distance(world, anchor);
        if (drift > worst)
            worst = drift;
        if (t > start) {
            const float d = glm_vec3_distance(world, prev);
            if (d > *step)
                *step = d;
        }
        glm_vec3_copy(world, prev);
    }
    return worst;
}

// A rig that walks: an animator playing `clip` with an IK system on the pose seam, both
// legs, both toes and the pelvis wired. `plant_only` zeroes lock_distance, which is the
// 12.4 behaviour and the A in every A/B here.
//
// The animator owns the system through `state->ik` and frees it, so one free_animator
// releases both. NULL with the reason on stderr when the rig cannot carry the case --
// and the toes are REFUSED rather than falling back to the ankle, since a case that
// quietly measured an ankle lock would report a number for a feature nobody asked for.
static Animator* ik_probe_walker(Skeleton* skel, Animation* clip, float stride, bool plant_only,
                                 int* left, int* right) {
    Animator* an = create_animator(skel);
    IkSystem* sys = create_ik_system(skel);
    if (!an || !sys) {
        fprintf(stderr, "ik-probe: could not build the walking rig\n");
        free_animator(an);
        free_ik_system(sys);
        return NULL;
    }
    *left = ik_add_foot(sys, "cetra_rig:LeftUpLeg", "cetra_rig:LeftLeg", "cetra_rig:LeftFoot",
                        (vec3){0.0f, 0.0f, 1.0f});
    *right = ik_add_foot(sys, "cetra_rig:RightUpLeg", "cetra_rig:RightLeg", "cetra_rig:RightFoot",
                         (vec3){0.0f, 0.0f, 1.0f});
    if (*left < 0 || *right < 0 || !ik_set_pelvis(sys, "cetra_rig:Hips")) {
        fprintf(stderr, "ik-probe: the rig lacks both legs\n");
        free_animator(an);
        free_ik_system(sys);
        return NULL;
    }
    if (!ik_foot_set_toe(sys, *left, "cetra_rig:LeftToeBase") ||
        !ik_foot_set_toe(sys, *right, "cetra_rig:RightToeBase")) {
        fprintf(stderr, "ik-probe: the rig lacks toe bones\n");
        free_animator(an);
        free_ik_system(sys);
        return NULL;
    }
    if (plant_only)
        sys->params.lock_distance = 0.0f;
    an->state->ik = sys;
    // A one-entry SPACE rather than animator_play, because a bare clip carries no stride
    // and ik_rate_playback would have nothing to divide by. The arithmetic is identical:
    // a one-entry space is the single-clip path exactly (animator.h).
    const AnimatorEntry only = {clip, 0.0f, stride};
    animator_play_space(an, clip->name, &only, 1, 0.0f, true);
    return an;
}

// The world the three PLANTING cases need, on the scene the caller has already set: a
// physics world, the floor on_init would have built, the ramp and steps, and a real
// character capsule at the stand point.
//
// The capsule is not decoration. ik-ray-self asserts a foot ray finds the world and
// not the body it was cast from, and in a world with no body that assertion is
// vacuously true -- the arm would pass on a solver that had no filter at all.
static PhysicsWorld* ik_probe_world(Game* game, const vec3 stand) {
    PhysicsConfig physics_config = physics_default_config();
    PhysicsWorld* physics = create_physics_world(&physics_config);
    if (!physics)
        return NULL;
    game_set_physics_world(game, physics);
    EntityManager* em = create_entity_manager(game);
    game_set_entity_manager(game, em);
    pbr_shader = engine_get_program(game->engine, CETRA_PROGRAM_PBR);

    Entity* floor = create_entity(em, "floor");
    glm_vec3_copy((vec3){0.0f, -GAMETEST_FLOOR_HALF_Y, 0.0f}, floor->position);
    PhysicsShapeDesc floor_shape = {.type = SHAPE_BOX,
                                    .box.half_extents = {25.0f, GAMETEST_FLOOR_HALF_Y, 25.0f},
                                    .density = 0.0f};
    entity_add_rigid_body(floor, physics, &floor_shape, MOTION_STATIC, OBJ_LAYER_STATIC);

    build_ik_ground(game);

    Entity* player = create_entity(em, "player");
    glm_vec3_copy((float*)stand, player->position);
    CharacterControllerConfig cc = character_controller_default_config();
    cc.capsule_radius = PLAYER_RADIUS;
    cc.capsule_half_height = PLAYER_HALF_H;
    entity_add_character_controller(player, physics, &cc);

    physics_world_optimize(physics);
    return physics;
}

/*
 * How far each joint has come apart, named, as a fraction of the limb hanging
 * off it. The measurement is the distance from a bone to the one it hangs from
 * against the same distance in the BIND pose: a chain joined at the bone heads
 * preserves it exactly whatever the pose, so the drift is the whole answer.
 *
 * It is reported against the CHILD's own capsule length rather than against
 * that bind distance, which is the trap this arm's first draft fell into -- a
 * hip and a thigh begin almost on top of each other, so dividing by how far
 * apart they belong turns a millimetre into a ratio of three and says the
 * pelvis exploded. A joint's natural scale is the limb hanging off it.
 *
 * Called twice, which is what separates the two ways of coming apart: BUILT
 * apart, before a single step has run, or pulled apart by a simulation whose
 * constraints do not hold.
 */
static void rd_probe_rigid(RagdollSystem* rd, const mat4* bind, mat4* globals) {
    ragdoll_apply(rd, globals);
    for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
        const int bone = ragdoll_bone_index(rd, i);
        const int parent_slot = ragdoll_bone_parent(rd, i);
        if (bone < 0 || parent_slot < 0)
            continue;
        const int parent_bone = ragdoll_bone_index(rd, parent_slot);
        float r = 0.0f, hh = 0.0f;
        ragdoll_capsule(rd, i, &r, &hh);
        const float limb = 2.0f * (r + hh);
        if (limb < 1e-4f)
            continue;
        const float want = glm_vec3_distance((float*)bind[bone][3], (float*)bind[parent_bone][3]);
        const float got = glm_vec3_distance(globals[bone][3], globals[parent_bone][3]);
        printf("ragdoll rigid %s drift %.6f %.6f\n", ragdoll_bone_name(i),
               (double)fabsf(got - want), (double)(fabsf(got - want) / limb));
    }
}

/*
 * What a ragdoll BUILDS and what it DOES, with no window (spec 12.16).
 *
 * Unlike the display probe, the simulation itself is reachable here -- a
 * headless game carries a real physics world -- so these cases step Jolt and
 * read the bodies back rather than asserting only the arithmetic.
 */
static int run_ragdoll_probe(Game* game, const char* which) {
    const bool all = !which || !strcmp(which, "all");
    bool ran = false;

    Scene* probe_scene =
        create_scene_from_model_path(puppet_path, NULL, game->engine->async_loader);
    if (!probe_scene || probe_scene->skeleton_count == 0) {
        fprintf(stderr, "ragdoll-probe: could not load a rig from '%s'\n", puppet_path);
        if (probe_scene)
            free_scene(probe_scene);
        return 1;
    }
    game_set_scene(game, probe_scene);
    Skeleton* skel = probe_scene->skeletons[0];

    // The mesh whose per-bone boxes measure the capsules. NULL is legal and
    // takes the fallback radius, which is what a rig with no skin gets.
    const Mesh* skinned[RD_MAX_SKINNED];
    const size_t skinned_count =
        collect_skinned_meshes(probe_scene->root_node, skinned, RD_MAX_SKINNED);

    if (all || !strcmp(which, "build") || !strcmp(which, "shapes")) {
        ran = true;
        RagdollSystem* rd = create_ragdoll(skel, skinned, skinned_count, 1.0f);
        if (!rd) {
            fprintf(stderr, "ragdoll-probe: build refused\n");
            return 1;
        }
        int bodies = 0, parented = 0;
        for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
            const char* slot = ragdoll_bone_name(i);
            const int bone = ragdoll_bone_index(rd, i);
            // Two numbers: the skeleton bone, and the parent SLOT the build
            // settled on. The second is what makes the constraint count an
            // assertion instead of an identity -- read back from the system
            // rather than restated as bodies minus one -- and it carries the
            // parent-first ordering CreateRagdoll depends on with it, since a
            // parent slot below its child's is the whole rule.
            printf("ragdoll build %s bone %d %d\n", slot, bone, ragdoll_bone_parent(rd, i));
            if (bone < 0) {
                continue;
            }
            bodies++;
            if (ragdoll_bone_parent(rd, i) >= 0) {
                parented++;
            }
            float r = 0.0f, hh = 0.0f;
            ragdoll_capsule(rd, i, &r, &hh);
            printf("ragdoll shapes %s capsule %.6f %.6f\n", slot, (double)r, (double)hh);
        }
        printf("ragdoll build count bodies %d\n", bodies);
        printf("ragdoll build count constraints %d\n", parented);
        free_ragdoll(rd);
    }

    if (all || !strcmp(which, "scale")) {
        ran = true;
        // The same rig at two node scales. Everything here is measured in MODEL
        // space and multiplied once, so the two must differ by exactly the
        // ratio -- 12.10 shipped a live bug through precisely this gap.
        RagdollSystem* one = create_ragdoll(skel, skinned, skinned_count, 1.0f);
        RagdollSystem* two = create_ragdoll(skel, skinned, skinned_count, 2.0f);
        if (!one || !two) {
            fprintf(stderr, "ragdoll-probe: scale build refused\n");
            return 1;
        }
        for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
            float r1 = 0.0f, h1 = 0.0f, r2 = 0.0f, h2 = 0.0f;
            if (!ragdoll_capsule(one, i, &r1, &h1))
                continue;
            ragdoll_capsule(two, i, &r2, &h2);
            const double rr = (r1 > 0.0f) ? (double)r2 / (double)r1 : 0.0;
            const double hr = (h1 > 0.0f) ? (double)h2 / (double)h1 : 0.0;
            printf("ragdoll scale %s ratio %.6f %.6f\n", ragdoll_bone_name(i), rr, hr);
        }
        free_ragdoll(one);
        free_ragdoll(two);
    }

    if (all || !strcmp(which, "settles") || !strcmp(which, "pose") || !strcmp(which, "frees")) {
        ran = true;
        // A floor to land on, and a body count to compare against. The world is
        // the ik probe's, which already stands a capsule on a plate for the
        // same reason: an assertion against a world with nothing in it is
        // vacuously true.
        PhysicsWorld* physics = ik_probe_world(game, (vec3){0.0f, 0.0f, 0.0f});
        if (!physics) {
            fprintf(stderr, "ragdoll-probe: no physics world\n");
            return 1;
        }

        AnimationState* state = create_animation_state(skel);
        RagdollSystem* rd = create_ragdoll(skel, skinned, skinned_count, 1.0f);
        if (!state || !rd) {
            fprintf(stderr, "ragdoll-probe: could not build\n");
            return 1;
        }
        state->ragdoll = rd; // the state owns it from here

        // The bind pose, lifted clear of the floor so the fall is the thing
        // being measured rather than the initial overlap.
        compute_bind_pose_matrices(state);
        mat4 to_world = GLM_MAT4_IDENTITY_INIT;
        to_world[3][1] = 3.0f;

        const int before_bodies = jolt_ragdoll_world_body_count(physics->physics_system);
        if (!ragdoll_start(rd, physics->physics_system, OBJ_LAYER_DYNAMIC, state->global_transforms,
                           to_world)) {
            fprintf(stderr, "ragdoll-probe: start refused\n");
            return 1;
        }
        const int after_bodies = jolt_ragdoll_world_body_count(physics->physics_system);

        const int hips_bone = ragdoll_bone_index(rd, RAGDOLL_HIPS);
        vec3 hips0 = {0.0f, 0.0f, 0.0f};
        ragdoll_hips_world(rd, hips0);
        // The clip's own answer for a bone, kept so the pose case can show the
        // body overrode it rather than merely differing from bind.
        mat4 clip_pose;
        glm_mat4_copy(state->global_transforms[hips_bone], clip_pose);

        /*
         * Before a single step: the pose that went in has to come straight back
         * out. Placing a body from a bone and reading a bone back from a body
         * are inverses by construction, so any offset here was BUILT in and
         * nothing the simulation does afterwards can be read as physics.
         *
         * It is not a tautology over the same arithmetic, because the pose goes
         * THROUGH Jolt in between -- and Jolt keeps an orientation as a
         * quaternion, which is where 12.16's worst defect lived: a left-handed
         * capsule basis is a reflection, no quaternion is one, and the nearest
         * rotation came back instead. Every existing arm stayed green.
         */
        mat4* rigid_bind = calloc(skel->bone_count, sizeof(mat4));
        mat4* scratch = calloc(skel->bone_count, sizeof(mat4));
        if (rigid_bind && scratch) {
            skeleton_compute_bind_globals(skel, rigid_bind);
            memcpy(scratch, state->global_transforms, skel->bone_count * sizeof(mat4));
            ragdoll_apply(rd, scratch);
            for (int i = 0; i < RAGDOLL_BONE_COUNT; i++) {
                const int bone = ragdoll_bone_index(rd, i);
                if (bone < 0)
                    continue;
                printf(
                    "ragdoll roundtrip %s off %.6f\n", ragdoll_bone_name(i),
                    (double)glm_vec3_distance(state->global_transforms[bone][3], scratch[bone][3]));
            }
        }
        free(scratch);

        for (int step = 0; step < 300; step++) {
            physics_world_update(physics, 1.0f / 60.0f, 4);
        }

        vec3 hips1 = {0.0f, 0.0f, 0.0f};
        ragdoll_hips_world(rd, hips1);
        // One more pair of steps to measure what is left moving, which is what
        // separates settled from slowly sliding.
        vec3 hips2 = {0.0f, 0.0f, 0.0f};
        physics_world_update(physics, 1.0f / 60.0f, 4);
        ragdoll_hips_world(rd, hips2);
        const float residual = glm_vec3_distance(hips1, hips2) * 60.0f;

        printf("ragdoll settles hips drop %.6f\n", (double)(hips0[1] - hips1[1]));
        printf("ragdoll settles hips speed %.6f\n", (double)residual);

        // The pose case: the bone's global now comes from its body. Applied
        // here rather than through the animator, because the animator would
        // re-sample a clip this rig may not have.
        ragdoll_apply(rd, state->global_transforms);
        const float moved = glm_vec3_distance(clip_pose[3], state->global_transforms[hips_bone][3]);
        printf("ragdoll pose hips moved %.6f\n", (double)moved);

        // Whether it is still a BODY, which is the one thing every arm above can
        // be green without: settling, freeing and taking its pose from its
        // bodies are all true of a cloud of limbs that never touch.
        if (rigid_bind) {
            rd_probe_rigid(rd, rigid_bind, state->global_transforms);
            free(rigid_bind);
        }

        free_animation_state(state); // frees the ragdoll, which removes its bodies
        const int freed_bodies = jolt_ragdoll_world_body_count(physics->physics_system);
        printf("ragdoll frees world bodies %d %d %d\n", before_bodies, after_bodies, freed_bodies);
    }

    if (!ran) {
        fprintf(stderr, "ragdoll-probe: unknown case '%s'\n", which ? which : "(null)");
        return 1;
    }
    return 0;
}

static int run_ik_probe(Game* game, const char* which) {
    Scene* probe_scene =
        create_scene_from_model_path(puppet_path, NULL, game->engine->async_loader);
    if (!probe_scene || probe_scene->skeleton_count == 0) {
        fprintf(stderr, "ik-probe: could not load a rig from '%s'\n", puppet_path);
        if (probe_scene)
            free_scene(probe_scene);
        return 1;
    }
    game_set_scene(game, probe_scene);

    Skeleton* skel = probe_scene->skeletons[0];
    AnimationState* state = create_animation_state(skel);
    IkSystem* ik = create_ik_system(skel);
    if (!state || !ik) {
        fprintf(stderr, "ik-probe: could not build a state or an IK system\n");
        return 1;
    }
    // +Z is the direction the puppet faces and the way its jump clip bends the knee.
    const int foot_index =
        ik_add_foot(ik, IK_PROBE_HIP, IK_PROBE_KNEE, IK_PROBE_ANKLE, (vec3){0.0f, 0.0f, 1.0f});
    if (foot_index < 0) {
        fprintf(stderr, "ik-probe: the rig lacks the left leg chain\n");
        return 1;
    }
    const IkFoot* foot = &ik->feet[foot_index];
    state->ik = ik; // the state owns it from here, and frees it

    // Bind gives the hip and the segment lengths this rig actually has, rather than
    // numbers restated here: thigh and shin measured, and the hip to solve from.
    compute_bind_pose_matrices(state);
    vec3 hip, knee, ankle;
    glm_vec3_copy(state->global_transforms[foot->hip_index][3], hip);
    glm_vec3_copy(state->global_transforms[foot->knee_index][3], knee);
    glm_vec3_copy(state->global_transforms[foot->ankle_index][3], ankle);
    const float seg_a = glm_vec3_distance(hip, knee);
    const float seg_b = glm_vec3_distance(knee, ankle);

    // Every case re-poses from bind first, so one case cannot leave the rig somewhere
    // the next one reads.
    ik->params.max_pelvis_drop = 0.0f;
    ik->params.transition_time = 0.0f;

    int rc = 0;

    if (!strcmp(which, "reach")) {
        // Nine directions at a distance the leg can certainly make, each solved from a
        // fresh bind pose. A solver that merely POINTS at a target passes nothing here.
        const float c = (seg_a + seg_b) * 0.85f;
        const vec3 dirs[9] = {{0, -1, 0},        {0.3f, -1, 0},     {-0.3f, -1, 0},
                              {0, -1, 0.3f},     {0, -1, -0.3f},    {0.2f, -1, 0.2f},
                              {-0.2f, -1, 0.2f}, {0.2f, -1, -0.2f}, {0.5f, -1, 0.1f}};
        for (int i = 0; i < 9; i++) {
            compute_bind_pose_matrices(state);
            vec3 dir, target;
            glm_vec3_copy((float*)dirs[i], dir);
            glm_vec3_normalize(dir);
            glm_vec3_scale(dir, c, target);
            glm_vec3_add(hip, target, target);
            ik_reset(ik);
            ik_foot_set_target(ik, foot_index, target, (vec3){0, 1, 0}, 1.0f);
            ik_solve(ik, state->global_transforms, 0.0f);
            printf("ik reach t%d residual %.6f\n", i,
                   (double)ik_probe_residual(state->global_transforms, foot, target));
            printf("ik reach t%d bend %.6f\n", i,
                   (double)ik_probe_bend_deg(state->global_transforms, foot));
        }
    } else if (!strcmp(which, "clamp")) {
        // Beyond reach the leg must go straight AND stay aimed; a target ON the hip is
        // the other unreachable end and must not divide by a zero-length direction.
        compute_bind_pose_matrices(state);
        vec3 far_target = {hip[0], hip[1] - (seg_a + seg_b) * 2.0f, hip[2]};
        ik_reset(ik);
        ik_foot_set_target(ik, foot_index, far_target, (vec3){0, 1, 0}, 1.0f);
        ik_solve(ik, state->global_transforms, 0.0f);
        vec3 solved_ankle, leg_dir, want_dir;
        glm_vec3_copy(state->global_transforms[foot->ankle_index][3], solved_ankle);
        glm_vec3_sub(solved_ankle, hip, leg_dir);
        glm_vec3_sub(far_target, hip, want_dir);
        glm_vec3_normalize(leg_dir);
        glm_vec3_normalize(want_dir);
        printf("ik clamp far dist %.6f\n", (double)glm_vec3_distance(solved_ankle, hip));
        printf("ik clamp far bend %.6f\n",
               (double)ik_probe_bend_deg(state->global_transforms, foot));
        printf("ik clamp far align %.6f\n", (double)glm_vec3_dot(leg_dir, want_dir));

        compute_bind_pose_matrices(state);
        ik_reset(ik);
        ik_foot_set_target(ik, foot_index, hip, (vec3){0, 1, 0}, 1.0f);
        ik_solve(ik, state->global_transforms, 0.0f);
        glm_vec3_copy(state->global_transforms[foot->ankle_index][3], solved_ankle);
        printf("ik clamp atHip dist %.6f\n", (double)glm_vec3_distance(solved_ankle, hip));
        printf("ik clamp atHip bend %.6f\n",
               (double)ik_probe_bend_deg(state->global_transforms, foot));
    } else if (!strcmp(which, "singular")) {
        // The two places acos's argument lands exactly on -1 or +1, where one ulp of
        // float error is a NaN. Full extension first, then the inner fold.
        compute_bind_pose_matrices(state);
        vec3 t = {hip[0], hip[1] - (seg_a + seg_b), hip[2]};
        ik_reset(ik);
        ik_foot_set_target(ik, foot_index, t, (vec3){0, 1, 0}, 1.0f);
        ik_solve(ik, state->global_transforms, 0.0f);
        printf("ik singular full bend %.6f\n",
               (double)ik_probe_bend_deg(state->global_transforms, foot));
        printf(
            "ik singular full dist %.6f\n",
            (double)glm_vec3_distance((float*)state->global_transforms[foot->ankle_index][3], hip));

        compute_bind_pose_matrices(state);
        vec3 folded = {hip[0], hip[1] - fabsf(seg_a - seg_b), hip[2]};
        ik_reset(ik);
        ik_foot_set_target(ik, foot_index, folded, (vec3){0, 1, 0}, 1.0f);
        ik_solve(ik, state->global_transforms, 0.0f);
        printf("ik singular folded bend %.6f\n",
               (double)ik_probe_bend_deg(state->global_transforms, foot));
        printf(
            "ik singular folded dist %.6f\n",
            (double)glm_vec3_distance((float*)state->global_transforms[foot->ankle_index][3], hip));
    } else if (!strcmp(which, "identity")) {
        // Asking for the ankle where it ALREADY is changes nothing -- and from a BENT
        // pose, not the bind one, so this is idempotence rather than a restatement of
        // the singular case. A weight of 0 must also be bit-identical to no IK at all.
        compute_bind_pose_matrices(state);
        vec3 bent = {hip[0], hip[1] - (seg_a + seg_b) * 0.8f, hip[2] + 0.05f};
        ik_reset(ik);
        ik_foot_set_target(ik, foot_index, bent, (vec3){0, 1, 0}, 1.0f);
        ik_solve(ik, state->global_transforms, 0.0f);

        mat4 before[3];
        glm_mat4_copy(state->global_transforms[foot->hip_index], before[0]);
        glm_mat4_copy(state->global_transforms[foot->knee_index], before[1]);
        glm_mat4_copy(state->global_transforms[foot->ankle_index], before[2]);

        vec3 settled;
        glm_vec3_copy(state->global_transforms[foot->ankle_index][3], settled);
        ik_foot_set_target(ik, foot_index, settled, (vec3){0, 1, 0}, 1.0f);
        ik_solve(ik, state->global_transforms, 0.0f);

        float worst = 0.0f;
        const int idx[3] = {foot->hip_index, foot->knee_index, foot->ankle_index};
        for (int j = 0; j < 3; j++) {
            const float* m = (const float*)state->global_transforms[idx[j]];
            const float* n = (const float*)before[j];
            for (int k = 0; k < 16; k++) {
                const float diff = fabsf(m[k] - n[k]);
                if (diff > worst)
                    worst = diff;
            }
        }
        printf("ik identity bent maxdiff %.6f\n", (double)worst);

        compute_bind_pose_matrices(state);
        mat4 untouched[3];
        for (int j = 0; j < 3; j++)
            glm_mat4_copy(state->global_transforms[idx[j]], untouched[j]);
        ik_foot_set_target(ik, foot_index, bent, (vec3){0, 1, 0}, 0.0f);
        ik_solve(ik, state->global_transforms, 0.0f);

        // All three bones, as the bent half above already does. Comparing the knee
        // alone let a weight-0 solve that rotated the hip or the ankle pass an arm
        // whose docstring claims the POSE is unchanged.
        float zero_worst = 0.0f;
        for (int j = 0; j < 3; j++) {
            const float* m = (const float*)state->global_transforms[idx[j]];
            const float* n = (const float*)untouched[j];
            for (int k = 0; k < 16; k++) {
                const float diff = fabsf(m[k] - n[k]);
                if (diff > zero_worst)
                    zero_worst = diff;
            }
        }
        printf("ik identity zeroweight maxdiff %.6f\n", (double)zero_worst);
    } else if (!strcmp(which, "pole")) {
        // The knee goes the way the pole says, and reversing the pole reverses it. The
        // SPAN between the two is what proves the pole is read at all rather than
        // defaulted, which a sign test alone would not catch.
        float knee_z[2];
        for (int s = 0; s < 2; s++) {
            free_ik_system(state->ik);
            state->ik = NULL;
            IkSystem* sys = create_ik_system(skel);
            if (!sys) {
                fprintf(stderr, "ik-probe: could not rebuild the system\n");
                return 1;
            }
            sys->params.max_pelvis_drop = 0.0f;
            sys->params.transition_time = 0.0f;
            const float sign = s == 0 ? 1.0f : -1.0f;
            const int fi = ik_add_foot(sys, IK_PROBE_HIP, IK_PROBE_KNEE, IK_PROBE_ANKLE,
                                       (vec3){0.0f, 0.0f, sign});
            state->ik = sys;
            compute_bind_pose_matrices(state);
            vec3 t = {hip[0], hip[1] - (seg_a + seg_b) * 0.9f, hip[2]};
            ik_reset(sys);
            ik_foot_set_target(sys, fi, t, (vec3){0, 1, 0}, 1.0f);
            ik_solve(sys, state->global_transforms, 0.0f);
            knee_z[s] = state->global_transforms[sys->feet[fi].knee_index][3][2] - hip[2];
        }
        printf("ik pole fwd kneez %.6f\n", (double)knee_z[0]);
        printf("ik pole back kneez %.6f\n", (double)knee_z[1]);
        printf("ik pole span delta %.6f\n", (double)fabsf(knee_z[0] - knee_z[1]));
    } else if (!strcmp(which, "analytic")) {
        // The arm this group is anchored on. The gate computes the same closed form in
        // Python and matches it; the sweep stops short of full extension deliberately,
        // because the derivative there is unbounded and a nearer value would be
        // asserting float noise rather than the solver.
        const float span = seg_a + seg_b;
        const float cs[5] = {span * 0.9878f, span * 0.9756f, span * 0.8537f, span * 0.7317f,
                             span * 0.6098f};
        for (int i = 0; i < 5; i++) {
            compute_bind_pose_matrices(state);
            vec3 t = {hip[0], hip[1] - cs[i], hip[2]};
            ik_reset(ik);
            ik_foot_set_target(ik, foot_index, t, (vec3){0, 1, 0}, 1.0f);
            ik_solve(ik, state->global_transforms, 0.0f);
            printf("ik analytic c%d bend %.6f\n", i,
                   (double)ik_probe_bend_deg(state->global_transforms, foot));
            printf("ik analytic c%d dist %.6f\n", i,
                   (double)glm_vec3_distance((float*)state->global_transforms[foot->ankle_index][3],
                                             hip));
            // What was ASKED for, so the gate can check the solve went where it was
            // sent. Without it the arm computes its expected bend from the distance the
            // solve reached, which is self-consistent for a solver that put the ankle
            // anywhere at all.
            printf("ik analytic c%d want %.6f\n", i, (double)cs[i]);
        }
    } else if (!strcmp(which, "drop")) {
        // The pelvis drop's CAP, which spec 12.5 found nothing had ever exercised: every
        // fixture asks for less than it, so min(deficit, cap) returns the deficit and a
        // wrong cap passes either way. Here the target is put half a metre past anything
        // the leg can reach, so the cap is the only thing that can answer.
        //
        // What it asserts is a REFUSAL as much as a value. The technique's author warns
        // that dragging the hips down to reach produces "T-Rex" posturing and that a
        // little sliding is better than breaking the source animation, so this cap is
        // deliberately small and the foot is deliberately left short.
        if (!ik_set_pelvis(ik, "cetra_rig:Hips")) {
            fprintf(stderr, "ik-probe: the rig lacks a pelvis\n");
            return 1;
        }
        ik->params.max_pelvis_drop = ik_default_params().max_pelvis_drop;
        // Release OFF, as the three planting cases run it and for the same reason. The
        // release fades a foot whose ankle sits above its target, and a target half a
        // metre below the floor is indistinguishable to it from a foot the clip has
        // lifted -- so with it on the foot is let go before the drop can see it and this
        // case measures nothing at all, which is how it read first time.
        ik->params.plant_fraction = 0.0f;
        compute_bind_pose_matrices(state);
        const float before = state->global_transforms[ik->pelvis_index][3][1];
        const float reach = seg_a + seg_b;
        const float asked = reach + 0.5f;
        vec3 target = {hip[0], hip[1] - asked, hip[2]};
        ik_reset(ik);
        ik_foot_set_target(ik, foot_index, target, (vec3){0.0f, 1.0f, 0.0f}, 1.0f);
        ik_solve(ik, state->global_transforms, 0.0f);
        printf("ik drop pelvis moved %.6f\n",
               (double)(before - state->global_transforms[ik->pelvis_index][3][1]));
        printf("ik drop pelvis cap %.6f\n", (double)(ik->params.max_pelvis_drop * reach));
        printf("ik drop reach deficit %.6f\n", (double)(asked - reach));
        printf("ik drop reach short %.6f\n",
               (double)ik_probe_residual(state->global_transforms, foot, target));
    } else if (!strcmp(which, "swing")) {
        // A walk cycle actually TICKED, with each ankle's vertical travel measured over
        // it. Every other case in this probe poses the rig once and solves once, which
        // is precisely why eleven arms passed while the player's feet were welded to
        // the floor: a solve that overwrites a stride looks perfect in a single pose.
        Animation* walk = scene_find_animation(probe_scene, "walk");
        if (!walk) {
            fprintf(stderr, "ik-probe: the rig lacks a walk clip\n");
            return 1;
        }

        for (int with_ik = 0; with_ik < 2; with_ik++) {
            Animator* an = create_animator(skel);
            if (!an) {
                fprintf(stderr, "ik-probe: could not create an animator\n");
                return 1;
            }
            int lf = -1, rf = -1;
            IkSystem* sys = NULL;
            if (with_ik) {
                // Its own system: the one above is owned by `state` and freed with it.
                sys = create_ik_system(skel);
                if (!sys) {
                    fprintf(stderr, "ik-probe: could not create an IK system\n");
                    return 1;
                }
                lf = ik_add_foot(sys, "cetra_rig:LeftUpLeg", "cetra_rig:LeftLeg",
                                 "cetra_rig:LeftFoot", (vec3){0.0f, 0.0f, 1.0f});
                rf = ik_add_foot(sys, "cetra_rig:RightUpLeg", "cetra_rig:RightLeg",
                                 "cetra_rig:RightFoot", (vec3){0.0f, 0.0f, 1.0f});
                ik_set_pelvis(sys, "cetra_rig:Hips");
                // PLANTING only, said rather than inherited. This case measures the stride
                // a release fade leaves, and its bar is argued in those terms; with the
                // defaults it would also lock -- silently, since a rig that never moves
                // needs no world matrix to do so -- and the number would be a blend of two
                // features under a docstring describing one. The `lock` case is where
                // locking is measured.
                sys->params.lock_distance = 0.0f;
                an->state->ik = sys;
            }
            animator_play(an, walk, 0.0f, true);

            float lo = 1e9f, hi = -1e9f;
            const int ankle_bone = ik->feet[foot_index].ankle_index;
            for (int t = 0; t < 60; t++) {
                // Targets first, from the pose the last tick left, then the tick --
                // the order the app runs in, where on_pre_render precedes the animator.
                if (sys) {
                    const int ids[2] = {lf, rf};
                    for (int k = 0; k < 2; k++) {
                        if (ids[k] < 0)
                            continue;
                        vec3 at = {0.0f, 0.0f, 0.0f};
                        glm_vec3_copy(
                            an->state->global_transforms[sys->feet[ids[k]].ankle_index][3], at);
                        // Flat ground at model y = 0. The ankle's clearance above the
                        // sole is the solver's, added by ik_foot_set_ground, and so is
                        // the release.
                        ik_foot_set_ground(sys, ids[k], (vec3){at[0], 0.0f, at[2]},
                                           (vec3){0.0f, 1.0f, 0.0f}, 1.0f);
                    }
                }
                animator_update(an, 1.0f / 60.0f);
                const float y = an->state->global_transforms[ankle_bone][3][1];
                if (y < lo)
                    lo = y;
                if (y > hi)
                    hi = y;
            }
            printf("ik swing %s travel %.6f\n", with_ik ? "on" : "off", (double)(hi - lo));
            free_animator(an);
        }
    } else if (!strcmp(which, "lock")) {
        // The slide, in metres: how far a foot already in contact travels across the
        // ground while the body moves over it. Planting keeps no memory, so the point a
        // foot solves to travels with the character and this reads the whole stance;
        // locking freezes one world point per contact and it should fall to the floor.
        //
        // The body's speed is not chosen here, it is READ OFF the clip -- the distance
        // the stance foot covers backwards in model space over its own contact window,
        // divided by that window. That is by construction the speed at which the
        // animation's own feet are stationary in the world, so every metre this reports
        // is the solver's and none of it is the fixture's. Choosing a speed instead
        // would make the number say as much about the choice as about the solve.
        if (load_animations_from_file(probe_scene, skel, WALK_CLIP, false, NULL) <= 0) {
            fprintf(stderr, "ik-probe: %s did not load\n", WALK_CLIP);
            return 1;
        }
        Animation* walk = scene_find_animation(probe_scene, "strut_walk");
        if (!walk || walk->ticks_per_second <= 0.0) {
            fprintf(stderr, "ik-probe: %s carries no usable clip\n", WALK_CLIP);
            return 1;
        }
        const float seconds = (float)(walk->duration / walk->ticks_per_second);
        const int cycle = (int)(seconds * 60.0f + 0.5f);
        const int ticks = cycle * IK_LOCK_CYCLES;
        if (cycle < 2 || ticks > IK_LOCK_TICKS) {
            fprintf(stderr, "ik-probe: a %.3f s clip does not fit the lock window\n",
                    (double)seconds);
            return 1;
        }

        // Four passes over one clip, differing only in what the solver is asked to do:
        //
        //   0  weight 0 -- the clip alone, and the ONLY source of the contact window and
        //      the body speed. A reference taken from a solved pass moves whenever the
        //      solver moves: the stride read 0.587 m/s under planting and 0.895 under an
        //      inertialized planting, which would have made every A/B incomparable with
        //      the one before it.
        //   1  planting, lock_distance 0 -- the 12.4 behaviour.
        //   2  locking.
        //   3  locking, with the body walked at three times the speed the clip implies.
        //
        // 1 and 2 are the A/B and are read over one interval; 3 is the refusal.
        const float leg = seg_a + seg_b;
        vec3 ank[IK_LOCK_TICKS] = {{0.0f, 0.0f, 0.0f}};
        vec3 toe[3][IK_LOCK_TICKS] = {{{0.0f, 0.0f, 0.0f}}};
        bool label[IK_LOCK_TICKS] = {false};  // the solver's contact label
        bool held[IK_LOCK_TICKS] = {false};   // and where it actually pinned one
        bool inside[IK_LOCK_TICKS] = {false}; // this instrument's own height window
        // Every summary below is an aggregate, and an aggregate cannot say WHICH ticks
        // went wrong. CETRA_IK_TRACE=1 prints the per-tick heights and label to stderr,
        // where the probe's numeric grammar cannot reach it; that dump is what showed the
        // toe joint dipping back through its own bind clearance in mid-swing, which no
        // summary here would have named.
        const bool trace = getenv("CETRA_IK_TRACE") != NULL;

        // lift and run carry no initialiser on purpose: pass 0 either sets both or
        // returns, so a value here would only ever be the one nothing reads. stride does,
        // because pass 1 reads it in an expression the analyser cannot order.
        float lift, stride = 0.0f, slide[2] = {0.0f, 0.0f}, jump[2] = {0.0f, 0.0f};
        int run, transitions, pins = 0, fast_hold = 0, fast_pins = 0;
        int hs = 0, hold_run = 0, hold_total = 0;
        vec3 dir = {0.0f, 0.0f, 0.0f};

        for (int pass = 0; pass < 4; pass++) {
            int lf, rf;
            // No stride: this case chooses the body's speed itself and plays at rate 1
            // throughout, which is what keeps its A/B about the solver and nothing else.
            Animator* an = ik_probe_walker(skel, walk, 0.0f, pass <= 1, &lf, &rf);
            if (!an)
                return 1;
            IkSystem* sys = an->state->ik;

            const float weight = pass == 0 ? 0.0f : 1.0f;
            const float speed = pass == 3 ? stride * IK_LOCK_FAST : stride;
            const int ankle_bone = sys->feet[lf].ankle_index;
            const int toe_bone = sys->feet[lf].toe_index;
            for (int t = 0; t < ticks; t++) {
                // Where the rig stands this tick. Identity on pass 0, where nothing reads
                // it; from there on it is the body's travel, which is what makes a frozen
                // contact a WORLD point rather than one that rides along.
                mat4 to_world;
                glm_mat4_identity(to_world);
                if (pass) {
                    vec3 at;
                    glm_vec3_scale(dir, speed * (float)t / 60.0f, at);
                    glm_translate(to_world, at);
                }
                ik_set_world(sys, to_world);

                // Targets first, from the pose the last tick left, then the tick -- the
                // order the app runs in, where on_pre_render precedes the animator.
                const int ids[2] = {lf, rf};
                for (int k = 0; k < 2; k++) {
                    vec3 at;
                    glm_vec3_copy(an->state->global_transforms[sys->feet[ids[k]].ankle_index][3],
                                  at);
                    ik_foot_set_ground(sys, ids[k], (vec3){at[0], 0.0f, at[2]},
                                       (vec3){0.0f, 1.0f, 0.0f}, weight);
                }
                animator_update(an, 1.0f / 60.0f);
                if (pass == 0)
                    glm_vec3_copy(an->state->global_transforms[ankle_bone][3], ank[t]);
                if (pass < 3)
                    glm_vec3_copy(an->state->global_transforms[toe_bone][3], toe[pass][t]);
                label[t] = sys->feet[lf].in_contact;
                held[t] = sys->feet[lf].lock != IK_LOCK_OFF;
                if (trace) {
                    vec3 body, world;
                    glm_vec3_scale(dir, speed * (float)t / 60.0f, body);
                    glm_vec3_add(an->state->global_transforms[toe_bone][3], body, world);
                    fprintf(stderr,
                            "ik-trace %d %3d toe %.4f ankle %.4f label %d locked %d "
                            "world %.4f %.4f w %.3f\n",
                            pass, t, (double)an->state->global_transforms[toe_bone][3][1],
                            (double)an->state->global_transforms[ankle_bone][3][1],
                            label[t] ? 1 : 0, held[t] ? 1 : 0, (double)world[0], (double)world[2],
                            (double)sys->feet[lf].applied_weight);
                }
            }
            free_animator(an); // frees the IK system with it

            if (pass == 3) {
                fast_hold = ik_probe_count(held, ticks);
                fast_pins = ik_probe_runs(held, ticks);
            }

            if (pass == 0) {
                IkProbeStance stance;
                if (!ik_probe_stance(ank, toe[0], ticks, inside, &stance))
                    return 1;
                lift = stance.lift;
                run = stance.run;
                stride = stance.stride;
                glm_vec3_copy(stance.dir, dir);

                // The label's own statistics, from the unsolved pass so they describe the
                // CLIP rather than the solver's response to itself, and against this
                // instrument's independent window: the label is the ankle's lift and
                // speed, the window is the ankle's height against its own travel, so
                // agreement is evidence rather than a tautology. `transitions` is how many
                // separate contacts were found, which is what says whether it is one
                // steady stance per cycle or a state flapping.
                const int labelled = ik_probe_count(label, ticks);
                transitions = ik_probe_runs(label, ticks);
                int both = 0, either = 0;
                for (int t = 0; t < ticks; t++) {
                    both += (label[t] && inside[t]) ? 1 : 0;
                    either += (label[t] || inside[t]) ? 1 : 0;
                }
                printf("ik lock clip seconds %.6f\n", (double)seconds);
                printf("ik lock clip stride %.6f\n", (double)stride);
                printf("ik lock foot lift %.6f\n", (double)lift);
                printf("ik lock label fraction %.6f\n", (double)((float)labelled / (float)ticks));
                printf("ik lock label agree %.6f\n",
                       (double)(either > 0 ? (float)both / (float)either : 0.0f));
                printf("ik lock label runs %.6f\n", (double)transitions);
                printf("ik lock label cycles %.6f\n", (double)IK_LOCK_CYCLES);
                printf("ik lock contact ticks %.6f\n", (double)run);
                printf("ik lock contact fraction %.6f\n", (double)((float)run / (float)cycle));
            }

            if (pass == 2) {
                // Both A/B passes are measured over ONE interval, the longest the lock
                // actually held, so the pair differs in the solver and not in where it was
                // read. Measuring each over its own window was tried and is not a
                // comparison: the height window opens about three ticks before the label
                // does, and that sliver of genuine swing is larger than everything the
                // feature does -- it read 0.109 planted against 0.134 held and said the
                // lock had made things worse.
                //
                // The interval being the feature's own claim is not a licence to shrink
                // it: ik-contact bounds the label's share of the cycle and its transition
                // count from the other side, and a lock cannot outlast the label that
                // admits it.
                for (int t = 0; t < ticks; t++) {
                    hold_total += held[t] ? 1 : 0;
                    pins += (held[t] && (t == 0 || !held[t - 1])) ? 1 : 0;
                }
                // Past the first cycle. The rig starts from ik_reset, so the first lock of
                // a recording is taken from a cold state and settles over a few ticks
                // that no later one spends -- measured at 0.026 m against 0.010 for the
                // cycles either side of it. Three cycles are recorded exactly so a
                // settled one is available to read; taking the longest run over all of
                // them was reporting the warm-up.
                hold_run = ik_probe_longest_run(held + cycle, ticks - cycle, &hs);
                hs += cycle;
                if (hold_run < 3) {
                    fprintf(stderr, "ik-probe: the lock never held\n");
                    return 1;
                }
                for (int p = 0; p < 2; p++)
                    slide[p] = ik_probe_drift(toe[p + 1], hs, hold_run, dir, stride, &jump[p]);
            }
        }

        printf("ik lock hold ticks %.6f\n", (double)hold_run);
        printf("ik lock hold fraction %.6f\n", (double)((float)hold_run / (float)cycle));
        printf("ik lock hold pins %.6f\n", (double)pins);
        printf("ik lock hold total %.6f\n", (double)hold_total);
        // The refusal: at three times the clip's own speed every lock breaks its unlock
        // distance almost immediately, and what matters is that it lets go rather than
        // straining.
        printf("ik lock fast ticks %.6f\n", (double)fast_hold);
        printf("ik lock fast pins %.6f\n", (double)fast_pins);
        // Reported twice on purpose: metres are what a person judges, and the fraction
        // of a leg is what an arm can assert on a rig of another size (spec 12.5).
        printf("ik lock plant slide %.6f\n", (double)slide[0]);
        printf("ik lock plant slidefrac %.6f\n", (double)(slide[0] / leg));
        printf("ik lock hold slide %.6f\n", (double)slide[1]);
        printf("ik lock hold slidefrac %.6f\n", (double)(slide[1] / leg));
        // The transition's own number, which Phase 4 planned and did not ship. A weight or
        // a target that steps rather than blending shows here and in nothing else.
        printf("ik lock plant step %.6f\n", (double)jump[0]);
        printf("ik lock hold step %.6f\n", (double)jump[1]);
    } else if (!strcmp(which, "rate")) {
        // The same stance slide the `lock` case reads, at THREE travel speeds instead of
        // one. `lock` walks the body at exactly the speed the clip's own feet imply, which
        // is the one operating point where locking works for free; a game travels at
        // whatever its controller says. This case is the claim that the feature survives
        // that, and the only thing that makes it survive is playing the clip at
        // `wanted / implied` -- which is what `ik_rate_playback` below asks the engine for.
        //
        // Read the LOCK case first. Everything here about the recording, the window, the
        // anchor and why the toe carries the measurement is stated there.
        if (load_animations_from_file(probe_scene, skel, WALK_CLIP, false, NULL) <= 0) {
            fprintf(stderr, "ik-probe: %s did not load\n", WALK_CLIP);
            return 1;
        }
        Animation* walk = scene_find_animation(probe_scene, "strut_walk");
        if (!walk || walk->ticks_per_second <= 0.0) {
            fprintf(stderr, "ik-probe: %s carries no usable clip\n", WALK_CLIP);
            return 1;
        }
        const float seconds = (float)(walk->duration / walk->ticks_per_second);
        const int cycle = (int)(seconds * 60.0f + 0.5f);
        const int ticks = cycle * IK_LOCK_CYCLES;
        if (cycle < 2 || ticks > IK_LOCK_TICKS) {
            fprintf(stderr, "ik-probe: a %.3f s clip does not fit the rate window\n",
                    (double)seconds);
            return 1;
        }

        // BOTH feet, and that is not thoroughness. `lock` walks the body at the LEFT
        // foot's own implied speed and then grades the left foot, which is self-consistent
        // and says nothing about the other leg; on this clip the right foot's stance is
        // 55 samples at 0.73 m/s against the left's 47 at 0.95, so a one-footed instrument
        // reports whichever foot the travel speed was tuned to. Every number below is the
        // WORSE of the two.
        const float leg = seg_a + seg_b;
        vec3 toe[2][IK_LOCK_TICKS] = {{{0.0f, 0.0f, 0.0f}}};
        vec3 asked[2][IK_LOCK_TICKS] = {{{0.0f, 0.0f, 0.0f}}}; // unsolved ankle, then the gap
        bool held[2][IK_LOCK_TICKS] = {{false}};

        // The reference comes from the ENGINE, and the travel and the playback rate both
        // derive from it, so at 1x they cancel exactly and the only residual left in the
        // measurement is the clip's own asymmetry. Taking the travel from one instrument
        // and the rate from another would bake their disagreement into every leg.
        const int ankle_bone[2] = {get_bone_index_by_name(skel, "cetra_rig:LeftFoot"),
                                   get_bone_index_by_name(skel, "cetra_rig:RightFoot")};
        const int toe_bone[2] = {get_bone_index_by_name(skel, "cetra_rig:LeftToeBase"),
                                 get_bone_index_by_name(skel, "cetra_rig:RightToeBase")};
        float clip_stride = 0.0f;
        vec3 dir = {0.0f, 0.0f, 0.0f};
        if (!animation_stride_speed(walk, skel, ankle_bone, toe_bone, &clip_stride, dir)) {
            fprintf(stderr, "ik-probe: %s implies no stride\n", WALK_CLIP);
            return 1;
        }
        printf("ik rate clip stride %.6f\n", (double)clip_stride);

        for (int m = 0; m < IK_RATE_COUNT; m++) {
            const float want = clip_stride * IK_RATE_MULT[m];
            float play[2] = {1.0f, 1.0f};
            float hold[2][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};
            float slide[2][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};
            float step[2][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};
            float fix[2][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};

            // The A/B this case exists for: the same body speed walked twice, once with
            // the clip played at rate 1 the way a game with no stride matching plays it,
            // and once at the rate the engine works out. Measuring only the second and
            // asserting a bar on it encodes whatever else the solver is doing; measuring
            // both asks the one question the feature answers.
            for (int matched = 0; matched < 2; matched++) {
                // Two passes at the same playback rate, differing only in whether the solver
                // is asked to do anything. The second is what the rig looks like; the first is
                // what the animator ASKED for, and the gap between them is the correction.
                // Slide alone cannot see it: a lock inside its unlock distance holds the foot
                // perfectly still while the leg is hauled a third of a metre away from the
                // pose, which is a foot that does not slide on a character that does not walk.
                for (int solved = 0; solved < 2; solved++) {
                    int lf, rf;
                    Animator* an = ik_probe_walker(skel, walk, clip_stride, false, &lf, &rf);
                    if (!an)
                        return 1;
                    IkSystem* sys = an->state->ik;
                    const int ids[2] = {lf, rf};
                    play[matched] = matched ? locomotion_rate(an, want) : 1.0f;
                    an->speed = play[matched];

                    for (int t = 0; t < ticks; t++) {
                        mat4 to_world;
                        vec3 at;
                        glm_mat4_identity(to_world);
                        glm_vec3_scale(dir, want * (float)t / 60.0f, at);
                        glm_translate(to_world, at);
                        ik_set_world(sys, to_world);

                        for (int k = 0; k < 2; k++) {
                            vec3 stand;
                            glm_vec3_copy(
                                an->state->global_transforms[sys->feet[ids[k]].ankle_index][3],
                                stand);
                            ik_foot_set_ground(sys, ids[k], (vec3){stand[0], 0.0f, stand[2]},
                                               (vec3){0.0f, 1.0f, 0.0f}, solved ? 1.0f : 0.0f);
                        }
                        animator_update(an, 1.0f / 60.0f);
                        for (int k = 0; k < 2; k++) {
                            vec3 solved_ankle;
                            glm_vec3_copy(
                                an->state->global_transforms[sys->feet[ids[k]].ankle_index][3],
                                solved_ankle);
                            if (solved) {
                                glm_vec3_copy(
                                    an->state->global_transforms[sys->feet[ids[k]].toe_index][3],
                                    toe[k][t]);
                                held[k][t] = sys->feet[ids[k]].lock != IK_LOCK_OFF;
                                // solved minus asked
                                glm_vec3_sub(solved_ankle, asked[k][t], asked[k][t]);
                            } else {
                                glm_vec3_copy(solved_ankle, asked[k][t]);
                            }
                        }
                    }
                    free_animator(an);
                }

                for (int k = 0; k < 2; k++) {
                    // Past the first cycle, for the reason the lock case gives: a cold rig's
                    // first lock settles over ticks no later one spends.
                    int hs = 0;
                    const int run = ik_probe_longest_run(held[k] + cycle, ticks - cycle, &hs);
                    hs += cycle;
                    if (run >= 3)
                        slide[matched][k] =
                            ik_probe_drift(toe[k], hs, run, dir, want, &step[matched][k]);
                    for (int t = hs; t < hs + run; t++) {
                        const float d = glm_vec3_norm(asked[k][t]);
                        fix[matched][k] = d > fix[matched][k] ? d : fix[matched][k];
                    }
                    // Against the cycle as it is actually TAKING, not as the clip is written:
                    // at 1.6x playback a loop passes in 86/1.6 ticks, so dividing by 86 would
                    // report a stance of the same share of the cycle as shrinking by a third
                    // and read as the lock giving up.
                    hold[matched][k] = (float)run * play[matched] / (float)cycle;
                    // The per-tick dump is what diagnosed the right foot: an aggregate says a
                    // leg is being corrected, and only the ticks say the correction is along
                    // TRAVEL, builds over the first 25 of them and then plateaus, which is a
                    // label that opened early rather than a lock that is losing ground.
                    if (getenv("CETRA_IK_TRACE")) {
                        fprintf(stderr,
                                "ik-rate %s %s foot %d hold %d..%d (%d) slide %.4f step %.4f "
                                "fix %.4f\n",
                                IK_RATE_TAG[m], matched ? "matched" : "free", k, hs, hs + run - 1,
                                run, (double)slide[matched][k], (double)step[matched][k],
                                (double)fix[matched][k]);
                        for (int t = hs; t < hs + run; t++) {
                            vec3 body, world;
                            glm_vec3_scale(dir, want * (float)t / 60.0f, body);
                            glm_vec3_add(toe[k][t], body, world);
                            fprintf(
                                stderr,
                                "ik-rate-tick %s %d %3d world %.4f %.4f %.4f fix %.4f %.4f %.4f\n",
                                IK_RATE_TAG[m], k, t, (double)world[0], (double)world[1],
                                (double)world[2], (double)asked[k][t][0], (double)asked[k][t][1],
                                (double)asked[k][t][2]);
                        }
                    }
                }
            }
            // Per foot, left then right, and not the worse of the two. They do not fail
            // the same way: the left tracks the travel speed exactly as the model says it
            // should, and the right carries a standing correction of its own that no
            // playback rate moves. Collapsing them to a maximum reports the right foot's
            // defect at every speed and hides what this case measures. The `free` rows are
            // the same walk with the clip played at rate 1, which is the A of the A/B.
            printf("ik rate %s play %.6f\n", IK_RATE_TAG[m], (double)play[1]);
            printf("ik rate %s hold %.6f %.6f\n", IK_RATE_TAG[m], (double)hold[1][0],
                   (double)hold[1][1]);
            printf("ik rate %s slide %.6f %.6f\n", IK_RATE_TAG[m], (double)slide[1][0],
                   (double)slide[1][1]);
            printf("ik rate %s slidefrac %.6f %.6f\n", IK_RATE_TAG[m], (double)(slide[1][0] / leg),
                   (double)(slide[1][1] / leg));
            printf("ik rate %s step %.6f %.6f\n", IK_RATE_TAG[m], (double)step[1][0],
                   (double)step[1][1]);
            printf("ik rate %s fixfrac %.6f %.6f\n", IK_RATE_TAG[m], (double)(fix[1][0] / leg),
                   (double)(fix[1][1] / leg));
            printf("ik rate %s freehold %.6f %.6f\n", IK_RATE_TAG[m], (double)hold[0][0],
                   (double)hold[0][1]);
            printf("ik rate %s freeslidefrac %.6f %.6f\n", IK_RATE_TAG[m],
                   (double)(slide[0][0] / leg), (double)(slide[0][1] / leg));
            printf("ik rate %s freefixfrac %.6f %.6f\n", IK_RATE_TAG[m], (double)(fix[0][0] / leg),
                   (double)(fix[0][1] / leg));
        }
    } else if (!strcmp(which, "scale")) {
        // The same walk twice, on a rig node scaled 1 and 2. Every threshold the solver
        // owns is in MODEL space and a fraction of the leg, so a scale on the node must
        // change nothing it decides -- and measured in model units the two runs have to
        // come out the same.
        //
        // This is the blind spot spec 12.9 shipped a live bug through: it compared a WORLD
        // distance against a model threshold, so at gametest's PLAYER_SCALE 2 the
        // documented unlock distance was behaving as half of itself. No arm could see it
        // because every case in this probe runs the rig at scale 1, and the app that
        // carries the scale has no arm at all. The model-to-world stride conversion is the
        // same seam one layer up, which is why the case is here rather than left implied.
        if (load_animations_from_file(probe_scene, skel, WALK_CLIP, false, NULL) <= 0) {
            fprintf(stderr, "ik-probe: %s did not load\n", WALK_CLIP);
            return 1;
        }
        Animation* walk = scene_find_animation(probe_scene, "strut_walk");
        if (!walk || walk->ticks_per_second <= 0.0) {
            fprintf(stderr, "ik-probe: %s carries no usable clip\n", WALK_CLIP);
            return 1;
        }
        const int cycle = (int)((float)(walk->duration / walk->ticks_per_second) * 60.0f + 0.5f);
        const int ticks = cycle * IK_LOCK_CYCLES;
        if (cycle < 2 || ticks > IK_LOCK_TICKS) {
            fprintf(stderr, "ik-probe: the clip does not fit the scale window\n");
            return 1;
        }
        const int ankle_bone[2] = {get_bone_index_by_name(skel, "cetra_rig:LeftFoot"),
                                   get_bone_index_by_name(skel, "cetra_rig:RightFoot")};
        const int toe_bone[2] = {get_bone_index_by_name(skel, "cetra_rig:LeftToeBase"),
                                 get_bone_index_by_name(skel, "cetra_rig:RightToeBase")};
        float clip_stride = 0.0f;
        vec3 dir = {0.0f, 0.0f, 0.0f};
        if (!animation_stride_speed(walk, skel, ankle_bone, toe_bone, &clip_stride, dir)) {
            fprintf(stderr, "ik-probe: %s implies no stride\n", WALK_CLIP);
            return 1;
        }

        const float leg = seg_a + seg_b;
        vec3 toe[IK_LOCK_TICKS] = {{0.0f, 0.0f, 0.0f}};
        bool held[IK_LOCK_TICKS] = {false};
        for (int si = 0; si < 2; si++) {
            const float rig = si ? 2.0f : 1.0f;
            int lf, rf;
            Animator* an = ik_probe_walker(skel, walk, clip_stride, false, &lf, &rf);
            if (!an)
                return 1;
            IkSystem* sys = an->state->ik;
            const int ids[2] = {lf, rf};

            for (int t = 0; t < ticks; t++) {
                // The body travels at the clip's speed in WORLD units, which is the model
                // stride times the rig's own scale. Getting that conversion wrong is the
                // failure this case exists to catch, so it is spelled out rather than
                // folded into the translation.
                mat4 to_world;
                vec3 at;
                glm_vec3_scale(dir, clip_stride * rig * (float)t / 60.0f, at);
                glm_translate_make(to_world, at);
                glm_scale_uni(to_world, rig);
                ik_set_world(sys, to_world);

                for (int k = 0; k < 2; k++) {
                    // Model space, and unchanged by the rig's scale: a ground target is
                    // where the ankle already is, flattened.
                    vec3 stand;
                    glm_vec3_copy(an->state->global_transforms[sys->feet[ids[k]].ankle_index][3],
                                  stand);
                    ik_foot_set_ground(sys, ids[k], (vec3){stand[0], 0.0f, stand[2]},
                                       (vec3){0.0f, 1.0f, 0.0f}, 1.0f);
                }
                animator_update(an, 1.0f / 60.0f);
                glm_vec3_copy(an->state->global_transforms[sys->feet[lf].toe_index][3], toe[t]);
                held[t] = sys->feet[lf].lock != IK_LOCK_OFF;
            }
            free_animator(an);

            int hs = 0;
            const int run = ik_probe_longest_run(held + cycle, ticks - cycle, &hs);
            hs += cycle;
            float step = 0.0f;
            // In MODEL units at both scales -- the recorded toe is model-space and the
            // body travel is added back at the model stride -- so the two runs are
            // directly comparable and a scale leaking into a threshold shows as a
            // difference rather than as a number needing interpretation.
            const float slide =
                run >= 3 ? ik_probe_drift(toe, hs, run, dir, clip_stride, &step) : 0.0f;
            printf("ik scale %s hold %.6f\n", si ? "two" : "one",
                   (double)((float)run / (float)cycle));
            printf("ik scale %s slidefrac %.6f\n", si ? "two" : "one", (double)(slide / leg));
        }
    } else if (!strcmp(which, "ground") || !strcmp(which, "slope") || !strcmp(which, "step")) {
        // The three cases that need a raycast, and so a world. The rig is placed at a
        // stand point and its origin put at the ground there; everything reported is
        // then a consequence of the geometry under each foot, which is the only thing
        // these arms assert.
        vec3 stand;
        if (!strcmp(which, "slope"))
            glm_vec3_copy((vec3){IK_RAMP_STAND, ik_ramp_height_at(IK_RAMP_STAND), 0.0f}, stand);
        else if (!strcmp(which, "step"))
            glm_vec3_copy((vec3){IK_STEP_NOSING, IK_STEP_RISER * 1.5f, 0.0f}, stand);
        else
            glm_vec3_copy((vec3){0.0f, 0.0f, 0.0f}, stand);

        PhysicsWorld* physics = ik_probe_world(game, stand);
        if (!physics) {
            fprintf(stderr, "ik-probe: could not build a world\n");
            return 1;
        }

        // Model space stands where the rig stands: the probe has no node chain, so the
        // transform is the stand point and nothing else.
        mat4 to_world, to_model;
        glm_translate_make(to_world, stand);
        glm_mat4_inv(to_world, to_model);

        const int right = ik_add_foot(ik, "cetra_rig:RightUpLeg", "cetra_rig:RightLeg",
                                      "cetra_rig:RightFoot", (vec3){0.0f, 0.0f, 1.0f});
        if (right < 0) {
            fprintf(stderr, "ik-probe: the rig lacks the right leg chain\n");
            return 1;
        }
        // ik_add_foot grows the array with realloc, which may move it, so the pointer
        // taken before that call is stale from here on.
        foot = &ik->feet[foot_index];
        ik_set_pelvis(ik, "cetra_rig:Hips");
        // The engine default, read rather than restated: these cases carry the app's own
        // bound, not a fixture number that drifts the day the default is retuned.
        ik->params.max_pelvis_drop = ik_default_params().max_pelvis_drop;
        // Release OFF for these three. They ask whether the solve reaches the ground it
        // was given; whether a foot should be planted at all is a different question,
        // and --ik-probe swing is what asks it. Left on, a foot whose target sits below
        // the clip's fades to partial weight and lands short -- which would read here as
        // a solver that misses, when it is a release doing exactly what it says.
        ik->params.plant_fraction = 0.0f;
        compute_bind_pose_matrices(state);

        // The stance is MEASURED rather than restated, so the gate can multiply it by
        // the slope it already knows and get the expected difference without either
        // side carrying a number the other has to match.
        vec3 la, ra;
        glm_vec3_copy(state->global_transforms[foot->ankle_index][3], la);
        glm_vec3_copy(state->global_transforms[ik->feet[right].ankle_index][3], ra);
        printf("ik %s rig stance %.6f\n", which, (double)fabsf(la[0] - ra[0]));
        // The fixture's own numbers, so the gate can derive what it expects instead of
        // restating constants that live here. Two places holding one number is how the
        // floor's visual and its collider drifted half a metre apart.
        printf("ik %s fixture slope %.6f\n", which, (double)IK_RAMP_SLOPE);
        printf("ik %s fixture riser %.6f\n", which, (double)IK_STEP_RISER);

        vec3 lt = {0.0f, 0.0f, 0.0f};
        vec3 ln = {0.0f, 0.0f, 0.0f};
        vec3 rt = {0.0f, 0.0f, 0.0f};
        vec3 rn = {0.0f, 0.0f, 0.0f};
        const bool lhit = ik_ground_under_foot(physics, to_world, to_model, la, lt, ln);
        const bool rhit = ik_ground_under_foot(physics, to_world, to_model, ra, rt, rn);
        if (!lhit || !rhit) {
            fprintf(stderr, "ik-probe: no ground under a foot at the stand point\n");
            return 1;
        }
        vec3 lw, rw;
        glm_mat4_mulv3(to_world, lt, 1.0f, lw);
        glm_mat4_mulv3(to_world, rt, 1.0f, rw);
        printf("ik %s ray left %.6f\n", which, (double)lw[1]);
        printf("ik %s ray right %.6f\n", which, (double)rw[1]);
        printf("ik %s ray delta %.6f\n", which, (double)fabsf(lw[1] - rw[1]));

        if (!strcmp(which, "ground")) {
            // Both rays again, one filtered and one not, from the same origin. The
            // unfiltered one is what an unguarded implementation would cast, and it is
            // shown here rather than argued about.
            vec3 origin, down = {0.0f, -1.0f, 0.0f};
            glm_mat4_mulv3(to_world, la, 1.0f, origin);
            origin[1] += IK_FOOT_RAY_UP;
            RaycastHit filtered, unfiltered;
            const bool fh = physics_world_raycast_filtered(physics, origin, down, IK_FOOT_RAY_LEN,
                                                           1u << OBJ_LAYER_STATIC, &filtered);
            const bool uh =
                physics_world_raycast(physics, origin, down, IK_FOOT_RAY_LEN, &unfiltered);
            const bool f_self = fh && filtered.entity && !strcmp(filtered.entity->name, "player");
            const bool u_self =
                uh && unfiltered.entity && !strcmp(unfiltered.entity->name, "player");
            printf("ik ground self filtered %d\n", f_self ? 1 : 0);
            printf("ik ground self unfiltered %d\n", u_self ? 1 : 0);
            printf("ik ground self onfloor %d\n",
                   (fh && filtered.entity && !strcmp(filtered.entity->name, "floor")) ? 1 : 0);
        }

        ik_reset(ik);
        // Ground, so these read what the app reads: ik_foot_set_ground adds the same
        // clearance the app's planting path gets. Targeting the bare ground here instead
        // would put the solver's release a whole clearance "lifted", and every arm built
        // on these cases would assert a released foot while the app plants one.
        ik_foot_set_ground(ik, foot_index, lt, ln, 1.0f);
        ik_foot_set_ground(ik, right, rt, rn, 1.0f);
        ik_solve(ik, state->global_transforms, 0.0f);
        printf("ik %s pose bendleft %.6f\n", which,
               (double)ik_probe_bend_deg(state->global_transforms, foot));
        printf("ik %s pose bendright %.6f\n", which,
               (double)ik_probe_bend_deg(state->global_transforms, &ik->feet[right]));

        // Where the ankle should be, taken from the engine's own stored target rather
        // than recomposed here: ik_foot_set_ground already wrote ground + sole_offset
        // into it. Rebuilding that sum in this file would be a second statement of the
        // composition, and the day it stops being a plain +Y -- IkFoot.normal exists for
        // exactly that -- the arm would keep asserting the old formula and pass on a
        // wrong pose.
        //
        // That leaves this arm asserting only that the solve REACHED what it was handed.
        // The separate soleoffset line below is what pins the handed value itself.
        vec3 want_l, want_r;
        glm_vec3_copy((float*)foot->target, want_l);
        glm_vec3_copy(ik->feet[right].target, want_r);

        vec3 solved_l, solved_r;
        glm_vec3_copy(state->global_transforms[foot->ankle_index][3], solved_l);
        glm_vec3_copy(state->global_transforms[ik->feet[right].ankle_index][3], solved_r);
        printf("ik %s pose soleleft %.6f\n", which, (double)glm_vec3_distance(solved_l, want_l));
        printf("ik %s pose soleright %.6f\n", which, (double)glm_vec3_distance(solved_r, want_r));
        // The one number the arms above cannot falsify: both sides of their comparison
        // now come from sole_offset, so doubling it or zeroing it moves the target and
        // the solved ankle together. This states what it IS -- the ankle's own height in
        // the bind pose -- against a value read independently of the solver.
        printf("ik %s rig soleoffset %.6f %.6f\n", which, (double)foot->sole_offset,
               (double)ankle[1]);
    } else {
        fprintf(stderr, "ik-probe: unknown case '%s'\n", which);
        rc = 1;
    }

    if (rc == 0)
        printf("ik %s rig segments %.6f %.6f\n", which, (double)seg_a, (double)seg_b);

    free_animation_state(state); // frees the IK system with it
    return rc;
}

// --anim-probe: the animator layer as a game sees it -- ANIMATOR components on
// entities, ticked by the loop's own update_all_animators at the fixed 1/60 --
// measured on the CPU and printed as `anim <case> <label> <key> <numbers>`,
// which is what the `anim` gate group reads. No window, no pixels: the
// puppet's clips are authored in closed form, so every number here has an
// expected value the gate states.
#define PROBE_DT (1.0f / 60.0f)

// Largest element-wise difference between two states' skinning matrices, over
// one bone or (bone < 0) all of them.
static float pose_maxdiff(const AnimationState* a, const AnimationState* b, int bone) {
    float worst = 0.0f;
    size_t lo = bone < 0 ? 0 : (size_t)bone;
    size_t hi = bone < 0 ? a->active_bone_count : (size_t)bone + 1;
    for (size_t i = lo; i < hi; i++) {
        const float* ma = (const float*)a->bone_matrices[i];
        const float* mb = (const float*)b->bone_matrices[i];
        for (int k = 0; k < 16; k++) {
            float d = fabsf(ma[k] - mb[k]);
            if (d > worst)
                worst = d;
        }
    }
    return worst;
}

static Animator* probe_rig(EntityManager* em, Skeleton* skeleton, const char* name) {
    Entity* e = create_entity(em, name);
    Animator* a = create_animator(skeleton);
    if (a) {
        // Asked for here rather than by the tick below, so that a function called
        // `tick` does not also configure. Harmless on a rig playing in-place clips,
        // which answer false whatever this says.
        a->root_motion = true;
        entity_add_animator(e, a);
    }
    return a;
}

static void probe_tick(EntityManager* em, int ticks) {
    for (int i = 0; i < ticks; i++)
        update_all_animators(em, PROBE_DT);
}

// Ticks, draining the root motion every tick the way a game's fixed step does, and
// totals what the clips laid down. Draining per tick rather than once at the end is
// the point: a drain that loses a tick's travel, or hands the same one out twice,
// shows up here as a distance that is not the clip's.
static void probe_tick_rooted(EntityManager* em, Animator* a, int ticks, vec3 out_travel,
                              float* out_yaw) {
    glm_vec3_zero(out_travel);
    *out_yaw = 0.0f;
    for (int i = 0; i < ticks; i++) {
        update_all_animators(em, PROBE_DT);
        vec3 step;
        float turned = 0.0f;
        if (animator_take_root_motion(a, step, &turned)) {
            glm_vec3_add(out_travel, step, out_travel);
            *out_yaw += turned;
        }
    }
}

// The events case's recorder: names, and the tick each fired on.
static int probe_event_count = 0;
static int probe_tick_index = 0;
static void probe_on_event(Animator* animator, const char* name, void* user) {
    (void)animator;
    printf("anim events %s fired %s %d\n", (const char*)user, name, probe_tick_index);
    probe_event_count++;
}

static int run_anim_probe(Game* game, const char* which) {
    Scene* scene = create_scene_from_model_path(puppet_path, NULL, game->engine->async_loader);
    if (!scene || scene->skeleton_count == 0) {
        fprintf(stderr, "anim-probe: could not load a rig from '%s'\n", puppet_path);
        if (scene)
            free_scene(scene);
        return 1;
    }
    game_set_scene(game, scene);
    EntityManager* em = create_entity_manager(game);
    game_set_entity_manager(game, em);
    Skeleton* skel = scene->skeletons[0];
    Animation* idle = scene_find_animation(scene, "idle");
    Animation* walk = scene_find_animation(scene, "walk");
    Animation* run = scene_find_animation(scene, "run");
    Animation* wave = scene_find_animation(scene, "wave");
    if (!idle || !walk || !run || !wave) {
        fprintf(stderr, "anim-probe: the rig lacks one of idle/walk/run/wave\n");
        return 1;
    }
    AnimatorEntry loco[3] = {{idle, 0.0f}, {walk, 0.5f}, {run, 1.0f}};
    int rc = 0;

    if (!strcmp(which, "locomotion")) {
        // The three weights at seven knob positions, two of them past the ends.
        Animator* a = probe_rig(em, skel, "a");
        animator_play_space(a, "locomotion", loco, 3, 0.0f, true);
        const float knobs[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, -0.5f};
        const char* labels[] = {"p000", "p025", "p050", "p075", "p100", "p150", "pm050"};
        for (int i = 0; i < 7; i++) {
            a->param = knobs[i];
            probe_tick(em, 1);
            printf("anim locomotion %s weights %.6f %.6f %.6f\n", labels[i], a->base.weights[0],
                   a->base.weights[1], a->base.weights[2]);
        }
    } else if (!strcmp(which, "crossfade")) {
        // A fades idle -> walk over 0.5 s; B plays walk from the switch tick
        // with no fade. After the fade the two must be the same bits.
        Animator* a = probe_rig(em, skel, "a");
        animator_play(a, idle, 0.0f, true);
        probe_tick(em, 10);
        animator_play(a, walk, 0.5f, true);
        Animator* b = probe_rig(em, skel, "b");
        animator_play(b, walk, 0.0f, true);
        printf("anim crossfade t0 fade %.6f\n", a->fade_weight);
        probe_tick(em, 15);
        printf("anim crossfade thalf fade %.6f\n", a->fade_weight);
        probe_tick(em, 15);
        printf("anim crossfade tF fade %.6f\n", a->fade_weight);
        probe_tick(em, 1);
        printf("anim crossfade tpost fade %.6f\n", a->fade_weight);
        printf("anim crossfade tpost settled %d\n", a->fading ? 0 : 1);
        printf("anim crossfade pose maxdiff %.6f\n", pose_maxdiff(a->state, b->state, -1));
    } else if (!strcmp(which, "layer")) {
        // A waves over idle on the right arm's subtree; B is idle alone.
        Animator* a = probe_rig(em, skel, "a");
        Animator* b = probe_rig(em, skel, "b");
        animator_play(a, idle, 0.0f, true);
        animator_play(b, idle, 0.0f, true);
        float mask[MAX_BONES];
        animator_mask_subtree(skel, "cetra_rig:RightArm", mask);
        animator_play_layer(a, wave, mask, 0.1f, 0.1f, false);
        probe_tick(em, 15);
        printf("anim layer w15 weight %.6f\n", a->layer.weight);
        printf("anim layer w15 finished %d\n", animator_layer_finished(a) ? 1 : 0);
        const char* outside[] = {"cetra_rig:Hips", "cetra_rig:Spine2", "cetra_rig:LeftArm"};
        float out_max = 0.0f;
        for (int i = 0; i < 3; i++) {
            float d = pose_maxdiff(a->state, b->state, get_bone_index_by_name(skel, outside[i]));
            out_max = d > out_max ? d : out_max;
        }
        printf("anim layer masked_out maxdiff %.6f\n", out_max);
        printf(
            "anim layer masked_in maxdiff %.6f\n",
            pose_maxdiff(a->state, b->state, get_bone_index_by_name(skel, "cetra_rig:RightArm")));
        probe_tick(em, 78);
        printf("anim layer after weight %.6f\n", a->layer.weight);
        printf("anim layer after finished %d\n", animator_layer_finished(a) ? 1 : 0);
        printf("anim layer after maxdiff %.6f\n", pose_maxdiff(a->state, b->state, -1));
    } else if (!strcmp(which, "two-rigs")) {
        // Three components ticked together: A idle, B run, C run. B and C
        // must agree to the bit, and A must differ from them.
        Animator* a = probe_rig(em, skel, "a");
        Animator* b = probe_rig(em, skel, "b");
        Animator* c = probe_rig(em, skel, "c");
        animator_play(a, idle, 0.0f, true);
        animator_play(b, run, 0.0f, true);
        animator_play(c, run, 0.0f, true);
        probe_tick(em, 30);
        printf("anim two-rigs ab maxdiff %.6f\n", pose_maxdiff(a->state, b->state, -1));
        printf("anim two-rigs bc maxdiff %.6f\n", pose_maxdiff(b->state, c->state, -1));
    } else if (!strcmp(which, "phase")) {
        // Walk and run at the knob's midpoint share one phase, advancing at
        // the weighted mean of their cycle frequencies: 0.5 * 1 Hz + 0.5 *
        // 2 Hz, so 20 ticks is half a cycle of each.
        Animator* a = probe_rig(em, skel, "a");
        AnimatorEntry pair[2] = {{walk, 0.0f}, {run, 1.0f}};
        animator_play_space(a, "pair", pair, 2, 0.0f, true);
        a->param = 0.5f;
        probe_tick(em, 20);
        const Animation* ref = a->base.entries[a->base.ref].clip;
        float phase = a->base.time / ref->duration;
        printf("anim phase walk frac %.6f\n", phase);
        printf("anim phase run frac %.6f\n", phase);
        float hz = 0.5f * (walk->ticks_per_second / walk->duration) +
                   0.5f * (run->ticks_per_second / run->duration);
        printf("anim phase expected frac %.6f\n", 20.0f * PROBE_DT * hz);
    } else if (!strcmp(which, "events")) {
        // Footsteps over 110 ticks (1.83 s): a walk plants four, a run
        // eight, the midpoint mix six from the walk alone (1.5 cycles per
        // second and only the heavier entry fires), and idle none.
        add_footsteps(walk);
        if (run != walk)
            add_footsteps(run);
        Animator* a = probe_rig(em, skel, "a");
        const float knobs[] = {0.5f, 0.75f, 1.0f, 0.0f};
        const char* labels[] = {"walk", "mixed", "run", "idle"};
        for (int i = 0; i < 4; i++) {
            AnimatorEntry pair[2] = {{walk, 0.5f}, {run, 1.0f}};
            if (i == 3)
                animator_play(a, idle, 0.0f, true);
            else
                animator_play_space(a, "pair", pair, 2, 0.0f, true);
            a->param = knobs[i];
            animator_set_event_callback(a, probe_on_event, (void*)labels[i]);
            probe_event_count = 0;
            for (probe_tick_index = 0; probe_tick_index < 110; probe_tick_index++)
                update_all_animators(em, PROBE_DT);
            printf("anim events %s count %d\n", labels[i], probe_event_count);
        }
    } else if (!strcmp(which, "import")) {
        // The committed walk clip binds onto the puppet by exact name.
        int n = load_animations_from_file(scene, skel, "assets/models/strut_walk.fbx", false, NULL);
        Animation* clip = n > 0 ? scene_find_animation(scene, "strut_walk") : NULL;
        if (!clip) {
            fprintf(stderr, "anim-probe: assets/models/strut_walk.fbx did not load\n");
            return 1;
        }
        int matched = 0;
        for (size_t i = 0; i < clip->channel_count; i++)
            matched += clip->channels[i].bone_index >= 0 ? 1 : 0;
        // `anim <case> <label> <key> <numbers>`, the one shape every probe line
        // takes -- the audio probe's too, so one regex reads them all.
        printf("anim import clip matched %d\n", matched);
        printf("anim import clip channels %zu\n", clip->channel_count);
        printf("anim import clip seconds %.3f\n", clip->duration / clip->ticks_per_second);
    } else if (!strcmp(which, "stride")) {
        // The ground speed two clips imply, measured the same way: one that walks and one
        // that only looks like it. The generated `walk` swings straight legs about the
        // hips, so each foot is lowest at mid-stride where it is fastest and both are
        // lowest at the same instant -- it has no stance, and the answer here is that
        // there is no answer. A number in that case would be worse than none: the fit
        // lands on a swing and points the body backwards.
        const int ankle[2] = {get_bone_index_by_name(skel, "cetra_rig:LeftFoot"),
                              get_bone_index_by_name(skel, "cetra_rig:RightFoot")};
        const int toe[2] = {get_bone_index_by_name(skel, "cetra_rig:LeftToeBase"),
                            get_bone_index_by_name(skel, "cetra_rig:RightToeBase")};
        if (load_animations_from_file(scene, skel, "assets/models/strut_walk.fbx", false, NULL) <=
            0) {
            fprintf(stderr, "anim-probe: assets/models/strut_walk.fbx did not load\n");
            return 1;
        }
        const Animation* walked = scene_find_animation(scene, "strut_walk");
        const Animation* swung = scene_find_animation(scene, "walk");
        if (!walked || !swung) {
            fprintf(stderr, "anim-probe: the rig lacks a clip to measure\n");
            return 1;
        }
        float speed = 0.0f;
        vec3 dir = {0.0f, 0.0f, 0.0f};
        const bool got = animation_stride_speed(walked, skel, ankle, toe, &speed, dir);
        printf("anim stride walk answered %d\n", got ? 1 : 0);
        printf("anim stride walk speed %.6f\n", (double)speed);
        // The direction is reported as a unit vector, so an arm can assert it points
        // somewhere rather than nowhere without depending on which way this clip faces.
        printf("anim stride walk dir %.6f %.6f %.6f\n", (double)dir[0], (double)dir[1],
               (double)dir[2]);
        float swing_speed = 0.0f;
        printf("anim stride swing answered %d\n",
               animation_stride_speed(swung, skel, ankle, toe, &swing_speed, NULL) ? 1 : 0);
    } else if (!strcmp(which, "rate")) {
        // What a blend space implies about the ground, which is the one number a game
        // cannot compute for itself. At ONE entry it is the entry's own stride and there
        // is nothing to get wrong; at two it is not the weighted mean of the strides, and
        // the gate recomputes both from the inputs printed here rather than taking the
        // engine's word for which it is.
        //
        // The two entries are chosen for their LENGTHS and not their motion -- 2.0 s
        // against 0.5 s, the widest ratio this rig offers -- because the mean and the true
        // answer part company exactly when the clips differ in duration, which is the
        // ordinary walk-and-run case. The strides are stated rather than measured: what is
        // under test is the blend, and measuring would add a second thing that could be
        // wrong. The committed clips cannot exhibit this any other way, since rescaling one
        // clip's time changes its stride inversely and under that the two expressions
        // coincide exactly.
        const Animation* one = scene_find_animation(scene, "idle");
        const Animation* two = scene_find_animation(scene, "run");
        if (!one || !two) {
            fprintf(stderr, "anim-probe: the rig lacks a pair to blend\n");
            return 1;
        }
        const float strides[2] = {1.0f, 2.0f};
        AnimatorEntry pair[2] = {{one, 0.0f, strides[0]}, {two, 1.0f, strides[1]}};
        Animator* an = create_animator(skel);
        if (!an) {
            fprintf(stderr, "anim-probe: could not create an animator\n");
            return 1;
        }
        animator_play_space(an, "pair", pair, 1, 0.0f, true);
        printf("anim rate single engine %.6f\n", (double)animator_stride_speed(an));
        printf("anim rate single stride %.6f\n", (double)strides[0]);

        animator_play_space(an, "pair", pair, 2, 0.0f, true);
        an->param = 0.5f;
        printf("anim rate pair seconds %.6f %.6f\n",
               (double)(one->duration / one->ticks_per_second),
               (double)(two->duration / two->ticks_per_second));
        printf("anim rate pair strides %.6f %.6f\n", (double)strides[0], (double)strides[1]);
        printf("anim rate pair param %.6f\n", (double)an->param);
        // Read BEFORE any tick, which is the order a game runs in: the knob is written in
        // the update and the animator ticks in pre-render. A query reading the weights the
        // last advance left would answer for the previous frame's knob.
        printf("anim rate pair engine %.6f\n", (double)animator_stride_speed(an));

        // A strideless entry lays down no ground and still occupies the clock, which is
        // what lets an idle at one end of a speed axis pull the answer DOWN instead of
        // erasing it. Same pair, same knob, the second entry's stride dropped.
        AnimatorEntry still[2] = {{one, 0.0f, strides[0]}, {two, 1.0f, 0.0f}};
        animator_play_space(an, "still", still, 2, 0.0f, true);
        an->param = 0.5f;
        printf("anim rate still engine %.6f\n", (double)animator_stride_speed(an));

        // And a space where NOTHING was measured answers 0 rather than a plausible zero
        // that means "these clips stand still".
        AnimatorEntry blind[2] = {{one, 0.0f, 0.0f}, {two, 1.0f, 0.0f}};
        animator_play_space(an, "blind", blind, 2, 0.0f, true);
        an->param = 0.5f;
        printf("anim rate blind engine %.6f\n", (double)animator_stride_speed(an));
        free_animator(an);
    } else if (!strcmp(which, "root")) {
        // What each clip STATES about its own displacement. The travelling clips carry a
        // distance the generator authored; everything else in the corpus is in place and
        // has to say so, which is the half of this that cannot be got by reasoning.
        //
        // EVERY shared clip, not just the walk. Four documents say the committed corpus
        // is in place, and one measurement is not a survey -- a single travelling clip
        // among the eight would put a character somewhere its animation never went, and
        // the sentence claiming otherwise would still be in four files.
        //
        // They are loaded with retargeting OFF, so their own translation keys reach the
        // pose. That is the only way to ask the question of them at all: a retargeted
        // channel takes its position from the bind pose, so a clip loaded the way the
        // game loads one answers "in place" whatever it holds.
        const int root = skeleton_root_bone(skel);
        if (root < 0) {
            fprintf(stderr, "anim-probe: the rig has no root bone\n");
            return 1;
        }
        for (size_t i = 0; i < sizeof SHARED_CLIPS / sizeof *SHARED_CLIPS; i++) {
            if (load_animations_from_file(scene, skel, SHARED_CLIPS[i], false, NULL) <= 0) {
                fprintf(stderr, "anim-probe: %s did not load\n", SHARED_CLIPS[i]);
                return 1;
            }
        }
        static const char* const names[] = {"idle",       "walk",       "travel_walk", "travel_run",
                                            "lunge",      "spin",       "strut_walk",  "steady_run",
                                            "quiet_idle", "swim_cycle", "float_idle",  "fall_cycle",
                                            "touch_down", "jump_start"};
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            const Animation* clip = scene_find_animation(scene, names[i]);
            if (!clip) {
                fprintf(stderr, "anim-probe: no clip named '%s'\n", names[i]);
                rc = 1;
                continue;
            }
            vec3 travel = {0.0f, 0.0f, 0.0f};
            float yaw = 0.0f;
            const bool states = animation_root_travel(clip, root, travel, &yaw);
            printf("anim root %s travel %d %.6f %.6f %.6f %.6f\n", names[i], states ? 1 : 0,
                   (double)travel[0], (double)travel[1], (double)travel[2], (double)yaw);
        }
    } else if (!strcmp(which, "rootmotion")) {
        // What the ANIMATOR hands a character, against what the clips state. Four
        // things can be wrong between the two and each has a label here: the loop
        // seam, the blend, a source switch, and whether the pose still carries the
        // travel it gave away.
        const int root = skeleton_root_bone(skel);
        const Animation* strider = scene_find_animation(scene, "travel_walk");
        const Animation* sprinter = scene_find_animation(scene, "travel_run");
        const Animation* turner = scene_find_animation(scene, "spin");
        if (root < 0 || !strider || !sprinter || !turner) {
            fprintf(stderr, "anim-probe: the rig lacks a travelling clip\n");
            return 1;
        }
        const float walk_s = strider->duration / strider->ticks_per_second;
        const float run_s = sprinter->duration / sprinter->ticks_per_second;
        vec3 got = {0.0f, 0.0f, 0.0f};
        float turned = 0.0f;

        // Three whole loops, so the seam is crossed three times. A wrap correction
        // that is missing reads as a distance one loop short per crossing; one
        // applied twice reads as one long.
        Animator* a = probe_rig(em, skel, "wrap");
        animator_play(a, strider, 0.0f, true);
        probe_tick_rooted(em, a, (int)(walk_s * 3.0f / PROBE_DT + 0.5f), got, &turned);
        printf("anim rootmotion wrap travelled %.6f %.6f %.6f\n", (double)got[0], (double)got[1],
               (double)got[2]);

        // The blend. Parked half way between two clips of DIFFERENT length, which is
        // where a mixture's travel stops being the mean of the two: one clock turns
        // both loops, so what the pair lays down per second is the weighted travel
        // per loop times the weighted loop rate. The gate recomputes that from the
        // numbers printed here rather than taking the engine's word for it.
        AnimatorEntry pair[2] = {{strider, 0.0f, 0.0f}, {sprinter, 1.0f, 0.0f}};
        Animator* b = probe_rig(em, skel, "blend");
        animator_play_space(b, "pair", pair, 2, 0.0f, true);
        b->param = 0.5f;
        const int blend_ticks = 120;
        probe_tick_rooted(em, b, blend_ticks, got, &turned);
        printf("anim rootmotion blend travelled %.6f %.6f %.6f\n", (double)got[0], (double)got[1],
               (double)got[2]);
        printf("anim rootmotion blend seconds %.6f %.6f %.6f\n", (double)walk_s, (double)run_s,
               (double)(blend_ticks * PROBE_DT));
        vec3 walk_travel, run_travel;
        animation_root_travel(strider, root, walk_travel, NULL);
        animation_root_travel(sprinter, root, run_travel, NULL);
        printf("anim rootmotion blend loops %.6f %.6f\n", (double)walk_travel[2],
               (double)run_travel[2]);

        // A switch is not travel. The clock jumps from wherever the walk had got to
        // back to the start of a new clip, and a reading that differenced the two
        // would hand the character most of a loop in one tick.
        Animator* c = probe_rig(em, skel, "switch");
        animator_play(c, strider, 0.0f, true);
        probe_tick_rooted(em, c, 40, got, &turned);
        animator_play(c, strider, 0.0f, true); // a CUT, mid-loop, to the same clip
        probe_tick_rooted(em, c, 1, got, &turned);
        printf("anim rootmotion switch travelled %.6f %.6f %.6f\n", (double)got[0], (double)got[1],
               (double)got[2]);

        // A crossfade between two clips that travel at DIFFERENT speeds. Over the
        // fade the pose's own root is a lerp of two curves that are metres apart, so
        // a reading taken from the blended pose hands the character that gap as
        // travel; per-entry readings cannot see it. The bar is the integral of the
        // two speeds under the fade envelope, which is a number the gate recomputes.
        // ONE spelling of each, because the gate recomputes the envelope from what is
        // printed: a second copy of a shape constant is the one thing a probe whose
        // whole contract is "recompute from this" must not carry.
        const float fade_seconds = 0.3f;
        const int fade_ticks = 60;
        Animator* fader = probe_rig(em, skel, "fade");
        animator_play(fader, strider, 0.0f, true);
        probe_tick_rooted(em, fader, fade_ticks, got, &turned); // settle; this window is dropped
        animator_play(fader, sprinter, fade_seconds, true);
        probe_tick_rooted(em, fader, fade_ticks, got, &turned);
        printf("anim rootmotion fade travelled %.6f %.6f %.6f\n", (double)got[0], (double)got[1],
               (double)got[2]);
        printf("anim rootmotion fade shape %.6f %.6f %.6f\n", (double)fade_seconds,
               (double)(fade_ticks * PROBE_DT), (double)PROBE_DT);

        // Half a turn, and the pose still standing where it was. The yaw goes to the
        // character; what the rig draws must not turn with it.
        Animator* d = probe_rig(em, skel, "spin");
        animator_play(d, turner, 0.0f, false);
        probe_tick_rooted(em, d, (int)(1.0f / PROBE_DT + 0.5f), got, &turned);
        printf("anim rootmotion spin turned %.6f\n", (double)turned);
        printf("anim rootmotion spin travelled %.6f %.6f %.6f\n", (double)got[0], (double)got[1],
               (double)got[2]);

        // And the pose the frame draws: the root pinned at its bind position and
        // heading however far the character has been sent.
        //
        // Read off the WRAP rig, which has been ticked by every window since -- each
        // probe_tick_rooted advances the whole entity manager, not the animator it
        // was handed. That is what makes this bar strong rather than incidental: it
        // lands the walk mid-loop, where an unpinned root stands 0.82 m downrange.
        // Read at a loop boundary instead and pinned and unpinned would differ by
        // float residue, and the bar would pass over a pin that never ran.
        vec3 posed;
        float posed_yaw = 0.0f;
        animation_pose_root(&a->base_pose, root, posed, &posed_yaw);
        const float* bind = skel->bones[root].local_transform[3];
        printf("anim rootmotion pinned offset %.6f %.6f %.6f\n", (double)(posed[0] - bind[0]),
               (double)(posed[1] - bind[1]), (double)(posed[2] - bind[2]));
        printf("anim rootmotion pinned yaw %.6f\n", (double)posed_yaw);
        // The same rig, spun: a pose that kept the clip's yaw would read half a turn.
        animation_pose_root(&d->base_pose, root, posed, &posed_yaw);
        printf("anim rootmotion spun yaw %.6f\n", (double)posed_yaw);
    } else {
        fprintf(stderr, "anim-probe: unknown case '%s'\n", which);
        rc = 1;
    }
    return rc;
}

// A pad held at full left deflection with A down, through the same reader seam
// the `gamepad` group scripts. No file and no device: it exists so the capture
// arm can assert what suppression DOES to a real source, rather than asking the
// flag to repeat what it was just told.
static bool ui_probe_pad(void* ctx, int pad, GLFWgamepadstate* out) {
    (void)ctx;
    if (pad != 0 || !out)
        return false;
    memset(out, 0, sizeof(*out));
    out->axes[GLFW_GAMEPAD_AXIS_LEFT_X] = 1.0f;
    out->buttons[GLFW_GAMEPAD_BUTTON_A] = GLFW_PRESS;
    return true;
}

// The row in a given state, as an index, or -1. There is no focus accessor and
// none is needed: UIElement.state is public, settled by the input pass, and is
// the same value the drawing reads -- so this asks what the picture asks.
static float ui_probe_state_index(UIElement** rows, int n, UIState want) {
    for (int i = 0; i < n; i++)
        if (rows[i]->state == want)
            return (float)i;
    return -1.0f;
}

static void ui_probe_rect(const char* which, const char* label, UIRect r) {
    printf("ui %s %s rect %.6f %.6f %.6f %.6f\n", which, label, (double)r.x, (double)r.y,
           (double)r.w, (double)r.h);
}

// Three equal buttons in a fixed-width panel: the smallest tree that exercises
// FIXED, GROW, padding and spacing at once, and the one every arm below shares
// so a number that moves means the layout moved and not the fixture.
static UIElement* ui_probe_column(UIScreen* screen, UIElement** rows) {
    UIElement* panel = ui_panel(ui_screen_root(screen));
    panel->size_mode[0] = UI_FIXED;
    panel->size[0] = 300.0f;
    panel->padding[0] = panel->padding[1] = panel->padding[2] = panel->padding[3] = 10.0f;
    panel->spacing = 8.0f;
    rows[0] = ui_button(panel, "One", NULL, NULL);
    rows[1] = ui_button(panel, "Two", NULL, NULL);
    rows[2] = ui_button(panel, "Three", NULL, NULL);
    for (int i = 0; i < 3; i++)
        rows[i]->size_mode[0] = UI_GROW;
    return panel;
}

/*
 * The element tree as a pure function: layout, wrapping, geometric navigation,
 * hit-testing, capture, the stack and theme resolution, printed as numbers.
 *
 * It needs a headless engine for ONE thing -- the font, because FIT sizing is
 * made of measurement and measuring a string is what bakes its glyphs -- and it
 * never draws a frame. ui_layout is pure, ui_update takes a plain struct of
 * values rather than a device, so navigation and activation are driven here
 * with no window, no pad and no GPU.
 */
static int run_ui_screens_probe(Game* game, const char* which) {
    Engine* engine = game->engine;
    Font* font = load_font(engine->text_renderer->font_pool, "apps/splash/assets/Roboto-Bold.ttf",
                           64.0f, true);
    if (!font) {
        fprintf(stderr, "ui-probe: could not load the font\n");
        return 1;
    }
    UISystem* ui = create_ui_system(engine);
    if (!ui) {
        fprintf(stderr, "ui-probe: could not create the ui system\n");
        return 1;
    }
    ui_set_font(ui, font, 22.0f);

    const float font_size = 22.0f;
    int rc = 0;

    if (!strcmp(which, "layout")) {
        UIScreen* s = ui_screen(ui, "probe");
        UIElement* rows[3];
        UIElement* panel = ui_probe_column(s, rows);
        ui_layout(s, 1280.0f, 720.0f);
        ui_probe_rect("layout", "panel", panel->rect);
        ui_probe_rect("layout", "row0", rows[0]->rect);
        ui_probe_rect("layout", "row1", rows[1]->rect);
        ui_probe_rect("layout", "row2", rows[2]->rect);
    } else if (!strcmp(which, "layout-resize")) {
        // The SAME tree at three sizes. Layout is a pure function of (tree,
        // width, height), so nothing may carry over between these three calls.
        UIScreen* s = ui_screen(ui, "probe");
        UIElement* rows[3];
        UIElement* panel = ui_probe_column(s, rows);
        ui_layout(s, 640.0f, 360.0f);
        ui_probe_rect("layout-resize", "small", panel->rect);
        ui_layout(s, 2560.0f, 1440.0f);
        ui_probe_rect("layout-resize", "large", panel->rect);
        // Back to the first size: the rects must be what they were, or layout
        // is accumulating something instead of deriving it.
        ui_layout(s, 640.0f, 360.0f);
        ui_probe_rect("layout-resize", "again", panel->rect);
    } else if (!strcmp(which, "wrap")) {
        const char* para = "the quick brown fox jumps over the lazy dog and keeps on running";
        const float limit = 400.0f;
        int lines = 0;
        const char* p = para;
        while (*p && lines < 16) {
            const size_t n = ui_text_wrap_point(font, font_size, 0.0f, p, limit);
            if (n == 0)
                break;
            char buf[256];
            const size_t copy = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
            memcpy(buf, p, copy);
            buf[copy] = '\0';
            char label[16];
            snprintf(label, sizeof(label), "line%d", lines);
            printf("ui wrap %s width %.6f\n", label,
                   (double)ui_text_width(font, font_size, 0.0f, buf));
            lines++;
            p += n;
            while (*p == ' ')
                p++;
        }
        printf("ui wrap lines count %.6f\n", (double)lines);
        printf("ui wrap limit points %.6f\n", (double)limit);
    } else if (!strcmp(which, "nav")) {
        UIScreen* s = ui_screen(ui, "probe");
        UIElement* rows[3];
        ui_probe_column(s, rows);
        ui_push(ui, s);
        const UIInput idle = {0};
        ui_update(ui, &idle, 1280.0f, 720.0f);
        printf("ui nav start focus %.6f\n", (double)ui_probe_state_index(rows, 3, UI_STATE_FOCUS));
        UIInput down = {0};
        down.nav_down = true;
        ui_update(ui, &down, 1280.0f, 720.0f);
        printf("ui nav down focus %.6f\n", (double)ui_probe_state_index(rows, 3, UI_STATE_FOCUS));
        ui_update(ui, &down, 1280.0f, 720.0f);
        printf("ui nav twice focus %.6f\n", (double)ui_probe_state_index(rows, 3, UI_STATE_FOCUS));
        ui_update(ui, &down, 1280.0f, 720.0f);
        printf("ui nav last focus %.6f\n", (double)ui_probe_state_index(rows, 3, UI_STATE_FOCUS));
        // Off the end: focus wraps to the top rather than sticking there. This
        // is the FOURTH press and not the third, because nothing is focused to
        // begin with and the first press only acquires -- a detail worth the
        // line, since counting presses instead of moves reads as a broken wrap.
        ui_update(ui, &down, 1280.0f, 720.0f);
        printf("ui nav wrapped focus %.6f\n",
               (double)ui_probe_state_index(rows, 3, UI_STATE_FOCUS));
        UIInput up = {0};
        up.nav_up = true;
        ui_update(ui, &up, 1280.0f, 720.0f);
        printf("ui nav up focus %.6f\n", (double)ui_probe_state_index(rows, 3, UI_STATE_FOCUS));
    } else if (!strcmp(which, "hit")) {
        UIScreen* s = ui_screen(ui, "probe");
        UIElement* rows[3];
        UIElement* panel = ui_probe_column(s, rows);
        ui_push(ui, s);
        const UIInput idle = {0};
        ui_update(ui, &idle, 1280.0f, 720.0f);

        // Point-in-rect against the rects layout settled: a control claims its
        // own point and none of its neighbours'.
        const float cx = rows[1]->rect.x + rows[1]->rect.w * 0.5f;
        const float cy = rows[1]->rect.y + rows[1]->rect.h * 0.5f;
        printf("ui hit centre row1 %.6f\n", ui_rect_hit(rows[1]->rect, cx, cy) ? 1.0 : 0.0);
        printf("ui hit centre row0 %.6f\n", ui_rect_hit(rows[0]->rect, cx, cy) ? 1.0 : 0.0);
        printf("ui hit centre row2 %.6f\n", ui_rect_hit(rows[2]->rect, cx, cy) ? 1.0 : 0.0);

        // A point in the panel's own padding belongs to the PARENT: inside the
        // panel, inside no row.
        const float px = panel->rect.x + 2.0f;
        const float py = panel->rect.y + 2.0f;
        printf("ui hit padding panel %.6f\n", ui_rect_hit(panel->rect, px, py) ? 1.0 : 0.0);
        int claimed = 0;
        for (int i = 0; i < 3; i++)
            claimed += ui_rect_hit(rows[i]->rect, px, py) ? 1 : 0;
        printf("ui hit padding rows %.6f\n", (double)claimed);

        // So does the spacing gap between two rows.
        const float gy = rows[0]->rect.y + rows[0]->rect.h + 2.0f;
        claimed = 0;
        for (int i = 0; i < 3; i++)
            claimed += ui_rect_hit(rows[i]->rect, cx, gy) ? 1 : 0;
        printf("ui hit gap rows %.6f\n", (double)claimed);

        // And the real pointer walk reaches the element under the cursor. The
        // test is that its state LEAVES normal rather than that it is hover
        // specifically: pointing at a control also focuses it, so the two
        // states are not alternatives and asking for one finds neither.
        UIInput at = {0};
        at.pointer_x = cx;
        at.pointer_y = cy;
        ui_update(ui, &at, 1280.0f, 720.0f);
        printf("ui hit pointer row1 %.6f\n", rows[1]->state != UI_STATE_NORMAL ? 1.0 : 0.0);
        printf("ui hit pointer row0 %.6f\n", rows[0]->state != UI_STATE_NORMAL ? 1.0 : 0.0);
    } else if (!strcmp(which, "capture")) {
        UIScreen* hud = ui_screen(ui, "hud");
        ui_screen_set_modal(hud, false);
        ui_panel(ui_screen_root(hud));
        UIScreen* menu = ui_screen(ui, "menu");
        UIElement* rows[3];
        ui_probe_column(menu, rows);

        ui_push(ui, hud);
        printf("ui capture hud captures %.6f\n", ui_captures_input(ui) ? 1.0 : 0.0);
        ui_push(ui, menu);
        printf("ui capture menu captures %.6f\n", ui_captures_input(ui) ? 1.0 : 0.0);
        ui_pop(ui);
        printf("ui capture popped captures %.6f\n", ui_captures_input(ui) ? 1.0 : 0.0);
        ui_pop_all(ui);
        printf("ui capture cleared captures %.6f\n", ui_captures_input(ui) ? 1.0 : 0.0);

        /*
         * And what the switch actually DOES, through a real source.
         *
         * A scripted pad holds the left stick and A, so move_x -- a game action
         * -- and ui_accept -- flagged `ui` -- both read. Raising suppression
         * must take one to zero and leave the other alone, which is the whole
         * contract and the branch's most dangerous new global.
         *
         * Asserting input_is_suppressed instead, as this did, proves only that
         * a bool remembers what it was told: with no source held both actions
         * read zero either way, and the arm passes over a layer that has gone
         * completely deaf.
         */
        input_bind(&game->input, actions, ACTION_COUNT);
        input_set_pad_reader(&game->input, ui_probe_pad, NULL, NULL);

        input_update(&game->input);
        printf("ui capture free move %.6f\n", (double)input_action_value(&game->input, "move_x"));
        printf("ui capture free accept %.6f\n",
               (double)input_action_value(&game->input, "ui_accept"));

        input_set_suppressed(&game->input, true);
        input_update(&game->input);
        printf("ui capture held move %.6f\n", (double)input_action_value(&game->input, "move_x"));
        printf("ui capture held accept %.6f\n",
               (double)input_action_value(&game->input, "ui_accept"));

        input_set_suppressed(&game->input, false);
        input_update(&game->input);
        printf("ui capture given move %.6f\n", (double)input_action_value(&game->input, "move_x"));
    } else if (!strcmp(which, "stack")) {
        UIScreen* a = ui_screen(ui, "a");
        UIScreen* b = ui_screen(ui, "b");
        ui_panel(ui_screen_root(a));
        ui_panel(ui_screen_root(b));
        printf("ui stack empty top %.6f\n", ui_top(ui) == NULL ? 1.0 : 0.0);
        ui_push(ui, a);
        printf("ui stack pushed top %.6f\n", ui_top(ui) == a ? 1.0 : 0.0);
        ui_push(ui, b);
        printf("ui stack second top %.6f\n", ui_top(ui) == b ? 1.0 : 0.0);
        ui_pop(ui);
        printf("ui stack popped top %.6f\n", ui_top(ui) == a ? 1.0 : 0.0);
        ui_push(ui, b);
        ui_pop_all(ui);
        printf("ui stack cleared top %.6f\n", ui_top(ui) == NULL ? 1.0 : 0.0);
        // Popping an empty stack is a no-op, not an underflow.
        ui_pop(ui);
        printf("ui stack underflow top %.6f\n", ui_top(ui) == NULL ? 1.0 : 0.0);
    } else if (!strcmp(which, "theme-identity")) {
        // Two identical trees, one of which sets a ZEROED style on every row.
        // If zero really means inherit, the two emit the same vertices; if it
        // means "black, no padding, no radius", they cannot.
        UIScreen* plain = ui_screen(ui, "plain");
        UIElement* plain_rows[3];
        ui_probe_column(plain, plain_rows);

        UIScreen* zeroed = ui_screen(ui, "zeroed");
        UIElement* zero_rows[3];
        ui_probe_column(zeroed, zero_rows);
        const UIStyle nothing = {0};
        for (int i = 0; i < 3; i++)
            ui_set_style(zero_rows[i], &nothing);

        UIDrawList* dl = ui_draw_list(ui);
        ui_push(ui, plain);
        ui_draw_list_begin(dl, 1280, 720);
        ui_build(ui, 1280.0f, 720.0f, 0.0f);
        const uint64_t sig_plain = ui_draw_list_signature(dl);
        ui_pop(ui);

        ui_push(ui, zeroed);
        ui_draw_list_begin(dl, 1280, 720);
        ui_build(ui, 1280.0f, 720.0f, 0.0f);
        const uint64_t sig_zeroed = ui_draw_list_signature(dl);
        ui_pop(ui);

        printf("ui theme-identity styles match %.6f\n", sig_plain == sig_zeroed ? 1.0 : 0.0);
        // A style that says something must NOT hash alike, or the comparison
        // above would pass on a signature that ignores style entirely.
        UIStyle loud = {0};
        loud.corner_radius = 14.0f;
        loud.bg[0] = loud.bg[3] = 1.0f;
        for (int i = 0; i < 3; i++)
            ui_set_style(zero_rows[i], &loud);
        ui_push(ui, zeroed);
        ui_draw_list_begin(dl, 1280, 720);
        ui_build(ui, 1280.0f, 720.0f, 0.0f);
        const uint64_t sig_loud = ui_draw_list_signature(dl);
        ui_pop(ui);
        printf("ui theme-identity styled differs %.6f\n", sig_loud != sig_plain ? 1.0 : 0.0);
    } else {
        fprintf(stderr, "ui-probe: unknown case '%s'\n", which);
        rc = 1;
    }

    free_ui_system(ui);
    return rc;
}

/*
 * --cam-probe (spec 12.19): the camera rig, checked with no window and no GL.
 *
 * It runs BEFORE the engine is created, the way --ui-probe settings does, and
 * for the same reason: camera_rig.c takes plain values and touches nothing. A
 * camera that can be asserted without a frame is the whole argument for the rig
 * being a pure function, so an arm that needed a window would be evidence the
 * design had slipped.
 */
static void cam_probe_pose(const char* which, const char* label, const CameraRig* rig) {
    printf("cam %s %s eye %.9g %.9g %.9g look %.9g %.9g %.9g yaw %.9g pitch %.9g\n", which, label,
           (double)rig->pose.eye[0], (double)rig->pose.eye[1], (double)rig->pose.eye[2],
           (double)rig->pose.look[0], (double)rig->pose.look[1], (double)rig->pose.look[2],
           (double)rig->yaw, (double)rig->pitch);
}

static int run_cam_probe(const char* which) {
    CameraRig* rig = create_camera_rig();
    if (!rig)
        return 1;

    if (!strcmp(which, "orbit")) {
        // An anchor away from the origin and a lift on each, so a term dropped
        // from the closed form cannot hide behind a zero.
        glm_vec3_copy((vec3){3.0f, 1.0f, -2.0f}, rig->anchor);
        rig->dist = 4.0f;
        rig->look_lift = 0.5f;
        rig->eye_lift = 1.5f;
        rig->yaw = 0.0f;
        rig->pitch = 0.0f;
        camera_rig_update(rig, 0.0f, 0.0f, 0.0f);
        cam_probe_pose(which, "start", rig);
        // 0.9 rad at full input over half a second, then the same 0.9 again at
        // half the input over twice the time: a rate is a rate, so the second
        // turn must equal the first exactly.
        camera_rig_update(rig, 0.5f, -1.0f, 0.0f);
        cam_probe_pose(which, "turned", rig);
        camera_rig_update(rig, 1.0f, -0.5f, 0.0f);
        cam_probe_pose(which, "again", rig);
        // Nothing asked for: the pose is DERIVED from the aim, so it must not
        // creep. An implementation that integrated the eye instead would.
        camera_rig_update(rig, 1.0f, 0.0f, 0.0f);
        cam_probe_pose(which, "held", rig);
    } else if (!strcmp(which, "clamp")) {
        rig->pitch_min = -1.25f;
        rig->pitch_max = 0.2f;
        rig->dist = 4.0f;
        camera_rig_update(rig, 10.0f, 0.0f, 1.0f);
        cam_probe_pose(which, "high", rig);
        camera_rig_update(rig, 10.0f, 0.0f, -1.0f);
        cam_probe_pose(which, "low", rig);
        // Held against the stop: a clamp holds where a wrap would come round.
        camera_rig_update(rig, 10.0f, 0.0f, -1.0f);
        cam_probe_pose(which, "lower", rig);
    } else if (!strcmp(which, "first-person")) {
        glm_vec3_copy((vec3){3.0f, 1.0f, -2.0f}, rig->anchor);
        rig->look_lift = 1.7f;
        rig->dist = 0.0f;
        rig->yaw = 0.3f;
        rig->pitch = -0.2f;
        camera_rig_update(rig, 0.0f, 0.0f, 0.0f);
        cam_probe_pose(which, "eye", rig);
    } else if (!strcmp(which, "pose")) {
        // A stated pose, then an update that changes nothing: the second must
        // reproduce the first, or a pinned camera drifts the moment it ticks.
        vec3 eye = {1.0f, 2.0f, 3.0f}, look = {-4.0f, 0.5f, 6.0f};
        rig->look_lift = 9.0f; // must be zeroed by the adopt, or the update moves
        rig->eye_lift = 9.0f;
        camera_rig_set_pose(rig, eye, look);
        cam_probe_pose(which, "set", rig);
        camera_rig_update(rig, 0.0f, 0.0f, 0.0f);
        cam_probe_pose(which, "ticked", rig);
    } else if (!strcmp(which, "basis")) {
        rig->yaw = 0.75f;
        float basis = -99.0f;
        bool steers = camera_rig_move_basis(rig, &basis);
        printf("cam basis off steers %d yaw %.9g\n", steers ? 1 : 0, (double)basis);
        rig->steers_controls = true;
        steers = camera_rig_move_basis(rig, &basis);
        printf("cam basis on steers %d yaw %.9g\n", steers ? 1 : 0, (double)basis);
    } else {
        fprintf(stderr, "cam-probe: unknown case '%s'\n", which);
        free_camera_rig(rig);
        return 1;
    }

    free_camera_rig(rig);
    return 0;
}

// --ui-probe (spec 12.2): the game-layer state a menu edits, checked with no
// window, no GL and no audio device. It runs BEFORE the engine is created
// rather than inside a headless game the way the audio and anim probes do,
// because none of it needs one -- settings.c is cJSON over a struct and an
// offline AudioSystem opens nothing. That is what lets the `ui` gate assert on
// it while the no-GPU-during-gates rule holds.
static void ui_probe_print(const char* label, const GameSettings* s) {
    printf("ui settings %s master %.6f\n", label, s->master_volume);
    printf("ui settings %s music %.6f\n", label, s->music_volume);
    printf("ui settings %s sfx %.6f\n", label, s->sfx_volume);
    printf("ui settings %s ui %.6f\n", label, s->ui_volume);
    printf("ui settings %s window_mode %.6f\n", label, (double)s->window_mode);
    printf("ui settings %s vsync %.6f\n", label, s->vsync ? 1.0 : 0.0);
}

static int run_ui_probe(const char* which) {
    if (strcmp(which, "settings") != 0) {
        fprintf(stderr, "ui-probe: unknown case '%s'\n", which);
        return 1;
    }

    // The platform path is part of what is under test, so the probe resolves it
    // rather than taking one on the command line. CETRA_SETTINGS_DIR is what
    // keeps a run out of the real user directory.
    char path[1024];
    if (!settings_default_path(path, sizeof(path))) {
        fprintf(stderr, "ui-probe: could not resolve a settings path\n");
        return 1;
    }

    GameSettings written;
    settings_defaults(&written);
    ui_probe_print("default", &written);

    written.master_volume = 0.25f;
    written.music_volume = 0.5f;
    written.sfx_volume = 0.75f;
    written.ui_volume = 0.125f;
    written.window_mode = ENGINE_WINDOW_FULLSCREEN;
    written.vsync = false;
    if (!settings_save(&written, path)) {
        fprintf(stderr, "ui-probe: save failed\n");
        return 1;
    }

    // Zeroed and not defaulted before the read: against a struct that already
    // holds the answer, a loader that merely leaves a field alone passes.
    GameSettings back;
    memset(&back, 0, sizeof(back));
    if (!settings_load(&back, path)) {
        fprintf(stderr, "ui-probe: load failed\n");
        return 1;
    }
    ui_probe_print("roundtrip", &back);

    // The enum rides the file as a NAME. Read back as a number it would still
    // round-trip while an inserted enumerator silently re-pointed every file a
    // player already has, so the name is the half worth asserting.
    bool named = false;
    FILE* file = fopen(path, "rb");
    if (file) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            if (strstr(line, "fullscreen")) {
                named = true;
                break;
            }
        }
        fclose(file);
    }
    printf("ui settings named fullscreen %.6f\n", named ? 1.0 : 0.0);

    // An absent file is the first run: defaults, not zeros. Zero is silence and
    // a windowed mode, which is a plausible-looking wrong answer.
    GameSettings absent;
    memset(&absent, 0, sizeof(absent));
    settings_load(&absent, "gametest-no-such-settings.json");
    ui_probe_print("absent", &absent);

    // What apply actually pushes. The getter is the only way to ask: miniaudio
    // has no master get, so a settings screen reads what the setter recorded.
    AudioSystem* audio = create_audio_system(true);
    if (!audio) {
        fprintf(stderr, "ui-probe: could not create the offline audio system\n");
        return 1;
    }
    settings_apply(&back, audio, NULL);
    printf("ui settings applied master %.6f\n", audio_get_bus_volume(audio, AUDIO_BUS_MASTER));
    printf("ui settings applied music %.6f\n", audio_get_bus_volume(audio, AUDIO_BUS_MUSIC));
    printf("ui settings applied sfx %.6f\n", audio_get_bus_volume(audio, AUDIO_BUS_SFX));
    printf("ui settings applied ui %.6f\n", audio_get_bus_volume(audio, AUDIO_BUS_UI));
    free_audio_system(audio);
    return 0;
}

static void display_probe_placement(const char* label, EngineWindowMode mode) {
    // A monitor rectangle that is nobody's real display and an origin that is
    // not 0,0, so a placement which ignored the monitor's position (the
    // second-display bug) cannot pass by landing on it accidentally.
    const EngineWindowPlacement at = engine_window_placement(
        mode, (EngineWindowRect){1600, 300, 2560, 1440}, (EngineWindowRect){40, 50, 640, 480});
    printf("display placement %s rect %d %d %d %d %d %d\n", label, at.fullscreen ? 1 : 0,
           at.decorated ? 1 : 0, at.rect.x, at.rect.y, at.rect.w, at.rect.h);
}

/*
 * What a display mode DECIDES, with no display involved (spec 12.15).
 *
 * The effect cannot be probed and deliberately so: engine_set_window_mode
 * refuses under headless, because a suite that seized a monitor would be
 * intolerable and a golden whose frame size came from the machine's display
 * would not be a golden. So what runs here is the pure placement function, the
 * name lookup, and the proof that the refusal holds -- the parts that can be
 * wrong in a way nobody would see.
 */
static int run_display_probe(Game* game, const char* which) {
    Engine* engine = game ? game->engine : NULL;
    const bool all = !which || !strcmp(which, "all");
    bool ran = false;
    if (all || !strcmp(which, "placement")) {
        ran = true;
        display_probe_placement("windowed", ENGINE_WINDOW_WINDOWED);
        display_probe_placement("fullscreen", ENGINE_WINDOW_FULLSCREEN);
        display_probe_placement("borderless", ENGINE_WINDOW_BORDERLESS);
    }

    if (all || !strcmp(which, "monitors")) {
        ran = true;
        int count = 0;
        const char* const* names = engine_monitor_names(engine, &count);
        printf("display monitors list count %d\n", count);
        // Resolving a display's OWN name must give its own index, or a settings
        // file would move the window every time it was read back.
        int self = 0;
        for (int i = 0; i < count; i++) {
            if (engine_monitor_index(engine, names[i]) != i)
                self = 1;
        }
        printf("display monitors resolve misresolved %d\n", self);
        printf("display monitors resolve unknown %d\n",
               engine_monitor_index(engine, "no display is called this"));
        printf("display monitors resolve empty %d\n", engine_monitor_index(engine, ""));
        printf("display monitors resolve null %d\n", engine_monitor_index(engine, NULL));
    }

    if (all || !strcmp(which, "apply")) {
        ran = true;
        // The end of the path the old code did not have: a file's values
        // reaching live engine state. vsync is the half that lands headless;
        // the mode is the half that must NOT, and both are read back here.
        GameSettings s;
        settings_defaults(&s);
        s.vsync = false;
        s.window_mode = ENGINE_WINDOW_BORDERLESS;
        settings_apply(&s, NULL, engine);
        printf("display apply engine vsync %d\n", engine->vsync ? 1 : 0);
        printf("display apply engine mode %d\n", (int)engine->window_mode);
    }

    // Unknown is the absence of a run rather than a list to keep in step with
    // the dispatch above: a probe that prints nothing looks exactly like a
    // feature with nothing to say, and an arm reading zero rows can pass on the
    // strength of a typo.
    if (!ran) {
        fprintf(stderr, "display-probe: unknown case '%s'\n", which);
        return 1;
    }
    return 0;
}

/*
 * The screens (spec 12.2, phase 7): a main menu, a pause menu, a settings
 * screen and a HUD. They replace phase 2's --ui-smoke, whose job was to put the
 * draw primitives in front of a pixel comparison before anything was built on
 * them, because every way they can be wrong (a glyph mirrored about its own
 * baseline, the scissor's point-to-pixel conversion, the corner SDF, the blend
 * func) is invisible to a compile and obvious in a picture.
 *
 * Nothing opens at startup unless --ui-screen says so. A main menu up by
 * default would pause the sim before the first frame, which is the state every
 * scripted-pad and trace run in the gate suite would then be driving against.
 */
// ESCAPE HATCH 1: a custom-drawn element, the master volume as a segmented
// meter painted straight into the draw list. It gets the same rect the layout
// settled and the same focus and hit-testing as a built-in kind -- ui_set_draw
// replaces the drawing and nothing else.
static void ui_draw_meter(UIElement* el, UIDrawList* dl, void* user) {
    (void)user;
    const int segments = 12;
    const float gap = 3.0f;
    const float w = (el->rect.w - gap * (float)(segments - 1)) / (float)segments;
    const int lit = (int)(ui_settings.master_volume * (float)segments + 0.5f);
    for (int i = 0; i < segments; i++) {
        const UIRect seg = {el->rect.x + (w + gap) * (float)i, el->rect.y, w, el->rect.h};
        vec4 on = {0.45f, 0.70f, 1.00f, 1.0f};
        vec4 off = {0.20f, 0.21f, 0.26f, 1.0f};
        ui_draw_rounded(dl, seg, 2.0f, i < lit ? on : off, NULL, 0.0f);
    }
}

// Every bound control reaches real state. A callback runs AFTER the value has
// been written, so it reads the new one -- and because the sliders bind into
// ui_settings directly, ONE handler pushing the whole struct does the same work
// as four that each have to know their own bus.
static void ui_settings_changed(UIElement* el, void* user) {
    (void)el;
    ui_settings_dirty = true;
    settings_apply(&ui_settings, ui_audio, user);
}

static void ui_tonemap_changed(UIElement* el, void* user) {
    (void)el;
    Engine* engine = user;
    if (engine && engine->postfx)
        engine->postfx->tonemap_mode = ui_tonemap;
}

// The selector binds an index; the file holds a NAME. The copy happens here
// before the shared handler applies the struct, so the one path that touches
// live subsystems stays the one path.
static void ui_monitor_changed(UIElement* el, void* user) {
    int count = 0;
    const char* const* names = engine_monitor_names(user, &count);
    if (ui_monitor_index >= 0 && ui_monitor_index < count) {
        snprintf(ui_settings.monitor, sizeof(ui_settings.monitor), "%s", names[ui_monitor_index]);
    }
    ui_settings_changed(el, user);
}

// Resume and Back are the same act -- close the screen on top -- so they are
// the same function under two labels. The frame-input pass then hands input and
// the sim back on its own, because it reads ui_captures_input every frame
// rather than on an edge.
static void ui_action_close(UIElement* el, void* user) {
    (void)el;
    (void)user;
    ui_pop(ui_system);
}

// New Game closes every menu WITHOUT taking the HUD with them, which is what
// ui_pop_all would do: the HUD is a screen like any other and lives at the
// bottom of the same stack.
static void ui_action_new_game(UIElement* el, void* user) {
    (void)el;
    (void)user;
    while (ui_top(ui_system) && ui_top(ui_system) != screen_hud)
        ui_pop(ui_system);
}

static void ui_action_open_settings(UIElement* el, void* user) {
    (void)el;
    (void)user;
    ui_push(ui_system, screen_settings);
}

// Quitting is a menu item, which is where it belongs -- not a key.
static void ui_action_quit(UIElement* el, void* user) {
    (void)el;
    Engine* engine = user;
    if (engine && engine->window)
        glfwSetWindowShouldClose(engine->window, GLFW_TRUE);
}

// Sets a label's text only when it actually changed: ui_set_text owns a copy,
// and a HUD rewriting two strings sixty times a second for the same characters
// is allocation with nothing to show for it.
static void hud_set_text(UIElement* el, const char* text) {
    if (el && (!el->text || strcmp(el->text, text) != 0))
        ui_set_text(el, text);
}

/*
 * Escape opens the menu and closes it again, which is what it does in every
 * game that has one; quitting is a menu item, not a key. The action is flagged
 * `ui`, so it keeps reading while the menu itself has taken input away from the
 * game -- otherwise the key that opened the menu could not close it.
 */
static void on_frame_input(Game* game) {
    /*
     * Once per FRAME, which is the whole reason these are taken here. on_update
     * runs once per fixed STEP, and input is polled outside that loop -- so a
     * frame that ran five steps read the same press five times, and one F5 was
     * five entire saves with five fsyncs. Only the EDGE moves here; the work
     * still happens in the step, against the settled world.
     *
     * Above the ui_system guard, because --no-ui is a game that can still save.
     */
    if (input_action_pressed(&game->input, "quicksave"))
        save_pending = true;
    if (input_action_pressed(&game->input, "quickload"))
        load_pending = true;

    if (!ui_system)
        return;

    // The HUD is the non-modal case, and it is what proves a screen coexists
    // with gameplay: it draws every frame over a live sim and takes nothing.
    char line[64];
    snprintf(line, sizeof(line), "speed  %.2f", (double)hud_ground_speed);
    hud_set_text(hud_speed_label, line);
    snprintf(line, sizeof(line), "blend  %.2f",
             player_animator ? (double)player_animator->param : 0.0);
    hud_set_text(hud_anim_label, line);

    // The app decides only when a menu OPENS; closing is the layer's, through
    // UIInput.back. Doing both here left ui_pop with no caller and the back
    // field permanently false -- and wiring the field without removing this
    // would pop twice for one press, once in each place.
    //
    // "Open" means a MENU, not any screen: the HUD is on the stack for the
    // whole run, so a bare ui_top test would read as a menu already being up
    // and Escape would never open one.
    const bool menu_pressed = input_action_pressed(&game->input, "menu");
    const bool menu_open = ui_top(ui_system) && ui_top(ui_system) != screen_hud;
    if (menu_pressed && !menu_open && screen_pause)
        ui_push(ui_system, screen_pause);

    // One synthetic "down" per frame while --ui-focus has any left, so a golden
    // can be taken with the focus somewhere other than where it lands.
    bool forced_down = false;
    if (ui_focus_steps > 0 && menu_open) {
        forced_down = true;
        ui_focus_steps--;
    }

    double mx = 0.0, my = 0.0;
    input_mouse_pos(&game->input, &mx, &my);
    const UIInput in = {
        .pointer_x = (float)mx,
        .pointer_y = (float)my,
        .pointer_down = input_mouse_down(&game->input, GLFW_MOUSE_BUTTON_LEFT),
        .pointer_pressed = input_mouse_pressed(&game->input, GLFW_MOUSE_BUTTON_LEFT),
        .pointer_released = input_mouse_released(&game->input, GLFW_MOUSE_BUTTON_LEFT),
        .nav_up = input_action_pressed(&game->input, "ui_up"),
        .nav_down = input_action_pressed(&game->input, "ui_down") || forced_down,
        .nav_left = input_action_pressed(&game->input, "ui_left"),
        .nav_right = input_action_pressed(&game->input, "ui_right"),
        .accept = input_action_pressed(&game->input, "ui_accept"),
        .back = menu_pressed && menu_open,
    };
    ui_update(ui_system, &in, (float)game->engine->win_width, (float)game->engine->win_height);

    // The file is written when the settings screen closes by ANY route -- its
    // own Back button, Escape, or a pop from somewhere else. In the button's
    // callback it would miss the other two; on each slider callback it would
    // rewrite the file once per frame of a drag.
    if (ui_settings_dirty && ui_top(ui_system) != screen_settings) {
        ui_settings_dirty = false;
        if (ui_settings_have_path)
            settings_save(&ui_settings, ui_settings_path);
    }

    // The one line that stops the character walking while a menu is up. Set
    // every frame rather than on the edges, so a screen closed by any route --
    // a button, a callback, a scene change -- gives input back.
    const bool captured = ui_captures_input(ui_system);
    input_set_suppressed(&game->input, captured);

    // The menu pauses the sim on its EDGES, and through the pause API rather
    // than by storing the field. Assigning it every frame made this the only
    // writer that mattered: the P key toggles pause from on_pre_render, later
    // in the same frame, and the next frame's assignment put it straight back
    // -- so the pause action lasted one frame and appeared dead.
    if (captured != ui_menu_paused) {
        ui_menu_paused = captured;
        if (captured)
            game_pause(game);
        else
            game_unpause(game);
    }
}

// A menu's column, styled the same way on each screen that has one.
static UIElement* ui_menu_panel(UIScreen* screen, const char* title) {
    UIElement* panel = ui_panel(ui_screen_root(screen));
    panel->align_cross = UI_ALIGN_CENTER;
    panel->size_mode[0] = UI_FIXED;
    panel->size[0] = 320.0f;

    // A per-element style: exactly the three-level chain the header describes,
    // used at its first level. Everything left at zero -- the colours, the
    // padding -- still comes from the theme and then the engine default, so
    // this says only what it means to change.
    UIElement* heading = ui_label(panel, title);
    const UIStyle heading_style = {
        .font_size = 30.0f,
        .tracking = 1.5f,
        .line_spacing = 1.15f,
        .fg = {0.96f, 0.97f, 1.00f, 1.0f},
        .padding = {6.0f, 0.0f, 14.0f, 0.0f},
    };
    ui_set_style(heading, &heading_style);
    return panel;
}

// GROW on the cross axis, so every control is the panel's width. Left at FIT
// each one hugs its own text and the column comes out ragged. The heading at
// index 0 is skipped: GROWn it is left-aligned text in a wide box, where FIT
// under the panel's centre alignment is the centred title this wants.
static void ui_rows_grow(UIElement* panel) {
    for (size_t i = 1; i < panel->child_count; i++)
        panel->children[i]->size_mode[0] = UI_GROW;
}

static bool ui_install(Engine* engine) {
    ui_font = load_font(engine->text_renderer->font_pool, "apps/splash/assets/Roboto-Bold.ttf",
                        64.0f, true);
    if (!ui_font) {
        fprintf(stderr, "ui: could not load the font\n");
        return false;
    }
    ui_system = create_ui_system(engine);
    if (!ui_system) {
        fprintf(stderr, "ui: could not create the ui system\n");
        return false;
    }
    ui_set_font(ui_system, ui_font, 22.0f);

    // The player's own file, read before a control is built so every slider
    // opens at the value it is about to edit. An absent file is the first run
    // and leaves defaults; the audio half is pushed from on_init, where the
    // audio system exists.
    if (settings_default_path(ui_settings_path, sizeof(ui_settings_path))) {
        ui_settings_have_path = true;
        settings_load(&ui_settings, ui_settings_path);
    } else {
        settings_defaults(&ui_settings);
    }

    // The live display list, and the stored NAME resolved back to an index in
    // it. A name nothing answers to leaves index 0, which is the same primary
    // the engine falls back to -- so an unplugged monitor reads as "primary"
    // on the screen and behaves as primary in the window, rather than the two
    // disagreeing.
    ui_monitor_names = engine_monitor_names(engine, &ui_monitor_count);
    const int chosen = engine_monitor_index(engine, ui_settings.monitor);
    ui_monitor_index = (chosen >= 0 && chosen < ui_monitor_count) ? chosen : 0;

    settings_apply(&ui_settings, NULL, engine);

    // The app carries its own GLSL, the apps/network precedent. Registered with
    // the engine's cache so the cache owns it; a failure is logged there and
    // leaves the backdrop as an ordinary panel rather than taking the menu down.
    ui_backdrop_program =
        create_program_from_source("ui_backdrop", UI_BACKDROP_VERT, UI_BACKDROP_FRAG, NULL);
    if (ui_backdrop_program)
        engine_add_program(engine, ui_backdrop_program);

    // The MAIN menu: the one screen with a backdrop, because it is the one
    // drawn over nothing in particular. It fades rather than slides, which is
    // what a game's first screen does.
    screen_main = ui_screen(ui_system, "main");
    ui_screen_set_modal(screen_main, true);
    ui_screen_transition(screen_main, UI_TRANSITION_FADE, 0.20f);

    // ESCAPE HATCH 2: a backdrop with its own fragment stage, filling the
    // screen behind the panel. This is the layer's headline claim -- that a
    // menu is not limited to what the element vocabulary can express.
    UIElement* backdrop = ui_panel(ui_screen_root(screen_main));
    backdrop->fill = true; // out of the flow, covering the screen
    if (ui_backdrop_program)
        ui_set_element_program(backdrop, ui_backdrop_program);

    UIElement* main_panel = ui_menu_panel(screen_main, "CETRA");
    ui_button(main_panel, "New Game", ui_action_new_game, NULL);
    ui_button(main_panel, "Settings", ui_action_open_settings, NULL);
    ui_button(main_panel, "Quit", ui_action_quit, engine);
    ui_rows_grow(main_panel);

    // The PAUSE menu, deliberately with NO backdrop: it is drawn over live
    // gameplay, and hiding the thing the player paused defeats the point. The
    // scene keeps rendering because pausing stops only the fixed step.
    screen_pause = ui_screen(ui_system, "pause");
    ui_screen_set_modal(screen_pause, true);
    ui_screen_transition(screen_pause, UI_TRANSITION_SLIDE, 0.18f);
    UIElement* pause_panel = ui_menu_panel(screen_pause, "Paused");
    ui_button(pause_panel, "Resume", ui_action_close, NULL);
    ui_button(pause_panel, "Settings", ui_action_open_settings, NULL);
    ui_button(pause_panel, "Quit", ui_action_quit, engine);
    ui_rows_grow(pause_panel);

    // SETTINGS. Every control moves something the player can hear or see: the
    // three sliders are the buses, VSync is the swap interval, Tonemap is the
    // live curve. Bloom binds the ENGINE's own bool with no callback at all,
    // which is the 11.108 dividend -- the field IS the setting, so there is
    // nothing for a handler to forward.
    screen_settings = ui_screen(ui_system, "settings");
    ui_screen_set_modal(screen_settings, true);
    ui_screen_transition(screen_settings, UI_TRANSITION_SLIDE, 0.18f);
    UIElement* set_panel = ui_menu_panel(screen_settings, "Settings");
    ui_slider(set_panel, "Master", 0.0f, 1.0f, &ui_settings.master_volume, ui_settings_changed,
              engine);
    ui_slider(set_panel, "Music", 0.0f, 1.0f, &ui_settings.music_volume, ui_settings_changed,
              engine);
    ui_slider(set_panel, "SFX", 0.0f, 1.0f, &ui_settings.sfx_volume, ui_settings_changed, engine);
    if (engine->postfx)
        ui_toggle(set_panel, "Bloom", &engine->postfx->bloom_enabled, NULL, NULL);
    ui_toggle(set_panel, "VSync", &ui_settings.vsync, ui_settings_changed, engine);
    // In SettingsWindowMode's own order, which is windowed / fullscreen /
    // borderless because borderless was appended to keep saved files reading
    // the same. Presenting a friendlier order would want a mapping between the
    // selector's index and the enum, and a mapping is a thing to get wrong.
    static const char* const display_modes[] = {"Windowed", "Fullscreen", "Borderless"};
    ui_selector(set_panel, "Display", display_modes, ENGINE_WINDOW_MODE_COUNT,
                &ui_settings.window_mode, ui_settings_changed, engine);
    // Offered only where there is a choice to make: one display is every laptop
    // on its own, and a selector with a single option is furniture.
    if (ui_monitor_count > 1)
        ui_selector(set_panel, "Monitor", ui_monitor_names, ui_monitor_count, &ui_monitor_index,
                    ui_monitor_changed, engine);
    static const char* const modes[] = {"Passthrough", "ACES", "Neutral", "AgX", "Linear"};
    ui_tonemap = engine->postfx ? engine->postfx->tonemap_mode : POSTFX_TONEMAP_NEUTRAL;
    ui_selector(set_panel, "Tonemap", modes, 5, &ui_tonemap, ui_tonemap_changed, engine);

    // ESCAPE HATCH 1 on screen: an element the app paints itself, which still
    // lays out, still takes focus and still takes a click -- dropping out of
    // the element vocabulary costs only the drawing.
    UIElement* meter = ui_panel(set_panel);
    ui_set_size(meter, UI_GROW, 0.0f, UI_FIXED, 26.0f);
    ui_set_draw(meter, ui_draw_meter, NULL);
    ui_button(set_panel, "Back", ui_action_close, NULL);
    ui_rows_grow(set_panel);

    // The HUD: NOT modal, so it never takes input, and pushed once for the
    // whole run with the menus stacking above it. Explicit START alignment
    // because a menu's root centres what it holds and a HUD's must not.
    screen_hud = ui_screen(ui_system, "hud");
    ui_screen_set_modal(screen_hud, false);
    UIElement* hud_root = ui_screen_root(screen_hud);
    hud_root->align_main = UI_ALIGN_START;
    hud_root->align_cross = UI_ALIGN_START;
    UIElement* hud_panel = ui_panel(hud_root);
    hud_speed_label = ui_label(hud_panel, "speed  0.00");
    hud_anim_label = ui_label(hud_panel, "blend  0.00");
    ui_push(ui_system, screen_hud);

    // The menus are closed by default, the way a game's are; --ui-screen opens
    // one at startup so a headless run can photograph it.
    if (ui_screen_at_start) {
        // Looked up BY NAME through the layer rather than matched against a
        // chain of handles here: a screen already carries the name it was made
        // with, so a fifth one is reachable without editing this. The HUD is
        // pushed above, so naming it is not an error, just nothing to do.
        UIScreen* start = ui_find_screen(ui_system, ui_screen_at_start);
        if (!start)
            fprintf(stderr, "ui-screen: no screen named '%s'\n", ui_screen_at_start);
        else if (start != screen_hud)
            ui_push(ui_system, start);
    }

    ui_attach(ui_system, engine);
    return true;
}

static void ui_shutdown(void) {
    free_ui_system(ui_system);
    ui_system = NULL;
}

/*
 * --save-probe: the save format asserted where it is a pure function of a file
 * and a world, printed as `save <case> <label> <key> <numbers>` at %.6f so the
 * one regex the audio, anim and ui probes already share reads it too.
 *
 * It builds its own small world rather than using on_init's: a probe game has
 * no init callback, and a handful of entities makes every number below
 * something this file can state in closed form.
 */
typedef struct ProbeState {
    float scaled;
} ProbeState;

static ProbeState probe_state;

static const SaveField SAVE_PROBE_FIELDS[] = {
    SAVE_ROW(SAVE_FLOAT, "scaled", ProbeState, scaled),
};
#define SAVE_PROBE_COUNT ((int)(sizeof(SAVE_PROBE_FIELDS) / sizeof(SAVE_PROBE_FIELDS[0])))

/*
 * The case tags cannot express: "scaled" meant percent at version 1 and a unit
 * fraction at version 2. Same key, same type, different meaning -- so only a
 * migration can repair it, and this is what one looks like.
 */
static bool probe_migrate_scaled(cJSON* section) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(section, "scaled");
    if (!cJSON_IsNumber(item))
        return false;
    cJSON_SetNumberHelper(item, item->valuedouble / 100.0);
    return true;
}

static int save_probe_path(char* out, size_t cap, const char* slot) {
    if (!save_default_path(out, cap, slot)) {
        fprintf(stderr, "save-probe: could not resolve a save path\n");
        return 0;
    }
    return 1;
}

static int run_save_probe(Game* game, const char* which) {
    // A scene of its own: spawn_box builds a visual, and the node it makes has
    // to belong somewhere even though this probe never draws a frame. The
    // program comes from the engine the same way on_init gets it -- the probe
    // stands in for the app, so it should reach the app's path rather than have
    // create_box_node tolerate a missing one.
    Scene* scene = create_scene();
    game_set_scene(game, scene);
    pbr_shader = engine_get_program(game->engine, CETRA_PROGRAM_PBR);

    PhysicsConfig physics_config = physics_default_config();
    PhysicsWorld* physics = create_physics_world(&physics_config);
    if (physics)
        game_set_physics_world(game, physics);
    EntityManager* em = create_entity_manager(game);
    game_set_entity_manager(game, em);

    save_system = create_save_system(game);
    if (!save_system) {
        fprintf(stderr, "save-probe: no save system\n");
        return 1;
    }
    save_register_table(save_system, "gametest", SAVE_APP_VERSION, SAVE_APP_FIELDS, SAVE_APP_COUNT,
                        game);
    save_register_spawner(save_system, "box", spawn_box_from_save, game);

    char path[1024];
    if (!save_probe_path(path, sizeof(path), "probe"))
        return 1;
    int rc = 0;

    if (!strcmp(which, "roundtrip")) {
        box_count = 7;
        player_yaw = 1.25f;
        chaser_yaw = -0.5f;
        heart_timer = 0.25f;
        catch_cooldown = 1.5f;
        door_open_pending = true;
        door_open_velocity = -6.0f;
        player_touching_door = true;
        printf("save roundtrip wrote box_count %.6f\n", (double)box_count);
        printf("save roundtrip wrote player_yaw %.6f\n", (double)player_yaw);
        printf("save roundtrip wrote door_open_velocity %.6f\n", (double)door_open_velocity);
        if (!save_write(save_system, path)) {
            fprintf(stderr, "save-probe: write failed\n");
            return 1;
        }
        // Zeroed, not merely left alone: against values that already hold the
        // answer, a reader that stores nothing at all would pass.
        box_count = 0;
        player_yaw = 0.0f;
        chaser_yaw = 0.0f;
        heart_timer = 0.0f;
        catch_cooldown = 0.0f;
        door_open_pending = false;
        door_open_velocity = 0.0f;
        player_touching_door = false;
        const SaveLoadResult r = save_read(save_system, path);
        printf("save roundtrip read ok %.6f\n", r.ok ? 1.0 : 0.0);
        printf("save roundtrip read box_count %.6f\n", (double)box_count);
        printf("save roundtrip read player_yaw %.6f\n", (double)player_yaw);
        printf("save roundtrip read chaser_yaw %.6f\n", (double)chaser_yaw);
        printf("save roundtrip read heart_timer %.6f\n", (double)heart_timer);
        printf("save roundtrip read catch_cooldown %.6f\n", (double)catch_cooldown);
        printf("save roundtrip read door_open_pending %.6f\n", door_open_pending ? 1.0 : 0.0);
        printf("save roundtrip read door_open_velocity %.6f\n", (double)door_open_velocity);
        printf("save roundtrip read player_touching_door %.6f\n", player_touching_door ? 1.0 : 0.0);
    } else if (!strcmp(which, "entities")) {
        vec3 pos = {1.0f, 2.0f, 3.0f};
        vec3 color = {0.5f, 0.25f, 0.125f};
        Entity* box = spawn_box(game, "probe_box", pos, 0.5f, color);
        if (!box) {
            fprintf(stderr, "save-probe: could not build a box\n");
            return 1;
        }
        RigidBody* rb = entity_get_rigid_body(box);
        rigid_body_set_linear_velocity(rb, (vec3){4.0f, -5.0f, 6.0f});
        printf("save entities wrote position %.6f %.6f %.6f\n", (double)box->position[0],
               (double)box->position[1], (double)box->position[2]);
        printf("save entities wrote velocity %.6f %.6f %.6f\n", 4.0, -5.0, 6.0);
        if (!save_write(save_system, path))
            return 1;

        // Moved and stopped, so a reader that does nothing cannot pass.
        rigid_body_set_position(rb, (vec3){-9.0f, -9.0f, -9.0f});
        rigid_body_set_linear_velocity(rb, (vec3){0.0f, 0.0f, 0.0f});
        glm_vec3_copy((vec3){-9.0f, -9.0f, -9.0f}, box->position);

        const SaveLoadResult r = save_read(save_system, path);
        vec3 v;
        rigid_body_get_linear_velocity(rb, v);
        printf("save entities read count %.6f\n", (double)r.entities_restored);
        printf("save entities read position %.6f %.6f %.6f\n", (double)box->position[0],
               (double)box->position[1], (double)box->position[2]);
        printf("save entities read velocity %.6f %.6f %.6f\n", (double)v[0], (double)v[1],
               (double)v[2]);
    } else if (!strcmp(which, "spawned")) {
        vec3 pos = {2.0f, 8.0f, -3.0f};
        vec3 color = {0.75f, 0.5f, 0.25f};
        Entity* box = spawn_box(game, "box_0", pos, 0.875f, color);
        if (!box)
            return 1;
        save_note_spawn(save_system, "box_0", "box", box_params(pos, 0.875f, color));
        if (!save_write(save_system, path))
            return 1;

        // A world that never had it: the record is now the only thing that
        // says this crate ever existed.
        destroy_entity(em, box);
        printf("save spawned before found %.6f\n", find_entity_by_name(em, "box_0") ? 1.0 : 0.0);
        const SaveLoadResult r = save_read(save_system, path);
        const Entity* back = find_entity_by_name(em, "box_0");
        printf("save spawned after count %.6f\n", (double)r.entities_spawned);
        printf("save spawned after found %.6f\n", back ? 1.0 : 0.0);
        if (back)
            printf("save spawned after position %.6f %.6f %.6f\n", (double)back->position[0],
                   (double)back->position[1], (double)back->position[2]);
    } else if (!strcmp(which, "drops")) {
        // One record naming an entity nothing provides, one naming a spawner
        // this build does not have. Both drop; neither costs the file.
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "version", SAVE_FORMAT_VERSION);
        cJSON* section = cJSON_AddObjectToObject(root, "entities");
        cJSON_AddNumberToObject(section, "version", 1);
        cJSON* list = cJSON_AddArrayToObject(section, "list");
        cJSON* a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "name", "a_ghost");
        cJSON_AddItemToArray(list, a);
        cJSON* b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "name", "b_ghost");
        cJSON_AddStringToObject(b, "spawner", "no_such_recipe");
        cJSON_AddItemToArray(list, b);
        char* text = cJSON_Print(root);
        cJSON_Delete(root);
        FILE* f = fopen(path, "wb");
        if (!f || !text) {
            free(text);
            if (f)
                fclose(f);
            return 1;
        }
        fputs(text, f);
        fclose(f);
        free(text);

        const SaveLoadResult r = save_read(save_system, path);
        printf("save drops read ok %.6f\n", r.ok ? 1.0 : 0.0);
        printf("save drops read missing %.6f\n", (double)r.dropped_missing_entity);
        printf("save drops read unknown_spawner %.6f\n", (double)r.dropped_unknown_spawner);
    } else if (!strcmp(which, "floor")) {
        player_yaw = 3.0f;
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "version", SAVE_FLOOR - 1);
        cJSON* section = cJSON_AddObjectToObject(root, "gametest");
        cJSON_AddNumberToObject(section, "version", 1);
        cJSON_AddNumberToObject(section, "player_yaw", 99.0);
        char* text = cJSON_Print(root);
        cJSON_Delete(root);
        FILE* f = fopen(path, "wb");
        if (!f || !text) {
            free(text);
            if (f)
                fclose(f);
            return 1;
        }
        fputs(text, f);
        fclose(f);
        free(text);

        const SaveLoadResult r = save_read(save_system, path);
        // Refused whole: nothing below the floor is applied, so the live value
        // is the one that was already there.
        printf("save floor read ok %.6f\n", r.ok ? 1.0 : 0.0);
        printf("save floor read player_yaw %.6f\n", (double)player_yaw);
    } else if (!strcmp(which, "migrate")) {
        probe_state.scaled = 0.0f;
        save_register_table(save_system, "probe", 2, SAVE_PROBE_FIELDS, SAVE_PROBE_COUNT,
                            &probe_state);
        save_register_migration(save_system, "probe", 1, probe_migrate_scaled);

        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "version", SAVE_FORMAT_VERSION);
        cJSON* section = cJSON_AddObjectToObject(root, "probe");
        cJSON_AddNumberToObject(section, "version", 1);
        cJSON_AddNumberToObject(section, "scaled", 75.0); // percent, at version 1
        char* text = cJSON_Print(root);
        cJSON_Delete(root);
        FILE* f = fopen(path, "wb");
        if (!f || !text) {
            free(text);
            if (f)
                fclose(f);
            return 1;
        }
        fputs(text, f);
        fclose(f);
        free(text);

        const SaveLoadResult r = save_read(save_system, path);
        printf("save migrate read ok %.6f\n", r.ok ? 1.0 : 0.0);
        printf("save migrate read steps %.6f\n", (double)r.migrations_run);
        printf("save migrate read scaled %.6f\n", (double)probe_state.scaled);
    } else {
        fprintf(stderr, "save-probe: unknown case '%s'\n", which);
        rc = 1;
    }

    free_save_system(save_system);
    save_system = NULL;
    return rc;
}

int main(int argc, const char* argv[]) {
    printf("=== Physics Test ===\n\n");

    // Parse command line arguments. The HDR path stays POSITIONAL, which is the
    // whole interface this app had before it grew flags -- anything not
    // recognised below is still taken as the environment.
    bool headless = false;
    bool force_taa = false;
    int msaa = 0;
    int frames = 0;
    int screenshot_every = 0;
    const char* screenshot = NULL;
    const char* pad_script = NULL;
    const char* gamepad_db = NULL;
    const char* audio_probe = NULL;
    const char* audio_file = NULL;
    const char* anim_probe = NULL;
    const char* ik_probe = NULL;
    const char* ragdoll_probe = NULL;
    const char* ui_probe = NULL;
    const char* cam_probe = NULL;
    const char* display_probe = NULL;
    const char* save_probe = NULL;
    bool ui_enabled = true;
    // 0 = the default below. A golden states the size it was baked at, so a
    // headless app that cannot be sized can only be photographed at whatever
    // this display happens to be.
    int win_w = 0;
    int win_h = 0;
    int display_mode = ENGINE_WINDOW_WINDOWED;
    const char* display_monitor = NULL;
    bool list_monitors = false;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "-x") || !strcmp(a, "--headless")) {
            headless = true;
        } else if (!strcmp(a, "--fullscreen")) {
            display_mode = ENGINE_WINDOW_FULLSCREEN;
        } else if (!strcmp(a, "--borderless")) {
            display_mode = ENGINE_WINDOW_BORDERLESS;
        } else if (!strcmp(a, "--monitor") && i + 1 < argc) {
            display_monitor = argv[++i];
        } else if (!strcmp(a, "--list-monitors")) {
            list_monitors = true;
        } else if (!strcmp(a, "--ragdoll-probe") && i + 1 < argc) {
            ragdoll_probe = argv[++i];
        } else if (!strcmp(a, "--taa")) {
            force_taa = true;
        } else if ((!strcmp(a, "-f") || !strcmp(a, "--frames")) && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if ((!strcmp(a, "-S") || !strcmp(a, "--screenshot")) && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (!strcmp(a, "--screenshot-every") && i + 1 < argc) {
            screenshot_every = atoi(argv[++i]);
        } else if ((!strcmp(a, "-W") || !strcmp(a, "--width")) && i + 1 < argc) {
            win_w = atoi(argv[++i]);
        } else if ((!strcmp(a, "-H") || !strcmp(a, "--height")) && i + 1 < argc) {
            win_h = atoi(argv[++i]);
        } else if (!strcmp(a, "--msaa") && i + 1 < argc) {
            msaa = atoi(argv[++i]);
        } else if (!strcmp(a, "--trace-player")) {
            trace_player = true;
        } else if (!strcmp(a, "--trace-every") && i + 1 < argc) {
            trace_every = atoi(argv[++i]);
            if (trace_every < 1)
                trace_every = 1;
        } else if (!strcmp(a, "--pad-script") && i + 1 < argc) {
            pad_script = argv[++i];
        } else if (!strcmp(a, "--gamepad-db") && i + 1 < argc) {
            gamepad_db = argv[++i];
        } else if (!strcmp(a, "--mute")) {
            audio_muted = true;
        } else if (!strcmp(a, "--audio-probe") && i + 1 < argc) {
            audio_probe = argv[++i];
        } else if (!strcmp(a, "--audio-file") && i + 1 < argc) {
            audio_file = argv[++i];
        } else if (!strcmp(a, "--no-puppet")) {
            no_puppet = true;
        } else if (!strcmp(a, "--no-chaser")) {
            no_chaser = true;
        } else if (!strcmp(a, "--no-follow-cam")) {
            follow_cam = false;
        } else if (!strcmp(a, "--fov") && i + 1 < argc) {
            cam_pose_fov_deg = (float)atof(argv[++i]);
        } else if (!strcmp(a, "--cam-eye") && i + 1 < argc) {
            cam_eye_set = parse_vec3_arg(argv[++i], cam_pose_eye);
            if (!cam_eye_set)
                fprintf(stderr, "--cam-eye expects x,y,z\n");
        } else if (!strcmp(a, "--cam-target") && i + 1 < argc) {
            cam_target_set = parse_vec3_arg(argv[++i], cam_pose_target);
            if (!cam_target_set)
                fprintf(stderr, "--cam-target expects x,y,z\n");
        } else if (!strcmp(a, "--cam-up") && i + 1 < argc) {
            if (!parse_vec3_arg(argv[++i], cam_pose_up))
                fprintf(stderr, "--cam-up expects x,y,z\n");
        } else if (!strcmp(a, "--speed") && i + 1 < argc) {
            player_speed = strtof(argv[++i], NULL);
            if (player_speed <= 0.0f)
                player_speed = PLAYER_SPEED;
            speed_override = true;
        } else if (!strcmp(a, "--no-lock")) {
            no_lock = true;
        } else if (!strcmp(a, "--no-ik")) {
            no_ik = true;
        } else if (!strcmp(a, "--no-root-motion")) {
            no_root_motion = true;
        } else if (!strcmp(a, "--no-crates")) {
            no_crates = true;
        } else if (!strcmp(a, "--puppet") && i + 1 < argc) {
            puppet_path = argv[++i];
        } else if (!strcmp(a, "--twin") && i + 1 < argc) {
            twin_clip = argv[++i];
        } else if (!strcmp(a, "--anim-probe") && i + 1 < argc) {
            anim_probe = argv[++i];
        } else if (!strcmp(a, "--ik-probe") && i + 1 < argc) {
            ik_probe = argv[++i];
        } else if (!strcmp(a, "--ui-probe") && i + 1 < argc) {
            ui_probe = argv[++i];
        } else if (!strcmp(a, "--cam-probe") && i + 1 < argc) {
            cam_probe = argv[++i];
        } else if (!strcmp(a, "--display-probe") && i + 1 < argc) {
            display_probe = argv[++i];
        } else if (!strcmp(a, "--save-probe") && i + 1 < argc) {
            save_probe = argv[++i];
        } else if (!strcmp(a, "--no-ui")) {
            ui_enabled = false;
        } else if (!strcmp(a, "--ui-screen") && i + 1 < argc) {
            // Open a screen at startup. A menu otherwise starts closed, the way
            // a game's does, which leaves no way to photograph one: a headless
            // run has no Escape key to press.
            ui_screen_at_start = argv[++i];
        } else if (!strcmp(a, "--ui-focus") && i + 1 < argc) {
            ui_focus_steps = atoi(argv[++i]);
        } else if (!strcmp(a, "--print-bindings")) {
            input_print_actions(actions, ACTION_COUNT);
            return 0;
        } else if (a[0] == '-') {
            // A dash-led token is never a path. Without this a typo'd flag,
            // or a value flag in final position whose guard above just
            // failed, becomes the environment path and the run reports
            // "Using HDR environment: --msaa".
            fprintf(stderr, "gametest: unknown or incomplete option '%s'\n", a);
            return -1;
        } else {
            hdr_path = a;
            printf("Using HDR environment: %s\n\n", hdr_path);
        }
    }

    // The settings case runs before the engine exists, because nothing it
    // measures needs one. Every other case needs a font -- FIT sizing is made
    // of measurement -- so it takes a headless game, the shape the audio and
    // anim probes already established, and still never draws a frame.
    if (ui_probe && !strcmp(ui_probe, "settings")) {
        return run_ui_probe(ui_probe);
    }
    // Every case is pure arithmetic over camera_rig.c, so this needs no engine
    // at all -- which is the rig's design asserted rather than described.
    if (cam_probe) {
        return run_cam_probe(cam_probe);
    }
    // The placement case is pure arithmetic and runs before any engine exists,
    // the shape --ui-probe settings already established. The other two need
    // GLFW initialised for the monitor list and a live Engine to apply onto, so
    // they take a headless game and still never draw a frame.
    if (display_probe && !strcmp(display_probe, "placement")) {
        return run_display_probe(NULL, display_probe);
    }
    if (display_probe) {
        GameConfig probe_config = {.engine = {.title = "display-probe", .headless = true}};
        Game* probe_game = create_game(&probe_config);
        if (!probe_game) {
            fprintf(stderr, "display-probe: could not create game\n");
            return -1;
        }
        int rc = run_display_probe(probe_game, display_probe);
        free_game(probe_game);
        return rc;
    }
    if (ui_probe) {
        GameConfig probe_config = {.engine = {.title = "ui-probe", .headless = true}};
        Game* probe_game = create_game(&probe_config);
        if (!probe_game) {
            fprintf(stderr, "ui-probe: could not create game\n");
            return -1;
        }
        int rc = run_ui_screens_probe(probe_game, ui_probe);
        free_game(probe_game);
        return rc;
    }

    // A self-contained offline render: a headless game, the offline audio
    // system, no window loop. It prints its measurements and exits, which is
    // what the `audio` gate reads.
    if (audio_probe) {
        GameConfig probe_config = {.engine = {.title = "audio-probe", .headless = true}};
        Game* probe_game = create_game(&probe_config);
        if (!probe_game) {
            fprintf(stderr, "audio-probe: could not create game\n");
            return -1;
        }
        int rc = run_audio_probe(probe_game, audio_probe, audio_file);
        free_game(probe_game);
        return rc;
    }

    // The same shape for the animator: a headless game, the rig loaded, the
    // components ticked by the loop's own function, no window.
    // The same shape again for the IK solver, and for the same reason: these ten cases
    // are arithmetic, so they want a rig and nothing else -- no physics world, no
    // window, no frame.
    if (ik_probe) {
        GameConfig probe_config = {.engine = {.title = "ik-probe", .headless = true}};
        Game* probe_game = create_game(&probe_config);
        if (!probe_game) {
            fprintf(stderr, "ik-probe: could not create game\n");
            return -1;
        }
        int rc = run_ik_probe(probe_game, ik_probe);
        free_game(probe_game);
        return rc;
    }

    // Unlike the four above, this one wants the physics world too: the build
    // cases are arithmetic, but the simulation cases step Jolt.
    if (ragdoll_probe) {
        GameConfig probe_config = {.engine = {.title = "ragdoll-probe", .headless = true}};
        Game* probe_game = create_game(&probe_config);
        if (!probe_game) {
            fprintf(stderr, "ragdoll-probe: could not create game\n");
            return -1;
        }
        int rc = run_ragdoll_probe(probe_game, ragdoll_probe);
        free_game(probe_game);
        return rc;
    }

    if (anim_probe) {
        GameConfig probe_config = {.engine = {.title = "anim-probe", .headless = true}};
        Game* probe_game = create_game(&probe_config);
        if (!probe_game) {
            fprintf(stderr, "anim-probe: could not create game\n");
            return -1;
        }
        int rc = run_anim_probe(probe_game, anim_probe);
        free_game(probe_game);
        return rc;
    }

    // And for the save format: a headless game, a physics world and a few
    // entities of its own, no window and no frame.
    if (save_probe) {
        GameConfig probe_config = {.engine = {.title = "save-probe", .headless = true}};
        Game* probe_game = create_game(&probe_config);
        if (!probe_game) {
            fprintf(stderr, "save-probe: could not create game\n");
            return -1;
        }
        int rc = run_save_probe(probe_game, save_probe);
        free_game(probe_game);
        return rc;
    }

    printf("Controls (keyboard / gamepad):\n");
    printf("  WASD / left stick, dpad - Move the player (idle -> walk -> run as it speeds up)\n");
    printf("  Space / A - Jump\n");
    printf("  E / LB - Wave (the right arm, over whatever the legs are doing)\n");
    if (clip_lunge)
        printf("  Q / RT - Lunge (the clip carries you a stated distance)\n");
    if (clip_spin)
        printf("  C / LT - Spin (half a turn on the spot, carrying nothing)\n");
    printf("A second puppet chases you. Let it catch you (--no-chaser to turn it off).\n");
    printf("  F / X - Spawn falling box\n");
    printf("  R / Y - Raycast downward from player\n");
    printf("  G / B - Print ground state\n");
    printf("  P / Start - Pause/unpause physics\n");
    printf("  Arrow keys - Turn the camera (--no-follow-cam for the fixed orbit instead)\n");
    printf("  --speed <m/s> - Walk at another speed (default %.0f). Foot locking engages\n",
           (double)PLAYER_SPEED);
    printf("                  only near the stride the clip implies, about 1 -- at the\n");
    printf("                  default nothing is ever in contact and --no-lock looks the same\n");
    printf("  Walk off the edge - fall into the grotto and swim; the chaser swims too\n");
    printf("  Escape - Pause menu (--no-ui to run without any of it)\n");
    printf("Audio: a beep on jump and spawn, footsteps in time with the stride, a looping\n");
    printf("       tone at the door (--mute to silence)\n");
    printf("\nWalk into the door (right side) to push it open!\n\n");

    srand(42); // Deterministic random for testing

    // Create game
    GameConfig config = {.engine = {.title = "Physics Test - JoltC Integration",
                                    .width = win_w > 0 ? win_w : 1280,
                                    .height = win_h > 0 ? win_h : 720,
                                    .headless = headless,
                                    .window_mode = display_mode,
                                    .monitor = display_monitor}};

    // TAA replaces MSAA rather than joining it. This app is rigid meshes on the
    // pbr program, so every surface writes a motion vector and the accumulator
    // has something honest to reproject -- which is what separates it from the
    // particle demo next door (spec 11.103).
    //
    // Headless keeps MSAA and skips TAA unless asked, because jitter plus a
    // history makes the frame sensitive to async load timing.
    if (!headless || force_taa) {
        config.engine.msaa_samples = 1;
        config.engine.taa = true;
    }
    // After the policy, so --taa --msaa 4 is expressible.
    if (msaa > 0)
        config.engine.msaa_samples = msaa;

    Game* game = create_game(&config);
    if (!game) {
        fprintf(stderr, "Failed to create game\n");
        return -1;
    }
    if (list_monitors) {
        // The names --monitor takes, printed from the engine's own view rather
        // than the platform's, so what a settings file should store and what
        // this build can actually find are the same list.
        int monitors = 0;
        const char* const* names = engine_monitor_names(game->engine, &monitors);
        printf("monitors %d\n", monitors);
        for (int i = 0; i < monitors; i++) {
            printf("monitor %d %s\n", i, names[i]);
        }
        free_game(game);
        return 0;
    }
    if (ui_enabled && !ui_install(game->engine)) {
        free_game(game);
        return -1;
    }
    game->engine->exit_after_frames = frames;
    engine_set_screenshot_path(game->engine, screenshot);
    game->engine->screenshot_every = screenshot_every;

    // A refused script or mapping file is a failed run, not a run with an
    // idle pad: a gate reading the trace must never mistake one for the other.
    if (gamepad_db && !input_load_gamepad_mappings(gamepad_db)) {
        free_game(game);
        return -1;
    }
    if (pad_script && !input_set_pad_script(&game->input, pad_script)) {
        free_game(game);
        return -1;
    }
    input_bind(&game->input, actions, ACTION_COUNT);

    // Set mouse callback
    engine_set_mouse_button_callback(game->engine, mouse_button_callback);

    // Set game callbacks
    // Unconditional: the hook carries the quicksave edges as well as the menu,
    // and it returns early of its own accord when there is no UI.
    game_set_frame_input(game, on_frame_input);
    game_set_init(game, on_init);
    game_set_update(game, on_update);
    game_set_pre_render(game, on_pre_render);
    game_set_render(game, on_render);
    game_set_shutdown(game, on_shutdown);

    // Run the game
    game_run(game);

    // Cleanup
    ui_shutdown();
    free_game(game);

    printf("Goodbye!\n");
    return 0;
}
