// The water surface, evaluated in ONE place.
//
// Both the position the vertex stage rasterizes and the normal the fragment
// stage shades with come from here, and that is the point: a normal derived from
// a different height than the raster used prints as shading that does not match
// the silhouette, and a shadow pass evaluating its own copy casts a shadow from a
// surface that is not where the surface is. wind.glsl makes the same argument for
// the same reason.
//
// Derivatives are ANALYTIC, never finite differences. The models this seam is
// built for both hand them over for free: a Gerstner term differentiates in
// closed form, and an FFT cascade transforms its derivative fields alongside its
// height in the same pass. A finite difference would need a second full
// evaluation per axis and would still be wrong at the crest, which is the one
// place the normal matters most.

uniform float waterLevel;
uniform vec2 waterWindDir;     // unit; the longest wave's travel direction
uniform float waterAmplitude;  // half crest-to-trough of the longest wave
uniform float waterWavelength; // longest wave, world units
uniform float waterSteepness;  // 0 = round sine crests, 1 = the sharpest legal
uniform float waterSpread;     // per-octave direction fan, radians

// Four octaves. Each is a quarter the length and a bit under half the height of
// the one before, which is the classic wave-train spacing: enough separation that
// the octaves do not beat against each other into a visible repeat.
const int OCEAN_WAVES = 4;
const float OCEAN_LENGTH_FALLOFF = 0.42;
const float OCEAN_AMPLITUDE_FALLOFF = 0.44;
const float OCEAN_GRAVITY = 9.81;

// Spectral cascades (waveModel 1). Sampled in the VERTEX stage as well as the
// fragment stage, which GL 3.3 allows and which is the whole point -- the long and
// medium bands displace real geometry.
uniform int waveModel; // 0 = Gerstner octaves, 1 = spectral cascades
uniform sampler2D cascade0_0;
uniform sampler2D cascade0_1;
uniform sampler2D cascade1_0;
uniform sampler2D cascade1_1;
uniform sampler2D cascade2_0;
uniform sampler2D cascade2_1;
uniform float cascadeLength[3];
uniform float cascadeChoppiness[3];
// Each band's mean square slope, measured off the spectrum it was seeded from (spec
// 11.42). What the filtering below removes is a FRACTION of these, and a fraction has to
// be a fraction of something -- without them the far field could only be lerped toward a
// literal, which is a look constant standing in for a property of the sea.
uniform float cascadeSlopeVar[3];
// Last frame's target 0 for the two cascades that displace, and whether they hold a
// frame yet. 0 on the first two frames, and then the previous position falls back to
// the current one -- which reports camera motion only, the behaviour the whole
// spectral path had before these existed.
uniform sampler2D cascadePrev0;
uniform sampler2D cascadePrev1;
uniform int prevAvailable;

// The baked bed, for shoaling: height in R, its world-space gradient in G,B. Absent
// (bedAvailable 0) is the normal case: the per-fragment water column comes from the
// resolved scene depth instead, which is exact and works against arbitrary geometry.
// This answers the one question screen depth cannot -- a vertex needs its own depth to
// know how far to move, and sampling screen depth would need the position that depth
// is meant to produce.
uniform sampler2D bedTex;
uniform int bedAvailable;
uniform float waterExtent;

// The eye, in world space. Two things need it and both are about magnitude rather than
// about the camera: it is the projector's origin, and it is the origin every cascade lookup
// is taken relative to so the argument stays small.
uniform vec3 waterCamPos;

/*
 * How many world units make a metre, so that a constant which is a PHYSICAL LENGTH can be
 * written as one (spec 11.44).
 *
 * The water carries a dozen such numbers -- the depth a wave shoals over, the distance the
 * short band fades across, the refraction bend's ceiling, the caustic depth window -- and
 * every one of them was a bare world-unit literal, correct only where a unit happened to be
 * a metre. apps/tree runs at 22 units per metre, so its waves shoaled over 12 CENTIMETRES of
 * depth and its surf zone was 0.38 m wide, which is the hard line at the shore rather than a
 * beach.
 *
 * Derived from Sky.world_units_per_km, which is the engine's existing authority on this and
 * already drives the atmosphere. One number, one meaning: an ocean with a private copy would
 * be free to disagree with the sky about how big the world is, which is the whole defect.
 */
uniform float waterUnitsPerMetre;

/*
 * PROJECTED GRID placement (spec 11.35).
 *
 * The lattice is a fixed grid in NDC rather than in the world: each vertex is a ray
 * through its own screen position, and where that ray meets the still-water plane is the
 * point the surface gets evaluated at. So sample density is uniform in PIXELS and reach is
 * whatever the frustum sees -- the two properties a camera-centred mesh has to trade
 * against each other, because for it they are both the same number.
 *
 * The ray is built from the FORWARD projection, not an uploaded inverse: invFocal undoes
 * the two focal scales and mat3(transpose(view)) is the view rotation's inverse while the
 * view is orthonormal, which is the idiom the rest of this tree reconstructs rays with.
 */

// The lattice's horizon row is placed exactly ON the horizon, where the ray is parallel to
// the plane and there is no intersection at all -- so "infinity" has to be a distance.
// Large enough that the row lands within a fraction of a pixel of the true vanishing line:
// the miss angle is atan(eyeHeight / this), 1e-4 rad at a 100-unit eye height.
const float OCEAN_FAR_DIST = 1.0e6;

// Rays are cast slightly outside the frustum. The horizontal half of the displacement
// moves a vertex ACROSS the screen, so an edge vertex can be pulled inward and leave a
// wedge of background along the frame border. Not applied to the horizon edge, which is
// not a screen edge -- there is no water beyond it to be pulled in from.
const float OCEAN_OVERSCAN = 1.12;

// A camera exactly in the plane projects the whole plane onto the horizon line, so every
// ray meets it at distance zero and the lattice collapses to a single point. Exactly right
// for a FLAT plane and useless for a displaced one, so the eye is held a hair off the
// surface rather than in it.
const float OCEAN_MIN_EYE_HEIGHT = 0.05;

vec2 oceanProjectedPosition(vec2 lattice, mat4 view, mat4 projection) {
    vec2 invFocal = 1.0 / vec2(projection[0][0], projection[1][1]);
    mat3 viewToWorld = mat3(transpose(view));

    float ndcX = lattice.x * 2.0 * OCEAN_OVERSCAN;
    bool above = waterCamPos.y >= waterLevel;

    /*
     * Which NDC row this column's horizon sits on.
     *
     * A ray's world-Y slope is ndcX*invFocal.x*a + ndcY*invFocal.y*b - c over the world-Y
     * components of the three view basis vectors; setting that to zero and solving for
     * ndcY gives the row running parallel to the plane. Per COLUMN, because a rolled
     * camera's horizon is not a horizontal line on screen.
     *
     * Solving for it is what makes the lattice worth its vertices. Rows past the horizon
     * see no water at all, so without this the usable fraction of the lattice is whatever
     * the pitch happens to leave -- and half is a common answer.
     */
    float a = viewToWorld[0].y;
    float b = viewToWorld[1].y;
    float c = viewToWorld[2].y;
    float denom = invFocal.y * b;
    // b is the world-Y of the view's up axis, so it vanishes looking straight down or up,
    // and then no row is parallel to the plane and the whole lattice is usable. Off the
    // top of the screen when the eye is above the surface, off the bottom when below.
    float horizonY = abs(denom) > 1e-6 ? (c - ndcX * invFocal.x * a) / denom
                                       : (above ? 1.0e30 : -1.0e30);

    // Above the surface the water is the region BELOW the horizon row; below it, the
    // region above. Either way the lattice spans exactly that region and nothing else.
    float y0 = above ? -OCEAN_OVERSCAN : max(horizonY, -OCEAN_OVERSCAN);
    float y1 = above ? min(horizonY, OCEAN_OVERSCAN) : OCEAN_OVERSCAN;
    float ndcY = mix(y0, y1, lattice.y + 0.5);

    vec3 rd = normalize(viewToWorld * vec3(vec2(ndcX, ndcY) * invFocal, -1.0));
    float eye = waterCamPos.y - waterLevel;
    eye = above ? max(eye, OCEAN_MIN_EYE_HEIGHT) : min(eye, -OCEAN_MIN_EYE_HEIGHT);
    float t = -eye / rd.y;
    // A ray running away from the plane, or grazing it, runs to the cap instead. Written as
    // a positive range rather than a negation so that a division by zero -- inf, or NaN
    // when the numerator vanishes too -- takes the same branch.
    t = (t > 0.0 && t < OCEAN_FAR_DIST) ? t : OCEAN_FAR_DIST;
    return waterCamPos.xz + rd.xz * t;
}

