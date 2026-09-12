#include "save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../animator.h"
#include "../ext/log.h"
#include "../json_util.h"
#include "../scene.h"
#include "../util.h"
#include "animator_component.h"
#include "character.h"
#include "entity.h"
#include "game.h"
#include "physics.h"

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

/*
 * What made one entity, and from what. The name is COPIED because an entity may
 * be destroyed while its record is still the only description of it; `params` is
 * owned here and deleted with the system.
 */
typedef struct SaveSpawnRecord {
    char name[64]; // matches Entity.name
    const char* spawner;
    cJSON* params;
} SaveSpawnRecord;

#define SAVE_MAX_MIGRATIONS 32

typedef struct SaveMigration {
    const char* section;
    int from_version;
    SaveMigrateFn fn;
} SaveMigration;

struct SaveSystem {
    Game* game; // borrowed
    SaveTable tables[SAVE_MAX_TABLES];
    int table_count;
    SaveSpawner spawners[SAVE_MAX_SPAWNERS];
    int spawner_count;
    SaveSpawnRecord* records;
    int record_count;
    int record_cap;
    SaveMigration migrations[SAVE_MAX_MIGRATIONS];
    int migration_count;
};

// Declared here so the entity walk can reach them; defined beside the
// registration functions they belong to, which are written further down.
static const SaveSpawner* _find_spawner(const SaveSystem* save, const char* name);
static SaveSpawnRecord* _find_record(const SaveSystem* save, const char* entity_name);
static bool _migrate_section(const SaveSystem* save, const char* section, cJSON* obj, int from,
                             int to, SaveLoadResult* r);

/*
 * A row's field, as void*. The offset arithmetic needs a byte pointer, and a
 * char*-to-float* cast is the one a portability checker reads as reinterpreting
 * unaligned bytes -- so the byte pointer never escapes these two functions.
 */
static void* _field_ptr(void* base, const SaveField* f) {
    if (f->addr)
        return f->addr;
    return (void*)((unsigned char*)base + f->offset);
}

static const void* _field_ptr_const(const void* base, const SaveField* f) {
    if (f->addr)
        return f->addr;
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
// where the row has one.
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
    }
}

static bool _write_field(cJSON* obj, const SaveField* f, const void* base) {
    if (!obj)
        return false;

    double v[4] = {0.0, 0.0, 0.0, 0.0};
    _load_value(f, base, v);

    switch ((SaveType)f->type) {
        case SAVE_BOOL:
            return cJSON_AddBoolToObject(obj, f->key, v[0] != 0.0) != NULL;
        // An int and a double are both exactly what cJSON's own printer writes;
        // only a float needs the narrower precision below.
        case SAVE_INT:
        case SAVE_DOUBLE:
            return cJSON_AddNumberToObject(obj, f->key, v[0]) != NULL;
        case SAVE_FLOAT:
            return json_add_float(obj, f->key, (float)v[0]);
        case SAVE_VEC3:
        case SAVE_QUAT: {
            const int n = _type_width((SaveType)f->type);
            cJSON* arr = cJSON_AddArrayToObject(obj, f->key);
            if (!arr)
                return false;
            for (int i = 0; i < n; i++) {
                // The same item a scalar float takes, so a position and a
                // height really are written alike rather than by two copies of
                // one rule.
                cJSON* item = json_float_item((float)v[i]);
                if (!item || !cJSON_AddItemToArray(arr, item)) {
                    cJSON_Delete(item);
                    return false;
                }
            }
            return true;
        }
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
    }
}

