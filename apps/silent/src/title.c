#include "title.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "hud.h"
#include "sounds.h"

// From the far pavement, back across the road to the house under its lamps: the first thing the
// player sees of the place, and what they stand in front of later.
static const vec3 TITLE_EYE = {-6.5f, 1.45f, -5.2f};
static const vec3 TITLE_TARGET = {0.5f, 2.4f, 10.0f};

#define TITLE_DIM     0.2f  // the street dimmed behind the title
#define LIST_DIM      0.55f // and behind the list of games, which has more to read
#define WORDMARK_SIZE 72.0f
#define ITEM_SIZE     26.0f
#define ROW_SIZE      22.0f
#define ITEM_WIDTH    520.0f
#define ITEM_PAD      10.0f
#define PLATE_ALPHA   0.45f // the plate behind the item the keys are on

static const vec4 WORDMARK_FG = {0.78f, 0.80f, 0.76f, 0.85f};
static const vec4 ITEM_FG = {0.62f, 0.63f, 0.60f, 1.0f};
static const vec4 FOCUS_FG = {0.93f, 0.93f, 0.89f, 1.0f};
static const vec4 DISABLED_FG = {0.35f, 0.36f, 0.34f, 1.0f};
static const vec4 NOTE_FG = {0.80f, 0.78f, 0.70f, 1.0f};

void title_view(vec3 eye, vec3 target) {
    glm_vec3_copy((float*)TITLE_EYE, eye);
    glm_vec3_copy((float*)TITLE_TARGET, target);
}

// An item as words on nothing, brightening onto a dim plate as the keys or the pointer come to it.
static void item_draw(UIElement* el, UIDrawList* dl, void* user) {
    (void)user;
    if (!el->text || !el->text[0] || el->rect.h <= 0.0f)
        return;
    const float f = el->disabled ? 0.0f : el->t_focus;
    if (f > 0.0f) {
        const vec4 bg = {0.0f, 0.0f, 0.0f, PLATE_ALPHA * f};
        ui_draw_rounded(dl, el->rect, 4.0f, (float*)bg, NULL, 0.0f);
    }
    UIStyle style = {.tracking = 1.5f, .font_size = el->style ? el->style->font_size : 0.0f};
    if (el->disabled)
        glm_vec4_copy((float*)DISABLED_FG, style.fg);
    else
        glm_vec4_lerp((float*)ITEM_FG, (float*)FOCUS_FG, f, style.fg);
    // The words from the top of the rect, as the label would sit inside its padding.
    ui_draw_text(dl, ui_rect_inset(el->rect, ITEM_PAD, ITEM_PAD, ITEM_PAD, ITEM_PAD), el->text,
                 &style, el->align_cross);
}

static UIElement* item(UIElement* parent, const char* text, float size, UIActionFn action,
                       void* user) {
    UIElement* el = ui_button(parent, text, action, user);
    const UIStyle style = {.font_size = size, .padding = {ITEM_PAD, ITEM_PAD, ITEM_PAD, ITEM_PAD}};
    ui_set_style(el, &style);
    ui_set_draw(el, item_draw, NULL);
    el->size_mode[0] = UI_GROW;
    el->align_cross = UI_ALIGN_CENTER;
    return el;
}

// Shown or not without leaving the tree, which keeps what it has: a hidden item takes no height
// and no key lands on it.
static void item_shown(UIElement* el, bool shown) {
    el->focusable = shown;
    el->disabled = !shown;
    el->size_mode[1] = shown ? UI_FIT : UI_FIXED;
    el->size[1] = 0.0f;
}

// A panel that lays out and draws nothing: a style cannot ask for no fill, since its zero inherits
// the theme's.
static void draw_nothing(UIElement* el, UIDrawList* dl, void* user) {
    (void)el;
    (void)dl;
    (void)user;
}

static UIElement* column(UIElement* root, float width) {
    UIElement* col = ui_panel(root);
    ui_set_draw(col, draw_nothing, NULL);
    col->dir = UI_COLUMN;
    col->align_cross = UI_ALIGN_CENTER;
    col->spacing = 6.0f;
    ui_set_size(col, UI_FIXED, width, UI_FIT, 0.0f);
    return col;
}

