#version 330 core

/*
 * Water surface shading (spec 11.32).
 *
 * A single-layer water interface: Schlick-approximated dielectric Fresnel, with F0
 * taken from the IOR rather than authored, splits the view into
 * a reflected share taken from the environment cubemap and a transmitted share
 * taken from the resolved opaque scene, attenuated through the body by
 * Beer-Lambert over the real path length the depth buffer gives.
 *
 * Output is LINEAR WORKING SPACE, like every other pass that writes the scene
 * HDR buffer. Anything sampled in absolute scene radiance -- the environment
 * cubemap, the authored scatter colour -- is multiplied by preExposure here; the
 * resolved scene colour is NOT, because it was written pre-exposed and would be
 * counted twice. view.glsl is the authority on that contract.
 */

// The full G-buffer. Every location the opaque draw-buffer list may enable has to
// be written: slots are turned on for the FRAME, by whether a consumer wants them,
// not per draw -- so a location enabled and left unwritten keeps whatever the
// previous pass put there.
layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 NormalOut;
layout(location = 2) out vec4 VelocityOut;
layout(location = 3) out vec4 AlbedoOut;
layout(location = 4) out vec4 DiffuseOut;
layout(location = 7) out vec4 SpecOut;

in vec3 WorldPos;
in vec3 ViewPos;
in vec3 Normal;
in vec4 CurrClip;
in vec4 PrevClip;
in float Jacobian;
in float Shoal;
in float Breaking;
// What the shore foam rides: the bed's downhill (unit, zero with no bed) and how far the
// tongue has run up the face. See the shore breakup.
in vec2 ShoreDir;
in float SwashRun;
in float Surf;
// Mean square slope of the displacing bands that the vertex stage's footprint filtered
// away. The other half of the geometry-to-BRDF handover: what left the mesh arrives here
// as roughness.
in float FilteredMss;
// The undisplaced planar parameter, which is the water parcel's LABEL. See water_vert.
in vec2 SurfParam;

uniform mat4 view;
uniform mat4 projection;
uniform vec2 screenSize;
// The animation clock, for the foam breakup's drift. The same value water_vert reads, so
// the two cannot describe different instants of one surface.
uniform float time;

uniform float waterRoughness;
uniform float waterIor;
uniform vec3 waterAbsorption; // extinction per world unit, per channel
uniform vec3 waterScatter;    // in-scattered colour, absolute scene radiance

// Mipped resolve of the opaque scene (colour + skybox), already pre-exposed.
uniform sampler2D sceneColorTex;
uniform int sceneColorAvailable;
// Single-sample resolved scene depth: the water column and the shoreline test.
uniform sampler2D sceneDepthTex;
uniform int sceneDepthAvailable;

uniform vec3 sunDir; // toward the sun, world space
uniform int sunAvailable;
// Scene radiance, NOT pre-exposed, with the atmosphere's transmittance already folded into
// it by the sky. Multiplied by preExposure where it is used; view.glsl is the authority.
uniform vec3 sunRadiance;
// Cascade slot of the sun, or -1 where it casts nothing. Gates both the shadow lookup and
// the deck, which names its occluded light the same way.
uniform int sunShadowSlot;
// 0 = no analytic sun lobe, which is the frame every capture before spec 11.42 rendered.
uniform int glitterEnabled;
// Accumulated foam, one band per channel, each in its OWN band's tiling space -- so this
// is sampled three times at three UVs rather than once. 0 = the pass has not run, and the
// instantaneous fold below is the whole selection.
uniform sampler2D foamTex;
uniform int foamAvailable;
uniform int cameraSubmerged;
uniform int causticsEnabled;
// 1 = the shoreline writes fractional coverage for alpha-to-coverage; 0 = the
// binary cutoff, which is all a single-sample target can express.
uniform int alphaToCoverage;

// Split-sum environment cubemap, engine-bound. The procedural sky bakes into it,
// so a reflection taken from here tracks the sun without asking.
uniform samplerCube prefilteredMap;
uniform sampler2D brdfLUT;
uniform int iblEnabled;
uniform float iblIntensity;
uniform float maxReflectionLOD;

#include "view.glsl"
#include "velocity.glsl"
#include "fresnel.glsl"
#include "depth.glsl"
// The surface definition, for its cascade samplers and its wave-model switch. The
// fragment stage does not re-evaluate the surface -- the vertex stage's position
// and normal are interpolated in -- but it reads the SHORT band, which by design
// never reaches the mesh.
#include "ocean.glsl"
#include "cloud_shadow.glsl"
// The cascades, for the glitter. The OUTERMOST-cascade lookup rather than pbr_frag's
// per-fragment PCSS selection, and that is the right one here rather than the cheap one:
// the widest cascade is scene-fitted and camera-independent, so a surface running to the
// horizon takes no cascade seam across itself. Same path the catcher and the motes use.
#define CSM_OUTERMOST_PCF
#define CSM_PCF_HALF_KERNEL 2
#include "csm.glsl"

// Coarsest mip the transmission may select. The resolve stops generating there.
const float WATER_TRANSMISSION_MAX_LOD = 6.0;
/*
 * Absorption is evaluated over a clamped path: past a few extinction lengths the
 * exponential is already zero, and an unbounded value from a sky-depth sample
 * would only cost precision.
 *
 * The budget is in EXTINCTION LENGTHS, which is scale-free; the same number in world
 * units means what it says at exactly one world scale. 3.84 is 64 units at a blue
 * extinction of 0.06 per unit -- clear seawater where a unit is a metre, which is what
 * this was tuned against.
 *
 * The WEAKEST channel sets it: the clamp has to sit past the point the LAST channel
 * dies, and in water that is blue. Truncation at the clamp therefore leaves at most
 * exp(-3.84) = 2.15%, a derivative kink rather than a step.
 *
 * 5.54 -- ln(255), where the leak falls under one 8-bit code -- was the alternative,
 * and 2.15% against 0.39% did not pay for re-baking the fixture golden and
 * re-validating every water arm. Raising it later is this one line.
 *
 * WATER_MAX_PATH survives as a FLOOR, so this can only lengthen a clamp, never shorten
 * one -- no scene loses reach to the rounding of the division.
 */
const float WATER_MAX_PATH = 64.0;
const float WATER_MAX_OPTICAL_DEPTH = 3.84;
/*
 * Floor on the extinction the budget divides by, and it is load-bearing rather than
 * defensive: a channel of exactly 0 is authorable (`water{ absorption }` takes what it
 * is given) and would make the budget infinite, then `exp(-0.0 * inf)` is NaN -- one
 * zeroed channel would take the whole surface with it.
 *
 * 1e-4 per unit is a 10,000-unit e-folding length, past every far plane in this tree, so
 * the floor can only bind on water nothing could see through in the first place.
 */
