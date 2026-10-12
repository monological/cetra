#include "title.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "hud.h"

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

static const vec4 NOTE_FG = {0.80f, 0.78f, 0.70f, 1.0f};

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
    snprintf(out, cap, "%s      %s      %s", s->where, when, played);
}

// The games kept, read again, onto the title's items and the list's rows.
static void refresh(Title* t) {
    t->count = save_game_list(t->slots);
    for (int i = 0; i < SAVE_GAME_SLOTS; i++) {
        char label[96] = "";
        if (i < t->count)
            slot_label(&t->slots[i], label, sizeof(label));
        ui_set_text(t->rows[i], label);
        hud_item_shown(t->rows[i], i < t->count);
    }
    hud_item_shown(t->resume, t->count > 0);
    t->load->disabled = t->count == 0;
}

// The game the list is on, or -1.
static int focused_row(const Title* t) {
    const UIElement* focused = ui_focused(t->games);
    for (int i = 0; i < t->count; i++)
        if (t->rows[i] == focused)
            return i;
    return -1;
}

// `choice` made, with the slot it is about, if any, and the title's screens taken down.
static void choose(Title* t, TitleChoice choice, const SaveSlot* slot) {
    if (slot)
        t->chosen = *slot;
    t->choice = choice;
    while (title_open(t))
        ui_pop(t->ui);
}

static void on_resume(UIElement* el, void* user) {
    (void)el;
    Title* t = user;
    choose(t, TITLE_LOAD, &t->slots[0]);
}

static void on_fresh(UIElement* el, void* user) {
    (void)el;
    Title* t = user;
    SaveSlot slot;
    if (save_game_unused_slot(&slot)) {
        choose(t, TITLE_NEW, &slot);
        return;
    }
    char note[96];
    snprintf(note, sizeof(note), "%d games are kept. Delete one in Load Game.", SAVE_GAME_SLOTS);
    ui_set_text(t->note, note);
}

static void on_load(UIElement* el, void* user) {
    (void)el;
    Title* t = user;
    if (t->count == 0)
        return;
    ui_push(t->ui, t->games);
    ui_set_focus(t->games, t->rows[0]);
}

static void on_settings(UIElement* el, void* user) {
    (void)el;
    Title* t = user;
    settings_menu_show(t->settings);
}

static void on_exit(UIElement* el, void* user) {
    (void)el;
    Title* t = user;
    choose(t, TITLE_EXIT, NULL);
}

static void on_row(UIElement* el, void* user) {
    Title* t = user;
    for (int i = 0; i < t->count; i++)
        if (t->rows[i] == el)
            choose(t, TITLE_LOAD, &t->slots[i]);
}

// Back to the list, on the game asked about.
static void on_no(UIElement* el, void* user) {
    (void)el;
    Title* t = user;
    ui_pop(t->ui);
}

// Back to the list on the game that took the deleted one's place, or to the title when none is
// left.
static void on_yes(UIElement* el, void* user) {
    (void)el;
    Title* t = user;
    const int row = focused_row(t);
    if (row >= 0)
        save_game_delete(t->slots[row].name);
    ui_pop(t->ui);
    refresh(t);
    ui_set_text(t->note, "");
    if (t->count > 0) {
        ui_set_focus(t->games, t->rows[row >= 0 && row < t->count ? row : t->count - 1]);
    } else {
        ui_pop(t->ui);
        ui_set_focus(t->main, t->fresh);
    }
}

// ------------------------------------------------------------------------------- the screens

void title_start(Title* t, UISystem* ui, Engine* engine, SettingsMenu* settings) {
    memset(t, 0, sizeof(*t));
    t->ui = ui;
    t->settings = settings;
    t->rig = create_camera_rig();
    if (t->rig) {
        camera_rig_set_pose(t->rig, TITLE_EYE, TITLE_TARGET);
        engine_set_camera_rig(engine, t->rig);
    }
    if (!ui)
        return;

    UIElement* root = NULL;
    t->main = hud_modal_screen(ui, "title", TITLE_DIM, &root);
    UIElement* col = hud_column(root, ITEM_WIDTH);
    hud_words(col, "s i l e n t", WORDMARK_SIZE, HUD_HEADING_FG, 6.0f);
    t->resume = hud_item(col, "Continue", ITEM_SIZE, on_resume, t);
    t->fresh = hud_item(col, "New Game", ITEM_SIZE, on_fresh, t);
    t->load = hud_item(col, "Load Game", ITEM_SIZE, on_load, t);
    hud_item(col, "Settings", ITEM_SIZE, on_settings, t);
    hud_item(col, "Exit to Desktop", ITEM_SIZE, on_exit, t);
    t->note = hud_words(col, "", ROW_SIZE, NOTE_FG, 1.0f);

    // The list and the question each take the place of what they are opened over, which would
    // otherwise show through.
    t->games = hud_modal_screen(ui, "games", LIST_DIM, &root);
    ui_screen_set_opaque(t->games, true);
    col = hud_column(root, 2.0f * ITEM_WIDTH);
    hud_words(col, "Load Game", ITEM_SIZE, HUD_HEADING_FG, 3.0f);
    for (int i = 0; i < SAVE_GAME_SLOTS; i++)
        t->rows[i] = hud_item(col, "", ROW_SIZE, on_row, t);
    hud_words(col, "Delete   Delete a game        Escape   Back", ROW_SIZE * 0.8f, HUD_ITEM_FG,
              1.0f);

    t->confirm = hud_modal_screen(ui, "delete", LIST_DIM, &root);
    ui_screen_set_opaque(t->confirm, true);
    col = hud_column(root, ITEM_WIDTH);
    hud_words(col, "Delete this game?", ITEM_SIZE, HUD_HEADING_FG, 2.0f);
    t->asked = hud_words(col, "", ROW_SIZE, HUD_ITEM_FG, 1.0f);
    t->no = hud_item(col, "No", ITEM_SIZE, on_no, t);
    t->yes = hud_item(col, "Yes", ITEM_SIZE, on_yes, t);
}

bool title_show(Title* t) {
    if (!t->ui)
        return false;
    t->choice = TITLE_WAITING;
    refresh(t);
    ui_push(t->ui, t->main);
    ui_set_focus(t->main, t->count > 0 ? t->resume : t->fresh);
    return true;
}

void title_open_screen(Title* t, const char* name) {
    if (!name || !t->ui || (strcmp(name, "games") && strcmp(name, "delete")))
        return;
    on_load(NULL, t);
    if (!strcmp(name, "delete"))
        title_ask_delete(t);
}

bool title_open(const Title* t) {
    const UIScreen* top = t->ui ? ui_top(t->ui) : NULL;
    return top && (top == t->main || top == t->games || top == t->confirm);
}

bool title_backs(const Title* t) {
    const UIScreen* top = t->ui ? ui_top(t->ui) : NULL;
    return top && (top == t->games || top == t->confirm);
}

void title_ask_delete(Title* t) {
    if (!t->ui || ui_top(t->ui) != t->games)
        return;
    const int row = focused_row(t);
    if (row < 0)
        return;
    ui_set_text(t->asked, t->rows[row]->text);
    ui_push(t->ui, t->confirm);
    ui_set_focus(t->confirm, t->no);
}

void title_refused(Title* t) {
    title_show(t);
    ui_set_text(t->note, "That game could not be read.");
}

void title_free(Title* t) {
    free_camera_rig(t->rig);
    memset(t, 0, sizeof(*t));
}