// Depth over which a wave goes from fully shoaled to fully open-water, IN METRES. Waves
// shorten and steepen as the bed rises under them; below the floor there is not
// enough water column left to carry any displacement at all.
//
// These two decide how wide the surf zone is: its width on the ground is this span divided
// by the bed's slope. Read as world units they were 12 cm of depth in a world at 22 units
// per metre, which is no surf zone at all.
const float OCEAN_SHOAL_MIN_M = 0.14;
const float OCEAN_SHOAL_FULL_M = 2.7;
/*
 * Tallest crest the WAVE FIELD may carry over a given water column, as a fraction of it.
 *
 * Above the breaking criterion (a shoaling wave breaks at a height of about 0.78 of the
 * depth, a crest of 0.39) because this bounds the open-water field as it fades across the
 * shoal window, not the bore that replaces it -- oceanSurf owns the bore and holds it to the
 * criterion itself. What this guarantees is that the field goes to zero where the water
 * does, so a swell cannot climb up through dry sand however energetic it is; the shoal
 * factor says the same thing but saturates at zero across the whole shallow end, where the
 * difference between a centimetre of water and a metre is exactly what a bound needs.
 */
const float OCEAN_BREAK_CREST = 1.15;

/*
 * What the bed under a point says about the water over it. One fetch, read once per vertex
 * and handed to everything that shoals, breaks or runs up, so they cannot disagree about
 * where the floor is.
 */
struct OceanBed {
    /*
     * 1 in open water, falling to 0 as the bed comes up.
     *
     * Multiplies displacement, not height alone: the horizontal term has to shrink with it
     * or the surface slides sideways over a beach it is no longer above. And because it
     * multiplies the displacement, its GRADIENT is a product-rule term in every surface
     * derivative -- the factor alone describes a surface whose normal is the open-water
     * one wherever the bed is steep, which is the surf zone specifically. Zero over a flat
     * bed and zero with no bed at all, so the term costs nothing where it says nothing.
     */
    float shoal;
    vec2 dShoal; // d(shoal)/d(world x, world z)
    /*
     * The raw water COLUMN, world units, which the breaking clamp needs and the smoothstepped
     * factor cannot give back -- it saturates at 0 across the whole shallow end, where the
     * clamp has to keep distinguishing a centimetre of water from a metre. Unbounded above
     * and negative where the bed is out of the water, both of which the consumers want: a
     * column of -2 says the sand here stands two units proud.
     */
    float column;
    vec2 dColumn; // d(column)/d(world x, world z): minus the bed's own gradient
};

OceanBed oceanBed(vec2 p) {
    OceanBed b;
    // No bed is INFINITELY deep, not zero-deep: the clamp must not bite where nothing has
    // been said about the floor.
    if (bedAvailable == 0) {
        b.shoal = 1.0;
        b.dShoal = vec2(0.0);
        b.column = 1.0e6;
        b.dColumn = vec2(0.0);
        return b;
    }
    vec2 uv = p / (waterExtent * 2.0) + 0.5;
    vec3 bed = texture(bedTex, clamp(uv, vec2(0.0), vec2(1.0))).rgb;
    b.column = waterLevel - bed.r;
    b.dColumn = -bed.gb;
    // The window converted into this world's units, so the shoal ramp is the same DEPTH of
    // water whatever a unit happens to be.
    float shoalMin = OCEAN_SHOAL_MIN_M * waterUnitsPerMetre;
    float span = (OCEAN_SHOAL_FULL_M - OCEAN_SHOAL_MIN_M) * waterUnitsPerMetre;
    float u = clamp((b.column - shoalMin) / span, 0.0, 1.0);
    // d/dp smoothstep(u(p)) = 6u(1-u) * du/dp, and du/dp = d(column)/dp / span. Flat
    // outside the window, where smoothstep has clamped and the bed can move without
    // the factor moving.
    float dFactor = 6.0 * u * (1.0 - u) / span;
    b.shoal = u * u * (3.0 - 2.0 * u);
    b.dShoal = dFactor * b.dColumn;
    return b;
}

struct OceanSurface {
    vec3 world;  // displaced world position
    vec3 normal; // unit normal from the analytic derivatives
    float shoal; // 1 = open water, 0 = the bed has reached the surface
    // Horizontal-map Jacobian; below 1 the map is compressing. 1 means "not
    // computed", which is the Gerstner path.
    float jacobian;
    // Mean square slope of the displacing bands that the sample footprint filtered away:
    // 0 where the surface is fully resolved, the band's whole variance where it has
    // collapsed to the still plane. What roughness picks up, so the energy is handed over
    // rather than lost -- see oceanCascadeLod.
    //
    // ABSOLUTE, not a fraction. A fraction cannot be turned into a lobe width without
    // knowing what it is a fraction of, which is why the far field used to lerp toward a
    // constant instead of widening by what it actually lost (spec 11.42).
    float filteredMss;
    // How much bore or swash is at this point right now, 0..1: the surf wave's crest
    // fraction, gated by how far inshore the point is. What the shore foam rides, so the
    // whitewater moves with the wave instead of sitting on a depth contour.
    float surf;
};

