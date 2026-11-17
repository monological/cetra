#include "background.h"

#include <float.h>
#include <math.h>

// view.glsl's WS_SCENE_MAX, the ceiling sceneLight clamps a texel to before the curve.
#define BACKGROUND_SCENE_MAX 60000.0f

/*
 * The curves as tonemap_frag runs them, in single precision as it does, so the inverse is checked
 * against the same arithmetic it inverts. Each mirrors the function of the same name there.
 */

#define NEUTRAL_START        (0.8f - 0.04f)
#define NEUTRAL_DESATURATION 0.15f

static void _neutral(const float in[3], float out[3]) {
    float c[3] = {in[0], in[1], in[2]};
    const float x = fminf(c[0], fminf(c[1], c[2]));
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    for (int i = 0; i < 3; i++)
        c[i] -= offset;
    const float peak = fmaxf(c[0], fmaxf(c[1], c[2]));
    if (peak < NEUTRAL_START) {
        for (int i = 0; i < 3; i++)
            out[i] = c[i];
        return;
    }
    const float d = 1.0f - NEUTRAL_START;
    const float new_peak = 1.0f - d * d / (peak + d - NEUTRAL_START);
    const float g = 1.0f - 1.0f / (NEUTRAL_DESATURATION * (peak - new_peak) + 1.0f);
    for (int i = 0; i < 3; i++) {
        const float scaled = c[i] * (new_peak / peak);
        out[i] = scaled + (new_peak - scaled) * g;
    }
}

static float _aces(float x) {
    return glm_clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
}

// AgX's matrices, column by column as the shader's mat3 constructors take them.
static const mat3 AGX_INSET = {{0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f},
                               {0.0784335999999992f, 0.878468636469772f, 0.0784336f},
                               {0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f}};
static const mat3 AGX_OUTSET = {{1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f},
                                {-0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f},
                                {-0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f}};
#define AGX_MIN_EV -12.47393f
#define AGX_MAX_EV 4.026069f

static float _agx_sigmoid(float x) {
    const float x2 = x * x, x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 +
           0.1191f * x - 0.00232f;
}

static void _agx(const float in[3], float out[3]) {
    vec3 c = {0.0f, 0.0f, 0.0f};
    glm_mat3_mulv((vec3*)AGX_INSET, (float*)in, c);
    vec3 s = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 3; i++) {
        const float ev = glm_clamp(log2f(fmaxf(c[i], 1e-10f)), AGX_MIN_EV, AGX_MAX_EV);
        s[i] = _agx_sigmoid((ev - AGX_MIN_EV) / (AGX_MAX_EV - AGX_MIN_EV));
    }
    glm_mat3_mulv((vec3*)AGX_OUTSET, s, c);
    for (int i = 0; i < 3; i++)
        out[i] = powf(fmaxf(c[i], 0.0f), 2.2f);
}

// The curve alone, on a value the scene's sanitize has already clamped.
static void _curve(const float in[3], PostFXTonemapMode mode, float out[3]) {
    float c[3];
    for (int i = 0; i < 3; i++)
        c[i] = glm_clamp(in[i], 0.0f, BACKGROUND_SCENE_MAX);
    switch (mode) {
        case POSTFX_TONEMAP_ACES:
            for (int i = 0; i < 3; i++)
                out[i] = _aces(c[i]);
            return;
        case POSTFX_TONEMAP_AGX:
            _agx(c, out);
            return;
        case POSTFX_TONEMAP_LINEAR:
            for (int i = 0; i < 3; i++)
                out[i] = glm_clamp(c[i], 0.0f, 1.0f);
            return;
        default:
            _neutral(c, out);
            return;
    }
}

void background_display(const float working[3], PostFXTonemapMode mode, vec3 display) {
    if (mode == POSTFX_TONEMAP_PASSTHROUGH) {
        for (int i = 0; i < 3; i++)
            display[i] = glm_clamp(working[i], 0.0f, 1.0f);
        return;
    }
    float linear[3];
    _curve(working, mode, linear);
    for (int i = 0; i < 3; i++)
        display[i] = powf(glm_clamp(linear[i], 0.0f, 1.0f), 1.0f / 2.2f);
}

/*
 * The inverses. Each lands near the answer by the curve's own structure, and a few Newton steps on
 * the forward map then take it the rest of the way in the curve's own arithmetic.
 */

// Neutral's identity region, below its knee: the output is the input less an offset set by the
// smallest channel, which in the toe is x - 6.25 x^2 and above it 0.04.
static void _neutral_inverse(const float linear[3], float out[3]) {
    const float m = fminf(linear[0], fminf(linear[1], linear[2]));
    const float x = m < 0.04f ? sqrtf(fmaxf(m, 0.0f) / 6.25f) : m + 0.04f;
    for (int i = 0; i < 3; i++)
        out[i] = linear[i] + (x - m);
}

