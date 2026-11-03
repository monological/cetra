# silent

A first-person walk through a foggy, rainy night town. The player starts in the back corner of the
kitchen of their own clapboard house, which is locked from the inside; the key is in a birdcage in
the basement, and taking it begins a haunting. Outside are a street of neighbours' houses, a Gothic
mansion up a hill at the street's east end with a black cat living in it, a crossroads and a chasm
at its west end, and through a gap in a barricade a track down to a lake with a log cabin and a
motorboat.

It is the engine's largest user. It has no gate arms and no goldens, by decision: it is checked by
running it and looking.

## Running it

```bash
./out/bin/silent                         # the title, then a game
./out/bin/silent --new-game              # straight into a new game
./out/bin/silent --day --no-rain         # an overcast day, dry
./out/bin/silent --player-at -78,118,90  # start anywhere: here, the dock looking at the cabin
./out/bin/silent -x -f 120 -W 960 -H 540 -S /tmp/kitchen.ppm \
    --cam-eye 2.2,1.95,13.55 --cam-target 2.2,1.25,10.1
```

| Action | Keys | Pad |
|--------|------|-----|
| Walk, look | WASD, the mouse or the arrows | left stick, right stick |
| Hurry | Shift | left bumper |
| Crouch (toggle), crouch while held | C, Ctrl | B |
| Flashlight, once found | F | Y |
| A door, or what is in reach | E | X |
| Put down what is held | Q or a right click | B |
| Backpack | Tab | Back |
| Town map, once had | M | right bumper |
| Begin or end the haunting | K | right-stick click |
| Pause menu | Escape | Start |
| Debug GUI | G or backtick | |

Every flag, with pinned camera views for each part of the town, is in `docs/cli-reference.md`,
"apps/silent". A run started from a state on the command line -- a headless run, `--player-at`, a
pinned camera, `--backpack`, `--story`, and so on -- skips the title and saves nowhere unless
`--slot` says where. `--pad-script` and `--trace-player` play it headless, as in gametest.

## What it exercises in the engine

- The game layer: the fixed-step loop, the character controller and its crouch, the gait, input,
  saves and settings, and the audio system with zones, voices, re-pitching and the loading phase.
- Animation and AI: the cat's animator, root motion, override layer, look-at, nav graph and brain,
  and the player's arms on a camera rig of their own.
