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

/*
 * How far along a ray from the aim point is clear, as a fraction of `want`.
 * 1 is nothing in the way. The rig applies its own skin and minimum arm to the
 * answer, so a probe reports geometry and decides no policy.
 *
 * A SEAM rather than a physics call, for `ui.h`'s reason and `GamepadReadFn`'s:
 * this file must not know what a PhysicsWorld or a heightfield is, and the two
 * apps that need one answer it in completely different ways -- a Jolt raycast
 * filtered to static bodies, and a terrain height query.
 *
 * It shortens the ARM and never moves the eye sideways or down, which is the
 * difference between a camera that tightens and one whose aim wanders. A
 * floor-clamp on the eye does the second.
 */
typedef float (*CameraRigProbeFn)(void* user, const vec3 from, const vec3 to, float want);

#define CAMERA_RIG_RAIL_MAX 16

typedef struct CameraRig {
    // ENGINE-OWNED (by the rig): what it worked out. Read freely, never write.
    CameraRigPose pose; // where the last update put it
    bool posed;         // false until the first update, so a blend has a `from`
    // The pose before the blend and the shake. They are written into `pose`
    // and read from here, which is what keeps a modifier off its own output.
    CameraRigPose base;
    /*
     * A pose camera_rig_set_pose was given, kept VERBATIM while the derivation
     * still agrees with it.
     *
     * A pose decomposed into an angle and an arm and put back together is not
     * the pose it started as: the error is proportional to the arm, and at a
     * 20,000-unit framing it reaches 0.002 world units -- enough that a restored
     * session does not reproduce the session it came from. Keeping what was
     * stated is what makes --cam-eye and a config restore exact.
     *
     * `stated_derive` is what the derivation produced at the moment the pose was
     * stated, and the update compares against it. Comparing the ANSWER rather
     * than enumerating what could have changed it is deliberate: the enumeration
     * was tried and was already incomplete, since a write to look_lift, to
     * either end of the response, to max_dist or to the probe left a held pose
     * silently ignoring it.
     */
    bool pose_stated;
    CameraRigPose stated;
    vec3 stated_derive;
    float wide; // the smoothed arm response, 0..1
    // DERIVED from the near/far pairs and `wide` every update, and shortened by
    // the probe. They are here rather than under SETTINGS because the response
    // is their only writer: a caller storing into one would be a second writer
    // of a value the rig recomputes, which is a field that silently stops
    // meaning what was written into it. camera_rig_set_distance is how a caller
    // with no response says "just this far".
    float dist;
    float eye_lift;
    float shake_left;  // seconds of shake still to run
    float shake_secs;  // what it started at, so the decay is a fraction
    float shake_amp;   // its amplitude at the moment it was fired
    float shake_clock; // seconds since this rig started, the noise's argument
    CameraRigPose blend_from;
    float blend_left; // seconds of crossfade still to run
    float blend_secs; // what it started at, so the fraction is recoverable
    // The rail, copied in by camera_rig_set_rail so a caller's array need not
    // outlive the call.
    vec3 rail[CAMERA_RIG_RAIL_MAX];
    int rail_count; // 0 = the arm places the eye; 2+ = the rail does
    bool rail_loop;

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
    float look_lift; // the aim point above the anchor
    float pitch_min, pitch_max;
    // Radians per second per unit of input, so a caller passes -1..1 from an
    // action or a stick and the rate lives here rather than in each app. These
    // are what the APP authored; a player setting never overwrites them.
    float yaw_rate, pitch_rate;
    /*
     * What a PLAYER chose about looking: a multiplier on the rates above, and
     * whether up on the stick looks down.
     *
     * Separate from the rates rather than folded into them, because settings are
     * applied on EVERY edit -- holding a sensitivity slider reaches this once a
     * frame -- and scaling a rate in place compounds until the camera spins.
     * Measured at 9.11 rad/s where 4.05 was wanted, two applies in. The same
     * idempotence engine_set_window_mode needed in spec 12.15, for the same
     * reason and found the same way.
     */
    float look_scale;  // 1 = as authored
    bool invert_pitch; // up on the stick looks down
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

    /*
     * The arm response: a 0..1 SIGNAL the app writes each frame, smoothed here,
     * lerping the distance and the eye lift between a near pair and a far one.
     *
     * The rig does not know what the signal MEANS, which is the whole of why it
     * generalises: `apps/gametest` drives it from how far the player has fallen
     * below the last ground there was, and the same field is what a framing that
     * opens out with speed would use. Deciding here would have picked one.
     *
     * The rates are asymmetric because opening out and coming back in are not
     * the same event: a shot that widens late reads as lag, and one that
     * tightens fast reads as a snap.
     */
    float want_wide; // the signal, 0..1
    float near_dist, near_eye_lift;
    float far_dist, far_eye_lift;
    float widen_rate, tighten_rate; // per second
    // A ceiling on the arm, 0 = none. Where camera_enforce_max_distance pulled a
    // camera back along its own view ray, this is the same statement made before
    // the eye is placed rather than after -- which is what keeps it from being
    // undone by the next update.
    float max_dist;

    // Where an arm may be shortened to, once a probe has reported. `skin` backs
    // off from the hit; `min_dist` is the floor, because collapsing onto the
    // anchor fills the frame with whatever the camera was following and starts
    // clipping it through the near plane.
    CameraRigProbeFn probe;
    void* probe_user;
    float probe_skin;
    float min_dist;

    // Shake: an offset added to the finished pose, scaled by this. 0 is the
    // motion-reduction setting, and it is exactly the no-shake path rather than
    // a very small one.
    float shake_scale;
    // The PLAYER's half of the same, kept apart for the reason look_scale is:
    // settings are applied on every edit, so a motion-reduction toggle writing
    // the authored amplitude would take an app's own choice with it the first
    // time any control was touched. 1 = as authored, 0 = no shake at all, and
    // the zero is exactly the no-shake path rather than a quiet one.
    float shake_player_scale;
    float shake_freq; // Hz

    // Where along the rail the eye sits, 0..1. The app drives it; the rig owns
    // no clock of its own for the same reason it owns no physics.
    float rail_t;
} CameraRig;