const float WATER_MIN_EXTINCTION = 1e-4;
// Below this column the surface has emerged through the bed; see the discard.
const float WATER_MIN_PATH = 0.01;
// Coverage below this contributes no samples at any supported sample count, so the
// fragment is dropped rather than shaded for nothing.
const float WATER_MIN_COVERAGE = 0.02;
// Depth at or above which the buffer is holding its cleared value rather than geometry. One
// code short of 1.0 at 24 bits, so a real surface sitting exactly on the far plane is the only
// thing this can misread -- and that surface is already at the limit of what depth can express.
const float WATER_DEPTH_EMPTY = 0.99999;
// Ceiling on the refraction bend, in METRES. The bend is a screen-space
// approximation, and past a metre or so of offset the sample it reaches has
// little to do with the ray -- the validity check below catches the wrong ones,
// but not reaching for them is cheaper than rejecting them.
const float WATER_MAX_BEND_M = 1.0;
// How far behind the surface a refraction probe has to be before its sample is trusted
// fully, in METRES. The bend fades out over this rather than switching off at a threshold:
// a switch on a continuous quantity draws a visible edge along wherever it flips.
const float WATER_BEND_FADE_M = 0.40;
// The short cascade's resolved slope is faded out between these view distances,
// its energy moving into roughness. Metres: at 12 m tiling, past ~120 m a period
// is a couple of pixels and a literal normal there is aliasing, not detail.
//
// These were world-unit literals saying the same numbers, which is why they read as metres
// already -- and why they were wrong by the world's scale everywhere a unit was not one.
const float WATER_SHORT_NEAR_M = 42.0;
const float WATER_SHORT_FAR_M = 118.0;
const float WATER_SHORT_SLOPE_GAIN = 0.42;
// How close to grazing the SHADING normal may get before it is bent back toward the eye. A
// normal pointing away describes a surface you could not be looking at, and both ways of
// leaving it that way print as artifacts -- see the bend in main().
const float WATER_MIN_NDV = 0.02;
// Jacobian compression at which foam starts and saturates.
const float WATER_FOAM_ON = 0.16;
const float WATER_FOAM_FULL = 0.42;
/*
 * Whitewater is very nearly white, and the grey this used to be is why a swash over pale
 * sand was invisible.
 *
 * Entrained air makes foam one of the brightest natural surfaces -- an albedo up around
 * 0.9, higher than dry sand and far higher than wet. At 0.72 it sat BELOW the beach it was
 * running over (0.93 since 11.44), so mixing toward it at the waterline made the frame
 * slightly darker and greyer, which reads as nothing at all. "Not white, a bright grey with
 * the sky in it" describes foam under an overcast; the sky it carries arrives through the
 * lighting now and does not need to be baked into the albedo as well.
 */
const vec3 WATER_FOAM_COLOR = vec3(0.95, 0.97, 0.97);
// Lambertian normalisation for the foam's direct term. The ambient half needs none: a mip
// of the environment is already an average radiance, where the sun arrives as one.
const float WATER_INV_PI = 0.31830988618;
/*
 * The bubble crust: cycles per METRE, how far it drifts, and how hard it tilts the normal.
 *
 * Finer than the breakup noise, which chooses WHERE foam is -- this is the texture of the
 * foam that is already there. 3.2 per metre is roughly a 30 cm clump, which is what reads
 * as bubbles at the distance a shore is looked at rather than as sandpaper.
 *
 * STEP is the finite-difference offset for the gradient, in noise units, and wants to stay
 * well under a cycle or the difference stops describing the slope it is standing on.
 */
const float WATER_FOAM_BUBBLE_PER_M = 3.2;
const vec2 WATER_FOAM_BUBBLE_DRIFT = vec2(0.021, -0.014);
const float WATER_FOAM_BUBBLE_STEP = 0.28;
const float WATER_FOAM_BUBBLE_RELIEF = 2.6;
// How thin the crust gets where the noise is darkest. Not zero: foam that vanishes in the
// troughs reads as holes punched in it rather than as varying thickness.
const float WATER_FOAM_BUBBLE_MIN = 0.55;
// How far light wraps around the foam past the terminator. 1 lights the whole sphere and
// reads as a self-lit blob; 0.6 keeps a sunward side while letting a shore under a sun on
// the horizon still carry surf.
const float WATER_FOAM_WRAP = 0.6;
// How completely foam replaces what is under it. Whitewater is opaque, and a swash over
// shallow water is the one place that matters -- at 0.62 the pale bed still showed through
// enough to keep the two indistinguishable.
const float WATER_FOAM_MAX = 0.88;
// Shore band strength. A breaking shore IS mostly whitewater, so this sits near the crest
// value rather than well under it -- the old 0.45 was set when the band covered the whole
// shelf, where that much foam was a wash. Confined to the swash it can be what a swash is.
const float WATER_SHORE_FOAM = 0.92;
// What is left of the shore band between waves, as a fraction of it. Not zero: the swash
// zone is never clean water, and a band that vanished entirely between bores would flash.
const float WATER_SHORE_FOAM_REST = 0.3;
// How far out the swash reaches, as shoal factor. Small: a swash is the last run of water
// up the sand, and anything wider is a sheet laid over the whole shelf -- which water-shoal
// reads directly, since foam is flat and the roughness it measures is not.
const float WATER_SWASH_SHOAL = 0.10;
/*
 * The breakup's base frequency, in cycles per METRE. waterFoamPattern lays its other two
 * bands on at 2.3x and 5.7x this, so one number sets the whole web's scale.
 *
 * FOAM IS CENTIMETRE-SCALE, and this is the constant that says so. At 0.42 a cell spanned
 * 2.4 m, which is wider than most of the whitewater in a frame -- the ridge was there and
 * every filament of it was metres across, so the pattern still read as lumps however it was
 * folded. 4 cycles a metre puts a cell at 25 cm, which is a clump of bubbles rather than a
 * bank of them, and the fine band inside takes it to 4 cm.
 *
 * There is a floor under this and it is aliasing: a web finer than the footprint averages to
 * a flat grey wash, which is the failure the whole Jacobian selection exists to avoid. The far
 * field is protected by the foam FADING there rather than by this number, so anything much
 * finer wants a mip chain and therefore a sampler this program does not have.
 */
const float WATER_FOAM_NOISE_A_PER_M = 4.0;
/*
 * The breakup is a COVERAGE THRESHOLD, not a brightness modulation, and the threshold rises
 * as the foam thins.
 *
 * Scaling foam by noise cannot fragment it: a sheet times a pattern is still a sheet, dimmer
 * in places, continuous everywhere. What whitewater actually does as it drains is come apart
 * -- the thin parts of the pattern go first and what is left is clumps, which keep their full
 * brightness because a clump of foam is as white as a sheet of it. So the noise chooses WHERE
 * rather than HOW MUCH, and how much foam there is chooses how high the bar sits.
 *
 * HI is the bar with no foam at all and SPAN is how far a full band lowers it. Calibrated
 * against the breakup's own distribution rather than by eye: it is two value noises at 0.62
 * and 0.38, so it concentrates about 0.5 with a spread near 0.15. A bore at full strength
 * puts the bar about a spread BELOW the mean and covers ~85% -- a bore is a sheet. The rest
 * value between waves puts it just above the mean, near half coverage, which is where a
 * pattern breaks into islands. EDGE is the soft ramp across the bar, wide enough not to
 * alias and narrow enough that the edge of a clump is an edge.
 *
 * The bar cannot be set from the shape of real whitewater alone, because how long foam LASTS
 * decides how much of it is there to erode. The reference this is ported from holds its
 * accumulation near full across a whole swash and can therefore erode much harder; ours
 * collapses to WATER_SHORE_FOAM_REST between bores, and a bar tuned for a sea that remembers
 * took the swash from 4000 whitewater pixels to 1741 -- foam that no longer reached the sand.
 * Persistence is what buys a harder bar, so these two numbers are due a revisit alongside it.
 *
 * This replaces a multiplicative floor that existed because the old form punched holes in
 * full-strength foam. A bar tied to the amount cannot do that -- at full strength there is no
 * hole to punch -- so the floor is not needed and would only stop the foam ever leaving.
 */
