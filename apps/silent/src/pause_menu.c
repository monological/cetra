#include "pause_menu.h"

#include <string.h>

#include "hud.h"

#define PAUSE_DIM    0.45f // the world dimmed behind it
#define HEADING_SIZE 34.0f
#define ITEM_SIZE    26.0f
#define WIDTH        520.0f

static void on_resume(UIElement* el, void* user) {
    (void)el;
    PauseMenu* pm = user;
    if (pause_menu_open(pm))
        ui_pop(pm->ui);
}

static void on_settings(UIElement* el, void* user) {
    (void)el;
    PauseMenu* pm = user;
    settings_menu_show(pm->settings);
}

static void on_exit(UIElement* el, void* user) {
    (void)el;
    PauseMenu* pm = user;
    pm->choice = PAUSE_EXIT;
}

void pause_menu_start(PauseMenu* pm, UISystem* ui, SettingsMenu* settings) {
    memset(pm, 0, sizeof(*pm));
    pm->ui = ui;
    pm->settings = settings;
    if (!ui)
        return;
    UIElement* root = NULL;
    pm->screen = hud_modal_screen(ui, "pause", PAUSE_DIM, &root);
    UIElement* col = hud_column(root, WIDTH);
    hud_words(col, "Paused", HEADING_SIZE, HUD_HEADING_FG, 4.0f);
    pm->resume = hud_item(col, "Resume", ITEM_SIZE, on_resume, pm);
    hud_item(col, "Settings", ITEM_SIZE, on_settings, pm);
    hud_item(col, "Exit to Desktop", ITEM_SIZE, on_exit, pm);
}

void pause_menu_show(PauseMenu* pm) {
    if (!pm->screen || pause_menu_open(pm))
        return;
    ui_push(pm->ui, pm->screen);
    ui_set_focus(pm->screen, pm->resume);
}

bool pause_menu_open(const PauseMenu* pm) {
    return pm->screen && ui_top(pm->ui) == pm->screen;
}

PauseChoice pause_menu_take(PauseMenu* pm) {
    const PauseChoice choice = pm->choice;
    pm->choice = PAUSE_STAYING;
    return choice;
}
