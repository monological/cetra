# Shader subsystems

How each shader subsystem works and why it is built that way. `AGENTS.md` keeps the inventory —
which files exist and what each one is for — plus the rules that constrain work in OTHER files;
this is the per-subsystem detail behind those entries.

**Read the entry before changing its subsystem.** Most of what is recorded here is a rejected
alternative or a failure mode that renders a plausible frame: a moon phase that inverts and still
looks like a moon, an IES fold that lights a room the wrong way round, a probe fade that weighs
every floor at zero, a road blended after the height instead of before. None of those produce an
error, and several shipped green through the full gate suite. That is what these notes are for.

**They also rot faster than the reasoning in them.** An audit of `AGENTS.md` found its wrong
claims concentrated almost entirely in names, counts, line references and file lists — not in the
rationale. So treat a `file.glsl:123` here as a starting point rather than a fact, and trust the
argument further than the address.

## Contents

Sky and night: [Day/night cycle](#daynight-cycle) · [The moon](#the-moon) ·
[Night floor](#night-floor) · [Stars](#stars) · [Cloud shadow](#cloud-shadow) ·
[Atmosphere](#atmosphere)

Image finishing: [Tonemap / exposure](#tonemap--exposure) ·
[Purkinje / scotopic shift](#purkinje--scotopic-shift)

Lighting and occlusion: [Specular occlusion](#specular-occlusion) · [IES profiles](#ies-profiles) ·
[Contact shadows](#contact-shadows) · [Clustered specular probes](#clustered-specular-probes)

Surfaces: [Water](#water) · [Clustered decals](#clustered-decals) ·
[Layered surfaces](#layered-surfaces) · [Roads](#roads) · [Composite cache](#composite-cache)

---

## Day/night cycle

`sky_cycle_tick` (spec 11.81), called once per frame from `engine.c`
BEFORE the GI sweep and the shadow pass — the key light it rewrites is what the cascades
fit, and a completed swap must land before the frame's first `bind_ibl_textures`. An app
render callback (the roadmap's sketch) is too late for both. Splits into `sky_cycle_advance`
(clock → sun → live view LUT) and `sky_slicer_pump` (a frame's budget of the sliced
re-bake), because a `--cycle-rebake-at` pumps without advancing and a frozen cycle advances
without pumping. `cycle_hour` (0-24, noon at 12) is a **double** — an accumulator, where a
float32 ulp near hour 24 is half a per-frame increment — and drives BOTH the sun (through
`sky_sun_path`, the equinox path on the stars' own latitude frame) and `stars_hour_deg`, so
sun and stars wheel off one clock. **The cost is inverted from what "slice the bake"
suggests**: the six env faces are ~10 ms and ~90% is the GGX + Charlie prefilter chains, so
the schedule is texel-WEIGHTED work items with mip 0 split by face, and the budget sits just
under the heaviest item so those run alone. The per-frame peak is one mip-0 face and is
irreducible without sub-face slicing (measured 4.74 ms at 800x500 through the `sky cycle`
profiler scope). Slices render into SHADOW textures and swap all four handles plus
`max_reflection_lod` atomically, adopting the outgoing chain rather than reallocating; the
tick returns whether a swap landed and `engine.c` marks the GI volume, because sky.c must
not know what a Scene is. **An atomic bake CANCELS a slice in flight** — without that, a GUI
floor edit or a config restore is silently reverted a few frames later by content baked
before it. Off by default everywhere; `--day-cycle <s>` (0 = frozen clock), `--time-of-day
<h>`, `--no-day-cycle`, `environment.cycle {enabled, day_seconds, hour}`, three config rows,
and `--cycle-rebake-at` as the diagnostic the 0 px `cycle-slice` arm rides.

## The moon

`include/moon.glsl` + one guarded block in `include/sky_radiance.glsl`, over
the surface `moon_surface.c` bakes (spec 11.82) — the sun-disc pattern a THIRD time, in the
two background variants and nowhere else, so the LUT chain, the slicer and the bake cadence
needed **zero** changes.
**PHASE IS DERIVED, NEVER AUTHORED**: `cos(alpha) = -dot(moon_dir, sun_dir)`, the NEGATIVE
of the elongation's cosine because alpha is measured at the moon (0 at full, 180 at new).
Get that sign wrong and the feature inverts while still rendering a plausible moon — and
the two readings AGREE at elongation 90, so a ladder that does not straddle it cannot see
the error. Deriving rather than authoring is what makes a crescent facing the wrong way
**unrepresentable**: the terminator is `dot(n, sunDir)` crossing zero in a disc frame whose
sunward axis IS the projected sun, so there is no sign to invert and no parameter to
mis-author. It also deletes a field rather than adding one.
**A full moon is NOT a Lambertian sphere**, and that is its whole visual signature —
regolith backscatters, so the quarter moon is ~1/11 of full and not 1/2 (Krisciunas &
Schaefer 1991, one line), and the lit face is FLAT with no centre-to-limb gradient.
Shading it by `cos(theta)` renders a snooker ball. Hence a soft STEP terminator one pixel
wide off `fwidth`, and `MOON_LIMB` 0.85 against the sun's 0.4 — same form, opposite intent.
**The PHYSICAL disc size is `sun_disc_deg`**, shared: the two subtend 0.53 and 0.52 degrees,
which is why eclipses work, and a second field would have cost six authoring surfaces for a
difference of ~1/50 px. The GUI slider is "Disc Size" for that reason. `moon_size` multiplies
it for the LOOK — a life-size moon is a dozen pixels — and the aureole's width divides by
its SQUARE ROOT rather than tracking it, because a halo scaled linearly onto a 12x disc is
50 degrees across and floods the star field the moon is meant to hang in front of.
**The SURFACE IS BAKED, and it carries relief and albedo both.** `moon_surface_bake` stamps
~43,000 craters over three size tiers into one 2048x1024 RGBA8 at startup (~0.09 s debug),
RGB the tangent-space normal and A the albedo, threaded on `cetra_bake_bands` and
bit-identical at any worker count. Three things about it are load-bearing. **An impact
EXCAVATES**: the stamp erases prior relief inside the rim and piles it at the crest, in
crater order, which is what makes a young crater cut a clean notch through an old one —
summing bumps averages the overlap into mush, and the overlap is what the eye reads as a
crater field. **The normal is TANGENT-space**, so a coarse mip averages toward flat; in the
body frame it averages toward the mean of the sphere's own normals, which is zero, and a
distant moon turns to noise. And **craters are height AND albedo**, because relief is
invisible at full phase (Lommel-Seeliger, below) and albedo is all there is — a moon built
from relief alone renders a blank disc at exactly the phase people picture.
The reason it is baked rather than evaluated per pixel is a POPULATION ceiling, not a cost
one: a 3x3x3 lattice holds ~27 candidates per octave, and no tuning reaches a saturated
field from there.
**The maria come from DATA** (`moon_map.h`, 32 KB generated by `tools/gen_moon_map.py` from
published selenographic centres) in a **face-locked** frame — indexed by the world normal
they swim, indexed by the terminator's own sun-relative frame they spin, and the second is
the natural wrong turn since that code builds such a frame anyway. Procedural noise gives a
plausible cratered world that is not the Moon: where the seas are is a historical accident
with no generating process to model. Only the LOW-FREQUENCY layout is data — craters, rays
and grain stay synthetic, which is what keeps the face sharp at any `--moon-size` where a
photograph would blur. Earthshine on the dark limb, blue because it is light off an ocean
planet.
**Shading is LOMMEL-SEELIGER**, `mu0/(mu0+mu)`: at full phase the two are equal everywhere
so the disc reads flat, and near the terminator it falls off slowly enough that crater walls
throw their own shade. One term gives both halves of the signature.
The LIGHT is a second `LIGHT_DIRECTIONAL` created **unconditionally** whenever a sky sun
is — gating it on enabled makes `sky.moon` a snapshot row that restores a disc and no
light, since a restore cannot CREATE a Light (measured 139,768 px). Free when disabled:
`shadow.c`'s classification skips non-casting lights BEFORE counting directionals.
`cast_shadows` rides `sky_night_factor`, an exact zero above +3°, so **above +3° the moon
never casts** and the daylight caster count is what it always was; the both-casting window
is exactly (0°, 3°), open at BOTH ends — `sky_night_factor` is an exact zero AT +3° and
`sky_horizon_fade` an exact zero AT 0°, so neither boundary casts — 17 scene-minutes, ≤7 s at
the GUI's longest day.
Under the cycle, `moon_hour = cycle_hour − cycle_moon_offset` with the lag advancing at
24 h per 29.53 simulated days — ONE rate behind two facts, because "rises ~50 minutes
later each day" and "the phase evolves through the month" are the same fact. Left
UNWRAPPED: `sky_sun_path` takes sin/cos of the hour and −5 h vs 19 h agree mathematically,
not bitwise. OFF by default in the library, ON in tree (`--no-moon` off); `--moon`,
`--moon-brightness`, `--moon-size`, `--moon-elevation`, `--moon-azimuth`, `--sky-disc`,
`--no-moon-maria`, `--no-moon-glow`, `--no-earthshine`, `--moon-probe`;
`environment.moon {enabled, brightness, size, elevation, azimuth}` — **no `phase` key,
deliberately**, since one beside a position could author a full moon next to the sun.

## Night floor

one term at the end of `sky_view_frag.glsl`'s march (spec 11.80) — airglow +
zodiacal + integrated starlight as a PREMULTIPLIED uniform, added to sky-side texels only and
dimmed by the ray's own `through`. Living in the LUT bake is the whole design: the background,
the env cube (and so the IBL and every lit surface), and the cloud march's ambient all sample
that LUT, and it re-bakes exactly when the ramp's one input (the sun) moves — so a floor
implemented in the background shader instead lifts the sky and never the ground, which is the
`nightfloor-ground` arm's named mutation. The CPU zenith march adds the same term before its
below-horizon early-out (night fog ambient); constants are C-side only in `sky.c`
(`SKY_NIGHT_FLOOR_*`) because a premultiplied uniform leaves no GLSL copy to sync. The tint
rides at the stars' 0.35 saturation — full extinction browns the whole lower sky at any level.
Shares `sky_night_factor` (the ONE definition of night, +3° to −8°) with the stars. The level
is a LOOK constant, not physics — exposure only darkens, so it stands in for the dark-adapted
eye. OFF by default in the library, ON in tree (`--no-night-floor` off); `--night-floor` /
`--night-floor-brightness` on render; `environment.night_floor {enabled, brightness}` in a
`.cscn`; GUI checkbox + slider ride the sun's re-bake chain, NOT the stars' live-uniform path.

## Stars

`include/stars.glsl` + one term in `include/sky_radiance.glsl` (spec 11.79) — a
procedural night field beside the sun disc, in BOTH background variants and nowhere else:
the env/IBL path must never carry it, the sun-disc firefly rule. Two octahedral lattices
(bright + faint wash) gathered 3×3 — one cell truncates at some zoom, since the core is
sized in PIXELS off `fwidth` while cells are fixed in angle. Hashed with its own PCG, not
`noise.glsl`'s sin-fract: cell ids are the integer lattice E10 measured at 1047 distinct of
4096, and the correlation rendered as visible strings of stars (E10's revival clause, on a
new consumer; the four existing consumers stand). The flux tail cap is the knob that bounds
the LARGEST halo — the glare wing scales with flux, and a hard wing cap flattens the glow
that is the look. `starIntensity` carries a CPU ramp through civil twilight (+3° to −8°) —
a visibility ramp standing in for the exposure adaptation `exposure_auto_gain` refuses, so
daylight is an exact 0 px; `starFrame` (latitude + hour angle) is what turns the sky.
OFF by default in the library (three gate arms assume a quiet sub-horizon backdrop), ON in
tree. Authored as `environment.stars` (a closed nested block), `--stars` /
`--stars-brightness` / `--no-stars` on render, four plain config rows. The `stars` gate
group reads every arm as a delta against a `--no-stars` twin — raw brightness moves the
OPPOSITE way as the sun drops — and its horizon arm's R/B leg is the load-bearing half.

## Tonemap / exposure

`tonemap_frag`, `lum_measure_frag`, `lum_histogram_frag`,
`lum_reduce_frag` (there is no `lum_adapt_frag`; the GPU adaptation pass and its 1x1 ping-pong
were deleted in `f98ab0a` and the blend moved to the CPU, where the value already had to arrive
for the readback). The metering chain is measure -> bin -> collapse since spec 11.52: a 64x64
R16F of log2 luminances, a 128-bin RG32F histogram carrying each bin's COUNT and SUM (laid out
128x8 and reduced by summing the rows -- `LUM_HISTOGRAM_ROWS` exists so one fragment is not
looping all 4096 texels alone), and a 1x1
the CPU reads back. A **gather** histogram, one fragment per bin looping the whole source, which
is O(bins x texels) and still the right trade at 4096 texels -- GL 4.1 has no compute, no atomics
and no imageStore, so the alternative is additive blending of point primitives

## Purkinje / scotopic shift

`include/purkinje.glsl` + `tools/gen_scotopic_weights.py`
(spec 11.83) — rod vision in dim light: colour drains, the image shifts blue, reds go
near-black. **It is NOT in the finishing stack**, which is entirely downstream of
`toneSelect`, and the roadmap's own row said otherwise for four specs. It splices into
`sceneToToned` between the `WS_SCENE_MAX` sanitize and the tone curve, on **the file's
THIRD siting rule**: the gamma line orders stages by whose data space they were authored
in and only rules out post-gamma, while chromatic aberration is already sited by the
OPTICAL CHAIN as a lens effect "before the sensor". This is the sensor's own spectral
response, so it lands after the lens and before the response curve — and grain, already
sited as sensor noise after that curve, is its other half. **Three wrong sites, each of
which renders a plausible frame**: `sceneTap` misses `bloomAdd`, so every night lamp gets
a fully-coloured halo round a drained core, and sits UPSTREAM of the sanitize where
`mix(c, INF*tint, 0.0)` is NaN; `toneSelect` is called by debug view 5, which would drain
for reasons nothing in the frame explains; and applying it to `color` after the call
makes the sharpen block high-pass the Purkinje term itself.
**The weight is a PRODUCT of two gates, and that is a measurement not a preference.** A
per-pixel gate on absolute radiance (via `oneOverPreExposure`, already in scope) and a
whole-frame gate on the metering 1×1, sampled on unit 7 — which is where its own ancestor
lived. Measured: the day frame's darkest decile sits at log2 −7.85, so **without the
global veto a noon shadow takes wLocal 0.997**, and the day frame's darkest half sits
BELOW the night frame's brightest third, so no per-pixel threshold separates them. Both
failure cases (a daylit shadow, a lamp at night) want a veto rather than a vote.
**This is why `postfx_measure_luminance` and `postfx_read_luminance` are separate**: the
readback stays gated on `automatic`,
the three draws run whenever the shift is on. `adapted_luminance` does not exist under
`--no-auto-exposure` — all 27 goldens and ~60 arms — so a term keyed to it would have been
structurally invisible to the suite, which is what the roadmap booked.
`PURKINJE_SCOTOPIC_W` is DERIVED (V′/V at each primary's dominant wavelength, normalised
to sum to exactly 1 so grey keeps its brightness) and anchored against the display white's
published S/P ratio at 2.36 vs ~2.4; `PURKINJE_ROD_TINT` is CHOSEN, with unit photopic
luma enforced, and is **algebraically identical** to Kirk & O'Brien's per-cone injection
rather than an approximation of it, since rods are monochromats and the LMS round trip of
a scalar collapses to one vector. Acuity and rod noise ship too, separately toggled.
**Every threshold is a LOOK constant**: this engine's whole day-to-night range is 4.2
stops where reality is ~17, and its sky is four decades under the real photometric scale,
so `purkinjeBiasEV` is the one knob that migrates them if 10.2 phase 5 ever lands.
OFF by default everywhere including `tree` (B15: its night sea is wrong, and a
desaturating model over it reads as a partial fix that has not happened). `--purkinje`,
`--no-purkinje`, `--purkinje-strength`, `--purkinje-bias`, `--purkinje-acuity`,
`--no-purkinje-acuity`, `--purkinje-noise`, `--no-purkinje-noise`, `--purkinje-debug`;
`post.purkinje {enabled, strength, bias_ev, acuity, noise}`.
**The 0 px daylight identity holds at the shipped bias, not at every bias** —
`--purkinje-bias 2` moves 396,713 px on a daylight frame, which is the flag doing its job.

## Specular occlusion

`spec_occ_composite_frag` + `include/spec_occ.glsl`
(specs 11.3, 11.4, 11.75, 11.76, 11.77). **Three** modes — off / legacy / split — default
`split`, and `POSTFX_SPEC_OCC_SPLIT` is **2**, renumbered when `bent` went.
**The default's term is computed in `gtao_frag`, not here** — the reflection lobe is tested
against the sector bitmask while that mask is still live, since it is loop-local and nothing
downstream can see it. Carried on a THIRD attachment of the GTAO FBO as the estimator's two
sums `(visible, total)`, RG16F at half res, divided only at the consumer: the accumulation, the
blur and the bilateral upsample all average this and the denominator varies per pixel, so a mean
of quotients is not the quotient of the means. It also hands the sweep's early-outs a true
neutral — `(0,0)` contributes nothing and leaves a neighbour's ratio alone. **Occluders use the
ROUND hit criterion and the lobe uses TOUCH**, which is not an inconsistency: an occluder is
asked whether it covers a sector, a lobe whether it reaches one, and a mirror lobe narrower than
one sector would round away to nothing and report the surface unoccluded. **Clamping the lobe's
sector range to the hemisphere IS the horizon reference** the cone form needed a second overlap
to estimate. **The cone is GONE** (11.77) — `specOcclusionCone`, `coneOverlap`,
`VIS_EDGE_SOFTNESS` and `TRUST_OPEN` with it, once `--spec-occ bent` was retired and took
their only call site. 11.76 kept them on the argument that a term with no live alternative
cannot be falsified; that was wrong, because `ao-ring` never ran the mode — the cone was
falsified against the pre-change renderer through git. **Keeping a bad implementation
reachable is not the same as keeping a comparison**, and only one of those is worth a mode.
The half-res roughness the sweep reads (`linDepthTex.w`) is the one thing the move gives up;
the depth-bilateral upsample cannot help at a roughness edge on flat ground.
**The bent NORMAL is a different thing and still ships** — written by `gtao_frag` into
`AoOut.gba`, carried by the blur/accumulation/upsample, read now by debug view 9 alone. It
costs a 64-iteration-per-pixel loop the shader's own comment flags as its one real ALU spend
for it. Whether that survives is an open question wanting a MEASUREMENT, not the roadmap's
booking of it for SSGI directionality / SSR occlusion / DDGI sampling — the same booking
reasoning 11.76 falsified for the specular case

## IES profiles

`include/lights_ubo.glsl`'s `IesBlock` + `iesProfile` (spec 11.57) — the
measured angular distribution of a real luminaire (IESNA LM-63), which **REPLACES** a spot's
analytic cone rather than multiplying it. Full 2D: rotational, quadrant, bilateral and fully
asymmetric all ship, because the roadmap deferred the asymmetric case by pricing a 2D table in
SAMPLER units and a UBO block costs bytes. Ceiling 32 vertical x 16 horizontal taps, two
independent limits (8 descriptors, 3968 shared pool floats = seven fully asymmetric profiles or
~125 symmetric ones), **its own block at binding 6** because the per-frame cluster upload orphans
its buffer and rewrites only the live light prefix -- a table sharing `LightsBlock` would be
undefined every frame. Per-light index rides `attenCutoff.y`, `-1` = none, matching
`shadowMisc.y`'s convention at zero bytes.
**The table is NORMALISED to peak 1 and multiplies `intensity`**, and the loader seeds `intensity`
from the file's own peak candela when the scene authors none -- so it is absolute by default
(normalised x peak IS absolute) while `intensity` stays the one brightness control and
`gates.py`'s `_scale_emitters` stays true. **The tail tap is exactly 0**, which is a contract:
`pbr_frag` skips nine shadow taps and the whole GGX chain on `attenuation <= 0.0`.
Note the tail is exact because a **snap** makes it so, not because files carry zeros:
`ies_tinytail.ies` is the only fixture whose 90-degree candela is nonzero (1.2e-6 cd), and
before it existed deleting that snap from both readers left every arm green.
**A partial horizontal sweep MIRRORS, it does not repeat** -- a bilateral file measured 0..180
describes 190 degrees as 170, not 10 -- and that fold exists three times (`iesFold`,
`ies_fold_horizontal`, `tools/gen_ies_table.py`). They cannot share a token: `mod()`, `fmodf`
and `%` disagree on negative input. **Each copy is held by a different thing, and for a spec
cycle two of the three were held by nothing** -- this file and both source comments claimed
`--ies-probe` held all three, which was false in a specific and instructive way. The probe
samples AT the taps, deliberately, and every tap lies inside `[0, span]` where a mirror and a
modulo agree exactly; so did both of the C fold's call sites. Now: the tool's copy is asserted
directly by `gen_ies_fixture.py`, the C copy by the probe's **`mirror` rows** (which ask for
`span + d` on purpose), and the GLSL copy -- the one that decides pixels -- only by a rendered
frame, which is what `ies-mirror` is. Replacing it with a modulo moves that arm from 0.0055 to
0.9861 and moved nothing else in the suite.
Outside the file's measured vertical range the profile is **zero, not the
clamped edge**: a file measured 0..90 stopped there because the luminaire emits nothing above it.

## Contact shadows

`contact_shadow_frag` — a full-internal-res depth march toward the key
directional AND toward every clustered point/spot light with **no punctual shadow map**
(spec 11.56). That second population is the larger one and is why the pass exists at all
now: the punctual atlas holds 8 layers and a point light spends 6, so past the FIRST point
light in a scene there is no map to be had, and every 11.49-derived panel is ineligible
because `create_light` defaults `cast_shadows` false. A light that DOES have a map is
skipped, which is a correctness requirement and not a budget — its 3x3-PCF perspective map
already resolves the contact (9.8's hairline was far-side depth STORAGE, fixed in
10.3/10.4; the map is 2048² at 2-6 punctual layers, 4096² for a lone spot, 1024² at 7+),
so marching it too would darken the seam twice. **Area panels are skipped from MARCHING**
-- a direction exists, but `pbr_frag` shades a panel through an LTC integral over its whole
area, so one ray at its centre is a DIFFERENT approximation, worse than none.
The per-light visibilities fold into the one R8 channel **weighted by each light's
contribution** -- radiance x `getDistanceAtt` x N.L, the fraction of direct light the pixel
loses. **The denominator is every light that REACHES the pixel, not every light marched**:
a mapped light and an area panel still deliver light here, and leaving them out made one
blocked practical beside a bright mapped spot take 23% off a pixel that should lose 1%.
Reads the cluster list through `#include "lights_ubo.glsl"` with **no C-side binding work**:
`create_post_program` links through `ubo_wire_blocks` against buffers bound for the
context's lifetime, which `froxel_inject` has relied on since 9.5.

## Water

`water_vert/frag` + `water_spectrum_frag` + `water_fft_frag` +
`include/ocean.glsl` (spec 11.32). The two spectral passes are a Tessendorf ocean run
as **fragment ping-pong** -- 3 cascades x (1 evolve + 14 Stockham stages) = 45 draws at
128² per frame under `--water-waves fft`. That works because a Stockham butterfly is a
pure gather (two texels plus a twiddle row, one texel out to two MRT targets) with no
shared memory, atomic or scatter, so GL 4.1's missing compute stage costs draws rather
than a redesign. **And the cost lands on CPU, not GPU**: +0.24 ms GPU against +1.19 ms
CPU at 800x500, because 45 dispatches become 45 framebuffer binds and uniform groups.
Deterministic regardless -- two runs of 45 ping-pong passes diff 0 px. The include is the
surface evaluation -- position AND analytic normal from one place, so a normal cannot
be derived from a different height than the raster used. Its own program rather than a
`pbr_frag` feature because it samples the resolved scene depth, and `pbr_frag` has
declared all sixteen fragment samplers the driver allows.
Since spec 11.33 the include also owns the PREVIOUS position (`oceanPreviousWorld`), so
the spectral path reports the motion its waves have rather than camera motion only --
which needs last frame's transform kept alive, and the 14 stages land back in buffer 0,
so one copy out of it before the evolve overwrites it is the whole mechanism. The
position arithmetic is factored (`oceanSpectralPosition`) for the same reason the file
exists: current and previous must be the same sum, or the velocity describes a surface
the raster never drew. `procedural/water_waves.c` is the CPU twin of the Gerstner half
and duplicates its constants -- **nothing compares the two**, which is the known gap.
Spec 11.42 adds `water_foam_frag` (one more pass a frame: whitewater accumulated per
cascade texel so it outlives the crest that made it, three bands in one RGB target),
a Cox-Munk sun lobe and a spectrum-derived far-field roughness inside `water_frag`, and
the seeded spectrum's own **height and slope variance**, which is what the last two are
built from. The variance is checked rather than asserted -- see `--water-fft-probe`.
**The water program hit 16/16 declared samplers in 11.42 and 11.45 took it back to 11**, by
folding six identical RGBA16F cascades into one `sampler2DArray` -- five units back for a
change that alters no read. Its units are now **0 the cascade ARRAY, 1 the previous ARRAY**,
2 foam pattern, 6 scene colour, 7 depth, 8 bed, 11 shadow array, 12-13 the IBL pair, 14 cloud
shadow, 15 foam (`water.h:47-90`). Two separate arrays rather than more layers of one, because
the two have different LIFETIMES -- the fields are this frame's render targets and the previous
are a copy taken before they are overwritten. 11 is deliberately not `SHADOW_MAP_TEXTURE_UNIT`
10, because two sampler TYPES against one image unit is an `INVALID_OPERATION` at draw.

## Atmosphere

`froxel_inject/integrate/composite_frag` + `include/froxel.glsl` (fog, a 3D
volume, spec 9.5; the screen-space `fog_frag` march it replaced is deleted) and
`aerial_lut_frag` (spec 9.6). `froxel_composite_frag` folds BOTH media into one
`(inscatter, transmittance)` pair, since the blend carries only one; either can run alone.
**`froxel_inject_frag` is where every medium meets** and now carries four: the global height
fog, a submerged water body (11.33), local volumes and the cloud deck's shadow (both 11.39).
Only the first is `fog_enabled`; the rest arm the pass by joining the union at the gate, because
that flag belongs to the app and the GUI and is never cleared per frame. Two media sharing a cell
combine **σ-weighted**, not added — the integrate pass reads the rgb channel as the medium's
source function rather than as radiance already scaled by how much medium is there, so adding
would count the light once per medium.
**Since 11.78 the composite resolves its fold at TWO depths, not one.** The aux attachment holds
one linear Z per pixel and the late pass never writes it — `engine_set_scene_draw_buffers(engine,
false)`, called from `engine_end_oit_pass` (`engine.c:2099`), drops the draw-buffer
count to 1 when the opaque scope closes, so the write `pbr_frag` still performs is discarded by
the buffer LIST, not by the depth mask — and a translucent surface was therefore fogged at the
depth of the wall behind it. The MBOIT moments already measure what aux cannot: `b0` is the
translucent stack's total absorbance along the pixel and `b1/b0` the absorbance-weighted mean of
its warped depth, so one atlas fetch gives both how much of the pixel is translucent and how far
off that part of it is. `mediumAt()` is called at the surface's depth and at the stack's, and the
two `(inscatter, transmittance)` pairs are mixed by `1 - exp(-b0)`. **The mix is the exact split
when the layers carry the background's own radiance and when coverage is 0 or 1, and an
approximation between** — the exact form wants the un-composited translucent colour, and the
accumulation buffer holding it is upstream of the TAA resolve this pass deliberately runs after.
`--no-oit-moments` carries no depth statistic and keeps the single-depth composite exactly, which
is the bisect lever. Costs no sampler unit anywhere: the post chain has its own ledger and this
is its fifth declaration of sixteen.

## Cloud shadow

`cloud_shadow_frag` (spec 11.39) — a 256² R16F sun-transmittance map marched
through the deck, built by `sky_clouds_march` from the SAME wind offset so the shadow cannot sit
a frame from the deck it belongs to. Read by shearing a froxel up to the shell, which is exact
rather than approximate for a horizontal layer, and tiled by `GL_REPEAT` because with detail off
the density field is periodic over the shape noise's own 8 km. World-anchored while the deck is
camera-anchored, deliberately: a dapple you can never walk out of is wrong in a way the sky's own
anchoring is not.

## Clustered specular probes

`probe_project_frag` + `include/probe_specular.glsl` (spec
11.70) — the first resamples one roughness level of a probe's prefiltered cube into one
octahedral atlas row (no second GGX importance sampler: `ibl_prefilter_cubemap` already ran
the tested kernel, and row r reads mip r); the second owns the blend, the box weights and the
atlas addressing. **Its sampler is a function PARAMETER**, which GLSL 330 allows and which is
the whole reason the lit surface and SSR share one weight formula across two programs with
different unit ledgers — pbr_frag passes `giAtlasTex`, ssr_frag its own. The row geometry is
NOT recomputed here: the CPU publishes a table, because every probe's column has the same
rows and a shader-side copy of the halving rule would force the row size to be a power of two
(C shifts, GLSL would divide by exp2, and those agree nowhere else).

## Clustered decals

`include/decals_ubo.glsl` (spec 11.73) — marks projected through an
oriented box onto whatever surface lies inside it, selected by a per-froxel 16-bit mask on
the same grid the lights and probes use. Declares **no sampler**: a decal image is a tenant
of `materialArray`, so the atlas arrives as a function PARAMETER exactly as the probe atlas
does. **Spliced into `pbr_frag` in TWO halves and the split is not tidiness**: the
accumulation and its albedo land above the `renderMode == 6` return, because that view is
what a gate reads painted bytes through and a mark invisible there could not be asserted as
an exact code; the normal, roughness and occlusion land at the wet-sand seam, where those
three finally exist and where modifying them in place is already precedented. Decals apply
AFTER the wetness — a poster covers the water rather than being soaked by it — and after
`sampleLayeredSurface`, which is why the composite cache and the VT bake need no change at
all, unlike roads. The loop's trip count is the CAP, broken on a dynamically uniform count,
so a scene with no decals takes one branch; the moment and depth-only passes skip it, a mark
changing neither coverage nor depth. **The facing test is what stops a projector smearing its
image down every wall it grazes**, and it reads the GEOMETRIC normal, since relief answering
"which way does this surface lie" would let a normal map punch holes in a poster.
**It takes its own derivatives rather than sharing `ddxWorld`, and that is measured**:
hoisting that pair up to serve both cost **11% of the opaque pass on scenes with no decals in
them**, against 3.1% for the whole feature — two vec3s live across the shader body is register
pressure every fragment pays whether or not it takes the branch, where recomputing them is two
instructions. `prepass-crossover` is what caught it, reading the prepass as a RATIO of the
opaque pass, so a uniformly heavier `pbr_frag` shrinks it — and it failed on MASTER in the same
session at a different noise floor, so the reading had to be attributed before it could be
acted on.

## Layered surfaces

`include/layers.glsl` + `include/triplanar.glsl` (spec 11.60) — N material
layers blended per texel from a splat map, height-weighted so gravel interlocks with sand rather
than averaging into mud, sampled through a WORLD-ALIGNED projection because a ground plane's UV is
top-down and stretches by 1/cos(slope) on exactly the slopes worth looking at. Declares **no
sampler**: the layers are tenants of `materialArray`. Gated on `layerCount`, which is 0 for every
material that has not asked, so the whole path is an exact identity and all 24 pre-existing
goldens are 0 px. **The splat's coordinate SPACE is a material field, not a policy** — world XZ
cannot address a vertical surface and a mesh-local reading makes the weights swim across a moving
prop, and shipping UV1-only made the feature inert on terrain, which writes UV1 as a literal zero.
Two things in `triplanar.glsl` are correctness rather than tuning: the cutoff is floored against
the largest weight (a fixed one collapses past sharpness 10.05 and returns a **NaN normal** into
the HDR target), and every tap is `textureGrad` (the skips are fragment-varying flow, where an
implicit-LOD fetch is undefined in GLSL 3.30 — the same rule `pbr_frag` and `stochastic.glsl`
already follow)

## Roads

`roadReshapedWeights` in `layers.glsl` + `include/roads_ubo.glsl` (spec 11.68) — a
world-XZ polyline that OVERRIDES the splat weights toward one of the material's own layers,
capsule SDF, mask `smoothstep(halfWidth, halfWidth + feather)`, applied **before the height
blend**. Before is the whole design: a road blended in afterwards is a stripe painted over the
ground, where one reshaped here is MADE of a layer and its shoulder height-interlocks with what
it runs over — measured as a 0.166 shoulder-width ratio against its own linear twin, where
after-the-blend reads exactly 1.000. The bake inherits roads by calling `sampleLayeredSurface`
UNCHANGED, so the fallback, the pages at 4x, the per-texel path, the dominant index and the
detail term all follow with **no bake code** — and `--no-layers-vt` stays lossless, which
drawing into the atlas FBO would have cost. Positions ride `authoredPos`; the lane write is a
constant-lane `equal()` compare, not `w[L]`, because that would be the dynamic local index the
file's own header rule forbids. Segments live in a UBO on binding 8 (`GpuRoadsBlock`, 1104 B —
geometry small enough for uniform space costs zero samplers, the IES lesson a fourth time), and
the whole path is a structural no-op at road count 0, which every scene but two is.
**`material_roads_ensure` (`roads.c`) is the one owner of the armed state** and arms ONE
road-bearing material per scene, refusing a UV1 splat outright rather than warning about it.
Roads join `MaterialLayersVtKey` as the struct itself, so a width change re-bakes the cache and
a field added to `MaterialRoad` joins the key by construction. Authored per material as
`roads: [{points, width, feather, layer}]` — the scene format's only array of arrays, where a
malformed vertex refuses the whole road, and `CSceneRoad` deliberately does not exist: an
authored road IS a `MaterialRoad`, so the applier assigns rather than copying field by field.
`roads_polyline_distance_xz` is the CPU twin the scatter rejects against; the two cannot share
a token and the gate arms are what hold them together.
**The caps are small and are the first thing to check before planning against this**: 4 roads
of 16 points, one road-bearing material per scene, evaluated per fragment on the per-texel path
(the default path is the cache, which pays nothing — roads reach it through the bake). A road
NETWORK wants the paged content era, not a hundred polylines.

## Composite cache

`layers_vt_bake_frag` + `sampleCachedSurface` in `layers.glsl` (spec 11.66,
D10 stage 1) — a WORLD-XZ-splat layered material's blend baked once into two 2D textures over its
splat domain (the MACRO: which layer wins where, with every layer at its top-mip mean via
`layerGrainFrozen`, so grain never lives in the atlas at any resolution), read back at `3 + 2A`
taps against the per-texel path's 9/17/25: two filtered macro taps, one `texelFetch` of the
dominant-layer index riding the atlas alpha (NEVER filtered — bilinear between indices passes
through every index between them), and one triplanar detail tap of that layer's own maps whose
albedo is a RATIO to the layer's mean (exactly 1 on a flat map, which is what makes the flat case
byte-exact) and whose roughness/AO are additive deviations. The pair rides units 0/1
(`albedoTex`/`normalTex`, provably unread when layered) and is deliberately NOT a `materialArray`
tenant — the canonical-size rule would promote every other layer to atlas resolution. Derived
res: half a unit per texel, clamped 256..2048 (forest: 2048², 42.7 MB); `--layers-vt-res` is the
diagnostic override, `--no-layers-vt` the bisect lever, `--layer-blend-at <frame:value>` the one
headless way to make the by-value bake key go stale (a fresh process always bakes from final
authored values — the `--shadows-off-at` reasoning exactly). UV1-splat materials never take the
cache, which keeps the per-texel path the general mechanism and `layer_fixture`'s four arms its
coverage.
**Spec 11.67 paged it**: a 64-slot guttered page atlas (256² tiles at 4x the fallback's density
— a RATIO, which bounds the virtual grid at 34² for every domain size), its page table a UBO on
binding 7 (`vt_pages.glsl` + `layers_vt_pages.h`, the IES tables-cost-no-samplers lesson), the
page pair riding units 3/4 (`clearcoatNormalTex`/`heightTex`, freed by an IES-style REFUSAL: a
layered material refuses both maps — the height half independently a bug fix, since POM ran a
dead march on layered surfaces that could discard). Residency is frustum prediction sorted
(seen, distance, id) with the capacity clamp as governor, an N-bakes-per-frame budget and
hysteresis-guarded farthest eviction, all owned by `material_layers_vt_ensure` on frame N−1's
pose. `layers_vt_feedback_vert/frag` is the **vote pass**: paged surfaces rasterize page IDs
into a small depth-tested target, read back through a **fixed-latency PBO ring** (always the
slot from 4 frames ago, never "whichever fence signalled" — content stays a pure function of
frame history, so every arm is deterministic; measured never stalling at R=4, the whole stage
+0.23 ms GPU). On today's content pages are a measured 0 px identity — the macro's finest
content is the splat the fallback already over-resolves — so their content era is roads/decals
drawn INTO pages through the bake FBO. Flags: `--no-layers-vt-pages` (stage 1 exactly),
`--no-layers-vt-feedback` (prediction alone), `--layers-vt-page-slots` (the churn knob),
`--layers-vt-page-budget` (bakes per frame, capped at 8), `--layers-vt-probe` (residency
counters + digest), and `--cam-at <frame:ex,ey,ez,tx,ty,tz>` (the teleport — the worst case
no walk can produce)
