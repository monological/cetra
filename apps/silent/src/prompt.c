#include <stdio.h>
#include <string.h>

#include "cetra/text.h"

#include "prompt.h"

// The face gametest's menus use, from the splash app's assets; relative to the repository root,
// which is where every app in this tree is run from.
#define PROMPT_FONT "apps/splash/assets/Roboto-Bold.ttf"
#define PROMPT_SIZE 22.0f
#define PROMPT_LIFT 0.14f // of the window's height, from its bottom edge to the prompt's

bool prompt_start(Prompt* prompt, Engine* engine) {
    memset(prompt, 0, sizeof(*prompt));
    Font* font = load_font(engine->text_renderer->font_pool, PROMPT_FONT, 64.0f, true);
    if (!font) {
        fprintf(stderr, "silent: cannot load %s; no prompts\n", PROMPT_FONT);
        return false;
    }
    prompt->ui = create_ui_system(engine);
    if (!prompt->ui) {
        fprintf(stderr, "silent: cannot make the UI; no prompts\n");
        return false;
    }
    ui_set_font(prompt->ui, font, PROMPT_SIZE);

    // A column filling the window, its one child centred across it and pushed to its foot.
    prompt->screen = ui_screen(prompt->ui, "prompt");
    ui_screen_set_modal(prompt->screen, false);
    UIElement* root = ui_screen_root(prompt->screen);
    root->dir = UI_COLUMN;
    root->align_main = UI_ALIGN_END;
    root->align_cross = UI_ALIGN_CENTER;
    root->padding[2] = PROMPT_LIFT * (float)engine->win_height;

    // A dim plate behind the words, so they read over a lamp or the fog alike.
    UIElement* plate = ui_panel(root);
    const UIStyle plate_style = {.bg = {0.0f, 0.0f, 0.0f, 0.5f},
                                 .corner_radius = 6.0f,
                                 .padding = {8.0f, 18.0f, 8.0f, 18.0f}};
    ui_set_style(plate, &plate_style);
    prompt->label = ui_label(plate, "");
    const UIStyle text_style = {.fg = {0.88f, 0.88f, 0.84f, 1.0f}, .tracking = 1.0f};
    ui_set_style(prompt->label, &text_style);
    ui_attach(prompt->ui, engine);
    return true;
}

void prompt_show(Prompt* prompt, const char* text) {
    if (!prompt->ui)
        return;
    if (!text) {
        if (prompt->shown)
            ui_pop_all(prompt->ui);
        prompt->shown = false;
        return;
    }
    if (strcmp(text, prompt->text) != 0) {
        snprintf(prompt->text, sizeof(prompt->text), "%s", text);
        ui_set_text(prompt->label, prompt->text);
    }
    if (!prompt->shown)
        ui_push(prompt->ui, prompt->screen);
    prompt->shown = true;
}

void prompt_free(Prompt* prompt) {
    free_ui_system(prompt->ui);
    memset(prompt, 0, sizeof(*prompt));
}