- The lighting: cached point- and area-light shadows (candles, lamps, the kitchen's tubes), GI
  volumes and reflection probes streamed by distance and cooked to disk, derived emissive panels,
  IES profiles, the capture budget and capture rest, and the loading screen.
- The post chain: one-sample jittered TAA headless, TAAU at 0.5, volumetric fog with local holes,
  an overcast photometric sky, local exposure, a LUT grade, the CRT, and app post passes.
- Rain, wind, wet surfaces and drips; bounded water with dry shapes; the fire flipbook.
- App shaders: a surface hook and a late-draw surface on the television, a lampshade hook, app UI
  fragment stages for the map's ink and the save candlestick (`shaders/candles_frag.glsl`), and a
  post pass for the haunting's grade.
- The UI: the title, menus, the HUD, the backpack's grid and the map, with items drawn by an
  in-overlay turntable.

## How the world is built

### The kit

The buildings and the ground are built by `kit.c` from flat-shaded boxes and prisms batched per
material (the trees are `tree_gen`'s, and the cat, arms, cage, lamp and boat are models), with
world-metre planar UVs so texel density is even everywhere, under CC0 photo scans cut to 256 px
(`tools/fetch_textures.py`, which says where each came from). On top of that:
- **Smooth solids** (`kit_frame_lathe`, `kit_frame_pipe`, a normal per vertex) for the few round,
  polished things: pots, the tap, jars, plates, mugs. Everything else stays faceted.
- **A soft box** (`kit_frame_soft_box`, edges rounded and faces puffed) for the bed and the bag,
  since lathes and pipes only make round or square things.
- **Cards** (`kit_frame_card`, explicit UVs) for photos, notes, letters, carvings, rugs, stained
  glass and book spines, each cut from one atlas a tool builds.
- **A Gothic vocabulary**: ear-clipped polygons and extrusions, pointed and Tudor arches, arched
  openings with their spandrels, walls at any yaw, and stairs.
- **Ramps for flights** (`kit_frame_ramp`): every flight collides as a ramp along its nosings. On
  its treads the capsule wedged against a riser, and the eye rode each tread in lumps.
- **Surfaces** (`KitSurfaces`): the kit notes the material of every body it makes, which is what
  footsteps are heard on.

**Kits beside each other.** The mansion's kit sits beside the street's (`kit_init_beside`) and
shares its materials, so each building has its own meshes and culls on its own; `kit_free_unused`
frees a shared material no kit used. A small kit made beside another and finished on its own --
the hall's pendant, the dock lamp, the boat -- has its own meshes, so it can move as a whole, and
`kit_keep_beside` counts what it used as used by the first, so `kit_free_unused` keeps its
materials. `Kit.casts_nothing` keeps a swinging kit, such as the pendant or the basement bulb, out of
the kept shadows, where it would be a mover every frame. The ground and the chasm's face are
a kit of their own with shadow cells six times the size (`Kit.shadow_cell_scale`): in 3 m cells the
ground made thousands of shadow draws of a few triangles each.

**A kit has an origin.** It is applied where vertices, colliders, drip lines and wicks are emitted,
and after the planar UVs, so the mansion's files (`house.c`, `tower.c`, `interior.c`, `hearth.c`,
`study.c`, `mansion.c`) build in house-local coordinates and every texture lands the same wherever
the house stands. `mansion_at` is the one translation for what is placed outside the kit: the study
lamp, the GI corner, the probe boxes, the cat's places, the sound boxes, the fog hole.

### The plan

`layout.h` is the plan: the street, both houses, the mansion's place (`MANSION_X`, `MANSION_Y`,
`MANSION_Z`), the hill, the woods' edges, the ridge, the lake and the cabin. The home's ground
storey is half a metre taller than the plan the mansion keeps: its `HOME_*` heights are the plan's
plus `HOME_RAISE`, and every home use takes them. `FRONT_DOOR_Z` is stated once there for the door
and the hall's probe box. `house.c`'s `WALLS` table states every wall's openings once, and
`interior.c` dresses them from it. `tools/make_cards.py` restates `LAKE_*` and `LAKE_MARK_*` for
the cabin's lake map, so change both together.

### Generated files

Never edit these headers; rerun the tool that writes them.

| Header | Tool | Holds |
|--------|------|-------|
| `src/cards.h` | `tools/make_cards.py` | each card's UVs and size in the cards picture, and the hall clock's arbor anchors |
| `src/gothic.h` | `tools/make_gothic.py` | the Gothic atlas's cards: carvings, rugs, stained glass, spines, arms, fireback |
| `src/map_art.h` | `tools/make_map.py` | the town map's `MapArt` rows |
| `src/cat_clips.h` | `tools/make_cat_blender.py` | the cat's clips: length, loop, stated travel, events, eye anchors |
| `src/arms_clips.h` | `tools/make_arms_blender.py` | the player's arms' clips and events |
| `src/boat_shape.h` | `tools/make_boat_blender.py` | the hull's draft, its dry shapes and its rim |
| `src/door_takes.h` | `tools/fetch_sounds.py` | the hits in the door-rattle recordings |

The Blender tools run headless (`Blender -b --factory-startup -P <tool> -- ...`) and also write their
models into `assets/models/` (`cat.glb`, `arms.glb`, `birdcage.glb`, `flashlight.glb`, the boat);
`make_cat_blender.py --preview clip@seconds` renders stills. `make_reeds.py` and
`make_chainlink.py` write cutout textures, and `make_grade.py` the colour grade,
`assets/lut/silent_grade.cube` (`--no-grade` drops it).

**The map is printed from the game's own plan**, so nothing about the town is copied into Python by
hand: `--map-export` writes every shape the map shows, from the code that builds it, into
`tools/town_plan.txt`, and `make_map.py` draws the print, the marks and the folded map's cover from
that. Re-export and rerun both when the street, the lake, the hill or the land change. Each mark
draws from a random stream of its own, so adding a map or a place leaves every existing mark's hand
as it was.

### Materials

- **Grime is baked into vertex colours**, which `pbr_frag` multiplies into the albedo. A material
  in `mats.c`'s grime table has its boxes' faces cut into a grid and darkened toward their edges,
  under a patch field and jitter so it gathers in stretches. Walls are left out, since a wall's slab
  seams are edges nobody built. On glass the albedo also tints what shows through, so the same dirt
  reads as a film. A smooth solid's grime gathers where it is fixed on.
