#include "save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ext/log.h"
#include "../json_util.h"
#include "../util.h"
#include "game.h"

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define SAVE_MKDIR(p)  _mkdir(p)
#define SAVE_FSYNC(fd) _commit(fd)
#define SAVE_FILENO(f) _fileno(f)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define SAVE_MKDIR(p)  mkdir((p), 0755)
#define SAVE_FSYNC(fd) fsync(fd)
#define SAVE_FILENO(f) fileno(f)
#endif

#define SAVE_APP_DIR      "cetra"
#define SAVE_MAX_TABLES   16
#define SAVE_MAX_SPAWNERS 32

/*
 * A registered table: rows, the object they address, and the version of THIS
 * section's format. Both pointers are borrowed from whoever registered them.
 */
typedef struct SaveTable {
    const char* section;
    int version;
    const SaveField* rows;
    int count;
    void* base;
} SaveTable;

typedef struct SaveSpawner {
    const char* name;
    SaveSpawnFn fn;
    void* user;
} SaveSpawner;

struct SaveSystem {
    Game* game; // borrowed
    SaveTable tables[SAVE_MAX_TABLES];
    int table_count;
    SaveSpawner spawners[SAVE_MAX_SPAWNERS];
    int spawner_count;
};

/*
 * A row's field, as void*. The offset arithmetic needs a byte pointer, and a
 * char*-to-float* cast is the one a portability checker reads as reinterpreting
 * unaligned bytes -- so the byte pointer never escapes these two functions.
 */
static void* _field_ptr(void* base, const SaveField* f) {
    return (void*)((unsigned char*)base + f->offset);
}

static const void* _field_ptr_const(const void* base, const SaveField* f) {
    return (const void*)((const unsigned char*)base + f->offset);
}

// How many doubles this type occupies in the decode buffer.
static int _type_width(SaveType type) {
    switch (type) {
        case SAVE_VEC3:
            return 3;
        case SAVE_QUAT:
            return 4;
        default:
            return 1;
    }
}

static const char* _enum_label(const SaveField* f, int value) {
    if (!f->labels || value < 0 || value >= f->label_count)
        return NULL;
    return f->labels[value];
}

static int _enum_value(const SaveField* f, const char* label) {
    for (int i = 0; i < f->label_count; i++) {
        if (f->labels[i] && strcmp(f->labels[i], label) == 0)
            return i;
    }
    return -1;
}

// --------------------------------------------------------------- sections

/*
 * One walker for both directions: the writer creates as it goes, the reader
 * must never create, because a missing section means the file did not carry it.
 */
static cJSON* _section(cJSON* root, const char* name, bool create) {
    cJSON* obj = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!obj && create)
        obj = cJSON_AddObjectToObject(root, name);
    return obj;
}

// ------------------------------------------------------------------ write

// Read one row out of `base` into up to four doubles, through the accessor pair
// where the row has one. Strings do not pass through here -- they are the one
// type with no numeric form.
static void _load_value(const SaveField* f, const void* base, double* v) {
    const int n = _type_width((SaveType)f->type);
    if (f->get) {
        f->get(base, v, n);
        return;
    }
    const void* p = _field_ptr_const(base, f);
    switch ((SaveType)f->type) {
        case SAVE_BOOL:
            v[0] = *(const bool*)p ? 1.0 : 0.0;
            break;
        case SAVE_INT:
        case SAVE_ENUM:
            v[0] = *(const int*)p;
            break;
        case SAVE_FLOAT:
            v[0] = *(const float*)p;
            break;
        case SAVE_DOUBLE:
            v[0] = *(const double*)p;
            break;
        case SAVE_VEC3:
        case SAVE_QUAT: {
            const float* fv = (const float*)p;
            for (int i = 0; i < n; i++)
                v[i] = fv[i];
            break;
        }
        case SAVE_STRING:
            break;
    }
}