// How far back to sit, with no response: sets the near and far ends and the
// live distance together, which is the invariant a plain store cannot keep.
void camera_rig_set_distance(CameraRig* rig, float dist);

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

// Aim exactly here, clamping the pitch. The companion to the rates above, and
// the two are different KINDS of input rather than the same one twice: a stick
// states how fast to turn, and a mouse drag states where to be looking. A drag
// computed from a latched angle and a pixel offset is the second, and rounding
// it into a rate would make a 200-pixel drag depend on the frame rate.
void camera_rig_aim(CameraRig* rig, float yaw, float pitch);

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
//
// A NULL rig, or one that has not been updated yet, writes nothing and says
// nothing: "no rig installed" is the normal state of an app that poses its own
// camera, and it is what the engine's frame loop hands this every frame in one
// of those apps. Treating it as an error would put a guard back at the very
// call site this exists to remove one from.
void camera_rig_apply(const CameraRig* rig, Camera* camera);

// The unit aim direction. One derivation of the formula the struct states, so
// an adapter that needs a forward vector does not subtract two poses -- which
// is a different vector under a rail or a shortened arm.
void camera_rig_direction(const CameraRig* rig, vec3 out);

// The yaw a steering rig reads the controls in; false, leaving `out_yaw`
// untouched, for a rig that does not steer them.
bool camera_rig_move_basis(const CameraRig* rig, float* out_yaw);

// --- Behaviours over the form above ---

// Install a probe, or NULL to stop shortening the arm.
void camera_rig_set_probe(CameraRig* rig, CameraRigProbeFn probe, void* user);

/*
 * Ride an authored path instead of an arm: the eye is a Catmull-Rom sample of
 * `points` at `rail_t` and the aim stays whatever the rig's anchor and lift say,
 * which is what makes a dolly past a subject one call rather than a mode.
 * `count` of 0 (or NULL) puts the arm back.
 *
 * Catmull-Rom rather than the polyline, because a camera is the one consumer
 * where the corner matters more than the path: a piecewise-linear rail changes
 * direction instantly at every authored point and that reads as a jolt. cglm's
 * glm_smc with GLM_HERMITE_MAT is the evaluator -- Catmull-Rom IS Hermite with
 * tangents taken from the neighbours, so this adds no spline arithmetic to the
 * tree.
 *
 * Copied in, up to CAMERA_RIG_RAIL_MAX; a longer path is refused by name rather
 * than truncated, since a rail silently missing its end is a shot that stops
 * somewhere nobody chose. Under 2 points there is no curve and it is refused
 * too.
 */
bool camera_rig_set_rail(CameraRig* rig, const vec3* points, int count, bool loop);

/*
 * Cross-fade from the pose the rig is in now to whatever it produces next, over
 * `seconds`. Call it on the frame something changes -- the rig switched, the
 * anchor jumped, a cut ended -- and the next `seconds` of updates interpolate
 * out of the old pose.
 *
 * Over POSES and not parameters: two rigs can have different anchors, and
 * blending a yaw against a yaw about a different point swings the camera
 * through an arc nobody asked for.
 */
void camera_rig_blend_from_here(CameraRig* rig, float seconds);

// Fire a shake of `amplitude` (world units) lasting `seconds`, decaying to
// nothing. A second shake replaces the first rather than adding, so a burst of
// events cannot stack into a camera nobody can read.
void camera_rig_shake(CameraRig* rig, float amplitude, float seconds);

/*
 * Frame a bounding sphere: the anchor goes to its centre and the distance to
 * `radius * fit`, leaving the aim alone so a framed subject is seen from
 * wherever the camera already was.
 *
 * Here rather than in each app because every app that frames anything wrote its
 * own, and the engine offered bounds (`scene_bounding_sphere`) and nothing that
 * used them. A `fit` of 0 takes 2.5, which is what `apps/render` has always
 * used; a radius of 0 is a degenerate scene and leaves the rig alone rather than
 * putting the eye on top of it.
 */
void camera_rig_frame_sphere(CameraRig* rig, const vec3 centre, float radius, float fit);

#endif // _CAMERA_RIG_H_