- **Container glass is 4 mm thick**: a jar is hollow, and thicker the refraction pass sampled past
  the jam to the wall behind.
- **The hall clock's picture has a quarter-size roughness map**, on purpose: every roughness map is
  a layer of the engine's material array, which brings all its layers up to the largest.
- **Stained glass is opaque and emissive by its own picture**: a transmissive pane showed only the
  fog behind it.
- **Porosity is stated per material** in `mats.c`, not read off the roughness, which soaked
  painted siding and car paint like soil.
- **Displacement maps are named `_disp.png`, never `_height.png`**: the engine gives parallax to any
  material whose albedo has a `_height` sibling, and the jars' contents share the grass's albedo.
- **Wax scatters light under its surface** (`mats.c`'s `ScatterSpec`, chosen by eye).

### The town

- **The street and the player's house** (`street.c`, `home.c`, `kitchen.c`, `bedroom.c`,
  `basement.c`): a two-storey clapboard house with a porch, a straight hall from the front door to
  a back window with the kitchen and living room off its front half and a bathroom and bedroom off
  its back, the stair shut, and a basement dug under the whole house down a steep cellar flight.
  `--seed` reseeds the clutter and the street's windows.
- **The mansion** (`mansion.c`, `house.c`, `tower.c`, `interior.c`, `hearth.c`, `study.c`):
  two storeys of weathered charcoal board-and-batten on a rubble base, holding a Gothic dining
  room, a double-height great hall with an empty firebox and a stair to a railed gallery, and an
  octagonal tower with the study upstairs, about a thousand seeded books, stained glass, and four
  gargoyles outside that drip from their jaws.
- **The drive** (`hill.c`, `trees.c`, `grounds.c`, `road.c`): up a faceted hill, carved to the
  road's own profile and collided as a mesh, through bare `tree_gen` trees, to iron gates, a fence
  and a graveyard.
- **The town's edges** (`land.c`, `terrace.c`, `fences.c`, `woods.c`, `crossroads.c`): one ground
  grid from `land_height` over the whole world; a stepped terrace behind a retaining wall on the
  far side; every yard fenced, the fences reporting each fallen length and the woods filling it
  (the player's yard is reached through its open side gate, its back gate padlocked and a dead
  tree lying through a fallen length of its back fence); conifer woods behind both sides, with the
  world's edge deep inside them; and at the west end a crossroads, each arm barricaded by one row
  of `BARRICADES` so a mechanic can open it, and the road straight on ending at a chasm.
- **The lake valley** (`road.c`, `lake.c`, `cabin.c`, `dock_lamp.c`, `boat.c`): the chasm's lip
  turns west at `RIDGE_Z` as a ridge's face, and the same ground grid grows south into a valley,
  the lake's bowl and bed added last in `land_height` (`lake_ground`). The bed is in the collider
  too: left out, the player falls through the world. `road.c`'s spline and carve serve both roads:
  the drive rises on a smoothstep, the track at a constant grade with eased ends, steep enough for
  puddles to stand on it. A dock with a round brass lantern (`fixture_lantern_round`) swinging from
  a crook in the breeze, its cached light following its bulb; two boats, reeds, drowned trees, a
  valley wood on its own random stream so the older woods' draws are untouched, and a log cabin with
  its door left open, so its fire shows from the dock.

Rules from building these:
- **Trees are hung once and never taken down**, kept cheap far off by their levels of detail (each
  model's meshes carry their own `lod_scale`, so the engine's bias stays the GUI's) and drawn only
  within the fog's reach by a `draw_distance` on their roots (`TREES_REACH_NIGHT`,
  `TREES_REACH_DAY`; none under `--no-fog`). The shadows still take every tree. Hanging and
  unhanging trees as the player walked changed the scene graph every few steps, which costs every
  cached light the faces that change touches.
- **A tree's wood and needles share one wind response**; the needles' extra life is their flutter.
  The trees are held at rest in the cached shadows (`cached_shadow_wind =
  CACHED_SHADOW_WIND_REST`): following the drive's dead trees' sway there cost 176 ms a frame.
- **Each trunk's collider** is the model's own radius at a body's height, leaning with the tree
  (`trees_trunk_collider`).
