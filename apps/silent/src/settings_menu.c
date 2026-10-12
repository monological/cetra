#include "settings_menu.h"

#include <stdio.h>
#include <string.h>

#include "cetra/game/save.h"

#include "hud.h"

#define SETTINGS_DIM 0.55f // the world dimmed behind it, which has more to read
#define HEADING_SIZE 34.0f
#define ROW_SIZE     22.0f
#define ITEM_SIZE    26.0f
#define WIDTH        600.0f
#define ROW_PAD      10.0f
#define TRACK_H      4.0f  // the UI's own slider track, which its drag is measured along
#define TRACK_BED    0.25f // of the words' colour, the track's unfilled part
#define KNOB_R       5.0f
#define ROW_PLATE    0.45f

static const char* const WINDOW_MODES[] = {"Windowed", "Fullscreen", "Borderless"};
_Static_assert(sizeof(WINDOW_MODES) / sizeof(WINDOW_MODES[0]) == ENGINE_WINDOW_MODE_COUNT,
               "a name for every window mode");

// How a slider's value reads: its value times `scale` through `format`.
typedef struct ValueFormat {
    const char* format;
    float scale;
} ValueFormat;

static const ValueFormat PERCENT = {"%.0f%%", 100.0f};
static const ValueFormat TIMES = {"%.2f", 1.0f};
static const ValueFormat WHOLE = {"%.0f", 1.0f};

static void value_of(const UIElement* el, const ValueFormat* fmt, char* out, size_t cap) {
    if (el->kind == UI_SLIDER && el->bound_float && fmt)
        snprintf(out, cap, fmt->format, (double)(*el->bound_float * fmt->scale));
    else if (el->kind == UI_TOGGLE && el->bound_bool)
        snprintf(out, cap, "%s", *el->bound_bool ? "On" : "Off");
    else if (el->kind == UI_SELECTOR && el->bound_int && el->option_count > 0)
        snprintf(out, cap, "<   %s   >", el->options[*el->bound_int % el->option_count]);
    else
        out[0] = '\0';
}

// A row as words on nothing, its name on the left and its value on the right, brightening onto a
// dim plate as the keys or the pointer come to it; a slider's track under them, filled to its
// value, where the UI measures a drag along.
static void row_draw(UIElement* el, UIDrawList* dl, void* user) {
    const float f = el->t_focus;
    if (f > 0.0f) {
        const vec4 bg = {0.0f, 0.0f, 0.0f, ROW_PLATE * f};
        ui_draw_rounded(dl, el->rect, 4.0f, (float*)bg, NULL, 0.0f);
    }
    UIStyle style = {.tracking = 1.5f, .font_size = el->style ? el->style->font_size : 0.0f};
    glm_vec4_lerp((float*)HUD_ITEM_FG, (float*)HUD_FOCUS_FG, f, style.fg);
    const UIRect line = ui_rect_inset(el->rect, ROW_PAD, ROW_PAD, ROW_PAD, ROW_PAD);
    ui_draw_text(dl, line, el->text, &style, UI_ALIGN_START);
    char value[48];
    value_of(el, user, value, sizeof(value));
    ui_draw_text(dl, line, value, &style, UI_ALIGN_END);
    if (el->kind != UI_SLIDER || !el->bound_float || el->range_hi <= el->range_lo)
        return;
    const UIRect track = {line.x, line.y + line.h - TRACK_H, line.w, TRACK_H};
    const float t =
        glm_clamp((*el->bound_float - el->range_lo) / (el->range_hi - el->range_lo), 0.0f, 1.0f);
    const vec4 bed = {style.fg[0], style.fg[1], style.fg[2], style.fg[3] * TRACK_BED};
    ui_draw_rounded(dl, track, 0.5f * TRACK_H, (float*)bed, NULL, 0.0f);
    ui_draw_rounded(dl, (UIRect){track.x, track.y, track.w * t, track.h}, 0.5f * TRACK_H, style.fg,
                    NULL, 0.0f);
    const float cx = track.x + track.w * t, cy = track.y + 0.5f * TRACK_H;
    ui_draw_rounded(dl, (UIRect){cx - KNOB_R, cy - KNOB_R, 2.0f * KNOB_R, 2.0f * KNOB_R}, KNOB_R,
                    style.fg, NULL, 0.0f);
}

