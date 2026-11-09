"""The water, beach and water-night groups:

  water               water surface (determinism, absorption, shoreline, reach; specs
                      11.32-11.35)
  beach               the shoaling bed the eye can see (surf ring, turquoise, bound; spec 11.44)
  water-night         the sea after dark: which light it takes, and what it does with none (spec
                      11.84)
"""

import base64
import functools
import json
import math
import struct
import re
import os
import subprocess
import sys

from gatelib.harness import (
    ASSET_KINDS, NO_HALOS, RENDER, ROOT, _probe_rows, _run, asset, asset_subpath, render,
)
from gatelib.images import (
    _SRGB_TO_LINEAR, _absorb_box_rgb, _box_luma_dense, _linear_luma, _linear_rgb, _read_ppm,
    compare,
)
from gatelib.profiling import _profiled_run
from gatelib.scenes import (
    WATER_BOUNDS_AWAY, WATER_DRY_OVER, WATER_FIXTURE, _cscn_camera, _projector, cscn_copy,
)


# --no-shadows on BOTH sides of the on/off comparison, and it is load-bearing.
# `--water` also suppresses the shadow catcher, and this fixture has one (its .cscn
# asks for a sky, and render.c turns that into shadow_catcher = true). The catcher
# quad covers about 62% of the frame here and overwrites normals, aux and albedo
# across all of it, so an arm that let it appear on one side only would measure the
# catcher: the first version read 299,283 px of 480,000, which is 62.4% -- the
# catcher's own coverage, and a number water could have contributed nothing to.
# Clearing shadows makes the catcher absent in both frames and leaves `--water` the
# only difference.
WATER_NO_CATCHER = ["--no-shadows"]
WATER_PIN = ["--no-auto-exposure", "-E", "1.0"]
# water-downwell (spec 13.4). The fixture's lit floor under still water, read through the
# identity tone curve so a ratio of two frames is a ratio of radiance, with bloom and AO off
# because each spreads light between pixels at different depths. The key and the water
# numbers mirror the fixture's own and are what the ratio is solved against.
WATER_DOWNWELL_FIXTURE = "water_downwell_fixture.cscn"
WATER_DOWNWELL_FLAGS = WATER_PIN + WATER_NO_CATCHER + ["--tonemap", "linear", *NO_HALOS,
                                                       "--no-ssao"]
WATER_DOWNWELL_KEY = (-0.3, -0.85, -0.43)
# The same scene under a key 33 degrees up rather than 58. The depth a row implies must not
# depend on which: charged the vertical depth rather than the refracted path, it comes out
# 15% shallower under the low key.
WATER_DOWNWELL_LOW_KEY = (-0.6, -0.55, -0.58)
WATER_DOWNWELL_ANGLE_TOL = 0.05
WATER_DOWNWELL_IOR = 1.333
WATER_DOWNWELL_ABSORB = (0.4, 0.2, 0.1)
# The dry box sits on the wedge above the waterline, the wet box on its submerged face.
WATER_DOWNWELL_DRY_BOX = (0.42, 0.31, 0.58, 0.42)
WATER_DOWNWELL_WET_BOX = (0.42, 0.55, 0.58, 0.95)
# Rows too dim to divide, or dimmed by less than this in red, are not read.
WATER_DOWNWELL_MIN_LINEAR = 0.01
WATER_DOWNWELL_WET_RATIO = 0.97
# The three channels' depths, per row, as (max - min) / mean. The same depth solved three
# ways from three extinctions a factor of four apart: a tint, a vertical path or a lost
# Fresnel term pulls them apart by far more than 8-bit rounding does.
WATER_DOWNWELL_SPREAD_MAX = 0.08
# The surface itself comes from the fixture's own `water` block since spec 11.33 phase
# 5, so nothing here asks for water on the command line -- only for the properties an
# individual arm needs to differ. The photometric arms keep the default lighting: their
# boxes read open water, the catcher is suppressed under water anyway, and holding
# shadows out changes what the bed radiates and flattens the ratios they measure.
WATER_FLAGS = WATER_PIN
# The on/off pair for the liveness arm ONLY, with the catcher held out of both. The off
# side needs --no-water explicitly now: omitting a flag no longer turns the surface off,
# because the scene file is what asks for it.
WATER_LIVE_ON = WATER_PIN + WATER_NO_CATCHER
WATER_LIVE_OFF = WATER_PIN + WATER_NO_CATCHER + ["--no-water"]
# Re-measured with the catcher held out of both sides.
WATER_LIVE_MIN_PX = 40000

# The spectral path, at an extent and level where the cascades are resolvable by the
# grid. Built from ONE base so each arm below varies exactly the flag it names --
# the first version differed in model, extent AND level at once, which meant a
# cascade chain transforming to zero would still have moved 330k px on the level
# change alone and passed the arm meant to catch it.
# The level is composed in rather than substituted out, so no arm depends on the
# position of a value in a list.
WATER_FFT_CORE = ["--water-extent", "70", "--no-auto-exposure", "-E",
                  "1.0"] + WATER_NO_CATCHER
WATER_FFT_FLAGS = WATER_FFT_CORE + ["--water-level", "0.6", "--water-waves", "fft"]
WATER_GERSTNER_REF = WATER_FFT_CORE + ["--water-level", "0.6", "--water-waves", "gerstner"]
# Level above the fixture's eye (y 1.35), so the camera is under the surface.
#
# SHALLOW, and GERSTNER, and both are forced by geometry rather than chosen (spec 11.35).
# The camera is pitched 11.9 degrees down, so the highest ray that can reach a surface
# overhead leaves at 9.1 degrees and the nearest visible interface sits at
# clearance / tan(9.1) = 6.25x the eye's depth. At the old level of 3.0 that is 10.5 units,
# where red is already down to e^(-0.45*10.5) = 1% transmitted -- the whole band the boxes
# read is saturated body colour and there is no gradient left to be monotone. So the depth
# has to be small for this arm's claim to be observable at all, and at a small depth the
# FFT sea state (metre-scale waves) puts the eye INSIDE the wave envelope, which is the
# projected grid's own degenerate case. Gerstner's authored amplitude of 0.06 is two orders
# under the clearance, so the framing is unambiguous.
WATER_SUBMERGED_FLAGS = WATER_FFT_CORE + ["--water-level", "1.9", "--water-waves", "gerstner"]
# One flag apart now, so this is the wave model alone.
WATER_FFT_LIVE_MIN_PX = 20000
# The submerged INTERFACE, read on the underside of the surface where the optical
# path is the sight line itself (water_frag takes `path = length(ViewPos)` below the
# plane). Boxes walk away from the nearest point of the interface: measured
# 0.3432 / 0.2617 / 0.0966, which is 4.0 to 18.7 units of sight line.
#
# The boxes themselves have never moved; the CONFIGURATION under them has, three times,
# and the reason is worth knowing before touching either. They are fixed screen positions
# reading a ratio off whatever the surface puts there. 11.32 phase 2's underwater medium
# saturated the open-water band they started in (all three flat at 0.053, the body
# colour). 11.33 phase 4's per-mode phase seeding reshaped the interface and flattened the
# ramp to 1.69x. 11.35's projected grid stopped drawing the near-overhead surface the
# clipmap covered, which is what moved the level and the wave model above.
#
# The ORDERING is the durable half of this arm; the endpoint ratio is not, and a floor
# well under the measurement is the only honest way to write it.
WATER_UNDER_BOXES = [(0.40, 0.030, 0.60, 0.055),
                     (0.40, 0.105, 0.60, 0.130),
                     (0.40, 0.160, 0.60, 0.185)]
# Measured end-to-end 3.55x. Ordering is asserted separately to catch an inversion the
# endpoints alone would hide.
WATER_UNDER_TOTAL_MIN = 2.0

# The medium acting on GEOMETRY, which is the thing that rendered as though in air
# before phase 2. Three bands down the submerged ramp: it is opaque, and with the eye
# under the surface there is no water interface between it and the camera, so the ONLY
# thing that can absorb it is the froxel volume.
#
# As fractions ALONG THE RAMP, sorted by distance from the eye (_water_fog_bed_boxes), not
# as screen rows near-to-far. Which end of the wedge is far is a property of which way it
# faces: turning it round made "further down the frame" mean nearer, and a falling R/B
# became a rising one (0.8312 / 0.9074 / 0.9821) with nothing wrong but the transcription.
# Measured 1.1448 / 0.9692 / 0.7910 = 1.45x once the order comes from the camera.
WATER_FOG_BED_SPANS = ((0.15, 0.25), (0.45, 0.55), (0.75, 0.85))
WATER_FOG_BED_TOTAL_MIN = 1.25

# Reach invariance (spec 11.35). The arm that catches "the water stops short of the
# horizon", which the clipmap did by 5.1 degrees while a comment claimed otherwise.
#
# Formulated so it needs NO horizon row, no camera parameters and no tuned threshold: if
# the surface reaches the horizon, multiplying the nominal extent by four orders of
# magnitude cannot move its top edge, because the edge is already at the vanishing line.
# If it does move, the extent was the reach.
#
# The top edge is "the highest row that differs from the same frame with no water" --
# above the surface both frames show identical sky, so agreement IS absence.
#
# Measured on the clipmap build: 207 -> 136 of 600 rows, a 71 px move. Independently
# predicted at 0.348 and 0.217 of frame height from the camera geometry, which is the
# reason to trust the instrument rather than the threshold.
WATER_REACH_BIG_EXTENT = "200000"
# A few pixels of slack for the lattice's own quantisation at the horizon row, well under
# the 71 px defect and far under the 78 px the geometry predicts for this framing.
WATER_REACH_MAX_MOVE_PX = 4
# Left strip, clear of the fixture's ramp. Shadows are held out of both sides so the
# ramp's cast shadow cannot read as water.
WATER_REACH_STRIP = (0.02, 0.30)

# Spectral motion vectors (spec 11.33 phase 4). The camera is static in headless, so
# camera velocity is exactly zero and motion blur is a no-op on anything that reports
# no motion of its own -- which is what makes this a velocity measurement rather than a
# blur-pass liveness one. Measured 0 px before the previous cascades were retained,
# 116,796 after; the Gerstner path, which always had it, reads 77,150 on the same pair.
WATER_FFT_MOTION_MIN_PX = 20000

# The .cscn water block (spec 11.33 phase 5). Two arms, because reproducing the CLI
# frame at 0 px proves only that the authored values match the defaults -- a key that
# parsed and was never applied would pass that.
#
# absorption has NO command-line flag, so a frame that moves when only absorption is
# authored proves the value reached the shader through a path the CLI cannot fake.
# Measured 262,950 px; red extinction 0.45 -> 2.2 is a strong change and the floor is a
# long way under it.
WATER_CSCN_ABSORB = {"absorption": [2.2, 0.5, 0.35]}
WATER_CSCN_MIN_PX = 20000

# The spectral sea state reaches the seeding (spec 11.42). Wind speed rather than any of
# the other four because it moves the spectrum furthest per unit of authoring, and 4 m/s
# against the default 11.5 is a different SEA rather than a dimmer one -- the spectrum
# takes its height from the wind, so there is no amplitude to turn down and this is the
# only way to ask a spectral ocean for calm.
#
# Both sides author `waves` so the wave model is not the variable. No flag can set any of
# the sea state, which is what makes this the only instrument on that path -- the same
# argument water-cscn makes for absorption.
#
# The WIND SEA's wind speed since spec 11.48, not the water's: the swell is its own train
# now and keeps its own wind, which is the point -- before this, most of the height came
# from a swell that ignored this number, and the arm was measuring through that dilution.
WATER_SEASTATE_REF = {"waves": "fft"}
WATER_SEASTATE_CALM = {"waves": "fft", "windSea": {"windSpeed": 4.0}}
WATER_SEASTATE_MIN_PX = 20000

# The transform carries the variance the seeding predicted (spec 11.42). Measured 0.69 to
# 0.99 over the three bands at two sea states: this is ONE realisation of a random field on
# a finite grid, so its sample variance deviates from the ensemble by roughly
# sqrt(2/N_modes), and the deficit is largest exactly where the energy concentrates into
# fewest modes -- the long band at 22 m/s reads 0.69, the same band at 11.5 reads 0.96.
#
# The band is wide because that spread is physical and narrow enough to be decisive: a
# transform that collapsed to zero reads 0.00, and one whose modes landed at the wrong
# wavenumbers moves the slope ratio by the square of how wrong they are.
#
# What this CANNOT catch, and water-fft-impulse exists for: a missed fftshift rearranges
# the field in space and leaves every variance here untouched.
WATER_FFT_VAR_MIN = 0.5
WATER_FFT_VAR_MAX = 1.5
# Mirrors WATER_CASCADE_COUNT (water.h). Asserted rather than assumed: a probe that
# printed two rows would otherwise be read as two passing cascades.
WATER_CASCADES = 3
# The ripple mip levels water-ripple-mips holds to the variance bounds: those carrying at least
# this fraction of the whole band's slope variance, which is levels 1-4. Measured 0.94-0.98
# through level 6 and 0.90 at level 7; levels 8 and 9 carry under 1e-5 of the band and read 2.25
# and 0.64, a ratio of two numbers too small to mean anything.
WATER_RIPPLE_MIP_FLOOR = 0.01

# The transform against its closed form (spec 11.42). Measured 0.0 and 1.9e-7 on the two
# modes -- fp32 scratch, so this is round-off over 14 stages and nothing else.
#
# The threshold is four orders above that and still decisive, which was verified by
# BREAKING the shader rather than by argument: dropping the conjugate from the inverse
# twiddle (water_fft_frag.glsl) takes mode_err to exactly 2.0, the largest error a
# unit-magnitude value admits. In that same run every variance ratio was unchanged to four
# decimals and twelve pixel arms passed, water-fft-det and water-fft-live among them --
# which is the blind spot D3 item 3 names, measured.
#
# dc_err alone would NOT have caught it: a constant field is invariant under the
# conjugation, which is why the second mode exists.
WATER_FFT_IMPULSE_MAX = 1e-3

# The analytic sun lobe (spec 11.42). The fixture's OWN framing cannot see it, and that is
# geometry rather than an oversight: the streak lands off to one side and largely behind the
# ramp. So the arm puts the sun dead ahead and lower, which is the geometry a glitter path
# needs and the one every photograph of one was taken in.
#
# NOTE the nested "sun" object, which is the shape parse_env reads. This override carried
# FLAT sun_elevation/sun_azimuth keys until 11.43 and so moved nothing at all: the arm was
# measuring the fixture's own sun and passing on it, reporting 26,334 px and a 1.52x box
# against the 258,246 px and 13.90x it reads once the sun actually moves.
#
# 22 degrees puts the specular point at eye_y/tan(el) = 3.0 units in front of the eye, which
# is clear of the ramp's near edge at z 2.2 -- and stays clear under BOTH ramp orientations,
# since the flip only moves where the ramp goes under water further away.
WATER_GLITTER_SUN = {"sun": {"elevation": 22.0, "azimuth": 180.0}}
WATER_GLITTER_CAMERA = {"eye": [0.0, 1.6, 7.0], "target": [0.0, 0.55, 0.0], "fov": 45}
# Spectral, because the lobe's width is the slope the surface stopped resolving and the
# spectral path is where that is a measured quantity rather than four dropped octaves.
WATER_GLITTER_WATER = {"waves": "fft"}
WATER_GLITTER_MIN_PX = 5000
# The box is DERIVED (_water_glitter_box) rather than written down. These two place it
# below the horizon, as a fraction of frame height: the lobe is a streak from the specular
# point to the horizon, and screen-space compression piles most of its energy into the rows
# just under that line -- measured, half the brightening lands in rows 0.318 to 0.368 against
# a horizon at 0.3189. The box this replaced spanned 0.28 to 0.40, so it straddled the
# horizon, was 32% SKY, and could not tell a bright sea from a bright sky.
WATER_GLITTER_BAND = (0.010, 0.060)
WATER_GLITTER_HALF_W = 0.12
# A ratio, not `lum_on > lum_off`. A strict inequality is satisfied by a lobe of 1e-9, so
# the arm it replaces passed on a glitter path that was effectively switched off -- which
# is exactly the state the dead sun override left it in.
WATER_GLITTER_RATIO_MIN = 1.15

# Persistent foam (spec 11.42). A ROUGH sea, because whitecaps are selected from the
# horizontal map folding and the default 11.5 m/s state folds rarely enough that the signal
# is a few per cent -- 20 m/s is the same instrument with the effect above its own noise,
# and it is only reachable at all because spec 11.42 made the sea state authorable.
#
# 90 frames, not 30: the accumulator has to have something to remember. At 30 the trail is
# barely longer than the crest that made it.
#
# `level` 1.1 clears the ramp's highest point of 0.9, so nothing in the frame is dry and
# the box below needs no notch cut round the wedge -- see _water_persist_variant.
WATER_PERSIST_SEA = {"waves": "fft", "windSea": {"windSpeed": 20.0},
                     "level": 1.1, "extent": 70.0}
WATER_PERSIST_CAMERA = {"eye": [0.0, 3.2, 7.0], "target": [0.0, 0.9, -8.0], "fov": 42}
WATER_PERSIST_FRAMES = 90
# Open water only -- above the ramp's crest and below the horizon band, so no dry geometry
# and no sky is inside it. The ramp is irrelevant to this arm: crest foam is a property of
# the wave field, where the SHORE band next door is a property of the bed, needs
# --water-bed dome, and is identically zero on this fixture without one.
#
# WATER_PERSIST_* rather than WATER_FOAM_*, for all five of this arm's constants: the
# WATER_FOAM_ family below belongs to water-shore-foam, a different arm reading different
# boxes on a different scene. The first version of this box reused WATER_FOAM_OPEN_BOX
# outright, the later definition won, and the arm measured the shore box on an open-water
# framing and read 0 -- a green-looking 0/0 rather than an error. Renaming one constant
# fixed that instance; keeping the whole set under its own prefix is what stops the next.
#
# The top edge is DERIVED from the horizon (_water_persist_box) rather than written down.
# It was 0.20 against a horizon that sits at 0.2254 once `level` is raised to 0.6, so the
# top 17% of the box was sky and the foam counts it reported were partly a count of bright
# sky -- which moves with the sun and not with the accumulator.
WATER_PERSIST_HORIZON_MARGIN = 0.015
# Re-swept for the debug-view classifier at spec 11.47 and KEPT at 0.60. A binary selection
# mask has no brightness to dilute in the near field the way the old luma/R-B read did, and
# the trend INVERTED below this -- a narrower box nearer the horizon reads a showier ratio
# (2.5x at 0.40 in one sample sweep) but on an unstable few hundred pixels that the exact
# derived box some frames reads as flatly 0, which is a box too small to trust rather than
# a stronger effect. 0.60 is the smallest window that reads a STABLE non-zero count on the
# without-history frame across repeated runs.
WATER_PERSIST_BOX_BOTTOM = 0.60
# Re-measured for the debug-view classifier at spec 11.47: 2,196 -> 2,503 px, 1.14x-1.15x
# across repeated runs (small jitter, same order every time). Lower than the pre-11.47
# luma-classifier reading of 1.85x, and that drop is a real, expected interaction rather
# than instrument noise: S4's crest-height gate tightens WHERE a fold is allowed to BIRTH
# into the trail, so less of what used to inflate the with-history count -- folding
# anywhere, crest or trough -- gets recorded there at all. The ratio still says persistence
# measurably adds foam; it says less of it than before S4 made the selection choosier,
# which is the point of S4. Set under the measured floor rather than at the old bar, which
# a correct S4 cannot clear.
WATER_PERSIST_RATIO = 1.10
# WATER_PERSIST_MIN_PX (an absolute floor on how many pixels the SHADED frame differed by
# between WITH and WITHOUT history) is gone as of spec 11.47: it was a floor on the LOOK,
# not on persistence, and broke every time crest opacity or colour moved.
# An absolute floor under the WITHOUT-history count, because a ratio has no scale: 1 -> 3
# foam pixels is 3.00x and passes any ratio bar while describing a frame with no whitewater
# in it at all. Recalibrated for the debug mask's pixel counts, which run at a different
# scale than the luma classifier's: measured 2,196 px without history, so this sits well
# under that rather than snug against it.
WATER_PERSIST_MIN_FOAM_PX = 800
# And `level` is a field both paths can set, so authoring it must land in exactly the
# same place the flag does. 0 px or one of them is lying.
WATER_CSCN_LEVEL = 0.9

# Whitecap coverage against Monahan & O'Muircheartaigh's W = 2.95e-6 * U10^3.52 (spec
# 11.47) -- nothing else in this suite checks the sea against a measurement taken outside
# itself. Read at NADIR: a pinhole images a plane perpendicular to its axis affinely, the
# 1/cos^3 of the plane's foreshortening cancelling the cos^3 of the perspective solid
# angle, so pixel fraction of the debug mask IS areal fraction with no crop box to place.
# Two sea states rather than one, since a single point proves nothing about the RELATION.
#
# 8 and 14 m/s, not 20: measured, coverage PLATEAUS well short of Monahan past here -- 14
# m/s reads 2.05% against a predicted 3.20% (0.64x, inside tolerance) and 20 m/s reads
# 1.99% against 11.21% (0.18x, nowhere close), two sea states that are barely different in
# this engine's own numbers despite a 3.5x gap in what they should produce. The likely
# cause is structural rather than a tuning miss: S4's crest gate is a SIGMA-normalised
# threshold, so it selects roughly the same FRACTION of a Gaussian-ish field regardless of
# how rough the sea actually is, where Monahan's real climb comes from a rising fraction of
# the surface actively breaking. Recording this as a known gap rather than forcing the arm
# to cover it -- steepening the wind-speed response is out of scope for what 11.47 set out
# to fix (the default sea's look), and chasing it here risks the opposite mistake this spec
# started from: tuning a constant to a number instead of to a measurement.
WATER_COVERAGE_STATES = (8.0, 14.0)
# waves/fft only -- no bed, and therefore no extent to author: oceanBed's whole body is
# behind `if (bedAvailable == 0) return early` (ocean.glsl), and this arm never installs
# one. The wind sea's windSpeed is the one thing that varies per state.
WATER_COVERAGE_SEA = {"waves": "fft"}
WATER_COVERAGE_FRAMES = 90
WATER_COVERAGE_CAM = ["--cam-eye", "0,300,0", "--cam-target", "0,0,0",
                     "--cam-up", "0,0,-1", "-F", "20"]
# --no-bloom so a mask value of exactly 1.0 cannot bleed a halo past the edge of a foam
# patch and inflate the count -- the classifier's own docstring claims no opacity or
# tonemap curve stands between it and the shader's selection, which this makes exactly true
# rather than almost true.
WATER_FOAM_DEBUG_ON = ["--water-foam-debug", "1", *NO_HALOS]
# Monahan & O'Muircheartaigh 1980: whitecap area fraction W = A * U10^B, U10 in m/s.
WATER_MONAHAN_A = 2.95e-6
WATER_MONAHAN_B = 3.52
# 0.5x-2x, not a tight band: Monahan's own data scatters roughly that much across the field
# campaigns it was fit to, and this measures one seeded FFT patch against a mean curve --
# an exact match would be suspicious rather than reassuring. Well inside the original
# data's scatter, so it catches a selection that is wrong by an order of magnitude without
# chasing this engine's particular sea state onto the published mean.
WATER_COVERAGE_RATIO_MIN = 0.5
WATER_COVERAGE_RATIO_MAX = 2.0

# Flag precedence, read through --water-probe because it is the only output that reports
# what the surface actually IS rather than what it looks like. Every case here was broken
# when the arm was written, and each failed silently:
#
#   --no-water plus any sibling flag  the siblings all set args.water, so the surface was
#                                     freed and then immediately recreated as a default
#   a negative flag on its own        --no-water-caustics / --no-water-coverage /
#                                     --water-bed none each turned the feature ON
#   --water-waves gerstner            a bool could only write its true half, so it could
#                                     not override an authored "waves": "fft"
#
# The pixel arms could not see any of it: water-cscn varies only `level`, and every
# --no-water in the suite is unaccompanied.
WATER_PRECEDENCE = [
    # (label, extra flags, authored waves, expect a surface, expected model or None)
    ("--no-water", ["--no-water"], "gerstner", False, None),
    ("--no-water + sibling", ["--no-water", "--water-waves", "fft"], "gerstner", False, None),
    ("--no-water + extent", ["--no-water", "--water-extent", "40"], "gerstner", False, None),
    ("authored fft", [], "fft", True, "fft"),
    ("flag overrides fft", ["--water-waves", "gerstner"], "fft", True, "gerstner"),
    ("flag overrides gerstner", ["--water-waves", "fft"], "gerstner", True, "fft"),
]

# The CPU wave query (spec 11.33 phase 5), read through --water-probe. There is no other
# way to see it: the GPU surface leaves no number behind, so without the probe the seam
# buoyancy is meant to consume would ship with nothing checking it at all.
#
# `residual` is the assertion that matters. The query has to INVERT the Gerstner
# horizontal map to answer about a WORLD position rather than about a wave parameter, and
# an unconverged inversion still returns a point on the surface -- just not the one over
# the query -- so every other number it prints looks fine either way. Measured worst
# 0.00146 world units against a 0.06 amplitude.
WATER_PROBE_MAX_RESIDUAL = 0.01
# The train has to actually be evaluated: a flat answer would satisfy the residual
# perfectly. Measured spread 0.154 over the 16 probes, against amplitude 0.06 summed over
# four falling octaves (0.106 is the analytic ceiling either side of the level).
WATER_PROBE_MIN_SPREAD = 0.03

# How far the GPU probe may sit from the CPU query on the GERSTNER path (spec 13.1).
# Both evaluate the same four-octave train and invert the same horizontal map, so what
# separates them is float precision and the two solvers' own tolerances -- this is a
# correctness bar rather than a quality one and belongs tight. Provisional until phase 2
# measures it; raising it needs a reason written beside the number.
WATER_GPU_MAX_GAP = 0.001

