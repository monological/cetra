#include <math.h>
#include <stdlib.h>
#include <string.h>
// strncasecmp; ies.c's note applies -- POSIX, and the cetra target already
// compiles with _POSIX_C_SOURCE.
#include <strings.h>

#include "lut.h"
#include "util.h"
#include "ext/log.h"

// ---- .cube parsing ---------------------------------------------------------
//
// Two passes, and the split is the format's rather than a convenience. Every
// header keyword precedes the data, so a line scan resolves them and finds
// where the numbers begin; the numbers themselves are then read as a TOKEN
// STREAM, because nothing in the format requires one triple per line and a
// line-based reader works on the files it was tested against and fails on the
// next exporter. That is ies.c's lesson about LM-63, in a second format.

typedef struct {
    const char* p;
    const char* end;
} Lexer;

// Skips whitespace, commas, and #-to-end-of-line. Comments are handled HERE
// rather than in the line scan because .cube permits them inside the data block
// too, where the line scan has already stopped looking.
static bool _next_float(Lexer* lx, float* out) {
    for (;;) {
        while (lx->p < lx->end && (*lx->p == ' ' || *lx->p == '\t' || *lx->p == '\r' ||
                                   *lx->p == '\n' || *lx->p == ','))
            lx->p++;
        if (lx->p < lx->end && *lx->p == '#') {
            while (lx->p < lx->end && *lx->p != '\n')
                lx->p++;
            continue;
        }
        break;
    }
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

// A header keyword at the start of a line, case-folded and requiring a
// delimiter after it -- so LUT_1D_SIZE is not matched by a prefix test against
// LUT_1D and TITLE_CARD is not matched as TITLE.
static bool _keyword(const char* s, const char* eol, const char* kw, const char** rest) {
    size_t n = strlen(kw);
    if ((size_t)(eol - s) < n || strncasecmp(s, kw, n) != 0)
        return false;
    const char* after = s + n;
    if (after < eol && *after != ' ' && *after != '\t')
        return false;
    while (after < eol && (*after == ' ' || *after == '\t'))
        after++;
    *rest = after;
    return true;
}

static bool _three_floats(const char* s, const char* eol, float out[3]) {
    Lexer lx = {s, eol};
    for (int i = 0; i < 3; i++) {
        if (!_next_float(&lx, &out[i]))
            return false;
    }
    return true;
}

// Resolve the header and return where the numbers start, or NULL on a refusal
// (which has already been named). `size` is 0 if the file declared none, and
// -1 if it declared one that is not a number -- two different refusals, and
// they were one until the distinction was measured: strtol maps both a missing
// value and "abc" to 0, so this reported a file that plainly declares a size as
// declaring none.
static const char* _parse_header(const char* text, size_t len, const char* path, long* size,
                                 char* title, size_t title_cap) {
    const char* end = text + len;
    const char* data_start = text;
    *size = 0;

    for (const char* line = text; line < end;) {
        const char* eol = line;
        while (eol < end && *eol != '\n' && *eol != '\r')
            eol++;

        const char* s = line;
        while (s < eol && (*s == ' ' || *s == '\t'))
            s++;

        // WHERE THE DATA BEGINS IS DECIDED BY THE LINE'S FIRST TOKEN, not by
        // whether a keyword matched. A .cube keyword can never start with a
        // digit, sign or dot and a data line always does, so this is an exact
        // discriminator -- where "no keyword matched" is not. That version
        // treated any header this reader does not know as the start of the
        // numbers, so `LUT_3D_INPUT_RANGE` (a real Iridas-era extension) and a
        // UTF-8 BOM both refused a loadable file as "truncated or malformed".
        // Naming the wrong reason is worse than not refusing: it sends the
        // reader to the data block to look for a fault that is in line 2.
        if (s < eol && *s != '#') {
            char* probe = NULL;
            strtod(s, &probe);
            if (probe != s) {
                data_start = line;
                break;
            }
        }

        const char* rest = NULL;
        bool consumed = false;
        if (s == eol || *s == '#') {
            consumed = true; // blank or comment; keep scanning
        } else if (_keyword(s, eol, "LUT_3D_SIZE", &rest)) {
            // Stays a `long` all the way to the caller's range check. Narrowing
            // here let an out-of-range declaration wrap back INTO range --
            // measured: LUT_3D_SIZE 4294967298 loaded as size 2, past a bound
            // whose own header promises a size is "refused by name rather than
            // clamped". The end pointer separates "not a number" from a
            // literal 0, which strtol alone maps to the same value.
            char* stop = NULL;
            *size = strtol(rest, &stop, 10);
            if (stop == rest)
                *size = -1;
            consumed = true;
        } else if (_keyword(s, eol, "LUT_1D_SIZE", &rest)) {
            // Refused by name. A 1D LUT is a per-channel curve -- exactly what
            // lift/gamma/gain already is -- so silently accepting one as a
            // degenerate 3D table would grade nothing and look like the feature
            // failing.
            log_warn("lut: '%s' is a 1D LUT; this path takes 3D .cube tables only", path);
            return NULL;
        } else if (_keyword(s, eol, "TITLE", &rest)) {
            const char* te = eol;
            while (rest < te && (*rest == '"' || *rest == ' '))
                rest++;
            while (te > rest && (te[-1] == '"' || te[-1] == ' ' || te[-1] == '\t'))
                te--;
            size_t n = (size_t)(te - rest);
            if (n >= title_cap)
                n = title_cap - 1;
            memcpy(title, rest, n);
            title[n] = '\0';
            consumed = true;
        } else if (_keyword(s, eol, "DOMAIN_MIN", &rest) ||
                   _keyword(s, eol, "DOMAIN_MAX", &rest)) {
            bool is_min = (strncasecmp(s, "DOMAIN_MIN", 10) == 0);
            float v[3];
            float want = is_min ? 0.0f : 1.0f;
            if (!_three_floats(rest, eol, v) || fabsf(v[0] - want) > 1e-6f ||
                fabsf(v[1] - want) > 1e-6f || fabsf(v[2] - want) > 1e-6f) {
                // A domain other than 0..1 means the table is indexed by
                // something that is not a display-referred pixel -- almost
                // always a log encoding. Nothing downstream would rescale into
                // it, so accepting it would sample the wrong region of the
                // table for every pixel in the frame.
                log_warn("lut: '%s' declares a %s other than %g; only the 0..1 domain is "
                         "supported (a log-encoded LUT needs a log input this pass does not have)",
                         path, is_min ? "DOMAIN_MIN" : "DOMAIN_MAX", (double)want);
                return NULL;
            }
            consumed = true;
        }

        // A keyword this reader does not know. The numeric probe above already
        // established the line is not data, so the file is well-formed and
        // carries something we have no use for -- note it and keep scanning
        // rather than refusing a table that is otherwise perfectly loadable.
        if (!consumed)
            log_warn("lut: '%s' has an unrecognised header line, ignored: %.*s", path,
                     (int)(eol - s), s);

        line = eol;
        while (line < end && (*line == '\n' || *line == '\r'))
            line++;
        data_start = line;
    }
    return data_start;
}

// How far a display-referred table may move mid-grey before it looks like it
// was authored for a different input space. A strong look moves it -- a
// contrast build can land 0.5 near 0.38 or 0.62 -- so this sits well outside
// that. A log-to-Rec709 show LUT is nowhere near: LogC's 0.5 is roughly 2.5
// stops over mid-grey, which lands near 0.9.
// TRILINEAR, and static, and both matter. The shipped default is TETRAHEDRAL,
// so a CPU sampler that disagrees with it is only safe while its one caller is
// the heuristic below -- which asks a yes/no question about mid-grey where the
// two interpolants agree. Exported, it would be the "an implementation that
// agrees with itself" hazard the gate's own oracle is built to avoid.
static void _sample_trilinear(const ColorLut* lut, const float in[3], float out[3]);

#define LUT_MIDGREY_SUSPECT 0.25f

static void _warn_if_not_display_referred(const ColorLut* lut, const char* path) {
    const float mid[3] = {0.5f, 0.5f, 0.5f};
    float out[3];
    _sample_trilinear(lut, mid, out);
    float worst = 0.0f;
    for (int i = 0; i < 3; i++)
        worst = fmaxf(worst, fabsf(out[i] - 0.5f));
    if (worst > LUT_MIDGREY_SUSPECT)
        log_warn("lut: '%s' maps mid-grey to (%.3f, %.3f, %.3f); this looks like a log-encoded "
                 "LUT rather than a display-referred one, and will grade the frame wrongly",
                 path, (double)out[0], (double)out[1], (double)out[2]);
}

bool lut_load_cube(const char* path, ColorLut* out) {
    if (!path || !path[0] || !out)
        return false;

    long len = 0;
    char* text = read_entire_file(path, &len);
    if (!text) {
        log_warn("lut: cannot read '%s'; the frame keeps its ungraded look", path);
        return false;
    }

    // A UTF-8 BOM is what any Windows editor writes, and it is not a keyword,
    // a comment or a number -- so without this the first line reads as neither
    // header nor data and the file is refused for a reason it has nothing to
    // do with.
    const char* body = text;
    long body_len = len;
    if (body_len >= 3 && (unsigned char)body[0] == 0xEF && (unsigned char)body[1] == 0xBB &&
        (unsigned char)body[2] == 0xBF) {
        body += 3;
        body_len -= 3;
    }

    long size = 0;
    char title[sizeof(out->title)] = {0};
    const char* data = _parse_header(body, (size_t)body_len, path, &size, title, sizeof(title));
    if (!data) {
        free(text);
        return false; // _parse_header already named the reason
    }
    if (size == 0) {
        log_warn("lut: '%s' declares no LUT_3D_SIZE; not a .cube file", path);
        free(text);
        return false;
    }
    if (size < 0) {
        log_warn("lut: '%s' declares a LUT_3D_SIZE that is not a number", path);
        free(text);
        return false;
    }
    if (size < LUT_MIN_SIZE || size > LUT_MAX_SIZE) {
        log_warn("lut: '%s' declares LUT_3D_SIZE %ld, outside the supported %d..%d", path, size,
                 LUT_MIN_SIZE, LUT_MAX_SIZE);
        free(text);
        return false;
    }

    size_t entries = (size_t)size * (size_t)size * (size_t)size;
    float* values = malloc(entries * 3 * sizeof(float));
    if (!values) {
        log_error("lut: out of memory for '%s' (%ld^3)", path, size);
        free(text);
        return false;
    }

    Lexer lx = {data, body + body_len};
    for (size_t i = 0; i < entries * 3; i++) {
        if (!_next_float(&lx, &values[i])) {
            log_warn("lut: '%s' ended after %zu of %zu values; truncated or malformed", path, i,
                     entries * 3);
            free(values);
            free(text);
            return false;
        }
        // Finite is not enough: the table uploads as GL_RGB16F, whose largest
        // value is 65504, so a finite-but-enormous entry becomes +inf in the
        // texture and NaN pixels through the rest of the chain. This is the
        // last place that sees the number before the driver does.
        if (!isfinite(values[i]) || fabsf(values[i]) > LUT_MAX_VALUE) {
            log_warn("lut: '%s' carries an out-of-range value %g at entry %zu; a .cube may go "
                     "outside 0..1 but not past what the fp16 table can hold",
                     path, (double)values[i], i / 3);
            free(values);
            free(text);
            return false;
        }
    }

    // Trailing numbers mean the declared size disagrees with what is in the
    // file. Refused, because the two readings differ by whole planes of the
    // table and the resulting image is plausible either way.
    float extra = 0.0f;
    if (_next_float(&lx, &extra)) {
        log_warn("lut: '%s' carries more than the %zu values LUT_3D_SIZE %ld declares", path,
                 entries * 3, size);
        free(values);
        free(text);
        return false;
    }

    free(text);
    out->size = (int)size;
    out->data = values;
    memcpy(out->title, title, sizeof(out->title));

    _warn_if_not_display_referred(out, path);
    log_info("LUT '%s': %d^3%s%s", path, out->size, out->title[0] ? ", " : "",
             out->title);
    return true;
}

void lut_free(ColorLut* lut) {
    if (!lut)
        return;
    free(lut->data);
    lut->data = NULL;
    lut->size = 0;
    lut->title[0] = '\0';
}

static void _sample_trilinear(const ColorLut* lut, const float in[3], float out[3]) {
    if (!lut || !lut->data || lut->size < 2) {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
        return;
    }
    int n = lut->size;
    int d = n - 1;
    int i0[3];
    float f[3];
    for (int k = 0; k < 3; k++) {
        float v = in[k] < 0.0f ? 0.0f : (in[k] > 1.0f ? 1.0f : in[k]);
        float p = v * (float)d;
        int idx = (int)p;
        if (idx > d - 1)
            idx = d - 1;
        i0[k] = idx;
        f[k] = p - (float)idx;
    }
    out[0] = out[1] = out[2] = 0.0f;
    for (int db = 0; db < 2; db++) {
        for (int dg = 0; dg < 2; dg++) {
            for (int dr = 0; dr < 2; dr++) {
                float w = (dr ? f[0] : 1.0f - f[0]) * (dg ? f[1] : 1.0f - f[1]) *
                          (db ? f[2] : 1.0f - f[2]);
                if (w == 0.0f)
                    continue;
                size_t o = (((size_t)(i0[2] + db) * (size_t)n + (size_t)(i0[1] + dg)) * (size_t)n +
                            (size_t)(i0[0] + dr)) *
                           3;
                for (int k = 0; k < 3; k++)
                    out[k] += w * lut->data[o + k];
            }
        }
    }
}
