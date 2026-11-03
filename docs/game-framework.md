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
