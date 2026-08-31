#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "cook.h"

#include "util.h" // fnv1a64: the 64-bit fold every key and payload hash uses

#include "ext/log.h"

// The .cca container. One file per artefact, flat directory: independent
// files invalidate independently, sync over Dropbox trivially, and a dead
// entry is just a file a later prune can sweep -- a pack would need an index
// with its own integrity and compaction story.
//
//   off  sz
//   0    4    magic "CCA1"
//   4    4    u32 container version (recipes version themselves in the key)
//   8    8    u64 key -- must equal the filename hex AND the recomputed key
//   16   4    u32 section_count
//   20   4    u32 flags (reserved, 0)
//   24   8    u64 payload hash: FNV-1a 64 over all section bytes in order.
//             terrain_stream got by on layout checks alone; this directory
//             lives under file sync, where a torn copy has a correct size and
//             a corrupt middle.
//   32   8    u64 total file bytes, cross-checked against the measured size
//   40   24   recipe, NUL-padded -- refusal by name needs the name in the file
//   64   16*N section table { u64 offset, u64 size }, offsets recomputed by
//             the reader and compared (the terrain_stream cross-check: a file
//             whose own offsets disagree with its own sizes is the failure
//             that renders)
//   64+16N    payload, sections tightly packed in table order
#define COOK_MAGIC 0x31414343u // "CCA1" little-endian
#define COOK_CONTAINER_VERSION 1u
#define COOK_HEADER_BYTES 64u


static struct {
    char dir[512];
    bool configured;
    bool enabled;
    bool store_disabled; // latched by the first unrecoverable store failure
    bool dir_made;
    int hits, misses, refused, cooked, store_failures;
    uint64_t bytes_read, bytes_written;
} g_cook;

// --- little-endian codecs (the terrain_stream.c idiom, private on purpose:
// --- cts and heightmap outputs are gate-asserted and gain nothing from churn)

static void put_u32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
    p[2] = (unsigned char)((v >> 16) & 0xffu);
    p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static void put_u64(unsigned char* p, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        p[i] = (unsigned char)((v >> (8 * i)) & 0xffu);
}

static uint32_t get_u32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

// --- the fold ---------------------------------------------------------------

static void fold_bytes(CookKey* key, const void* data, size_t bytes) {
    // The invalid-key no-op is what makes a DISABLED run cheap: sites fold
    // multi-megabyte inputs unconditionally, and without this every --no-cook
    // leg and every app that never called cook_init would pay the full hash
    // for a fetch that structurally refuses.
    if (key->valid)
        key->hash = fnv1a64(key->hash, data, bytes);
}

CookKey cook_key(const char* recipe) {
    CookKey key;
    memset(&key, 0, sizeof(key));
    key.hash = FNV1A64_BASIS;
    if (!g_cook.configured || !g_cook.enabled)
        return key; // valid stays false; folds no-op, fetch and store refuse
    if (!recipe || strlen(recipe) >= sizeof(key.recipe)) {
        log_warn("cook: recipe '%s' does not fit; this artefact is uncacheable",
                 recipe ? recipe : "(null)");
        return key;
    }
    strcpy(key.recipe, recipe);
    key.valid = true;
    fold_bytes(&key, recipe, strlen(recipe) + 1);
    return key;
}

void cook_key_u32(CookKey* key, uint32_t v) {
    unsigned char b[4];
    put_u32(b, v);
    fold_bytes(key, b, sizeof(b));
}

void cook_key_u64(CookKey* key, uint64_t v) {
    unsigned char b[8];
    put_u64(b, v);
    fold_bytes(key, b, sizeof(b));
}

void cook_key_i32(CookKey* key, int32_t v) {
    cook_key_u32(key, (uint32_t)v);
}

void cook_key_f32(CookKey* key, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    cook_key_u32(key, bits);
}

void cook_key_str(CookKey* key, const char* s) {
    if (s)
        fold_bytes(key, s, strlen(s) + 1);
}

void cook_key_label(CookKey* key, const char* name) {
    if (name) {
        strncpy(key->name, name, sizeof(key->name) - 1);
        key->name[sizeof(key->name) - 1] = '\0';
    }
}

void cook_key_bytes(CookKey* key, const void* data, size_t bytes) {
    cook_key_u64(key, (uint64_t)bytes);
    if (data && bytes)
        fold_bytes(key, data, bytes);
}

// --- lifecycle ---------------------------------------------------------------

void cook_init(const char* dir, bool enabled) {
    memset(&g_cook, 0, sizeof(g_cook));
    g_cook.configured = true;
    const char* env_off = getenv("CETRA_NO_COOK");
    if (env_off && env_off[0] && strcmp(env_off, "0") != 0)
        enabled = false;
    if (!dir)
        dir = getenv("CETRA_COOK_DIR");
    if (!dir)
        dir = "cooked";
    if (strlen(dir) >= sizeof(g_cook.dir)) {
        log_warn("cook: directory path does not fit; cook disabled");
        enabled = false;
    } else {
        strcpy(g_cook.dir, dir);
    }
    g_cook.enabled = enabled;
}