const float WATER_FOAM_ERODE_HI = 1.05;
const float WATER_FOAM_ERODE_SPAN = 1.15;
const float WATER_FOAM_ERODE_EDGE = 0.15;
// How hard the coarse band is folded about zero. Higher is a thinner, sharper web; at 1 the
// ridge is as wide as the noise itself and stops reading as a filament at all.
const float WATER_FOAM_RIDGE = 1.2;
/*
 * Caustics. Light crossing the surface is focused by the surface's own curvature,
 * so the brightness on the bed is a property of the WAVES above the point being
 * lit -- not of a texture pasted onto the floor. Sampling the cascade derivatives
 * where the refracted sun ray CROSSED the surface is what ties the two together;
 * the reference study rejected cellular caustics for exactly this reason and then
 * WATER_CAUSTIC_GAIN sets the strength.
 */
const float WATER_CAUSTIC_ON = 0.060;
const float WATER_CAUSTIC_FULL = 0.27;
const float WATER_CAUSTIC_GAIN = 1.35;
// Caustics need water above them to focus through, and lose coherence with depth. Path
// lengths, so METRES: a window written in world units puts the whole effect in the first
// centimetre of a world at 22 units to the metre.
const float WATER_CAUSTIC_SHALLOW_M = 0.35;
const float WATER_CAUSTIC_DEEP_ON_M = 9.0;
const float WATER_CAUSTIC_DEEP_OFF_M = 20.0;

/*
 * COX-MUNK SUN GLITTER (spec 11.42).
 *
 * The sea's specular response to the sun is not a GGX highlight: it is an anisotropic
 * Gaussian slope distribution, wider along the wind than across it, and that anisotropy is
 * what stretches the glitter into a path rather than a blob. Cox and Munk measured it from
 * photographs of exactly this.
 *
 * The two variances come from the SPECTRUM this sea was seeded with, not from Cox-Munk's
 * clean-sea regression on wind speed. The regression is a stand-in for a spectrum, and
 * there is a real one here -- so the glitter widens when the sea state does, for the same
 * reason and by the same number as the rest of the surface.
 *
 * The variance passed in is the UNRESOLVED slope, the same quantity the roughness above is
 * built from. Feeding it the total would count the waves twice: the ones the mesh and the
 * short band already resolve are in the normal, and a lobe as wide as the whole spectrum
 * sitting on top of them is a second sea.
 *
 * The spectrum carries MORE slope than Cox and Munk's wind regression, not less -- measured
 * 0.101 against their 0.062 for an 11.5 m/s sea, printed at seeding. So there is no capillary
 * remainder owed to this lobe from below the cascades' cutoff, and a term for one would be
 * dead arithmetic. What the lobe wants is the unresolved part, and the unresolved part is
 * what the footprint dropped.
 */
// Cox-Munk's measured upwind/crosswind ratio. The SPLIT is theirs; the total is the
// spectrum's, because the seeding accumulates a single isotropic k^2 sum and a directional
// pair would have to survive the same realisation noise the probe already reports.
const float WATER_GLITTER_ANISO = 1.45;
// Floor on either variance. A perfectly resolved surface has no unresolved slope at all,
// and a zero-width Gaussian is a division by zero rather than a sharp highlight.
const float WATER_GLITTER_MIN_VAR = 1.0e-5;
// Smith masking-shadowing at the interface, at a fixed roughness. The lobe's own width is
// already in the distribution, and threading it through here as well made the horizon
// darken as the sea roughened, which is backwards for a surface whose grazing reflection
// should approach total.
const float WATER_GLITTER_SMITH_K = 0.0307;
// Dimensionless ceiling on the lobe, in the sense pbr_frag's BRDF_MAX is: it bounds the
// reflectance rather than the radiance, so it is independent of the sun's magnitude and of
// the exposure. The distribution goes as 1/variance, so a nearly resolved surface -- calm
// water close to the camera -- puts a spike here that no tonemap recovers from.
const float WATER_GLITTER_MAX = 400.0;

/*
 * Value noise, for breaking foam coverage up after it has been selected.
 *
 * ALU rather than a texture: this program has no sampler declaration to spare, and a hash
 * costs less than the fetch would anyway. Smoothstep interpolation rather than linear, so
 * the derivative is continuous and the pattern has no lattice creases in it.
 */
float waterHash21(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float waterValueNoise(vec2 p) {
    vec2 cell = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(waterHash21(cell), waterHash21(cell + vec2(1.0, 0.0)), f.x),
               mix(waterHash21(cell + vec2(0.0, 1.0)), waterHash21(cell + vec2(1.0, 1.0)), f.x),
               f.y);
}

/*
 * THE FOAM PATTERN, and the ridge is the whole of it.
 *
 * Value noise is smooth and its level sets are blobs, so thresholding it -- however cleverly
 * -- gives rounded lumps. Whitewater is not lumps. It is a WEB: thin films stretched between
 * bubbles, so its structure is filaments and the cells between them.
 *
 * Folding a zero-mean field about zero is what produces that. `1 - |n|` puts a crest along
 * every zero crossing of the noise, which is a curve rather than a region, and squaring it
 * sharpens the crest into a strand. Three bands, coarse to fine, and only the coarsest is
 * ridged: it carries the web, and the other two are there to keep the strands from reading as
 * a regular mesh.
 *
 * The weights are the ones the structure was tuned at and the constant term matters as much as
 * the rest -- it is the wash BETWEEN the filaments, without which the pattern thresholds into
 * disconnected specks instead of a net that thins.
 */
float waterFoamPattern(vec2 p) {
    float web = waterValueNoise(p) * 2.0 - 1.0;
    float w = max(0.0, 1.0 - WATER_FOAM_RIDGE * abs(web));
    float mid = waterValueNoise(p * 2.3 + vec2(11.3, 4.7)) * 2.0 - 1.0;
    float fine = waterValueNoise(p * 5.7 - vec2(7.1, 2.9)) * 2.0 - 1.0;
    return clamp(0.55 * w * w + 0.25 + 0.18 * mid + 0.12 * fine, 0.0, 1.0);
}

float waterSmith(float ndx) {
    return ndx / (ndx * (1.0 - WATER_GLITTER_SMITH_K) + WATER_GLITTER_SMITH_K);
}