# Shoaling, over the analytic dome --water-bed installs (spec 11.33 phase 6). The two
# real Tier 3 consumers cannot measure this: apps/forest is not pixel-deterministic and
# apps/tree's floor is tens of thousands of pixels. This config is 0 px twice.
#
# Read as surface ROUGHNESS -- the per-pixel spread of luma in a box. A shoaled surface
# has lost its displacement, so its reflection is uniform where an open-water one is
# broken up; that is what a wave looks like to a box of pixels.
WATER_DOME_BED = ["--water-bed", "dome"]
# No WATER_NO_BED: no bed is the DEFAULT, so the base flag set already is the no-bed
# frame. Spelling it out rendered a byte-identical duplicate of a frame two arms already
# had on disk, which is a third of a minute of suite time for a restated default.
# The dome's radius is 0.62 of the extent, so at extent 70 it reaches 43 units and the
# far field is beyond it -- flat bed, column 9 units, shoal exactly 1. Which is why the
# arm can assert the open box does NOT move: a local bed that calmed the whole sea
# would be a global amplitude knob wearing a bed's name.
WATER_SHOAL_MID_BOX = (0.06, 0.40, 0.30, 0.55)
WATER_SHOAL_OPEN_BOX = (0.06, 0.17, 0.30, 0.26)
# WITH THE SURF OFF, on both sides. Shoaling is the vertex stage shortening waves over a
# rising bed; the surf is the separate incident wave the shore builds on top of it, and over
# this dome it covers the whole bar -- the fixture authors the library's default ocean
# (11.5 m/s over 120 km, Hs 3.69 m) onto a bar 0.6 units proud in 9 units of water, so every
# bit of it is inside the breaker zone. That is correct and it leaves this arm nothing
# unbroken to measure. Isolated rather than accommodated, the same way the foam it already
# excludes is; water-surf below is what tests the thing being switched off here.
WATER_NO_SURF = ["--no-water-surf"]
# Measured 0.63x on the mid box originally, and 0.80x since 11.44 gave the shore band a
# swash. That is not the shoaling weakening: the mid box sits where the shoal factor is at
# or near ZERO -- fully shoaled, which is exactly where a swash belongs -- so foam now
# covers part of what this reads, and foam is FLAT where the roughness it measures is not.
# Narrowing the swash does not help, because 1 - smoothstep(0, w, 0) is 1 for every w.
#
# Relaxed rather than left at a bar it passes by rounding. The claim still holds and the
# arm still fails an inert bed; what it wants is its mid box moved onto the RAMP rather
# than the shallow end, which is a re-derivation of the box and not a threshold change.
WATER_SHOAL_MAX_RATIO = 0.88
WATER_SHOAL_OPEN_TOL = 0.05

# The surf (spec 11.44). Measured 40k+ px of surf over the dome and 20k+ between two frames
# a quarter of a wave period apart; the no-bed side is an exact 0 and is not a measurement
# with slack in it -- oceanSurf returns early on bedAvailable == 0.
WATER_SURF_MIN_PX = 15000
WATER_SURF_MIN_MOVED = 5000

# The shore foam band, on the GERSTNER path -- which is what isolates it. Crest foam is
# selected from Jacobian compression and the Gerstner map's steepness is clamped so it
# cannot compress, so on that path the shore band is the only foam there is. Measured
# 3,441 px between open water and the swash, 0 in open water, 16,140 in the swash, and 0
# everywhere with no bed.
WATER_FOAM_LUMA_MIN = 0.21
WATER_FOAM_RB_MIN = 0.55
# A FALLING EDGE on the shoal factor since 11.44, not a window: the swash is strongest at
# the water's edge and fades out to sea, so foam is expected at the shallow extreme and its
# ABSENCE there was the defect. The arm asserted zero at both ends and so encoded the older
# model -- with a lower cutoff scaled into metres it left 5.4 m of bare waterline, and the
# sea met the sand at a line.
#
# Nearer is further down the frame here (the camera sits over the crown), so the
# fully-shoaled box is the low one and the open-water box is the high one.
WATER_FOAM_OPEN_BOX = (0.02, 0.10, 0.32, 0.24)
# EVERYTHING BETWEEN the other two, rather than a 5% slice at a written-down row.
#
# This was (0.02, 0.26, 0.32, 0.31) and read 0 px: the band it was cut for had moved a few
# per cent down the frame and the slice was left sitting just above it, asserting the
# absence of foam it was written to find. Nothing about it was wrong except that a thin
# absolute row is the most fragile way to name a region -- its position is a function of
# the camera, the still level and the bed's shape at once, so any of the three moving
# retunes it, and the failure it produces looks exactly like the feature being broken.
#
# Bounded by its neighbours instead. The open box ends at 0.24 and the shoaled box starts
# at 0.55, so this is the rest of the water by construction, and on this fixture that IS
# the shoaling ramp: the dome bed carries the 0.14-2.7 m shoal window over r/radius
# 0.21-0.40, which lands between them. The claim is unchanged -- foam exists where the bed
# shoals, between open water and the swash -- but it can no longer go stale from a shift
# too small to matter.
WATER_FOAM_BAND_BOX = (0.02, 0.24, 0.32, 0.55)
WATER_FOAM_SHOALED_BOX = (0.02, 0.55, 0.32, 0.90)
# Measured 3,441 px over the widened box, against the 1,306 the thin slice was calibrated
# to. Kept at 300 rather than raised to match: the bar's job is to catch a band that has
# gone EMPTY, and a decade of margin is what makes it insensitive to the retunes that
# broke its predecessor.
WATER_FOAM_BAND_MIN_PX = 300
# What the shallow extreme has to carry now. Its own floor rather than the band's, because
# it is a different claim: the band says foam EXISTS where the bed shoals, this says it
# reaches the sand. Measured 25,285 px over a box that is most of the shallows.
WATER_FOAM_SWASH_MIN_PX = 4000
# What the no-bed frame may carry in the band box, and why it is not an exact 0.
#
# The claim is structural and holds exactly: with no bed oceanBed returns shoal 1 everywhere,
# so `band` is identically 0, and Breaking is gated on bedAvailable -- there IS no shore foam
# without a shore, in the shader rather than as an observation. What is not exact is the
# READER: _water_foam_px thresholds on luma and red/blue, and a specular glint off a Gerstner
# crest can pass both. Measured 6 such pixels, scattered singles across rows 0.40-0.55 rather
# than anything clustered.
#
# The thin box this replaced read 0 by missing them, not by their absence, so the exact
# assertion was luck rather than rigour. A band that had actually leaked would be hundreds of
# pixels and contiguous, which this still catches with two decades of room.
WATER_FOAM_NO_BED_MAX_PX = 20

# The surface's mesh structure (spec 11.35). A large extent with the level at the eye's
# height is the framing that stresses near AND far at once, which is what the grid has to
# resolve simultaneously.
WATER_CLIP_FLAGS = ["--water-waves", "fft", "--water-extent", "500",
                    "--water-level", "0.0", "--no-auto-exposure", "-E", "1.0"] + \
                   WATER_NO_CATCHER
# Exact structure: ONE lattice, one draw, no instancing, no rings -- 256^2 cells = 131,072
# triangles. Integers, so there is no floor to measure. The count is deliberately the same
# as the clipmap's 5 levels of a 128 grid (32,768 + 4 * 24,576), so this arm asserts the
# projected grid spends the budget it replaced rather than buying its reach with triangles.
WATER_GRID_DRAWS = 1
WATER_GRID_INSTANCES = 1
WATER_GRID_TRIANGLES = 256 * 256 * 2
# A T-junction crack is a HOLE, so what it shows is whatever the water was covering --
# exactly, because no water fragment was written there. So the test is a per-pixel
# distance from the same frame rendered with no water at all: everything the surface
# does moves a pixel AWAY from that background, and only a hole leaves it untouched.
#
# Two earlier versions of this arm were both proxies and both broke. R/B measured FOAM,
# because bare bed (R/B ~1.0) and whitewater (0.92) are equally near-neutral -- it read
# 4,705 of 16,128 pixels "cracked" on a frame with no crack in it. A luma CEILING then
# worked only while foam stayed darker than bare bed, and the moment the sea state got
# rougher (spec 11.33 phase 4) foam reached 0.67 against bare bed's 0.50 and the two
# populations crossed. Distance-from-background cannot cross: it is the definition of
# the defect rather than a correlate of it.
#
# Measured on a crack-free frame, the closest any water pixel comes to the background
# is 0.133 per channel; the floor here is a third of that.
WATER_CRACK_MIN_DELTA = 0.04

# Far-field filtering (spec 11.35 phase 2). A projected grid's distant cells cover more than
# a wave period, so evaluating the field at full detail there samples one arbitrary phase per
# cell -- which is the speckle band the horizon gained the moment the surface reached it.
# --no-water-lod is the control: it reports a zero footprint, which is full detail on every
# band, and reaches the pre-phase-2 frame at exactly 0 px.
#
# Just under the horizon, clear of the ramp. Well below the water's top edge at 0.225 of
# frame height, which water-horizon pins to within 4 px, so this box cannot eat sky.
WATER_FAR_BOX = (0.05, 0.25, 0.30, 0.30)
# Spatial standard deviation in that box, which is what aliasing IS: a filtered surface
# returns nearly one value across a box that far away, an unfiltered one returns noise.
# Measured 0.0314 unfiltered against 0.0090 filtered, a 3.5x drop.
WATER_FAR_SD_RATIO_MAX = 0.5
# And the filtering has to be CONFINED to the far field: the near box is byte-identical
# across the same pair, which is what fails if a footprint term leaks into the near field.
WATER_FAR_NEAR_BOX = (0.06, 0.86, 0.20, 0.94)
# The handover, measured as the far field's SENSITIVITY to the authored roughness. Filtering
# moves the resolved slope energy into roughness, so out there the value the author asked for
# has been taken over; with filtering off it still governs. Measured 0.00972 unfiltered
# against 0.00118 filtered, an 8.2x drop.
#
# What this does NOT establish is the DIRECTION -- a handover that drove roughness toward the
# calm value instead of the rippled one would read as the same insensitivity. That half is a
# code fact rather than a measurement here: since spec 11.42 the roughness is
# sqrt(sqrt(authored^4 + removedMss)), which is monotone in a removed variance that only
# rises with footprint, so it cannot move toward calm. (Before 11.42 the same argument was
# one mix toward a WATER_ROUGH_RIPPLED literal, which that spec deleted.) The measurement is
# unavailable here either way: at this fixture's viewing angles the specular share is about
# 2% and roughness barely moves a pixel except at grazing.
#
# The value below is an AUTHORED roughness for the .cscn override, and 0.115 is only what
# the deleted literal happened to be -- nothing in the shader carries that number now.
WATER_FAR_ROUGH_AUTHORED = 0.115
WATER_FAR_ROUGH_RATIO_MAX = 0.35

# The waterline, decided per PIXEL (spec 11.42). A framing the fixture cannot supply and
# is the reason the defect shipped: crests have to close OVER the eye while the still
# level is still below it, which needs an amplitude two orders above the fixture's 0.06
# and a level just under its camera at y 1.35. Authored rather than flagged because
# amplitude has no CLI override.
WATER_WATERLINE_BLOCK = {"level": 1.05, "amplitude": 0.55}
# The band where crests close over the eye, and foreground the change cannot reach.
# Read as a RATIO of the two: the reference box measured byte-identical across the fix
# (0.5057 both sides), so it anchors the crest band against exposure and build drift.
WATER_WATERLINE_CREST_BOX = (0.10, 0.02, 0.90, 0.18)
WATER_WATERLINE_REF_BOX = (0.30, 0.62, 0.70, 0.72)
# Measured 0.1851 with the per-frame split against 0.2477 with the per-pixel one. A
# backfacing crest charged the depth buffer BEHIND the surface is charged the bed's
# distance through water that is not there, so it reads far too absorbed and the
# failing direction is DOWN.
WATER_WATERLINE_MIN_RATIO = 0.22

# Shoreline coverage (spec 11.33 phase 3). The waterline on this fixture runs across
# the ramp in a wave-modulated band; the diff between the two coverage modes lands
# entirely inside these heights and this x range, so the measurement window is where the
# effect is and nowhere else.
#
# Stated as HEIGHTS ON THE RAMP and projected (_water_ramp_band), not as screen rows. The
# band straddles the waterline, which is height 0 by definition -- so it follows the wedge
# when the wedge moves, instead of aiming at where the wedge used to be. The rows these
# produce on the pre-flip ramp are 0.470 and 0.555, which is what they replace.
WATER_SHORE_HEIGHTS = (-0.5535, 0.3241)
# One flag apart at 4x MSAA. Small because most of this edge was never a shader edge: the
# water surface CROSSES the bed here, and the per-sample depth test antialiases a depth
# crossing on its own. Coverage carries the part the depth test cannot see, which is the
# threshold sliver the discard removes.
#
# 1,121 px since the ramp was turned round in 11.43, against 2,980 before it. The effect
# did not weaken -- the sharpest-fall ratio went the right way, 0.846x to 0.756x -- the
# waterline simply moved from z 0.18 to z -1.38, half again as far from the eye, so the
# same sliver covers fewer pixels. The floor follows the geometry rather than pinning a
# count the fixture no longer produces.
WATER_SHORE_MIN_PX = 700
# And it has to be a SOFTENING, not just a change: the sharpest single-row fall across
# the waterline drops. Measured medians 0.0343 (coverage) against 0.0416 (cutoff) =
# 0.823x, so this ceiling sits above the effect while still failing a wash, an offset,
# or a coverage value that came out constant.
WATER_SHORE_FALL_RATIO = 0.92
# Caustics move 221,316 px (spectral) and 241,330 (Gerstner) of a 480,000 px frame on this
# fixture since spec 13.2 -- the refracted-grid pattern darkens the gaps as well as lighting
# the lines, so it reaches the whole shallow bed where the Jacobian heuristic before it moved
# 12,509. Half the smaller: a floor far below the signal passes a regression that loses most
# of the effect.
WATER_CAUSTIC_MIN_PX = 110000
# Calm water is exactly 1 at every texel, give or take the half float the target is stored in:
# measured 0.9995 to 1.0000.
WATER_CAUSTIC_CALM_TOL = 5e-3
# How far a sea's mean may sit from 1. Measured 0.994 Gerstner, 0.979 spectral (the per-cell
# ceiling and cells smaller than a texel both drop light), and 1.032 over the dome, traced to its
# baked bed.
WATER_CAUSTIC_MEAN_TOL = {"gerstner": 0.02, "spectral": 0.03, "dome": 0.05}
# water-caustic-spectrum (spec 13.4). The bare caustic pattern at the reference plane's depth
# (--water-caustic-debug 3), on the fixture's spectral sea under the 13.3 close framing, once
# spread from one trace and once traced in 16 bands of the spectrum.
#
# What is held is what the spread gets RIGHT: every channel carries the same light as the
# traced spectrum's, and about as much colour. Not where the colour sits: at a few metres the
# ripples' light has crossed itself before the floor, the colour lands as specks that move with
# every wavelength, and 8 traced bands against 16 still disagree by 0.7 of the signal -- no
# single trace reaches that, and spec 13.4 chose not to pay for more.
WATER_CAUSTIC_SPECTRUM_FLAGS = WATER_PIN + ["--water-waves", "fft", "--cam-eye", "-5,1.5,5",
                                            "--cam-target", "-5,-1,1", "--tonemap", "linear",
                                            *NO_HALOS, "--water-caustic-debug", "3"]
WATER_CAUSTIC_SPECTRUM_BOX = (0.31, 0.54, 0.59, 0.83)
WATER_CAUSTIC_SPECTRUM_MEAN_TOL = 0.02  # relative, per channel
# The spread's colour RMS over the traced spectrum's: measured 1.08 on this sea and 0.91 on a
# calm one, and 0 with no dispersion at all.
WATER_CAUSTIC_SPECTRUM_CHROMA = (0.7, 1.4)
# water-caustic-relief (spec 13.4). The bed's relief is read off its brightness against its
# neighbourhood, so on the fixture's flat-coloured bed it must leave every uniform patch alone
# -- the wedge's own face, inside the box -- and act only where two materials meet, the wedge's
# edges, which moved 9,641 px when measured.
WATER_CAUSTIC_RELIEF_FLAT_BOX = (0.40, 0.62, 0.60, 0.90)
WATER_CAUSTIC_RELIEF_FLAT_MAX = 1
WATER_CAUSTIC_RELIEF_EDGE_MIN_PX = 2000
# water-specks (spec 13.4). Motes in the column, one lattice cell in 83 at three depths, under
# the 13.3 close framing: present, and SPARSE -- 2,770 px when measured, 0.58% of the frame. The
# ceiling is what catches a hash or a density gone wrong, which fills the water rather than
# dotting it and would pass a liveness count.
WATER_SPECKS_FLAGS = WATER_PIN + WATER_NO_CATCHER + ["--water-waves", "fft", "--cam-eye",
                                                     "-5,1.5,5", "--cam-target", "-5,-1,1"]
WATER_SPECKS_MIN_PX = 500
WATER_SPECKS_MAX_FRACTION = 0.03
# water-ripple-drop (spec 13.4). One tap into the fixture's water at frame 2, inside the square
# the simulation places ahead of the default eye, read back at two frames: the ring must have
# travelled outward and lost height between them, and a second run of the later frame must print
# the same numbers -- the steps are fixed ones from the frame clock, which is the whole claim.
WATER_RIPPLE_DROP_AT = "-2,3,2"
WATER_RIPPLE_DROP_FRAMES = ("20", "50")
WATER_RIPPLE_DROP_MIN_GROWTH = 1.5  # radius at the later frame over the earlier
# Three boxes down the left side, clear of the ramp, at increasing distance.
WATER_ABSORB_BOXES = [(0.06, 0.86, 0.20, 0.94),
                      (0.06, 0.72, 0.20, 0.80),
                      (0.06, 0.60, 0.20, 0.68)]
# The emerged part of the ramp: dry land, and the control that fails if the
# surface draws over it. Heights again, well clear of the waterline at 0 on the dry side,
# so the box is on land whichever way round the wedge faces. Rows 0.420 and 0.460 on the
# pre-flip ramp.
WATER_DRY_HEIGHTS = (0.3968, 0.6470)
# Measured steps are 1.48x and 1.52x, so this is a wide floor under them that
# still fails a constant tint.
WATER_ABSORB_STEP_MIN = 1.25
# Dry land reads 2.07 and water never exceeds 0.33 anywhere in the frame, so this
# sits in an empty band between the two populations.
WATER_DRY_RB_MIN = 1.4


def _water_rb(pix, w, h, box):
    rgb = _absorb_box_rgb(pix, w, h, box)
    return rgb[0] / rgb[2] if rgb[2] > 1e-6 else float("nan")


def _water_closest_to_background(pix, bg, w, h, box):
    """Smallest per-pixel distance to the no-water frame, over a fractional box.

    Per-pixel and a MINIMUM, not a mean: a hole in the surface is one or two pixels
    wide, and any average over the box would swallow it.
    """
    x0, y0, x1, y1 = box
    worst = 1e9
    for py in range(max(0, int(y0 * h)), min(h, int(y1 * h))):
        for px in range(max(0, int(x0 * w)), min(w, int(x1 * w))):
            o = (py * w + px) * 3
            worst = min(worst, max(abs(_SRGB_TO_LINEAR[pix[o]] - _SRGB_TO_LINEAR[bg[o]]),
                                   abs(_SRGB_TO_LINEAR[pix[o + 1]] - _SRGB_TO_LINEAR[bg[o + 1]]),
                                   abs(_SRGB_TO_LINEAR[pix[o + 2]] - _SRGB_TO_LINEAR[bg[o + 2]])))
    return worst


def _water_caustic_probe(extra, scene=None):
    """Run --water-caustic-probe and return (levels announced, one dict of floats per level).

    Each target conserves light, so its mean over the window is the energy check: 1 on any sea,
    and exactly 1 at every texel on a calm one. Every level rather than the first line, since
    there is one line per level (spec 13.3) and reading the first alone would pass a broken fine
    level unseen; the header says how many there are, so a missing one is caught too.
    """
    cmd = [RENDER, "-m", scene or asset(WATER_FIXTURE), "-x", "-f", "10",
           "-W", "200", "-H", "150", "--water-caustic-probe"] + extra
    r = _run(cmd, capture_output=True, text=True)
    announced, levels = 0, []
    for line in (r.stdout + r.stderr).splitlines():
        if not line.startswith("water-caustic-probe ") or "available=1" not in line:
            continue
        parts = line.split()[1:]
        if parts[0] == "header":
            announced = int(dict(p.split("=", 1) for p in parts[1:])["levels"])
        else:
            levels.append({k: float(v) for k, v in (p.split("=", 1) for p in parts[1:])})
    return announced, sorted(levels, key=lambda lv: lv["level"])


def _water_touch_probe(extra, frames, scene=None):
    """Run --water-touch-probe and return its fields as floats, or None if it declined."""
    cmd = [RENDER, "-m", scene or asset(WATER_FIXTURE), "-x", "-f", frames,
           "-W", "200", "-H", "150", "--water-touch-probe"] + extra
    r = _run(cmd, capture_output=True, text=True)
    for row in _probe_rows(r.stdout + r.stderr, "water-touch-probe"):
        if row.get("available") == "1":
            return {k: float(v) for k, v in row.items()}
    return None


def _water_probe(extra, scene=None):
    """Run --water-probe and return (header dict, [row dicts]).

    An empty header means no surface survived the flags, which is a result rather than a
    failure -- the precedence arm asserts exactly that for --no-water.

    The probe prints two kinds of non-grid line -- the `model=` header and the `key=` row
    naming the picked directional (spec 11.84) -- and both fold into `head`. Distinguished
    by the leading token carrying an `=`, not by counting lines: a grid row starts with a
    bare coordinate, so anything that does not is a header of some kind and a third one
    added later needs no change here.
    """
    # Four frames, because the query answers WATER_PROBE_LATENCY (2) passes late and the
    # first answer retires on the third. The header prints `latency` and a `reason`, so a
    # larger constant fails here as reason=unfilled rather than as a missing field.
    cmd = [RENDER, "-m", scene or asset(WATER_FIXTURE), "-x", "-f", "4",
           "-W", "200", "-H", "150", "--water-probe"] + extra
    r = _run(cmd, capture_output=True, text=True)
    head, rows = {}, []
    for line in (r.stdout + r.stderr).splitlines():
        if not line.startswith("water-probe "):
            continue
        parts = line.split()[1:]
        if "=" in parts[0]:
            head.update(dict(p.split("=", 1) for p in parts))
            continue
        row = {"x": float(parts[0]), "z": float(parts[1])}
        row.update({k: v for k, v in (p.split("=", 1) for p in parts[2:])})
        rows.append(row)
    return head, rows


def _water_ramp_edges():
    """The ramp's low and high edge midpoints, read out of the fixture's own glTF.

    The ramp is the only geometry in this scene, and where it meets the water is what every
    shoreline box in this group is placed against. Read rather than transcribed, for exactly
    the reason _cscn_camera is read: turning the wedge round moved the waterline from screen
    row 0.508 to 0.453 and left every hand-written box aiming at the old one, with two of
    them still passing. A box derived from the mesh follows it instead.

    Returned as (low, high) by HEIGHT, so which end is near and which is far comes from the
    file rather than from an assumption this function makes about the orientation.
    """
    with open(asset("water_fixture.gltf")) as f:
        doc = json.load(f)
    raw = base64.b64decode(doc["buffers"][0]["uri"].split(",", 1)[1])
    prim = next(m["primitives"][0] for m in doc["meshes"] if m["name"] == "water_ramp")
    acc = doc["accessors"][prim["attributes"]["POSITION"]]
    view = doc["bufferViews"][acc["bufferView"]]
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    corners = [struct.unpack_from("<3f", raw, base + i * 12) for i in range(acc["count"])]
    lo, hi = min(corners, key=lambda p: p[1]), max(corners, key=lambda p: p[1])
    # x = 0 -- the centre line. The wedge is a rectangle in plan, so its slope is the same
    # along any line of constant x, and every box here is placed by height, not by width.
    return (0.0, lo[1], lo[2]), (0.0, hi[1], hi[2])


def _water_ramp_at(height):
    """The point on the ramp's centre line at a given world height."""
    lo, hi = _water_ramp_edges()
    t = (height - lo[1]) / (hi[1] - lo[1])
    return tuple(lo[i] + t * (hi[i] - lo[i]) for i in range(3))


def _water_fog_bed_boxes():
    """Three bands down the submerged ramp, returned NEAREST first.

    The ordering is computed from the camera rather than written into the row numbers,
    because which end of the wedge is far is a property of which way the wedge faces.
    """
    lo, hi = _water_ramp_edges()
    cam = _cscn_camera(WATER_FIXTURE)
    eye = cam["eye"]
    project = _projector(cam, 400.0, 300.0)

    def at(t):
        return tuple(lo[i] + t * (hi[i] - lo[i]) for i in range(3))

    def dist(t):
        return sum((at(t)[i] - eye[i]) ** 2 for i in range(3))

    bands = []
    for t0, t1 in WATER_FOG_BED_SPANS:
        rows = sorted(project(at(t))[1] / 300.0 for t in (t0, t1))
        bands.append((dist(0.5 * (t0 + t1)), (0.40, rows[0], 0.60, rows[1])))
    return [box for _, box in sorted(bands, key=lambda b: b[0])]


def _water_ramp_band(h0, h1, x0f, x1f):
    """A fractional box spanning two heights ON THE RAMP, in the fixture's own framing.

    Heights rather than depths because that is what the ramp is placed by: the waterline is
    height 0, dry land is above it, and the shoaling bed is below. Which screen row each one
    lands on is then the projection's business and not a constant anyone has to re-measure.
    """
    project = _projector(_cscn_camera(WATER_FIXTURE), 400.0, 300.0)
    rows = sorted(project(_water_ramp_at(h))[1] / 300.0 for h in (h0, h1))
    return (x0f, rows[0], x1f, rows[1])