void cook_shutdown(void) {
    if (g_cook.configured && g_cook.enabled &&
        (g_cook.hits | g_cook.misses | g_cook.cooked | g_cook.refused))
        printf("cook-summary dir=%s cooked=%d hit=%d miss=%d refused=%d "
               "bytes_written=%llu bytes_read=%llu store_failures=%d\n",
               g_cook.dir, g_cook.cooked, g_cook.hits, g_cook.misses, g_cook.refused,
               (unsigned long long)g_cook.bytes_written, (unsigned long long)g_cook.bytes_read,
               g_cook.store_failures);
    memset(&g_cook, 0, sizeof(g_cook));
}

// --- paths -------------------------------------------------------------------

// <dir>/<recipe with '/' as '-'>-<16 hex>.cca -- recipe first so ls groups by
// kind; the hex is the identity; the extension names the container.
static void artefact_path(char* out, size_t cap, const CookKey* key, const char* suffix) {
    char flat[COOK_RECIPE_MAX];
    strcpy(flat, key->recipe);
    for (char* c = flat; *c; ++c)
        if (*c == '/')
            *c = '-';
    snprintf(out, cap, "%s/%s-%016llx.cca%s", g_cook.dir, flat, (unsigned long long)key->hash,
             suffix ? suffix : "");
}

// A failed mkdir over an existing directory is fine; a missing parent is not.
// Neither is distinguished here: the store's own fopen is the real check, and
// it latches store_disabled with one warning.
static void ensure_dir(void) {
    if (g_cook.dir_made)
        return;
#ifdef _WIN32
    _mkdir(g_cook.dir);
#else
    mkdir(g_cook.dir, 0755);
#endif
    g_cook.dir_made = true;
}

// --- fetch -------------------------------------------------------------------

static uint64_t hash_sections(const CookBlob* sections, int count) {
    uint64_t h = FNV1A64_BASIS;
    for (int s = 0; s < count; ++s)
        h = fnv1a64(h, sections[s].data, sections[s].size);
    return h;
}

static void refuse(const char* path, const char* why) {
    log_warn("cook: %s refused (%s); baking live", path, why);
    g_cook.refused++;
}

// Count, warn ONCE, latch. Store is advisory, so the run continues; the latch
// is what keeps a full disk from warning per artefact.
static bool store_failed(const char* temp) {
    g_cook.store_failures++;
    if (!g_cook.store_disabled) {
        log_warn("cook: cannot write %s; the cache keeps what it has and this run "
                 "stores nothing more",
                 temp);
        g_cook.store_disabled = true;
    }
    return false;
}

bool cook_fetch(const CookKey* key, CookBlob* sections, int section_count) {
    // Count validated BEFORE the memset it sizes -- the guard exists for the
    // bad caller, so it cannot run after the damage.
    if (!sections || section_count < 1 || section_count > COOK_MAX_SECTIONS)
        return false;
    memset(sections, 0, sizeof(*sections) * (size_t)section_count);
    if (!g_cook.configured || !g_cook.enabled || !key || !key->valid)
        return false;

    char path[600];
    artefact_path(path, sizeof(path), key, NULL);
    FILE* f = fopen(path, "rb");
    if (!f) {
        g_cook.misses++;
        return false;
    }

    bool ok = false;
    unsigned char header[COOK_HEADER_BYTES];
    unsigned char table[16 * COOK_MAX_SECTIONS];
    uint64_t sizes[COOK_MAX_SECTIONS];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        refuse(path, "shorter than a header");
        goto done;
    }
    if (get_u32(header + 0) != COOK_MAGIC) {
        refuse(path, "not a cook artefact");
        goto done;
    }
    if (get_u32(header + 4) != COOK_CONTAINER_VERSION) {
        refuse(path, "container version mismatch");
        goto done;
    }
    if (get_u64(header + 8) != key->hash) {
        refuse(path, "header key disagrees with its filename");
        goto done;
    }
    if (memcmp(header + 40, key->recipe, strlen(key->recipe) + 1) != 0) {
        refuse(path, "recipe name disagrees");
        goto done;
    }
    uint32_t stored_sections = get_u32(header + 16);
    if (stored_sections != (uint32_t)section_count) {
        // The tripwire for a recipe that changed shape without a version bump.
        refuse(path, "section count disagrees with the caller's recipe");
        goto done;
    }
    if (fread(table, 1, 16u * stored_sections, f) != 16u * stored_sections) {
        refuse(path, "ends inside its section table");
        goto done;
    }
    // Recompute the layout the sizes imply and compare against the stored
    // offsets -- a file whose own offsets disagree with its own sizes would
    // otherwise read somewhere legal and hand a section to the wrong owner.
    uint64_t expect = COOK_HEADER_BYTES + 16ull * stored_sections;
    for (uint32_t s = 0; s < stored_sections; ++s) {
        uint64_t off = get_u64(table + 16 * s);
        sizes[s] = get_u64(table + 16 * s + 8);
        if (off != expect) {
            refuse(path, "section table disagrees with the layout its sizes imply");
            goto done;
        }
        expect += sizes[s];
    }
    if (get_u64(header + 32) != expect) {
        refuse(path, "header total disagrees with the layout");
        goto done;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (uint64_t)ftell(f) != expect) {
        refuse(path, "file size disagrees with its header");
        goto done;
    }
    fseek(f, (long)(COOK_HEADER_BYTES + 16ull * stored_sections), SEEK_SET);
    for (int s = 0; s < section_count; ++s) {
        sections[s].size = (size_t)sizes[s];
        sections[s].data = malloc(sizes[s] ? (size_t)sizes[s] : 1u);
        if (!sections[s].data) {
            refuse(path, "allocation failed");
            goto done;
        }
        if (fread(sections[s].data, 1, (size_t)sizes[s], f) != (size_t)sizes[s]) {
            refuse(path, "short read");
            goto done;
        }
    }
    if (hash_sections(sections, section_count) != get_u64(header + 24)) {
        refuse(path, "payload hash mismatch");
        goto done;
    }
    ok = true;