// All vectors in VIEW space, which is where this shader does its shading. windV is the
// wind direction carried into view space; it need not be tangent to the surface, since the
// frame below projects it.
float waterSunGlitter(vec3 N, vec3 V, vec3 L, vec3 windV, float mss) {
    float ndv = max(dot(N, V), 1.0e-3);
    float ndl = max(dot(N, L), 1.0e-3);
    vec3 H = normalize(V + L);
    float ndh = max(dot(N, H), 1.0e-3);

    // A surface frame aligned to the wind. Degenerate looking straight down the wind, so
    // the cross product is taken against the normal rather than against the wind itself.
    vec3 T = windV - N * dot(windV, N);
    float tLen = length(T);
    T = tLen > 1.0e-4 ? T / tLen : normalize(cross(N, vec3(0.0, 0.0, 1.0)) + vec3(1.0e-4));
    vec3 B = normalize(cross(N, T));

    float varAlong = max(mss * WATER_GLITTER_ANISO / (1.0 + WATER_GLITTER_ANISO),
                         WATER_GLITTER_MIN_VAR);
    float varAcross = max(mss / (1.0 + WATER_GLITTER_ANISO), WATER_GLITTER_MIN_VAR);
    // Facet slopes: the half-vector's tilt out of the surface plane, per axis.
    float sAlong = dot(H, T) / ndh;
    float sAcross = dot(H, B) / ndh;
    float pdf = exp(-0.5 * (sAlong * sAlong / varAlong + sAcross * sAcross / varAcross)) /
                (6.28318530718 * sqrt(varAlong * varAcross));
    // Slope density to facet density: the Jacobian of the slope-to-normal map is 1/cos^4.
    float D = pdf / max(ndh * ndh * ndh * ndh, 1.0e-4);
    float G = waterSmith(ndv) * waterSmith(ndl);
    // The cosine that would multiply the incoming radiance cancels the one in the
    // microfacet denominator, so this is the whole reflectance and the caller multiplies
    // by radiance alone.
    return D * G / max(4.0 * ndv, 1.0e-3);
}