def _water_persist_box():
    """The open-water band this arm counts foam in, with its top edge on the horizon.

    Derived because `level` is what moves it, and this arm raises the level to drown the
    ramp: a hand-written top edge of 0.20 sat 0.025 of a frame height ABOVE the horizon it
    was meant to sit under, so 17% of the box was sky and the foam counts were partly a
    count of bright sky -- which moves with the sun and not with the accumulator.
    """
    cam = {"eye": tuple(float(v) for v in WATER_PERSIST_CAMERA["eye"]),
           "target": tuple(float(v) for v in WATER_PERSIST_CAMERA["target"]),
           "fovy_deg": float(WATER_PERSIST_CAMERA["fov"])}
    w, h = 400.0, 300.0 # render()'s size; only the projection matters here
    horizon = _projector(cam, w, h)((0.0, float(WATER_PERSIST_SEA["level"]), -1.0e6))[1] / h
    return (0.05, horizon + WATER_PERSIST_HORIZON_MARGIN, 0.95, WATER_PERSIST_BOX_BOTTOM)


def _water_glitter_box():
    """Where the sun's streak lands, derived from the framing the arm authors.

    Anchored to the HORIZON rather than to the specular point, because on a rough sea the
    lobe is a streak running from one to the other and screen-space compression puts most
    of its energy in the rows just below the horizon. The x centre still comes from the
    specular point, so the box follows the sun's bearing instead of assuming it is dead
    ahead -- change the azimuth and the box moves with it.

    Derived for the same reason _cscn_camera exists: a transcribed box keeps passing while
    measuring somewhere else, and that is precisely what happened here.
    """
    eye = tuple(float(v) for v in WATER_GLITTER_CAMERA["eye"])
    cam = {"eye": eye, "target": tuple(float(v) for v in WATER_GLITTER_CAMERA["target"]),
           "fovy_deg": float(WATER_GLITTER_CAMERA["fov"])}
    w, h = 400.0, 300.0 # render()'s size; the projection is what matters, not the count
    project = _projector(cam, w, h)
    with open(asset(WATER_FIXTURE)) as f:
        level = float(json.load(f)["water"]["level"])

    el = math.radians(WATER_GLITTER_SUN["sun"]["elevation"])
    az = math.radians(WATER_GLITTER_SUN["sun"]["azimuth"])
    # sky.c's convention: sun_dir = (cos el sin az, sin el, cos el cos az), pointing AT the
    # sun. Mirror it in the still plane and follow it from the eye to reach the point whose
    # reflection is the sun -- one ray, because for a plane the specular point is analytic.
    t = (eye[1] - level) / math.sin(el)
    spec = (eye[0] + math.cos(el) * math.sin(az) * t, level,
            eye[2] + math.cos(el) * math.cos(az) * t)
    sx = project(spec)[0] / w
    # The same plane at effectively unbounded range. The camera has no roll, so the horizon
    # is one row and any bearing gives it.
    hy = project((spec[0], level, -1.0e6))[1] / h
    return (sx - WATER_GLITTER_HALF_W, hy + WATER_GLITTER_BAND[0],
            sx + WATER_GLITTER_HALF_W, hy + WATER_GLITTER_BAND[1])


# A JSON key as parse_* spells it. NOT [A-Za-z]+: `probe_scene` already exists, and a key
# carrying a digit or an underscore would drop out of the read set, the known set AND the
# fixture set at once -- all three would agree on not having it, so the arm below would pass
# while covering nothing, which is the one way these checks can fail silently.
_CSCN_KEY = r'"([A-Za-z0-9_]+)"'


def _cscn_key_sets(func, block):
    """(keys `func` reads off `block`, keys its known[] tolerates, keys it REFUSES).

    Read rather than transcribed. A copy of any of the three here would be another place to
    keep in step, which is the failure this is meant to catch in the first place.

    Parameterised over the function and the cJSON variable so parse_water and
    parse_wave_train share one reader. Each call still slices ONE function body and ONE
    closed known[], so no assertion is weakened by the sharing -- what would weaken them is
    a single regex spanning both nesting levels, which this is not.

    The refused set is a THIRD category and comes from the same slice because it cannot be
    told apart by shape: a refusal tests for its key with the same `(block, "name")` form
    every accessor uses, so a refused key reads as read. It belongs to neither known[] nor
    the fixture, so without it a retirement drifts both of those at once. Keyed off the
    user-visible message rather than the control flow around it, which makes that sentence
    load-bearing -- delete it and the arm notices.

    Fails LOUDLY on a formatting change: renaming `known`, or a formatter moving the brace
    off `known[] = {`, raises IndexError here rather than returning an empty set.
    """
    src = open(os.path.join(ROOT, "cetra", "src", "cscene.c")).read()
    body = _c_function_body(src, "static void %s(" % func)
    # Every read names the block: get_float(water, "level", ...), get_vec3, get_bool,
    # get_floats, cJSON_GetObjectItemCaseSensitive for waves, and since spec 11.48
    # parse_wave_train(water, "windSea", ...) for each nested train -- which the same
    # pattern catches, because a sub-object is a key of water like any other.
    read = set(re.findall(r'\(%s,\s*' % block + _CSCN_KEY, body))
    known = set(re.findall(_CSCN_KEY, body.split("known[] = {")[1].split("};")[0]))
    refused = set(re.findall(r'"cscene: %s\.(\w+) is gone' % block, body))
    return read, known, refused


def _cscn_wave_train_names():
    """The nested train sub-objects parse_water installs, from the C source.

    Derived rather than written down for the same reason as the key sets: a third train
    added to the parser and authored in the fixture would otherwise keep read == fixture at
    the outer level and never be checked at the nested one.
    """
    src = open(os.path.join(ROOT, "cetra", "src", "cscene.c")).read()
    body = src.split("static void parse_water(")[1].split("\nstatic ")[0]
    return set(re.findall(r'parse_wave_train\(water,\s*' + _CSCN_KEY, body))


def _cscn_wave_train_parse_fields():
    """The CSceneWaveTrain fields parse_wave_train sets a has_ flag on, from the C source."""
    src = open(os.path.join(ROOT, "cetra", "src", "cscene.c")).read()
    body = src.split("static void parse_wave_train(")[1].split("\nstatic ")[0]
    return set(re.findall(r"out->has_([a-z0-9_]+)\s*=", body))