/*
 * SURF: the incident wave, refracted, breaking, running up and draining back (spec 11.44).
 *
 * The wave field proper cannot do this. Both models are stationary in the sense that
 * matters at a shore: a Gerstner train travels downwind and a Tessendorf field is periodic
 * over its tile and travels downwind too, and the shoal factor's whole effect is to scale
 * either to nothing at the sand. So the sea arrived at the beach as a still line however big
 * the swell behind it was -- there was no term for the thing a beach IS, which is waves
 * coming in.
 *
 * What comes in is the incident wave AFTER refraction. Entering shallow water it slows as
 * sqrt(g h), so its crests turn parallel to the depth contours whatever direction it left
 * deep water in, and it shortens as it comes; it breaks, and what crosses the surf zone is a
 * BORE whose height the depth sets; and at the shoreline the bore collapses into the SWASH,
 * a thin lens that runs up the beach face and drains back. Two things, then, and they are
 * different shapes:
 *
 *   the bore    amplitude min(0.39 h, Hs/2), the saturated surf-zone crest -- H = 0.78 h --
 *               capped by the sea that feeds it, and gated to nothing across the shoal
 *               window where the wave field itself is whole. Phase omega * (t + tau(h)),
 *               tau = 2 sqrt(h) / (s sqrt(g)), the shallow-water travel time to shore over
 *               a bed of slope s: written as a time-to-shore rather than a wavenumber
 *               integral so the crest at the shore NOW is the crest that was at depth h a
 *               time tau AGO. Crests move toward the sand at the local speed and bunch up as
 *               they come. A bore's waveform is a peaked crest and a long flat trough, so
 *               that is what it is, zero-mean.
 *
 *   the swash   NOT a wave with amplitude R at the shoreline -- that put a wall of water
 *               metres tall over five centimetres of depth, because R is how far the water's
 *               EDGE climbs, and the sheet behind the edge is thin. It is a lens: the edge
 *               sits at height E(t) on the beach face and the surface behind it slopes down
 *               toward the sea nearly parallel to the sand, at a fixed fraction of the sand's
 *               own drop, so it is centimetres thick at the tip and tens of centimetres at
 *               the still line. One phase for the whole lens, the bore's phase at the
 *               shoreline lagged by a third of a period (the bore arrives, THEN the water
 *               climbs). It replaces the field over the last fraction of the run-up in depth,
 *               blended, so the backwash drains a thin sheet and the trough exposes sand a
 *               little below the still line -- about what a real swash does around its
 *               setup level.
 *
 *   how far     R from Stockdon et al. 2006, the empirical run-up on a natural beach:
 *               R2% = 1.1 * (0.35 beta sqrt(H0 L0) + sqrt(H0 L0 (0.563 beta^2 + 0.004)) / 2),
 *               beta the beach slope, H0 the deep-water significant height, L0 the peak's
 *               deep-water wavelength -- so a steep beach throws the water higher than a
 *               flat one from the same sea, which no constant fraction of Hs can say. That
 *               is the 2% exceedance; the sets below carry the mean under it and the biggest
 *               wave of a set up to it.
 *
 *   sets        waves come in groups. The envelope is one slow sinusoid travelling at the
 *               peak's group speed along the wind, so a big set arrives, works along the
 *               shore, and is followed by small ones -- and two stretches of beach are not
 *               in the same phase of the same set.
 *
 *   obliquity   a small along-wind term in the phase, so the crest is not exactly a depth
 *               contour: on a round island the contours are circles, and a wave that
 *               arrived everywhere at once would make the whole shoreline breathe. Held at
 *               a fixed angle rather than taken from Snell's law, whose along-shore phase
 *               would need an arc-length along a shoreline this shader has no line for.
 *
 * ON DRY SAND TOO. The column is negative there, the shoal factor holds the field to
 * nothing and the bore has no depth to stand in, which is right; the lens is added after
 * both and is not scaled by the shoal factor, so the swash surface rises above the sand and
 * the tongue climbs the beach, then falls below it and drains. Where it is under the sand
 * the depth test hides it, and where it is above, the shoreline coverage draws its edge --
 * so the run-up costs the shoreline machinery nothing new. What bounds it is R, which is a
 * physical height, not a clamp against the sand.
 *
 * THE BEACH SLOPE IS ONE NUMBER, not the bed's gradient under each vertex. The travel time
 * goes as one over it and the run-up nearly linearly in it, and a beach's local slope
 * varies along the shore by more than the beach slope itself wherever the shoreline
 * wanders -- so reading it per vertex re-timed the wave by the shore's own wobble and
 * printed every crest as a comb of humps at the wobble's period. The incident wave was
 * timed by the beach as a whole; it arrives at the bumps, it is not re-planned by them. The
 * C side measures the mean foreshore slope when it bakes the bed.
 *
 * DERIVATIVES ANALYTIC, like everything else in this file, with one named exception: the
 * bore amplitude's sea cap is treated as constant under d/dp where it binds, which is a
 * flat top and costs nothing there.
 */
// Significant wave height of the sea arriving at the shore, METRES, and its peak angular
// frequency, rad/s. Both from the seeded spectrum on the spectral path and from the
// authored train on the Gerstner one; 0 height is no surf.
uniform float waterSurfHeight;
uniform float waterSurfOmega;
// The beach face's mean slope, rise per run. See the header; 0 with no shore.
uniform float waterBeachSlope;

// A saturated bore's crest as a fraction of the depth it runs over: H = 0.78 h.
const float OCEAN_BORE_CREST = 0.39;
// The lens's thickness behind its edge, as a fraction of the sand's own drop from the edge:
// the tongue at the still line is this fraction of the run-up deep.
const float OCEAN_LENS_RATIO = 0.15;
// How far seaward of the still line the lens still owns the surface, as a fraction of R in
// depth. Small: the swash is the last of the run-up, and the bore owns the rest.
const float OCEAN_SWASH_REACH = 0.3;
// The tongue climbs a third of a period after the bore reaches the shoreline.
const float OCEAN_SWASH_LAG = 2.1;
// The SETUP: the fraction of the run-up that is a standing rise of the mean water level at
// the shore, about which the swash oscillates. Stockdon's R2% is setup plus swash, and the
// setup term of it (0.35 beta sqrt(H0 L0)) is about this fraction of the whole on a beach
// like this one. Oscillating the tongue about the STILL level instead retreated its tip
// 0.6 R below still water at every trough -- metres offshore on a shallow slope -- and
// drained the whole swash zone to a film with a step of water at its seaward edge.
const float OCEAN_SWASH_SETUP = 0.4;
// The bore stops shortening below this depth, in metres: the shallow-water wavenumber goes
// as 1/sqrt(h) and would be infinite at the waterline. The lens takes its phase from here.
const float OCEAN_SURF_MIN_DEPTH_M = 0.05;
// Below this beach slope the travel-time form has nothing to stand on; it only bounds the
// divide, for a bed with no shore in it.
const float OCEAN_SURF_MIN_SLOPE = 0.01;
// Waves per set, and how much the sets modulate the run-up: the biggest wave of a set is
// 1 + this times the mean and the smallest 1 - this.
const float OCEAN_SURF_GROUP_WAVES = 6.5;
const float OCEAN_SURF_GROUP_MOD = 0.4;
// Sine of the angle the arriving crest makes with the shore. See the header.
const float OCEAN_SURF_OBLIQUE = 0.3;
/*
 * THE INCIDENT WAVE IS SEVERAL TRAINS, not one, and this is what stops the swash reading
 * as a moving line however well its waveform is shaped.
 *
 * A sea arriving at a beach is a BAND of frequencies. One train is one sinusoid in space
 * and time, and a single sinusoid IS a straight front travelling along the shore -- there
 * is nothing for it to interfere with, so every point does the same thing a fixed phase
 * apart. Three trains at INCOMMENSURATE periods, each refracted to its own residual angle,
 * superpose into a front that is high where they agree and barely arrives where they
 * oppose, and because the ratios are irrational the pattern never repeats.
 *
 * The ratios are close together because refraction has already sorted the band -- what
 * reaches the swash is the peak and its neighbours, not the whole spectrum. Weights sum to
 * 1, so the combined crest is still 1 where they align and the amplitudes above stay the
 * amplitudes they say they are.
 *
 * Shallow water is NON-DISPERSIVE, which is what makes this nearly free: every frequency
 * travels at sqrt(gh), so all three share one travel time to shore and the expensive part
 * -- the bed fetch and the sqrt -- is computed once.
 */
