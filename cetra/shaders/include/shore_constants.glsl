/*
 * THE SURF'S CALIBRATION -- every number the run-up model stands on, and why it is that number.
 *
 * INCLUDED BY BOTH LANGUAGES, which is the whole reason this file exists apart from shore.glsl.
 * The run-up is evaluated on the GPU by shore.glsl and on the CPU by shore_runup.c, because the
 * swash solver runs host-side and GLSL cannot be linked into C. That duplication is structural
 * and cannot be removed -- but the CONSTANTS are the half of it that actually drifts, since a
 * tuned number is what gets changed, and a hand-mirrored copy of one is a silent divergence.
 * `procedural/water_waves.c` is the precedent and the warning: two copies, nothing comparing
 * them, and the tree has carried that gap for several specs.
 *
 * So the numbers live once, here, and both sides include this file. What stays duplicated is
 * the arithmetic, which is reviewable by eye in a way a bare literal is not.
 *
 * WRITING FOR TWO PREPROCESSORS. Everything here is a `#define` with an `f`-suffixed literal,
 * which is valid in C and in GLSL 1.20 and later (this tree is 330 core). The suffix is
 * load-bearing on the C side: an unsuffixed `9.81` is a double there, and would promote every
 * expression it touches to double precision that the GPU copy does not have. Array data is
 * defined per element, since C and GLSL spell an array initialiser differently -- each language
 * assembles its own array from these, and the VALUES are what had to be shared.
 *
 * Nothing here may use a type, a function or a qualifier: `const`, `float[3](...)` and `static`
 * are all specific to one side. Numbers only.
 */

#define OCEAN_GRAVITY 9.81f

// The tongue climbs a third of a period after the bore reaches the shoreline.
#define OCEAN_SWASH_LAG 2.1f

// The SETUP: the fraction of the run-up that is a standing rise of the mean water level at the
// shore, about which the swash oscillates. Stockdon's R2% is setup plus swash, and the setup
// term of it (0.35 beta sqrt(H0 L0)) is about this fraction of the whole on a beach like this
// one. Oscillating the tongue about the STILL level instead retreated its tip 0.6 R below still
// water at every trough -- metres offshore on a shallow slope -- and drained the whole swash
// zone to a film with a step of water at its seaward edge.
#define OCEAN_SWASH_SETUP 0.4f

// The bore stops shortening below this depth, in metres: the shallow-water wavenumber goes as
// 1/sqrt(h) and would be infinite at the waterline. The lens takes its phase from here.
#define OCEAN_SURF_MIN_DEPTH_M 0.05f

// Below this beach slope the travel-time form has nothing to stand on; it only bounds the
// divide, for a bed with no shore in it.
#define OCEAN_SURF_MIN_SLOPE 0.01f

// Waves per set, and how much the sets modulate the run-up: the biggest wave of a set is
// 1 + this times the mean and the smallest 1 - this.
#define OCEAN_SURF_GROUP_WAVES 6.5f
#define OCEAN_SURF_GROUP_MOD 0.4f

// Sine of the angle the arriving crest makes with the shore. See shore.glsl's header.
#define OCEAN_SURF_OBLIQUE 0.3f

/*
 * THE INCIDENT WAVE IS SEVERAL TRAINS, not one, and this is what stops the swash reading as a
 * moving line however well its waveform is shaped.
 *
 * A sea arriving at a beach is a BAND of frequencies. One train is one sinusoid in space and
 * time, and a single sinusoid IS a straight front travelling along the shore -- there is
 * nothing for it to interfere with, so every point does the same thing a fixed phase apart.
 * Three trains at INCOMMENSURATE periods, each refracted to its own residual angle, superpose
 * into a front that is high where they agree and barely arrives where they oppose, and because
 * the ratios are irrational the pattern never repeats.
 *
 * The ratios are close together because refraction has already sorted the band -- what reaches
 * the swash is the peak and its neighbours, not the whole spectrum. Weights sum to 1, so the
 * combined crest is still 1 where they align and the amplitudes stay the amplitudes they say
 * they are.
 *
 * Shallow water is NON-DISPERSIVE, which is what makes this nearly free: every frequency
 * travels at sqrt(gh), so all three share one travel time to shore and the expensive part --
 * the bed fetch and the sqrt -- is computed once.
 *
 * Per element rather than as an array, because the two languages disagree about how to write
 * one; each assembles its own from these.
 */
