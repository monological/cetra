#include "settings.h"
#include "../camera_rig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../engine.h"
#include "../ext/cJSON.h"
#include "../ext/log.h"
#include "../util.h"
#include "audio.h"

#if defined(_WIN32)
#include <direct.h>
#define SETTINGS_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define SETTINGS_MKDIR(p) mkdir((p), 0755)
#endif

/*
 * ONE table, walked by both the writer and the reader.
 *
 * The technique is config_snapshot.c's and the reason is the same: a writer and
 * a reader maintained separately drift, and the drift is silent -- a key that
 * one saves and the other never looks for simply stops persisting, with nothing
 * to fail.
 *
 * What is NOT borrowed is that file's CFG_STRUCT token paste, which exists to
 * make an owner/member mismatch a compile error. There is one owner here, so
 * the mismatch it guards against cannot be written.
 */
typedef enum { SET_FLOAT, SET_INT, SET_BOOL, SET_STRING } SettingType;

typedef struct SettingField {
    SettingType type;
    const char* section; // JSON object this key lives under
    const char* key;
    size_t offset;
    const char* const* labels; // an int written as a NAME rather than a number
    int label_count;
    size_t cap; // SET_STRING only: the member's size, so the copy cannot run off it
} SettingField;

#define SET_ROW(type_, section_, key_, member_) \
    {type_, section_, key_, offsetof(GameSettings, member_), NULL, 0, 0}
#define SET_ROW_ENUM(section_, key_, member_, labels_)       \
    {SET_INT, section_,                                      \
     key_,    offsetof(GameSettings, member_),               \
     labels_, (int)(sizeof(labels_) / sizeof((labels_)[0])), \
     0}
#define SET_ROW_STRING(section_, key_, member_) \
    {SET_STRING,                                \
     section_,                                  \
     key_,                                      \
     offsetof(GameSettings, member_),           \
     NULL,                                      \
     0,                                         \
     sizeof(((GameSettings*)0)->member_)}

// Index IS the enum value, which is why EngineWindowMode may be appended to and
// never inserted into. This assert is what makes a mode added there without a
// label here a compile error rather than a number in a player's file.
static const char* const SETTINGS_WINDOW_MODES[] = {"windowed", "fullscreen", "borderless"};
_Static_assert(sizeof(SETTINGS_WINDOW_MODES) / sizeof(*SETTINGS_WINDOW_MODES) ==
                   ENGINE_WINDOW_MODE_COUNT,
               "SETTINGS_WINDOW_MODES must name every EngineWindowMode");

static const SettingField SETTINGS_FIELDS[] = {
    SET_ROW(SET_FLOAT, "audio", "master", master_volume),
    SET_ROW(SET_FLOAT, "audio", "music", music_volume),
    SET_ROW(SET_FLOAT, "audio", "sfx", sfx_volume),
    SET_ROW(SET_FLOAT, "audio", "ui", ui_volume),
    SET_ROW_ENUM("window", "mode", window_mode, SETTINGS_WINDOW_MODES),
    SET_ROW(SET_BOOL, "window", "vsync", vsync),
    SET_ROW_STRING("window", "monitor", monitor),
    SET_ROW(SET_FLOAT, "camera", "fov_degrees", fov_degrees),
    SET_ROW(SET_FLOAT, "camera", "look_sensitivity", look_sensitivity),
    SET_ROW(SET_BOOL, "camera", "invert_look_y", invert_look_y),
    SET_ROW(SET_BOOL, "camera", "reduce_motion", reduce_motion),
};
#define SETTINGS_FIELD_COUNT (sizeof(SETTINGS_FIELDS) / sizeof(SETTINGS_FIELDS[0]))

/*
 * A row's field, as void*. The offset arithmetic needs a byte pointer, and a
 * char*-to-float* cast is the one a portability checker reads as reinterpreting
 * unaligned bytes -- so the byte pointer never escapes these two functions.
 */
static void* _field_ptr(void* base, const SettingField* f) {
    return (void*)((unsigned char*)base + f->offset);
}

static const void* _field_ptr_const(const void* base, const SettingField* f) {
    return (const void*)((const unsigned char*)base + f->offset);
}

#define SETTINGS_VERSION 1
#define SETTINGS_APP_DIR "cetra"
#define SETTINGS_FILE    "settings.json"

