# Game framework and particles

The optional game layer under `cetra/src/game/`, and the particle system. The game layer wraps the
`Engine` in a fixed-timestep loop and adds Jolt physics, ECS-lite entities, characters, a small
utility AI, input, audio, player settings and saves. Each is optional: `apps/spores` uses the loop
for particles alone, and `apps/gametest` exercises the whole stack.

## The loop

```c
GameConfig config = {.engine = {.title = "My Game", .headless = headless}}; // zero = default
Game* game = create_game(&config); // creates and initialises the engine
game->engine->exit_after_frames = frames; // the run's settings go on the engine
game_set_scene(game, scene);
game_set_init(game, on_init);      // + on_update / on_pre_render / on_shutdown; on_render
                                   // only for what the app does AROUND the draw, since the
                                   // loop draws the scene itself when none is set
game_run(game);                    // fixed-timestep loop
free_game(game);
```

Each frame, `game.c` polls input, runs the frame-input hook (see [The UI and the
game](#the-ui-and-the-game)), then runs as many fixed steps as the accumulator holds. Each fixed
step, in order:

1. sync kinematic bodies from their entities;
2. the app's `on_update`;
3. `update_all_brains`: after the app has moved the world, so a brain decides on this step's
   state, and before physics, so what it decides moves this step;
4. `scene_update_particle_systems`;
5. character controllers;
6. `physics_world_update` (4 collision sub-steps), collision events, physics synced back to
   entities;
7. character controllers synced to entities, then entity transforms to scene nodes.

Then, once per rendered frame, `game_pre_render` runs the app's `on_pre_render`, ticks the
animators and points the audio listener.

**Two clocks.** `sim_clock` moves in whole steps and is 0 on a frame that runs none; it is what the
engine animates from. `frame_clock` (spec 13.47) is the sim time a frame is drawn at and moves
every frame; a look, a fade or a screen eases by it, since on a display faster than the step the
sim clock moves in 60 Hz jumps.

**Pausing is the app's.** `game->paused` stops only the fixed step; the whole render path keeps
running, which is why the scene behind a pause screen still draws, and a menu over a live world
needs no support at all.

## Physics

`physics.c` is Jolt: rigid bodies, raycasts, sweeps, five constraint types (fixed, distance, hinge,
slider, 6DOF) with motors, and the `CharacterVirtual` controller below.

- **Jolt's default `Trace` is `DummyTrace`, which is `JPH_ASSERT(false)`**, so a condition Jolt
  only wants to report takes a breakpoint. The one met so far is `SHAPE_MESH` over a large run of
  exactly coplanar triangles: the tree builder cannot split them, falls back to a random split and
  traces (`AABBTreeBuilder.cpp`). The trace is `JPH_IF_DEBUG`, so a debug build stops, and a
  release build indexes the same degenerate soup silently and badly. Avoid flat collision
  geometry: it is bad regardless. A handler could be installed, since `JPH::Trace` and
  `JPH::AssertFailed` are assignable pointers that JoltC gives no C entry point to; nothing
  installs one.
- **Before costing anything against "Jolt does not do that", check whether JoltC merely fails to
  expose it.** `Ragdoll/`, `Vehicle/` and `SoftBody/` are all vendored and compiled, and JoltC binds
  none of them. A one-TU C++ file reaches what it lacks: `ragdoll_jolt.cpp` the ragdoll,
  `character_jolt.cpp` a character's `SetShape` with `RotatedTranslatedShape`, `physics_cook.cpp`
  shape serialization. Vehicles and soft bodies are unbound and reachable the same way. The casts
  between JoltC's handles and Jolt's objects are stated once, in `jolt_handles.hpp`.

## Entities and components

`entity.c/h` and `component.h` are ECS-lite. `RIGID_BODY`, `CHARACTER`, `ANIMATOR`,
`AUDIO_SOURCE` and `BRAIN` are implemented; a new type is appended last, so no existing type
renumbers. `MESH_RENDERER` is a bare enum line: an entity's visual is its `node`.

## Characters

`character.c` is a character controller on Jolt's `CharacterVirtual`, created and reshaped through
`character_jolt.cpp`, so a controller's shape is one kind whatever its history.

- **Its capsule's height changes while it stands** (`character_controller_set_half_height`, spec
  13.48), which is a crouch. The shorter capsule is translated down inside the one the controller
  was made with, so the foot and the controller's position never move: apps read the feet as the
  position less a constant, and a crouch that moved the position would walk the gait down a step.
  Shrinking always fits. Growing is checked as Jolt's own crouch sample checks it, at half again the
  penetration bodies rest at, and refused where it would not fit, so a caller asks again each step
  until there is room.
- **Only the capsule's lower sphere stands on anything** (spec 13.55), Jolt's supporting volume, set
  as Jolt's own sample sets it. With Jolt's default any contact supports, so a wall brushing the
  capsule's side reads as steep ground, the character counts as supported in mid-air, and
  `ExtendedUpdate` never sticks it to the floor it walked off.
- **`character_controller_is_grounded` answers yes on steep ground.** A game that turns gravity off
  while standing asks the ground state for `CHAR_GROUND_ON_GROUND`, as silent's player does.

`gametest --character-probe` and the `character` gate group step real controllers at a beam and on
open floor.

## Brains

`brain.c` is the `BRAIN` component (spec 13.17), a small utility AI. The game registers activities,
each with a score, begin/step/end, a cooldown, a fail cooldown and a minimum time, and the brain
keeps the best one running, ticked each fixed step after the app's update and before physics.

Four things are decided here rather than in each game, because each fails looking like temperament
rather than a bug:

- **Commitment.** What runs is held at the score it was chosen with, or its live score if higher:
  doing a thing spends the want for it, and measured against its live score every activity would
  be dropped half done. A forced `brain_start` is held as high as the best of the others.
- **Hysteresis.** A challenger must beat that by `hysteresis`, or two slowly crossing scores trade
  places at every re-score.
- **Ties are drawn.** Scores within `tie_band` of the best are drawn by the brain's own seeded
  xorshift32, so one seed makes the same choices and registration order picks nothing.
- **A refused begin falls through** to the next best in the same tick, on its fail cooldown, never
  to the activity it was replacing, which has just been ended.

A brain has no memory and no senses: what an entity knows is the game's. The engine's half of sight
is `physics_world_line_of_sight`, a ray that ignores the looker's own body and counts the target as
seen. An activity must not read a per-frame edge, since a frame runs any number of steps. Teardown
frees the brain and never calls `end`, since components go after the app's shutdown. Register
activities only once the entity can do them: a brain with something to choose begins it, and a
begin that fails holds the fail cooldown against it (silent's cat registers its activities once it
is in the house).

## Navigation

`nav_graph.c` (in `cetra/src/`, not `game/`; spec 13.17) is places and the links between them: an
authored graph, not a navmesh, so an agent goes only where someone put a place. It has no GL, no
physics, no clock and no game type.

- **Links** are a line, steps (a flight, with its risers and goings and the line through the
  nosings) or an arc (a jump, a parabola whose top is exactly `apex` over the higher end). Each has
  an app-numbered kind whose descriptor carries a cost scale, a drive and whether corners between
  such links are rounded.
- **Routes** are A* under a query of allowed kinds, ties broken by node index so a route is the
  same on every run.
- **A follower** is driven by distance (metres, from root motion) or progress (0..1, from a jump's
  clock). Corners between smoothed links are a quadratic Bezier inside the place's radius, drawn
  from both links so the halves meet.
- **A replan finishes the current link first**, so an agent half way up a flight or through a jump
  does not turn round, except part way along a level two-way walk, where it turns back when that is
  cheaper.
- **`nav_graph_check` walks every link's profile**, not its chord, through an app ray probe.

## Animation on an entity

`animator_component.c` is the `ANIMATOR` component: an `Animator` the entity owns, its pose bound to
the entity's node, ticked once per rendered frame from `game_pre_render` with the sim clock's delta.
Never per fixed step: the tick begins with the prev-pose latch, and two in one frame lose a step's
motion vector. A paused sim passes 0 and the rig holds, with zero deformation velocity.

The tick is two calls: the state machine bound to the animator (`anim_graph.h`) decides, then the
animator plays. The order is settled here rather than in each app's hook, for three reasons: it is
the one cadence the animator's clocks agree with; it reads the finished edge on the frame that
produced it; and it sits after the fixed steps have drained root motion and before the next
accumulation, so a switch costs at most one rendered frame of travel. A graph reaches the tick
through the animator it bound to rather than a field of its own: that pointer already exists, and a
second copy of one relationship is a second place for the two to disagree.

## Input

`input.c` (`game->input`, spec 11.109) is polled once a frame, before the fixed steps: keys, the
mouse, and up to four gamepads.

- **An edge is per frame.** Every device's state is one struct held twice, this frame's and the
  last, and every edge (a key's, a pad button's, an action's) is the two compared. A frame that runs
  two fixed steps hands both the same press, and a frame that runs none drops it.
- **A game reads actions, not key codes.** A table of `InputAction` rows, each a name and up to six
  sources (a key, a mouse button, a pad button or a pad axis, each with a scale), bound with
  `input_bind` and read with `input_action_value` / `_down` / `_pressed` / `_released`, or two at
  once as a ground-plane move with `input_action_move`. An action is one float in -1..1 from
  whichever source is largest in magnitude, so a stick and a key pair are one action, not two code
  paths. It is evaluated on read, so there is no cache, no cap and no ordering to get wrong. The
  per-device queries remain for a game that wants a specific device.
- **Gamepads come through GLFW's standard layout**: any controller the bundled SDL database knows
  reads as fifteen buttons and six axes, with a radial stick dead zone and a trigger dead zone, both
  rescaled. Hot-plug is logged by name. A pad that appears has its previous state set to its
  current one, so a button already down is not a press; a slot that disconnects releases
  everything it held. A controller newer than the build is picked up with
  `input_load_gamepad_mappings` (an SDL mapping file, any time after the engine exists).
- **A pad is read through a seam** (`GamepadReadFn`). The default asks GLFW;
  `input_set_pad_script` replays a text script on slot 0, which is how the `gamepad` gate group
  verifies the layer above the seam with no controller, playing scripts through `apps/gametest`. Nothing in the suite exercises GLFW's real
  device path; `docs/verification.md`, The gamepad device path, has the recipes.
- **The wheel is the engine's.** Its scroll callback accumulates `engine->input.scroll_dx/dy` after
  the GUI gate and zeroes them before each end-of-frame poll, so any frame's update hook reads one
  frame's delta; `input_scroll` is that, read at the poll.

## Audio

`audio.c` (`game->audio`, spec 12.0) is one output device over miniaudio's high-level engine, an
`AudioSystem` the game owns as it owns the physics world. Build it with `create_audio_system`,
install it with `game_set_audio_system`; `free_game` frees it after the entity manager, since
`AUDIO_SOURCE` components hold sounds that live in it.

**What it plays.** 2D one-shots and music; held sounds from a file (WAV, MP3, FLAC), a procedural
tone, or procedural noise in three colours from a fixed seed (a bed such as rain); mixer buses; and
3D positional sound. The listener is the camera, pointed each frame from `game_pre_render` after
the app has posed it, with the camera rig's pose put on it first, since the engine applies the rig
only after that hook. A positional source is an `AUDIO_SOURCE` component synced from its entity in
the same pass, at an offset in the entity's frame. A held sound is re-pitched while it plays with
`audio_sound_set_pitch`, which is how one recording of an idling motor covers its range of speeds.

**Voices.** `audio_play_voice` plays fire-and-forget copies of a decoded sound from a pool of 32,
each at its own volume and pitch, optionally riding an entity. A voice is held by id, never by
pointer, and reaped on the main thread; a new one takes a free or finished voice before stealing
the oldest, which a headless run needs, since one that pulls no PCM never reaches a voice's end. A
streamed or generated sound is refused, having nothing decoded to copy. `audio_play_oneshot` is
miniaudio's own fire-and-forget, an async load an offline render would race; nothing calls it, and
a deterministic one-shot is a voice.

**Zones are the audio's walls** (spec 13.33). The app adds named sets of boxes and links between
them, each link a `through`, the share of sound that crosses it, settable at any time for a door.
The app never multiplies a wall into a sound it places.

- A placed sound (positioned by the app, an `AUDIO_SOURCE`, a voice) is heard along the best path
  from its zone to the listener's: the largest product of `through` along any chain of links.
- It is a graph, not nested boxes, because a storey is the outdoors' neighbour and the storey
  under it at once, which nesting gets wrong.
- Gains ease over `AUDIO_ZONE_FADE` (0.4 s), so `audio_system_update` takes a `dt`.
  `game_pre_render` passes the engine's render-clock delta, so a game that holds its sim under a
  menu while its view moves has its zones follow the view.
- **The first update lands every gain whatever its `dt`**, since until then the listener was
  nowhere. `game_run` makes it before frame 1 from where init left the view, the rig's pose put on
  the camera first, so a game starting indoors neither hears its rooms fade up nor reads the
  outdoors' answer in its first hook.
- The gain is the node's output-bus volume, a stage `ma_sound_set_volume` never touches, so the
  app's volume and the zone's multiply with nothing cached.
- Every held sound, a file's included, is made 2D, and placing it is what spatializes it. A sound
  never placed is heard whole, and with no zone added every sound is.
- A refused zone is `AUDIO_ZONE_NONE`, and a link to it is refused in turn rather than joining the
  outdoors in its place. `audio_zone_heard` is the eased answer, for a sound an app shapes itself.
- There is no low-pass: a zone turns sound down, it does not muffle it.

**A game fades a bus with `audio_set_bus_fade`** (spec 13.53), the bus group's output-bus volume,
which multiplies with the player's `audio_set_bus_volume` and never writes it, so a menu muting the
world never fights a settings screen setting it.

**Loading is a phase** (spec 13.39). Between `audio_system_begin_loading` and
`audio_system_end_loading`, every file decodes on miniaudio's job thread under one fence;
`audio_sound_from_file` returns at once with a Sound set up and started as usual; and the device is
stopped, so nothing mixes a half-decoded buffer. `game_run` ends a phase the game's init began
before the first frame, drawing the loading screen while `audio_system_loading`.
`audio_system_read_pcm` waits on the fence too, so an offline mix is byte-identical to a synchronous
load's whatever the caller's order. A phase was chosen over an async variant of the load because
stopping the device for the first such load silenced a game for good when it loaded one after the
wait. A load that fails in a phase (a missing file, or not audio) is refused and the phase still
ends: the vendored `miniaudio.h` carries one fix, marked `cetra`, for a failed job releasing the
fence twice, and `audio-load` holds it.

**The device is a seam**, as the gamepad reader is. A windowed run opens the OS device and mixes on
miniaudio's own thread, through its CoreAudio, ALSA or WASAPI backend. A headless run opens none
(`noDevice`) and renders offline through `audio_system_read_pcm`, so everything above the device is
a deterministic function of the frames pulled. The `audio` gate group asserts on that with
procedural sources and no committed audio. Nothing in the suite exercises the OS device path;
`docs/verification.md`, The audio device path, records which platforms have been heard.

## The UI and the game

The UI (`cetra/src/ui.h`, spec 12.2; see `docs/ui.md`) is not a game subsystem. `create_ui_system`
takes an `Engine`, draws as one of its overlays (`ui_attach`), and takes its input as a plain
struct of values rather than a `GameInputState`, so a menu works with no game framework and the
dependency points from the optional layer to the engine. The game layer adds two things:

- **Where the input pass goes.** `game_set_frame_input` runs a hook right after `input_update()`
  and before the fixed steps, the one window in the frame where a UI can take input without losing
  one in either direction. A menu opened after the steps would let the same frame also walk the
  character, and the frame that closed it would drop a step of real input.
- **One switch takes input from the game.** `input_set_suppressed` makes every action whose `ui`
  flag is false read zero, so a game reads its own action table unchanged and no call site needs
  guarding; the UI's own actions carry `ui = true` and keep reading, which lets the key that opened
  a menu close it. Raise and lower it every frame from `ui_captures_input`, not on edges, so a
  screen closed by any route (a button, a callback, a scene change) hands input back. It is separate
  from the debug GUI's capture check, which is applied per source inside the reader: folded
  together, an ImGui text field would silence a gamepad.

A game that pauses under a menu sets `paused` on the edges of capture, not every frame: assigned
every frame it is the only writer that matters, and any other pause (gametest's P key) is put back
a frame later.

## Settings

`settings.c` (spec 12.2) is what a player chose, persisted across runs: the four bus volumes,
window mode, monitor and vsync.

- **One descriptor table, walked in both directions**, `config_snapshot.c`'s technique, because a
  writer and a reader kept separately drift silently. It cannot be rows in that file, whose owner
  enum has no Game and no AudioSystem.
- **`settings_default_path` resolves the platform's per-user location** (`%APPDATA%`,
  `~/Library/Application Support`, `$XDG_CONFIG_HOME`); every other `fopen` in the engine is
  relative to the working directory. `CETRA_SETTINGS_DIR` overrides it, which keeps a gate and a
  golden bake hermetic.
- **Window mode applies** through `engine_set_window_mode`, with the monitor stored by name: an
  index renumbers when a display is unplugged and silently moves the game to another screen. vsync
  goes through `engine_set_vsync`, never `glfwSwapInterval`, so the engine keeps the value and can
  put it back after a mode change drops it.
- **The mode is stored as `EngineWindowMode` itself**, written as its label. A new label is
  appended, never inserted, or every file already on disk re-points; a static assert holds the label
  count to `ENGINE_WINDOW_MODE_COUNT`. `settings.h` includes `engine.h`, which is legal: everything
  under `game/` is public. Check an include boundary before routing around it.
- **Input bindings are not carried**: `input_bind` takes a borrowed const table, so persisting a
  rebinding waits for the remapping UI it would exist for.

## Saves

`save.c` (`cetra/src/game/save.h`, spec 12.3) is where the player was, persisted across runs. Like
the UI it is not a game subsystem: `create_save_system` borrows the game, and what gets written is
what an app registers. It is a third descriptor table beside `config_snapshot.c` and `settings.c`,
neither of which could carry it: the snapshot's owner enum has no Entity, RigidBody,
CharacterController or Animator, and settings are what a player chose rather than what they did.

- **The engine's half** is the sim clock, the entities and their components. Everything else the
  app hands over with `save_register_table`, a descriptor table over any struct, because an entity
  walk cannot reach a facing angle in a file static or the counter that names the next spawned
  thing; without them the player faces the wrong way and the next crate collides with an existing
  name.
- **A row may carry a GET/SET pair instead of an offset**, which is how state inside Jolt (a body's
  velocity) or a file static reaches the file.
- **Entities match by name**, never id: an id is `next_id++`, so creation order decides it and a
  flag changes it. Components are keyed by name, never by `ComponentType`, whose bit renumbers on a
  removal.
- **A spawned entity carries the recipe that made it** (`save_register_spawner`,
  `save_note_spawn`). A registered name is permanent: retiring one leaves a tombstone.
- **Versions are per section**, with a migration chain that runs on the parsed tree before the
  descriptor walk, so a migration written today still behaves years later.
- **The failure rule:** file-level problems refuse the file; record-level problems drop the record
  and count it in `SaveLoadResult`. A save naming an entity a patched level no longer has is the
  ordinary result of shipping an update, and refusing the whole file would make a released game
  unpatchable.
- **The pose is pushed back into the body.** `entity->position` is a copy
  `sync_physics_to_entities` rewrites from Jolt every step, so a restore that wrote only that would
  be overwritten before the next frame drew, and the load would appear to do nothing.
- **A restored world is not bit-exact, on purpose.** Jolt's solver state stays out, or the format
  would pin to `JPH_VERSION_ID` and break every save on a physics upgrade.

## Particle System

A general Niagara-style system: **system -> emitter -> composable modules (spawn, init, update) ->
pluggable renderer**, over SoA pools with swap-remove compaction, with the sim behind a vtable.

**Two backends ship.** `create_cpu_particle_sim_backend`, and `create_tf_particle_sim_backend`, a
GPU backend where emission and lifecycle stay on the CPU and the per-particle update runs through
transform feedback. The second is not a fallback: `apps/spores` and the `.cscn` particle path both
use it. A GL 4.3 compute backend would be a third behind the same seam.

**Particle systems are scene citizens.** A `SceneNode` borrows a `ParticleSystem*`, the `Scene`
owns it, and it is ticked and drawn (`render.c`) without the app's help. The tick has two homes:
the game's fixed step when the app runs the game framework, and the engine's own per-frame update
when it does not, since then nobody else owns the sim (`apps/render` never calls `game_run`, and its
window dust still ticks).

```c
ParticleSystem* sys = create_particle_system("spores");
particle_system_set_backend(sys, create_cpu_particle_sim_backend());

ParticleEmitter* em = create_particle_emitter("spore", 20000);
particle_emitter_set_renderer(em, create_billboard_particle_renderer(particle_prog));
particle_emitter_add_module(em, particle_module_spawn_rate(2000.0f));
particle_emitter_add_module(em, particle_module_init_box_location(min, max));
// ... init_lifetime / init_size / init_color, update_curl_noise / drift / integrate
particle_system_add_emitter(sys, em);

scene_add_particle_system(scene, sys);        // scene owns it (ticked + drawn)
SceneNode* node = create_node();
node_set_particle_system(node, sys);          // node transform = emitter spawn frame
node_add_child(scene->root_node, node);
```

The emitter spawns in the node's world frame, then simulates in world space. Never free a
`SceneNode` whose particle system is still registered on the `Scene`.
