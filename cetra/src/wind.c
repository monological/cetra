#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wind.h"
#include "util.h" // safe_strdup

// The amplitude coefficients windOffset() uses, from the file the shader reads
// them out of. Included rather than restated so the bound below cannot drift
// from the displacement it is a bound ON.
#include "../shaders/include/wind_bounds.glsl"

Wind* create_wind(const char* name) {
    Wind* wind = malloc(sizeof(Wind));
    if (!wind)
        return NULL;
    wind->name = NULL;
    set_wind_name(wind, name ? name : "wind");
    wind->type = WIND_DIRECTIONAL;
    glm_vec3_copy((vec3){0.0f, 0.0f, 1.0f}, wind->direction);
    wind->strength = 0.02f;
    wind->speed = 1.5f;
    wind->gust_frequency = 0.15f;
    wind->gust_amount = 0.8f;
    wind->turbulence = 0.2f;
    // Lockstep, which is what every scene authored before this existed had.
    wind->phase_variation = 0.0f;
    return wind;
}

void free_wind(Wind* wind) {
    if (!wind)
        return;
    free(wind->name);
    free(wind);
}

void set_wind_name(Wind* wind, const char* name) {
    if (!wind)
        return;
    free(wind->name);
    wind->name = safe_strdup(name);
}

void wind_upload_to_program(const Wind* wind, UniformManager* u) {
    if (!u)
        return;
    // No wind (or none on this scene): strength 0 makes every wind-aware shader
    // early-out, so the scene renders exactly as it did before the feature.
    if (!wind) {
        uniform_set_float(u, "uWindStrength", 0.0f);
        return;
    }
    uniform_set_vec3(u, "uWindDir", (const float*)wind->direction);
    uniform_set_float(u, "uWindStrength", wind->strength);
    uniform_set_float(u, "uWindSpeed", wind->speed);
    uniform_set_float(u, "uWindGustFreq", wind->gust_frequency);
    uniform_set_float(u, "uWindGustAmount", wind->gust_amount);
    uniform_set_float(u, "uWindTurbulence", wind->turbulence);
    uniform_set_float(u, "uWindPhaseVariation", wind->phase_variation);
}

// |vec3(sin a, 0, cos b)| at its worst, which is what both turbulence terms
// displace along -- the two sines are independently phased, so they can peak
// together and the bound has to assume they do.
#define WIND_LATERAL_MAX GLM_SQRT2f

// |vec3(1, WIND_LEAF_FLUTTER_Y, -WIND_LEAF_FLUTTER_Z)|, the fixed direction the
// leaf flutter rides along. Constant-folded, and out here rather than inside
// the branch that uses it because it is a property of the model, not of a call.
#define WIND_LEAF_DIR_MAX                                                                      \
    sqrtf(1.0f + WIND_LEAF_FLUTTER_Y * WIND_LEAF_FLUTTER_Y +                                   \
          WIND_LEAF_FLUTTER_Z * WIND_LEAF_FLUTTER_Z)

float wind_max_offset(const Wind* wind, float response, int mode, float flex_max, float leaf_max) {
    // The shader's own early-out, mirrored. Not just an optimisation: a
    // negative response would otherwise come back as a negative margin and
    // SHRINK the bound, which is the one direction a bound must never move.
    if (!wind || wind->strength <= 0.0f || response <= 0.0f)
        return 0.0f;

    // Every factor below is bounded by its own range rather than by a constant.
    // gust = mix(1 - gust_amount, 1, cubed 0..1) never exceeds 1 for the
    // documented gust_amount in [0,1]; the fabsf arm covers a scene that
    // authored one outside it. `dir` is normalized in the shader, so direction
    // contributes exactly 1 however the author wrote it. Every sin/cos is in
    // [-1,1], and `sway`, `mask` and h*h are in [0,1] and reach it.
    float gust = fmaxf(1.0f, fabsf(1.0f - wind->gust_amount));
    float amp = wind->strength * response * gust;
    float turb = wind->turbulence;

    if (mode == 0) {
        // Cloth: a forward billow of at most `amp` along the wind, plus a
        // lateral flutter whose vec3(sin, 0, cos) has magnitude up to sqrt(2).
        return amp * (1.0f + WIND_LATERAL_MAX * WIND_CLOTH_FLUTTER * turb);
    }

    // Vegetation: whole-body lean, plus a per-branch sway and a turbulent
    // flutter that both scale with the vertex flex weight. flex and the leaf
    // term's uv0.y are raw unclamped attributes, so they arrive measured from
    // the mesh rather than assumed to be within [0,1].
    float bound =
        amp * (WIND_VEG_LEAN +
               flex_max * (WIND_VEG_SWAY + WIND_LATERAL_MAX * WIND_VEG_TURB * turb));

    if (mode == 2) {
        // Leaf flutter rides on top of that.
        bound += amp * leaf_max * WIND_LEAF_DIR_MAX * turb;
    }
    return bound;
}
