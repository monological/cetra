#ifndef _SPECTRUM_H_
#define _SPECTRUM_H_

#include <cglm/cglm.h>

/*
 * Light as a spectrum, taken to the engine's colour (specs 13.4 and 13.14). Two consumers:
 * water's caustics split into bands across the visible range, and fire's blackbody emission.
 *
 * The observer is CIE 1931's 2-degree one by Wyman, Sloan and Shirley's multi-lobe fit
 * ("Simple Analytic Approximations to the CIE XYZ Color Matching Functions", JCGT 2013), and
 * the working colour space is linear Rec.709 with D65 white, the engine's. Pure CPU, no GL.
 */

// The colour matching functions x-bar, y-bar, z-bar at `nm` nanometres.
void spectrum_cie_xyz(float nm, vec3 out);

// CIE XYZ to linear Rec.709, unclamped: a colour outside the gamut comes back with a negative
// channel, which is the caller's to handle.
void spectrum_xyz_to_rec709(const vec3 xyz, vec3 out);

// Planck's law: a blackbody's spectral radiance at `nm` nanometres and `kelvin`, in
// W / (sr m^2 nm).
double spectrum_planck(double nm, double kelvin);

// What a blackbody at `kelvin` looks like, integrated against the observer over 360-830 nm:
// its CIE XYZ with Y in cd/m^2 (683 lm/W), so Y is its luminance.
void spectrum_blackbody_xyz(float kelvin, vec3 out);

// The same as linear Rec.709 in nits per channel, negatives clamped to zero: the colour a
// glowing body of that temperature is drawn with. A blackbody under about 1900 K sits outside
// the gamut on the red side, and the clamp drops only the blue it cannot have.
void spectrum_blackbody_rec709(float kelvin, vec3 out);

#endif // _SPECTRUM_H_