static bool _write_field(cJSON* obj, const SaveField* f, const void* base) {
    if (!obj)
        return false;

    if ((SaveType)f->type == SAVE_STRING) {
        // An accessor row cannot be a string: there is no numeric buffer to
        // carry one, and no consumer has wanted it.
        const char* s = (const char*)_field_ptr_const(base, f);
        return cJSON_AddStringToObject(obj, f->key, s) != NULL;
    }

    double v[4] = {0.0, 0.0, 0.0, 0.0};
    _load_value(f, base, v);

    switch ((SaveType)f->type) {
        case SAVE_BOOL:
            return cJSON_AddBoolToObject(obj, f->key, v[0] != 0.0) != NULL;
        case SAVE_INT:
            return cJSON_AddNumberToObject(obj, f->key, v[0]) != NULL;
        case SAVE_FLOAT:
            return json_add_float(obj, f->key, (float)v[0]);
        case SAVE_DOUBLE:
            return cJSON_AddNumberToObject(obj, f->key, v[0]) != NULL;
        case SAVE_VEC3:
        case SAVE_QUAT: {
            const int n = _type_width((SaveType)f->type);
            cJSON* arr = cJSON_AddArrayToObject(obj, f->key);
            if (!arr)
                return false;
            for (int i = 0; i < n; i++) {
                // Through the same %.9g writer a scalar float takes, so a
                // position and a height read alike in the file.
                char text[32];
                snprintf(text, sizeof(text), "%.9g", v[i]);
                cJSON* item = cJSON_CreateRaw(text);
                if (!item || !cJSON_AddItemToArray(arr, item)) {
                    cJSON_Delete(item);
                    return false;
                }
            }
            return true;
        }
        case SAVE_ENUM: {
            const int value = (int)v[0];
            const char* label = _enum_label(f, value);
            // An out-of-range value is a bug upstream, not here: name it rather
            // than writing a label that would read back as something else.
            if (!label) {
                log_warn("save: %s holds unknown value %d", f->key, value);
                return cJSON_AddNumberToObject(obj, f->key, value) != NULL;
            }
            return cJSON_AddStringToObject(obj, f->key, label) != NULL;
        }
        case SAVE_STRING:
            break;
    }
    return false;
}

// ------------------------------------------------------------------- read

/*
 * The item this row should read, by current key or by what it used to be
 * called. NULL when the file carries neither, which is silence: a save may
 * legitimately predate a field.
 */
static const cJSON* _find_item(const cJSON* obj, const SaveField* f) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, f->key);
    if (!item && f->former_key)
        item = cJSON_GetObjectItemCaseSensitive(obj, f->former_key);
    return item;
}

/*
 * Decode one item into up to four doubles, in the shape the row wants. Returns
 * the count, or 0 when the item cannot be that shape -- refused BY NAME rather
 * than coerced, because a silently-zeroed field reads as a value somebody chose.
 */
static int _decode_value(const SaveField* f, const cJSON* item, double* out) {
    switch ((SaveType)f->type) {
        case SAVE_BOOL:
            if (!cJSON_IsBool(item))
                return 0;
            out[0] = cJSON_IsTrue(item) ? 1.0 : 0.0;
            return 1;
        case SAVE_INT:
        case SAVE_FLOAT:
        case SAVE_DOUBLE:
            if (!cJSON_IsNumber(item))
                return 0;
            out[0] = item->valuedouble;
            return 1;
        case SAVE_VEC3:
        case SAVE_QUAT: {
            const int want = _type_width((SaveType)f->type);
            if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != want)
                return 0;
            for (int i = 0; i < want; i++) {
                const cJSON* e = cJSON_GetArrayItem(item, i);
                if (!cJSON_IsNumber(e))
                    return 0;
                out[i] = e->valuedouble;
            }
            return want;
        }
        case SAVE_ENUM: {
            // A number is accepted as well as a label, so a hand-edited file
            // still loads; the writer only ever emits labels.
            if (cJSON_IsNumber(item)) {
                out[0] = item->valuedouble;
                return 1;
            }
            if (!cJSON_IsString(item) || !item->valuestring)
                return 0;
            const int value = _enum_value(f, item->valuestring);
            if (value < 0)
                return 0;
            out[0] = value;
            return 1;
        }
        case SAVE_STRING:
            return cJSON_IsString(item) ? 1 : 0;
    }
    return 0;
}

// Store a decoded value into the row's field. The mirror of _write_field's
// switch, and the reason both live here: they are one contract.
static void _store_value(const SaveField* f, void* base, const double* v, int n) {
    if (f->set) {
        f->set(base, v, n);
        return;
    }
    void* p = _field_ptr(base, f);
    switch ((SaveType)f->type) {
        case SAVE_BOOL:
            *(bool*)p = v[0] != 0.0;
            break;
        case SAVE_INT:
        case SAVE_ENUM:
            *(int*)p = (int)v[0];
            break;
        case SAVE_FLOAT:
            *(float*)p = (float)v[0];
            break;
        case SAVE_DOUBLE:
            *(double*)p = v[0];
            break;
        case SAVE_VEC3:
        case SAVE_QUAT: {
            float* dst = (float*)p;
            for (int i = 0; i < n; i++)
                dst[i] = (float)v[i];
            break;
        }
        case SAVE_STRING:
            break;
    }
}

