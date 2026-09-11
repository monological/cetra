#ifndef _GAME_SETTINGS_H_
#define _GAME_SETTINGS_H_

#include <stdbool.h>
#include <stddef.h>

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

typedef enum {
    SETTINGS_WINDOW_WINDOWED = 0,
    SETTINGS_WINDOW_FULLSCREEN,
    SETTINGS_WINDOW_COUNT
} SettingsWindowMode;

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

    int window_mode; // SettingsWindowMode
    bool vsync;
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
