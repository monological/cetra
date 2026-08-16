#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stochastic_tex.h"

/*
 * The Gaussian quantile (probit), Acklam's rational approximation.
 *
 * Needed because the transform is "replace each texel by the Gaussian value at the same
 * position in the distribution", and that position comes from the texture's own CDF -- so the
 * map from a CDF value back to a Gaussian is the inverse normal CDF, which has no closed form.
 * Relative error under 1.15e-9, far past what an 8-bit texel can carry.
 */
static double _probit(double p) {
    static const double a[6] = {-3.969683028665376e+01, 2.209460984245205e+02,
                                -2.759285104469687e+02, 1.383577518672690e+02,
                                -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[5] = {-5.447609879822406e+01, 1.615858368580409e+02,
                                -1.556989798598866e+02, 6.680131188771972e+01,
                                -1.328068155288572e+01};
    static const double c[6] = {-7.784894002430293e-03, -3.223964580411365e-01,
                                -2.400758277161838e+00, -2.549732539343734e+00,
                                4.374664141464968e+00,  2.938163982698783e+00};
    static const double d[4] = {7.784695709041462e-03, 3.224671290700398e-01,
                                2.445134137142996e+00, 3.754408661907416e+00};
    const double p_low = 0.02425;

    if (p <= 0.0)
        return -8.0;
    if (p >= 1.0)
        return 8.0;
    if (p < p_low) {
        const double q = sqrt(-2.0 * log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    if (p > 1.0 - p_low) {
        const double q = sqrt(-2.0 * log(1.0 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    const double q = p - 0.5;
    const double r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

void stochastic_gaussianize(unsigned char* rgb, int width, int height, float* inv_lut) {
    if (!inv_lut)
        return;
    // An identity table first, so an early return below still leaves something that maps a
    // value to itself rather than to zero.
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < STOCHASTIC_LUT_SIZE; i++)
            inv_lut[i * 3 + c] =
                ((float)i + 0.5f) / (float)STOCHASTIC_LUT_SIZE;
    }
    if (!rgb || width <= 0 || height <= 0)
        return;

    const int n = width * height;
    for (int c = 0; c < 3; c++) {
        /*
         * A COUNTING sort, not a comparison sort, and it is the natural fit rather than an
         * optimisation: the input is 8-bit, so the histogram IS the sort, and it gives the CDF
         * directly in 256 bins for any texture size.
         */
        int hist[256];
        memset(hist, 0, sizeof(hist));
        for (int i = 0; i < n; i++)
            hist[rgb[(size_t)i * 3 + c]]++;

        // Cumulative counts. cum[v] is how many texels are at or below v.
        int cum[256];
        int running = 0;
        for (int v = 0; v < 256; v++) {
            running += hist[v];
            cum[v] = running;
        }

        /*
         * Each distinct value maps to the MIDPOINT of the CDF step it occupies. Ties have to
         * agree -- every texel of one value must transform to one Gaussian value, or the
         * transform is not a function of the texel and the inverse cannot undo it.
         */
        float gauss_of[256];
        for (int v = 0; v < 256; v++) {
            const int lo = v > 0 ? cum[v - 1] : 0;
            const double p = ((double)lo + (double)cum[v]) * 0.5 / (double)n;
            const double g = _probit(p);
            // Into [0,1] across the stored sigma span, for an 8-bit unsigned texture.
            const double stored = g / (2.0 * (double)STOCHASTIC_SIGMA_SPAN) + 0.5;
            gauss_of[v] = (float)(stored < 0.0 ? 0.0 : (stored > 1.0 ? 1.0 : stored));
        }

        /*
         * The inverse table, indexed by the CDF value the shader recovers from its blended
         * Gaussian. Entry i answers "what original value sits at this point in the
         * distribution", which is a lookup into the same cumulative counts.
         */
        for (int i = 0; i < STOCHASTIC_LUT_SIZE; i++) {
            const double u = ((double)i + 0.5) / (double)STOCHASTIC_LUT_SIZE;
            const int target = (int)(u * (double)n);
            int lo = 0;
            int hi = 255;
            while (lo < hi) {
                const int mid = (lo + hi) / 2;
                if (cum[mid] > target)
                    hi = mid;
                else
                    lo = mid + 1;
            }
            inv_lut[i * 3 + c] = (float)lo / 255.0f;
        }

        // Written back over the source: the shader samples this and never the original, which
        // is exactly why the transform needs no sampler unit of its own.
        for (int i = 0; i < n; i++) {
            const unsigned char v = rgb[(size_t)i * 3 + c];
            rgb[(size_t)i * 3 + c] = (unsigned char)(gauss_of[v] * 255.0f + 0.5f);
        }
    }
}
