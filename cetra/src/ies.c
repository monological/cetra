#include "ies.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"
#include "util.h"

struct IesLibrary {
    IesProfile profiles[IES_MAX_PROFILES];
    int count;
    int pool_used; // floats of IES_POOL_FLOATS the loaded tables occupy
};

IesLibrary* create_ies_library(void) {
    IesLibrary* lib = calloc(1, sizeof(IesLibrary));
    if (!lib)
        log_error("Failed to allocate IES library");
    return lib;
}

void free_ies_library(IesLibrary* lib) {
    if (!lib)
        return;
    for (int i = 0; i < lib->count; i++)
        free(lib->profiles[i].path);
    free(lib);
}

int ies_library_count(const IesLibrary* lib) {
    return lib ? lib->count : 0;
}

int ies_library_pool_used(const IesLibrary* lib) {
    return lib ? lib->pool_used : 0;
}

bool ies_library_pack(const IesLibrary* lib, GpuIesBlock* block) {
    if (!lib || !block || lib->count == 0)
        return false;
    memset(block, 0, sizeof(*block));
    block->ies_counts[0] = lib->count;
    for (int i = 0; i < lib->count; i++) {
        const IesProfile* p = &lib->profiles[i];
        block->ies_desc[i * 2 + 0][0] = (float)p->offset;
        block->ies_desc[i * 2 + 0][1] = (float)p->v_taps;
        block->ies_desc[i * 2 + 0][2] = (float)p->h_taps;
        block->ies_desc[i * 2 + 0][3] = p->span;
        block->ies_desc[i * 2 + 1][0] = p->v_lo;
        block->ies_desc[i * 2 + 1][1] = p->v_hi;
        memcpy(&block->ies_pool[p->offset], p->table,
               (size_t)(p->v_taps * p->h_taps) * sizeof(float));
    }
    return true;
}

const IesProfile* ies_library_at(const IesLibrary* lib, int index) {
    if (!lib || index < 0 || index >= lib->count)
        return NULL;
    return &lib->profiles[index];
}

float ies_fold_horizontal(float angle_deg, float span_deg) {
    float period = 2.0f * span_deg;
    float f = fmodf(angle_deg, period);
    if (f < 0.0f)
        f += period;
    return f > span_deg ? period - f : f;
}

// Linear interpolation over a uniform grid of `taps` samples spanning [lo, hi],
// clamped at both ends. The table is resampled uniform at load precisely so the
// runtime lookup is this and not a search.
static float _lerp_uniform(const float* v, int taps, int stride, float t01) {
    if (taps <= 1)
        return v[0];
    float x = t01 * (float)(taps - 1);
    if (x <= 0.0f)
        return v[0];
    if (x >= (float)(taps - 1))
        return v[(taps - 1) * stride];
    int i = (int)x;
    float f = x - (float)i;
    return v[i * stride] * (1.0f - f) + v[(i + 1) * stride] * f;
}

float ies_profile_sample(const IesProfile* p, float v_deg, float h_deg) {
    if (!p)
        return 1.0f;
    // Outside the measured range the luminaire emits nothing -- see iesProfile
    // in lights_ubo.glsl, which this is the CPU twin of.
    if (v_deg < p->v_lo - 1e-3f || v_deg > p->v_hi + 1e-3f)
        return 0.0f;
    float vt = p->v_hi > p->v_lo ? (v_deg - p->v_lo) / (p->v_hi - p->v_lo) : 0.0f;
    if (vt < 0.0f)
        vt = 0.0f;
    if (vt > 1.0f)
        vt = 1.0f;
    if (p->h_taps <= 1) {
        // Rotationally symmetric: one column, no horizontal term at all.
        return _lerp_uniform(p->table, p->v_taps, p->h_taps, vt);
    }
    float ht = ies_fold_horizontal(h_deg, p->span) / p->span;

    // Bilinear: interpolate the two bracketing horizontal columns vertically,
    // then between them. The vertical walk strides by h_taps because the table
    // is v-major.
    float x = ht * (float)(p->h_taps - 1);
    if (x < 0.0f)
        x = 0.0f;
    if (x > (float)(p->h_taps - 1))
        x = (float)(p->h_taps - 1);
    int h0 = (int)x;
    int h1 = h0 + 1 < p->h_taps ? h0 + 1 : h0;
    float hf = x - (float)h0;
    float a = _lerp_uniform(&p->table[h0], p->v_taps, p->h_taps, vt);
    float b = _lerp_uniform(&p->table[h1], p->v_taps, p->h_taps, vt);
    return a * (1.0f - hf) + b * hf;
}

