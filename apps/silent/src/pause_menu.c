#include "pause_menu.h"

#include <string.h>

#include "hud.h"

#define PAUSE_DIM 0.45f // the world dimmed behind it

static void on_to_title(UIElement* el, void* user) {
    (void)el;
    PauseMenu* pm = user;
    pm->to_title = true;
}

void pause_menu_start(PauseMenu* pm, UISystem* ui, Game* game, SettingsMenu* settings,
                      bool titled) {
    memset(pm, 0, sizeof(*pm));
    pm->ui = ui;
    if (!ui)
        return;
    UIElement* root = NULL;
    pm->screen = hud_modal_screen(ui, "pause", PAUSE_DIM, &root);
    UIElement* col = hud_column(root, HUD_MENU_WIDTH);
    hud_heading(col, "Paused");
    pm->resume = hud_back_item(col, "Resume", HUD_ITEM_SIZE, ui);
    settings_menu_item(col, HUD_ITEM_SIZE, settings);
    if (titled)
        hud_item(col, "Quit to Title", HUD_ITEM_SIZE, on_to_title, pm);
    hud_quit_item(col, HUD_ITEM_SIZE, game);
}

void pause_menu_show(PauseMenu* pm) {
    pm->to_title = false;
    hud_show(pm->ui, pm->screen, pm->resume);
}

bool pause_menu_take_to_title(PauseMenu* pm) {
    if (!pm->to_title)
        return false;
    pm->to_title = false;
    if (hud_on_top(pm->ui, pm->screen))
        ui_pop(pm->ui);
    return true;
}

bool pause_menu_up(const PauseMenu* pm) {
    return ui_on_stack(pm->ui, pm->screen);
}