- **The lake is a water of its own**, bounded to a rectangle so the chasm at the same depth stays
  dry, and switched on only within the fog's reach of it plus 15 m, since its surface costs a frame
  wherever the eye is. Startup prints the lowest ground on its bounds, which must stay above the
  level. Its waves are the FFT's, a breeze and no swell (`--lake-waves gerstner` for the other
  model).
- **The fog is the street's rectangle with a hole round each house and the cabin**, reaching down
  into the chasm; its haze floor is at the lake. The basement's haze starts at its own floor.
- **The street adds drip lines while it is built and draws nothing from its random stream for
  them**, so every seed builds the same street.

## The player

`player.c`, over the engine's `gait.h`:
- **The body is drawn between its 60 Hz steps**, with a speed ramp and a fast turn of heading, and
  climbs a flight at the pace a person keeps on it. The gait eases the eye up kerbs and steps, bobs
  it 2 cm walking with a centimetre of sway, and lets each foot fall on what it lands on
  (`footsteps.c`): the kit body's material, a rug's own test, and on the open ground what is drawn
  there, wet under the rain. `--no-interp`, `--no-gait`, `--bob`, `--climb`, `--no-footsteps` and
  `--trace-gait` take it apart.
- **Crouching** shrinks the capsule from 1.7 m to 1.15 through
  `character_controller_set_half_height`, keeping its foot, and eases the eye from 1.62 m to 1.05;
  half a walk's pace, half the bob, footsteps 6 dB softer. Sprinting asks to stand, and standing
  waits until there is room. Nothing a standing body cannot get under is open to a crouched one
  except the gallery sconces, which lead nowhere. The cat sees a crouched player low
  (`player_eye_height`), so what stands between hides them.
- **The crouch toggle is read once a frame, never in a fixed step**, where a press counts twice on a
  frame of two steps.
- **The player owns the cursor.** In play a click takes it in a window or borderless, since the
  window shares the desktop, and exclusive fullscreen takes it whenever play has the input. The
  title and every screen over the game show it only while the mouse drives them, hiding it as soon
  as a key or a pad does. The loading screen hides it. G frees it while the debug GUI is open, or
  the sliders could not be reached.

## The house and the story

**The story** (`story.c`): the front door is locked from the inside, and the key hangs in a
birdcage on the basement's workbench, down the cellar stair the flashlight gets the player down.
Taking the key begins the haunting (`story_key`), and the cellar door slams shut over the player's
head. With the key and no map, the player thinks of the map in the kitchen and the front door stays
shut; with both, it opens, the haunting ends, and it is an ordinary door from then on. The stage is
saved as a `story` section by name and the game autosaves at each turn. A player saved anywhere but
in the house counts the door open, so a game started out in the town is never locked out. K begins
and ends the haunting without moving the story.

**The backpack** (`backpack.c`) lies on the back bedroom's bed with the flashlight in it. Until it
is taken, F thinks "I don't have a flashlight." and a body nobody sees bars the basement at the
cellar flight's foot. The flashlight (`flashlight.glb`) is clipped high on the jacket's chest, its
beam aimed at what the look meets. The backpack keeps an item's node on the scene graph exactly
while it does not hold it (`backpack_lay`).

**The birdcage** (`birdcage.c`, `inspect.c` over `arms.c`): E picks it up in the player's own arms.
The view glides to a framing set from the cage and onto the arms' `View` bone, a camera rig of the
hold's own, while the body stays where it stood. The mouse or the stick spins it, E opens the face
and takes the key, Q sets it back shut. While it is held the flashlight is lit and held on it and
the night's exposure drops four stops. A load or a return to the title puts it straight back on the
bench, open exactly when the key is had. The arms, the cage and the flashlight come in through one
import (`library.c`).

**The haunting** (`haunt.c`): the power goes -- every light in the house, the TV and its hiss, the
street lamps, and the light no lamp gives: the GI bounce, the reflection probes' and the sky's,
which the lit surface scales together by `ibl->intensity`, and the fog's glow. A second and a half
later the hall's lights, the floor lamp and the TV come back red, under an after-tonemap post pass
(`shaders/haunt_grade.glsl`) that drains the picture to red; the bathroom bulb, the bedside lamp,
the kitchen's tubes (`lights_set_mains`) and the cellar bulb (`Basement.mains`) stay out. The clock
stops (`clock_stop`, then
runs late rather than jumping), the fridge clunks off, a recording and a drone play, and the
pendant swings. Then the doors, each waiting for the player rather than a clock:
- from 4 s, once it stands shut, the cellar door is tried from its other side -- the knob turned
  under a rattle, the leaf jolted on the recordings' own hits (`door_jolt`) -- until the player
  opens it on nobody;
