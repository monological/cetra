#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cetra/program.h"
#include "cetra/text.h"

#include "hud.h"
#include "silent_shaders.h"

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

#define MODAL_FADE_IN 0.15f // seconds a screen over the world takes to come up
#define ITEM_PAD      10.0f
#define ITEM_PLATE    0.45f // the plate behind the menu item the keys are on

// The save mark comes up over SAVE_IN, its candles lit one after another over SAVE_LIGHTING while
// it says it is saving, then it says it saved for SAVE_STAY and goes over SAVE_OUT.
#define SAVE_IN       0.25f
#define SAVE_LIGHTING 1.2f
#define SAVE_STAY     1.6f
#define SAVE_OUT      0.8f
#define SAVE_END      (SAVE_LIGHTING + SAVE_STAY + SAVE_OUT)
#define SAVE_ICON     34.0f // the candlestick's square, in points
#define SAVE_MARGIN   26.0f // from the window's right and bottom edges
#define SAVE_GAP      10.0f // between the candlestick and the words
#define SAVE_SIZE     18.0f // the words' size

static const vec4 PROMPT_FG = {0.88f, 0.88f, 0.84f, 1.0f};
static const vec4 THOUGHT_FG = {0.80f, 0.78f, 0.70f, 1.0f};

const vec4 HUD_ITEM_FG = {0.62f, 0.63f, 0.60f, 1.0f};
const vec4 HUD_FOCUS_FG = {0.93f, 0.93f, 0.89f, 1.0f};
const vec4 HUD_DISABLED_FG = {0.35f, 0.36f, 0.34f, 1.0f};
const vec4 HUD_HEADING_FG = {0.78f, 0.80f, 0.76f, 0.85f};

// The words' look but their colour; the face and its size are the UI's.
static const UIStyle WORDS = {.tracking = 1.0f, .line_spacing = 1.0f};

static bool line_shown(const HudLine* line) {
    return line->label->text && line->label->text[0] && line->alpha > 0.0f;
}

// A dim plate behind a line's words, so they read over a lamp or the fog alike, drawn only while
// the line has words to show.
static void plate_draw(UIElement* el, UIDrawList* dl, void* user) {
    const HudLine* line = user;
    if (!line_shown(line))
        return;
    const vec4 bg = {0.0f, 0.0f, 0.0f, PLATE_BG * line->alpha};
    ui_draw_rounded(dl, el->rect, 6.0f, (float*)bg, NULL, 0.0f);
}

// The words, as the label would draw them, at the line's alpha.
static void words_draw(UIElement* el, UIDrawList* dl, void* user) {
    const HudLine* line = user;
    if (!line_shown(line))
        return;
    UIStyle style = WORDS;
    glm_vec4_copy((float*)line->fg, style.fg);
    style.fg[3] *= line->alpha;
    ui_draw_text(dl, el->rect, el->text, &style, el->align_cross);
}

static void line_build(HudLine* line, UIElement* parent, const float* fg) {
    line->fg = fg;
    line->alpha = 1.0f;
    line->plate = ui_panel(parent);
    line->plate->padding[0] = line->plate->padding[2] = PLATE_PAD;
    line->plate->padding[1] = line->plate->padding[3] = 2.25f * PLATE_PAD;
    ui_set_draw(line->plate, plate_draw, line);
    line->label = ui_label(line->plate, "");
    ui_set_style(line->label, &WORDS);
    ui_set_draw(line->label, words_draw, line);
}