// One row against one already-located object. True when it stored something.
static bool _read_field(const cJSON* obj, const SaveField* f, void* base) {
    const cJSON* item = _find_item(obj, f);
    if (!item)
        return false;

    if ((SaveType)f->type == SAVE_STRING) {
        if (!cJSON_IsString(item) || !item->valuestring || f->cap == 0) {
            log_warn("save: '%s' is not a string; ignored", f->key);
            return false;
        }
        char* dst = (char*)_field_ptr(base, f);
        snprintf(dst, f->cap, "%s", item->valuestring);
        return true;
    }

    double v[4] = {0.0, 0.0, 0.0, 0.0};
    const int n = _decode_value(f, item, v);
    if (n == 0) {
        log_warn("save: '%s' is not a %s; ignored", f->key,
                 (SaveType)f->type == SAVE_ENUM ? "known value" : "value of the right shape");
        return false;
    }
    _store_value(f, base, v, n);
    return true;
}

// ------------------------------------------------------------- world table

/*
 * The sim clock and the pause flag.
 *
 * `accumulator` is deliberately absent. It is the fraction of a fixed step the
 * loop has not yet run, so restoring it restores at most one step of sub-frame
 * phase and reproduces nothing a player could see -- while a stale one taken
 * against a different fixed_timestep would run a step the save never took.
 */
static const SaveField SAVE_WORLD_FIELDS[] = {
    SAVE_ROW(SAVE_DOUBLE, "time", Game, time),
    SAVE_ROW(SAVE_BOOL, "paused", Game, paused),
};
#define SAVE_WORLD_COUNT   ((int)(sizeof(SAVE_WORLD_FIELDS) / sizeof(SAVE_WORLD_FIELDS[0])))
#define SAVE_WORLD_VERSION 1

// ----------------------------------------------------------------- system

SaveSystem* create_save_system(Game* game) {
    if (!game) {
        log_error("save: no game");
        return NULL;
    }
    SaveSystem* save = calloc(1, sizeof(SaveSystem));
    if (!save) {
        log_error("save: out of memory");
        return NULL;
    }
    save->game = game;
    save_register_table(save, "world", SAVE_WORLD_VERSION, SAVE_WORLD_FIELDS, SAVE_WORLD_COUNT,
                        game);
    return save;
}

void free_save_system(SaveSystem* save) {
    // Every table and every spawner is borrowed; there is nothing else to free.
    free(save);
}

bool save_register_table(SaveSystem* save, const char* section, int version, const SaveField* rows,
                         int count, void* base) {
    if (!save || !section || !rows || count <= 0 || !base) {
        log_error("save: a table needs a section, rows and an object");
        return false;
    }
    for (int i = 0; i < save->table_count; i++) {
        if (strcmp(save->tables[i].section, section) == 0) {
            log_error("save: section '%s' is already registered", section);
            return false;
        }
    }
    if (save->table_count >= SAVE_MAX_TABLES) {
        log_error("save: no room for section '%s' (%d tables is the cap)", section,
                  SAVE_MAX_TABLES);
        return false;
    }
    save->tables[save->table_count++] = (SaveTable){
        .section = section, .version = version, .rows = rows, .count = count, .base = base};
    return true;
}

bool save_register_spawner(SaveSystem* save, const char* name, SaveSpawnFn fn, void* user) {
    if (!save || !name || !name[0]) {
        log_error("save: a spawner needs a name");
        return false;
    }
    for (int i = 0; i < save->spawner_count; i++) {
        if (strcmp(save->spawners[i].name, name) == 0) {
            log_error("save: spawner '%s' is already registered", name);
            return false;
        }
    }
    if (save->spawner_count >= SAVE_MAX_SPAWNERS) {
        log_error("save: no room for spawner '%s' (%d is the cap)", name, SAVE_MAX_SPAWNERS);
        return false;
    }
    // fn may be NULL on purpose: that is the tombstone a retired recipe leaves,
    // so the name still resolves and the entity is dropped by a decision
    // rather than by a lookup that silently stopped matching.
    save->spawners[save->spawner_count++] = (SaveSpawner){.name = name, .fn = fn, .user = user};
    return true;
}

// ------------------------------------------------------------------- path

