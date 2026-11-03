# Cameras and walkers

What moves a camera (`camera_rig.c/h`), the app helpers that feed it (`app.c/h`), and a first-person
walker's gait (`gait.c/h`). The engine has no camera modes: a follow camera, an orbit and first
person are one rig with different fields written.

## The rig

`camera_rig.c/h` (spec 12.19) is what moves a camera, and what the controls mean under it. A rig is
an anchor, an arm and an aim:

```
look = anchor + look_lift*Y        the point it aims at
eye  = look + eye_lift*Y - dir(yaw, pitch)*dist
```

There is no mode enum. A follow camera is an anchor that moves, a viewer orbit one that does not,
first person a distance of zero, and a pinned pose derives all three. What varies is which fields a
caller writes.

- **`eye_lift` raises the eye and not the aim**, a look-down bias of a size the pitch does not state;
  gametest's follow camera frames the player through it.
- **First person has an aim direction, not an aim point**; at distance 0 the look point is put one
  `typical_size` along the aim, since the DoF autofocus and the near clip read that distance.
- **A stated pose is kept verbatim** (`camera_rig_set_pose`) rather than re-derived. Decomposing a
  pose into an angle and an arm and rebuilding it loses precision in proportion to the arm, so a
  distant framing restored that way does not reproduce.

### Layered on the arm

- **A response**: a 0..1 signal the app writes each frame (`want_wide`), smoothed with asymmetric
  rates, lerping distance and eye lift between a near pair and a far one. The rig does not interpret
  it, so a fall and a speed-reactive framing are the same field.
- **An occlusion probe** (`camera_rig_set_probe`): reports how much of the arm is clear, and the rig
  shortens the arm, never below `min_dist`. The aim is untouched, which a floor clamp on the eye
  does not manage.
- **A blend of poses** (`camera_rig_blend_from_here`), not of parameters: blending a yaw against a yaw
  about a different point swings the camera.
- **A shake** (`camera_rig_shake`) that decays to nothing; a second replaces the first. A zero
  `shake_scale` or `shake_player_scale` is bit-identical to no shake.
- **A rail** (`camera_rig_set_rail`): a Catmull-Rom spline through cglm's `glm_smc`, which places
  the eye at `rail_t` while the aim stays the anchor's.

### Steering the controls

`steers_controls` says what the controls mean, and defaults to false. A rig that steers publishes
its yaw through `camera_rig_move_basis`, and a game reads its input in that frame; one that does not
leaves the game reading input in world axes. False by default because pinning a camera states a pose,
and having that silently rotate a player's controls is a worse surprise. `cam-basis` and
`pad-camera-relative` are the arms that say which scheme is live.

**The player's look preference sits beside the authored rates, never on them**: `look_scale` and
`invert_pitch`, and `shake_player_scale` beside `shake_scale`. Settings apply on every edit, so a
preference written into the authored value would compound.

### Who updates and who applies

The rig's update (`camera_rig_update`, or an adapter's) stays with the app, because that is where
the input is. Update a rig once a frame: a second update advances the blend and the shake twice. The
engine applies the installed rig to the camera right after the app's pre-render hook and then derives
the matrices; [`architecture.md`](architecture.md#render-pipeline), Render Pipeline, step 4, has
that window and why its edges matter.

No GL, no physics, no clock and no `game/` type reach the rig. It takes plain values, so an app with
no game framework can use one and an arm can assert a camera with no window.

## What feeds it: `app.c/h`

The app helpers are input adapters, not cameras: each turns a pointer into what a rig is told, and
the rig decides where the eye goes. Neither owns the drag; the engine's input state does.

- **`CameraDrag`**, for a 3D viewer. A drag becomes an aim, a shift-drag an anchor, the wheel and the
  keys a distance. It keeps only pointer state: the aim as the button went down, so an offset from
  the press is an absolute angle rather than an accumulation. `camera_drag_update` updates the rig,
  so nothing else may update the same rig that frame.
- **`PlaneDrag`**, the 2D twin, for a plane under an orthographic camera: pan, move a picked node,
  zoom about the cursor. Its arithmetic is its own, since a 2D camera never rotates, but it writes
  its pose through a rig, so only a rig moves a camera. The 2D canvas is not a scene and pans its own
  views.

Also here: a three-point light rig (`scene_add_three_point_lights`) and the input gate
(`app_can_process_3d_input`).

`apps/pcb` builds against `app.h` from its own repository, so a change to this header can break it
where this tree cannot see.

## The walker's gait

`gait.c/h` (spec 13.47) is a first-person walker's gait as its eye and ears know it: the stride, the
footfalls, the eye's lag on the ground's height, the head's bob and sway, and the pace a person keeps
on a climb, for a walker with no body to animate.

- **A level step is `0.40 + 0.25 * speed`** metres (120 a minute walking, 163 jogging). On a flight
  it is Blondel's 2R + G of 0.63 m, one step a tread on any stair built to the rule, whatever it
  collides as.
- **The eye trails the body's height on a critically damped spring**, clamped tighter above than
  below, so a low ceiling over a flight down is never put through.

Four things about it are easy to get backwards:

- **The stride runs on ground covered**, never on time or on the speed asked for. A walker held
  against a wall stops walking, and how frames fall cannot change how many steps a path takes;
  `gait-split` holds three splits of one path to the same count.
- **It runs at two rates.** `gait_step` runs once a fixed step on that step's exact move (the grade,
  how much of it is a flight, the pace, a landing); `gait_frame` runs once a rendered frame on the
  drawn body's move and the render time (the stride, the footfalls, where the eye goes). So nothing
  the simulation reads depends on the frame rate. Both take displacements, so neither cares where the
  origin is.
- **A footfall is at the bob's trough**, as a heel strikes, never at the phase's wrap: there it lands
  at the top of the head's rise, half a step out, which reads as a limp (`gait-trough`).
- **The bob is a move, never a turn**: lowest as a heel strikes, highest over the standing foot,
  swayed toward it, about half a body's own excursion, with no pitch or roll, which is what makes a
  bob sickening. `bob_scale` 0 is bit-identical to no bob.

Like the rig, it has no GL, no physics, no clock and no `game/` type, so the `gait` group's arms run
on moves `gametest --gait-probe` makes up.
