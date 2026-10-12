#include "settings_menu.h"

#include <stdio.h>
#include <string.h>

#include "cetra/game/save.h"

#include "hud.h"

#define WIDTH     600.0f
#define TRACK_BED 0.25f // of the words' colour, the track's unfilled part
#define KNOB_R    5.0f

static const char* const WINDOW_MODES[] = {"Windowed", "Fullscreen", "Borderless"};
_Static_assert(sizeof(WINDOW_MODES) / sizeof(WINDOW_MODES[0]) == ENGINE_WINDOW_MODE_COUNT,
               "a name for every window mode");

static void value_of(const UIElement* el, const SettingsRow* row, char* out, size_t cap) {
    if (el->kind == UI_SLIDER)
        snprintf(out, cap, row->format, (double)(*el->bound_float * row->scale));
    else if (el->kind == UI_TOGGLE)
        snprintf(out, cap, "%s", *el->bound_bool ? "On" : "Off");
    else
        snprintf(out, cap, "<   %s   >", el->options[*el->bound_int % el->option_count]);
}

// A row in the items' look, its name on the left and its value on the right; under a slider, its
// track filled to its value, where the UI measures a drag along.
static void row_draw(UIElement* el, UIDrawList* dl, void* user) {
    const SettingsRow* row = user;
    UIStyle style;
    const UIRect words = hud_item_look(el, dl, &style);
    ui_draw_text(dl, words, el->text, &style, UI_ALIGN_START);
    char value[48];
    value_of(el, row, value, sizeof(value));
    ui_draw_text(dl, words, value, &style, UI_ALIGN_END);
    if (el->kind != UI_SLIDER)
        return;
    const UIRect track = ui_slider_track(row->menu->ui, el);
    const float t =
        glm_clamp((*el->bound_float - el->range_lo) / (el->range_hi - el->range_lo), 0.0f, 1.0f);
    const vec4 bed = {style.fg[0], style.fg[1], style.fg[2], style.fg[3] * TRACK_BED};
    const float r = 0.5f * track.h;
    ui_draw_rounded(dl, track, r, (float*)bed, NULL, 0.0f);
    ui_draw_rounded(dl, (UIRect){track.x, track.y, track.w * t, track.h}, r, style.fg, NULL, 0.0f);
    const float cx = track.x + track.w * t, cy = track.y + r;
    ui_draw_rounded(dl, (UIRect){cx - KNOB_R, cy - KNOB_R, 2.0f * KNOB_R, 2.0f * KNOB_R}, KNOB_R,
                    style.fg, NULL, 0.0f);
}

static void on_change(UIElement* el, void* user) {
    (void)el;
    SettingsMenu* sm = user;
    sm->dirty = true;
    settings_menu_apply(sm);
}

// The next row of the screen: `el`, drawn reading its value through `format` times `scale`.
static UIElement* row(SettingsMenu* sm, int* count, UIElement* el, const char* format,
                      float scale) {
    SettingsRow* r = &sm->rows[(*count)++];
    *r = (SettingsRow){sm, format, scale};
    return hud_dress(el, HUD_ROW_SIZE, row_draw, r);
}

void settings_menu_load(SettingsMenu* sm, bool mute, float fov) {
    memset(sm, 0, sizeof(*sm));
    sm->mute = mute;
    sm->fov = fov;
    settings_defaults(&sm->settings);
    if (save_default_path(sm->path, sizeof(sm->path), "silent-settings"))
        settings_load(&sm->settings, sm->path);
    else
        sm->path[0] = '\0';
    if (sm->settings.fov_degrees <= 0.0f)
        sm->settings.fov_degrees = SETTINGS_FOV;
}

void settings_menu_start(SettingsMenu* sm, UISystem* ui, Engine* engine, AudioSystem* audio) {
    sm->ui = ui;
    sm->engine = engine;
    sm->audio = audio;
    if (!ui)
        return;
    UIElement* root = NULL;
    sm->screen = hud_modal_screen(ui, "settings", HUD_LIST_DIM, &root);
    ui_screen_set_opaque(sm->screen, true);
    UIElement* col = hud_column(root, WIDTH);
    GameSettings* s = &sm->settings;
    int n = 0;
    hud_heading(col, "Settings");
    sm->first = row(sm, &n, ui_slider(col, "Volume", 0.0f, 1.0f, &s->master_volume, on_change, sm),
                    "%.0f%%", 100.0f);
    row(sm, &n,
        ui_selector(col, "Display", WINDOW_MODES, ENGINE_WINDOW_MODE_COUNT, &s->window_mode,
                    on_change, sm),
        NULL, 0.0f);
    row(sm, &n, ui_toggle(col, "VSync", &s->vsync, on_change, sm), NULL, 0.0f);
    row(sm, &n, ui_slider(col, "Look speed", 0.25f, 3.0f, &s->look_sensitivity, on_change, sm),
        "%.2f", 1.0f);
    row(sm, &n, ui_toggle(col, "Invert look", &s->invert_look_y, on_change, sm), NULL, 0.0f);
    row(sm, &n, ui_slider(col, "Field of view", 50.0f, 100.0f, &s->fov_degrees, on_change, sm),
        "%.0f", 1.0f);
    hud_back_item(col, "Back", HUD_ITEM_SIZE, ui);
}

void settings_menu_apply(const SettingsMenu* sm) {
    GameSettings applied = sm->settings;
    if (sm->mute)
        applied.master_volume = 0.0f;
    if (sm->fov > 0.0f)
        applied.fov_degrees = sm->fov;
    settings_apply(&applied, sm->audio, sm->engine);
}

static void on_open(UIElement* el, void* user) {
    (void)el;
    settings_menu_show(user);
}

UIElement* settings_menu_item(UIElement* parent, float size, SettingsMenu* sm) {
    return hud_item(parent, "Settings", size, on_open, sm);
}

void settings_menu_show(SettingsMenu* sm) {
    hud_show(sm->ui, sm->screen, sm->first);
}

bool settings_menu_open(const SettingsMenu* sm) {
    return hud_on_top(sm->ui, sm->screen);
}

void settings_menu_save(SettingsMenu* sm) {
    if (!sm->dirty)
        return;
    sm->dirty = false;
    if (sm->path[0] && settings_save(&sm->settings, sm->path))
        printf("silent: settings kept in %s\n", sm->path);
}

void settings_menu_frame(SettingsMenu* sm) {
    if (!settings_menu_open(sm))
        settings_menu_save(sm);
}
