#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heightmap.h"

#include "../compat.h" // strcasecmp
#include "../ext/log.h"
#include "../ext/stb_image.h"
#include "../util.h"

// The full range of the storage. A 16-bit sample of 65535 IS max_y, not one step
// short of it, which is what makes a save/load round trip land on the endpoints
// rather than drifting inward by a texel every time it is repeated.
#define HEIGHTMAP_R16_MAX 65535.0f
#define HEIGHTMAP_R8_MAX  255.0f

// The three mask siblings, in the order TerrainMask declares them so a loop over
// one can index the other.
static const char* const MASK_SUFFIX[3] = {"_flow.r8", "_deposit.r8", "_wear.r8"};

static float* mask_plane(const TerrainField* field, int which) {
    switch (which) {
    case 0:
        return field->flow;
    case 1:
        return field->deposit;
    default:
        return field->wear;
    }
}

// "<dir>/<stem>.r16" -> "<dir>/<stem>_flow.r8". Writes into `out`, returns false
// if it would not fit rather than producing a truncated neighbour of some other
// file.
static bool sibling_path(char* out, size_t cap, const char* path, const char* suffix) {
    const char* dot = strrchr(path, '.');
    size_t stem = dot ? (size_t)(dot - path) : strlen(path);
    if (stem + strlen(suffix) + 1 > cap)
        return false;
    memcpy(out, path, stem);
    strcpy(out + stem, suffix);
    return true;
}

static bool write_all(const char* path, const unsigned char* bytes, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        log_warn("heightmap: cannot open %s for writing", path);
        return false;
    }
    size_t wrote = fwrite(bytes, 1, len, f);
    fclose(f);
    if (wrote != len) {
        log_warn("heightmap: short write to %s (%zu of %zu bytes)", path, wrote, len);
        return false;
    }
    return true;
}

static bool fill_from_u16(TerrainField* field, const unsigned short* src, int res, float min_y,
                          float max_y) {
    if (!terrain_field_alloc(field, res))
        return false;
    float span = max_y - min_y;
    size_t n = (size_t)res * (size_t)res;
    for (size_t k = 0; k < n; k++)
        field->height[k] = min_y + ((float)src[k] / HEIGHTMAP_R16_MAX) * span;
    field->min_y = min_y;
    field->max_y = max_y;
    return true;
}

// Read whichever mask siblings exist beside `path` into the field. Silent when a
// file is absent -- that is a heightmap with no erosion history, which is the
// normal case for anything a DCC tool produced -- and loud when one is present
// but the wrong size, which is a mismatched pair rather than a missing one.
static void load_masks(const TerrainField* field, const char* path) {
    size_t want = (size_t)field->res * (size_t)field->res;
    for (int m = 0; m < 3; m++) {
        char sib[1024];
        if (!sibling_path(sib, sizeof(sib), path, MASK_SUFFIX[m]))
            continue;
        long len = 0;
        char* raw = read_entire_file(sib, &len);
        if (!raw)
            continue;
        if ((size_t)len != want) {
            log_warn("heightmap: %s holds %ld bytes against the height's %zu -- ignoring a "
                     "mask that does not match the field it sits beside",
                     sib, len, want);
            free(raw);
            continue;
        }
        const unsigned char* bytes = (const unsigned char*)raw;
        float* plane = mask_plane(field, m);
        for (size_t k = 0; k < want; k++)
            plane[k] = (float)bytes[k] / HEIGHTMAP_R8_MAX;
        free(raw);
    }
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
    // sqrt is exactly rounded for every integer below 2^52, and the equality on
    // the next line is the real guard either way: a sloppy root can only cause a
    // false REJECTION here, never a silent accept.
    int res = (int)sqrt((double)samples);
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
    if (!terrain_field_alloc(field, res)) {
        free(raw);
        return false;
    }

    // Assembled from bytes and mapped in one pass. The two are together because
    // an intermediate u16 buffer would be a second allocation of the file's whole
    // size -- 33 MB on a 4096 square export -- to reach a helper the PNG path
    // needs and this one does not.
    const unsigned char* bytes = (const unsigned char*)raw;
    float span = max_y - min_y;
    for (size_t k = 0; k < samples; k++) {
        unsigned v = (unsigned)bytes[k * 2] | ((unsigned)bytes[k * 2 + 1] << 8);
        field->height[k] = min_y + ((float)v / HEIGHTMAP_R16_MAX) * span;
    }
    field->min_y = min_y;
    field->max_y = max_y;
    free(raw);

    load_masks(field, path);
    log_info("heightmap: loaded %s, %dx%d, range %.3f..%.3f", path, res, res, (double)min_y,
             (double)max_y);
    return true;
}

static bool load_png16(TerrainField* field, const char* path, float min_y, float max_y) {
    // Refused by name rather than up-converted. stb widens an 8-bit file to 16
    // silently, so the whole justification for this path -- that a DCC tool
    // exports PNG16 -- would be satisfied by a 256-level terraced heightfield
    // that renders as terrain and steps like a staircase.
    if (!stbi_is_16_bit(path)) {
        log_warn("heightmap: %s is an 8-bit PNG. A heightfield needs 16, and stb would widen "
                 "this one silently into 256 terraces",
                 path);
        return false;
    }

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
    if (ok) {
        load_masks(field, path);
        log_info("heightmap: loaded %s, %dx%d, range %.3f..%.3f", path, w, w, (double)min_y,
                 (double)max_y);
    }
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
    const char* dot = strrchr(path, '.');
    if (dot && strcasecmp(dot, ".png") == 0)
        return load_png16(field, path, min_y, max_y);
    return load_r16(field, path, min_y, max_y);
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
    bool ok = write_all(path, out, n * 2u);

    // The masks, into siblings. Written even when a bake left one flat -- an
    // absent file means "no erosion history" on the load side, and a field that
    // was eroded and simply has no wear anywhere is a different statement.
    for (int m = 0; ok && m < 3; m++) {
        const float* plane = mask_plane(field, m);
        if (!plane)
            continue;
        char sib[1024];
        if (!sibling_path(sib, sizeof(sib), path, MASK_SUFFIX[m])) {
            log_warn("heightmap: %s is too long to hang a %s sibling off", path, MASK_SUFFIX[m]);
            ok = false;
            break;
        }
        for (size_t k = 0; k < n; k++) {
            float t = plane[k];
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            out[k] = (unsigned char)(t * HEIGHTMAP_R8_MAX + 0.5f);
        }
        ok = write_all(sib, out, n);
    }

    free(out);
    if (ok)
        log_info("heightmap: wrote %s + 3 masks, %dx%d, range %.3f..%.3f", path, field->res,
                 field->res, (double)min_y, (double)max_y);
    return ok;
}
