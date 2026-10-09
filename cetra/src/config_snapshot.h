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
    // --clouds. Here rather than as a table row because the noise bake is a
    // one-shot at startup gated on the layer being on, so "restore the clouds"
    // has to be answered before the engine exists; a row alone would store a
    // flag every consumer then refuses.
    bool clouds;
    // Texture compression (spec 11.85), here for the same reason clouds is: the
    // encode is a one-shot at LOAD, so a restore has to answer it before the
    // engine exists and a table row would store a flag every consumer then
    // refuses -- the textures are already uploaded by the time an apply runs.
    // Without these a --texture-compress-colour session does not round-trip, and
    // that is a measured RMSE 0.0063 on raiden rather than a theoretical one.
    bool no_texture_compression;
    bool texture_compress_colour;
    // --sky-scale (spec 13.7), 0 = not set. Here as well as in the sky.radiance_scale
    // row because it changes the sky's UNITS: a scene-captured probe is shot at load and
    // never again, so a scale that lands after it leaves every reflection of the sky
    // tens of thousands of times off, reproduced faithfully.
    float sky_scale;
    // Window size. Filled by the writer from the live engine and ignored on the
    // way in; a reader takes them from the parsed block. 0 = not recorded.
    int width, height;
} ConfigSnapshotSource;

// Record what was loaded. Call once, after arguments are resolved. Without it
// the `source` block is omitted and a snapshot cannot stand alone on a command
// line -- everything else in the file still dumps.
void config_snapshot_set_source(const ConfigSnapshotSource* src);

// Serialise the live engine + scene. Returns a malloc'd string the caller frees,
// or NULL. Sections whose owner is absent (no water, no sky, no probes) are
// omitted rather than written empty.
//
// `out_fields` receives how many values were actually WRITTEN, which is not the
// table's size on any scene missing a subsystem -- reporting the latter would
// say the same number whether or not the water block came out.
char* config_snapshot_write(struct Engine* engine, struct Scene* scene, int* out_fields);

// config_snapshot_write to a file. Reports the path it wrote on success and the
// reason on failure; both to stdout, so a headless run records what it produced.
bool config_snapshot_save(struct Engine* engine, struct Scene* scene, const char* path);

// Fold into `key` the settings a light capture of this scene reads (spec 13.42): the scene,
// shadow, sky, cloud, IBL, GI and rain settings, and every material's and decal's rows -- by the
// same table the writer walks, so a setting added to it is folded for free. Leaves out what a run
// decides rather than the scene: the camera, the post chain, the exposure and the engine's own
// section; and the lights, which scene_capture_fold folds whole. A key that could not be folded
// is left invalid.
struct CookKey;
void config_snapshot_fold(struct Engine* engine, struct Scene* scene, struct CookKey* key);

/*
 * Apply a snapshot to the live engine + scene. Returns the number of values
 * written, or -1 if the file could not be read or parsed.
 *
 * Run AFTER the model has loaded and after any scene-radius derivation: several
 * PostFX reaches are floored against the scene's own size at startup, so a
 * restore that lands before that block has its values raised back out from under
 * it. An unknown key warns and is skipped; a section whose subsystem this scene
 * does not have warns and is skipped -- a snapshot from a watery scene is still
 * a usable description of everything else in it.
 */
int config_snapshot_apply_file(struct Engine* engine, struct Scene* scene, const char* path);

/*
 * The `source` block alone, read without an engine.
 *
 * Separate because it is needed EARLIER than everything else: the model path has
 * to reach argument resolution before the window exists, where the rest of the
 * file describes objects that do not exist yet. `out` points into a buffer owned
 * here, valid until the next call.
 */
bool config_snapshot_read_source(const char* path, const ConfigSnapshotSource** out);

#endif // _CONFIG_SNAPSHOT_H_