// A row the width of its column, in the menus' look.
static UIElement* row(UIElement* el, const ValueFormat* fmt) {
    const UIStyle style = {.font_size = ROW_SIZE, .padding = {ROW_PAD, ROW_PAD, ROW_PAD, ROW_PAD}};
    ui_set_style(el, &style);
    ui_set_draw(el, row_draw, (void*)fmt);
    el->size_mode[0] = UI_GROW;
    return el;
}

static void apply(const SettingsMenu* sm) {
    settings_apply(&sm->settings, sm->audio, sm->engine);
    if (sm->audio && sm->mute)
        audio_set_bus_volume(sm->audio, AUDIO_BUS_MASTER, 0.0f);
    if (sm->look) {
        sm->look->look_scale = sm->settings.look_sensitivity;
        sm->look->invert_pitch = sm->settings.invert_look_y;
    }
}

static void on_change(UIElement* el, void* user) {
    (void)el;
    SettingsMenu* sm = user;
    sm->dirty = true;
    apply(sm);
}

static void on_back(UIElement* el, void* user) {
    (void)el;
    SettingsMenu* sm = user;
    if (settings_menu_open(sm))
        ui_pop(sm->ui);
}

void settings_menu_load(SettingsMenu* sm) {
    memset(sm, 0, sizeof(*sm));
    settings_defaults(&sm->settings);
    if (save_default_path(sm->path, sizeof(sm->path), "silent-settings"))
        settings_load(&sm->settings, sm->path);
    else
        sm->path[0] = '\0';
    if (sm->settings.fov_degrees <= 0.0f)
        sm->settings.fov_degrees = SETTINGS_FOV;
}

void settings_menu_start(SettingsMenu* sm, UISystem* ui, Engine* engine, AudioSystem* audio,
                         CameraRig* look) {
    sm->ui = ui;
    sm->engine = engine;
    sm->audio = audio;
    sm->look = look;
    apply(sm);
    if (!ui)
        return;
    UIElement* root = NULL;
    sm->screen = hud_modal_screen(ui, "settings", SETTINGS_DIM, &root);
    ui_screen_set_opaque(sm->screen, true);
    UIElement* col = hud_column(root, WIDTH);
    GameSettings* s = &sm->settings;
    hud_words(col, "Settings", HEADING_SIZE, HUD_HEADING_FG, 4.0f);
    sm->first =
        row(ui_slider(col, "Volume", 0.0f, 1.0f, &s->master_volume, on_change, sm), &PERCENT);
    row(ui_selector(col, "Display", WINDOW_MODES, ENGINE_WINDOW_MODE_COUNT, &s->window_mode,
                    on_change, sm),
        NULL);
    row(ui_toggle(col, "VSync", &s->vsync, on_change, sm), NULL);
    row(ui_slider(col, "Look speed", 0.25f, 3.0f, &s->look_sensitivity, on_change, sm), &TIMES);
    row(ui_toggle(col, "Invert look", &s->invert_look_y, on_change, sm), NULL);
    row(ui_slider(col, "Field of view", 50.0f, 100.0f, &s->fov_degrees, on_change, sm), &WHOLE);
    hud_item(col, "Back", ITEM_SIZE, on_back, sm);
}

void settings_menu_show(SettingsMenu* sm) {
    if (!sm->screen || settings_menu_open(sm))
        return;
    ui_push(sm->ui, sm->screen);
    ui_set_focus(sm->screen, sm->first);
}

bool settings_menu_open(const SettingsMenu* sm) {
    return sm->screen && ui_top(sm->ui) == sm->screen;
}

void settings_menu_frame(SettingsMenu* sm) {
    if (!sm->dirty || settings_menu_open(sm))
        return;
    sm->dirty = false;
    if (sm->path[0] && settings_save(&sm->settings, sm->path))
        printf("silent: settings kept in %s\n", sm->path);
}
