# gametest

The game layer's demo and test bed (`docs/game-framework.md`): a character on a Jolt
`CharacterVirtual`, played as an animated puppet, on a plate hung over a shaft into a flooded
crater on an eroded island. It has falling crates, a hinged door carrying a sound, a second puppet
that chases the player, a ragdoll, menus, saves and a follow camera, all read through the action
table. It also carries the headless probes a dozen gate groups read.

## Running it

```bash
./out/bin/gametest
./out/bin/gametest ENV.hdr                          # an HDR environment, given positionally
./out/bin/gametest --puppet assets/models/t_pose.fbx
./out/bin/gametest -x -f 120 -S /tmp/gametest.ppm
```

Controls, keyboard and pad:

| Action | Keys | Pad |
|--------|------|-----|
| Move | WASD | left stick, D-pad |
| Turn the camera | arrows | right stick |
| Jump | Space | A |
| Spawn a crate | F | X |
| Wave | E | left bumper |
| Lunge, spin | Q, C | right trigger, left trigger |
| Ragdoll | K | left stick click |
| Quicksave, quickload | F5, F9 | right bumper, right stick click |
| Pause the simulation | P | Start |
| Print the ground state, raycast down | G, R | B, Y |
| Pause menu | Escape | Back |

`--print-bindings` lists the action table and exits.

Flags to start with: `--no-puppet` (the red box in place of the rig), `--puppet <path>` (any
rigged humanoid), `--no-follow-cam` (a fixed orbit), `--cam-eye x,y,z --cam-target x,y,z` (a
stated camera, so a framing can be photographed twice), `--no-ui`, `--mute`. The full list, the
probes' cases and what each prints are in `docs/cli-reference.md`, "The other apps".

It is frame-deterministic headless: the engine hands the loop its fixed dt, the seed is fixed and
Jolt steps a fixed dt, so two runs of one pad script trace and render identically.

## The player and animation

The player is the procedural puppet (`assets/models/puppet.gltf`) on an `ANIMATOR` component. What
plays is an animation graph (`anim_graph.h`): each frame the app writes what is true -- a ground
knob, a rate, a depth under water in metres, grounded, vertical speed, ragdolled -- and the graph's
table decides what plays. `--print-graph` prints the table and what the rig loaded can reach. The
facing is a yaw on an inner node, since the entity node's local transform is the game loop's to
write. Footsteps fire from the clips' own events.

**Travel speed comes from the clips**, one of three ways:
- **Stated by the clips (root motion).** The generated puppet's `travel_walk` and `travel_run`
  state how far they go, so the axis is those speeds, playback stays at rate 1, and the character
  moves by the distance drained from the animator each step. The lunge (Q) and spin (C) are the
  same: distances an animator chose, which rate-scaling would make wrong.
- **Measured from the stride.** An imported rig's clips state no travel, so `build_locomotion`
  measures each locomotion clip's stride at init; each blend entry sits at the speed its clip
  implies, the knob is metres per second, and full stick is the fastest stride times the
  playback ceiling. `--speed` caps it.
- **A fallback.** When the clips neither state nor imply a travel (the generated puppet under
  `--no-root-motion`, whose walk is a straight-leg pendulum), the axis is a fraction of
  `PLAYER_SPEED` and the feet slide. The run says which of the three it is in one line at startup.

`CHASER_FRACTION` and `GROTTO_SWIM_FRACTION` are fractions of the player's derived speed, never
speeds, or they would outrun a player whose speed came from the clips.

Rules that are easy to get wrong:
- **Under root motion the blend knob comes from the stick**, not the achieved speed: a knob fed by
  the achieved speed starts at zero, selects the standing clip, travels nothing and stays there.
  So walking into a wall plays the walk on the spot, where the stride path stops it. **The facing
  is what the stick sets**, since the velocity is derived from it.
- **The standing entry is anchored at zero, not measured.** A breathing idle measures a little
  speed, and an axis starting there would play the idle slow while nothing moved.