static UIElement* words(UIElement* parent, const char* text, float size, const float* fg,
                        float tracking) {
    UIElement* el = ui_label(parent, text);
    UIStyle style = {.font_size = size, .tracking = tracking, .padding = {6.0f, 0.0f, 18.0f, 0.0f}};
    glm_vec4_copy((float*)fg, style.fg);
    ui_set_style(el, &style);
    return el;
}

// ------------------------------------------------------------------------------- the games

// "Home, kitchen    12 Oct 21:40    1 h 23 min": where, when, and how long.
static void slot_label(const SaveSlot* s, char* out, size_t cap) {
    char when[32] = "";
    const time_t at = (time_t)s->saved_at;
    const struct tm* t = localtime(&at);
    if (t)
        strftime(when, sizeof(when), "%d %b  %H:%M", t);
    const int minutes = (int)(s->played / 60.0);
    char played[24];
    if (minutes < 60)
        snprintf(played, sizeof(played), "%d min", minutes);
    else
        snprintf(played, sizeof(played), "%d h %02d min", minutes / 60, minutes % 60);
    snprintf(out, cap, "%s      %s      %s", sounds_room_label(sounds_room_at(s->feet)), when,
             played);
}

// The games kept, read again, onto the title's items and the list's rows.
static void refresh(Title* t) {
    t->count = save_game_list(t->slots);
    for (int i = 0; i < SAVE_GAME_SLOTS; i++) {
        if (i < t->count)
            slot_label(&t->slots[i], t->labels[i], sizeof(t->labels[i]));
        ui_set_text(t->rows[i], i < t->count ? t->labels[i] : "");
        item_shown(t->rows[i], i < t->count);
    }
    item_shown(t->resume, t->count > 0);
    t->load->disabled = t->count == 0;
    t->load->focusable = t->count > 0;
}

static void choose(Title* t, TitleChoice choice, const SaveSlot* slot) {
    t->chosen = *slot;
    t->choice = choice;
    t->up = false;
    while (title_open(t))
        ui_pop(t->ui);
}

static void on_resume(UIElement* el, void* user) {
    (void)el;
    Title* t = user;
    if (t->count > 0)
        choose(t, TITLE_LOAD, &t->slots[0]);
}

static void on_fresh(UIElement* el, void* user) {
    (void)el;
    Title* t = user;
    SaveSlot slot;
    if (save_game_free_slot(&slot)) {
        choose(t, TITLE_NEW, &slot);
        return;
    }
    char note[96];
    snprintf(note, sizeof(note), "%d games are kept. Delete one in Load Game.", SAVE_GAME_SLOTS);
    ui_set_text(t->note, note);
}

// The list in the title's place rather than over it, which would show through; the title comes
// back when the list goes.
static void on_load(UIElement* el, void* user) {
    (void)el;
    Title* t = user;
    if (t->count == 0)
        return;
    if (ui_top(t->ui) == t->main)
        ui_pop(t->ui);
    ui_push(t->ui, t->games);
    ui_set_focus(t->games, t->rows[0]);
}

static void on_row(UIElement* el, void* user) {
    Title* t = user;
    for (int i = 0; i < t->count; i++)
        if (t->rows[i] == el)
            choose(t, TITLE_LOAD, &t->slots[i]);
}

// The question goes either way; title_input puts the list back in its place, on the row asked
// about or the one that took its place, or the title when no game is left.
static void on_no(UIElement* el, void* user) {
    (void)el;
    Title* t = user;
    ui_pop(t->ui);
}

static void on_yes(UIElement* el, void* user) {
    (void)el;
    Title* t = user;
    if (t->doomed >= 0 && t->doomed < t->count)
        save_game_delete(t->slots[t->doomed].name);
    ui_pop(t->ui);
    refresh(t);
    ui_set_text(t->note, "");
    t->doomed = t->count == 0 ? -1 : t->doomed < t->count ? t->doomed : t->count - 1;
}

// ------------------------------------------------------------------------------- the screens

