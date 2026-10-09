#include <math.h>
#include <string.h>

#include "backpack_menu.h"
#include "hud.h"

// The pane, in window points: its share of the window's width up to a most, the share of its
// inside the chosen item's side takes, the picture's height over its width, and the most of the
// window's height the grid may take.
#define PANE_SHARE   0.9f
#define PANE_MAX     980.0f
#define DETAIL_SHARE 0.4f
#define VIEW_ASPECT  0.62f
#define GRID_ROOM    0.62f
#define PAD          22.0f
#define GAP          26.0f
#define BRACKET      14.0f // the frame's corner brackets' arms
#define ITEM_INSET   3.0f  // an item's footprint inside its cells
#define TURN_RATE    0.6f  // radians a second

#define TITLE_SIZE 26.0f
#define NAME_SIZE  22.0f
#define LINE_SIZE  16.0f
#define LINE_TRACK 0.5f
#define LINE_ROWS  4 // the line about an item has room for this many
#define HINT_SIZE  14.0f
#define SPACING    1.25f // between a label's lines

// Amber on near black, as Deus Ex draws it; the UI is in display values.
static const vec4 AMBER = {0.96f, 0.69f, 0.22f, 1.0f};
static const vec4 AMBER_DIM = {0.96f, 0.69f, 0.22f, 0.55f};
static const vec4 RULE = {0.96f, 0.69f, 0.22f, 0.35f};
static const vec4 GRID_LINE = {0.96f, 0.69f, 0.22f, 0.14f};
static const vec4 GRID_FILL = {0.96f, 0.69f, 0.22f, 0.03f};
static const vec4 TEXT = {0.93f, 0.87f, 0.74f, 1.0f};
static const vec4 PANE_BG = {0.015f, 0.014f, 0.012f, 0.9f};

// The grid's pictures side on, the long way across; the chosen one turning from there, a little
// from above.
static const ItemShot ICON = {0.5f * GLM_PIf, 0.35f, true};
#define TURNING_TILT 0.32f

static void line_quad(UIDrawList* dl, float x, float y, float w, float h, const vec4 colour) {
    ui_draw_quad(dl, (UIRect){x, y, w, h}, (float*)colour);
}

// The four corner brackets of a rect, `arm` long and `t` thick.
static void brackets(UIDrawList* dl, UIRect r, float arm, float t, const vec4 colour) {
    const float x1 = r.x + r.w, y1 = r.y + r.h;
    line_quad(dl, r.x, r.y, arm, t, colour);
    line_quad(dl, r.x, r.y, t, arm, colour);
    line_quad(dl, x1 - arm, r.y, arm, t, colour);
    line_quad(dl, x1 - t, r.y, t, arm, colour);
    line_quad(dl, r.x, y1 - t, arm, t, colour);
    line_quad(dl, r.x, y1 - arm, t, arm, colour);
    line_quad(dl, x1 - arm, y1 - t, arm, t, colour);
    line_quad(dl, x1 - t, y1 - arm, t, arm, colour);
}

static void draw_nothing(UIElement* el, UIDrawList* dl, void* user) {
    (void)el;
    (void)dl;
    (void)user;
}

// The pane: its ground, a hairline round it and brackets on its corners.
static void pane_draw(UIElement* el, UIDrawList* dl, void* user) {
    (void)user;
    const UIRect r = el->rect;
    ui_draw_quad(dl, r, (float*)PANE_BG);
    line_quad(dl, r.x, r.y, r.w, 1.0f, RULE);
    line_quad(dl, r.x, r.y + r.h - 1.0f, r.w, 1.0f, RULE);
    line_quad(dl, r.x, r.y, 1.0f, r.h, RULE);
    line_quad(dl, r.x + r.w - 1.0f, r.y, 1.0f, r.h, RULE);
    brackets(dl, ui_rect_inset(r, -3.0f, -3.0f, -3.0f, -3.0f), BRACKET, 2.0f, AMBER);
}

static void rule_draw(UIElement* el, UIDrawList* dl, void* user) {
    (void)user;
    line_quad(dl, el->rect.x, el->rect.y, el->rect.w, 1.0f, RULE);
}

