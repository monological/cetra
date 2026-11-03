# Cetra Graphics Library

A C11 PBR rendering engine on OpenGL 3.3+ / 4.1 (GL 4.1 is the hard ceiling on
macOS -- no compute shaders). Forward-rendered into a fat HDR MSAA G-buffer,
followed by a large screen-space post-processing stack. Scene-graph core, with an
optional game framework layer (fixed-timestep loop + Jolt physics + entities).

## Planning Workflow

When a new plan is approved (ExitPlanMode), before writing any implementation code, do these
steps in order:

1. **Branch off master.** If currently on `master`, create a new feature branch off it. If NOT
   on `master`, STOP and ask the user how to proceed -- do not branch off or commit onto an
   unexpected branch.

   **ALWAYS prefix the branch name with the spec number it implements.** The branch name is the
   spec's `specs/` filename with the `.md` dropped, so the two cannot drift apart and anyone on
   the branch can find the document that explains it. A branch named for what the work felt like
   at the start stops describing what it turned out to be, and once the name has drifted nothing
   connects the branch to its spec. Rename the branch whenever the spec is renamed; if the work
   turns out to be two specs, split the branch so each keeps its own number.

   **When the work wants a worktree, use the `EnterWorktree` tool, not `git worktree add`.**
   It puts the checkout in `.claude/worktrees/<name>` and switches the session into it;
   `ExitWorktree` leaves, keeping or removing. `git worktree add` at a path of your own
   choosing leaves the session in the ORIGINAL tree, so every subsequent edit lands on
   master while the branch you just made sits untouched -- and a path inside the repo needs
   a `.gitignore` entry it does not have, so the checkout shows up as untracked in the
   parent's `git status`.

   **A worktree gets these instructions automatically.** `AGENTS.md` and `CLAUDE.md` are
   TRACKED, so a fresh checkout contains them and nothing needs symlinking back. They were
   gitignored until they were not, and the old instruction to `ln -s` them into the worktree
   is gone with that -- doing it now would leave a symlink shadowing a real tracked file.
   `CLAUDE.md` is committed as a symlink to `AGENTS.md`, so there is still exactly one copy
   of this content and an edit through either name is the same edit.
2. **Copy the plan into `specs/`** as a markdown file.
3. **Commit the plan** (the `specs/` file, on the new branch) as its own commit, before touching
   any code.
4. **Create a task list** with the task tool -- one item per major step of the plan. ALWAYS update
   the task list as you go and complete items. Do not forget to update the list as you go.
5. **Start the implementation.**

**If the work has a look, the plan needs a step where somebody looks at it, and that step is not
the last one.** A plan made of instruments produces instruments. Spec 12.9 put "build the
instrument first" at phase 0 and "watch it" at phase 6, and what shipped was six green phases and
a demo that could not show the feature -- found in five minutes by the person who asked for it,
after seven review agents and nineteen passing arms had not. Run the app at the end of each phase
that changes what it draws.

**The 14.x series has a master plan, `docs/eda-roadmap.md`, and every 14.x spec keeps it true.**
It lists what cetra gains so that pcb, an EDA program, can be built on it: the 2D canvas,
idle mode, skipping the 3D pass, the 3D scene in part of the window, text input and scrolling panes,
the editor widget, rendering at any scale, the glTF import's differences, and what the panes and
the fonts need of the UI. Read the item before
planning its spec. Then update the roadmap on the spec's own branch, at least in its as-built
commit, whenever any of these happens:
- an item is started or finished, or gets a spec number;
- a decision listed under "To decide" is made;
- the spec does something other than what the roadmap says, which is stated with the reason;
- an item, or a need of pcb's, is found that the roadmap does not list;
- the order changes.

A roadmap that still describes the plan from before the work is how this tree's documents have gone
wrong before: the next spec plans against it and trusts it.

## Example Apps

Each app's `README.md`, in its own directory, says what it is and how to drive it.

