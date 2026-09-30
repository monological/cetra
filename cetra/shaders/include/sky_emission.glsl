// What the sky emits for this bake (spec 13.7), as sky_bind_emission uploads it:
// the overcast's share and the radiance scale, already applied. The model lives
// in sky.c and arrives here as numbers, so a clear relative sky multiplies by
// exactly 1 and adds exact zeros, and no shader ever sees either setting.
uniform float skySunScale;   // what arrives of the sun's own radiance, on the sky's scale
uniform float skyDomeZenith; // the overcast dome's zenith radiance
uniform float skyDomeFloor;  // the overcast dome's irradiance over pi
uniform float skyScatterMax; // the ceiling on the sun's scattered radiance

// The overcast dome along a ray at cos(zenith) mu: the CIE standard overcast
// sky, Lz (1 + 2 mu) / 3 -- three times brighter overhead than at the horizon,
// and the same in every azimuth. The horizon, mu = 0, is what a far enough
// surface fades to.
float skyDome(float mu) {
    return skyDomeZenith * (1.0 + 2.0 * max(mu, 0.0)) / 3.0;
}
