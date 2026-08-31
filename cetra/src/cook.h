#ifndef _COOK_H_
#define _COOK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The derived-data cook (spec 11.99, roadmap F2): a transparent cache over
 * the heavy deterministic startup derivations, the UE DDC model. A call site builds
 * a key from its inputs, asks cook_fetch; a hit returns the stored sections
 * and the site installs them where the bake would have written; a miss falls
 * through to the live bake, whose result cook_store remembers for next time.
 *
 * THE KEY IS THE IDENTITY. It folds every input that determines the output,
 * plus a recipe version string bumped when the bake's code changes, plus a
 * library version where a library defines the byte format (Jolt's cooked
 * shapes). A stale artefact is therefore never detected -- it is UNFINDABLE,
 * because nothing computes its key any more. The only runtime check is
 * integrity of the file the key named, and a file failing it is refused by
 * name and treated as a miss. There is no verify-then-warn.
 *
 * Two rules for building keys. Never fold a struct by sizeof -- padding bytes
 * are indeterminate; fold fields through the typed calls, in declared order.
 * And never fold a worker count: bit-identity at any worker count is the bake
 * modules' own proven contract, and folding it would fracture the cache
 * across machines while contradicting the invariant it restates.
 *
 * This is the one sanctioned crossing of "two builds are not two runs": the
 * key never carries the build type, so an artefact cooked by one build may be
 * served to another. Where a producer's output bytes differ across builds
 * (tree_gen's floats move under -O2), so do the content bytes its consumers
 * fold, and each build addresses its own entries with no policy needed; where
 * they agree (the rock prototypes), sharing is what happens. Gates that
 * measure a LIVE bake pin themselves with cook disabled or a controlled dir.
 *
 * What may NOT be cooked: the material texture array (GPU resample,
 * scene-dependent layer assignment), sky/IBL/LTC GPU LUTs and captures,
 * anything holding GL handles, shader programs (Apple GL reports zero binary
 * formats), and anything whose producer is not a pure function of foldable
 * inputs -- the scatter stays live for that reason and for its gate story.
 *
 * Process-global behind cook_init/cook_shutdown -- the
 * texture_set_compression_enabled lever shape, because the wrapped sites span
 * five modules that share no context type. An app that never calls cook_init
 * behaves exactly as before: every fetch misses, every store no-ops.
 * MAIN-THREAD ONLY in v1; every wrapped site runs at startup on the main
 * thread, and the async loader's worker side is deliberately unwrapped.
 *
 * A 64-bit key collision within one recipe namespace would serve the wrong
 * payload silently. Accepted: at this corpus (hundreds of artefacts) the
 * probability is ~1e-14, below the disk-corruption floor the payload hash
 * already accepts, and the alternative is storing and comparing full inputs
 * -- the verify-then-warn design this module exists to reject.
 */

#define COOK_RECIPE_MAX   24 // "cluster-dag/1" and kin, NUL included
#define COOK_NAME_MAX     24 // the per-artefact label a recipe may carry
#define COOK_MAX_SECTIONS 20

// A running 64-bit FNV-1a fold plus the recipe naming its namespace. By value
// on the caller's stack; key building allocates nothing.
typedef struct CookKey {
    uint64_t hash;
    char recipe[COOK_RECIPE_MAX];
    char name[COOK_NAME_MAX]; // report label; empty until cook_key_name
    bool valid;               // false = recipe did not fit; fetch and store refuse
} CookKey;

// A key built while the cook is disabled or unconfigured comes back INVALID,
// and every fold on an invalid key is a no-op -- so a disabled run pays no
// hashing for fetches that would structurally refuse. Sites therefore build
// keys unconditionally and stay branch-free.
CookKey cook_key(const char* recipe);        // "erosion/1" -- name + VERSION, one string
void cook_key_u32(CookKey* key, uint32_t v); // folds 4 LE bytes
void cook_key_u64(CookKey* key, uint64_t v);
void cook_key_i32(CookKey* key, int32_t v);
void cook_key_f32(CookKey* key, float v);       // the IEEE bit pattern, never text
void cook_key_str(CookKey* key, const char* s); // folds the bytes INCLUDING the NUL
// Labels the artefact's report rows -- for a recipe stamped more than once
// per run ("grass", "r_2_3"). REPORTING ONLY, never folded: a label is not an
// input, and the first draft that folded it keyed the cluster DAGs by a
// build-order ordinal -- two byte-identical meshes cooked two artefacts and
// an inserted prototype re-keyed every later one. An input that happens to be
// a string folds through cook_key_str, deliberately spelled at the site.
void cook_key_label(CookKey* key, const char* name);
// Folds a u64 LE length prefix, then the bytes -- so no two distinct fold
// SEQUENCES of the same content can collide by concatenation.
void cook_key_bytes(CookKey* key, const void* data, size_t bytes);

// One fetched or stored section. Fetch fills each with an INDEPENDENT malloc,
// so a caller can install one as an owned buffer (a mesh's index array, a
// field's plane) without copying, and free() the rest.
typedef struct CookBlob {
    void* data;
    size_t size;
} CookBlob;

// dir NULL resolves CETRA_COOK_DIR, then "cooked" against the working
// directory (the repo root for every app in this tree). CETRA_NO_COOK=1
// disables regardless -- the lever for apps that grew no flags. Calling twice
// reconfigures; the counters reset.
void cook_init(const char* dir, bool enabled);
void cook_shutdown(void); // prints the cook-summary row if anything ran

// Hit: fills sections[0..section_count-1] and returns true, printing the
// artefact's result=hit row. ANY other outcome -- disabled, no file, failed
// integrity, wrong section count (a recipe that changed shape without a
// version bump; refused loudly for that reason) -- returns false with every
// section zeroed, and the caller bakes as today.
//
// The module validates the CONTAINER; section CONTENT is the caller's. A site
// whose section sizes are not derivable from its own folded inputs checks
// them and treats a mismatch as a miss -- with a log_warn naming the
// artefact, because a silent discard leaves the ledger showing hit-and-cooked
// for a site that is quietly re-baking every run.
bool cook_fetch(const CookKey* key, CookBlob* sections, int section_count);

// Write temp + rename, atomic; prints the result=cooked row. The return is
// for the ledger -- callers ignore it, because the bake's result is already
// live in memory and a failed store costs the NEXT run, not this one. A store
// failure warns once and disables further stores.
//
// Store BORROWS the sections, so it must run BEFORE any ownership transfer --
// four sites store a buffer and then hand it to texture_load_memory_owned,
// which frees it, and reversing that order is a use-after-free only the
// determinism arm can see, as corrupted bytes on disk.
bool cook_store(const CookKey* key, const CookBlob* sections, int section_count);

#endif // _COOK_H_