// ---- LM-63 parsing ---------------------------------------------------------
//
// The numeric blocks are read as ONE TOKEN STREAM rather than line by line. The
// standard lets the angle and candela blocks wrap at any column, so a line-based
// reader works on the files it was tested against and fails on the next
// exporter. This is the single most common way a naive LM-63 reader breaks.

typedef struct {
    const char* p;
    const char* end;
} Lexer;

static bool _next_float(Lexer* lx, float* out) {
    while (lx->p < lx->end && (*lx->p == ' ' || *lx->p == '\t' || *lx->p == '\r' ||
                               *lx->p == '\n' || *lx->p == ','))
        lx->p++;
    if (lx->p >= lx->end)
        return false;
    char* stop = NULL;
    double v = strtod(lx->p, &stop);
    if (stop == lx->p)
        return false;
    lx->p = stop;
    *out = (float)v;
    return true;
}

static bool _read_floats(Lexer* lx, float* dst, int n) {
    for (int i = 0; i < n; i++) {
        if (!_next_float(lx, &dst[i]))
            return false;
    }
    return true;
}

// The declared horizontal sweep, in degrees. LM-63 states its symmetry by which
// horizontal angles it lists -- one plane is rotationally symmetric, and a range
// ending at 90, 180 or 360 is quadrant, bilateral or none. So this reads the
// declaration; it never inspects the values to guess.
static bool _declared_span(const float* horiz, int n, float* out) {
    if (n == 1) {
        *out = 360.0f; // one plane describes the whole circle
        return true;
    }
    float last = horiz[n - 1];
    const float spans[3] = {90.0f, 180.0f, 360.0f};
    for (int i = 0; i < 3; i++) {
        // Files occasionally stop just short (179.5); snap rather than read that
        // as an unmirrored sweep.
        if (fabsf(last - spans[i]) <= 1.0f) {
            *out = spans[i];
            return true;
        }
    }
    return false;
}

static float _sample_irregular(const float* angles, const float* values, int n, float a) {
    if (a <= angles[0])
        return values[0];
    if (a >= angles[n - 1])
        return values[n - 1];
    int lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (angles[mid] <= a)
            lo = mid;
        else
            hi = mid;
    }
    float t = (a - angles[lo]) / (angles[hi] - angles[lo]);
    return values[lo] * (1.0f - t) + values[hi] * t;
}

#define IES_MAX_FILE_VERT 256
#define IES_MAX_FILE_HORIZ 256