- a second after the player first stands in the hall, the bathroom door creaks ajar, or shut
  (`door_drift`);
- the first time the player stands at the front of the hall looking back down it, an open cellar
  door slams (`door_slam`).

Every capture rests the house as it was, so nothing the haunting puts out is kept in the lighting.
`--haunt`, `--haunt-at`, `--story`, `--key`, `--inspect` and `--trace-haunt` drive it.

**Doors** (`door.c`) are kinematic bodies, Gothic plank leaves in the mansion, four-panel ones at
home and battened boards at the cabin (a `DoorLeafFn` each, hung by `door_hang`), swung by E or pad
X only, never by pushing. A door that will not open is tried by hand with the haunting's knob and
rattle takes (`door_try.c`). Every slam is heard from where it lands (`door_update`), from one set
of door sounds (`DoorSounds`).

**The hall clock** (`clock.c`) stands under the hall's lantern. Its pendulum and hands are each a
node of their own, built by a small kit round its pivot and turned from `game->time`; the painted
dial and the arbors' positions come from `make_cards.py`.

**The television** (`tv.c`) shows snow, through the engine's app shaders: `shaders/tv_static.glsl`
on a card over the glass in the late draw, past TAA, so a new field every 1/59.94 s is not averaged
into grey; and a surface hook on the glass (`tv_glass.glsl`) giving the glass's light the
picture's shape, both from one chunk (`tv_picture.glsl`). So SSR, the probes and TAA's history see
the screen lit while the card adds only the noise. The room is lit by an area panel on the glass,
as bright as the picture averages, with a specular share of 0, since the glass's own light is what
reflects: a point light in front of the screen hung a ball of lit fog in the air. `--no-static`
switches it off.

**The basement** (`basement.c`): a bare bulb hangs over the flight, set swinging by the draft the
first time the door opens, and dims now and then on a failing supply (`lights_brownout`). Its light
is cached with no body and sets the engine's `Light.shadow_follow` while the swing is wide, so its
shadows follow it rather than stepping each time it passes the kept shadows' tolerance. The cellar
and the flight behind its door are a sound zone of their own, so the house, the wind and the rain
come down through the floor, and the door opening lets the tap's drip up into the hall. Cobwebs
were tried and taken out: nothing painted read as silk.

**The hall** is lit by a pendant on a chain and a lantern on a bracket (`fixtures.c`), brass with
clear glass round filament bulbs. The glass casts nothing for a cached light, so the light leaves
through the panes and only the brass shadows it. The pendant is a kit kept beside the street's,
hidden from captures, swinging about its rose by an eased `Lights.swing_want`. The bedside lamp's
shade is lit from inside by the lampshade hook, and its light is cached with a near plane past the
shade's rim, since a shade drawn into its own light's shadow boxes the bulb in.

## Sound

- **Every recording is a CC0 Freesound recording shaped by `tools/fetch_sounds.py`** (needs
  ffmpeg), which lists each one's source. Loops are cut from the steadiest stretch, crossfaded into
  themselves and levelled alike, so the volumes in the C files are the only statement of loudness.
  `fetch_sounds.py --audition DIR` renders every candidate to choose by ear, and `--only JOB ...`
  fetches and makes only the jobs named.
- **The rooms are the engine's audio zones** (`sounds.c`): each house, the home's living room,
  kitchen and basement, the mansion's upper storey, the cabin, as boxes (the tower's octagon as five,
  from `tower_boxes`), joined by links for an outside wall, an open doorway, a floor and a storey.
  The rooms and links are two tables; each door is one row, whose swing opens its link. Every placed
  sound is heard through the walls between it and the listener, so no module multiplies a wall into
  a volume. A floor lets slightly more through than an open doorway, which was left so by ear.
- **The wind is a 2D bed placed nowhere**, two layers crossfaded by how far the doors stand open, at
  the level of the best-heard room with an outside wall. The rain's two noise beds take the same
  level under a roof, read from the rain's occlusion map at the player's head (`rain_bed.c`), so a
  front door opening changes the wind's mix and not the basement's level.