// AgX stage by stage: the 2.2 power, the outset matrix, the sigmoid by bisection, the log encode
// and the inset matrix, each undone.
static void _agx_inverse(const float linear[3], float out[3]) {
    vec3 encoded, s = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 3; i++)
        encoded[i] = powf(fmaxf(linear[i], 0.0f), 1.0f / 2.2f);
    mat3 outset_inv;
    glm_mat3_inv((vec3*)AGX_OUTSET, outset_inv);
    glm_mat3_mulv(outset_inv, encoded, s);
    vec3 v = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 3; i++) {
        float lo = 0.0f, hi = 1.0f;
        for (int k = 0; k < 40; k++) {
            const float mid = 0.5f * (lo + hi);
            if (_agx_sigmoid(mid) < s[i])
                lo = mid;
            else
                hi = mid;
        }
        v[i] = exp2f(0.5f * (lo + hi) * (AGX_MAX_EV - AGX_MIN_EV) + AGX_MIN_EV);
    }
    mat3 inset_inv;
    glm_mat3_inv((vec3*)AGX_INSET, inset_inv);
    glm_mat3_mulv(inset_inv, v, out);
}

// Newton on the curve: `w` moved until its curve gives `linear`, the Jacobian by differences. The
// closed-form inverses above are its start, and this is the answer: every step stays inside the
// range a cleared texel can hold, and the iterate nearest the colour is kept, so a colour the curve
// cannot give ends at the nearest it can rather than wherever the last step left it.
static void _polish(const float linear[3], PostFXTonemapMode mode, float w[3]) {
    float best[3] = {w[0], w[1], w[2]};
    float best_error = FLT_MAX;
    for (int step = 0; step < 32; step++) {
        float f[3];
        _curve(w, mode, f);
        vec3 residual = {f[0] - linear[0], f[1] - linear[1], f[2] - linear[2]};
        const float error = fabsf(residual[0]) + fabsf(residual[1]) + fabsf(residual[2]);
        if (error < best_error) {
            best_error = error;
            glm_vec3_copy(w, best);
        }
        if (error < 1e-7f)
            break;
        mat3 jacobian;
        for (int j = 0; j < 3; j++) {
            const float h = fmaxf(fabsf(w[j]) * 1e-3f, 1e-5f);
            float probe[3] = {w[0], w[1], w[2]}, fp[3];
            probe[j] += h;
            _curve(probe, mode, fp);
            for (int i = 0; i < 3; i++)
                jacobian[j][i] = (fp[i] - f[i]) / h;
        }
        if (fabsf(glm_mat3_det(jacobian)) < 1e-12f)
            break;
        mat3 inverse;
        glm_mat3_inv(jacobian, inverse);
        vec3 delta = {0.0f, 0.0f, 0.0f};
        glm_mat3_mulv(inverse, residual, delta);
        for (int i = 0; i < 3; i++)
            w[i] = glm_clamp(w[i] - delta[i], 0.0f, BACKGROUND_SCENE_MAX);
    }
    glm_vec3_copy(best, w);
}

bool background_working(const float display[3], PostFXTonemapMode mode, vec3 working) {
    float d[3];
    for (int i = 0; i < 3; i++)
        d[i] = glm_clamp(display[i], 0.0f, 1.0f);
    // A passthrough frame is copied into the picture as it is, with no curve and no encode.
    if (mode == POSTFX_TONEMAP_PASSTHROUGH) {
        glm_vec3_copy(d, working);
        return true;
    }
    float linear[3];
    for (int i = 0; i < 3; i++)
        linear[i] = powf(d[i], 2.2f);
    switch (mode) {
        case POSTFX_TONEMAP_LINEAR:
            glm_vec3_copy(linear, working);
            break;
        case POSTFX_TONEMAP_ACES:
            for (int i = 0; i < 3; i++) {
                // (2.51 - 2.43 L) x^2 + (0.03 - 0.59 L) x - 0.14 L = 0, the positive root.
                const double l = linear[i], a = 2.51 - 2.43 * l, b = 0.03 - 0.59 * l;
                const double disc = b * b + 4.0 * a * 0.14 * l;
                working[i] = a > 1e-9 ? (float)((-b + sqrt(fmax(disc, 0.0))) / (2.0 * a)) : 1e4f;
            }
            break;
        case POSTFX_TONEMAP_AGX:
            _agx_inverse(linear, working);
            break;
        default:
            _neutral_inverse(linear, working);
            break;
    }
    for (int i = 0; i < 3; i++)
        working[i] = glm_clamp(working[i], 0.0f, BACKGROUND_SCENE_MAX);
    _polish(linear, mode, working);
    vec3 back = {0.0f, 0.0f, 0.0f};
    background_display(working, mode, back);
    for (int i = 0; i < 3; i++)
        if (fabsf(back[i] - d[i]) > 0.25f / 255.0f)
            return false;
    return true;
}