const float OCEAN_TRAIN_FREQ[3] = float[3](1.0, 0.79, 1.27);
const float OCEAN_TRAIN_WEIGHT[3] = float[3](0.48, 0.30, 0.22);
// Residual angle off the wind, radians, after refraction has turned each train shoreward.
const float OCEAN_TRAIN_ANGLE[3] = float[3](0.0, 0.38, -0.26);
// The bore waveform is ((1 + cos psi) / 2)^2, whose mean over a period is 3/8; this takes
// it back to zero mean and a unit crest. (The skew below moves the mean by under 2%, which
// is not worth a second constant.)
const float OCEAN_BORE_MEAN = 0.375;
// How far the crest leans forward: psi = phi + skew cos(phi). A bore is a FRONT, not a
// peak -- steep face, long back -- and a symmetric crest read as a comb of cusps marching
// in. The face is the LOW-phase side, since at a fixed point phase increases with time and
// the front arrives first; this form is steep there and slow behind. (phi - skew sin(phi),
// the obvious guess, is odd about the crest and only broadens it.) 0 is symmetric; 1 is
// the sawtooth limit where the face goes vertical.
const float OCEAN_BORE_SKEW = 0.6;
// The swash leans harder than the bore that made it: uprush takes about a third of the
// cycle and the backwash the other two thirds, where a bore is a travelling front whose
// face is steep but whose back is not that long.
const float OCEAN_SWASH_SKEW = 0.85;
/*
 * SWASH CAPTURE, and this is the nonlinear one -- the reason real run-ups are so unequal.
 *
 * A bore arriving while the previous backwash is still draining runs into it: the two
 * collide, the incoming one is slowed and absorbed, and it stops well short. One arriving
 * just after the beach has drained meets nothing and runs much further. So the run-up
 * depends on the wave BEFORE it, which is what stops a swash zone looking metronomic.
 *
 * Approximated by scaling the run-up down by how big the previous cycle was -- a big one
 * leaves more water on the face to climb through. That needs no stored state, because the
 * previous cycle is this expression evaluated one primary period ago, and with
 * incommensurate trains that is a genuinely different number every wave.
 */
const float OCEAN_SWASH_CAPTURE = 0.45;
/*
 * BEACH CUSPS, from a subharmonic edge wave (Guza & Inman 1975).
 *
 * A shore traps waves that run ALONG it, and the one a normally-incident sea excites most
 * strongly is the subharmonic: half the incident frequency, standing, so alternate waves
 * run high where the one before ran short. That is the mechanism behind the scalloped
 * shoreline every real beach has, and it is what breaks the STRAIGHTNESS of the front
 * where the trains above break its uniformity.
 *
 * Its spacing is not a look constant: edge waves have their own dispersion relation,
 * omega^2 = g k sin(beta) for the lowest mode, so at half the incident frequency the cusp
 * spacing is 8 pi g sin(beta) / omega^2 -- 29 m for a 6.7 s sea on a 1:10 face, which is
 * where real cusps on such a beach are. Bigger swell gives wider cusps, as observed.
 */
const float OCEAN_CUSP_AMP = 0.35;

struct OceanSurf {
    float height; // the surface, world units above the still level, field included
    vec2 dHeight; // d(height)/d(world x, world z)
    float crest;  // 0..1, how much bore or tongue is here -- what the foam reads
};

// The bore waveform and its derivative in phase, leaning forward by `skew`.
vec2 oceanBoreWave(float phase, float skew) {
    float psi = phase + skew * cos(phase);
    float dPsi = 1.0 - skew * sin(phase);
    float half1 = 0.5 + 0.5 * cos(psi);
    float w = (half1 * half1 - OCEAN_BORE_MEAN) / (1.0 - OCEAN_BORE_MEAN);
    // d/dpsi of half1^2 is -half1 sin(psi), then the chain through psi.
    float dw = -half1 * sin(psi) * dPsi / (1.0 - OCEAN_BORE_MEAN);
    return vec2(w, dw);
}

/*
 * The incident wave at a point: OCEAN_TRAIN_FREQ superposed, as (value, d/dx, d/dz).
 *
 * `tau` is the shared travel time to shore and `dTau` its gradient -- shared because
 * shallow water is non-dispersive. `lag` shifts the whole set (the swash climbs after the
 * bore lands) and `skew` chooses how far the crests lean.
 */
vec3 oceanSurfTrains(vec2 p, float t, float tau, vec2 dTau, float lag, float skew) {
    float sum = 0.0;
    vec2 dSum = vec2(0.0);
    for (int i = 0; i < 3; i++) {
        float om = waterSurfOmega * OCEAN_TRAIN_FREQ[i];
        float a = OCEAN_TRAIN_ANGLE[i];
        float ca = cos(a), sa = sin(a);
        vec2 dir = vec2(waterWindDir.x * ca - waterWindDir.y * sa,
                        waterWindDir.x * sa + waterWindDir.y * ca);
        // Each train's own deep-water wavenumber, so the three differ in the DIRECTION and
        // the RATE their phase runs along the shore -- which is what makes the sum
        // interfere in space as well as in time.
        vec2 kv = dir * (om * om / (OCEAN_GRAVITY * waterUnitsPerMetre) * OCEAN_SURF_OBLIQUE);
        vec2 w = oceanBoreWave(om * (t + tau) - dot(kv, p) - lag, skew);
        sum += OCEAN_TRAIN_WEIGHT[i] * w.x;
        dSum += OCEAN_TRAIN_WEIGHT[i] * w.y * (om * dTau - kv);
    }
    return vec3(sum, dSum);
}

/*
 * The surface at p with the surf on it. fieldY / dFieldY are the shoaled wave field's
 * height above the still level and its gradient; what comes back is the total, so the
 * caller replaces rather than adds.
 */
