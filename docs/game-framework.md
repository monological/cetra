# Game framework and particles

The optional game layer under `cetra/src/game/`, and the particle system.

## Game Framework

`cetra/src/game/` layers a classic fixed-timestep game loop over the Engine, with an
optional Jolt physics world and an ECS-lite entity system.

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

Each fixed step, in order: sync kinematic bodies -> user `on_update` ->
**`update_all_brains`** (spec 13.17: after the app has moved the world, so a brain decides on
this step's state, and before physics, so what it decides moves this step) ->
**`scene_update_particle_systems`** -> character controllers ->
`physics_world_update` (4 collision sub-steps) -> collision events -> sync physics ->
sync character controllers to entities -> sync transforms. Physics and entities are optional (the `spores` app uses the loop for
particles only; `gametest` exercises the full stack). Physics is **Jolt** (rigid bodies,
raycasts/sweeps, fixed/distance/hinge/slider/6DOF constraints with motors, and a
`CharacterVirtual` controller).

**Input is polled once a frame, BEFORE the fixed steps** (`game->input`, spec 11.109), so a
press or release is an edge per FRAME: a frame that runs two fixed steps hands both the
same press, and a frame that runs none drops it. A game reads ACTIONS rather than key codes
-- a table of `InputAction` rows, each a name and up to six sources (a key, a mouse button,
a pad button, or a pad axis, each with a scale), bound with `input_bind` and read with
`input_action_value` / `_down` / `_pressed` / `_released`, or two at once as a ground-plane
move with `input_action_move`. Every action is one float in -1..1 from whichever source is
largest in magnitude, so a stick and a key pair are the same action and not two code paths.
The keyboard, mouse and per-slot pad queries are still there for a game that wants a
specific device. **Gamepads come through GLFW's standard layout** (any controller the
bundled SDL database knows reads as fifteen buttons and six axes) with a radial stick dead
zone and a trigger dead zone, both rescaled; hot-plug is logged by name; a slot that
disconnects releases everything it held, and a button already down when a pad appears is not
a press. **A pad is read through a seam** (`GamepadReadFn`): the default asks GLFW, and
`input_set_pad_script` replays a text script on slot 0 instead, which is how the whole layer
above the seam is verified with no controller present -- the `gamepad` gate group is five
such scripts through `apps/gametest`. GLFW's real device path is what the seam sits on, and
nothing in the suite exercises it; `docs/verification.md` carries the two recipes for
closing that. A controller released after the build is picked up through
`input_load_gamepad_mappings` (an SDL mapping file, any time after the engine exists).
**The wheel is the engine's**: its scroll callback accumulates `engine->input.scroll_dx/dy`
after the GUI gate and zeroes them before each end-of-frame poll, so any frame's update hook
reads one frame's delta; the game layer's `input_scroll` is that, read at the poll.