static bool _parse_and_resample(const char* text, size_t len, const char* path, IesProfile* out) {
    // TILT is line-structured and comes before the numeric stream.
    const char* tilt = strstr(text, "TILT=");
    if (!tilt) {
        log_warn("ies: '%s' has no TILT= line; not an LM-63 file", path);
        return false;
    }
    if (strncmp(tilt, "TILT=NONE", 9) != 0) {
        // TILT=INCLUDE embeds a second table describing how output varies with
        // mounting angle. Refused rather than ignored: dropping it silently
        // renders a tilted luminaire at its untilted output.
        log_warn("ies: '%s' declares a TILT table, which is not supported; profile skipped", path);
        return false;
    }

    Lexer lx = {tilt + 9, text + len};
    float head[13];
    if (!_read_floats(&lx, head, 13)) {
        log_warn("ies: '%s' is truncated before the photometric header", path);
        return false;
    }
    float multiplier = head[2];
    int n_vert = (int)head[3];
    int n_horiz = (int)head[4];
    float ballast = head[10];
    if (n_vert < 2 || n_horiz < 1 || n_vert > IES_MAX_FILE_VERT || n_horiz > IES_MAX_FILE_HORIZ) {
        log_warn("ies: '%s' has an unusable angle grid (%d vertical x %d horizontal)", path,
                 n_vert, n_horiz);
        return false;
    }

    static float vert[IES_MAX_FILE_VERT];
    static float horiz[IES_MAX_FILE_HORIZ];
    static float cd[IES_MAX_FILE_VERT * IES_MAX_FILE_HORIZ];
    if (!_read_floats(&lx, vert, n_vert) || !_read_floats(&lx, horiz, n_horiz) ||
        !_read_floats(&lx, cd, n_vert * n_horiz)) {
        log_warn("ies: '%s' is truncated in its angle or candela block", path);
        return false;
    }
    for (int i = 0; i + 1 < n_vert; i++) {
        if (vert[i] >= vert[i + 1]) {
            log_warn("ies: '%s' has non-increasing vertical angles; profile skipped", path);
            return false;
        }
    }
    float span = 0.0f;
    if (!_declared_span(horiz, n_horiz, &span)) {
        log_warn("ies: '%s' ends its horizontal angles at %g, which is none of 90/180/360; "
                 "profile skipped",
                 path, horiz[n_horiz - 1]);
        return false;
    }

    // Candela are horizontal-major in the file and absolute in cd; the
    // multiplier and the ballast factor are both real derates a file uses.
    float scale = multiplier * (ballast > 0.0f ? ballast : 1.0f);
    bool symmetric = n_horiz == 1;
    int v_taps = n_vert < IES_MAX_VERT ? n_vert : IES_MAX_VERT;
    int h_taps = symmetric ? 1 : (n_horiz < IES_MAX_HORIZ ? n_horiz : IES_MAX_HORIZ);
    if (v_taps < 2)
        v_taps = 2;
    if (!symmetric && h_taps < 2)
        h_taps = 2;

    float peak = 0.0f;
    static float column[IES_MAX_FILE_VERT];
    static float plane[IES_MAX_FILE_HORIZ];
    for (int iv = 0; iv < v_taps; iv++) {
        float a_v = vert[0] + (vert[n_vert - 1] - vert[0]) * (float)iv / (float)(v_taps - 1);
        for (int ih = 0; ih < h_taps; ih++) {
            const float* col;
            if (symmetric) {
                col = cd; // the file's single plane, already v-ordered
            } else {
                float a_h = span * (float)ih / (float)(h_taps - 1);
                float folded = ies_fold_horizontal(a_h, span);
                for (int v = 0; v < n_vert; v++) {
                    for (int h = 0; h < n_horiz; h++)
                        plane[h] = cd[h * n_vert + v];
                    column[v] = _sample_irregular(horiz, plane, n_horiz, folded);
                }
                col = column;
            }
            float value = _sample_irregular(vert, col, n_vert, a_v) * scale;
            out->table[iv * h_taps + ih] = value;
            if (value > peak)
                peak = value;
        }
    }
    if (peak <= 0.0f) {
        log_warn("ies: '%s' has no positive candela anywhere; profile skipped", path);
        return false;
    }
    for (int i = 0; i < v_taps * h_taps; i++) {
        float t = out->table[i] / peak;
        // The tail tap is EXACTLY zero when the file's own tail is. pbr_frag
        // skips nine shadow taps and the whole GGX chain on attenuation <= 0.0,
        // which is only safe because every factor reaches zero exactly; a
        // profile that merely got small there would change which fragments take
        // that path.
        out->table[i] = t < 1e-6f ? 0.0f : t;
    }

    out->v_taps = v_taps;
    out->h_taps = h_taps;
    out->span = span;
    out->v_lo = vert[0];
    out->v_hi = vert[n_vert - 1];
    out->peak_cd = peak;

    // The angular support: the last vertical tap carrying anything. Read off the
    // NORMALISED table so it means the same thing whatever the file's absolute
    // scale, and taken as the tap's own angle rather than v_hi, because a file
    // measured to 180 whose upper half is all zero has a support of 90.
    out->support_deg = out->v_lo;
    for (int iv = v_taps - 1; iv >= 0; iv--) {
        bool live = false;
        for (int ih = 0; ih < h_taps && !live; ih++)
            live = out->table[iv * h_taps + ih] > 0.0f;
        if (live) {
            out->support_deg =
                out->v_lo + (out->v_hi - out->v_lo) * (float)iv / (float)(v_taps - 1);
            break;
        }
    }
    return true;
}