OceanSurf oceanSurf(vec2 p, OceanBed bed, float t, float fieldY, vec2 dFieldY) {
    OceanSurf s;
    s.height = fieldY;
    s.dHeight = dFieldY;
    s.crest = 0.0;
    float gate = 1.0 - bed.shoal;
    // Gated on the bed like the crest limit is: with no bed there is no shore for anything to
    // come in to, and the sentinel column would otherwise be run through a sqrt. And nothing
    // to do at all outside the shoal window, where the field is whole.
    if (bedAvailable == 0 || waterSurfHeight <= 0.0 || gate <= 0.0)
        return s;
    vec2 dGate = -bed.dShoal;

    float upm = waterUnitsPerMetre;
    float g = OCEAN_GRAVITY;
    float omega = waterSurfOmega;

    // Time to shore, from the column in metres. Floored, so in the last centimetres it is a
    // constant -- which is also the lens's phase.
    float hm = bed.column / upm;
    float hs = max(hm, OCEAN_SURF_MIN_DEPTH_M);
    float slope = max(waterBeachSlope, OCEAN_SURF_MIN_SLOPE);
    float invSlopeG = 1.0 / (slope * sqrt(g));
    float tau = 2.0 * sqrt(hs) * invSlopeG;
    // d(tau)/dp = (1/(s sqrt g)) * hs^-1/2 * d(hm)/dp, and zero where the floor holds.
    vec2 dTau = hm > OCEAN_SURF_MIN_DEPTH_M
                    ? invSlopeG * inversesqrt(hs) * (bed.dColumn / upm)
                    : vec2(0.0);
    float tauShore = 2.0 * sqrt(OCEAN_SURF_MIN_DEPTH_M) * invSlopeG;

    // Sets: an envelope moving downwind at the group speed. omega_g = omega / waves per
    // set, and k_g = omega_g / c_g with c_g = g / (2 omega).
    float omegaG = omega / OCEAN_SURF_GROUP_WAVES;
    float kG = omegaG * 2.0 * omega / g / upm;
    float groupPhase = omegaG * t - kG * dot(waterWindDir, p);
    float env = 1.0 + OCEAN_SURF_GROUP_MOD * sin(groupPhase);
    vec2 dEnv = -OCEAN_SURF_GROUP_MOD * cos(groupPhase) * kG * waterWindDir;

    // The bore, world units. Depth-limited, sea-capped, gated offshore.
    float depthCrest = OCEAN_BORE_CREST * max(bed.column, 0.0);
    float seaCrest = 0.5 * waterSurfHeight * upm;
    float boreAmp = min(depthCrest, seaCrest);
    vec2 dBoreAmp = (bed.column > 0.0 && depthCrest < seaCrest) ? OCEAN_BORE_CREST * bed.dColumn
                                                                : vec2(0.0);
    vec3 bw = oceanSurfTrains(p, t, tau, dTau, 0.0, OCEAN_BORE_SKEW);
    float bore = boreAmp * env * bw.x * gate;
    vec2 dBore = dBoreAmp * env * bw.x * gate + boreAmp * dEnv * bw.x * gate +
                 boreAmp * env * bw.yz * gate + boreAmp * env * bw.x * dGate;

    // The run-up: Stockdon's 2% exceedance, scaled so the biggest wave of a set reaches it.
    float l0 = 6.28318530718 * g / (omega * omega);
    float hl = waterSurfHeight * l0;
    float r2 = 1.1 * (0.35 * slope * sqrt(hl) + 0.5 * sqrt(hl * (0.563 * slope * slope + 0.004)));
    float runup = r2 / (1.0 + OCEAN_SURF_GROUP_MOD) * upm;

    /*
     * How far THIS wave gets, which is not how far the average one does. Two factors, and
     * between them they are what stops the swash reading as one line moving in and out.
     *
     * CAPTURE: scaled down by how big the cycle before it was, since that water is still
     * draining down the face. The previous cycle is this same expression one primary period
     * ago -- no stored state -- and with incommensurate trains it is a different number
     * every wave.
     *
     * CUSPS: the trapped subharmonic, standing along the shore. Alongshore is taken
     * perpendicular to the wind, which is the same straight-shore approximation the
     * obliquity above already makes.
     */
    float period = 6.28318530718 / omega;
    vec3 sw = oceanSurfTrains(p, t, tauShore, vec2(0.0), OCEAN_SWASH_LAG, OCEAN_SWASH_SKEW);
    vec3 swPrev =
        oceanSurfTrains(p, t - period, tauShore, vec2(0.0), OCEAN_SWASH_LAG, OCEAN_SWASH_SKEW);
    float prev = clamp(swPrev.x, 0.0, 1.0);
    float capture = 1.0 - OCEAN_SWASH_CAPTURE * prev;
    vec2 dCapture = (swPrev.x > 0.0 && swPrev.x < 1.0) ? -OCEAN_SWASH_CAPTURE * swPrev.yz
                                                       : vec2(0.0);

    float omegaE = 0.5 * omega;
    // The lowest edge-wave mode: omega^2 = g k sin(beta), with the beach slope standing in
    // for its sine, which they agree on to a per cent at any slope a beach has.
    float kE = omegaE * omegaE / (g * slope * upm);
    vec2 alongDir = vec2(-waterWindDir.y, waterWindDir.x);
    float along = dot(p, alongDir);
    float cuspTime = OCEAN_CUSP_AMP * cos(omegaE * t);
    float cusp = 1.0 + cuspTime * cos(kE * along);
    vec2 dCusp = -cuspTime * kE * sin(kE * along) * alongDir;

    // The lens: its edge on the beach face, about the setup and up to the run-up, and the
    // sheet behind it.
    float climb = capture * cusp;
    vec2 dReachF = dCapture * cusp + capture * dCusp;
    float swash = OCEAN_SWASH_SETUP + (1.0 - OCEAN_SWASH_SETUP) * sw.x;
    vec2 dSwash = (1.0 - OCEAN_SWASH_SETUP) * sw.yz;
    float edge = runup * climb * env * swash;
    vec2 dEdge = runup * (dReachF * env * swash + climb * dEnv * swash + climb * env * dSwash);
    float lens = OCEAN_LENS_RATIO * edge - (1.0 - OCEAN_LENS_RATIO) * bed.column;
    vec2 dLens = OCEAN_LENS_RATIO * dEdge - (1.0 - OCEAN_LENS_RATIO) * bed.dColumn;

    // Where the lens owns the surface: everywhere on the sand, fading out over the first
    // fraction of R in depth. A smoothstep in depth, differentiated through the column.
    float reach = max(OCEAN_SWASH_REACH * runup, 0.02 * upm);
    float u = clamp(bed.column / reach, 0.0, 1.0);
    float w = 1.0 - u * u * (3.0 - 2.0 * u);
    vec2 dW = (bed.column > 0.0 && bed.column < reach) ? -6.0 * u * (1.0 - u) / reach * bed.dColumn
                                                       : vec2(0.0);

    float a = fieldY + bore;
    vec2 da = dFieldY + dBore;
    s.height = mix(a, lens, w);
    s.dHeight = da + (dLens - da) * w + (lens - a) * dW;
    // The foam's cue: the bore's crest where the bore owns the surface, the tongue's where
    // the lens does.
    s.crest = clamp(mix(bw.x * gate, swash, w), 0.0, 1.0);
    return s;
}

/*
 * THE FAR FIELD IS A FILTERING PROBLEM (spec 11.35).
 *
 * A projected grid's far cells cover kilometres, so a sample there is one arbitrary phase
 * of a wave the cell cannot resolve -- which shimmers rather than describes anything. Both
 * wave models therefore take the world footprint of the cell being evaluated and drop what
 * sits under it: the spectral path by mip level, the Gerstner path by octave. Both report
 * how much they dropped, and water_frag turns that into roughness.
 *
 * The filtered surface converges to its own MEAN, and for a wave field the mean is the
 * still plane -- so the far field flattens by construction instead of by a special case.
 */

// The cascades' resolution, so a world footprint converts to a mip level without a second
// constant that could drift from the C side: a band's texel is cascadeLength/cascadeRes and
// its top level is log2(cascadeRes), where the field is a single texel and therefore its
// mean.
uniform float cascadeRes;

// Cells per wavelength at which a Gerstner octave is fully gone and fully kept. Two is the
// Nyquist bound; leaving at five rather than at three is what stops an octave popping in
// and out as the camera moves, and linear reconstruction between samples needs the margin
// anyway.
const float OCEAN_NYQUIST_OFF = 2.0;
const float OCEAN_NYQUIST_ON = 5.0;

float oceanCascadeTopLod() {
    return log2(max(cascadeRes, 2.0));
}

// Mip level for one band at a given world footprint. Clamped at 0 because a cell finer
// than a texel gains nothing from a sharper level that does not exist.
/*
 * The variance a band gives up when the slope reaching the normal is scaled by `kept`.
 *
 * `kept` scales the SLOPE, so the surviving variance is kept^2 * V and what has to be
 * handed to roughness is the rest of it. NOT (1 - kept)^2 * V, which is the variance of
 * the difference field -- a quantity that would only be the answer if the kept and removed
 * halves were independent fields, where they are one field scaled. That form under-delivers
 * everywhere between the endpoints and is wrong by 3x at kept = 0.5, which is the middle of
 * the band the handover exists for.
 *
 * One function because the two wave models and the short band all need it and all three
 * wrote it differently.
 */
float oceanRemovedMss(float kept, float variance) {
    return max(0.0, 1.0 - kept * kept) * variance;
}

float oceanCascadeLod(float footprint, int band) {
    float texel = cascadeLength[band] / max(cascadeRes, 1.0);
    return clamp(log2(max(footprint / max(texel, 1e-6), 1.0)), 0.0, oceanCascadeTopLod());
}