- **Give entries of very different length the same period.** The space keeps one clock, so a
  two-tick rest pose beside an 86-tick walk turns the walk many times too fast.
- **The swim axis is stated, not measured**: a swimmer's feet press nothing, and
  `animation_stride_speed` refuses the stroke for that reason. Swimming is a graph state of its
  own, never a fourth entry in the locomotion space, which would blank the trace's three weight
  columns and fail `anim-trace-idle`.
- **The waterline is a band, not a plane** (`PLAYER_SUBMERGE_DEPTH`, `PLAYER_SURFACE_DEPTH`): the
  buoyancy drive aims at the surface, so a floating character crosses it about once a second and a
  plane would crossfade stroke and walk forever. The bob that crossing comes from is caused by the
  drive being a spring below the surface and nothing above it; it is named in the code and left,
  since a continuous response changes how the water feels.
- **A ragdolled player's state has no source, and that stops the animator.** Leaving it running
  kept its clock turning, its footsteps firing and its root motion piling up.
- **Never read a per-frame edge from the fixed step.** A frame may run no steps or several, so
  such an edge is dropped on one and doubled on the other; headless, every frame runs exactly one
  step and hides it.
- **`--no-swim` withholds the stroke clips**, which makes the water state unreachable and is how a
  rig with a walk and no swim is reached on a committed asset.

**`--puppet <path>` takes any rigged humanoid.** A rig with no locomotion of its own is given
`SHARED_CLIPS`, loaded with `t_pose.fbx`'s skeleton as the retarget source. That rig is imported
as a whole scene, the only public way to a `Skeleton`, and freed straight after, since a channel
keeps what it needs of its source bone by value. Without the source the loader has one rest pose
where it needs two, and a third-party rig lies on its face. Drain the texture loader either side of
that import: a `TexturePool` belongs to one `AsyncLoader`, and embedded texture keys (`"*0"`,
`"*1"`, ...) are per file, so two scenes in flight answer each other's lookups.

**Foot planting and locking** (`ik.h`): the feet are placed on what a downward ray finds, and held
in world space through each stance. A ramp rising 1 in 4 from x = 14 and three half-metre steps from
x = -14 to -20 are always in the world, since on flat ground planting is correctly a no-op and shows nothing.
`--no-ik` runs without planting and `--no-lock` keeps planting without the lock. On the generated
puppet the two look the same, since its walk has no stance to lock; a rig whose clips imply a
stride, such as `t_pose.fbx`, shows the difference.

`--twin <clip>` stands a second rig beside the player playing its own clip: two independent poses
over one set of meshes in one frame.

## The camera

The follow camera is on by default, and the arrow keys or the right stick turn it. It is a
`CameraRig` with `steers_controls` set, so the game asks `camera_rig_move_basis` what its input
means and movement is camera-relative: W goes into the screen whichever way the camera points. A
rig that does not steer -- the `--no-follow-cam` drag orbit, a `--cam-eye` pinning -- leaves
movement world-aligned. `pad-camera-relative` is the arm that says which is live. What stays in the
app is what only this game knows: which drop counts as falling, and that swimming counts as footing.

`--cam-eye`, `--cam-target`, `--cam-up` and `--fov` state a pose. The camera is otherwise derived
from the player every frame, so without them no framing can be photographed twice and no change can
be A/B'd. Eye and target come together or not at all.

## The world

The 50x50 plate hangs over a shaft into a crater pool, in an eroded heightfield island built through
`cetra/src/procedural/`. The field is filled by hand rather than by `terrain_field_seed`, which
nulls the installed field and writes the analytic fbm, and island shaping is a dome with no
inverse. Three things about the terrain are load-bearing and none is visible:
- **Detail lives in a horizontal warp, not in the height.** The crater wall's slope is about 4, so
  displacing it sideways is worth about four times displacing it upward; a vertical term only bends
  the rate of climb.
