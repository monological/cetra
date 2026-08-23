#ifndef _CONFIG_SNAPSHOT_H_
#define _CONFIG_SNAPSHOT_H_

#include <stdbool.h>

/*
 * The whole live configuration, as JSON (spec 11.71).
 *
 * A `.cscn` says what EXISTS in the world -- the model, which lights there are,
 * material texture bindings, layer maps, roads, fog-volume placement. A snapshot
 * says how it is TUNED right now: every slider, checkbox, combo and the camera
 * pose. Neither contains the other, so a snapshot names the model or scene file
 * it was taken against in its `source` block and is applied on TOP of it.
 *
 * ONE descriptor table, walked in both directions. The writer and the reader
 * cannot list different fields, because there is only one list -- which is the
 * failure this module exists to prevent, and the one render.c's frame_schedule
 * comment records having lived with ("stood at two for three specs while the
 * body ran five").
 *
 * What the table deliberately omits is as load-bearing as what it carries: GPU
 * handles, the lazy-allocation guards, the per-frame PUBLISHED blocks (probe,
 * fog volumes, cloud shadow, water medium, fog casters, aerial) and the temporal
 * histories are not configuration. Restoring one corrupts the frame rather than
 * reproducing it -- a stale froxel_prev_frame makes reprojection read a volume
 * from a camera that no longer exists.
 */

struct Engine;
struct Scene;

// The half of a snapshot that is not live engine state: which content was
// loaded, and the process-level choices made before the engine existed.
//
// A module static rather than a writer parameter, because the GUI button is the
// primary producer and gui.c has no access to the app's parsed arguments -- the
// frame_schedule shape. Strings are copied, so nothing here outlives its buffer.
typedef struct ConfigSnapshotSource {
    const char* model;    // -m: a model path, or the .cscn that names one
    const char* hdr;      // -e
    const char* lut;      // --lut
    const char* textures; // -t
    bool sky;             // --sky: the environment is procedural, not a file
} ConfigSnapshotSource;

// Record what was loaded. Call once, after arguments are resolved. Without it
// the `source` block is omitted and a snapshot cannot stand alone on a command
// line -- everything else in the file still dumps.
void config_snapshot_set_source(const ConfigSnapshotSource* src);

// Serialise the live engine + scene. Returns a malloc'd string the caller frees,
// or NULL. Sections whose owner is absent (no water, no sky, no probes) are
// omitted rather than written empty.
char* config_snapshot_write(struct Engine* engine, struct Scene* scene);

// config_snapshot_write to a file. Reports the path it wrote on success and the
// reason on failure; both to stdout, so a headless run records what it produced.
bool config_snapshot_save(struct Engine* engine, struct Scene* scene, const char* path);

#endif // _CONFIG_SNAPSHOT_H_