#define OCEAN_TRAIN_FREQ_0 1.0f
#define OCEAN_TRAIN_FREQ_1 0.79f
#define OCEAN_TRAIN_FREQ_2 1.27f
#define OCEAN_TRAIN_WEIGHT_0 0.48f
#define OCEAN_TRAIN_WEIGHT_1 0.30f
#define OCEAN_TRAIN_WEIGHT_2 0.22f
// Residual angle off the wind, radians, after refraction has turned each train shoreward.
#define OCEAN_TRAIN_ANGLE_0 0.0f
#define OCEAN_TRAIN_ANGLE_1 0.38f
#define OCEAN_TRAIN_ANGLE_2 -0.26f

// The bore waveform is ((1 + cos psi) / 2)^2, whose mean over a period is 3/8; this takes it
// back to zero mean and a unit crest. (The skew below moves the mean by under 2%, which is not
// worth a second constant.)
#define OCEAN_BORE_MEAN 0.375f

// The swash leans harder than the bore that made it: uprush takes about a third of the cycle
// and the backwash the other two thirds, where a bore is a travelling front whose face is steep
// but whose back is not that long.
#define OCEAN_SWASH_SKEW 0.85f

/*
 * SWASH CAPTURE, and this is the nonlinear one -- the reason real run-ups are so unequal.
 *
 * A bore arriving while the previous backwash is still draining runs into it: the two collide,
 * the incoming one is slowed and absorbed, and it stops well short. One arriving just after the
 * beach has drained meets nothing and runs much further. So the run-up depends on the wave
 * BEFORE it, which is what stops a swash zone looking metronomic.
 *
 * Approximated by scaling the run-up down by how big the previous cycle was -- a big one leaves
 * more water on the face to climb through. That needs no stored state, because the previous
 * cycle is this expression evaluated one primary period ago, and with incommensurate trains
 * that is a genuinely different number every wave.
 */
#define OCEAN_SWASH_CAPTURE 0.45f

/*
 * BEACH CUSPS, from a subharmonic edge wave (Guza & Inman 1975).
 *
 * A shore traps waves that run ALONG it, and the one a normally-incident sea excites most
 * strongly is the subharmonic: half the incident frequency, standing, so alternate waves run
 * high where the one before ran short. That is the mechanism behind the scalloped shoreline
 * every real beach has, and it is what breaks the STRAIGHTNESS of the front where the trains
 * above break its uniformity.
 *
 * Its spacing is not a look constant: edge waves have their own dispersion relation,
 * omega^2 = g k sin(beta) for the lowest mode, so at half the incident frequency the cusp
 * spacing is 8 pi g sin(beta) / omega^2 -- 29 m for a 6.7 s sea on a 1:10 face, which is where
 * real cusps on such a beach are. Bigger swell gives wider cusps, as observed.
 */
#define OCEAN_CUSP_AMP 0.35f

/*
 * THE DRYING WINDOW: how far back the wet sand looks, and in how many steps.
 *
 * Shared because the SOLVER has to size its tip history from them. The shader marches back
 * SHORE_TAP_PERIODS of wave time in SHORE_TAPS steps; the solver records one history slot per
 * step of that same march, so the ring it keeps spans exactly the window the taps can ask
 * about. Sized in wave periods rather than in seconds because the thing being sampled is the
 * arrival of waves, so a longer-period sea should look proportionally further back.
 *
 * These were the two halves of a real defect and are shared for that reason: the solver used to
 * record a slot per FRAME, so its history spanned a fifth of a second while the taps reached
 * back tens of seconds. Every tap but the first ran off the end of the ring, nine taps returned
 * two distinct values, and the accumulated wet history collapsed into the two-tone step it was
 * built to replace -- which made the sand worse with the simulator on than off.
 */
#define SHORE_TAPS 9
#define SHORE_TAP_PERIODS 3.0f