// The save mark in the bottom right corner of the window the element covers: the candlestick, then
// the words, both in the words' colour. It is placed for the longer of its words, so the
// candlestick stays where it is when they change.
static void save_mark_draw(UIElement* el, UIDrawList* dl, void* user) {
    const Hud* hud = user;
    const float t = hud->saved_age;
    const float alpha = glm_clamp(fminf(t / SAVE_IN, (SAVE_END - t) / SAVE_OUT), 0.0f, 1.0f);
    if (alpha <= 0.0f)
        return;
    UIStyle style = WORDS;
    style.font_size = SAVE_SIZE;
    style.tracking = 1.5f;
    glm_vec4_copy((float*)THOUGHT_FG, style.fg);
    style.fg[3] *= alpha;
    const float widest = ui_text_width(hud->font, SAVE_SIZE, style.tracking, "Saving...");
    const UIRect r = el->rect;
    const UIRect icon = {r.x + r.w - SAVE_MARGIN - widest - SAVE_GAP - SAVE_ICON,
                         r.y + r.h - SAVE_MARGIN - SAVE_ICON, SAVE_ICON, SAVE_ICON};
    if (hud->candles) {
        ui_set_draw_program(dl, hud->candles, icon,
                            3.0f * glm_clamp(t / SAVE_LIGHTING, 0.0f, 1.0f));
        ui_draw_quad(dl, icon, style.fg);
        ui_set_draw_program(dl, NULL, icon, 0.0f);
    }
    const float lh = ui_line_height(hud->font, SAVE_SIZE, 1.0f);
    const UIRect words = {icon.x + SAVE_ICON + SAVE_GAP, icon.y + 0.5f * (SAVE_ICON - lh), widest,
                          lh};
    ui_draw_text(dl, words, t < SAVE_LIGHTING ? "Saving..." : "Saved", &style, UI_ALIGN_START);
}

static void line_set(HudLine* line, const char* text) {
    if (line->label->text && strcmp(text, line->label->text) == 0)
        return;
    ui_set_text(line->label, text);
}

bool hud_start(Hud* hud, Engine* engine) {
    memset(hud, 0, sizeof(*hud));
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
    // A line tall, words or none, with the label's own padding over and under it.
    ui_set_size(hud->prompt.label, UI_FIT, 0.0f, UI_FIXED,
                ui_line_height(font, HUD_SIZE, 1.0f) + 8.0f);
    hud->thought_age = THOUGHT_IN + THOUGHT_STAY + THOUGHT_OUT;
    // The save mark over the whole window, out of the column's flow, drawn in its corner.
    hud->save_mark = ui_panel(root);
    hud->save_mark->fill = true;
    ui_set_draw(hud->save_mark, save_mark_draw, hud);
    hud->saved_age = SAVE_END;
    hud->candles = create_ui_draw_program("save_candles", candles_frag_shader_str);
    if (hud->candles)
        engine_add_program(engine, hud->candles);
    else
        fprintf(stderr, "silent: the save mark's candlestick did not build; it says so in words\n");
    ui_push(hud->ui, hud->screen);
    ui_attach(hud->ui, engine);
    return true;
}

void hud_prompt(Hud* hud, const char* text) {
    if (hud->ui)
        line_set(&hud->prompt, text ? text : hud->hint_left > 0.0f ? hud->hint : "");
}

void hud_hint(Hud* hud, const char* text, float seconds) {
    hud->hint = text;
    hud->hint_left = seconds;
}

void hud_think(Hud* hud, const char* text) {
    if (!hud->ui)
        return;
    // The same thought had again carries on from as far as it had come in, rather than going out
    // and coming in again.
    const bool again = line_shown(&hud->thought) && strcmp(text, hud->thought.label->text) == 0;
    line_set(&hud->thought, text);
    hud->thought_age = again ? hud->thought.alpha * THOUGHT_IN : 0.0f;
}

void hud_saved(Hud* hud) {
    hud->saved_age = hud->saved_age < SAVE_END ? fminf(hud->saved_age, SAVE_LIGHTING) : 0.0f;
}

void hud_update(Hud* hud, float dt, float height) {
    if (!hud->ui)
        return;
    // From the window as it is now: a change of window mode resizes it.
    ui_screen_root(hud->screen)->padding[2] = HUD_LIFT * height;
    hud->hint_left = fmaxf(hud->hint_left - dt, 0.0f);
    hud->saved_age = fminf(hud->saved_age + dt, SAVE_END);

    hud->thought_age += dt;
    const float t = hud->thought_age;
    const float alpha = t < THOUGHT_IN ? t / THOUGHT_IN
                        : t < THOUGHT_IN + THOUGHT_STAY
                            ? 1.0f
                            : 1.0f - (t - THOUGHT_IN - THOUGHT_STAY) / THOUGHT_OUT;
    hud->thought.alpha = glm_clamp(alpha, 0.0f, 1.0f);
    if (hud->thought.alpha <= 0.0f)
        line_set(&hud->thought, "");
}