// One row against one already-located object. True when it stored something.
static bool _read_field(const cJSON* obj, const SaveField* f, void* base) {
    const cJSON* item = _find_item(obj, f);
    if (!item)
        return false;

    double v[4] = {0.0, 0.0, 0.0, 0.0};
    const int n = _decode_value(f, item, v);
    if (n == 0) {
        log_warn("save: '%s' is not a value of the right shape; ignored", f->key);
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

// ---------------------------------------------------------------- entities

/*
 * A body's live state, through the only entry points that reach it.
 *
 * The getters take a non-const RigidBody because asking Jolt is not a const
 * operation on its side; nothing here mutates the body. There is no
 * rigid_body_get_position at all -- a body's pose reaches C through
 * sync_physics_to_entities, which writes entity->position and entity->rotation
 * each step, so the pose is saved off the ENTITY rows below and pushed back
 * into the body on load.
 */
static void _get_body_linear(const void* base, double* out, int n) {
    vec3 v = {0.0f, 0.0f, 0.0f};
    rigid_body_get_linear_velocity((RigidBody*)base, v);
    for (int i = 0; i < n; i++)
        out[i] = v[i];
}

static void _set_body_linear(void* base, const double* v, int n) {
    vec3 out = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < n; i++)
        out[i] = (float)v[i];
    rigid_body_set_linear_velocity((RigidBody*)base, out);
}

static void _get_body_angular(const void* base, double* out, int n) {
    vec3 v = {0.0f, 0.0f, 0.0f};
    rigid_body_get_angular_velocity((RigidBody*)base, v);
    for (int i = 0; i < n; i++)
        out[i] = v[i];
}

static void _set_body_angular(void* base, const double* v, int n) {
    vec3 out = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < n; i++)
        out[i] = (float)v[i];
    rigid_body_set_angular_velocity((RigidBody*)base, out);
}

/*
 * A CharacterVirtual is not a body, so none of the above reaches it and
 * physics_world_shift_origin's warning applies here too: its pose has its own
 * entry points and nothing else moves it.
 */
static void _get_char_position(const void* base, double* out, int n) {
    vec3 v = {0.0f, 0.0f, 0.0f};
    character_controller_get_position((const CharacterController*)base, v);
    for (int i = 0; i < n; i++)
        out[i] = v[i];
}

static void _set_char_position(void* base, const double* v, int n) {
    vec3 out = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < n; i++)
        out[i] = (float)v[i];
    character_controller_set_position((CharacterController*)base, out);
}

static void _get_char_velocity(const void* base, double* out, int n) {
    vec3 v = {0.0f, 0.0f, 0.0f};
    character_controller_get_velocity((const CharacterController*)base, v);
    for (int i = 0; i < n; i++)
        out[i] = v[i];
}

static void _set_char_velocity(void* base, const double* v, int n) {
    vec3 out = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < n; i++)
        out[i] = (float)v[i];
    character_controller_set_velocity((CharacterController*)base, out);
}

/*
 * The entity's own state. `id` is deliberately absent: create_entity hands out
 * id = next_id++, and creation order depends on which content a run built, so
 * the same id names a different object between two runs of one binary. The NAME
 * is the identity, which is what every match below uses.
 */
static const SaveField SAVE_ENTITY_FIELDS[] = {
    SAVE_ROW(SAVE_BOOL, "active", Entity, active),
    SAVE_ROW(SAVE_VEC3, "position", Entity, position),
    SAVE_ROW(SAVE_QUAT, "rotation", Entity, rotation),
    SAVE_ROW(SAVE_VEC3, "scale", Entity, scale),
};
#define SAVE_ENTITY_COUNT ((int)(sizeof(SAVE_ENTITY_FIELDS) / sizeof(SAVE_ENTITY_FIELDS[0])))

static const SaveField SAVE_BODY_FIELDS[] = {
    SAVE_ROW_FN(SAVE_VEC3, "linear_velocity", _get_body_linear, _set_body_linear),
    SAVE_ROW_FN(SAVE_VEC3, "angular_velocity", _get_body_angular, _set_body_angular),
};
#define SAVE_BODY_COUNT ((int)(sizeof(SAVE_BODY_FIELDS) / sizeof(SAVE_BODY_FIELDS[0])))

static const SaveField SAVE_CHAR_FIELDS[] = {
    SAVE_ROW_FN(SAVE_VEC3, "position", _get_char_position, _set_char_position),
    SAVE_ROW_FN(SAVE_VEC3, "velocity", _get_char_velocity, _set_char_velocity),
};
#define SAVE_CHAR_COUNT ((int)(sizeof(SAVE_CHAR_FIELDS) / sizeof(SAVE_CHAR_FIELDS[0])))

/*
 * A component, keyed by a NAME and never by its ComponentType value.
 *
 * COMPONENT_BIT is 1u << type, so removing a member of that enum renumbers
 * every one after it and an old file's numbers would silently mean different
 * components. component_mask is a runtime query optimisation; it does not reach
 * disk. Keyed by name, the enum is free to be reordered or have members retired
 * and no save file notices.
 */
typedef struct SaveComponent {
    const char* key;
    const SaveField* rows;
    int count;
    void* (*of)(Entity* entity); // the component on this entity, or NULL
} SaveComponent;