int ies_library_load(IesLibrary* lib, const char* path) {
    if (!lib || !path || !path[0])
        return -1;
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->profiles[i].path, path) == 0)
            return i; // one luminaire named twice is one profile
    }
    if (lib->count >= IES_MAX_PROFILES) {
        log_warn("ies: more than %d profiles; '%s' skipped", IES_MAX_PROFILES, path);
        return -1;
    }

    FILE* fh = fopen(path, "rb");
    if (!fh) {
        log_warn("ies: cannot open '%s'; the light keeps its analytic cone", path);
        return -1;
    }
    fseek(fh, 0, SEEK_END);
    long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fh);
        log_warn("ies: '%s' is empty", path);
        return -1;
    }
    char* text = malloc((size_t)size + 1);
    if (!text) {
        fclose(fh);
        log_error("ies: out of memory reading '%s'", path);
        return -1;
    }
    size_t got = fread(text, 1, (size_t)size, fh);
    fclose(fh);
    text[got] = '\0';

    IesProfile* p = &lib->profiles[lib->count];
    memset(p, 0, sizeof(*p));
    bool ok = _parse_and_resample(text, got, path, p);
    free(text);
    if (!ok)
        return -1;

    // The pool is the second limit, independent of the descriptor count: a
    // profile spends only its own taps, so a set of symmetric downlights costs a
    // sixteenth of what the same number of wall-washers would. Refused by name
    // rather than truncated, since a table cut short is a plausible wrong shape.
    int taps = p->v_taps * p->h_taps;
    if (lib->pool_used + taps > IES_POOL_FLOATS) {
        log_warn("ies: '%s' needs %d of %d remaining table floats; profile skipped", path, taps,
                 IES_POOL_FLOATS - lib->pool_used);
        return -1;
    }
    p->offset = lib->pool_used;
    lib->pool_used += taps;

    p->path = safe_strdup(path);
    log_info("IES '%s': %dx%d taps, %s, span %g deg, peak %.1f cd", path, p->v_taps, p->h_taps,
             p->h_taps == 1 ? "symmetric" : "asymmetric", (double)p->span, (double)p->peak_cd);
    return lib->count++;
}

// ---- the probe --------------------------------------------------------------

void ies_library_probe(const IesLibrary* lib) {
    int count = ies_library_count(lib);
    // Header FIRST with available=/reason=, the water/wind shape rather than the
    // emissive one: nothing here is produced by the walk below, so there is no
    // number the rows could contradict.
    if (count == 0) {
        printf("ies-probe header available=0 reason=%s\n",
               lib ? "no-profiles-loaded" : "no-library");
        fflush(stdout);
        return;
    }
    printf("ies-probe header available=1 profiles=%d pool_used=%d pool_cap=%d\n", count,
           ies_library_pool_used(lib), IES_POOL_FLOATS);

    for (int i = 0; i < count; i++) {
        const IesProfile* p = ies_library_at(lib, i);
        printf("ies-probe profile index=%d path=%s v_taps=%d h_taps=%d span=%.1f "
               "v_lo=%.3f v_hi=%.3f support=%.3f peak_cd=%.4f symmetric=%d\n",
               i, p->path, p->v_taps, p->h_taps, (double)p->span, (double)p->v_lo,
               (double)p->v_hi, (double)p->support_deg, (double)p->peak_cd, p->h_taps == 1 ? 1 : 0);

        // A sweep in ABSOLUTE candela, which is what the file states and so what
        // a gate can check against a generator's own arithmetic. Normalised
        // times peak IS absolute -- that identity is the whole reason the
        // normalised reading loses nothing, so printing it here is also what
        // holds that claim up.
        //
        // Sampled at the TAPS rather than at round angles: an interpolated value
        // between two taps tests the lerp, where a value AT one tests the table,
        // and it is the table a resample can get wrong.
        for (int iv = 0; iv < p->v_taps; iv++) {
            float v = p->v_lo + (p->v_hi - p->v_lo) * (float)iv / (float)(p->v_taps - 1);
            for (int ih = 0; ih < p->h_taps; ih++) {
                float h = p->h_taps == 1 ? 0.0f
                                         : p->span * (float)ih / (float)(p->h_taps - 1);
                float rel = ies_profile_sample(p, v, h);
                printf("ies-probe sample index=%d v=%.4f h=%.4f rel=%.6f cd=%.6f\n", i,
                       (double)v, (double)h, (double)rel, (double)(rel * p->peak_cd));
            }
        }
    }
    fflush(stdout);
}
