#include "spectrum.h"

#include <math.h>

// The second radiation constant hc/k in m K, and the first for radiance 2hc^2 in W m^2 / sr
// (CODATA 2018).
#define PLANCK_C1L 1.191042972e-16
#define PLANCK_C2  1.438776877e-2

// Lumens per watt at 555 nm: what turns the observer's Y into luminance.
#define SPECTRUM_KM 683.0f

// The range and step the blackbody is integrated over. The fit is zero to the precision that
// matters outside it, and 1 nm is finer than any feature in the matching functions.
#define SPECTRUM_NM_LO   360.0f
#define SPECTRUM_NM_HI   830.0f
#define SPECTRUM_NM_STEP 1.0f

static float _cie_lobe(float x, float mu, float s_lo, float s_hi) {
    const float t = (x - mu) / (x < mu ? s_lo : s_hi);
    return expf(-0.5f * t * t);
}

void spectrum_cie_xyz(float nm, vec3 out) {
    out[0] = 1.056f * _cie_lobe(nm, 599.8f, 37.9f, 31.0f) +
             0.362f * _cie_lobe(nm, 442.0f, 16.0f, 26.7f) -
             0.065f * _cie_lobe(nm, 501.1f, 20.4f, 26.2f);
    out[1] =
        0.821f * _cie_lobe(nm, 568.8f, 46.9f, 40.5f) + 0.286f * _cie_lobe(nm, 530.9f, 16.3f, 31.1f);
    out[2] =
        1.217f * _cie_lobe(nm, 437.0f, 11.8f, 36.0f) + 0.681f * _cie_lobe(nm, 459.0f, 26.0f, 13.8f);
}

void spectrum_xyz_to_rec709(const vec3 xyz, vec3 out) {
    const float x = xyz[0], y = xyz[1], z = xyz[2];
    out[0] = 3.2406f * x - 1.5372f * y - 0.4986f * z;
    out[1] = -0.9689f * x + 1.8758f * y + 0.0415f * z;
    out[2] = 0.0557f * x - 0.2040f * y + 1.0570f * z;
}

double spectrum_planck(double nm, double kelvin) {
    if (!(kelvin > 0.0) || !(nm > 0.0))
        return 0.0;
    const double m = nm * 1e-9;
    const double e = PLANCK_C2 / (m * kelvin);
    // Past this the exponential is so large the radiance is zero to every digit kept.
    if (e > 700.0)
        return 0.0;
    // Per metre of wavelength, then per nanometre.
    return PLANCK_C1L / (m * m * m * m * m * expm1(e)) * 1e-9;
}

void spectrum_blackbody_xyz(float kelvin, vec3 out) {
    double sum[3] = {0.0, 0.0, 0.0};
    for (float nm = SPECTRUM_NM_LO; nm <= SPECTRUM_NM_HI; nm += SPECTRUM_NM_STEP) {
        const double radiance = spectrum_planck(nm, kelvin) * SPECTRUM_NM_STEP;
        vec3 cmf = {0.0f, 0.0f, 0.0f};
        spectrum_cie_xyz(nm, cmf);
        for (int c = 0; c < 3; c++)
            sum[c] += radiance * cmf[c];
    }
    for (int c = 0; c < 3; c++)
        out[c] = (float)(sum[c] * SPECTRUM_KM);
}

void spectrum_blackbody_rec709(float kelvin, vec3 out) {
    vec3 xyz = {0.0f, 0.0f, 0.0f};
    spectrum_blackbody_xyz(kelvin, xyz);
    spectrum_xyz_to_rec709(xyz, out);
    for (int c = 0; c < 3; c++)
        out[c] = fmaxf(out[c], 0.0f);
}
