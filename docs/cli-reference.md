# CLI reference

Every flag the apps take beyond the basic headless capture set, and what each one is FOR.
`AGENTS.md` keeps the capture flags (`-x`, `-f`, `-S`, `--screenshot-every`, `--headless-jitter`)
because those are the everyday workflow; everything else is here.

Most entries carry more than a description. A flag in this engine usually exists because
something was measured, or because a default reads as a bug when you meet it cold, and that
reason is recorded beside the flag rather than in whichever spec introduced it.

## Contents

- [PostFX and environment flags](#postfx-and-environment-flags) — the render app: TAA, water, sky,
  clouds, contact shadows, decals, IES, LUTs, layers, the probes
- [apps/tree](#appstree) — its own flag set, and two defaults that read as bugs
- [apps/forest](#appsforest) — instancing, LOD, culling, the island, erosion, streaming, origin
  shifting
- [The other apps](#the-other-apps-and-the-aa-mode-each-one-chose) — gametest, spores,
  shapes, pcb, and why splash is not in any of this

---

## PostFX and environment flags

**PostFX / environment CLI flags** (render app): `--taa`, `--render-scale <f>`
(TAAU; headless needs `--taa --headless-jitter`),
`--render-scale-at <frame:scale[,...]>` (diagnostic: switch scale mid-run,
exercising the runtime rebuild; same headless preconditions),
`--shadows-off-at <frame>` (spec 11.56 — diagnostic: clear `shadow_system->enabled` mid-run, which
is the ONE state `--no-shadows` cannot produce. That flag clears the switch before frame 0, so no
punctual layer is ever assigned and every index the depth pass maintains is still at its initial
value; turning it off AFTER the pass has run is what leaves `Light.shadow_layer` pointing at layers
nothing draws, and it is the state the GUI checkbox produces. **A defect reachable only through that
transition is invisible to every headless arm until this flag exists** — 11.56 shipped one and the
full suite was green), `--no-ssao`, `--ssgi`,
`--no-ssr`, `--fog`, `--sss` (`--sss-radius`, `--sss-color`),
`--contact-shadows` (`--cs-distance <f>`, `--cs-strength <f>`, `--cs-debug`; all three imply
enable, and `--cs-distance 0` is the exact off path. OFF by default. Since spec 11.56 it marches
the key directional AND every map-less local light, so **the run gate is "a directional OR a
point/spot with no punctual layer"** -- before that a room lit only by practicals skipped the pass
entirely, which is where the feature was needed most. `--cs-debug` shows the raw visibility term
with **no display encode on it**: read those bytes linearly, since decoding sRGB reports a measured
0.6326 as 0.3564. Costs 2.43 ms per FULLY-COVERING light at 3200x2000 internal (0.38 ms per
megapixel per light), but cost tracks COVERAGE rather than count -- sixteen lights spread across a
scene cost +0.77 ms against the +38 ms sixteen coincident ones would, because a pixel marches only
the lights whose cluster entry reaches it. There is deliberately no per-pixel light cap),
`--motion-blur`, `--dof`,
`-E/--exposure` / `--no-auto-exposure`, `--no-bloom`, `--tonemap <neutral|aces|agx|linear>`
(`linear` is the identity curve WITH the display encode, which passthrough is not: an authored
colour at unit exposure reaches the screen as authored, and it is what `engine_set_2d_preset`
selects for a flat-colour scene; **not** `passthrough`: `render_args.h` notes the "unset" sentinel deliberately coincides
with `POSTFX_TONEMAP_PASSTHROUGH = 0`, so it is unreachable from the CLI by design — any
gate needing a linear read has to be written as a ratio instead, spec 11.32),
`--water` (`--water-level <f>`, `--water-extent <f>`, `--water-waves <gerstner|fft>`,
`--water-bed <none|dome>`, `--water-probe`, `--water-fft-probe`; `--no-water`, `--no-water-caustics`,
`--no-water-glitter`, `--no-water-foam-history`,
`--no-water-coverage`, `--no-water-lod`; specs 11.32 to 11.35 and 11.42 — suppresses the shadow catcher,
see the pass order above. Gerstner is the default and allocates nothing; `fft` is an OCEAN and adds 45
passes plus 24 textures. **Crest foam and caustics are FFT-only**, and that is by
construction rather than an omission: both are selected from Jacobian compression, and the
Gerstner path's steepness is clamped so its mapping cannot compress. The SHORE foam band is
not — it is selected from the shoal factor and runs on both models, which is what makes
Gerstner the way to isolate it.
**A scene file can author the whole surface** (`water{}`, 20 keys plus 8 in each of two nested
wave trains — the 16th flat key is `farLod`, added by 11.35 for far-field filtering; specs 11.33
phase 5, 11.35, 11.42, 11.48), and the flags override it rather than the reverse — so `--no-water`
exists and is the only way to switch off a surface a `.cscn` asked for.
`assets/scenes/water_fixture.cscn` authors all 36 and is the block's worked example (35 until 11.84 split the in-scatter in two). **No flag can set any
of the SEA STATE**, which makes a scene file the only way in and `water-seastate` the only arm on
that path; editing any of it re-seeds the initial spectrum on the next frame, which is a CPU pass
over three 128² grids.

**The sea state is two WAVE TRAINS since spec 11.48**, `windSea{}` and `swell{}`, each a complete
set of the same eight keys (`windSpeed`, `fetch`, `direction`, `scale`, `peakEnhancement`, `focus`,
`spreadGain`, `spreadBlend`) beside a shared flat `seaDepth`. 11.42's four flat keys `windSpeed` /
`fetch` / `peakEnhancement` / `swell` are **gone**, and `parse_water`'s closed `known[]` now warns
per key on a scene still using them. The reason is that the swell was never authorable at all: it
was seeded from a hardcoded 8.4 m/s over 310 km, so a scene lowering its wind kept a gale's swell —
in `apps/tree` that was **300% of the wind sea**, and since `Breaking` tests the depth-limited
criterion `disp.y / (0.39 · depth)`, a shallow shelf under it broke everywhere at once and painted
a white blob over the whole bay. `Breaking` was reporting the truth about an absurd sea, which is
why 11.47's six commits on the foam path never touched it. Note the old flat `swell` was ONE knob
doing TWO jobs — the wind sea's directional sharpener AND the swell's amplitude — so it migrates to
two: `windSea.focus` and `swell.scale`. `swell.scale` 0 removes the train.
`--water-fft-probe`'s cascade 2 carries no swell (`secondary_scale` 0) and read bit-identical
across the change, which is what pins the wind sea as untouched.

**`windDirection` now reaches BOTH wave models** (11.42). It was Gerstner-only, and the spectral
seeding used a private −0.48 rad constant — so a scene authoring a wind direction got a spectral sea
travelling somewhere else entirely. Unifying them means the DEFAULT spectral sea rotated by about
58°, since `create_water`'s documented lake default (0.86, 0.51) is +0.535 rad. No golden moved with
it: both water goldens are Gerstner.

**The in-scatter is a FRACTION of the light falling on the water, plus an optional floor**
(spec 11.84). `scatterAlbedo` is dimensionless and `scatterGlow` is absolute radiance added
regardless; the old flat `scatter` was a radiance on its own, so the sea was as bright at midnight
as at noon and **`water.scatter` is now refused BY NAME** — its units changed rather than its
meaning, so an old value still parses and means something several times too large (six, on
water_fixture). Convert by dividing by the scene's own daylight `incident`, which
`--water-probe` reports. The two fields exist separately because **one value cannot do both jobs**:
an authored constant reproduces a stylised night sea and cannot produce a dark realistic one, and
a pure albedo does the reverse. **Nothing in tree ships with a non-zero glow**: `apps/tree` was the
motivating case and tried it, then reverted to a pure albedo lifted by real MOONLIGHT
(`moon_brightness` 2, `moon_size` 6), which keeps the sea tracking its light. So the field is an
authoring escape hatch with no in-tree consumer but the `water-glow` arm — worth knowing before
citing it as a worked example.
**The key light is the BRIGHTEST directional**, ranked by `intensity x peak channel`
(`water_key_light`), not the sky's sun by name: a sun below the horizon still exists with its
intensity faded to zero, so water used to hold a direction pointing underground and could never
reach the moon. A weight of zero selects nothing, which is what switches the caustics off in true
darkness. **Do not read brightness on `water_fixture` and call it lighting** — its geometry is
emissive over a black base, which is what makes it a good absorption instrument and what makes its
water band read 0.2207 at midnight against 0.2478 at noon whatever the sea does. Every arm in the
`water-night` group is a twin DELTA for that reason.

**`absorption` is extinction per WORLD UNIT, and the library default is clear water per METRE**
(spec 11.36) — the two agree only where a unit IS a metre, which is true of the fixture and false
of `apps/tree` at 22 units to the metre, where it made the sea about six times too absorbing and
nothing under the surface visible. An app whose unit is not a metre divides the default by its own
scale; the in-scatter carries no length and does NOT scale. The path clamp that hid it is now a budget in
extinction lengths (`WATER_MAX_OPTICAL_DEPTH`) rather than a length in units, with the old
`WATER_MAX_PATH` kept as a floor so the change can only lengthen a clamp. Truncation at the clamp
leaves at most 2.15%, which is the bound to quote — grazing sight lines to a deep bed DO get
clamped. Note the shader still has no idea how big a world unit is, so `WATER_MAX_BEND`,
`WATER_SHORT_NEAR/FAR`, the caustic depth window and `OCEAN_SHOAL_*` remain mis-scaled by the same
factor in tree; `Sky.world_units_per_km` is the number the engine already has and tree never sets.
**Below the surface is finished too** since 11.33 phase 2: submerging the camera arms the
froxel volume for that frame and the body becomes a second medium, so submerged geometry is
absorbed. Two approximations, both in `froxel_inject_frag.glsl` — a cell holds one scalar
extinction, so water's per-channel value arrives as its luminance mean with the colour moved
into the in-scatter, and the in-scatter is constant rather than scattered sunlight, so there
are no shafts in the body.
**The grid is a PROJECTED grid** since 11.35 — a fixed lattice in NDC, one draw, each vertex a
ray onto the still plane — so density is uniform in PIXELS and the surface reaches the horizon.
The clipmap it replaced (11.33 phase 1) welded those two together: rings tile only because
every level snaps to the coarsest cell, so pushing the extent toward the horizon coarsened the
finest cell until the swell disappeared, and the surface stopped 5° short while a comment
claimed otherwise. **`--water-extent` is now the SHOALING BED's domain and nothing else** — it
does not bound the drawn surface, and outside it the bed field reads its edge, which is open
water. Both shipped D3 *without* spending the tessellation stage the roadmap scheduled it to
open.
**The far field is a filtering problem, not a mesh-reach one** (11.35 phase 2): distant cells
cover more than a wave period, so each model drops what sits under its cell footprint — mip
levels on the spectral path, whole octaves on the Gerstner one — and hands the removed slope
energy to roughness. **Since 11.42 that energy is an absolute mean square slope rather than a
fraction, and converts to a lobe width by the Beckmann relation** — so the horizon is as rough as
the waves it stopped resolving instead of lerping toward an inherited 0.115 literal, which was low
by about a factor of three against this spectrum's own slope variance and made the horizon the same
roughness for a millpond and a gale. It composes with the authored roughness by adding variances, so
it collapses to exactly the authored value where nothing is filtered.
`--no-water-lod` is the bisect lever and reaches the unfiltered surface
exactly; without it the horizon aliases into a speckle band. What survives in the mid field is
the longest octave alone, which at grazing incidence reads as regular swell lines: that is the
corduroy a single-direction Gerstner train gives, and the FFT path's directional spectrum does
not have it.
`--water-bed dome` installs an analytic bed so shoaling has a deterministic fixture at all;
`--water-probe` prints the CPU wave query, which is otherwise invisible from outside the
process. `--no-water-coverage` is the bisect lever for the shoreline's derivative coverage
and reaches the pre-11.33 frame),
`--film`, `--sky` (`--sun-elevation`, `--sun-azimuth`, `--sky-debug`), `-e/--env <hdr>`,
`--world-scale <units-per-km>` (atmosphere scale; 1000 = 1 unit is 1 metre), `--no-aerial`,
`--no-cloud-shadows` (spec 11.39 — the deck casts into the froxel fog by default whenever
`--clouds` is on, because a cloud that casts nothing is wrong rather than un-featured.
**`--sky-debug` now shows the shadow map itself** as a tile below the two cloud-noise fields
(spec 11.40), and that tile is the only way to tell a bad map from a bad lookup: the gate arms
watch the shadow's EFFECT on the fog, and a map with no range at all still darkens the frame and
still varies downstream. That is not hypothetical — 11.39 shipped a map saturated to zero at every
texel and both arms passed. **Spec 11.41 shadows the GROUND too** — `pbr_frag`, the shadow catcher
and water's caustics — and it needed no sampler unit freed: the map rides `sceneColorTex` by
`#define` in the opaque pass, where unit 6 is idle in every scene. The `pbr_frag` half was booked as
hard-blocked on D0 for a spec cycle and never was.
**11.41 also rewrote the march, and every cloud figure taken before it is stale.** The old
`CLOUD_SHADOW_SPAN_CAP_KM` capped the SLANT path at 1.2 km while crossing the deck takes
`2.5/sin(el)`, so the fraction of cloud traversed fell with the sun — 48% at the zenith, 4% at 5° —
and below ~10° the march never left the cloud base and the map came back uniformly **1.0**.
`apps/tree`'s 0.8° sun had never had a cloud shadow from either half. The march now steps equal
ALTITUDE increments across the whole deck and clamps only the HORIZONTAL excursion, at the shape
field's own **8 km tile period** (the reason to clamp is that wandering further re-reads the same
field and decorrelates a texel from its column; 1.2 km was tried first and binds below 64°, which
reported the map lighter than the truth everywhere).
**Coverage is a GAP fraction as far as the ground is concerned.** Extinction 25/km over a 2.5 km deck
makes tau 62.5 × density, so half transmittance needs density 0.011 — any real cloud in a column
blocks the sun outright and the dapple is a map of the holes. Measured fraction of the map above half
transmittance at a 35° sun: **47.9% at coverage 0.10 against 0.0% at the 0.45 default**, where the
deck instead casts a flat 32% dimming with no pattern in it. The default stays 0.45; the gate arms
pin `--cloud-coverage 0.10` explicitly, because an arm wants the configuration where the property is
legible and must not be silently re-tuned by a change of default. Costs nothing where nothing reads
it — with no `--fog` and no lit ground the map is still built and the frame is 0 px either way),
`--no-fog-volumes` (spec 11.39 — drops a scene file's `fogVolumes[]`. Local fog volumes are
world-space AABBs with density, an inward feather and a tint, authored ONLY in `.cscn` since a box
needs a place and a size that no flag can carry; this is the only way off, mirroring `--no-water`.
A volume arms the froxel pass by itself and must never set `fog_enabled`. **Note the narrowing that
came with it:** `fog_enabled` now means "the global height medium exists" and the pass gate means
"some medium does", so the global density uploads as zero when the app did not ask for fog — before
there was one medium and the two were the same statement),
`--no-decals` / `--decal-probe <n>` (spec 11.73 — drop a scene file's `decals[]`, and print the
decal diagnostic every n frames. Authored ONLY in `.cscn`, the fog-volume rule and then some: a
decal needs a place, a size, a FACING and an image. Cap `DECAL_MAX` 16, and that number is what
sizes the froxel mask — 32 would pin the descriptor at seven vec4 rows forever and 64 would not fit
GL 4.1's guaranteed 16 KB block at all, so it is a structural ceiling rather than a preference.
**The probe is the instrument, not the frame**: a mark projected through a box half a metre out
still lands on a wall and still looks like a poster, so what is checkable from outside the process
is the froxel mask and the array layers — and it names why a decal is not live, since an authored
decal whose image never loaded is invisible in exactly the way one that was never authored is.
Costs nothing where nothing uses it: every golden is 0 px and a decal-free scene compares 0 px
ACROSS BUILDS against master),
`--emissive-lights` (spec 11.49 — an emissive mesh becomes a real LTC area panel: plane-fit a
rectangle, take the material's radiance in nits, register it in the cluster list. **OFF by default,
and that is a measurement rather than caution** — 30 of 32 emissive materials in this corpus are the
unlit-flat-colour trick, a glow over a BLACK base used to make something a flat bright colour, so
on-by-default turns every one of them into a lamp. A material declines individually with
`emissiveLight: "off"` in a `.cscn`. Two shape tests, and the second was missing for a spec cycle:
planarity is FLATNESS only, so an L, a ring, a quad with a hole and two strips five metres apart all
read exactly 1.0 and got one rectangle spanning the lot — `fill` is what rejects those. A derived
panel inherits `cast_shadows` FALSE like any other light, so it lights through walls until a
`light_overrides` entry says otherwise, which is the one thing to know before using it in an
interior. Costs 0.017–0.021 ms CPU to reconcile and +2–3 ms of shading per panel — the price is the
LIGHT, not the machinery),
`--emissive-light-probe` (prints the panel every emissive mesh would derive, plus every skip and its
reason, in the `--water-fft-probe` idiom. The instrument exists because a wrong fit hides inside a
plausible image: a panel half a metre off or sqrt(2) too wide still lights the room),
`--ies-profile <file>` (spec 11.57 -- apply an IES file to every point and spot light, overriding
whatever the scene authored. Added after the fact, because a `.cscn` was the ONLY way to attach a
profile and the four fixture files could not be looked at without hand-editing a committed asset --
which is also the real workflow, since the thing you want to do with this feature is point it at a
manufacturer's `.ies` on a scene you already have. Directionals and area panels are skipped for the
same reason they refuse an authored one. Applied AFTER `apply_cscene_light_overrides`, so the flag
wins, and byte-identical to authoring the same file: `ies-flag` asserts **0 px** against the
authored twin, which is the reading that says the two paths are one path),
`--ies-probe` (spec 11.57 -- one row per loaded IES profile and one per sampled angle, carrying
`rel` and absolute `cd`. The instrument exists because a profile's correctness is a NUMERIC claim
no frame can make: a table resampled off the wrong plane, folded with a modulo where LM-63 mirrors,
or scaled by the wrong multiplier still lights a room plausibly. Sampled AT the taps, not between
them -- a value between two tests the lerp, a value at one tests the table, and it is the table a
resample gets wrong. **Plus `mirror` rows, which are the exception to that rule and exist because
of it**: every tap lies inside `[0, span]`, where a mirror and a modulo agree exactly, so
tap-sampling alone left the C fold unreachable from outside the process. Those rows ask for
`span + d` and pair it against `span - d`. Note the probe still says nothing about the SHADER's
fold -- for that see `ies-mirror`, which has to look at a frame),
`--occlusion-probe` (spec 11.98 — the masked occlusion buffer against its brute-force twin, as
`occlusion-probe` k=v rows after the loop with no GL and no frame. Two-sided: hierarchical-hidden
must imply reference-hidden with zero exceptions over the scene's items AND a seeded frustum sweep,
and the catch rate must clear a floor so the safety half cannot pass by hiding nothing. The twin
replays the RECORDED box set the frame accepted — not a re-gather, which could drift — through the
same raster path at 4x resolution, so what it verifies is the hierarchy: the tile fold, the footprint
rounding, the mask. Also validates every material-flagged proxy's interior contract, box against its
own triangles, with both violation kinds judged against the 3x3 neighbourhood because the
conservative fill leaves a diagonal seam a box face does not have. Read by `occl-probe`),
`--wind-bound-probe` (spec 11.54 — per wind-responsive mesh, the largest displacement `windOffset`
can be driven to beside the bound `wind_max_offset` claims, as `max_abs` / `max_l2` / `bound` and
their ratios. **It drives the REAL shader through transform feedback**, not a CPU port: the two
halves share their coefficients through `wind_bounds.glsl` and cannot share their arithmetic, so a
term added to the shader makes the bound non-conservative and culling then drops geometry that is on
screen. A port was built first and thrown away on the numbers — it catches a term added to the model
(0.966 → 1.258) and reads straight through one added to the GLSL (0.966, unmoved), which is the
failure the probe is for. Read by the `cull-bound` arm, which asserts BOTH `measured <= bound` and a
floor on the tightest sweep, since the worst case is genuinely reachable and a low reading means the
grid stopped sweeping rather than that the bound got safer),
`--exposure-probe` (spec 11.52 — one line per METERED frame: raw and adapted luminance, the gain,
the camera multiplier, their product and EV100. Silent on a pinned frame, because there is no
measurement on one. Nothing else can observe the metered value: no log line, and the GUI readout
came with the same spec),
`--meter-mode <uniform|centre|spot>` / `--meter-radius <f>` / `--meter-low <f>` / `--meter-high <f>` /
`--adapt-up <f>` / `--adapt-down <f>` (spec 11.52 — the metering mask and the percentile tails, also
authorable as a `post.metering` block in a `.cscn`, CLI winning. **Low defaults to 0.70 and that is
not conservatism**: a meter including the black background is measuring the background, and zero
times a thousand is still zero, so a mild low percentile leaves the metered value barely moving when
the scene brightens. Measured at x1000 the metered value moves 3.85 stops at low 0.10 against 9.96
at 0.70, where 9.97 is correct. Same reason UE's Low Percent defaults to 80. The adaptation rates are
per FRAME, not per second, because a headless run of N frames must adapt identically every time),
`--lut <f.cube>` / `--no-lut` / `--lut-strength <f>` / `--lut-interp <trilinear|tetrahedral>`
(spec 11.58 — a 3D colour-grading LUT, the general map lift/gamma/gain cannot be. **DISPLAY-REFERRED:
the table is applied AFTER `displayEncode`**, which is the space a colourist's `.cube` is authored in;
applying one to the LDR-linear values `toneSelect` returns gives a plausible frame that is not the
look the artist made. Also authorable as a `post.lut` block, CLI winning, and `--no-lut` is the only
way off one a `.cscn` asked for. **Size comes from the file** (2–64; 33³ is Resolve's default and the
roadmap's "32³" is not a size the format produces). **Tetrahedral is the default, and the 0.076-of-a-code
figure 11.58 first published for it is WRONG by 28x** — that was measured on a table whose every
output channel is a ridge function of two inputs, so trilinear degenerates to bilinear on it and the
three-way cross term tetrahedral drops is identically zero. On a table with a real three-way term
(any saturation rolloff) it reads **2.7 codes**. It also ships for two exact properties: greys stay
exactly grey where trilinear tints them 5 codes, and an identity table is a bit-exact 0 px where
trilinear is 12,088 px at PAE 1/255 (the texture unit's fixed-point filter weights). fp16 storage
costs 0.004 of a code — the fp32 rejection was ALSO first measured on an identity table whose lattice
values are exactly representable in fp16, so it could not have shown storage error at all. Trilinear stays reachable because
tetrahedral alone cannot be falsified. **A log/show LUT loads cleanly and renders washed out** and
nothing in the format lets the loader tell — a non-0..1 `DOMAIN` is refused by name, but log tables
often use 0..1 too, so the mid-grey heuristic is a warning rather than a test. Costs nothing when
unused: the loaded texture IS the enable, so all 24 goldens are 0 px),
`--no-layers-vt` / `--layers-vt-res <n>` / `--layer-blend-at <frame:value>` (spec 11.66 — the
composite cache's bisect lever, its diagnostic resolution override, and the one headless way to make
its by-value bake key go stale; all three exist in `render`, the first two in `forest` too),
`--no-layers-vt-pages` / `--no-layers-vt-feedback` / `--layers-vt-page-slots <n>` /
`--layers-vt-page-budget <n>` / `--layers-vt-probe <n>` / `--cam-at <frame:ex,ey,ez,tx,ty,tz>`
(spec 11.67 — the paged near field's bisect levers, the churn and bake-rate knobs, the residency
probe, and the camera teleport; all but `--cam-at` in `forest` too),
`--road-width-at <frame:value>` (spec 11.68 — set every road's width mid-run. The `--layer-blend-at`
idiom, and it exercises BOTH halves of the road path in one stroke: the segment block re-uploads
and the composite cache's by-value key goes stale. A road is authored only in a `.cscn`, so this
is the one headless way to change one after the first bake),
`--pointer-script <path>` / `--trace-camera` (spec 12.19 — replay a MOUSE from a text file, and
print the pose the frame draws from. The pointer script is `--pad-script`'s grammar through the
same parser: one line per frame or `from-to` range, `#` comments, the last matching line winning,
with tokens `at=x,y` (framebuffer pixels, +Y up), `by=dx,dy`, `down`, `shift` and `wheel=<f>`. A
range states the pointer's STATE, so a press is `down` appearing and a release is it going. It
drives the engine's own pointer path, so a replayed drag reaches the drag state, the ray picking
and the app's forwarded callback exactly as a hand does — which is what let the `camera` group
assert a viewer camera that nothing could press before. `apps/tree` and `apps/shapes` take both
too, and shapes gained `-x` and `-f` with them: it is `CanvasController`'s only caller, so the 2D
camera had no automated coverage at all until it could run headless),
`--config-dump <path>` / `--config <path>` (spec 11.71 — dump the whole tuned session to JSON on
exit, and restore one. **See "Reproducing a session" below for when to reach for it**; the one thing
to know here is that the apply lands AFTER the scene-radius derivation at `render.c:3349`, which is
load-bearing: `fog_far`, `fog_floor_y`, `fog_density` and `fog_height_falloff` are unconditional
ASSIGNMENTS there and `ssao_radius` / `ssr_max_distance` / `ssr_thickness_min` are `fmaxf` floors, so
an apply above that block loses the first four outright and silently raises the rest. Also note a
restored sun does not re-capture a probe SET — 11.70 defers that, and a set cannot re-capture because
its cubes are released into the atlas, so on `cornell_rooms` a moved sun lands 0.144 RMSE from the
flag answer against the original's 0.159; on a probe-free scene it is exactly 0 px),
`--no-texture-compression` / `--texture-compress-colour` / `--texture-probe` (spec 11.85 — the
bisect lever, the opt-in for the lossy half, and the ledger. The probe is the instrument because
texture memory is invisible from outside the process and a saving is a NUMERIC claim no frame
can make: a scene whose normals silently failed to compress renders exactly like one whose
normals compressed. It prints the internal format the driver HOLDS, not the one asked for,
which is what catches a format being declined. Its lines are the house `<prefix> <kind> k=v`
shape since 11.86, read through `_probe_render` like every other probe; `apps/tree` has
`--texture-probe` too, and is the only place a purely procedural texture set can be priced;
since 11.101 each row also carries `distribute_from=`, the first mip level whose alpha is a
binary dither -- -1 on every texture whose chain never enters one before its 1x1 tail, which on
this corpus is everything but a regular lattice), `--no-alpha-jitter` (spec 11.101 — the
diagnostic lever for the jittered alpha lookup, which is live only while TAA accumulates at one
sample and is gated off under alpha-to-coverage. With it the dot lattice's deep mips churn 358
pixels a frame on `assets/alpha_ladder_fixture`; without it 2,917, and the Moire returns. It
recovers none of the coverage the one-sample path loses -- MSAA 4 is the path that keeps it, and
spec 11.102 priced that path at +296% of frame on a dense cutout scene and found it recovers
nothing the eye can see, so this is the shipped answer and not a stopgap for one --
and it rides the config snapshot as `engine.alpha_jitter`, because the GUI has the same switch
and `config-coverage` refuses a control with no row),
`--clearcoat-debug` (spec 11.86 — the coat normal as bytes, `Nc * 0.5 + 0.5` where there is a
coat and EXACT black where there is not. A named spelling of `--render-mode 13`, and a render
MODE rather than a late flag because the quantity is written in `pbr_frag`: any non-PBR mode
takes the passthrough blit, so these bytes arrive with no tonemap, no display encode and no
dither. **Read them LINEARLY** — the `--cs-debug` trap. Black is unambiguous because a
normalized vector cannot encode there, the darkest reachable being (-1,-1,-1)/sqrt(3) at 0.211,
which is what makes an uncoated surface an in-frame control),
`--no-oit` / `--no-oit-moments`, `--no-instancing`, `--no-frustum-cull` (spec 11.53 — submit every
item, culled or not. A bisect lever in the `--no-instancing` idiom and 0 px by construction, since
culling only ever removes geometry that contributed nothing; it is what the `cull` gate group compares
against),
`--no-occlusion-cull` (spec 11.98 — occlusion rejection off, the same kind of lever held to the same
0 px bar: the cull is conservative by four stated roundings, so this differing from the default is a
defect and the `occlusion` group's identity arms are what say so. Camera-pass-only either way — shadow
layers and captures never see the occlusion answer, so the flag cannot move them),
`--no-lod` / `--lod-bias <f>`,
`--no-sort-opaque` (front-to-back opaque ordering is ON by default, spec 11.30, and it is the larger
of the two overdraw levers by a wide margin — `apps/forest` opaque 306 → 169 ms. Still **not** a 0 px
flag, but only barely and no longer for the reason 11.30 gave: masked materials stopped blending in
11.31, and what is left is coplanar ties, where draw order decides which of two surfaces at exactly
equal depth wins. Raiden moves 31 px, `cornell_box` 1),
`--ortho <height>` (orthographic camera, view volume height in world units; spec 11.103. The
engine has always had this camera mode and until 11.103 nothing that could CAPTURE a frame could
ask for it, which is how the TAA jitter came to be wrong there for the whole life of the feature
— derived for perspective, it shifted the raster by ~150 px a frame under an orthographic camera.
Applied AFTER the auto-framing, which decides the projection shape along with the clip planes.
**A real camera since spec 11.104**, which took the perspective-only maths out of everything an
orthographic frame reaches: the depth inverse, the view-position reconstruction (GTAO, contact
shadows, fog), the five near-plane recoveries, the world-to-screen lengths (GTAO's radius, the SSS
footprint), the view ray (fog integration, spec-occ), the per-fragment view vector (specular,
Fresnel, the parallax march, water, SSR), the light cluster wedge, LOD selection, the occluder
backface test and the cascade fit. Every site branches on `projection[2][3]`, the element
`glm_ortho` zeroes and `glm_perspective` sets to −1, and the perspective side of each branch is
the previous expression. The `ortho` gate group carries the proof; `taa-ortho` still compares an
orthographic leg against an orthographic leg and cannot see any of it.
**Deferred, recorded rather than fixed**: the skybox cube (a ±1 cube covers about 1% of an
orthographic frame) and the sky background, the ocean's projected grid, the cloud march, the
depth-sort key (perf only) and the gizmo's world-per-pixel. An orthographic camera looking at a
sky is a design question, and none of these is a wrong pixel on the content the fix is for. The
grid is deferred for cause and not for want of a branch: one was tried, and under a parallel
view its far-capped rows spike (fp32 at a million units lands them pixels off) while its uniform
lattice moires against the wave field, which the perspective grid's near-dense rows never
showed; and the water plane crosses the near plane under any tilted parallel view, so where the
eye sits along the axis decides how much of the plane is clipped. Water's own shading is
orthographic since 11.104; the placement of its mesh is not),
`--depth-prepass` (depth before shading, OFF by default and measured to LOSE everywhere in this
corpus — spec 11.31. Masked geometry IS prepassed since 11.31, through its own program in a
depth-only mode, and reaches a better depth complexity than the sort while still being slower.
**It reverses sign under alpha-to-coverage** (spec 11.102): at `--msaa 4` on a dense cutout scene
it halves the opaque row, 105.6 → 47.9 ms, because A2C is what stops the alpha test discarding and
leaves the overdraw for a prepass to remove. At one sample on the same scene it moves 17.3 → 15.6
and does not separate from the floor, so the standing verdict holds on the path this engine ships),
`--profiler` (per-pass GPU time, CPU time and submission
counts; HUD tables under "Profiler" and three on stdout at exit, specs 11.27 and 11.28.
**It cannot price a sub-millisecond scope inside a heavy frame, and it reports that as 0.000 ms
rather than as a failure** — timer queries are checked and never waited on (`PROFILER_RING` 4,
deliberately: a blocking read would report the stall it caused as the pass's cost). On a frame with
a 31 ms opaque pass, contact shadows, the GTAO sweep, the AO denoise, spec-occ, SSR and all three
resolves ALL retired unavailable on every slot and printed zero, while the same scopes read
normally on a light frame. So a zero row is not a free pass; price a small pass on a LIGHT scene at
high resolution instead. The GPU column also drifts by a factor of ~3 between sessions on the same
build — a sun-only contact-shadow pass read 0.29 ms and 0.055 ms an hour apart with every other
small scope moving together — so **only samples adjacent in time may be differenced**, which is the
same A/B/A/B rule the forest note below states for a different reason).
Note `--dof-max-coc` is in half-RENDER-res texels: under `--render-scale` the
same value blurs ~1/scale wider on screen (documented, not compensated).
`--oit-moments` implies `--oit` (it is a better weight inside the same
accumulate, not a second transparency path) and costs a large amount of VRAM --
two fp32 moment targets at the scene sample count plus a double-height resolve
atlas, 156 MB at 640x400 on a Retina framebuffer, logged at allocation. Both are
now ON by default, so `--oit` and `--oit-moments` are only useful for restating
a default that a `.cscn` or another flag turned off.

## apps/tree

**apps/tree's own flags**, which went unlisted anywhere for a long time. Capture: `-x/--headless`,
`-f/--frames`,
`-S/--screenshot`, `--screenshot-every`, `-W`/`-H`. Scene: `--seed`, `--no-shadows`, `--no-fog`,
`--no-falling-leaves`. Debug: `--render-mode N` (10 = HDR hotspots, 12 = extrapolation),
`--msaa N` -- **this app ran 4x MSAA *and* TAA until 11.88 and now runs ONE sample in every mode**,
interactive and headless alike, which is how the MSAA-only grass specks came to surface here first
(spec 11.38); `--msaa 4` is how to reach them now. The second reason for one sample is a look
rather than a cost: above one sample masked geometry takes the alpha-to-coverage path, and against
near-black leaves on a sunset that traces a bright fringe round the whole canopy. Sun: `--sun-elevation` (**default 0.8 degrees, not 14**),
`--sun-azimuth` (**193, not 235**). Water: `--no-water` (drops the sea AND the seabed together),
`--water-level`, `--gerstner-waves` (the sea is FFT by default), `--no-water-surf` (no incident
wave at the shore — removes the bore from the GEOMETRY too, since depth-limited breaking is gated
on the surf existing, which is what makes it the bisect lever for anything shore-shaped), and the
spectral sea state `--wind-speed` / `--fetch` / `--swell`. **The first two address the WIND SEA and
`--swell` is the swell train's `scale`** (spec 11.48); this app authors both trains itself — 6 m/s
over 15 km and 3.0 m/s over 40 km, against library defaults of 11.5/120 km and 8.4/310 km — because
the library's are an ocean and this is a 28 m lagoon. Camera: `--cam-eye`,
`--cam-target`, `--cam-up`, `--fov` -- added because a look bug in a scene this size arrives as a
reported viewpoint and the orbit controller's state cannot be written on a command line. Walker:
`--player`, `--walk-speed`, `--look-rate`, `--no-invert-arrows` (pitch is inverted by DEFAULT;
yaw never is). WASD moves, arrows turn the head, shift sprints, space jumps; **the mouse does not
touch the camera at all** and the cursor is never captured, so the ImGui sliders stay clickable.
Night (spec 11.79): stars are **ON by default** here where the library defaults off —
`--no-stars` is the way off, `--star-hour <deg>` turns the field about the celestial pole (what
moves the Milky Way band; tree defaults it to 90, where the arc crosses this framing), and the
fade-in runs +3° to −8°, so the default 0.8° sun shows the first few. `--day-cycle <seconds>`
runs the clock (spec 11.81) — `--day-cycle 180 --time-of-day 16` is the sunset — and while it
runs BOTH sun sliders grey out in both panels, since the tick owns them. The MOON is ON here
too (spec 11.82) — `--no-moon` is the way off, and under a cycle its phase and position both
follow the sun off one clock, so `--day-cycle 1` sweeps most of a lunar month in a 400-frame
capture. **It is EXAGGERATED here** (spec 11.84): `moon_size` 6 against the library's life-size
1, because a physical moon is a dozen pixels, and `moon_brightness` 2, because this app's sea is
what the moon has to sell and at 1 the water away from the glitter track sits in the bottom few
codes. `--moon-size` scales the disc and its halo alone; `--moon-brightness` drives the disc AND
the light, so it is the one that lifts the sea -- and it keeps the sea PHYSICAL where a scatter
glow would not. `-c/--config <path>`
restores a config snapshot — the shared GUI panel's Dump Config button has written
`cetra_config.json` since 11.71, and this app can finally take one back.

**Two things about tree's defaults that read as bugs if you meet them cold.** At a 0.8 degree sun
the frame is lit mostly by sky and is DARK, and the shadow map cannot hold the span it needs --
roughly 37,000 units against a 6,000 budget -- so **shadows run off the map rather than reaching
across the island**. `--sun-elevation 8` is the framing that keeps both. And its island is now
`GROUND_RADIUS` 620 / `GROUND_HEIGHT` 190 (was 900 / 20), so it reads as a hill rather than the
sandbar 11.32 had.

## apps/forest

**apps/forest is the scene where instancing, LOD and culling all matter at once**
(spec 11.29). `--headless --frames --screenshot --profiler`, plus
`--no-instancing`, `--no-lod`, `--lod-bias`, `--no-spatial-sort`,
`--render-mode`, `--seed`, `--cam-eye`/`--cam-target`, and `--water`
/ `--water-level <f>` (spec 11.32 — floods the terrain; `terrain_height_at`
satisfies the water system's `WaterHeightFn` directly, so the surface shoals against
real terrain with no heightmap stored and nothing copied. Costs 0.535 ms GPU against
a 78.9 ms opaque pass at 800x450). Exposure is pinned in the
app, so its frames are comparable without `--no-auto-exposure`.

**Since spec 11.63 it is an ISLAND on a quadtree with resident regions**, and the flags
that reach that are the ones to know before comparing any forest capture. The ground
falls to a shoreline past 0.72 of the half-extent and the sea is on by default, so
`--water` is now a restatement rather than a request; `--no-island` takes both the falloff
and the sea away and is the configuration every arm written before 11.63 measures.
`--terrain-extent <f>` is the domain half-width and the only way to ask what a bigger
world costs — 16x the ground area takes the quadtree from 364 patches to 706 where the
fixed grid goes 64 to 1024. `--no-quadtree` is the fixed tile grid, NOT an identity: a
quadtree draws a different surface at a different density, which is the point of it.
`--no-regions` is one always-resident region over everything. `--region-radius <f>` /
`--region-span <f>` dial residency down far enough to churn, which the shipping radius
deliberately does not — it is wider than a kilometre world's own diagonal, so this app's
historical configuration keeps every region resident and nothing about it moved. `--walk
<speed>` drives the character forward and turns about-face at the halfway frame, which is
the ONLY way a headless run crosses a region boundary: residency follows the camera, the
camera follows the player, and no key is ever pressed. Probes: `--terrain-quadtree-probe`
(selection, morph windows, seams, and how much relief each level gave up),
`--region-probe` (residency and a per-cell placement digest), `--cluster-probe`.

**Since spec 11.68 it has a TRAIL**: a gravel path from the island's centre to a shore, as one
road on the terrain material, and the scatter keeps props off its course. `--no-trail` is the
ground before it. Two things worth knowing: the trail draws from its OWN generator rather than
`rnd()`, so the scatter's stream is exactly where it has always been and the reject only turns
candidates away (measured 165 rejections, 1936 trees to 1935); and it is SUBTLE at this scale —
three units wide on a kilometre island, under a canopy — so read it through `--scatter-probe`'s
`scatter-probe road` row rather than expecting to find it in a wide shot. The trail is held in
terrain-LOCAL coordinates and handed to the material as local plus centre, the frame
`splat_origin` is already in, so an origin shift moves the road and the scatter test together.
**And a road makes the DOMINANT-INDEX read ulp-sensitive under an origin shift.** Two runs that
shift the world by different amounts reconstruct `authoredPos` with different rounding; the
dominant-layer index rides the atlas alpha and is read with an unfiltered `texelFetch`, so a
road's hard discontinuity in that field flips one texel and switches the whole detail tap.
Measured 106 px of 1.44M at an offset of 4,096, with the scatter rejecting an identical 168
candidates on both legs — so it is the rendered frame, not the placement. **This is not about
"analytic edges"**, which is what 11.68 first recorded and got wrong: forest arms the composite
cache, so the road reaches those pixels through the baked atlas exactly as the splat does. The
general property is that any splat feature sharp enough to flip the dominant index is
ulp-sensitive under a shift. Only `origin-auto` passes `--no-trail` for it — it is the one arm
demanding exactly 0 px between two runs that shifted by different amounts, where every other
origin bar is 2% of frame, a 20% floor or a ratio. `origin-shift` deliberately keeps the trail:
it is the suite's only render of a road under a shift, and a road that failed to follow
`authoredPos` would land in the wrong place, which is full-size rather than sub-pixel.

**Two things about the island that read as bugs if you meet them cold.** Sea level is
**−35 and not 0**: the fbm is symmetric about zero, so a waterline there would flood half
the interior. And the island reads as a LOW DISC — the fbm's realized relief inside is
about ±15 units over a kilometre against a 140-unit rim, which is the terrain's own
character and not the falloff. `--erode` is the cheapest thing that gives it structure.

**The regions are a grid in the APP, not a quadtree depth**, and the two are deliberately
uncoupled: the scatter, the prototypes and the collider are all `forest.c`'s, and the
quadtree's patches live at many levels while residency wants one size. A region is seeded
from its own CELL COORDINATES rather than drawn from one global stream, which is what lets
it be freed and rebuilt identically — the clump field it rejects against is a function of
position, so groves still run across a border. Its instance nodes go under the GLOBAL
per-prototype groups and NOT under a node of its own: a group per region would put a
foreign mesh between every pair of instances the batcher wants to join, and every run would
be length one. What a region keeps instead is the list of (group, node) pairs it added.
**An origin shift frees and rebuilds every resident region and resets those groups to
identity** — `scene_apply_origin_delta` translates every root child, which is right for
instances placed before the shift and wrong for any placed after.

**And since spec 11.59 it can ERODE its terrain, or load one somebody else made.**
`--erode` (`--erode-res <n>` default **513, not 512** — the grid is node-centred, so it has
res−1 cells and a mip pyramid halves only while that is even; at 512 the field gets no
levels at all and every coarse quadtree patch point-samples the full-resolution surface),
`--erode-iterations <n>` default 220,
`--erode-workers <n>` to pin the thread count, `--erode-save <p>` to write the result),
`--heightmap <p>` + `--heightmap-range <lo> <hi>` to load one instead, and two probes:
`--terrain-erosion-probe` and `--terrain-height-probe`. Since 11.60 there is a third,
`--scatter-probe`: the drainage the scatter placed trees into, beside the terrain's own
distribution and the FRACTION of the domain the rule rejects. That last number is the point of it —
`erosion.c` normalises flow to its own peak, so a maximum near 1.0 is true of any sim that ran and
says nothing about whether there were candidates to reject. It is also how `TREE_MAX_FLOW` was found
rejecting **43.9%** of the terrain under a comment claiming a few per cent; derived from
`TERRAIN_CHANNEL_FLOW_LO/HI` instead of stated beside them, it rejects 9.9%. All OFF by default — the bake costs
**452 ms at 512² × 220 on eight threads, 1839 ms on one** (4.1×; taken at 512, while the default is
now 513 — 0.4% more cells, so read them as the size rather than as the shipping default), and nothing about the app needs
it. **Those are RELEASE figures**: `build.sh` defaults to the debug preset, which passes no `-O` at
all, and the same bake there is 2.3 s / 14.8 s. A timing taken from `out/bin` rather than
`out/release/bin` is measuring the absence of inlining, not the sim.

**And since spec 11.69 it can STREAM a field rather than hold one.** `--terrain-stream <p.cts>`
installs one (opt-in by naming a file, so there is no `--no-terrain-stream`: the off leg is the
whole-file `--heightmap` path every terrain arm already covers, and a stream WINS over both
`--heightmap` and `--erode`, said out loud like the pair below it).
`--terrain-stream-save <p>` writes the installed field out and doubles as the `.r16`→`.cts`
converter — it runs after the pyramid site, because the levels are what the file stores, and it
**refuses a field with no coarse levels** (which is what 512, or the 256² committed fixture,
produces). Then `--terrain-stream-budget <n>` (tiles read per update), `--terrain-stream-window
<n>` (window edge in tiles), `--terrain-stream-resident-res <n>` (the diagnostic that lowers the
whole-level threshold so a fixture-scale field streams at all) and `--terrain-stream-probe <n>`.

**What it costs, and the scaling is not the one the word "streaming" suggests.** A 4097² eroded
field is a 300 MB file against a 291 MB resident one; streamed over a 4 km domain the windows hold
**86.5 MB**, and over an 8 km domain **36.5 MB** — LESS, because resident cost tracks the coverage
the app asks for divided by the CELL rather than the size of the map, and doubling the world at a
fixed node count doubles the cell. **Growing a world is free; refining it is not.** On forest's own
1 km default it buys nothing and holds 274 MB, because the region radius asks for exact heights
across the whole world — the system answering correctly, not failing to arm. This is a big-world
feature and forest's shipping world is not one.

**Two things about it that read as bugs if you meet them cold.** Level 0's window is sized by
whichever is LARGER of `--terrain-stream-window` and the coverage a region load needs, and the
latter comes from `--region-radius` — so at the shipping 1500 the finest window covers a
kilometre domain outright and nothing about streaming is observable. **`--region-radius` is the
lever that makes level 0 actually stream**, which is why every gate arm here passes it and why
the walk arm inherits `REGION_CHURN`. And at forest's default 513 field, `RESIDENT_RES` 1025
holds *every* level whole, so streaming quiesces into an exact identity with no special case —
that is the identity configuration, not a failure to arm.

**The `.cts` fixture is generated at gate time, not committed.** `terrain_fixture.r16` is 256
nodes and 255 is odd, so it halves zero times, carries no coarse level, and the save refuses it;
the streaming arms paint a 257-node twin from the same closed form `gates.py` already restates.
No second asset, no second statement of what that terrain is.

**That warning did not stop it happening a second time, so here is the general form** (spec 11.64).
Forest's whole startup is **6.57 s debug against 1.40 s release**, and the ratio is not uniform —
Jolt's per-region BVH build is **24x** (0.981 s → 0.040 s) where the scatter is 5.7x
(0.206 s → 0.036 s). So a debug profile does not merely inflate, it **re-orders**, and the danger
is specific: it makes cheap things look expensive in exactly the proportion that motivates work on
them. A whole perf backlog was written against `-O0` arithmetic and retired on one release run,
where the scatter and the colliders together are 6% of startup rather than 26%. **The largest
region-load cost in either build is Jolt's BVH builder at 85% of collider time** — inside a
vendored library, which is why nothing reading cetra's own source found it.

`--heightmap` WINS over `--erode`:
a file is a statement about what the terrain is, and re-eroding it would be eroding someone's
finished work.

**The derived-data cook** (spec 11.99) — forest and render both take the same three flags.
`--cook-dir <p>` points the transparent cache somewhere other than the default (`cooked/` at the
repo root, or `CETRA_COOK_DIR`); `--no-cook` runs every bake live and touches no cache — **the
flag any measurement OF a bake must carry**, since a warm cache serves the artefact instead of
running the work being measured (the terrain group forces it wholesale, because `terrain-threads`
varies a worker count the cook key deliberately omits); `--cook` is the pre-warm verb, a headless
run whose deliverable is the per-artefact `cook site=...` rows and the `cook-summary`; on render
it exits when the async loader has drained, so a texture-heavy model warms fully rather than at
five publishes per frame. A `--cook` against an already-warm directory exits 0 with `cooked=0`,
which is success. Note `--terrain-erosion-probe` under a cook hit refuses by name rather than
printing stats — the sim it describes never ran; pass `--no-cook` to measure it. Measured on the
gate fixture (eroded 257×60, single samples off the suite's own runs): cold ~6.7 s debug, warm
~2.7 s.

**And since spec 11.62 it can put its world anywhere, and move it while running.**
`--world-offset <units>` places the terrain, scatter, lights, camera and physics that far from the
origin with the offset MATERIALISED into every coordinate — the instrument fp32's relative precision
was measured with. `--origin-shift-at <frame>` re-centres the world on the camera at a named frame,
in the `--shadows-off-at` idiom, because a shift is a TRANSITION and no headless arm can reach one
otherwise. `--origin-shift-distance <units>` does the same automatically once the camera drifts that
far. **`--water` is refused with either**, and says so: water's bed domain is a half-size about the
storage origin with no centre of its own, so an offset world would shoal against terrain kilometres
away. `apps/render` gains `--shadow-center <x,y,z|auto>`, which is the only way to reach
`ShadowSystem.scene_center` from outside the process.

**What that measurement found is the thing to carry**, because it is not what anyone predicts: the
dominant cost of moving a world is NOT precision. It is anything reading a world position as an
IDENTITY. Forest's wind phase hash re-phased **43% of the frame from twelve units out, and was still
43% at 12,000** — flat, because a hash is discontinuous. Underneath it the actual precision curve is
orderly: 0.10% of the frame at offset 12, 0.73% at 1,200, 3.95% at 12,000, 45% at 262,140, and the
terrain height function's own error quadruples as the offset quadruples. So `include/world_origin.glsl`
and its `authoredPos()` are the larger half of the feature, not a footnote to the subtraction.

**`terrain_height_at` now has two SOURCES and one contract** (`procedural/terrain.h`). NULL field =
the fbm, which is byte-identical to every frame before 11.59 (measured 0 px on forest at
`--render-mode 6`, against a 0 px floor). A `TerrainField` = a Catmull-Rom sample of a stored grid.
Three things about it are contracts rather than details:

**Since 11.69 a field has two RESIDENCIES, which is a different axis from its two sources.**
`TerrainField.stream` non-NULL means the plane pointers address a resident WINDOW rather than the
whole level, so the res-by-res indexing every other field admits is invalid on that one — the
paths that would do it (`heightmap_save`, `terrain_field_measure`, erosion) refuse a streamed
field by name and the range comes from the file's manifest. The SAMPLER is unchanged either way:
`sample_plane` became a view over `(base, res, stride, origin)`, and at stride == res with origin
zero the address reduces to what it always was, so the unstreamed path is bit-identical and stays
a single untested branch rather than becoming a mode.

**And since 11.63 it has LEVELS, on both sources.** `terrain_height_at_level(p, x, z, k)` is the same
surface with everything below level k's own cell removed — a filtered mip on the field path, dropped
octaves on the fbm path — and **level 0 is `terrain_height_at` bit for bit**. A consumer names a CELL
rather than a level index (`terrain_level_for_cell`), which is what keeps a quadtree patch and its
parent one level apart without either knowing the other exists. The pyramid **FILTERS** under a
separable `[1 2 1]` tent, and the obvious alternative is worth knowing about because the spec
originally mandated it: every other node, so a coarse node IS a fine node — which delivers **nothing**
whenever a patch cell is a whole multiple of a field cell, because reading level 0 at every other node
returns exactly the floats level 1 stores. Measured 0.000000 units dropped at every level; the tent
drops 1.87 at one quadtree level and 4.02 at the next. What a coarse mesh needs is the detail REMOVED,
not addressed more cheaply.

**`island_start` / `island_depth` shape a domain into an island** (11.63), applied to the ANALYTIC
height alone — so `terrain_field_seed` bakes it in and an eroded island is eroded AS an island, while
a heightmap loaded from a file is left alone because a file is a statement about what the terrain is.
The radius is Euclidean against a square domain: it passes 1 at the edge midpoints and reaches √2 at
the corners, so the corners are open sea.

- **Bicubic, not bilinear**, because `terrain_normal_at` central-differences over 1.30 units on a
  ~2-unit cell and a piecewise-constant derivative would facet the normals — reaching the scatter's
  slope gate as rows of missing trees, not just the shading.
- **Outside `[-extent, +extent]` a field CLAMPS its coordinate**, not its taps. Clamping taps alone
  still interpolates with an out-of-range `t`, which extrapolates the cubic past the edge. Callers
  really do query out there: the third-person camera eye trails the player and leaves the domain.
  Note this is nearly untestable far out — Catmull-Rom over four EQUAL taps returns that value, so
  an unclamped sampler also returns the edge once every tap has clamped to one column. The
  difference lives in the first cell beyond the boundary, which is where `terrain-clamp` samples.
- **Thread-safety is unchanged, not improved.** The analytic path still memoizes a permutation table
  in file statics. The field path happens to touch no statics, but the function's contract is the
  weaker of the two.

**The masks are why the sim exists, more than the silhouette is.** `terrain_mask_at` returns flow /
deposit / wear, all exactly 0 with no field — so a caller blending by them degrades to the
un-eroded look rather than to a special case, which is what keeps the analytic path an exact
identity. `flow` is **drainage, not depth**: rain falls on every cell so standing water is high
everywhere and says nothing, and it is log1p-compressed before scaling because drainage is
heavy-tailed where erosion and deposition are not. Its mean sits near 0.4, so **a channel is the
top few per cent and a threshold near that mean paints the whole map as riverbed.**

**Two numbers to know before touching `erosion.c`.** Rain over evaporation IS the equilibrium water
depth — the first defaults put ten units of standing water over two-unit cells and the whole terrain
was a lake with nothing draining. And the sim is **bit-identical at any worker count** by
construction (double-buffered Eulerian, disjoint row bands, no synchronisation past the joins, the
cloud bake's shape); `--terrain-erosion-probe` prints an FNV-1a `digest` over all four planes, which
is the only thing that can make that claim — the budget sums cannot, because addition hides
compensating differences. **Droplet/Lagrangian erosion is refused on exactly that test.**
**The claim is WITHIN one build**: the digest is identical across worker counts and differs between
an `-O0` and an `-O2` build of the same source, which is the same "two runs of one build is not two
builds" rule the render side already lives under.

**It has wind on its 2,000 trees since spec 11.53**, and the header comment saying it deliberately
had none is gone with it. A wind material used to be exempt from every frustum test, so 2,000
uncullable trees would have defeated the app's whole purpose; with the bound in place the wind costs
**5 meshes of cull out of 28,256** and nothing in the opaque pass. Both prototypes come from
`tree_gen`, which already authors the UV1 branch phase and flex weight, so this is a material field
and not new geometry. The scene sets `phase_variation`, without which every instance of a shared
prototype sways on the same beat -- wind is evaluated in object space from per-mesh uniforms, so
making the trees cullable is necessary and *not sufficient* for a forest to look like one.
**`wind.phaseVariation` is a `.cscn` key with no CLI flag**, in turns, and it **defaults to 0** --
which is the lockstep every scene authored before 11.53 had, and an exact identity rather than a
close one, so no existing wind frame moved. It is derived from the object's world origin rather than
stored per instance: an InstanceBlock field would cost a kilobyte of std140 padding to restate what
the model matrix already says, and a phase keyed to batching would flip as LOD and culling reshuffle
runs and would break the instanced-against-non-instanced identity.

Two things it establishes that no fixture could. **Scatter ORDER decides whether
batching works at all**: the batcher joins only consecutive survivors, so with
most props frustum-culled a randomly-ordered scatter collapses to runs of one.
Morton-ordering the instances within each prototype takes the same 2979
instances from 2368 draws to 1287 — identical instances and identical triangles,
only the draw count moves, which is what `forest-order` asserts. And **LOD fights
instancing**, inherently: the batch key is `(mesh, lod)`, so one prototype
spanning a range of distances splits into separate runs. It is a CPU-for-GPU
trade whose direction depends on the framing.

**AGENTS.md used to claim apps/tree is 0 px headless across two runs. It is
not** -- measured 31,034 px between two runs of one unmodified build, and 5,655
px at frame 1, so it is not accumulation. `camera_drag_update` at `tree.c:761`
takes `glfwGetTime()`, deliberately, for drag damping. Anything comparing tree
frames has to measure that floor first.

**Instancing and LOD are both ON by default and both are escape-hatch flags.**
`--no-instancing` gives one draw per mesh; the batched and unbatched paths carry
the same floats into the same arithmetic, so it compares at 0 px and exists for
bisecting rather than for looking different. `--no-lod` pins every draw to LOD
level 0. Neither moves a golden today: LOD chains are built at import by
meshoptimizer, which weights boundary and seam edges heavily, so the leaf cards
and grass blades that dominate this corpus barely simplify, and every skinned
mesh is refused outright (weights do not transfer to surviving vertices). raiden
builds 0 chains, `abandoned_window` builds 23 of 71. Note **weighted, not
locked** -- `lod.c` passes options 0, and only `meshopt_SimplifyLockBorder` makes
a border uncollapsible. The distinction matters the moment anything tiles
(`apps/forest`): without that flag two neighbours at different levels can
T-junction, so a crack is a possibility the code does not currently exclude. The `triangles` column in the
SUBMISSION table is the only counter a level change moves -- `draws` and
`instances` are blind to it.

---

## The other apps, and the AA mode each one chose

Spec 11.103 asked what anti-aliasing every app should run and got a different answer four times,
which is why this section exists rather than one line saying "they inherit the default".

**`apps/gametest`** gained a CLI in 11.103 and had none before it: `-x/--headless`, `-f/--frames`,
`-S/--screenshot`, `--screenshot-every`, `--taa`, `--msaa <n>`. The HDR environment stays
POSITIONAL — an unrecognised token is still taken as the path, which is the whole interface the app
had. It runs **one sample plus TAA windowed**, the render-app policy, because every surface in it is
a rigid mesh on the `pbr` program and so writes a motion vector. **Spec 11.109 added five**, which
make it the one app a headless run can play: `--pad-script <file>` replays a scripted gamepad on
slot 0 (the format is in `cetra/src/game/input.h`; a file that is missing or will not parse exits
1, so a gate never mistakes an idle pad for a passing layer), `--gamepad-db <file>` adds an SDL
controller mapping file to GLFW's bundled table (same refusal), `--trace-player` prints the
player's pose, velocity, ground state and the move and jump the step acted on, every
`--trace-every <steps>` (default 30), and `--print-bindings` lists the action table and exits.

**Spec 12.0 added audio** — a beep on jump and spawn and a looping tone carried by the door as an
`AUDIO_SOURCE` component, all procedural (no committed audio). `--mute` silences the master bus.
`--audio-probe <case>` is a headless, self-contained offline render (miniaudio's `noDevice` engine)
that plays a fixed geometry, measures the mixed PCM and prints it, then exits — the cases are
`onset`, `pan`, `distance`, `master` and `decode`, which the `audio` gate group reads; `decode`
takes the WAV to load from `--audio-file <path>`. Because it opens no device, it runs anywhere the
gate suite does.

**Spec 12.1 made the player a rig.** `apps/gametest` loads `assets/models/puppet.gltf` and drives it from
an `ANIMATOR` component: idle, walk and run blended from the character controller's POST-SOLVE
speed (so walking into a wall stops the walk rather than running on the spot), a jump one-shot
crossfaded onto the base layer that returns to the locomotion space by itself, a wave on the right
arm's subtree bound to **E / left bumper**, and footsteps fired from the clips' own timeline events
through 12.0's audio. `--no-puppet` keeps the original red box (and the all-rigid frame);
`--puppet <path>` swaps the rig; `--twin <clip>` stands a second rig beside the player playing its
own clip, which is what two independent poses in one frame look like. `--anim-probe <case>` is the
headless probe the `anim` gate group reads -- `locomotion`, `crossfade`, `layer`, `two-rigs`,
`phase`, `events`, `import`, `stride`, `rate`, and since 12.18 `root` and `rootmotion` -- built
like `--audio-probe`: a self-contained headless game with
no window that ticks ANIMATOR components through the loop's own `update_all_animators` at a fixed
1/60, prints `anim <case> <label> <key> <numbers>` and exits. `--trace-player` gains a tail after
`jump`: `anim <knob> <w_idle> <w_walk> <w_run> <fade> <layer> <source>`, APPENDED rather than
inserted so the `gamepad` group's regex still matches the same line -- and since 12.18 a
`yaw <radians>` column after the camera's, appended for the same reason.

**Since 12.20 the rig plays what a TABLE says.** `cetra/src/anim_graph.h` carries the states and
the transitions; what the app writes each frame is what is true (a ground knob, a rate, a depth in
metres, grounded, vertical speed, ragdolled) and the graph decides the rest. Three consequences a
driver of this app will meet. **`--graph-probe <case>`** is the headless probe the `graph` gate
group reads -- `order`, `any`, `and`, `trigger`, `elapsed`, `hysteresis`, `reenter`, `disabled`,
`instances`, `unreachable`, `authoring`, and the four that need a rig: `finished`, `fade`, `guard`,
`return`, `identity`. Most create **no engine at all**, because a graph binds to a NULL animator
and a state may name no source, so the table is exercisable as the pure function over named values
it is. **`--trace-player` gains a third tail**, `graph <state> <seconds>`, after `yaw` and by the
same append-never-insert rule -- and no state may be named containing `" yaw "`, since that
regex is greedy. The source column above says what is PLAYING, which a state and its resume can
share; this says which state chose it. **`--no-swim`** withholds the stroke clips, which makes the
water state unreachable: the run says so once by name at startup and prunes every row into it.
It exists because no committed rig reaches that configuration -- the generated puppet carries a
stroke and a rig with no locomotion of its own is given the shared set, which carries one too --
while a downloaded humanoid with its own walk and no swim is the ordinary case `--puppet` is for.
Before the graph, that character was labelled as swimming while its walk cycle played, had its
playback rate pinned and its root-motion branch skipped, and decayed to a standstill.

Two states on the committed default are **unreachable and say so**: it carries no `fall_cycle` and
no `touch_down`, so `air` and `land` are pruned and a jump is the rise alone. The full airborne
path needs a rig with no locomotion of its own -- `--puppet assets/models/t_pose.fbx`, which takes
the shared set -- and that is what `graph-parity-air` runs on.

**It is frame-deterministic headless, and this paragraph said otherwise for six specs.** Two runs
of one script trace byte-identically and their frames differ by **0 px** (measured, spec 11.109),
because headless the engine hands the loop its FIXED frame dt, the seed is `srand(42)` and Jolt
steps a fixed dt. The 48 px this entry used to report between two identical runs, and its
explanation that wall-clock frame time fed the accumulator, were the FPS overlay: drawn headless
from the wall clock, and off since 11.109 (108 px with it on, 0 without, same build). Two runs at
different sample counts are still not comparable, for the ordinary reason that they resolve
differently.

**Spec 12.2 gave it menus.** `apps/gametest` carries four screens — a main menu, a pause menu, a
settings screen and a non-modal HUD — built on the game UI layer (`cetra/src/ui.h`). They are on
by default and all closed: **Escape** opens the pause menu, and quitting is an item inside it
rather than a key, which is the change every app in the tree took. `--no-ui` runs without any of
it. `--ui-screen <name>` opens one at startup (`main`, `pause` or `settings`), because a headless
run has no Escape key to press and a menu otherwise never appears in a capture; `--ui-focus <n>`
then presses "down" n times through the real navigation path, which is how `menu_focus` is
photographed with the focus somewhere other than where it lands — and because nothing is focused
to begin with, the first press only ACQUIRES, so `2` is what reaches the second button. `-W <n>`
and `-H <n>` set the window size, which this app had no way to state before and a golden recipe
needs.

`--ui-probe <case>` is the headless probe the `ui` gate group reads, in the shape `--audio-probe`
and `--anim-probe` established: it prints `ui <case> <label> <key> <numbers>` and exits. The cases
are `layout`, `layout-resize`, `wrap`, `nav`, `hit`, `capture`, `stack`, `theme-identity` and
`settings`. **It draws no frame**, which is the claim worth making precisely, because this line
used to say "no window and no GPU" and that is true of exactly one case: `settings` needs no
engine at all and runs before one is created, while every other case needs a font — FIT sizing is
made of measurement — so it takes a headless game and therefore a hidden GLFW window and a live
GL context, exactly like `--save-probe` below. What is true of all of them is that layout is a
pure function of (tree, width, height) and the input pass takes a struct of values rather than a
device. `CETRA_SETTINGS_DIR` points the settings case, and any gametest run, at a directory of
its own rather than the player's real one; the gate and the golden bake both set it.

**Display modes (spec 12.15).** `--fullscreen` and `--borderless` start the run in that mode
rather than switching into it after a windowed frame, and `--monitor <name>` says which display —
`--list-monitors` prints the names this build can see and exits. Both non-windowed modes take the
monitor at its CURRENT video mode: nothing here ever changes a display's resolution, so there is
no mode to restore after a crash, and `--render-scale` remains the performance lever it already
was. The same three modes are on the settings screen, where a Monitor row appears only when there
is more than one display to choose between.

**Ragdoll (spec 12.16).** `K` or the left stick click drops the player into a physically
simulated heap: the controller stops steering, the bodies take over, and the camera keeps
following because the entity tracks the hips. There is no way back up — getting up is a
different problem and a different spec. A key rather than a consequence because this app has no
damage, no health and no death to hang it on.

`--ragdoll-probe <case>` is the headless probe the `ragdoll` gate group reads, in the same shape
as the four above: cases `build`, `shapes`, `scale`, `settles`, `pose`, `frees`, or `all`.
Unlike `--display-probe` the SIMULATION is reachable — a headless game carries a real physics
world — so the last three step Jolt and read the bodies back. `--puppet <rig>` selects the
skeleton, which is how the build cases run against both the generated puppet and `t_pose.fbx`.

**A case is not one arm, and that is what keeps the group to four processes for eight arms**:
`build` prints the shapes rows too, and `settles` prints pose, roundtrip, rigid and frees, since
all five read the same stepped world. Asking for a case you did not name costs nothing; the
group's reader keeps every row the process produced rather than filtering to what it asked for.

`--display-probe <case>` is the headless probe the `display` gate group reads, in the same shape:
it prints `display <case> <label> <key> <numbers>` and exits, with cases `placement`, `monitors`,
`apply`, or `all`. An unrecognised case is a failed run rather than a silent one. It reads what a
mode DECIDES rather than what it does — the switch is refused under headless, so that a suite can
never seize a display and a golden's frame size can never come from the machine's monitor.

**Saving (spec 12.3).** `F5` / right bumper quicksaves and `F9` / right stick click quickloads,
both ordinary rows in the action table so they rebind like anything else. They are deliberately
NOT flagged `ui`, so a menu suppresses them — saving from inside a pause screen would fold the
menu's own state into what the world looks like. The file goes beside `settings.json` in the
platform's per-user location, so `CETRA_SETTINGS_DIR` points it somewhere hermetic too.

`--save-probe <case>` is the headless probe the `save` gate group reads, in the same shape as
the three above: it prints `save <case> <label> <key> <numbers>` and exits. The cases are
`roundtrip`, `entities`, `spawned`, `drops`, `floor` and `migrate`. It builds its own scene,
physics world and entities rather than using `on_init`'s, since a probe game has no init
callback — **no window and no frame**, though unlike `--ui-probe` it does create a GL context,
because the crates it spawns upload their meshes. Each case wants its own `CETRA_SETTINGS_DIR`:
they share one slot name, so they would otherwise read each other's file.

The ground spec 12.4 needs to plant a foot on is **always in the scene**: a ramp rising 1 in 4
from x = 14, and three steps of half a metre at x = −15/−17/−19. Walk **+X** for the ramp, **−X**
for the stairs; that is where the knee actually bends, since on the flat floor the solve is
correctly an identity and there is nothing to see.

It stood behind an `--ik-ground` flag at first, to keep it out of the two menu goldens — the
menu is drawn over the live scene through a backdrop only 77% opaque, so the floor reads
through it and anything in frame moves them. That traded the only demonstration of the feature
for two unchanged reference images, which is the wrong way round: the goldens carry the fixture
now and were re-baked for it.

The follow camera is **on by default**; `--no-follow-cam` gives back the fixed orbit about the
world origin. It shipped behind `--follow-cam` while the camera was being settled and was turned
around once it was: a demo whose subject is a drop into a cavern should not open with the camera
pinned to a point the player walks away from. The opt-out spelling matches `--no-puppet`,
`--no-chaser` and `--no-ik`.

That makes the whole of spec 12.6 default-on, which is the 12.4 reasoning carried to its end —
a flag that hides a feature from the only two pictures showing the world trades the
demonstration for two unchanged reference images. Both menu goldens photograph this camera, so
both were re-baked for it.

**The arrow keys turn it and nothing else does**, `apps/forest`'s bindings and rates — the right
stick too, on a pad. It orbits the player's position at a fixed distance, pitch clamped so the
eye cannot roll under the floor. Movement under the flag is camera-relative: W into the screen,
S back toward the lens, whichever way you have aimed it.

**That sentence was false for four specs and is worth keeping as a warning rather than quietly
repairing.** Spec 12.13 made movement WORLD-ALIGNED — W a fixed world direction, so turning the
camera left the character walking across the frame — and updated its own code comment and
neither document. 12.17 put it back, and the reason it went back is as much that the code and
the docs disagreed as that the scheme was wrong: the behaviour a player meets should be the one
written down. **Since spec 12.19 that is a FIELD with a name rather than a
file static somebody read**: a camera rig publishes `steers_controls`, defaulting to false, and
the game asks the camera what its input means. The follow rig sets it; `--no-follow-cam`'s drag
orbit and a `--cam-eye` pinned framing leave it false and movement world-aligned, deliberately,
since pinning a camera states a POSE and should not silently rotate the controls. Turning it off
on the follow rig reddens `pad-camera-relative` by 104.85 degrees -- to the second decimal the
error 12.17 measured for the scheme it was reverting -- so a third silent flip is not available.

It shipped first as a camera that trailed the player's facing automatically, which was wrong in
a way worth recording because two sign changes failed to fix it. `player_yaw` follows the
velocity, so a camera chasing it can never be in front of you: pressing back turned the
character round and the camera swung in behind, both directions read as forward, and there was
no backward left to invert. A heading the player controls has none of that.

It is mutually exclusive with mouse-drag orbit, the way `apps/tree`'s walker is: both own the
camera, and the camera rig rewrites the eye from its anchor, arm and aim every frame. The
arrows are shared with menu navigation, which is safe rather than the collision the action
table warns about — `ui_up`/`ui_down` carry the `ui` flag, so a menu suppresses the camera
actions and the arrows navigate it, while with no menu open the UI ignores them.

**And since 12.7 the camera can be STATED rather than followed**: `--cam-eye x,y,z`,
`--cam-target x,y,z`, `--cam-up x,y,z` and `--fov <degrees>`, the render app's spelling. Both
eye and target or neither — half a pose is a direction nobody gave, and it is refused by name
rather than half-applied. A stated pose stands the follower down, so it also overrides
`--no-follow-cam`'s orbit.

It exists because a followed camera cannot be re-photographed. The eye is derived from the
player every frame, so a framing seen once is gone, and *every* question of the form "what
does this look like, and did that change it" needs the same pixels twice. Diagnosing the
cluster-overflow wedge took four A/B comparisons against a pinned pose; without one there is
nothing to compare, and the frame moves for reasons that have nothing to do with the change
under test. The same reason apps/render grew these first.

`--no-ik` runs the player without foot planting at all, which is the quickest way to tell a
planting artefact from an animation one — worth knowing that the walk clip bends no knee of its
own, so on flat ground the two look identical by design.

`--no-lock` (spec 12.9) keeps planting and drops the LOCKING above it: contacts are still found
and feet still meet the ground, but no world-space point is held — planting's behaviour, though
not bit-identical to 12.4's, since the transition blend and the pelvis cap both changed under it.
**On the generated puppet the two are 0 px apart at any speed**, and that is the mechanism
working: its walk is a straight-leg pendulum with no stance, so the contact label never fires and
there is nothing to hold. The run says so at startup — *"Locomotion clips imply no stride"* — and
travel stays a fraction of `PLAYER_SPEED` 10.

**On a rig whose clips DO imply a stride, travel comes from them** (spec 12.10) and the
comparison needs no flag at all:

```bash
./out/bin/gametest --puppet assets/models/t_pose.fbx            # full stick is 2.67 m/s
./out/bin/gametest --puppet assets/models/t_pose.fbx --no-lock  # the same, planting only
```

measures **2217 px** apart at full stick. The axis is metres per second there: each locomotion
entry sits at the speed its clip implies, and the clip is played between 0.6x and 1.6x to cover
what sits between and beyond them.

`--speed <m/s>` still caps travel, and since 12.10 it also **overrides the derived figure** — so
it is now a way to walk slower than full stick rather than the only way to see the feature. It no
longer carries the old caveat about which constant the knob divides by: an absolute axis has no
gear to re-normalise.

**`--no-root-motion` (spec 12.18) is the other half of that story, from the opposite side.** On
the generated puppet the locomotion pair is `travel_walk` and `travel_run` — clips that state how
far they travel — so the character goes exactly as far as the animation says and full stick is
**6.40 m/s**: the run clip carries 1.60 m per half second, which is 3.20 in the rig's own
units, and the demo stands its player on a node scaled 2. The flag puts the in-place pair
back, and the startup line tells you which way round you are:

```bash
./out/bin/gametest                    # "Locomotion travels 0 to 6.40 m/s, stated by the clips"
./out/bin/gametest --no-root-motion   # "Locomotion clips imply no stride"; travel is the constant
```

**Two keys come with it**, and they are the thing stride matching cannot express: **Q** (or the
right trigger) lunges a stated 1.20 m -- 2.40 on the ground, the rig being scaled 2 -- and **C**
(or the left trigger) spins half a turn on the spot. Both are distances an animator chose, so rate-scaling one is exactly wrong. The triggers
rather than buttons because every one of GLFW's fifteen is already bound.

Two behaviours invert with the ownership and are worth expecting before you meet them. Walking
into a wall plays the walk ON THE SPOT, where the stride-matched path stops it — the blend knob
has to come from the stick, since the travel comes from the clip the knob selects. And the
character turns toward where you point and then travels forward, rather than travelling in the
direction you point: the facing is what the stick sets.

`--puppet` never enters any of this. No committed FBX clip carries a root curve — `strut_walk`
states 0.000071 m of travel over its whole loop — so an imported rig measures its stride exactly
as it did before.

`--ik-probe <case>` is the headless probe the `ik` gate group reads, in the same shape as the
four above. The cases split on whether physics is the point. `reach`, `clamp`, `singular`,
`identity`, `pole` and `analytic` build a rig and nothing else — a two-bone solve is a pure
function of a hip, a target and two segment lengths, and giving those a world would only make
exact arithmetic depend on contact slop. **`swing` is a seventh rig-only case and the one the
group leans on hardest**: it ticks an actual walk cycle and measures each ankle's vertical
travel with planting on and off. Every other case poses the rig ONCE and solves once, which is
how eleven arms came to pass while the player's feet were welded to the floor — a solve that
overwrites a stride looks perfect in a single pose. `ground`, `slope` and `step` add a physics world and
the ramp-and-steps fixture, because planting's input is a RAYCAST and three bug classes live only
there: the ray hitting the character's own capsule, the ray missing the ramp and finding the
floor beneath it, and a hit point that is right while the plane is wrong. Those three run with
release disabled — they ask whether the solve reaches the ground it was handed, not whether a
foot should be planted at all. `swing` is the one that asks the second question: it ticks a walk
cycle and measures the ankle's vertical travel, and it is the only case that would notice the
feet welding to the floor, which every other arm passed straight through. It runs PLANTING only,
said rather than inherited, since its bar is argued in terms of the release fade.

Spec 12.9 added two more rig-only cases, taking the tally to nine: `drop` puts a foot half a
metre past full extension so the pelvis cap is the only thing that can answer (nothing had ever
exercised it), and `lock` runs the same walk clip FOUR times — at weight 0 for a clip-only
reference, then planting, then locking, then locking with the body walked at three times the
speed its own feet imply. The first exists because a reference read off a solved pass moves
whenever the solver does, which makes every A/B incomparable with the one before it; the last is
the refusal, asserting the feature lets go rather than straining. `CETRA_IK_TRACE=1` dumps that
case's per-tick heights, contact label and lock state to stderr, where the probe's numeric
grammar cannot reach them — it is what found a retargeted clip's toe joint passing back through
its own bind clearance in mid-swing, which no aggregate would have named.

**`apps/spores`** gained `--taa`, `--headless-jitter` and `--msaa <n>` in 11.103 and **kept its four
samples**, which is the one refusal in that spec. `particle_frag` declares a single output, so a
mote writes no motion vector and reprojects through whatever geometry sits behind it — zero on a
static camera. Under TAA its 32,000 motes stop being dots and become dashes. The flags are the
instrument that measured that and stay for whoever gives the particle pass a motion vector.

**`apps/shapes`** keeps 4x MSAA with no temporal filter, and since 11.103 says so in a comment rather
than leaving it to look like an omission. Multisampling is the natural AA for 2D line art; nothing in
the scene moves, so an accumulator would integrate only its own jitter while switching on the aux
G-buffer, the resolve and the eight passes that key off `taa_resolving`.
The rest of its look is `engine_set_2d_preset`: bloom, GTAO, SSR, vignette, dither and shadows
off, exposure pinned at unity, the `linear` tone curve, and a white ambient radiance on a scene
with no light in it -- under which a material's albedo is the colour on screen. Its filled
primitives carry a +Z normal since the flat generators started writing one; before that they had
none, no light could reach them, and what read as red for years was the 3% ambient floor that
spec 10.1 removed.

**`apps/pcb` is gitignored** (`apps/.gitignore`) and so is not part of this repository at all, which
is worth knowing before looking for it — the example-apps table in `AGENTS.md` still lists it. The
same reasoning applies where it exists locally, plus one more: most of it draws through the `shape`
program, which writes no motion vector, while its filled primitives draw through `pbr`, which does,
so a dragged shape would reproject two halves of one frame differently.

**`apps/splash` takes no part in any of this**, and it is worth knowing before reaching for a flag.
It never calls `engine_run` — it clears and swaps the DEFAULT framebuffer in its own loop and draws
SDF text straight to it, so `engine->framebuffer` is built and never bound. Its anti-aliasing is the
GLFW window hint, no app-side sample count reaches it, and the engine's screenshot path lives inside
the loop it does not use.