void title_start(Title* t, UISystem* ui) {
    memset(t, 0, sizeof(*t));
    t->ui = ui;
    t->doomed = -1;
    t->rig = create_camera_rig();
    if (t->rig)
        camera_rig_set_pose(t->rig, TITLE_EYE, TITLE_TARGET);
    if (!ui)
        return;

    UIElement* root = NULL;
    t->main = hud_modal_screen(ui, "title", TITLE_DIM, &root);
    UIElement* col = column(root, ITEM_WIDTH);
    words(col, "s i l e n t", WORDMARK_SIZE, WORDMARK_FG, 6.0f);
    t->resume = item(col, "Continue", ITEM_SIZE, on_resume, t);
    t->fresh = item(col, "New Game", ITEM_SIZE, on_fresh, t);
    t->load = item(col, "Load Game", ITEM_SIZE, on_load, t);
    t->note = words(col, "", ROW_SIZE, NOTE_FG, 1.0f);

    t->games = hud_modal_screen(ui, "games", LIST_DIM, &root);
    col = column(root, 2.0f * ITEM_WIDTH);
    words(col, "Load Game", ITEM_SIZE, WORDMARK_FG, 3.0f);
    for (int i = 0; i < SAVE_GAME_SLOTS; i++)
        t->rows[i] = item(col, "", ROW_SIZE, on_row, t);
    words(col, "Delete   Delete a game        Escape   Back", ROW_SIZE * 0.8f, ITEM_FG, 1.0f);

    t->confirm = hud_modal_screen(ui, "delete", LIST_DIM, &root);
    col = column(root, ITEM_WIDTH);
    words(col, "Delete this game?", ITEM_SIZE, WORDMARK_FG, 2.0f);
    t->asked = words(col, "", ROW_SIZE, ITEM_FG, 1.0f);
    t->no = item(col, "No", ITEM_SIZE, on_no, t);
    t->yes = item(col, "Yes", ITEM_SIZE, on_yes, t);
}

void title_frame(Title* t, Engine* engine) {
    if (t->rig)
        engine_set_camera_rig(engine, t->rig);
}

void title_show(Title* t) {
    if (!t->ui)
        return;
    t->choice = TITLE_WAITING;
    t->up = true;
    refresh(t);
    ui_push(t->ui, t->main);
    ui_set_focus(t->main, t->count > 0 ? t->resume : t->fresh);
}

void title_open_screen(Title* t, const char* name) {
    if (!name || !t->ui || (strcmp(name, "games") && strcmp(name, "delete")))
        return;
    on_load(NULL, t);
    if (!strcmp(name, "delete"))
        title_input(t, true);
}

bool title_open(const Title* t) {
    const UIScreen* top = t->ui ? ui_top(t->ui) : NULL;
    return top && (top == t->main || top == t->games || top == t->confirm);
}

bool title_backs(const Title* t) {
    const UIScreen* top = t->ui ? ui_top(t->ui) : NULL;
    return top && (top == t->games || top == t->confirm);
}

void title_input(Title* t, bool delete_pressed) {
    if (!t->ui)
        return;
    // Each screen takes the last one's place rather than going over it, where it would show
    // through. The question gone, the list comes back on the row it was about; the list gone, or
    // its last game deleted, the title on Load Game while there is something to load.
    if (t->up && !title_open(t)) {
        if (t->doomed >= 0 && t->doomed < t->count) {
            ui_push(t->ui, t->games);
            ui_set_focus(t->games, t->rows[t->doomed]);
        } else {
            ui_push(t->ui, t->main);
            ui_set_focus(t->main, t->count > 0 ? t->load : t->fresh);
        }
        t->doomed = -1;
    }
    if (!delete_pressed || ui_top(t->ui) != t->games)
        return;
    const UIElement* focused = ui_focused(t->games);
    for (int i = 0; i < t->count; i++) {
        if (t->rows[i] != focused)
            continue;
        t->doomed = i;
        ui_set_text(t->asked, t->labels[i]);
        ui_pop(t->ui);
        ui_push(t->ui, t->confirm);
        ui_set_focus(t->confirm, t->no);
        return;
    }
}

TitleChoice title_choice(const Title* t) {
    return t->choice;
}

void title_refused(Title* t) {
    title_show(t);
    ui_set_text(t->note, "That game could not be read.");
}

void title_free(Title* t) {
    free_camera_rig(t->rig);
    memset(t, 0, sizeof(*t));
}