/*
 * Spectral surface: the long and medium bands displace the mesh, the short band
 * is left for the fragment stage's slope distribution.
 *
 * Every derivative comes out of the same transform as the height -- field0.a is a
 * cross derivative, field1.rg the two slopes, field1.ba the two horizontal
 * derivatives -- so the normal below is analytic and costs nothing extra. That
 * packing is the reason to spend two RGBA targets per cascade.
 */
// A linear random sea is vertically symmetric, and a real one is not: crests are
// sharper than troughs are deep. These are the low-order bound-harmonic correction
// for that, kept small deliberately -- pushed harder the surface folds, which is what
// the choppy-wave literature bounds. The subtracted constants are each band's mean
// square, so the correction reshapes the surface without raising its mean level.
const float OCEAN_BOUND_LONG = 0.14;
const float OCEAN_BOUND_MED = 0.32;
const float OCEAN_BOUND_LONG_VAR = 0.080;
const float OCEAN_BOUND_MED_VAR = 0.030;

/*
 * One band's tiling lookup. Written out eleven times before this existed, twice per band
 * for the two targets of a single sample point.
 *
 * Sampled relative to a camera origin SNAPPED to this band's own period (spec 11.35). fract
 * is periodic with exactly that period, so subtracting an integer multiple of it cannot
 * change the answer -- what it changes is the MAGNITUDE of the argument, and therefore how
 * many mantissa bits are left for the fraction the lookup needs. Without it, p/240 at a
 * quarter-million units is ~1300, which has spent eleven bits on an integer part that fract
 * is about to discard. The snap is exact arithmetic: floor of a quotient times the divisor,
 * then a subtraction of two nearby values, which fp32 does without error.
 *
 * WHAT THIS DOES NOT FIX, measured rather than assumed. The division was only one of the
 * terms, and the small one: `p` ITSELF is quantised, because it is built as
 * waterCamPos.xz + rd.xz*t and the camera term dominates -- 0.03 units at a quarter-million.
 * Removing the division's share moved a translation-invariance comparison by 3%. Fixing the
 * rest means carrying the lattice position camera-RELATIVE, so the error tracks distance from
 * the eye rather than from the world origin, and that is an engine-wide change rather than
 * water's: absolute fp32 world coordinates are what every mesh in such a world is built from.
 * Nothing in this tree is further than about a thousand units out, where all of this is far
 * below a last bit.
 */
vec2 oceanCascadeUv(vec2 p, int band) {
    float period = cascadeLength[band];
    vec2 origin = floor(waterCamPos.xz / period) * period;
    return fract((p - origin) / period + 0.5);
}

/*
 * One band's horizontal-map Jacobian, from its packed derivatives. Below 1 the map is
 * compressing, which is what selects whitecaps and caustic focus.
 *
 * NO shoal term, deliberately: this is asked about the bands that shade the interface and
 * never displace the mesh, so there is no shoal factor scaling them and the two
 * off-diagonals really are equal. That is why this is a square where oceanAssemble's
 * determinant is not -- the distinction is the shoal gradient, and it belongs here in
 * writing because three hand-written copies of this shape invited "unifying" them the
 * wrong way.
 */
float oceanBandJacobian(vec4 field0, vec4 field1, float choppiness) {
    float c = field0.a * choppiness;
    vec2 d = field1.ba * choppiness;
    return (1.0 + d.x) * (1.0 + d.y) - c * c;
}

/*
 * The UNSHOALED displacement of the two bands that reach the mesh: horizontal in .xz,
 * height in .y, crest term included.
 *
 * The one place the height model is written. The previous frame's position comes from
 * retained copies of exactly these two samples and has to be the same arithmetic, and the
 * derivative rows below multiply the same value -- so a second copy is a second place for
 * the bound-harmonic term to drift, which is the failure this file exists to prevent.
 */
vec3 oceanSpectralDisplacement(vec4 long0, vec4 med0) {
    vec2 h = long0.rg * cascadeChoppiness[0] + med0.rg * cascadeChoppiness[1];
    float y = long0.b + med0.b +
              OCEAN_BOUND_LONG * (long0.b * long0.b - OCEAN_BOUND_LONG_VAR) +
              OCEAN_BOUND_MED * (med0.b * med0.b - OCEAN_BOUND_MED_VAR);
    /*
     * METRES to world units, here and nowhere else (spec 11.44).
     *
     * The cascades are seeded from a JONSWAP spectrum in real units -- gravity in m/s^2,
     * wind in m/s, fetch in metres -- so every value in them is metres of displacement over
     * metres of ocean. The crest terms above are part of that arithmetic and their variances
     * are in m^2, which is why the conversion lands after them rather than on the samples.
     *
     * Only the displacement converts. The DERIVATIVE rows do not: they are metres of
     * displacement per metre of ocean, which is the same number as world units per world
     * unit, so slope, the Jacobian and the mean square slope are all unitless and stay put.
     */
    return vec3(h.x, y, h.y) * waterUnitsPerMetre;
}

/*
 * Both wave models end here.
 *
 * disp is the unshoaled displacement; dispDx/dispDz are its derivatives with respect to
 * the undisplaced grid coordinates. The shoal factor scales the displacement and its
 * GRADIENT therefore enters as a product-rule term -- over a flat bed sh.yz is zero and
 * these reduce to the plain scaled derivatives.
 *
 * Shared rather than written per model because the two epilogues were the same expression
 * character for character: the identity rows, the product rule, the cross order and the
 * determinant each existed twice, and the Jacobian in particular is easy to "simplify"
 * back into a square, which is only correct while the gradient is zero.
 */
