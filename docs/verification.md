# Verification: gates, goldens and deterministic renders

How this project decides that a change did what it claims, and what each instrument can and
cannot see. `docs/architecture.md` owns the architecture; this file owns the method.

Three instruments, answering different questions:

| instrument | asserts | blind to |
|---|---|---|
| `scripts/gates.py` | analytic properties: a ratio, a count, a penumbra width, whether two runs agree | anything nobody wrote an arm for; a bar too slack to fail |
| `scripts/goldens.py` | 33 committed PNGs, pixel for pixel | whether the image is right, only whether it changed |
| a hand A/B | everything else | nothing, and that is the problem: it has no bar unless you measure one |

**A pixel count quoted without its noise floor is not a measurement.** That rule governs all three.
Every verdict below was taken at one resolution, one frame count and one flag set, and all three
move it.

**Both suites run on macOS, Linux and Windows** (`./build.sh --target linux|windows`, see
`docs/build-vms.md`). Most gate arms pass everywhere, since they assert an analytic property rather
than a stored image. Two things do not travel and are not regressions: timing arms mean nothing
under a software rasterizer, and goldens compare real pixels, so a real rasterizer difference
shows. Never re-bake the corpus off macOS.

**A macOS update moves goldens too, and no flag reaches it.** An update moves pixels by one code
(rarely two), in bands where a smooth gradient crosses a code, so the sky, fog, cloud, water and
large-gradient fixtures are the ones that move. To tell an update from a change, build the commit
that last baked the corpus clean and check it on the updated system: if it fails the same goldens
by the same counts, the update moved them. Build it from an export, so `out/` keeps the current
tree's binaries: `git archive` into a scratch directory, the seven submodules linked in, a
`VERSION` file, and `cmake -S <export> -B <export>/out -G Ninja -DCMAKE_BUILD_TYPE=Debug`.

## Contents

