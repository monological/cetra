// The overcast deck (spec 13.7), as sky_bind_deck uploads it. The model lives
// in sky.c and arrives here as numbers already weighted by the deck, so a clear
// sky multiplies by exactly 1 and adds exact zeros, and no shader ever sees
// the setting itself.
uniform float deckSunScale; // what arrives of the sun's own radiance
uniform float deckZenith;   // the dome's zenith radiance, deck-weighted
uniform float deckFloor;    // the dome's irradiance over pi, deck-weighted

// The dome along a ray at cos(zenith) mu: the CIE standard overcast sky,
// Lz (1 + 2 mu) / 3 -- three times brighter overhead than at the horizon, and
// the same in every azimuth. The horizon, mu = 0, is what a far enough surface
// fades to.
float deckDome(float mu) {
    return deckZenith * (1.0 + 2.0 * max(mu, 0.0)) / 3.0;
}
