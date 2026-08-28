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
`-E/--exposure` / `--no-auto-exposure`, `--no-bloom`, `--tonemap <neutral|aces|agx>`
(**not** `passthrough`: `render_args.h` notes the "unset" sentinel deliberately coincides
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
`assets/water_fixture.cscn` authors all 36 and is the block's worked example (35 until 11.84 split the in-scatter in two). **No flag can set any
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
`--texture-probe` too, and is the only place a purely procedural texture set can be priced),
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
against), `--no-lod` / `--lod-bias <f>`,
`--no-sort-opaque` (front-to-back opaque ordering is ON by default, spec 11.30, and it is the larger
of the two overdraw levers by a wide margin — `apps/forest` opaque 306 → 169 ms. Still **not** a 0 px
flag, but only barely and no longer for the reason 11.30 gave: masked materials stopped blending in
11.31, and what is left is coplanar ties, where draw order decides which of two surfaces at exactly
equal depth wins. Raiden moves 31 px, `cornell_box` 1),
`--depth-prepass` (depth before shading, OFF by default and measured to LOSE everywhere in this
corpus — spec 11.31. Masked geometry IS prepassed since 11.31, through its own program in a
depth-only mode, and reaches a better depth complexity than the sort while still being slower),
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
`--msaa N` -- **this app runs 4x MSAA *and* TAA**, unlike `render` which drops to 1 sample
interactively, which is why the MSAA-only grass specks surfaced here first (spec 11.38);
`--msaa 1` is the immune path. Sun: `--sun-elevation` (**default 0.8 degrees, not 14**),
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
px at frame 1, so it is not accumulation. `mouse_drag_update` at `tree.c:749`
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
