#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cetra/text.h"

#include "hud.h"

// The face gametest's menus use, from the splash app's assets; relative to the repository root,
// which is where every app in this tree is run from.
#define HUD_FONT  "apps/splash/assets/Roboto-Bold.ttf"
#define HUD_SIZE  22.0f
#define HUD_LIFT  0.14f // of the window's height, from its bottom edge to the prompt's
#define PLATE_BG  0.5f  // the dim plate's opacity behind the words
#define PLATE_PAD 8.0f

// A thought comes up over a quarter second, stays, and goes over a second.
#define THOUGHT_IN   0.25f
#define THOUGHT_STAY 4.0f
#define THOUGHT_OUT  1.0f

static const vec4 PROMPT_FG = {0.88f, 0.88f, 0.84f, 1.0f};
static const vec4 THOUGHT_FG = {0.80f, 0.78f, 0.70f, 1.0f};

// A dim plate behind a line's words, so they read over a lamp or the fog alike, drawn only while
// the line has words to show.
static void plate_draw(UIElement* el, UIDrawList* dl, void* user) {
    const HudLine* line = user;
    if (!line->text[0] || line->alpha <= 0.0f)
        return;
    const vec4 bg = {0.0f, 0.0f, 0.0f, PLATE_BG * line->alpha};
    ui_draw_rounded(dl, el->rect, 6.0f, (float*)bg, NULL, 0.0f);
}

// The words at `alpha`, in `fg`.
static void line_style(HudLine* line, const vec4 fg, float alpha) {
    const UIStyle style = {.fg = {fg[0], fg[1], fg[2], fg[3] * alpha}, .tracking = 1.0f};
    ui_set_style(line->label, &style);
    line->alpha = alpha;
}

static void line_build(HudLine* line, UIElement* parent, const vec4 fg) {
    line->plate = ui_panel(parent);
    line->plate->padding[0] = line->plate->padding[2] = PLATE_PAD;
    line->plate->padding[1] = line->plate->padding[3] = 2.25f * PLATE_PAD;
    ui_set_draw(line->plate, plate_draw, line);
    line->label = ui_label(line->plate, "");
    line_style(line, fg, 1.0f);
}

static void line_set(HudLine* line, const char* text) {
    if (strcmp(text, line->text) == 0)
        return;
    snprintf(line->text, sizeof(line->text), "%s", text);
    ui_set_text(line->label, line->text);
}

bool hud_start(Hud* hud, Engine* engine) {
    memset(hud, 0, sizeof(*hud));
    hud->engine = engine;
    Font* font = load_font(engine->text_renderer->font_pool, HUD_FONT, 64.0f, true);
    hud->font = font;
    if (!font) {
        fprintf(stderr, "silent: cannot load %s; no words on screen\n", HUD_FONT);
        return false;
    }
    hud->ui = create_ui_system(engine);
    if (!hud->ui) {
        fprintf(stderr, "silent: cannot make the UI; no words on screen\n");
        return false;
    }
    ui_set_font(hud->ui, font, HUD_SIZE);

    // A column filling the window, the thought over the prompt, both centred across it and
    // pushed to its foot. The prompt keeps its place when empty, so a thought never jumps.
    hud->screen = ui_screen(hud->ui, "hud");
    ui_screen_set_modal(hud->screen, false);
    UIElement* root = ui_screen_root(hud->screen);
    root->dir = UI_COLUMN;
    root->align_main = UI_ALIGN_END;
    root->align_cross = UI_ALIGN_CENTER;
    root->spacing = 10.0f;
    line_build(&hud->thought, root, THOUGHT_FG);
    line_build(&hud->prompt, root, PROMPT_FG);
    hud->thought_age = THOUGHT_IN + THOUGHT_STAY + THOUGHT_OUT;
    ui_push(hud->ui, hud->screen);
    ui_attach(hud->ui, engine);
    return true;
}

void hud_prompt(Hud* hud, const char* text) {
    if (hud->ui)
        line_set(&hud->prompt, text ? text : "");
}

void hud_think(Hud* hud, const char* text) {
    if (!hud->ui)
        return;
    line_set(&hud->thought, text);
    hud->thought_age = 0.0f;
}

void hud_update(Hud* hud, float dt) {
    if (!hud->ui)
        return;
    // From the window as it is now: a change of window mode resizes it.
    ui_screen_root(hud->screen)->padding[2] = HUD_LIFT * (float)hud->engine->win_height;

    hud->thought_age += dt;
    const float t = hud->thought_age;
    const float alpha = t < THOUGHT_IN ? t / THOUGHT_IN
                        : t < THOUGHT_IN + THOUGHT_STAY
                            ? 1.0f
                            : 1.0f - (t - THOUGHT_IN - THOUGHT_STAY) / THOUGHT_OUT;
    const float a = glm_clamp(alpha, 0.0f, 1.0f);
    if (a <= 0.0f && hud->thought.text[0])
        line_set(&hud->thought, "");
    if (fabsf(a - hud->thought.alpha) > 1.0f / 512.0f ||
        (a == 0.0f) != (hud->thought.alpha == 0.0f))
        line_style(&hud->thought, THOUGHT_FG, a);
}

void hud_free(Hud* hud) {
    free_ui_system(hud->ui);
    memset(hud, 0, sizeof(*hud));
}