def _c_function_body(src, signature):
    """The braced body of one C function, by brace matching from its signature.

    Bounding a body at the next `static ` instead -- which this file did until
    11.60 -- is only correct when the next function is also static. It was not:
    apply_wave_train is followed by four NON-static functions, so its "body" ran
    to the end of them and the scraper had been reading four extra functions all
    along. It went unnoticed because nothing else in that stretch used the
    src->has_/dst-> idiom, until a layered material's `has_uv_scale` did.
    """
    start = src.index(signature) + len(signature)
    open_brace = src.index("{", start)
    depth = 0
    for i in range(open_brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[open_brace:i + 1]
    raise ValueError(f"unbalanced braces after {signature!r}")


def _cscn_wave_train_apply_fields():
    """Return (fields apply_wave_train GUARDS on, fields it WRITES), from the C source.

    The other three readers are all on the read side -- parser, known[] and fixture. This is
    the only one that looks at the bridge onto the live WaterWaveTrain, which is the one site
    in the chain whose omission is completely silent: it compiles, parses, warns nothing, and
    the authored key simply never arrives.

    Guards and writes are returned separately because they fail differently: a missing `if`
    applies an absent key over the library default, a missing assignment drops an authored
    one, and neither shows up if you only count lines.
    """
    src = open(os.path.join(ROOT, "apps", "render", "src", "cscene_apply.c")).read()
    body = _c_function_body(src, "static void apply_wave_train(")
    return (set(re.findall(r"src->has_([a-z0-9_]+)", body)),
            set(re.findall(r"dst->([a-z0-9_]+)\s*=", body)))


def _water_fft_probe(extra, scene=None):
    """Run --water-fft-probe and return (header, per-band rows, impulse dict, ripple mip rows).

    The header is the probe's own account of what it measured: whether it ran, why not
    when it did not, and how many cascades this build has. Returning it is the difference
    between a caller that can say WHICH of a Gerstner surface, an unseeded spectrum and a
    failed readback it got, and one that can only report an empty list.

    Empty results are a failure at every call site here, unlike _water_probe where
    declining is one of the results: the flag is only ever passed with a spectral surface
    already asked for, so nothing to measure means the surface did not survive the flags.
    """
    cmd = [RENDER, "-m", scene or asset(WATER_FIXTURE), "-x", "-f", "4",
           "-W", "200", "-H", "150", "--water-fft-probe"] + extra
    r = _run(cmd, capture_output=True, text=True)
    head, rows, impulse, mips = {}, [], {}, []
    for line in (r.stdout + r.stderr).splitlines():
        if not line.startswith("water-fft-probe "):
            continue
        parts = line.split()[1:]
        # Dispatched on the line's own tag, not on whether its first field happens to
        # parse as a number -- which made every new header key a change to this parser.
        tag = parts[0]
        fields = dict(p.split("=", 1) for p in parts[1:] if "=" in p)
        if tag == "header":
            head = fields
        elif tag in ("cascade", "ripple"):
            # The ripple band (spec 13.3) is measured the same way at its own size, so it
            # is one more row of the same shape, told apart by its kind.
            rows.append({"kind": tag, **{k: float(v) for k, v in fields.items()}})
        elif tag == "impulse":
            impulse = {k: float(v) for k, v in fields.items() if k != "available"}
        elif tag == "ripple-lod":
            mips.append({k: float(v) for k, v in fields.items()})
    return head, rows, impulse, mips


def _water_glitter_variant(src, dst):
    """Copy the fixture with the sun ahead and low, and the camera facing it.

    Three blocks rather than the water one alone, which is why this is not a
    _water_cscn_variant call: the lobe is a property of where the sun IS relative to the
    eye, so the framing is the instrument and the water block only picks the wave model.
    """
    def mutate(d):
        d["environment"].update(WATER_GLITTER_SUN)
        d["camera"] = dict(WATER_GLITTER_CAMERA)
        # Through the merge even though WATER_GLITTER_WATER is flat today, so it stays
        # correct if it ever gains a train.
        _merge_water_block(d.setdefault("water", {}), WATER_GLITTER_WATER)

    cscn_copy(src, dst, mutate)


def _water_persist_variant(src, dst):
    """Copy the fixture with the ramp DROWNED and the eye lifted clear of the sea.

    Its own camera, like the glitter arm's, because the ramp is what makes a box hard to
    place: a level above the ramp's highest point (0.9) leaves no dry geometry anywhere,
    so every row below the horizon is open water and the box needs no notch cut out of it.
    That also makes the arm indifferent to which way round the ramp faces -- at 0.6 the dry
    wedge sat at rows 0.43-0.61 one way and 0.29-0.34 the other, and a box avoiding both
    had nothing left to measure.

    The eye rises with the water, and further: 2.1 units of freeboard over a 20 m/s sea,
    where 0.25 would let a crest close over the camera and hand this arm the waterline
    branch to measure instead of the foam.
    """
    def mutate(d):
        d["camera"] = dict(WATER_PERSIST_CAMERA)
        _merge_water_block(d.setdefault("water", {}), WATER_PERSIST_SEA)

    cscn_copy(src, dst, mutate)


def _merge_water_block(water, overrides):
    """Apply a water-block override, merging one level into a nested train.

    A nested object (windSea, swell) MERGES rather than replaces, so an arm naming one
    field of a train keeps the fixture's other seven. A plain dict.update swaps the whole
    object for the one-key one, and the seven it dropped then fall back to create_water's
    defaults -- which are the water fixture's own values today, so that mistake would not
    show until the fixture changed.

    One function rather than a rule each override path remembers: it was written as a
    closure inside _water_cscn_variant and _water_persist_variant kept its flat update,
    which is the only other path carrying a nested constant. Every arm that reached
    water-foam-persist ran with seven windSea fields off the library defaults instead of
    the fixture's -- invisible, for exactly the reason above.

    An override naming a train the fixture does NOT author installs it whole, since there
    is nothing to merge into.
    """
    for key, value in overrides.items():
        if isinstance(value, dict) and isinstance(water.get(key), dict):
            water[key].update(value)
        else:
            water[key] = value


def _water_cscn_variant(src, dst, overrides):
    """Copy a .cscn with its water block overridden."""
    cscn_copy(src, dst, lambda d: _merge_water_block(d.setdefault("water", {}), overrides))


def _water_roughness(pix, w, h, box):
    """Per-pixel linear-luma standard deviation in a fractional box, FOAM EXCLUDED.

    A wave, to a box of pixels, is spread: the surface tilts, so the reflection it
    returns varies across the box. A shoaled surface has lost its displacement and
    returns nearly one value. The MEAN would not see this at all -- calm and choppy
    water average to much the same place.

    Whitewater is skipped, using water-shore-foam's own test, and that is not a detail.
    Foam is patchy and bright, so it carries a large variance of its own, and this measure
    cannot tell "the waves here are shorter" from "there is foam here" -- it just reports
    spread. Once 11.44 gave the shore band a swash, the swash covered the very box this
    reads and the ratio INVERTED, 0.34x becoming 1.59x: the arm was reporting the arrival
    of foam as the absence of shoaling. Excluding it measures the water again.

    Returns 0.0 if the box is entirely foam, which the caller reads as no measurement
    rather than as a calm surface.
    """
    x0, y0, x1, y1 = box
    vals = []
    for py in range(int(y0 * h), int(y1 * h)):
        for px in range(int(x0 * w), int(x1 * w)):
            o = (py * w + px) * 3
            r = _SRGB_TO_LINEAR[pix[o]]
            g = _SRGB_TO_LINEAR[pix[o + 1]]
            b = _SRGB_TO_LINEAR[pix[o + 2]]
            if (r + g + b) / 3.0 > WATER_FOAM_LUMA_MIN and r / max(b, 1e-6) > WATER_FOAM_RB_MIN:
                continue
            vals.append(_linear_luma(pix, w, h, px, py))
    if not vals:
        return 0.0
    mean = sum(vals) / len(vals)
    return (sum((v - mean) ** 2 for v in vals) / len(vals)) ** 0.5


def _water_box_max_delta(a, b, w, h, box):
    """Largest per-pixel channel difference between two frames over a fractional box.

    A MAXIMUM, where the arms above take means: this is used to assert that a region did
    not change, and a mean can average a real local change back to nothing.
    """
    x0, y0, x1, y1 = box
    worst = 0
    for py in range(int(y0 * h), int(y1 * h)):
        for px in range(int(x0 * w), int(x1 * w)):
            o = (py * w + px) * 3
            worst = max(worst, abs(a[o] - b[o]), abs(a[o + 1] - b[o + 1]),
                        abs(a[o + 2] - b[o + 2]))
    return worst


# water-bounds-* (spec 13.41). Bounds round all the world must leave the frame alone; bounds
# off it must leave no water at all. The cut runs along x = 0, the plane the fixture's eye and
# target both stand in, so it is the frame's middle column, and a view ray never crosses it:
# what the surface refracts on one side lies on that side. The margin is the refraction's
# screen-space bend, which reads scene colour a few pixels across. "The whole world" is far past
# the horizon the projected grid runs out to: bounds of a kilometre cut the sea off short of it.
WATER_BOUNDS_WHOLE = "-1e20,-1e20,1e20,1e20"
WATER_BOUNDS_CUT = "-1e20,-1e20,0,1e20"
WATER_BOUNDS_MARGIN = 0.04
# An eye under the level, over the fixture's floor, looking along it at the wedge.
WATER_BOUNDS_UNDER_CAM = ["--cam-eye", "0,-0.6,6.4", "--cam-target", "0,-0.9,-1"]
# water-dry-* (spec 13.56): a dry shape is the bounds' rule turned inside out, so its arms are the
# bounds' arms. The cut is a superellipse 10 km across, so square that its edge is the line x = 0
# over the whole fixture, standing on the other side of it from the bounds' cut.
WATER_DRY_AWAY = "1000,1000,0,1,1"
WATER_DRY_CUT = "-1e4,0,90,1e4,1e4,40"
WATER_BOUNDS_ARMS = ["water-bounds-whole", "water-bounds-cut", "water-bounds-eye",
                     "water-dry-away", "water-dry-cut", "water-dry-eye"]
WATER_BOUNDS_RUNS = (
    ("none", ["--no-water"]), ("whole", ["--water-bounds", WATER_BOUNDS_WHOLE]),
    ("away", ["--water-bounds", WATER_BOUNDS_AWAY]), ("cut", ["--water-bounds", WATER_BOUNDS_CUT]),
    ("under", WATER_BOUNDS_UNDER_CAM),
    ("under_away", WATER_BOUNDS_UNDER_CAM + ["--water-bounds", WATER_BOUNDS_AWAY]),
    ("under_none", WATER_BOUNDS_UNDER_CAM + ["--no-water"]),
    ("dry_away", ["--water-dry", WATER_DRY_AWAY]), ("dry_cut", ["--water-dry", WATER_DRY_CUT]),
    ("under_dry", WATER_BOUNDS_UNDER_CAM + ["--water-dry", WATER_DRY_OVER]),
)


def _water_bounds_frames(workdir, scene):
    """`scene` under each of WATER_BOUNDS_RUNS: (frames by name, the first render error)."""
    frames, err = {}, None
    for name, extra in WATER_BOUNDS_RUNS:
        frames[name] = os.path.join(workdir, f"water_bounds_{name}.ppm")
        err = err or render(scene, frames[name], WATER_DOWNWELL_FLAGS + extra)
    return frames, err


def _water_downwell_depths(on, off, w, h, box, key):
    """The depth each channel implies, by row of a fractional box, for water-downwell.

    A row of the fixture's wedge is one depth, so each row is averaged before anything is
    solved: a single pixel's blue channel moves a few per cent in one 8-bit code, which at
    blue's small extinction is a fifth of the depth. Rows whose light is too dim to divide,
    or not dimmed enough to be under the water, are left out. `key` is the direction the
    light shines, as the scene file states it.
    """
    lx, ly, lz = key
    norm = math.sqrt(lx * lx + ly * ly + lz * lz)
    cosi = -ly / norm
    n = WATER_DOWNWELL_IOR
    sint2 = (1.0 - cosi * cosi) / (n * n)
    cost = math.sqrt(1.0 - sint2)
    rs = (cosi - n * cost) / (cosi + n * cost)
    rp = (n * cosi - cost) / (n * cosi + cost)
    transmitted = 1.0 - 0.5 * (rs * rs + rp * rp)
    x0, y0, x1, y1 = box
    rows = {}
    for py in range(int(y0 * h), int(y1 * h)):
        sums_on, sums_off = [0.0] * 3, [0.0] * 3
        for px in range(int(x0 * w), int(x1 * w)):
            a = _linear_rgb(on, w, h, px, py)
            b = _linear_rgb(off, w, h, px, py)
            for c in range(3):
                sums_on[c] += a[c]
                sums_off[c] += b[c]
        if min(sums_off) <= 0.0:
            continue
        ratios = [sums_on[c] / sums_off[c] for c in range(3)]
        if min(sums_off) / (int(x1 * w) - int(x0 * w)) < WATER_DOWNWELL_MIN_LINEAR or \
                ratios[0] > WATER_DOWNWELL_WET_RATIO:
            continue
        rows[py] = [-math.log(max(ratios[c], 1e-6) / transmitted) * cost /
                    WATER_DOWNWELL_ABSORB[c] for c in range(3)]
    return rows


def _water_chroma_stats(pix, w, h, box):
    """Mean linear R, G, B over a fractional box, and the RMS of (R - G, B - G) across it.

    The second is how much colour a frame carries, wherever it sits: water-caustic-spectrum
    holds the caustics' spread to the traced spectrum's amount, not its placement.
    """
    x0, y0, x1, y1 = box
    sums, sq, n = [0.0, 0.0, 0.0], 0.0, 0
    for py in range(int(y0 * h), int(y1 * h)):
        for px in range(int(x0 * w), int(x1 * w)):
            r, g, b = _linear_rgb(pix, w, h, px, py)
            sums[0] += r
            sums[1] += g
            sums[2] += b
            sq += (r - g) ** 2 + (b - g) ** 2
            n += 1
    return [s / n for s in sums], math.sqrt(sq / (2 * n))


def _water_foam_debug_px(pix, w, h, box=(0.0, 0.0, 1.0, 1.0)):
    """(crest-mask px, sea px) in a fractional box, from a --water-foam-debug=1 frame.

    Green marks water at all, red marks the crest band the shader selected -- written as a
    binary override at the end of water_frag's main(), after the real G-buffer writes, so
    this reads the SELECTION rather than the composited colour. No opacity, no tonemap
    curve, no albedo constant stands between this count and what the shader chose.

    The sea count is the denominator coverage needs: a fraction of SEA AREA, not of frame,
    so no crop box has to be placed to exclude the sky or any dry geometry in shot -- see
    water-whitecap-coverage, which reads the default full-frame box at nadir where that
    denominator is exact.
    """
    x0, y0, x1, y1 = box
    sea = 0
    foam = 0
    for py in range(int(y0 * h), int(y1 * h)):
        for px in range(int(x0 * w), int(x1 * w)):
            o = (py * w + px) * 3
            if pix[o + 1] > 127 and pix[o + 2] < 127:
                sea += 1
                if pix[o] > 127:
                    foam += 1
    return foam, sea


def _water_foam_px(pix, w, h, box):
    """Pixels in a fractional box that read as whitewater: bright AND near-neutral.

    Both halves are needed. Bright alone catches a specular hit off calm water and the
    sky above the horizon; neutral alone catches the dry ramp, which is the mistake the
    first version of water-crack made. Foam is a bright grey with the sky in it, and
    water at this depth is decisively blue, so the red/blue ratio separates them where
    brightness cannot.
    """
    x0, y0, x1, y1 = box
    n = 0
    for py in range(int(y0 * h), int(y1 * h)):
        for px in range(int(x0 * w), int(x1 * w)):
            o = (py * w + px) * 3
            r = _SRGB_TO_LINEAR[pix[o]]
            g = _SRGB_TO_LINEAR[pix[o + 1]]
            b = _SRGB_TO_LINEAR[pix[o + 2]]
            if (r + g + b) / 3.0 > WATER_FOAM_LUMA_MIN and r / max(b, 1e-6) > WATER_FOAM_RB_MIN:
                n += 1
    return n


def _water_top_row(wet, dry, w, h, min_run=6):
    """Highest row where the wet frame differs from the dry one over a run of pixels.

    A run rather than a single pixel so one stray LSB cannot report an edge; and a
    difference rather than a colour test so the arm needs to know nothing about what water
    or sky look like.
    """
    x0, x1 = int(WATER_REACH_STRIP[0] * w), int(WATER_REACH_STRIP[1] * w)
    for y in range(h):
        run = 0
        for x in range(x0, x1):
            o = (y * w + x) * 3
            d = max(abs(wet[o] - dry[o]), abs(wet[o + 1] - dry[o + 1]),
                    abs(wet[o + 2] - dry[o + 2]))
            run = run + 1 if d > 2 else 0
            if run >= min_run:
                return y
    return h


def _water_shore_fall(path, window):
    """Median sharpest single-row fall down each column crossing the waterline.

    A hard edge spends the whole dry-to-water drop in one row; coverage splits it
    across two, so the sharpest step falls. Median over columns rather than a mean:
    the ramp's own silhouette contributes a few columns with a much larger step and
    a mean would follow those instead of the waterline.
    """
    w, h, pix = _read_ppm(path)
    x0, y0, x1, y1 = window
    falls = []
    for px in range(int(x0 * w), int(x1 * w)):
        col = [_linear_luma(pix, w, h, px, py) for py in range(int(y0 * h), int(y1 * h))]
        falls.append(max(col[i] - col[i + 1] for i in range(len(col) - 1)))
    falls.sort()
    return falls[len(falls) // 2] if falls else float("nan")


# ----------------------------------------------------------------------------
# Water after dark (spec 11.84). No arm in the water or beach groups had ever
# rendered a sea with the sun below the horizon: both fixtures author +35 and
# +32 degrees, and the one arm that moves the sun authors +22. So the entire
# night half of this feature could have shipped broken from any spec since
# 11.41 without a gate going red, and two defects did.
#
# The variant is the committed fixture with its LIGHTS REMOVED, which is the
# whole instrument. water_fixture authors a 1.2-lux studio key that does not
# know what time it is, and it outranks a 0.33-lux moon -- so on the file as
# committed the pick at midnight is that key, correctly, and the sun/moon
# crossover this group exists to measure is invisible. Removing it leaves the
# sky's own sun and moon as the only candidates.
WATER_NIGHT_SUN_DOWN = "-12"
# Straddles the crossover, which measurement puts between +1 and 0: the sun is
# still 3.33 lux at +1 and below the moon's 0.045 at 0. Four rungs either side
# rather than two, because a ladder that only just clears the crossing cannot
# tell a correct pick from one that happens to switch at the wrong elevation.
#
# The below-horizon rungs are what falsify the by-name pick this replaced --
# that one answers sky_sun at every rung, including the ones where its
# intensity is exactly 0. The above-horizon rungs falsify the opposite error,
# a pick that always takes the moon.
WATER_NIGHT_PICK_LADDER = [("20", "sky_sun"), ("8", "sky_sun"), ("3", "sky_sun"),
                           ("1", "sky_sun"), ("0", "sky_moon"), ("-2", "sky_moon"),
                           ("-6", "sky_moon"), ("-12", "sky_moon")]
# The moon where the app puts it, which is up and near full at this sun: 0.33 lux
# against the 0.045 it carries at sunset. Its intensity rides the SUN's elevation
# through sky_night_factor, so the sun is what has to be down, not the moon.
WATER_NIGHT_MOON = ["--moon"]
# The same moon MOVED in front of the camera, for the one arm that needs its
# reflection rather than its light. Placed at WATER_GLITTER_SUN's own elevation and
# azimuth so the streak lands in the box that arm already derives.
#
# Costs most of its brightness to get there and that is not a defect: PHASE is derived
# from the sun-moon elongation (spec 11.82), so moving the moon to face the camera
# while the sun stays put makes it a thin crescent -- 0.009 lux against 0.33. Enough
# for a specular track, nowhere near enough to read as illumination, which is why
# water-night-lit uses the unplaced moon above.
WATER_NIGHT_MOON_AHEAD = WATER_NIGHT_MOON + [
    "--moon-elevation", repr(WATER_GLITTER_SUN["sun"]["elevation"]),
    "--moon-azimuth", repr(WATER_GLITTER_SUN["sun"]["azimuth"])]
# The specular box is the daylight arm's, so a re-framed glitter arm must carry this one
# with it -- which only holds while the angles are DERIVED above rather than transcribed.
# Measured 19.99x on the daylight sun; a crescent moon's track is dimmer but the box is
# near-black without it, so a third of the daylight ratio still fails any misplacement.
WATER_NIGHT_GLITTER_RATIO = 1.5
# Measured 16,318 px. A third of it is clear of noise and well under the signal.
WATER_NIGHT_GLITTER_MIN_PX = 5000
# A directional that is BRIGHT by intensity and nearly black by colour, which is the
# only shape that can tell `intensity x peak` from `intensity` alone. Every real
# candidate in the corpus -- sun, moon, both fixtures' keys -- is near-white, so the
# two rules agree everywhere and the peak factor was unfalsifiable without this.
#
# 2.0 x 0.04 = 0.08 against the moon's 0.33 x ~1.0: peak-weighted the moon wins,
# by raw intensity this does, 2.0 to 0.33. Not a contrived shape -- an authored fill
# at high intensity over a near-black colour is the same trick 11.49 measured on 30 of
# 32 emissive materials.
WATER_NIGHT_DECOY = {"name": "DimDecoy", "type": "directional",
                     "direction": [-0.35, -0.82, -0.45], "color": [0.04, 0.04, 0.04],
                     "intensity": 2.0, "cast_shadows": False}
# Caustics measured at 27,450 px moonlit and 26,143 by day, against exactly 0
# with nothing above the horizon. The floor is shared by both live legs.
WATER_NIGHT_CAUSTIC_MIN_PX = 8000
# The in-scatter's OWN contribution, isolated against a zero-scatter twin so that
# whatever the water is drawn over cancels out of both legs.
#
# That cancellation is the whole reason this is a delta and not a brightness read.
# water_fixture's geometry is EMISSIVE over a black base -- bed 0.55 grey, ramp
# (0.42, 0.34, 0.22) -- which is exactly what makes it a good absorption instrument (a
# backdrop that cannot vary with the light) and exactly what makes it unable to show
# night. Its water band reads 0.2207 at midnight against 0.2478 at noon whatever the
# sea does, because most of that is self-lit sand seen through it. Do not read
# brightness on this fixture and call it lighting.
#
# Measured: 0.01632 by day, 0.00023 under a moon, 0.00000 with nothing above the
# horizon. Before 11.84's second half all three read 0.0163 -- the SAME number, which
# is what an authored constant means.
WATER_NIGHT_LIT_DAY_MIN = 0.008
WATER_NIGHT_LIT_RATIO_MIN = 20.0
WATER_NIGHT_LIT_MOON_MIN = 1.0e-4
# Derived rather than measured, because the measurement cannot support it: the dark leg
# reads 0.00000 at the precision it is printed, which establishes only "< 5e-6" and says
# nothing about the margin. One 8-bit code at the darkest end is _SRGB_TO_LINEAR[1] =
# 3.0e-4, spread over a 12x12 box that is ~2.1e-6 per stray sample -- so this bar is
# "about five stray codes", which is a reason a reader can check.
WATER_NIGHT_LIT_DARK_MAX = 1.0e-5
# One value put on each field in turn, which is what makes water-glow a comparison rather
# than two readings: with no light above the horizon the glow must still light the body
# and the albedo must do exactly nothing, and asking the SAME number to do both is what
# rules out a build where the two fields quietly behave alike.
#
# apps/tree's own authored sea, so the arm exercises the magnitude a real scene uses.
# Ambient dust, authored onto the night fixture so there are MOTES to light. Big and dense
# against the library recipe, because this is read as a whole-frame mean and the shipping
# defaults are a haze: 0.0015-unit motes at 110/s contribute below the noise.
#
# Dust spawns in a box around the SCENE CENTRE (apply_cscene_dust), so the host fixture has
# to be one whose camera looks at its own middle -- moon_fixture aims 40 degrees up and away
# from the model, and reads an exact 0.000000 contribution.
WATER_NIGHT_DUST = {"enabled": True, "spawnRate": 3000.0, "lifetime": [8.0, 12.0],
                    "size": [0.02, 0.05], "color": [1.0, 1.0, 1.0, 0.9], "colorJitter": 0.0}
# Measured 1.18x with the moon against 0.993x on the first-in-array scan this replaced.
# The bar sits between, and the separation is all there is to have: uSunColor takes the
# light's COLOUR and not its intensity, so a brighter moon does not widen it (1.18x at
# moon-brightness 2, 6 and 12 alike).
WATER_NIGHT_MOTE_RATIO = 1.08
WATER_GLOW_VALUE = [0.03, 0.13, 0.14]
ZERO3 = [0.0, 0.0, 0.0]
# Both halves off, which is the shared baseline every separability read is taken against.
WATER_SCATTER_OFF = {"scatterAlbedo": ZERO3, "scatterGlow": ZERO3}
# Measured 342,413 px for the glow. A seventeenth of it, which is loose because this bar is
# only load-bearing when the albedo leg reads exactly 0 and drives the ratio below to
# infinity; at the measured 151 px the ratio is what decides the arm.
WATER_GLOW_MIN_PX = 20000
# Measured 342,413 px for the glow against 151 for the albedo. The albedo leg is not 0
# and should not be asserted as 0: a sky at -12 degrees is very dark rather than black,
# so a fraction of it is a few hundred pixels of the darkest codes -- the albedo term
# doing exactly what it says. A ratio is the honest bar; 100x still fails any build where
# the two fields behave alike.
WATER_GLOW_RATIO_MIN = 100.0


def _frame_mean_luma(path):
    """Linear-luma mean over a whole PPM.

    Whole-frame because its consumer reads a DIFFERENCE of two frames -- what a scene
    element contributes, wherever it happens to land. A box would need to know where the
    thing being measured is, which for drifting particles is the question, not the input.
    """
    w, h, pix = _read_ppm(path)
    total = 0.0
    for i in range(0, len(pix), 3):
        total += (0.2126 * _SRGB_TO_LINEAR[pix[i]] + 0.7152 * _SRGB_TO_LINEAR[pix[i + 1]] +
                  0.0722 * _SRGB_TO_LINEAR[pix[i + 2]])
    return total / float(w * h)


def _vec3_field(head, key):
    """A `x,y,z` probe field as three floats, zeros when the field is absent.

    Absent rather than raising, so an arm reading a field an older binary does not print
    fails on its own assertion rather than on a parse.
    """
    raw = head.get(key)
    if not raw:
        return [0.0, 0.0, 0.0]
    return [float(v) for v in raw.split(",")]


def _water_night_variant(src, dst, overrides=None, mutate=None):
    """The fixture with no lights of its own, so the sky's sun and moon are the candidates.

    `overrides` merges into the water block, which is what most arms here want and is
    _water_cscn_variant's body -- taken as data rather than as a callback so the six call
    sites cannot disagree about how they reach that block. They did: five wrote a bare
    `d["water"]` where the shared helper uses `d.setdefault`, which is a KeyError waiting
    for the first fixture that authors no water block.

    `mutate` is the general escape, for the two arms that vary something outside it (a
    camera, an extra light). Runs after the merge.
    """
    def strip(d):
        d["lights"] = []
        if overrides:
            _merge_water_block(d.setdefault("water", {}), overrides)
        if mutate:
            mutate(d)

    cscn_copy(src, dst, strip)


def run_water_night_gate(workdir):
    """The sea after dark: which light it takes, and what it does when there is none.

      water-night-pick     --water-probe names the picked directional across an elevation
                           ladder straddling the sun/moon crossover. The by-name pick this
                           replaced answers sky_sun at every rung, including the four where
                           its intensity is exactly 0.
      water-night-incident the CPU twin of the incident term falls with the sun and lifts
                           under a moon. water_incident_light drives the SUBMERGED medium
                           and every submerged arm runs in daylight, so without this the
                           function 11.84 added has no coverage at all.
      water-night-weight   the rank is intensity x PEAK CHANNEL, not intensity. The only
                           arm that can see the difference: every real candidate in the
                           corpus is near-white, so it takes an authored decoy to separate
                           the two rules at all.
      water-night-glitter  the moon puts a specular track on the water. Exactly 0 before
                           the pick moved, because the glitter multiplies sunRadiance and
                           a faded sun's is the zero vector.
      water-night-caustic  caustics are OFF with nothing above the horizon, and LIVE under
                           both a moon and a sun. The dark leg alone passes on a build with
                           no caustics at all, so neither leg is safe without the others.
      water-night-lit      the in-scatter is a FRACTION of the light that falls on the sea,
                           not a constant: its own contribution is 0.0163 by day, 0.0002
                           under a moon and exactly 0 with nothing above the horizon.
                           Before 11.84's second half all three read the same number.
      water-glow           the two halves are SEPARABLE. With nothing above the horizon the
                           glow alone still lights the body and the albedo alone is exactly
                           nothing -- which is what makes a stylised night sea authorable
                           without giving up a dark realistic one.
      water-scatter-refuse the old `scatter` key is refused BY NAME. Its units changed, so
                           an old value still parses and would mean something around five
                           times too large; silence would render a plausible frame.
      water-night-motes    PARTICLES take the same key by the same rule. The renderer
                           scanned for the first directional, which is the sun, whose
                           colour is black below the horizon -- so ambient dust was lit by
                           nothing every night and the moon never reached a mote.

    Every arm here is a twin DELTA rather than a brightness read, and that is forced by
    the fixture: water_fixture's geometry is EMISSIVE over a black base, so its water band
    reads 0.2207 at midnight against 0.2478 at noon whatever the sea does.
    """
    failures = []
    scene = asset(WATER_FIXTURE)
    base = WATER_PIN + WATER_NO_CATCHER
    night = os.path.join(workdir, "water_night.cscn")
    _water_night_variant(scene, night)

    # Which light, across the ladder. Read through the probe rather than off a frame:
    # a sea lit by the wrong directional still looks like a sea.
    picks, want = [], []
    for elev, expect in WATER_NIGHT_PICK_LADDER:
        head, _ = _water_probe(base + WATER_NIGHT_MOON + ["--sun-elevation", elev],
                               scene=night)
        picks.append((elev, head.get("key", "?"), float(head.get("intensity", "0")),
                      _vec3_field(head, "incident")))
        want.append(expect)
    got = [p[1] for p in picks]
    # Every rung must also be DELIVERING light. The defect was a key with a direction and
    # no radiance, which "the right name" alone would not have caught.
    delivering = all(p[2] > 0.0 for p in picks)
    ok = got == want and delivering
    print(f"  water-night-pick {'PASS' if ok else 'FAIL'}  " +
          " ".join(f"{e}:{k}@{i:.3f}" for e, k, i, _inc in picks) +
          f" (want {' '.join(want)}, all delivering: {delivering})")
    if not ok:
        failures.append("water-night-pick")

    # The INCIDENT term itself, off the same eight probes -- the CPU twin of what
    # water-night-lit asserts about the shader, and the quantity 11.84 added. It was
    # printed by --water-probe and read by nothing, so water_incident_light shipped with
    # no coverage at all: the submerged arms that consume it all run in daylight.
    #
    # Monotone in sun elevation while the sun is the key, because that is the one shape a
    # wrong sign, a missing cosine or a stale cache all break. Compared only across the
    # sun's own rungs: the moon takes over below the horizon and rises on a different
    # schedule, so a single monotone run across the crossover would be asserting the sun
    # and the moon are one curve.
    sun_inc = [sum(p[3]) for p in picks if p[1] == "sky_sun"]
    falling = all(sun_inc[i] >= sun_inc[i + 1] for i in range(len(sun_inc) - 1))
    moon_up = sum(next(p[3] for p in picks if p[1] == "sky_moon"))
    lit_head, _ = _water_probe(base + ["--sun-elevation", WATER_NIGHT_SUN_DOWN,
                                       "--no-moon"], scene=night)
    moon_off = sum(_vec3_field(lit_head, "incident"))
    ok = falling and len(sun_inc) >= 2 and moon_up > moon_off
    print(f"  water-night-incident {'PASS' if ok else 'FAIL'}  falls with the sun across "
          f"{len(sun_inc)} rungs ({', '.join(f'{v:.3f}' for v in sun_inc)}): {falling}; "
          f"and a moon lifts it off its floor, {moon_off:.5f} -> {moon_up:.5f} "
          f"(want >, or the CPU twin never reached the light the pick found)")
    if not ok:
        failures.append("water-night-incident")

    # The peak-channel factor, which nothing else in the suite can see: every real
    # candidate is near-white, so `intensity x peak` and `intensity` agree on all of them.
    # The moon has to beat a light carrying SIX times its raw intensity and a twenty-fifth
    # of its colour.
    decoy = os.path.join(workdir, "water_night_decoy.cscn")
    _water_night_variant(scene, decoy,
                         mutate=lambda d: d["lights"].append(dict(WATER_NIGHT_DECOY)))
    head, _ = _water_probe(base + WATER_NIGHT_MOON +
                           ["--sun-elevation", WATER_NIGHT_SUN_DOWN], scene=decoy)
    picked = head.get("key", "?")
    # And the arm's own PREMISE, which is otherwise only in a comment: the winner must be
    # the light with the LOWER raw intensity. If moonlight ever rises past the decoy's,
    # both ranking rules pick the moon and this arm silently covers nothing -- and it is
    # the only arm that can see the peak factor, so there is no pair to catch that.
    won_with = float(head.get("intensity", "0"))
    premise = 0.0 < won_with < WATER_NIGHT_DECOY["intensity"]
    ok = picked == "sky_moon" and premise
    print(f"  water-night-weight {'PASS' if ok else 'FAIL'}  picked {picked} at "
          f"{won_with:.3f} over a {WATER_NIGHT_DECOY['intensity']}-intensity light at "
          f"colour {WATER_NIGHT_DECOY['color'][0]} (want sky_moon, and it must win from "
          f"BELOW on raw intensity or the two rules agree and nothing is tested: "
          f"{premise})")
    if not ok:
        failures.append("water-night-weight")

    # The moon's own specular track, in the box the daylight glitter arm derives.
    glit = os.path.join(workdir, "water_night_glitter.cscn")

    def frame_it(d):
        d["camera"] = dict(WATER_GLITTER_CAMERA)
        _merge_water_block(d.setdefault("water", {}), WATER_GLITTER_WATER)

    _water_night_variant(scene, glit, mutate=frame_it)
    g_common = base + WATER_NIGHT_MOON_AHEAD + ["--sun-elevation", WATER_NIGHT_SUN_DOWN]
    g_on = os.path.join(workdir, "water_night_glitter_on.ppm")
    g_off = os.path.join(workdir, "water_night_glitter_off.ppm")
    err = render(glit, g_on, g_common)
    if not err:
        err = render(glit, g_off, g_common + ["--no-water-glitter"])
    if err:
        print(f"  water-night-glitter ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-night-glitter")
    else:
        ae_g, _ = compare(g_off, g_on)
        # WHERE, not just how much. A whole-frame count passes on a track anywhere in
        # frame -- a mirrored azimuth, a sign error on the moon direction, or a lobe built
        # from the view vector instead of the light all move more than the floor. The box
        # is the daylight arm's, derived from the same camera and the same 22/180 the moon
        # is placed at, which is what placing it there was FOR.
        gw, gh, g_on_pix = _read_ppm(g_on)
        _, _, g_off_pix = _read_ppm(g_off)
        g_box = _water_glitter_box()
        g_lum_on = _box_luma_dense(g_on_pix, gw, gh, g_box)
        g_lum_off = _box_luma_dense(g_off_pix, gw, gh, g_box)
        g_ratio = g_lum_on / max(g_lum_off, 1e-6)
        ok = ae_g >= WATER_NIGHT_GLITTER_MIN_PX and g_ratio >= WATER_NIGHT_GLITTER_RATIO
        print(f"  water-night-glitter {'PASS' if ok else 'FAIL'}  the moon's track moves "
              f"{ae_g} px (want >={WATER_NIGHT_GLITTER_MIN_PX}; exactly 0 before the pick "
              f"reached a light that delivers any) and lands in the specular box, "
              f"{g_lum_off:.4f} -> {g_lum_on:.4f} = {g_ratio:.2f}x (want "
              f">={WATER_NIGHT_GLITTER_RATIO}x)")
        if not ok:
            failures.append("water-night-glitter")

    # Caustics: off in true darkness, live under either source.
    caustic = os.path.join(workdir, "water_night_caustic.cscn")
    _water_night_variant(scene, caustic)
    # Spectral by FLAG, not by borrowing the glitter arm's water block: caustics are
    # FFT-only, and reaching that through a constant named for another arm hid the reason.
    # The day leg leaves the fixture's own sun elevation alone.
    caustic_waves = ["--water-waves", "fft"]
    legs = (("dark", ["--sun-elevation", WATER_NIGHT_SUN_DOWN, "--no-moon"] + caustic_waves),
            ("moonlit", ["--sun-elevation", WATER_NIGHT_SUN_DOWN] + WATER_NIGHT_MOON +
             caustic_waves),
            ("day", caustic_waves))
    moved, cerr = {}, None
    for tag, flags in legs:
        on = os.path.join(workdir, f"water_night_caustic_{tag}_on.ppm")
        off = os.path.join(workdir, f"water_night_caustic_{tag}_off.ppm")
        cerr = render(caustic, on, base + flags) or \
            render(caustic, off, base + flags + ["--no-water-caustics"])
        if cerr:
            break
        moved[tag], _ = compare(off, on)
    if cerr:
        print(f"  water-night-caustic ERROR render failed: {cerr.strip()[-200:]}")
        failures.append("water-night-caustic")
    else:
        ok = (moved["dark"] == 0 and
              moved["moonlit"] >= WATER_NIGHT_CAUSTIC_MIN_PX and
              moved["day"] >= WATER_NIGHT_CAUSTIC_MIN_PX)
        print(f"  water-night-caustic {'PASS' if ok else 'FAIL'}  dark {moved['dark']} px "
              f"(want 0: nothing above the horizon focuses nothing), moonlit "
              f"{moved['moonlit']} and day {moved['day']} px (want "
              f">={WATER_NIGHT_CAUSTIC_MIN_PX} on both, or the dark leg is 0-vs-0)")
        if not ok:
            failures.append("water-night-caustic")

    # Does the sea know what time it is. Read as the in-scatter's own contribution --
    # the authored value against a zero-scatter twin -- because a brightness read on this
    # fixture measures its emissive backdrop, not its water.
    #
    # `night` IS the authored side: it is the fixture with its lights stripped and nothing
    # else touched. A second copy of it was built here and was byte-identical.
    zero = os.path.join(workdir, "water_night_zero.cscn")
    _water_night_variant(scene, zero, WATER_SCATTER_OFF)
    # The DAY leg takes no --sun-elevation: the fixture authors its own 35, and naming it
    # again here is a second place for that number to live.
    lit, lerr = {}, None
    for tag, flags in (("day", []),
                       ("moonlit", ["--sun-elevation", WATER_NIGHT_SUN_DOWN] +
                        WATER_NIGHT_MOON),
                       ("dark", ["--sun-elevation", WATER_NIGHT_SUN_DOWN, "--no-moon"])):
        pa = os.path.join(workdir, f"water_night_lit_{tag}_a.ppm")
        pz = os.path.join(workdir, f"water_night_lit_{tag}_z.ppm")
        lerr = render(night, pa, base + flags) or render(zero, pz, base + flags)
        if lerr:
            break
        wa, ha, pixa = _read_ppm(pa)
        _, _, pixz = _read_ppm(pz)
        # Mean absolute channel difference over the three open-water boxes.
        diffs = []
        for box in WATER_ABSORB_BOXES:
            ca = _absorb_box_rgb(pixa, wa, ha, box)
            cz = _absorb_box_rgb(pixz, wa, ha, box)
            diffs.append(sum(abs(ca[c] - cz[c]) for c in range(3)) / 3.0)
        lit[tag] = sum(diffs) / len(diffs)
    if lerr:
        print(f"  water-night-lit ERROR render failed: {lerr.strip()[-200:]}")
        failures.append("water-night-lit")
    else:
        lit_ratio = lit["day"] / lit["moonlit"] if lit["moonlit"] > 0 else float("inf")
        ok = (lit["day"] >= WATER_NIGHT_LIT_DAY_MIN and
              lit_ratio >= WATER_NIGHT_LIT_RATIO_MIN and
              lit["moonlit"] >= WATER_NIGHT_LIT_MOON_MIN and
              lit["dark"] <= WATER_NIGHT_LIT_DARK_MAX)
        print(f"  water-night-lit {'PASS' if ok else 'FAIL'}  in-scatter contributes "
              f"{lit['day']:.5f} by day (want >={WATER_NIGHT_LIT_DAY_MIN}), "
              f"{lit['moonlit']:.5f} under a moon (want >={WATER_NIGHT_LIT_MOON_MIN}: the "
              f"moon LIGHTS it, it does not merely switch off) and {lit['dark']:.2e} with "
              f"nothing above the horizon (want <={WATER_NIGHT_LIT_DARK_MAX}); "
              f"day/moonlit {lit_ratio:.1f}x (want >={WATER_NIGHT_LIT_RATIO_MIN}, an authored "
              f"constant reads 1.0x)")
        if not ok:
            failures.append("water-night-lit")

    # The two halves are separable, read where the difference is unambiguous: no light
    # above the horizon at all, so the albedo term is multiplied by exactly zero.
    # One value put on each field in turn, which is what makes this a comparison rather
    # than two readings: the glow must still light the body where the albedo does nothing.
    # Equal magnitudes matter -- a glow that merely moved MORE would pass on a build that
    # had quietly kept both terms responding to light.
    #
    # The OFF leg is `zero`, already built above and rendered under these very flags as
    # the lit arm's dark twin. Both were duplicated before.
    dark_flags = base + ["--sun-elevation", WATER_NIGHT_SUN_DOWN, "--no-moon"]
    p_night_dark = os.path.join(workdir, "water_night_lit_dark_a.ppm")
    p_off = os.path.join(workdir, "water_night_lit_dark_z.ppm")
    glow_scn = os.path.join(workdir, "water_glow.cscn")
    alb_scn = os.path.join(workdir, "water_glow_albedo.cscn")
    _water_night_variant(scene, glow_scn,
                         {"scatterAlbedo": ZERO3, "scatterGlow": WATER_GLOW_VALUE})
    _water_night_variant(scene, alb_scn,
                         {"scatterAlbedo": WATER_GLOW_VALUE, "scatterGlow": ZERO3})
    p_glow = os.path.join(workdir, "water_glow.ppm")
    p_alb = os.path.join(workdir, "water_glow_albedo.ppm")
    gerr = lerr or render(glow_scn, p_glow, dark_flags) or render(alb_scn, p_alb, dark_flags)
    if gerr:
        print(f"  water-glow ERROR render failed: {gerr.strip()[-200:]}")
        failures.append("water-glow")
    else:
        ae_glow, _ = compare(p_off, p_glow)
        ae_alb, _ = compare(p_off, p_alb)
        glow_ratio = ae_glow / ae_alb if ae_alb else float("inf")
        ok = ae_glow >= WATER_GLOW_MIN_PX and glow_ratio >= WATER_GLOW_RATIO_MIN
        print(f"  water-glow {'PASS' if ok else 'FAIL'}  with nothing above the horizon "
              f"the glow moves {ae_glow} px (want >={WATER_GLOW_MIN_PX}) against {ae_alb} "
              f"px for the SAME value on the albedo, {glow_ratio:.0f}x (want "
              f">={WATER_GLOW_RATIO_MIN:.0f}x; not 0 px, because a -12 degree sky is very "
              f"dark rather than black and a fraction of it is the albedo doing its job)")
        if not ok:
            failures.append("water-glow")

    # The old key is refused by name, and the refusal is what the author reads.
    stale = os.path.join(workdir, "water_stale_key.cscn")
    _water_night_variant(scene, stale, {"scatter": WATER_GLOW_VALUE})
    r = _run([RENDER, "-m", stale, "-x", "-f", "2", "-W", "200", "-H", "150"] + base,
             capture_output=True, text=True)
    log = r.stdout + r.stderr
    named = "water.scatter is gone" in log
    tells = "scatterAlbedo" in log and "scatterGlow" in log
    # Both halves, because cscene.c says both must fire: the specific message carries the
    # migration and the generic one is what listing the key in known[] would silence. That
    # comment argues its way to a double warning, so the double warning is the contract.
    generic = "not a recognised water parameter" in log
    # And it must not have been APPLIED: the stale scene has to render as the twin that
    # never mentioned it. A warning that fires while the value still lands is worse than
    # no warning, because it reads as handled.
    #
    # Against `auth` -- the fixture's own authored sea -- and not against the zeroed twin.
    # The stale scene adds a dead key and changes nothing else, so what it must equal is
    # the fixture, not a sea with no in-scatter at all.
    p_stale = os.path.join(workdir, "water_stale_key.ppm")
    serr = render(stale, p_stale, dark_flags) or render(night, p_night_dark, dark_flags)
    if serr:
        # Reported rather than folded into the verdict. Swallowing it printed a bare
        # `-1 px` FAIL with no diagnostic, which is the one failure mode a reader cannot
        # act on and every other arm in this file avoids.
        print(f"  water-scatter-refuse ERROR render failed: {serr.strip()[-200:]}")
        failures.append("water-scatter-refuse")
    else:
        ae_stale, _ = compare(p_night_dark, p_stale)
        ok = named and tells and generic and ae_stale == 0
        print(f"  water-scatter-refuse {'PASS' if ok else 'FAIL'}  refused by name: "
              f"{named}, names both replacements: {tells}, still reported as unknown: "
              f"{generic}, and {ae_stale} px against the twin that never authored it "
              f"(want 0: warned AND ignored, not warned and applied)")
        if not ok:
            failures.append("water-scatter-refuse")

    # PARTICLES take the same key, through the same rule. The renderer scanned for the
    # first directional in scene order, which is the sun -- and a sun below the horizon
    # still exists with its colour faded to black, so the motes took a key of exactly zero
    # every night and the moon standing beside it never lit one.
    #
    # Read as the motes' OWN contribution (dust on minus dust off) so everything the moon
    # does to the rest of the frame cancels: at -12 with a big disc up, a plain on/off
    # comparison moves 99.9% of the frame and says nothing about particles.
    dusty = os.path.join(workdir, "water_night_dust.cscn")
    _water_night_variant(scene, dusty, mutate=lambda d: d.update({"dust": WATER_NIGHT_DUST}))
    motes, merr = {}, None
    for tag, flags in (("moonlit", ["--sun-elevation", WATER_NIGHT_SUN_DOWN] +
                        WATER_NIGHT_MOON),
                       ("dark", ["--sun-elevation", WATER_NIGHT_SUN_DOWN, "--no-moon"])):
        p_on = os.path.join(workdir, f"water_night_dust_{tag}_on.ppm")
        p_off = os.path.join(workdir, f"water_night_dust_{tag}_off.ppm")
        merr = render(dusty, p_on, base + flags) or render(night, p_off, base + flags)
        if merr:
            break
        motes[tag] = _frame_mean_luma(p_on) - _frame_mean_luma(p_off)
    if merr:
        print(f"  water-night-motes ERROR render failed: {merr.strip()[-200:]}")
        failures.append("water-night-motes")
    else:
        mote_ratio = motes["moonlit"] / motes["dark"] if motes["dark"] > 0 else float("inf")
        ok = motes["dark"] > 0.0 and mote_ratio >= WATER_NIGHT_MOTE_RATIO
        print(f"  water-night-motes {'PASS' if ok else 'FAIL'}  ambient dust contributes "
              f"{motes['dark']:.6f} with no moon and {motes['moonlit']:.6f} under one, "
              f"{mote_ratio:.2f}x (want >={WATER_NIGHT_MOTE_RATIO}x; the first-in-array "
              f"scan reads 0.99x, and the dark leg must be >0 or there are no motes)")
        if not ok:
            failures.append("water-night-motes")

    return failures


def run_water_gate(workdir):
    """The water surface is alive, deterministic, and absorbs with path length.

    The arms, in the order they run. Keep this list and the code in step: a docstring
    naming a different set than the function runs is what a reviewer reads to decide
    what is covered, which makes a stale one worse than none.

      water-det       two runs of one build at 0 px. The surface is a pure function
                      of the frame clock, so this is the precondition every other
                      arm and the golden rest on -- and the one thing no amount of
                      looking at a frame establishes.
      water-live      the flag moves the frame. Three zeros are also what a dead
                      flag, an unparsed argument or a failed program compile
                      produce, so the off-path arms alone prove nothing. Shadows
                      are OFF on both sides so this is not measuring the catcher --
                      see WATER_NO_CATCHER.
      water-absorb    R/B falls monotonically with distance. Fails a constant tint,
                      which is what absorption applied without the path length is.
      water-dry       the emerged ramp is NOT water-tinted. NOTE this does not test
                      the shoreline discard: those water fragments are behind the
                      ramp and GL_LESS rejects them anyway. It tests that nothing
                      tints dry land, which is a weaker but real claim.
      water-fft-det   the spectral path reproduces across two runs, over 45
                      ping-pong passes and three cascades.
      water-fft-live  the spectral surface differs from Gerstner at the SAME extent
                      and level, so the wave model is the only variable. Fails a
                      cascade chain that transformed to zero.
      water-fft-motion the spectral surface reports the motion its waves have. The
                      camera is static, so motion blur can only move a pixel whose
                      velocity is the wave's own -- it moved 0 px before the previous
                      cascades were retained.
      water-caustic   light focusing moves the frame on both wave models; one flag apart.
      water-submerged absorption is monotone along the submerged INTERFACE, which only
                      that branch produces. A pixel count here would pass on a surface
                      that drew nothing, since raising the level moves 91% of the frame
                      regardless. Shallow and Gerstner for a geometric reason, not a
                      convenient one -- see WATER_SUBMERGED_FLAGS.
      water-under-fog the submerged RAMP fades with distance. It is opaque and has no
                      water interface between it and the eye, so the froxel volume's
                      second medium is the only thing that can absorb it -- which is
                      what rendered as though in air before 11.33.
      water-cscn      the scene file's water block reaches the surface, and the flags
                      override it rather than the reverse. absorption is read because
                      no flag can set it, so the frame can only have moved through the
                      authoring path.
      water-seastate  the spectral sea state reaches the seeding. Wind speed, fetch,
                      depth, peak enhancement and swell are authorable since spec 11.42
                      and NO flag can set any of them, so a scene file is the only way in
                      and this is the only arm on that path. The wind DIRECTION reaches
                      both models now and is covered by water-fft-live moving with it.
      water-glitter   the sea has a specular response to its own sun, and the streak is
                      at least WATER_GLITTER_RATIO_MIN brighter than the same water with
                      the lobe off. Under the procedural sky the environment cubemap
                      carries no disc, so before spec 11.42 there was none at all. The
                      box is DERIVED from the specular point and the horizon
                      (_water_glitter_box), and the bar is a ratio rather than a strict
                      inequality -- until 11.43 it was neither, and the sun override it
                      authors did not reach the engine either.
      water-foam-persist whitewater OUTLIVES the crest that made it. Read against
                      --no-water-foam-history, which selects foam from this frame's fold
                      alone and is the pre-11.42 behaviour exactly. On open water and on a
                      rough sea for reasons that are both in WATER_PERSIST_SEA, in a box
                      whose top edge follows the horizon that raising `level` moves. The
                      ratio carries an absolute floor under it, since 1 -> 3 px is 3.00x
                      on a frame with no whitewater in it, and the arm renders the 90-frame
                      config TWICE: it is the only place the accumulator runs that long
                      and the only determinism claim over it. Reads --water-foam-debug
                      rather than the shaded frame, so it cannot move when a LOOK constant
                      does (spec 11.47).
      water-whitecap-coverage  the sea's whitecap fraction against Monahan &
                      O'Muircheartaigh's published relation, at two wind speeds, read at
                      nadir where pixel fraction of the debug mask is areal fraction
                      exactly. Nothing else in this suite checks the sea against a number
                      that came from outside it.
      water-fft-var   the transformed field carries the variance the SEEDING predicted,
                      per band -- the three cascades and the ripple band -- and in both
                      height and slope. The first arm here that
                      reads the spectrum rather than a picture of it, and the only one
                      that can fail on a transform which is deterministic, differs from
                      Gerstner, and is still wrong. Blind to a missed fftshift, which
                      moves the field in space and not in variance -- that is
                      water-fft-impulse's half.
      water-ripple-mips the ripple band's mip chain carries, level by level, the slope
                      variance seeding predicted for it -- the number roughness takes the
                      ripples' filtered-out share from, and a model of glGenerateMipmap
                      rather than a reading of it. Held on the levels carrying at least
                      WATER_RIPPLE_MIP_FLOOR of the band (spec 13.3).
      water-fft-impulse the transform matches its CLOSED FORM on two single modes: a
                      centred impulse must come back constant, and its neighbour as one
                      cycle across the grid. Run through the same stages and the same
                      twiddle tables the sea uses -- 14 at 128 and 18 at the ripple band's
                      512, reporting the worse -- so it tests this transform rather than
                      a copy. Verified by breaking the shader -- see
                      WATER_FFT_IMPULSE_MAX for what the rest of the suite did then.
      water-horizon   the surface reaches the horizon, asserted as REACH INVARIANCE:
                      multiplying the nominal extent by 14,000 must not move the water's
                      top edge, because an edge already at the vanishing line cannot go
                      higher. Needs no horizon row and no camera parameters. The clipmap
                      fails it by 71 px, which is what it was written against.
      water-fixture-roundtrip the fixture's GENERATOR still produces the fixture, and the
                      key set it authors is the one parse_water reads. No renders: it
                      regenerates into a temp directory and compares text. Exists because
                      gen_water_fixture.py stopped emitting the `water` block when 11.33
                      made that block create the surface, and stayed that way for three
                      specs -- so the docstring's "regenerate with" line was an instruction
                      to strip the water and fail every arm below.
      water-waterline the above/below split is decided PER PIXEL. On a framing where
                      crests close over a camera that is still above the still level, a
                      backfacing fragment takes the sight line as its optical path
                      rather than the depth buffer behind it -- which is air there, so
                      charging it the bed's distance read far too absorbed. An in-frame
                      ratio against foreground the fix cannot touch, so it is exposure-
                      and build-independent. The fixture cannot see this at all and the
                      arm authors its own framing -- see WATER_WATERLINE_BLOCK.
      water-flags     the flags override the scene file, and a NEGATIVE flag neither
                      creates a surface nor loses to a sibling. Read through the probe,
                      which reports what the surface is rather than what it looks like:
                      every case here was a silent defect no rendered frame could show.
      water-cpu       the CPU wave query inverts the horizontal map (an unconverged
                      inversion answers about the wrong point while looking correct),
                      evaluates a real train rather than a plane, and ANSWERS on the
                      spectral model through the GPU query (spec 13.1) with a surface
                      rather than the still plane it used to hand back.
      water-gpu       the surface query reproduces the closed form: same fixture, same
                      sixteen points, same clock, on the GERSTNER path -- the only path with a
                      closed form to be wrong against. That comparison is what proves the
                      pass, the pack-buffer ring, its latency and the inversion, BEFORE
                      any of it is pointed at a spectral sea that has no reference answer
                      of its own.
      water-still     a spectral sea with no energy answers exactly its still level: any
                      remainder is an offset every spectral sea carries.
      water-caustic-energy  the caustics conserve light: the target is exactly 1 over a
                      calm sea and averages 1 over Gerstner, spectral and a baked dome bed,
                      which a pixel count cannot tell from a brighten-only effect -- in each
                      colour channel on its own, since each is traced at its own index.
      water-caustic-spectrum the caustics' dispersion (spec 13.4): one trace spread across the
                      spectrum carries the same light per channel as 16 traced bands, and about
                      as much colour. Not where the colour sits, which no single trace matches
                      at these depths -- see WATER_CAUSTIC_SPECTRUM_FLAGS.
      water-caustic-relief the bed's relief, read off its brightness, shapes the caustics only
                      where the bed has some: against --no-water-caustic-relief, a uniform patch
                      of the fixture's flat bed does not move and its material edges do.
      water-specks    sunlit motes in the column (spec 13.4) are there and SPARSE: a floor
                      on the pixels they touch against --no-water-specks, and a ceiling on
                      the fraction, which is what a broken hash or density would blow.
      water-ripple-drop a tap into the water (spec 13.4) becomes a ring that travels outward
                      and fades, read through --water-touch-probe at two frames, and a repeat
                      run of the later frame prints the same numbers: the simulation steps at
                      a fixed rate from the frame clock.
      water-downwell  a submerged surface is lit through the water (spec 13.4): with and
                      without --no-water-downwell, dry pixels are identical, and every
                      submerged row's per-channel ratio is (1 - F) exp(-sigma d / cos)
                      for ONE depth d -- the three channels, solved separately, agree,
                      which a tint or a missing Fresnel term breaks -- and d is the same
                      under a key at two elevations, which a vertical rather than refracted
                      path breaks. On water_downwell_fixture, whose floor is lit and whose
                      key is the only light.
      water-bounds-whole bounds round the whole world leave the frame at 0 px (spec 13.41).
      water-bounds-cut the bounds cut the water and the light under it together: bounds off
                      the water are the frame with none, and bounds ending down the frame's
                      middle are the unbounded frame on their side and the frame with no water
                      on the other, both at 0 px. On the downwell fixture, where the lit floor
                      shows the light under the level as well as the surface.
      water-bounds-eye an eye under the level is in the water only inside the bounds: outside
                      them the frame is the one with no water, to 0 px, and unbounded it is not.
      water-dry-away  a dry shape off the water leaves the frame at 0 px (spec 13.56).
      water-dry-cut   a dry shape over the frame's left half: that half is the frame with no
                      water and the other the frame with no dry shape, both at 0 px, on the
                      downwell fixture, so the light under the level goes with the surface.
      water-dry-eye   an eye under the level inside a dry shape over everything is not in the
                      water: the frame is the one with no water, to 0 px, which an eye test
                      that forgot the shape would not give, arming the water's medium.
      water-shoal    waves shorten over a rising bed, and ONLY over it. Needs
                      --water-bed dome, since every other arm here runs over a bed the
                      vertex stage cannot see. The second half -- open water beyond the
                      dome unchanged -- is what stops a global roughness change passing
                      as shoaling. Both sides run --no-water-surf: see WATER_NO_SURF.
      water-surf      the incident wave the SHORE builds -- a bore that comes in and a
                      swash that runs up -- which neither wave model has. Present over
                      the bed, exactly absent without one, and different between two
                      frames a fraction of a period apart, which is the claim: it moves.
      water-shore-foam the shore band is whitewater where the bed shoals and nowhere
                      else: present in the band, zero at both extremes, and zero in the
                      same box with no bed under it. Run on GERSTNER deliberately --
                      crest foam is FFT-only, so that path is what isolates the shore
                      band from it.
      water-shore-soft the shoreline is antialiased rather than cut off: the frame
                      moves against --no-water-coverage AND the sharpest single-row
                      fall across the waterline gets shallower. The second half is
                      what makes this a softening claim and not just a liveness one.
      water-shore-hard the same flag at ONE sample is 0 px. Alpha-to-coverage has
                      nothing to dither into there, so the shader must fall back to
                      the cutoff -- if it kept the fractional fragment instead, the
                      sliver would be written at full strength.
      water-crack     the projected grid has no HOLE in it, read per pixel rather than
                      as a mean: the closest pixel in each absorption box to the same
                      framing with no water must still be far from it. A crack is one
                      pixel wide and every other arm here averages, so nothing else in
                      this suite can see one.
      water-farfield  the far field is FILTERED rather than aliased, the filtering is
                      confined to the far field, and the slope energy it removes arrives
                      as roughness rather than being dropped. Read against
                      --no-water-lod, which reports a zero footprint and so reaches the
                      unfiltered surface exactly. The direction of the roughness handover
                      is not measurable here -- see WATER_FAR_ROUGH_AUTHORED.
      water-draws     the surface is ONE draw of one instance at the lattice's own
                      triangle count. Integers off the profiler rather than pixels, so
                      it is blind to how the frame looks and sensitive to the projected
                      grid quietly becoming a many-draw clipmap again.
      water-row       the profiler row appears with the flag and is ABSENT without
                      it, which is what a scope opened inside the pass predicate
                      gives.

    Not an arm here, deliberately: "arming the fog volume when submerged cannot leak
    into an above-water frame" is a cross-BUILD claim, and the water golden already is
    that instrument -- it is an above-water frame and it would move. An arm comparing
    two renders of one build cannot see it at all.

    The blind spot this used to record -- no height provider, so shoaling and the
    shore-foam band were dead in every frame -- is closed: `--water-bed dome` installs an
    analytic bed, and water-shoal and water-shore-foam run over it.

    What is still NOT covered, and cannot be from here: nothing compares the CPU wave
    query against the rasterized surface. water-cpu checks the solver against itself.
    The two evaluate the same sum from the same fields with duplicated constants, and
    closing that needs a GPU readback this harness does not have.
    """
    scene = asset(WATER_FIXTURE)
    if not os.path.exists(scene):
        print(f"  water-det    SKIP  ({WATER_FIXTURE} not present)")
        return []

    a = os.path.join(workdir, "water_a.ppm")
    b = os.path.join(workdir, "water_b.ppm")
    off = os.path.join(workdir, "water_off.ppm")
    on_nc = os.path.join(workdir, "water_live_on.ppm")
    for path, extra in ((a, WATER_FLAGS), (b, WATER_FLAGS),
                        (on_nc, WATER_LIVE_ON), (off, WATER_LIVE_OFF)):
        err = render(scene, path, extra)
        if err:
            print(f"  water-det    ERROR render failed: {err.strip()[-200:]}")
            return ["water-det"]

    failures = []

    ae, _ = compare(a, b)
    ok = ae == 0
    print(f"  water-det    {'PASS' if ok else 'FAIL'}  {ae} px between two runs, want 0")
    if not ok:
        failures.append("water-det")

    ae_live, _ = compare(on_nc, off)
    ok = ae_live >= WATER_LIVE_MIN_PX
    print(f"  water-live   {'PASS' if ok else 'FAIL'}  {ae_live} px vs no --water, "
          f"want >={WATER_LIVE_MIN_PX}")
    if not ok:
        failures.append("water-live")

    w, h, pix = _read_ppm(a)
    ratios = [_water_rb(pix, w, h, box) for box in WATER_ABSORB_BOXES]
    steps = [ratios[i] / max(ratios[i + 1], 1e-6) for i in range(len(ratios) - 1)]
    ok = all(s >= WATER_ABSORB_STEP_MIN for s in steps)
    print(f"  water-absorb {'PASS' if ok else 'FAIL'}  "
          f"R/B={'/'.join(f'{r:.4f}' for r in ratios)} "
          f"steps={','.join(f'{s:.2f}x' for s in steps)} want >={WATER_ABSORB_STEP_MIN}x")
    if not ok:
        failures.append("water-absorb")

    dry = _water_rb(pix, w, h, _water_ramp_band(*WATER_DRY_HEIGHTS, 0.44, 0.56))
    ok = dry >= WATER_DRY_RB_MIN
    print(f"  water-dry    {'PASS' if ok else 'FAIL'}  ramp R/B={dry:.4f} "
          f"want >={WATER_DRY_RB_MIN}")
    if not ok:
        failures.append("water-dry")

    fa = os.path.join(workdir, "water_fft_a.ppm")
    fb = os.path.join(workdir, "water_fft_b.ppm")
    fft_ok = True
    for path in (fa, fb):
        err = render(scene, path, WATER_FFT_FLAGS)
        if err:
            print(f"  water-fft-det ERROR render failed: {err.strip()[-200:]}")
            failures.append("water-fft-det")
            # Named, not silently dropped: three arms below hang off this render,
            # and a pass that prints nothing reads as a pass.
            for dropped in ("water-fft-live", "water-caustic", "water-submerged"):
                print(f"  {dropped:<15} SKIP  (spectral render failed)")
            fft_ok = False
            break
    if fft_ok:
        ae_fft, _ = compare(fa, fb)
        ok = ae_fft == 0
        print(f"  water-fft-det {'PASS' if ok else 'FAIL'}  {ae_fft} px between two runs, want 0")
        if not ok:
            failures.append("water-fft-det")

        # The spectral surface must not merely run -- it must produce a DIFFERENT
        # surface from the Gerstner one. A cascade chain that transformed to zero
        # would still be deterministic, and would still pass the arm above.
        # Compared against a Gerstner render at the SAME extent and level, so the
        # wave model is the only thing that moved.
        gref = os.path.join(workdir, "water_gerstner_ref.ppm")
        err = render(scene, gref, WATER_GERSTNER_REF)
        if err:
            # Appended and CONTINUED, not returned. This used to bail, which abandoned
            # the nine arms below it with no output at all -- the exact "a pass that
            # prints nothing reads as a pass" failure the SKIP block above exists to
            # avoid. Only water-shore-foam needs this frame, and it says so itself.
            print(f"  water-fft-live ERROR render failed: {err.strip()[-200:]}")
            failures.append("water-fft-live")
            gref = None
        if gref:
            ae_model, _ = compare(fa, gref)
            ok = ae_model >= WATER_FFT_LIVE_MIN_PX
            print(f"  water-fft-live {'PASS' if ok else 'FAIL'}  {ae_model} px vs gerstner, "
                  f"want >={WATER_FFT_LIVE_MIN_PX}")
            if not ok:
                failures.append("water-fft-live")

    # The spectral surface's own motion, read through the one pass that consumes
    # velocity and nothing else.
    if fft_ok:
        mb = os.path.join(workdir, "water_fft_motionblur.ppm")
        err = render(scene, mb, WATER_FFT_FLAGS + ["--motion-blur"])
        if err:
            print(f"  water-fft-motion ERROR render failed: {err.strip()[-200:]}")
            failures.append("water-fft-motion")
        else:
            ae_mb, _ = compare(fa, mb)
            ok = ae_mb >= WATER_FFT_MOTION_MIN_PX
            print(f"  water-fft-motion {'PASS' if ok else 'FAIL'}  {ae_mb} px under "
                  f"--motion-blur with a static camera, want >={WATER_FFT_MOTION_MIN_PX}")
            if not ok:
                failures.append("water-fft-motion")

    # Caustics, and the submerged side of the interface. Both only exist on the
    # spectral path, so both hang off the FFT render above.
    if fft_ok:
        nc = os.path.join(workdir, "water_nocaustic.ppm")
        err = render(scene, nc, WATER_FFT_FLAGS + ["--no-water-caustics"])
        if err:
            print(f"  water-caustic ERROR render failed: {err.strip()[-200:]}")
            failures.append("water-caustic")
        else:
            ae_c, _ = compare(fa, nc)
            # And on Gerstner, which has had caustics since spec 13.2: refracting through a
            # surface normal needs no spectrum, where the heuristic before it needed a fold.
            gnc = os.path.join(workdir, "water_gerstner_nocaustic.ppm")
            err = render(scene, gnc, WATER_FLAGS + ["--no-water-caustics"])
            ae_g = -1 if err else compare(a, gnc)[0]
            ok = ae_c >= WATER_CAUSTIC_MIN_PX and ae_g >= WATER_CAUSTIC_MIN_PX
            print(f"  water-caustic {'PASS' if ok else 'FAIL'}  {ae_c} px spectral and {ae_g} "
                  f"Gerstner vs no caustics, want >={WATER_CAUSTIC_MIN_PX} on both")
            if not ok:
                failures.append("water-caustic")

        # The submerged interface, measured by what only it produces. Raising the
        # level moves 91% of the frame whatever the underwater branch does, so a
        # pixel-count arm here would pass on a surface that drew nothing, NaN'd, or
        # forgot the normal flip. From below the optical path IS the sight line, so
        # absorption grows with distance along the underside -- the same
        # monotonicity `water-absorb` reads, on the other side of the interface.
        sub = os.path.join(workdir, "water_submerged.ppm")
        err = render(scene, sub, WATER_SUBMERGED_FLAGS)
        if err:
            print(f"  water-submerged ERROR render failed: {err.strip()[-200:]}")
            failures.append("water-submerged")
        else:
            w2, h2, pix2 = _read_ppm(sub)
            under = [_water_rb(pix2, w2, h2, box) for box in WATER_UNDER_BOXES]
            ordered = all(under[i] > under[i + 1] for i in range(len(under) - 1))
            total = under[0] / max(under[-1], 1e-6)
            ok = ordered and total >= WATER_UNDER_TOTAL_MIN
            print(f"  water-submerged {'PASS' if ok else 'FAIL'}  interface "
                  f"R/B={'/'.join(f'{r:.4f}' for r in under)} "
                  f"falling={ordered} end-to-end={total:.2f}x "
                  f"want >={WATER_UNDER_TOTAL_MIN}x")
            if not ok:
                failures.append("water-submerged")

            # The medium on geometry. Separate arm and separate boxes from the one
            # above on purpose: that reads the interface, this reads the opaque ramp
            # behind it, which nothing but the froxel volume can absorb.
            bed = [_water_rb(pix2, w2, h2, box) for box in _water_fog_bed_boxes()]
            bed_ordered = all(bed[i] > bed[i + 1] for i in range(len(bed) - 1))
            bed_total = bed[0] / max(bed[-1], 1e-6)
            ok = bed_ordered and bed_total >= WATER_FOG_BED_TOTAL_MIN
            print(f"  water-under-fog {'PASS' if ok else 'FAIL'}  submerged ramp "
                  f"R/B={'/'.join(f'{r:.4f}' for r in bed)} "
                  f"falling={bed_ordered} end-to-end={bed_total:.2f}x "
                  f"want >={WATER_FOG_BED_TOTAL_MIN}x")
            if not ok:
                failures.append("water-under-fog")

    # The scene file's water block: authored values reach the surface, and the flags
    # override them rather than the other way round.
    absorb_scn = os.path.join(workdir, "water_cscn_absorb.cscn")
    level_scn = os.path.join(workdir, "water_cscn_level.cscn")
    _water_cscn_variant(scene, absorb_scn, WATER_CSCN_ABSORB)
    _water_cscn_variant(scene, level_scn, {"level": WATER_CSCN_LEVEL})
    cscn_absorb = os.path.join(workdir, "water_cscn_absorb.ppm")
    cscn_level = os.path.join(workdir, "water_cscn_level.ppm")
    cli_level = os.path.join(workdir, "water_cli_level.ppm")
    err = render(absorb_scn, cscn_absorb, WATER_PIN)
    if not err:
        err = render(level_scn, cscn_level, WATER_PIN)
    if not err:
        err = render(scene, cli_level, WATER_PIN + ["--water-level", str(WATER_CSCN_LEVEL)])
    if err:
        print(f"  water-cscn   ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-cscn")
    else:
        ae_absorb, _ = compare(a, cscn_absorb)
        ae_level, _ = compare(cscn_level, cli_level)
        ok = ae_absorb >= WATER_CSCN_MIN_PX and ae_level == 0
        print(f"  water-cscn   {'PASS' if ok else 'FAIL'}  authored absorption moves "
              f"{ae_absorb} px (want >={WATER_CSCN_MIN_PX}, no flag can set it); "
              f"authored level vs --water-level {ae_level} px (want 0)")
        if not ok:
            failures.append("water-cscn")

    # The spectral sea state is authorable and reaches the seeding.
    ss_ref_scn = os.path.join(workdir, "water_seastate_ref.cscn")
    ss_calm_scn = os.path.join(workdir, "water_seastate_calm.cscn")
    _water_cscn_variant(scene, ss_ref_scn, WATER_SEASTATE_REF)
    _water_cscn_variant(scene, ss_calm_scn, WATER_SEASTATE_CALM)
    ss_ref = os.path.join(workdir, "water_seastate_ref.ppm")
    ss_calm = os.path.join(workdir, "water_seastate_calm.ppm")
    err = render(ss_ref_scn, ss_ref, WATER_PIN + WATER_NO_CATCHER)
    if not err:
        err = render(ss_calm_scn, ss_calm, WATER_PIN + WATER_NO_CATCHER)
    if err:
        print(f"  water-seastate ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-seastate")
    else:
        ae_sea, _ = compare(ss_ref, ss_calm)
        ok = ae_sea >= WATER_SEASTATE_MIN_PX
        print(f"  water-seastate {'PASS' if ok else 'FAIL'}  authored wind speed moves "
              f"{ae_sea} px (want >={WATER_SEASTATE_MIN_PX}, no flag can set it)")
        if not ok:
            failures.append("water-seastate")

    # The sun's own reflection. Under the procedural sky the environment cubemap carries no
    # disc at all (sky_env_frag), so before this lobe the sea had no specular response to
    # its key light and --no-water-glitter reaches exactly that frame.
    glit_scene = os.path.join(workdir, "water_glitter.cscn")
    _water_glitter_variant(scene, glit_scene)
    glit_on = os.path.join(workdir, "water_glitter_on.ppm")
    glit_off = os.path.join(workdir, "water_glitter_off.ppm")
    err = render(glit_scene, glit_on, WATER_PIN + WATER_NO_CATCHER)
    if not err:
        err = render(glit_scene, glit_off, WATER_PIN + WATER_NO_CATCHER + ["--no-water-glitter"])
    if err:
        print(f"  water-glitter ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-glitter")
    else:
        ae_glit, _ = compare(glit_off, glit_on)
        gw, gh, g_on_pix = _read_ppm(glit_on)
        _, _, g_off_pix = _read_ppm(glit_off)
        glit_box = _water_glitter_box()
        lum_on = _box_luma_dense(g_on_pix, gw, gh, glit_box)
        lum_off = _box_luma_dense(g_off_pix, gw, gh, glit_box)
        glit_ratio = lum_on / max(lum_off, 1e-6)
        ok = ae_glit >= WATER_GLITTER_MIN_PX and glit_ratio >= WATER_GLITTER_RATIO_MIN
        print(f"  water-glitter {'PASS' if ok else 'FAIL'}  {ae_glit} px vs no glitter "
              f"(want >={WATER_GLITTER_MIN_PX}), streak box {lum_off:.4f} -> {lum_on:.4f} "
              f"= {glit_ratio:.2f}x (want >={WATER_GLITTER_RATIO_MIN}x)")
        if not ok:
            failures.append("water-glitter")

    # Foam outlives the crest that made it. Read on open water, where the fold is the only
    # thing that can select whitewater -- the shore band is a different mechanism and is
    # identically zero here without a bed.
    #
    # THROUGH --water-foam-debug, not the composited image, as of spec 11.47. The luma/R-B
    # classifier this arm used to read was measuring the SAME opacity and colour constants
    # S5 exists to tune, so every time the look moved this arm moved with it for reasons
    # that had nothing to do with persistence -- which is what broke it across 11.42, 11.44
    # and 11.47 in turn. The debug mask has no opacity: it is the crest band the shader
    # selected, before a single look constant touches it, so this arm now decouples from
    # exactly what it should never have depended on.
    foam_scene = os.path.join(workdir, "water_foam.cscn")
    _water_persist_variant(scene, foam_scene)
    foam_on = os.path.join(workdir, "water_foam_on.ppm")
    foam_off = os.path.join(workdir, "water_foam_off.ppm")
    # A third render of the SAME config, because this arm is the only one that runs the
    # accumulator over 90 frames and a running minimum is where drift would show. Every
    # other determinism arm here stops at 30 on the default sea, so nothing else covers it.
    foam_twice = os.path.join(workdir, "water_foam_on_b.ppm")
    foam_debug = WATER_PIN + WATER_NO_CATCHER + WATER_FOAM_DEBUG_ON
    err = render(foam_scene, foam_on, foam_debug, frames=WATER_PERSIST_FRAMES)
    if not err:
        err = render(foam_scene, foam_twice, foam_debug, frames=WATER_PERSIST_FRAMES)
    if not err:
        err = render(foam_scene, foam_off, foam_debug + ["--no-water-foam-history"],
                     frames=WATER_PERSIST_FRAMES)
    if err:
        print(f"  water-foam-persist ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-foam-persist")
    else:
        # Two runs of the SAME debug config, over the full 90-frame accumulator window --
        # see the comment on WATER_PERSIST_FRAMES above for why this is the one arm that
        # has to cover it.
        ae_repeat, _ = compare(foam_on, foam_twice)
        fw, fh, f_on_pix = _read_ppm(foam_on)
        _, _, f_off_pix = _read_ppm(foam_off)
        foam_box = _water_persist_box()
        px_on, _ = _water_foam_debug_px(f_on_pix, fw, fh, foam_box)
        px_off, _ = _water_foam_debug_px(f_off_pix, fw, fh, foam_box)
        # Qualified, like every other headline number in this function (mid_ratio,
        # sd_ratio, sens_ratio): a bare `ratio` is now bound by three separate arms.
        foam_ratio = px_on / max(px_off, 1)
        ok = (foam_ratio >= WATER_PERSIST_RATIO and px_off >= WATER_PERSIST_MIN_FOAM_PX and
              ae_repeat == 0)
        print(f"  water-foam-persist {'PASS' if ok else 'FAIL'}  open-water foam {px_off} "
              f"-> {px_on} = {foam_ratio:.2f}x (want >={WATER_PERSIST_RATIO} on at least "
              f"{WATER_PERSIST_MIN_FOAM_PX} px), {ae_repeat} px across two 90-frame runs "
              f"(want 0)")
        if not ok:
            failures.append("water-foam-persist")

    # Whitecap coverage against Monahan & O'Muircheartaigh, at nadir -- see
    # WATER_COVERAGE_STATES above for why this is the one measurement in the suite read
    # against something that came from outside the engine.
    cov_ok = True
    for wind_speed in WATER_COVERAGE_STATES:
        cov_scene = os.path.join(workdir, f"water_coverage_{int(wind_speed)}.cscn")
        _water_cscn_variant(scene, cov_scene,
                            dict(WATER_COVERAGE_SEA, windSea={"windSpeed": wind_speed}))
        cov_out = os.path.join(workdir, f"water_coverage_{int(wind_speed)}.ppm")
        err = render(cov_scene, cov_out,
                     WATER_PIN + WATER_NO_CATCHER + WATER_FOAM_DEBUG_ON + WATER_COVERAGE_CAM,
                     frames=WATER_COVERAGE_FRAMES)
        if err:
            print(f"  water-whitecap-coverage ERROR render failed at {wind_speed:.0f} m/s: "
                  f"{err.strip()[-200:]}")
            cov_ok = False
            continue
        cw, ch, cov_pix = _read_ppm(cov_out)
        foam_px, sea_px = _water_foam_debug_px(cov_pix, cw, ch)
        measured = foam_px / max(sea_px, 1)
        predicted = WATER_MONAHAN_A * wind_speed ** WATER_MONAHAN_B
        ratio = measured / max(predicted, 1e-9)
        ok = WATER_COVERAGE_RATIO_MIN <= ratio <= WATER_COVERAGE_RATIO_MAX
        print(f"  water-whitecap-coverage {'PASS' if ok else 'FAIL'}  {wind_speed:.0f} m/s: "
              f"measured {measured * 100:.3f}% vs Monahan {predicted * 100:.3f}% = "
              f"{ratio:.2f}x (want {WATER_COVERAGE_RATIO_MIN}x-{WATER_COVERAGE_RATIO_MAX}x)")
        cov_ok = cov_ok and ok
    if not cov_ok:
        failures.append("water-whitecap-coverage")

    # The transform carries the variance the seeding predicted. The first arm in this
    # suite that reads the SPECTRUM rather than a picture of it.
    probe_head, var_rows, impulse, ripple_mips = _water_fft_probe(
        WATER_PIN + ["--water-waves", "fft"])
    probe_cascades = int(probe_head.get("cascades", 0))
    cascade_rows = [row for row in var_rows if row["kind"] == "cascade"]
    ripple_rows = [row for row in var_rows if row["kind"] == "ripple"]
    if probe_head.get("available") != "1":
        # The probe's own reason, not this arm's guess at one. Without it a Gerstner
        # surface, an unseeded spectrum and a failed readback are the same empty list.
        print("  water-fft-var FAIL  --water-fft-probe declined: reason="
              f"{probe_head.get('reason', 'it printed no header at all')}")
        failures.append("water-fft-var")
    elif (probe_cascades != WATER_CASCADES or len(cascade_rows) != probe_cascades
          or len(ripple_rows) != 1):
        # Two questions: the build has the cascades this suite is written against, and the
        # probe printed a row for each of them and one for the ripple band.
        print(f"  water-fft-var FAIL  --water-fft-probe reports {probe_cascades} cascades "
              f"(want {WATER_CASCADES}) and printed {len(cascade_rows)} cascade rows and "
              f"{len(ripple_rows)} ripple rows (want one)")
        failures.append("water-fft-var")
    else:
        # NOT `hr`/`sr`: `hr` is this function's name for a frame HEIGHT in the width/height
        # pairs a dozen later arms unpack, and a list of floats wearing it is one
        # copy-pasted `h = hr` away from being live.
        height_ratios = [row["height_ratio"] for row in var_rows]
        slope_ratios = [row["slope_ratio"] for row in var_rows]
        ok = all(WATER_FFT_VAR_MIN <= v <= WATER_FFT_VAR_MAX
                 for v in height_ratios + slope_ratios)
        print(f"  water-fft-var {'PASS' if ok else 'FAIL'}  measured/predicted height "
              f"{'/'.join(f'{v:.2f}' for v in height_ratios)} slope "
              f"{'/'.join(f'{v:.2f}' for v in slope_ratios)} "
              f"(want {WATER_FFT_VAR_MIN}-{WATER_FFT_VAR_MAX} on all {2 * len(var_rows)})")
        if not ok:
            failures.append("water-fft-var")

    # The ripple band's mip chain against the slope variance seeding predicted for each level
    # (spec 13.3). That prediction is the whole of what roughness takes over from ripples a
    # footprint filtered away, and it is a model of glGenerateMipmap rather than a measurement
    # of it -- so the levels that carry the band are held to the variance arm's bounds.
    # Levels below WATER_RIPPLE_MIP_FLOOR of the whole band are where fp16 and the aliasing
    # the model ignores dominate a quantity nothing can see.
    band_slope = ripple_rows[0]["slope_pred"] if ripple_rows else 0.0
    carried = [m for m in ripple_mips
               if m.get("slope_pred", 0.0) >= WATER_RIPPLE_MIP_FLOOR * band_slope]
    if probe_head.get("available") != "1":
        print("  water-ripple-mips FAIL  --water-fft-probe declined: reason="
              f"{probe_head.get('reason', 'it printed no header at all')}")
        failures.append("water-ripple-mips")
    elif not ripple_rows or not carried:
        print(f"  water-ripple-mips FAIL  {len(ripple_rows)} ripple rows and "
              f"{len(ripple_mips)} mip rows printed, {len(carried)} of them carrying the band")
        failures.append("water-ripple-mips")
    else:
        ok = all(WATER_FFT_VAR_MIN <= m["slope_ratio"] <= WATER_FFT_VAR_MAX for m in carried)
        print(f"  water-ripple-mips {'PASS' if ok else 'FAIL'}  measured/predicted slope at "
              f"level " + ", ".join(f"{int(m['level'])} {m['slope_ratio']:.2f}"
                                    for m in carried) +
              f" (want {WATER_FFT_VAR_MIN}-{WATER_FFT_VAR_MAX})")
        if not ok:
            failures.append("water-ripple-mips")

    # The transform against its closed form. The only arm in this suite that can fail on
    # a transform which is deterministic, differs from Gerstner, and is still wrong.
    if "dc_err" not in impulse:
        why = ("the probe declined before reaching it"
               if probe_head.get("available") != "1" else "the scratch transform did not run")
        print(f"  water-fft-impulse FAIL  no impulse result: {why}")
        failures.append("water-fft-impulse")
    else:
        dc = impulse.get("dc_err", 1.0)
        mode = impulse.get("mode_err", 1.0)
        ok = dc <= WATER_FFT_IMPULSE_MAX and mode <= WATER_FFT_IMPULSE_MAX
        print(f"  water-fft-impulse {'PASS' if ok else 'FAIL'}  dc {dc:.2e} mode "
              f"{mode:.2e} (want <={WATER_FFT_IMPULSE_MAX} on both)")
        if not ok:
            failures.append("water-fft-impulse")

    # Reach invariance. `off` is the matching dry reference -- same flags, --no-water.
    reach_a = os.path.join(workdir, "water_reach_authored.ppm")
    reach_b = os.path.join(workdir, "water_reach_big.ppm")
    err = render(scene, reach_a, WATER_PIN + WATER_NO_CATCHER)
    if not err:
        err = render(scene, reach_b, WATER_PIN + WATER_NO_CATCHER +
                     ["--water-extent", WATER_REACH_BIG_EXTENT])
    if err:
        print(f"  water-horizon ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-horizon")
    else:
        wr, hr, pix_a = _read_ppm(reach_a)
        _, _, pix_b = _read_ppm(reach_b)
        _, _, pix_dry = _read_ppm(off)
        top_a = _water_top_row(pix_a, pix_dry, wr, hr)
        top_b = _water_top_row(pix_b, pix_dry, wr, hr)
        moved = abs(top_a - top_b)
        # Both must find a surface at all: two misses would report 0 movement and pass.
        found = top_a < hr and top_b < hr
        ok = found and moved <= WATER_REACH_MAX_MOVE_PX
        print(f"  water-horizon {'PASS' if ok else 'FAIL'}  top water row {top_a} -> "
              f"{top_b} of {hr} at extent x{int(float(WATER_REACH_BIG_EXTENT) / 14)}, "
              f"moved {moved} px (want <={WATER_REACH_MAX_MOVE_PX}); surface found "
              f"both sides {found}")
        if not ok:
            failures.append("water-horizon")

    # The fixture's generator still produces the fixture. No renders, no GPU: this is a
    # text comparison, and it is here because the alternative to asserting it is finding
    # out from a stripped water block and twenty-odd red arms.
    gen = os.path.join(ROOT, "assets", "generators", "gen_water_fixture.py")
    regen_dir = os.path.join(workdir, "regen")
    # An alternate corpus ROOT, so it is shaped like the real one: asset_path
    # appends the kind directory to whatever root it is given, and a generator
    # must not have to mkdir on a path it might only be reading.
    for kind in set(ASSET_KINDS.values()):
        os.makedirs(os.path.join(regen_dir, kind), exist_ok=True)
    # `proc`, not `r`: this function uses `r` as a probe-ROW loop variable in several
    # comprehensions, and `r` is the file's universal name for a CompletedProcess. The two
    # meanings only stay apart because Python scopes comprehension targets.
    proc = subprocess.run([sys.executable, gen, regen_dir], capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"  water-fixture-roundtrip FAIL  generator exited {proc.returncode}: "
              f"{(proc.stderr or proc.stdout).strip()[-200:]}")
        failures.append("water-fixture-roundtrip")
    else:
        drifted = []
        for name in ("water_fixture.gltf", WATER_FIXTURE):
            committed = asset(name)
            regenerated = os.path.join(regen_dir, *asset_subpath(name).split("/"))
            if not os.path.exists(regenerated):
                drifted.append(f"{name}: not emitted")
                continue
            # NOT `a`/`b`: those are live render paths in this function, and binding file
            # handles to them here fed a TextIOWrapper to compare() several arms later.
            with open(committed) as f_committed, open(regenerated) as f_regenerated:
                if f_committed.read() != f_regenerated.read():
                    drifted.append(name)
        # Three key sets that have to agree, and none of them transcribed here: what
        # parse_water reads, what it will accept without warning, and what the fixture
        # authors. A key added to the parser and not to the fixture leaves the corpus with
        # no coverage of it; a key in the fixture the parser does not read is a typo that
        # authors nothing, which is exactly what get_float cannot distinguish from absence.
        read_keys, known_keys, refused_keys = _cscn_key_sets("parse_water", "water")
        # Minus the refused ones, which the reader cannot tell from accepted keys and
        # which belong in neither of the two sets below (spec 11.84).
        accepted_keys = read_keys - refused_keys
        fixture_water = json.load(open(asset(WATER_FIXTURE))).get("water", {})
        fixture_keys = {k for k in fixture_water if not k.startswith("_")}
        if accepted_keys != known_keys:
            drifted.append(f"parse_water accepts {sorted(accepted_keys ^ known_keys)} "
                           "but its known[] list disagrees")
        if accepted_keys != fixture_keys:
            drifted.append(f"fixture vs parse_water differ on "
                           f"{sorted(accepted_keys ^ fixture_keys)}")
        # A refused key must be refused and NOT tolerated: listing it in known[] would
        # silence the generic warning and leave only the specific one, which is a
        # defensible-looking change that quietly makes this whole check vacuous.
        if refused_keys & known_keys:
            drifted.append(f"refused keys {sorted(refused_keys & known_keys)} are also in "
                           "known[], so they are tolerated rather than refused")
        if refused_keys & fixture_keys:
            drifted.append(f"the fixture authors refused keys {sorted(refused_keys & fixture_keys)}")
        # And the same three-way agreement one level down, for each nested train (spec
        # 11.48). Checking only the outer level would let a train's key set drift freely,
        # which is where the coverage matters most: every one of those eight is unreachable
        # from the command line, so the fixture is the corpus's only exercise of them.
        train_read, train_known, _ = _cscn_key_sets("parse_wave_train", "train")
        if train_read != train_known:
            drifted.append(f"parse_wave_train reads {sorted(train_read ^ train_known)} "
                           "but its known[] list disagrees")
        for train in sorted(_cscn_wave_train_names()):
            authored = {k for k in fixture_water.get(train, {}) if not k.startswith("_")}
            if authored != train_read:
                drifted.append(f"fixture water.{train} vs parse_wave_train differ on "
                               f"{sorted(authored ^ train_read)}")
        # The WRITE side, which the three sets above cannot see. They all describe reading a
        # .cscn; a key parsed, tolerated and authored still reaches nothing if the copy onto
        # WaterWaveTrain forgets it -- and that is silent, because get_float cannot tell a key
        # nobody applied from a key nobody authored, which is the exact defect class 11.48
        # exists to remove. The corpus cannot catch it either: water_fixture authors the
        # library defaults exactly, so every assignment in apply_wave_train is a no-op there.
        apply_has, apply_set = _cscn_wave_train_apply_fields()
        if apply_has != apply_set:
            drifted.append(f"apply_wave_train guards and writes differ on "
                           f"{sorted(apply_has ^ apply_set)}")
        parse_fields = _cscn_wave_train_parse_fields()
        if apply_set != parse_fields:
            drifted.append(f"apply_wave_train vs parse_wave_train differ on "
                           f"{sorted(apply_set ^ parse_fields)}")
        ok = not drifted
        print(f"  water-fixture-roundtrip {'PASS' if ok else 'FAIL'}  regenerated 2 files, "
              f"{len(read_keys)} water keys, {len(train_read)} per train and "
              f"{len(apply_set)} applied, "
              f"{'all identical to the committed pair' if ok else 'DRIFTED: ' + '; '.join(drifted)}")
        if not ok:
            failures.append("water-fixture-roundtrip")

    # The waterline, decided per pixel. The fixture cannot see this -- at amplitude 0.06
    # no crest ever shows its underside, so !gl_FrontFacing is never true there -- so the
    # arm authors a straddling framing of its own and reads it as an in-frame ratio.
    wl_scene = os.path.join(workdir, "water_waterline.cscn")
    _water_cscn_variant(scene, wl_scene, WATER_WATERLINE_BLOCK)
    wl = os.path.join(workdir, "water_waterline.ppm")
    err = render(wl_scene, wl, WATER_PIN + WATER_NO_CATCHER)
    if err:
        print(f"  water-waterline ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-waterline")
    else:
        ww, wh, wpix = _read_ppm(wl)
        crest = _box_luma_dense(wpix, ww, wh, WATER_WATERLINE_CREST_BOX)
        ref = _box_luma_dense(wpix, ww, wh, WATER_WATERLINE_REF_BOX)
        crest_ratio = crest / max(ref, 1e-9)
        ok = crest_ratio >= WATER_WATERLINE_MIN_RATIO
        print(f"  water-waterline {'PASS' if ok else 'FAIL'}  crest/foreground "
              f"{crest:.4f}/{ref:.4f} = {crest_ratio:.4f} (want "
              f">={WATER_WATERLINE_MIN_RATIO})")
        if not ok:
            failures.append("water-waterline")

    # Flag precedence over the scene file. No pixels: the probe reports what the surface
    # IS, and every case here was a silent defect that a rendered frame could not show.
    prec_fail = []
    for label, extra, waves, want_surface, want_model in WATER_PRECEDENCE:
        variant = os.path.join(workdir, f"water_prec_{waves}.cscn")
        _water_cscn_variant(scene, variant, {"waves": waves})
        head, _ = _water_probe(WATER_PIN + extra, scene=variant)
        got_surface = bool(head)
        if got_surface != want_surface:
            prec_fail.append(f"{label}: surface={got_surface} want {want_surface}")
        elif want_model and head.get("model") != want_model:
            prec_fail.append(f"{label}: model={head.get('model')} want {want_model}")
    ok = not prec_fail
    print(f"  water-flags  {'PASS' if ok else 'FAIL'}  {len(WATER_PRECEDENCE)} precedence "
          f"cases" + ("" if ok else "; " + "; ".join(prec_fail)))
    if not ok:
        failures.append("water-flags")

    # The CPU wave query. Nothing else in this suite can see it.
    head, rows = _water_probe(WATER_PIN)
    fft_head, fft_rows = _water_probe(WATER_PIN + ["--water-waves", "fft"])
    if not rows or not fft_rows:
        print("  water-cpu    FAIL  --water-probe printed nothing")
        failures.append("water-cpu")
    else:
        worst = max(float(r["residual"]) for r in rows)
        heights = [float(r["h"]) for r in rows]
        spread = max(heights) - min(heights)
        upright = all(float(r["n"].split(",")[1]) > 0.5 for r in rows)
        # The spectral model ANSWERS since spec 13.1, through the GPU query, and the claim
        # this clause carries is the same one with the sign flipped: until then it had to
        # say it could not, and now its answer must be a real surface rather than the plane.
        fft_heights = [float(r["h"]) for r in fft_rows if "h" in r]
        fft_real = (fft_head.get("available") == "1" and len(fft_heights) == len(fft_rows) and
                    max(fft_heights) - min(fft_heights) >= WATER_PROBE_MIN_SPREAD)
        ok = (head.get("available") == "1" and worst <= WATER_PROBE_MAX_RESIDUAL and
              spread >= WATER_PROBE_MIN_SPREAD and upright and fft_real)
        print(f"  water-cpu    {'PASS' if ok else 'FAIL'}  inverse residual <= "
              f"{worst:.5f} (want <={WATER_PROBE_MAX_RESIDUAL}), height spread "
              f"{spread:.4f} (want >={WATER_PROBE_MIN_SPREAD}), normals upright "
              f"{upright}, spectral answers {fft_real}")
        if not ok:
            failures.append("water-cpu")

    # The query against the closed form it must reproduce (spec 13.1), printed on the same
    # rows. Checked on GERSTNER, where water_surface_at is an exact reference: agreeing with
    # it on sixteen points is what proves the pass, the ring and the inversion -- the same
    # machinery that answers the spectral sea, which has no reference answer at all.
    if not rows:
        print("  water-gpu    SKIP  (--water-probe printed no grid)")
    elif not all("cpu_h" in r for r in rows):
        print(f"  water-gpu    FAIL  the query did not answer every point "
              f"(available={head.get('available')} reason={head.get('reason')}, "
              f"latency={head.get('latency')})")
        failures.append("water-gpu")
    else:
        gap = max(abs(float(r["h"]) - float(r["cpu_h"])) for r in rows)
        ok = gap <= WATER_GPU_MAX_GAP
        print(f"  water-gpu    {'PASS' if ok else 'FAIL'}  the query against the closed form "
              f"over {len(rows)} points, worst gap {gap:.8f} "
              f"(want <={WATER_GPU_MAX_GAP})")
        if not ok:
            failures.append("water-gpu")

    # A spectral sea with no energy must answer exactly its still level (spec 13.1). Every
    # term of the displacement is a product of the spectrum, so anything left over is an
    # offset the surface carries on every sea -- which is what the bound-harmonic mean
    # removal was, subtracting a constant variance instead of the sea's own.
    variant = os.path.join(workdir, "water_still.cscn")
    _water_cscn_variant(scene, variant, {"waves": "fft", "windSea": {"scale": 0.0},
                                         "swell": {"scale": 0.0}})
    still_head, still_rows = _water_probe(WATER_PIN, scene=variant)
    still_h = [float(r["h"]) for r in still_rows if "h" in r]
    if still_head.get("available") != "1" or len(still_h) != len(still_rows) or not still_h:
        print(f"  water-still  FAIL  a zero-energy sea did not answer "
              f"(available={still_head.get('available')} reason={still_head.get('reason')})")
        failures.append("water-still")
    else:
        level = float(still_head["level"])
        still_off = max(abs(h - level) for h in still_h)
        ok = still_off < 1e-6
        print(f"  water-still  {'PASS' if ok else 'FAIL'}  zero-energy sea over "
              f"{len(still_h)} points, worst distance from the still level {still_off:.8f} "
              f"(want 0)")
        if not ok:
            failures.append("water-still")

    # The caustics conserve light (spec 13.2): each landed cell carries the beam area it came
    # from, so the target averages 1 over any sea and is exactly 1 over a calm one. A pixel count
    # cannot tell this from the brighten-only heuristic it replaced; a mean can. Calm reuses
    # water-still's zero-energy sea; the dome leg traces onto a baked bed rather than the plane.
    # Every leg is held at every level (spec 13.3): the fine level is a separate trace with its
    # own window and cell, and an energy defect in it would not show in the coarse one.
    legs = {"calm": _water_caustic_probe(WATER_PIN, scene=variant),
            "gerstner": _water_caustic_probe(WATER_PIN),
            "spectral": _water_caustic_probe(WATER_PIN + ["--water-waves", "fft"]),
            "dome": _water_caustic_probe(BEACH_BED, scene=asset(BEACH_FIXTURE))}
    short = {name: f"{len(levels)} of {announced}" for name, (announced, levels) in legs.items()
             if announced == 0 or len(levels) != announced}
    # A finer level over water too deep for it is skipped rather than drawn (spec 13.3), and the
    # surface does not read it; there is no light to account for.
    drawn = {name: [lv for lv in levels if lv["drawn"]] for name, (_, levels) in legs.items()}
    skipped = [f"{name} level {int(lv['level'])}" for name, (_, levels) in legs.items()
               for lv in levels if not lv["drawn"]]
    calm = drawn["calm"]
    seas = {name: levels for name, levels in drawn.items() if name != "calm"}
    if short:
        print("  water-caustic-energy FAIL  levels rendered against levels announced: " +
              ", ".join(f"{name} {n}" for name, n in short.items()))
        failures.append("water-caustic-energy")
    else:
        calm_off = max(max(abs(lv["min"] - 1.0), abs(lv["max"] - 1.0)) for lv in calm)
        ok = calm_off <= WATER_CAUSTIC_CALM_TOL and all(
            abs(lv["mean"] - 1.0) <= WATER_CAUSTIC_MEAN_TOL[name]
            for name, levels in seas.items() for lv in levels)
        means = ", ".join("/".join(f"{lv['mean']:.4f}" for lv in levels) + " " + name
                          for name, levels in seas.items())
        print(f"  water-caustic-energy {'PASS' if ok else 'FAIL'}  calm within "
              f"{calm_off:.5f} of 1 everywhere (want <={WATER_CAUSTIC_CALM_TOL}); means by "
              f"level {means} (want within "
              f"{'/'.join(str(WATER_CAUSTIC_MEAN_TOL[n]) for n in seas)} of 1); skipped for "
              f"depth: {', '.join(skipped) or 'none'}")
        if not ok:
            failures.append("water-caustic-energy")

    # The caustics' colour (spec 13.4): the spread of one trace against 16 traced bands.
    spec_spread = os.path.join(workdir, "water_caustic_spread.ppm")
    spec_bands = os.path.join(workdir, "water_caustic_bands.ppm")
    err = render(scene, spec_spread, WATER_CAUSTIC_SPECTRUM_FLAGS)
    if not err:
        err = render(scene, spec_bands,
                     WATER_CAUSTIC_SPECTRUM_FLAGS + ["--water-caustic-bands", "16"])
    if err:
        print(f"  water-caustic-spectrum ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-caustic-spectrum")
    else:
        sw, sh, spread_pix = _read_ppm(spec_spread)
        _, _, bands_pix = _read_ppm(spec_bands)
        spread_means, spread_chroma = _water_chroma_stats(spread_pix, sw, sh,
                                                          WATER_CAUSTIC_SPECTRUM_BOX)
        bands_means, bands_chroma = _water_chroma_stats(bands_pix, sw, sh,
                                                        WATER_CAUSTIC_SPECTRUM_BOX)
        mean_off = max(abs(a / max(b, 1e-6) - 1.0) for a, b in zip(spread_means, bands_means))
        chroma_ratio = spread_chroma / max(bands_chroma, 1e-6)
        lo, hi = WATER_CAUSTIC_SPECTRUM_CHROMA
        ok = mean_off <= WATER_CAUSTIC_SPECTRUM_MEAN_TOL and lo <= chroma_ratio <= hi
        print(f"  water-caustic-spectrum {'PASS' if ok else 'FAIL'}  channels within "
              f"{mean_off:.4f} of the traced spectrum's (want <={WATER_CAUSTIC_SPECTRUM_MEAN_TOL}); "
              f"colour {spread_chroma:.4f} against {bands_chroma:.4f} = {chroma_ratio:.2f}x "
              f"(want {lo}..{hi})")
        if not ok:
            failures.append("water-caustic-spectrum")

    # The bed's relief on the caustics (spec 13.4): nothing on a featureless patch, something at
    # a brightness edge.
    relief_on = os.path.join(workdir, "water_relief_on.ppm")
    relief_off = os.path.join(workdir, "water_relief_off.ppm")
    err = render(scene, relief_on, WATER_PIN + WATER_NO_CATCHER)
    if not err:
        err = render(scene, relief_off, WATER_PIN + WATER_NO_CATCHER + ["--no-water-caustic-relief"])
    if err:
        print(f"  water-caustic-relief ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-caustic-relief")
    else:
        moved, _ = compare(relief_on, relief_off)
        rw, rh, ron_pix = _read_ppm(relief_on)
        _, _, roff_pix = _read_ppm(relief_off)
        flat = _water_box_max_delta(ron_pix, roff_pix, rw, rh, WATER_CAUSTIC_RELIEF_FLAT_BOX)
        ok = flat <= WATER_CAUSTIC_RELIEF_FLAT_MAX and moved >= WATER_CAUSTIC_RELIEF_EDGE_MIN_PX
        print(f"  water-caustic-relief {'PASS' if ok else 'FAIL'}  featureless patch moves "
              f"{flat} (want <={WATER_CAUSTIC_RELIEF_FLAT_MAX}); {moved} px moved at the edges "
              f"(want >={WATER_CAUSTIC_RELIEF_EDGE_MIN_PX})")
        if not ok:
            failures.append("water-caustic-relief")

    # Specks in the water (spec 13.4): there, and sparse.
    specks_on = os.path.join(workdir, "water_specks_on.ppm")
    specks_off = os.path.join(workdir, "water_specks_off.ppm")
    err = render(scene, specks_on, WATER_SPECKS_FLAGS)
    if not err:
        err = render(scene, specks_off, WATER_SPECKS_FLAGS + ["--no-water-specks"])
    if err:
        print(f"  water-specks ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-specks")
    else:
        dotted, _ = compare(specks_on, specks_off)
        sw, sh, _ = _read_ppm(specks_on)
        fraction = dotted / float(sw * sh)
        ok = dotted >= WATER_SPECKS_MIN_PX and fraction <= WATER_SPECKS_MAX_FRACTION
        print(f"  water-specks {'PASS' if ok else 'FAIL'}  {dotted} px of motes "
              f"(want >={WATER_SPECKS_MIN_PX}), {fraction:.4f} of the frame "
              f"(want <={WATER_SPECKS_MAX_FRACTION})")
        if not ok:
            failures.append("water-specks")

    # Touch ripples (spec 13.4): a tap spreads outward and fades, the same way every run.
    drop = WATER_PIN + ["--water-drop", WATER_RIPPLE_DROP_AT]
    early = _water_touch_probe(drop, WATER_RIPPLE_DROP_FRAMES[0])
    late = _water_touch_probe(drop, WATER_RIPPLE_DROP_FRAMES[1])
    again = _water_touch_probe(drop, WATER_RIPPLE_DROP_FRAMES[1])
    if not (early and late and again):
        print("  water-ripple-drop FAIL  the probe declined -- no ripple field was simulated")
        failures.append("water-ripple-drop")
    else:
        growth = late["radius_m"] / max(early["radius_m"], 1e-6)
        ok = (growth >= WATER_RIPPLE_DROP_MIN_GROWTH and late["peak_m"] < early["peak_m"] and
              late == again)
        print(f"  water-ripple-drop {'PASS' if ok else 'FAIL'}  ring {early['radius_m']:.3f} -> "
              f"{late['radius_m']:.3f} m = {growth:.2f}x (want >={WATER_RIPPLE_DROP_MIN_GROWTH}), "
              f"peak {early['peak_m']:.4f} -> {late['peak_m']:.4f} m (want falling), repeat "
              f"{'identical' if late == again else 'DIFFERS'}")
        if not ok:
            failures.append("water-ripple-drop")

    # A submerged surface is lit through the water (spec 13.4). Dry pixels must not move at
    # all, each submerged row's three channels must imply one depth, and that depth must not
    # change with the key's elevation.
    dw_high = asset(WATER_DOWNWELL_FIXTURE)
    dw_low = os.path.join(workdir, "water_downwell_low.cscn")
    cscn_copy(dw_high, dw_low,
              lambda d: d["lights"][0].update(direction=list(WATER_DOWNWELL_LOW_KEY)))
    dw_frames, err = {}, None
    for name, dw_scene in (("high", dw_high), ("low", dw_low)):
        for side, extra in (("on", []), ("off", ["--no-water-downwell"])):
            path = os.path.join(workdir, f"water_downwell_{name}_{side}.ppm")
            err = err or render(dw_scene, path, WATER_DOWNWELL_FLAGS + extra)
            dw_frames[name, side] = path
    if err:
        print(f"  water-downwell ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-downwell")
    else:
        pix = {k: _read_ppm(p) for k, p in dw_frames.items()}
        dw, dh = pix["high", "on"][0], pix["high", "on"][1]
        dry_delta = _water_box_max_delta(pix["high", "on"][2], pix["high", "off"][2], dw, dh,
                                         WATER_DOWNWELL_DRY_BOX)
        depths = {name: _water_downwell_depths(pix[name, "on"][2], pix[name, "off"][2], dw, dh,
                                               WATER_DOWNWELL_WET_BOX, key)
                  for name, key in (("high", WATER_DOWNWELL_KEY),
                                    ("low", WATER_DOWNWELL_LOW_KEY))}
        spreads = [(max(d) - min(d)) / max(sum(d) / 3.0, 1e-6)
                   for rows in depths.values() for d in rows.values()]
        worst = max(spreads) if spreads else float("inf")
        shared = sorted(set(depths["high"]) & set(depths["low"]))
        angle = [abs(sum(depths["low"][r]) / max(sum(depths["high"][r]), 1e-6) - 1.0)
                 for r in shared]
        worst_angle = max(angle) if angle else float("inf")
        ok = (dry_delta == 0 and len(shared) >= 10 and worst <= WATER_DOWNWELL_SPREAD_MAX and
              worst_angle <= WATER_DOWNWELL_ANGLE_TOL)
        high = [sum(depths["high"][r]) / 3.0 for r in shared]
        span = f"{min(high):.3f}..{max(high):.3f}" if high else "none"
        print(f"  water-downwell {'PASS' if ok else 'FAIL'}  dry wedge moves {dry_delta} "
              f"(want 0); {len(shared)} submerged rows (want >=10) at depths {span}; channels "
              f"agree within {worst:.3f} (want <={WATER_DOWNWELL_SPREAD_MAX}); the two key "
              f"angles within {worst_angle:.3f} (want <={WATER_DOWNWELL_ANGLE_TOL})")
        if not ok:
            failures.append("water-downwell")

    # The sea's bounds (spec 13.41), on the downwell fixture's lit floor, so the light under the
    # level is cut with the surface. The unbounded frame is the downwell arm's.
    wb, wb_err = _water_bounds_frames(workdir, dw_high) if not err else ({}, err)
    if wb_err:
        for arm in WATER_BOUNDS_ARMS:
            print(f"  {arm} ERROR render failed: {wb_err.strip()[-200:]}")
        failures += WATER_BOUNDS_ARMS
    else:
        wb["unbounded"] = dw_frames["high", "on"]
        whole, _ = compare(wb["whole"], wb["unbounded"])
        ok = whole == 0
        print(f"  water-bounds-whole {'PASS' if ok else 'FAIL'}  {whole} px from the unbounded "
              f"frame under bounds round the whole world (want 0)")
        if not ok:
            failures.append("water-bounds-whole")

        pix = {k: _read_ppm(p) for k, p in wb.items()}
        w, h = pix["unbounded"][0], pix["unbounded"][1]
        left = (0.0, 0.0, 0.5 - WATER_BOUNDS_MARGIN, 1.0)
        right = (0.5 + WATER_BOUNDS_MARGIN, 0.0, 1.0, 1.0)
        away, _ = compare(wb["away"], wb["none"])
        water_moves = [_water_box_max_delta(pix["unbounded"][2], pix["none"][2], w, h, box)
                       for box in (left, right)]
        inside = _water_box_max_delta(pix["cut"][2], pix["unbounded"][2], w, h, left)
        outside = _water_box_max_delta(pix["cut"][2], pix["none"][2], w, h, right)
        ok = away == 0 and min(water_moves) > 0 and inside == 0 and outside == 0
        print(f"  water-bounds-cut {'PASS' if ok else 'FAIL'}  bounds off the water: {away} px "
              f"from no water (want 0); cut down the middle: the inside half {inside} from the "
              f"unbounded frame and the outside half {outside} from no water at most (want 0, "
              f"0), where water moves the halves {water_moves[0]} and {water_moves[1]} (want > 0)")
        if not ok:
            failures.append("water-bounds-cut")

        # An eye under the level is in the water only inside the bounds: outside them nothing is
        # submerged, so the frame is the dry one, where unbounded the water closes round the eye.
        outside, _ = compare(wb["under_away"], wb["under_none"])
        inside, _ = compare(wb["under"], wb["under_none"])
        ok = outside == 0 and inside > 0
        print(f"  water-bounds-eye {'PASS' if ok else 'FAIL'}  an eye under the level outside "
              f"the bounds: {outside} px from no water (want 0); inside them {inside} (want > 0)")
        if not ok:
            failures.append("water-bounds-eye")

        # Dry shapes (spec 13.56), the bounds turned inside out: one off the water leaves the
        # frame alone; one over the frame's left half takes the water and the light under it
        # there and nowhere else; an eye under the level inside one is not in the water. That
        # takes one over everything, since round the eye alone the water beyond it shows from
        # below, and over everything only an eye test that forgot it would arm the water's
        # medium and move the frame.
        away, _ = compare(wb["dry_away"], wb["unbounded"])
        ok = away == 0
        print(f"  water-dry-away {'PASS' if ok else 'FAIL'}  a dry shape off the water: {away} px "
              f"from the frame with none (want 0)")
        if not ok:
            failures.append("water-dry-away")
        inside = _water_box_max_delta(pix["dry_cut"][2], pix["none"][2], w, h, left)
        outside = _water_box_max_delta(pix["dry_cut"][2], pix["unbounded"][2], w, h, right)
        ok = inside == 0 and outside == 0 and min(water_moves) > 0
        print(f"  water-dry-cut {'PASS' if ok else 'FAIL'}  dry down the middle: the dry half "
              f"{inside} from no water and the wet half {outside} from the frame with no dry "
              f"shape at most (want 0, 0)")
        if not ok:
            failures.append("water-dry-cut")
        dry_eye, _ = compare(wb["under_dry"], wb["under_none"])
        wet_eye, _ = compare(wb["under"], wb["under_none"])
        ok = dry_eye == 0 and wet_eye > 0
        print(f"  water-dry-eye {'PASS' if ok else 'FAIL'}  an eye under the level in a dry "
              f"shape over everything: {dry_eye} px from no water (want 0); with none "
              f"{wet_eye} (want > 0)")
        if not ok:
            failures.append("water-dry-eye")

    # Shoaling, which needs the diagnostic bed: every other water arm runs over a bed
    # the vertex stage cannot see, so the whole Tier 3 path was untested.
    # The no-bed reference is `fa`, already on disk: no bed IS the default, so
    # WATER_FFT_FLAGS alone is the no-bed frame and rendering it again under
    # --water-bed none produced a byte-identical duplicate.
    dome = os.path.join(workdir, "water_dome.ppm")
    nobed = os.path.join(workdir, "water_nosurf_nobed.ppm")
    err = None if not fft_ok else render(scene, dome,
                                         WATER_FFT_FLAGS + WATER_DOME_BED + WATER_NO_SURF)
    if not err and fft_ok:
        err = render(scene, nobed, WATER_FFT_FLAGS + WATER_NO_SURF)
    if not fft_ok:
        print("  water-shoal  SKIP  (the spectral render this compares against failed)")
    elif err:
        print(f"  water-shoal  ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-shoal")
    else:
        wd, hd, pixd = _read_ppm(dome)
        wn, hn, pixn = _read_ppm(nobed)
        mid_on = _water_roughness(pixd, wd, hd, WATER_SHOAL_MID_BOX)
        mid_off = _water_roughness(pixn, wn, hn, WATER_SHOAL_MID_BOX)
        open_on = _water_roughness(pixd, wd, hd, WATER_SHOAL_OPEN_BOX)
        open_off = _water_roughness(pixn, wn, hn, WATER_SHOAL_OPEN_BOX)
        mid_ratio = mid_on / max(mid_off, 1e-6)
        open_ratio = open_on / max(open_off, 1e-6)
        ok = (mid_ratio <= WATER_SHOAL_MAX_RATIO and
              abs(open_ratio - 1.0) <= WATER_SHOAL_OPEN_TOL)
        print(f"  water-shoal  {'PASS' if ok else 'FAIL'}  roughness over the bed "
              f"{mid_off:.4f} -> {mid_on:.4f} = {mid_ratio:.2f}x "
              f"(want <={WATER_SHOAL_MAX_RATIO}), beyond it {open_off:.4f} -> "
              f"{open_on:.4f} = {open_ratio:.2f}x (want 1.00 +/-"
              f"{WATER_SHOAL_OPEN_TOL})")
        if not ok:
            failures.append("water-shoal")

    # The surf itself (spec 11.44): the incident wave the shore builds, which no wave model
    # has -- both are downwind-travelling fields that the shoal factor scales to nothing at
    # the sand, so before this the sea met the beach as a still line.
    #
    # Three claims, and the second is the one that matters. It has to arrive ONLY where
    # there is a shore: a surf that moved open water would be a global amplitude knob
    # wearing a beach's name, which is the same trap water-shoal's open box guards. And it
    # has to MOVE -- a bore that stood still would be the painted band this replaces -- so
    # the third claim reads two different frame counts rather than two runs of one.
    surf = os.path.join(workdir, "water_surf.ppm")
    surf_late = os.path.join(workdir, "water_surf_late.ppm")
    surf_open = os.path.join(workdir, "water_surf_open.ppm")
    err = None if not fft_ok else render(scene, surf, WATER_FFT_FLAGS + WATER_DOME_BED)
    if not err and fft_ok:
        err = render(scene, surf_late, WATER_FFT_FLAGS + WATER_DOME_BED, frames=44)
    if not err and fft_ok:
        err = render(scene, surf_open, WATER_FFT_FLAGS)
    if not fft_ok:
        print("  water-surf   SKIP  (the spectral render this compares against failed)")
    elif err:
        print(f"  water-surf   ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-surf")
    else:
        # Against the same bed with the surf off, and against no bed with it off: the first
        # is how much surf there is, the second is whether it stayed where it belongs.
        shore_px, _ = compare(dome, surf)
        open_px, _ = compare(nobed, surf_open)
        moved_px, _ = compare(surf, surf_late)
        ok = (shore_px >= WATER_SURF_MIN_PX and open_px == 0 and
              moved_px >= WATER_SURF_MIN_MOVED)
        print(f"  water-surf   {'PASS' if ok else 'FAIL'}  {shore_px} px over the bed "
              f"(want >={WATER_SURF_MIN_PX}), {open_px} px with no bed (want 0: a surf "
              f"that moved open water is an amplitude knob), {moved_px} px between "
              f"frames 30 and 44 (want >={WATER_SURF_MIN_MOVED}: it has to come IN)")
        if not ok:
            failures.append("water-surf")

    # The shore foam band, isolated by running Gerstner: no crest foam on that path, so
    # whatever whitewater is in the frame is the shore band.
    # Its no-bed reference is `gref` -- already rendered, and no bed is the default.
    gdome = os.path.join(workdir, "water_gerstner_dome.ppm")
    err = None if not gref else render(scene, gdome, WATER_GERSTNER_REF + WATER_DOME_BED)
    if not gref:
        print("  water-shore-foam SKIP  (the gerstner reference render failed)")
    elif err:
        print(f"  water-shore-foam ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-shore-foam")
    else:
        wg, hg, pixg = _read_ppm(gdome)
        wb, hb, pixb = _read_ppm(gref)
        band = _water_foam_px(pixg, wg, hg, WATER_FOAM_BAND_BOX)
        at_open = _water_foam_px(pixg, wg, hg, WATER_FOAM_OPEN_BOX)
        at_shoal = _water_foam_px(pixg, wg, hg, WATER_FOAM_SHOALED_BOX)
        none_band = _water_foam_px(pixb, wb, hb, WATER_FOAM_BAND_BOX)
        ok = (band >= WATER_FOAM_BAND_MIN_PX and at_open == 0 and
              at_shoal >= WATER_FOAM_SWASH_MIN_PX and none_band <= WATER_FOAM_NO_BED_MAX_PX)
        print(f"  water-shore-foam {'PASS' if ok else 'FAIL'}  band {band} px "
              f"(want >={WATER_FOAM_BAND_MIN_PX}), open water {at_open} (want 0) and the "
              f"swash {at_shoal} (want >={WATER_FOAM_SWASH_MIN_PX}: foam has to REACH the "
              f"sand), same box with no bed {none_band} "
              f"(want <={WATER_FOAM_NO_BED_MAX_PX})")
        if not ok:
            failures.append("water-shore-foam")

    # Shoreline coverage, one flag apart at 4x MSAA, and then the same flag at one
    # sample where it must do nothing at all.
    hard = os.path.join(workdir, "water_shore_hard.ppm")
    err = render(scene, hard, WATER_FLAGS + ["--no-water-coverage"])
    if err:
        print(f"  water-shore-soft ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-shore-soft")
    else:
        ae_cov, _ = compare(a, hard)
        shore_window = _water_ramp_band(*WATER_SHORE_HEIGHTS, 0.30, 0.70)
        soft_fall = _water_shore_fall(a, shore_window)
        hard_fall = _water_shore_fall(hard, shore_window)
        ratio = soft_fall / max(hard_fall, 1e-6)
        ok = ae_cov >= WATER_SHORE_MIN_PX and ratio <= WATER_SHORE_FALL_RATIO
        print(f"  water-shore-soft {'PASS' if ok else 'FAIL'}  {ae_cov} px vs cutoff "
              f"(want >={WATER_SHORE_MIN_PX}), sharpest fall {soft_fall:.4f} vs "
              f"{hard_fall:.4f} = {ratio:.3f}x want <={WATER_SHORE_FALL_RATIO}")
        if not ok:
            failures.append("water-shore-soft")

    # The fallback, and it is the reason the shader carries the cutoff at all: at one
    # sample glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE) is a no-op, so a fractional alpha
    # would write the sliver at full strength instead of dropping it. --taa is how the
    # render app reaches one sample headless.
    soft_1s = os.path.join(workdir, "water_shore_1s_soft.ppm")
    hard_1s = os.path.join(workdir, "water_shore_1s_hard.ppm")
    err = render(scene, soft_1s, WATER_FLAGS + ["--taa"])
    if not err:
        err = render(scene, hard_1s, WATER_FLAGS + ["--taa", "--no-water-coverage"])
    if err:
        print(f"  water-shore-hard ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-shore-hard")
    else:
        ae_1s, _ = compare(soft_1s, hard_1s)
        # Presence first, because 0 px is also what two WATERLESS frames read. --taa is
        # the only config in this gate that runs at one sample, so nothing else here
        # establishes that the surface survives it -- and an arm whose pass condition is
        # "identical" must prove there is something to be identical about.
        w1s, h1s, pix1s = _read_ppm(soft_1s)
        present = all(_water_rb(pix1s, w1s, h1s, box) < WATER_DRY_RB_MIN
                      for box in WATER_ABSORB_BOXES)
        ok = ae_1s == 0 and present
        print(f"  water-shore-hard {'PASS' if ok else 'FAIL'}  {ae_1s} px at one sample "
              f"(want 0), water present in all three boxes {present}")
        if not ok:
            failures.append("water-shore-hard")

    # Mesh structure. The draw counts are integers; the crack check is a per-pixel
    # distance from the same frame with no water in it, over the boxes water-absorb
    # reads as means. A crack is one pixel wide and a mean would swallow it.
    clip = os.path.join(workdir, "water_clip.ppm")
    # The background a hole would show is `off` -- the same framing with the surface
    # absent, already rendered for water-live and byte-identical to a fresh one.
    clip_bg = off
    err = render(scene, clip, WATER_CLIP_FLAGS)
    if err:
        print(f"  water-crack  ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-crack")
    else:
        wc, hc, pixc = _read_ppm(clip)
        _, _, pixbg = _read_ppm(clip_bg)
        closest = min(_water_closest_to_background(pixc, pixbg, wc, hc, box)
                      for box in WATER_ABSORB_BOXES)
        ok = closest >= WATER_CRACK_MIN_DELTA
        print(f"  water-crack  {'PASS' if ok else 'FAIL'}  closest px to the no-water "
              f"frame={closest:.4f} want >={WATER_CRACK_MIN_DELTA} (a hole reads 0)")
        if not ok:
            failures.append("water-crack")

    # Far-field filtering, against its own bisect lever. Four renders, because both halves
    # are two-sided: filtering on/off says whether it is live and confined, and the same
    # pair at a second authored roughness says whether the energy went into roughness or
    # was simply dropped.
    far_on = os.path.join(workdir, "water_far_on.ppm")
    far_off = os.path.join(workdir, "water_far_off.ppm")
    far_r_on = os.path.join(workdir, "water_far_rough_on.ppm")
    far_r_off = os.path.join(workdir, "water_far_rough_off.ppm")
    rough_scene = os.path.join(workdir, "water_rough.cscn")
    cscn_copy(scene, rough_scene,
              lambda d: d["water"].update({"roughness": WATER_FAR_ROUGH_AUTHORED}))
    err = render(scene, far_on, WATER_PIN + WATER_NO_CATCHER)
    if not err:
        err = render(scene, far_off, WATER_PIN + WATER_NO_CATCHER + ["--no-water-lod"])
    if not err:
        err = render(rough_scene, far_r_on, WATER_PIN + WATER_NO_CATCHER)
    if not err:
        err = render(rough_scene, far_r_off,
                     WATER_PIN + WATER_NO_CATCHER + ["--no-water-lod"])
    if err:
        print(f"  water-farfield ERROR render failed: {err.strip()[-200:]}")
        failures.append("water-farfield")
    else:
        wf, hf, p_on = _read_ppm(far_on)
        _, _, p_off = _read_ppm(far_off)
        _, _, p_r_on = _read_ppm(far_r_on)
        _, _, p_r_off = _read_ppm(far_r_off)
        sd_on = _water_roughness(p_on, wf, hf, WATER_FAR_BOX)
        sd_off = _water_roughness(p_off, wf, hf, WATER_FAR_BOX)
        sd_ratio = sd_on / max(sd_off, 1e-9)
        near_delta = _water_box_max_delta(p_on, p_off, wf, hf, WATER_FAR_NEAR_BOX)
        sens_on = abs(_box_luma_dense(p_on, wf, hf, WATER_FAR_BOX) -
                      _box_luma_dense(p_r_on, wf, hf, WATER_FAR_BOX))
        sens_off = abs(_box_luma_dense(p_off, wf, hf, WATER_FAR_BOX) -
                       _box_luma_dense(p_r_off, wf, hf, WATER_FAR_BOX))
        sens_ratio = sens_on / max(sens_off, 1e-9)
        ok = (sd_ratio <= WATER_FAR_SD_RATIO_MAX and near_delta == 0 and
              sens_ratio <= WATER_FAR_ROUGH_RATIO_MAX)
        print(f"  water-farfield {'PASS' if ok else 'FAIL'}  far-box spread {sd_off:.4f} -> "
              f"{sd_on:.4f} = {sd_ratio:.2f}x (want <={WATER_FAR_SD_RATIO_MAX}), near box "
              f"unchanged {near_delta} (want 0), authored-roughness sensitivity "
              f"{sens_off:.5f} -> {sens_on:.5f} = {sens_ratio:.2f}x (want "
              f"<={WATER_FAR_ROUGH_RATIO_MAX})")
        if not ok:
            failures.append("water-farfield")

    clipt = _profiled_run(workdir, "water_clip", WATER_CLIP_FLAGS + ["--profiler"],
                          fixture=WATER_FIXTURE, size=("400", "300"))
    if clipt is None or "water" not in clipt.get("submit", {}):
        print("  water-draws  FAIL  no water SUBMISSION row")
        failures.append("water-draws")
    else:
        row = clipt["submit"]["water"]
        got = (row["draws"], row["instances"], row["triangles"])
        want = (WATER_GRID_DRAWS, WATER_GRID_INSTANCES, WATER_GRID_TRIANGLES)
        ok = got == want
        print(f"  water-draws  {'PASS' if ok else 'FAIL'}  "
              f"draws/instances/triangles={got[0]}/{got[1]}/{got[2]} want "
              f"{want[0]}/{want[1]}/{want[2]}")
        if not ok:
            failures.append("water-draws")

    on = _profiled_run(workdir, "water_on", WATER_FLAGS + ["--profiler"],
                       fixture=WATER_FIXTURE, size=("400", "300"))
    off_t = _profiled_run(workdir, "water_off",
                          ["--profiler", "--no-water", "--no-auto-exposure", "-E", "1.0"],
                          fixture=WATER_FIXTURE, size=("400", "300"))
    if on is None or off_t is None:
        failures.append("water-row")
    else:
        present = "water" in on["gpu"]
        absent = "water" not in off_t["gpu"]
        ok = present and absent
        print(f"  water-row    {'PASS' if ok else 'FAIL'}  present={present} absent_off={absent}")
        if not ok:
            failures.append("water-row")

    return failures


BEACH_FIXTURE = "beach_fixture.cscn"
BEACH_MESH = "beach_fixture.gltf"
# Bigger than render()'s shared 400x300 because everything here is a RING a few pixels
# wide: the island's waterline is 5.7 world units inside a 60-unit domain, so at the
# shared size a radial walk resamples the same texel and the ring has no width to find.
BEACH_SIZE = ("800", "600")
# The analytic bed is a FLAG and no scene key can ask for it. Without it bedAvailable is
# 0, so nothing shoals and the frame has no surf, no shore foam and no run-up -- the
# turquoise that remains is absorption through the depth buffer, which needs no bed.
# Every arm installs it, because the claim this fixture exists to make is that the DRAWN
# dome and the ANALYTIC one are the same dome.
BEACH_BED = ["--water-bed", "dome"]
# Gerstner throughout, deliberately, and for two reasons rather than one. Crest foam is
# FFT-only by construction, so on this path whatever whitewater is in frame is the SHORE
# band -- the isolation water-shore-foam already uses. And it removes the foam HISTORY,
# which accumulates per CASCADE texel and therefore tiles the world: with the spectral sea
# the bed's effect is measurable out to the horizon, which would make beach-surf-zone read
# as a failure of a claim it is not making.
#
# And no caustics, for the same isolation. Since spec 13.2 they run on Gerstner and trace onto
# the baked bed, so the bed changes them too -- and a caustic line is a bigger luma change than
# the shore band, which put beach-shoreline's argmax on the pattern rather than the waterline at
# half the azimuths. Caustics have their own arms in the water group.
BEACH_GERSTNER = ["--water-waves", "gerstner", "--no-water-caustics"]
# All the way round at 20 degrees. A ring asked about on one aspect is not a ring, and
# asking about every aspect at once is the one thing this fixture can do that the ramp
# cannot -- water_fixture has a single shoreline facing a single way.
BEACH_AZIMUTHS = tuple(range(0, 360, 20))
# The shore band sits SEAWARD of the waterline, because foam is on water and not on sand,
# and close to it. Measured peaks cluster at 5.5-8.1 world units against a waterline the
# mesh puts at 5.67, so this window holds the effect while a bed disagreeing with the mesh
# by more than about a quarter of the island's radius falls out of it.
BEACH_RING_INNER = -1.0
BEACH_RING_OUTER = 2.5
# How far past the window the radial search may still look. Enough that the argmax can land
# outside and fail the arm -- which it does, at the two azimuths below -- and short of the
# open water, where the bed goes on changing the image and can change it by more than the
# shore band does. Measured across the range: 16 of 18 at margins 0.5 and 1.0, 15 at 2.0,
# 14 at 4.0, and 11 from 8.0 out to an unbounded search, which FAILS. So the bound is
# load-bearing and not tidiness.
#
# The brightness argmax this replaced was barely sensitive to the same sweep -- 6 of 18 out to
# margin 2.0 and 5 from 4.0 on -- which says where its failure actually lived. 11.44 bounded
# the ray to keep the SUN out of the search and the bound was worth one azimuth; the CROWN was
# already inside the window's own inner half, where no bound could reach it. See
# _beach_ring_hits.
BEACH_RING_MARGIN = 1.0
# 16 of 18 azimuths put the bed's largest effect in that window. The two that miss are still
# the two the GLITTER crosses, as they were under the brightness statistic, but they now miss
# for the opposite reason and it is worth knowing which: their peak is at r 8.6 and 8.9, just
# seaward of the window, and it is NEGATIVE -- -114 and -86 luma. The bed is not adding foam
# out there, it is SUPPRESSING a specular highlight, because shoaling changes the surface it
# is reflected off. An unsigned argmax counts that as the bed's largest effect and it is one.
# So this stays a count rather than a requirement on every azimuth.
#
# The bar is 13 and has never moved. It was written in 11.44 against a brightness argmax
# that read 16, went red at 9 in 11.45 and 6 by 11.48, and reads 16 again under the
# difference statistic -- the ring was there for all four of those specs and the arm could
# not see it. Left at 13 deliberately: re-deriving a bar from the reading that made it green
# is how an arm stops being able to fail.
BEACH_RING_MIN_ON = 13
# Radii for the turquoise ramp, on azimuths clear of the glitter. World radii on the dome,
# so what depth each stands in comes from the mesh rather than from a screen row that has
# to be re-measured whenever the framing moves.
BEACH_DEPTH_RADII = (7.0, 9.0, 12.0, 16.0, 21.0)
BEACH_DEPTH_AZIMUTHS = (120, 140, 160, 180, 200)
# Measured 1.0757 -> 0.7962 -> 0.4967 -> 0.3194 -> 0.2500 for depths 0.29 to 5.12 m: the
# smallest step is 1.28x, so this floor sits under it and still fails a constant tint.
# Seen to fail: --no-water reads 1.2042 -> 1.2051 over the same five radii, every step
# 1.00x, because what is left is the emissive bed with no column in front of it.
# The ramp deliberately stops at 21: absorption has saturated by 27 (0.2386) and RISES
# again by 33 as sky reflection takes over, which is the same band docs/verification.md warns not to
# read absorption monotonicity through on the ramp fixture.
BEACH_DEPTH_STEP_MIN = 1.15
# A MAGNITUDE, not inequality. Output dither is on by default and puts +/-1 LSB on every
# 8-bit write, so a differing byte is not by itself a claim about the bed; at this
# threshold the innermost reach moves from r 0.25 (noise) to r 3.90 (the band).
BEACH_DIFF_THRESH = 8
# The dry crown, as a fraction of the waterline radius. Measured innermost reach across
# the 18 azimuths is 3.90 to 5.05, i.e. 0.688 of the waterline at worst, so this sits
# inside the closest approach with room and asks for exactly zero.
# Seen to fail: at --water-level 0.5, which puts the still line 0.1 under a crown 0.6
# high, the reach is r 0.20 at EVERY azimuth -- the island is drowned and there is no dry
# crown left for the bound to be about.
BEACH_CROWN_FRACTION = 0.60
# The shore band, in world units either side of the waterline, and how much of it the bed
# has to move. Per-azimuth fractions run 0.525 to 1.000 and the pooled fraction is ~0.9.
#
# The band is deliberately HALF DRY: the waterline is its centre, so the inner half is beach
# above the still line and only moves because the run-up climbs it. That makes it sensitive to
# what the swash paints, and it caught a real defect rather than needing to be loosened for
# one -- an alongshore coordinate with a cut in it fell to 0.556 here, and once the foam was
# aligned to the shore's DIRECTION instead the fraction came back at 0.690.
BEACH_BAND_HALF = 1.0
BEACH_BAND_MIN_FRAC = 0.60
# How much of the run-up ceiling the CPU twin's samples must span, and how far one primary
# period moves them. Both fractions of the ceiling rather than absolute heights, so the arm
# does not need re-tuning every time the fixture's sea state moves. Measured 0.216 and 0.171.
# Half-width of the disc `moved` reads over, in pixels. Small: it is there to span one
# filament of the foam web, not to blur the shore band into its surroundings.
BEACH_MOVED_DISC = 2
SHORE_TWIN_MIN_SPREAD = 0.10
SHORE_TWIN_MIN_DRIFT = 0.05


def _beach_render(out, extra, frames=30):
    cmd = [RENDER, "-m", asset(BEACH_FIXTURE), "-x", "-f", str(frames),
           "-W", BEACH_SIZE[0], "-H", BEACH_SIZE[1], "-S", out] + WATER_PIN + extra
    r = _run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return r.stdout + r.stderr
    return None


@functools.cache
def _beach_profile():
    """The dome's (radius, height) profile, read out of the fixture's own mesh.

    Cached because _beach_height_at is on the sampling path -- a few thousand points per
    arm, each of which would otherwise re-parse the glTF and re-decode its vertex buffer.

    Read rather than re-derived, for the reason _water_ramp_edges is. This dome's shape
    exists in three places -- render_dome_bed_height in C, gen_beach_fixture.py which built
    the mesh, and whatever a gate assumes -- and a gate that computes it from the formula is
    a third copy that agrees with the other two only until one of them moves. Reading the
    mesh leaves two, and those two are exactly the pair beach-shoreline compares.

    A surface of revolution, so one radius has one height and the ring vertices collapse.
    """
    with open(asset(BEACH_MESH)) as f:
        doc = json.load(f)
    raw = base64.b64decode(doc["buffers"][0]["uri"].split(",", 1)[1])
    prim = next(m["primitives"][0] for m in doc["meshes"] if m["name"] == "beach_dome")
    acc = doc["accessors"][prim["attributes"]["POSITION"]]
    view = doc["bufferViews"][acc["bufferView"]]
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    rings = {}
    for i in range(acc["count"]):
        x, y, z = struct.unpack_from("<3f", raw, base + i * 12)
        rings[round(math.hypot(x, z), 4)] = y
    return sorted(rings.items())


def _beach_height_at(radius):
    profile = _beach_profile()
    for i in range(1, len(profile)):
        if profile[i][0] >= radius:
            (r0, y0), (r1, y1) = profile[i - 1], profile[i]
            t = (radius - r0) / max(r1 - r0, 1e-9)
            return y0 + t * (y1 - y0)
    return profile[-1][1]


def _beach_waterline():
    """The radius where the drawn dome crosses the still level."""
    profile = _beach_profile()
    for i in range(1, len(profile)):
        (r0, y0), (r1, y1) = profile[i - 1], profile[i]
        if y0 >= 0.0 > y1:
            return r0 + (r1 - r0) * y0 / (y0 - y1)
    return profile[-1][0]


def _beach_point(radius, deg):
    """A world point ON the dome at a radius and azimuth."""
    a = math.radians(deg)
    return (radius * math.cos(a), _beach_height_at(radius), radius * math.sin(a))


def _beach_pixel(pix, w, h, project, radius, deg):
    """The pixel a dome point lands on, or None if it falls outside the frame.

    Projected into FRACTIONS first, so the read is independent of the framebuffer scale --
    a Retina context hands back twice the requested pixels.
    """
    sx, sy = project(_beach_point(radius, deg))
    px = int(sx / float(BEACH_SIZE[0]) * w)
    py = int(sy / float(BEACH_SIZE[1]) * h)
    if 0 <= px < w and 0 <= py < h:
        return (py * w + px) * 3
    return None


def _beach_ring_hits(on, off, project, window):
    """How many azimuths put the BED'S LARGEST EFFECT inside `window`.

    The argmax of |with bed - without bed| along a ray outward from the island, rather
    than a threshold on it: a threshold is a second calibration that would need its own
    justification, where the ARGMAX needs none. That much is 11.44's reasoning and it
    still holds -- what changed is the quantity, from brightness to the bed's effect on
    it.

    BRIGHTNESS COULD NOT ANSWER THE QUESTION THE ARM ASKS. The ray crosses the DRY CROWN,
    and sunlit sand and whitewater are the same brightness: measured at 800x600 the crown
    peaks at 580-591 luma against a shore band at 570-607, so the argmax was decided by
    ties of a few codes between two unrelated surfaces. At two azimuths the two peaks were
    EQUAL and the crown won on walk order alone. That is what the reported 6 of 18 was --
    not a missing ring. The band was intact underneath it the whole time, and visible in
    the same rays as a window maximum the bed lifts from 527 to 572 and from 526 to 591.

    11.44 already made this correction at the OUTER end, stopping the ray short of the
    sun's glitter so the arm would stop asking "is the shore brighter than the sun", and
    recorded the reasoning in the constants above. The crown is the same fact at the inner
    end -- but stopping short of it is NOT available, because the window's own inner half
    is dry beach by construction (BEACH_RING_INNER is negative, and deliberately: the
    run-up climbs above the still line). An arm that started its walk at the window edge
    would find sunlit sand just inside that edge -- median argmax 4.81 against a window
    opening at 4.66 -- and report a green 18 of 18 for the wrong reason. Measured.

    Differencing removes both competitors at once, and removes them structurally rather
    than by choosing where to look. The bed does not touch dry sand, so dry sand differences
    to nothing and cannot win: measured over the crown, 853 of 864 samples are bit-identical
    between the two renders and the worst channel anywhere is 1 code, which is the output
    dither's own LSB and an order below the floor. Not an assumption here either -- it is the
    standing assertion of beach-surf-zone below, whose crown samples must read EXACTLY 0
    moved, and that arm is green.

    The magnitude floor is what carries the other half of the claim, "without the bed there
    is no ring". It is not a new calibration: BEACH_DIFF_THRESH is the same LSB-vs-signal
    threshold beach-surf-zone reads, and the peaks here clear it by a wide margin (20 to
    114 against a floor of 8). A build whose bed did nothing produces no peak above dither
    at any azimuth and scores zero.
    """
    w, h, pix = on
    _, _, poff = off
    stop = window[1] + BEACH_RING_MARGIN
    hits = []
    for deg in BEACH_AZIMUTHS:
        best_r, best_d = float("nan"), -1.0
        r = 1.0
        while r < stop:
            o = _beach_pixel(pix, w, h, project, r, deg)
            if o is not None:
                d = abs((pix[o] + pix[o + 1] + pix[o + 2])
                        - (poff[o] + poff[o + 1] + poff[o + 2]))
                if d > best_d:
                    best_d, best_r = d, r
            r += 0.05
        hits.append(window[0] <= best_r <= window[1] and best_d >= BEACH_DIFF_THRESH)
    return sum(hits)


def run_beach_gate(workdir):
    """The shoaling bed is the one the eye can see (spec 11.44 phase 5).

      beach-shoreline  the shore band the ANALYTIC bed produces sits on the waterline the
                       DRAWN mesh has, all the way round. Without the bed there is no ring.
      beach-shoal      the water over that bed reddens toward the shore: R/B rises with
                       decreasing depth across five radii read off the mesh.
      beach-surf-zone  the bed's effect is bounded -- it moves the shore band and never
                       reaches the dry crown.
      shore-twin       the CPU run-up the swash film is driven by keeps the contract it
                       shares with the shader's copy (spec 11.45).

    The water corpus could not see any of this. Both goldens are Gerstner with no bed, so
    crest foam does not exist in them by construction and shore foam is identically zero:
    spec 11.43 landed three foam fixes and moved zero golden pixels. water_fixture's ramp
    can shoal, but it has ONE shoreline facing ONE way, so nothing there can tell a ring
    from a band that happens to cross the frame.

    The claim that needs a mesh is beach-shoreline's. gen_beach_fixture.py duplicates
    render_dome_bed_height on purpose -- the water shoals against the analytic field and
    this mesh is what the eye watches it shoal against -- and a duplicate that drifts would
    leave the surface shoaling against nothing visible, which is a defect no arm reading
    only the water could name.
    """
    scene = asset(BEACH_FIXTURE)
    if not os.path.exists(scene):
        print(f"  beach-shoreline SKIP  ({BEACH_FIXTURE} not present)")
        return []

    failures = []
    on = os.path.join(workdir, "beach_bed.ppm")
    off = os.path.join(workdir, "beach_nobed.ppm")
    err = _beach_render(on, BEACH_GERSTNER + BEACH_BED)
    if not err:
        err = _beach_render(off, BEACH_GERSTNER)
    if err:
        print(f"  beach-shoreline ERROR render failed: {err.strip()[-200:]}")
        return ["beach-shoreline"]

    project = _projector(_cscn_camera(BEACH_FIXTURE),
                         float(BEACH_SIZE[0]), float(BEACH_SIZE[1]))
    w, h, pix = _read_ppm(on)
    wo, ho, poff = _read_ppm(off)
    shore = _beach_waterline()
    window = (shore + BEACH_RING_INNER, shore + BEACH_RING_OUTER)
    hits = _beach_ring_hits((w, h, pix), (wo, ho, poff), project, window)
    ok = hits >= BEACH_RING_MIN_ON
    print(f"  beach-shoreline {'PASS' if ok else 'FAIL'}  waterline at r {shore:.2f} from the "
          f"mesh; the bed's largest effect on the ray lands in [{window[0]:.2f}, "
          f"{window[1]:.2f}] at {hits} of {len(BEACH_AZIMUTHS)} azimuths (want "
          f">={BEACH_RING_MIN_ON}, each clearing {BEACH_DIFF_THRESH} luma so a bed that "
          f"changed nothing scores zero)")
    if not ok:
        failures.append("beach-shoreline")

    # The turquoise ramp, as R/B against depth. Median over azimuths rather than mean: one
    # ray can cross a crest, and a crest is a bright specular sample that no amount of
    # averaging removes from a five-sample set.
    ramp = []
    for radius in BEACH_DEPTH_RADII:
        vals = []
        for deg in BEACH_DEPTH_AZIMUTHS:
            o = _beach_pixel(pix, w, h, project, radius, deg)
            if o is not None and pix[o + 2] > 0:
                vals.append(pix[o] / pix[o + 2])
        if vals:
            ramp.append(sorted(vals)[len(vals) // 2])
    steps = [ramp[i - 1] / max(ramp[i], 1e-6) for i in range(1, len(ramp))]
    ok = len(ramp) == len(BEACH_DEPTH_RADII) and all(s >= BEACH_DEPTH_STEP_MIN for s in steps)
    print(f"  beach-shoal  {'PASS' if ok else 'FAIL'}  R/B "
          f"{' -> '.join(f'{v:.4f}' for v in ramp)} at depths "
          f"{' '.join(f'{-_beach_height_at(r):.2f}' for r in BEACH_DEPTH_RADII)}; smallest "
          f"step {min(steps) if steps else float('nan'):.2f}x (want "
          f">={BEACH_DEPTH_STEP_MIN} at every one)")
    if not ok:
        failures.append("beach-shoal")

    """
    Bounded: the bed moves the shore band and leaves the dry crown exactly alone.

    Read over a small DISC rather than at the single pixel the sample lands on. Whitewater is
    a centimetre-scale web (spec 11.45), so one pixel answers whether that sample happened to
    fall on a filament or in a hole -- which is the pattern's phase and not the bed's effect,
    and it swung the pooled fraction by several per cent between builds that differed only in
    how fine the foam was. A neighbourhood asks the question the arm's name asks.
    """
    def moved(radius, deg):
        o = _beach_pixel(pix, w, h, project, radius, deg)
        if o is None:
            return None
        best = 0
        for dy in range(-BEACH_MOVED_DISC, BEACH_MOVED_DISC + 1):
            for dx in range(-BEACH_MOVED_DISC, BEACH_MOVED_DISC + 1):
                p = o + (dy * w + dx) * 3
                if 0 <= p < len(pix) - 3:
                    best = max(best, max(abs(pix[p + k] - poff[p + k]) for k in range(3)))
        return best > BEACH_DIFF_THRESH

    crown_hits, crown_n, band_hits, band_n = 0, 0, 0, 0
    for deg in BEACH_AZIMUTHS:
        for step in range(2, int(BEACH_CROWN_FRACTION * shore * 10)):
            got = moved(step / 10.0, deg)
            if got is not None:
                crown_n += 1
                crown_hits += 1 if got else 0
        for step in range(21):
            radius = shore - BEACH_BAND_HALF + step * BEACH_BAND_HALF / 10.0
            got = moved(radius, deg)
            if got is not None:
                band_n += 1
                band_hits += 1 if got else 0
    frac = band_hits / max(band_n, 1)
    ok = crown_hits == 0 and frac >= BEACH_BAND_MIN_FRAC
    print(f"  beach-surf-zone {'PASS' if ok else 'FAIL'}  the bed moves {frac:.3f} of the "
          f"shore band within {BEACH_BAND_HALF} unit of the waterline (want "
          f">={BEACH_BAND_MIN_FRAC}, {band_n} samples) and {crown_hits} of {crown_n} samples "
          f"inside r {BEACH_CROWN_FRACTION * shore:.2f} on the dry crown (want exactly 0)")
    if not ok:
        failures.append("beach-surf-zone")

    # THE CPU TWIN, against the contract it shares with the shader, and the film's history
    # window against the march the shader makes over it.
    #
    # The swash film runs on the CPU and is driven by a C copy of shore.glsl's run-up, because
    # a closed form cannot be shared between C and GLSL. What IS shared is the constants
    # (shaders/include/shore_constants.glsl), which removes the drift a hand-mirrored tuning
    # number would otherwise invite.
    #
    # BE EXACT ABOUT WHAT THE FIRST THREE PROPERTIES ESTABLISH. Every number they read comes
    # from the CPU copy, so they check that copy is self-consistent: bounded by its own
    # ceiling, spread across it, and not repeating a period later. They would NOT notice the
    # shader's arithmetic changing. The header used to claim otherwise and it was wrong -- an
    # arm that compares one copy to itself is not a comparison of two.
    #
    # THE WINDOW CHECK IS THE CROSS-FILE ONE, and it is the arm that would have caught the
    # defect this became: the solver records a tip history and the shader marches back over it,
    # and those two spans are sized in different files. They were out by a factor of ninety --
    # a ring covering a fifth of a second against taps reaching back eighteen seconds -- so
    # every tap but the first clamped to the oldest slot and the wet sand collapsed to two
    # tones, which is precisely what the accumulation exists to avoid.
    cmd = [RENDER, "-m", asset(BEACH_FIXTURE), "-x", "-f", "3",
           "-W", "320", "-H", "240", "--shore-probe"] + BEACH_BED + BEACH_GERSTNER
    r = _run(cmd, capture_output=True, text=True)

    def _fields(line):
        # Tagged lines, so the kind is read rather than sniffed from whether a field parses as
        # a number. Same shape as _water_fft_probe's reader.
        parts = line.split()
        return dict(p.split("=", 1) for p in parts[2:] if "=" in p)

    lines = [l for l in (r.stdout + r.stderr).splitlines() if l.startswith("shore-probe ")]
    head = next((_fields(l) for l in lines if l.startswith("shore-probe header")), None)
    window = next((_fields(l) for l in lines if l.startswith("shore-probe window")), None)
    if r.returncode != 0 or not head or not window:
        print(f"  shore-twin   ERROR probe produced nothing: {(r.stdout + r.stderr)[-200:]}")
        failures.append("shore-twin")
        return failures

    ceiling = float(head["ceiling"])
    slope = float(head["slope"])
    edges, repeats = [], []
    for line in lines:
        if not line.startswith("shore-probe sample"):
            continue
        f = _fields(line)
        edges.append(float(f["edge"]))
        repeats.append(float(f["next"]))
    span = float(window["span"])
    taps = float(window["taps"])
    covers = span >= taps
    within = all(0.0 <= e <= ceiling for e in edges)
    spread = (max(edges) - min(edges)) if edges else 0.0
    # The largest |edge - next| over the samples: one period on, an incommensurate sum has
    # moved somewhere else. A single train would land back where it started.
    drift = max((abs(a - b) for a, b in zip(edges, repeats)), default=0.0)
    ok = (len(edges) >= 8 and within and slope > 0.0 and covers and
          spread >= SHORE_TWIN_MIN_SPREAD * ceiling and
          drift >= SHORE_TWIN_MIN_DRIFT * ceiling)
    print(f"  shore-twin   {'PASS' if ok else 'FAIL'}  {len(edges)} samples on a slope of "
          f"{slope:.3f}, all within [0, {ceiling:.4f}]: {within}; spread "
          f"{spread / max(ceiling, 1e-9):.3f} of the ceiling (want "
          f">={SHORE_TWIN_MIN_SPREAD}) and one period on it has moved "
          f"{drift / max(ceiling, 1e-9):.3f} (want >={SHORE_TWIN_MIN_DRIFT}: incommensurate "
          f"trains do not repeat); the film's history spans {span:.2f}s against {taps:.2f}s "
          f"of taps (want >=, or the oldest taps clamp to one slot)")
    if not ok:
        failures.append("shore-twin")

    return failures