static void* _body_of(Entity* entity) {
    return entity_get_rigid_body(entity);
}

static void* _character_of(Entity* entity) {
    return entity_get_character_controller(entity);
}

static const SaveComponent SAVE_COMPONENTS[] = {
    {"rigid_body", SAVE_BODY_FIELDS, SAVE_BODY_COUNT, _body_of},
    {"character", SAVE_CHAR_FIELDS, SAVE_CHAR_COUNT, _character_of},
};
#define SAVE_COMPONENT_COUNT ((int)(sizeof(SAVE_COMPONENTS) / sizeof(SAVE_COMPONENTS[0])))

#define SAVE_ENTITIES_VERSION 1

/*
 * The animator, by hand rather than through a table, and the reason is the one
 * thing a table of offsets cannot hold: what plays is named by a BORROWED
 * const char* -- a pointer to a clip's or a space's own name, not storage this
 * module could take an offset of or write into. config_snapshot.c hand-writes
 * its source block for exactly the same reason.
 *
 * What is saved is a SNAP: the source's name, its clock, and the three plain
 * settings. A save taken mid-crossfade therefore loads with the DESTINATION
 * playing, which is a frame of discontinuity on a load screen nobody watches,
 * against carrying both sources, the fade envelope, the resume source and the
 * layer's whole bone mask.
 */
static void _write_animator(cJSON* entry, const Animator* animator) {
    cJSON* obj = cJSON_AddObjectToObject(entry, "animator");
    if (!obj)
        return;
    const char* source = animator_source_name(animator);
    cJSON_AddStringToObject(obj, "source", source ? source : "");
    json_add_float(obj, "time", animator->base.time);
    cJSON_AddBoolToObject(obj, "looping", animator->base.looping);
    cJSON_AddBoolToObject(obj, "playing", animator->playing);
    json_add_float(obj, "speed", animator->speed);
    json_add_float(obj, "param", animator->param);
}

static bool _read_animator(const cJSON* entry, Animator* animator, Scene* scene) {
    const cJSON* obj = cJSON_GetObjectItemCaseSensitive(entry, "animator");
    if (!cJSON_IsObject(obj))
        return false;

    const char* source = json_string_or(obj, "source");
    const char* playing_now = animator_source_name(animator);

    /*
     * Only play something when the name DIFFERS from what is already playing.
     * The common case is that the app played its locomotion space in its own
     * init and the save names that same space, where re-playing would rebuild
     * a blend space this module does not have the entries for. A different
     * name can only be a single clip, which the scene can still find by name.
     */
    const cJSON* looping = cJSON_GetObjectItemCaseSensitive(obj, "looping");
    const bool loops = cJSON_IsTrue(looping);

    if (source && source[0] && (!playing_now || strcmp(source, playing_now) != 0)) {
        const Animation* clip = scene ? scene_find_animation(scene, source) : NULL;
        if (clip)
            animator_play(animator, clip, 0.0f, loops);
        else
            log_warn("save: no animation '%s' in this scene; the rig keeps what it plays", source);
    } else if (cJSON_IsBool(looping)) {
        // The COMMON path takes it too. Applied only by the re-play branch
        // above, `looping` was written every save and read back on none of the
        // loads that matter -- the app has usually played this very source in
        // its own init, which is the case the branch skips.
        animator->base.looping = loops;
    }

    const cJSON* time = cJSON_GetObjectItemCaseSensitive(obj, "time");
    if (cJSON_IsNumber(time))
        animator->base.time = (float)time->valuedouble;
    const cJSON* playing = cJSON_GetObjectItemCaseSensitive(obj, "playing");
    if (cJSON_IsBool(playing))
        animator->playing = cJSON_IsTrue(playing);
    const cJSON* speed = cJSON_GetObjectItemCaseSensitive(obj, "speed");
    if (cJSON_IsNumber(speed))
        animator->speed = (float)speed->valuedouble;
    const cJSON* param = cJSON_GetObjectItemCaseSensitive(obj, "param");
    if (cJSON_IsNumber(param))
        animator->param = (float)param->valuedouble;
    return true;
}