- **Placed sounds** include the hall clock's tick and tock (put through a wooden case), the fridge,
  the tubes' buzz (which drops out with the failing tube's flicker), the television's hiss, the
  basement tap's drip, the lake lapping at the shore nearest the listener, and the cabin's hearth.
- **The motorboat's** engine loop is re-pitched by its speed through `audio_sound_set_pitch`.
- `--mute` silences everything. `--audio-dump PATH` with `-x` writes what the listener hears as a
  WAV, which is how to listen without a window.

## Lighting

- **The kitchen's tubes are emissive strips the engine turns into area panels**, so a flicker is a
  material strength and the light follows it. `--no-flicker` keeps the failing one steady; a
  headless frame that lands in its stutter shows the tube dark and reads as an underlit room.
- **The flashlight is registered before any other spot**, because the fog scatters only the
  scene's first spot.
- **The street lamps are point lights shaped by an IES profile**, which is what draws a cone of fog
  under each head rather than a ball round it.
- **The candles, the lamps and the tubes cast cached shadows**, so no light reaches through a wall.
  A candle's soft shadow is the average of eight views over its flame and follows the flame with no
  face redrawn; the clock's pendulum and hands are drawn over a copy of the hall's still casters
  each frame. Each tube is cached from its strip's centre with a near plane past the steel channel
  it sits in, which otherwise shadows the ceiling round it. The tiles go to the nearest lights, so
  at home the mansion's candles do not cast.
- **The candles' light is lifted 20 times and the cabin hearth's 8** over what their flames cast:
  the night is exposed for a house lit by a few candles, and at physical brightness a fire leaves
  its own room dark.
- **GI volumes**: the mansion's grid is 11x7x11 on 1.21 m cells, the one spacing found that keeps
  every probe centre 0.15 m off every wall and slab, and the startup print says when a layout
  change breaks that. The home's is laid over its bounds, down to the basement floor, on 1.25 m
  cells with classification on (`create_gi_volume_spaced`), which switches off a probe inside a
  wall instead; 1 m cells swept much slower for a change in bounce light under a code. The cabin's
  is 1 m.
- **Reflection probes**: thirteen boxes, seven in the home, five in the mansion and one in the
  cabin. Each box runs to the plane its room's doors and panes hang in -- a shared wall's centre
  line, an outside pane's room-side face, and for the hall the front door's middle -- since a door
  outside every box reflects the sky. Each fade is half a pane, so a window's street face never
  takes the room's probe; at the engine's default fade a room's reflection reached through its wall
  onto the panelling next door. The study is two boxes, room and tower bay, since one round both
  took in the facade. The great hall's box goes to the ridge, its roof being open. The cabin's is capped at
  the eave, or the tin over the room took the probe. The Probe Coverage render mode, in the debug
  GUI, paints whatever falls through to the sky magenta: indoors any magenta is a miss.
- **The GI volumes and probes are installed on the third frame** (`total_frames == 2`), not at
  load: the tubes' panels cast only from the second frame, and a volume's first sweep is its only
  full-weight one, so it has to see the lit room. Each house's volume sweeps, and its probes
  capture, only once its cached lights hold their tiles, which the nearest get first, so the
  mansion's are taken on the way up the drive. The frame fades in once the lighting round the eye is
  ready (`scene_lighting_ready_in`).
- **Its light is cooked** (`Scene.cook_lighting`): a launch after the first loads the sweeps and
  probe columns from the cook unless something they see changed. `CETRA_NO_COOK=1` captures live.
- **A capture holds the scene at rest** (`rest_lights`): the failing tube lit, the basement bulb
  straight on a full supply, the TV's field at its mean, the flashlight off, and the clock's
  pendulum and hands `capture_hidden`.
- **The captures are paced**: under the loading screen, and in the black while a game comes up,
  they take up to `LOADING_CAPTURE_MS` of each frame, so the window keeps answering; otherwise the
  play budget, so PLAY answers a press within a short frame. `--capture-budget-ms` pins one,
  headless too, where the default is no limit.
- `--no-gi` leaves out the volumes and the probes captured from them, which is what separates a
  change in the direct light from one in what the captures saw. `--no-candle-shadows`,
  `--tile-views` and `--tiles-probe` are the shadows' switches.

