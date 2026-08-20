#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heightmap.h"

#include "../ext/stb_image.h"
#include "../ext/log.h"
#include "../util.h"

// The full range of the storage. A 16-bit sample of 65535 IS max_y, not one step
// short of it, which is what makes a save/load round trip land on the endpoints
// rather than drifting inward by a texel every time it is repeated.
#define HEIGHTMAP_R16_MAX 65535.0f

static bool ends_with_ci(const char* s, const char* suffix) {
    size_t ls = strlen(s), lx = strlen(suffix);
    if (lx > ls)
        return false;
    const char* tail = s + (ls - lx);
    for (size_t i = 0; i < lx; i++) {
        char a = tail[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

// Largest r with r*r <= n, by integer bisection. Not sqrtf: a double rounding at
// 4096 would silently accept a file one row short, which is exactly the corruption
// a headerless format cannot otherwise detect.
static int isqrt_floor(size_t n) {
    size_t lo = 0, hi = 65536;
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        if (mid * mid <= n)
            lo = mid;
        else
            hi = mid - 1;
    }
    return (int)lo;
}

static bool fill_from_u16(TerrainField* field, const unsigned short* src, int res, float min_y,
                          float max_y) {
    if (!terrain_field_alloc(field, res))
        return false;
    float span = max_y - min_y;
    size_t n = (size_t)res * (size_t)res;
    for (size_t k = 0; k < n; k++)
        field->height[k] = min_y + ((float)src[k] / HEIGHTMAP_R16_MAX) * span;
    return true;
}

static bool load_r16(TerrainField* field, const char* path, float min_y, float max_y) {
    long len = 0;
    char* raw = read_entire_file(path, &len);
    if (!raw) {
        log_warn("heightmap: cannot read %s", path);
        return false;
    }
    if (len <= 0 || (len % 2) != 0) {
        log_warn("heightmap: %s is %ld bytes, not a whole number of 16-bit samples", path, len);
        free(raw);
        return false;
    }

    size_t samples = (size_t)len / 2u;
    int res = isqrt_floor(samples);
    if ((size_t)res * (size_t)res != samples) {
        log_warn("heightmap: %s holds %zu samples, which is not a square -- a headerless "
                 ".r16 must be one, and a truncated file looks exactly like this",
                 path, samples);
        free(raw);
        return false;
    }
    if (res < 2) {
        log_warn("heightmap: %s is %dx%d, too small to interpolate", path, res, res);
        free(raw);
        return false;
    }

    // Assembled from bytes rather than cast, so the file reads the same whatever
    // the host's endianness is.
    unsigned short* samples16 = malloc(samples * sizeof(unsigned short));
    if (!samples16) {
        free(raw);
        return false;
    }
    const unsigned char* bytes = (const unsigned char*)raw;
    for (size_t k = 0; k < samples; k++)
        samples16[k] = (unsigned short)(bytes[k * 2] | ((unsigned)bytes[k * 2 + 1] << 8));

    bool ok = fill_from_u16(field, samples16, res, min_y, max_y);
    free(samples16);
    free(raw);
    if (ok)
        log_info("heightmap: loaded %s, %dx%d, range %.3f..%.3f", path, res, res, (double)min_y,
                 (double)max_y);
    return ok;
}

static bool load_png16(TerrainField* field, const char* path, float min_y, float max_y) {
    int w = 0, h = 0, channels = 0;
    // Forced to one channel: a heightmap's colour is not a thing, and a grey PNG
    // saved with an alpha would otherwise arrive interleaved.
    unsigned short* pixels = stbi_load_16(path, &w, &h, &channels, 1);
    if (!pixels) {
        log_warn("heightmap: cannot read %s as a 16-bit PNG (%s)", path, stbi_failure_reason());
        return false;
    }
    if (w != h || w < 2) {
        log_warn("heightmap: %s is %dx%d -- a heightfield must be square and at least 2", path, w,
                 h);
        stbi_image_free(pixels);
        return false;
    }

    bool ok = fill_from_u16(field, pixels, w, min_y, max_y);
    stbi_image_free(pixels);
    if (ok)
        log_info("heightmap: loaded %s, %dx%d, range %.3f..%.3f", path, w, w, (double)min_y,
                 (double)max_y);
    return ok;
}

bool heightmap_load(TerrainField* field, const char* path, float min_y, float max_y) {
    if (!field || !path)
        return false;
    if (!(max_y > min_y)) {
        log_warn("heightmap: %s asked for range %.3f..%.3f, which is empty or inverted", path,
                 (double)min_y, (double)max_y);
        return false;
    }
    if (ends_with_ci(path, ".png"))
        return load_png16(field, path, min_y, max_y);
    return load_r16(field, path, min_y, max_y);
}

void heightmap_height_range(const TerrainField* field, float* min_y, float* max_y) {
    if (!field || !field->height || field->res < 1) {
        if (min_y)
            *min_y = 0.0f;
        if (max_y)
            *max_y = 1.0f;
        return;
    }
    size_t n = (size_t)field->res * (size_t)field->res;
    float lo = field->height[0], hi = field->height[0];
    for (size_t k = 1; k < n; k++) {
        if (field->height[k] < lo)
            lo = field->height[k];
        if (field->height[k] > hi)
            hi = field->height[k];
    }
    // A perfectly flat field has no range to normalise against, and dividing by
    // it would put every sample at infinity rather than at a plane.
    if (!(hi > lo))
        hi = lo + 1.0f;
    if (min_y)
        *min_y = lo;
    if (max_y)
        *max_y = hi;
}

bool heightmap_save(const TerrainField* field, const char* path, float min_y, float max_y) {
    if (!field || !field->height || field->res < 2 || !path)
        return false;
    if (!(max_y > min_y)) {
        log_warn("heightmap: refusing to write %s over range %.3f..%.3f", path, (double)min_y,
                 (double)max_y);
        return false;
    }

    size_t n = (size_t)field->res * (size_t)field->res;
    unsigned char* out = malloc(n * 2u);
    if (!out)
        return false;

    float inv = 1.0f / (max_y - min_y);
    for (size_t k = 0; k < n; k++) {
        float t = (field->height[k] - min_y) * inv;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        // Round rather than truncate: truncation biases every sample downward by
        // half a step, which over a save/load cycle walks a terrain into the floor.
        unsigned v = (unsigned)(t * HEIGHTMAP_R16_MAX + 0.5f);
        out[k * 2] = (unsigned char)(v & 0xFFu);
        out[k * 2 + 1] = (unsigned char)((v >> 8) & 0xFFu);
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        log_warn("heightmap: cannot open %s for writing", path);
        free(out);
        return false;
    }
    size_t wrote = fwrite(out, 1, n * 2u, f);
    fclose(f);
    free(out);

    if (wrote != n * 2u) {
        log_warn("heightmap: short write to %s (%zu of %zu bytes)", path, wrote, n * 2u);
        return false;
    }
    log_info("heightmap: wrote %s, %dx%d, range %.3f..%.3f", path, field->res, field->res,
             (double)min_y, (double)max_y);
    return true;
}