void main() {
    vec2 uv = gl_FragCoord.xy / max(screenSize, vec2(1.0));

    vec3 N = normalize(Normal);
    /*
     * The geometry-to-BRDF handover, which arrives from two directions.
     *
     * `FilteredMss` is the mean square slope the vertex stage's cell footprint removed from
     * the bands that DISPLACE -- mip levels on the spectral path, dropped octaves on the
     * Gerstner one -- and it is what makes the far field a widening specular lobe rather
     * than the glass plane the filtering leaves behind. Below, the short band adds what its
     * own distance fade removed from the band that only SHADES.
     *
     * SUMMED, where this used to take the larger of two fractions. The cascades own
     * disjoint wavenumber windows, so there is no overlap to double-count: what each band
     * loses is slope variance the others never carried, and variances of independent
     * fields add. The old `max` was avoiding a double count that the abutting cutoffs had
     * already made impossible.
     */
    float removedMss = FilteredMss;
    float foam = 0.0;
    if (waveModel == 1) {
        // The short cascade shades but never displaces. Carried into the mesh it
        // would alias into a ridged texture; carried into the NORMAL alone it
        // shimmers as the waves recede past a pixel. So its slope is faded out
        // with distance and its remaining energy moved into roughness instead --
        // the geometry-to-BRDF transition, which is what keeps the horizon stable
        // without throwing the simulated detail away.
        // Sampled at the DISPLACED position, where the vertex stage samples at the
        // undisplaced grid parameter. The two lattices are offset by the long and medium
        // horizontal displacement, and that is deliberate: this band shades the surface
        // the eye sees, so its detail belongs where that surface ended up.
        // LOD 0 explicitly. oceanCascadeUv wraps with fract, so the implicit screen
        // derivative reads a whole period across every tile seam -- which, the moment this
        // band's texture carries mips, prints as a blurred line through each of them. The
        // short band is not mipped today and this still states the intent rather than
        // relying on it.
        vec2 shortUv = oceanCascadeUv(WorldPos.xz, 2);
        vec4 short0 = textureLod(cascade2_0, shortUv, 0.0);
        vec4 short1 = textureLod(cascade2_1, shortUv, 0.0);
        float fade = 1.0 - smoothstep(WATER_SHORT_NEAR_M * waterUnitsPerMetre,
                                      WATER_SHORT_FAR_M * waterUnitsPerMetre,
                                      length(ViewPos));
        vec2 shortSlope = short1.rg * fade;
        N = normalize(N + vec3(-shortSlope.x, 0.0, -shortSlope.y) * WATER_SHORT_SLOPE_GAIN);
        // Roughness takes the slope variance the fade REMOVED, which is what makes this a
        // handover rather than two channels dimming together. Driving it from the faded
        // slope instead sent distant water toward the calm value in both, so the horizon
        // got smoother as its waves went sub-pixel, which is backwards.
        //
        // The band's OWN variance is what is scaled, not this one texel's slope: a
        // per-texel read would make the lobe width depend on where in the wave the pixel
        // happened to land, where the removed detail is a property of the whole band.
        //
        // The kept scale is the FADE ONLY, deliberately. WATER_SHORT_SLOPE_GAIN also scales
        // the slope reaching the normal, so a strict accounting would fold it in here too --
        // but the gain is 0.42, so that hands 82% of this band to roughness even in the
        // fully resolved near field, where this expression currently hands over nothing.
        // Measured: it flattens water-shoal's shoaled-vs-open contrast from 0.54x to 0.96x.
        // That is a look decision about what the gain MEANS -- an artistic damping, or a
        // physical attenuation whose remainder is owed back -- and it wants an A/B rather
        // than being smuggled in behind a correctness fix.
        removedMss += oceanRemovedMss(fade, cascadeSlopeVar[2]);

        // Foam where the horizontal map COMPRESSES, from the vertex Jacobian plus
        // the short band's own. Deformation, not a height threshold: a tall smooth
        // swell does not break and a compressing one does, and selecting on height
        // puts white on the wrong crests.
        float shortJ = oceanBandJacobian(short0, short1, cascadeChoppiness[2]);
        // This frame's fold. The vertex Jacobian is the only one carrying the shoal
        // gradient, so it stays whatever else is added to it.
        float compression = max(0.0, 1.0 - Jacobian) + max(0.0, 1.0 - shortJ) * fade * 0.62;
        if (foamAvailable == 1) {
            /*
             * The trail the crest left behind (spec 11.42).
             *
             * Three fetches from one texture, each in its own band's tiling space, because
             * that is the space the accumulator read its Jacobian in. The value is a
             * running minimum with a leak, so 1 - it is compression that OUTLIVES the fold
             * -- which is what puts foam behind a breaking wave rather than only on it.
             *
             * Keyed on SurfParam, the UNDISPLACED parameter, not on WorldPos.xz. The
             * accumulator writes one texel per cascade cell, and a cascade cell is indexed
             * by that parameter -- so foam belongs to the water parcel that folded, and it
             * has to ride that parcel's displacement. Reading it at the displaced position
             * instead looked the foam up under a DIFFERENT parcel, off by the horizontal
             * map, and since that offset changes every frame the whitewater slid around
             * over a surface it was supposed to be sitting on.
             *
             * Combined with max against the instantaneous term rather than replacing it:
             * the accumulator cannot see the shoal gradient, so a surf-zone crest still
             * needs the vertex Jacobian to be selected at all.
             */
            float turbLong = texture(foamTex, oceanCascadeUv(SurfParam, 0)).r;
            float turbMed = texture(foamTex, oceanCascadeUv(SurfParam, 1)).g;
            float turbShort = texture(foamTex, oceanCascadeUv(SurfParam, 2)).b;
            float persistent = max(0.0, 1.0 - min(turbLong, turbMed)) +
                               max(0.0, 1.0 - turbShort) * fade * 0.62;
            compression = max(compression, persistent);
        }
        /*
         * NOT scaled by `fade` again. The short band's share of `compression` above is
         * already weighted by it, and the rest comes from the vertex Jacobian, which is the
         * long and medium bands and has nothing to do with the short band's aliasing.
         *
         * Multiplying the total by it a second time zeroed ALL crest foam past
         * WATER_SHORT_FAR, however hard the surface was folding -- so whitecaps stopped at a
         * fixed distance from the eye, which projects onto a flat sea as a straight line
         * across the frame. Whitecaps run to the horizon on a real sea.
         */
        foam = smoothstep(WATER_FOAM_ON, WATER_FOAM_FULL, compression);
    }
    /*
     * Shore foam, on both wave models. The whitewater a beach carries even where nothing is
     * breaking, strongest in the SWASH at the water's edge and fading out to sea.
     *
     * THERE IS NO INNER EDGE, and that is the point. The band used to open over shoal 0.02
     * to 0.22, on the reasoning that at shoal 0 the surface is about to be discarded anyway.
     * That held while the shoal window was being read as 2.56 WORLD UNITS and 0.02 of it was
     * a couple of centimetres. Once 11.44 made the window the 2.56 METRES it always meant,
     * the same expression left 5.4 m of waterline with no foam on it -- the sea met the sand
     * at a bare line, which is the one thing a beach never does. Narrowing the ramp only
     * narrowed the bare strip.
     *
     * The swash is STRONGEST at the water's edge and fades out to sea, so the band is a
     * single falling edge, full where the water is shallowest. What stops it drawing foam on
     * dry ground is the discard, which runs off the water COLUMN and not off this, with
     * derivative coverage already feathering it.
     *
     * It falls FAST, and how fast is a measured constraint rather than a taste. Carried out
     * to shoal 0.72 -- the old upper edge -- it put full-strength foam over every metre of
     * depth on the shelf, which is a sheet and not a swash, and it flattened water-shoal's
     * roughness contrast from 0.34x to 0.80x: the foam masking the shoaling it sits on. A
     * swash is thin on a real beach for the same reason it has to be thin here.
     *
     * No bedAvailable guard: with no bed the shoal factor is exactly 1 everywhere, so this is
     * zero. The uniform test enforced the same fact a second time.
     *
     * AND IT MOVES. The band is a function of depth, which does not change, so on its own it
     * is a painted stripe the sea slides under. What breaks and runs up is the surf wave, and
     * its crest fraction arrives from the vertex stage: the whitewater is full on the bore
     * and on the tongue it pushes up the sand, and falls back to a residue between waves --
     * so the foam comes in and drains with the water instead of marking where the water was.
     */
    float band = 1.0 - smoothstep(0.0, WATER_SWASH_SHOAL, Shoal);
    // Kept apart from the crest foam above: the two ride different things, so they are broken
    // up in different places below and only the results are combined.
    float shoreFoam = band * WATER_SHORE_FOAM * mix(WATER_SHORE_FOAM_REST, 1.0, Surf);

    /*
     * Breaking crests, on both wave models -- the surf zone between the swash and open water.
     *
     * Selected from the depth limit rather than from the fold, which is why it is the one
     * whitecap source the Gerstner path has: its steepness is clamped so its horizontal map
     * can never compress, and until now the surf zone of a Gerstner sea was bare between the
     * swash and open water. The selection itself is done where the depth is (see
     * OceanSurface.breaking); what arrives here is already a fraction.
     *
     * Deliberately NOT gated by `band`: the swash is the last few metres and this is
     * everything seaward of it standing in too little water, which is what a surf zone is.
     */
    shoreFoam = max(shoreFoam, WATER_SHORE_FOAM * Breaking);

    /*
     * Break the coverage up, AFTER the physical selection has chosen where foam is -- and
     * over BOTH bands, which is the correction 11.44 owed this.
     *
     * It sat inside the spectral branch and so reached crest foam only. The shore band came
     * out as a flat wash of grey, and once 11.44 widened the surf zone from 0.38 m to tens
     * of metres that wash became most of the middle distance: a painted band across the
     * frame, which is precisely the look the whole Jacobian selection exists to avoid. The
     * band is a smooth function of depth and has no structure of its own, so if it does not
     * borrow this one it has none.
     *
     * Value noise evaluated rather than sampled: it costs no sampler in a program that has
     * none left, and a hash costs less than the fetch would anyway. Two scales drifting at
     * different rates, so the texture does not read as a stationary pattern the waves slide
     * under.
     *
     * It may only take coverage AWAY. Foam the noise could ADD would be whitewater where
     * nothing folded and no bed shoaled.
     */
    // Cycles per metre over a position in world units, so the frequency divides. Shared by
    // both breakups below, which differ in where they are SAMPLED, not in scale.
    float noiseA = WATER_FOAM_NOISE_A_PER_M / waterUnitsPerMetre;
    if (foam > 0.0) {
        float breakup = waterFoamPattern(WorldPos.xz * noiseA + time * 0.03);
        // The bar is read from the foam BEFORE it is eroded, so thinning raises it and the
        // pattern comes apart; what survives keeps the strength it had.
        float bar = WATER_FOAM_ERODE_HI - WATER_FOAM_ERODE_SPAN * foam;
        foam *= smoothstep(0.0, WATER_FOAM_ERODE_EDGE, breakup - bar);
    }
    if (shoreFoam > 0.0) {
        /*
         * THE SHORE'S FOAM RIDES THE SHEET, which is the whole of this.
         *
         * Whitewater at a beach sits ON the water, and the water there is a sheet running up
         * the face and draining back. Sampled at the world position it is instead a fixed set
         * of shapes that the moving band reveals and hides -- a stencil, and it reads as one:
         * static blobs winking in and out while the tide moves over them.
         *
         * So the lookup is offset by how far the tongue has run, along the bed's own downhill.
         * A DISPLACEMENT, not a change of coordinates: two earlier attempts tried to give the
         * pattern the shore's own frame and both failed on the geometry rather than the idea.
         * An alongshore arc length has a CUT where the tracer's chain closes, and interpolating
         * across it swept the whole coast inside one grid cell, printing a band of hairlines.
         * Rotating world position into the shore's basis is worse: on a round island the
         * position vector is radial and its tangential component is identically zero, so the
         * pattern collapses to a function of radius and the sea fills with bullseyes.
         *
         * A displacement has neither failure. It is world position plus a smooth offset, so it
         * cannot collapse and it has nothing to be discontinuous across.
         */
        vec2 q = WorldPos.xz - ShoreDir * SwashRun;
        float breakup = waterFoamPattern(q * noiseA + time * 0.03);
        float bar = WATER_FOAM_ERODE_HI - WATER_FOAM_ERODE_SPAN * shoreFoam;
        shoreFoam *= smoothstep(0.0, WATER_FOAM_ERODE_EDGE, breakup - bar);
    }
    foam = max(foam, shoreFoam);

    /*
     * The removed slope, as a lobe width (spec 11.42).
     *
     * A Beckmann/GGX lobe of width alpha carries a mean square slope of alpha squared, so
     * the slope the mesh gave up converts to a width by a square root and composes with the
     * authored one by adding VARIANCES -- which is what makes this a handover: the surface
     * is as rough as the waves it stopped resolving, no more and no less.
     *
     * alpha is the squared perceptual roughness this tree shades with, so the trip out is a
     * second square root. With nothing filtered the expression collapses to exactly
     * waterRoughness, which is what keeps the near field the water the scene authored.
     *
     * This replaced a lerp toward a 0.115 literal, inherited from the reference study. That
     * constant was a look control standing where a property of the sea belongs: it made the
     * horizon the same roughness for a millpond and a gale, and at this spectrum's measured
     * slope variance it was low by a factor of three.
     */
    float alphaAuthored = waterRoughness * waterRoughness;
    float alpha = sqrt(alphaAuthored * alphaAuthored + max(removedMss, 0.0));
    float roughness = clamp(sqrt(alpha), waterRoughness, 1.0);

    vec3 V = normalize(-ViewPos);
    // The interface is shaded in view space, so the normal has to arrive there
    // too -- the surface normal is authored in world space by ocean.glsl.
    vec3 Nv = normalize(mat3(view) * N);
    /*
     * Is the body of water on the far side of this interface from the eye.
     *
     * ONE answer, read by the normal flip here and by the optical path below, because
     * they are the same question asked twice. They used to disagree in scope -- the flip
     * per PIXEL, the path per FRAME from cameraSubmerged alone -- so a crest closing over
     * a camera still above the still level had its normal flipped toward the eye and was
     * then charged its path against the depth buffer BEHIND the surface, which is air
     * there.
     *
     * cameraSubmerged stays per frame deliberately: it is compared against the STILL
     * level, so a camera at the waterline does not switch models several times a second
     * as crests pass and reset every temporal history with them (water.c). The facing
     * test is what carries the per-pixel half, and folding the two here is what lets the
     * wavy boundary the eye actually sees decide instead of the flat one.
     *
     * Off the GEOMETRIC facing and the height, not off the shading normal (spec 11.44).
     * You can only see the underside of the sea from above the still level if that piece of
     * water is over your head, so a fragment below the eye is not being seen from below
     * however its normal happens to point. Taken off facing alone, every distant cell that
     * merely wound backwards switched to the submerged model and lost its sun glitter --
     * a dark patch riding a crest, invisible with the glitter off because then it matched
     * its neighbours, which is why it read as the glitter breaking.
     */
    bool seenFromBelow = cameraSubmerged == 1 || (!gl_FrontFacing && WorldPos.y > waterCamPos.y);
    // Face the eye. Under the surface the normal points into the body, and left alone the
    // Fresnel and the refract() below sit on the wrong side of the interface -- the latter
    // returning a direction that sends the refraction sample to an arbitrary texel.
    if (seenFromBelow)
        Nv = -Nv;
    /*
     * ...and then bend the SHADING normal back into the hemisphere the eye can see.
     *
     * It points away from the eye in plenty of places the GEOMETRY does not: it is
     * interpolated across a quad and then tilted by the short band's slope, and at grazing
     * incidence on a rough sea that tips it past the horizon on thin streaks along every
     * crest. Measured, not feared -- they cover a good fraction of the mid field.
     *
     * Neither obvious repair is right, and each was worn for a while. FLIPPING it points it
     * AT the eye, which is the low-Fresnel case, so the body shows through as a dark teal
     * patch. CLAMPING N.V to zero leaves it exactly edge-on, where Fresnel saturates and the
     * same fragments read as a white mirror instead. Same pixels, opposite colours, one
     * cause.
     *
     * Bending keeps the tangential direction the normal already had, is continuous across
     * the threshold instead of a branch between two shadings, and does nothing at all to a
     * surface already facing the eye.
     */
    float ndv = dot(Nv, V);
    if (ndv < WATER_MIN_NDV)
        Nv = normalize(Nv + V * (WATER_MIN_NDV - ndv));
    float NdotV = clamp(dot(Nv, V), 0.0, 1.0);

    // Optical path through the body: the view-Z gap between this surface and
    // whatever the depth buffer holds behind it, stretched from planar depth onto
    // the actual view ray. Without the stretch a grazing sight line would be
    // charged the vertical column and read too shallow exactly where water is
    // deepest-looking.
    float surfaceDist = max(-ViewPos.z, 1e-4);
    // Derives only from a uniform, so it is one value for the whole draw.
    float minExtinction = max(min(min(waterAbsorption.r, waterAbsorption.g), waterAbsorption.b),
                              WATER_MIN_EXTINCTION);
    float maxPath = max(WATER_MAX_PATH, WATER_MAX_OPTICAL_DEPTH / minExtinction);
    float path = maxPath;
    // How much of this pixel still has water in it. 1 everywhere but the shoreline.
    float coverage = 1.0;
    if (seenFromBelow) {
        // From below, the body is between the EYE and the surface rather than
        // beyond it, so the optical path is the sight line itself. The depth buffer
        // behind the surface describes air and has nothing to say about it.
        path = length(ViewPos);
    } else if (sceneDepthAvailable == 1 && texture(sceneDepthTex, uv).r < WATER_DEPTH_EMPTY) {
        /*
         * There is geometry behind this fragment, so the column can be measured.
         *
         * The guard is load-bearing and its absence was a visible defect (spec 11.35). Where
         * nothing was drawn the depth buffer holds its cleared value, which reads back as the
         * FAR PLANE -- and the surface itself now runs to the horizon, far past any far plane.
         * So `bedDist - surfaceDist` came out hugely negative out there, the shoreline test
         * read it as "the bed has come up through the surface", and the discard removed every
         * fragment past the last geometry in the scene. It printed as a band of empty sky
         * between the sea and the horizon, whose lower edge sat exactly at the outermost mesh:
         * a water bug that looked like a sky bug.
         *
         * No bed behind means OPEN WATER, which is the `path = maxPath` and
         * `coverage = 1` this branch is skipped in favour of.
         */
        float bedNdc = texture(sceneDepthTex, uv).r * 2.0 - 1.0;
        float bedDist = -viewZFromNdcZ(bedNdc);
        float rayScale = length(ViewPos) / surfaceDist;
        // Signed, and kept signed until coverage has been taken from it: clamping
        // to zero first flattens the derivative on the dry side of the edge, which
        // halves the width the transition is measured over.
        float signedPath = (bedDist - surfaceDist) * rayScale;
        // Shoreline. Where the column has closed the surface has come up through
        // the bed, and what is left is a sliver almost coplanar with it: depth
        // rounding decides which wins per pixel and it reads as a dotted edge
        // rather than as a thin film. So the sliver goes -- but the boundary it
        // leaves is a hard step across one pixel, and an edge inside the opaque
        // lane gets no smoothing from anything downstream.
        //
        // Derivative coverage over that step: the distance to the threshold
        // divided by how fast the column changes per pixel is the threshold's
        // offset MEASURED IN PIXELS, so a half-covered pixel reads 0.5 whatever
        // the scene scale or the camera angle. Alpha-to-coverage then spends it as
        // samples. Only pixels the edge actually crosses come out fractional; a
        // silhouette of submerged geometry that happens to sit within one pixel
        // width of the surface is the one place this reads low where the water is
        // whole, and it is a pixel wide, at a shoreline, where the water is
        // already going.
        float edge = signedPath - WATER_MIN_PATH;
        coverage = clamp(edge / max(fwidth(edge), 1e-5) + 0.5, 0.0, 1.0);
        path = max(signedPath, 0.0);
        // One rule and one discard. A single-sample target has nothing to dither into, so
        // there the fraction rounds to the cutoff -- keeping a fractional fragment would
        // write the sliver at full strength. fwidth above still sits in uniform control
        // flow, before any discard.
        if (alphaToCoverage == 0)
            coverage = step(0.0, edge);
        if (coverage < WATER_MIN_COVERAGE)
            discard;
    }
    path = min(path, maxPath);

    // Transmitted share: the scene behind, bent along the refracted ray and then
    // absorbed over the full path. The bend itself is capped at WATER_MAX_BEND,
    // where the absorption is not.
    vec3 refrDir = refract(-V, Nv, 1.0 / waterIor);
    vec3 bed;
    if (sceneColorAvailable == 1) {
        vec3 exitView = ViewPos + refrDir * min(path, WATER_MAX_BEND_M * waterUnitsPerMetre);
        vec4 refrClip = projection * vec4(exitView, 1.0);
        vec2 refrUV = clamp(refrClip.xy / refrClip.w * 0.5 + 0.5, vec2(0.001), vec2(0.999));
        // Validity: the offset is a screen-space approximation of a world-space
        // bend, so it can walk onto something that is NOT under the water --
        // most cheaply the sky above the far shore, which arrives as bright
        // mottled patches wherever a wave happens to aim the ray there. If the
        // sample it landed on sits in FRONT of this surface, it was never behind
        // the water and the unbent sample is the honest answer.
        //
        // FADED rather than switched, which is the correction spec 11.44 owed this. It used
        // to snap the UV back the instant the probe came forward of the surface -- a binary
        // decision on a continuous quantity, so wherever the test flipped it drew a step one
        // pixel wide, and on a flat sea the locus of "the probe is in front" is a contour of
        // constant distance, which projects to a straight horizontal LINE across the frame.
        // Rare while the bend was capped at one world unit; unmissable once 11.44 made that
        // cap a metre in a world of 22 units, so the ray reached far enough to trip it over
        // most of the middle distance.
        if (sceneDepthAvailable == 1) {
            float probeNdc = texture(sceneDepthTex, refrUV).r * 2.0 - 1.0;
            float behind = -viewZFromNdcZ(probeNdc) - surfaceDist;
            refrUV = mix(uv, refrUV,
                         smoothstep(0.0, WATER_BEND_FADE_M * waterUnitsPerMetre, behind));
        }
        bed = textureLod(sceneColorTex, refrUV, roughness * WATER_TRANSMISSION_MAX_LOD).rgb;

        // Caustics, on whatever the surface is refracting -- the bed, a rock, a
        // hull. Walk back along the refracted sun ray to where it crossed the
        // surface, and read the compression of the cascades THERE: a converging
        // patch of surface is a lens, and its focus is what brightens the floor.
        //
        // FFT only, and that is principled rather than a gap: caustics come from
        // compression, and the Gerstner path's steepness is clamped precisely so
        // its mapping cannot compress. The same reasoning gates its foam.
        if (waveModel == 1 && sunAvailable == 1 && causticsEnabled == 1) {
            vec2 crossing = WorldPos.xz - sunDir.xz / max(sunDir.y, 0.12) * path * 0.18;
            vec2 uvMed = oceanCascadeUv(crossing, 1);
            vec2 uvShort = oceanCascadeUv(crossing, 2);
            // LOD 0: `crossing` walks with the sun ray and the path length, so its screen
            // derivative describes neither the surface nor a footprint, and the medium band
            // IS mipped. Caustics are a near-field effect anyway -- WATER_CAUSTIC_DEEP_OFF
            // closes them well before a cell covers a period.
            float mj = oceanBandJacobian(textureLod(cascade1_0, uvMed, 0.0),
                                         textureLod(cascade1_1, uvMed, 0.0),
                                         cascadeChoppiness[1]);
            float sj = oceanBandJacobian(textureLod(cascade2_0, uvShort, 0.0),
                                         textureLod(cascade2_1, uvShort, 0.0),
                                         cascadeChoppiness[2]);
            float focus = max(0.0, 1.0 - mj) * 0.48 + max(0.0, 1.0 - sj) * 0.52;
            float window = smoothstep(0.0, WATER_CAUSTIC_SHALLOW_M * waterUnitsPerMetre, path) *
                           (1.0 - smoothstep(WATER_CAUSTIC_DEEP_ON_M * waterUnitsPerMetre,
                                             WATER_CAUSTIC_DEEP_OFF_M * waterUnitsPerMetre, path));
            float focused =
                pow(smoothstep(WATER_CAUSTIC_ON, WATER_CAUSTIC_FULL, focus), 2.0) * window;
            // A caustic is focused SUNLIGHT, so a deck over the crossing point dims it
            // (spec 11.41). Read at the crossing rather than at the fragment for the same
            // reason the Jacobian is: this is a property of the ray, not of the pixel.
            //
            // One of the two places cloud shadow enters this shader; the other is the sun
            // lobe below, which spec 11.42 gave it. The reflection is still not a third:
            // it is the split-sum environment lookup, which carries the deck through the
            // sky bake already.
            bed *= 1.0 + focused * WATER_CAUSTIC_GAIN *
                             cloudSunAt(vec3(crossing.x, WorldPos.y, crossing.y));
        }
    } else {
        bed = vec3(0.0);
    }
    vec3 T = exp(-waterAbsorption * path);
    vec3 body = bed * T + waterScatter * preExposure * (1.0 - T);

    // Reflected share: the split-sum environment lobe, the same lookup every
    // other material makes. F0 0.020 falls out of IOR 1.333 rather than being
    // authored.
    float f0s = (waterIor - 1.0) / (waterIor + 1.0);
    vec3 F0 = vec3(f0s * f0s);
    vec3 F = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec2 ab = texture(brdfLUT, vec2(NdotV, roughness)).rg;
    vec3 specWeight = F * ab.x + ab.y;
    vec3 reflected = vec3(0.0);
    if (iblEnabled > 0) {
        vec3 R = reflect(-V, Nv);
        vec3 Rw = normalize(mat3(transpose(view)) * R);
        reflected = textureLod(prefilteredMap, Rw, roughness * maxReflectionLOD).rgb *
                    iblIntensity * preExposure;
    }

    // Energy split: what the interface reflects it does not transmit.
    vec3 color = body * (1.0 - specWeight) + reflected * specWeight;

    /*
     * The sun itself (spec 11.42).
     *
     * The environment lobe above cannot supply this. sky_env_frag bakes the sky WITHOUT the
     * disc -- at that face size a 0.53 degree sun is a couple of texels and aliases the
     * prefilter -- so under the procedural sky the reflection carries no sun at all, and
     * the sea had no specular response to its own key light. An HDR environment does carry
     * one, but blurred to whatever mip the roughness picks.
     *
     * Suppressed from below: a submerged eye sees the sun through total internal reflection
     * and refraction that this lobe does not model, and the honest answer there is nothing
     * rather than a highlight in the wrong place.
     */
    // What survives the caster and the deck. The slot gates both: a sun with no cascade is
    // one nothing can name, and cloudSunForSlot returns 1 for every other light. Hoisted
    // out of the lobe below because the foam is lit by the same sun and owes the same debt
    // to the same shadow -- whitewater standing bright inside a cloud's dapple is the
    // giveaway that it was never lit at all.
    float sunVis = 1.0;
    if (sunShadowSlot >= 0) {
        sunVis = (1.0 - csmOutermostOcclusion(WorldPos, sunShadowSlot)) *
                 cloudSunForSlot(WorldPos, sunShadowSlot);
    }

    if (glitterEnabled == 1 && sunAvailable == 1 && !seenFromBelow) {
        vec3 Lv = normalize(mat3(view) * sunDir);
        vec3 windV = normalize(mat3(view) * vec3(waterWindDir.x, 0.0, waterWindDir.y));
        float glitter = waterSunGlitter(Nv, V, Lv, windV, removedMss);
        // Dimensionless ceiling on the BRDF, the reasoning pbr_frag states for BRDF_MAX:
        // it bounds the lobe rather than the product, so it is independent of both the
        // sun's magnitude and the exposure. A near-mirror sea puts an unbounded spike here
        // otherwise, and one pixel of it survives the tonemap as a hole.
        glitter = min(glitter, WATER_GLITTER_MAX);
        // Fresnel at the FACET rather than at the surface normal, which is what makes the
        // glitter brighten toward grazing along with the rest of the interface.
        vec3 Hv = normalize(V + Lv);
        vec3 Fs = fresnelSchlick(max(dot(V, Hv), 0.0), F0);
        color += Fs * glitter * sunVis * sunRadiance * preExposure;
    }

    // Foam sits ON the interface, so it replaces both halves rather than being
    // added to them: whitewater is opaque, and adding it would let the body colour
    // show through a surface that is full of air.
    if (foam > 0.0) {
        /*
         * Foam is LIT, like everything else in this frame.
         *
         * WATER_FOAM_COLOR is an ALBEDO. Scaling it by the exposure alone treated it as a
         * radiance, so whitewater came out the same grey whatever the sun was doing -- it
         * saw neither the sun, the sky, the cascade nor the cloud deck, and on a sea lit by
         * a sun 0.8 degrees above the horizon it sat at a fixed 0.75 over near-black water.
         * Every other term here scales a radiance it sampled.
         *
         * Lambertian, because that is what a layer of entrained air bubbles is. The sky half
         * is the prefiltered environment at its blurriest mip rather than an irradiance
         * cube: this program declares 16 of 16 samplers so there is no room for one, and the
         * top mip is already a hemispherical average -- which is the same quantity, up to
         * the pi that the sun half carries explicitly.
         */
        /*
         * BUBBLES (spec 11.44). Foam is a crust of entrained air, not a flat coat of paint,
         * and shading it off the water's own smooth normal is what made it read as one.
         *
         * The relief is value noise sampled three times -- centre and two neighbours -- so
         * its gradient is a difference rather than a second field, which is the same reason
         * ocean.glsl derives a normal from the displacement it drew. Evaluated, not sampled:
         * this program has no sampler left, and a bubble map would need one.
         *
         * Two things come out of the one field. The gradient tilts the normal, so bubbles
         * catch the sun and the sky at their own angles; the value itself mottles the albedo,
         * because a foam crust is thicker in some places than others and thin foam lets the
         * water under it through.
         */
        vec2 bubbleP = WorldPos.xz * (WATER_FOAM_BUBBLE_PER_M / waterUnitsPerMetre) +
                       time * WATER_FOAM_BUBBLE_DRIFT;
        float b0 = waterValueNoise(bubbleP);
        float bx = waterValueNoise(bubbleP + vec2(WATER_FOAM_BUBBLE_STEP, 0.0));
        float bz = waterValueNoise(bubbleP + vec2(0.0, WATER_FOAM_BUBBLE_STEP));
        vec3 nWorld = normalize(mat3(transpose(view)) * Nv);
        // Tilted in WORLD xz, which is the plane the noise lives in. Weighted by how much
        // foam is here: a wisp does not deserve the same relief as a bank of it.
        vec3 bubbleN = normalize(nWorld + vec3(-(bx - b0), 0.0, -(bz - b0)) *
                                              WATER_FOAM_BUBBLE_RELIEF * foam);
        vec3 ambient = iblEnabled > 0 ? textureLod(prefilteredMap, bubbleN, maxReflectionLOD).rgb *
                                            iblIntensity
                                      : vec3(1.0);
        /*
         * WRAPPED, not Lambertian. Foam is a dense froth of air in water, and light entering
         * it scatters many times before leaving -- so it is lit well past the terminator and
         * from directions a flat diffuse surface gets nothing from. That is why surf GLOWS at
         * sunset while everything around it goes dark.
         *
         * Straight N.L gets that badly wrong exactly there: the surface normal is near
         * vertical, so at a sun 0.8 degrees up it is 0.014 and correctly-lit foam is black.
         * The frame then shows the sea meeting the sand at a bare line, which is what it did.
         *
         * The (N.L + w)/(1 + w) form is the cheap standard for it, and the normalisation is
         * what keeps it from being a brightness cheat: it cannot exceed 1, so foam facing the
         * sun is no brighter than before -- only foam facing away stops being nothing.
         */
        float wrapped = max(dot(bubbleN, sunDir) + WATER_FOAM_WRAP, 0.0) /
                        (1.0 + WATER_FOAM_WRAP);
        vec3 direct = sunAvailable == 1
                          ? sunRadiance * wrapped * sunVis * WATER_INV_PI
                          : vec3(0.0);
        // Thickness: never to zero, or the crust reads as holes rather than as texture.
        float thick = mix(WATER_FOAM_BUBBLE_MIN, 1.0, b0);
        vec3 lit = WATER_FOAM_COLOR * thick * (ambient + direct) * preExposure;
        color = mix(color, lit, clamp(foam, 0.0, 1.0) * WATER_FOAM_MAX);
    }

    // Alpha is the shoreline coverage, in BOTH writes below, because which output
    // the driver derives alpha-to-coverage from is not something a shader gets to
    // choose. These two are the only outputs whose alpha water does not otherwise
    // need: FragColor's is inert in the opaque lane, where nothing blends, and the
    // normals' is a marker read for its SIGN. The three above them each carry a
    // value a consumer reads -- aux roughness, albedo metalness, SSS profile -- so
    // coverage cannot be put there to be safe.
    //
    // Which means this rests on the driver NOT reading the highest-numbered alpha
    // it can find, and that is measured rather than assumed: albedo binds under
    // --ssgi with a 0 in its alpha, above both of these, and the shoreline still
    // moves by the same 4.4k px it moves without it. If the rule were "highest
    // alpha-bearing output", zero coverage from there would have erased the
    // surface entirely.
    FragColor = vec4(min(color, vec3(WS_SCENE_MAX)), coverage);
    // Non-negative alpha, so NOT the catcher's negative reflective marker: SSR
    // only shades the catcher, and giving water the marker would put it in a
    // channel whose magnitude is already the catcher's edge falloff -- the two
    // are not separable there (see the spec's phase 1b note).
    NormalOut = vec4(Nv, coverage);
    // Linear view-Z in .z is what makes the atmosphere composite fog this
    // surface at the water's own depth rather than the bed's.
    VelocityOut = packVelocityAux(ViewPos.z, roughness);
    // Alpha is METALNESS here, not coverage. Water is a dielectric, and 1.0 would
    // tell SSGI to give it no indirect diffuse (kD = 1 - alpha) and tell the
    // spec-occ composite to hand nearly all its energy to the specular answer.
    AlbedoOut = vec4(waterScatter, 0.0);
    DiffuseOut = vec4(0.0);
    // No ambient specular routed out for occlusion: water's reflection stays in
    // FragColor above, so there is nothing here for the spec-occ composite to
    // scale and nothing double-counted by leaving it at zero.
    SpecOut = vec4(0.0);
}
