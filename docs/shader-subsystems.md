# Shader subsystems

How each shader subsystem works and why it is built that way. `docs/shaders.md` keeps the
inventory -- which files exist and what each one is for -- and the rules that span files; this is
the per-subsystem detail behind those entries. `docs/lighting.md` and `docs/architecture.md` hold
the engine side of several of these subsystems.

**Read the entry before changing its subsystem.** Most of what is recorded here is a rejected
alternative or a failure mode that renders a plausible frame: a moon phase that inverts and still
looks like a moon, an IES fold that lights a room the wrong way round, a probe fade that weighs
every floor at zero, a road blended after the height instead of before. None of those produce an
error, and several pass the full gate suite.

**Names, counts, line references and file lists rot faster than the reasoning.** Treat a
`file.glsl:123` here as a starting point rather than a fact, and trust the argument further than
the address.

## Contents

Sky and night: [Day/night cycle](#daynight-cycle) · [The moon](#the-moon) ·
[Night floor](#night-floor) · [Overcast](#overcast) · [Sky scale](#sky-scale) · [Stars](#stars) ·
[Cloud shadow](#cloud-shadow) · [Atmosphere](#atmosphere)

Image finishing: [Tonemap / exposure](#tonemap--exposure) ·
[Purkinje / scotopic shift](#purkinje--scotopic-shift) · [Diffraction glare](#diffraction-glare) ·
[Local exposure](#local-exposure) · [Film grain](#film-grain) · [The CRT](#the-crt)

Lighting and occlusion: [Specular occlusion](#specular-occlusion) · [IES profiles](#ies-profiles) ·
[Contact shadows](#contact-shadows) · [Cascade soft shadows](#cascade-soft-shadows) ·
[Area-light shadows](#area-light-shadows) ·
[Cached shadows](#cached-shadows) ·
[Clustered specular probes](#clustered-specular-probes)

Surfaces: [Water](#water) · [Clustered decals](#clustered-decals) ·
[Layered surfaces](#layered-surfaces) · [Roads](#roads) · [Composite cache](#composite-cache)

Weather: [Rain](#rain)

An app's own GLSL: [Shader hooks](#shader-hooks)

---

## Day/night cycle

`sky_cycle_tick` (spec 11.81), called once per frame from `engine.c` before the GI sweep and the
shadow pass: the key light it rewrites is what the cascades fit, and a completed swap must land
before the frame's first `bind_ibl_textures`. An app render callback is too late for both.

**Two halves.** `sky_cycle_advance` moves the clock, the sun and the live view LUT;
`sky_slicer_pump` spends a frame's budget of the sliced re-bake. They are separate because
`--cycle-rebake-at` pumps without advancing and a frozen cycle advances without pumping.

**One clock.** `cycle_hour` (0-24, noon at 12) is a `double`: it is an accumulator, and a float32
ulp near hour 24 is half a per-frame increment. It drives both the sun (through `sky_sun_path`, the
equinox path on the stars' own latitude frame) and `stars_hour_deg`, so sun and stars wheel off one
clock.

**The cost is not where "slice the bake" suggests.** The six env faces are about 10 ms, and about
90% of that is the GGX and Charlie prefilter chains. So the schedule is texel-weighted work items,
mip 0 split by face, with the budget just under the heaviest item so those run alone. The per-frame
peak is one mip-0 face (4.74 ms at 800x500 under the `sky cycle` profiler scope) and cannot fall
without slicing below a face.

**The swap.** Slices render into shadow textures and swap all four handles plus
`max_reflection_lod` at once, adopting the outgoing chain rather than reallocating. The tick
returns whether a swap landed and `engine.c` marks the GI volume, because `sky.c` must not know
what a Scene is. **An atomic bake cancels a slice in flight**: otherwise a GUI floor edit or a
config restore is silently reverted a few frames later by content baked before it.

**Reached by:** off by default everywhere; `--day-cycle <s>` (0 = frozen clock),
`--time-of-day <h>`, `--no-day-cycle`, `environment.cycle {enabled, day_seconds, hour}`, three
config rows, and `--cycle-rebake-at`, the diagnostic the `cycle-slice` arm runs on.

## The moon

`include/moon.glsl` and one guarded block in `include/sky_radiance.glsl`, over the surface
`moon_surface.c` bakes (spec 11.82). It follows the sun disc's pattern, in the two background
variants and nowhere else, so the LUT chain, the slicer and the bake cadence know nothing of it.

**Phase is derived, never authored**: `cos(alpha) = -dot(moon_dir, sun_dir)`, negative because
alpha is measured at the moon (0 at full, 180 at new). With the sign wrong the feature inverts and
still renders a plausible moon, and the two readings agree at elongation 90, so a test ladder that
does not straddle 90 cannot see it. The terminator is `dot(n, sunDir)` crossing zero in a disc
frame whose sunward axis is the projected sun, so a crescent facing the wrong way cannot be
represented: there is no sign to invert and no parameter to mis-author.

**A full moon is not a Lambertian sphere**, and that is its visual signature. Regolith
backscatters, so the quarter moon is about 1/11 of full rather than 1/2 (Krisciunas & Schaefer
1991), and the lit face is flat with no centre-to-limb gradient. Shading it by `cos(theta)`
renders a snooker ball. Hence a soft step terminator one pixel wide off `fwidth`, and `MOON_LIMB`
0.85 against the sun's 0.4: same form, opposite intent.

**Shading is Lommel-Seeliger**, `mu0/(mu0+mu)`: at full phase the two are equal everywhere so the
disc reads flat, and near the terminator it falls off slowly enough that crater walls throw their
own shade. One term gives both halves of the signature.

**Size.** The physical disc size is `sun_disc_deg`, shared with the sun: the two subtend 0.53 and
0.52 degrees, and a second field would have cost six authoring surfaces for about 1/50 px. The GUI
slider is "Disc Size" for that reason. `moon_size` multiplies it for the look, since a life-size
moon is a dozen pixels. The aureole's width divides by its square root rather than tracking it: a
halo scaled linearly onto a 12x disc is 50 degrees across and floods the star field the moon hangs
in front of.

**The surface is baked, and carries relief and albedo both.** `moon_surface_bake` stamps about
43,000 craters over three size tiers into one 2048x1024 RGBA8 at startup (about 0.09 s debug): RGB
the tangent-space normal, A the albedo, threaded on `cetra_bake_bands` and bit-identical at any
worker count. It is baked rather than evaluated per pixel for a population ceiling, not cost: a
3x3x3 lattice holds about 27 candidates per octave, and no tuning reaches a saturated field from
there. Three things about it are load-bearing:
- **An impact excavates.** The stamp erases prior relief inside the rim and piles it at the crest,
  in crater order, so a young crater cuts a clean notch through an old one. Summing bumps averages
  the overlap into mush, and the overlap is what the eye reads as a crater field.
- **The normal is tangent-space**, so a coarse mip averages toward flat. In the body frame it
  averages toward the mean of the sphere's normals, which is zero, and a distant moon turns to
  noise.
- **Craters are height and albedo**, because relief is invisible at full phase under
  Lommel-Seeliger and albedo is all there is. A moon of relief alone renders a blank disc at the
  phase people picture.

**The maria come from data** (`moon_map.h`, 32 KB, generated by `tools/gen_moon_map.py` from
published selenographic centres), in a face-locked frame. Indexed by the world normal they swim;
indexed by the terminator's sun-relative frame, the natural wrong turn since that code builds such
a frame anyway, they spin. Procedural noise gives a plausible cratered world that is not the Moon:
where the seas are has no generating process to model. Only the low-frequency layout is data;
craters, rays and grain stay synthetic, which keeps the face sharp at any `--moon-size` where a
photograph would blur. Earthshine lights the dark limb, blue because it is light off an ocean
planet.

**The light** is a second `LIGHT_DIRECTIONAL`, created whenever a sky sun is, whether or not the
moon is enabled: gated on enabled, `sky.moon` would be a snapshot row that restores a disc and no
light, since a restore cannot create a Light. It costs nothing when disabled, because `shadow.c`'s
classification skips non-casting lights before counting directionals. `cast_shadows` rides
`sky_night_factor`, an exact zero above +3 degrees, so above +3 the moon never casts and the
daylight caster count is unchanged. The window where both cast is (0, 3) degrees, open at both
ends: `sky_night_factor` is exactly zero at +3 and `sky_horizon_fade` exactly zero at 0. That is
17 scene-minutes, at most 7 s at the GUI's longest day.

**Under the cycle**, `moon_hour = cycle_hour - cycle_moon_offset`, the lag advancing 24 h per
29.53 simulated days: one rate behind two facts, since rising about 50 minutes later each day and
the phase evolving through the month are the same fact. The offset is left unwrapped:
`sky_sun_path` takes sin and cos of the hour, and -5 h and 19 h agree mathematically, not bitwise.

**Reached by:** off by default in the library, on in tree (`--no-moon` off); `--moon`,
`--moon-brightness`, `--moon-size`, `--moon-elevation`, `--moon-azimuth`, `--sky-disc`,
`--no-moon-maria`, `--no-moon-glow`, `--no-earthshine`, `--moon-probe`;
`environment.moon {enabled, brightness, size, elevation, azimuth}`. There is deliberately no
`phase` key: one beside a position could author a full moon next to the sun.

## Night floor

One term at the end of `sky_view_frag.glsl`'s march (spec 11.80): airglow, zodiacal light and
integrated starlight as a premultiplied uniform, added to sky-side texels only and dimmed by the
ray's own `through`.

**It lives in the LUT bake, and that is the whole design.** The background, the env cube (so the
IBL and every lit surface) and the cloud march's ambient all sample that LUT, and it re-bakes
exactly when the ramp's one input, the sun, moves. A floor in the background shader instead lifts
the sky and never the ground; that is the `nightfloor-ground` arm's named mutation. The CPU zenith
march adds the same term before its below-horizon early-out (the night fog ambient).

- **Constants are C-side only**, in `sky.c` (`SKY_NIGHT_FLOOR_*`): a premultiplied uniform leaves
  no GLSL copy to keep in step.
- **The tint rides at the stars' 0.35 saturation**: full extinction browns the whole lower sky at
  any level.
- **Night is one definition**, `sky_night_factor` (+3 to -8 degrees), shared with the stars.
- **The level is a look constant, not physics**: exposure only darkens, so the floor stands in for
  the dark-adapted eye.

**Reached by:** off by default in the library, on in tree (`--no-night-floor` off);
`--night-floor` and `--night-floor-brightness` on render; `environment.night_floor {enabled,
brightness}` in a `.cscn`. The GUI checkbox and slider ride the sun's re-bake chain, not the stars'
live-uniform path.

## Overcast

`include/sky_emission.glsl`, with `sky_emission_at` and `sky_bind_emission` in `sky.c` (spec 13.7).
Named for what they hold rather than for the overcast: they carry the radiance scale too, and "deck"
is the volumetric cloud layer's word in `clouds.glsl`.

**The model is the CIE standard overcast sky,** L = Lz(1 + 2 cos θ)/3. Lz is Krochmann's zenith fit,
(8.6 sin h + 0.123) kcd/m², carried onto the sky's scale by `SKY_SUN_ILLUMINANCE / SKY_SUN_KLUX`.
Its horizon constant fades on the night ramp, so an overcast evening darkens with the cycle.

**The model lives in C, and the shaders only apply it.**
- `SkyEmission` = {clear, sun_scale, dome_zenith, dome_floor, scatter_max} is computed once, from
  the sun the bake is for. The slicer bakes from a latched sun, and a dome read from the live one
  would seam the env faces.
- `sky_bind_emission` is its only upload: to the sky-view LUT, the aerial volume, the env faces and
  the cloud march.
- The dome terms arrive premultiplied by the amount, so a clear sky adds exact zeros and multiplies
  by exactly 1. No branch guards the default path, and none should: a branch around unchanged
  arithmetic is not byte-identical on this driver.

**One weight:** the sun's scatter, its disc and its light (and with it the shadows), the stars and
the moon disc all fade by the overcast's clear fraction. The night floor does not, because cloud at
night reflects a town.

**Four places carry the dome, and each has a named asymptote:**
- **The LUT.** Sky rays go toward the curve, and ground rays toward the floor it lights,
  GROUND_ALBEDO·(7/9)·Lz.
- **The virtual ground.** Its overcast share is linear in the amount, weighted once.
- **The aerial volume converges on the horizon, Lz/3,** the sky drawn right behind a far surface.
  Any other asymptote seams, and fading the sun's share alone takes distance to black.
- **The CPU zenith march** adds (7/9)·Lz: the dome's horizontal irradiance over π, which is what
  the fog ambient and the water read it as.

**Reached by:**
- `--overcast <0..1>` and `--sky-probe` (Lz, the zenith radiance, the scale, both bodies' lights)
  on render;
- `environment.overcast` in a `.cscn`, and `sky.overcast` in a config snapshot;
- a GUI slider on the sun's re-bake chain.

Scene-captured probes and probe sets keep the sky they were captured under. The `overcast` gate
group holds the dome's level, shape and zero against the probe's Lz.

## Sky scale

`SkyAtmosphere.radiance_scale` K (spec 13.7): nits per unit of the sky's relative scale.

**Why it exists.** On the relative scale a noon zenith is a couple of units, about four decades
under a real sun, so a sky scene and a photometric lamp cannot share an exposure. At
`SKY_PHOTOMETRIC_SCALE` (`SKY_SUN_KLUX`·1000 / `SKY_SUN_ILLUMINANCE` = 42,500) an unattenuated sun
delivers 127.5 klux.

**K is applied where things are baked, never where they are read.** Both alternatives fail:
- folded into the IBL intensity, it counts twice in GI and in scene-captured probes, which already
  see the scaled sky;
- stored relative and scaled at read, it sinks the night floor into fp16 subnormals.

**So it rides `SkyEmission`, and everything the sun drives is stored absolute:** the LUTs, the
dome, the cloud march, the disc (20·K), the zenith march and the sun light
(`sun_base_intensity × K`).
- **The sun's scatter is held under `100·K`,** 100 at K = 1: the key light carries the sun's
  energy, and the IBL is not meant to.
- **Storage is a separate bound, `SKY_STORE_FP16_MAX`, applied at every sky target's write.** A sum
  of bounded terms is not bounded -- the dome, the floor and a cloud composite are all added after
  the scatter ceiling -- so one clamp on one term could not keep RGBA16F from overflowing.
- **The background clamps in working space, after the exposure,** because a photometric disc is
  850,000 nits.
- **K is in the sun light's full level, not in its fade,** so the cast floor (a thousandth of the
  full level) scales with it. As a fade it would keep a photometric sun casting at 2.4e-8 of its
  light.

**The night terms are absolute and do not take K:** the floor, the stars, the moon disc and the
moon light. So a night frame is the same on either scale, and on the photometric one twilight sits
K above the floor, where a meter can open for it.

**The sun's base.** `sun_base_intensity = SKY_SUN_ILLUMINANCE` is the value at which the sun agrees
with the sky. The render app keeps its 10, which is 3.3 times sun-heavy on either scale. The moon's
light is a fraction of the same base without the scale, so an app that zeroes the base loses its
moonlight too.

**K is a change of units, not of look.** A scene-captured probe is shot once, at load, so a K that
arrives after it leaves every reflection of the sky tens of thousands of times off. That is why the
config snapshot carries K in its `source` block, applied before the load, as well as in the
`sky.radiance_scale` row. The GUI's live checkbox leaves such probes as shot and says so, as an
overcast drag does.

**Reached by:** `--sky-scale <f|photometric>`, `environment.sky_scale` (a number or
"photometric"), the snapshot's source block and `sky.radiance_scale` row, and a GUI "Photometric"
checkbox. The flag and the file refuse a value outside `SKY_RADIANCE_SCALE_MIN..MAX`, and
`sky_radiance_scale`, the accessor everything downstream reads, clamps whatever is stored to the
same range. The `scale` group's `sky` entry holds it invariant at x1000, to 1 LSB.

## Stars

`include/stars.glsl` and one term in `include/sky_radiance.glsl` (spec 11.79): a procedural night
field beside the sun disc, in both background variants and nowhere else. The env and IBL path must
never carry it, for the sun disc's reason: a point that bright in a prefiltered cube is a firefly.

- **Two octahedral lattices**, bright stars and a faint wash, each gathered 3x3. One cell
  truncates at some zoom, since a star's core is sized in pixels off `fwidth` while cells are
  fixed in angle.
- **Hashed with its own PCG, not `noise.glsl`'s sin-fract.** The cell ids are an integer lattice,
  on which the sin-fract hash collapses to few distinct values (see `docs/shaders.md`, Hashes), and
  the correlation renders as visible strings of stars.
- **The flux tail cap bounds the largest halo.** The glare wing scales with flux, and a hard cap
  on the wing instead flattens the glow that is the look.
- **`starIntensity` carries a CPU ramp through civil twilight** (+3 to -8 degrees): a visibility
  ramp standing in for the exposure adaptation `exposure_auto_gain` refuses, so daylight is
  exactly untouched. `starFrame` (latitude and hour angle) is what turns the sky.

**Reached by:** off by default in the library, since several gate arms assume a quiet sub-horizon
backdrop; on in tree. `environment.stars` (a closed nested block), `--stars`, `--stars-brightness`
and `--no-stars` on render, and four plain config rows. The `stars` gate group reads every arm as a
delta against a `--no-stars` twin, because raw brightness moves the opposite way as the sun drops;
its horizon arm's R/B leg is the load-bearing half.

## Tonemap / exposure

`tonemap_frag`, `lum_measure_frag`, `lum_histogram_frag`, `lum_reduce_frag`. There is no adaptation
pass on the GPU: the blend runs on the CPU, where the value already arrives for the readback.

**Metering is measure, bin, collapse** (spec 11.52):
- a 64x64 R16F of log2 luminances;
- a 128-bin RG32F histogram carrying each bin's count and sum, laid out 128x8 and reduced by
  summing the rows (`LUM_HISTOGRAM_ROWS`, so no one fragment loops all 4096 texels alone);
- a 1x1 the CPU reads back.

The histogram is a gather, one fragment per bin looping the whole source: O(bins x texels), and
still the right trade at 4096 texels. GL 4.1 has no compute, no atomics and no `imageStore`, so
the only alternative is additive blending of point primitives.

**The finished frame is read through `sceneLight`** (`include/view.glsl`, spec 13.20): under
`WS_SCENE_MAX`, and never below zero. The neutral curve's toe offsets every channel by
`x - 6.25x^2` of the smallest, which for a negative `x` adds to all three, and the local exposure
takes a log and lifts what is dark tenfold, so a few thousandths below zero turn into white dots.
TAA still leaves a few beside a very bright window. The tonemap, the glare source (which
thresholds what the tonemap takes out, so the two must read the same light) and the local
exposure's grid all read through it. A NaN comes out black where a bare `min` gives white; GLSL
leaves either undefined.

## Purkinje / scotopic shift

`include/purkinje.glsl` and `tools/gen_scotopic_weights.py` (spec 11.83): rod vision in dim light.
Colour drains, the image shifts blue, reds go near-black.

**It is not in the finishing stack**, which is entirely downstream of `toneSelect`. It splices into
`sceneToToned` between the `WS_SCENE_MAX` sanitize and the tone curve. The siting rule: this is
the sensor's own spectral response, so it lands after the lens (chromatic aberration is sited
before the sensor) and before the response curve; grain, sensor noise after that curve, is its
other half. **Three wrong sites, each of which renders a plausible frame:**
- `sceneTap` misses `bloomAdd`, so every night lamp gets a fully coloured halo round a drained
  core; and it sits upstream of the sanitize, where `mix(c, INF*tint, 0.0)` is NaN.
- `toneSelect` is called by debug view 5, which would drain for reasons nothing in the frame
  explains.
- Applied to `color` after the call, the sharpen block high-passes the Purkinje term itself.

**The weight is a product of two gates, and that is a measurement, not a preference.** A per-pixel
gate on absolute radiance (through `oneOverPreExposure`), and a whole-frame gate on the metering
1x1, sampled on unit 7. A day frame's darkest decile sits at log2 -7.85, so without the global
veto a noon shadow takes wLocal 0.997; and the day frame's darkest half sits below the night
frame's brightest third, so no per-pixel threshold separates them. Both failure cases, a daylit
shadow and a lamp at night, want a veto rather than a vote.

**So `postfx_measure_luminance` and `postfx_read_luminance` are separate**: the three draws run
whenever the shift is on, and the blocking readback only under automatic exposure.
`adapted_luminance` does not exist under `--no-auto-exposure`, which most goldens and many arms
run with, so a term keyed to it would be invisible to the suite.

**The weights.** `PURKINJE_SCOTOPIC_W` is derived: V′/V at each primary's dominant wavelength,
normalised to sum to exactly 1 so grey keeps its brightness, and checked against the display
white's published S/P ratio (2.36 against about 2.4). `PURKINJE_ROD_TINT` is chosen, with unit
photopic luma enforced. It is algebraically identical to Kirk & O'Brien's per-cone injection, not
an approximation of it: rods are monochromats, and the LMS round trip of a scalar collapses to one
vector. Acuity and rod noise are separately toggled.

**Every threshold is a look constant.** This engine's whole day-to-night range is 4.2 stops where
reality's is about 17, and its sky runs four decades under the photometric scale unless an app sets
it photometric (spec 13.7), so `purkinjeBiasEV` is the one knob that moves them for an app that
does. The daylight identity holds at the shipped bias, not at every bias: a bias of 2 moves a
daylight frame, which is the flag doing its job.

**Reached by:** off by default everywhere, `tree` included: its night sea is wrong
(`docs/aaa-rendering-roadmap.md`, B15), and a desaturating model over it reads as a partial fix
that has not happened. `--purkinje`, `--no-purkinje`, `--purkinje-strength`, `--purkinje-bias`,
`--purkinje-acuity`, `--no-purkinje-acuity`, `--purkinje-noise`, `--no-purkinje-noise`,
`--purkinje-debug`; `post.purkinje {enabled, strength, bias_ev, acuity, noise}`.

## Specular occlusion

`spec_occ_composite_frag` and `include/spec_occ.glsl` (specs 11.3, 11.4, 11.75, 11.76, 11.77).
Three modes, off, legacy and split, default split (`POSTFX_SPEC_OCC_SPLIT`, 2).

**The default's term is computed in `gtao_frag`, not here.** The reflection lobe is tested against
the sector bitmask while that mask is live; it is loop-local and nothing downstream can see it.

- **It is carried as the estimator's two sums, `(visible, total)`**, on a third attachment of the
  GTAO FBO, RG16F at half res, and divided only at the consumer. The accumulation, the blur and the
  bilateral upsample all average it and the denominator varies per pixel, so a mean of quotients
  is not the quotient of the means. It also gives the sweep's early-outs a true neutral: `(0,0)`
  contributes nothing and leaves a neighbour's ratio alone.
- **Occluders use the round hit criterion and the lobe uses touch.** An occluder is asked whether
  it covers a sector, a lobe whether it reaches one; a mirror lobe narrower than one sector would
  round away to nothing and report the surface unoccluded.
- **Clamping the lobe's sector range to the hemisphere is the horizon reference.** A cone-overlap
  estimate needed a second overlap to estimate it; that form (the old `bent` mode) was removed,
  and git, not a mode, is its comparison.
- **What it gives up**: the sweep reads half-res roughness (`linDepthTex.w`), and the
  depth-bilateral upsample cannot help at a roughness edge on flat ground.

`ao-ring` is the only arm that runs any specular-occlusion mode.

**The bent normal is a different thing and still ships.** `gtao_frag` writes it into
`AoOut.gba`, the blur, accumulation and upsample carry it, and only debug view 9 reads it. It
costs a 64-iteration-per-pixel loop, the shader's one real ALU spend for it. Whether it stays is an
open question wanting a measurement; booking it for SSGI directionality, SSR occlusion or DDGI
sampling on argument alone is the reasoning that failed for the specular case.

## IES profiles

`include/lights_ubo.glsl`'s `IesBlock` and `iesProfile` (spec 11.57): the measured angular
distribution of a real luminaire (IESNA LM-63), which **replaces** a spot's analytic cone rather
than multiplying it. Every symmetry ships -- rotational, quadrant, bilateral and fully asymmetric --
since a 2D table costs bytes in a UBO, not a sampler unit.

**Storage.** At most 32 vertical by 16 horizontal taps, under two independent limits: 8
descriptors and 3968 shared pool floats (seven fully asymmetric profiles or about 125 symmetric
ones). It is its own block, at binding 6, because the per-frame cluster upload orphans its buffer
and rewrites only the live light prefix: a table sharing `LightsBlock` would be undefined every
frame. A light's profile index rides `attenCutoff.y`, `-1` for none, matching `shadowMisc.y`'s
convention at no cost in bytes.

**The table is normalised to peak 1 and multiplies `intensity`**, and the loader seeds `intensity`
from the file's own peak candela when the scene states none. So a profile is absolute by default
(normalised times peak is absolute), `intensity` stays the one brightness control, and `gates.py`'s
`_scale_emitters` stays true.

**The tail tap is exactly 0, and that is a contract**: `pbr_frag` skips nine shadow taps and the
whole GGX chain on `attenuation <= 0.0`. The tail is exact because both readers snap it, not
because files carry zeros. `ies_tinytail.ies`, whose 90-degree candela is 1.2e-6 cd, is the
fixture that sees a missing snap.

**Outside the file's measured vertical range the profile is zero, not the clamped edge**: a file
measured 0..90 stopped there because the luminaire emits nothing above it.

**A partial horizontal sweep mirrors, it does not repeat.** A bilateral file measured 0..180
describes 190 degrees as 170, not 10. The fold exists three times (`iesFold`,
`ies_fold_horizontal`, `tools/gen_ies_table.py`) and they cannot share a token: `mod()`, `fmodf`
and `%` disagree on negative input. Each copy is held by a different thing:
- the tool's, by `gen_ies_fixture.py`;
- the C one, by `--ies-probe`'s `mirror` rows, which ask for `span + d` on purpose. The probe's
  other rows sample at the taps, all inside `[0, span]`, where a mirror and a modulo agree exactly,
  so they cannot see the fold;
- the GLSL one, which decides pixels, only by a rendered frame: `ies-mirror`. A modulo there fails
  that arm and no other.

## Contact shadows

`contact_shadow_frag`: a full-internal-res depth march toward the key directional and toward every
clustered point or spot light with **no punctual shadow map** (spec 11.56). The second population
is why the pass exists: the punctual pool holds sixteen layers and a point light takes six, so a
scene with more than a few point lights has lights with no map.

- **A light with a map is skipped**, as a correctness requirement, not a budget. Its PCF map
  already resolves the contact, and marching it too darkens the seam twice. A cached light (spec
  13.16) holds tiles rather than a layer and counts as mapped: its `shadowMisc.y` is
  `SHADOW_TILE_MARK`, and the C-side mapless count asks `shadow_light_mapped`, never the layer
  field.
- **Area panels are not marched.** A direction exists, but `pbr_frag` shades a panel through an LTC
  integral over its whole area, so one ray at its centre is a different approximation, worse than
  none.
- **The per-light visibilities fold into the one R8 channel weighted by each light's
  contribution**: radiance x `getDistanceAtt` x N.L, the fraction of direct light the pixel loses.
  **The denominator is every light that reaches the pixel, not every light marched**: a mapped
  light and an area panel still deliver light there. Leaving them out makes one blocked practical
  beside a bright mapped spot take 23% off a pixel that should lose 1%.

It reads the cluster list through `#include "lights_ubo.glsl"` with no C-side binding work:
`create_post_program` links through `ubo_wire_blocks` against buffers bound for the context's
lifetime, as `froxel_inject` does.

## Cascade soft shadows

`cascadeOpaqueTap` in `pbr_frag` (PCSS): a blocker search over a disk the emitter's size, the
average blocker's distance setting a penumbra, then PCF over that penumbra, every tap compared
against the receiver's plane (`include/receiver_plane.glsl`). The catcher (`catcher_frag`)
compares against its own plane the same way, through a 5x5 kernel on the outermost cascade.

**Every threshold here is counted in texels of the sampled cascade's map, never as a fraction of
its depth range** (spec 13.58).
- **What the depth range is:** a cascade's light eye is pushed back past its slice by the scene
  pad (`0.5 * far_plane`, and the render app's far plane is 8 scene radii). So the range, and any
  fraction of it, is about 4% of the scene's radius, at any scale.
- **What a fraction would cost:** a blocker margin of 1% of the distance from that eye, or a
  catcher bias of 0.5% of the range, gives every occluder shorter than that no soft shadow and
  nothing on the catcher -- a resistor on a circuit board, or the printed marking on top of it.
- **So:** the blocker margin is one texel (`CSM_BLOCKER_TEXELS`, the cached tiles'
  `TILE_BIAS_TEXELS`), and the catcher's bias is the receiver-plane floor.
- **The cost, accepted on look:** a shadow map's self-shadowing at a curved surface's terminator,
  which PCF has always shown, shows under PCSS too.
- **The particles, the water and the fog keep a flat bias:** air and a refracted bed have no plane
  to extrapolate.
- **What holds it:** the `pcss` gate group, on `short_occluder_fixture`.

## Area-light shadows

`panelCubeFace` in `include/cube_face.glsl`, read in `pbr_frag`'s area branch (spec 13.27). The
engine side, the pool and the range, is in `docs/lighting.md`, The per-frame punctual pool.

**The frame.** A panel's per-frame shadow is five layers of the punctual pool: a cube in the
panel's own frame -- `r = cross(up, normal)`, the width axis `ltc.glsl` uses, then the up and the
normal, as x, y and z -- drawn +r -r +u -u +n from the panel's centre. Each face is rolled to
another frame axis rather than to world up, so its square frustum covers the region
`punctualCubeFace` assigns it in that frame.

**The face behind the panel is never drawn**: every direction whose dominant frame axis is -n is
behind the panel's plane, where `ltcPanel` is already zero. In a world-axis cube that skip is wrong
for a tilted panel, since the face opposite the normal's dominant axis still covers directions in
front of it; `area-tilt` is the arm that sees it.

**The occlusion is a hard binary test of the panel's centre**, multiplied into the whole LTC
integral; nothing occludes one edge of the rectangle and not the other. Unreal shadows a rect light
the same way, from its centre, as a point light.

**The faces end at the panel's range**, since a panel lights nothing past it. Points and spots
keep the scene's far plane.

**Rejected:** one perspective map down the normal with a fixed 120-degree cone.
`punctualShadow` reads anything off a map as lit by design, so every receiver more than 60 degrees
off the normal is unshadowed, walls included, and a ceiling tube lights the room beside it through
the wall. Fitting the cone instead is no better: a room's walls reach the panel's own plane, so the
conservative cone is 180 degrees.

## Cached shadows

`include/punctual_tiles.glsl` over `include/tile_lookup.glsl` (spec 13.16). What a cached light
is, how the budget and the store cells are given out, and how the kept faces follow scene changes
are in `docs/lighting.md`, Cached point- and area-light shadows; this is the lookup and the views.

A point light with `cast_shadows`, `shadow_cache` and a range keeps its cube faces as 256² tiles
of the punctual array, drawn once by `shadow.c` and kept. A panel may be cached too (spec 13.27),
from its centre over the same six world-axis faces, with one view and no body: `light_has_body` is
false for a panel whatever it carries, and `create_light` refuses it one by name, since the capsule
would run along its normal.

**The lookup projects onto a face analytically** from the view's origin, the near plane and the
far, with per-face axis, right and up tables written from `glm_lookat` over `light_space_up`, and
finds the tile from the array's own size. Six matrices a light would fit in no uniform array; a
projection is three dot products and a divide. A light with no body takes the per-frame map's 3x3
receiver-plane PCF.

**A cached panel's near plane must clear its fitting.** A cached light's default near plane is
0.2% of its range. A strip set inside a steel channel then shadows the ceiling round the tube from
within, a hard dark patch the per-frame cube's 0.1 m clips away, so the app states a near plane
past the channel, as a lantern states one past its panes.

### Views over a body

**A light with a body is the average of views over it.** Eight views (`SHADOW_TILE_VIEWS`), each a
real cube map from a point of the body as it was when drawn: view 0 at its centre, which keeps the
fog's single tap and a bodiless light's one view on the same origin with no new packing, and the
rest by R3, stratified along the segment with a point of the ball each. `tile_body_point` in C and
`tileBodyPoint` here are two copies of one formula; a drift between them projects views from the
wrong point, which is loud, which is why two copies are allowed.

**`source_length` is the length between the end caps, not the whole spine.** Written as the whole
spine, the caps carry the body a diameter too far, into the wax of a candle, where views inside
the candle see straight through it.

**The drawn body is packed whole, in slots no type reads for anything else**: the centre in
`shadowTile.xyz`, the segment in `attenCutoff.zw` and `shadowMisc.x` (the reserved slot and the
cone cosines, which only a spot reads, and a spot is never cached), the radius in
`colorIntensity.w` and the near plane in `upArea.w`. Never `shadowMisc.zw`: that is a panel's
extent, and the LTC would read a cached panel's rectangle from it (`tiles-area-size`).
`tileLightAt` in `tile_lookup.glsl` is the one place that decodes it, for the surface and the fog
alike, and it asks no light's type.

**Three designs were refused**, measured on `tile_core_fixture` against a trace of the same body
against the rim with no shadow map (`tiles-truth`):
- **PCSS capped at the guard band**: 8 texels of a 256 tile is about 2.7 cm each side on a desk
  40 cm under a candle, where the flame spreads the edge over tens.
- **A march through one view** (Unreal's SMRT): a march decides a ray is blocked when a step is
  behind a stored surface, and every ray from a point the view cannot see starts there, so the
  full shadow is the size of the centre's hard shadow, 25.5 cm against 12.5 traced. One view
  cannot tell "hidden from me" from "solid".
- **A march through three views along the flame**: each view still cannot light what it cannot
  see, so the result is the views' hard shadows stacked, a staircase with full shadow to 18.6 cm.
  Unreal closes this with an unpublished gap-filling guess; worked through, the depth-jump form of
  it lets light through the rim wherever a ray enters its face near its edge.

### The blur

A finite count draws the shadow as overlapping copies, one in eight at each view's hard edge. Each
view stands for about s = cbrt(volume / 8) of the body, whose own penumbra is a ramp
s (dR - dB) / dB wide on the receiver, so a second pass reads each view at a point jittered across
the receiver's plane by that, and the jitter averages to the ramp under TAA. dB comes from the
first pass: each view that finds the receiver hidden reports its occluder's depth. Two ways it
fails while plausible:
- **Without TAA the jitter does not turn**, and a fixed jitter moves each view's copy bodily,
  sheared by the spread -- worse than the copies. Off TAA the result is the plain average.
- **The first pass must read at jittered points too.** At the receiver alone it cuts the outer half
  of every ramp: past the last view's edge no view is hidden, the lookup returns 1, and where the
  views' edges coincide (beside a candle's vertical side) that half is most of the step, so the
  edge comes back crisp. Its width is the ramp an occluder a body's length from the light would
  cast.

Jittered reads at a corner slide off the receiver's plane into the surface beside it. A guard for
it moved nothing measurable and is not in; if it shows, that is the place.

**The dance is a read, not a redraw.** A light moved by D casts its shadow -D (dR - dB) / dB from
where it was, so the kept views are read at the receiver moved +D (dR - dB) / dB, D being the
centre's move since drawing. It is first order: a flame's lean is a rotation one shift of the
centre cannot express, and is what remains of the gap to the current flame's reference
(`tiles-dance`).

### Movers and the kept faces

A face that sees a moving caster is its store cell's copy of the still casters plus the movers,
drawn each frame. `tiles-movers` holds that copy to the scene that never moved, at the swing's rest
frame, and fails if the overlay is skipped; `tiles-store` holds the copy and the whole draw to the
same picture. A drawn face that needs a cell and finds none free takes an idle cell first, the
farthest light's, then a cell of a light more than 2 m farther. What is easy to get backwards on
the drawing side:

- **A pose is a mover on every frame** (spec 13.18). A skinned, swaying or morphing surface moves
  without its node moving, so a face drawn once would freeze it. It is in no store and in no face
  drawn whole and kept: it is drawn only over a copy, into the faces its posed bounds reach this
  frame. A face is drawn over its copy until a frame in which no moving caster reaches it, and is
  kept from then on, so a character walking past a candle costs that candle's faces only while it
  is in them.
- **A sway is a mover only when the scene's wind can move it**, which classification asks of
  `wind_mesh_max_offset`.
- **`cachedShadowWind: "rest"` sets the wind response per draw**, to 0 in every kept set, not with
  the material's block, which is uploaded once across every set of the depth pass: a rest material
  uploaded for a cascade and then drawn into a kept face would keep its sway there. Rest is one end
  of the lean, not its middle, so a swaying caster sees its own shadow off by at most its sway.
- **A material change the graph cannot see is found by comparing the draw list with the one the
  faces last saw**, item by item (`tiles_mark_changed_looks`): its wind, its opacity, its shadow
  role, its cut-out, or the rest setting can move a caster into or out of the kept faces. Such a
  caster is drawn again where it stands and where it stood (`tiles-rest-toggle`).
- **A graph change is matched by node serial and mesh id**, both lists sorted on that and merged
  (`tiles_mark_graph_change`). Matched by pointer, a node freed and another hung in its place at
  the same address is the same caster that never moved, and its old shadow stays in the kept faces
  (`tiles-graph-replace`). `tile_generation` is bumped only when the array's contents are lost,
  never for a graph change, and an origin shift moves the recorded boxes with the blocks.
- **Stores are taken with every other block, before the region is laid out.** A store taken
  mid-frame lands past the array until the region grows, and its blit is to an invalid
  framebuffer.
- **The rain's cover moves with the tiles when the array is rebuilt.** The per-frame layers are
  redrawn by the same pass, but the rain's layer is drawn only by the rain pass, which a capture
  does not run; a rebuild inside a capture otherwise lights every GI probe under a layer nothing
  drew, and the volume keeps the result. The cover is folded for the new edge.

## Water

`water_vert/frag`, `water_spectrum_frag`, `water_fft_frag` and `include/ocean.glsl` (spec 11.32).

**The ocean is a Tessendorf FFT run as fragment ping-pong**: 3 cascades x (1 evolve + 14 Stockham
stages) = 45 draws at 128² per frame under `--water-waves fft`. A Stockham butterfly is a pure
gather (two texels plus a twiddle row, one texel out to two MRT targets) with no shared memory,
atomic or scatter, so GL 4.1's missing compute stage costs draws rather than a redesign. The cost
lands on the CPU, not the GPU (+0.24 ms GPU against +1.19 ms CPU at 800x500), because 45 dispatches
are 45 framebuffer binds and uniform groups. It is deterministic.

**`include/ocean.glsl` is the surface evaluation**: position and analytic normal from one place, so
a normal cannot be derived from a different height than the raster used. Water is its own program
rather than a `pbr_frag` feature because it samples the resolved scene depth, and `pbr_frag` has
no sampler left.

**Motion vectors.** The include also owns the previous position (`oceanPreviousWorld`, spec 11.33),
so the spectral path reports the motion its waves have, not only camera motion. That needs last
frame's transform kept alive: the 14 stages land back in buffer 0, and one copy out of it before
the evolve overwrites it is the whole mechanism. The position arithmetic is factored
(`oceanSpectralPosition`) because current and previous must be the same sum, or the velocity
describes a surface the raster never drew.

**The CPU twin.** `procedural/water_waves.c` is the CPU twin of the Gerstner half and duplicates
its constants. Nothing compares the two; that is a known gap.

**Foam and roughness** (spec 11.42). `water_foam_frag` accumulates whitewater per cascade texel, so
it outlives the crest that made it, in three bands of one RGB target. `water_frag` adds a Cox-Munk
sun lobe and a far-field roughness, both built from the seeded spectrum's own height and slope
variance, which `--water-fft-probe` checks rather than asserts.

**The water program declares 15 samplers of 16.** The six identical RGBA16F cascade textures are
one `sampler2DArray`, one unit where six textures would take six. The units (the table in
`water.h`): 0 the cascade array, 1 the previous array, 2 foam pattern, 3 the caustics targets (a
layer per level), 4 the ripple band's array, 5 the touch ripples, 6 scene colour, 7 depth, 8 bed,
9 the rain cover, 11 shadow array, 12-13 the IBL pair, 14 cloud shadow, 15 foam.
- The cascades and their previous copy are two arrays rather than more layers of one, because
  their lifetimes differ: the fields are this frame's render targets, the previous a copy taken
  before they are overwritten.
- Two sampler types against one image unit is an `INVALID_OPERATION` at draw, so a unit's type is
  fixed across the program.

**The ripple band (spec 13.3)** is a fourth spectral band, past the short band's 24 rad/m, in its
own 512² transform over 6 m -- 19 more draws, 64 a frame -- because a 128 transform over a tile
short enough for centimetre waves repeats every metre or two. It shades and never displaces. It is
sampled at the mip its footprint asks for rather than faded by distance: the slope variance each
level keeps is predicted at seeding, so what a footprint filters out is a known number handed to
roughness. `water-ripple-mips` holds that prediction against what `glGenerateMipmap` built.

**The caustics have two levels** (spec 13.3): the 40 m window of 9.4 cm cells, and a 10 m window
of 2.34 cm cells (two ripple texels) blended over it where it reaches, used only in water shallower
than 8 m of traced depth. Ripple focus does not survive deeper, and over a 9 m bed the fine level
loses a quarter of its light for a reason still open.
- **A `glFlush` between the trace and the lattice draw is load-bearing.** The lattice reads its
  traced points by vertex-stage `texelFetch`, and on Apple's GL that fetch otherwise sees the
  texture from before the pass that rendered it: the caustics run a frame late.
- **The key's visibility at the bed is averaged over a disc** as wide as the waves spread the
  refracted key over the path down, so a shadow falling on the bed fades the caustics out rather
  than stopping them on a line.

**What came from Clearwater** (spec 13.4). Three of the six are easy to get wrong:
- **Light reaching a submerged surface is weakened on the way down**, in `pbr_frag`, through
  `include/water_light.glsl`: `1 - F` at the surface and Beer-Lambert along the key's refracted
  path, the sky by its diffuse coefficient. `water_fixture`'s bed is emissive and blind to it.
  `water-downwell` runs on `water_downwell_fixture.cscn` and solves the depth from each channel at
  two key angles, which is what catches a vertical path or a missing Fresnel.
- **The in-scatter is normalised against the nadir, not the sphere.** Both the refracted sight
  line and the refracted sun point down, so an eye above the water only ever sees the back of a
  forward-peaked lobe; a sphere normalisation dims every sea to a third, which Clearwater covers
  with a constant of 3.2. The column is integrated exactly, where Clearwater samples the sun at
  mid-depth, an estimate that sends bottomless water to black.
- **The caustics' colour is one trace spread across the spectrum**, which gets the amount right
  and not the placement. At a few metres the ripples' light has crossed itself before the floor,
  and each wavelength's crossings land elsewhere; 8 traced bands against 16 still disagree by 0.7
  of the signal. Three traces at three indices, Clearwater's way, is three samples of a continuous
  spectrum and prints saturated speckle at every fold. A per-point dispersion from the trace was
  tried and did not earn its second target. `--water-caustic-bands` and
  `--water-caustic-debug 3` are the instrument, and `water-caustic-spectrum` holds the amount.

The other three are plainer: a cubic B-spline read of the shading bands where they are magnified,
the bed's relief read off its brightness against its neighbourhood, and sunlit specks in the
column. The touch ripples are a 256² wave equation over 7 m that follows the view, stepped at a
fixed 60 Hz from the frame clock so a headless run repeats, and read through `oceanShadingNormal`
so the caustics refract through the rings as well.

## Diffraction glare

`glare.c` plus four `glare_*_frag` passes and `include/glare_threshold.glsl` (specs 13.4 and 13.5,
after Clearwater). **Off by default**; a `.cscn` turns it on with `post.glare`, an app with
`postfx->glare_enabled`.

**How it works.** The aperture's diffraction pattern is the power spectrum of a round lens with
flattened sides, scratches and dust. It is summed over 32 wavelengths between Clearwater's eight
band colours, and faded to its inscribed circle. That pattern is convolved with the frame's light
past `glare_threshold` by FFT on a 512x256 grid, so the cost does not depend on how many glints
there are. Red and green ride one complex signal and are separated in the multiply by Hermitian
symmetry.

**It moves light rather than adding it.** The tonemap takes what the star carries away out of the
pixel it came from, by the same threshold rule the source pass uses, and the frame's light is
conserved to 1.007 (`glare-conserves`). A frame with nothing past the threshold is identical to
`--no-glare`.

**Three decisions that look like tuning and are not:**
- **The Airy disk is cut from the kernel**, and its light, about three quarters, stays where it
  was at full resolution. The grid's cell is several frame pixels, and moving the core through it
  redraws every bright edge soft and stepped.
- **The source thresholds every frame texel before it downsamples.** Thresholding a block's
  average misses a lone glint, and puts back 58% of what the tonemap removed.
- **Clearwater's eightfold far-field lift is not carried.** It imitates a phone lens rather than
  an aperture, and over a glittering sea it sums into a veil and long streaks.

## Local exposure

`local_exposure.c` plus seven `le_*_frag` passes, sliced in the tonemap by
`include/local_exposure.glsl` (spec 13.19). The sources are Durand & Dorsey 2002, Chen, Paris &
Durand 2007 and Unreal 5's Local Exposure; `docs/papers/README.md` says what each gave.
**Off by default**; a `.cscn` turns it on with `post.local_exposure`, an app with
`postfx->local_exposure_enabled`.

**How it works.** The frame's luminance, in EXPOSED log2, is split into a base and a detail.
- **The base:** a bilateral grid sliced at each pixel's own luminance, mixed with a heavily
  blurred luminance by `blend`.
- **Contrast:** the base's distance from middle grey (`exposure.key`) is scaled by `highlights`
  above grey and `shadows` below it; the detail by `detail`.
- **The factor** is what turns the pixel's luminance into that, applied to its RGB before the
  curve. Every scale at 1 is the identity.

**The grid**, Unreal's geometry:
- **Cells** of 64x64 half-res texels; **32 bins** over 30 stops, centred on the key.
- **Built by gather, in two passes.** GL 4.1 has no atomics to splat with, so a fragment owns one
  (8x8 sub-block, bin) pair and keeps the tent share of each texel near its bin; a sub-block's 32
  bins are neighbouring fragments, which fetch the same texels together. A second pass sums a
  cell's 64 sub-blocks.
- **Blurred** by Chen's separable 5-tap Gaussian in x, y and the bins.
- **Stored** as homogeneous (sum, weight) tiles of one RG32F atlas, with the blurred luminance in
  its own region of the same atlas, so the tonemap reads it through one sampler.

`local_exposure.c` owns the passes: the frame at half res, the grid gathered and blurred, the
blurred luminance. It is made on first use and latched off if that fails, as `glare.c` is.
`local_exposure_probe` (`--le-probe`) prints the frame's luminance percentiles in stops from grey,
the range the settings are asked to fit; measure with it before tuning by eye.

**What a plausible frame hid while it was built:**
- **The window looked like it would need 10 stops of compression.** `--le-probe` measured 5.8:
  silent's kitchen is an ordinary overcast interior. The strength it needs comes from the day key
  of 0.45, which leaves about one stop above grey before white.
- **Unreal's default blend of 0.6 is the weaker choice in a lit room.** The blurred luminance
  averages the dark walls into the window's base. On `abandoned_window`, blend 0 recovered most.
- **A plain blur is what the grid is FOR.** On the gate's window-in-a-wall, blend 1 darkens the
  wall beside the window to 0.37 of itself, and the grid leaves it at 1.000. `le-halo` carries
  both halves.
- **The off path is a uniform gate, and the goldens did not move.** Every one stayed 0 px, with no
  codegen drift from the new branch.
- **The first gather cost 18.8 ms, and the fetch count was not why.** One fragment per (cell, bin)
  walked all 4096 of a cell's texels: a few thousand long serial loops at silent's 3200x1800,
  against a 1.4 ms tonemap. The blurred luminance's 256-tap blocks cost 0.7 ms at 4K the same way.
  Both are now two passes of short loops over many fragments, as the meter's histogram is: 0.8-0.9
  ms in silent's kitchen, 1.2 ms on `abandoned_window` at 4K. The grid's first pass is most of
  what is left.

## Film grain

The finishing stack's sensor noise in `tonemap_frag`, after the LUT and before the dither: one
monochrome draw a pixel, scaled by `grain_strength` and by a midtone weight that falls to nothing
at black and white. **Off by default**; `--grain <f>`, `post.grain`.

**Every frame is new noise, from `frameNoise`** (`include/noise.glsl`, spec 13.28): Jarzynski &
Olano's `pcg4d` (`include/pcg4d.glsl`, also the rain's and the fire's) over (pixel x, pixel y,
frame), the top 24 bits. Unreal does the same job by jumping a grain texture to a random offset each
frame. The Purkinje rod noise draws from it too, on `uv * 1024` so its grain stays one size at any
resolution.

**Never seed noise that is seen by adding the frame to its coordinates.** `hash21(gl_FragCoord.xy +
frame)` adds the frame to both coordinates, so each frame is the last one moved a pixel along the
diagonal (0.994 correlated at that shift), and the eye follows it as lines crawling across the
picture. Any "advance the pattern by an offset" seeding does this. It is fine for noise a temporal
filter averages away, and wrong for noise that is seen.

`grain-still` reads the crawl, and `grain-amount` the spread against the uniform draw's
1/sqrt(12). The dither is not grain: it is static on purpose (spec 11.24).

## The CRT

`crt.c` with `crt_resample_frag` and `crt_frag` (spec 13.28), after Lottes' CRTS (2018) and the
slot mask of his earlier crt-lottes (2014), both public domain; `docs/papers/README.md` has both.
**Off by default**; an app turns it on with `postfx->crt_enabled`, the render app with `--crt`.

**Where it runs.** With it on, the post chain writes the finished picture into PostFX's picture
(`PostFX.picture`, window-sized RGBA16F, shared with an app's after-tonemap passes) instead of the
window, and the debug tiles and the app's overlay draw into it after it. The CRT then draws that
into the window, and the debug GUI goes on top. So a game's door prompt and menus are on the
television and the developer's panel is not. The picture is window-sized exactly, because the UI
scissors in window pixels. Everything is in linear light: the resample decodes the picture through
`include/display.glsl`'s inverse of the tonemap's 2.2, and the CRT encodes through the same file.

`crt.c` knows nothing of PostFX: `crt_present` takes the picture and a `CrtLook`. It is made on
first use and latched off if that fails, as `glare.c` is.

**How it draws.**
- **The signal.** The picture is area-filtered down to `crt_lines` lines (480 by default), as wide
  as the window's shape makes it. CRTS expects an input that small and says so.
- **The beams.** Each window pixel is lit by the two nearest lines, each a windowed-cosine beam
  whose thinness `crt_scanlines` sets, filtered across by CRTS's `exp2(blur * d^2)` over six texels.
- **The bleed.** Colour takes a wider filter than brightness: the wide filter's colour with its luma
  replaced by the narrow filter's, which is what a YIQ round trip with chroma blurred wider does,
  without the matrices. Composite video carried colour in a narrower band than brightness. The
  widest setting is taste bounded by the six taps; NTSC's I bandwidth would reach past them.
- **The mask.** crt-lottes's slot mask: RGB stripes a pixel each, a dark row every second line,
  offset in alternate groups of three columns. Only darkened, never pushed past white. It is in
  WINDOW pixels, not warped, and its pixel is the window's height over 1080 rounded, at least one
  -- two on silent's 1800 -- so a phosphor stays something the eye can resolve.
- **The tube.** CRTS's warp, each axis bowed by the other's square, black past the picture with
  rounded corners.
- **The tone.** CRTS's curve on each pixel's peak channel lifts mid-grey back over what the beams
  and mask take.
- **Last, the encode and the dither**, this being the pass that writes the window (spec 11.24).

**What a plausible frame hid while it was built:**
- **The frame comes out darker than it went in, and that is the design.** The curve holds 0.18 and
  compresses everything above it: under the default mask, 0.4 comes out at 0.91 of itself. A gate
  on the frame's mean would have asserted something CRTS does not claim. `crt-mean` predicts each
  flat block from the plain frame through the mask and the curve, and holds it to 2% (0.83%
  measured).
- **Scanlines a pixel or so apart are moire, not scanlines.** They fade to fused between 2.5 and
  1.5 window pixels a line, so a 480-line signal shows them faintly on a window over 720 pixels
  high and fully over 1200.
- **The dither had to move.** Left in the tonemap, it would have been resampled away with the rest
  of the picture's fine detail, and the window's 8-bit write would have banded.
- **It costs about 0.65 ms** at silent's 3200x1800 and is not why a heavy frame is slow; measure
  `--profiler`'s `crt` row before blaming it.

## Shader hooks

An app, or a `.cscn`, puts its own GLSL at four places, so a new effect is a shader file and a
setting rather than an engine module (spec 13.29). The shape is Unreal's: a surface material
feeding the engine's lighting, a translucency pass of "After Motion Blur", and post materials at
named Blendable Locations. The four are the post passes, the late lane, the surface hook and its
offset; under them sit the include resolver and the shader params. `shader_hooks_fixture` has a
piece at every place and the `shader-hooks` gate group an arm for each. The inventory of the
files involved is in [shaders.md](shaders.md#inventory).

### The include resolver and the splice

`create_shader` (`shader.c`) expands every source's `#include "x.glsl"` against
`shader_includes.h`, the `include/` chunks unexpanded, so an app's shader may include
`noise.glsl` like the engine's own. The engine's shaders were expanded at build time and pass
through unchanged.
- **Nested and include-once**, with a `#line` back to the including file's own numbering after
  each top-level include, so an app's compile error names its own line.
- **The splice is text only.** `shader_source_splice` puts an app's chunk at a marker line in an
  engine shader, numbered as source string 1 so its errors read `1:<line>`, and leaves its
  includes to the one walk. A splice that expanded its own includes would number nothing after one
  and would treat a chunk anywhere in the host as already held.
- **Each build-time `// ---- begin X ----` line counts X as included from that line on**, so a hook
  shares the copies above its marker. A hook including a chunk the host defines only below its
  marker, directly or through another chunk, is refused by name rather than defining it twice.

**Embedding.** `cetra_embed_shaders(<target> <dir>)` (CMake) turns an app's `shaders/*.glsl` into a
header of strings, includes left for the resolver. Its sibling `cetra_embed_files(<target> <name>
<INDEX> <file> ...)` (spec 14.6) builds files of bytes into the program, a font for instance, so
the program is one file:
- the table is `<target>_<name>[]` of `CetraEmbeddedFile` (`embedded_file.h`: its path from the
  source root, its bytes and their count), indexed by `<TARGET>_<NAME>_<INDEX>`;
- it is declared in `<target>_<name>.h` and defined in a generated source file of its own, so the
  bytes are compiled only when a file changes, never with the code that reads them, and two files
  including the header share one copy;
- the path is the name a font made from the entry is pooled under, so two files of one name in two
  directories are two fonts; `load_font_memory` and `create_glyph_cache_memory` make a font from an
  entry;
- the list is the one place a file is named: the code says only what each is for.

### Post passes

`postfx_add_pass(fx, at, program)`, over a program from `create_post_pass_program`, at
`POSTFX_AT_BEFORE_DOF`, `POSTFX_AT_BEFORE_BLOOM` or `POSTFX_AT_AFTER_TONEMAP`. The pass borrows its
program, which its owner registers with `engine_add_program`.
- **Scratch and blit.** GL 4.1 has no texture barrier, so a pass draws over the frame into a
  scratch target and is blitted back, the motion-blur idiom, and the frame keeps its handle. The
  HDR passes share their scratch with motion blur, whose use of it a frame never overlaps.
- **Inputs bound by name**: `sceneColor`, `sceneDepth` (render res), `texelSize`, `time`, `frame`,
  `projection`, `view` and the pass's params. A pass that never declares `sceneDepth` costs the
  frame no depth resolve.
- **What each point sees.** Before DoF: the fogged frame with the late draw in it. Before bloom:
  whatever DoF left, ahead of the meter, so the pass is metered. After the tone map: PostFX's
  picture (`PostFX.picture`, the CRT's intermediate), which the tone map writes without its
  dither; then the CRT or `present_frag` writes the window and dithers, so the dither stays last.
- **Who writes the window is decided once a frame**, by `postfx_run` (`PostFXShow`): the CRT
  whenever it is on, the present pass when a tone-mapped frame has passes after it, the tone map
  otherwise. A passthrough frame, a debug view's or a display-ready one's, stays undithered and
  runs no pass. The engine draws its overlays into whatever `PostFXShow` says. A picture that could
  not be made is tried again at the next size, as the fog layer is.
- **Replacing the tone map is not offered.** Its curve is a function inside a shader that also
  composites AO, contact shadows, bloom, flare and glare and runs the curve at five sharpen taps,
  which is not a stage boundary.

Two things are easy to get wrong when measuring:
- **The identity is not exact after the tone map.** The picture is fp16, and the dither lands one
  code differently on about a tenth of the pixels against a frame written straight to the window.
  The HDR copy-back is exact.
- **Each mark needs its own baseline.** Measured against the frame with no passes, a mark picks up
  its neighbours' bloom; `post-order` measures each against the frame without that one mark.

### The late lane

`Material.pass = MATERIAL_PASS_LATE_DRAW`, over a program from `create_late_surface_program`,
drawn by `render_late_items` from the post chain's late draw, ahead of fire and rain: past TAA and
motion blur, so a surface that changes every frame (noise, a screen) is not averaged into grey.
Drawn before the TAA seam, the fixture's noise keeps 5 codes of its 31 and correlates +0.72 with
the frame before.
- **Order and blend**: farthest first, ties in graph order, premultiplied (alpha 0 adds, 1
  replaces), through the shared submission (`SubmitState`, `submit_draw_run`).
- **Hiding is a depth compare.** The late canvas has no depth attachment, so
  `include/late_surface.glsl`'s `lateVisible` compares against the resolved depth, Unreal's own
  choice in that pass for the same jitter reason. `lateEmit` carries emitted light through the
  frame's fog and the pre-exposure.
- **Bound by name**: `model`, `view`, the unjittered `projection`, `time` and `frame`, the material
  under the lit surface's names (`albedo`, `emissiveFactor`, `albedoTex`, `emissiveTex` with their
  `*TexExists`), its params, and through `postfx_late_draw_bind`, which fire and rain share,
  `sceneDepth`, the fog volume and `viewport`. `vColor` is white on a mesh with no colours, as a
  hook sees it.
- **It reaches nothing else.** It casts no shadow, the rain's cover included: every caster set
  names the lanes it takes, so a lane casts nothing until it is listed. No capture sees it, it
  lights nothing under `--emissive-lights`, and neither subtree setter (`node_set_programs` among
  them) replaces its program.
- **Refused by name**: either half of a late surface without the other, and one on a mesh that
  skins, sways or is shadow-only, since the late vertex stage places a vertex by its model matrix
  alone.

### The surface hook

`create_shader_hook` and `Material.shader_hook` (`shader_hook.c/h`): `void cetraSurface(inout
CetraSurface s)` over `include/surface_hook.glsl`, spliced into `pbr_frag` at
`// CETRA_SURFACE_HOOK_CHUNK` before `main()`. It decides a lit surface's albedo, coverage, normal,
roughness, metallic, AO and emission. The engine owns a hook; a material borrows it by a plain
pointer, and a `.cscn` describes each hook once under `shaderHooks`, however many materials name
it.
- **Not a mask bit.** A bit switches off code the shader holds; a hook brings code in, so its define
  arrives in the same string as its source.
- **Called twice, from the same gathered albedo and coverage**, so a hook multiplying its albedo
  does it once: before the alpha test, keeping albedo and alpha (the cut, decals, the albedo view
  and the prepass exit read them), and after the gather, keeping normal, roughness, metallic, AO
  and emission.
- **No sampler is spent**: the hook reads what its variant already declares.
- **The variant key includes the hook's id** (`pbr-<mask>-h<id>`), and the resolver moves a
  material whose program's hook is not its own. Without the id every hooked material at one mask
  draws with whichever hook compiled first.
- **Refused where the app made it.** `create_shader_hook` compiles the hook's lit stages at every
  feature and at none, plus the skinned vertex stage when it has an offset. A hook naming something
  only a feature declares (`sheenTex`, a chunk the host includes only under the rain or cached-tile
  bit) compiles at the full mask and fails at `pbr-0`; built at both ends, it builds at any. A hook
  on a material whose program is not a lit-surface variant is refused by name, since its surface
  would ignore it while its bound and shadow did not.
- **Its shadow programs are built with it.** A hook whose offset does not build into them is
  refused. A surface that does not, one reading the normal map (the shadow stage declares only the
  albedo), keeps its offset and loses only its shadow cut (`shadow_cuts`), casting by its
  material's alpha.
- **A hook whose variant fails anyway is dropped everywhere** (`ShaderHook.broken`, read through
  `shader_hook_live`). The check compiles but links nothing, so a hook whose halves disagree lands
  here. It is said once by name, and every reader (the resolver, `classify`, the bound, the
  refusal) treats its materials as unhooked, so the surface, its bound and its shadow never
  disagree. The shadow pass runs before the resolver, so the frame it fails on is the one frame
  they differ.

### The offset

`vec3 cetraOffset(CetraVertex v)`, spliced at `// CETRA_OFFSET_HOOK_CHUNK` into
`object_position.glsl`'s `cetra_local_displacement`. It is the third displacer, so every displacer
rule in [shaders.md, Vertex Attributes](shaders.md#vertex-attributes) holds:
- **its bound**: `offset_bound` widens `draw_item_bounds`;
- **one splice reaches every stage**, the previous frame's offset at the previous time for the
  motion vector;
- **the lean prepass is sat out**. Only a surface moved away from the eye shows a broken prepass:
  the shading pass's one-sided `LEQUAL` passes every fragment that came nearer, which is why the
  fixture carries a quad pushed back beside its dome.

Shadows:
- **A light draws a caster through the hook's own shadow program** wherever `classify` marked it
  `DRAW_HOOKED_CASTER`: an offset, or the alpha of a foliage caster.
- **A masked hook casts through its alpha**, since `DRAW_FOLIAGE` accepts a hook in place of an
  albedo map. Only a hook whose alpha reached its shadow program counts; a card with no albedo map
  falling back to the plain program is cut by whatever texture the last caster left on unit 0.
- **One owner primes a shadow layer's programs.** `_draw_shadow_items` forgets the program the last
  layer left bound and sets the light matrix and displacement inputs on each program as the layer
  first draws through it. Primed by the caller instead, a layer's matrix went to whatever was
  bound, and the plain casters of the next layer drew through the previous layer's matrix.
- **Kept shadow faces.** A hook declared `animated` is a mover for the cached tiles, whichever of
  the two halves it changes, and a kept face redraws a caster whose hook or params change, over
  the old bound and the new: marked from the new alone, a face that saw only the old reach keeps
  its stale shadow.

### Shader params and the clock

`ShaderParams` (`shader_params.h`, `SHADER_PARAM_MAX` 8 named vec4s, no GL, so a scene file's
description carries them) ride a material or a pass and are uploaded by name wherever its program
is bound: the scene pass, the late draw, the shadow passes. Nothing resets a name another owner of
the same program set.

**The clock is one upload.** `shader_clock_upload` sets `time` and `frame` (wrapped at 2^24), and
`engine_upload_displacement_uniforms` calls it, so every stage that places geometry, the shadow
passes included, reads the same frame a hook does in the shading pass.

### In a `.cscn`

- `post.passes: [{at, shader, params, enabled}]`;
- a top-level `shaderHooks: {name: {surface, offset, offsetBound, animated}}`, each one hook
  however many materials name it;
- on a material, `shaderHook` (a name from that table), `lateShader` and `shaderParams`.

Shader paths resolve against the scene file, as an IES profile's does.

## Atmosphere

`froxel_inject/integrate/composite_frag` + `include/froxel.glsl` (the fog, a 3D froxel volume,
spec 9.5) and `aerial_lut_frag` (the aerial perspective, spec 9.6). `froxel_composite_frag` folds
both into one `(inscatter, transmittance)` pair, since the blend carries only one; either can run
alone.

### Where the media meet

`froxel_inject_frag` evaluates every medium once a cell.
- **The media.** The global height fog, the local fog volumes (spec 11.39) and the falling rain
  (spec 13.9, see Rain) share a cell. A submerged water body (spec 11.33) does not: a cell under the
  water's surface is the water's alone. The cloud deck's shadow is not a medium but a visibility
  term on the sun, multiplied into the fog's light (see Cloud shadow).
- **Only the global fog is `fog_enabled`.** The others arm the pass by joining the union in
  `postfx_has_medium`, because that flag belongs to the app and the GUI and is never cleared per
  frame.
- **Media sharing a cell combine σ-weighted, not added.** The integrate pass reads a cell's rgb as
  the medium's source function, not as radiance already scaled by how much medium there is, so
  adding would count the light once per medium.

### The composite at two depths

The aux attachment holds one linear Z per pixel, and the late pass never writes it:
`engine_end_oit_pass` drops the draw-buffer count to 1 (`engine_set_scene_draw_buffers(engine,
false)`) as the opaque scope closes, so the write `pbr_frag` still performs is discarded by the
buffer list, not by the depth mask. Folded at that depth alone, a translucent surface is fogged at
the depth of the wall behind it. So the composite resolves its fold at two depths (spec 11.78):
- **The MBOIT moments measure what aux cannot.** `b0` is the translucent stack's total absorbance
  along the pixel and `b1/b0` the absorbance-weighted mean of its warped depth, so one atlas fetch
  gives how much of the pixel is translucent and how far off that part is.
- **`mediumAt()` is evaluated at the surface's depth and at the stack's**, and the two pairs are
  mixed by `1 - exp(-b0)`.
- **The mix is exact when the layers carry the background's own radiance and when coverage is 0 or
  1, and approximate between.** The exact form wants the un-composited translucent colour, and the
  buffer holding it is upstream of the TAA resolve this pass deliberately runs after.
- **`--no-oit-moments`** carries no depth statistic and keeps the single-depth composite: the
  bisect lever.

The moments are the composite's fifth sampler of sixteen; the post chain has its own ledger, so
they cost the lit surface nothing.

### A cell with no history

The volume covers the view and no more, so as the camera turns, the cells entering at the leading
edge reproject outside the previous volume and have nothing to blend. With one jittered sample
beside neighbours averaged over many frames, such a cell differs a great deal from them -- the
jitter moves a sample across its whole depth slice, and the rain's binary cover tap and steep
handover vary within one -- and a turn draws a hard-edged band along the grid's columns. So a cell
that misses its history averages `fog_history_miss_samples` (4) points (spec 13.45): this frame's
and the ones before it in the Halton sequence, which its neighbours' averages weigh most, wrapping
at the sequence's start. That is Unreal's and Flax's `MissedHistorySamplesCount`.

- **It is a branch in the inject's own `main`; a second pass is the refused alternative.** A pass
  that discarded every cell with history would leave the rest unchanged by construction, which a
  branch on this driver does not promise. But it cost 2.8 ms of GPU on every frame the camera moved,
  at any sample count: each layer attached is a render pass of its own on this GPU, and a second
  walk over the 64 slices is 64 more (0.94 ms even drawn inside the inject's passes). The branch
  costs nothing on a still frame, where no cell misses. `fog-miss-confined` holds that a turn
  changes no cell with history, and `fog-miss-jitter` that TAA's jittered projection, which a
  window builds the volume from, makes no still cell miss.
- **A turn misses rows as well as columns.** A level turn grows a cell's height on screen by
  cos t / cos(t + step), which takes the top and bottom rows near the leading edge past the
  previous frame's vertical edge. The same growth makes a cell read its history from nearer the top
  or bottom than itself, so a missed row's value creeps inward over a turn. `fog-miss-count` holds
  the count to that geometry exactly.
- **It costs what a sample costs.** In silent's rain and clustered lights it is +1.0 ms of GPU while
  turning, +0.23 at two samples and +2.6 at eight. A cut misses every cell and pays the full N that
  frame.

`FROXEL_MISS_COUNT` compiles the same `main` a second time as `--fog-miss-probe`'s count: every
other way out discards and the several-samples branch writes and returns, so the count is that
branch rather than a second statement of when it is taken. It is drawn with colour writes off
under an occlusion query, and made on first use.

**Not every band at the leading edge of a turn is the fog's.** Wet ground's reflection has one of
its own to avoid; see Rain, "Wet ground in SSR".

## Cloud shadow

`cloud_shadow_frag` (spec 11.39): a 256² R16F map of the sun's transmittance through the deck,
built by `sky_clouds_march` from the same wind offset as the march, so the shadow cannot sit a frame
off the deck it belongs to. The fog reads it as a visibility term on the sun.
- **Read by shearing** a froxel up to the shell, which is exact, not approximate, for a horizontal
  layer.
- **Tiled by `GL_REPEAT`**: with detail off the density field is periodic over the shape noise's
  own 8 km, so one tile is the whole world.
- **World-anchored while the deck is camera-anchored**, on purpose: a dapple you can never walk out
  of is wrong in a way the sky's own anchoring is not.

## Clustered specular probes

`probe_project_frag` + `include/probe_specular.glsl` (spec 11.70), the shader half of the
reflection probe set. The engine half -- authoring, the box fade's direction, capture, the atlas
and residency -- is in `docs/lighting.md`, Reflection probes and The lighting atlas and residency.

- **`probe_project_frag`** resamples one roughness level of a probe's prefiltered cube into one
  octahedral atlas row: row r reads mip r. There is no second GGX importance sampler, since
  `ibl_prefilter_cubemap` has already run the kernel.
- **`probe_specular.glsl`** owns the blend, the box weights and the atlas addressing. **Its sampler
  is a function parameter**, which GLSL 330 allows, and that is what lets the lit surface and SSR
  share one weight formula across two programs with different unit ledgers: `pbr_frag` passes
  `giAtlasTex`, `ssr_frag` its own.
- **The row geometry comes from a table the CPU publishes**, never recomputed in the shader. Every
  probe's column has the same rows, and a shader copy of the halving rule would force the row size
  to a power of two (C shifts, GLSL divides by exp2, and those agree nowhere else).
- **The descriptors are in probe order.** Only loaded resident probes are published, in the order
  of their indices rather than their slots, since the blend sums in descriptor order. `pbr_frag`
  takes the set on `probeInfo.x >= 1`, after the single-probe cube path, which a world of one probe
  keeps.
- **A box's fade is a fraction of each axis's own half-size** (`probeBoxWeight`), so one fade is a
  different distance on each axis: the longest axis reaches furthest, and on a short axis the fade
  can be a centimetre or two, so a door hung a little way into a wall can fall outside every box
  (spec 13.23). The engine floors it at a thousandth of each half-size.

### Seeing a miss

What falls outside every box reflects the global environment, dimmed by `envVisible` to the room's
brightness but keeping the sky's colour and evenness, so in the shaded frame a miss is only a faint
grey sheen. **`RENDER_MODE_PROBE_COVERAGE` (14) is how to see one**: the albedo, turned magenta by
`envShare`, the share of the reflection no probe answers.
- Outdoors everything is magenta, and right; indoors any magenta is a miss.
- It shows the fallback, not a wrong probe. A surface inside another room's box reads as covered;
  that is found from outside, where anything not magenta has taken a room's probe.
- Two misses no box can avoid: a wall at 45 degrees, and the top of a pitched roof under a box that
  must reach the ridge.

`probe-set-coverage` holds the view to the albedo view inside a box and to pure magenta outside
every box.

### Streaming and capture, the parts the shaders see

The probes and the GI volumes stream (spec 13.24); `docs/lighting.md` has the residency rules and
the capture rules. Four consequences are easy to miss:
- **Two runs with different histories agree to a code, not a bit.** An item comes back to its home
  slot bit for bit, but different histories place items differently.
- **A probe's readiness reads the shadow tiles' assignment.** The tiles are assigned once a frame
  before any capture, and a held block counts when it is whole or when its light emits, since the
  capture's own depth pass then draws it whole. `scene_capture_ready` states the whole rule, the
  texture loader's idleness included.
- **A `capture_hidden` caster is never held by a kept shadow face**, and the movers pass redraws
  every dynamic face inside a capture burst. Hiding the surface alone would leave its shadow in the
  probes through the cached tiles.
- **A capture's gathered light list shades like the per-cell lists** (`docs/lighting.md`,
  Captures): a light reaching nothing in a face is windowed to an exact 0, so only a light with no
  range differs, by its tail past the cull radius, which the cells would cut off.

### GI classification, the shader half

`classify` (set by `create_gi_volume_spaced`; `docs/lighting.md`, GI volumes) switches off a probe
inside a wall, which otherwise sees out both sides and lights the room with the sky beyond. In the
opening sweep each probe also captures back faces only; `gi_project_frag`'s mode 2 compares their
depth with the front faces' over the sphere and, past a quarter of directions, writes 0 into the
irradiance tile's alpha, which the sampler multiplies the probe's weight by.
- **A re-convergence projects through an RGB-only mask**, so the alpha is the classification's
  alone. Blended at the re-convergence's 0.97, an in-wall probe creeps back to half weight over a
  few sun changes.
- **The back faces are drawn depth only** (spec 13.32, `_render_capture_depth`), and that is the
  shaded pass's depth to the bit only while every discard in `pbr_frag` sits above the depth-only
  exit and nothing writes `gl_FragDepth`.
- **Counting back faces from the gather's own rays**, DDGI's production form (Majercik et al. 2021,
  §6), would need one capture drawing both sides, which changes what a probe sees through one-sided
  geometry: a look to choose, not a drop-in.

## Clustered decals

`include/decals_ubo.glsl` (spec 11.73): marks projected through an oriented box onto whatever
surface lies inside it, selected by a per-froxel 16-bit mask on the grid the lights and probes use,
the boxes in a UBO on binding 10. It declares no sampler: a decal image is a tenant of
`materialArray`, so the array arrives as a function parameter, as the probe atlas does.

- **Spliced into `pbr_frag` in two halves, and the split is not tidiness.** The accumulation and
  its albedo land above the `renderMode == 6` return, because that view is what a gate reads painted
  bytes through, and a mark invisible there could not be asserted as an exact code. The normal,
  roughness and occlusion land at the wet-sand seam, where those three first exist.
- **After the wetness and after `sampleLayeredSurface`.** A poster covers the water rather than
  being soaked by it, and because the layered blend comes first, the composite cache and the VT bake
  need no change, unlike roads.
- **The loop's trip count is the cap, broken on a dynamically uniform count**, so a scene with no
  decals takes one branch. The moment and depth-only passes skip it, since a mark changes neither
  coverage nor depth.
- **The facing test stops a projector smearing its image down every wall it grazes**, and it reads
  the geometric normal: relief answering "which way does this surface lie" would let a normal map
  punch holes in a poster.
- **It takes its own derivatives rather than sharing `ddxWorld`.** Hoisting that pair to serve both
  cost 11% of the opaque pass on scenes with no decals, against 3.1% for the whole feature: two
  vec3s live across the shader body are register pressure every fragment pays, where recomputing
  them is two instructions. `prepass-crossover`, which reads the prepass as a ratio of the opaque
  pass, is the arm that sees a uniformly heavier `pbr_frag`; attribute its reading against master
  before acting on it, since its noise floor moves between sessions.

## Layered surfaces

`include/layers.glsl` + `include/triplanar.glsl` (spec 11.60): N material layers blended per texel
from a splat map, height-weighted so gravel interlocks with sand rather than averaging into mud.
They are sampled through a world-aligned projection, because a ground plane's UV is top-down and
stretches by 1/cos(slope) on exactly the slopes worth looking at. No sampler is declared: the layers
are tenants of `materialArray`. The path is gated on `layerCount`, 0 for every material that has not
asked, so an unlayered material is an exact identity.

- **The splat's coordinate space is a material field, not a policy.** World XZ cannot address a
  vertical surface, a mesh-local reading makes the weights swim across a moving prop, and terrain
  writes UV1 as a literal zero, so no one space serves every material.
- **The triplanar cutoff is floored against the largest weight.** A fixed cutoff collapses past
  sharpness 10.05 and returns a NaN normal into the HDR target.
- **Every tap is `textureGrad`.** The skips are fragment-varying flow, where an implicit-LOD fetch
  is undefined in GLSL 3.30; `pbr_frag` and `stochastic.glsl` follow the same rule.

## Roads

`roadReshapedWeights` in `layers.glsl` + `include/roads_ubo.glsl` (spec 11.68): a world-XZ polyline
that overrides the splat weights toward one of the material's own layers, by a capsule SDF masked
with `smoothstep(halfWidth, halfWidth + feather)`.

- **Applied before the height blend, which is the whole design.** A road blended in afterwards is a
  stripe painted over the ground; one reshaped here is made of a layer, and its shoulder
  height-interlocks with what it runs over.
- **The bake inherits roads by calling `sampleLayeredSurface` unchanged**, so the fallback, the
  pages, the per-texel path, the dominant index and the detail term all follow with no bake code,
  and `--no-layers-vt` stays lossless, which drawing roads into the atlas FBO would cost.
- **Positions ride `authoredPos`.** The lane write is a constant-lane `equal()` compare, not `w[L]`,
  which would be the dynamic local index the file's header rule forbids.
- **Segments live in a UBO** on binding 8 (`GpuRoadsBlock`, 1104 bytes), so roads cost no sampler,
  and the path is a structural no-op at road count 0.
- **`material_roads_ensure` (`roads.c`) is the one owner of the armed state.** It arms one
  road-bearing material per scene and refuses a UV1 splat outright.
- **Roads join `MaterialLayersVtKey` as the struct itself**, so a width change re-bakes the cache and
  a field added to `MaterialRoad` joins the key by construction.
- **Authored per material** as `roads: [{points, width, feather, layer}]`, the scene format's only
  array of arrays; a malformed vertex refuses the whole road. There is no `CSceneRoad`: an authored
  road is a `MaterialRoad`, so the applier assigns rather than copying field by field.
- **`roads_polyline_distance_xz` is the CPU twin** the scatter rejects against. The two cannot share
  a token, so the gate arms are what hold them together.

**The caps are small; check them before planning against roads**: 4 roads of 16 points
(`MATERIAL_MAX_ROADS`, `MATERIAL_MAX_ROAD_POINTS`), one road-bearing material per scene, evaluated
per fragment on the per-texel path. The default path is the cache, which pays nothing, since roads
reach it through the bake. A road network wants paged content, not a hundred polylines.

## Composite cache

`layers_vt_bake_frag` + `sampleCachedSurface` in `layers.glsl` (spec 11.66, roadmap D10 stage 1):
a world-XZ-splat
layered material's blend baked once into two 2D textures over its splat domain, then read back at
`3 + 2A` taps against the per-texel path's 9/17/25.

- **The bake is the macro**: which layer wins where, with every layer at its top-mip mean via
  `layerGrainFrozen`, so grain never lives in the atlas at any resolution.
- **The read**: two filtered macro taps; one `texelFetch` of the dominant-layer index riding the
  atlas alpha, never filtered, since bilinear between indices passes through every index between
  them; and one triplanar detail tap of that layer's own maps. The detail albedo is a ratio to the
  layer's mean, exactly 1 on a flat map, which keeps the flat case byte-exact; roughness and AO are
  additive deviations.
- **The pair rides units 0 and 1** (`albedoTex`/`normalTex`, provably unread when layered) and is
  not a `materialArray` tenant: the canonical-size rule would promote every other layer to the
  atlas's resolution.
- **Resolution** is derived, half a unit a texel clamped to 256..2048 (forest: 2048², 42.7 MB).
- **UV1-splat materials never take the cache**, which keeps the per-texel path the general mechanism
  and `layer_fixture`'s four arms its coverage.
- **Flags**: `--layers-vt-res` overrides the resolution, `--no-layers-vt` is the bisect lever, and
  `--layer-blend-at <frame:value>` is the one headless way to make the by-value bake key go stale,
  since a fresh process always bakes from the final authored values.

### Pages

The cache is paged (spec 11.67):
- **A 64-slot guttered page atlas** of 256² tiles at 4x the fallback's density. The density is a
  ratio, which bounds the virtual grid at 34² for every domain size.
- **Its page table is a UBO** on binding 7 (`vt_pages.glsl`, `layers_vt_pages.h`), so it costs no
  sampler.
- **The page pair rides units 3 and 4** (`clearcoatNormalTex`/`heightTex`), freed by a refusal: a
  layered material refuses both maps. Refusing the height map is right on its own, since parallax
  would run a dead march on a layered surface, and one that could discard.
- **Residency** is frustum prediction sorted by (seen, distance, id), with the capacity clamp as
  governor, a bakes-per-frame budget and hysteresis-guarded farthest eviction, all owned by
  `material_layers_vt_ensure` on frame N-1's pose.
- **The vote pass** (`layers_vt_feedback_vert/frag`): paged surfaces rasterize page IDs into a
  small depth-tested target, read back through a fixed-latency PBO ring, always the slot from
  `VT_FEEDBACK_RING` (4) frames ago, never whichever fence signalled. Content stays a pure function
  of frame history, so every arm is deterministic. At 4 it does not stall, and the whole stage costs
  about 0.23 ms of GPU.
- **On today's content pages add nothing over the fallback**: the macro's finest content is the
  splat, which the fallback already over-resolves. Their use is content drawn into pages through
  the bake FBO, such as roads and decals.

Flags: `--no-layers-vt-pages` (the unpaged cache), `--no-layers-vt-feedback` (prediction alone),
`--layers-vt-page-slots` (the churn knob), `--layers-vt-page-budget` (bakes per frame, capped at
8), `--layers-vt-probe` (residency counters and a digest), and
`--cam-at <frame:ex,ey,ez,tx,ty,tz>` (a teleport, the worst case no walk produces).

## Rain

Spec 13.9, with wind, glass, drips and puddle relief from spec 13.12. The engine half -- `rain.c`'s
one knob `rate_mmh` and the physics it derives, the wetness state, `Rain.travel`, the drip lines and
how the wind carries the rain -- is in `docs/lighting.md`, Rain. The numbers the CPU and the GPU
must agree on are `include/rain_constants.glsl`, which both languages compile.

### Cover

An orthographic depth map along the rain's travel, a tenant of the punctual shadow array (its own
layer past every light's, a fixed 1024² corner of it), so the rain costs the lit surface no sampler.
- **The render matrix and the lookup matrix differ by that corner** and must not be confused.
- **`include/rain_occlusion.glsl` answers it two ways**: one tap for a point (streak ends, splash
  droplets, fog cells), and for a surface a PCSS-shaped blocker search, then a 12-tap Vogel disk as
  wide as the rain's spread over the blocker's height. A regular grid of taps wider than a texel
  prints ghost copies of a lamp arm as dry stripes on the pavement.
- **The map is stored with the shadow pass's slope bias** (`RAIN_MAP_SLOPE_BIAS`), so anything that
  needs the surface rather than a side of it takes the bias back out. Left in, splashes bury
  themselves 5 cm under a windy surface.
- **In wind, the cover and the splashes read the gust cycle's mean**, `1 - a + a * 5/16` of the
  peak, and the drops this instant's air: a cover that followed the gusts would crawl across the
  ground.
- **The cover is asked on the struck side.** A glass roof shelters its own underside, so asked where
  it is seen it never beads from below.
- **A vertical pane does not shelter its own lee** in the map. The rain meets it nearly edge on,
  and the slope-scaled offset stores it deeper than the point behind it; an arm reading the struck
  side has to use a flat canopy.
- `shadow_rain_cover_ask/answer` is the CPU half, a texel read back through a fixed-latency ring.

### Streaks and splashes

**Streaks** (`rain_vert/frag`) are stateless: a drop's position is a function of its index and the
rain's clock, wrapped into three nested boxes that follow the camera. They are drawn from PostFX's
late draw, after TAA, since a temporal filter ghosts or clamps away thin, fast streaks that the
velocity buffer sees as still; in HDR, each fogged at its own depth. Four things a plausible frame
hides:
- **Opacity**: a streak's is its drop's image area over the area drawn, times the drops it stands
  for. At physical opacity and a drawable count the rain is invisible.
- **What a drop refracts** is a mipped copy of the frame, its field being about 165 degrees.
  Showing the sky or the fog behind it draws black sticks in front of every lit wall.
- **`streak_sheen`** lifts what a drop refracts. Physical rain at night is invisible outside its
  lamps.
- **Glints** are Rayleigh's ringing, and only drops over a millimetre ring visibly. Given to every
  drop, the small-drop majority stripes each streak with twenty to thirty bands.

**Splashes** are the droplets a crown throws, drawn by the same program as small drops on ballistic
arcs, from a world-anchored grid of slots firing at the flux of drops over 1 mm.
- **Each lands on the map's surface found back up the rain's own path**; straight up, in any wind,
  would land it tens of metres downwind.
- **On water a splash lands on the water's plane**, met by walking up the travel from the map's
  surface. Water is not in the occlusion map, so a crown thrown from the bed under it would never
  break the surface.

### Wet surfaces

`include/rain_surface.glsl`, one call in `pbr_frag` under `PBR_FEAT_RAIN` (bit 64), runs the
shore's model, `include/wet_surface.glsl`: porosity darkening, a Lekner-Dorf hue power, a film that
smooths and flattens.
- **Only up-facing surfaces hold a film, and open ground holds 0.6 of one.** With a full film
  everywhere every wet surface is already the mirror a puddle is, and no puddle reads.
- **Puddles** are value noise under a level, on ground within eight degrees of level, only where the
  rain lands, with a one-pixel waterline and rings from `include/rain_ripples.glsl`. `water_frag`
  takes the rings too, through the cover on unit 9, or water under a roof would ring.
- **Darkening the bed under standing water is rejected**: the film's power already darkens at least
  as much as a flat water surface's internal reflection does.
- **Puddle relief** (`PBR_FEAT_RELIEF`, bit 128) moves a puddle's depth by the material's height map
  about the map's own mean, read from its coarsest mip. It is a bit of its own because it declares
  `heightTex`, which under `PBR_FEAT_RAIN` every material in a wet world would pay. Layered
  materials are refused it: their unit 4 is the virtual-texture page.

### Wet ground in SSR

A wet texel marks the normals alpha below -1, as `-(1 + film)`; the catcher keeps (-1, 0). Every
reader asks `include/ssr_marker.glsl` rather than testing the value: a sign test hands wet ground
the catcher's treatment.

- **SSR reads the texel's own roughness** from the aux buffer, and water's F0.
- **The reflection replaces the pixel's share of the environment's** rather than lerping the pixel,
  which would dim the diffuse by a Fresnel already applied. The replacement is made in the split
  composite (spec 13.21, `wetReflection` in `spec_occ_composite_frag`):
  `spec * SO * (1 - coverage) + reflection`, every term non-negative. The pair carries the
  reflection with its Fresnel and the coverage bare. What it replaces is the share the composite
  put back under its occlusion, not the unoccluded share, which would take puddles under a roof
  black.
- **It must be made before TAA.** Subtracted after TAA, it takes this frame's render-res specular
  out of a frame TAA has resolved and, at a render scale, upscaled. The two agree on open ground and
  not on a thin ripple or an object's edge, which go below zero as dark outlines and rings.
  `rain-ssr-taa` and `rain-ssr-taau` hold the frame at no pixel below zero; every other rain arm runs
  without TAA, where the two orders agree.
- **The late fold** (`ssr_fold_wet_frag`), on a wet frame, folds only the catcher, its tent averaging
  the catcher only with its own class, and gives wet and unmarked pixels nothing: a puddle's coverage
  taken as a catcher's Fresnel would lerp the floor beside it.
- **The pair is the frame before's** (`PostFX.ssr_prev`), the only trace there is when the
  composite runs, read at the pixel less the surface's velocity and scaled to this frame's
  pre-exposure.
  - Only the frame immediately before is one velocity away, so the first wet frame, or the first
    after SSR was off, shows the environment's reflection alone.
  - It is kept by handle, since which buffer holds it (the raw trace, the accumulator's side or the
    denoised one) depends on that frame's switches.
  - A reflection moves with parallax rather than with its surface, which the SSR accumulator's
    reprojection already accepts.
  - The trace reads the canvas after TAA, which holds the frame before's wet reflections, so a
    puddle can see another's, bounded by its Fresnel.
  - The read is bilinear and blind to class, so where wet ground meets the catcher it can take a
    catcher's pair as wet; no fixture has that border.
- **Off the frame before's edge, wet ground takes the nearest of its reflections** (spec 13.46).
  The strip a turn reveals at its leading edge has no previous position on the frame. Falling back
  to the environment's share alone, what `--no-ssr` shows, it flashes for a frame: beside it, at the
  edge, the trace's hits fade (`edgeFade`; wet ground reflects within its own column, so its hits
  sit at the edge too) and the reflection probe takes their weight, which under a fogged night sky
  is far darker than the environment's share.
  - **The borrow fades out between `WET_EDGE_REACH` (0.1 of the frame off its edge) and twice
    that.** A cut has every position far off the frame, and lending one edge column to the whole
    wet ground lands further from the true frame (1.24 codes on a 90° cut) than the environment's
    share does (0.46). Past the reach a cut takes that share; a turn of several times the gate's
    rate is borrowed whole.
  - **`rain-ssr-edge` holds it, and needs a probe.** Its rule: the wet pixels at a turn's leading
    edge must not sit at exactly their `--no-ssr` value. With no probe the pair at the edge is
    nearly empty, and a fallback cannot be told from a reflection.
- **A replacing hit is seen through the global height fog** along its own reflected segment
  (`reflectedThroughFog`), because the environment it replaces is baked through the same fog
  (`include/env_medium.glsl`, `IBLResources.reflect_fog`). Unfogged, a puddle on a foggy street
  mirrors the houses across it as holes.

### The medium

Past the streaks the rain is a medium in `froxel_inject_frag` (see Atmosphere): the Marshall-Palmer
extinction times `mist`, lit through the drops' phase (`include/rain_phase.glsl`, which the streaks
share) with its own lobe, `mist_forward_g`. It folds in by extinction like the local volumes, is
absent under cover, and comes in only past the outermost streak box, inside which the streaks
already stand for every drop.

### Drops on glass

`include/rain_glass.glsl`, pure arithmetic and no sampler:
- **The beads**: two layers of jittered cells.
- **The running drops**: lanes with a stick-slip motion and a trail.
- **The optics**: a paraboloid cap tilts the normal and clears the smear's roughness. Each drop is a
  lens that shifts the transmission sample, and a dark rim marks total internal reflection past
  water's critical angle; that rim is what outlines a bead against a lit street.

Two things a plausible frame hides (the cover's side and a pane's lee are under Cover):
- **The lens is not faded where the beads are unresolved, and the tilt is.** Sub-pixel beads still
  spread the view inside a pixel, which is what raises the transmission's mip and makes a wet
  window across a room a diffuser.
- **Zero-weight steps are skipped**, not run with a weight of zero. A renormalised normal is not
  bit-equal to the one it started as.

### Drips

A third instance range in `rain_vert`, after the splash droplets. Each drip is a hanging bead that
grows as the cube root of its time, then a fall under linear drag, then the splash crown, shared
with the splashes through `crownDroplet`. The fall time is solved by Newton on both sides.
- **Drips skip the cover**: a drip comes off the edge that made the cover.
- **They fall straight down.** Drifted with the open air, a gutter's drop crosses the wall into the
  room behind it.
- **They take light from area panels as small sources**, or the drips past a lit window take none
  from its tubes.