| App | Path | Headless |
|-----|------|----------|
| render | `apps/render/` | yes |
| spores | `apps/spores/` | yes |
| forest | `apps/forest/` | yes |
| gametest | `apps/gametest/` | yes |
| tree | `apps/tree/` | yes (but NOT frame-deterministic on the orbit path: floor is 9k-31k px depending on framing, see `docs/verification.md`) |
| silent | `apps/silent/` | yes |
| shapes | `apps/shapes/` | no |
| sprites | `apps/sprites/` | yes |
| network | `apps/network/` | yes |
| splash | `apps/splash/` | no |
| canvas | `apps/canvas/` | yes |

## Verification

**`docs/verification.md` owns this** — how to run the two suites, what a release run moves, the
determinism-by-source table, the per-asset ledger of what is safe to compare, and the cross-build
recipe with the six times it has moved. `scripts/gates.py` asserts analytic properties;
`scripts/goldens.py` compares 33 committed PNGs.

**`assets/` is split by KIND since spec 12.8** — `scenes/`, `models/`, `textures/`, `goldens/`,
`generators/`, `lut/`, `ies/`, `data/`, with `abandoned_window/` and `ivy_arcade/` left
whole because each carries its own scenes and producers. **Nothing that names an asset spells a
directory**: a generator says `asset_path("foo.gltf")` to write one and `asset_ref("foo.png")` to
name one inside a file it emits, `gates.py` says `asset("foo.cscn")`, and all three route through
`fixture_paths.asset_subpath` — ONE statement of the taxonomy, imported rather than restated, so a
new fixture lands in the right place with nothing to register and a later re-split changes one
function rather than two hundred call sites. It shipped as two copies of the map and they had
already drifted before either was used in anger, which is the argument for the import. Three couplings are what
that routing exists to hold, and each renders a plausible frame when broken: a generator resolves
its outputs against its own `__file__`, which is what lets `fixture-gen` sandbox one by copying it;
a `.cscn` resolves its model against the SCENE FILE's directory, so it names `../models/<x>`; and a
glTF image `uri` resolves through the texture pool, whose directory is the model's, so it names
`../textures/<x>` — which works because `find_existing_subpath` probes `base + subpath` VERBATIM
first and only then starts stripping leading segments. Stripping alone can only ever shorten a
name, so a BARE sibling filename can never reach another kind's directory; the explicit `../` is
what does the reaching.

**Look at it before you believe a number.** The suites exist so a change can be re-checked
cheaply later, not so a feature can be declared working without anybody seeing it. Spec 12.4's
defect was found by watching the character walk and reported as "the legs don't move" while
eleven of its twelve arms were green; spec 12.9 repeated that exactly, declaring six phases done
off probe output and shipping a demo that could not show the feature at all. A green suite means
nothing broke that was already covered. It does not mean the thing works.

`docs/verification.md` has the rest -- noise floors, what pins exposure across builds, which
binary directory goes with which suite, and when a bake needs `--no-cook`. Read it when you are
about to quote a measurement, not before every change.

Both run on macOS, Linux and Windows (`./build.sh --target`, see `docs/build-vms.md`). Most gate
arms pass everywhere; timing arms and goldens are the two things that do not travel.

## Conventions

- All transforms are 4x4 matrices (mat4 from cGLM).
- Positions in world space; transforms propagate parent-to-child.
- Y-up, right-handed coordinate system.
- Angles in radians.
- Colors as vec3 (0.0-1.0) or hex integers.

## Comments

The test for every comment: **does it say something the code at this location
cannot?** Intent, invariants, units, non-obvious whys -- never a restatement of
the code beside it.

- **Declaration comments state the contract, in one line.** What the field
  means or what setting it does, in the codebase's own vocabulary:
  `bool area_lights_enabled; // false = LIGHT_AREA lights are skipped at gather`
  -- not the downstream mechanism that gives it effect.
- **Mechanism is documented where it is implemented.** Never narrate another
  file's causal chain (a header comment describing what the .c or a shader
  does rots silently -- whoever edits the mechanism has no reason to know the
  distant comment exists).