void hud_free(Hud* hud) {
    free_ui_system(hud->ui);
    memset(hud, 0, sizeof(*hud));
}

UIElement* hud_bare(UIElement* parent, UIDir dir, float spacing) {
    // A fill that is there and see-through, since a zero would take the theme's.
    static const UIStyle CLEAR = {.bg = {1.0f, 1.0f, 1.0f, 0.0f}};
    UIElement* el = ui_panel(parent);
    el->dir = dir;
    el->spacing = spacing;
    for (int i = 0; i < 4; i++)
        el->padding[i] = HUD_BARE;
    ui_set_style(el, &CLEAR);
    return el;
}

// A menu item as words on nothing, brightening onto a dim plate as the keys or the pointer come to
// it.
static void item_draw(UIElement* el, UIDrawList* dl, void* user) {
    (void)user;
    if (!el->text || !el->text[0] || el->rect.h <= 0.0f)
        return;
    const float f = el->disabled ? 0.0f : el->t_focus;
    if (f > 0.0f) {
        const vec4 bg = {0.0f, 0.0f, 0.0f, ITEM_PLATE * f};
        ui_draw_rounded(dl, el->rect, 4.0f, (float*)bg, NULL, 0.0f);
    }
    UIStyle style = {.tracking = 1.5f, .font_size = el->style ? el->style->font_size : 0.0f};
    if (el->disabled)
        glm_vec4_copy((float*)HUD_DISABLED_FG, style.fg);
    else
        glm_vec4_lerp((float*)HUD_ITEM_FG, (float*)HUD_FOCUS_FG, f, style.fg);
    // The words from the top of the rect, as the label would sit inside its padding.
    ui_draw_text(dl, ui_rect_inset(el->rect, ITEM_PAD, ITEM_PAD, ITEM_PAD, ITEM_PAD), el->text,
                 &style, el->align_cross);
}

UIElement* hud_item(UIElement* parent, const char* text, float size, UIActionFn action,
                    void* user) {
    UIElement* el = ui_button(parent, text, action, user);
    const UIStyle style = {.font_size = size, .padding = {ITEM_PAD, ITEM_PAD, ITEM_PAD, ITEM_PAD}};
    ui_set_style(el, &style);
    ui_set_draw(el, item_draw, NULL);
    el->size_mode[0] = UI_GROW;
    el->align_cross = UI_ALIGN_CENTER;
    return el;
}

void hud_item_shown(UIElement* el, bool shown) {
    el->disabled = !shown;
    el->size_mode[1] = shown ? UI_FIT : UI_FIXED;
    el->size[1] = 0.0f;
}

UIElement* hud_column(UIElement* root, float width) {
    UIElement* col = hud_bare(root, UI_COLUMN, 6.0f);
    col->align_cross = UI_ALIGN_CENTER;
    ui_set_size(col, UI_FIXED, width, UI_FIT, 0.0f);
    return col;
}

UIElement* hud_words(UIElement* parent, const char* text, float size, const float* fg,
                     float tracking) {
    UIElement* el = ui_label(parent, text);
    UIStyle style = {.font_size = size, .tracking = tracking, .padding = {6.0f, 0.0f, 18.0f, 0.0f}};
    glm_vec4_copy((float*)fg, style.fg);
    ui_set_style(el, &style);
    return el;
}

UIScreen* hud_modal_screen(UISystem* ui, const char* name, float dim, UIElement** root) {
    UIScreen* screen = ui_screen(ui, name);
    ui_screen_set_modal(screen, true);
    ui_screen_transition(screen, UI_TRANSITION_FADE, MODAL_FADE_IN);
    *root = ui_screen_root(screen);
    (*root)->align_main = UI_ALIGN_CENTER;
    (*root)->align_cross = UI_ALIGN_CENTER;
    // The world dimmed behind it, out of the flow.
    UIElement* backdrop = ui_panel(*root);
    backdrop->fill = true;
    const UIStyle style = {.bg = {0.0f, 0.0f, 0.0f, dim}, .corner_radius = HUD_BARE};
    ui_set_style(backdrop, &style);
    return screen;
}