## Loading

It loads under the engine's loading screen, shown at the top of `on_init` and drawn at every
`load_seam` and between its materials. Once the lighting round the eye is in, the screen shows
PLAY until the `start` action (Enter, Space, E, a click, pad A or Start) lets it go; that action is
read while the rest of the input is held, and only while PLAY is up. Headless, and under
`--no-play-prompt`, it goes at once. The fade and the player's input wait for the screen to switch
off, the input held from the frame-input hook, and a click takes no cursor while it is held.
`--no-loading-screen` loads under a black window, and `--loading-screen` keeps the screen headless.
- Textures decode on the engine's loader threads, and `mats_register` waits for them before it
  returns, since a kit frees an unused material as it finishes.
- Sounds decode on miniaudio's job thread: silent begins the audio's loading phase as it makes the
  system, and `game_run` ends it before the first frame.
- The physics broadphase is optimised before the cat is placed, whose place check casts thousands
  of rays.
- The kit's shadow cells, the woods and the cat draw the screen between their pieces. The longest
  single stand-stills left are the land's mesh collider and the prompt's font atlas, each one call
  of about 140 ms.
- `--startup-ms` prints how long each step took and how long the screen stood still in it.

## The frame

- **One sample with jittered TAA, headless as well as windowed.** With MSAA the fog composite takes
  one depth per edge pixel and outlines every silhouette in the street. The jitter follows the
  frame index, so a headless run still repeats itself exactly. Render scale defaults to 0.5.
- **No film grain.** `--crt` shows the game, its prompts and menus included, on a consumer
  television; it is off by default.
- **`--day` is an overcast day on a photometric sky**: the sky is in nits like the tubes, fully
  overcast so nothing casts, and metered with the meter floored so a room never opens past the
  night's pinned exposure. The kitchen by day is the night's exposure plus its window.
- **By day there is a local exposure** (highlights 0.2, shadows 0.6, blend 0), so the kitchen
  window onto the street reads without the room going dark. The window is about 6 stops over the
  room and the day key of 0.45 leaves about a stop above grey, which is why it takes so strong a
  setting. It is off at night, whose exposure is pinned.

## Rain and weather

It rains by default at 6 mm/h, already soaked (`--rain <mm/h>`, `--no-rain`). What silent changes
from the engine's physical rain is a look, each a constant in `silent.c`: brighter, denser, slower
streaks, lighter wet darkening, bigger splashes and brighter drips, because in fog this dense a drop
refracts air about as bright as itself. Its reflections see the fog (`reflect_fog`), and SSR
reaches 40 m so a puddle mirrors the street.

A gusting breeze blows the rain off the street onto the houses (`--no-wind` for still air), which
is what wets the facades and the kitchen window under the pent roof; in still air both stay dry.
Glass beads, opaque panes included. Both houses carry a gutter and downpipe
(`house_front_gutter`), and its leaks are drip lines -- two joints, an overflow past the kitchen
window and the downpipe's shoe -- while the neighbours drip from bare eaves and porch roofs. The
drips are lit by the kitchen's tubes as well as the lamps. The road, pavement and yard carry their
scans' displacement, and the puddles stand in their lows (`--no-relief` for noise alone).

## The cat

A black cat with yellow eyes lives in the mansion (`--no-cat`). Its body is built headless in
Blender by `tools/make_cat_blender.py`; `out/cat_blender/cat.blend` is kept to look at. It comes
in through a library scene (`docs/architecture.md`, Memory Ownership) and is `capture_hidden`, so
it goes into the house at once and no capture keeps it.
- `cat_body.c` is the body and its look: the import, the coat read from `assets/scenes/cat.cscn`,
  the colours (`--cat-fur`, `--cat-eyes`), and the eyeshine that throws the flashlight back.
- `cat.c` moves it: a posture ladder (curl, lie, sit, stand) a transition clip apart, quarter-turn
  clips on the spot, walks, trots, runs and the stair advanced by root motion, jumps played at the
  rate that makes their air time the arc's own and turning in the air to land facing on, and acts in
  place with the face's vocals on the override layer masked to the bones they move.
- `cat_places.c` is the mansion as a graph of places and links (walks, the flight, jumps, and the
  gallery's hand rail, walked one way, east, with nothing turning on it, and left out of every route
  but the one meant for it).