void settings_defaults(GameSettings* out) {
    if (!out)
        return;
    *out = (GameSettings){.master_volume = 1.0f,
                          .music_volume = 1.0f,
                          .sfx_volume = 1.0f,
                          .ui_volume = 1.0f,
                          .window_mode = ENGINE_WINDOW_WINDOWED,
                          .vsync = true,
                          .monitor = "", // empty = primary, which is the first run's answer
                          // 0 leaves whatever the app framed with: a default FOV
                          // here would override every app's own choice on a
                          // first run, which is the opposite of a preference.
                          .fov_degrees = 0.0f,
                          .look_sensitivity = 1.0f,
                          .invert_look_y = false,
                          .reduce_motion = false};
}

// ------------------------------------------------------------------- path

bool settings_default_path(char* out, size_t cap) {
    if (!out || cap == 0)
        return false;

    char dir[1024];
    const char* override_dir = getenv("CETRA_SETTINGS_DIR");
    if (override_dir && override_dir[0]) {
        snprintf(dir, sizeof(dir), "%s", override_dir);
    } else {
#if defined(_WIN32)
        const char* base = getenv("APPDATA");
        if (!base || !base[0]) {
            log_error("settings: APPDATA is not set; cannot locate a per-user directory");
            return false;
        }
        snprintf(dir, sizeof(dir), "%s\\%s", base, SETTINGS_APP_DIR);
#elif defined(__APPLE__)
        const char* home = getenv("HOME");
        if (!home || !home[0]) {
            log_error("settings: HOME is not set; cannot locate a per-user directory");
            return false;
        }
        snprintf(dir, sizeof(dir), "%s/Library/Application Support/%s", home, SETTINGS_APP_DIR);
#else
        const char* xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && xdg[0]) {
            snprintf(dir, sizeof(dir), "%s/%s", xdg, SETTINGS_APP_DIR);
        } else {
            const char* home = getenv("HOME");
            if (!home || !home[0]) {
                log_error("settings: neither XDG_CONFIG_HOME nor HOME is set");
                return false;
            }
            snprintf(dir, sizeof(dir), "%s/.config/%s", home, SETTINGS_APP_DIR);
        }
#endif
    }

    // An existing directory is a success, not a failure: EEXIST is the common
    // case after the first run, and treating it as an error would mean settings
    // saved once and never again.
    if (SETTINGS_MKDIR(dir) != 0 && !path_exists(dir)) {
        log_error("settings: cannot create '%s'", dir);
        return false;
    }

    const int n = snprintf(out, cap, "%s/%s", dir, SETTINGS_FILE);
    if (n < 0 || (size_t)n >= cap) {
        log_error("settings: path is longer than the %zu bytes the caller offered", cap);
        return false;
    }
    return true;
}

// ------------------------------------------------------------------- json

static cJSON* _section(cJSON* root, const char* name, bool create) {
    cJSON* obj = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!obj && create) {
        obj = cJSON_CreateObject();
        if (obj)
            cJSON_AddItemToObject(root, name, obj);
    }
    return obj;
}

bool settings_save(const GameSettings* settings, const char* path) {
    if (!settings || !path)
        return false;

    cJSON* root = cJSON_CreateObject();
    if (!root)
        return false;
    cJSON_AddNumberToObject(root, "version", SETTINGS_VERSION);

    for (size_t i = 0; i < SETTINGS_FIELD_COUNT; i++) {
        const SettingField* f = &SETTINGS_FIELDS[i];
        cJSON* obj = _section(root, f->section, true);
        if (!obj)
            continue;
        const void* base = _field_ptr_const(settings, f);
        switch (f->type) {
            case SET_FLOAT:
                cJSON_AddNumberToObject(obj, f->key, (double)*(const float*)base);
                break;
            case SET_BOOL:
                cJSON_AddBoolToObject(obj, f->key, *(const bool*)base);
                break;
            case SET_INT: {
                const int v = *(const int*)base;
                if (f->labels && v >= 0 && v < f->label_count)
                    cJSON_AddStringToObject(obj, f->key, f->labels[v]);
                else
                    cJSON_AddNumberToObject(obj, f->key, (double)v);
                break;
            }
            case SET_STRING:
                cJSON_AddStringToObject(obj, f->key, (const char*)base);
                break;
        }
    }

    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) {
        log_error("settings: could not serialise");
        return false;
    }

    FILE* file = fopen(path, "wb");
    if (!file) {
        log_error("settings: cannot write '%s'", path);
        free(text);
        return false;
    }
    fputs(text, file);
    fclose(file);
    free(text);
    log_info("settings saved to '%s'", path);
    return true;
}