// Where `item` lies in the grid drawn at `r`, inside its cells.
static UIRect footprint(const BackpackMenu* menu, UIRect r, ItemId item) {
    const float cell = r.w / (float)BAG_COLS;
    const UIRect cells = {r.x + cell * (float)menu->pack->cell[item][0],
                          r.y + cell * (float)menu->pack->cell[item][1],
                          cell * (float)ITEMS[item].cells[0], cell * (float)ITEMS[item].cells[1]};
    return ui_rect_inset(cells, ITEM_INSET, ITEM_INSET, ITEM_INSET, ITEM_INSET);
}

// The held items in the order the arrows walk them: by row, then column.
static int walk_order(const BackpackMenu* menu, ItemId* out) {
    const Backpack* pack = menu->pack;
    int n = 0;
    for (int row = 0; row < pack->rows; row++)
        for (int col = 0; col < BAG_COLS; col++)
            for (int k = 0; k < pack->held_count; k++)
                if (pack->cell[pack->held[k]][0] == col && pack->cell[pack->held[k]][1] == row)
                    out[n++] = pack->held[k];
    return n;
}

// A picture of `item` drawn into `r` by `view`, at the window's pixels.
static void picture(BackpackMenu* menu, UIDrawList* dl, ItemView* view, ItemId item,
                    const ItemShot* shot, UIRect r) {
    const int w = (int)lroundf(r.w * menu->scale), h = (int)lroundf(r.h * menu->scale);
    const Texture* t = item_view_draw(view, &menu->stage, menu->pack->models[item], shot, w, h);
    if (t)
        ui_draw_textured_quad(dl, r, t, (vec4){1.0f, 1.0f, 1.0f, 1.0f});
}

// The grid: faint cells, then each thing carried across its footprint, lit when the pointer or
// the arrows are on it and outlined when it is the one chosen.
static void grid_draw(UIElement* el, UIDrawList* dl, void* user) {
    BackpackMenu* menu = user;
    const UIRect r = el->rect;
    const float cell = r.w / (float)BAG_COLS;
    ui_draw_quad(dl, r, (float*)GRID_FILL);
    for (int c = 0; c <= BAG_COLS; c++)
        line_quad(dl, r.x + cell * (float)c - (c == BAG_COLS ? 1.0f : 0.0f), r.y, 1.0f, r.h,
                  GRID_LINE);
    for (int row = 0; row <= menu->pack->rows; row++)
        line_quad(dl, r.x, r.y + cell * (float)row - (row == menu->pack->rows ? 1.0f : 0.0f), r.w,
                  1.0f, GRID_LINE);
    ItemId order[ITEM_COUNT];
    const int n = walk_order(menu, order);
    for (int k = 0; k < n; k++) {
        const ItemId item = order[k];
        const UIRect at = footprint(menu, r, item);
        const bool chosen = menu->chosen == item;
        const bool lit = menu->hovered == item || menu->cursor == item;
        vec4 fill = {AMBER[0], AMBER[1], AMBER[2], chosen ? 0.22f : lit ? 0.15f : 0.07f};
        vec4 edge = {AMBER[0], AMBER[1], AMBER[2], chosen ? 1.0f : lit ? 0.8f : 0.35f};
        ui_draw_rounded(dl, at, 2.0f, fill, edge, chosen ? 1.5f : 1.0f);
        picture(menu, dl, &menu->icons[item], item, &ICON,
                ui_rect_inset(at, 4.0f, 4.0f, 4.0f, 4.0f));
    }
}

// The chosen item turning, in brackets that are there whether or not anything is chosen.
static void view_draw(UIElement* el, UIDrawList* dl, void* user) {
    BackpackMenu* menu = user;
    const UIRect r = el->rect;
    ui_draw_quad(dl, r, (float*)GRID_FILL);
    brackets(dl, r, BRACKET, 1.0f, RULE);
    if (menu->chosen == ITEM_NONE) {
        const UIStyle hint = {.fg = {AMBER_DIM[0], AMBER_DIM[1], AMBER_DIM[2], AMBER_DIM[3]},
                              .font_size = HINT_SIZE,
                              .tracking = 2.5f};
        const float lh = ui_line_height(menu->font, HINT_SIZE, 1.0f);
        ui_draw_text(dl, (UIRect){r.x, r.y + 0.5f * (r.h - lh), r.w, lh}, "SELECT AN ITEM", &hint,
                     UI_ALIGN_CENTER);
        return;
    }
    const ItemShot turning = {ICON.angle + menu->angle, TURNING_TILT, false};
    picture(menu, dl, &menu->turntable, menu->chosen, &turning, r);
}