OceanSurface oceanAssemble(vec2 p, vec3 disp, vec3 dispDx, vec3 dispDz, OceanBed bed,
                           float t) {
    float shoal = bed.shoal;
    OceanSurface s;
    /*
     * DEPTH-LIMITED height: a wave cannot be taller than the water it is standing in.
     *
     * The shoal factor alone does not say this. It is a smoothstep that saturates at 0
     * across the whole shallow end, so once 11.44 let the height keep heaving there, a crest
     * metres tall could still be drawn over sand standing well out of the water -- the sea
     * climbing up THROUGH the beach rather than running up it. The factor cannot fix it
     * either: it has already thrown away the difference between a centimetre of water and a
     * metre, which is exactly the difference that decides how big a wave may be.
     *
     * So the clamp reads the raw column, and it is the breaking criterion: a shoaling wave
     * breaks at H of about 0.78 times the depth, which is a crest of 0.39. That is a real
     * limit rather than a fudge -- it is why surf exists, and it goes to zero exactly where
     * the water does, so the run-up stops at the waterline on its own.
     *
     * SATURATED, not clamped. A clamp is C0: its derivative jumps at the point it engages,
     * and it engages per VERTEX, so the jump runs along the lattice and prints as a scalloped
     * band of cells down the shoreline. tanh reaches the same limit smoothly, is the identity
     * for waves well inside it, and hands back its own slope -- so the derivative rows can be
     * scaled by sech^2 and the normal keeps describing the surface that was drawn.
     *
     * Gated on the BED, not on a sentinel depth. Standing in for no-bed with a huge column
     * and letting the arithmetic run put a float32 round-trip -- tanh of about 6e-8, scaled
     * back by 1e6 -- in front of every open-water surface in the tree, for an identity. It
     * moved both water goldens.
     *
     * The ARGUMENT IS CLAMPED, and that is not tidiness. Where the sand stands out of the
     * water the limit is its 1e-4 floor and the wave is units, so the ratio is around 1e5;
     * tanh is exp under the hood and exp of that is inf, and inf/inf is NaN. A NaN vertex
     * drops every triangle it touches, so the shoreline lost whole lattice cells wherever
     * the sea dipped -- which printed as a staircase of screen-aligned steps along the
     * water's edge, worst when the water was out, and was chased through the island mesh,
     * the bed resolution and the lattice before this line was read. tanh(20) is 1 to float
     * precision, so nothing inside the clamp changes.
     */
    if (bedAvailable == 1) {
        float crestLimit = max(OCEAN_BREAK_CREST * max(bed.column, 0.0), 1.0e-4);
        float sat = tanh(clamp(disp.y / crestLimit, -20.0, 20.0));
        // d(tanh)/du. The limit's OWN gradient is dropped here -- it is the same order as
        // the shoal factor's product-rule term below and needs the bed slope, which arrives
        // as d(factor)/dp rather than d(column)/dp. It shows only where the bed is steep AND
        // the limit is biting, which is the last few units of the run-up.
        float dsat = 1.0 - sat * sat;
        disp.y = crestLimit * sat;
        dispDx.y *= dsat;
        dispDz.y *= dsat;
    }
    /*
     * The shoal factor scales the whole displacement, height included. Between 11.44's
     * first cut and this one the height kept a fraction of itself across the shallow end so
     * that the shore would heave at all; that was a stand-in for the surf, and with the surf
     * here it is the wrong thing -- a stationary field's vertical wobble where the surf zone
     * should be shore-normal bores. The field fades across the window and the surf takes
     * over, which is what a surf zone is.
     */
    vec3 scale = vec3(shoal);
    vec3 dScaleDx = vec3(bed.dShoal.x);
    vec3 dScaleDz = vec3(bed.dShoal.y);
    s.world = vec3(p.x, waterLevel, p.y) + disp * scale;
    vec3 dPdx = vec3(1.0, 0.0, 0.0) + dispDx * scale + disp * dScaleDx;
    vec3 dPdz = vec3(0.0, 0.0, 1.0) + dispDz * scale + disp * dScaleDz;
    // The surf takes the shoaled field's height and hands back the surface with the bore
    // and the swash on it: what the shore does with the wave, not part of the wave. See
    // oceanSurf.
    OceanSurf surf = oceanSurf(p, bed, t, s.world.y - waterLevel, vec2(dPdx.y, dPdz.y));
    s.world.y = waterLevel + surf.height;
    dPdx.y = surf.dHeight.x;
    dPdz.y = surf.dHeight.y;
    s.surf = surf.crest;
    // cross(dPdz, dPdx) and not the reverse: on a flat surface that is +Y, and the
    // flipped order would light every wave from underneath.
    s.normal = normalize(cross(dPdz, dPdx));
    // The horizontal map's determinant, from those same rows. The two off-diagonals are
    // NOT equal once the shoal gradient is in, so this cannot be shortened back to a
    // square -- and the difference between them IS the shoaling compression that selects
    // surf-zone foam.
    s.jacobian = dPdx.x * dPdz.z - dPdz.x * dPdx.z;
    s.shoal = shoal;
    // Each model overwrites this with what its own filtering removed.
    s.filteredMss = 0.0;
    return s;
}

/*
 * The spectral surface's POSITION alone, for the previous frame's motion-vector half.
 *
 * Through oceanAssemble with zero derivative rows, not a private `p + d * shoal`: that
 * shortcut was the pre-11.44 arithmetic and stayed behind when the crest limit, the heave
 * split and the surf were added to the current position, so near a shore the velocity
 * described a surface the raster never drew. Sharing the assembly costs a few multiplies of
 * derivative arithmetic per vertex and makes the two positions the same expression by
 * construction, which is this file's whole argument.
 */
vec3 oceanSpectralPosition(vec2 p, vec4 long0, vec4 med0, OceanBed bed, float t) {
    return oceanAssemble(p, oceanSpectralDisplacement(long0, med0), vec3(0.0), vec3(0.0), bed,
                         t)
        .world;
}

/*
 * Spectral surface. No amplitude knob, deliberately: the spectrum is already physical --
 * its height comes out of the wind speed and fetch it was seeded with -- so scaling it
 * would be authoring over a sea state rather than choosing one. waterAmplitude belongs to
 * the Gerstner path, where there is no sea state to ask. A calmer spectral ocean is a
 * lower wind speed, not a smaller number here.
 */
OceanSurface oceanEvaluateSpectral(vec2 p, float t, OceanBed bed, float footprint) {
    // Explicit LOD, never the implicit derivative, and not only because the vertex stage
    // has none: oceanCascadeUv wraps with fract, so a screen-space derivative reads a whole
    // period across the tile seam and blurs a line through every one of them.
    float lodLong = oceanCascadeLod(footprint, 0);
    float lodMed = oceanCascadeLod(footprint, 1);
    vec2 uvLong = oceanCascadeUv(p, 0);
    vec2 uvMed = oceanCascadeUv(p, 1);
    vec4 long0 = textureLod(cascade0_0, uvLong, lodLong);
    vec4 long1 = textureLod(cascade0_1, uvLong, lodLong);
    vec4 med0 = textureLod(cascade1_0, uvMed, lodMed);
    vec4 med1 = textureLod(cascade1_1, uvMed, lodMed);

    float q0 = cascadeChoppiness[0];
    float q1 = cascadeChoppiness[1];

    // Not named `cross`: that is a builtin oceanAssemble calls.
    float crossDeriv = long0.a * q0 + med0.a * q1;
    // The crest-sharpening term changes the height, so its derivative has to be in the
    // slope or the normal is the normal of a DIFFERENT surface than the one rasterized.
    // d/dp of b*(h*h - c) is 2*b*h*dh, per band.
    vec2 slope = long1.rg * (1.0 + 2.0 * OCEAN_BOUND_LONG * long0.b) +
                 med1.rg * (1.0 + 2.0 * OCEAN_BOUND_MED * med0.b);
    vec2 dHoriz = long1.ba * q0 + med1.ba * q1;

    OceanSurface s = oceanAssemble(p, oceanSpectralDisplacement(long0, med0),
                                   vec3(dHoriz.x, slope.x, crossDeriv),
                                   vec3(crossDeriv, slope.y, dHoriz.y), bed, t);
    /*
     * Each band's slope survives in proportion to how much of it the mip still resolves,
     * reaching nothing at the top level. Weighted by each band's OWN variance and summed
     * rather than averaged: the cascades own disjoint wavenumber windows, so what they
     * lose adds, and the two do not in fact carry comparable energy -- the medium band
     * measures slightly more slope than the long one despite its smaller waves, because
     * slope is amplitude times wavenumber.
     *
     * The mip level enters as a KEPT SCALE, `1 - lod/top`, so it goes through the same
     * oceanRemovedMss as the other two paths. That is a heuristic either way -- a mip is
     * a box filter, not a scaling of the field -- but stating it in the same vocabulary is
     * what lets the three consumers be compared at all. They used three different laws.
     */
    float top = oceanCascadeTopLod();
    s.filteredMss = oceanRemovedMss(1.0 - lodLong / top, cascadeSlopeVar[0]) +
                    oceanRemovedMss(1.0 - lodMed / top, cascadeSlopeVar[1]);
    return s;
}

