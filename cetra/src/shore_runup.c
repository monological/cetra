#include <math.h>

#include "shore_runup.h"

#define TWO_PI 6.28318530718f

// Assembled from the shared per-element defines: C and GLSL spell an array initialiser
// differently, so each side builds its own from the same numbers.
static const float OCEAN_TRAIN_FREQ[3] = {OCEAN_TRAIN_FREQ_0, OCEAN_TRAIN_FREQ_1,
                                          OCEAN_TRAIN_FREQ_2};
static const float OCEAN_TRAIN_WEIGHT[3] = {OCEAN_TRAIN_WEIGHT_0, OCEAN_TRAIN_WEIGHT_1,
                                            OCEAN_TRAIN_WEIGHT_2};
static const float OCEAN_TRAIN_ANGLE[3] = {OCEAN_TRAIN_ANGLE_0, OCEAN_TRAIN_ANGLE_1,
                                           OCEAN_TRAIN_ANGLE_2};

float shore_runup_slope(const ShoreRunupParams* p) {
    return p->beach_slope > OCEAN_SURF_MIN_SLOPE ? p->beach_slope : OCEAN_SURF_MIN_SLOPE;
}

static float shore_r2(const ShoreRunupParams* p) {
    const float slope = shore_runup_slope(p);
    const float l0 = TWO_PI * OCEAN_GRAVITY / (p->surf_omega * p->surf_omega);
    const float hl = p->surf_height * l0;
    return 1.1f * (0.35f * slope * sqrtf(hl) +
                   0.5f * sqrtf(hl * (0.563f * slope * slope + 0.004f)));
}

float shore_runup_slot_interval(const ShoreRunupParams* p) {
    // The same expression shoreSwash spaces its taps by, from the same shared constants: one
    // slot per tap means the ring covers exactly the window the taps reach back over.
    const float period = p->surf_omega > 0.0f ? TWO_PI / p->surf_omega : 0.0f;
    const float interval = period * SHORE_TAP_PERIODS / (float)SHORE_TAPS;
    // A floor, so a degenerate sea cannot ask for a slot every frame and collapse the history
    // back to the fifth of a second this exists to fix.
    return interval > 1.0e-3f ? interval : 1.0e-3f;
}

float shore_runup_ceiling(const ShoreRunupParams* p) {
    return (1.0f + OCEAN_CUSP_AMP) * shore_r2(p) * p->units_per_metre;
}

// oceanBoreWave's value half; the chain has no use for its derivative in phase.
static float bore_wave(float phase, float skew) {
    const float psi = phase + skew * cosf(phase);
    const float half1 = 0.5f + 0.5f * cosf(psi);
    return (half1 * half1 - OCEAN_BORE_MEAN) / (1.0f - OCEAN_BORE_MEAN);
}

// oceanSurfTrains's value half.
static float surf_trains(const ShoreRunupParams* p, float x, float z, float t, float tau,
                         float lag, float skew) {
    float sum = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float om = p->surf_omega * OCEAN_TRAIN_FREQ[i];
        const float a = OCEAN_TRAIN_ANGLE[i];
        const float ca = cosf(a), sa = sinf(a);
        const float dx = p->wind_dir[0] * ca - p->wind_dir[1] * sa;
        const float dz = p->wind_dir[0] * sa + p->wind_dir[1] * ca;
        const float k = om * om / (OCEAN_GRAVITY * p->units_per_metre) * OCEAN_SURF_OBLIQUE;
        const float phase = om * (t + tau) - (dx * k * x + dz * k * z) - lag;
        sum += OCEAN_TRAIN_WEIGHT[i] * bore_wave(phase, skew);
    }
    return sum;
}

float shore_runup_edge(const ShoreRunupParams* p, float x, float z, float t) {
    if (p->surf_height <= 0.0f)
        return 0.0f;
    const float upm = p->units_per_metre;
    const float g = OCEAN_GRAVITY;
    const float omega = p->surf_omega;
    const float slope = shore_runup_slope(p);
    const float inv_slope_g = 1.0f / (slope * sqrtf(g));
    const float tau_shore = 2.0f * sqrtf(OCEAN_SURF_MIN_DEPTH_M) * inv_slope_g;

    const float omega_g = omega / OCEAN_SURF_GROUP_WAVES;
    const float k_g = omega_g * 2.0f * omega / g / upm;
    const float group_phase =
        omega_g * t - k_g * (p->wind_dir[0] * x + p->wind_dir[1] * z);
    const float env = 1.0f + OCEAN_SURF_GROUP_MOD * sinf(group_phase);

    const float runup = shore_r2(p) / (1.0f + OCEAN_SURF_GROUP_MOD) * upm;

    const float period = TWO_PI / omega;
    const float sw = surf_trains(p, x, z, t, tau_shore, OCEAN_SWASH_LAG, OCEAN_SWASH_SKEW);
    const float sw_prev =
        surf_trains(p, x, z, t - period, tau_shore, OCEAN_SWASH_LAG, OCEAN_SWASH_SKEW);
    const float prev = sw_prev < 0.0f ? 0.0f : (sw_prev > 1.0f ? 1.0f : sw_prev);
    const float capture = 1.0f - OCEAN_SWASH_CAPTURE * prev;

    const float omega_e = 0.5f * omega;
    const float k_e = omega_e * omega_e / (g * slope * upm);
    const float along = -p->wind_dir[1] * x + p->wind_dir[0] * z;
    const float cusp_time = OCEAN_CUSP_AMP * cosf(omega_e * t);
    const float cusp = 1.0f + cusp_time * cosf(k_e * along);

    const float climb = capture * cusp;
    const float swash = OCEAN_SWASH_SETUP + (1.0f - OCEAN_SWASH_SETUP) * sw;
    return runup * climb * env * swash;
}
