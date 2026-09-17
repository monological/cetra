#ifndef _GAME_SETTINGS_H_
#define _GAME_SETTINGS_H_

#include <stdbool.h>
#include <stddef.h>

#include "../engine.h"

/*
 * What a PLAYER chose, persisted across runs (spec 12.2).
 *
 * This is not config_snapshot.c and could not be. That file is a renderer
 * tuning snapshot: its owners are Engine, PostFX, Exposure, Scene, Camera,
 * Shadow, Sky, Cloud, IBL, GI, LightCluster and Water, and there is no Game, no
 * AudioSystem and no input among them -- so there is nowhere in its table for a
 * volume or a window mode to live, and no CFG_STRUCT entry they could hang
 * from. What is shared is the TECHNIQUE, deliberately: one descriptor table
 * walked in both directions, so the writer and the reader cannot list different
 * fields because there is only one list.
 *
 * WHERE IT GOES. settings_default_path resolves the platform's own per-user
 * location -- %APPDATA% on Windows, ~/Library/Application Support on macOS,
 * $XDG_CONFIG_HOME or ~/.config elsewhere -- and creates the directory. Nothing
 * else in this engine writes anywhere but the working directory, which is fine
 * for a screenshot and wrong for something a player expects to survive.
 * CETRA_SETTINGS_DIR overrides it, which is what keeps a test hermetic.
 *
 * WHAT IT DOES NOT CARRY, stated so the omission is a decision rather than an
 * oversight: input bindings. input_bind takes a BORROWED const table the app
 * owns, so persisting a rebinding means owning mutable storage and re-binding
 * after load -- a bigger change than the values here, and one that wants the
 * remapping UI it would exist for. Owed, not forgotten.
 */

// The longest display name carried. GLFW's are short ("Built-in Retina
// Display"); a longer one is truncated at save rather than refused, since a
// truncated name simply fails to match and falls back to the primary monitor.
#define SETTINGS_NAME_CAP 64

/*
 * Every field's value at zero is NOT its default -- a zeroed GameSettings is
 * silent, not unity. settings_defaults fills it, and load starts from defaults
 * so a file that omits a key keeps the default rather than inheriting a zero.
 */
typedef struct GameSettings {
    // Linear gain, 1 = unity, matching audio_set_bus_volume's units exactly so
    // no conversion sits between the file and the mixer.
    float master_volume;
    float music_volume;
    float sfx_volume;
    float ui_volume;

    // EngineWindowMode (engine.h); 0 = windowed. Held as an int, not as the
    // enum, because the descriptor table reaches this through an int* and an
    // enum's compatible integer type is implementation-defined -- the one
    // portability question this file cannot answer for three platforms.
    int window_mode;
    bool vsync;
    // The display a non-windowed mode goes to, by NAME. Empty = the primary,
    // which is also what an unrecognised name resolves to -- an INDEX would
    // renumber when a monitor is unplugged and silently move the game to a
    // different screen than the one that was chosen.
    char monitor[SETTINGS_NAME_CAP];

    /*
     * The camera, as a PLAYER states it (spec 12.19).
     *
     * FOV is here as well as in the config snapshot, and the two are not the
     * same question: the snapshot is a renderer tuning value for reproducing a
     * frame, this is what somebody chose in a menu. The precedence is stated so
     * it is not discovered -- defaults, then this file, then the CLI, then a
     * snapshot -- which is the chain a .cscn already sits in.
     */
    float fov_degrees;      // vertical; 0 = leave the app's own
    float look_sensitivity; // multiplies a rig's turn rates; 1 = as authored
    bool invert_look_y;     // up on the stick looks down
    // Motion reduction: camera shake off. The camera half of the roadmap's
    // accessibility row; the post stack's motion blur is NOT covered by this and
    // is named as still open.
    bool reduce_motion;
} GameSettings;

// Unity volumes, windowed, vsync on.
void settings_defaults(GameSettings* out);

/*
 * The per-user file this build reads and writes, into `out` (a caller buffer of
 * `cap` bytes). Creates the directory if it is missing. False, logged, when the
 * platform location cannot be resolved or made -- a caller that ignores the
 * result and writes anyway would put a player's settings in the working
 * directory, which is the bug this exists to avoid.
 */
bool settings_default_path(char* out, size_t cap);

// Read and write. Both return false and log by name on failure; a load failure
// leaves `out` at defaults rather than half-populated, so a truncated or
// hand-edited file costs a player their settings and not their game.
bool settings_load(GameSettings* out, const char* path);
bool settings_save(const GameSettings* settings, const char* path);

// Push every value where it actually takes effect. Separate from load because
// loading is reading a file and applying is touching live subsystems, and a
// caller restoring defaults mid-session wants the second without the first.
struct AudioSystem;
struct Engine;
void settings_apply(const GameSettings* settings, struct AudioSystem* audio, struct Engine* engine);

#endif // _GAME_SETTINGS_H_