done:
    fclose(f);
    if (!ok) {
        // Pre-zeroed and free(NULL) is defined, so no high-water bookkeeping.
        for (int s = 0; s < section_count; ++s)
            free(sections[s].data);
        memset(sections, 0, sizeof(*sections) * (size_t)section_count);
        return false;
    }
    g_cook.hits++;
    size_t total = 0;
    for (int s = 0; s < section_count; ++s)
        total += sections[s].size;
    g_cook.bytes_read += total;
    printf("cook site=%s name=%s result=hit bytes=%zu key=%016llx\n", key->recipe,
           key->name[0] ? key->name : "-", total, (unsigned long long)key->hash);
    return true;
}

// --- store -------------------------------------------------------------------

bool cook_store(const CookKey* key, const CookBlob* sections, int section_count) {
    if (!g_cook.configured || !g_cook.enabled || g_cook.store_disabled || !key || !key->valid ||
        !sections || section_count < 1 || section_count > COOK_MAX_SECTIONS)
        return false;
    for (int s = 0; s < section_count; ++s)
        if (!sections[s].data && sections[s].size)
            return false; // a bake that half-failed is not an artefact

    ensure_dir();
    char temp[600], path[600], suffix[32];
    snprintf(suffix, sizeof(suffix), ".tmp.%d", (int)
#ifdef _WIN32
                                                    _getpid()
#else
                                                    getpid()
#endif
    );
    artefact_path(temp, sizeof(temp), key, suffix);
    artefact_path(path, sizeof(path), key, NULL);

    uint64_t total = COOK_HEADER_BYTES + 16ull * (uint32_t)section_count;
    for (int s = 0; s < section_count; ++s)
        total += sections[s].size;

    unsigned char header[COOK_HEADER_BYTES];
    memset(header, 0, sizeof(header));
    put_u32(header + 0, COOK_MAGIC);
    put_u32(header + 4, COOK_CONTAINER_VERSION);
    put_u64(header + 8, key->hash);
    put_u32(header + 16, (uint32_t)section_count);
    put_u64(header + 24, hash_sections(sections, section_count));
    put_u64(header + 32, total);
    memcpy(header + 40, key->recipe, strlen(key->recipe) + 1);

    FILE* f = fopen(temp, "wb");
    if (!f)
        return store_failed(temp);
    bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
    uint64_t off = COOK_HEADER_BYTES + 16ull * (uint32_t)section_count;
    for (int s = 0; ok && s < section_count; ++s) {
        unsigned char row[16];
        put_u64(row + 0, off);
        put_u64(row + 8, sections[s].size);
        ok = fwrite(row, 1, sizeof(row), f) == sizeof(row);
        off += sections[s].size;
    }
    for (int s = 0; ok && s < section_count; ++s)
        if (sections[s].size)
            ok = fwrite(sections[s].data, 1, sections[s].size, f) == sections[s].size;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
#ifdef _WIN32
        remove(path); // Windows rename refuses to replace; the race window is
                      // a refused file the next run re-bakes
#endif
        ok = rename(temp, path) == 0;
    }
    if (!ok) {
        remove(temp);
        return store_failed(temp);
    }
    g_cook.cooked++;
    uint64_t payload = total - COOK_HEADER_BYTES - 16ull * (uint32_t)section_count;
    g_cook.bytes_written += payload;
    printf("cook site=%s name=%s result=cooked bytes=%llu key=%016llx\n", key->recipe,
           key->name[0] ? key->name : "-", (unsigned long long)payload,
           (unsigned long long)key->hash);
    return true;
}