static bool _write_entities(SaveSystem* save, cJSON* root, int* written) {
    EntityManager* em = save->game ? save->game->entity_manager : NULL;
    if (!em)
        return true; // a game with no entities writes no section, rather than an empty one

    cJSON* section = _section(root, "entities", true);
    if (!section)
        return false;
    cJSON_AddNumberToObject(section, "version", SAVE_ENTITIES_VERSION);
    cJSON* list = cJSON_AddArrayToObject(section, "list");
    if (!list)
        return false;

    for (size_t e = 0; e < em->count; e++) {
        Entity* entity = em->entities[e];
        if (!entity)
            continue;
        cJSON* obj = cJSON_CreateObject();
        if (!obj || !cJSON_AddItemToArray(list, obj)) {
            cJSON_Delete(obj);
            return false;
        }
        // Both identities, the way config_snapshot.c writes its array elements:
        // the name is the stable key and the index is what an unnamed entry
        // would match on. Every Entity has a name, so the index is a diagnostic
        // rather than a fallback -- but writing it costs nothing and reading a
        // file by eye is easier for it.
        cJSON_AddStringToObject(obj, "name", entity->name);
        cJSON_AddNumberToObject(obj, "index", (double)e);

        /*
         * An entity nobody authored carries the recipe that made it. An authored
         * one does not: its record is a state overlay on something the scene
         * provides, so there is nothing to rebuild it from and nothing to write.
         */
        const SaveSpawnRecord* rec = _find_record(save, entity->name);
        if (rec) {
            cJSON_AddStringToObject(obj, "spawner", rec->spawner);
            if (rec->params) {
                cJSON* copy = cJSON_Duplicate(rec->params, true);
                if (!copy || !cJSON_AddItemToObject(obj, "params", copy)) {
                    cJSON_Delete(copy);
                    return false;
                }
            }
        }

        for (int i = 0; i < SAVE_ENTITY_COUNT; i++) {
            if (!_write_field(obj, &SAVE_ENTITY_FIELDS[i], entity))
                return false;
            (*written)++;
        }

        for (int c = 0; c < SAVE_COMPONENT_COUNT; c++) {
            const SaveComponent* comp = &SAVE_COMPONENTS[c];
            const void* base = comp->of(entity);
            if (!base)
                continue;
            cJSON* cobj = cJSON_AddObjectToObject(obj, comp->key);
            if (!cobj)
                return false;
            for (int i = 0; i < comp->count; i++) {
                if (!_write_field(cobj, &comp->rows[i], base))
                    return false;
                (*written)++;
            }
        }

        const Animator* animator = entity_get_animator(entity);
        if (animator)
            _write_animator(obj, animator);
    }
    return true;
}