bool save_default_path(char* out, size_t cap, const char* slot) {
    if (!out || cap == 0)
        return false;
    if (!slot || !slot[0])
        slot = "save";

    char dir[1024];
    const char* override_dir = getenv("CETRA_SETTINGS_DIR");
    if (override_dir && override_dir[0]) {
        snprintf(dir, sizeof(dir), "%s", override_dir);
    } else {
#if defined(_WIN32)
        const char* base = getenv("APPDATA");
        if (!base || !base[0]) {
            log_error("save: APPDATA is not set; cannot locate a per-user directory");
            return false;
        }
        snprintf(dir, sizeof(dir), "%s\\%s", base, SAVE_APP_DIR);
#elif defined(__APPLE__)
        const char* home = getenv("HOME");
        if (!home || !home[0]) {
            log_error("save: HOME is not set; cannot locate a per-user directory");
            return false;
        }
        snprintf(dir, sizeof(dir), "%s/Library/Application Support/%s", home, SAVE_APP_DIR);
#else
        const char* xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && xdg[0]) {
            snprintf(dir, sizeof(dir), "%s/%s", xdg, SAVE_APP_DIR);
        } else {
            const char* home = getenv("HOME");
            if (!home || !home[0]) {
                log_error("save: neither XDG_CONFIG_HOME nor HOME is set");
                return false;
            }
            snprintf(dir, sizeof(dir), "%s/.config/%s", home, SAVE_APP_DIR);
        }
#endif
    }

    // An existing directory is a success, not a failure -- EEXIST is the common
    // case after the first save.
    if (SAVE_MKDIR(dir) != 0 && !path_exists(dir)) {
        log_error("save: cannot create '%s'", dir);
        return false;
    }

    const int n = snprintf(out, cap, "%s/%s.json", dir, slot);
    if (n < 0 || (size_t)n >= cap) {
        log_error("save: path is longer than the %zu bytes the caller offered", cap);
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ write

/*
 * Text to `path`, through a temporary renamed over it.
 *
 * The old file survives until the new one is complete and flushed, so a crash
 * or a pulled plug costs the newest save and not the only one. rename() is
 * atomic on every platform this builds for when both paths are on one
 * filesystem, which a temporary beside the target always is.
 */
static bool _write_atomic(const char* path, const char* text) {
    char tmp[1200];
    const int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        log_error("save: '%s' is too long to write beside", path);
        return false;
    }

    FILE* f = fopen(tmp, "wb");
    if (!f) {
        log_error("save: cannot write '%s'", tmp);
        return false;
    }
    const size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len && fputc('\n', f) != EOF;
    if (ok)
        ok = fflush(f) == 0;
    if (ok)
        ok = SAVE_FSYNC(SAVE_FILENO(f)) == 0;
    if (fclose(f) != 0)
        ok = false;

    if (!ok) {
        log_error("save: short write to '%s'; '%s' is unchanged", tmp, path);
        remove(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        log_error("save: cannot move '%s' onto '%s'", tmp, path);
        remove(tmp);
        return false;
    }
    return true;
}

bool save_write(SaveSystem* save, const char* path) {
    if (!save || !path) {
        log_error("save: nothing to write, or nowhere to write it");
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root)
        return false;
    cJSON_AddNumberToObject(root, "version", SAVE_FORMAT_VERSION);

    bool ok = true;
    int written = 0;
    for (int t = 0; ok && t < save->table_count; t++) {
        const SaveTable* table = &save->tables[t];
        cJSON* obj = _section(root, table->section, true);
        if (!obj) {
            ok = false;
            break;
        }
        // The section's own version, beside its rows rather than at the root: a
        // file-wide number would force every section to bump together.
        cJSON_AddNumberToObject(obj, "version", table->version);
        for (int i = 0; ok && i < table->count; i++) {
            ok = _write_field(obj, &table->rows[i], table->base);
            if (ok)
                written++;
        }
    }

    if (!ok) {
        cJSON_Delete(root);
        log_error("save: could not serialise");
        return false;
    }

    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) {
        log_error("save: could not serialise");
        return false;
    }

    ok = _write_atomic(path, text);
    free(text);
    if (ok)
        log_info("save: wrote '%s' (%d fields)", path, written);
    return ok;
}

// ------------------------------------------------------------------- read

SaveLoadResult save_read(SaveSystem* save, const char* path) {
    SaveLoadResult r = {0};
    if (!save || !path) {
        log_error("save: nothing to read into, or nothing to read");
        return r;
    }

    char* text = read_entire_file(path, NULL);
    if (!text) {
        log_error("save: cannot read '%s'", path);
        return r;
    }
    cJSON* root = cJSON_Parse(text);
    free(text);
    if (!root) {
        log_error("save: '%s' will not parse; nothing applied", path);
        return r;
    }

    r.from_version = json_int_or(root, "version", 0);
    if (r.from_version < SAVE_FLOOR) {
        // One of exactly two file-level refusals. Everything else in this
        // function drops a record and counts it.
        log_error("save: '%s' is version %d; this build reads %d and newer, nothing applied", path,
                  r.from_version, SAVE_FLOOR);
        cJSON_Delete(root);
        return r;
    }

    for (int t = 0; t < save->table_count; t++) {
        const SaveTable* table = &save->tables[t];
        const cJSON* obj = _section(root, table->section, false);
        if (!obj || !cJSON_IsObject(obj))
            continue;
        for (int i = 0; i < table->count; i++) {
            if (_read_field(obj, &table->rows[i], table->base))
                r.fields_restored++;
        }
    }

    cJSON_Delete(root);
    r.ok = true;
    log_info("save: read '%s' (version %d, %d fields)", path, r.from_version, r.fields_restored);
    return r;
}
