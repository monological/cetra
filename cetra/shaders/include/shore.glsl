// The shore's run-up, in the half of the surf model that touches no texture.
//
// Split out of ocean.glsl for one reason: ocean.glsl declares nine samplers, and a
// program that wants only the run-up cannot afford them. pbr_frag has been at its
// sixteen-sampler ceiling since 4.10, so "include the ocean" is not an option there --
// but the swash edge is a closed form of position and time over scalar uniforms alone,
// so it can be had for no sampler at all.
//
// That is what lets the SAND agree with the SEA about where the water is. The waterline
// is exactly h = edge by algebra rather than by two calibrations that drift apart, which
// a beach painted with its own wet band cannot promise.
//
// The bed stays on the ocean side. Anything here that needs to know which way the shore
// lies takes it as a PARAMETER: the water passes the bed's own downhill, and a lit
// surface passes its geometric normal, which on a beach face is the same direction read
// off different data.

uniform float waterLevel;
uniform vec2 waterWindDir; // unit; the longest wave's travel direction
uniform float waterExtent; // the shoaling bed's domain; nothing outside it has a shore

const float OCEAN_GRAVITY = 9.81;

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
 *               that is what it is, zero-mean. It needs the bed, so it stays in ocean.glsl.
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
 *               setup level. Its EDGE is what this file computes, and the edge needs no bed.
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
 * THE BEACH SLOPE IS ONE NUMBER, not the bed's gradient under each vertex. The travel time
 * goes as one over it and the run-up nearly linearly in it, and a beach's local slope
 * varies along the shore by more than the beach slope itself wherever the shoreline
 * wanders -- so reading it per vertex re-timed the wave by the shore's own wobble and
 * printed every crest as a comb of humps at the wobble's period. The incident wave was
 * timed by the beach as a whole; it arrives at the bumps, it is not re-planned by them. The
 * C side measures the mean foreshore slope when it bakes the bed.
 *
 * DERIVATIVES ANALYTIC, like everything else in this seam, with one named exception: the
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

// What the swash tongue is doing at a point, with no bed involved.
struct ShoreRunup {
    float edge;  // the tongue's edge, world units above the still level
    vec2 dEdge;  // d(edge)/d(world x, world z)
    float runup; // R itself, the scale the edge oscillates within
    float swash; // 0..1, the tongue's own waveform -- the foam's cue on the sand
};

// The beach face's slope, floored. Read through here rather than off the uniform so the
// travel time and the run-up cannot end up standing on two different numbers.
float shoreSlope() {
    return max(waterBeachSlope, OCEAN_SURF_MIN_SLOPE);
}

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
 *
 * A pure function of position and time: no bed, no texture, no state. That is what makes
 * it callable from a lit surface, and it is the whole reason this file exists.
 */
ShoreRunup shoreRunup(vec2 p, float t) {
    ShoreRunup r;
    float upm = waterUnitsPerMetre;
    float g = OCEAN_GRAVITY;
    float omega = waterSurfOmega;
    float slope = shoreSlope();
    float invSlopeG = 1.0 / (slope * sqrt(g));
    // The lens takes its phase from the shallowest water the bore reaches, not from the
    // column under the point -- which is what lets the tongue exist on dry sand at all.
    float tauShore = 2.0 * sqrt(OCEAN_SURF_MIN_DEPTH_M) * invSlopeG;

    // Sets: an envelope moving downwind at the group speed. omega_g = omega / waves per
    // set, and k_g = omega_g / c_g with c_g = g / (2 omega).
    float omegaG = omega / OCEAN_SURF_GROUP_WAVES;
    float kG = omegaG * 2.0 * omega / g / upm;
    float groupPhase = omegaG * t - kG * dot(waterWindDir, p);
    float env = 1.0 + OCEAN_SURF_GROUP_MOD * sin(groupPhase);
    vec2 dEnv = -OCEAN_SURF_GROUP_MOD * cos(groupPhase) * kG * waterWindDir;

    // The run-up: Stockdon's 2% exceedance, scaled so the biggest wave of a set reaches it.
    float l0 = 6.28318530718 * g / (omega * omega);
    float hl = waterSurfHeight * l0;
    float r2 = 1.1 * (0.35 * slope * sqrt(hl) + 0.5 * sqrt(hl * (0.563 * slope * slope + 0.004)));
    float runup = r2 / (1.0 + OCEAN_SURF_GROUP_MOD) * upm;

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

    float climb = capture * cusp;
    vec2 dReachF = dCapture * cusp + capture * dCusp;
    r.swash = OCEAN_SWASH_SETUP + (1.0 - OCEAN_SWASH_SETUP) * sw.x;
    vec2 dSwash = (1.0 - OCEAN_SWASH_SETUP) * sw.yz;
    r.runup = runup;
    r.edge = runup * climb * env * r.swash;
    r.dEdge = runup * (dReachF * env * r.swash + climb * dEnv * r.swash + climb * env * dSwash);
    return r;
}
