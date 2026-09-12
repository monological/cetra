#ifndef _GAME_SAVE_H_
#define _GAME_SAVE_H_

/*
 * Where the player WAS, persisted across runs (spec 12.3).
 *
 * Four files describe a session and none contains another. A .cscn says what
 * EXISTS in the world; a config snapshot says how the renderer is TUNED;
 * settings say what the player CHOSE; this says where they were. A save is
 * applied on TOP of a scene the app has already built -- it restores state onto
 * objects and does not author them, with one exception (save_register_spawner)
 * for entities that exist only because the player made them.
 *
 * The technique is config_snapshot.c's and settings.c's, for the third time:
 * ONE descriptor table walked in both directions, so the writer and the reader
 * cannot list different fields because there is only one list. Neither of those
 * tables could have carried this -- config_snapshot's owner enum has no Entity,
 * no RigidBody, no CharacterController and no Animator, and settings.c is what
 * a player chose rather than what they did.
 *
 * WHAT IT IS NOT: a deterministic replay. A restored world is NOT bit-exact,
 * and that is a decision rather than a shortfall. Jolt's contact caches, island
 * assignments and sleep timers stay out of the file, because serializing them
 * would pin the format to JPH_VERSION_ID (which physics_cook.h already exports
 * for exactly that reason) and break every existing save on a physics upgrade.
 * Poses and velocities are restored into a freshly built world and the solver
 * settles from there, so a crate may come to rest a fraction of a millimetre
 * from where it was. That drift is NOT yet bounded by a gate arm -- see "The
 * save paths" in docs/verification.md for what is owed and why the tolerance
 * has to be measured before it is quoted. Anyone measuring a difference and
 * filing it as corruption should read this first.
 *
 * WHY JSON, since the question comes up: a save is small, read once, on one
 * machine, by one binary. Protocol Buffers and Cap'n Proto each want a
 * host-side code generator on three platforms before the build starts, which is
 * what vendoring everything exists to avoid, and they sell zero-copy mapping
 * and cross-language compatibility that nothing here needs. What they get right
 * is tagged, name-keyed fields -- which a descriptor table already is. The axis
 * that actually matters is self-describing against packed: skipping an unknown
 * field in a packed stream means knowing how many bytes to jump, and getting
 * that wrong desynchronizes the rest of the file with no error at all. If this
 * ever wants to be binary the answer is CBOR, and the table, the versioning and
 * the drop policy below all survive that swap unchanged.
 */

#include <stdbool.h>
#include <stddef.h>

#include "../ext/cJSON.h"

struct Game;
struct Entity;
struct EntityManager;

typedef struct SaveSystem SaveSystem;

/*
 * The file this build writes, and the oldest it will read.
 *
 * A section carries its own version rather than the file carrying one for
 * everything: a single number forces every subsystem to agree on when to bump,
 * so the entity format could not change without claiming the app's had too.
 * SAVE_FLOOR is what makes old migrations deletable -- without a floor, every
 * migration ever written has to be kept forever.
 */
#define SAVE_FORMAT_VERSION 1
#define SAVE_FLOOR          1

// ------------------------------------------------------------------ fields

/*
 * Every type here has a live row. A string type and a named-enum type were
 * written first and carried no consumer -- and the dead arms were not free:
 * the string case in the decoder returned a success it had not written a value
 * for, which would have handed the store an uninitialised buffer the day
 * anything reached it. config_snapshot.c has both if they are wanted back, and
 * wants a consumer before they return.
 */
typedef enum {
    SAVE_BOOL = 0,
    SAVE_INT,
    SAVE_FLOAT,
    SAVE_DOUBLE,
    SAVE_VEC3,
    SAVE_QUAT, // a cglm versor: four floats, x y z w
} SaveType;

/*
 * A value that does not live at an offset.
 *
 * Most rows are a member of a struct and are read and written through the
 * offset. Some are not: a rigid body's velocity is inside Jolt, reachable only
 * through rigid_body_get_linear_velocity, and a character's position likewise.
 * config_snapshot.c has an apply hook for the way IN and nothing for the way
 * out, because a snapshot reads live fields directly. A save needs both
 * directions, so a row may carry a pair instead of an offset.
 *
 * `n` is how many doubles the row's type occupies: 1 for a scalar, 3 for a
 * vec3, 4 for a quat.
 */
