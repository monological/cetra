# CLI reference

Every flag the apps take, and what each one is for. The headless capture set comes first, since
it is the everyday workflow.

Beside each flag is its default and any rule a user needs: a flag that only means something with
another, or a default that surprises on purpose and the reason for it.

## Contents

- [Headless Mode](#headless-mode) — the capture set, which apps take it, how a headless run starts
  and ends, and the render app's animation flags
- [PostFX and environment flags](#postfx-and-environment-flags) — the render app: TAA, water, sky,
  clouds, contact shadows, decals, IES, LUTs, layers, the probes, rain
- [apps/tree](#appstree) — its own flag set, and two defaults that read as bugs
- [apps/forest](#appsforest) — instancing, LOD, culling, the island, erosion, streaming, origin
  shifting
- [apps/silent](#appssilent) — the town walked in first person: the title, saves and pause menu,
  framing, the look's A/B switches, and its overcast day
- [The other apps](#the-other-apps-and-the-aa-mode-each-one-chose) — gametest, spores,
  shapes, pcb, why splash is not in any of this, and the 2D canvas's own app

---

## Headless Mode

Prefer a headless run for checking behaviour: the log output (asset imports, skeleton extraction,
bone mapping, the first frame's animation dump, GL errors) and the rendered frame are the same as
in a window, a run takes a few seconds, and it is what CI uses. From code, set `.headless = true`
in the `EngineConfig` handed to `create_engine`.

**The capture set:**
- `-x, --headless` — a hidden GLFW window with vsync off. The whole pipeline still runs (GL
  context, shaders, skinning, GUI code); nothing appears and no focus is taken. The render app
  turns its orbit's auto-rotation off, so screenshots compare across runs.
- `-f, --frames N` — exit cleanly after N frames.
- `-S, --screenshot <path>` — save the final frame as binary PPM. Convert with
  `magick out.ppm out.png`.
- `--screenshot-every N` — also save `<path>_000400.ppm` and so on every N frames, for a
  transition. Tile them with `magick f1.ppm f2.ppm ... -resize 384x216 +append strip.png`. It is
  the engine field `screenshot_every`, which a game-framework app reaches through `game->engine`.
- `-W`, `-H` — the window size, where an app takes them.
- `--headless-jitter` — keep TAA's jitter headless, where it is otherwise off for determinism.

**Which apps take it:**
- render, forest and tree: `-x`, `-f`, `-S`, `--screenshot-every`, `-W`, `-H`.
- spores: `-x`, `-f`, `-S`, `--screenshot-every`.
- gametest: `-x`, `-f`, `-S`, `--screenshot-every`, `-W`, `-H`; with `--pad-script` and
  `--trace-player` a headless run can play it.
- silent: the same as gametest, and `--cam-eye`/`--cam-target` to pin a framing.
- canvas: `-x`, `-f`, `-S`, `-W`, `-H`, its view through `--view`.
- sprites and network: `-x`, `-f`, `-S`. shapes: `-x` and `-f`. splash: none.

**Scripted input.** render, shapes and canvas take `--input-script <path>`, which moves and presses
the pointer, types and presses keys through the same path as the window's callbacks, so a headless
run can drag, click and type. The grammar is under the render app's `--input-script` entry below.

**A headless run starts with every texture its start-up asked for** (spec 14.8): `engine_run`
waits for the loader before frame 1, so frame N is the same picture on a busy machine as on an idle
one. A window draws its first frame at once, and a texture asked for after the run has begun
still streams. `textures-in` holds it.

**Under idle mode a headless run ends at the first frame after which nothing asks for another**
(spec 14.1), and that frame is its screenshot; `-f N` is then the most it draws, not the count.

### The render app's capture and animation flags

```bash
./out/bin/render -m model.glb -a anim.fbx -x -f 2000 -S /tmp/run.ppm --screenshot-every 400
```

- `--no-springs` — no spring-bone secondary motion. Required for a byte-identical render of a rig
  with spring bones (`docs/verification.md`).
- `-b, --show-bones` — the bone X-ray overlay: bind pose green, animated pose red.
- `--check-stretch` — skins every vertex on the CPU at frame 60 and reports triangle edges whose
  animated length is over 3x the bind length, with the bone weights involved. The diagnostic for
  "spike" artifacts: rigid geometry stays near 1x, a mis-bound vertex shows as a 100x edge.
- `--anim-clip <name>` — play this clip rather than the first `-a` one.
- `--anim-space <a>,<b>` and `--anim-blend <t>` — a two-entry blend space, `a` at 0 and `b` at 1,
  and its knob. Parked on an entry it is that clip alone, to the pixel.
- `--anim-switch-to <name>`, `--anim-switch-at <frame>`, `--anim-fade <sec>` — crossfade to a
  clip on a frame (fade 0.25 by default). Once settled the pose is the incoming clip's own, as
  though it had been snapped at the switch.
- `--anim-layer <name>` and `--anim-mask <bone>` — play a clip on the override layer over that
  bone's subtree; `--anim-layer-once` plays it once and lets it release itself.
- `--anim-probe` — print the frame's weights and every bone's global pose at `%.9g`, so a textual
  diff is a bit diff. The `anim` gate group reads it.

---

## PostFX and environment flags

The render app's flags for the post chain, the environment and the scene's content, and the
diagnostics that read them. Where a feature is also authored in a `.cscn`, the flag wins over the
file, and a `--no-*` flag is the only way off something the file asked for.

### Resolution, TAA and idle

- `--taa` — temporal antialiasing. The render app turns it on for every windowed session and leaves
  it off headless (`docs/architecture.md`, Render Pipeline).
- `--render-scale <f>` — TAAU, rendering at f of the post resolution (0.5 to 1). Headless needs
  `--taa --headless-jitter`, or the app refuses and renders at full resolution.
- `--render-scale-at <frame:scale[,...]>` — diagnostic: switch the scale mid-run, exercising the
  runtime rebuild. Same headless preconditions.
- `--idle` (spec 14.1) — the engine's idle mode. A scene with anything in it still draws every
  frame, so a scene loaded here draws as it does without it; the `idle-3d` arm holds that.
- `--dof-max-coc` is in half-render-resolution texels, so under `--render-scale` the same value
  blurs about 1/scale wider on screen. This is not compensated.

### Exposure and tone

- `-E/--exposure`, `--no-auto-exposure`.
- `--exposure-at <frame:multiplier>` (spec 13.20) — diagnostic: write the exposure multiplier on a
  named frame. With `--no-auto-exposure` it is an exact one-frame step, where the meter moves 4% a
  frame. A physical camera ignores the multiplier. Read against a run at the new value from frame 0,
  it shows whether the temporal histories follow the exposure.
- `--no-history-rescale` (spec 13.20) — diagnostic: histories of light keep the exposure they were
  written at, so a change of exposure lags by the blend. The A/B for the rescale.
- `--exposure-probe` (spec 11.52) — one line per metered frame: raw and adapted luminance, the gain,
  the camera multiplier, their product and EV100. Silent on a pinned frame, which has no
  measurement. Nothing else shows the metered value but the GUI readout.
- `--meter-mode <uniform|centre|spot>`, `--meter-radius <f>`, `--meter-low <f>`,
  `--meter-high <f>`, `--adapt-up <f>`, `--adapt-down <f>` (spec 11.52) — the metering mask and the
  percentile tails, also authorable as a `post.metering` block.
  - **Low defaults to 0.70 on purpose**: a meter that includes a black background is measuring the
    background, and a low percentile leaves the metered value barely moving when the scene
    brightens (at x1000 it moves 3.85 stops at low 0.10 against 9.96 at 0.70, where 9.97 is
    correct). Unreal's Low Percent defaults to 80 for the same reason.
  - The adaptation rates are per frame, not per second, so a headless run of N frames adapts the
    same every time.
- `--tonemap <neutral|aces|agx|linear>` — `linear` is the identity curve with the display encode,
  which passthrough is not: an authored colour at unit exposure reaches the screen as authored, and
  it is what `engine_set_2d_preset` selects. `passthrough` cannot be reached from the CLI by design,
  because the "unset" sentinel in `render_args.h` is `POSTFX_TONEMAP_PASSTHROUGH = 0`; a gate that
  needs a linear read is written as a ratio instead.
- `--lut <f.cube>`, `--no-lut`, `--lut-strength <f>`, `--lut-interp <trilinear|tetrahedral>`
  (spec 11.58) — a 3D colour-grading LUT. Also a `post.lut` block.
  - **Display-referred: the table is applied after `displayEncode`**, the space a colourist's
    `.cube` is authored in. Applied to the linear values `toneSelect` returns it gives a plausible
    frame that is not the artist's look.
  - The size comes from the file, 2 to 64 (33³ is Resolve's default).
  - Tetrahedral is the default: greys stay exactly grey where trilinear tints them by 5 codes, and
    an identity table is bit-exact where trilinear is not (the texture unit's fixed-point filter
    weights). Trilinear stays as the comparison. fp16 storage costs 0.004 of a code.
  - **A log or show LUT loads cleanly and renders washed out**, and nothing in the format tells the
    loader. A `DOMAIN` outside 0..1 is refused by name, but log tables often use 0..1 too, so the
    mid-grey heuristic only warns.
  - The loaded texture is the enable, so an unused LUT costs nothing.

### Post effects

- `--no-ssao`, `--ssgi`, `--no-ssr`, `--fog`, `--motion-blur`, `--dof`, `--no-bloom`, `--film`.
- `--sss` (`--sss-radius`, `--sss-color`).
- `--fog-history-miss-samples <n>` (spec 13.45) — how many samples a fog cell with no history takes,
  1 to 8, default 4. Such a cell is one entering the view as the camera turns or moves; with one
  sample it shows one jitter position beside neighbours averaged over many, a hard-edged band at
  the leading edge of a turn. 1 gives that back, for the A/B.
- `--contact-shadows` (`--cs-distance <f>`, `--cs-strength <f>`, `--cs-debug`; each implies the
  enable, and `--cs-distance 0` is the exact off path). Off by default.
  - It marches the key directional and every local light with no punctual shadow layer, so the pass
    runs when the scene has a directional or a point or spot without a map.
  - **`--cs-debug` shows the raw visibility term with no display encode**: read the bytes linearly,
    since decoding them as sRGB reports 0.6326 as 0.3564.
  - It costs 2.43 ms per fully covering light at 3200x2000 internal (0.38 ms per megapixel per
    light). Cost tracks coverage rather than count, because a pixel marches only the lights whose
    cluster entry reaches it, so there is no per-pixel light cap.
- `--glare` (spec 13.5) — the aperture's diffraction star, off by default. `--no-glare` overrides a
  scene file's `post.glare`; `--glare-strength` is the share of the light past the threshold moved
  into the star, 0 to 1; `--glare-threshold` the working-space radiance a point must pass;
  `--glare-probe` prints the light the star carries against its source's, and the frame's light
  taken out against put back.
- `--local-exposure` (spec 13.19) — an exposure per pixel on top of the camera's, off by default and
  the identity until a contrast is lowered. `--no-local-exposure` overrides a scene file's
  `post.local_exposure`.
  - `--le-highlights` and `--le-shadows`: the base's contrast above and below middle grey, 1
    unchanged, 0.6 to 1 Unreal's suggestion.
  - `--le-detail`: the detail's contrast.
  - `--le-blend`: the base's share from the blurred luminance, 0.6 by default; 0 is the pure
    bilateral grid, which holds a lit room's window best.
  - `--le-kernel`: that blur's width as a share of the frame.
  - `--le-grey-bias`: stops added to the middle grey everything scales about.
  - `--le-probe`: the frame's luminance percentiles in stops from middle grey, the range to tune
    against.
- `--crt` (spec 13.28) — the finished picture, the app's overlay included and the debug GUI not, on
  a consumer CRT television. Off by default; each setting below implies it.
  - `--crt-lines <n>`: the signal's lines, 480 by default.
  - `--crt-scanlines <f>`: 0 fused to 1 thin with dark gaps. Scanlines fade to fused below 2.5
    window pixels a line, so a 400x300 capture at 480 lines shows none; pass `--crt-lines` a
    quarter of the window's height to see them.
  - `--crt-mask <f>` the slot mask's depth, `--crt-curvature <f>` the tube's bow (0 flat),
    `--crt-bleed <f>` the composite colour bleed.
  - The tone curve holds mid-grey and compresses what is brighter, so the frame comes out a little
    darker. That is CRTS's design, not exposure going wrong.

### The loading screen

- `--loading-screen` (spec 13.34) — the engine's loading screen over the frame from the start,
  never lifted: a preview of it. Headless too, where its clock is a frame's fixed step, so a capture
  is frame-exact. Windowed, **P** cycles its palette, **R** plays it again and **H** lifts it. A
  frame it covers draws nothing of its own, so the preview runs at the screen's own cost.
- `--loading-palette <n>` — 0 sunset, 1 harvest, 2 phosphor, 3 broadcast.
- `--loading-lift-at <n>` — let it go at frame n, so a headless run reaches its switch-off. It lifts
  only once its ident has played, glitch included.
- `--loading-ready-at <n>` — say the game is ready at frame n: once the ident has played, its
  LOADING sign becomes PLAY and it waits for `--loading-lift-at`.

The last three imply `--loading-screen`.

### Water

`--water` turns on a water surface, which suppresses the shadow catcher (`docs/architecture.md`,
Render Pipeline). Its flags:
- `--water-level <f>`, `--water-extent <f>`, `--water-waves <gerstner|fft>`,
  `--water-bed <none|dome>`.
- `--water-bounds x0,z0,x1,z1` (spec 13.41) — hold the water to that xz rectangle: past it there is
  no surface, no light weakened under the level, no submerged medium and no rain landing on the
  plane. All zero, the default, is everywhere.
- `--water-dry cx,cz,yaw,hl,hw[,power]` (spec 13.56) — a dry shape: no water inside that
  superellipse, its length along the yaw in degrees from +z, power 2 (an ellipse) by default. The
  inside of a floating hull.
- `--water-drop x,z,frame` (repeatable) — press a ripple into the water there on that frame.
- Off switches: `--no-water`, `--no-water-caustics`, `--no-water-glitter`, `--no-water-downwell`,
  `--no-water-caustic-relief`, `--no-water-specks`, `--no-water-foam-history`,
  `--no-water-coverage` (the shoreline's derivative coverage), `--no-water-lod` (the far-field
  filtering, below).
- Probes:
  - `--water-probe` — the surface query over a 4x4 grid after the last frame: `h=` is
    `water_probe_result` on either wave model, and on Gerstner `cpu_h=` is the closed form beside it
    at the instant that answer describes (spec 13.1). The answer is `WATER_PROBE_LATENCY` (2) passes
    late, so a run of fewer than three frames prints `available=0 reason=unfilled`. It also reports
    the daylight `incident`.
  - `--water-fft-probe` — the spectral cascades.
  - `--water-caustic-debug <1|2|3>` — 1 the factor the caustics multiplied the bed by, 2 the raw
    caustics target laid on the surface, 3 that target spread across the spectrum and the key's disc
    at the reference plane's depth. Half grey where nothing is concentrated.
  - `--water-caustic-probe` — the target's mean, min and max over its inner 80%, per channel. The
    mean is the energy check: 1 for any sea.
  - `--water-caustic-bands <0..16>` — trace the caustics in N real bands of the spectrum rather than
    once and spread: the reference the spread is measured against, at a whole trace per band.
  - `--water-touch-probe` — the ripples' deepest point and its distance from the first drop.

**The two wave models.** Gerstner is the default and allocates nothing. `fft` is an ocean and adds
64 passes a frame: three cascades at 128² and a ripple band at 512².
- Crest foam is FFT-only by construction: it is selected from Jacobian compression, and the Gerstner
  path's steepness is clamped so its mapping cannot compress.
- Caustics run on both: they refract the key light through the surface normal onto the floor, which
  needs a normal and not a fold.
- The shore foam band runs on both, selected from the shoal factor, which makes Gerstner the way to
  isolate it.
- In the mid field Gerstner's longest octave reads as regular swell lines at grazing incidence, the
  corduroy a single-direction train gives. The FFT's directional spectrum does not have it.

**A scene file can author the whole surface** in a `water{}` block, and the flags override it.
`assets/scenes/water_fixture.cscn` authors every key and is the worked example. **No flag sets the
sea state**, so a scene file is the only way in and `water-seastate` the only arm on that path.
Editing any of it re-seeds the initial spectrum on the next frame, a CPU pass over three 128² grids.

**The sea state is two wave trains**, `windSea{}` and `swell{}`, each with the same eight keys
(`windSpeed`, `fetch`, `direction`, `scale`, `peakEnhancement`, `focus`, `spreadGain`,
`spreadBlend`), beside a shared `seaDepth`.
- The old flat keys `windSpeed`, `fetch`, `peakEnhancement` and `swell` are warned per key by
  `parse_water`'s list of known keys. The old `swell` did two jobs and maps to two keys:
  `windSea.focus` and `swell.scale`. `swell.scale` 0 removes the train.
- Author the swell for the scene. `Breaking` tests the depth-limited criterion
  `disp.y / (0.39 · depth)`, so a swell too big for a shallow shelf breaks everywhere at once and
  paints the whole bay white; that is `Breaking` reporting an absurd sea, not a foam bug.
- `windDirection` reaches both wave models.

**The in-scatter is a fraction of the light falling on the water, plus an optional floor**
(spec 11.84). `scatterAlbedo` is dimensionless; `scatterGlow` is absolute radiance added
regardless.
- The old `water.scatter` is refused by name: it was a radiance, so an old value would still parse
  and mean something several times too large. Convert it by dividing by the scene's daylight
  `incident`, which `--water-probe` reports.
- There are two fields because one cannot do both jobs: a constant reproduces a stylised night sea
  and cannot make a dark realistic one, and a pure albedo does the reverse. Nothing in the tree uses
  a non-zero glow but the `water-glow` arm; `apps/tree` lights its night sea with moonlight
  instead.

**The key light is the brightest directional**, ranked by intensity times peak channel
(`water_key_light`), not the sky's sun by name, so the moon can be the key. A weight of zero selects
nothing, which switches the caustics off in true darkness. **Do not read brightness on
`water_fixture` as lighting**: its geometry is emissive over a black base, which makes it a good
absorption instrument and leaves its water band almost the same at midnight and noon. Every arm in
the `water-night` group is a twin delta for that reason.

**`absorption` is extinction per world unit, and the library default is clear water per metre**
(spec 11.36). They agree only where a unit is a metre; an app whose unit is not divides the default
by its units per metre, or its sea is that factor too absorbing. The in-scatter carries no length
and does not scale. The shader's own lengths (the refraction bend, the short band's fade, the
shoal window, the caustic window) convert through `Sky.world_units_per_km`, so an app at another
scale sets that. The path clamp is a budget in extinction lengths (`WATER_MAX_OPTICAL_DEPTH`), with
`WATER_MAX_PATH` as a floor; truncation at the clamp leaves at most 2.15%, and grazing sight lines
to a deep bed do get clamped.

**Below the surface**, a submerged camera arms the froxel volume for that frame and the body
becomes a second medium. Two approximations, both in `froxel_inject_frag.glsl`: a cell holds one
scalar extinction, so water's per-channel value arrives as its luminance mean with the colour moved
into the in-scatter; and the in-scatter is constant rather than scattered sunlight, so there are no
shafts in the body.

**The surface is a projected grid**: a fixed lattice in NDC, one draw, each vertex a ray onto the
still plane, so density is uniform in pixels and the surface reaches the horizon. **`--water-extent`
is the shoaling bed's domain and nothing else**: it does not bound the drawn surface, and outside it
the bed reads its edge, which is open water. `--water-bed dome` installs an analytic bed so
shoaling has a deterministic fixture.

**The far field is filtered.** Distant cells cover more than a wave period, so each model drops
what sits under its cell footprint (mip levels on the spectral path, whole octaves on Gerstner) and
hands the removed slope energy to roughness, as a mean square slope converted to a lobe width by the
Beckmann relation. It adds to the authored roughness as a variance, so it is exactly the authored
value where nothing is filtered. `--no-water-lod` reaches the unfiltered surface, whose horizon
aliases into a speckle band.

### Sky, scale and the camera's planes

- `--sky` (`--sun-elevation`, `--sun-azimuth`, `--sky-debug`), and `--overcast <0..1>`,
  `--sky-scale <f|photometric>` and `--sky-probe` (spec 13.7, `docs/shader-subsystems.md`).
- `-e/--env <hdr>` — an HDR environment.
- `--world-scale <units-per-km>` — the atmosphere's scale; 1000 is 1 unit a metre.
- `--no-aerial` — no aerial perspective.
- `--typical-size <metres>` (spec 13.57) — `Engine.typical_size`, the size of what the scene holds:
  1 for rooms, the default, and 0.001 for a circuit board. Overrides a `.cscn`'s `typicalSize`, and
  refuses anything but a positive number, "1mm" included. `docs/architecture.md`, The Typical Size,
  says which lengths it scales. **It is not `--import-scale`**, which scales the model and leaves
  every one of the engine's lengths at a metre's: a board imported a thousand times larger renders
  as a room, and its lights, fog and camera have to be scaled by hand.
- `--fit-clip` (spec 13.57) — the camera's near and far fitted every frame to what is in front of
  it, as `docs/architecture.md`, The Typical Size, describes. A `.cscn` camera's `fitClip` is the
  same switch, and with it on the render app's own per-frame near stands down. The boxes are at
  rest, so wind, a skinned pose, the terrain's morph, a hook's offset or a wave's crest can carry
  geometry past a fitted plane, and particles, fire and rain are not counted.

**Cloud shadows.** With `--clouds` on, the deck casts into the froxel fog and onto the ground
(`pbr_frag`, the shadow catcher and water's caustics) by default; `--no-cloud-shadows` turns it off.
The map rides `sceneColorTex` by `#define` in the opaque pass, where unit 6 is idle.
- `--sky-debug` shows the shadow map as a tile below the two cloud-noise fields, and that tile is
  the only way to tell a bad map from a bad lookup: the gate arms watch the shadow's effect on the
  fog, and a map with no range at all still darkens the frame.
- The march steps equal altitude increments across the whole deck and clamps only the horizontal
  excursion, at the shape field's 8 km tile period (further re-reads the same field).
- **Coverage is a gap fraction as far as the ground is concerned.** Extinction 25/km over a 2.5 km
  deck blocks the sun outright under any real cloud, so the dapple is a map of the holes. At the
  0.45 default the deck casts a flat dimming with no pattern; at 0.10 about half the map is lit.
  The gate arms pin `--cloud-coverage 0.10`, where the property is legible, so a change of default
  cannot re-tune them.
- The map is built even when nothing reads it.

### Scene content from the scene file

- `--no-fog-volumes` (spec 11.39) — drop a scene file's `fogVolumes[]`: world-space boxes with a
  density, an inward feather and a tint, authored only in a `.cscn`, since a box needs a place and a
  size no flag can carry. A volume arms the froxel pass by itself and must never set `fog_enabled`,
  which means only that the global height medium exists; the global density uploads as zero when
  the app did not ask for fog.
- `--no-decals`, `--decal-probe <n>` (spec 11.73) — drop a scene file's `decals[]`, and print the
  decal diagnostic every n frames. Decals are authored only in a `.cscn`: each needs a place, a
  size, a facing and an image.
  - `DECAL_MAX` is 16, a structural ceiling: it sizes the froxel mask, 32 would pin the descriptor
    at seven vec4 rows and 64 would not fit GL 4.1's guaranteed 16 KB block.
  - **The probe is the instrument, not the frame**: a mark projected through the wrong box still
    lands on a wall and looks like a poster. The probe prints the froxel mask and the array layers,
    and names why a decal is not live, since one whose image never loaded is invisible exactly as
    one never authored is.
- `--emissive-lights` (spec 11.49) — an emissive mesh becomes a real LTC area panel: a rectangle
  plane-fitted to it, the material's radiance in nits, registered in the cluster list.
  - **Off by default because most emissive materials are not lamps**: 30 of the 32 in this corpus
    are a glow over a black base used to make something a flat bright colour. A material declines
    with `emissiveLight: "off"` in a `.cscn`.
  - Two shape tests, planarity and `fill` (`docs/lighting.md`, Derived area panels).
  - A derived panel has `cast_shadows` false like any light, so it lights through walls until a
    `light_overrides` entry says otherwise. That entry's `shadow_cache`, with a `range`, caches its
    shadow.
  - It costs about 0.02 ms of CPU to reconcile and 2 to 3 ms of shading per panel.
- `--emissive-light-probe` — the panel every emissive mesh would derive, and every skip with its
  reason. A wrong fit hides in a plausible image: a panel half a metre off or sqrt(2) too wide still
  lights the room.
- `--ies-profile <file>` (spec 11.57) — apply an IES file to every point and spot light, overriding
  what the scene authored, so a manufacturer's `.ies` can be tried on any scene. Directionals and
  area panels are skipped, as they refuse an authored profile. It is applied after
  `apply_cscene_light_overrides`, so the flag wins, and it is the same as authoring the file
  (`ies-flag`).
- `--ies-probe` (spec 11.57) — one row per loaded profile and one per sampled angle, with `rel` and
  absolute `cd`. A profile's correctness is a numeric claim no frame can make. Rows are sampled at
  the taps, where they test the table rather than the lerp; the `mirror` rows ask for `span + d`
  against `span - d`, the only way to reach the C fold from outside, since inside `[0, span]` a
  mirror and a modulo agree. The shader's fold is `ies-mirror`'s.
- `--tree-probe` (spec 13.35) — each tree in the scene file's `trees` block grown again from its
  parameters: a `tree-probe` row with an FNV-1a digest of every stream of its bark and foliage
  meshes, and for a conifer a `tree-shape` row (trunk top, highest spray, longest branch per crown
  quarter, each mesh's height range) and a `tree-lod` row (sprays per level of detail). The digests
  are taken before any chain is built, so a level of detail never moves them. Read by the `conifer`
  group over `conifer_fixture.cscn`.

### Lighting that streams, and the captures

- `--stream-probe <n>` (spec 13.24) — every n frames and at exit: the resident GI volumes and
  reflection probes by slot, each one's distance, capture count, state, a probe's faces drawn of a
  capture under way, and an FNV digest of its texels (from the atlas while resident, from the kept
  copy while not); the residency the frame before left; and the cached lights in distance order,
  with whether each holds a whole block of shadow tiles. The `lighting-stream` gates read it.
- `--tile-blocks-per-frame <n>` — how many cached lights may be given shadow tiles a frame once the
  tiles have opened (default 2, 0 no limit). At 1 a room with two lights takes two frames to be
  shadowed, which makes a capture's wait for its lights visible.
- `--capture-hide <node>` — leave a named node out of every GI and probe capture, surface and
  shadow, as a skinned mesh always is. `--remove-node <node>` takes one out of the scene, the
  reference a hidden one is compared against.
- `--draw-distance <node> <m>` (spec 13.38, repeatable) — the camera draws nothing of the node, or
  of what hangs under it, past m metres, while shadows and captures still take it. `root` names the
  scene's root whatever the file called it.
- `--cam-at <frame:ex,ey,ez,tx,ty,tz>` — a camera teleport on a frame; repeatable, up to 128, a
  schedule that walks one run through several places or turns the camera a step a frame.
- `--capture-budget-ms <f>` (spec 13.32; render and silent) — `Engine.capture_budget_ms`, the most
  of a frame the GI probes and the reflection probes' faces may take before the rest wait. By
  default 8 windowed and no limit headless, where a run must repeat to the bit; the flag pins it
  either way, which is how a headless run reaches the paced path. 0 means no limit; render takes 0
  to 1000. Each kind of capture takes at least one unit a frame, so 0.001 lets through exactly that,
  the same on every run. silent's own default is 100 while its view is black and the engine's once
  it comes up; the flag pins it there too.
- `--capture-timing` (spec 13.42) — the GI world's `timing`: each volume prints a `gi-timing` row as
  its sweep converges, each part's CPU and wall milliseconds as `cpu/wall` (the bursts' setup with
  their shadow pass, the shaded faces, the classification's back faces, the projections) with the
  faces' draws and triangles. Each part is bracketed by a `glFinish`, so a timed sweep is slower;
  compare `cpu` with `wall` for how much of a part is submission.
- `--gi-cull-pixels <f>` (spec 13.42; render and silent) — the GI world's `cull_pixels`: a probe
  leaves out what spans fewer than f pixels across its 16-pixel faces, unless it gives off light
  (with what flickers held at rest), holds the probe inside its bound, or hangs under a node set
  `capture_always`. Decided once a probe, for its shaded faces and its wall test alike. 0, the
  default, takes everything.
- `--gi-cook` (spec 13.42) — the scene's `cook_lighting`: a volume's opening sweep and a probe's
  column are loaded from the cook when a run of the same scene stored them, and stored when not.
  The key is the scene at rest within the capture's reach, the engine's source and the driver, so
  any change a capture would see misses. A scene that changes during a sweep is not stored and logs
  `gi-cook volume=N result=unstable`. Off by default, since the suite shares one cook directory;
  `--cook-dir` and `--no-cook` apply. silent turns it on.
- `--capture-rest-unshadow <light>` (spec 13.44) — a capture rest hook under which the named
  shadow-casting light casts nothing while a burst holds the scene at rest, as silent's rest does
  with its flashlight. The headless way to give a burst fewer per-frame shadow layers than its
  frame, which `capture-keeps-shadow-array` reads. A light that does not exist or does not cast is
  refused, exiting 1, and the run prints `capture-rest-unshadow rests=N` at exit.

A `.cscn` authors any number of GI volumes as `giVolumes: [{boxMin, boxMax, spacing, classify}]`,
and up to 64 probes and 64 lights.

### Layers, roads and materials mid-run

- `--no-layers-vt`, `--layers-vt-res <n>`, `--layer-blend-at <frame:value>` (spec 11.66) — the
  composite cache's bisect lever, its resolution override, and the one headless way to make its
  by-value bake key go stale. The first two are in `forest` too.
- `--no-layers-vt-pages`, `--no-layers-vt-feedback`, `--layers-vt-page-slots <n>`,
  `--layers-vt-page-budget <n>`, `--layers-vt-probe <n>` (spec 11.67) — the paged near field's
  bisect levers, its churn and bake-rate knobs, and the residency probe. All in `forest` too.
- `--road-width-at <frame:value>` (spec 11.68) — set every road's width mid-run. It exercises both
  halves of the road path: the segment block re-uploads and the composite cache's key goes stale. A
  road is authored only in a `.cscn`, so this is the one headless way to change one after the first
  bake.
- `--material-at <frame> <material> <key> <value>` (spec 13.26; repeatable, up to 8) — set one
  parameter of every material of that name on a frame, a number or an enum label, as a `.cscn` sets
  it. Four words rather than `frame:value`, since a label may hold a space ("vegetation branch"). A
  material field is a plain write the graph does not see, so this is the headless way to change one
  after something kept what it decided. A bad frame, a value past an enum's labels or a material the
  scene lacks is refused by name.

### Textures, alpha and debug views

- `--no-texture-compression`, `--texture-compress-colour`, `--texture-probe` (spec 11.85) — the
  bisect lever, the opt-in for the lossy colour half, and the ledger. Texture memory is invisible
  from outside the process, and a scene whose normals failed to compress renders exactly like one
  whose normals compressed. The probe prints the internal format the driver holds, not the one asked
  for, which catches a declined format, in the `<prefix> <kind> k=v` shape every probe uses. Each row
  carries `distribute_from=`, the first mip level whose alpha is a binary dither, -1 where the chain
  never enters one before its 1x1 tail. `apps/tree` has `--texture-probe` too, the only place a
  purely procedural texture set can be priced.
- `--no-alpha-jitter` (spec 11.101) — the diagnostic lever for the jittered alpha lookup, live only
  while TAA accumulates at one sample and off under alpha-to-coverage. Without it a cutout's deep
  mips churn and moire. It recovers none of the coverage one sample loses; four samples keep it, at
  +296% of a dense cutout scene's frame for nothing the eye finds (spec 11.102), so this is the
  shipped answer. It rides the config snapshot as `engine.alpha_jitter`.
- `--clearcoat-debug` (spec 11.86) — the coat normal as bytes, `Nc * 0.5 + 0.5` where there is a
  coat and exact black where there is not. A name for `--render-mode 13`: any non-PBR mode takes the
  passthrough blit, so the bytes have no tonemap, encode or dither, and are read linearly. Black is
  unambiguous, since the darkest a normal can encode is (-1,-1,-1)/sqrt(3) at 0.211, which makes an
  uncoated surface an in-frame control.
- `--render-mode 14` — the probe-coverage view: the albedo, magenta by the share of each pixel's
  reflection no reflection probe covers and the sky answers instead. Indoors any magenta is a
  surface outside every probe box; outdoors it is everything, and right. Also "Probe Coverage" in
  the GUI's Render Mode list.

### Culling, sorting and transparency

- `--no-oit`, `--no-oit-moments`, `--oit`, `--oit-moments`. OIT and its moment weighting are on by
  default, so `--oit` and `--oit-moments` only restate a default a `.cscn` or another flag turned
  off. `--oit-moments` implies `--oit`: it is a better weight inside the same accumulate. It costs
  two fp32 moment targets at the scene sample count plus a double-height resolve atlas, 156 MB at
  640x400 on a Retina framebuffer, logged at allocation.
- `--no-instancing`.
- `--no-frustum-cull` (spec 11.53) — submit every item. 0 px by construction, since culling only
  removes geometry that contributed nothing; the `cull` gate group compares against it.
- `--no-occlusion-cull` (spec 11.98) — occlusion rejection off, held to the same 0 px bar: the cull
  is conservative by four stated roundings, so a difference from the default is a defect, which the
  `occlusion` group's identity arms report. It is camera-pass-only, so shadow layers and captures
  are the same either way.
- `--no-lod`, `--lod-bias <f>`.
- `--no-sort-opaque` — front-to-back opaque ordering is on by default (spec 11.30), the larger of
  the two overdraw levers (`apps/forest`'s opaque pass 306 to 169 ms). Not a 0 px flag: where two
  surfaces lie at exactly equal depth, draw order decides which wins.
- `--depth-prepass` — depth before shading, off by default because it loses everywhere in this
  corpus at one sample (spec 11.31). Masked geometry is prepassed through its own program in a
  depth-only mode. **It reverses sign under alpha-to-coverage** (spec 11.102): at `--msaa 4` on a
  dense cutout scene it halves the opaque pass, because A2C stops the alpha test discarding and
  leaves overdraw for a prepass to remove.

### Diagnostics

- `--negative-probe` (spec 13.21) — every frame, the float frame the tonemap is about to read, read
  back, and a `negative-probe` row with the pixels below -0.0005 and below -0.005 and the most
  negative value. Light below zero is a bug wherever it comes from, and the tonemap clamps it out of
  sight. A whole-frame readback, so not for timing.
- `--fog-miss-probe` (spec 13.45) — every fog frame, a `fog-miss-probe frame= armed= cells=` row:
  whether any cell could take several samples (history existing and more than one asked for), and
  how many did, from an occlusion query read at once, so not for timing.
- `--shadows-off-at <frame>` (spec 11.56) — clear `shadow_system->enabled` mid-run, the state the
  GUI checkbox produces and `--no-shadows` cannot. That flag clears the switch before frame 0, so no
  punctual layer is ever assigned; turning it off after the pass has run leaves `Light.shadow_layer`
  pointing at layers nothing draws. A defect reachable only through that transition is invisible to
  every other headless run.
- `--occlusion-probe` (spec 11.98) — the masked occlusion buffer against its brute-force twin, as
  `occlusion-probe` k=v rows after the loop, with no GL and no frame. Two-sided: hierarchical-hidden
  must imply reference-hidden with no exception over the scene's items and a seeded frustum sweep,
  and the catch rate must clear a floor so the safety half cannot pass by hiding nothing. The twin
  replays the recorded box set the frame accepted, not a re-gather, through the same raster path at
  4x resolution, so what it checks is the hierarchy: the tile fold, the footprint rounding, the
  mask. It also checks every material-flagged proxy's interior contract, box against its own
  triangles, judged against the 3x3 neighbourhood because the conservative fill leaves a diagonal
  seam a box face does not have. Read by `occl-probe`.
- `--wind-bound-probe` (spec 11.54) — per wind-responsive mesh, the largest displacement `windOffset`
  can be driven to beside the bound `wind_max_offset` claims, as `max_abs`, `max_l2`, `bound` and
  their ratios. **It drives the real shader through transform feedback**, not a CPU port: the two
  halves share coefficients through `wind_bounds.glsl` but not arithmetic, so a term added to the
  shader makes the bound non-conservative and culling drops geometry that is on screen, which a CPU
  port cannot see. The `cull-bound` arm asserts `measured <= bound` and a floor on the tightest
  sweep, since a low reading means the grid stopped sweeping.
- `--profiler` (specs 11.27 and 11.28) — per-pass GPU time, CPU time and submission counts; HUD
  tables under "Profiler", and three on stdout at exit.
  - **A sub-millisecond scope inside a heavy frame reads 0.000 ms**, not a failure: timer queries are
    checked and never waited on (`PROFILER_RING` 4), since a blocking read would report its own stall
    as the pass's cost. A zero row is not a free pass; price a small pass on a light scene at high
    resolution.
  - The GPU column drifts by about 3x between sessions on one build, every scope together, so only
    samples adjacent in time may be differenced (A/B/A/B).

### The orthographic camera

`--ortho <height>` (spec 11.103) — an orthographic camera, the view volume's height in world units,
applied after the auto-framing, which decides the projection along with the clip planes. Every
site of the engine that assumed a perspective projection branches on `projection[2][3]` (spec
11.104; `docs/architecture.md`, Render Pipeline), and the `ortho` gate group covers them.

Not yet orthographic: the skybox cube (a ±1 cube covers about 1% of an orthographic frame), the sky
background, the ocean's projected grid, the cloud march, the depth-sort key (performance only) and
the gizmo's world-per-pixel. Water's shading is orthographic; the placement of its mesh is not. A
branch for the grid was tried and refused: under a parallel view its far-capped rows spike (fp32 at
a million units lands them pixels off), its uniform lattice moires against the wave field, and the
water plane crosses the near plane under any tilted parallel view.

### Scripted input and the camera trace

`--input-script <path>` and `--trace-camera` (spec 12.19) replay a mouse and keyboard from a text
file, and print the pose the frame draws from with its near and far planes at the line's end, which
the `fit-clip` group reads. `apps/shapes` takes both and `apps/canvas` takes `--input-script`.

The input script is `--pad-script`'s grammar through the same parser: one line per frame or
`from-to` range, `#` comments, the last matching line winning.
- **Pointer state:** `at=x,y` (framebuffer pixels, +Y up), `by=dx,dy`, `down`, `shift` and
  `wheel=<f>`. A range states the pointer's state, so a press is `down` appearing and a release is
  it going.
- **Typing is events:** `type=TEXT` (UTF-8, `\s` a space and `\\` a backslash) and `key=NAME` with
  modifiers joined before it by `+` (`shift`, `ctrl`, `alt`, `super`, and `mod`, the platform's
  shortcut key: `key=mod+k`), delivered on every frame of their range in the line's order. A NAME is
  a letter, a digit, `f1` to `f25`, or one of `left`, `right`, `up`, `down`, `home`, `end`, `pageup`,
  `pagedown`, `backspace`, `delete`, `enter`, `tab`, `escape`, `space` and `slash`.
- **`close`** (spec 14.7) asks to close the window as its close button would, through the app's
  close hook: once, as the first frame of its range ends, so a close taken makes that frame the
  run's last and its screenshot.

It drives the engine's own pointer path, so a replayed drag reaches the drag state, the ray picking
and the app's forwarded callback exactly as a hand does.

### The config snapshot

`--config-dump <path>` and `--config <path>` (spec 11.71) dump the whole tuned session to JSON on
exit, and restore one; `docs/verification.md`, "Reproducing a session", says when to reach for it.
- The apply lands after the block in `render.c` that derives lengths from the scene's radius, and
  must stay there: `fog_far`, `fog_floor_y`, `fog_density` and `fog_height_falloff` are assigned
  outright in that block and `ssao_radius`, `ssr_max_distance` and `ssr_thickness_min` are `fmaxf`
  floors, so an apply above it loses the first four and silently raises the rest.
- A restored sun does not re-capture a probe set: a set's cubes are released into the atlas, so it
  cannot re-capture.

### Rain

**Rain** (spec 13.9). `--rain <mm/h>` sets the rate of a scene's authored rain, or brings one of
its own, and re-settles it, so the frame shows that rate's soaked world rather than the file's;
`--no-rain` removes it; `--rain-sheen <f>` overrides the streaks' sheen. A `.cscn` authors it as a
top-level `rain` block, every key a knob of the `Rain` struct: `rate`, `wind`, `fallScale`,
`settled` (absent = already soaked), `wetTime`, `dryTime`, `puddleFillTime`, `puddleDrainTime`,
`puddleCoverage`, `puddleScale`, `rippleStrength`, `rippleSize`, `wetDarkening`,
`occlusionExtent`, `occlusionSoftness`, `streakCount`, `streakRadius`, `shutter`, `streakWidth`,
`streakBrightness`,
`streakForwardG`, `streakGlint`, `streakSheen`, `splashCount`, `splashRadius`, `splashAmount`,
`splashSize` and `mist`. Materials take `porosity` (-1 derives it from roughness). Instruments:
`--rain-probe` prints the physics ladder, the state, the medium as the post chain was handed it
and, with `--rain-probe-at x,y,z` (repeatable), whether the rain reaches each point;
`--rain-map <ppm>` writes the occlusion map; `--rain-ask x,y,z` asks the CPU cover query about a
point every frame and the probe prints its answer. `assets/scenes/rain_fixture.cscn` and
`rain_water_fixture.cscn` are the fixtures, from `assets/generators/gen_rain_fixture.py`.

**More of the rain block** (spec 13.12):
- **The scene's wind:** `followSceneWind` (default true) lets a `.cscn` `wind` block that states
  `airSpeed` (m/s) carry the rain in place of the block's own `wind`.
- **The mist:** `mistForwardG` (default 0.8) is the medium's own lobe, apart from the streaks'.
- **Glass:** `glassLens` (1 = physical) scales how far a drop bends what is seen through it, and
  `glassDropSize` the drops' size; a material's `rainBeads` -- `auto`, `off` or `on`, auto
  meaning glass that transmits -- says whether it beads at all.
- **Drips:** `drips: [{from, to, rate, ground}]` (a point when `to` is left out), `dripCount`
  (the slots they share) and `dripBrightness`.
- **After the rain:** `dryFor <s>` opens the scene that long after the rain stopped.
- **Puddle relief:** `puddleRelief` (0..1, default 0), or `--rain-relief <f>`, stands a material's
  puddles in its own height map's lows. A `.cscn` has no height key; a model's material gets a map
  through `<albedo stem>_height.png` beside its albedo, which also turns parallax on, so compare
  relief with `--no-parallax`.

`rain_glass_fixture.cscn` and `rain_relief_fixture.cscn` are the glazed and relief twins.

### Lights in the scene file, and cached shadows

**A light's specular share** (spec 13.30). A `.cscn` light's `specular`, 0 to 1 and 1 by default, is
the share of it the reflective lobes take: the base and clearcoat highlights, sheen, a panel's LTC
specular and, on a sun, water's glint. 0 lights diffusely only, what the diffuse gives up for the
lobes it keeps, which suits a light standing in for a lit surface whose own reflection SSR and the
probes already draw -- a screen, a lampshade. A value outside 0..1 is clamped with a warning.

**Cached shadows** (specs 13.16 and 13.27). A `.cscn` point or area light with
`"cast_shadows": true`, `"shadow_cache": true` and a `range` keeps its shadow in tiles drawn once;
`shadow_near` (metres) is where its faces start, for a lamp inside its own glass or a strip inside
its fixture, and `source_radius` / `source_length` give a point light a body -- a capsule, the
length between its caps -- whose shadow is soft. A panel is cached from its centre and refuses a
body by name. `shadow_follow` draws the kept faces again whenever the light moves at all, rather
than once it is the system's tolerance (5 cm) from where they were drawn: for a light with no body
that moves, whose shadow otherwise steps. A `light_overrides` entry takes `shadow_cache` and
`range` too, which is how a derived panel (`--emissive-lights`) is cached, and an override key the
parser does not know is named rather than dropped. A fire's flame writes its own body into its
light each frame. Instruments:
- **`--tiles-probe`:** prints the tile region and every cached light's block at exit: its views,
  cells, faces kept, the body they were drawn over, and each view's origin and tiles.
- **`--tile-map <light> <ppm>`:** the light's faces as a picture, two rows a view, grey by metres.
- **`--tile-views <n>`:** every cached light with a body drawn from n views rather than eight;
  `--tile-views 1` is its centre alone, the comparison the eight are measured against.
- **`--tile-reference <n>`:** every cached light drawn each frame from n views of its body as it is
  now (at most 64), averaged with no blur -- the soft shadow by its definition.
- **`--tiles-refresh`:** every cached face redrawn every frame.
- **`--node-swing <node> <m>`:** swings a named node along x by up to m metres once a second, a
  caster that moves under a cached light.
- **`--graph-churn <node> <out> <back>`:** takes a named node out of the graph on frame `out` and
  puts it back on frame `back` (0 = never; spec 13.38): a graph change under a cached light,
  which draws again only the faces it touches.
- **`--graph-replace <node> <frame> <dx,dy,dz>`:** frees a named node on a frame and hangs a new
  one in its place holding its meshes, moved by the offset. It frees first, so the new node may
  take the old one's address: what a cache remembering nodes by pointer would take for the same
  node.
- **`--tile-stores <n>`:** how many store cells faces that see a mover may keep their still casters
  in (spec 13.26; default and at most 64, on top of the budget). 0 draws every such face whole
  each frame, and says so by name.

**A swaying material in a cached shadow** (spec 13.26). A material with a `windResponse` moves its
kept faces only under a scene `wind` that can move it. `"cachedShadowWind": "rest"` keeps it at its
rest pose in them -- Unreal's "Rigid" -- while the camera and the other shadow maps keep the sway;
the default `"sway"` redraws every face its sway reaches every frame. `--material-at` (above) is how
a run changes it mid-way.

`tile_core_fixture.cscn` (from `gen_tile_core_fixture.py`) is a flame-sized light just past a
candle-sized rim over a wall; render it with `--ortho 0.8 --tonemap linear` and the full shadow's
radius reads off the wall in pixels. The `shadow-tiles` gate group traces its truth.

### An app's own shaders

**An app's own shaders, from a scene file** (spec 13.29; `docs/shader-subsystems.md`, Shader hooks). No flag sets
any of them: they are scene content, each a `.glsl` resolved against the scene file as an IES
profile is, and a file that does not read or compile is said by name and left out.
- `post.passes: [{"at": "beforeDof" | "beforeBloom" | "afterTonemap", "shader": "...",
  "params": {...}, "enabled": true}]` -- fullscreen passes at three points of the post chain, in
  order at each; up to 8, and `at` in any case.
- `shaderHooks: {"name": {"surface": "...", "offset": "...", "offsetBound": 0.6, "animated":
  false}}` -- surface hooks, each one hook however many materials name it. `surface` defines
  `cetraSurface`, `offset` defines `cetraOffset`; `offsetBound` is metres, the culling margin (an
  offset without one is warned at load and culls as though it never moved), and `animated` makes
  whatever the hook decides of a caster redraw cached shadows each frame.
- On a material: `"shaderHook": "name"` draws it through that hook; `"lateShader"` draws it in
  the late draw, past TAA, lit by nothing, and takes no hook (a material naming both is warned and
  keeps the late shader).
- `"shaderParams": {"name": number or [up to 4 numbers]}` on a material, `"params"` on a pass:
  vec4 uniforms by name, the rest of the vec4 zero.

`shader_hooks_fixture.cscn` (from `gen_shader_hooks_fixture.py`) carries every one of these; the
`shader-hooks` gate group reads it. Render it with `--taa --headless-jitter` to see the late
quad's noise survive the history, and with `--dof --dof-focus 8.6 --dof-range 1.0` to see which
post marks were defocused.

## apps/tree

A procedural tree on a domed island in a sea at sunset (`apps/tree/README.md`).

- **Capture:** `-x/--headless`, `-f/--frames`, `-S/--screenshot`, `--screenshot-every`, `-W`/`-H`.
- **Scene:** `--seed`, `--no-shadows`, `--fog` (the volumetric fog, off by default here),
  `--no-falling-leaves`.
- **Tree** (spec 13.35): `--preset broadleaf|dead|spruce|fir|snag` grows one of `tree_gen`'s named
  trees, broadleaf being this app's own; a conifer preset bakes the needle-spray atlas in place of
  the broadleaf sprigs. `--irregularity F` overrides a conifer's raggedness, 0 a tidy cone, the A/B
  for the branch pattern.
- **Debug:** `--render-mode N` (10 HDR hotspots, 12 extrapolation), `--texture-probe`.
- **Samples:** `--msaa N`, default 1 in every mode, with TAA carrying the edges. Above one sample
  masked geometry takes the alpha-to-coverage path, which traces a bright fringe round the canopy
  against near-black leaves on a sunset. `--msaa 4` is how to reach the MSAA-only grass specks
  (spec 11.38).
- **`--headless-jitter`** keeps TAA's jitter and the 0.70 render scale headless: what the window
  draws, but not deterministic.
- **Sun:** `--sun-elevation` (**default 0.8 degrees**) and `--sun-azimuth` (**default 193**).
- **Glare: on here**, where the engine leaves it off, for the low sun's glints over the sea;
  `--no-glare` drops it (spec 13.5).
- **Water:**
  - `--no-water` drops the sea and the seabed together. `--water-level`.
  - `--gerstner-waves`: the closed-form octaves; the sea is FFT by default.
  - `--no-water-surf`: no incident wave at the shore, no run-up and no bore. Depth-limited breaking
    is gated on the surf existing, so this removes the bore from the geometry too, which makes it
    the bisect lever for anything shore-shaped.
  - `--no-water-wetness` (the swash leaves the sand as it found it), `--no-water-film` (no swash
    solver; the closed-form run-up drives), `--water-foam-debug N` (foam as a binary mask: 1 the
    crest band after erosion, 2 before it, 3 breaking alone).
  - The spectral sea state: `--wind-speed` and `--fetch` address the wind sea (default 6 m/s over
    15 km), and `--swell` is the swell train's weight (default 1, 0 no swell). This app authors both
    trains itself, the swell at 3 m/s over 40 km, because the library's defaults (11.5 m/s over
    120 km, and 8.4 m/s over 310 km) are an ocean and this is a 28 m lagoon.
- **Camera:** `--cam-eye`, `--cam-target`, `--cam-up`, `--fov` (degrees). A look bug in a scene this
  size arrives as a reported viewpoint, and the orbit controller's state cannot be written on a
  command line.
- **Walker:** `--player`, `--walk-speed`, `--look-rate`, `--no-invert-arrows` (pitch is inverted by
  default; yaw never is). WASD moves, the arrows turn the head, Shift sprints, Space jumps. **The
  mouse does not touch the camera** and the cursor is never captured, so the ImGui sliders stay
  clickable. With `--cam-eye`, the walker spawns at its x,z.
- **Night** (spec 11.79):
  - Stars are **on by default** here, where the library leaves them off; `--no-stars` turns them
    off. `--star-hour <deg>` turns the field about the celestial pole, which moves the Milky Way
    band; tree defaults it to 90, where the arc crosses this framing. The fade-in runs +3 to -8
    degrees, so the default 0.8 degree sun shows the first few.
  - `--no-night-floor` drops the night-sky floor, the airglow that lights the world after sunset.
  - The moon is **on** here too (spec 11.82); `--no-moon` turns it off. It is exaggerated: size 6
    against the library's life-size 1, since a physical moon is a dozen pixels, and brightness 2,
    since at 1 the sea away from the glitter track sits in the bottom few codes. `--moon-size`
    scales the disc and its halo alone; `--moon-brightness` drives the disc and the light, so it is
    the one that lifts the sea, and keeps the sea physical where a scatter glow would not.
- **The clock** (spec 11.81): `--day-cycle <seconds>` runs it, real seconds per day, and
  `--time-of-day <h>` (0 to 24, noon at 12) wins over the sun flags; `--day-cycle 180
  --time-of-day 16` is the sunset. While a cycle runs both sun sliders grey out in both panels,
  since the tick owns them. Under a cycle the moon's phase and position follow the sun off one
  clock, so `--day-cycle 1` sweeps most of a lunar month in a 400-frame capture.
- `-c/--config <path>` restores a config snapshot, as the GUI's Dump Config button writes one.

**Two things about tree's defaults that read as bugs if you meet them cold.** At a 0.8 degree sun
the frame is lit mostly by sky and is dark. And the shadow map cannot hold the span such a low sun
needs, roughly 37,000 units against a 6,000 budget, so **shadows run off the map rather than
reaching across the island**. `--sun-elevation 8` is the framing that keeps both.

## apps/forest

**apps/forest is the scene where instancing, LOD and culling all matter at once** (spec 11.29).
- Capture: `--headless --frames --screenshot --profiler`.
- Levers: `--no-instancing`, `--no-lod`, `--lod-bias`, `--no-spatial-sort`, `--render-mode`,
  `--seed`, `--cam-eye`/`--cam-target`, `--glare` (the aperture's diffraction star, which the
  engine leaves off; spec 13.5).
- `--water` / `--water-level <f>` (spec 11.32) floods the terrain. `terrain_height_at` satisfies the
  water system's `WaterHeightFn` directly, so the surface shoals against the real terrain with no
  heightmap stored or copied, for about half a millisecond of GPU.
- Exposure is pinned in the app, so its frames compare without `--no-auto-exposure`.

**The island, the quadtree and the regions.** Forest is an island on a CDLOD quadtree with resident regions (spec 11.63), and these are the flags
to know before comparing any forest capture.
- The ground falls to a shoreline past 0.72 of the half-extent and the sea is on by default, so
  `--water` restates a default. `--no-island` takes the falloff and the sea away: the flat domain
  the arms written before the island measure.
- `--terrain-extent <f>` is the domain's half-width and the way to ask what a bigger world costs:
  16x the ground area takes the quadtree from 364 patches to 706, where the fixed grid goes from 64
  to 1024.
- `--no-quadtree` is the fixed tile grid, and not an identity: a quadtree draws a different surface
  at a different density.
- `--no-regions` is one always-resident region over everything. `--region-radius <f>` and
  `--region-span <f>` dial residency down far enough to churn. The shipping radius is wider than the
  world's own diagonal, so every region stays resident.
- `--walk <speed>` drives the character forward and turns about-face at the halfway frame. It is
  the only way a headless run crosses a region boundary: residency follows the camera, the camera
  follows the player, and no key is pressed.
- Probes: `--terrain-quadtree-probe` (selection, morph windows, seams, and how much relief each
  level gave up), `--region-probe` (residency and a per-cell placement digest), `--cluster-probe`.

**Two things about the island that read as bugs if you meet them cold.** Sea level is **-35, not
0**: the fbm is symmetric about zero, so a waterline there would flood half the interior. And the
island reads as a low disc: the fbm's relief inside is about ±15 units over a kilometre against a
140-unit rim, which is the terrain's own character, not the falloff. `--erode` is the cheapest way
to give it structure.

**The regions are a grid in the app, not a quadtree depth.** The scatter, the prototypes and the
collider are `forest.c`'s, and the quadtree's patches live at many levels while residency wants one
size.
- A region is seeded from its own cell coordinates rather than drawn from one global stream, so it
  can be freed and rebuilt identically. The clump field it rejects against is a function of
  position, so groves still run across a border.
- Its instance nodes go under the global per-prototype groups, not under a node of its own: a group
  per region would put a foreign mesh between every pair of instances the batcher wants to join, and
  every run would be length one. A region keeps the list of (group, node) pairs it added instead.
- **An origin shift frees and rebuilds every resident region and resets those groups to identity.**
  `scene_apply_origin_delta` translates every root child, which is right for instances placed before
  the shift and wrong for any placed after.

**The trail.** A gravel path runs from the island's centre to a shore as one road on the terrain material, and the
scatter keeps props off its course (spec 11.68). `--no-trail` is the ground without it.
- The trail draws from its own generator rather than `rnd()`, so the scatter's stream is unchanged
  and the reject only turns candidates away.
- It is subtle at this scale, three units wide under a canopy on a kilometre island, so read it
  through `--scatter-probe`'s `scatter-probe road` row rather than looking for it in a wide shot.
- It is held in terrain-local coordinates and handed to the material as local plus centre, the
  frame `splat_origin` is in, so an origin shift moves the road and the scatter test together.
- **A road makes the dominant-layer read ulp-sensitive under an origin shift**, as any splat feature
  sharp enough to flip the dominant index does. Two runs shifted by different amounts reconstruct
  `authoredPos` with different rounding, the dominant index rides the atlas alpha through an
  unfiltered `texelFetch`, and one flipped texel switches the whole detail tap. So `origin-auto`,
  the one arm demanding exactly 0 px between two different shifts, passes `--no-trail`, and
  `origin-shift` keeps the trail as the suite's only render of a road under a shift.

**Erosion and heightmaps.** Forest can erode its terrain, or load one somebody else made (spec
11.59). All off by default.
- `--erode`, with `--erode-res <n>` (**default 513, not 512**: the grid is node-centred, so it has
  res-1 cells and a mip pyramid halves only while that is even; at 512 the field gets no levels and
  every coarse quadtree patch point-samples the full-resolution surface), `--erode-iterations <n>`
  (default 220), `--erode-workers <n>` to pin the thread count, and `--erode-save <p>`.
- `--heightmap <p>` with `--heightmap-range <lo> <hi>` loads one instead. **`--heightmap` wins over
  `--erode`**: a file is a statement about what the terrain is, and re-eroding it would erode
  someone's finished work.
- Probes: `--terrain-erosion-probe`, `--terrain-height-probe`, and `--scatter-probe` (spec 11.60),
  which prints the drainage the scatter placed trees into beside the terrain's own distribution and
  the fraction of the domain the rule rejects. That fraction is the point: `erosion.c` normalises
  flow to its own peak, so a maximum near 1 is true of any sim that ran and says nothing about
  whether there were candidates to reject. `TREE_MAX_FLOW` is derived from
  `TERRAIN_CHANNEL_FLOW_LO/HI` and rejects about 10%.
- The bake costs 452 ms at 512² x 220 on eight threads and 1839 ms on one, in a release build. A
  debug build (`build.sh`'s default, with no `-O`) takes 2.3 s and 14.8 s, so time a bake from
  `out/release/bin`, not `out/bin`.

**A debug profile re-orders costs, not just inflates them** (`docs/verification.md`, Which binaries
a number describes). In a release build the scatter and the colliders are 6% of forest's startup. The largest
region-load cost in either build is Jolt's BVH builder, 85% of collider time, inside a vendored
library.

**Streaming a field.** Forest can stream a field rather than hold it (spec 11.69).
- `--terrain-stream <p.cts>` installs one. It is opt-in by naming a file, so there is no
  `--no-terrain-stream`, and a stream wins over both `--heightmap` and `--erode`.
- `--terrain-stream-save <p>` writes the installed field out and is also the `.r16` to `.cts`
  converter. It runs after the pyramid is built, since the levels are what the file stores, and it
  **refuses a field with no coarse levels**, which is what a 512 field or the 256² fixture gives.
- `--terrain-stream-budget <n>` (tiles read per update), `--terrain-stream-window <n>` (window edge
  in tiles), `--terrain-stream-resident-res <n>` (lowers the whole-level threshold so a
  fixture-scale field streams at all) and `--terrain-stream-probe <n>`.

**What it costs: resident memory tracks the coverage the app asks for divided by the cell, not the
size of the map.** A 4097² eroded field is a 300 MB file against 291 MB held whole; streamed over a
4 km domain the windows hold 86.5 MB, and over 8 km 36.5 MB, less, because doubling the world at a
fixed node count doubles the cell. Growing a world is free; refining it is not. On forest's own 1 km
world it buys nothing (274 MB), because the region radius asks for exact heights across the whole
world. It is a big-world feature.

**Two things about it that read as bugs if you meet them cold.**
- Level 0's window is sized by the larger of `--terrain-stream-window` and the coverage a region
  load needs, which comes from `--region-radius`. At the shipping radius the finest window covers a
  kilometre domain outright and nothing about streaming is observable, so **`--region-radius` is
  the lever that makes level 0 stream**. Every streaming gate arm passes it, and the walk arm
  inherits `REGION_CHURN`.
- At forest's default 513 field, `RESIDENT_RES` 1025 holds every level whole, so streaming settles
  into an exact identity: that is the identity configuration, not a failure to arm.

**The `.cts` fixture is generated at gate time, not committed.** `terrain_fixture.r16` is 256 nodes
and 255 is odd, so it has no coarse level and the save refuses it; the streaming arms paint a
257-node twin from the closed form `gates.py` already states.

**The derived-data cook.** Forest and render take the same three flags (spec 11.99).
- `--cook-dir <p>` points the cache somewhere other than the default, `cooked/` at the repo root or
  `CETRA_COOK_DIR`.
- `--no-cook` runs every bake live and touches no cache. **Any measurement of a bake must carry
  it**, since a warm cache serves the artefact instead of running the work. The terrain gate group
  forces it, because `terrain-threads` varies a worker count the cook key omits.
- `--cook` is the pre-warm verb: a headless run whose deliverable is the per-artefact
  `cook site=...` rows and the `cook-summary`. On render it exits when the async loader has drained,
  so a texture-heavy model warms fully; since a headless run drains the loader before frame 1, that
  is the second frame for what the start-up asked. A `--cook` against a warm directory exits 0 with
  `cooked=0`, which is success.
- `--terrain-erosion-probe` under a cook hit refuses by name rather than printing stats, since the
  sim it describes never ran; pass `--no-cook` to measure it.
- On the gate fixture (eroded 257 x 60) a cold start is about 6.7 s debug and a warm one about
  2.7 s.

**A world away from the origin** (spec 11.62):
- **`--world-offset <units>`:** places the terrain, scatter, lights, camera and physics that far
  from the origin, the offset carried in every coordinate. It is the instrument for fp32's
  precision far out.
- **`--origin-shift-at <frame>`:** re-centres the world on the camera at that frame, in the
  `--shadows-off-at` idiom, since a shift is a transition no headless run reaches otherwise.
- **`--origin-shift-distance <units>`:** re-centres automatically once the camera drifts that far.
- **`--water` is skipped, with a warning, under any of the three.** Water's bed domain is a
  half-size about the storage origin with no centre of its own, so a moved world would shoal
  against terrain that is not under it.
- The render app's **`--shadow-center <x,y,z|auto>`** sets `ShadowSystem.scene_center`, which
  nothing else outside the process reaches.

What moving a world costs is mostly not precision: a read of a world position as an identity, such
as forest's wind phase hash, must take the authored position (`docs/architecture.md`, Step 4 and
the transform walk). Precision itself degrades gradually: about 0.1% of the frame at an offset of 12, 4% at
12,000 and 45% at 262,140.

The contracts these flags reach (a field's two residencies, its levels, island shaping, bicubic
sampling and clamping, thread safety, what the masks mean, the erosion's equilibrium and its digest)
are in `docs/terrain.md`. `terrain-clamp` samples the first cell past the edge, the only place
clamping the coordinate and clamping the taps differ.

**Wind on the trees.** Both prototypes come from `tree_gen`, which authors the UV1 branch phase and
flex weight, so wind is a material field. A swaying mesh carries a cull bound, so the trees stay
cullable. Wind is evaluated in object space from per-mesh uniforms, so without `phase_variation`
every instance of a shared prototype sways on the same beat; the scene sets it.
`wind.phaseVariation` is a `.cscn` key with no CLI flag, defaulting to 0 (lockstep). The phase is
derived from the object's world origin rather than stored per instance: an `InstanceBlock` field
would cost a kilobyte of std140 padding to restate the model matrix, and a phase keyed to batching
would flip as LOD and culling reshuffle runs.

**Scatter order decides whether batching works at all.** The batcher joins only consecutive
survivors, so with most props frustum-culled a randomly ordered scatter collapses to runs of one.
The instances are Morton-ordered within each prototype, which takes the same 2,979 instances from
2,368 draws to 1,287; `--no-spatial-sort` scatters in draw order instead, and `forest-order`
asserts the difference. **LOD fights instancing**: the batch key is `(mesh, lod)`, so one prototype
spanning a range of distances splits into separate runs, a CPU-for-GPU trade whose direction
depends on the framing.

**Instancing and LOD are on by default, and both flags are escape hatches.**
- **`--no-instancing`:** one draw per mesh. The batched and unbatched paths carry the same floats
  into the same arithmetic, so it compares at 0 px; it is for bisecting.
- **`--no-lod`:** every draw at LOD level 0. LOD chains are built at import by meshoptimizer,
  which weights boundary and seam edges heavily, so leaf cards and grass blades barely simplify,
  and a skinned mesh is refused, since weights do not transfer to surviving vertices.
- Borders are weighted rather than locked, and only the `triangles` column of the submission table
  sees a level change: `docs/architecture.md`, The draw list.

---

## apps/silent

What the game is, its controls and how its world is built are in `apps/silent/README.md`. These
are its flags. The pinned views are in world coordinates, at `-W 960 -H 540`.

### Capture, framing and playing headless

- **`-x`, `-f <n>`, `-S <path>`, `--screenshot-every <n>`, `-W`/`-H`** (default 1600x900).
- **`--cam-eye x,y,z` with `--cam-target x,y,z`:** pins the camera, so a view can be photographed
  twice and a change A/B'd. The two go together; one alone is ignored with a warning.
- **`--fov <deg>`:** the vertical field of view (default 68). It wins over the settings file.
- **`--player-at X,Z[,YAW]`:** starts the player there, standing on whatever is under them (the
  ground, a floor, the dock), facing YAW degrees from +z. `-52,30` is the south barricade's gap,
  `-65,113` the track's foot at the cabin, `-78,118,90` the dock looking at the cabin. Over open
  water it stands the player on the bed, under the surface, so look at the lake from a pinned view.
- **`--seed <n>`:** reseeds the clutter on the counters and shelves, and the street: which windows
  are lit, blinded or boarded.
- **`--pad-script <file>`:** replays a scripted gamepad on slot 0, in the format
  `cetra/src/game/input.h` gives. **`--trace-player`** prints the player's position every 30
  steps. That is gametest's pair, so a walk can be driven and read back with no controller.
- **`--profiler`:** per-pass timing and submission counts, at exit.

Some pinned views:
- the kitchen towards its window: `--fov 72 --cam-eye 2.2,1.95,13.55 --cam-target 2.2,1.25,10.1`;
- the street past the nearest lamp: `--fov 70 --cam-eye -0.8,1.7,5.0 --cam-target -16,1.6,-1.5`;
- the house from the street: `--cam-eye 4,1.7,-2 --cam-target 0,4,14`;
- the hall, from the front door and back: `--cam-eye -0.75,1.9,10.5 --cam-target -0.75,1.5,19`
  and `--cam-eye -0.75,1.9,18.9 --cam-target -0.75,1.5,10`;
- the living room and the bathroom: `--cam-eye -1.9,1.9,11.0 --cam-target -3.6,1.0,15.5` and
  `--cam-eye 0.3,1.9,14.1 --cam-target 2.1,1.0,15.9`;
- the street's end, where the drive leaves it: `--cam-eye 30,1.7,1.5 --cam-target 60,2,0`;
- the mansion from the drive: `--cam-eye 115,11,24 --cam-target 115,12,45`;
- the dining room: `--cam-eye 115.6,9.9,53.3 --cam-target 118.5,9.0,51.2`;
- the armour in the mansion's hall: `--flashlight --cam-eye 114.75,9.6,52.3 --cam-target
  113.7,9.2,53.15`;
- the layout from above: `--day --no-rain --no-fog --cam-eye 70,160,25 --cam-target
  70.01,0,25.2`.

### The title, saves and pause menu

A windowed run comes up on the title, where Load Game lists the games kept, newest first, and
Delete asks about the one the list is on. A game saves itself into its slot, `silent-1` to
`silent-8`, when something is taken, a place is found, the player goes into another room (at most
every 10 s) and on quitting. The slots are JSON files in the per-user directory (`~/Library/Application Support/cetra` on macOS), which
`CETRA_SETTINGS_DIR` moves. `silent-settings.json` (the settings) and `silent-start.json` (the
world as built, which Quit to Title puts back) sit beside them.

**A run that starts somewhere of its own skips the title, saves nowhere unless `--slot` says where,
and has no Quit to Title.** That is a headless run, or one started from a state on the command
line: any of the flags below, or the player, the bag, the map, the key, the finds, the crouch, the
story, the haunting, the cat's place or a pinned camera.
- **`--load <slot>`:** starts from the game saved in `<slot>`.
- **`--new-game`:** starts a new game, without the title.
- **`--slot <slot>`:** saves this run into `<slot>` as it goes.
- **`--no-autosave`:** a game from the title that never saves itself.
- **`--title`:** the title headless too, for pictures and pad scripts.
- **`--title-screen <s>`:** the title with `games` (the list) or `delete` (asking about the newest)
  over it; implies `--title`.
- **`--pause-screen <s>`:** the pause menu (`menu`), or the settings over it (`settings`), up once
  the game is played, for a headless picture.

### The frame

**One sample with jittered TAA, headless as well as windowed**, which is not the engine's usual
headless setup:
- **MSAA outlines the street.** The fog composite takes one depth for an edge pixel, and where half
  its samples are sky that depth is too near, so every silhouette comes out traced in a dark,
  unfogged line.
- **Headless TAA without jitter does nothing useful**: it integrates one sample position forever,
  and the engine refuses a render scale under it.

So a screenshot is what a player sees, and it still repeats exactly, because the jitter follows the
frame index.
- **`--no-taa`:** no temporal filter and so no upscale: full resolution, raw edges.
- **`--msaa <n>`:** puts multisampling back, outlines and all.
- **`--render-scale <f>`** (0.5-1, default 0.5): the upscale. It rides on TAA, so `--no-taa`
  ignores it. At 0.5 fine texture turns to mush in a 1:1 crop and changes little at viewing size,
  since 0.5 of a Retina window is still about 960x540.

### Loading and the cooked light

In a window it loads under the engine's loading screen (spec 13.34). A headless run has no screen,
so its frames are the game's from the first.
- **`--no-loading-screen`:** loads under a black window instead.
- **`--no-play-prompt`:** the screen switches off by itself once the game is ready, with no PLAY to
  press.
- **`--loading-screen`:** the screen headless too. Its clock is then a frame's fixed step, so it
  lifts later in frames than it does windowed on the wall clock.
- **`--capture-budget-ms F`:** the most of a frame the light captures may take, headless too. By
  default it is `LOADING_CAPTURE_MS` (100) behind the loading screen and the black, the engine's
  budget after it, and no limit headless.
- **`--startup-ms`:** prints a `startup-ms` row for each loading step and held frame, how long it
  took, and rows for when the lighting is in (`lit`) and the view comes up (`up`).
  - Each row's `still=` is the longest the loading screen stood without a chance to draw in it, and
    the `lit` row's `worst-still=` the longest of the held frames after the first three (spec
    13.39, `engine_loading_screen_take_longest_wait`).
  - `frame0` holds the sounds' last decoding, which `game_run` waits for before the first frame,
    and headless any texture still loading, which `engine_run` waits for (spec 14.8).
  - It is counted headless too, so `-x --loading-screen --capture-budget-ms 100` measures what a
    window would freeze for.
  - The `lit` row is followed by each GI volume's `stream gi` line with a digest of its tiles, so
    two runs that captured the rooms alike can be told from two that did not (spec 13.42).
    Headless, the view comes up the frame the lighting is in, and that frame prints both rows.
- **`--capture-timing`:** each GI volume prints its `gi-timing` row as it converges, as the render
  app's flag does. It slows the sweep, so time `lit` without it.
- **`--gi-cell M`:** the home's GI cell in metres (default 1.25), laid over the house's bounds.
- **`--gi-cull-pixels F`:** a GI probe leaves out what spans fewer pixels than F across its faces;
  0, the default, takes everything.

**The light is cooked** (spec 13.42): the GI volumes' opening sweeps and the reflection probes'
columns go into the cook (`cooked/`, or `CETRA_COOK_DIR`) the first time, and a later launch whose
scene is the same loads them, `lit` at about 2 s rather than 13. Any change to the engine's
source, the driver or what the captures see misses and captures again. `CETRA_NO_COOK=1` captures
live.

### Walking and crouching

The gait and the crouch are described in the README's "The player" (specs 13.47 and 13.48).
- **`--no-interp`:** draws the body where the last step left it, for the A/B.
- **`--no-gait`:** the eye rides the body rigidly, with no bob, and a climb is not paced.
- **`--bob <x>`:** the head's bob as a share of the gait's own (default 1; 0 is none).
- **`--climb <x>`:** the pace up and down a flight as a share of the gait's own (default 1).
- **`--no-footsteps`:** walks without a sound.
- **`--footstep-say`:** plays every footstep take in turn, two a second, for an audio dump.
- **`--trace-gait`:** prints every fixed step's move (feet, rise, run, ground state and normal,
  speed, grade, how much is a flight, pace, whether crouched), every frame's feet, eye, phase,
  speed and how far the eye has gone down to a crouch, and every footfall with its surface. A
  surface with no recordings says so at startup and in this trace.
- **`--crouch`:** starts crouched, as though C had been pressed.

### The backpack, the map and the story

Until the backpack is taken, F finds no flashlight and the cellar flight's foot is as far as the
player goes. On the backpack's screen the arrows, WASD or the D-pad walk the grid, Enter, E, pad A
or a click chooses, and Tab or Escape (pad B) shuts it. On the map a drag, WASD, the arrows or the
left stick move the view, and the wheel, `=` and `-`, or the triggers zoom it; M, Tab or Escape
(pad B) shuts it. Tab from the game's map goes on to the backpack, and the map opened from the
backpack goes back to it.
- **`--backpack`:** starts with the backpack, the flashlight in it.
- **`--flashlight`:** starts with the torch on, and so with the backpack.
- **`--open-backpack <item>`:** starts with its screen open on `flashlight`, `map` or `key`, for a
  headless picture. It implies `--backpack`; `map` implies `--map` and `key` implies `--key`.
- **`--map`:** starts with the map in the backpack, the fridge bare; implies `--backpack`.
- **`--open-map`:** starts with the map's screen open, over the backpack's under
  `--open-backpack`; implies `--map`.
- **`--found <list>`:** starts with places found, written on the map when it next opens:
  `barricade`, `road-end` and `cabin`, comma-separated, or `all`. The house is found with the map.
- **`--map-export <path>`:** writes the town's plan, every shape the map prints, for
  `tools/make_map.py`, and stops, headless with no cat and no loading screen. The committed plan is
  `apps/silent/tools/town_plan.txt`; re-export it and rerun the tool when the street, the lake, the
  hill or the land changes.
- **`--key`:** starts with the front door's key in the backpack, its birdcage open and empty on
  the workbench, and nothing begun; implies `--backpack`.
- **`--story <stage>`:** starts the story at `locked`, `haunted` (the key taken: the haunting
  begins as the game is played) or `open` (the front door unlocked). A player started anywhere but
  in the house starts it `open` whatever this says.

### The birdcage and the haunting

Neither the hold of the birdcage nor the player's place in it is saved: a load or a return to the
title puts the cage back on the bench, open exactly when the key is had.
- **`--inspect pickup|closed|open`:** the cage in the hands once the game is played, wherever the
  player stands: picked up from the bench, already held shut, or held open with the hand waiting.
  It skips the title.
- **`--trace-hold`:** prints each of the hold's states and its clips' events, with the frame each
  came on.
- **Headless:** with `--pad-script`, `x` on a frame is E, and `b` puts the cage down.
- **Pinned view** of the cage on the bench, with `--backpack --flashlight`: `--cam-eye
  3.95,-0.75,16.35 --cam-target 4.61,-1.38,16.45`.

The haunting (spec 13.52) ends with the house put back as it was; the doors stay where it moved
them.
- **`--haunt`:** begins it as the game is first played, and skips the title.
- **`--haunt-at S`:** begins it S seconds in, the slam and the creak past and the bathroom door
  where the creak left it; past 4 s the tries on the cellar door are under way.
- **`--trace-haunt`:** prints each beat as it comes (when it was watched for, the slam, the creak,
  each try with its turns of the knob and its shakes, the tries over) and each turn of the story (a
  door tried, the key taken and the haunting begun, the front door unlocked, a load haunted again).
- **Headless:** with `--pad-script`, `rstick` on a frame presses K.

### Lighting and shadows

- **`--no-gi`:** no bounce light: no GI volume, and no reflection probes, which are captured from
  its light. It separates a change in the direct light from one in what the captures saw.
- **`--no-candles`:** the candles stand unlit.
- **`--no-candle-shadows`:** the candles light through walls; the home's hall lantern keeps its
  shadow.
- **`--tile-views <n>`:** every cached light from n views over its body rather than eight. At 1,
  harder, darker pools under the candlesticks.
- **`--tiles-probe`:** the tiles and each light's block, at exit.
- **`--tile-stores <n>`:** store cells for faces that see a mover (default and at most 64); 0 draws
  each such face whole, every frame.
- **`--no-flicker`:** keeps the failing ceiling tube steady. The flicker is a pure function of the
  sim clock, so a headless frame catches the same instant every run, and one that lands in a
  stutter shows the tube dark and reads as an underlit room. Use it when comparing lighting.
- **Probe Coverage**, in the Render Mode list under G (spec 13.23), turns magenta whatever falls
  outside the thirteen reflection probes and reflects the sky. Indoors only the gap round a front
  door should, since it looks outdoors.

Pinned views of the cached shadows:
- the study desk's candlesticks: `--cam-eye 111.0,12.7,51.0 --cam-target 110.55,11.92,50.1`;
- a great hall floor stand against the panelling: `--cam-eye 112.7,9.75,57.5 --cam-target
  112.1,9.5,59.3`;
- the candelabra on the dining table: `--cam-eye 119.6,10.2,50.6 --cam-target 117.0,8.9,52.3`;
- the home's clock under the lantern, through the cased opening: `--cam-eye -0.2,1.5,14.6
  --cam-target -1.45,1.0,12.9`.

### The look, the day and exposure

- **`--no-grade`:** the frame without the green-grey LUT (`assets/lut/silent_grade.cube`, written
  by `apps/silent/tools/make_grade.py`).
- **`--crt`:** the game on a consumer CRT television (spec 13.28), the door prompt and menus on it
  and the debug GUI over it; off by default. Silent has no film grain either way. At the default
  window the 480 lines are 3.75 framebuffer pixels apart and show fully; at `-W 960 -H 540` they are
  2.25 apart and a little fainter, and below 1.5 apart they fuse.
- **`--no-static`:** the living room's television showing nothing (spec 13.30): its glass a faint
  blue-grey, no snow and no hiss.
- **`--no-fog`:** drops the fog volumes and the haze. The trees and the lake's water are then
  drawn at any distance.
- **`--day`:** an overcast day (spec 13.7). The sky is photometric by night and day; by day it is
  fully overcast, so nothing casts and the fog takes the dome's light. The exposure is metered by
  day but never opens a room past the night's pinned exposure, so the kitchen by day is exposed as
  at night plus its window's daylight, and the street closes down to a light-grey fog world.
- **Local exposure, by day only** (spec 13.19): highlights 0.2, shadows 0.6, blend 0, so the
  kitchen window onto the street reads and the room is lifted.
  - `--no-local-exposure` drops it and `--local-exposure` forces it on.
  - `--le-highlights`, `--le-shadows` and `--le-blend` override the day's values.
  - `--le-probe` prints the luminance range it is asked to fit; `--exposure-probe` prints what the
    meter decided.
  - The same knobs are sliders under G, in the Post section; G frees the mouse while the GUI is
    open.
- **`--no-history-rescale`:** the fog and the temporal histories no longer follow the exposure as
  it adapts (spec 13.20), so stepping outdoors settles slowly, as an expanding sphere. For
  comparison.
- **`--negative-probe`:** prints, each frame, how much of the frame reaches the tonemap below zero,
  as the render app's does (spec 13.21).
- **`--fog-history-miss-samples N`:** how many samples a fog cell entering the view takes, 1..8
  (spec 13.45). At 1 a turn draws a band at the screen's leading edge. It is History Miss Samples
  under G, in the Volumetric Fog section.

### Rain and wind

It rains by default at 6 mm/h (spec 13.9), already soaked. What silent changes from the engine's
physical rain is a look, each a constant in `silent.c` (the README's "Rain and weather").
- **`--rain <mm/h>`:** another rate. **`--no-rain`:** the dry street.
- **`--no-wind`:** still air. By default a gusting breeze drives the rain onto the house, which is
  what wets the facade and beads the kitchen window under its eave.
- **`--no-relief`:** puddles where the noise puts them, not in the lows of the road's, the
  pavement's and the yard's own scans.

The drips (the gutter's leaks, the downpipe's shoe, the neighbours' eaves) have no switch. Pinned
views of them:
- through the kitchen window: `--cam-eye 2.6,1.75,11.0 --cam-target 2.6,1.6,8.0`;
- outside, the overflow past that window: `--cam-eye 2.0,1.7,7.9 --cam-target 2.6,2.0,9.6`;
- the downpipe's shoe: `--cam-eye 3.6,0.9,8.3 --cam-target 4.8,0.5,9.7`;
- a neighbour's eaves: `--cam-eye 9.0,1.7,6.5 --cam-target 14.0,2.5,10.0`.

### Sound

- **`--mute`:** silences everything. It wins over the settings file.
- **`--audio-dump <path>`:** with `-x`, writes what the listener hears as a 48 kHz stereo WAV,
  pulled a frame at a time so it keeps step with the picture, from the first frame through the
  title and the pause menu as a device plays; the run prints how far into it play began. With
  `--cam-eye` and `--cam-target` it is how a spot sounds without a window. Without `-x` it is
  ignored, since a windowed run plays to the device and leaves nothing to pull.

### The town's edges, the lake and the boat

- **`--no-woods`:** without the woods behind the yards and round the crossroads, or anything lying
  under them: the A/B for their cost, about 6.6 ms of a frame from a yard (4.7 of it on the GPU)
  and 1.6 from the street, at 1600x900 by night. The camera draws a tree only within the fog's
  reach, 45 m by night and 30 by day; the shadows still take every tree, which is most of what is
  left from a yard.
- **The lake's water is on only within the fog's reach of it plus 15 m.** `--no-fog` keeps it on
  everywhere. The chasm at the same depth stays dry either way.
- **`--lake-waves fft|gerstner`:** the lake's wave model (spec 13.56). FFT is the default: a breeze
  of 5 m/s over 150 m and no swell, about 1.2 ms more CPU at the dock than Gerstner, and only
  within the lake's reach.
- **`--in-boat`:** starts sat in the boat at its mooring.
- **`--boat-at X,Z[,H]`:** sat in it out on the lake, heading H degrees from +z (default -90, the
  way it moors); `-100,120,-60` is near the middle.
- **`--boat-running`:** with its motor running. **`--pull-cord`:** with its pull cord's bar up.
  Each of the boat flags implies `--in-boat`.
- **`--trace-boat`:** its place, speed, slip, turn, throttle, tiller, motor and how far it is into
  the open water, every half second of steps. With `--pad-script` it is how its handling is read
  headless.

Pinned views:
- the terrace's stairs: `--cam-eye -9,1.7,-1.5 --cam-target -7,2.6,-9`;
- our back yard and the dead tree through its fallen fence: `--cam-eye 0,1.7,23 --cam-target
  2,3.0,40`;
- the crossroads, toward the chasm: `--cam-eye -50,1.7,0 --cam-target -66,1.0,0`;
- the north barricade: `--cam-eye -51,1.7,-6 --cam-target -52,1.2,-20`;
- the lip's face and the leaning pole from inside the chasm, with `--no-fog`: `--cam-eye
  -75,-6,14 --cam-target -67,-2,0`;
- the cabin from the dock, the fire through its door: `--cam-eye -78,-9.45,118 --cam-target
  -64,-9.6,118`;
- out over the lake from the dock: `--cam-eye -80,-9.45,118 --cam-target -102,-11.6,127`;
- inside the cabin, toward the fireplace: `--cam-eye -66.4,-8.75,116.2 --cam-target
  -62.6,-9.5,118.6`; toward the table and the door: `--cam-eye -62.9,-8.8,116.0 --cam-target
  -66.0,-9.5,120.4`;
- the valley from over the south wall, with `--day --no-rain --no-fog`: `--cam-eye -80,95,170
  --cam-target -80,-10,100`;
- the dock lamp and the moored boat from the dock's foot: `--cam-eye -77.5,-9.45,118.3
  --cam-target -84,-10.3,116.8`; the lantern close: `--cam-eye -82.3,-9.5,118.0 --cam-target
  -83.5,-9.15,116.7`.

### The cat

G shows the cat's panel (what it is doing and every activity's score, its needs, what it senses,
an activity or a place to send it to, its colours) and draws its place graph over the frame: links
by kind, its route, its sight line green or red, and what it watches.
- **`--no-cat`:** without it.
- **`--cat-fur RRGGBB`**, **`--cat-eyes RRGGBB`:** its coat (default 262424) and eyes (E8B923),
  sRGB hex. A light coat takes pink ears and nose.
- **`--cat-at <place>`:** where it starts; the usage line lists the places it rests at.
- **`--cat-clip <name>[@s]`:** holds one clip there, playing or `s` seconds in. Nothing moves it
  and it has no mind.
- **`--cat-goto <place>[:trot|:run]`:** sends it there once it is in the house, by the rail if that
  is the way. Scripted, so it has no mind.
- **`--cat-activity <name>`:** starts its mind on one activity: sleep, stretch, loaf, explore,
  watch_flame, window, avoid, startle, follow or rail.
- **`--cat-seed <n>`:** its mind's seed (default 1). Two runs on one seed trace identically.
- **`--cat-blind`:** it neither sees nor hears the player.
- **`--cat-cam`:** the camera rides behind it, at a fixed offset, so it goes into walls.
- **`--cat-say`:** it makes every sound it has, one every 1.5 s, and purrs. With `-x` and
  `--audio-dump` that is how its sounds are heard without a window; the dump less the same with
  `--no-cat` is the cat alone.
- **`--trace-cat`:** every 30 steps, where it is, what it plays, the link it is on, and what it
  wants.
- **`--no-eyeshine`:** its eyes do not throw the flashlight back.

The cat senses the player's body, never the camera, so a pinned camera or `--cat-cam` is only a
view: it watches for the player where the body stands, at the spawn point in a headless run.
Pinned views:
- asleep on the study chair: `--cam-eye 111.30,12.05,48.70 --cam-target 109.98,11.72,49.02`;
- on the dining chair, watching the candelabra: `--cat-at head_chair --cat-activity watch_flame
  --cam-eye 116.6,9.4,50.9 --cam-target 116.0,8.9,52.05 -f 80`;
- on the window seat, watching the rain: `--cat-at window_seat --cam-eye 117.7,9.4,52.6
  --cam-target 117.7,8.9,50.2`;
- its eyes in the flashlight, lying on the great hall's rug: `--flashlight --cat-at rug --cat-clip
  lie --cam-eye 114.83,9.2,57.78 --cam-target 114.25,8.42,56.4`;
- on the rail, from the great hall: `--cat-at gal_w --cat-goto newel_cap --fov 60 --cam-eye
  115.40,9.70,58.00 --cam-target 115.40,12.10,55.00 -f 900`.

## The other apps, and the AA mode each one chose

Each app's anti-aliasing is a decision of its own, stated in its entry; none simply inherits the
default.

### apps/gametest

What it is, its controls and how its player animates are in `apps/gametest/README.md`.

**Anti-aliasing:** one sample plus TAA windowed, the render app's policy, because every surface is
a rigid mesh on the `pbr` program and so writes a motion vector. `--taa` and `--msaa <n>` override
it.

**Capture:** `-x`, `-f`, `-S`, `--screenshot-every`, `-W <n>` and `-H <n>`. The HDR environment is
positional: an unrecognised token is taken as its path.

**It is frame-deterministic headless**: the engine hands the loop its fixed frame dt, the seed is
`srand(42)` and Jolt steps a fixed dt, so two runs of one script trace byte-identically and render
0 px apart. Keep the FPS overlay off when comparing frames, since it draws the wall clock. Runs at
different sample counts are not comparable.

**Playing it headless:**
- **`--pad-script <file>`:** replays a scripted gamepad on slot 0 (the format is in
  `cetra/src/game/input.h`). A file that is missing or will not parse exits 1, so a gate never
  mistakes an idle pad for a passing layer.
- **`--gamepad-db <file>`:** adds an SDL controller mapping file to GLFW's bundled table, with the
  same refusal.
- **`--trace-player`:** prints the player's pose, velocity, ground state and the move and jump the
  step acted on, every `--trace-every <steps>` (default 30). Three tails follow the columns the
  `gamepad` group reads: `anim <knob> <w_idle> <w_walk> <w_run> <fade> <layer> <source>` after
  `jump`, `yaw <radians>` after the camera's columns, and `graph <state> <seconds>` after `yaw`.
  New columns are appended, never inserted, so that regex keeps matching; and no graph state may be
  named with `" yaw "` in it, since the regex is greedy. The `source` column says what is playing,
  which a state and its resume can share; `graph` says which state chose it.
- **`--print-bindings`:** lists the action table and exits.
- **`--no-crates`:** no falling crates at startup. They fall at `rand()` positions, which differ
  per platform's libc, so an arm that reads where the player ended up passes it.

**The player:**
- **`--no-puppet`:** the red box in place of the rig, and the all-rigid frame.
- **`--puppet <path>`:** any rigged humanoid; also selects the skeleton for `--ragdoll-probe`.
- **`--twin <clip>`:** a second rig beside the player playing its own clip.
- **`--no-chaser`:** without the second puppet that chases the player.
- **`--no-ik`:** no foot planting, the quickest way to tell a planting artefact from an animation
  one. The walk clip bends no knee of its own, so on flat ground the two look the same.
- **`--no-lock`** (spec 12.9): keeps planting and drops the locking above it: contacts are still
  found and feet still meet the ground, but no world-space point is held. On the generated puppet
  it changes nothing, since its walk is a straight-leg pendulum with no stance to lock; on a rig
  whose clips imply a stride, `--puppet assets/models/t_pose.fbx`, it does.
- **`--no-root-motion`** (spec 12.18): puts the in-place locomotion pair back on the generated
  puppet, whose default `travel_walk` and `travel_run` state their own travel (full stick 6.40 m/s
  on its node scaled 2). Without either root motion or a stride, travel is a fraction of
  `PLAYER_SPEED`. The startup line says which: "Locomotion travels 0 to 6.40 m/s, stated by the
  clips" or "Locomotion clips imply no stride". An imported rig never enters root motion, since no
  committed FBX clip states a travel.
- **`--speed <m/s>`:** caps travel, and overrides the speed derived from the clips.
- **`--no-swim`:** withholds the stroke clips, which makes the water state unreachable; the run
  says so once by name and prunes every row into it. It is how a rig with its own walk and no swim,
  the ordinary downloaded humanoid, is reached on a committed asset.
- **`--print-graph`:** prints the animation graph's table and what bind made of it (the states, the
  rows in evaluation order, and an `x` against everything this rig cannot reach), then carries on.
  On the committed default `air` and `land` are pruned, since it carries no `fall_cycle` and no
  `touch_down`, so a jump is the rise alone. The full airborne path needs a rig with no locomotion
  of its own, such as `--puppet assets/models/t_pose.fbx`, which is what `graph-parity-air` runs
  on.

The ground for planting is always in the scene: a ramp rising 1 in 4 from x = 14 (walk +X) and
three half-metre steps at x = -15, -17 and -19 (walk -X). On flat ground planting is correctly an
identity.

**The camera:**
- The follow camera is on by default; the arrow keys and the right stick turn it, orbiting the
  player at a fixed distance with the pitch clamped above the floor. **`--no-follow-cam`** gives a
  mouse-drag orbit about the world origin instead; the two both own the camera, so they exclude
  each other.
- **`--cam-eye x,y,z`, `--cam-target x,y,z`, `--cam-up x,y,z`, `--fov <deg>`** state a pose, the
  render app's spelling. Eye and target come together or not at all: half a pose is refused by
  name. A stated pose stands the follower down, and overrides `--no-follow-cam`'s orbit too.
- **Movement is camera-relative under the follow camera and world-aligned otherwise.** The follow
  rig sets `steers_controls`; the drag orbit and a stated pose leave it false, since pinning a
  camera states a pose and must not rotate the controls. `pad-camera-relative` says which is live.
- The arrows are shared with menu navigation, safely: `ui_up` and `ui_down` carry the `ui` flag, so
  a menu suppresses the camera's actions while it is open, and with none open the UI ignores the
  arrows.

**Audio:**
- **`--mute`:** silences the master bus.
- **`--glare`:** turns on the aperture's diffraction star, which the engine leaves off (spec 13.5).
- **`--audio-probe <case>`:** a headless offline render with no device (miniaudio's `noDevice`
  engine), so it runs anywhere the gate suite does. It plays a fixed geometry, measures the mixed
  PCM, prints it and exits. Cases: `onset`, `pan`, `distance`, `master`, `decode`, `noise`, `zone`,
  `zone_path`, `zone_start` and `zone_fade`, which the `audio` group reads. `decode`, `zone` and
  `zone_fade` take the WAV to load from `--audio-file <path>`.

**Menus** (spec 12.2):
- **`--no-ui`:** runs without the menus and the HUD.
- **`--ui-screen <name>`:** opens `main`, `pause` or `settings` at startup, since a headless run has
  no Escape to press.
- **`--ui-focus <n>`:** then presses "down" n times through the real navigation path. Nothing is
  focused to begin with, so the first press only takes the focus: `2` reaches the second button.
- **`--ui-probe <case>`:** the `ui` group's probe, printing `ui <case> <label> <key> <numbers>`.
  Cases: `layout`, `layout-resize`, `wrap`, `nav`, `hit`, `capture`, `stack`, `theme-identity` and
  `settings`. It draws no frame. `settings` needs no engine and runs before one is created; every
  other case needs a font, since FIT sizing is measurement, so it takes a headless game, a hidden
  GLFW window and a live GL context.
- **`CETRA_SETTINGS_DIR`** points the settings, the quicksave and every probe at a directory of
  their own rather than the player's; the gates and the golden bake set it.
- **`--ui-bench`** (spec 14.3): prints the UI's timings as `ui-bench <what> <how> <ms> spread <ms>`,
  each the median of five runs and the spread between the fastest and the slowest, and exits:
  - a frame's UI work and its two layouts on four screens of pcb's size: the chat's column at 200
    and at 4,000 messages, the jump over 5,000 names, and a settings screen of 40 elements;
  - a 13.7 KB text walked, and the share of it spent kerning;
  - one keystroke in a multi-line field of 1, 20 and 1,000 KB;
  - `load_font` from an empty pool;
  - the code editor (spec 14.4) on texts of pcb's lines at 80 KB, 10 MB and 64 MB in a 900 by 700
    editor: opening the text (`open_ms`), the first frame, the budget's walk until every row is
    wrapped (`settle_frames`, its total and its longest frame), a keystroke and Enter at one caret
    and at 100, a jump far down, and a resize.

  Run it from a release build (`./build.sh --release`, then `out/release/bin/gametest -x
  --ui-bench`). No gate reads it, since timing does not travel.

**Display modes** (spec 12.15):
- **`--fullscreen`**, **`--borderless`:** start the run in that mode, at the monitor's current
  video mode; nothing ever changes a display's resolution, so `--render-scale` stays the
  performance lever. The settings screen has the same three modes, and a Monitor row when there is
  more than one display.
- **`--monitor <name>`:** which display. **`--list-monitors`** prints the names and exits.
- **`--display-probe <case>`:** the `display` group's probe, printing `display <case> <label> <key>
  <numbers>`. Cases: `placement`, `monitors`, `apply` or `all`; an unrecognised case fails the run.
  It reads what a mode decides, not what it does: the switch is refused headless, so a suite never
  seizes a display and a golden's size never comes from the machine's monitor.

**Ragdoll** (spec 12.16): K or the left stick click drops the player into a simulated heap, and
there is no way back up.
- **`--ragdoll-probe <case>`:** the `ragdoll` group's probe. Cases: `build`, `shapes`, `scale`,
  `settles`, `pose`, `frees` or `all`. A headless game carries a real physics world, so the last
  three step Jolt and read the bodies back. A case prints every row its world can give, not only
  the one it names: `build` prints the shapes rows, and `settles` prints pose, roundtrip, rigid and
  frees. That keeps the group at four processes for eight arms, and its reader keeps every row.

**Saving** (spec 12.3): F5 or the right bumper quicksaves and F9 or the right stick click
quickloads. They are not `ui` actions, so a menu suppresses them and a save never holds the menu's
state. The file goes beside the settings in the per-user directory.
- **`--save-probe <case>`:** the `save` group's probe. Cases: `roundtrip`, `entities`, `spawned`,
  `drops`, `floor` and `migrate`. It builds its own scene, physics world and entities, with no
  window and no frame, but it does create a GL context, since the crates it spawns upload their
  meshes. Give each case its own `CETRA_SETTINGS_DIR`: they share one slot name.

**The animation probes**, each a self-contained headless game ticked at a fixed 1/60 that prints
`<group> <case> <label> <key> <numbers>` and exits:
- **`--anim-probe <case>`** (`anim`): `locomotion`, `crossfade`, `layer`, `two-rigs`, `phase`,
  `events`, `import`, `stride`, `rate`, `root` and `rootmotion`, ticking `ANIMATOR` components
  through the loop's own `update_all_animators`.
- **`--graph-probe <case>`** (`graph`): `order`, `any`, `and`, `trigger`, `elapsed`, `hysteresis`,
  `reenter`, `disabled`, `instances`, `unreachable`, `authoring`, and the five that need a rig:
  `finished`, `fade`, `guard`, `return`, `identity`. Most create no engine at all, since a graph
  binds to a NULL animator and a state may name no source.
- **`--ik-probe <case>`** (`ik`):
  - `reach`, `clamp`, `singular`, `identity`, `pole` and `analytic` build a rig and nothing else: a
    two-bone solve is a pure function of a hip, a target and two lengths.
  - `ground`, `slope` and `step` add a physics world and the ramp and steps, because planting's
    input is a raycast and three bugs live only there: the ray hitting the character's own capsule,
    the ray missing the ramp for the floor beneath, and a right hit point on a wrong plane. They run
    with release disabled: they ask whether the solve reaches the ground it was handed.
  - `swing` ticks a walk cycle with planting on and off and measures each ankle's vertical travel.
    Every other case poses the rig once, and a solve that overwrites a stride looks perfect in one
    pose, so it is the only case that sees feet welded to the floor. It runs planting only.
  - `drop` puts a foot half a metre past full extension, so only the pelvis cap can answer.
  - `lock` runs one walk clip four times: at weight 0 as a clip-only reference, then planting, then
    locking, then locking with the body walked at three times the speed its feet imply, where the
    feature must let go rather than strain. The reference comes from the clip, not a solved pass,
    so it does not move when the solver does.
  - `CETRA_IK_TRACE=1` dumps `lock`'s per-tick heights, contact label and lock state to stderr.
- **`--cam-probe <case>`** (`camera`), **`--gait-probe <case>`** (`gait`) and
  **`--character-probe <case>`** (`character`): the camera rig and the gait with no window, and
  real character controllers at a beam and on open floor. An unknown case is refused by name.

### apps/spores

It keeps four MSAA samples and no TAA: `particle_frag` writes no motion vector, so under TAA a mote
reprojects through the geometry behind it and becomes a dash. `--taa`, `--headless-jitter` and
`--msaa <n>` stay for whoever gives the particle pass one. Its other flags are in
`apps/spores/README.md`.

### apps/shapes

It keeps 4x MSAA with no temporal filter: multisampling is the natural AA for 2D line art, and
nothing in the scene moves, so an accumulator would integrate only its own jitter while switching
on the aux G-buffer, the resolve and the passes that key off `taa_resolving`. The rest of its look
is `engine_set_2d_preset` (`docs/architecture.md`). It takes `-x` and `-f <n>`, and
`--input-script` and `--trace-camera` for the camera gate, but no screenshot flag.

### apps/pcb

pcb is a repository of its own (`../apps/pcb`, with cetra as a submodule). Beside the 2D reasons,
most of it draws through the `shape` program, which writes no motion vector, while its filled
primitives draw through `pbr`, which does, so under TAA a dragged shape would reproject two halves
of one frame differently.

### apps/splash

It takes no part in any of this. It never calls `engine_run`: it clears and swaps the default
framebuffer in its own loop and draws SDF text straight to it, so the engine's framebuffer is built
and never bound, its anti-aliasing is the GLFW window hint, and the screenshot path is never
reached.

### apps/canvas

The 2D canvas's own app (spec 14.0); what it is, and its keys in a window, are in
`apps/canvas/README.md`. It takes no part in the AA question either: the canvas draws after tone
mapping into a single-sample target of its own, every edge antialiased by its exact distance, and
leaves the window's multisampling off. It takes the capture set `-x`, `-f`, `-S`, `-W` and `-H`,
and:
- `--sheet NAME` picks what is drawn: `shapes` (the default), `areas`, `polygons`, `ramp`, `thin`,
  `long`, `long-short`, `glyphs`, `tags`, `overlap`, `symbol`, `board`, `strokes` (caps and joins)
  or `dashes` (spec 14.22). Each is laid out in millimetres and drawn in nanometres, pcb's units.
- `--dash LENGTH,GAP` (mm), `--cap round|butt|square` and `--join round|mitre|bevel` give every
  paint a sheet makes that stroke, its fills' and its text's too, which the canvas leaves as they
  are; what a sheet sets on a paint itself stands. `--sheet symbol --dash 0.5,0.5 --cap butt --join
  mitre` draws the symbol as pcb's ghost would. `--dash-min-px PX` sets `Canvas.dash_min_px`.
- `--view X,Y,WIDTH` centres the view on (X, Y) mm and shows WIDTH mm across it, in place of the
  sheet's own framing. `--offset UNITS` moves the whole sheet, and the view with it, that many units
  from the origin: `--offset 1099511627776` (2^40) is what `canvas-translation` draws at.
- `--mirror` draws the view mirrored, as a board seen from below. `--tile-shift N` makes the canvas
  with tiles of 2^N units, 22 by default; 16 cuts nearly every shape at a tile edge.
- `--split` draws two views side by side; on the board the second is the underside, mirrored,
  showing the plane, the bottom copper and the outline. `--only-view 0|1` draws one of the two
  alone, for the twin.
- `--ui` puts a panel of the game UI over the canvas, at `UI_OVERLAY_ORDER`. `--picture` adds an
  after-tonemap pass that changes nothing, so the overlays draw into PostFX's picture rather than
  the window.
- `--ui-borders` (spec 14.12) draws boxes of one size and radius whose borders lie over clear,
  half-alpha and opaque fills on one backdrop, which `canvas-ui-borders` holds to Porter-Duff over.
- `--edit-thread` fills the board's edit on a thread of its own.
- `--canvas-probe` prints where each shape of a sheet landed and its measures, in framebuffer pixels
  with rows from the top; on the board it also runs a script, a step a frame, printing what each
  frame uploaded. `--budget` prints each frame's canvas draws in milliseconds, timed between two
  `glFinish` calls. Both are what the `canvas` gate group reads.
- `--idle` turns on the engine's idle mode (spec 14.1): a frame is drawn only when something asks
  for one. Under it:
  - `--idle-wake S,...` asks for timed wakes, each on its frame in turn; times joined by `+` are
    asked on the same frame (`0.6+0.2`, spec 14.9). On the first frame with none left to ask for,
    the late work below starts. Each frame prints its wall clock (`canvas idle at N T`), which
    `idle-wakes-kept` reads.
  - `--edit-thread` fills the board on a thread started as late work, after a pause standing for
    a parse, and applies it on the frame the thread's end wakes. `--work-delay MS` sets the pause,
    50 by default (spec 14.7); headless, the frame after the one that started the thread waits for
    it, whatever the pause, and the run prints `canvas idle work begun N` and
    `canvas idle work landed M`, which `idle-work-waits` reads.
  - `--idle-load PATH` loads a texture, named from the working directory, as late work.
  - `--ui-enter` puts the UI panel up (as `--ui` does), fades it in, and puts focus on a button
    in it.
  - `--gui-click` opens the debug GUI and hands it a click on the first frame.

  Headless, the run ends at the first frame after which nothing asks for another. It prints each
  frame's `delta_time`, the frames drawn and the seconds waited, which the `idle` gate group reads.
- `--input-script PATH` replays a scripted pointer and typing (the grammar is under the render
  app's `--input-script`) in place of the mouse. Its `close` asks to close the window as the close
  button would, once, as the first frame of its line's range ends.
- `--refuse-close` (spec 14.7) refuses the first close the window is asked for (its close button,
  Alt+F4, the macOS menu's Quit and Cmd+Q, or a script's `close`), says so on stdout, and takes the
  next. Under `--canvas-probe` the run prints `canvas close asked N refused M frames K`, which
  `engine-close-hook` reads.
- `--palette` (spec 14.2) gives the app a modal command palette over its own commands with their
  keys. Cmd+K opens it (Ctrl+K off macOS), and typing filters and orders the commands by
  `fuzzy_match`, with the matched letters lit. Up and Down move the highlight, Enter or a click
  runs it, and Esc closes. `/` opens the jump over 5,000 generated part and net names, and a name
  chosen there is printed, since there is nothing to jump to. Both lists are virtual rows over
  every match, twelve shown at a time. The key that opens one is not typed into its field: a field
  the app focuses takes none of that pass's keys. Under `--canvas-probe` the run prints the palette
  as it left it (`canvas palette state`: open, the text's length, the matches, the first one's item
  and its lit bytes), the rows built (`canvas palette list`), and the field's box.
- `--history` (spec 14.2) adds a 360-point column at the window's right, and the views share what
  is left:
  - 200 messages as virtual rows of read-only fields, which follow the end, each speaker in a
    colour and every fortieth message a part's name in a second face, a pixel font;
  - under them a multi-line input line that submits on Enter, Shift+Enter starting a new line;
  - Enter in a view puts the caret in the input line, and Esc returns to the view;
  - a sent message is answered by a reply that streams in a word a frame headless, and every 50 ms
    in a window;
  - the wheel over the column scrolls it, and elsewhere zooms the view.

  Under `--canvas-probe` it prints the column's box, the pane's box, the column's colour and the
  input line's top (`canvas history column`), which `ui-history-clip` reads.
- `--ui-text TEXT` sets the text of the `--ui` panel's label (implies `--ui`). The glyph arms draw
  é, Ω and a codepoint the atlas lacks through it.
- `--ui-keys` (spec 14.10) makes the `--ui` panel focusable and gives it a key hook (implies
  `--ui`), as a pane an app draws would have: a click focuses it, and its label says the last two
  things it was handed, a key by its code and modifiers and a character as itself. It takes every
  character and no key, so Tab moves the focus on and Esc lets go. A text field under it is there to
  move the pointer across: the panel keeps the focus until a press moves it. The app's own keys
  (= and - to zoom) stand down while it holds the focus.
- `--font-sheet` (spec 14.4) draws one line of code in JetBrains Mono at 11, 13 and 16 points, each
  first from the code editor's bitmaps made at the size shown and then as the UI's distance field
  of the same face. The app's fonts are built into it (spec 14.6); under `--canvas-probe`, run from
  the source root, it holds each to the file it was built from (the bytes, the font and the editor
  face made from them) and prints `canvas fonts`, which `canvas-font-memory` reads.
- `--editor FILE` (spec 14.4) opens FILE in the code editor over the whole window, focused, and
  stands in for pcb round it:
  - an S-expression highlighter on pcb's lexical rules (comments, strings, `:keywords`, numbers,
    brackets, a form's head);
  - a parse on a thread of its own after each change, whose errors (a bracket left open or closing
    nothing, a string its line ends inside) come back at the version it read and are underlined
    where the typing since moved them;
  - F6 lands `(note landed)` at the text's start, as an edit from another view, and marks it;
  - F7 reads the file again, through the text's whole new text.

  A parse's result is taken on the frame its thread's end wakes, as the board thread's is.
  `--editor-lag N` takes it N frames after it began instead, joining its thread if it has not
  finished, so a headless run without idle mode repeats too (spec 14.7). Under `--canvas-probe` the
  run prints the text as hex (`canvas editor hex`), the selections, the errors as they stand and
  where the first is drawn, whether every row is wrapped (`canvas editor settled`), how many changes
  the texts refused (`canvas editor refused`) and the clipboard as hex (`canvas editor clip`), which
  the `ui-editor-*`, `ui-vim-*` and `idle-editor` arms read, of the focused pane.

  `--read-only` (spec 14.7) makes every file's text read-only: nothing typed in the editor changes
  it, in the standard mode or the vim mode, and each refusal is counted and, with `--vim`, said on
  its line ("Cannot make changes, the text is read only") until the next key.

  `--editor-fold FIRST,LAST` (spec 14.16) folds the first pane's lines FIRST to LAST, numbered as
  the gutter numbers them, closed, through `ui_editor_set_folds`: one row in the theme's fold
  colour saying how many lines it holds, opened by a click on it. `--editor-filler LINE,ROWS` puts
  ROWS filler rows above line LINE of the first pane, through `ui_editor_set_fillers`.

  `--editor` may be given up to eight times (spec 14.6), each file a text of its own with its own
  parse, and the editors are panes, the first opening on the first file:
  - F8 splits the focused pane: a row takes its place, it moves into the row, and a second editor
    on the same text, with carets of its own, goes beside it and takes the focus;
  - F9 closes the focused pane, the focus going to the pane left in its place; a row left holding
    one pane hands it to the row's own parent. The last pane stays;
  - F10 shows the next file in the focused pane, its carets and its view where that pane last was
    in that file, brought through the edits made there since.

  `--split-share F` (spec 14.9) gives F8's first side `UI_SHARE` F across its row and the new pane
  the rest, so a split holds its share as the window is resized; without it both sides share the
  row evenly.

  F6 and F7 act on the focused pane's file. Under `--canvas-probe` the run also prints each pane in
  the window's order (`canvas pane K file F top OFFSET focused 0|1 sel ...`), each file's text
  as hex (`canvas file F`), the panes' tree with each pane its number and each row its children in
  parentheses (`canvas panes tree ((0,1))` after one split, `(0)` for one pane), and the pane the
  vim line is answering (`canvas panes asking K`, -1 for none), which `canvas-panes` reads.
- `--vim` (spec 14.5), with `--editor`, puts the editor in the vim mode, the app's own keys a
  layer over it. A line under the editor shows vim's state (`-- INSERT --`, `-- VISUAL --`...)
  and the keys of a half-typed command; `/`, `?` and `:` from normal mode type into it, a search
  lit as it is typed, Enter handing the line to the vim mode and Esc letting it go, and "Pattern
  not found" or "Not an editor command" said there until the next key. Under `--canvas-probe` the
  run also prints `canvas editor vim` with the state and that message, which `ui-vim-script`
  reads.
  - `--vim-lookup` (spec 14.9) answers `gd`, `gD`, Ctrl+] and `K` as a host would: it says on the
    line what it was asked and the name (the atom at the caret on pcb's rules, or a one-line
    visual area's text), as `gd memory-bank`, and answers that it went. Without it no host is set,
    and `gd` and `gD` are vim's own search.
  - `--vim-layout` (spec 14.9) answers `=`, `gq` and a new line's indent: two spaces for every
    bracket still open where a line starts, one fewer for a line opening with `)`; and `gq`
    joining the lines' words on one line at the first line's indent.