static UIElement* label(UIElement* parent, const char* text, float size, const vec4 fg,
                        float tracking) {
    UIElement* el = ui_label(parent, text);
    const UIStyle s = {.fg = {fg[0], fg[1], fg[2], fg[3]},
                       .font_size = size,
                       .tracking = tracking,
                       .line_spacing = SPACING};
    ui_set_style(el, &s);
    return el;
}

// A container that draws nothing and has no padding of its own.
static UIElement* bare(UIElement* parent, UIDir dir, float spacing) {
    UIElement* el = ui_panel(parent);
    el->dir = dir;
    el->spacing = spacing;
    for (int i = 0; i < 4; i++)
        el->padding[i] = HUD_BARE;
    ui_set_draw(el, draw_nothing, NULL);
    return el;
}

// A hairline across its parent.
static void rule(UIElement* parent) {
    UIElement* el = bare(parent, UI_ROW, 0.0f);
    ui_set_size(el, UI_GROW, 0.0f, UI_FIXED, 1.0f);
    ui_set_draw(el, rule_draw, NULL);
}

bool backpack_menu_start(BackpackMenu* menu, UISystem* ui, Font* font, const Backpack* pack) {
    memset(menu, 0, sizeof(*menu));
    menu->chosen = menu->hovered = menu->cursor = ITEM_NONE;
    menu->scale = 1.0f;
    if (!ui || !font || !item_stage_start(&menu->stage))
        return false;
    menu->ui = ui;
    menu->font = font;
    menu->pack = pack;

    UIElement* root = NULL;
    menu->screen = hud_modal_screen(ui, "backpack", 0.5f, &root);
    menu->pane = bare(root, UI_COLUMN, 14.0f);
    for (int i = 0; i < 4; i++)
        menu->pane->padding[i] = PAD;
    ui_set_draw(menu->pane, pane_draw, NULL);
    label(menu->pane, "BACKPACK", TITLE_SIZE, AMBER, 5.0f);
    rule(menu->pane);
    UIElement* body = bare(menu->pane, UI_ROW, GAP);
    menu->grid = bare(body, UI_ROW, 0.0f);
    ui_set_draw(menu->grid, grid_draw, menu);
    menu->detail = bare(body, UI_COLUMN, 10.0f);
    menu->view = bare(menu->detail, UI_ROW, 0.0f);
    ui_set_draw(menu->view, view_draw, menu);
    // The name and the line hold their room while nothing is chosen, so choosing does not resize
    // the pane under the pointer.
    menu->name = label(menu->detail, "", NAME_SIZE, AMBER, 3.0f);
    ui_set_size(menu->name, UI_FIT, 0.0f, UI_FIXED,
                ui_line_height(font, NAME_SIZE, SPACING) + 8.0f);
    rule(menu->detail);
    menu->line = label(menu->detail, "", LINE_SIZE, TEXT, LINE_TRACK);
    ui_set_size(menu->line, UI_FIT, 0.0f, UI_FIXED,
                (float)LINE_ROWS * ui_line_height(font, LINE_SIZE, SPACING) + 8.0f);
    rule(menu->pane);
    label(menu->pane, "TAB  CLOSE        ARROWS  MOVE        ENTER  LOOK", HINT_SIZE, AMBER_DIM,
          2.5f);
    return true;
}

bool backpack_menu_open(const BackpackMenu* menu) {
    return menu->screen && ui_top(menu->ui) == menu->screen;
}

void backpack_menu_show(BackpackMenu* menu) {
    if (!menu->screen || backpack_menu_open(menu))
        return;
    ItemId order[ITEM_COUNT];
    menu->chosen = menu->hovered = ITEM_NONE;
    menu->cursor = walk_order(menu, order) > 0 ? order[0] : ITEM_NONE;
    ui_set_text(menu->name, "");
    ui_set_text(menu->line, "");
    ui_push(menu->ui, menu->screen);
}