**Audio is a Game subsystem like the physics world** (`game->audio`, spec 12.0): one
output device wrapping miniaudio's high-level engine, built with `create_audio_system`,
installed with `game_set_audio_system`, and freed by `free_game` AFTER the entity manager --
because `AUDIO_SOURCE` components hold sounds that live in the engine, so the components must
tear down while it is still alive (the physics ordering). A game plays 2D one-shots and
music, or holds a `Sound` from a file or a procedural tone and places it in the world; the
listener is the camera, pointed each frame from `game_pre_render` (after the app has posed
it, and with the camera rig's pose put on it first, since spec 13.17 -- the engine applies the
rig only after that hook, so the listener used to hear from last frame's camera), and a
positional source is an `AUDIO_SOURCE` component synced from its entity in the same pass.
**Since 13.17 a source's sound sits at an offset in the entity's frame**, and
**fire-and-forget voices come from a 32-voice pool** (`audio_play_voice`): copies of a
decoded sound that may ride an entity, held by ID and never by pointer, reaped on the main
thread, taking a free or finished voice before stealing the oldest -- which a headless run
needs, since one that pulls no PCM never reaches a voice's end. A streamed or generated sound
is refused rather than copied: there is nothing decoded to copy. `audio_play_oneshot` is still
miniaudio's own fire-and-forget, an ASYNC load an offline render would race; nothing calls it,
and a game that wants a deterministic one-shot plays a voice. **The device is a seam matching the gamepad reader's**: a
windowed run opens the OS device and mixes on miniaudio's own thread; a headless run opens
NONE (`noDevice`) and renders offline through `audio_system_read_pcm`, so the whole layer above
the device is a deterministic function of the frames pulled -- which is what the `audio` gate
group asserts on (onset, pan, distance falloff, bus routing, file decode, noise, and since
13.33 a zone's link, the best path, the first update and the fade) with procedural sources and
no committed audio. **Zones are the audio's walls** (spec 13.33; see `audio.c/h` in Key Files):
the app adds rooms as boxes and links between them, and never multiplies a wall into a sound it
places. GLFW's analogue here is the OS device path; nothing in the suite exercises
it, and `docs/verification.md` says which OS it has been heard on (none, at writing).

**The UI is NOT a Game subsystem** (`cetra/src/ui.h`, spec 12.2), and that asymmetry with audio
and physics is the thing to know about it. `create_ui_system` takes an `Engine`, draws as one of
its overlays (`ui_attach`), and takes its input as a plain struct of VALUES rather than a
`GameInputState` -- so a menu works with no game framework at all, and the dependency points
from the optional layer to the engine rather than backwards. What the framework adds is only
where the input pass goes: `game_set_frame_input` runs a hook immediately after `input_update()`
and BEFORE the fixed steps, which is the one window in the frame where a UI can take input
without losing one in each direction. A menu opened after the steps would let the same frame
also walk the character, and the frame that closed it would drop a step of real input.

**A menu takes input away from the game with one switch.** `input_set_suppressed` makes every
action whose `ui` flag is false read zero, so a game reads its own action table unchanged and no
call site needs guarding; the UI's own actions carry `ui = true` and keep reading, which is what
lets the key that opened a menu close it. An app raises and lowers it EVERY FRAME from
`ui_captures_input` rather than on edges, so a screen closed by any route -- a button, a
callback, a scene change -- hands input back. Note this is separate from the debug GUI's capture
check, which is applied per SOURCE inside the reader: folding the two together would have let an
ImGui text field silence a gamepad.

**Pausing is the app's, not the layer's.** A menu over a live world needs no support at all,
because pausing stops only the fixed step while the whole render path keeps running -- which is
why the scene behind a pause screen still draws. `apps/gametest` pauses on the EDGES of capture
rather than assigning every frame: assigning made it the only writer that mattered, and the P
key's own pause was put back a frame later.

**A save is not a Game subsystem either** (`cetra/src/game/save.h`, spec 12.3), and for the
same reason the UI is not: `create_save_system` BORROWS the game, and what gets written is what
an app registers. The engine's own half is the sim clock, the entities and their components;
everything else an app hands over, because an entity walk cannot reach a facing angle held in a
file static or the counter that names the next spawned thing -- and a save without those
restores a world whose player faces the wrong way and whose next crate collides with an
existing name. `save_register_table` takes a descriptor table over any struct, and a row may
carry a GET/SET pair instead of an offset, which is how state living inside Jolt or inside a
file static reaches the file at all.

**Two things about a load are worth knowing before reading it.** The first is the failure rule:
file-level problems refuse the file, record-level problems drop the record and count it in
`SaveLoadResult`. A save naming an entity a patched level no longer has is the ordinary
consequence of shipping an update, and refusing the whole file for it would make a released
game unpatchable -- so one unbuildable object never costs a player the rest. The second is that
the entity POSE has to be pushed back into the body rather than stored on the entity:
`entity->position` is a copy `sync_physics_to_entities` rewrites from Jolt every step, so a
restore that wrote only that would be overwritten before the next frame drew, and the load
would appear to have done nothing at all.

## Particle System

A general Niagara-style system: **System -> Emitter -> composable Modules
(spawn/init/update) -> pluggable Renderer**, over SoA pools, with the sim behind a vtable.
**TWO backends ship**: `create_cpu_particle_sim_backend`, and
`create_tf_particle_sim_backend` — a GPU backend where emission and lifecycle stay on the CPU
and the per-particle update runs through transform feedback. It is not hypothetical and it is not
the fallback: `apps/spores` and the `.cscn` particle path both select it. A GL 4.3 COMPUTE
backend behind the same seam would be a third. Particle
systems are **scene citizens**: a `SceneNode` borrows a `ParticleSystem*`, the `Scene`
owns it, and it is auto-ticked and auto-rendered (`render.c`) -- the app just builds and
attaches. **The tick has TWO homes, not one**: `game.c`'s fixed step when the app uses the Game
framework, and the Engine's own per-frame hook (`engine.c:2507-2516`) when it does not, since
with no framework there is nobody else to own the sim. `apps/render` never calls `game_run` and
its window dust still ticks.

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
node_set_particle_system(node, sys);             // node transform = emitter spawn frame
node_add_child(scene->root_node, node);
```

The emitter spawns in the node's world frame (transform-at-spawn, then world-space sim).
Ownership invariant: never free a `SceneNode` whose particle system is still registered
on the `Scene`.

## Modules

### `nav_graph.c/h`

Places and the links between them (spec 13.17): an authored graph, NOT a navmesh, so an agent goes only where someone put a place. Links are a LINE, STEPS (a flight, with its risers and goings and the line through the nosings) or an ARC (a jump, a parabola whose top is exactly `apex` over the higher end), each of an app-numbered KIND whose descriptor carries a cost scale, a drive and whether corners between such links are rounded. A* under a query of allowed kinds, ties broken by node index so a route is the same on every run, and a FOLLOWER driven by DISTANCE (metres, from root motion) or PROGRESS (0..1, from a jump's clock). Corners between smoothed links are a quadratic Bezier inside the place's radius, drawn from both links so the halves meet. A replan finishes the current link first -- half way up a flight or through a jump an agent does not turn round -- except part way along a level two-way walk, where it turns back when that is cheaper. `nav_graph_check` walks every link's PROFILE, not its chord, through an app ray probe. NO GL, NO PHYSICS, NO CLOCK and no game type

### `game.c/h`

Fixed-timestep loop, init/update/render/shutdown callbacks, ticks particles + physics. Two clocks: `sim_clock`, which moves in whole steps and is 0 on a frame that runs none -- what the engine animates from -- and `frame_clock` (spec 13.47), the sim time a frame is drawn at, which moves every frame: what a look, a fade or a screen eases by, or on a display faster than the step it moves in 60 Hz jumps

### `physics.c/h`

Jolt Physics: rigid bodies, raycasts, sweeps, 5 constraint types + motors. **Jolt's default `Trace` is `DummyTrace`, which is `JPH_ASSERT(false)`**, so any condition Jolt merely wants to REPORT takes a breakpoint instead. The one met so far is `SHAPE_MESH` over a large run of exactly coplanar triangles — the tree builder cannot split them, falls back to a random split, and traces. Spec 11.63's island hit it with a perfectly flat sea floor and fixed it at the geometry. **Two things this entry claimed for a spec cycle and got wrong, both narrowing it.** The trace is `JPH_IF_DEBUG`-wrapped (`AABBTreeBuilder.cpp:205`), so the death is **debug-build only** — a release build indexes the same degenerate soup silently and badly, which is the worse half. And `JPH::Trace` / `JPH::AssertFailed` are **assignable `JPH_EXPORT extern` pointers**, so a handler absolutely can be installed; what JoltC lacks is a C entry point for doing it, and a C++ TU has been precedented since `cluster_build.cpp`. Nothing installs one today, so the geometry fix is what is load-bearing — but it is the right place because flat collision geometry is bad regardless, not because it was the only place reachable. **What JoltC does not bind is a longer list than it looks, and spec 12.16 shortened it**: `Ragdoll/`, `Vehicle/` and `SoftBody/` are all vendored and compiled, and a grep for them across `JoltC/` returns a single enum tag. Ragdoll is now reached through `ragdoll_jolt.cpp`, and a character's `SetShape` with `RotatedTranslatedShape` through `character_jolt.cpp` (spec 13.48); vehicles and soft body are still unbound and fixable exactly the same way. **Before costing anything against "Jolt does not do that", check whether JoltC merely fails to expose it** — that distinction was months of imagined work on the roadmap

### `entity.c/h`, `component.h`

ECS-lite entities + components. `RIGID_BODY`, `CHARACTER`, `ANIMATOR` (12.1), `AUDIO_SOURCE` (12.0) and `BRAIN` (13.17, appended last so no existing type renumbers) are implemented; `MESH_RENDERER` is a bare enum line -- an entity's visual is its `node`

### `brain.c/h`

The BRAIN component (spec 13.17): a small utility AI. The game registers ACTIVITIES -- a score, begin/step/end, a cooldown, a fail cooldown and a minimum time -- and the brain keeps the best one running, ticked each fixed step after the app's update and before physics. Four things are decided here rather than in each game, because each fails looking like temperament rather than a bug. COMMITMENT: what runs is held at the score it was chosen with, or its live score if higher, since doing a thing spends the want for it and measured against its live score every activity would be dropped half done -- a forced `brain_start` is held as high as the best of the others. A challenger must beat that by `hysteresis`, or two slowly crossing scores trade places every re-score. Scores within `tie_band` of the best are DRAWN by the brain's own seeded xorshift32, so a run with one seed makes the same choices and registration order picks nothing. And a refused begin falls through to the next best in the same tick, on its fail cooldown -- never to the activity it was replacing, which has just been ended. No memory and no senses: what an entity knows is the game's, and the engine's half of sight is `physics_world_line_of_sight`, a ray that ignores the looker's own body and counts the target as seen. An activity must not read a per-frame EDGE -- a frame runs any number of steps. Teardown frees the brain only and never calls `end`, since components go after the app's shutdown. silent's cat is the first user, and registers its activities only once it is in the house: a brain with something to choose before then begins it, fails, and holds that against it

### `character.c/h`

Character controller on Jolt `CharacterVirtual`. **Its capsule's height changes while it stands** (spec 13.48, `character_controller_set_half_height`), which is a crouch: a shorter capsule is translated down inside the one the controller was made with, so its foot stays where it was and the controller's POSITION never moves for it -- every app reads its feet as the position less a constant, and a crouch that moved the position would have moved the feet with it and walked the gait down a step. Shrinking always fits; growing is checked as Jolt's own crouch sample checks it, at half again the penetration bodies rest at, and refused where it would not fit, so a caller asks again each step until there is room. Through `character_jolt.cpp`, the fourth one-TU C++ split, since JoltC binds neither `CharacterVirtual::SetShape` nor `RotatedTranslatedShape`; creation builds its capsule there too, so a controller's shape is one kind whatever its history. The casts between JoltC's handles and Jolt's objects, which every one of those C++ files takes, are stated once in `jolt_handles.hpp`. `gametest --character-probe` and the `character` gate group step real controllers at a beam and on open floor. **Only the capsule's lower sphere stands on anything** (spec 13.55, Jolt's supporting volume, set in `character_jolt.cpp` as Jolt's own sample sets it). Jolt's default lets any contact support, so a wall brushing the capsule's side read as steep ground: the character counted as supported in mid-air, `ExtendedUpdate` never stuck it to the floor it had walked off, and in silent, which took steep ground as standing and turned gravity off, a sprint down the cellar flight along its wall went straight out over the stairs and hung there. `character_controller_is_grounded` still answers yes on steep ground; a game that turns gravity off for standing asks the ground state for `CHAR_GROUND_ON_GROUND`, as silent's player does

### `animator_component.c/h`

The `ANIMATOR` component (spec 12.1): an `Animator` the entity owns, its pose bound to the entity's node, ticked once per RENDERED frame from `game_pre_render` with the sim clock's delta -- never per fixed step, because the tick begins with the prev-pose latch and two in one frame would lose a step's motion vector. A paused sim passes 0 and the rig holds, reading zero deformation velocity. **Since 12.20 the tick is two calls**: the state machine bound to an animator decides, then the animator plays. The order is settled here rather than in each app's hook and is load-bearing three ways -- it is the one cadence the animator's clocks agree with, it reads the FINISHED edge on the frame that produced it, and it sits after the fixed steps have drained root motion and before the next accumulation, so a switch costs at most one rendered frame of travel where a switch from an app's own fixed step costs however many steps that frame happened to run. A graph reaches this through the animator it bound to rather than a field of its own: that pointer already exists so a second graph can stand the first down, and a second copy of one relationship is a second place for the two to disagree

### `input.c/h`

The game layer's input, polled once a frame before the fixed steps: keys, mouse, up to four gamepads in GLFW's standard layout behind a reader seam (GLFW, or a scripted pad from a text file), and the action table a game reads instead of key codes (spec 11.109). Every device's state is one struct held twice, this frame's and the previous frame's, and every edge -- a key's, a pad button's, an action's -- is the two compared; a pad that appears has its previous state set to its current one, which is the whole connect rule and reaches an action for free. An action is evaluated on read, so there is no cache, no cap and no ordering to get wrong

### `settings.c/h`

What a PLAYER chose, persisted across runs (spec 12.2): the four bus volumes, window mode and vsync, as ONE descriptor table walked in both directions -- `config_snapshot.c`'s technique, because a writer and a reader maintained separately drift silently. It could not be rows in that file, whose owner enum has no Game and no AudioSystem. `settings_default_path` resolves the platform's own per-user location (`%APPDATA%`, `~/Library/Application Support`, `$XDG_CONFIG_HOME`), which nothing else in this engine does -- every other `fopen` writes relative to cwd; `CETRA_SETTINGS_DIR` overrides it, which is what keeps a gate and a golden bake hermetic. Input bindings are NOT carried: `input_bind` takes a borrowed const table, so persisting a rebinding wants the remapping UI it would exist for. **Window mode APPLIES since 12.15**, through `engine_set_window_mode`, and the file gained a monitor NAME to go with it -- an index renumbers when a display is unplugged and silently moves the game to another screen. The mode is `EngineWindowMode` itself, not a mirror of it: `settings.h` includes `engine.h`, which is legal and always was (everything under `game/` is public and `game.h` already did it), and a spec cycle was spent asserting otherwise and paying four static asserts to route around a wall that was not there. **Check an include boundary before conceding to it.** `borderless` was APPENDED to the mode labels rather than slotted between the existing two: the index is the value and the value is written as a name, so a label inserted in the middle re-points every file already on disk -- the one surviving assert holds the label count against `ENGINE_WINDOW_MODE_COUNT`. vsync goes through `engine_set_vsync` rather than `glfwSwapInterval` here, which was a second writer of a value the engine did not keep and therefore could not put back after a mode change dropped it

### `save.c/h`

Where the player WAS, persisted across runs (spec 12.3). The THIRD descriptor table after `config_snapshot.c` and `settings.c`, and neither could have carried it: that file's owner enum has no Entity, no RigidBody, no CharacterController and no Animator, and settings are what a player chose rather than what they did. Entities match by NAME (an id is `next_id++`, so creation order decides it and a flag changes that), components are keyed by name and never by `ComponentType`, whose `1u << type` renumbers on a removal. A row may carry a GET/SET pair instead of an offset, which is what reaches a body's velocity inside Jolt -- and the pose is pushed back INTO the body, since `entity->position` is a copy `sync_physics_to_entities` rewrites every step. **A restored world is deliberately NOT bit-exact**: Jolt's solver state stays out, or the format would pin to `JPH_VERSION_ID` and break every save on a physics upgrade. One rule orders the failures -- file-level problems refuse the file, record-level problems drop the record and count it in `SaveLoadResult`, so one unbuildable crate never costs a player the rest. A spawned entity carries the recipe that made it (`save_register_spawner`, `save_note_spawn`), and a registered name is permanent: retiring one leaves a tombstone. Versions are PER SECTION with a migration chain that runs on the parsed tree before the descriptor walk, so a migration written today still behaves in three years

### `audio.c/h`

The game layer's audio (spec 12.0): one output device wrapping miniaudio's high-level engine, wrapped as an `AudioSystem` the `Game` owns like the physics world. 2D fire-and-forget SFX and music, held voices from a file (WAV/MP3/FLAC), a procedural tone, or procedural noise in three colours (spec 13.9, a bed such as rain, from a fixed seed), mixer buses, and 3D positional sound whose listener is the camera and whose sources are `AUDIO_SOURCE` components synced from their entities -- at an offset in the entity's frame since 13.17 -- plus a 32-voice pool of fire-and-forget copies of a decoded sound (`audio_play_voice`), which may ride an entity held by id and are reaped on the main thread, the oldest stolen when the pool is full, each at a volume and, since 13.47, a pitch of its own -- the playback rate, so a footstep's takes need not repeat exactly. A held sound is re-pitched while it plays with `audio_sound_set_pitch` (spec 13.56), which is how one recording of a motor idling is its whole range of speeds. **Since 13.33 sound has walls: ZONES** -- named sets of boxes joined by LINKS, each a `through`, the share of a sound that crosses it, settable any time for a door. A placed sound (positioned by the app, an `AUDIO_SOURCE`, a voice) is heard along the BEST PATH from its zone to the listener's, the largest product of `through` along any chain of links, eased over `AUDIO_ZONE_FADE` (0.4 s, which is why `audio_system_update` takes a `dt`; `game_pre_render` passes the engine's RENDER clock's delta, so a game that holds its sim under a menu while its view still moves -- silent's title -- has its zones follow the view) and landed where it starts. **The first update lands every gain whatever its `dt`**, since until it the listener was nowhere, and `game_run` makes it before frame 1 from where init left the view -- an installed camera rig's pose put on the camera first, as the first frame would -- so a game starting indoors neither hears its rooms fade up nor reads the outdoors' answer in its first hook. A refused zone is `AUDIO_ZONE_NONE`, and a link to it is refused in turn rather than joining the outdoors in its place. **A graph and not nested boxes**, because a storey is the outdoors' neighbour and the storey under it at once, which nesting gets wrong. The gain is the node's OUTPUT-BUS volume, a stage `ma_sound_set_volume` never touches, so the app's volume and the zone's multiply with nothing cached. A sound never placed is heard whole -- every held sound, a file's included, is made 2D, and placing it is what spatializes it, so a sound in the world and a sound with a zone are one test -- and with no zone added every sound is; `audio_zone_heard` is the eased answer for a sound an app shapes itself. No low-pass yet: a zone turns sound down, it does not muffle it. **A game fades a bus with `audio_set_bus_fade`** (spec 13.53), the bus group's output-bus volume -- the zones' trick on a group -- which multiplies with the volume a player chose through `audio_set_bus_volume` and never writes it, so a menu muting the world never fights a settings screen setting it. **Loading is a PHASE of the system** (spec 13.39): between `audio_system_begin_loading` and `audio_system_end_loading`, every file decodes on miniaudio's job thread under one fence, `audio_sound_from_file` returns at once with a Sound set up and started as usual, and the DEVICE is stopped, so nothing mixes a half-decoded buffer. `game_run` ends a phase the game's init began before the first frame, drawing the loading screen while `audio_system_loading`. And `audio_system_read_pcm` waits on the fence too, so the offline mix is byte-identical to a synchronous load's by construction rather than by the caller's order. A load that fails in a phase -- a file not there, or not audio -- is refused and the phase still ends (spec 13.56): miniaudio runs that load's job inline, and on its failure released the fence in the job and again as though the job had never been posted, and the phase's end asserted. The vendored `miniaudio.h` carries that one fix, marked `cetra`, and `audio-load` holds it. A phase was chosen over an async variant of the load: as a side effect of the first such load, the device's stop silenced a game for good the first time it loaded one after the wait. The device is the seam: a windowed run opens the OS device, a headless run opens NONE and renders offline through `audio_system_read_pcm`, so everything above the device is deterministic and the `audio` gate group asserts on it with no hardware. miniaudio (single-header, vendored) carries its own CoreAudio/ALSA/WASAPI backends
