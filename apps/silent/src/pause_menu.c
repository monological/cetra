#include "pause_menu.h"

#include <string.h>

#include "hud.h"

#define PAUSE_DIM 0.45f // the world dimmed behind it

void pause_menu_start(PauseMenu* pm, UISystem* ui, Game* game, SettingsMenu* settings,
                      UIActionFn to_title) {
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
    if (to_title)
        hud_item(col, "Quit to Title", HUD_ITEM_SIZE, to_title, NULL);
    hud_quit_item(col, HUD_ITEM_SIZE, game);
}

void pause_menu_show(PauseMenu* pm) {
    hud_show(pm->ui, pm->screen, pm->resume);
}

bool pause_menu_up(const PauseMenu* pm) {
    return ui_on_stack(pm->ui, pm->screen);
}
