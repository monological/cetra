# Cetra

A C11 PBR rendering engine on OpenGL 4.1, the ceiling on macOS, so there are no compute shaders.
It forward-renders into an HDR MSAA G-buffer and runs a screen-space post chain over it, around a
scene graph. An optional game layer (`cetra/src/game/`) adds a fixed-timestep loop, Jolt physics,
entities, input and audio. The 2D canvas, idle mode and the UI's text, panes and code editor are
there for pcb, an EDA program built on cetra in its own repository.

This file is the rules. What the engine is and how it works is in `docs/`.

## Read before you touch

| Touching | Read first |
|----------|------------|
| The frame, the scene graph, ownership, the render pipeline | `docs/architecture.md` |
| A source file | its row in `docs/modules.md`, then its header |
| A shader, a sampler, a vertex attribute | `docs/shaders.md`, then the subsystem in `docs/shader-subsystems.md` |
| Shadows, GI, IBL, the sky, probes, rain | `docs/lighting.md` |
| `cetra/src/game/`, particles | `docs/game-framework.md` |
| The build, compiler flags, using cetra from another project | `docs/building.md` |
| Gates, goldens, any measurement | `docs/verification.md` |
| An app's flags, headless runs | `docs/cli-reference.md` |
| An example app | `apps/<app>/README.md` |
| A 14.x spec | `docs/eda-roadmap.md` |

## Build

`./build.sh` configures with CMake and builds with Ninja into `out/`; apps land in `out/bin/<app>`.
Every dependency is vendored under `cetra/src/ext/` and built from source. Never reach for a system
library: the same command has to build on macOS, Linux and Windows.

A committed `.clangd` points clangd at `out/`. If clangd shows a wall of errors, starting with
`cglm/cglm.h` not found, check that file survived before believing any of them.

## Workflow

When a plan is approved:

1. **Branch off master**, named after the spec: its `specs/` filename without `.md`. If you are not
   on master, ask before branching. Rename the branch when the spec is renamed.
2. **Copy the plan into `specs/`** and commit it alone, before any code.
3. **Keep a task list**, one item per step, and update it as you go.
4. **Look at it.** Work that changes what something draws needs a phase, before the last one, where
   the app is run and somebody looks. Run the app at the end of every phase that changes what it
   draws.
5. **After each phase**, update the spec's As built and commit it.

A 14.x spec keeps `docs/eda-roadmap.md` true on its own branch, at least in its as-built commit,
whenever an item is started, finished or numbered; a decision under "To decide" is made; the spec
departs from the roadmap (say why); a need of pcb's turns up that the roadmap lacks; or the order
changes.

For a worktree, use the `EnterWorktree` tool, never `git worktree add`, which leaves the session in
the original tree.

## Verifying

- `scripts/gates.py` asserts analytic properties; `scripts/goldens.py` compares the committed
  golden PNGs. Run gates scoped, `--only <group>,<group>`. The full suite takes over 40 minutes:
  ask before starting it.
- Never build, render or edit `scripts/gates.py` while gates or goldens run. A commit counts as an
  edit, since the pre-commit hook stashes unstaged files.
- Prefer headless runs (`-x -f N -S out.ppm`) for checking behaviour and for measuring.
- Before quoting a pixel difference, render one configuration twice. The difference between those
  two is the noise floor, and a number without it means nothing.
- **Look at it before you believe a number.** A green suite means nothing already covered broke. It
  does not mean the new thing works.
- Name an asset through `fixture_paths` (`asset_path`, `asset_ref`, `asset`), never by its
  directory.

## Rules that break silently

Each of these has rendered a plausible frame when broken, with the suite green. The doc says why.

- **Never assume `clip.w = -z`.** Branch on `projection[2][3]` through `include/depth.glsl`'s
  helpers; orthographic cameras are real. (`docs/architecture.md`, Render Pipeline)
- **Anything that moves vertices in `cetra_local_position`** needs a CPU bound in the draw list and
  must reach all five programs that include the chunk, through
  `engine_upload_displacement_uniforms`. (`docs/shaders.md`, Vertex Attributes)
- **`pbr_frag` declares 16 samplers, the most it can have.** Read the sampler model before adding
  one; there is usually a way that costs no unit. (`docs/shaders.md`, Texture / Sampler Model)
- **The lit shader's variant mask is subtractive**: no define means every feature, so a mask never
  assembled gives the slow program, never a broken one. Keep it that way. (`docs/shaders.md`)
- **A header both C and GLSL include** (`include/*_constants.glsl`, `pbr_features.glsl`) holds
  `#define`s only, and every float literal takes `f`. (`docs/shaders.md`)
- **`shader_strings.h` is generated into `out/generated/`**, never into `cetra/src/`, and only
  `program.c` includes it. (`docs/shaders.md`)
- **What the frame's geometry depends on goes in the app's pre-render hook**: the camera, and any
  node added, removed or moved. Written in the render hook it is a frame late, shadows included.
  (`docs/architecture.md`, Render Pipeline)