- [Running the gates](#running-the-gates) — which binaries a number describes, what a release run
  moves, why frame counts are a correctness question, how to read a `--profiler` table
- [Deterministic renders](#deterministic-renders) — the bar, the noise floor, reading a diff
- [The scale twin](#the-scale-twin) — a scene and itself shrunk 1024 times at 0 px, why that is
  exact, and what it cannot see
- [What breaks determinism](#what-breaks-determinism) — the per-source table, and the trap that
  two runs of one build are not two builds
- [Assets, and which are safe](#assets-and-which-are-safe) — the per-asset ledger
- [The recipe](#the-recipe) — the cross-build baseline
- [Capturing a before](#capturing-a-before)
- [The gamepad device path](#the-gamepad-device-path) — what the `gamepad` group does not
  cover, and the two recipes (a pad on this Mac; Linux uinput) that would
- [The audio device path](#the-audio-device-path) — what the `audio` group does not cover,
  and how to hear it on real hardware
- [The idle wait's real events](#the-idle-waits-real-events) — what the `idle` group does not
  cover, the early wake a hidden window gets, and how to look at a still window not drawing
- [The real-character path](#the-real-character-path) — what the `anim` group asserts about
  the math, and what only watching a real rig can settle
- [The root-motion path](#the-root-motion-path) — what the root-motion arms assert, and why the
  benefit root motion exists for cannot be measured on any asset in this tree
- [The UI paths](#the-ui-paths) — what the `ui` group does not cover: the layer's pixels,
  another display, a real pad through a menu, and the other two settings locations
- [The save paths](#the-save-paths) — what the `save` group asserts, why a restored world is
  deliberately not bit-exact, and the migration nobody can test until time passes
- [The foot-planting path](#the-foot-planting-path) — what the `ik` group asserts about the
  solve, what it cannot see, and why planting is not locking
- [Reproducing a session: the config snapshot](#reproducing-a-session-the-config-snapshot-spec-1171)
  — handing over a tuned session as one JSON file, what it carries, and what it cannot
- [Debugging an animation](#debugging-an-animation) — the puppet, the retarget A/B, and the real
  rig
- [Where assets live](#where-assets-live) — the corpus split by kind, and the three couplings that
  naming an asset through `fixture_paths` holds

---

## Running the gates

`gates.py` needs Python 3 with numpy (`python3 -m pip install numpy`; without it the script fails
at the import, before any group runs) and ImageMagick's `magick` on the path for `compare`.

### Which binaries a number describes

`scripts/gates.py` and `scripts/goldens.py` both take `--bin-dir` (or `CETRA_BIN_DIR`). Which
binaries a number describes is part of the result, so gates prints the resolved directory on every
run.

- **goldens.py defaults to `out/bin`**, the debug build.
- **gates.py picks by freshness**: `out/release/bin` (from `./build.sh --release`) when its
  `forest` is at least as new as the debug tree's, `out/bin` otherwise. A release tree older than
  the source is therefore never used.
- **Falsify a mutation with an explicit `--bin-dir`.** A rebuild confined to `apps/render` never
  re-links `forest`, so a debug-only rebuild can leave the selector on a stale release binary,
  and the arm comes back green against code that was never built.

The release win is almost entirely forest's (spec 11.65): forest starts in 6.58 s debug against
1.36 s release, because its startup is CPU work `-O2` reaches (procedural texture bakes,
cluster-DAG builds, the scatter, erosion). `render` goes from 0.41 s to 0.36 s, since its startup
is a GL context and driver-side shader compiles.

### What a release run moves

A release run moves no verdict, but it moves numbers. Besides milliseconds and forest pixel counts,
`forest-lod`, `cluster-bands` and `cluster-parity` move, because **meshoptimizer makes slightly
different collapse decisions under `-O2`, so a LOD chain and a cluster DAG depend on the build
type**. The raw triangle count with `--no-lod` is identical in both: the simplifier's decisions
moved, not the geometry. **Forest triangle counts do not compare across build types.**

The cook (spec 11.99) does not hide that. Two builds reading one cook dir draw whichever build
cooked first, but the tree prototypes' vertex bytes themselves differ across builds, so their DAG
entries split by content and each build cooks its own. The suite is unaffected either way: gates
runs against a per-invocation cache dir, so everything an arm reads was cooked by the binary under
test.

### Goldens must not follow the gates onto release

Goldens are committed PNGs, and `-O2` changes CPU float results, so checking release frames against
debug-baked goldens is a comparison across two builds, which does not hold (see What breaks
determinism). Move goldens to release only together with `--rebake`.

### Frame counts are a correctness question, not a performance one

A frame at the suite's shared 400x300 costs about 0.0013 s, so thirty frames cost 0.04 s more than
one. The suite's cost is its process startups, not its pixels. Lowering a frame count saves almost
nothing and risks an arm that reads a pose silently reading a different one. `render()`'s
docstring in `gates.py` records what the engine actually forces: one frame for a static, pinned,
fog-free capture (a headless run's start-up textures are in before frame 1), and 60, 150, 240 or
400 where the accumulators, the meter, the trace cadence and the walk geometry need them.

### Reading a `--profiler` table

**Every row is per frame**, not per call (spec 11.97). A pass entered once in the window and a pass
entered every frame both publish what they cost the average frame, which is what makes the rows
addable and `TIMED` meaningful.

**`FRAME (wall)` and `PERIOD` are different quantities.** FRAME is the frame's own bracket, begin to
end; PERIOD is begin to begin, so it also carries the swap, the vsync wait and the poll.
- For the CPU column `TIMED <= FRAME` is an identity (flat scopes, all closing inside the bracket),
  so a violation is a bug in `profiler.c`, never a slow frame.
- For the GPU column it is a loose bound both ways, because the driver runs behind and a pass's
  time can land outside the bracket that submitted it.
- PERIOD is what matches an fps counter and what a budget is read against. Headless has vsync off,
  so the two nearly agree there and diverge in a window.

**A 0.000 GPU row is not necessarily a free pass.** It may also be a driver that accepts timer
queries without measuring, which `gpu-nonzero` watches for. The query ring is drained at each
latch and a query given up on is reported by name.

## Deterministic renders

### The bar, and the noise floor

The bar for "this refactor changed nothing" is **0 differing pixels**, and you can only claim that
from a scene that renders identically across two runs of the same build. Measure the noise floor
first:

```bash
# same build, twice -- must print "0 (0)" before the comparison means anything
magick compare -metric AE run_a.ppm run_b.ppm null:
```

**Measure the floor even when this file says the asset is safe.** Every verdict in the table below
was taken at one resolution, frame count and flag set, and all three move it: `apps/forest` reads
7,134 px at 20 frames and 34,991 at 60. The floor costs one extra render.

### Reading a diff

`-metric AE` counts differing pixels (the useful bar). `-metric RMSE` gives magnitude, `-metric PAE`
the worst single-channel delta. A `PAE` of `1/255` (0.00392) is last-bit rounding: the compiler
scheduled the same math differently, not a behaviour change. To see where two renders differ:

```bash
magick compare -metric AE -compose src before.ppm after.ppm diff.png
# sparse specks are usually LSB noise; dilate to make them visible:
magick diff.png -negate -morphology Dilate disk:4 -negate -resize 1000x big.png
```

### The scale twin

**`scale-twin` (spec 13.57) renders each of its scenes twice, as it is and shrunk 1024 times,
and passes only at 0 px.**
- **What the twin scales:** it multiplies by K = 2^-10 every length the scene states (positions,
  ranges, sizes, a light's body, the camera's eye and target, an SSS radius, a material's
  thickness, the fog's near and far) and the typical size.
- **What follows from it:** point and spot candela go as K², so illuminance is unchanged, and fog
  density per metre goes as 1/K. Directional lux, a panel's nits, exposure and the environment
  stay.
- **So a twin differs only where the engine holds a length of its own:** a tuning length that does
  not follow `Engine.typical_size`.

**0 px is exact, not a tolerance.** Multiplying by a power of two is exact in floating point, and
so is every sum, difference, product, quotient and square root of numbers it has scaled: the twin
does the original's arithmetic with the exponent moved. A moved pixel names a missed length.
- **The one exception is fp16's subnormals**: a length stored under fp16's normal range, 6.1e-5 m.
- **A scene already measured in millimetres is twinned by growing it** (K = 1024), and the argument
  holds unchanged that way. The board's view depths run from a millimetre to a few centimetres;
  shrunk, they fall under that floor and the twin reads noise.

**A key the twin cannot classify is refused by name, never copied.** Copied unscaled, it would
render a twin that differs, and the difference would read as a length the engine holds.
- Every block the twin walks is classified key by key (`TWIN_LIGHT_KEYS`, `TWIN_PROBE_KEYS`, and so
  on), and a flag the rows pass is listed in `TWIN_FLAGS` by kind.
- A material's lengths are read from `material.c`'s `.length = true` rows.
- So a new length gets its row there before a fixture using it gets a twin row.

**What it cannot see:**
- **Scenes whose physics does not scale** with the scene: water, whose waves gravity sets, rain,
  particles, and anything animated. Each excluded fixture is listed beside `TWIN_ROWS` with its
  reason.
- **The classification itself.** The twin proves that every length scales with the scene, not that
  each follows the right thing: a physical length wrongly multiplied by the typical size scales
  with the scene too, and twins to 0 px. Only reading the classification catches that.

**`fit-clip` holds a fitted camera's planes to the fit** worked out in the gate from the board's
glTF in double precision, at the board's three views and from inside the table, within 1e-4.

### What breaks determinism

**Two runs of one build matching does not mean two builds match.** Auto-exposure adapts per frame
(a fixed 0.04 blend plus a snap deadband, not a delta-scaled rate; see `exposure.c`), so two
equal-length headless runs of one build adapt identically and compare at exactly 0, which looks
like proof and is not. Across two builds the approach path can differ (a texture asked for after
the run began arrives in a different order and perturbs the frames the meter reads), exposure
lands slightly elsewhere, and since it multiplies the whole frame, every pixel changes. On a change
that was provably a no-op, 99.77% of a 4K frame differed with auto-exposure on; with
`--no-auto-exposure -E 1.0`, 9 pixels at PAE 1/255.

So:
- **Always pin exposure for a cross-build comparison.**
- **A whole-image diff with no recognisable structure**, backdrop and subject shifted alike, is the
  signature of an exposure difference, not a shading bug. Render the diff map (`-compose src`)
  before diagnosing; if everything is lit up uniformly, suspect exposure first.

| source | deterministic across builds? | how to pin |
|---|---|---|
| **Auto-exposure** | **Converged: yes.** The snap makes steady state a pure function of the scene. Before convergence, no: the approach path shifts with the arrival order of textures asked for after the run began (start-up's are in before frame 1), and exposure scales every pixel. When it converges is scene-dependent: frame 91 on `postfx_convergence_fixture`, not the deadband's first touch around 18 | **`--no-auto-exposure -E 1.0`** for any before/after; cheaper than reasoning about whether your frame count cleared convergence. `--exposure-probe` prints `target_log2` and `adapted_log2`; equal means converged |
| **Wind** (`wind_response` materials) | Yes headless: rides the fixed-step frame clock, frame N is phase N | (automatic) |
| **Spring bones** (`springbone.c`) | Yes headless: stepped by the animation clock, which is the frame clock | `--no-springs` only to isolate spring motion in an A/B |
| **Particles** (falling leaves, spores) | Yes headless: ticked from the frame clock; the game loop runs exactly one step per frame | (automatic) |
| **Skeletal animation** | Yes headless: the frame clock steps a fixed 1/60 per frame, so frame N is pose N | (automatic) |
| **Animation blending** (`animator.c`) | Yes headless: every clock in it (the blend space's, the crossfade's envelope, the override layer's) advances by the same fixed dt, so frame N is blend state N. In the game framework the dt is the sim clock's, a whole number of fixed steps, so a paused sim holds the pose and reads zero deformation velocity | (automatic); `--anim-probe` prints the weights and every bone's pose at `%.9g`, so a textual diff is a bit diff |
| **The animation state machine** (`anim_graph.c`) | Yes headless: the graph ticks once per rendered frame, right before the animator it drives, and headless a frame is exactly one fixed step. **Windowed it is not**: a frame that runs three steps or none still decides once, and the time-in-state a row compares against is in seconds off the sim clock | (automatic); `--graph-probe` prints states and clocks at `%.9g`, and most of its cases create no engine; `--trace-player` appends the live state and its clock as its last two columns |
| **The game UI** (`ui.c`) | Yes headless: every clock it has (hover, focus, a toggle's travel, a screen's entrance) advances by the fixed frame dt, not the wall clock. It draws after tone mapping, so no post pass can move it | (automatic); `--ui-probe` prints layout, navigation, capture and settings as numbers, and needs no window |
| **The place graph** (`nav_graph.c`) | Yes: routes break ties by node index, a follower advances by the root motion a clip states and a jump by its clip's clock | (automatic); silent's `--trace-cat` prints the link and the distance along it every 30 steps |
| **A brain** (`game/brain.c`) | Yes headless, ticked once per fixed step: its only randomness is its own xorshift32 from its seed, and a sense is a ray against the physics world, which is exact. **Windowed it is not**: what it senses depends on where the player happens to be | (automatic); silent's `--cat-seed` picks the seed, and two `--trace-cat` runs on one seed are byte-identical |
| **Voices** (`audio_play_voice`) | Yes in an offline render: a voice is decoded on the calling thread, never by an async load, and reaped from the mix pulled, so a dump is a function of the frames. **A headless run that pulls no PCM never ends a voice**; the pool steals the oldest instead, which no picture can see | `--audio-dump`, pulled a frame at a time |
| **Audio zones** (`audio.c`) | Yes: the best paths are a closure over the links, recomputed when one changes, and the fade eases by the sim clock's delta. The gain changes once a frame, between the frames an offline render pulls, and the first update, before frame 1, lands every gain | (automatic); the `audio-zone*` arms |
| **The head look-at** (`look_at.c`) | Yes: it eases by the animator's dt, and does nothing at weight zero | (automatic) |
| **TAA jitter** | Yes: off headless unless `--headless-jitter` | (automatic) |
| **Orbit camera** | Yes: auto-rotation is off headless | `--cam-eye`/`--cam-target` for exact repro |
| GTAO / SSR temporal accumulation | Frame-count driven, not wall-clock; no drift observed across builds | `--no-ssr --no-ssao` if isolating |
| **A config snapshot** (spec 11.71) | **It pins the look, not the frame index.** `--config` restores about 230 settings including the adaptation state, so exposure continues rather than re-approaching, but not the frame counter, the temporal histories or the async-load order. A snapshot plus the same `-f` is reproducible; at a different frame count it is a different frame | It is the pin. Dump `--config-dump` from the run you want to reproduce rather than transcribing flags |
| **Cooked artefacts** (spec 11.99) | **Deterministic by construction, and the one allowed crossing of the across-builds rule**: the cook key carries content and recipe and library versions, never the build type, so an artefact cooked by one build may serve another. Where a producer's bytes differ across builds (the tree meshes under `-O2`) the content fold splits the entries; where they agree (the rocks) they are shared. A container failing its version or checksum is refused | `--no-cook` (or a fresh `--cook-dir`) whenever the measurement is of a bake; `CETRA_COOK_DIR` to pin where a comparison reads from. `gates.py` and `goldens.py` isolate onto their workdirs automatically |

### Assets, and which are safe

Where the corpus lives and how to name an asset is in [Where assets live](#where-assets-live). One
consequence matters before reading a path here: **a model and its `.cscn` do not share a
directory**, so `-m foo.glb` auto-loading a sibling `foo.cscn` (`cscene_apply.c`) reaches only the
self-contained bundles (`abandoned_window/`, `ivy_arcade/`). Every gate and golden names its scene
explicitly; an interactive run wanting a scene's settings passes the `.cscn`, not the model.

**`assets/ivy_arcade/` has no coverage.** The `fixture-gen` gate prunes the self-contained bundles,
and its generator (`gen_ivy_mat.py`) would need Blender's sibling (`build_arcade.py`) to mean
anything. It is also not pixel-comparable: two pinned runs differ by about 8,700 px.

**A `.cscn` key in the wrong place parses and is ignored**, which recurs below (`sun` keys flat
instead of nested, `position` for `eye`, `"sky": true` for `"mode": "sky"`). `parse_env` and
`parse_water` report a key nothing read; check the log.

| asset | wind | springs | verdict |
|---|---|---|---|
| `my_models/raiden/source/raiden_textured_rigged.glb` | no | **yes** | **The best baseline.** Rigged, animated and normal-mapped; 0 px headless with or without springs. Exercises the skinned tangent path nothing else does. |
| `assets/models/parallax_fixture.gltf` | no | no | Deterministic, but 4 vertices and no mirrored UVs: only proves the `w = +1` path. |
| `assets/abandoned_window/abandoned_window.glb` (+ sibling `.cscn`) | **yes** (curtains) | no | 0 px pairwise headless with pinned exposure. `-m foo.glb` auto-loads the sibling `foo.cscn`; `--no-scene-file` escapes it. |
| `assets/models/room.fbx`, `assets/models/c64.fbx` | no | no | room's textures point at a dead path; c64 renders fully, textured and lit. |
| `apps/tree` | **yes** + particles | no | **Not pixel-deterministic on the orbit path**: about 9k to 31k px run-to-run depending on resolution and frame count, so measure the floor at your framing. The cause is `camera_drag_update`, fed `glfwGetTime()` because drag damping is input response. `--player` never creates the drag controller and steps by `engine->render_delta`, so **`--player --headless` is 0 px** and is the framing for any A/B in this app. Wind, grass sway and falling leaves are frame-index pure. It has a sea and a seabed, both under one `--no-water` switch. **Its water is spectral (FFT) by default**; `--gerstner-waves` switches back, and the four Gerstner wave params it authors are dead unless that flag is passed. So the FFT's cost and its crest foam apply to this app by default. |
| `apps/spores` | particles | no | 0 px headless; the game loop's step count is exact. `--taa`, `--headless-jitter` and `--msaa` exist to show why it keeps four samples, not to be used: its particles write no motion vector, so TAA reprojects every mote through the geometry behind it and turns dots into dashes. |
| `apps/gametest` | physics | no | **0 px run-to-run and byte-identical traces** (two `--trace-player --trace-every 10` traces `cmp` equal at 240 frames). Headless the loop gets the fixed frame dt, so the wall clock never reaches the accumulator; the FPS overlay, which draws from the wall clock, is off headless. **The `gamepad` group reads the trace's commanded move, not the position**: five boxes fall at `rand()` positions, which differ per platform's libc, so a leg walking into one on Linux would fail a displacement there alone. Across builds a face-on contact with a box can resolve its tie-break the other way (an 8 mm sideways nudge, mirrored); each build is exact against itself. Windowed it runs one sample plus TAA: every surface writes a motion vector, rigid meshes on `pbr` and the skinned rig on `pbr_skinned` from the previous frame's bone rows (`uPrevBoneRows`). `--no-puppet` gives an all-rigid frame. Its flags are in `apps/gametest/README.md`. The real gamepad path is unverified; see [The gamepad device path](#the-gamepad-device-path). |
| `apps/silent` | physics + tube flicker | no | **0 px run-to-run, with its jittered TAA and without it** (the kitchen view at 60 frames). Headless it runs one sample with TAA jittered as in the window; the jitter follows the frame index. The flicker is a pure function of the sim clock, so a capture lands on the same instant every run, but a frame in a stutter burst shows the ceiling tube dark, which reads as a lighting change between two configurations; `--no-flicker` takes it out. The GI volumes and probes start at frame 2, so an early frame is still converging, and a house's volume waits for its candles to hold cached tiles, which go to the lights nearest the camera: read the log's "converged" line before trusting a frame. No goldens and no gate arms, by decision. |
| `assets/scenes/puppet.cscn` | no | no | **The blending instrument** (spec 12.1), a rig built to be measured: twenty-two rigid boxes, one per bone, on the `cetra_rig:` names the committed walk clip carries, a T-pose with identity rest rotations and pure-translation bind offsets, so a wrong axis or order reads as a number in `--anim-probe`. Feet on y = 0 and centred in x/z, which makes the render app's recentre a no-op (the generator asserts it). Seven clips, all in closed form: `idle`, `walk` and `run` loop in cosine phase so the read frame is an extreme rather than a crossing; `jump` and `wave` are one-shots that end on bind; `hold90` swings one forearm to 90 degrees and holds it past the read frame (a clip ending there would wrap to bind on the frame measured); `rest` is the bind pose as a clip, the other end of the analytic midpoint blend. Every animated joint also carries a translation track holding its bind offset, because an embedded clip with rotation keys alone reads position (0,0,0) and collapses the joint onto its parent. `puppet_golden` is baked from `--anim-clip run`. |
| `assets/models/strut_walk.fbx` | no | no | A real walk cycle, animation-only: 52 channels, 86 ticks at 60 tps, authored on `t_pose.fbx`. Binds onto the puppet by exact name, twenty-two channels, which `anim-clip-loads` asserts; the other 30 are fingers, a second toe joint and a head tip the puppet does not model. On its own the retarget delta is identity and the rig folds; with `-s assets/models/t_pose.fbx` it walks. See [Debugging an animation](#debugging-an-animation). |
| `apps/shapes`, `apps/splash` | no | no | **No capture path, and none planned.** shapes keeps 4x MSAA deliberately and has nothing to regress. splash draws to the default framebuffer outside `engine_run`, where the screenshot lives, so it cannot be captured without porting. |
| `apps/sprites`, `apps/network` | particles / no | no | Capturable (`-x -f -S`) and 0 px run-to-run; both are 4x MSAA with TAA off under the 2D preset. Neither has a golden, so a capture against the previous commit is their whole proof. |
| `apps/pcb` | no | no | A repository of its own (`../apps/pcb`, cetra as a submodule); capturable the same way and 0 px run-to-run. It builds against the public headers from outside the tree, so it is the one consumer that tests the public surface. |
| `apps/forest` | no (deliberately) | no | **Not pixel-deterministic**: 34,991 px run-to-run at 60 frames / 3200x1800, 7,134 at 20 / 1600x900; it accumulates. With `--taa` it is far worse (about 10% of an 800x450 frame at 20 frames, measured on an older build: re-measure before citing). **The Hillaire sky is the source**: `--no-sky` drops it to 4 px and `--render-mode 6` to 0. Physics, geometry, scatter, LOD and textures are exact (two `--trace-player` traces are byte-identical), and every `forest-*` arm reads integers. For a 0 px claim use `--no-sky` or `--render-mode 6`, or assert on the submission integers. Nothing automated sees a change in sway phase: `windObjectPhase` hashes an object's position rounded to a whole unit, and the only wind fixture runs `phase_variation = 0`, which returns before that. **Its GPU timings are tight**: 0.23% run-to-run at 1600x900 and 0.8% at 3200x1800 with the runs back to back. **Interleave configurations (A/B/A/B)** rather than running A then B, and take each configuration's floor from samples adjacent in time to those compared; a sequence of other heavy runs between two samples measures drift, not jitter. |
| `assets/scenes/mask_fixture.cscn` | no | no | **The only visible ALPHA_MASK geometry in the corpus** (spec 11.31): an opaque quad, a MASK quad above the cutoff and one below it. **It is a golden**, the only one with visible masked geometry at the 4x MSAA default, where alpha-to-coverage is live. Two of its arms pass `--taa` to drop to one sample, because at four samples a fractional alpha becomes fractional sample coverage by design and the kept-quad equality holds only at one. What does not vary with the sample count is which quad is there at all: `mask-samples` asks that, rendering bare `--msaa 1`. |
| `assets/scenes/water_fixture.cscn` | no | no | **The water instrument.** A ramp descending toward the camera into the water over a flat bed, so one frame carries a shoreline, a continuous absorption ramp (the path grows as the sight line goes grazing) and a Fresnel reflection. 0 px x2 same build: the surface is a pure function of the frame clock. Read it as ratios: R/B falls 0.2168, 0.1461, 0.0963 with distance, and dry land reads 1.98. The frame also carries the horizon, which `water-horizon` reads as reach invariance and `water-farfield` as filtering; do not extend the absorption monotonicity read up into that band, where sky reflection legitimately raises R/B again. The fixture's own `water{}` block creates the surface and authors every key, so an arm names only what it varies and `--no-water` turns it off. **An arm overriding one field of a wave train must go through `_water_cscn_variant`'s one-level merge**: a plain `dict.update` replaces the whole nested object, and the fields it drops fall back to `create_water`'s defaults, which equal this fixture's values and so hide the mistake. Arms also drive `--water-bed dome` (shoaling), `--water-probe` (the surface query on both wave models, the GPU pass beside the Gerstner closed form it is checked against), `--water-fft-probe` (the spectrum's variance and the transform against its closed form) and `--no-water-lod` off this one file. Three arms author their own framing because the fixture's cannot see what they measure: `water-waterline` needs crests closing over the eye, `water-glitter` needs the sun dead ahead and lower, and `water-foam-persist` needs a 20 m/s sea over 90 frames and raises the level to 1.1 to drown the ramp. Its sun is a nested `sun` object, as `parse_env` reads it. **Every box that reads the shoreline is derived from the wedge's corners in the glTF** through `_water_ramp_band`, as heights on the ramp rather than screen rows. `water_submerged` cannot see caustics: from under the surface the bed does not come through the refraction path they are applied on. |
| `assets/scenes/beach_fixture.cscn` | no | no | **The surf instrument** (spec 11.44). A dome rising out of the sea: open water at the rim, the whole shoaling ramp, and a dry crown 0.6 units up, with a shoreline facing every direction, which is what lets `beach-shoreline` ask whether the surf is a ring. **The analytic bed is a flag, not a scene key**: without `--water-bed dome` nothing shoals, so every arm installs it. The mesh duplicates `render_dome_bed_height` deliberately (the water shoals against the analytic field, the mesh is what the eye sees it against), and the gate reads the waterline (r 5.66) out of the glTF rather than re-deriving the formula, so two copies are compared rather than three kept. **Its arms run Gerstner (`--water-waves gerstner`)**, for two reasons: crest foam is FFT-only, so the whitewater left is the shore band; and the spectral path's foam history accumulates per cascade texel and tiles the world, so the bed would change the frame out to the horizon. Read as R/B against depth: 1.0123, 0.7962, 0.5000, 0.3172, 0.2482 over depths 0.29 to 5.12 m, flat at 1.204 with `--no-water`. **Its `.cscn` says `"waves": "fft"` and no arm renders it that way**: opened by hand (`render -m assets/models/beach_fixture.gltf --water-bed dome`) it gets the spectral sea and a blown-out white ring over the shoal that no gate covers. **Do not read a brightness peak here as the shore band**: the sunlit dry crown is as bright (580-591 luma against 570-607), and the sun's glitter owns the right of frame, so brightness is read on the left. `beach-shoreline` therefore takes the argmax of \|with bed − without bed\| along each outward ray, which cannot read dry sand because the bed does not touch it. The group's arms are `beach-shoal`, `beach-shoreline`, `beach-surf-zone` and `shore-twin`, the last reading the swash film's CPU twin against its own ceiling. |
| `assets/scenes/fog_volume_mix_fixture.cscn` | no | no | **The σ-weighted fold instrument** (spec 11.40). Same geometry, camera and crop boxes as the fixture below; only the volume set differs. Left strip warm alone at density D, right strip white D/2 coincident with warm D/2: total extinction D on both, so transmittance and the depth mix are identical and the only variable is what the tint sum is made of. R agrees to four decimals, which is the control. Nothing else can tell the σ-weighted fold from a plain `S *= tint`: with no global fog `airSigma` is 0 and the fold is exactly that. Adding global fog does not make an instrument: the clear air in front of every box drowns the box before it dilutes its tint. |
| `assets/scenes/fog_volume_fixture.cscn` | no | no | **The local fog volume instrument** (spec 11.39). One emissive backdrop and two boxes of denser air at equal density differing only in tint, with a gap of bare backdrop between them as the control. It authors no global fog, which makes `fogvol-off` also a test that the volume armed the pass. **Needs 60 frames, not 30**: at 30 two runs of one build differ by 3/255 across the frame while the froxel accumulator settles; at 60 they are byte-identical. The classic `--fog` arming is 0 px at 30 on the same scene, so the instability belongs to arming by a per-frame published count, and is not explained. A white tint is an exact identity, and `--no-fog-volumes` returns box/gap to exactly 1.0000. |
| `assets/scenes/absorption_fixture.cscn` | no | no | **The volume absorption instrument** (spec 11.32). Three transmissive panels over one uniform emissive backdrop, same attenuation, thicknesses 0.0, 0.3 and 0.6. The backdrop is uniform because thickness also drives the refraction bend, so a patterned one would confound absorption with which backdrop each panel covers. Zero thickness is an exact identity (G/R 1.0000), and red is authored unabsorbed as the in-frame reference. |
| `assets/scenes/sss_msaa_fixture.cscn` | no | no | **The SSS profile-tag instrument** (spec 11.37). `.cscn`-only on `sss_fixture.gltf`: two profiles with opposite channel weights at equal radius, on spheres small enough that the silhouette is a real fraction of each. `sss_skin_a` is tag 1 and `sss_skin_b` tag 2 (slots follow the order of the materials block, and startup prints them). Only the tag-2 sphere can show the defect, because its silhouette averages 2 with the uncovered 0 and lands on 1; the tag-1 sphere averages toward 0 and is dropped, which makes it the control. **Read it by varying the profile count at a fixed `--msaa 4`, never 4x against 1x**: antialiasing dominates that comparison and moves the rim the other way, so the defect reads as absent. |
| `assets/scenes/wind_uv_fixture.cscn` | wind (deliberately) | no | **The UV1-as-wind-data instrument** (spec 11.51). Three identical quads differing only in TEXCOORD_1.y (1.0, 0.5, 0.0), which `wind_mode` 2 reads as a flex weight. 0 px x2 same build. Ships with a `_still.cscn` twin at strength 0, because wind has no CLI flag and a single frame cannot substitute: a linear ramp of displacement keeps the quads evenly spaced, so three quads moved by the same wrong amount look like three moved right. Read as travel in px (48.57, 31.80, 15.04). **The failure it guards is silent**: geometry arriving without UV1 gets phase 0 and flex 0 and still leans (the height-mask body term carries no flex), so a broken frame reads as a calm day. It is written in Python rather than exported from Blender because what cetra owns is assimp's two V-flips cancelling, which an assimp bump or a change to `uv_flip_flag()` would invert silently. Blender's own flip (`v_gltf = 1 - v_blender`, so author `UV1.y = 1 - flex` with the stem at `v = 1`) is on the far side of the file boundary and is in the spec. |
| `assets/scenes/wind_cull_fixture.cscn` | wind (deliberately) | no | **The wind-cull instrument** (spec 11.53), failing in two directions on purpose. Three wind quads behind the camera must be culled (exactly 3 of 6, 3 draws), and two whose import AABBs lie entirely outside the left frustum plane, one cloth and one vegetation, are blown back into view and must read 0 px against `--no-frustum-cull`. A culling arm is easy to write green, since "nothing visible was dropped" is satisfied by a build that culls nothing, so neither arm is safe alone. **The vegetation quad is what tests the mechanism `apps/forest` runs on**: at `turbulence: 0` with no `windMode`, `wind_max_offset` collapses to `strength × response` and none of `wind_bounds.glsl`'s coefficients is reached. Its UV1 is authored flat, not as a ramp: UV0.y runs 1 at the base and 0 at the top, so a height ramp makes the leaf term's `max \|uv1.y · uv0.y\|` 0. The geometry is derived: the generator computes the frustum half-width (4.4183) from the fov and aspect the gate renders at, states the quad's offset as a fraction of it, and asserts the fixture still tests something. Its light casts, which gives `cull-cascade` a row: it asserts the cascade culls zero (the `shadowcull-draws` idiom), since every mesh is inside the cascade fit; a cull wired to the wrong frustum passes `cull-wind` and fails this. |
| `assets/scenes/shadow_lag_fixture.cscn` (+ `shadow_lag_still.cscn`) | skeletal clip (deliberately) | yes | **The shadow-lag instrument** (spec 11.96), the corpus's first moving caster. A box holds still until t = 0.25, then slides along +X; the sun is overhead and the caster floats above the camera, so the only thing in frame that can change is its shadow and the read is a whole-frame compare. **It reads against a parked twin**, not an earlier frame of itself: consecutive frames discriminate only while the onset falls between the sampled pair, so a retimed clip would silently stop testing anything. Frame 15 against the twin is 0 px (the hold, the control) and frame 16 is not. **The motion has to be an authored clip**: a `.cscn` cannot express a moving object, and a schedule flag driven from the render app's update hook runs before the shadow pass and passes against the very build it exists to catch. Being skinned, it covers the pose half; the node-transform half is the `transform` group's, off a CPU probe. Falsify it with an explicit `--bin-dir` (see Which binaries a number describes). **Three geometry traps, all rendering as nothing rather than wrong**: a single quad casts no shadow, because the depth pass culls a face and a zero-thickness plane then writes no depth; the obvious winding over the ground's corners gives a −Y normal, so the plane is culled from above and the frame is empty; and a box whose faces share one index order has three wound inward, leaving vertical quads whose shadow under a vertical sun is a hairline, which sends you to the wrong pass. The generator asserts every caster triangle's normal points away from the box centre. |
| `assets/scenes/occlusion_fixture.cscn` (+ `_bare`, `_mat`, `_inside`, `_near`, `occlusion_scatter`) | no | no | **The occlusion instruments** (spec 11.98), and the only fixtures where anything stands behind anything: every other scene is fully visible or frustum-culled, so a culler that hides on-screen geometry, or hides nothing, is green everywhere but here. A portal room: four wall slabs framing a window, 300 distinct heavy meshes behind it (culled exactly, read as an integer off the submission table), three visible through the gap, and one marginal mesh whose projection pokes into the window by 2.3 mask texels, wide enough that conservatism must keep it and narrow enough that an inward rounding deletes it. Five `.cscn` variants select five configurations from one `.gltf`: authored boxes; the material row; no occluders key at all (the empty-context early-out no flag reaches, hence `occl-bare`); the camera inside the room; and the camera almost touching a slab, where the near plane clips the wall open and only front-face classification keeps the raster honest (`occl-near`). The scatter twin is one shared mesh in one instanced run, pillars hiding every third column: draws go up while triangles go down. **Goldens are blind to this feature by construction**, since correct occlusion changes no pixel; the group's 0 px identity arms against `--no-occlusion-cull` are the coverage. The generator asserts every property an arm depends on, including that the app's model recentre is a no-op, since the authored world-space boxes would not follow a shift. |
| `assets/scenes/skinned_cull_fixture.cscn` | no | no | **The posed-bound instrument** (spec 11.53), and the corpus's first animated skinned asset (`skinned_instance_fixture` is skinned but unanimated, deliberately, since it is about which program a mesh takes). One joint swings a panel from x ≈ −6.4 (against the same 4.4183 half-width) to centre frame, read as 0 px against `--no-frustum-cull`, plus an aimed-away arm at 2 of 2 culled. **The clip holds the swung pose rather than ending on it**: the render app plays looping, so a clip ending at the read frame wraps to the bind pose exactly when measured, and reads as the panel being culled. |
| `assets/scenes/varying_fixture.cscn` | no | no | **The partial-coverage instrument** (spec 11.38). 24 thin splayed fins with fanned normals and a base-to-tip colour ramp, a full-frame backdrop, and one quad whose UVs are authored outside [0,1]. 0 px x2 same build. Read with `--render-mode 12` at `--msaa 4` against `--msaa 1`: the one-sample frame is ground truth, because coverage is a single centre test there, so extrapolation is impossible. Every element is load-bearing: a big flat quad (`parallax_fixture`, which reads exactly 0) has almost no partly covered pixels, and a constant normal extrapolates to itself. Opaque on purpose: alpha-to-coverage is gated on `msaa_samples > 1`, so a MASK material would change with the sample count being varied. |
| `assets/scenes/lut_fixture.cscn` (+ four `.cube` files) | no | no | **The colour-grading LUT instrument** (spec 11.58). A 24-patch chart (a grey ramp along the top, then 18 colours over the RGB cube), every patch emissive over a black base. A LUT is indexed by the pixel's own value, so the fixture must control where in the table each patch lands, and an emissive patch maps through exposure and the tonemap alone. 0 px x2 same build, range 32..244 with nothing clipped, all six greys exactly r == g == b. **The four tables have three sizes (9, 17, 33) on purpose**: size is the one header field the loader must act on, and an all-33 corpus would let a hardcoded 33 pass. `lut_identity` is the no-op; `lut_swap` (linear, so both interpolants agree exactly) isolates the table from the interpolation and is the only one that catches the data block read blue-fastest; `lut_steep` is the realistic-size interpolation read; `lut_neutral` is the probe the tetrahedral claim rests on. **That probe must be neither separable nor channel-symmetric**: a separable table preserves greys under trilinear because the off-axis weights factor out, and a symmetric one because the corner pairs cancel, so both measure exactly zero. It lifts red alone, and the generator asserts against both wrong shapes. The gate reads through its own tetrahedral interpolator, not the generator's closed forms: importing the generator would make ground truth a function of the thing under test, and it writes at module scope, so it would rewrite the committed assets mid-gate. |
| `assets/scenes/postfx_convergence_fixture.cscn` | no | no | **The metering instrument** (spec 11.52), and the only fixture with `"auto_exposure": true`. Every other one pins exposure, by CLI or by authoring `post.exposure` with no `auto_exposure` key, which `cscene_apply` turns into a pin, so no golden can see a metering change. The `exposure` group reads it through `--exposure-probe`, not as pixels. **Read only converged frames**: the meter reads a pre-exposed frame, so its value sits inside a feedback loop until the scene settles, here at frame 91 (the gap touches the deadband around 18 and is pushed back out twice first), so the arms use 150. Gain: 0.490 uniform, 0.531 centre-weighted, 0.842 spot. |
| `assets/scenes/cornell_box.cscn`, `assets/scenes/cornell_leak.cscn` | no | no | **The reference pair for derived area lights, deliberately not converted to use them** (spec 11.49). Each authors a hand-placed light beside an emissive quad emitting a token 1 nit, and three arms generate a derived twin at run time (`cscn_copy`) and compare: does a fitted panel light the room the same (`emissive-match`), lift the indirect term the same (`emissive-gi`), add the same to a reflection probe (`emissive-spec`). Convert the fixture and there is no authored side to compare against; the twin is generated rather than committed so the two halves differ only in what the mutation touches. **Two traps.** `cornell_box`'s authored light sits at y 1.97 and its quad at y 1.98: `emissive-fit` asserts 1.98, the mesh's value, since the fit is a fit of the mesh, and `EMISSIVE_MATCH_EPS` 0.03 is sized for that centimetre. And `cornell_box` cannot see an area panel's shadow at all (toggling `cast_shadows` moves nothing, for the authored light too), which is why `cornell_leak` exists: one room cut in half by a partition, where an unshadowed derived panel reads 0.41 of the lit half through the wall and a `light_overrides` entry takes it to exactly 0. |
| `assets/scenes/cornell_rooms.cscn` | no | no | **The clustered specular probe instrument** (spec 11.70), the fourth variant `gen_cornell_rooms.py` emits. Two rooms joined by a doorway over **one floor mesh spanning both**: per-draw probe selection hands one mesh a single probe and lights half of it with the wrong room's reflections while every per-room measurement still passes, so a split floor could not tell per-draw from per-fragment selection. **The doorway is at the front**, not the middle, because a middle doorway puts the partition's near half in front of the band where the two probes overlap, the only place a seam can be seen. Lit by spots rather than area panels, for `cornell_leak`'s reason; the partition carries its own thickness (`ROOMS_PARTITION_HALF`), since 6 cm is thinner than a shadow texel here. **It authors its own `environment` block**, which is load-bearing: a probe cannot be created without a precomputed IBL, so without it the file opened by hand renders with its probes refused. Read as the dark room's floor against a variant that moves probe B's capture point into room A and changes nothing else, which isolates selection from storage (1.82x), with the lit room as an in-frame control. A screen-space artifact that comes and goes with an unrelated toggle is worth suspecting as a discontinuity in a term that toggle moves: the stair-stepped band once seen on this floor was the specular-occlusion cone, not the probes. |
| `assets/scenes/board_fixture.cscn` | no | no | **The true-scale instrument** (spec 13.57): a circuit board in metres (a 0402 is a millimetre long) at a typical size of 0.001 with fitted planes, lit by three area panels and a lamp, on a table. 0 px x2 same build. The golden is the three-quarter view at a pinned exposure of 0.0088. Its three views are `scale-twin` rows, run grown 1024 times. `fit-clip` works its planes out from its glTF. **Contact shadows reach no light on it**: its panels are area lights and its lamp casts a map of its own, so the frame is byte-identical without `--contact-shadows`. Generated by `gen_board_fixture.py`, whose header carries the other two views' flags. |
| `assets/models/emissive_fixture.gltf` | no | no | **The derived area light instrument** (spec 11.49), read through `--emissive-light-probe` rather than as pixels, because a panel half a metre out or sqrt(2) too wide still lights a room plausibly. Five emitters, each answering one question. `emissive_quad` carries the only emissive texture in the corpus, half black and half white, so the 1x1 top-mip readback has a known answer (0.5). `emissive_box` is a closed cube whose faces cancel: planarity 0. `emissive_strip` is 2.0 x 0.25 rotated 30 degrees in its own plane, the case the covariance principal axis gets wrong and an axis-aligned bound reads as 1.8571 x 1.2165. `emissive_split` (two strips, fill 0.2857) and `emissive_ell` (fill 0.51) are perfectly flat (planarity 1.0) and show that planarity does not reject non-rectangles; two, because disjoint and concave fail for different reasons. **Every emitter lies on z = 0, so two overlapping in xy z-fight**; the constraint is written beside the generator, `gen_emissive_fixture.py`. The lighting comparison lives on `cornell_box`, which has a hand-authored panel to compare against. |
| `assets/scenes/overdraw_layers.cscn`, `assets/scenes/overdraw_tiles.cscn` | no | no | **The depth-complexity instruments** (spec 11.31). `layers` is N full-frame opaque quads submitted far to near, so the counter must read exactly N. `tiles` is a 24x24 grid of distinct meshes at complexity 1.00, so the prepass has nothing to reject and can only cost. |
| `assets/scenes/ies_fixture.cscn` (+ five `.ies` files) | no | no | **The IES photometric instrument** (spec 11.57). Every candela in it is a closed form of its two angles, because an IES reader cannot be tested against a picture (a table resampled off the wrong plane still lights a room) or against a real `.ies` (nobody knows what a real luminaire's candela at 37.5 degrees should be). Five files: one horizontal plane (rotational, most real downlights), 0..180 (bilateral, the case a modulo fold gets wrong), 0..90 (quadrant, the second mirror), 0..360 (none, where a reader assuming a mirror renders the far half backwards), and `ies_tinytail.ies`, whose 90-degree candela is 1.2e-6 rather than 0, the only input the tail snap acts on. The horizontal ramp is monotone on purpose: a symmetric one reads the same whether the reader mirrors or wraps. `gen_ies_fixture.py` imports and runs `tools/gen_ies_table.py` rather than reimplementing it, holding the tool, the C reader and the shader to one arithmetic. The scene authors no intensity, which exercises the peak-seeding path. **Its light is a point**: `spotConeFactor` returns 1.0 for every non-spot, so on this scene multiplying the profile by the cone and replacing the cone with it are byte-identical, and `ies-replace` builds its own spots to tell them apart. Three of the six arms read `--ies-probe` and three render; **neither half substitutes for the other**, because the shader is a third implementation of the table and no probe reaches it. |
| `assets/scenes/contact_local_fixture.cscn` (+ `contact_local_bare.gltf`) | no | no | **The local-light contact shadow instrument** (spec 11.56), the only scene with no directional light. A cube on a plane with two point lights mirrored about a receiver strip in front of it, same height and distance, both at x = 0, so along z = 0.55 they have identical falloff and N.L and their weights differ by exactly their authored intensities. One is occluded by the cube and one is not, so a contribution-weighted fold is predictable in closed form and an unweighted one does not move. **Read the raw bytes of `--cs-debug`**: the debug view returns before the display encode, so decoding sRGB reports 0.6326 as 0.3564. **Sample the line, not a box**: the mirror holds at z = 0.55 only, and 0.02 either side already costs 2.8% of the weight ratio. The cube's front face, which the occluded ray crosses, faces the camera at full incidence: a screen-space march reads no occlusion from a surface the camera cannot see. Strip 0.6326 against an in-frame control of exactly 1.0000. Both practicals are map-less. The mixed path is `contact-mixed`'s: it adds a shadow-casting directional as a gate variant, then a mapped lamp beside it (0.6326, 0.8192, 0.8957), each step one light joining the fold's denominator; drop skipped lights from the denominator and the third read collapses onto the second. The bare twin drops the cube from the same vertex arrays, so an arm swapping the model varies the occluder and nothing else. **The binding constraint in its generator is the screen clamp, not the march reach**: `MAX_UV_LEN` cuts 0.8 units of world march to 0.72 here, so a fixture checked only against `csDistance` can be short by a factor of two. |
| `assets/scenes/layer_fixture.cscn` (+ 7 painted maps) | no | no | **The layered-surface instrument** (spec 11.60). A floor and a wall sharing one layered material; the perpendicular normals are the point, since the projection is world-aligned and one plate tests one axis of it. 0 px x2 same build. Every arm reads `--render-mode 6`, where the shader returns `linearToSRGB(factor * sRGBToLinear(blend))` against a white factor, so a correct renderer hands back the painted byte exactly: `layers-select` reads 128, 48 and 208 off by zero. A lit read would test the BRDF as much as the blend. **Layer 1 is a checker, not stripes**: the wall sits at constant z, so a projection frozen on Y still varies stripes correctly along x. It is also the only layer with a relief surface map: a flat tangent normal makes `triplanarBlendNormal` an exact identity, so its axis-sign flips could be deleted unseen, and per-layer roughness and AO need a map of their own to be covered. **UV0 is authored different from UV1** (doubled and v-flipped), or "the splat reads UV1" is untestable. The flat map is shared by layers 0, 2 and 3, which exercises `material_texture_layer_for`'s dedup: the build logs 7 layers for 5 files. |
| `assets/scenes/layer_vt_fixture.cscn` (+ its own splat; maps shared with `layer_fixture`) | no | no | **The composite-cache instrument** (spec 11.66). A floor and a 45-degree ramp on one layered material whose splat is authored in world XZ, which is why it exists beside `layer_fixture`: that one's splat is UV1-space and the cache serves world-XZ splats only. 0 px x2 same build. **The ramp is the anti-vacuity**: on the flat floor the cached path is byte-exact by construction (macro of top-mip means, detail ratio of map over mean), so a floor alone cannot tell the cache on from the flag doing nothing; the ramp sits exactly between the Y and Z projections, where the two paths legitimately differ. Cached against per-texel differs by a few thousand pixels (splat-resample edges and the blend crossover) while every column centre reads exactly 128/48/208, and at a forced 8x8 atlas the columns still read exact while the checker still alternates 32/224 through the detail term. Splat rows: ramp = checker alone, floor near band = the four selection columns, floor mid band = the layer 2-into-3 transition the invalidate arm watches move across a `--layer-blend-at` re-bake. It reuses `gen_layer_fixture`'s painted maps by name, so the codes the arms predict have one source; its generator asserts the coarse texel exceeds a checker period and that columns align to the coarse texel grid. **The ramp draws first, and the order is load-bearing**: the feedback vote pass's occlusion claim rests on its depth test, and with the occluder drawn last painter's order alone hides the hidden surface, so deleting the depth test would change nothing. The road arms (spec 11.68) each build a runtime `cscn_copy` variant, so the fixture stays road-free and the golden and the other arms render an unmutated file. **A whole-frame pixel count cannot see a road**: two legs of the composite cache differ by a code or two across every flat surface, so road arms read bytes. **An arm that moves the camera must move the projector too**: `_projector` builds from `_cscn_camera`, which reads the committed scene, so an arm passing `--cam-eye` samples pixels the render never drew; pass `_cscn_camera(name, eye=..., target=...)`. |
| `assets/scenes/decal_fixture.cscn` (+ three painted images) | no | no | **The clustered decal instrument** (spec 11.73). A floor, a wall facing the camera and an oblique plate, with a poster on the wall and a scorch on the floor. 0 px x2 same build. **The poster is painted in four quadrants**, because a single-colour or symmetric patch cannot test a projector: an image laid backwards and upside down paints the same frame. The generator asserts the four codes separate in every channel, so each wrong transform gives a different wrong answer. **The oblique plate is the other anti-vacuity**: it sits inside the poster's box and past its angle fade, so a correct build leaves it at its own substrate, and without the facing test the poster smears onto it. `decals-albedo` alone passes a build with no facing test, and `decals-angle-fade` alone passes a build that projects nothing; neither is safe without the other. **The plate stands in front of the wall, so where it sits decides what every arm reads**: read points are generator constants, asserted clear of the plate, inside the opaque region and on the surface being read (a point at the box's centre hangs a quarter unit in front of the wall, which is wrong at an edge). Ground truth is painted and read as a byte through `--render-mode 6`, and the substrates are `pow(1/2.2)` of the authored factors, not the piecewise sRGB curve, which is two codes off on the floor. The half-texel UV inset has no arm: the images carry a transparent margin, so a wrapped blend is between two transparent texels and the inset moves nothing. The edge feather starts at 0.35, and the generator asserts the feather band overlaps the margin, or the margin retires the alpha before the ramp begins. The scorch's normal map tilts the surface 39 degrees, so the tangent frame is observable. Its `environment` block is `"mode": "sky"`; `"sky": true` parses, warns and renders a scene whose probes would be refused. |
| `assets/scenes/purkinje_fixture.cscn` | no | no | **The scotopic shift instrument** (spec 11.83). Columns are a luminance ladder over 7 stops, rows are chromaticity held constant, which is why `lut_fixture` (the right idiom, emissive over black, but all colours at one magnitude) could not be reused: this feature turns on how the drain varies with luminance. 0 px x2 same build. Six rows: neutral (grey is an exact fixed point of desaturation, so anything moving it is the tint; a desaturate-only build reads exactly 0 there), red (rods see it worst, V'(610) about 1/32 of V(610)), green, blue (the row a tint-only build pushes the wrong way), a mid-saturation warm for the cube's interior, and the rod tint's own chromaticity as a near fixed point that must move least. **The `.cscn` authors `post.exposure` well above 1**: absolute radiances stay scotopic, which the rod ramp reads, while displayed codes land in readable 8-bit territory; drop `oneOverPreExposure` and the ladder shifts by `log2(EXPOSURE)`, which `purkinje-absolute` reports. The generator asserts the ladder straddles the shipped ramp, and that **the chart is a minority of the frame** (0.140 against a 0.20 bar): the meter keeps the brightest 30% of the population, so a chart filling that tail makes the metered mean its own bright rungs, closes the global gate, and renders its subject unshifted. For the same reason `moon_fixture` at its shipped 49x reads a global gate of 0, so `purkinje-lamp` renders a 12x variant. Read colour at rungs 5 and 1 (7 stops cannot all be legible in 8 bits; the dimmest rung lands at code 2), with `--no-purkinje-acuity`, since acuity pools across patch boundaries and lifts a dim rung toward its brighter neighbour. |
| `assets/models/clearcoat_fixture.gltf` | no | no | **The clearcoat instrument** (spec 11.86). 0 px x2 same build. Three spheres: a coated dielectric, the same base with no coat, and a coated near-black metal. **The uncoated sphere reads exactly black under `--clearcoat-debug`**, which a normalized vector cannot encode, so it catches a build that writes the geometric normal everywhere. Both coated materials carry a procedural carbon-fibre weave as a clearcoat normal map, embedded as a data URI, the corpus's only tangent normal reaching `pbr_frag` through its own path: assimp exposes it at `aiTextureType_CLEARCOAT` index 2, which `import.c`'s index-0 mapping table cannot reach, so it takes a bespoke read. **Its two debug reads are orthogonal and neither substitutes**: with the normal's Z rebuild removed mean blue falls from about 238 to 18 while the weave contrast barely moves; with the map ignored the contrast falls from about 6 to 0.4 while mean blue holds. The golden auto-frames to match its two siblings (`sheen_fixture`, `specular_fixture`), and the gate group pins its own tighter camera so the weave is big enough to resolve a twill cell. |
| `assets/scenes/texcomp_fixture.cscn` (+ 3 painted maps) | no | no | **The block-compression instrument** (spec 11.85), and **the only fixture that minifies**. Every golden is unchanged with every mip above level 0 painted black, since they sit near the camera, while most of a compressed texture's bytes live in the chain. This is a plane running 1 to 400 units out under a camera aimed down it, so one frame sweeps from a few texels per pixel to hundreds. 0 px x2 same build. Three maps, one per format, each shaped so its format can fail: the albedo is a hue wheel over a luminance ramp, because DXT quantises to RGB565 with four colours a block and errs worst where a block spans hues at one luminance; the normal's x and y use different frequencies and a phase offset so no block sees dy as a function of dx, which tests BC5's separate per-channel endpoints; the roughness is one channel, since a three-channel grey would exercise the decline path while looking like it tested BC4. **Its arms are bounded both ways**: normals stored as DXT5 instead of BC5 leave the byte ledger identical, so `texcomp-format` reads the format name, and `texcomp-colour` carries a floor because a dead colour switch reads RMSE 0 and would pass a ceiling alone. Its camera key is `eye`; `position` parses, warns nowhere and leaves the auto-framed camera. |
| `assets/scenes/alpha_ladder_fixture.cscn` (+ 2 generated cards, + `alphacov_dots.png`) | no | no | **The mip ladder** (spec 11.101): three rows of facing cards, each card one mip deeper (the UV range doubles per card), on a flat backdrop, so one frame shows every regime of an alpha-tested texture under one AA state. Foliage blobs, hard specks and the alphacov lattice: the first two never dither before the 1x1 tail, the lattice does at mip 3, and `ladder-fire` pins that from the texture probe without a render. **Its numbers are calibrated to the gate's 800x600 framebuffer**: a hand render at `-W 800` is 1600x1200 on a HiDPI host and shifts every card one mip finer. Its churn arms take both frames from one process (`--screenshot-every 61 -f 62`), because two jittered processes are not byte-comparable; `ladder-gate` is the one two-process 0 px identity. It shows the jittered lookup cutting one-sample churn 8x, and that churn is not monotonic in the sample count: two samples churn more than one, and the coverage four samples preserve is coverage the first two rows never have at a visible mip, which is why alpha-tested materials stay at one sample. Every arm states its own sample count (`ladder-churn` passes `--msaa 1`) rather than inheriting the render app's TAA policy. |
| `assets/data/terrain_fixture.r16` | no | no | **The heightfield instrument** (spec 11.59), the only fixture that is not a scene: a 256² headerless 16-bit heightmap read by `forest --heightmap` and two probes. **Ground truth is painted**: every sample is a closed form of its two normalised coordinates, so the gate knows what the sampler owes at any point, including between nodes where a filter is decided (0.47 codes on the lattice, 53 at a cell midpoint). **It carries a near-Nyquist band**, 7.5 samples per cycle, the regime forest bakes in: a smoother field leaves bilinear and bicubic a few codes apart, and `terrain-bicubic` passes against a bilinear sampler. The generator asserts that separation, asymmetry (a transpose must move more than 500 codes; it is the commonest heightmap bug and renders as plausible terrain) and headroom. **Probe rows snap onto nodes**, so the mapping arm and the filter arm do not contaminate each other: on the lattice every correct sampler is exact, so a transpose cannot hide behind interpolation error. |
| `assets/scenes/moon_fixture.cscn` | no | no | **The moon, framed** (spec 11.82), the one fixture that exists to be looked at rather than measured: a near-full disc filling most of the view over the star field. 0 px x2 same build. Loads with no flags: it authors its own sky, sun, stars, moon, camera and pinned exposure. It borrows `aerial_fixture.gltf` because a `.cscn` must name a model, and none of it is in frame (the horizon sits 20 degrees below the bottom edge). **The sun's azimuth is 20 degrees off opposition**: dead full puts the light head-on, where Lommel-Seeliger correctly flattens the relief, and 20 degrees lands the terminator just inside the right limb, where the crater walls shade themselves. **The size comes from `moon.size 49`, not a wider `sun_disc_deg`**: the latter authors a 26-degree sun disc, invisible only while the sun is below the horizon. |

### The recipe

```bash
# rigged + animated + normal-mapped, byte-identical ACROSS BUILDS:
./out/bin/render -m my_models/raiden/source/raiden_textured_rigged.glb \
    -t my_models/raiden/textures -e my_models/studio_small_03_8k.hdr \
    -a assets/models/strut_walk.fbx -s assets/models/t_pose.fbx \
    -x -f 120 --no-springs --no-auto-exposure -E 1.0 -S out.ppm
```

The clip is the committed one, `assets/models/strut_walk.fbx`, not the copy under `my_models/`,
which is gitignored and so reproduces for nobody else. The two pose this rig identically: they
differ only in bone-name prefixes, which `normalize_bone_name` strips.

- **`--no-auto-exposure -E 1.0` is required.** It pins the exposure, which otherwise drifts with
  async-load order before it converges (see "What breaks determinism").
- **`--no-springs` is not needed for determinism**, since springs ride the frame clock. Keep it so
  a new baseline compares with the existing ones, and use it on its own to isolate spring motion in
  an A/B.

Two builds of the same renderer give 0 px on this recipe.

### What moves the recipe's output

Before concluding that a diff against an old capture is your change, check whether it is one of
these.

- **The hair is sensitive to last-bit arithmetic.** The rig carries an alpha-blend mesh, so it is
  the one real asset that goes through OIT, and the moment reconstruction is ill-conditioned where
  layers sit at one depth, which hair does. Reordering the shader's arithmetic moves the hair
  silhouette by about 0.1% of the frame and nothing else. `--no-oit` and `--no-oit-moments` take the
  hair out of that path. The analytic OIT gate is the instrument for OIT itself: it does not move
  with such a change, which is how a hair diff is read as sensitivity rather than regression.
- **Output dither is on by default**: the 8-bit write carries a +/-1 LSB triangular dither, so
  most of the frame differs by exactly one code from an undithered one. Use `--no-dither` for any
  comparison against an undithered capture, and for any gate that reads single pixels or
  high-frequency residuals rather than whole-frame structure.
- **`--translucent-shadows` is opt-in** and changes the groom and the shoulder under it: an
  ALPHA_MASK strand mesh casts, and the ALPHA_BLEND ones attenuate rather than casting solid. The
  recipe's `-e <hdr>` is what makes it visible, since the HDR's key lights cast and the fallback
  three-point rig does not; most of the fixture corpus cannot see this feature.
- **A shader edit can move pixels by codegen alone, and no flag reaches it.** A change to how a
  vertex stage receives its inputs (an include, a select) lets the driver schedule the float math
  differently: isolated specks at exactly 1/255 with no structure in the diff map. Re-bake the
  goldens it moves. `--no-instancing` does not reach this, since it governs whether draws are
  batched, not the shader; batched against unbatched is itself 0 px, the UBO carrying the same
  floats into the same arithmetic.
- **An ALPHA_MASK fragment above the cutoff is fully opaque**, as glTF's binary MASK means, and the
  opaque lane does not blend it. A capture from a build that blended masked fragments toward the
  clear colour differs across the whole groom.

### Masked materials and the mip chain

The masked shading path is covered by the `mask_fixture` golden. **The mip chain of a masked
texture is covered by no golden**: coverage preservation only acts where a cutout minifies,
`mask_fixture` declares no textures, and `translucent_shadow`'s masked albedo is a linear alpha
ramp, which a box filter leaves a linear ramp. `assets/scenes/alphacov_fixture.cscn` and the
`alphacov` group are the whole instrument.

- **Its arms are bounded in both directions**, because saturating distant cutouts solid pushes the
  mid/near ratio up, and a bound only below passed a chain that did exactly that.
- **`alphacov-deep` reads a camera-facing quad by mean luma**, not a count of bright pixels.
  Grazing anisotropy averages a sparse dither into a smooth low-alpha field the sharpened test
  deletes, so a count reads the distributing chain lower than a vanishing one. The grazing loss is
  the technique's ceiling, not the arm's.
- **A rewritten level 0 has no arm**: the dot grid is binary there, so the fixture cannot see it.
  It is checked at the cook artefact's bytes instead.

**`apps/tree` and `apps/forest` are the only in-tree consumers and neither is a golden**, so a
change to the mip chain moves both leaf atlases at every level with nothing automated able to see
it; the `forest-*` arms read submission integers. Compare canopy captures by hand before believing
a green suite on anything touching `texture_alpha_coverage`.

- **Do not use `assets/ivy_arcade` for that A/B.** It is not pixel-comparable: about 9,000 px
  between two runs at 800x600, pinned or not, so anything under that on it is noise.
- **Forest's floors that work** are `--render-mode 6` at a fixed camera (0 px) and `--no-sky`
  (40 px), both at the suite's wide framing, `-f 20 -W 800 -H 450 --no-fog --cam-eye 0,300,700
  --cam-target 0,-20,0`. The 4 px `--no-sky` figure in the asset table is at another framing.

### Capturing a before

Capture the "before" from the base commit before starting work (`git stash`, build, render,
`git stash pop`). Both builds must be complete: **never pipe `./build.sh` through `head`**, which
kills the build mid-link with SIGPIPE and leaves a stale binary that renders the old code.

**Run both legs `--no-cook` when the change touches anything a bake produces** (spec 11.99). The
cook is keyed on a bake's inputs, not on its code, so a changed bake whose recipe version was not
bumped computes the same key, the after leg renders the before's artefact, and a real change reads
0 px.

For a pure refactor there is a stronger check than pixels: diff the generated GLSL.
`out/generated/shader_strings.h` is reproducible (sorted), so unescaping both versions and
diffing them with comments stripped proves no shader code changed, without a renderer.

### The gamepad device path

The game layer reads a pad through a seam: one function fills GLFW's standard layout for a slot
(spec 11.109). The `gamepad` gate group puts a scripted reader in the seam and verifies everything
above it. **Nothing in either suite exercises GLFW's real joystick backends** (evdev, IOKit,
XInput); macOS has no supported way to fake a pad, since a virtual HID device there needs a
DriverKit extension.

**The mouse is in the same position.** The pointer has the same kind of seam, `--input-script`,
through the same frame-range parser, so the `camera` group asserts a drag, a shift-pan and a wheel
notch with no hand on a mouse: the drag state, the ray picking and the app's forwarded callback all
run as they do for a real event. **GLFW's real cursor and button path is exercised by nothing.**
To check it, run `./out/bin/render -m assets/models/puppet.gltf` windowed, drag, shift-drag and
turn the wheel.

Two ways to close the pad path, each about a minute once the hardware or the machine is there:

**A pad on this Mac.** Pair any Xbox, PlayStation or Switch controller over Bluetooth
(all three are in the bundled SDL database), run `./out/bin/gametest --trace-player`
windowed, push the left stick and press A, and read the trace: the `move` column follows
the stick through the dead zone and `Jump!` prints once per press. The startup log names
the pad (`gamepad 0 connected: <name>`), and `(no mapping)` after it means the database
has no row for that GUID; pass a newer SDL `gamecontrollerdb.txt` with `--gamepad-db`.

**A Linux machine with no pad.** The kernel's `uinput` lets a process create an input
device the system serves like any other, and GLFW's Linux backend reads
`/dev/input/event*` with an inotify hot-plug watch, so a virtual pad is indistinguishable
from a real one, provided it matches a database row, which GLFW does by the full GUID.
Read `cetra/src/ext/glfw/src/linux_joystick.c` before writing the device. What it needs:

- **Bus 0x03, vendor 0x045e, product 0x028e and version 0x0104**, all four non-zero.
  GLFW builds the GUID `030000005e0400008e02000004010000` from them, which is the
  "Microsoft X-Box 360 pad" row of `mappings.h`. A zero version takes the other branch of
  that `if` and yields a name-keyed GUID that matches nothing, so the pad connects as a
  joystick and `glfwGetGamepadState` returns false for it.
- **Exactly eleven key bits, in ascending code**: `BTN_A`, `BTN_B`, `BTN_X`, `BTN_Y`,
  `BTN_TL`, `BTN_TR`, `BTN_SELECT`, `BTN_START`, `BTN_MODE`, `BTN_THUMBL`, `BTN_THUMBR`.
  GLFW numbers buttons by walking the key bits from `BTN_MISC` upward, so the row's
  `a:b0 ... rightstick:b10` holds only if those are the bits and nothing else is set:
  `BTN_C` or `BTN_Z` between them shifts every later index.
- **Six absolute axes** `ABS_X`, `ABS_Y`, `ABS_Z`, `ABS_RX`, `ABS_RY`, `ABS_RZ`, each with
  range information (the backend queries `EVIOCGABS` per axis and skips one it cannot),
  which the row reads as `leftx:a0 lefty:a1 lefttrigger:a2 rightx:a3 righty:a4
  righttrigger:a5`; and **the hat pair** `ABS_HAT0X` / `ABS_HAT0Y` for the dpad.
- **A `SYN_REPORT` after every batch of events**, or nothing is delivered.
- **Two permission grants**: writing `/dev/uinput` to create the device, and reading
  `/dev/input/event*` to let gametest see it (the `input` group, or a udev rule).

The tooling is `python3-evdev` (Ubuntu's universe repository) and `modprobe uinput`. The
script creates that device with `evdev.UInput`, then replays the scripted-pad text format
from `cetra/src/game/input.h` at a wall-clock frame rate, writing each range's buttons and
axes and a `SYN_REPORT`. Then run `./out/bin/gametest -x --trace-player` with no `--pad-script`,
so the default reader, `glfwGetGamepadState`, is the one that sees it. The inputs to replay are
the three gate scripts (`pad_edge.txt`, `pad_legs.txt`, `pad_off.txt` in the gates' temp
directory under `--keep`, or the strings in `run_gamepad_gate`), and the `gamepad` group's
expected values are the answers. It is a recipe and not a committed tool because it cannot be run
here, and this repository does not keep code nobody has run.

### The audio device path

The audio subsystem's seam is the device (spec 12.0). A windowed run opens the OS device and
mixes on miniaudio's own real-time thread; a headless run opens none (`noDevice`) and renders the
mix offline, which the caller pulls with `audio_system_read_pcm`. Everything above the device --
the buses, the voices, the spatialization -- is what the `audio` gate group verifies, off that
offline PCM, so it needs no sound card and is a pure function of the frames pulled.

The `audio` group runs ten arms through `gametest --audio-probe`, each a headless offline render
measured as per-channel RMS:

- **onset**: a tone is silent before it is played and energetic after.
- **pan**: a source to the right is louder in the right channel, and to the left in the left; a
  ratio each way, so a swapped channel fails.
- **distance**: the same tone is louder near than far and still audible far (attenuation, not a
  cutoff).
- **master**: the master bus at 1 passes energy and at 0 passes silence.
- **decode**: a WAV the gate synthesizes, with no committed binary, loads from a file and decodes
  to energy.
- **noise** (spec 13.9): white, pink and brown are each loud, and each darker than the last.
- **zone** (spec 13.33, as are the next three): a tone through a link of 0.25 is a quarter of
  itself, and a sound decoded from a file and never placed is whole, being 2D (spatialized from
  the start, it would sit at the origin, in the tone's zone).
- **zone-path**: a direct link of 0.1 against a way round at 0.5 and 0.5 gives the best path,
  0.25.
- **zone-start**: the first update lands a gain outright whatever its delta, and a tone placed
  after it starts at its zone's gain.
- **zone-fade**: the listener stepping into the tone's zone eases it up, every window where the
  0.4 s fade predicts; an opened link lets it through whole; a voice started in another zone is a
  quarter of itself from its first window.

The tone arms drive procedural `ma_waveform` tones and the noise arm `ma_noise` beds through the
real spatializer, so the layer is exercised end to end with nothing on disk but the decode arm's
temporary WAV, which the zone arms reuse. **The voice pool and a source's several sounds (spec
13.17) have no arm**, by decision; they were checked with silent's offline dumps, `--cat-say`
against `--no-cat`. Nor does an `AUDIO_SOURCE`'s zone gain, which is the held sound's, eased by
the same call.

**No suite covers the OS device path**: that a real speaker produces the sound on each platform's
backend (CoreAudio, ALSA/PulseAudio, WASAPI). The offline render proves the mix, not the driver,
and says nothing about whether it sounds right. Closing it is a listen: run `./out/bin/gametest`
windowed on each OS, jump (a high beep), spawn a box (a low beep), and walk the camera round the
door, whose looping tone should pan left and right and fade with distance. The startup log names
the mode (`audio: device, 2 ch @ <rate> Hz` with a card, `audio: offline (no device)` headless).
**Heard on macOS (2026-09-10)** through CoreAudio. **Linux and Windows are owed** the same listen.

### The idle wait's real events

Idle mode (spec 14.1) waits for window events, and a headless run has none. So the `idle` group
proves everything but the real ones: requests from threads, timed wakes, the loader's wake, the
UI's and the debug GUI's own requests, a scripted pointer, the clocks across a wait, the headless
end, and that a 3D scene is drawn every frame as without idle mode. It drives them through
`apps/canvas --idle` and `apps/render --idle`.

**A hidden window is woken once in its first tenth of a second on macOS**, a spurious return from
`glfwWaitEvents` that GLFW's documentation allows. A headless wait asks again after any wake, but
a request already made is then found, so work ending inside that tenth is drawn even with the
empty event or the loader's wake removed, and an arm would pass on it. So the arms' late work --
the board's thread, the late load -- starts on the frame a 0.5 s timed wake draws, past it. With
the empty event or the loader's wake removed, those runs wait for good.

**No suite covers real input waking the wait.** In a window any wake draws a frame, so every event
does, with nothing for a callback to mark. Nor does any suite cover a windowed input script's last
frame, which the frame after the wait draws; headless draws it either way. Closing it is a look:
run `./out/bin/canvas --idle --sheet board` windowed and press F. The readout's count of frames
drawn should stand still while nothing moves, the process should sit at 0% CPU in Activity
Monitor, and a drag, the wheel, G's debug GUI and its clicks should each draw at once. **Done on
macOS (2026-10-29)**, at 0.0% CPU while still. **Linux and Windows are owed** the same look.

### The real-character path

The blend layer's arms (spec 12.1) run on the puppet, whose clips are authored in closed form, so
they assert numbers: an endpoint is 0 px, an unmasked bone moves by exactly 0, a settled crossfade
equals the incoming clip bit for bit, the nlerp midpoint reads exact angles at the quarter points.
All of that is true of the math and is no claim about how a character looks.

**No suite covers a real rig.** The puppet has twenty-two bones, identity rest rotations, boxes for
limbs and no spring bones; a real character has a hundred-odd bones, an authored rest pose, skin
that slides over joints, and hair. Four things only a person watching one can settle:

- **Foot sliding through the locomotion blend.** The shared phase stops feet skating between walk
  and run, but whether the two clips' stride lengths agree at a given speed is a content question.
- **The crossfade transient on real limbs.** 0.25 s reads well on box arms; whether an arm with a
  shoulder and a wrist swings through the fade or snaps at its ends is a look.
- **The override layer over a real upper body**, with fingers under the mask and a spring-boned
  mane over it. The layer runs before the springs, so the springs should follow the waving arm
  rather than fight it.
- **`assets/models/strut_walk.fbx` on a rig whose rest pose is not identity.** On the puppet it
  binds by name with an identity delta; on a real rig the retarget delta does the work
  (`docs/animation-retargeting-analysis.md`).

Closing it is a watch:

```bash
./out/bin/render -m my_models/raiden/source/raiden_textured_rigged.glb \
    -t my_models/raiden/textures -e my_models/studio_small_03_8k.hdr \
    -a assets/models/strut_walk.fbx -a assets/models/flair.fbx \
    -s assets/models/t_pose.fbx \
    --anim-switch-to flair --anim-switch-at 120 --anim-fade 0.35
```

and, windowed, `./out/bin/gametest --twin run`: two rigs side by side under TAA, which is the one
thing `anim-twin`'s headless frames cannot see, that neither smears into the other. **Owed.**

**Two cautions about `anim-twin`:**

- **An intermittent failure of it has been a fault elsewhere.** It once failed because of a heap
  overflow in the glyph atlas, which faulted only when an allocation ended on a page boundary.
  Suspect the rest of the app before the blend layer.
- **Its pixel count varies a great deal run to run** (from about 36,000 to 225,000 px in one
  session, every run far past its `>= 20` bar), which a frame-deterministic app should not do,
  since the two processes differ only in the twin's clip. This is unexplained. Before reading
  anything into its number, render each of the two configurations twice and diff those, to find
  whether one configuration even reproduces.

---

### The root-motion path

Root motion's arms (spec 12.18) are in the `anim` group. Seven measure the engine with no app:
what a clip states (`anim-root-travel`), what the animator hands out over a loop seam
(`anim-root-wrap`), a mixture (`anim-root-blend`), a crossfade (`anim-root-fade`), a turn
(`anim-root-yaw`), the pose it leaves behind (`anim-root-zeroed`), and one pin
(`anim-root-switch`). Two drive the demo with a scripted pad and read its trace,
`anim-root-lunge` and `anim-root-spin`, both two-sided against `--no-root-motion`.

**Every distance is one `assets/generators/gen_puppet_fixture.py` declares and asserts** -- 1.20 m
per walk loop, 1.60 per run loop, 1.20 for the lunge, half a turn for the spin -- so the gate and
the asset are two ends of one statement. The arms expect one update less than the whole distance,
which is the contract: a source's first update has nothing to difference and rebases instead.

**The arms make no claim that a stance foot cannot slide**, root motion's textbook benefit, and no
arm or doc should. It is unmeasurable on every asset here: the generated puppet's `gait()` is a
straight-leg pendulum with no stance phase, so its feet slide whatever the body does, and every
committed FBX clip is in place (`strut_walk` states 0.000071 m of travel over its loop). What the
arms assert is that the character travels exactly what the clip states.

**Three things only a person watching can settle:**

- **Whether a wall reads as a bug.** Under root motion the blend knob is what the stick asks for,
  since the travel comes from the clip the knob selects and a knob fed by the achieved speed would
  start at zero and stay there. So walking into a wall plays the walk on the spot, where the
  stride-matched path stops it. Whether that wants a blocked-detector is undecided.
- **The lunge's opening.** It carries 2.192 m of the 2.40 it states, the 0.08 s crossfade
  discounting the start. Whether the move reads as landing short is a look.
- **A rig that is not the puppet.** No imported clip carries a root curve, so `--puppet` never
  enters this path. A character with authored root motion would be the first real test, and would
  want the retarget to carry translation, which it does not.

**Owed.** Nobody has watched a character whose animation was authored to move it.

---

## The foot-planting path

The `ik` group asserts two-bone IK where it is a pure function.

- **Synthetic solves**, with no physics: a bend is `acos((c² − a² − b²)/2ab)` and nothing else,
  matched at five distances against a closed form the gate computes itself from the segment
  lengths the probe reports, so neither side carries the other's number.
- **A ramp rising 1 in 4**, where the feet are 0.20 apart and the ground under them differs by
  exactly 0.050000, and **a staircase** where it differs by one riser with the uphill foot on the
  other side, which catches a solver that has hardcoded which leg bends.
- **`ik-swing`** ticks a walk cycle and measures the ankle's vertical travel. Every other planting
  arm poses the rig once and solves once, and **a solve that overwrites a stride looks perfect in a
  single pose**: the feet were once welded to the floor with every arm but one green. Watch the
  character walk before believing the group.

**What the group asserts is the solve, not walking.** Five things only a person watching can
settle:

- **A stride through real locomotion.** `ik-swing` measures amplitude and nothing about shape; a
  walk of the right height and the wrong rhythm passes it.
- **The plant fraction as a look.** It is 0.10 of leg length, and its two failure modes are
  opposite and both visible: too small and feet float over a slope, too large and swinging feet are
  dragged back down. No number here says which way it is wrong.
- **The pelvis drop as a silhouette.** `docs/foot-locking.md` records the warning that dragging the
  hips down to reach produces "T-Rex" posturing. The cap is 0.31 of a leg (0.2542 m on this rig),
  which `ik-drop` exercises with a foot asking for more than the cap; nobody has judged how it
  reads.
- **Two characters.** The chaser carries the same rig and nothing plants its feet, so the
  per-entity question `anim-two-rigs` asks of the animator goes unasked here.
- **Nothing photographs it.** Every arm reads CPU numbers, so a solve correct in model space but
  spliced into the wrong node would pass them all. `ik-scale` covers the narrower case, that a rig
  node scaled 2 changes nothing the solver decides. A pixel arm was considered and dropped: on flat
  ground the solve is an identity by design, so IK against `--no-ik` at the default spawn measures
  nothing, and gametest has no flag to stand the player on the ramp.

**Planting is not locking.** Planting answers "where is the ground under this foot" every frame
and keeps no memory, so on its own a walking character slides. Locking (spec 12.9) holds a
world-space contact; `docs/foot-locking.md` has the method and how the engine differs from it.
The staircase case runs with release disabled: a foot reaching down to a lower tread is 0.25 above
its target, and a release decided on height alone cannot tell that from a foot the clip has swung
up.

Closing it is a watch:

```bash
./out/bin/gametest                    # walk onto the ramp at +X, the stairs at -X
./out/bin/gametest --no-ik            # the same without planting, to tell the two apart
./out/bin/gametest --no-lock          # planting without locking -- but see the stance note below
```

**What the instruments can and cannot see:**

- **No golden sees the geometric path.** A build that failed five `ik` arms by the same 0.08 m
  passed every golden; only the synthetic arms see it.
- **`ik-pole` is weak**: it asserts opposite signs and a span, never a magnitude, so its value can
  move by a third and still pass.
- **`springbone.c` has no instrument of any kind.** No gate arm and no golden, and it is inert in
  the corpus: the render app registers chains only under the prefix `hair`, and no fixture rig has
  such a bone, so `spring_bone_update` returns at its first guard in every golden. It shares a
  write-back with `ik.c`, whose half is covered and whose spring half is not.
- **The travel speed comes from the clips** (spec 12.10): `animation_stride_speed` measures what a
  clip's feet imply, `animator_stride_speed` blends it across a space, and gametest places its
  entries at those speeds. On `--puppet assets/models/t_pose.fbx` the lock shows clearly against
  `--no-lock`. The generated puppet shows nothing at any speed, and that is the mechanism working:
  its walk has no stance, and the stride measurement refuses it by name (said at startup under
  `--no-root-motion`; by default its clips state their own travel and the measurement is not
  reached).
- **The generated puppet's walk has no stance phase, and that is the remaining limit.** `gait()`
  swings thighs about a straight leg, so the foot never dwells and a lock has nothing to hold
  during locomotion. `--ik-probe lock` plays the committed `strut_walk.fbx` for that reason.
  `--speed 1.2` makes the lock visible on it by dropping into the idle/walk blend, where the feet
  are slow enough to be labelled, which shows the feature round the edges of the problem rather
  than through it. Giving the generated clips a real stance is the next thing this fixture wants.
  Root motion and locking meet on no asset here: only the puppet's own clips carry a root curve,
  and they are the same stanceless pendulum.
- **On the committed humanoid the right leg carries a standing correction of 0.45 of a leg**
  that no playback rate moves: the contact label opens during late swing. It is filed, not fixed,
  and every lock measurement reads the left foot, so no arm sees it. Moving the label would move
  `ik-contact`, `ik-slide` and `ik-hysteresis` with it.
- **The state machine's arms prove sameness, not rightness.** `graph-parity-ground`,
  `graph-parity-air`, `graph-parity-water` and `graph-no-swim` read the trace's source column
  against state sequences spelled out in `gates.py`, which were learnt by recording the
  hand-written machine the graph replaced. If that sequence was wrong, it is wrong identically now.
- **The graph decides once per rendered frame, not per fixed step**, and headless the two are the
  same, one step per frame, which every gametest arm rests on. So no arm can see a frame that runs
  three steps or none; that wants a person watching at an uncapped frame rate and at a throttled
  one.
- **How a transition reads has no arm**: whether a fade is the right length for the move it
  covers, whether a landing lands on the beat, what a swim looks like.

**Owed.** The solve is exact and the lock holds to 0.4% of a leg, on ground nobody has walked
across at a speed the animation agrees with.

### The grotto, the ocean and swimming

gametest's platform stands in a walled basin on an eroded heightfield, with a sea round it that
both characters swim in (specs 12.6 and 12.7). **None of it has a gate arm:**

- **Swimming is a feel, and nothing measures a feel.** The float is a spring drive toward the
  surface with the rise clamped. Its two failure modes are opposite and only visible: too stiff
  and the character pops out like a cork, too soft and it sinks before recovering. The numbers
  were chosen against two hazards: the seabed sits below the foot ray's reach, and three metres
  under the feet so `stick_to_floor_distance` cannot grab it.
- **The water's colour is authored, not derived.** Turquoise is a pale bed seen through clear
  water, so it depends on the seabed's albedo, the depth and the sun's elevation together, and any
  of the three moving changes the look with no arm to notice. The `water` and `beach` groups drive
  the render app against their own fixtures and never see gametest.
- **The swim clip is checked for existence, not shape.** `fixture-gen` asserts the committed asset
  is what the generator emits, but whether a breaststroke reads as one is a watch, and the clip
  shares the rig's limitation that its walk bends no knee.
- **The follow camera is covered by arms, not pixels.** `cam-pitch-clamp` saturates 12 radians of
  demand at each end, `cam-basis` reads the published frame and its sentinel, and
  `pad-camera-relative` reads the movement scheme end to end through a scripted pad; turning
  `steers_controls` off reddens it by 104.85 degrees. Nothing photographs the camera's rest pose.
- **The chaser swims after you**, and nothing asks the per-entity question of it.
- **The clustered light budget's failure looks like a renderer fault.** The index pool is
  `LC_MAX_CLUSTER_INDICES`, 65,536 slots in a 64 KB uniform block (spec 13.51). A light whose
  sphere holds the camera claims every tile of every slice out to its range, and a panel authored
  with `range = 0` derives a cull radius from its size, thousands of units for a 90x90 panel, so it
  sits in every froxel. Running out leaves one cluster partly lit and every cluster after it in the
  z-major walk unlit: a hard horizontal line on a tile row that moves with the camera. The
  `light-pool` group is the budget's arm: `pool-fit` stores more indices than 16 KB holds, and
  `pool-overflow` holds the warning, which names the demand, the pool and the widest-reaching
  light and repeats every 600 builds while an overflow lasts. **Read the warnings.**
- **Terrain correctness is one measurement, and not a picture.** The pool floor's highest point is
  checked against the swimmer's foot probe at build time, because a floating character whose feet
  reach rock plants on the bottom. It has caught four regressions none of which was visible in a
  frame. Anything that moves the field's noise lattice can breach it, so extent, resolution and
  seeds are all inputs to it.
- **Startup cost has no arm.** `terrain.c` memoises the Perlin permutation table one entry deep, so
  noise sources with different seeds interleaved per point rebuild it on every tap; the terrain
  shares one seed for that reason. Nothing would notice a regression; it took a hand profile to
  find.

Closing it is a watch:

```bash
./out/bin/gametest                      # walk off the edge, land in water, swim; arrows turn the camera
./out/bin/gametest --no-follow-cam      # the fixed orbit instead, for comparison
./out/bin/gametest --no-chaser          # the NPC's swim, isolated by removing it
```

**Owed.** Every part of it.

---

## The UI paths

The `ui` group (spec 12.2) asserts the UI where it is a pure function: `ui_layout` is (tree, width,
height) with no GL, no clock and no input, and the input pass takes a struct of values rather than
a device. So navigation, hit-testing, capture, the stack, wrapping, theme resolution and the
settings round-trip are checked with **no window and no GPU**, and nothing compares the layer's
pixels.

What that leaves open:

- **Another display.** The layer works in window points and is captured at framebuffer resolution,
  so a Retina panel doubles it and a 1x panel does not. Every golden was baked at 2x on one Mac.
  Whether a menu reads correctly on a 4K panel at another scale factor, or a 1x monitor, is
  unlooked-at, and `_detect_fb_scale` reports a mismatch as "not the display it was baked on"
  rather than as a bug.
- **A real controller through a menu.** The `ui` group drives navigation by writing the `UIInput`
  struct directly, which is the layer's contract and deliberately not a device. The pad path above
  it -- `ui_up`/`ui_down`/`ui_accept` as actions sharing a table with the game's -- is exercised
  only by the scripted pad in the `gamepad` group, so closing the gamepad device path closes this.
- **The settings file on Linux and Windows.** `settings_default_path` resolves three locations and
  only `~/Library/Application Support` has ever been written. The gate sets `CETRA_SETTINGS_DIR`,
  as a hermetic test must, so it does not exercise the platform branches either: they are
  untested, not merely unasserted.
- **A ragdoll that looks right, as opposed to one that behaves.** The `ragdoll` group (spec 12.16)
  steps Jolt 300 times and asserts what a number can carry: it falls, comes to rest, takes its pose
  from its bodies, holds together, and leaves the world's body count where it found it. Whether a
  heap of limbs looks like a person who fell over is a look. The joint limits are authored
  constants (a knee gets 5 degrees of cone and 75 of plane, a hinge pretending to be symmetric) in
  `RAGDOLL_HUMANOID[]` in `ragdoll.c`, and are the first thing to reach for if a character reads as
  rubbery.

  **An arm that measures one body at a time cannot see a ragdoll come apart.** `ragdoll-roundtrip`
  and `ragdoll-rigid` measure the relationship between two bodies, which is the only thing a
  ragdoll is. When looking, take a strip at `--screenshot-every 20` through the collapse: a figure
  come apart is obvious in it, and a couple of small captures of a segmented character can pass
  for the box puppet's own geometry.
- **A window actually moving.** Window modes are refused headless, since a suite that seized a
  display would be intolerable and a golden whose size came from the machine's monitor would not be
  a golden. So the `display` group asserts the decision (`engine_window_placement`, a pure function
  of the mode, the monitor rectangle and the saved geometry) and `display-headless` that the refusal
  holds. Owed: a person watching a window go fullscreen and come back to the size it left, on a
  machine with a second display; this one has one, so `display-monitors` cannot tell a correct name
  lookup from one that always answers 0.

  The switch's idempotence was checked on a real window: a 640x480 window at (220, 180) stayed put
  through thirty `settings_apply` calls, half a second of a held slider, and moved to the top-left
  corner with the early-out removed. Both halves of the fix are needed: the windowed rectangle kept
  current, so a replay lands where the window already is, and the early-out when the mode and the
  monitor are unchanged. It needs a window, so it cannot be an arm.
- **`uTime` windowed.** The clock a custom element program reads is
  `total_frames * ENGINE_FIXED_FRAME_DT`, right headless and wrong windowed: it advances at
  sixty-over-refresh rather than in seconds, so a shader backdrop runs slow on a 144 Hz panel and
  fast on a stalling one. **It cannot take `engine->render_time`**: under the game framework that
  is the sim's clock, which stops while the sim is paused, which is when a menu is up, so every
  screen with a transition would draw at alpha 0. The fix is a wall clock on the windowed side
  only, and it wants its own before/after.

**Why there are no menu goldens.** `menu` and `menu_focus` were `apps/gametest` frames, and that
app is 0 px run-to-run. Their recipes reproduced at 0 px by hand and through `goldens.py --only`,
but inside full runs `menu` failed more than once (once by 48,886 px, the Quit button focused) and
never reproduced on a re-run of the same binary; spec 13.6 dropped both. **A golden that fails
once in a full run: re-run it before bisecting.** The cause was never found. The first suspect was
the async texture upload budget (<=5 a frame), the one part of a gametest frame that depended on
how busy the machine is. A headless run's start-up textures are now in before frame 1 (spec 14.8),
which clears that for anything loaded at start-up; a texture a menu asks for after the run has
begun still streams, and a recipe should give it frames to land.

---

## The save paths

The `save` group (spec 12.3) asserts the save format where it is a pure function of a file and a
world. The probe builds its own scene, physics world and entities, so every number an arm checks
can be stated in closed form: eight app fields back from a zeroed struct, a pose and a velocity
restored after both were moved away, a destroyed crate rebuilt from its recipe at the position it
had, one drop each for an absent entity and an unregistered recipe with the file still loading, a
refusal below the version floor that applies nothing, and a 75 at version 1 that becomes 0.75 at
version 2.

**A restored world is not bit-exact, by design.** Jolt's contact caches, island assignments and
sleep timers are not serialized, because that would pin the format to `JPH_VERSION_ID` (which
`physics_cook.h` exports for exactly that reason) and break every save on a physics upgrade. Poses
and velocities are restored into a freshly built world and the solver settles from there, so a
crate coming to rest a fraction of a millimetre from where it was is the trade, not a defect. No
arm yet bounds how far that drift goes over many steps: a save-step-reload-step comparison with a
measured tolerance is owed, and the tolerance has to be measured before it is quoted.

Five more things this cannot settle:

- **The rename path.** `former_key` on a descriptor row lets a renamed key cost no migration: the
  reader tries the current name, then the old one. Nothing in the tree writes a `SAVE_ROW_WAS`, so
  it has no consumer and no arm, and the first person to rely on it will be the first to exercise
  it.
- **A migration in anger.** The chain is exercised by a synthetic version-1 file the probe writes
  itself, which proves the mechanism and nothing about the judgement. The real test is a format
  that changed for an unforeseen reason, read by a build shipped months later. The fixture ladder
  the spec describes would catch it, with real historical files rather than hand-authored ones;
  there is one version so far, so none is committed.
- **The save file on Linux and Windows.** `save_default_path` resolves the same three locations
  `settings_default_path` does, with the same gap as the UI paths' settings file.
- **Forward compatibility.** An older build reading a newer save skips keys it does not know and
  reads the ones it does. A key whose meaning changed in the newer build reads wrong, silently.
  Unknown-field preservation, writing back what it did not understand, would fix it, and becomes
  necessary once a save syncs between two machines on different patch levels.
- **A crash mid-write.** The write is atomic by temp file and rename, flushed and fsynced before
  the rename, unlike `settings.c` and `config_snapshot.c`. Nothing has pulled the plug on one to
  watch the old file survive.

## Reproducing a session: the config snapshot (spec 11.71)

**Reach for this whenever the question is "why does my frame look like that".** A `.cscn` plus a
command line does not describe a session: the GUI has about 185 controls, and dragging one leaves
its state on screen and nowhere else. `--config-dump <path>`, or the **Dump Config** button beside
Print Camera, writes about 230 settings across engine, postfx, exposure, scene, shadow, sky, ibl,
gi and water, plus every probe, light and material. `--config <path>` reads it back.

```bash
./out/bin/render -m assets/scenes/cornell_rooms.cscn        # drag sliders, hit Dump Config
./out/bin/render --config cetra_config.json                 # the same frame, no other flag
./out/bin/render --config cetra_config.json -x -f 2 -S /tmp/agent.ppm
```

**It carries its own model and framing**, so the JSON is the whole handoff: give it to somebody and
they see your pixels, exactly.

**It is a settings snapshot, not a scene.** It cannot create a light, bind a texture, place a probe
or a fog volume, or author a road: a `.cscn`'s load-time half has no runtime setter, so restoring
it is a reload. The snapshot names its `.cscn` in its `source` block and applies on top of it.

**Precedence is defaults, then the `.cscn`, then the command line, then the snapshot.** The
snapshot owns the look and the command line owns the run: `-x`, `-f`, `-S`,
`--screenshot-every` and `--profiler` are never written or applied, so a snapshot handed to a
headless capture cannot fight it.

**Three things are not carried, each for want of a stable key:**
- spring-bone parameters, which are per `SceneNode`;
- SSS profile slots, assigned in material-block order at load;
- `camera.near_clip`, which the render app (`apps/render/src/render.c`, not the engine's
  `render.c`) recomputes every frame from the rig's arm unless `fit_clip` is on.

`config-perturb` names all three.

**A restored sun or cloud goes through the chain the GUI's sliders do.** `scene_environment_changed`
is the one place the env cube, the sky-mirroring probes and the GI volume re-derive from; the GUI's
two release sites and the restore all call it. Bare `sky_update_sun` is a subset: it re-bakes the
cube without the cloud deck, refreshes no probe and re-arms no GI. An environment-only probe is
refreshed; a probe set is not re-captured, since its members' cubes were released into the atlas.

**`--clouds` rides the `source` block, not a table row alone.** The cloud noise is baked once at
start-up and only when the layer is on, so a row alone would store `true` against noise that never
existed, render a clear sky, and dump `true` back, passing `config-roundtrip` on the wrong frame.
`sky.clouds.enabled` refuses by name when no noise was baked, and `config-clouds` reads both halves.

## Debugging an animation

1. **Start on the puppet.** `render -m assets/scenes/puppet.cscn --anim-probe` poses a rig whose
   bind is pure translation and whose clips are authored in closed form, so a wrong axis, order or
   handedness reads as a number rather than a silhouette.
2. **Retarget onto it.** `-a assets/models/strut_walk.fbx` on the puppet is a real walk cycle bound
   by exact name, and by itself it is the failure case (spec 12.11): the clip is authored on
   `t_pose.fbx`, and with only one rest pose the loader has nothing to reconcile, so the rig folds
   its legs over its head. Add `-s assets/models/t_pose.fbx` and it walks. That pair is the A/B a
   retarget is read against; `anim-retarget` is it as an arm.
3. **Then the real rig:** render the model with no animation as the bind-pose baseline; play the
   clip on its native skeleton (`-m T-Pose.fbx -a clip.fbx`) to check the core path; then
   retarget onto the target model and compare frame strips. Feeding the T-pose itself as the
   animation (`-a T-Pose.fbx`) must reproduce the bind pose exactly.

Cross-rig retargeting needs the source skeleton's rest pose. A clip exported with its skin embeds
its own rig, which is used automatically; for an animation-only file pass `-s <tpose.fbx>`, or the
rotations reach the target raw. The loader says which happened: the per-clip summary's third
number counts corrections actually computed, so a load that reconciled nothing prints
`0 retargeted`, and the bone table's `[R]` means the same.

## Where assets live

`assets/` is split by kind: `scenes/`, `models/`, `textures/`, `shaders/`, `fonts/`, `audio/`,
`goldens/`, `generators/`, `lut/`, `ies/` and `data/`. `abandoned_window/` and `ivy_arcade/` stay
whole, since each carries its own scenes and producers.

**Nothing that names an asset spells a directory.** A generator writes one through
`asset_path("foo.gltf")` and names one inside a file it emits through `asset_ref("foo.png")`;
`gates.py` finds one through `asset("foo.cscn")`. All three route through
`fixture_paths.asset_subpath`, the one statement of the taxonomy, imported rather than restated, so
a new fixture lands in the right place with nothing to register.

Three couplings depend on that routing, and each renders a plausible frame when broken:
- **A generator resolves its outputs against its own `__file__`**, which is what lets `fixture-gen`
  sandbox one by copying it. A generator joining an absolute path would overwrite the corpus the
  gate compares against.
- **A `.cscn` resolves its model against the scene file's directory**, so it names
  `../models/<x>`.
- **A glTF image `uri` resolves through the texture pool, whose directory is the model's**, so it
  names `../textures/<x>`. That works because `find_existing_subpath` tries `base + subpath`
  verbatim before stripping leading segments; stripping only shortens a name, so a bare sibling
  filename can never reach another kind's directory.