- **Both warps are gated on the unwarped radius.** A horizontal warp drags whatever stands at the
  sample point toward the centre, and ungated it hauls wall height into the pool until a swimmer
  can stand on the bottom. Measure the pool floor's highest point to check it.
- **Every noise source shares one seed.** `terrain.c` memoises the Perlin permutation table one
  entry deep and reshuffles on each miss, so interleaved seeds cost millions of shuffles.

The land is 1600 units across and drowns at its edge, so the domain's square boundary is under the
sea and the only edge in view is a shoreline. Walking off the plate drops the player into water.
Both characters float on a drive toward the surface written where gravity is applied, because Jolt's
buoyancy is not bound, `gravity_factor` is rigid-body-only, and the gravity handed to
`character_controller_update` is world-wide. The submersion test asks the water where its surface
is; it does not yet read the wave displacement through `water_probe_result`.

**The seabed is not decoration.** Water colour is the refraction of real geometry attenuated over a
depth path, so a pale floor under shallow water is what makes it turquoise; with nothing behind the
surface every pixel takes the maximum path and the sea goes flat.

**The platform's key light is raked 45 degrees off vertical, and moved rather than turned.** Hung
straight down it lights the plate fully and a standing figure barely, so a character reads as a
silhouette on its own lit stage, and the meter makes it worse. Turning it alone drags the pool off
the plate, so the lamp sits offset and aims back at the centre, its intensity carrying the longer
throw.

## Audio, menus, saves

- **Audio** (`audio.h`): a beep on jump and spawn, footsteps, and a looping tone carried by the door
  as an `AUDIO_SOURCE` component, all procedural. `--mute` silences it.
- **Menus** (`ui.h`): a main menu, a pause menu, a settings screen and a non-modal HUD, all closed
  at startup. Escape opens the pause menu, and quitting is an item in it. `--ui-screen <name>`
  opens one at startup, since a headless run has no Escape to press, and `--ui-focus <n>` moves the
  focus through the real navigation path.
- **Saves** (`save.h`): F5 and F9 quicksave and quickload. They are deliberately not `ui` actions,
  so a menu suppresses them and a save never holds the menu's own state. The file goes beside the
  settings in the per-user directory; `CETRA_SETTINGS_DIR` moves both somewhere hermetic, which
  every gate and golden bake sets.
- **Display modes**: `--fullscreen`, `--borderless` and `--monitor <name>`, each at the monitor's
  current video mode, and the same three on the settings screen.

## The probes the gates read

Each prints rows of numbers and exits; most need no frame drawn, and several no engine.

| Flag | Gate group | What it drives |
|------|------------|----------------|
| `--pad-script`, `--trace-player` | `gamepad` | a scripted pad on slot 0, and the player's pose, move and jump every 30 steps |
| `--audio-probe <case>` | `audio` | an offline mix with no device |
| `--anim-probe <case>` | `anim` | animators ticked at a fixed 1/60 |
| `--graph-probe <case>` | `graph` | the animation graph, mostly with no engine |
| `--ik-probe <case>` | `ik` | planting and locking, rig-only or on the ramp and steps |
| `--ragdoll-probe <case>` | `ragdoll` | a ragdoll in a stepped Jolt world |
| `--ui-probe <case>` | `ui` | layout, navigation and hit tests, drawing no frame |
| `--save-probe <case>` | `save` | a save's round trip and its failure rules |
| `--display-probe <case>` | `display` | what a window mode decides, never applied |
| `--cam-probe <case>` | `camera` | the camera rig |
| `--gait-probe <case>` | `gait` | the gait over made-up moves |
| `--character-probe <case>` | `character` | real controllers at a beam and on open floor |

`--trace-player` appends its `anim` and `graph` tails after the columns the `gamepad` group reads;
new columns go at the end, never between. `--ui-bench` prints the UI's timings and means something
only from a release build; no gate reads it, since timing does not travel.