- **The transform walk may run again in a frame; the latch must not.** `scene_latch_prev_transforms`
  runs once, or every motion vector goes to zero. (`docs/architecture.md`)
- **TAA is on in a window and off headless** unless `--taa`, so anything gated on `taa_resolving`
  can be invisible in a golden. (`docs/architecture.md`)
- **A length the engine chose is multiplied by `Engine.typical_size`; a length the scene states
  never is.** (`docs/architecture.md`, The Typical Size)
- **The `_POSIX_C_SOURCE` define is Linux-only.** On macOS it hides Darwin's declarations.
  (`docs/building.md`)

## Code

- **Names are subject first**: the first word is the type of the first parameter (`scene_add_light`,
  `node_set_position`), then the verb, then the object.
- **`create_X` makes and `free_X` frees** every object. Never swap a factory for `malloc` to skip
  its defaults.
- **`x_get_y` logs when it returns NULL; `x_find_y` is silent**, for the optional case. A mutator
  returns void and logs bad arguments. A request the caller can act on when refused returns bool.
- **A public struct's fields sit under three banners**: ENGINE-OWNED (read, never write), BY
  FUNCTION (write through the function named beside it), SETTINGS (plain writes at any time). A
  setter exists only where the library has to react.
- **An app names only `cetra/<header>.h`.** The allowlist in `cetra/CMakeLists.txt` says which
  headers are public.
- **Conventions**: Y-up, right-handed, radians, 4x4 `mat4` transforms (cglm), colours as vec3 in
  0..1 or hex.
- **Python files never have whitespace-only lines.**
- **Change behaviour with the Edit tool**, so the change can be read as it is made. A script
  rewriting a file shows the script, not the result. Scripts are for mechanical sweeps nobody will
  read line by line: a rename across thirty files, a regenerated fixture.

## Comments

The test for every comment: **does it say something the code at this location cannot?** Intent,
invariants, units, non-obvious whys; never a restatement of the code beside it.

- **Declaration comments state the contract, in one line**, in the codebase's own vocabulary:
  `bool area_lights_enabled; // false = LIGHT_AREA lights are skipped at gather`. Not the
  mechanism that gives it effect.
- **Mechanism is documented where it is implemented.** A header comment describing what the .c or a
  shader does rots silently: whoever edits the mechanism has no reason to know it exists.
- **Never name the callers.** "(GUI toggle)" on a field points the dependency backwards and goes
  stale when a second caller appears.
- **No self-praise and no verification claims** ("exactly the no-panel path", "measured 0 px").
  Those belong in a commit message.
- **Long comments must earn their length** with something the code cannot express: a rejected
  alternative, a measured tradeoff, a failure mode being avoided. The `touched` bitset comment in
  `light_cluster.h` is the standard.

## Docs

- **A doc describes the engine as it is.** How it got there belongs to the specs and git history.
  Pointing at the spec that explains something is fine; narrating it is not: no "until 12.17 this
  was", no "this line said X for several specs", no measurement that only proves a change worked.
- **One home per fact.** Say a thing in the doc that owns it and link to it from anywhere else.
- **This file stays rules.** Engine knowledge goes in `docs/`, an app's in its own README.

## Reviews

- Run code-quality reviews as background agents at effort high, and apply nothing until all of them
  are back: a fix applied mid-review moves the lines the next one reports against.
- Two are used here, neither shipped with this repository: `/thermo-nuclear-code-quality-review`
  (the Cursor Team Kit's,
  <https://github.com/cursor/plugins/blob/main/cursor-team-kit/skills/thermo-nuclear-code-quality-review/SKILL.md>),
  which hunts for restructurings that delete complexity, and `/simplify`, a fan-out for reuse,
  simplification, efficiency and depth. Neither is a correctness pass, and neither replaces
  reading the diff or running the thing.
- **File length is never a finding**, whatever a checklist says. A split is argued from structure:
  two things in one file that change for different reasons, or a function nobody can hold in their
  head.

## Example apps

Each has a `README.md` in its directory.

| App | What it is | Headless |
|-----|------------|----------|
| `apps/render` | Model and scene viewer; every golden is rendered by it | yes |
| `apps/spores` | Particle demo on the game loop | yes |
| `apps/forest` | A walkable island: terrain quadtree, instanced props, a character | yes |
| `apps/gametest` | The game layer: physics, a character, animation, audio, menus, the UI probes | yes |
| `apps/tree` | Procedural tree generator on an island | yes |
| `apps/silent` | A first-person walk through a foggy night town; the engine's largest user | yes |
| `apps/canvas` | The 2D canvas, idle mode and the UI's text input, panes and editor | yes |
| `apps/shapes` | Procedural geometry | no |
| `apps/sprites` | A particle globe under the 2D preset | yes |
| `apps/network` | A wireframe sphere with its own shaders | yes |
| `apps/splash` | SDF text | no |

## Never run rm -rf

Never run `rm -rf`, or any recursive or forced delete, for any reason: it stops for a permission
prompt and blocks an unattended session. Leave stale temp files alone, and use a new path when a
fresh directory is needed. If something must be deleted, ask the user to do it.