// The line about the chosen item, broken at words to `width`.
static void wrap_line(BackpackMenu* menu, float width) {
    const char* text = ITEMS[menu->chosen].line;
    char wrapped[512];
    size_t n = 0;
    while (*text && n + 2 < sizeof(wrapped)) {
        size_t cut = ui_text_wrap_point(menu->font, LINE_SIZE, LINE_TRACK, text, width);
        cut = cut > 0 ? cut : strlen(text);
        for (size_t i = 0; i < cut && n + 2 < sizeof(wrapped); i++)
            wrapped[n++] = text[i];
        text += cut;
        while (*text == ' ')
            text++;
        if (*text)
            wrapped[n++] = '\n';
    }
    wrapped[n] = '\0';
    ui_set_text(menu->line, wrapped);
    menu->wrapped_at = width;
}

void backpack_menu_choose(BackpackMenu* menu, ItemId item) {
    if (!menu->screen || item < 0 || item >= ITEM_COUNT || !backpack_holds(menu->pack, item))
        return;
    if (menu->chosen != item)
        menu->angle = 0.0f;
    menu->chosen = item;
    ui_set_text(menu->name, ITEMS[item].name);
    menu->wrapped_at = 0.0f;
}

void backpack_menu_layout(BackpackMenu* menu, float width, float height, float scale) {
    menu->scale = scale > 0.0f ? scale : 1.0f;
    if (!menu->screen)
        return;
    // The pane to the window: the grid's cells square and the bag's rows tall, the chosen item's
    // side beside it, both no taller than the window leaves.
    const float pane = fminf(PANE_SHARE * width, PANE_MAX);
    const float inside = pane - 2.0f * PAD - GAP;
    float detail = DETAIL_SHARE * inside;
    float cell = (inside - detail) / (float)BAG_COLS;
    const float room = GRID_ROOM * height;
    const float rows = (float)menu->pack->rows;
    if (cell * rows > room) {
        cell = room / rows;
        detail = inside - cell * (float)BAG_COLS;
    }
    ui_set_size(menu->pane, UI_FIXED, pane, UI_FIT, 0.0f);
    ui_set_size(menu->grid, UI_FIXED, cell * (float)BAG_COLS, UI_FIXED, cell * rows);
    ui_set_size(menu->detail, UI_FIXED, detail, UI_FIT, 0.0f);
    ui_set_size(menu->view, UI_FIXED, detail, UI_FIXED, VIEW_ASPECT * detail);
}

void backpack_menu_input(BackpackMenu* menu, const UIInput* in) {
    if (!backpack_menu_open(menu))
        return;
    ItemId order[ITEM_COUNT];
    const int n = walk_order(menu, order);
    menu->hovered = ITEM_NONE;
    for (int k = 0; k < n; k++)
        if (ui_rect_hit(footprint(menu, menu->grid->rect, order[k]), in->pointer_x,
                        in->pointer_y)) {
            menu->hovered = order[k];
            if (in->pointer_pressed) {
                menu->cursor = order[k];
                backpack_menu_choose(menu, order[k]);
            }
        }
    if (n == 0)
        return;
    int at = 0;
    for (int k = 0; k < n; k++)
        if (order[k] == menu->cursor)
            at = k;
    if (in->nav_left || in->nav_up)
        at = (at + n - 1) % n;
    if (in->nav_right || in->nav_down)
        at = (at + 1) % n;
    menu->cursor = order[at];
    if (in->accept)
        backpack_menu_choose(menu, menu->cursor);
}

void backpack_menu_update(BackpackMenu* menu, float dt) {
    if (menu->chosen == ITEM_NONE)
        return;
    // Wrapped here, once the pane has its width: a choice made before then has none to wrap to.
    if (menu->wrapped_at != menu->detail->size[0])
        wrap_line(menu, menu->detail->size[0]);
    if (backpack_menu_open(menu))
        menu->angle = fmodf(menu->angle + TURN_RATE * dt, 2.0f * GLM_PIf);
}

void backpack_menu_free(BackpackMenu* menu) {
    for (int i = 0; i < ITEM_COUNT; i++)
        item_view_free(&menu->icons[i]);
    item_view_free(&menu->turntable);
    item_stage_free(&menu->stage);
    memset(menu, 0, sizeof(*menu));
}