/*
 * Sum of Gerstner waves.
 *
 * A Gerstner wave moves its surface points horizontally as well as vertically,
 * bunching them toward the crest, which is what sharpens a crest and broadens a
 * trough instead of leaving a symmetric sine. The horizontal term is what makes
 * it a Gerstner wave rather than a height field, and it is also what can fold the
 * surface over itself: the mapping stops being injective once Q*A*k exceeds 1.
 *
 * So steepness is NOT passed through to Q. It is divided by the octave's own A*k
 * and by the octave count, so the set sums to the authored value: at or below 1
 * the map stays injective at any amplitude or wavelength. Above 1 it folds, which
 * is why the C side clamps the uniform rather than trusting the caller. Authoring
 * Q directly would instead make "sharp" mean something different at every
 * wavelength.
 *
 * `t` drives the octaves here; the spectral path reads cascades that already hold one
 * instant -- see the previous-position note in water_vert -- and passes it on to the surf,
 * which is closed-form on both models.
 */
OceanSurface oceanEvaluateAt(vec2 p, float t, OceanBed bed, float footprint) {
    if (waveModel == 1)
        return oceanEvaluateSpectral(p, t, bed, footprint);

    // Accumulated UNSHOALED, then scaled once at the end by oceanAssemble. The shoal
    // factor multiplies the whole displacement, so its gradient multiplies the whole
    // displacement too -- which is one line out there and would be six inside the loop.
    vec3 disp = vec3(0.0);
    // Partial derivatives of the displacement with respect to the undisplaced grid
    // coordinates. The flat plane's own identity rows are added at the end, since they
    // are not part of what shoals.
    vec3 dDispDx = vec3(0.0);
    vec3 dDispDz = vec3(0.0);
    // Slope variance the footprint removed, summed over the octaves. This is the Gerstner
    // path's own answer to what mip levels do for the spectral one: an octave shorter than
    // a couple of cells is not detail the mesh can hold, and evaluating its closed form
    // anyway samples one arbitrary phase per cell.
    float removedMss = 0.0;

    float wavelength = waterWavelength;
    // Note what the shoal factor must NOT be folded into: q is amplitude's reciprocal,
    // so scaling amplitude here leaves qa = steepness/(k*N) unchanged and the lateral
    // shuffle would run at full strength over a beach whose vertical motion had
    // already faded to nothing.
    float amplitude = waterAmplitude;
    vec2 base = normalize(waterWindDir + vec2(1e-6, 0.0));

    for (int i = 0; i < OCEAN_WAVES; i++) {
        // Fan the octaves off the wind, alternating sides so the set stays
        // centred on it. A single direction reads as corduroy however many
        // octaves are stacked on it.
        float fan = waterSpread * float(i) * ((i % 2 == 0) ? 1.0 : -1.0);
        vec2 dir = vec2(base.x * cos(fan) - base.y * sin(fan),
                        base.x * sin(fan) + base.y * cos(fan));

        float k = 6.28318530718 / max(wavelength, 0.01);
        // Deep-water dispersion: long waves travel faster, which is what keeps
        // the octaves sliding past each other instead of marching in lockstep.
        float omega = sqrt(OCEAN_GRAVITY * k);
        float phase = k * dot(dir, p) - omega * t;
        float sinp = sin(phase);
        float cosp = cos(phase);

        // Q normalised by this octave's own A*k and by the octave count, so the
        // steepness uniform sums to itself across the set: at or below 1 the map
        // stays injective. See the header note.
        float q = waterSteepness / max(k * amplitude * float(OCEAN_WAVES), 1e-4);
        float qa = q * amplitude;

        // How much of this octave the footprint can carry. Applied to the displacement AND
        // its derivatives, for the reason the shoal factor is: fading the height while
        // leaving the lateral shuffle at full strength describes a surface that slides
        // without rising. amplitude*k is the octave's slope amplitude, so its square is the
        // variance it contributes.
        float keep = smoothstep(OCEAN_NYQUIST_OFF, OCEAN_NYQUIST_ON,
                                wavelength / max(footprint, 1e-4));
        // Mean square of a sine of slope amplitude a*k, and both axes already sum to it
        // because the octave travels in one direction.
        float slope = amplitude * k;
        removedMss += oceanRemovedMss(keep, slope * slope * 0.5);

        disp.x += keep * qa * dir.x * cosp;
        disp.y += keep * amplitude * sinp;
        disp.z += keep * qa * dir.y * cosp;

        float dqa = keep * qa * k * sinp;
        float dah = keep * amplitude * k * cosp;
        dDispDx.x -= dqa * dir.x * dir.x;
        dDispDx.y += dah * dir.x;
        dDispDx.z -= dqa * dir.y * dir.x;
        dDispDz.x -= dqa * dir.x * dir.y;
        dDispDz.y += dah * dir.y;
        dDispDz.z -= dqa * dir.y * dir.y;

        wavelength *= OCEAN_LENGTH_FALLOFF;
        amplitude *= OCEAN_AMPLITUDE_FALLOFF;
    }

    OceanSurface s = oceanAssemble(p, disp, dDispDx, dDispDz, bed, t);
    // What the footprint dropped, as a mean square slope rather than as a fraction. The
    // halving is the mean square of a sine: an octave whose slope amplitude is a*k
    // contributes (a*k)^2 / 2, and the two axes already sum to that because the octave
    // travels in one direction.
    //
    // Accumulated through oceanRemovedMss per octave rather than as a difference of two
    // running totals: `keep` scales the octave's AMPLITUDE, so the survivor is keep^2 of
    // the variance and a plain (energy - kept) books (1 - keep) where (1 - keep^2) is owed.
    s.filteredMss = removedMss;
    // A Gerstner map DOES compress -- that bunching is what sharpens its crests -- but
    // its Jacobian is not reported here, so the determinant oceanAssemble computed is
    // overwritten. 1 means "no selector on this path", which is what the foam and
    // caustics gates read it as; reporting a real one would turn the clamped-steepness
    // argument for why they are FFT-only into a lie.
    s.jacobian = 1.0;
    return s;
}

/*
 * Where this point was one frame ago, which is a motion vector's other half.
 *
 * Gerstner re-evaluates at t - dt and gets it in closed form. The spectral path
 * cannot: its cascades hold ONE instant, the current one, so reading them at t - dt
 * returns the same surface and the waves report no motion of their own. That is what
 * the retained copies answer -- the same position arithmetic against last frame's
 * fields.
 *
 * Only the two displacing bands are retained, which is all this needs: the short band
 * never reaches the mesh, so it has no position to have moved.
 *
 * The FOOTPRINT has to be the caller's own, on both models: a previous position filtered
 * differently from the current one is the velocity of a surface the raster never drew, and
 * near the horizon -- where the level is the top one and the surface is the still plane --
 * the difference would be the whole wave.
 */
vec3 oceanPreviousWorldAt(vec2 p, float tPrev, OceanBed bed, float footprint) {
    if (waveModel == 0)
        return oceanEvaluateAt(p, tPrev, bed, footprint).world;
    float lodLong = oceanCascadeLod(footprint, 0);
    float lodMed = oceanCascadeLod(footprint, 1);
    vec2 uvLong = oceanCascadeUv(p, 0);
    vec2 uvMed = oceanCascadeUv(p, 1);
    // Fetched inside the branches, not before them: reading the current fields and then
    // overwriting from the retained ones spent two texture fetches on every vertex from
    // frame two onward, which is essentially always. A ternary on the samplers themselves
    // is not the alternative -- GLSL 3.30 samplers are opaque and may not appear in an
    // expression -- so this is two branches.
    vec4 long0, med0;
    if (prevAvailable == 1) {
        long0 = textureLod(cascadePrev0, uvLong, lodLong);
        med0 = textureLod(cascadePrev1, uvMed, lodMed);
    } else {
        long0 = textureLod(cascade0_0, uvLong, lodLong);
        med0 = textureLod(cascade1_0, uvMed, lodMed);
    }
    return oceanSpectralPosition(p, long0, med0, bed, tPrev);
}
