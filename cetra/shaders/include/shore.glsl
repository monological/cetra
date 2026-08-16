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

/*
 * The numbers this model stands on, in the one file both languages read.
 *
 * They are not here because shore_runup.c evaluates the same model on the CPU and a mirrored
 * copy of a tuned constant is a silent divergence waiting to happen. The arithmetic below is
 * still duplicated there -- that part cannot be shared and is reviewable by eye; a bare literal
 * is not.
 */
#include "shore_constants.glsl"

const float OCEAN_TRAIN_FREQ[3] =
    float[3](OCEAN_TRAIN_FREQ_0, OCEAN_TRAIN_FREQ_1, OCEAN_TRAIN_FREQ_2);
const float OCEAN_TRAIN_WEIGHT[3] =
    float[3](OCEAN_TRAIN_WEIGHT_0, OCEAN_TRAIN_WEIGHT_1, OCEAN_TRAIN_WEIGHT_2);
const float OCEAN_TRAIN_ANGLE[3] =
    float[3](OCEAN_TRAIN_ANGLE_0, OCEAN_TRAIN_ANGLE_1, OCEAN_TRAIN_ANGLE_2);

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


// The film's tips, where one is running. Included here rather than by each consumer so that
// anything asking this file where the water is gets the same answer -- simulated if there is a
// simulation, closed form if there is not.
#include "shore_film.glsl"

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

/*
 * How much shore a point has: 1 well inside the bed's domain, 0 outside it.
 *
 * SQUARE, because that is the shape the bed texture covers -- ocean.glsl addresses it as
 * p / (waterExtent * 2) + 0.5, a square of half-width waterExtent centred on the origin. A lit
 * surface has no bed to sample and wrote its own test as a CIRCLE of radius waterExtent, so the
 * two disagreed about where the shore was: the square's corners shoaled in the water and were
 * bone dry on the sand, and the outer quarter of the radius faded on one side only. This file
 * exists so the sand and the sea agree by algebra rather than by two calibrations, and the
 * domain is part of that agreement.
 *
 * The fade band keeps the edge off a hard cut, where a step would print the bed's boundary as a
 * line across the ground.
 */
const float SHORE_DOMAIN_FADE = 0.75;

float shoreDomain(vec2 p) {
    vec2 d = abs(p) / max(waterExtent, 1.0e-4);
    return 1.0 - smoothstep(SHORE_DOMAIN_FADE, 1.0, max(d.x, d.y));
}

// Stockdon's 2% exceedance run-up, METRES. Its own function because the ceiling below needs
// the same number and a second copy of an empirical fit is a second thing to get wrong.
float shoreR2() {
    float g = OCEAN_GRAVITY;
    float omega = waterSurfOmega;
    float slope = shoreSlope();
    float l0 = 6.28318530718 * g / (omega * omega);
    float hl = waterSurfHeight * l0;
    return 1.1 * (0.35 * slope * sqrt(hl) + 0.5 * sqrt(hl * (0.563 * slope * slope + 0.004)));
}

/*
 * The highest the swash edge can ever reach, world units above the still level.
 *
 * A uniform-computable scalar, which is what makes the sand affordable: every factor above
 * the run-up is separately bounded -- the cusp by its amplitude, the sets by their modulation,
 * the tongue's own waveform by 1 -- so their product is a constant and anything above it is
 * dry with certainty. A fragment that clears this runs no taps at all.
 */