static void _read_entities(SaveSystem* save, const cJSON* root, SaveLoadResult* r) {
    EntityManager* em = save->game ? save->game->entity_manager : NULL;
    if (!em)
        return;

    cJSON* section = _section((cJSON*)root, "entities", false);
    if (!section)
        return; // a file from a game with no entities carries no section
    if (!cJSON_IsObject(section)) {
        log_warn("save: 'entities' is not an object; no entity was restored");
        return;
    }

    // Migrated whole, before a single record is read: a step that rewrites how
    // entries are shaped has to see the list, not one entry at a time.
    const int was = json_int_or(section, "version", 1);
    if (!_migrate_section(save, "entities", section, was, SAVE_ENTITIES_VERSION, r))
        return;

    const cJSON* list = cJSON_GetObjectItemCaseSensitive(section, "list");
    if (!cJSON_IsArray(list)) {
        log_warn("save: 'entities.list' is not an array; no entity was restored");
        return;
    }

    const cJSON* entry = NULL;
    cJSON_ArrayForEach(entry, list) {
        const char* name = json_string_or(entry, "name");
        if (!name) {
            // The name IS the identity; a record without one addresses nothing.
            log_warn("save: an entity record carries no name; dropped");
            r->dropped_missing_entity++;
            continue;
        }

        Entity* entity = find_entity_by_name(em, name);
        if (!entity) {
            const char* spawner_name = json_string_or(entry, "spawner");
            if (!spawner_name) {
                /*
                 * An AUTHORED record whose target the scene no longer provides.
                 * Dropped and counted rather than refused: a patched level is
                 * the ordinary way this happens, and refusing the file would
                 * mean every content edit invalidated every save already in
                 * players' hands.
                 */
                r->dropped_missing_entity++;
                continue;
            }

            const SaveSpawner* spawner = _find_spawner(save, spawner_name);
            if (!spawner || !spawner->fn) {
                /*
                 * Either this build never knew the name, or it knows it as a
                 * TOMBSTONE -- a registration with no function, which is how a
                 * recipe is retired so that losing these entities is a decision
                 * somebody wrote down. Both drop one record; neither costs the
                 * file, because one unbuildable crate must not cost a player
                 * everything else in the save.
                 */
                log_warn("save: no spawner '%s' in this build; '%s' is not restored", spawner_name,
                         name);
                r->dropped_unknown_spawner++;
                continue;
            }

            const cJSON* params = cJSON_GetObjectItemCaseSensitive(entry, "params");
            // The NAME goes with the params. A recipe that invented its own
            // would build an entity the file no longer describes, and the next
            // save would write a different one -- so the identity the file
            // recorded is handed over rather than re-derived.
            entity = spawner->fn(em, name, params, spawner->user);
            if (!entity) {
                log_warn("save: spawner '%s' refused to rebuild '%s'", spawner_name, name);
                r->dropped_unknown_spawner++;
                continue;
            }

            // Re-noted so that saving again writes this entity out the same way
            // it came in; without it a load would quietly strip every spawned
            // thing from the NEXT save.
            save_note_spawn(save, entity->name, spawner->name,
                            params ? cJSON_Duplicate(params, true) : NULL);
            r->entities_spawned++;
        }

        for (int i = 0; i < SAVE_ENTITY_COUNT; i++) {
            if (_read_field(entry, &SAVE_ENTITY_FIELDS[i], entity))
                r->fields_restored++;
        }

        for (int c = 0; c < SAVE_COMPONENT_COUNT; c++) {
            const SaveComponent* comp = &SAVE_COMPONENTS[c];
            const cJSON* cobj = cJSON_GetObjectItemCaseSensitive(entry, comp->key);
            if (!cJSON_IsObject(cobj))
                continue;
            void* base = comp->of(entity);
            if (!base) {
                // The file carries a component this entity does not have -- the
                // same class of mismatch as a key this build no longer knows,
                // and counted with it.
                r->dropped_unknown_component++;
                continue;
            }
            for (int i = 0; i < comp->count; i++) {
                if (_read_field(cobj, &comp->rows[i], base))
                    r->fields_restored++;
            }
        }

        Animator* animator = entity_get_animator(entity);
        if (animator && _read_animator(entry, animator, save->game->scene))
            r->fields_restored++;

        /*
         * The pose has to be pushed into the BODY, not just stored on the
         * entity. entity->position and entity->rotation are a copy that
         * sync_physics_to_entities rewrites from Jolt every step, so a restore
         * that wrote only those would be overwritten before the next frame drew
         * -- the load would appear to do nothing at all.
         */
        RigidBody* body = entity_get_rigid_body(entity);
        if (body) {
            rigid_body_set_position(body, entity->position);
            rigid_body_set_rotation(body, entity->rotation);
            rigid_body_activate(body);
        }

        r->entities_restored++;
    }
}

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
    if (!save)
        return;
    // Tables and spawners are borrowed. The spawn records are not: their params
    // were handed over by save_note_spawn and this is where they end.
    for (int i = 0; i < save->record_count; i++)
        cJSON_Delete(save->records[i].params);
    free(save->records);
    free(save);
}