- **Never name the callers.** "(GUI toggle)" on a struct field points the
  dependency arrow backwards and goes stale the moment a second caller
  appears.
- **No self-praise and no verification claims** ("exactly the no-panel path",
  "measured 0 px") -- those are commit-message material, not code.
- **Long comments must earn their length** by recording something the code
  cannot express: a rejected alternative, a measured tradeoff, a failure mode
  being avoided (the `touched` bitset comment in `light_cluster.h` and the
  `_POSIX_C_SOURCE` note in this file are the house standard). Length is
  fine; content-free length is not.

## Reviews

Always run code-quality reviews in BACKGROUND agents, and wait for all of them to finish before
applying any change. Applying fixes while a review is still running means the next one reports
against a file that has moved, and its line numbers stop resolving.

Two are used here, neither of them shipped with this repository:

- **`/thermo-nuclear-code-quality-review`** — an ultra-strict quality audit from the Cursor Team
  Kit, at
  <https://github.com/cursor/plugins/blob/main/cursor-team-kit/skills/thermo-nuclear-code-quality-review/SKILL.md>.
  Install it, or read the SKILL.md and apply its standard by hand; the useful part is that it
  hunts for restructurings that DELETE complexity rather than rearranging it.
- **`/simplify`** — a fan-out of reviewers over the diff for reuse, simplification, efficiency and
  whether a fix sits at the right depth. Quality only; it is not a correctness pass.

Neither substitutes for reading the diff, and neither substitutes for running the thing. A
review that returns nineteen findings against code nobody has watched work is an expensive way
to polish something broken.

**File length is not a review finding in this repo, and any review checklist saying otherwise is
overridden here** -- specifically the 1000-line threshold the thermo-nuclear skill treats as a
presumptive blocker. Decomposition is argued from structure:
two things in one file that change for different reasons, or a function nobody can hold in their
head. A file that is long because the subject is long stays long.

## Edits

**Default to the Edit tool, and the reason is REVIEW, not tooling taste.** An Edit shows the
before and after in the transcript, so a change can be read as it is made; a `python3 - <<'PY'`
heredoc that rewrites a file shows the script and not the result, so the same change arrives
invisible and is only reviewable after the fact by diffing. Prefer Edit even when a script would
be fewer keystrokes.

Python is still the right tool for a mechanical sweep no human is going to read line by line --
renaming a symbol across thirty files, regenerating a fixture, reformatting a table. Use it there.
The test is whether the edit is one somebody would want to see: if it changes behaviour, use Edit.

## Where the rest is

| Doc | What it holds |
|-----|---------------|
| `docs/architecture.md` | The objects, ownership, patterns, the frame loop and render pipeline, the typical size |
| `docs/modules.md` | What each source file is for |
| `docs/shaders.md` | Shader sources and variants, vertex attributes, textures and samplers, render modes |
| `docs/shader-subsystems.md` | Each shader subsystem in detail, app shaders included |
| `docs/lighting.md` | Shadows, GI, IBL, sky, probes, rain, captures |
| `docs/game-framework.md` | The game layer and the particle system |
| `docs/building.md` | The build, compiler flags, out-of-tree use |
| `docs/verification.md` | Gates, goldens, determinism, the config snapshot |
| `docs/cli-reference.md` | Every app's flags, and headless mode |
| `README.md` | Features, setup, using cetra in a project, dependencies, common operations |
| `apps/<app>/README.md` | Each example app |

# ABSOLUTE RULE -- NEVER RUN rm -rf

**NEVER** run `rm -rf` (or any recursive/forced delete) for any reason, including cleaning up
temp or scratch directories mid-task. It triggers a permission prompt that BLOCKS the session,
which kills any unattended run dead until a human comes back. Leave stale temp files and
directories alone -- disk is cheap and a blocked session is not. If a fresh directory is needed,
use a new differently-named path instead of deleting the old one. If something genuinely must be
deleted, ask the user and let them do it.