float shoreEdgeCeiling() {
    // climb <= 1 + OCEAN_CUSP_AMP, env <= 1 + OCEAN_SURF_GROUP_MOD, swash <= 1, and the
    // run-up already divides by the second of those.
    return (1.0 + OCEAN_CUSP_AMP) * shoreR2() * waterUnitsPerMetre;
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
    float runup = shoreR2() / (1.0 + OCEAN_SURF_GROUP_MOD) * upm;

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

/*
 * WHAT THE SWASH LEFT: the beach's memory of the last few waves.
 *
 * Sand that the water reached is darker than sand it did not, and it stays darker for a while
 * after the water has gone -- which is the one thing a painted wet band, fixed at a depth,
 * cannot say. The whole model is available here because the run-up is a closed form: walking
 * BACKWARDS in time is just evaluating it at earlier arguments, so a beach can remember
 * without anything storing a memory.
 *
 * ACCUMULATED, not maximised, and that is what makes the history legible. The lower beach is
 * covered by every wave and saturates dark; the upper beach is reached by one wave in twenty
 * and stays barely damp. Taking a maximum over the same taps gives two levels with a step
 * between them, which is the painted band again with extra steps.
 *
 * Three things come back because they dry at three rates. The FILM is free water still lying
 * on the surface and drains in seconds; WET is water in the pores and leaves over tens of
 * seconds; FOAM is what the tongue's tip deposited and is the only one that is a pattern
 * rather than a depth.
 */
struct ShoreSwash {
    float wet;  // 0..1 pore saturation -- the darkening
    float film; // 0..1 free water on the surface -- the sheen, and it goes first
    float foam; // 0..1 whitewater the tip left behind
};

// SHORE_TAPS and SHORE_TAP_PERIODS come from shore_constants.glsl -- three taps per primary
// period over three periods. Fewer than three per period and a wave can pass a point between
// taps, which reads as the swash skipping; more than three periods back and the taps describe
// water that has already dried by the constants below. Shared with the C side because the
// solver sizes its tip history from exactly this window.
// How sharply the waterline reads, as a fraction of the run-up. The tongue's edge is a real
// edge but a sub-pixel one, and a hard step aliases along the shore.
const float SHORE_COVER_SOFT = 0.06;
// Drying, in SECONDS. The film drains off the surface quickly; the pores hold on. Both are
// eyeballed -- the two-rate SHAPE is what the time-varying-BRDF work establishes, and fitted
// numbers for sand were not to hand.
const float SHORE_DRY_FILM = 1.6;
const float SHORE_DRY_PORE = 24.0;
// Foam's own life on the sand, seconds, and how wide a band around the tip it is laid in as a
// fraction of the run-up. Whitewater is left AT the edge -- that is where the tongue thins to
// nothing and the bubbles strand -- not spread over everything the water touched.
const float SHORE_FOAM_LIFE = 3.5;
const float SHORE_FOAM_BAND = 0.10;
// How far below the still line the sand is simply never dry. A beach has a water table, and
// without this the lower foreshore flickers between waves.
const float SHORE_TABLE_DEPTH = 0.35;

ShoreSwash shoreSwash(vec2 p, float h, float t) {
    ShoreSwash s;
    s.wet = 0.0;
    s.film = 0.0;
    s.foam = 0.0;
    if (waterSurfHeight <= 0.0)
        return s;

    float period = 6.28318530718 / waterSurfOmega;
    float dt = period * SHORE_TAP_PERIODS / float(SHORE_TAPS);
    float soft = max(SHORE_COVER_SOFT * shoreEdgeCeiling(), 1.0e-4);
    float coverNow = 0.0;

    /*
     * The SIMULATED tip where a film is running, and the closed form where none is.
     *
     * The film knows what the formula cannot: that this wave met the last one's backwash and
     * stopped short. Where it exists the sand reads it, so the wet line follows the water the
     * sea is actually drawing rather than a parallel estimate of it. The search is done once
     * here rather than once per tap -- the columns do not move between taps, only the history
     * slot does.
     */
    ShoreFilmSample film = shoreFilmNearest(p);

    for (int k = 0; k < SHORE_TAPS; k++) {
        float age = float(k) * dt;
        float e = film.found ? shoreFilmEdge(film, age) : shoreRunup(p, t - age).edge;
        float cover = 1.0 - smoothstep(-soft, soft, h - e);
        if (k == 0)
            coverNow = cover;
        // Union rather than a sum: two waves reaching the same sand do not wet it twice.
        s.wet = 1.0 - (1.0 - s.wet) * (1.0 - cover * exp(-age / SHORE_DRY_PORE));
        s.film = 1.0 - (1.0 - s.film) * (1.0 - cover * exp(-age / SHORE_DRY_FILM));
        // Deposited where the TIP was, which is a band about the edge rather than everything
        // under it, and then aged.
        float d = (h - e) / max(SHORE_FOAM_BAND * shoreEdgeCeiling(), 1.0e-4);
        s.foam = max(s.foam, exp(-d * d) * exp(-age / SHORE_FOAM_LIFE));
    }

    // Below the water table the sand never dries out.
    float table = 1.0 - smoothstep(-SHORE_TABLE_DEPTH * shoreEdgeCeiling(), 0.0, h);
    s.wet = max(s.wet, table);
    // Swallowed: foam under the water right now belongs to the water, which draws its own.
    // Leaving it here would double it exactly where the two surfaces meet.
    s.foam *= 1.0 - coverNow;
    return s;
}