bool save_register_table(SaveSystem* save, const char* section, int version, const SaveField* rows,
                         int count, void* base) {
    if (!save || !section || !rows || count <= 0 || !base) {
        log_error("save: a table needs a section, rows and an object");
        return false;
    }
    /*
     * The entity array is written by this module under its own name, and it is
     * not a registered table -- so the duplicate check below cannot see it, and
     * without this an app could register rows over it. Both walks would then
     * write one object and the migration chain would run twice across it.
     * ("world" needs no such guard: it IS registered, first, so it trips the
     * duplicate check.)
     */
    if (strcmp(section, "entities") == 0) {
        log_error("save: the section name 'entities' is reserved for the entity array");
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

static const SaveSpawner* _find_spawner(const SaveSystem* save, const char* name) {
    for (int i = 0; i < save->spawner_count; i++) {
        if (strcmp(save->spawners[i].name, name) == 0)
            return &save->spawners[i];
    }
    return NULL;
}

static SaveSpawnRecord* _find_record(const SaveSystem* save, const char* entity_name) {
    for (int i = 0; i < save->record_count; i++) {
        if (strcmp(save->records[i].name, entity_name) == 0)
            return &save->records[i];
    }
    return NULL;
}

bool save_note_spawn(SaveSystem* save, const char* entity_name, const char* spawner,
                     cJSON* params) {
    if (!save || !entity_name || !entity_name[0] || !spawner || !spawner[0]) {
        cJSON_Delete(params); // owned on every path, refusals included
        log_error("save: a spawn record needs an entity name and a spawner");
        return false;
    }

    SaveSpawnRecord* rec = _find_record(save, entity_name);
    if (rec) {
        cJSON_Delete(rec->params);
        rec->spawner = spawner;
        rec->params = params;
        return true;
    }

    if (save->record_count == save->record_cap) {
        const int cap = save->record_cap ? save->record_cap * 2 : 32;
        SaveSpawnRecord* grown = realloc(save->records, (size_t)cap * sizeof(*grown));
        if (!grown) {
            cJSON_Delete(params);
            log_error("save: out of memory recording '%s'", entity_name);
            return false;
        }
        save->records = grown;
        save->record_cap = cap;
    }

    rec = &save->records[save->record_count++];
    snprintf(rec->name, sizeof(rec->name), "%s", entity_name);
    rec->spawner = spawner;
    rec->params = params;
    return true;
}

// -------------------------------------------------------------- migrations

bool save_register_migration(SaveSystem* save, const char* section, int from_version,
                             SaveMigrateFn fn) {
    if (!save || !section || !section[0] || !fn) {
        log_error("save: a migration needs a section and a function");
        return false;
    }
    for (int i = 0; i < save->migration_count; i++) {
        if (save->migrations[i].from_version == from_version &&
            strcmp(save->migrations[i].section, section) == 0) {
            log_error("save: '%s' already migrates from version %d", section, from_version);
            return false;
        }
    }
    if (save->migration_count >= SAVE_MAX_MIGRATIONS) {
        log_error("save: no room for a migration of '%s' (%d is the cap)", section,
                  SAVE_MAX_MIGRATIONS);
        return false;
    }
    save->migrations[save->migration_count++] =
        (SaveMigration){.section = section, .from_version = from_version, .fn = fn};
    return true;
}

/*
 * Bring one section's tree from the version the file wrote to the version this
 * build reads, one registered step at a time.
 *
 * A file NEWER than this build runs nothing at all -- `from` is already at or
 * past `to`, the loop does not execute, and what is left is the ordinary
 * name-keyed read where unknown keys are skipped and known ones land. That is
 * as far as forward compatibility goes here, and it is worth knowing it is not
 * free: a key whose meaning changed in the newer build reads wrong, silently,
 * because nothing in an older binary can know that happened.
 */
static bool _migrate_section(const SaveSystem* save, const char* section, cJSON* obj, int from,
                             int to, SaveLoadResult* r) {
    for (int v = from; v < to; v++) {
        const SaveMigration* step = NULL;
        for (int i = 0; i < save->migration_count && !step; i++) {
            if (save->migrations[i].from_version == v &&
                strcmp(save->migrations[i].section, section) == 0)
                step = &save->migrations[i];
        }
        if (!step) {
            log_warn("save: nothing migrates '%s' from version %d to %d; that section is left "
                     "as it was",
                     section, v, v + 1);
            return false;
        }
        if (!step->fn(obj)) {
            log_warn("save: migrating '%s' from version %d failed; that section is left as it was",
                     section, v);
            return false;
        }
        r->migrations_run++;
    }
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

    if (ok)
        ok = _write_entities(save, root, &written);

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
        cJSON* obj = _section(root, table->section, false);
        if (!obj || !cJSON_IsObject(obj))
            continue;
        // A section absent a version is version 1: the first format wrote one,
        // so this only covers a hand-written file, and guessing the oldest is
        // the reading that runs every migration rather than skipping any.
        const int was = json_int_or(obj, "version", 1);
        if (!_migrate_section(save, table->section, obj, was, table->version, &r))
            continue;
        for (int i = 0; i < table->count; i++) {
            if (_read_field(obj, &table->rows[i], table->base))
                r.fields_restored++;
        }
    }

    _read_entities(save, root, &r);

    cJSON_Delete(root);
    r.ok = true;
    log_info("save: read '%s' (version %d, %d fields, %d entities, %d dropped)", path,
             r.from_version, r.fields_restored, r.entities_restored,
             r.dropped_missing_entity + r.dropped_unknown_spawner + r.dropped_unknown_component);
    return r;
}
