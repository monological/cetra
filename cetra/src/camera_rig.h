#ifndef _CAMERA_RIG_H_
#define _CAMERA_RIG_H_

/*
 * What MOVES a camera, and what the controls MEAN under it (spec 12.19).
 *
 * `camera.h` owns a pose, a projection and the moves that get from one pose to
 * another. This owns the behaviour over time: where the camera sits relative to
 * something, how input turns it, how it gets out of the way of a wall, and how
 * one camera becomes another. Four apps hand-rolled that four times before this
 * existed, and the roadmap's complaint was not the duplication but its
 * consequence -- a hand-rolled camera also decides what the CONTROLS mean, and
 * in `apps/gametest` that decision flipped twice with nothing able to see it.
 *
 * WHAT A RIG IS: an anchor, an arm and an aim.
 *
 *     look = anchor + look_lift*Y                      the point it aims at
 *     eye  = look + eye_lift*Y - dir(yaw, pitch)*dist  where it sits
 *
 * There is no mode enum for that, because there does not need to be one. A
 * third-person follow is an anchor that moves; a viewer orbit is an anchor that
 * does not; first person is `dist` of zero; and a PINNED pose is
 * camera_rig_set_pose, which derives all three from an eye and a target rather
 * than being a fourth kind of thing. What varies between them is which fields a
 * caller writes, not which branch runs.
 *
 * `eye_lift` raises the eye WITHOUT raising the point it looks at, so the view
 * tilts down by an amount the pitch does not state. That is not a quirk to be
 * tidied away: it is `apps/gametest`'s follow camera, whose two menu goldens
 * frame the player through exactly that bias.
 *
 * FIRST PERSON has no aim POINT, only an aim direction, and the header says so
 * rather than the code smoothing it over: at `dist` 0 the eye and the look
 * point would coincide, so the target is put one unit along the aim. One unit
 * rather than any other because that is what both hand-rolled first-person
 * cameras already used, and because `distance(position, look_at)` is read
 * downstream -- by the DoF autofocus, by the near-clip recompute and by
 * `max_distance` -- so a target at a token distance would be read as a focus
 * plane in somebody's face.
 *
 * NO GL, NO PHYSICS, NO CLOCK AND NO GAME TYPE reach this file. It takes its
 * input as plain values the way `ui.h` does, so an app with no game framework
 * can use one, and so an arm can assert a camera with no window at all.
 */

#include <cglm/cglm.h>
#include <stdbool.h>

typedef struct Camera Camera;

// A pose, as the rig produces and blends them. Not a Camera: a blend needs two
// of these at once and a Camera is the thing being written, not a value.
typedef struct CameraRigPose {
    vec3 eye;
    vec3 look;
} CameraRigPose;

typedef struct CameraRig {
    // ENGINE-OWNED (by the rig): what it worked out. Read freely, never write.
    CameraRigPose pose; // where the last update put it
    bool posed;         // false until the first update, so a blend has a `from`

    // BY FUNCTION: camera_rig_set_pose derives these three from an eye and a
    // target together, which is the only way to write them consistently; after
    // that they are the rig's own and an input moves them.
    // The aim, as radians. The direction is
    //   dir = (sin(yaw)cos(pitch), sin(pitch), cos(yaw)cos(pitch))
    // written out here because three apps derived it with three different sign
    // conventions and the only way to tell which one a reader is looking at was
    // to work backwards from where the camera ended up.
    float yaw;
    float pitch; // clamped to [pitch_min, pitch_max] every update

    // SETTINGS: plain stores. Write them directly, at any time.
    vec3 anchor;     // the point the rig is about; an app writes it each frame
    float dist;      // eye to look point; 0 is first person
    float look_lift; // the aim point above the anchor
    float eye_lift;  // the eye above the aim point -- a look-DOWN bias
    float pitch_min, pitch_max;
    // Radians per second per unit of input, so a caller passes -1..1 from an
    // action or a stick and the rate lives here rather than in each app.
    float yaw_rate, pitch_rate;
    // What the CONTROLS mean. False -- the default -- is a camera that does not
    // touch them, and a game reads its input in world axes. True publishes this
    // rig's yaw through camera_rig_move_basis, and a game reads its input in
    // that frame.
    //
    // Default false on purpose, and it is spec 12.17's decision rather than a
    // convenience: pinning a camera states a POSE, and having that silently
    // rotate a player's controls is a worse surprise than a viewer camera
    // keeping the scheme it had. A rig that steers says so.
    bool steers_controls;
} CameraRig;

// A rig at the origin with a viewer's defaults: no lift, pitch free between
// -1.5 and 1.5, rates of 1.8 and 1.2 radians a second (`apps/gametest`'s and
// `apps/forest`'s, which were the same number written twice), and not steering
// the controls. NULL on allocation failure, logged.
CameraRig* create_camera_rig(void);
void free_camera_rig(CameraRig* rig);

// Turn by `yaw_in` and `pitch_in`, each -1..1, over `dt` seconds, then place the
// eye. A dt of 0 holds the aim and still places the eye, which is what lets an
// app move the anchor on a paused clock and keep the framing.
void camera_rig_update(CameraRig* rig, float dt, float yaw_in, float pitch_in);

// Adopt an eye and a target: the anchor, distance and aim are derived so the
// next update reproduces this pose exactly. What `--cam-eye` does, and what a
// 2D controller does with a pose it worked out itself.
//
// It ZEROES both lifts, which is a statement rather than a side effect: a lift
// is how a DERIVED pose is shaped relative to something it follows, and a pose
// somebody stated has already been shaped. Leaving them would displace the very
// pose being adopted.
void camera_rig_set_pose(CameraRig* rig, const vec3 eye, const vec3 look);

// Write the rig's pose to a camera. Separate from the update because the update
// is pure and this is the one line that touches something else.
void camera_rig_apply(const CameraRig* rig, Camera* camera);

// The yaw a steering rig reads the controls in; false, leaving `out_yaw`
// untouched, for a rig that does not steer them.
bool camera_rig_move_basis(const CameraRig* rig, float* out_yaw);

#endif // _CAMERA_RIG_H_