typedef void (*SaveGetFn)(const void* base, double* out, int n);
typedef void (*SaveSetFn)(void* base, const double* v, int n);

typedef struct SaveField {
    unsigned char type;
    const char* key;
    /*
     * What this key used to be called, or NULL. The reader tries `key` first
     * and falls back to this, which is what makes a rename cost nothing: a
     * pure rename needs no migration function and no version bump, the way
     * Unity's FormerlySerializedAs works. It does NOT cover a change of
     * meaning -- same name, same type, different units -- which no tagged
     * format can detect and only a migration can repair.
     */
    const char* former_key;
    size_t offset;
    /*
     * A variable that is not a member of anything. Set, it IS the field's
     * address and `offset` is unused -- which is what lets an app put a table
     * over the file statics it already has, without gathering them into a
     * struct for the serializer's benefit or writing an accessor pair per
     * variable to reach them.
     */
    void* addr;
    SaveGetFn get; // non-NULL means neither offset nor addr is used
    SaveSetFn set;
} SaveField;

/*
 * Designated rather than positional, so adding a field to SaveField does not
 * mean editing every macro and the struct's field ORDER stops being
 * load-bearing. config_snapshot.c's positional CFG_ROW is not a precedent to
 * copy here: it predates this and amortises over three hundred rows.
 *
 * Note what is NOT borrowed from that file: its CFG_STRUCT token paste, which
 * welds an owner enum to a struct type so a row naming one owner with another's
 * member is a compile error. The hazard it guards cannot be written here --
 * SAVE_ROW names the struct and the member in ONE offsetof, so the compiler
 * checks the pairing itself.
 */
#define SAVE_ROW(type_, key_, struct_, member_) \
    {.type = (unsigned char)(type_), .key = (key_), .offset = offsetof(struct_, member_)}

// The same row, naming what the key used to be so old files still find it.
#define SAVE_ROW_WAS(type_, key_, former_, struct_, member_) \
    {.type = (unsigned char)(type_),                         \
     .key = (key_),                                          \
     .former_key = (former_),                                \
     .offset = offsetof(struct_, member_)}

// A plain variable, by address. `&x` is an address constant, so a table of
// these is still a static initialiser.
#define SAVE_ROW_AT(type_, key_, addr_) \
    {.type = (unsigned char)(type_), .key = (key_), .addr = (addr_)}

// A value reachable only through a function pair -- state that lives inside
// Jolt, or anywhere else this module cannot take the address of.
#define SAVE_ROW_FN(type_, key_, get_, set_) \
    {.type = (unsigned char)(type_), .key = (key_), .get = (get_), .set = (set_)}

// ------------------------------------------------------------------ result

/*
 * What a load did, rather than whether it worked.
 *
 * THE RULE THIS STRUCT EXISTS FOR: file-level problems refuse the file,
 * record-level problems drop the record and count it. A save naming an entity a
 * patched level no longer has, or a spawner this build no longer registers, is
 * the normal consequence of shipping an update -- refusing the whole file would
 * make a released game unpatchable, since every level edit would invalidate
 * every save in the wild. So those drop, and the counts come back here: a gate
 * arm asserts them exactly, and a game turns a non-zero one into "this save was
 * made with an earlier version; some items could not be restored", which is
 * what a player is owed and what silence denies them.
 *
 * Only two things set ok = false: the file will not parse, and its version is
 * below SAVE_FLOOR.
 */
typedef struct SaveLoadResult {
    bool ok;
    int from_version; // the file's own, before any migration ran
    int migrations_run;
    int fields_restored;
    int entities_restored;
    int entities_spawned;
    int dropped_missing_entity;
    int dropped_unknown_spawner;
    int dropped_unknown_component;
} SaveLoadResult;

// ------------------------------------------------------------------ system

// Borrows the game; the world section is registered over it here, so a save
// carries the sim clock and the pause flag without the app listing them.
SaveSystem* create_save_system(struct Game* game);
void free_save_system(SaveSystem* save);

/*
 * A table of the app's own state, under its own section name.
 *
 * The engine cannot walk to a facing angle held in a file static, or to the
 * counter that names the next spawned thing -- an entity walk reaches neither,
 * and a save without them restores a world whose player faces the wrong way.
 * So an app collects that state into one struct and registers a table over it,
 * exactly as this module registers its own.
 *
 * `rows` and `base` are BORROWED and must outlive the system. False (logged) if
 * the section is already registered or there is no room; a bounded array that
 * refuses is the convention, not a silent drop.
 */