bool settings_load(GameSettings* out, const char* path) {
    if (!out || !path)
        return false;

    // Defaults FIRST, so a file that omits a key keeps the default rather than
    // inheriting whatever was in the caller's struct -- and so a failure below
    // leaves a usable settings object rather than a half-populated one.
    settings_defaults(out);

    char* text = read_entire_file(path, NULL);
    if (!text)
        return false; // absent is not an error: the first run has no file

    cJSON* root = cJSON_Parse(text);
    free(text);
    if (!root) {
        log_error("settings: '%s' will not parse; defaults kept", path);
        return false;
    }

    const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsNumber(version) && version->valueint != SETTINGS_VERSION) {
        log_info("settings: '%s' is version %d, this build writes %d; unknown keys are ignored",
                 path, version->valueint, SETTINGS_VERSION);
    }

    for (size_t i = 0; i < SETTINGS_FIELD_COUNT; i++) {
        const SettingField* f = &SETTINGS_FIELDS[i];
        const cJSON* obj = _section(root, f->section, false);
        if (!obj)
            continue;
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, f->key);
        if (!item)
            continue;
        void* base = _field_ptr(out, f);
        switch (f->type) {
            case SET_FLOAT:
                if (cJSON_IsNumber(item))
                    *(float*)base = (float)item->valuedouble;
                break;
            case SET_BOOL:
                if (cJSON_IsBool(item))
                    *(bool*)base = cJSON_IsTrue(item);
                break;
            case SET_INT:
                if (cJSON_IsString(item) && f->labels) {
                    for (int l = 0; l < f->label_count; l++) {
                        if (strcmp(item->valuestring, f->labels[l]) == 0) {
                            *(int*)base = l;
                            break;
                        }
                    }
                } else if (cJSON_IsNumber(item)) {
                    *(int*)base = item->valueint;
                }
                break;
            case SET_STRING:
                // Truncated rather than refused: an over-long name is one no
                // display answers to, which already resolves to the primary
                // monitor, so the worst case is the fallback the feature has.
                if (cJSON_IsString(item) && item->valuestring && f->cap > 0) {
                    snprintf((char*)base, f->cap, "%s", item->valuestring);
                }
                break;
        }
    }

    cJSON_Delete(root);
    return true;
}

// ------------------------------------------------------------------ apply

void settings_apply(const GameSettings* settings, AudioSystem* audio, Engine* engine) {
    if (!settings)
        return;

    if (audio) {
        audio_set_bus_volume(audio, AUDIO_BUS_MASTER, settings->master_volume);
        audio_set_bus_volume(audio, AUDIO_BUS_MUSIC, settings->music_volume);
        audio_set_bus_volume(audio, AUDIO_BUS_SFX, settings->sfx_volume);
        audio_set_bus_volume(audio, AUDIO_BUS_UI, settings->ui_volume);
    }

    if (engine) {
        // Through the engine, not GLFW: a second writer of the swap interval is
        // a value the engine cannot put back once a mode change drops it.
        engine_set_vsync(engine, settings->vsync);
        // Pushed whole; what this machine cannot honour, the engine refuses. A
        // file carried from a two-monitor desk applies as much of itself as it
        // can here rather than being filtered by every caller in turn.
        engine_set_window_mode(engine, settings->window_mode, settings->monitor);

        // The FOV is the camera's; the rest is whatever rig is live. Nothing
        // here reaches for a rig the app did not install -- an app that poses
        // its own camera keeps every one of these as its own business.
        if (engine->camera && settings->fov_degrees > 0.0f)
            engine->camera->fov_radians = glm_rad(settings->fov_degrees);

        CameraRig* rig = engine->camera_rig;
        if (rig) {
            // Stored beside the authored rates, never onto them. This runs on
            // every settings edit -- once a frame while a slider is held -- so
            // anything that scaled a rate in place would compound.
            rig->look_scale = settings->look_sensitivity;
            rig->invert_pitch = settings->invert_look_y;
            // The PLAYER's half, never the authored amplitude: this runs on
            // every settings edit, so writing shake_scale here would take an
            // app's own choice with it the first time any control moved.
            rig->shake_player_scale = settings->reduce_motion ? 0.0f : 1.0f;
        }
    }
}
