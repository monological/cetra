#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terrain_stream.h"

#include "../ext/log.h"

/*
 * The file is little-endian by construction rather than by host: every scalar
 * goes through these four, so a field written on one machine reads on another
 * and neither has to know which it is. heightmap.c assembles its 16-bit samples
 * the same way and for the same reason.
 */
static void put_u32(unsigned char* b, uint32_t v) {
    b[0] = (unsigned char)(v & 0xffu);
    b[1] = (unsigned char)((v >> 8) & 0xffu);
    b[2] = (unsigned char)((v >> 16) & 0xffu);
    b[3] = (unsigned char)((v >> 24) & 0xffu);
}

static uint32_t get_u32(const unsigned char* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void put_u64(unsigned char* b, uint64_t v) {
    put_u32(b, (uint32_t)(v & 0xffffffffu));
    put_u32(b + 4, (uint32_t)(v >> 32));
}

static uint64_t get_u64(const unsigned char* b) {
    return (uint64_t)get_u32(b) | ((uint64_t)get_u32(b + 4) << 32);
}

// Floats travel as their bit pattern, not as text: a stream's whole reason for
// existing is that its coarse answer equals the unstreamed pyramid's bit for
// bit, and any decimal round trip gives that up on the first digit.
static void put_f32(unsigned char* b, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    put_u32(b, bits);
}

static float get_f32(const unsigned char* b) {
    uint32_t bits = get_u32(b);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

#define STREAM_HEADER_BYTES 64
#define STREAM_LEVEL_BYTES  16

// Level 0 carries the height and all three masks; every level above it is
// height alone, because that is what the in-memory pyramid holds and the file
// is a copy of it.
static int planes_at(int level) {
    return level == 0 ? 4 : 1;
}

// Tiles covering `res` nodes. The last one runs past the edge and is padded,
// which costs 55% at 257 nodes and 1.5% at 4097 -- the small case is the one
// that stays whole in memory anyway, so the waste is never paid where it would
// matter.
static int tiles_for(int res, int tile) {
    return (res + tile - 1) / tile;
}

static uint64_t tile_bytes_of(int tile) {
    return (uint64_t)tile * (uint64_t)tile * sizeof(float);
}

/*
 * Where every level starts, computed rather than trusted.
 *
 * Both the writer and the reader derive this, and the reader then checks the
 * table it READ against the table it computed. A file whose offsets disagree
 * with its own resolutions is refused whole: the alternative is a field that
 * loads, renders plausibly, and is a level out somewhere in the middle.
 */
static uint64_t stream_layout(int res, int level_count, int tile, int mask_res, uint64_t* offset,
                              int* out_res, int* out_tiles, uint64_t* mask_offset) {
    uint64_t at = STREAM_HEADER_BYTES + (uint64_t)level_count * STREAM_LEVEL_BYTES;
    if (mask_res > 0) {
        *mask_offset = at;
        at += (uint64_t)mask_res * (uint64_t)mask_res * 3u * sizeof(float);
    } else {
        *mask_offset = 0;
    }
    int r = res;
    for (int k = 0; k < level_count; ++k) {
        int t = tiles_for(r, tile);
        out_res[k] = r;
        out_tiles[k] = t;
        offset[k] = at;
        at += (uint64_t)t * (uint64_t)t * (uint64_t)planes_at(k) * tile_bytes_of(tile);
        r = (r - 1) / 2 + 1;
    }
    return at;
}

static const float* field_plane(const TerrainField* field, int plane) {
    switch (plane) {
    case 0:
        return field->height;
    case 1:
        return field->flow;
    case 2:
        return field->deposit;
    default:
        return field->wear;
    }
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

// The mask resolution the fallback keeps resident: halved from the field's own
// until it fits, which is the same node-centred rule the height pyramid halves
// by, so the coarse grid's nodes land on the fine grid's nodes.
static int mask_res_for(int res) {
    int m = res;
    while (m > TERRAIN_STREAM_RESIDENT_RES && ((m - 1) & 1) == 0)
        m = (m - 1) / 2 + 1;
    return m;
}

/*
 * Reduce a mask to the fallback resolution by averaging each destination node's
 * block of source nodes.
 *
 * Deliberately NOT filter_plane's [1 2 1] tent, and it does not have to be: the
 * height's coarse levels are compared against the in-memory pyramid bit for bit
 * by a gate arm, so their filter is a contract, while the masks have no
 * in-memory pyramid at all -- terrain.h records that building one was three
 * quarters of the memory for arrays nothing read. This grid is therefore an
 * artifact with no twin to agree with, and what it owes is only that it be a
 * smooth, deterministic summary of the plane beneath it.
 *
 * The consequence is worth stating plainly: a mask read through the fall is an
 * APPROXIMATION where a height read through the fall is exact. Every consumer
 * that cares -- the scatter's flow gate above all -- ensures its rect first and
 * never sees this.
 */
static bool reduce_mask(const float* src, int src_res, float* dst, int dst_res) {
    if (dst_res < 2 || src_res < dst_res)
        return false;
    int step = (src_res - 1) / (dst_res - 1);
    if (step < 1 || (src_res - 1) != step * (dst_res - 1))
        return false;
    for (int j = 0; j < dst_res; ++j) {
        for (int i = 0; i < dst_res; ++i) {
            int si = i * step, sj = j * step;
            float sum = 0.0f;
            int n = 0;
            for (int dj = 0; dj < step; ++dj) {
                for (int di = 0; di < step; ++di) {
                    int x = si + di, y = sj + dj;
                    if (x >= src_res)
                        x = src_res - 1;
                    if (y >= src_res)
                        y = src_res - 1;
                    sum += src[(size_t)y * (size_t)src_res + (size_t)x];
                    ++n;
                }
            }
            dst[(size_t)j * (size_t)dst_res + (size_t)i] = n ? sum / (float)n : 0.0f;
        }
    }
    return true;
}

// Gather one tile out of a whole plane, replicating the edge where the tile
// runs past it. The padding is never read back as terrain -- the sampler clamps
// to the level's own res long before -- so what it owes is only to be defined.
static void gather_tile(const float* plane, int res, int tile, int tx, int tz, float* out) {
    for (int j = 0; j < tile; ++j) {
        int sj = tz * tile + j;
        if (sj >= res)
            sj = res - 1;
        for (int i = 0; i < tile; ++i) {
            int si = tx * tile + i;
            if (si >= res)
                si = res - 1;
            out[(size_t)j * (size_t)tile + (size_t)i] = plane[(size_t)sj * (size_t)res + (size_t)si];
        }
    }
}

bool terrain_stream_save(const TerrainField* field, const char* path) {
    if (!field || !field->height || field->res < 2 || !path)
        return false;
    if (field->level_count < 2 || !field->levels) {
        log_warn("terrain_stream: %s not written -- the field has no coarse levels, so a "
                 "streamed read of it would have nothing to fall back to. A node-centred "
                 "grid halves only while res - 1 is even; %d does not.",
                 path, field->res);
        return false;
    }
    if (field->level_count > TERRAIN_STREAM_MAX_LEVELS) {
        log_warn("terrain_stream: %s not written -- %d levels exceeds the format's %d", path,
                 field->level_count, TERRAIN_STREAM_MAX_LEVELS);
        return false;
    }

    int tile = TERRAIN_STREAM_TILE_NODES;
    int mask_res = mask_res_for(field->res);
    int lres[TERRAIN_STREAM_MAX_LEVELS], ltiles[TERRAIN_STREAM_MAX_LEVELS];
    uint64_t offset[TERRAIN_STREAM_MAX_LEVELS], mask_offset = 0;
    stream_layout(field->res, field->level_count, tile, mask_res, offset, lres, ltiles,
                  &mask_offset);

    for (int k = 0; k < field->level_count; ++k) {
        if (field->levels[k].res != lres[k] || !field->levels[k].height) {
            log_warn("terrain_stream: %s not written -- level %d is %d nodes where the "
                     "halving rule says %d",
                     path, k, field->levels[k].res, lres[k]);
            return false;
        }
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        log_warn("terrain_stream: cannot open %s for writing", path);
        return false;
    }

    unsigned char head[STREAM_HEADER_BYTES];
    memset(head, 0, sizeof(head));
    put_u32(head + 0, TERRAIN_STREAM_MAGIC);
    put_u32(head + 4, TERRAIN_STREAM_VERSION);
    put_u32(head + 8, (uint32_t)field->res);
    put_u32(head + 12, (uint32_t)field->level_count);
    put_u32(head + 16, (uint32_t)tile);
    put_u32(head + 20, (uint32_t)mask_res);
    put_f32(head + 24, field->min_y);
    put_f32(head + 28, field->max_y);
    bool ok = fwrite(head, 1, sizeof(head), f) == sizeof(head);

    for (int k = 0; ok && k < field->level_count; ++k) {
        unsigned char row[STREAM_LEVEL_BYTES];
        put_u32(row + 0, (uint32_t)lres[k]);
        put_u32(row + 4, (uint32_t)ltiles[k]);
        put_u64(row + 8, offset[k]);
        ok = fwrite(row, 1, sizeof(row), f) == sizeof(row);
    }

    // The masks first, because the layout puts them first: a reader that wants
    // only the fallback stops after them without seeking into the tiles.
    if (ok && mask_res > 0) {
        size_t n = (size_t)mask_res * (size_t)mask_res;
        float* coarse = malloc(n * sizeof(float));
        unsigned char* bytes = malloc(n * sizeof(float));
        if (!coarse || !bytes) {
            ok = false;
        } else {
            for (int m = 0; ok && m < 3; ++m) {
                const float* src = field_plane(field, m + 1);
                if (mask_res == field->res)
                    memcpy(coarse, src, n * sizeof(float));
                else if (!reduce_mask(src, field->res, coarse, mask_res))
                    ok = false;
                for (size_t t = 0; ok && t < n; ++t)
                    put_f32(bytes + t * sizeof(float), coarse[t]);
                if (ok)
                    ok = fwrite(bytes, 1, n * sizeof(float), f) == n * sizeof(float);
            }
        }
        free(coarse);
        free(bytes);
    }

    size_t tn = (size_t)tile * (size_t)tile;
    float* scratch = malloc(tn * sizeof(float));
    unsigned char* tbytes = malloc(tn * sizeof(float));
    if (!scratch || !tbytes)
        ok = false;
    for (int k = 0; ok && k < field->level_count; ++k) {
        int planes = planes_at(k);
        for (int tz = 0; ok && tz < ltiles[k]; ++tz) {
            for (int tx = 0; ok && tx < ltiles[k]; ++tx) {
                for (int pl = 0; ok && pl < planes; ++pl) {
                    const float* src = k == 0 ? field_plane(field, pl) : field->levels[k].height;
                    gather_tile(src, lres[k], tile, tx, tz, scratch);
                    for (size_t t = 0; t < tn; ++t)
                        put_f32(tbytes + t * sizeof(float), scratch[t]);
                    ok = fwrite(tbytes, 1, tn * sizeof(float), f) == tn * sizeof(float);
                }
            }
        }
    }
    free(scratch);
    free(tbytes);

    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        log_warn("terrain_stream: short write to %s", path);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Reading tiles
// ---------------------------------------------------------------------------

static bool read_at(FILE* f, uint64_t offset, void* dst, size_t bytes) {
    if (fseek(f, (long)offset, SEEK_SET) != 0)
        return false;
    return fread(dst, 1, bytes, f) == bytes;
}

// Window-tile coordinates of a level tile, and whether it is inside the window
// at all. Window origins are tile multiples, so the two grids share a lattice
// and this is a subtraction rather than a search.
static bool window_tile_of(const TerrainStreamLevel* L, int tile, int tx, int tz, int* wtx,
                           int* wtz) {
    *wtx = tx - L->win_x0 / tile;
    *wtz = tz - L->win_z0 / tile;
    return *wtx >= 0 && *wtz >= 0 && *wtx < L->win_tiles && *wtz < L->win_tiles;
}

// One tile of one plane into the window. Nodes of the tile that fall outside
// either the level or the window are dropped rather than clipped against, since
// the tile is padded and the window may hang off the level's edge.
static bool read_tile_plane(TerrainStream* s, int level, int plane, int tx, int tz,
                            unsigned char* bytes, float* scratch) {
    TerrainStreamLevel* L = &s->lv[level];
    int tile = s->tile_nodes;
    size_t tn = (size_t)tile * (size_t)tile;
    uint64_t index = ((uint64_t)tz * (uint64_t)L->tiles + (uint64_t)tx) * (uint64_t)planes_at(level);
    uint64_t at = L->offset + (index + (uint64_t)plane) * tile_bytes_of(tile);
    if (!read_at(s->f, at, bytes, tn * sizeof(float)))
        return false;
    for (size_t t = 0; t < tn; ++t)
        scratch[t] = get_f32(bytes + t * sizeof(float));

    float* dst = L->mem[plane];
    for (int j = 0; j < tile; ++j) {
        int nj = tz * tile + j;
        if (nj >= L->res || nj < L->win_z0 || nj >= L->win_z0 + L->win_nodes)
            continue;
        for (int i = 0; i < tile; ++i) {
            int ni = tx * tile + i;
            if (ni >= L->res || ni < L->win_x0 || ni >= L->win_x0 + L->win_nodes)
                continue;
            dst[(size_t)(nj - L->win_z0) * (size_t)L->win_nodes + (size_t)(ni - L->win_x0)] =
                scratch[(size_t)j * (size_t)tile + (size_t)i];
        }
    }
    return true;
}

// Every plane of one tile, marked resident once they are all in. One tile is
// the unit of the budget and of the counter, whether that is four planes at
// level 0 or one above it -- a level-0 tile is 64 KB of one contiguous read.
static bool read_tile(TerrainStream* s, int level, int tx, int tz, unsigned char* bytes,
                      float* scratch) {
    TerrainStreamLevel* L = &s->lv[level];
    int wtx, wtz;
    if (!window_tile_of(L, s->tile_nodes, tx, tz, &wtx, &wtz))
        return true; // outside the window: nothing to hold it in
    if (L->resident[wtz * L->win_tiles + wtx])
        return true;
    for (int pl = 0; pl < planes_at(level); ++pl)
        if (!read_tile_plane(s, level, pl, tx, tz, bytes, scratch))
            return false;
    L->resident[wtz * L->win_tiles + wtx] = 1;
    s->loaded++;
    return true;
}

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------

TerrainStream* terrain_stream_open(const char* path, float extent, float l0_coverage,
                                   int resident_res, int window_tiles, int budget) {
    if (!path)
        return NULL;
    FILE* f = fopen(path, "rb");
    if (!f) {
        log_warn("terrain_stream: cannot open %s", path);
        return NULL;
    }

    unsigned char head[STREAM_HEADER_BYTES];
    if (fread(head, 1, sizeof(head), f) != sizeof(head)) {
        log_warn("terrain_stream: %s is shorter than a header", path);
        fclose(f);
        return NULL;
    }
    if (get_u32(head + 0) != TERRAIN_STREAM_MAGIC) {
        log_warn("terrain_stream: %s is not a terrain stream", path);
        fclose(f);
        return NULL;
    }
    if (get_u32(head + 4) != TERRAIN_STREAM_VERSION) {
        log_warn("terrain_stream: %s is version %u, this build reads %u", path, get_u32(head + 4),
                 TERRAIN_STREAM_VERSION);
        fclose(f);
        return NULL;
    }
    int res = (int)get_u32(head + 8);
    int level_count = (int)get_u32(head + 12);
    int tile = (int)get_u32(head + 16);
    int mask_res = (int)get_u32(head + 20);
    float min_y = get_f32(head + 24);
    float max_y = get_f32(head + 28);
    if (res < 2 || level_count < 1 || level_count > TERRAIN_STREAM_MAX_LEVELS || tile < 2 ||
        mask_res < 0) {
        log_warn("terrain_stream: %s has an impossible header (res %d, %d levels, tile %d)", path,
                 res, level_count, tile);
        fclose(f);
        return NULL;
    }

    int lres[TERRAIN_STREAM_MAX_LEVELS], ltiles[TERRAIN_STREAM_MAX_LEVELS];
    uint64_t offset[TERRAIN_STREAM_MAX_LEVELS], mask_offset = 0;
    uint64_t want =
        stream_layout(res, level_count, tile, mask_res, offset, lres, ltiles, &mask_offset);

    // The level table is read AND recomputed, then compared. A file whose own
    // offsets disagree with its own resolutions is the failure that renders:
    // every read lands somewhere legal and a level of the terrain is silently
    // somebody else's.
    for (int k = 0; k < level_count; ++k) {
        unsigned char row[STREAM_LEVEL_BYTES];
        if (fread(row, 1, sizeof(row), f) != sizeof(row)) {
            log_warn("terrain_stream: %s ends inside its level table", path);
            fclose(f);
            return NULL;
        }
        if ((int)get_u32(row + 0) != lres[k] || (int)get_u32(row + 4) != ltiles[k] ||
            get_u64(row + 8) != offset[k]) {
            log_warn("terrain_stream: %s level %d disagrees with the layout its own header "
                     "implies",
                     path, k);
            fclose(f);
            return NULL;
        }
    }

    if (fseek(f, 0, SEEK_END) != 0 || (uint64_t)ftell(f) != want) {
        log_warn("terrain_stream: %s is %lld bytes where its header describes %llu", path,
                 (long long)ftell(f), (unsigned long long)want);
        fclose(f);
        return NULL;
    }

    TerrainStream* s = calloc(1, sizeof(TerrainStream));
    if (!s) {
        fclose(f);
        return NULL;
    }
    s->f = f;
    snprintf(s->path, sizeof(s->path), "%s", path);
    s->tile_nodes = tile;
    s->level_count = level_count;
    s->resident_res = resident_res > 0 ? resident_res : TERRAIN_STREAM_RESIDENT_RES;
    s->window_tiles = window_tiles > 0 ? window_tiles : TERRAIN_STREAM_WINDOW_TILES;
    s->budget = budget > 0 ? budget : TERRAIN_STREAM_BUDGET;
    if (s->budget > TERRAIN_STREAM_BUDGET_MAX)
        s->budget = TERRAIN_STREAM_BUDGET_MAX;

    // Level 0's window is sized from the world the caller says it must serve
    // without falling, not from the constant the coarse levels use: it is the
    // level a region load reads, and a region load happens out to its own
    // radius. Rounded up to whole tiles and never past the level itself.
    int l0_tiles = s->window_tiles;
    float cell0 = terrain_field_cell(extent, res);
    if (l0_coverage > 0.0f && cell0 > 0.0f) {
        int nodes = (int)(2.0f * l0_coverage / cell0) + 2 * TERRAIN_STREAM_APRON_NODES;
        int want_tiles = tiles_for(nodes, tile);
        if (want_tiles > l0_tiles)
            l0_tiles = want_tiles;
    }

    for (int k = 0; k < level_count; ++k) {
        TerrainStreamLevel* L = &s->lv[k];
        L->res = lres[k];
        L->tiles = ltiles[k];
        L->offset = offset[k];
        L->whole = lres[k] <= s->resident_res;
        int wt = k == 0 ? l0_tiles : s->window_tiles;
        // n * tile + 1 nodes, NOT n * tile, and it is the same reason a level is
        // 2^k + 1: the grid is node-centred, so a run of whole tiles covers one
        // node fewer than it looks. Sized the other way, no tile-aligned window
        // can ever reach the level's last node -- the far edge of the terrain
        // becomes permanently unreachable, every rect touching it silently
        // fails to become resident, and what gets built there comes from the
        // fall. It reaches Jolt as a collider flat enough that the tree builder
        // cannot split it.
        //
        // It also makes the origin bound land on a tile: res - win_nodes is
        // 2^k - n * tile, which the tile divides.
        L->win_nodes = L->whole ? lres[k] : wt * tile + 1;
        if (L->win_nodes > lres[k])
            L->win_nodes = lres[k];
        L->win_tiles = (L->win_nodes - 1) / tile + 1;
        L->win_x0 = L->win_z0 = 0;
    }

    // The coarsest whole level is where the fall terminates, so it has to
    // exist: a file whose finest level already exceeds the threshold and whose
    // pyramid stops short would have no answer for a miss.
    s->coarsest_whole = -1;
    for (int k = level_count - 1; k >= 0; --k) {
        if (s->lv[k].whole) {
            s->coarsest_whole = k;
            break;
        }
    }
    if (s->coarsest_whole < 0) {
        log_warn("terrain_stream: %s has no level at or under %d nodes, so a miss would have "
                 "nothing to fall to",
                 path, s->resident_res);
        terrain_stream_free(s);
        return NULL;
    }

    size_t tn = (size_t)tile * (size_t)tile;
    unsigned char* bytes = malloc(tn * sizeof(float));
    float* scratch = malloc(tn * sizeof(float));
    bool ok = bytes && scratch;

    for (int k = 0; ok && k < level_count; ++k) {
        TerrainStreamLevel* L = &s->lv[k];
        size_t wn = (size_t)L->win_nodes * (size_t)L->win_nodes;
        for (int pl = 0; ok && pl < planes_at(k); ++pl) {
            L->mem[pl] = calloc(wn, sizeof(float));
            if (!L->mem[pl])
                ok = false;
        }
        L->resident = calloc((size_t)L->win_tiles * (size_t)L->win_tiles, 1);
        if (!L->resident)
            ok = false;
        if (!ok || !L->whole)
            continue;
        for (int tz = 0; ok && tz < L->tiles; ++tz)
            for (int tx = 0; ok && tx < L->tiles; ++tx)
                ok = read_tile(s, k, tx, tz, bytes, scratch);
    }

    if (ok && mask_res > 0) {
        size_t n = (size_t)mask_res * (size_t)mask_res;
        unsigned char* mb = malloc(n * sizeof(float));
        if (!mb) {
            ok = false;
        } else {
            for (int m = 0; ok && m < 3; ++m) {
                s->coarse_mask[m] = malloc(n * sizeof(float));
                if (!s->coarse_mask[m]) {
                    ok = false;
                    break;
                }
                ok = read_at(f, mask_offset + (uint64_t)m * n * sizeof(float), mb,
                             n * sizeof(float));
                for (size_t t = 0; ok && t < n; ++t)
                    s->coarse_mask[m][t] = get_f32(mb + t * sizeof(float));
            }
            s->mask_res = mask_res;
        }
        free(mb);
    }

    free(bytes);
    free(scratch);
    if (!ok) {
        log_warn("terrain_stream: %s could not be read into memory", path);
        terrain_stream_free(s);
        return NULL;
    }

    // The seam. `res` and the level table are the file's, so level selection --
    // terrain_level_for_cell and everything built on it -- works on a streamed
    // field exactly as on a resident one; the plane pointers address windows,
    // which is what TerrainField.stream warns every other reader about.
    s->field.res = s->lv[0].res;
    s->field.height = s->lv[0].mem[0];
    s->field.flow = s->lv[0].mem[1];
    s->field.deposit = s->lv[0].mem[2];
    s->field.wear = s->lv[0].mem[3];
    s->field.min_y = min_y;
    s->field.max_y = max_y;
    s->field.stream = s;
    s->field.levels = calloc((size_t)level_count, sizeof(TerrainFieldLevel));
    if (!s->field.levels) {
        terrain_stream_free(s);
        return NULL;
    }
    for (int k = 0; k < level_count; ++k) {
        s->field.levels[k].res = s->lv[k].res;
        s->field.levels[k].height = s->lv[k].mem[0];
    }
    s->field.level_count = level_count;
    return s;
}

void terrain_stream_free(TerrainStream* s) {
    if (!s)
        return;
    for (int k = 0; k < s->level_count; ++k) {
        for (int pl = 0; pl < 4; ++pl)
            free(s->lv[k].mem[pl]);
        free(s->lv[k].resident);
    }
    for (int m = 0; m < 3; ++m)
        free(s->coarse_mask[m]);
    // Not terrain_field_free: level 0 aliases a window this loop already
    // released, and the planes were never terrain_field_alloc's to begin with.
    free(s->field.levels);
    if (s->f)
        fclose(s->f);
    free(s);
}

// ---------------------------------------------------------------------------
// Residency
// ---------------------------------------------------------------------------

// Node coordinate of a world XZ at a level, unclamped. The window is placed in
// the same space the sampler indexes, so this is the one conversion both use.
static float node_of(const TerrainParams* p, int res, float w, float centre) {
    float cell = terrain_field_cell(p->extent, res);
    return cell > 0.0f ? ((w - centre) + p->extent) / cell : 0.0f;
}

/*
 * Where a window wants to sit, given where its anchor is.
 *
 * The DEAD-BAND is the second clause and it is what stops a camera breathing
 * across a tile boundary from re-reading a column every frame: a window only
 * moves once its anchor comes within a tile of an edge, so crossing back and
 * forth inside it costs nothing. Snapped to whole tiles so a move can keep its
 * overlap, and clamped so a window never hangs off the level it indexes.
 */
static int window_target(int anchor, int cur, int win_nodes, int res, int tile) {
    if (win_nodes >= res)
        return 0;
    if (anchor >= cur + tile && anchor < cur + win_nodes - tile)
        return cur;
    int want = anchor - win_nodes / 2;
    int max = res - win_nodes;
    if (want < 0)
        want = 0;
    if (want > max)
        want = max;
    want = (want / tile) * tile;
    return want;
}

/*
 * Slide a window's contents to a new origin, keeping the overlap.
 *
 * Both windows are the same size, so a node's index moves by a CONSTANT --
 * (oz0 - nz0) * W + (ox0 - nx0) -- which is what makes this a shift rather than
 * a gather: the row order only has to avoid clobbering, forward when the offset
 * is negative and backward when it is positive. The alternative, toroidal
 * addressing, would put a modulo on every one of the sixteen taps a sample
 * makes, to save a memmove that happens once per tile crossed.
 */
static void window_shift(TerrainStreamLevel* L, int planes, int nx0, int nz0, int tile) {
    int W = L->win_nodes;
    int ox0 = L->win_x0, oz0 = L->win_z0;
    if (ox0 == nx0 && oz0 == nz0)
        return;

    int lo_x = ox0 > nx0 ? ox0 : nx0;
    int hi_x = (ox0 + W < nx0 + W ? ox0 + W : nx0 + W);
    int lo_z = oz0 > nz0 ? oz0 : nz0;
    int hi_z = (oz0 + W < nz0 + W ? oz0 + W : nz0 + W);

    if (hi_x > lo_x && hi_z > lo_z) {
        // delta is dst MINUS src, so the source of a row is dst - delta. Written
        // the other way it slides the terrain the wrong direction, and the tiles
        // it lands on stay marked resident -- so nothing falls, nothing warns,
        // and the surface is simply somewhere else.
        long delta = (long)(oz0 - nz0) * (long)W + (long)(ox0 - nx0);
        int width = hi_x - lo_x;
        for (int pl = 0; pl < planes; ++pl) {
            float* m = L->mem[pl];
            // Rows in the order that cannot clobber a source before it is read:
            // ascending when the data moves down in memory, descending when up.
            if (delta < 0) {
                for (int j = lo_z; j < hi_z; ++j) {
                    size_t dst = (size_t)(j - nz0) * (size_t)W + (size_t)(lo_x - nx0);
                    memmove(m + dst, m + (size_t)((long)dst - delta), (size_t)width * sizeof(float));
                }
            } else {
                for (int j = hi_z - 1; j >= lo_z; --j) {
                    size_t dst = (size_t)(j - nz0) * (size_t)W + (size_t)(lo_x - nx0);
                    memmove(m + dst, m + (size_t)((long)dst - delta), (size_t)width * sizeof(float));
                }
            }
        }
    }

    /*
     * The tile bitmap follows the same shift, so a window that moved one column
     * keeps the residency of everything it kept the data for -- with one
     * exception that is easy to miss and expensive to have.
     *
     * A window is n * tile + 1 nodes, so its LAST tile on each axis holds a
     * single node column. Shift by a tile and that stub becomes an interior
     * tile needing all 64, of which 63 were never read. Carrying its bit marks
     * a tile resident over stale memory: nothing falls, nothing warns, and the
     * ground there is whatever the window held two positions ago. So residency
     * carries only from a source tile that was COMPLETE, which costs one tile
     * row and column of re-reading per move.
     */
    int T = L->win_tiles;
    unsigned char* keep = calloc((size_t)T * (size_t)T, 1);
    if (keep) {
        int dtx = (nx0 - ox0) / tile, dtz = (nz0 - oz0) / tile;
        for (int tz = 0; tz < T; ++tz) {
            for (int tx = 0; tx < T; ++tx) {
                int stx = tx + dtx, stz = tz + dtz;
                bool complete = (stx + 1) * tile <= L->win_nodes && (stz + 1) * tile <= L->win_nodes;
                if (stx >= 0 && stz >= 0 && stx < T && stz < T && complete)
                    keep[tz * T + tx] = L->resident[stz * T + stx];
            }
        }
        memcpy(L->resident, keep, (size_t)T * (size_t)T);
        free(keep);
    } else {
        memset(L->resident, 0, (size_t)T * (size_t)T);
    }
    L->win_x0 = nx0;
    L->win_z0 = nz0;
}

void terrain_stream_update(TerrainStream* s, const TerrainParams* p, const vec3 eye,
                           const vec3 focus) {
    if (!s || !p)
        return;
    size_t tn = (size_t)s->tile_nodes * (size_t)s->tile_nodes;
    unsigned char* bytes = NULL;
    float* scratch = NULL;
    int spent = 0;

    for (int k = 0; k < s->level_count; ++k) {
        TerrainStreamLevel* L = &s->lv[k];
        if (L->whole)
            continue;
        // Level 0 follows the PLAYER and the coarse levels follow the CAMERA,
        // which is the same split residency already makes: what is stood on is
        // decided by one and what is drawn by the other.
        const float* a = k == 0 ? focus : eye;
        int ax = (int)node_of(p, L->res, a[0], p->center[0]);
        int az = (int)node_of(p, L->res, a[2], p->center[1]);
        int nx = window_target(ax, L->win_x0, L->win_nodes, L->res, s->tile_nodes);
        int nz = window_target(az, L->win_z0, L->win_nodes, L->res, s->tile_nodes);
        window_shift(L, planes_at(k), nx, nz, s->tile_nodes);

        // Fill toward the window in a fixed order, nearest tile row first, so
        // two runs of one build spend the same budget on the same tiles.
        for (int tz = 0; tz < L->win_tiles && spent < s->budget; ++tz) {
            for (int tx = 0; tx < L->win_tiles && spent < s->budget; ++tx) {
                if (L->resident[tz * L->win_tiles + tx])
                    continue;
                if (!bytes) {
                    bytes = malloc(tn * sizeof(float));
                    scratch = malloc(tn * sizeof(float));
                    if (!bytes || !scratch) {
                        free(bytes);
                        free(scratch);
                        return;
                    }
                }
                if (!read_tile(s, k, tx + L->win_x0 / s->tile_nodes, tz + L->win_z0 / s->tile_nodes,
                               bytes, scratch))
                    break;
                spent++;
            }
        }
    }
    free(bytes);
    free(scratch);
}

bool terrain_stream_ensure_rect(TerrainStream* s, const TerrainParams* p, float x0, float z0,
                                float span, int level) {
    if (!s || !p || !(span > 0.0f))
        return false;
    if (level < 0)
        level = 0;
    if (level >= s->level_count)
        level = s->level_count - 1;
    s->ensured++;
    TerrainStreamLevel* L = &s->lv[level];
    if (L->whole)
        return true;

    float nx0 = node_of(p, L->res, x0, p->center[0]) - TERRAIN_STREAM_APRON_NODES;
    float nz0 = node_of(p, L->res, z0, p->center[1]) - TERRAIN_STREAM_APRON_NODES;
    float nx1 = node_of(p, L->res, x0 + span, p->center[0]) + TERRAIN_STREAM_APRON_NODES;
    float nz1 = node_of(p, L->res, z0 + span, p->center[1]) + TERRAIN_STREAM_APRON_NODES;
    int i0 = (int)nx0, j0 = (int)nz0, i1 = (int)nx1 + 1, j1 = (int)nz1 + 1;
    if (i0 < 0)
        i0 = 0;
    if (j0 < 0)
        j0 = 0;
    if (i1 > L->res)
        i1 = L->res;
    if (j1 > L->res)
        j1 = L->res;
    if (i1 <= i0 || j1 <= j0)
        return true; // entirely outside the domain; the clamp serves it

    if (i1 - i0 > L->win_nodes || j1 - j0 > L->win_nodes) {
        if (!s->warned_capacity) {
            s->warned_capacity = true;
            log_warn("terrain_stream: a %dx%d node rect at level %d does not fit a %d node "
                     "window; it reads coarse. Raise --terrain-stream-window.",
                     i1 - i0, j1 - j0, level, L->win_nodes);
        }
        return false;
    }

    // Re-anchor if the rect is not inside the window. This is the teleport
    // path: a caller may ask for ground the anchors have not reached, and a
    // window that refused to move would hand it a coarse collider forever.
    int tile = s->tile_nodes;
    if (i0 < L->win_x0 || i1 > L->win_x0 + L->win_nodes || j0 < L->win_z0 ||
        j1 > L->win_z0 + L->win_nodes) {
        int nx = (i0 + i1) / 2 - L->win_nodes / 2;
        int nz = (j0 + j1) / 2 - L->win_nodes / 2;
        int max = L->res - L->win_nodes;
        if (nx < 0)
            nx = 0;
        if (nz < 0)
            nz = 0;
        if (nx > max)
            nx = max;
        if (nz > max)
            nz = max;
        nx = (nx / tile) * tile;
        nz = (nz / tile) * tile;
        window_shift(L, planes_at(level), nx, nz, tile);
    }

    // Checked AFTER the move, because a silent failure here is the expensive
    // one: read_tile drops tiles outside the window without complaining, so an
    // ensure that did not cover its rect would return success and leave the
    // caller building a collider out of the fall. Loud, once, and false.
    if (i0 < L->win_x0 || i1 > L->win_x0 + L->win_nodes || j0 < L->win_z0 ||
        j1 > L->win_z0 + L->win_nodes) {
        if (!s->warned_capacity) {
            s->warned_capacity = true;
            log_warn("terrain_stream: level %d could not place a window over nodes "
                     "[%d,%d]x[%d,%d]; it reads coarse",
                     level, i0, i1, j0, j1);
        }
        return false;
    }

    size_t tn = (size_t)tile * (size_t)tile;
    unsigned char* bytes = malloc(tn * sizeof(float));
    float* scratch = malloc(tn * sizeof(float));
    bool ok = bytes && scratch;
    int tx0 = i0 / tile, tx1 = (i1 - 1) / tile;
    int tz0 = j0 / tile, tz1 = (j1 - 1) / tile;
    for (int tz = tz0; ok && tz <= tz1; ++tz)
        for (int tx = tx0; ok && tx <= tx1; ++tx)
            ok = read_tile(s, level, tx, tz, bytes, scratch);
    free(bytes);
    free(scratch);
    return ok;
}

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

static uint32_t stream_digest(const TerrainStream* s) {
    uint32_t h = 2166136261u;
    for (int k = 0; k < s->level_count; ++k) {
        const int v[2] = {s->lv[k].win_x0, s->lv[k].win_z0};
        for (int i = 0; i < 2; ++i) {
            h ^= (uint32_t)v[i];
            h *= 16777619u;
        }
        int n = s->lv[k].win_tiles * s->lv[k].win_tiles;
        for (int t = 0; t < n; ++t) {
            h ^= (uint32_t)s->lv[k].resident[t];
            h *= 16777619u;
        }
    }
    return h;
}

static size_t stream_resident_bytes(const TerrainStream* s) {
    size_t bytes = 0;
    for (int k = 0; k < s->level_count; ++k) {
        size_t wn = (size_t)s->lv[k].win_nodes * (size_t)s->lv[k].win_nodes;
        bytes += wn * (size_t)planes_at(k) * sizeof(float);
    }
    if (s->mask_res > 0)
        bytes += (size_t)s->mask_res * (size_t)s->mask_res * 3u * sizeof(float);
    return bytes;
}

void terrain_stream_probe(const TerrainStream* s, const TerrainParams* p, size_t frame,
                          bool final) {
    if (!s)
        return;
    printf("terrain-stream-probe frame=%zu levels=%d l0=%d,%d window_kb=%zu loaded=%llu "
           "ensured=%llu misses=%llu digest=%08x\n",
           frame, s->level_count, s->lv[0].win_x0, s->lv[0].win_z0,
           stream_resident_bytes(s) / 1024u, s->loaded, s->ensured, s->misses, stream_digest(s));
    if (!final)
        return;
    for (int k = 0; k < s->level_count; ++k) {
        const TerrainStreamLevel* L = &s->lv[k];
        int n = L->win_tiles * L->win_tiles, have = 0;
        for (int t = 0; t < n; ++t)
            have += L->resident[t] ? 1 : 0;
        printf("terrain-stream-probe level idx=%d res=%d window=%d tiles=%d resident=%d "
               "whole=%d\n",
               k, L->res, L->win_nodes, n, have, L->whole ? 1 : 0);
    }
    /*
     * The fallback rows, and they deliberately do NOT ensure: every other probe
     * here measures the stored data, where these measure the miss policy, and a
     * row that made its own point resident would be reporting on a query it had
     * just prevented.
     *
     * The pair is the point. `h` is what a caller gets asking for level 0 --
     * through the fall, when level 0 cannot serve it -- and `expect` is what
     * asking for the level the fall SETTLED on returns, which re-enters the
     * resolver at a level that contains and so never falls at all. Two routes
     * to one number, and they are equal only if the fall returns the coarse
     * level's own value: a miss answered with a zero, a clamp to the window or
     * the wrong level's plane all separate them.
     */
    if (!p)
        return;
    const int G = 4;
    for (int j = 0; j < G; ++j) {
        for (int i = 0; i < G; ++i) {
            float u = (float)(2 * i + 1) / (float)(2 * G);
            float v = (float)(2 * j + 1) / (float)(2 * G);
            float x = terrain_world_x(p, -p->extent + 2.0f * p->extent * u);
            float z = terrain_world_z(p, -p->extent + 2.0f * p->extent * v);
            const TerrainStreamLevel* L = &s->lv[0];
            int ni = (int)node_of(p, L->res, x, p->center[0]);
            int nj = (int)node_of(p, L->res, z, p->center[1]);
            int wtx = ni / s->tile_nodes - L->win_x0 / s->tile_nodes;
            int wtz = nj / s->tile_nodes - L->win_z0 / s->tile_nodes;
            int res = (wtx >= 0 && wtz >= 0 && wtx < L->win_tiles && wtz < L->win_tiles &&
                       L->resident[wtz * L->win_tiles + wtx])
                          ? 1
                          : 0;
            // The order is load-bearing: last_level is what the h read just
            // settled on, so expect has to be taken after it and before
            // anything else samples.
            float h = terrain_height_at(p, x, z);
            int settled = s->last_level;
            float expect = terrain_height_at_level(p, x, z, settled);
            printf("terrain-stream-probe fallback x=%.4f z=%.4f h=%.9g expect=%.9g level=%d "
                   "resident=%d\n",
                   (double)x, (double)z, (double)h, (double)expect, settled, res);
        }
    }
}