bool save_register_table(SaveSystem* save, const char* section, int version, const SaveField* rows,
                         int count, void* base);

/*
 * How to rebuild an entity that exists only because the player made one.
 *
 * An authored entity's record is a state overlay on something the scene already
 * provides, so a missing one is dropped. A SPAWNED entity's record is the only
 * record of its existence, so it carries the name of a recipe and the arguments
 * to it, and the loader calls that recipe before applying the rest.
 *
 * A REGISTERED NAME IS A PERMANENT CONTRACT, the same rule as never reusing a
 * retired key: once a name has shipped it means one thing forever. Retiring a
 * recipe means registering a tombstone that returns NULL rather than deleting
 * the registration, so old saves lose those entities by a decision somebody
 * wrote down instead of by a name that silently stopped resolving.
 *
 * `params` is the object the writer produced for this entity; the function owns
 * nothing and must not retain it.
 */
typedef struct Entity* (*SaveSpawnFn)(struct EntityManager* em, const char* name,
                                      const cJSON* params, void* user);

bool save_register_spawner(SaveSystem* save, const char* name, SaveSpawnFn fn, void* user);

/*
 * Remember that `entity_name` was made by `spawner` from `params`.
 *
 * This is the half a registry alone cannot supply. Knowing HOW to rebuild a
 * crate says nothing about which crates exist or how big each one is -- in
 * gametest those come from rand01() and are then stored only inside a Material
 * and a Jolt shape, where nothing can read them back. So the arguments are
 * recorded here, at the moment of spawning, by the only code that has them.
 *
 * TAKES OWNERSHIP of `params` (which may be NULL), including when it refuses,
 * so a caller never has to unwind. Re-noting a name replaces its record.
 */
bool save_note_spawn(SaveSystem* save, const char* entity_name, const char* spawner, cJSON* params);

// -------------------------------------------------------------- migrations

/*
 * One step of a section's history: the tree as version N, rewritten in place to
 * be version N+1. False means the step could not be made, which skips the rest
 * of that section rather than costing the file.
 *
 * IT RUNS ON THE PARSED TREE, BEFORE THE DESCRIPTOR WALK, and that is the point
 * of the design rather than an implementation detail. A migration written today
 * must still behave identically in three years, when the struct it used to
 * correspond to has moved on -- so it is handed JSON and never a C object. This
 * is Minecraft's DataFixerUpper shape, and the reason Unreal reaches for
 * FCustomVersion rather than one engine-wide number.
 *
 * What a migration is FOR is the case tags cannot express: a key whose MEANING
 * changed -- same name, same type, different units. A rename is cheaper, and
 * wants a row's former_key and no function at all. A field added or removed
 * needs nothing whatsoever.
 */
typedef bool (*SaveMigrateFn)(cJSON* section);

/*
 * Register the step that takes `section` from `from_version` to from_version+1.
 * The chain is walked in order, so a file three versions behind runs three
 * functions. A MISSING step stops that section where it is: its rows are left
 * at their current values and the load says so by name, because half-migrated
 * data is worse than none.
 *
 * `section` is borrowed. False (logged) if that step is already registered or
 * there is no room.
 */
bool save_register_migration(SaveSystem* save, const char* section, int from_version,
                             SaveMigrateFn fn);

// ------------------------------------------------------------------- file

/*
 * The per-user file for `slot`, into `out` (a caller buffer of `cap` bytes).
 * Resolves the same platform location settings.json uses and honours
 * CETRA_SETTINGS_DIR, which is what keeps a gate hermetic. False, logged, when
 * the location cannot be resolved or created.
 */
bool save_default_path(char* out, size_t cap, const char* slot);

/*
 * Write every registered table. False, logged by name, on any failure.
 *
 * Written to a temporary beside the target and renamed over it, which is a
 * DELIBERATE departure from settings.c and config_snapshot.c -- both overwrite
 * in place, and both are right to, because a half-written renderer config costs
 * a re-dump. A half-written save costs a player their progress, so the old file
 * survives until the new one is complete on disk.
 */
bool save_write(SaveSystem* save, const char* path);

// Read a file over the live world. See SaveLoadResult for what a failure means
// and what merely counts.
SaveLoadResult save_read(SaveSystem* save, const char* path);

#endif // _GAME_SAVE_H_