- `cat_brain.c` is an aloof watcher on the engine's brain: sleep, stretch, loaf, explore, watch the
  candelabra's flame, watch the rain, avoid, startle, follow, and the rail to the stair's newel,
  with needs and memory of places, and a head turned by the engine's look-at to whatever it
  watches. It senses the player's body, never the camera.
- `cat_voice.c` plays its recordings from the clips' events at its head or feet, and purrs for
  someone close it likes.
- `cat_debug.c` draws its graph, route and sight line in the debug GUI.

Two runs on one seed (`--cat-seed`) trace identically. `--cat-at`, `--cat-clip`, `--cat-goto`,
`--cat-activity`, `--cat-blind`, `--cat-cam`, `--cat-say` and `--trace-cat` script and read it.

## The boat

The moored boat (`boat.c`) is a wooden rowboat with an outboard, modelled, baked and fitted by
`tools/make_boat_blender.py`; its twin lies turned over on the bank. E from the dock gets in, onto
the stern seat, the view carried by the boat; E starts the motor by its pull cord (`pull_cord.c`:
three presses while a marker running along a bar is in its lit zone, on the HUD's gauge); WASD
drives; E stops it; Q or a right click gets out alongside the dock, and the boat is moored and tied.
The first time on a trip it reaches the spot circled on the cabin's lake map (`LAKE_MARK_*`), the
motor dies. It bumps off the bodies that stop a wader and the dock analytically, against its rim;
its hull keeps the water out with the water's dry shapes; a save keeps it. `--in-boat`, `--boat-at`,
`--boat-running`, `--pull-cord` and `--trace-boat` drive it.

## The UI screens

- **The title** (`title.c`): a windowed run comes up on it over the street -- Continue, New Game,
  Load Game, Settings, Exit to Desktop -- under a camera rig of its own, the game held on a clock of
  its own while the weather goes on.
- **Saves** (`save_game.c`): no manual saves. A game saves itself into its slot, `silent-1` to
  `silent-8` in the engine's per-user directory, when something is taken, a place found, another
  room entered and on quitting, with a candlestick in the corner saying so.
- **The pause menu** (`pause_menu.c`): Resume, Settings, Quit to Title, Exit to Desktop, the whole
  world frozen under it on the sim clock, the rain in the air included. **Quit to Title** saves,
  goes black, puts the world back from `silent-start.json` (written at load), makes the cat's mind
  afresh and loads again round the title's view. So taking the bag or the map takes its node off the
  graph and keeps it, and a load or a return lays it back.
- **Settings** (`settings_menu.c`): one volume, window mode and vsync, look speed and invert, and
  the field of view, applied as they change and kept in `silent-settings.json` beside the slots
  through the engine's `settings.h`. `--mute` and `--fov` win over the file.
- **The HUD** (`hud.c`): the door prompt, and over it a thought that fades in and out, on one
  non-modal screen.
- **The backpack's screen** (`backpack_menu.c`): one amber pane, the bag's grid on the left with
  each thing across its own footprint as a picture of itself, the chosen one turning on the right.
  The game packs the grid -- first fit in the order things were had, then largest first, then a row
  more -- so the player never arranges anything and it is never full. The pictures come from
  `item_view.c`, which draws an item's meshes through their VAOs into a target of its own from
  inside the overlay and puts back every piece of GL state, since the engine draws one view of one
  world.
- **The map** (`fridge_map.c`, `map_screen.c`, `town_map.c`): a street map hangs half open on the
  fridge's freezer door; E takes it into the bag once there is one. M opens it as a view with a pan,
  a zoom and a red arrow for the player. The player's house is circled "my house?" the first time.
  Each find in `town_map.c`'s `PLACES` table -- the barricaded north arm, the road's end at the
  gorge, the cabin -- is found by walking into its box and thought about, with "M   Map" hinted
  once the map is had; a find made before is written on once it is. Blue ink writes each mark on
  stroke by stroke the first time the map shows it: an app UI fragment stage (`shaders/map_ink_frag.glsl`, built by
  `create_ui_draw_program`) reads when the pen reached each pixel from the marks picture's red,
  through `ui_draw_textured_quad_uv`. The map's two pictures decode on the loader threads
  (`engine_load_texture`). A map is a `MapId` with one generated `MapArt` row.
