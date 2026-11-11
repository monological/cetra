/*
 * The --ui-probe cases of spec 14.2, kept apart from the twelve of spec 12.2 in gametest.c: what
 * they measure is text and scrolling rather than a menu. Like those, they lay out, take input as a
 * struct of values and emit into the draw list, and never draw a frame.
 */

#include "ui_text_probe.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <GLFW/glfw3.h>

#include "cetra/engine.h"
#include "cetra/fuzzy.h"
#include "cetra/internal/ui_field.h" // a field's caret and rows, which the field arms read
#include "cetra/internal/ui_text.h"  // a label's lines, which the wrap arms count
#include "cetra/text.h"
#include "cetra/ui_table.h"

#define PROBE_SIZE 22.0f

// The probe's long text: plain words, so its breaks are a closed form of ui_text_wrap_point.
static const char* PARAGRAPH = "the quick brown fox jumps over the lazy dog and keeps on running "
                               "past the river and over the hill until the evening comes";

void ui_probe_rect(const char* which, const char* label, UIRect r) {
    printf("ui %s %s rect %.6f %.6f %.6f %.6f\n", which, label, (double)r.x, (double)r.y,
           (double)r.w, (double)r.h);
}

// A fixed-width panel with a wrapping label stretched across it.
static UIElement* wrap_fixture(UISystem* ui, UIElement** label, float width, const char* text) {
    UIScreen* s = ui_screen(ui, "wrap");
    UIElement* panel = ui_panel(ui_screen_root(s));
    panel->size_mode[0] = UI_FIXED;
    panel->size[0] = width;
    panel->padding[0] = panel->padding[1] = panel->padding[2] = panel->padding[3] = 10.0f;
    *label = ui_label(panel, text);
    (*label)->wrap = true;
    (*label)->size_mode[0] = UI_GROW;
    ui_push(ui, s);
    return panel;
}

// The lines ui_text_wrap_point breaks `text` into at `width`, the closed form a wrapping label's
// count is held to.
static int wrap_lines(const Font* font, const char* text, float width) {
    int lines = 0;
    for (const char* p = text; *p && lines < 256;) {
        const size_t n = ui_text_wrap_point(font, PROBE_SIZE, 0.0f, p, width);
        lines++;
        p += n ? n : 1;
        if (*p == '\n')
            p++;
    }
    return lines ? lines : 1;
}

// The widest word of `text`, measured one word at a time.
static float longest_word(const Font* font, const char* text) {
    float best = 0.0f;
    char word[128];
    for (const char* p = text; *p;) {
        size_t n = strcspn(p, " \n");
        if (n > 0 && n < sizeof(word)) {
            memcpy(word, p, n);
            word[n] = '\0';
            const float w = ui_text_width(font, PROBE_SIZE, 0.0f, word);
            best = w > best ? w : best;
        }
        p += n;
        if (*p)
            p++;
    }
    return best;
}

// The glyph quads the list holds, in order, written to `out`; their count.
static size_t glyph_quads(UISystem* ui, UIRect* rects, vec4* colors, size_t cap) {
    const UIDrawList* dl = ui_draw_list(ui);
    size_t n = 0;
    for (size_t i = 0; i < ui_draw_list_quad_count(dl) && n < cap; i++) {
        UIRect r;
        vec4 c;
        int kind = 0;
        ui_draw_list_quad(dl, i, &r, c, &kind);
        if (kind != 2)
            continue;
        rects[n] = r;
        glm_vec4_copy(c, colors[n]);
        n++;
    }
    return n;
}

static int probe_wrap_label(UISystem* ui, Font* font) {
    UIElement* label = NULL;
    wrap_fixture(ui, &label, 300.0f, PARAGRAPH);
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    ui_probe_rect("wrap-label", "label", label->rect);
    const float inner = 300.0f - 20.0f;
    printf("ui wrap-label inner width %.6f\n", (double)inner);
    printf("ui wrap-label lines count %.6f\n", (double)(label->lines ? label->lines->count : 0));
    printf("ui wrap-label lines want %.6f\n", (double)wrap_lines(font, PARAGRAPH, inner));
    printf("ui wrap-label line height %.6f\n", (double)ui_line_height(font, PROBE_SIZE, 1.0f));

    // The widest drawn line: every glyph's right edge, past the label's left, never over the width.
    ui_build(ui, 1280.0f, 720.0f, 0.0f);
    UIRect rects[512];
    vec4 colors[512];
    const size_t n = glyph_quads(ui, rects, colors, 512);
    float right = 0.0f;
    for (size_t i = 0; i < n; i++) {
        const float r = rects[i].x + rects[i].w - label->rect.x;
        right = r > right ? r : right;
    }
    printf("ui wrap-label drawn right %.6f\n", (double)right);
    ui_pop(ui);

    // Given no width, a wrapping label measures as its longest word.
    UIScreen* s = ui_screen(ui, "fit");
    UIElement* fit = ui_label(ui_screen_root(s), PARAGRAPH);
    fit->wrap = true;
    ui_layout(s, 1280.0f, 720.0f);
    printf("ui wrap-label fit width %.6f\n", (double)fit->rect.w);
    printf("ui wrap-label fit word %.6f\n", (double)longest_word(font, PARAGRAPH));
    return 0;
}

// A layout step's breaks and measures of the label so far.
static void say_cache(const char* step, const UIElement* label) {
    printf("ui wrap-cache %s wraps %.6f\n", step, (double)label->wraps);
    printf("ui wrap-cache %s measures %.6f\n", step, (double)label->measures);
}

static int probe_wrap_cache(UISystem* ui) {
    UIElement* label = NULL;
    UIElement* panel = wrap_fixture(ui, &label, 300.0f, PARAGRAPH);
    UIScreen* s = ui_top(ui);
    ui_layout(s, 1280.0f, 720.0f);
    say_cache("first", label);
    ui_layout(s, 1280.0f, 720.0f);
    ui_layout(s, 1280.0f, 720.0f);
    say_cache("again", label);
    panel->size[0] = 420.0f;
    ui_layout(s, 1280.0f, 720.0f);
    say_cache("wider", label);
    ui_set_text(label, "a new text, which breaks afresh");
    ui_layout(s, 1280.0f, 720.0f);
    say_cache("retext", label);
    // A style changes the face's size without touching the text: its key, not a call, drops both.
    ui_set_style(label, &(UIStyle){.font_size = 30.0f});
    ui_layout(s, 1280.0f, 720.0f);
    say_cache("restyle", label);
    ui_pop(ui);
    return 0;
}

// A second face, wider than the first, for the runs that change it.
static Font* wide_font(Game* game) {
    Font* wide = load_font(game->engine->text_renderer->font_pool,
                           "apps/splash/assets/PressStart2P.ttf", 64.0f, true);
    if (!wide)
        fprintf(stderr, "ui-probe: could not load the second font\n");
    return wide;
}

static int probe_runs_draw(Game* game, UISystem* ui) {
    Font* wide = wide_font(game);
    if (!wide)
        return 1;

    // Colour: "b" and "c" red, the rest the label's own.
    UIScreen* s = ui_screen(ui, "runs");
    UIElement* label = ui_label(ui_screen_root(s), "abcd");
    const UITextRun red = {.start = 1, .length = 2, .color = {1.0f, 0.0f, 0.0f, 1.0f}};
    ui_set_text_runs(label, &red, 1);
    ui_push(ui, s);
    ui_build(ui, 1280.0f, 720.0f, 0.0f);
    UIRect rects[64];
    vec4 colors[64];
    size_t n = glyph_quads(ui, rects, colors, 64);
    printf("ui runs-draw glyphs count %.6f\n", (double)n);
    for (size_t i = 0; i < n; i++)
        printf("ui runs-draw glyph%zu color %.6f %.6f %.6f\n", i, (double)colors[i][0],
               (double)colors[i][1], (double)colors[i][2]);

    // Face: the same glyph is a different quad in another face.
    ui_set_text(label, "abcd");
    ui_build(ui, 1280.0f, 720.0f, 0.0f);
    n = glyph_quads(ui, rects, colors, 64);
    const float plain_w = n > 3 ? rects[3].w : 0.0f;
    const UITextRun face = {.start = 3, .length = 1, .font = wide};
    ui_set_text_runs(label, &face, 1);
    ui_build(ui, 1280.0f, 720.0f, 0.0f);
    n = glyph_quads(ui, rects, colors, 64);
    const float face_w = n > 3 ? rects[3].w : 0.0f;
    printf("ui runs-draw face widths %.6f %.6f\n", (double)plain_w, (double)face_w);
    ui_pop(ui);
    return 0;
}

// Wrapping across a change of face: a wide face takes more of the line, and nothing it draws
// reaches past the label.
static int probe_runs_wrap(Game* game, UISystem* ui, Font* font) {
    Font* wide = wide_font(game);
    if (!wide)
        return 1;
    UIRect rects[64];
    vec4 colors[64];
    const char* words = "aaaa bbbb cccc dddd eeee";
    const float width = ui_text_width(font, PROBE_SIZE, 0.0f, "aaaa bbbb cccc") + 4.0f;
    UIElement* wrapped = NULL;
    wrap_fixture(ui, &wrapped, width + 20.0f, words);
    UIScreen* ws = ui_top(ui);
    ui_layout(ws, 1280.0f, 720.0f);
    const size_t plain_lines = wrapped->lines ? wrapped->lines->count : 0;
    const UITextRun wide_word = {.start = 5, .length = 4, .font = wide};
    ui_set_text_runs(wrapped, &wide_word, 1);
    ui_build(ui, 1280.0f, 720.0f, 0.0f);
    const size_t run_lines = wrapped->lines ? wrapped->lines->count : 0;
    const size_t n = glyph_quads(ui, rects, colors, 64);
    float right = 0.0f;
    for (size_t i = 0; i < n; i++) {
        const float r = rects[i].x + rects[i].w - wrapped->rect.x;
        right = r > right ? r : right;
    }
    printf("ui runs-wrap lines plain %.6f\n", (double)plain_lines);
    printf("ui runs-wrap lines face %.6f\n", (double)run_lines);
    printf("ui runs-wrap drawn right %.6f\n", (double)right);
    printf("ui runs-wrap label width %.6f\n", (double)wrapped->rect.w);
    ui_pop(ui);
    return 0;
}

static int probe_runs_refused(UISystem* ui) {
    // "aé b": a at 0, é at 1 and 2, the space at 3, b at 4.
    UIElement* r = ui_label(ui_screen_root(ui_screen(ui, "refuse")), "a\xc3\xa9 b");
    const UITextRun ok[3] = {
        {.start = 0, .length = 1}, {.start = 1, .length = 2}, {.start = 4, .length = 1}};
    const UITextRun order[2] = {{.start = 3, .length = 1}, {.start = 0, .length = 1}};
    const UITextRun overlap[2] = {{.start = 0, .length = 3}, {.start = 1, .length = 2}};
    const UITextRun past[1] = {{.start = 4, .length = 5}};
    const UITextRun split[1] = {{.start = 2, .length = 1}};
    printf("ui runs-refused valid ok %.6f\n", ui_set_text_runs(r, ok, 3) ? 1.0 : 0.0);
    printf("ui runs-refused order ok %.6f\n", ui_set_text_runs(r, order, 2) ? 1.0 : 0.0);
    printf("ui runs-refused overlap ok %.6f\n", ui_set_text_runs(r, overlap, 2) ? 1.0 : 0.0);
    printf("ui runs-refused past ok %.6f\n", ui_set_text_runs(r, past, 1) ? 1.0 : 0.0);
    printf("ui runs-refused split ok %.6f\n", ui_set_text_runs(r, split, 1) ? 1.0 : 0.0);
    printf("ui runs-refused kept count %.6f\n", (double)r->run_count);
    ui_set_text(r, "fresh");
    printf("ui runs-refused retext count %.6f\n", (double)r->run_count);
    return 0;
}

// ---- the scrolling pane

#define PANE_ROWS 20
#define ROW_H     30.0f
#define ROW_GAP   8.0f

static int g_activated[PANE_ROWS + 4];

static void on_row(UIElement* el, void* user) {
    (void)el;
    g_activated[(int)(intptr_t)user]++;
}

// A 300x200 pane centred on a 1280x720 screen that is not modal, holding rows 30 points tall 8
// apart: buttons when `focusable`, which navigation and the pointer reach. `follow` is set before
// the first layout.
static UIElement* pane_fixture(UISystem* ui, UIElement** rows, int n, bool focusable, bool follow) {
    UIScreen* s = ui_screen(ui, "pane");
    ui_screen_set_modal(s, false);
    UIElement* pane = ui_scroll(ui_screen_root(s));
    ui_set_size(pane, UI_FIXED, 300.0f, UI_FIXED, 200.0f);
    pane->spacing = ROW_GAP;
    pane->follow_end = follow;
    for (int i = 0; i < n; i++) {
        char name[32];
        snprintf(name, sizeof(name), "Row %d", i);
        rows[i] = focusable ? ui_button(pane, name, on_row, (void*)(intptr_t)i) : ui_panel(pane);
        ui_set_size(rows[i], UI_GROW, 0.0f, UI_FIXED, ROW_H);
    }
    ui_push(ui, s);
    ui_layout(s, 1280.0f, 720.0f);
    return pane;
}

// One input pass with the pointer at (x, y) and nothing else, then the eases run out and the
// layout settles on what the pass decided.
static void pass(UISystem* ui, const UIInput* in) {
    ui_update(ui, in, 1280.0f, 720.0f);
    ui_build(ui, 1280.0f, 720.0f, 1.0f);
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
}

static void say_scroll(const char* which, const char* label, const UIElement* pane) {
    printf("ui %s %s scroll %.6f %.6f\n", which, label, (double)pane->scroll,
           (double)pane->scroll_target);
}

static int probe_scroll_layout(UISystem* ui) {
    UIElement* rows[PANE_ROWS];
    UIElement* pane = pane_fixture(ui, rows, PANE_ROWS, false, false);
    ui_probe_rect("scroll-layout", "pane", pane->rect);
    printf("ui scroll-layout content height %.6f\n", (double)pane->scroll_content);
    printf("ui scroll-layout rows at %.6f %.6f %.6f\n", (double)rows[0]->rect.y,
           (double)rows[1]->rect.y, (double)rows[3]->rect.y);
    const float cx = pane->rect.x + 100.0f, cy = pane->rect.y + 100.0f;
    pass(ui, &(UIInput){.pointer_x = cx, .pointer_y = cy, .wheel_y = -3.0f});
    say_scroll("scroll-layout", "wheeled", pane);
    printf("ui scroll-layout moved at %.6f %.6f %.6f\n", (double)rows[0]->rect.y,
           (double)rows[1]->rect.y, (double)rows[3]->rect.y);
    return 0;
}

static int probe_scroll_wheel(UISystem* ui) {
    UIElement* rows[PANE_ROWS];
    UIElement* pane = pane_fixture(ui, rows, PANE_ROWS, false, false);
    const float cx = pane->rect.x + 100.0f, cy = pane->rect.y + 100.0f;
    printf("ui scroll-wheel step points %.6f\n", (double)PROBE_SIZE);
    pass(ui, &(UIInput){.pointer_x = cx, .pointer_y = cy, .wheel_y = -3.0f});
    say_scroll("scroll-wheel", "three", pane);
    pass(ui, &(UIInput){.pointer_x = cx, .pointer_y = cy, .wheel_y = -1000.0f});
    say_scroll("scroll-wheel", "bottom", pane);
    pass(ui, &(UIInput){.pointer_x = cx, .pointer_y = cy, .wheel_y = 1000.0f});
    say_scroll("scroll-wheel", "top", pane);
    pass(ui, &(UIInput){.pointer_x = 20.0f, .pointer_y = 20.0f, .wheel_y = -3.0f});
    say_scroll("scroll-wheel", "outside", pane);
    ui_pop(ui);

    // A pane in a pane: the inner one under the pointer takes the wheel.
    UIScreen* s = ui_screen(ui, "nested");
    UIElement* outer = ui_scroll(ui_screen_root(s));
    ui_set_size(outer, UI_FIXED, 300.0f, UI_FIXED, 200.0f);
    UIElement* inner = ui_scroll(outer);
    ui_set_size(inner, UI_GROW, 0.0f, UI_FIXED, 100.0f);
    for (int i = 0; i < 10; i++)
        ui_set_size(ui_panel(inner), UI_GROW, 0.0f, UI_FIXED, ROW_H);
    for (int i = 0; i < 10; i++)
        ui_set_size(ui_panel(outer), UI_GROW, 0.0f, UI_FIXED, ROW_H);
    ui_push(ui, s);
    ui_layout(s, 1280.0f, 720.0f);
    pass(ui, &(UIInput){.pointer_x = inner->rect.x + 50.0f,
                        .pointer_y = inner->rect.y + 50.0f,
                        .wheel_y = -2.0f});
    say_scroll("scroll-wheel", "inner", inner);
    say_scroll("scroll-wheel", "outer", outer);
    return 0;
}

static int probe_scroll_clip(UISystem* ui) {
    UIElement* rows[PANE_ROWS];
    UIElement* pane = pane_fixture(ui, rows, PANE_ROWS, true, false);
    // A row in view takes a click; one laid out below the pane, under the point, does not.
    const float x = pane->rect.x + 100.0f;
    const float in_view = rows[2]->rect.y + ROW_H * 0.5f;
    const float out_of_view = rows[10]->rect.y + ROW_H * 0.5f;
    pass(ui,
         &(UIInput){
             .pointer_x = x, .pointer_y = in_view, .pointer_pressed = true, .pointer_down = true});
    pass(ui, &(UIInput){.pointer_x = x, .pointer_y = in_view, .pointer_released = true});
    pass(ui, &(UIInput){.pointer_x = x,
                        .pointer_y = out_of_view,
                        .pointer_pressed = true,
                        .pointer_down = true});
    pass(ui, &(UIInput){.pointer_x = x, .pointer_y = out_of_view, .pointer_released = true});
    printf("ui scroll-clip clicks in %.6f\n", (double)g_activated[2]);
    printf("ui scroll-clip clicks out %.6f\n", (double)g_activated[10]);
    printf("ui scroll-clip over in %.6f\n", ui_pointer_over(ui, x, in_view) ? 1.0 : 0.0);
    printf("ui scroll-clip over out %.6f\n", ui_pointer_over(ui, x, out_of_view) ? 1.0 : 0.0);

    // What is emitted: the rows the pane shows, and none wholly out of it.
    ui_build(ui, 1280.0f, 720.0f, 1.0f);
    const UIDrawList* dl = ui_draw_list(ui);
    int rows_drawn = 0, outside = 0;
    for (size_t i = 0; i < ui_draw_list_quad_count(dl); i++) {
        UIRect r;
        int kind = 0;
        ui_draw_list_quad(dl, i, &r, NULL, &kind);
        if (kind != 0 || r.h != ROW_H)
            continue;
        rows_drawn++;
        if (r.y >= pane->rect.y + pane->rect.h || r.y + r.h <= pane->rect.y)
            outside++;
    }
    printf("ui scroll-clip rows drawn %.6f\n", (double)rows_drawn);
    printf("ui scroll-clip rows outside %.6f\n", (double)outside);
    return 0;
}

static int probe_scroll_into_view(UISystem* ui) {
    UIElement* rows[PANE_ROWS];
    UIElement* pane = pane_fixture(ui, rows, PANE_ROWS, true, false);
    UIScreen* s = ui_top(ui);
    // The pointer moving onto row 5, which is half out of view, focuses it and scrolls nothing:
    // hovering is not navigating.
    pass(ui, &(UIInput){.pointer_x = pane->rect.x + 50.0f, .pointer_y = 0.0f});
    pass(ui, &(UIInput){.pointer_x = pane->rect.x + 50.0f, .pointer_y = rows[5]->rect.y + 4.0f});
    printf("ui scroll-into-view hover focused %.6f\n", ui_focused(s) == rows[5] ? 1.0 : 0.0);
    printf("ui scroll-into-view hover moved %.6f\n", (double)pane->scroll_target);
    ui_set_focus(s, NULL);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    // Down from nothing focused acquires row 0; five more presses reach row 5, the first not
    // wholly in view.
    for (int i = 0; i < 6; i++)
        pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f, .nav_down = true});
    printf("ui scroll-into-view focus row5 %.6f\n", ui_focused(s) == rows[5] ? 1.0 : 0.0);
    say_scroll("scroll-into-view", "down", pane);
    for (int i = 0; i < 5; i++)
        pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f, .nav_up = true});
    say_scroll("scroll-into-view", "up", pane);
    ui_set_focus(s, rows[15]);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    say_scroll("scroll-into-view", "set", pane);
    // The app shows row 2, above the view, with the focus left on row 15: its top comes into view.
    ui_scroll_into_view(ui, rows[2]);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    say_scroll("scroll-into-view", "app", pane);
    printf("ui scroll-into-view app focus %.6f\n", ui_focused(s) == rows[15] ? 1.0 : 0.0);
    return 0;
}

static int probe_scroll_follow(UISystem* ui) {
    UIElement* rows[PANE_ROWS];
    // Following from its first layout, the pane begins at its end.
    UIElement* pane = pane_fixture(ui, rows, PANE_ROWS, false, true);
    say_scroll("scroll-follow", "first", pane);
    printf("ui scroll-follow content height %.6f\n", (double)pane->scroll_content);
    ui_set_size(ui_panel(pane), UI_GROW, 0.0f, UI_FIXED, ROW_H);
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    say_scroll("scroll-follow", "grown", pane);
    printf("ui scroll-follow grown height %.6f\n", (double)pane->scroll_content);
    pass(ui, &(UIInput){.pointer_x = pane->rect.x + 100.0f,
                        .pointer_y = pane->rect.y + 100.0f,
                        .wheel_y = 3.0f});
    say_scroll("scroll-follow", "up", pane);
    ui_set_size(ui_panel(pane), UI_GROW, 0.0f, UI_FIXED, ROW_H);
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    say_scroll("scroll-follow", "kept", pane);
    return 0;
}

static int probe_scroll_bar(UISystem* ui) {
    UIElement* rows[PANE_ROWS];
    UIElement* pane = pane_fixture(ui, rows, PANE_ROWS, false, false);
    // The thumb, read from what the pane emits: the 6-point quad at its right edge.
    UIRect thumb = {0};
    const UIDrawList* dl = ui_draw_list(ui);
    ui_build(ui, 1280.0f, 720.0f, 1.0f);
    for (size_t i = 0; i < ui_draw_list_quad_count(dl); i++) {
        UIRect r;
        ui_draw_list_quad(dl, i, &r, NULL, NULL);
        if (r.w == 6.0f && r.x > pane->rect.x + pane->rect.w - 10.0f)
            thumb = r;
    }
    ui_probe_rect("scroll-bar", "pane", pane->rect);
    ui_probe_rect("scroll-bar", "thumb", thumb);
    printf("ui scroll-bar content height %.6f\n", (double)pane->scroll_content);
    const float tx = thumb.x + 3.0f, ty = thumb.y + thumb.h * 0.5f;
    pass(ui, &(UIInput){
                 .pointer_x = tx, .pointer_y = ty, .pointer_pressed = true, .pointer_down = true});
    pass(ui, &(UIInput){.pointer_x = tx, .pointer_y = ty + 50.0f, .pointer_down = true});
    say_scroll("scroll-bar", "dragged", pane);
    pass(ui, &(UIInput){.pointer_x = tx, .pointer_y = ty + 50.0f, .pointer_released = true});
    // A press on the track below the thumb pages down.
    const float before = pane->scroll_target;
    const float below = pane->rect.y + pane->rect.h - 4.0f;
    pass(ui,
         &(UIInput){
             .pointer_x = tx, .pointer_y = below, .pointer_pressed = true, .pointer_down = true});
    pass(ui, &(UIInput){.pointer_x = tx, .pointer_y = below, .pointer_released = true});
    printf("ui scroll-bar paged by %.6f\n", (double)(pane->scroll_target - before));
    return 0;
}

// ---- the text field

static InputKeyEvent g_events[256];
static int g_event_count;
static double g_clock; // the input's clock, a second a pass apart unless a test says otherwise
static int g_submits, g_changes, g_keys_taken;

static void ev_text(const char* s) {
    for (const char* p = s; *p && g_event_count < 256;)
        g_events[g_event_count++] = (InputKeyEvent){.codepoint = text_utf8_next(&p)};
}

static void ev_key(int key, int mods) {
    if (g_event_count < 256)
        g_events[g_event_count++] = (InputKeyEvent){.key = key, .mods = mods};
}

// One pass with the queued typing and the pointer off the screen; the queue is spent.
static void type_pass(UISystem* ui) {
    g_clock += 1.0;
    pass(ui, &(UIInput){.pointer_x = -1.0f,
                        .pointer_y = -1.0f,
                        .key_events = g_events,
                        .key_event_count = g_event_count,
                        .time = g_clock});
    g_event_count = 0;
}

static void one_key(UISystem* ui, int key, int mods) {
    ev_key(key, mods);
    type_pass(ui);
}

static void on_submit(UIElement* el, void* user) {
    (void)el;
    (void)user;
    g_submits++;
}

static void on_change(UIElement* el, void* user) {
    (void)el;
    (void)user;
    g_changes++;
}

static bool take_up(UIElement* el, const InputKeyEvent* key, void* user) {
    (void)el;
    (void)user;
    if (key->key != GLFW_KEY_UP)
        return false;
    g_keys_taken++;
    return true;
}

// A screen that is not modal holding a 240-point field, focused, and a button under it.
static UIElement* field_fixture(UISystem* ui, unsigned flags, UIElement** button) {
    UIScreen* s = ui_screen(ui, "field");
    ui_screen_set_modal(s, false);
    UIElement* panel = ui_panel(ui_screen_root(s));
    UIElement* field = ui_text_field(panel, flags, on_submit, NULL);
    ui_field_set_on_change(field, on_change, NULL);
    UIElement* b = ui_button(panel, "After", NULL, NULL);
    if (button)
        *button = b;
    ui_push(ui, s);
    ui_set_focus(s, field);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    return field;
}

static int cursor_of(const UIElement* field) {
    int c = -1;
    ui_field_inspect(field, &c, NULL, NULL, NULL, NULL, NULL);
    return c;
}

static void say_field(const char* which, const char* label, const UIElement* field) {
    int c = 0, a = 0, b = 0;
    ui_field_inspect(field, &c, &a, &b, NULL, NULL, NULL);
    printf("ui %s %s caret %d %d %d %zu\n", which, label, c, a, b, strlen(ui_field_text(field)));
}

// Whether the field's text is `want`, said as 1 or 0, since a probe line carries only numbers.
static void say_text(const char* which, const char* label, const UIElement* field,
                     const char* want) {
    printf("ui %s %s text %d\n", which, label, strcmp(ui_field_text(field), want) == 0 ? 1 : 0);
    if (strcmp(ui_field_text(field), want) != 0)
        fprintf(stderr, "ui-probe %s %s: text is '%s', want '%s'\n", which, label,
                ui_field_text(field), want);
}

static int probe_field_type(UISystem* ui) {
    const UIElement* field = field_fixture(ui, 0, NULL);
    ev_text("hello");
    type_pass(ui);
    say_text("field-type", "first", field, "hello");
    say_field("field-type", "first", field);
    printf("ui field-type first changes %d\n", g_changes);
    ev_text(" world");
    type_pass(ui);
    say_text("field-type", "second", field, "hello world");
    printf("ui field-type second changes %d\n", g_changes);
    // A pass that types nothing changes nothing.
    type_pass(ui);
    printf("ui field-type idle changes %d\n", g_changes);
    return 0;
}

static int probe_field_edit(UISystem* ui) {
    UIElement* field = field_fixture(ui, 0, NULL);
    ui_field_set_text(field, "hello world");
    one_key(ui, GLFW_KEY_LEFT, 0);
    one_key(ui, GLFW_KEY_LEFT, 0);
    printf("ui field-edit left caret %d\n", cursor_of(field));
    one_key(ui, GLFW_KEY_LEFT, INPUT_MOD_WORD);
    printf("ui field-edit word caret %d\n", cursor_of(field));
    one_key(ui, GLFW_KEY_HOME, 0);
    printf("ui field-edit home caret %d\n", cursor_of(field));
    one_key(ui, GLFW_KEY_END, 0);
    printf("ui field-edit end caret %d\n", cursor_of(field));
    one_key(ui, GLFW_KEY_BACKSPACE, 0);
    say_text("field-edit", "backspace", field, "hello worl");
    one_key(ui, GLFW_KEY_BACKSPACE, INPUT_MOD_WORD);
    say_text("field-edit", "wordback", field, "hello ");
    one_key(ui, GLFW_KEY_HOME, 0);
    one_key(ui, GLFW_KEY_DELETE, 0);
    say_text("field-edit", "delete", field, "ello ");
    printf("ui field-edit delete caret %d\n", cursor_of(field));
    return 0;
}

static int probe_field_utf8(UISystem* ui) {
    UIElement* field = field_fixture(ui, 0, NULL);
    ev_text("a\xc3\xa9\xce\xa9\xf0\x9f\x98\x80"); // a, é, Ω, 😀
    type_pass(ui);
    say_field("field-utf8", "typed", field);
    one_key(ui, GLFW_KEY_LEFT, 0);
    printf("ui field-utf8 left1 caret %d\n", cursor_of(field));
    one_key(ui, GLFW_KEY_LEFT, 0);
    printf("ui field-utf8 left2 caret %d\n", cursor_of(field));
    one_key(ui, GLFW_KEY_BACKSPACE, 0);
    say_field("field-utf8", "erased", field);
    // The text decodes whole: every codepoint printed, no U+FFFD among them.
    printf("ui field-utf8 erased codepoints");
    for (const char* p = ui_field_text(field); *p;)
        printf(" %u", (unsigned)text_utf8_next(&p));
    printf("\n");
    return 0;
}

// The x, in window points, of byte `at` of a single-line field's text: its content box's left plus
// the text's width up to there, as the field's own walk measures it.
static float field_x(const UIElement* field, Font* font, const char* text, size_t at) {
    char prefix[256];
    const size_t n = at < sizeof(prefix) - 1 ? at : sizeof(prefix) - 1;
    memcpy(prefix, text, n);
    prefix[n] = '\0';
    return field->rect.x + 10.0f + ui_text_width(font, PROBE_SIZE, 0.0f, prefix);
}

static int probe_field_select(Game* game, UISystem* ui, Font* font) {
    UIElement* field = field_fixture(ui, 0, NULL);
    const char* text = "alpha beta gamma";
    ui_field_set_text(field, text);
    one_key(ui, GLFW_KEY_HOME, 0);
    for (int i = 0; i < 5; i++)
        one_key(ui, GLFW_KEY_RIGHT, GLFW_MOD_SHIFT);
    say_field("field-select", "shift", field);
    one_key(ui, GLFW_KEY_A, INPUT_MOD_SHORTCUT);
    say_field("field-select", "all", field);

    // Two presses close together on "beta" select it; a third, the whole line.
    const float x = field_x(field, font, text, 8), y = field->rect.y + field->rect.h * 0.5f;
    for (int i = 0; i < 3; i++) {
        g_clock += 0.1;
        pass(ui, &(UIInput){.pointer_x = x,
                            .pointer_y = y,
                            .pointer_pressed = true,
                            .pointer_down = true,
                            .time = g_clock});
        pass(ui,
             &(UIInput){.pointer_x = x, .pointer_y = y, .pointer_released = true, .time = g_clock});
        say_field("field-select", i == 0 ? "click" : i == 1 ? "double" : "triple", field);
    }

    // A read-only field with runs selects and copies its text, plain.
    UIScreen* s = ui_top(ui);
    UIElement* ro = ui_text_field(ui_screen_root(s), UI_FIELD_READ_ONLY, NULL, NULL);
    ui_field_set_text(ro, "net SDA ok");
    const UITextRun run = {.start = 4, .length = 3, .color = {1.0f, 0.5f, 0.0f, 1.0f}};
    printf("ui field-select runs ok %d\n", ui_set_text_runs(ro, &run, 1) ? 1 : 0);
    ui_set_focus(s, ro);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    one_key(ui, GLFW_KEY_A, INPUT_MOD_SHORTCUT);
    one_key(ui, GLFW_KEY_C, INPUT_MOD_SHORTCUT);
    const char* clip = engine_clipboard_get(game->engine);
    printf("ui field-select copied plain %d\n", strcmp(clip, "net SDA ok") == 0 ? 1 : 0);
    // An editable field refuses runs.
    printf("ui field-select editable runs %d\n", ui_set_text_runs(field, &run, 1) ? 1 : 0);
    return 0;
}

static int probe_field_clipboard(const Game* game, UISystem* ui) {
    Engine* engine = game->engine;
    UIElement* field = field_fixture(ui, 0, NULL);
    ui_field_set_text(field, "hello world");
    for (int i = 0; i < 5; i++)
        one_key(ui, GLFW_KEY_LEFT, GLFW_MOD_SHIFT);
    one_key(ui, GLFW_KEY_C, INPUT_MOD_SHORTCUT);
    printf("ui field-clipboard copied clip %d\n",
           strcmp(engine_clipboard_get(engine), "world") == 0 ? 1 : 0);
    one_key(ui, GLFW_KEY_HOME, 0);
    one_key(ui, GLFW_KEY_V, INPUT_MOD_SHORTCUT);
    say_text("field-clipboard", "pasted", field, "worldhello world");
    // A single-line field takes a pasted newline as a space.
    engine_clipboard_set(engine, "a\nb");
    one_key(ui, GLFW_KEY_END, 0);
    one_key(ui, GLFW_KEY_V, INPUT_MOD_SHORTCUT);
    say_text("field-clipboard", "flattened", field, "worldhello worlda b");
    // Cut takes the selection to the clipboard.
    for (int i = 0; i < 3; i++)
        one_key(ui, GLFW_KEY_LEFT, GLFW_MOD_SHIFT);
    one_key(ui, GLFW_KEY_X, INPUT_MOD_SHORTCUT);
    say_text("field-clipboard", "cut", field, "worldhello world");
    printf("ui field-clipboard cutcopied clip %d\n",
           strcmp(engine_clipboard_get(engine), "a b") == 0 ? 1 : 0);
    // Read-only copies, and neither cuts nor pastes.
    UIScreen* s = ui_top(ui);
    UIElement* ro = ui_text_field(ui_screen_root(s), UI_FIELD_READ_ONLY, NULL, NULL);
    ui_field_set_text(ro, "fixed");
    ui_set_focus(s, ro);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    one_key(ui, GLFW_KEY_A, INPUT_MOD_SHORTCUT);
    one_key(ui, GLFW_KEY_X, INPUT_MOD_SHORTCUT);
    printf("ui field-clipboard rocopied clip %d\n",
           strcmp(engine_clipboard_get(engine), "fixed") == 0 ? 1 : 0);
    one_key(ui, GLFW_KEY_V, INPUT_MOD_SHORTCUT);
    ev_text("typed");
    type_pass(ui);
    say_text("field-clipboard", "readonly", ro, "fixed");
    return 0;
}

static int probe_field_undo(UISystem* ui) {
    const UIElement* field = field_fixture(ui, 0, NULL);
    ev_text("abc def");
    type_pass(ui);
    one_key(ui, GLFW_KEY_Z, INPUT_MOD_SHORTCUT);
    say_text("field-undo", "undo1", field, "abc");
    one_key(ui, GLFW_KEY_Z, INPUT_MOD_SHORTCUT);
    say_text("field-undo", "undo2", field, "");
    one_key(ui, GLFW_KEY_Z, INPUT_MOD_SHORTCUT | GLFW_MOD_SHIFT);
    say_text("field-undo", "redo1", field, "abc");
    one_key(ui, GLFW_KEY_Z, INPUT_MOD_SHORTCUT | GLFW_MOD_SHIFT);
    say_text("field-undo", "redo2", field, "abc def");
    return 0;
}

// The caret's quad in the list: the 1.5-point bar the focused field draws.
static bool caret_quad(UISystem* ui, UIRect* out) {
    const UIDrawList* dl = ui_draw_list(ui);
    for (size_t i = 0; i < ui_draw_list_quad_count(dl); i++) {
        UIRect r;
        int kind = 0;
        ui_draw_list_quad(dl, i, &r, NULL, &kind);
        if (kind == 0 && r.w == 1.5f) {
            *out = r;
            return true;
        }
    }
    return false;
}

static int probe_field_caret_x(UISystem* ui, Font* font) {
    UIElement* field = field_fixture(ui, 0, NULL);
    const char* text = "Wave AVATAR fly";
    ui_field_set_text(field, text);
    one_key(ui, GLFW_KEY_HOME, 0);
    const float scale = PROBE_SIZE / font->base_size;
    float worst = 0.0f;
    const size_t n = strlen(text);
    for (size_t i = 0; i <= n; i++) {
        ui_build(ui, 1280.0f, 720.0f, 0.0f);
        UIRect caret;
        if (!caret_quad(ui, &caret)) {
            printf("ui field-caret-x missing caret %zu\n", i);
            return 0;
        }
        // Where the glyph at i is drawn, its pen recovered from its quad; past the last, the
        // text's width.
        float pen;
        if (i < n) {
            UIRect rects[64];
            vec4 colors[64];
            size_t glyph = 0; // a space draws no quad, so the glyph's index skips them
            const size_t count = glyph_quads(ui, rects, colors, 64);
            for (size_t c = 0; c < i; c++)
                if (text[c] != ' ')
                    glyph++;
            if (text[i] == ' ') {
                one_key(ui, GLFW_KEY_RIGHT, 0);
                continue;
            }
            const GlyphInfo* g = font_get_glyph(font, (unsigned char)text[i]);
            pen = glyph < count ? rects[glyph].x - g->x0 * scale : 0.0f;
        } else {
            pen = field->rect.x + 10.0f + ui_text_width(font, PROBE_SIZE, 0.0f, text);
        }
        const float off = fabsf(caret.x - pen);
        worst = off > worst ? off : worst;
        one_key(ui, GLFW_KEY_RIGHT, 0);
    }
    printf("ui field-caret-x worst off %.6f\n", (double)worst);
    return 0;
}

static int probe_field_scroll_x(UISystem* ui) {
    UIElement* field = field_fixture(ui, 0, NULL);
    ev_text("the quick brown fox jumps over the lazy dog and keeps going");
    type_pass(ui);
    ui_build(ui, 1280.0f, 720.0f, 0.0f);
    UIRect caret = {0};
    caret_quad(ui, &caret);
    const float left = field->rect.x + 10.0f, right = field->rect.x + field->rect.w - 10.0f;
    printf("ui field-scroll-x content edges %.6f %.6f\n", (double)left, (double)right);
    printf("ui field-scroll-x end caret %.6f\n", (double)caret.x);
    float sx = 0.0f;
    ui_field_inspect(field, NULL, NULL, NULL, &sx, NULL, NULL);
    printf("ui field-scroll-x end scroll %.6f\n", (double)sx);
    one_key(ui, GLFW_KEY_HOME, 0);
    ui_build(ui, 1280.0f, 720.0f, 0.0f);
    caret_quad(ui, &caret);
    ui_field_inspect(field, NULL, NULL, NULL, &sx, NULL, NULL);
    printf("ui field-scroll-x home caret %.6f\n", (double)caret.x);
    printf("ui field-scroll-x home scroll %.6f\n", (double)sx);
    return 0;
}

static int probe_field_multiline(UISystem* ui, Font* font) {
    UIElement* field = field_fixture(ui, UI_FIELD_MULTILINE, NULL);
    field->max_rows = 3;
    ev_text("one two three four five six seven eight nine ten eleven twelve thirteen fourteen "
            "fifteen sixteen seventeen eighteen");
    type_pass(ui);
    size_t rows = 0;
    ui_field_inspect(field, NULL, NULL, NULL, NULL, NULL, &rows);
    printf("ui field-multiline rows count %zu\n", rows);
    ui_probe_rect("field-multiline", "field", field->rect);
    printf("ui field-multiline line height %.6f\n", (double)ui_line_height(font, PROBE_SIZE, 1.0f));
    // Up from the end keeps the caret's x, give or take a glyph.
    ui_build(ui, 1280.0f, 720.0f, 0.0f);
    UIRect before = {0}, after = {0};
    caret_quad(ui, &before);
    one_key(ui, GLFW_KEY_UP, 0);
    ui_build(ui, 1280.0f, 720.0f, 0.0f);
    caret_quad(ui, &after);
    printf("ui field-multiline up x %.6f %.6f\n", (double)before.x, (double)after.x);
    printf("ui field-multiline up dy %.6f\n", (double)(before.y - after.y));
    // Home goes to the wrapped row's start, not the text's: a word's start, after a space.
    one_key(ui, GLFW_KEY_HOME, 0);
    const int home = cursor_of(field);
    printf("ui field-multiline home caret %d\n", home);
    printf("ui field-multiline home space %d\n",
           home > 0 && ui_field_text(field)[home - 1] == ' ' ? 1 : 0);
    // End on a line ending in spaces goes past them, to its newline at byte 6.
    ui_field_set_text(field, "abc   \ndef");
    one_key(ui, GLFW_KEY_UP, 0);
    one_key(ui, GLFW_KEY_END, 0);
    printf("ui field-multiline end caret %d\n", cursor_of(field));
    // With no most, it grows to every row it has.
    const char* longer = "one two three four five six seven eight nine ten eleven twelve thirteen "
                         "fourteen fifteen sixteen seventeen eighteen nineteen twenty";
    field->max_rows = 0;
    ui_field_set_text(field, longer);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    ui_field_inspect(field, NULL, NULL, NULL, NULL, NULL, &rows);
    printf("ui field-multiline nomost rows %zu\n", rows);
    printf("ui field-multiline nomost height %.6f\n", (double)field->rect.h);
    // Held to three rows, Page Down moves the caret down by the three it shows.
    field->max_rows = 3;
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    one_key(ui, GLFW_KEY_HOME, GLFW_MOD_CONTROL);
    float scroll_before = 0.0f, scroll_after = 0.0f;
    ui_field_inspect(field, NULL, NULL, NULL, NULL, &scroll_before, NULL);
    ui_build(ui, 1280.0f, 720.0f, 0.0f);
    caret_quad(ui, &before);
    one_key(ui, GLFW_KEY_PAGE_DOWN, 0);
    ui_build(ui, 1280.0f, 720.0f, 0.0f);
    ui_field_inspect(field, NULL, NULL, NULL, NULL, &scroll_after, NULL);
    caret_quad(ui, &after);
    const float lh = ui_line_height(font, PROBE_SIZE, 1.0f);
    printf("ui field-multiline page rows %.6f\n",
           (double)(((after.y + scroll_after) - (before.y + scroll_before)) / lh));
    return 0;
}

// ---- the placeholder, and appending

static int probe_field_placeholder(UISystem* ui) {
    UIElement* field = field_fixture(ui, 0, NULL);
    const UIRect bare = field->rect;
    // A hint far longer than the field: drawn dim while the field is empty, and sizing nothing.
    ui_field_set_placeholder(field,
                             "a hint far longer than the field is wide, which sizes nothing");
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    printf("ui field-placeholder size same %d\n",
           field->rect.w == bare.w && field->rect.h == bare.h ? 1 : 0);
    UIRect rects[128];
    vec4 colors[128];
    ui_build(ui, 1280.0f, 720.0f, 0.0f);
    size_t n = glyph_quads(ui, rects, colors, 128), dim = 0;
    for (size_t i = 0; i < n; i++)
        dim += colors[i][3] < 0.6f;
    printf("ui field-placeholder empty glyphs %zu\n", n);
    printf("ui field-placeholder empty dim %zu\n", dim);
    // Typed into, the hint goes, and the text is drawn at full strength.
    ev_text("ok");
    type_pass(ui);
    ui_build(ui, 1280.0f, 720.0f, 0.0f);
    n = glyph_quads(ui, rects, colors, 128), dim = 0;
    for (size_t i = 0; i < n; i++)
        dim += colors[i][3] < 0.6f;
    printf("ui field-placeholder typed glyphs %zu\n", n);
    printf("ui field-placeholder typed dim %zu\n", dim);
    // A field's text is not ui_set_text's: refused, and neither the text nor the hint changes.
    ui_set_text(field, "replaced");
    printf("ui field-placeholder settext refused %d\n",
           strcmp(ui_field_text(field), "ok") == 0 && field->text == NULL ? 1 : 0);
    return 0;
}

// A pane's one row: a read-only, multi-line field holding "hello", as a history's message is.
static void build_append_row(UIElement* row, int index, void* user) {
    (void)index;
    (void)user;
    UIElement* f = ui_text_field(row, UI_FIELD_MULTILINE | UI_FIELD_READ_ONLY, NULL, NULL);
    ui_set_size(f, UI_GROW, 0.0f, UI_FIT, 0.0f);
    ui_field_set_text(f, "hello");
}

static int probe_field_append(Game* game, UISystem* ui) {
    UIElement* pane = pane_fixture(ui, NULL, 0, false, false);
    ui_scroll_set_rows(pane, 1, 30.0f, build_append_row, NULL);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    const UIElement* row = ui_scroll_find_row(pane, 0);
    printf("ui field-append found row %d\n", row && row->child_count ? 1 : 0);
    printf("ui field-append found none %d\n", ui_scroll_find_row(pane, 1) == NULL ? 1 : 0);
    if (!row || !row->child_count)
        return 1;
    UIElement* field = row->children[0];
    // "hello" selected, as a reader copying it would have it.
    ui_set_focus(ui_top(ui), field);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    one_key(ui, GLFW_KEY_A, INPUT_MOD_SHORTCUT);
    const unsigned builds = pane->row_builds;
    const float before = row->rect.h;
    // A word with a run of its own, then a sentence long enough to wrap onto rows of its own.
    const UITextRun lit = {.start = 1, .length = 5, .color = {1.0f, 0.5f, 0.0f, 1.0f}};
    const bool added = ui_field_append(field, " world", &lit, 1) &&
                       ui_field_append(field,
                                       " and then a sentence long enough to wrap the field onto "
                                       "rows of its own",
                                       NULL, 0);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    printf("ui field-append added ok %d\n", added ? 1 : 0);
    say_text("field-append", "joined", field,
             "hello world and then a sentence long enough to wrap the field onto rows of its own");
    int start = -1, end = -1;
    ui_field_inspect(field, NULL, &start, &end, NULL, NULL, NULL);
    printf("ui field-append selection kept %d\n", start == 0 && end == 5 ? 1 : 0);
    printf("ui field-append run start %zu\n",
           field->run_count ? field->runs[field->run_count - 1].start : (size_t)0);
    printf("ui field-append row builds %u\n", pane->row_builds - builds);
    printf("ui field-append row grew %d\n", row->rect.h > before ? 1 : 0);
    // Past the most, refused whole.
    const size_t most = (size_t)1 << 20;
    char* big = malloc(most + 1);
    if (big) {
        memset(big, 'x', most);
        big[most] = '\0';
        const size_t had = strlen(ui_field_text(field));
        printf("ui field-append toolong refused %d\n",
               !ui_field_append(field, big, NULL, 0) && strlen(ui_field_text(field)) == had ? 1
                                                                                            : 0);
        free(big);
    }
    // The runs changed to a wider face and appended to with no layout between: the rows are the
    // text's in the new face, as a field made beside it with the same text and runs breaks them
    // from scratch at its first layout.
    Font* wide = wide_font(game);
    if (wide) {
        size_t was = 0, rows = 0, fresh = 0;
        ui_field_inspect(field, NULL, NULL, NULL, NULL, NULL, &was);
        const UITextRun wider = {.start = 0,
                                 .length = strlen(ui_field_text(field)),
                                 .font = wide,
                                 .color = {1.0f, 1.0f, 1.0f, 1.0f}};
        ui_set_text_runs(field, &wider, 1);
        ui_field_append(field, ".", NULL, 0);
        UIElement* twin = ui_text_field(ui_scroll_find_row(pane, 0),
                                        UI_FIELD_MULTILINE | UI_FIELD_READ_ONLY, NULL, NULL);
        ui_set_size(twin, UI_GROW, 0.0f, UI_FIT, 0.0f);
        ui_field_set_text(twin, ui_field_text(field));
        ui_set_text_runs(twin, field->runs, field->run_count);
        pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
        ui_field_inspect(field, NULL, NULL, NULL, NULL, NULL, &rows);
        ui_field_inspect(twin, NULL, NULL, NULL, NULL, NULL, &fresh);
        printf("ui field-append restyled rows %zu %zu %zu\n", rows, fresh, was);
    }
    // An editable field stays in one style: an append with runs is refused.
    UIElement* editable = ui_text_field(ui_screen_root(ui_top(ui)), 0, NULL, NULL);
    printf("ui field-append editable refused %d\n",
           !ui_field_append(editable, "x", &lit, 1) && !ui_field_text(editable)[0] ? 1 : 0);
    return 0;
}

static int probe_field_keys(UISystem* ui) {
    UIElement* button = NULL;
    UIElement* field = field_fixture(ui, 0, &button);
    UIScreen* s = ui_top(ui);
    one_key(ui, GLFW_KEY_ENTER, 0);
    printf("ui field-keys enter submits %d\n", g_submits);
    // The key hook sees Up first and takes it; the caret stays.
    ui_field_set_text(field, "abc");
    ui_set_on_key(field, take_up, NULL);
    one_key(ui, GLFW_KEY_UP, 0);
    printf("ui field-keys hook taken %d\n", g_keys_taken);
    printf("ui field-keys hook caret %d\n", cursor_of(field));
    // Tab moves on to the button, Shift+Tab back.
    one_key(ui, GLFW_KEY_TAB, 0);
    printf("ui field-keys tab button %d\n", ui_focused(s) == button ? 1 : 0);
    one_key(ui, GLFW_KEY_TAB, GLFW_MOD_SHIFT);
    printf("ui field-keys backtab field %d\n", ui_focused(s) == field ? 1 : 0);
    // A key after a Tab in one pass goes where the Tab put the focus: past the button to a second
    // field, which takes the character.
    const UIElement* second = ui_text_field(field->parent, 0, NULL, NULL);
    ev_key(GLFW_KEY_TAB, 0);
    ev_key(GLFW_KEY_TAB, 0);
    ev_text("x");
    type_pass(ui);
    say_text("field-keys", "tabcarry", second, "x");
    ui_set_focus(s, field);
    type_pass(ui);
    // Esc in a screen that is not modal lets go of the focus, and a page key after it in the same
    // pass finds nothing focused.
    ev_key(GLFW_KEY_ESCAPE, 0);
    ev_key(GLFW_KEY_HOME, 0);
    type_pass(ui);
    printf("ui field-keys escape focus %d\n", ui_focused(s) == NULL ? 1 : 0);
    printf("ui field-keys escape onstack %d\n", ui_on_stack(ui, s) ? 1 : 0);
    ui_pop(ui);

    // A multi-line field that submits on Enter: Shift+Enter is a new line.
    UIScreen* chat = ui_screen(ui, "chat");
    UIElement* input = ui_text_field(
        ui_screen_root(chat), UI_FIELD_MULTILINE | UI_FIELD_SUBMIT_ON_ENTER, on_submit, NULL);
    input->max_rows = 6;
    ui_push(ui, chat);
    ui_set_focus(chat, input);
    // The keys of the pass the app focused it in came before it had the focus.
    ev_text("no");
    type_pass(ui);
    say_text("field-keys", "before", input, "");
    ev_text("hi");
    ev_key(GLFW_KEY_ENTER, GLFW_MOD_SHIFT);
    ev_text("there");
    type_pass(ui);
    say_text("field-keys", "newline", input, "hi\nthere");
    const int submits = g_submits;
    one_key(ui, GLFW_KEY_ENTER, 0);
    printf("ui field-keys chat submits %d\n", g_submits - submits);
    // Esc in a modal screen closes it.
    printf("ui field-keys modal before %d\n", ui_on_stack(ui, chat) ? 1 : 0);
    one_key(ui, GLFW_KEY_ESCAPE, 0);
    printf("ui field-keys modal after %d\n", ui_on_stack(ui, chat) ? 1 : 0);
    return 0;
}

static int probe_field_focus(UISystem* ui) {
    UIElement* button = NULL;
    UIElement* field = field_fixture(ui, 0, &button);
    UIScreen* s = ui_top(ui);
    printf("ui field-focus wants editing %d\n", ui_wants_keyboard(ui) ? 1 : 0);
    // The pointer passing over the button leaves the field focused.
    const float bx = button->rect.x + button->rect.w * 0.5f;
    const float by = button->rect.y + button->rect.h * 0.5f;
    pass(ui, &(UIInput){.pointer_x = bx, .pointer_y = by});
    pass(ui, &(UIInput){.pointer_x = bx + 1.0f, .pointer_y = by});
    printf("ui field-focus hover kept %d\n", ui_focused(s) == field ? 1 : 0);
    // A press on nothing lets go, and the keyboard is the app's again.
    pass(ui,
         &(UIInput){
             .pointer_x = 5.0f, .pointer_y = 5.0f, .pointer_pressed = true, .pointer_down = true});
    pass(ui, &(UIInput){.pointer_x = 5.0f, .pointer_y = 5.0f, .pointer_released = true});
    printf("ui field-focus nothing focus %d\n", ui_focused(s) == NULL ? 1 : 0);
    printf("ui field-focus wants after %d\n", ui_wants_keyboard(ui) ? 1 : 0);
    // A press on the field takes the focus back, and one on the button moves it there.
    const float fx = field->rect.x + 20.0f, fy = field->rect.y + field->rect.h * 0.5f;
    pass(ui, &(UIInput){
                 .pointer_x = fx, .pointer_y = fy, .pointer_pressed = true, .pointer_down = true});
    pass(ui, &(UIInput){.pointer_x = fx, .pointer_y = fy, .pointer_released = true});
    printf("ui field-focus click field %d\n", ui_focused(s) == field ? 1 : 0);
    pass(ui, &(UIInput){
                 .pointer_x = bx, .pointer_y = by, .pointer_pressed = true, .pointer_down = true});
    pass(ui, &(UIInput){.pointer_x = bx, .pointer_y = by, .pointer_released = true});
    printf("ui field-focus click button %d\n", ui_focused(s) == button ? 1 : 0);
    printf("ui field-focus wants button %d\n", ui_wants_keyboard(ui) ? 1 : 0);
    return 0;
}

// ---- virtual rows

static float g_built_h = 30.0f; // the height a button row is built at

static void build_button_row(UIElement* row, int index, void* user) {
    (void)user;
    char name[32];
    snprintf(name, sizeof(name), "Row %d", index);
    ui_set_size(ui_button(row, name, NULL, NULL), UI_GROW, 0.0f, UI_FIXED, g_built_h);
}

// A row of text one to four sentences long, so rows differ in height and a width changes them.
static void build_text_row(UIElement* row, int index, void* user) {
    (void)user;
    char text[512] = "";
    for (int i = 0; i <= index % 4; i++)
        strncat(text, "the quick brown fox jumps over the lazy dog ",
                sizeof(text) - strlen(text) - 1);
    UIElement* label = ui_label(row, text);
    label->wrap = true;
    label->size_mode[0] = UI_GROW;
}

// The row `index`'s element while it is built: its container's first child.
static UIElement* built_row(const UIElement* pane, int index) {
    const UIElement* row = ui_scroll_find_row(pane, index);
    return row && row->child_count ? row->children[0] : NULL;
}

static int probe_rows_built(UISystem* ui) {
    UIElement* pane = pane_fixture(ui, NULL, 0, false, false);
    ui_scroll_set_rows(pane, 100000, 30.0f, build_button_row, NULL);
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    printf("ui rows-built near held %zu\n", pane->child_count);
    printf("ui rows-built near builds %u\n", pane->row_builds);
    printf("ui rows-built near content %.6f\n", (double)pane->scroll_content);
    ui_scroll_to_row(ui, pane, 50000);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    printf("ui rows-built far held %zu\n", pane->child_count);
    printf("ui rows-built far builds %u\n", pane->row_builds);
    printf("ui rows-built far shown %d\n", built_row(pane, 50000) ? 1 : 0);
    // Laying out again with nothing moved builds nothing.
    const unsigned builds = pane->row_builds;
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    printf("ui rows-built again builds %u\n", pane->row_builds - builds);
    return 0;
}

static int probe_rows_measure(UISystem* ui) {
    UIElement* pane = pane_fixture(ui, NULL, 0, false, false);
    ui_scroll_set_rows(pane, 50, 30.0f, build_text_row, NULL);
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    const unsigned first = pane->row_builds;
    // Heights measured: the content is the built rows' heights, the rest at the estimate, and the
    // gaps.
    float built = 0.0f;
    for (size_t k = 0; k < pane->child_count; k++)
        built += pane->children[k]->rect.h;
    printf("ui rows-measure built count %zu\n", pane->child_count);
    printf("ui rows-measure built height %.6f\n", (double)built);
    printf("ui rows-measure content height %.6f\n", (double)pane->scroll_content);
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    printf("ui rows-measure again builds %u\n", pane->row_builds - first);
    // A new width measures the built rows again, without building them again.
    const float narrow = pane->scroll_content;
    pane->size[0] = 500.0f;
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    printf("ui rows-measure wider builds %u\n", pane->row_builds - first);
    printf("ui rows-measure wider content %.6f %.6f\n", (double)narrow,
           (double)pane->scroll_content);
    // A changed row is built again.
    ui_scroll_rows_changed(pane, 0, 1);
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    printf("ui rows-measure changed builds %u\n", pane->row_builds - first);
    return 0;
}

static int probe_rows_anchor(UISystem* ui) {
    UIElement* pane = pane_fixture(ui, NULL, 0, false, false);
    g_built_h = 60.0f; // built twice as tall as the estimate
    ui_scroll_set_rows(pane, 200, 30.0f, build_button_row, NULL);
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    ui_scroll_to_row(ui, pane, 100);
    for (int i = 0; i < 3; i++)
        pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    const UIElement* target = built_row(pane, 100);
    printf("ui rows-anchor reveal shown %d\n", target ? 1 : 0);
    if (target)
        printf("ui rows-anchor reveal bottoms %.6f %.6f\n",
               (double)(target->rect.y + target->rect.h), (double)(pane->rect.y + pane->rect.h));
    // The first row in view, and where it is.
    int first = -1;
    float before = 0.0f;
    for (int row = 0; row < 200 && first < 0; row++) {
        const UIElement* b = built_row(pane, row);
        if (b && b->rect.y + b->rect.h > pane->rect.y) {
            first = row;
            before = b->rect.y;
        }
    }
    // The row above it, built and out of view, built again half as tall again: the first row in
    // view does not move.
    g_built_h = 90.0f;
    ui_scroll_rows_changed(pane, 0, first);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    const UIElement* above = built_row(pane, first - 1);
    printf("ui rows-anchor above height %.6f\n", above ? (double)above->rect.h : -1.0);
    const UIElement* again = built_row(pane, first);
    printf("ui rows-anchor first moved %.6f\n", again ? (double)(again->rect.y - before) : -1.0);
    g_built_h = 30.0f;
    return 0;
}

static int probe_rows_focus(UISystem* ui) {
    UIElement* pane = pane_fixture(ui, NULL, 0, false, false);
    ui_scroll_set_rows(pane, 1000, 30.0f, build_button_row, NULL);
    UIScreen* s = ui_top(ui);
    ui_layout(s, 1280.0f, 720.0f);
    // The pointer onto the pane, where the wheel will turn, focuses the row under it -- a menu's
    // hover -- and holding still then leaves it focused.
    const float px = pane->rect.x + 50.0f, py = pane->rect.y + 50.0f;
    pass(ui, &(UIInput){.pointer_x = px, .pointer_y = py});
    const UIElement* focused = ui_focused(s);
    const int row = focused && focused->text ? atoi(focused->text + 4) : -1;
    printf("ui rows-focus first built %d\n", focused && focused == built_row(pane, row) ? 1 : 0);
    // Scrolled far away, the focused row stays built and focused.
    pass(ui, &(UIInput){.pointer_x = px, .pointer_y = py, .wheel_y = -1000.0f});
    printf("ui rows-focus far scroll %.6f\n", (double)pane->scroll);
    printf("ui rows-focus far kept %d\n",
           ui_focused(s) && ui_focused(s) == focused && focused == built_row(pane, row) ? 1 : 0);
    printf("ui rows-focus far held %zu\n", pane->child_count);
    return 0;
}

// ---- the matcher

static void say_match(const char* label, const char* query, const char* name) {
    FuzzyPositions matched;
    const int score = fuzzy_match(query, name, &matched);
    printf("ui match %s score %d\n", label, score);
    printf("ui match %s count %zu\n", label, matched.count);
    if (matched.count) {
        printf("ui match %s at", label);
        for (size_t i = 0; i < matched.count; i++)
            printf(" %zu", matched.at[i]);
        printf("\n");
    }
}

// ---- the most an element measures, and lengths that are none

// A panel `w` by `h` points, FIXED both ways, in `parent`.
static UIElement* fixed_panel(UIElement* parent, float w, float h) {
    UIElement* p = ui_panel(parent);
    ui_set_size(p, UI_FIXED, w, UI_FIXED, h);
    return p;
}

// A row 600 points across with no padding and no gaps, whose room is all its children's.
static UIElement* bare_row(UIElement* parent) {
    UIElement* row = fixed_panel(parent, 600.0f, 30.0f);
    row->dir = UI_ROW;
    for (int i = 0; i < 4; i++)
        row->padding[i] = -1.0f;
    row->spacing = -1.0f;
    return row;
}

static UIElement* growing(UIElement* parent, float most) {
    UIElement* c = ui_panel(parent);
    ui_set_size(c, UI_GROW, 0.0f, UI_FIXED, 20.0f);
    c->max_size[0] = most;
    return c;
}

static int probe_max_size(UISystem* ui) {
    UIScreen* s = ui_screen(ui, "max");
    ui_screen_set_modal(s, false);
    UIElement* root = ui_screen_root(s);
    // A fitting pane of 30 rows held to 200 points: that tall, and scrolling the rest.
    UIElement* pane = ui_scroll(root);
    ui_set_size(pane, UI_FIXED, 300.0f, UI_FIT, 0.0f);
    pane->max_size[1] = 200.0f;
    pane->spacing = ROW_GAP;
    for (int i = 0; i < 30; i++)
        ui_set_size(ui_panel(pane), UI_GROW, 0.0f, UI_FIXED, ROW_H);
    // Three growing children of a 600-point row, the first held to 100: it takes 100, and the
    // other two share the rest.
    UIElement* shared = bare_row(root);
    const UIElement* held = growing(shared, 100.0f);
    const UIElement* const rest[2] = {growing(shared, 0.0f), growing(shared, 0.0f)};
    // Two growing children both held to 100, centred: the room neither takes is the alignment's.
    UIElement* centred = bare_row(root);
    centred->align_main = UI_ALIGN_CENTER;
    const UIElement* const both[2] = {growing(centred, 100.0f), growing(centred, 100.0f)};
    // A fixed element ignores its most.
    UIElement* fixed = fixed_panel(root, 50.0f, 20.0f);
    fixed->max_size[0] = 10.0f;
    ui_push(ui, s);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    printf("ui max-size pane height %.6f\n", (double)pane->rect.h);
    printf("ui max-size pane content %.6f\n", (double)pane->scroll_content);
    pass(ui, &(UIInput){.pointer_x = pane->rect.x + 50.0f,
                        .pointer_y = pane->rect.y + 50.0f,
                        .wheel_y = -1.0f});
    printf("ui max-size pane scrolled %.6f\n", (double)pane->scroll);
    printf("ui max-size held width %.6f\n", (double)held->rect.w);
    printf("ui max-size rest widths %.6f %.6f\n", (double)rest[0]->rect.w, (double)rest[1]->rect.w);
    printf("ui max-size centred widths %.6f %.6f\n", (double)both[0]->rect.w,
           (double)both[1]->rect.w);
    printf("ui max-size centred offset %.6f\n", (double)(both[0]->rect.x - centred->rect.x));
    printf("ui max-size fixed width %.6f\n", (double)fixed->rect.w);
    return 0;
}

// A row `w` points across, 30 high, with 8 points of padding round it and 6 between its children.
static UIElement* share_row(UIElement* parent, float w) {
    UIElement* row = fixed_panel(parent, w, 30.0f);
    row->dir = UI_ROW;
    for (int i = 0; i < 4; i++)
        row->padding[i] = 8.0f;
    row->spacing = 6.0f;
    return row;
}

static UIElement* sharing(UIElement* parent, float share) {
    UIElement* c = ui_panel(parent);
    ui_set_size(c, UI_SHARE, share, UI_FIXED, 10.0f);
    return c;
}

static UIElement* grow_rest(UIElement* parent) {
    UIElement* c = ui_panel(parent);
    ui_set_size(c, UI_GROW, 0.0f, UI_FIXED, 10.0f);
    return c;
}

static void say_rect(const char* label, const char* key, const UIElement* el) {
    printf("ui share %s %s %.9g %.9g %.9g %.9g\n", label, key, (double)el->rect.x,
           (double)el->rect.y, (double)el->rect.w, (double)el->rect.h);
}

// UI_SHARE (spec 14.9): each case's elements as exact rects, which the gate holds to the rule.
static int probe_share(UISystem* ui) {
    UIScreen* s = ui_screen(ui, "share");
    ui_screen_set_modal(s, false);
    UIElement* root = ui_screen_root(s);
    // A share of 0.7 and a GROW side, at widths whose 0.7 is whole and is not.
    static const float widths[] = {600.0f, 601.0f, 333.5f};
    enum { SPLITS = sizeof(widths) / sizeof(widths[0]) };
    const UIElement* split[SPLITS][3];
    for (int i = 0; i < SPLITS; i++) {
        UIElement* row = share_row(root, widths[i]);
        split[i][0] = row;
        split[i][1] = sharing(row, 0.7f);
        split[i][2] = grow_rest(row);
    }
    // The GROW side a row of its own, measuring two children and its gap.
    UIElement* nested = share_row(root, 600.0f);
    const UIElement* nested_share = sharing(nested, 0.7f);
    UIElement* inner = ui_panel(nested);
    ui_set_size(inner, UI_GROW, 0.0f, UI_FIXED, 10.0f);
    inner->dir = UI_ROW;
    inner->spacing = 6.0f;
    fixed_panel(inner, 20.0f, 10.0f);
    fixed_panel(inner, 20.0f, 10.0f);
    // A share across the main axis: half of a 31-point height.
    UIElement* across_row = bare_row(root);
    across_row->size[1] = 31.0f;
    UIElement* across = ui_panel(across_row);
    ui_set_size(across, UI_FIXED, 40.0f, UI_SHARE, 0.5f);
    // A share held at its most.
    UIElement* held_row = share_row(root, 600.0f);
    UIElement* held = sharing(held_row, 0.7f);
    held->max_size[0] = 100.0f;
    const UIElement* held_rest = grow_rest(held_row);
    // Two halves of an odd length, and two shares past 1.
    UIElement* halves = share_row(root, 599.0f);
    const UIElement* const half[2] = {sharing(halves, 0.5f), sharing(halves, 0.5f)};
    UIElement* over = share_row(root, 600.0f);
    const UIElement* const past[2] = {sharing(over, 0.7f), sharing(over, 0.7f)};
    // A share under a parent that fits its children: half of what the parent comes to.
    UIElement* fit = ui_panel(root);
    ui_set_size(fit, UI_FIT, 0.0f, UI_FIXED, 30.0f);
    fit->dir = UI_ROW;
    for (int i = 0; i < 4; i++)
        fit->padding[i] = -1.0f;
    fit->spacing = -1.0f;
    fixed_panel(fit, 100.0f, 10.0f);
    const UIElement* fit_share = sharing(fit, 0.5f);
    // A share of nothing.
    UIElement* zero_row = share_row(root, 600.0f);
    const UIElement* zero = sharing(zero_row, 0.0f);
    const UIElement* zero_rest = grow_rest(zero_row);
    ui_push(ui, s);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    for (int i = 0; i < SPLITS; i++) {
        char label[16];
        snprintf(label, sizeof label, "split%d", i);
        say_rect(label, "row", split[i][0]);
        say_rect(label, "share", split[i][1]);
        say_rect(label, "grow", split[i][2]);
    }
    say_rect("nested", "row", nested);
    say_rect("nested", "share", nested_share);
    say_rect("nested", "grow", inner);
    say_rect("across", "row", across_row);
    say_rect("across", "share", across);
    say_rect("held", "row", held_row);
    say_rect("held", "share", held);
    say_rect("held", "grow", held_rest);
    say_rect("halves", "row", halves);
    say_rect("halves", "a", half[0]);
    say_rect("halves", "b", half[1]);
    say_rect("over", "row", over);
    say_rect("over", "a", past[0]);
    say_rect("over", "b", past[1]);
    say_rect("fit", "row", fit);
    say_rect("fit", "share", fit_share);
    say_rect("zero", "row", zero_row);
    say_rect("zero", "share", zero);
    say_rect("zero", "grow", zero_rest);
    return 0;
}

// The glyph rows of `el`'s text as built: their quads' vertical centres, each row once, in order.
// Every glyph of the probe's text is the same letter, so a row's quads share one centre.
static size_t glyph_rows(UISystem* ui, float* rows, size_t cap) {
    UIRect quads[64];
    vec4 colors[64];
    const size_t n = glyph_quads(ui, quads, colors, 64);
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        const float y = quads[i].y + quads[i].h * 0.5f;
        bool seen = false;
        for (size_t k = 0; k < count; k++)
            seen = seen || fabsf(rows[k] - y) < 0.5f;
        if (!seen && count < cap)
            rows[count++] = y;
    }
    return count;
}

// A screen holding one control, built alone, and its glyph rows.
static size_t built_rows(UISystem* ui, UIScreen* s, float* rows, size_t cap) {
    ui_push(ui, s);
    ui_build(ui, 1280.0f, 720.0f, 1.0f);
    const size_t n = glyph_rows(ui, rows, cap);
    ui_pop(ui);
    return n;
}

// Controls whose text is two lines draw it where their height was measured for it: a button's on
// its centre, as one line's is, and a slider's label above its track.
static int probe_control_lines(UISystem* ui) {
    float one[4], two[4], label[4];
    UIScreen* s1 = ui_screen(ui, "single");
    UIScreen* s2 = ui_screen(ui, "twice");
    UIScreen* s3 = ui_screen(ui, "slider");
    UIElement* single = ui_button(ui_screen_root(s1), "xx", NULL, NULL);
    UIElement* twice = ui_button(ui_screen_root(s2), "xx\nxx", NULL, NULL);
    static float value = 0.5f;
    UIElement* slider = ui_slider(ui_screen_root(s3), "xx\nxx", 0.0f, 1.0f, &value, NULL, NULL);
    const size_t n1 = built_rows(ui, s1, one, 4);
    const size_t n2 = built_rows(ui, s2, two, 4);
    const size_t n3 = built_rows(ui, s3, label, 4);
    // A row's centre less the button's: the same for one line and for two lines' middle.
    const float mid1 = single->rect.y + single->rect.h * 0.5f;
    const float mid2 = twice->rect.y + twice->rect.h * 0.5f;
    printf("ui control-lines rows count %zu %zu %zu\n", n1, n2, n3);
    if (n1 == 1 && n2 == 2) {
        printf("ui control-lines one offset %.6f\n", (double)(one[0] - mid1));
        printf("ui control-lines two offset %.6f\n", (double)((two[0] + two[1]) * 0.5f - mid2));
    }
    if (n3 == 2) {
        const UIRect track = ui_slider_track(ui, slider);
        printf("ui control-lines slider clear %.6f\n", (double)(track.y - label[1]));
    }
    return 0;
}

// A text mesh's per-character colour, by character: in "héllo" the é is character 1 of 5.
static int probe_text_mesh(Font* font) {
    TextMesh* mesh = create_text_mesh(font, "h\xc3\xa9llo", 32.0f);
    if (!mesh)
        return 1;
    printf("ui text-mesh chars count %zu\n", mesh->char_count);
    text_mesh_set_char_color(mesh, 1, (vec4){1.0f, 0.0f, 0.0f, 1.0f});
    text_mesh_set_char_color(mesh, 5, (vec4){0.0f, 0.0f, 1.0f, 1.0f}); // past the last: ignored
    text_mesh_rebuild(mesh);
    const size_t quads = mesh->vertex_count / 4;
    printf("ui text-mesh quads count %zu\n", quads);
    for (size_t q = 0; q < quads; q++)
        printf("ui text-mesh quad%zu green %.6f\n", q, (double)mesh->vertices[q * 4].g);
    free_text_mesh(mesh);
    return 0;
}

static int probe_padding_none(UISystem* ui) {
    UIScreen* s = ui_screen(ui, "padding");
    ui_screen_set_modal(s, false);
    UIElement* root = ui_screen_root(s);
    // Negative padding and spacing: none, so two 20-point children stack to 40 at the top-left.
    UIElement* none = ui_panel(root);
    for (int i = 0; i < 4; i++)
        none->padding[i] = -1.0f;
    none->spacing = -1.0f;
    const UIElement* first = fixed_panel(none, 20.0f, 20.0f);
    fixed_panel(none, 20.0f, 20.0f);
    // Zero: the style's padding and the theme's spacing, so the same two take more.
    UIElement* inherit = ui_panel(root);
    fixed_panel(inherit, 20.0f, 20.0f);
    fixed_panel(inherit, 20.0f, 20.0f);
    // Per side: none above and below, 6 either side.
    UIElement* sides = ui_panel(root);
    sides->padding[0] = sides->padding[2] = -1.0f;
    sides->padding[1] = sides->padding[3] = 6.0f;
    fixed_panel(sides, 20.0f, 20.0f);
    ui_push(ui, s);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    ui_probe_rect("padding-none", "none", none->rect);
    printf("ui padding-none first offset %.6f %.6f\n", (double)(first->rect.x - none->rect.x),
           (double)(first->rect.y - none->rect.y));
    ui_probe_rect("padding-none", "inherit", inherit->rect);
    ui_probe_rect("padding-none", "sides", sides->rect);
    return 0;
}

// One framebuffer pixel in points, the screen built `width` points across: the grid every glyph
// quad's corner is snapped to, how far a pen read back from a quad may lie from where it was.
static float probe_pixel(const Game* game, float width) {
    return game->engine->fb_width > 0 ? width / (float)game->engine->fb_width : 1.0f;
}

// The pen and baseline a built glyph quad of `c` at `size` was drawn from: its corner less the
// glyph's own offset from them.
static void quad_origin(const Font* font, UIRect quad, uint32_t c, float size, float* pen,
                        float* baseline) {
    const GlyphInfo* g = font_get_glyph((Font*)font, c);
    *pen = quad.x - g->x0 * (size / font->base_size);
    *baseline = quad.y - g->y0 * (size / font->base_size);
}

// A label drawn inside its padding: its first line's baseline its top padding and its face's ascent
// below its top, and its pen its left padding in from its left.
static int probe_label_padding(const Game* game, UISystem* ui, const Font* font) {
    const float width = 1280.0f;
    UIScreen* s = ui_screen(ui, "label-padding");
    UIElement* label = ui_label(ui_screen_root(s), "xxxx");
    label->padding[0] = 10.0f;
    label->padding[3] = 12.0f;
    ui_push(ui, s);
    ui_build(ui, width, 720.0f, 1.0f);
    UIRect quads[8];
    vec4 colors[8];
    const size_t n = glyph_quads(ui, quads, colors, 8);
    printf("ui label-padding glyphs count %zu\n", n);
    printf("ui label-padding pixel size %.6f\n", (double)probe_pixel(game, width));
    if (n > 0) {
        float pen, baseline;
        quad_origin(font, quads[0], 'x', PROBE_SIZE, &pen, &baseline);
        const float ascent = font->ascent * (PROBE_SIZE / font->base_size);
        printf("ui label-padding baseline off %.6f\n",
               (double)(baseline - (label->rect.y + 10.0f + ascent)));
        printf("ui label-padding pen off %.6f\n", (double)(pen - (label->rect.x + 12.0f)));
    }
    ui_pop(ui);
    return 0;
}

// A label of one "x" at `size`.
static UIElement* sized_x(UIElement* parent, float size) {
    UIElement* label = ui_label(parent, "x");
    ui_set_style(label, &(UIStyle){.font_size = size});
    return label;
}

// A row on the baseline.
static UIElement* lined_row(UIElement* parent) {
    UIElement* row = ui_panel(parent);
    row->dir = UI_ROW;
    row->align_cross = UI_ALIGN_BASELINE;
    return row;
}

// A row on the baseline: labels of three sizes, one in a padded column, two in a row on the
// baseline of its own, one after an icon with no text in another, and one with 40 points of padding
// under it, all on one line; a panel with no text at the row's top; the row's own baseline its
// line; and the row as tall as the lowest of its children reaches, which is the padded label, moved
// down onto the line.
static int probe_baseline(const Game* game, UISystem* ui, const Font* font) {
    const float width = 1280.0f;
    UIScreen* s = ui_screen(ui, "baseline");
    UIElement* row = lined_row(ui_screen_root(s));
    for (int i = 0; i < 4; i++)
        row->padding[i] = 4.0f;
    static const float SIZES[] = {14.0f, 28.0f, 10.0f, 12.0f, 20.0f, 16.0f, 10.0f};
    sized_x(row, SIZES[0]);
    sized_x(row, SIZES[1]);
    UIElement* column = ui_panel(row);
    for (int i = 0; i < 4; i++)
        column->padding[i] = 3.0f;
    sized_x(column, SIZES[2]);
    UIElement* inner = lined_row(row);
    sized_x(inner, SIZES[3]);
    sized_x(inner, SIZES[4]);
    UIElement* iconed = lined_row(row);
    fixed_panel(iconed, 12.0f, 12.0f);
    sized_x(iconed, SIZES[5]);
    UIElement* deep = sized_x(row, SIZES[6]);
    deep->padding[0] = 4.0f;
    deep->padding[2] = 40.0f;
    const UIElement* textless = fixed_panel(row, 10.0f, 10.0f);
    ui_push(ui, s);
    ui_build(ui, width, 720.0f, 1.0f);
    UIRect quads[8];
    vec4 colors[8];
    const size_t n = glyph_quads(ui, quads, colors, 8);
    printf("ui baseline glyphs count %zu\n", n);
    printf("ui baseline pixel size %.6f\n", (double)probe_pixel(game, width));
    float line = 0.0f;
    for (size_t i = 0; i < n && i < sizeof(SIZES) / sizeof(SIZES[0]); i++) {
        float pen;
        quad_origin(font, quads[i], 'x', SIZES[i], &pen, &line);
        printf("ui baseline glyph%zu line %.6f\n", i, (double)line);
    }
    printf("ui baseline row own %.6f %.6f\n", (double)(row->rect.y + row->baseline), (double)line);
    printf("ui baseline textless top %.6f\n", (double)(textless->rect.y - (row->rect.y + 4.0f)));
    float reach = 0.0f;
    for (size_t i = 0; i < row->child_count; i++) {
        const UIRect* r = &row->children[i]->rect;
        reach = glm_max(reach, r->y + r->h - row->rect.y);
    }
    printf("ui baseline row height %.6f %.6f\n", (double)row->rect.h, (double)(reach + 4.0f));
    ui_pop(ui);
    return 0;
}

// A baseline asked of the UI (spec 14.31): ui_line_ascent at 14 and 28 points against the face's
// ascent scaled to each; a label of each size with no padding, its glyph standing that far below
// its top; and with no face, none.
static int probe_line_ascent(const Game* game, UISystem* ui, const Font* font) {
    const float width = 1280.0f;
    static const float SIZES[] = {14.0f, 28.0f};
    enum { COUNT = sizeof(SIZES) / sizeof(SIZES[0]) };
    UIScreen* s = ui_screen(ui, "line-ascent");
    const UIElement* labels[COUNT];
    for (size_t i = 0; i < COUNT; i++) {
        UIElement* label = sized_x(ui_screen_root(s), SIZES[i]);
        for (int k = 0; k < 4; k++)
            label->padding[k] = -1.0f;
        labels[i] = label;
    }
    ui_push(ui, s);
    ui_build(ui, width, 720.0f, 1.0f);
    UIRect quads[COUNT];
    vec4 colors[COUNT];
    const size_t n = glyph_quads(ui, quads, colors, COUNT);
    printf("ui line-ascent glyphs count %zu\n", n);
    printf("ui line-ascent pixel size %.6f\n", (double)probe_pixel(game, width));
    for (size_t i = 0; i < COUNT; i++) {
        const float ascent = ui_line_ascent(font, SIZES[i]);
        printf("ui line-ascent face%zu off %.6f\n", i,
               (double)(ascent - font->ascent * (SIZES[i] / font->base_size)));
        if (i >= n)
            continue;
        float pen, baseline;
        quad_origin(font, quads[i], 'x', SIZES[i], &pen, &baseline);
        printf("ui line-ascent glyph%zu off %.6f\n", i,
               (double)(baseline - (labels[i]->rect.y + ascent)));
    }
    printf("ui line-ascent none face %.6f\n", (double)ui_line_ascent(NULL, SIZES[0]));
    ui_pop(ui);
    return 0;
}

// ---- the table (spec 14.30)

#define TABLE_ROWS 100000
#define TABLE_GAP  10.0f
#define TABLE_PAD  5.0f

// Column 0's widest cell, at row 90,000, far past any view, until it is shrunk; the next widest at
// row 50. Every other cell of column 0 is "name" and of the others "7".
static const char* WIDEST = "the widest name of them all";
static const char* SECOND = "the second widest name";
static bool g_widest_shrunk;
static int g_cells_asked; // the source's cell calls

static void table_cell(int row, int column, UITableCell* out, void* user) {
    (void)user;
    g_cells_asked++;
    out->text = "7";
    if (column == 0)
        out->text = row == 90000 && !g_widest_shrunk ? WIDEST : row == 50 ? SECOND : "name";
}

// A FIT column fitting its widest cell, a FIT column narrower than its title, a FIXED, a SHARE and
// two GROW columns.
static const UITableColumn TABLE_COLUMNS[] = {
    {.title = "Name", .size = UI_FIT},
    {.title = "A long column title", .size = UI_FIT},
    {.title = "Fixed", .size = UI_FIXED, .length = 80.0f},
    {.title = "Share", .size = UI_SHARE, .length = 0.3f},
    {.title = "Grow", .size = UI_GROW},
    {.title = "Grow", .size = UI_GROW},
};
#define TABLE_COLUMN_COUNT ((int)(sizeof(TABLE_COLUMNS) / sizeof(TABLE_COLUMNS[0])))

// A `w` by `h` table of `columns` and `rows` rows read through `source`, on a screen that is not
// modal, padded TABLE_PAD and its columns TABLE_GAP apart; not yet laid out.
static UIElement* table_fixture(UISystem* ui, const char* name, float w, float h,
                                const UITableColumn* columns, int column_count, int rows,
                                const UITableSource* source) {
    UIScreen* s = ui_screen(ui, name);
    ui_screen_set_modal(s, false);
    UIElement* table = ui_table(ui_screen_root(s));
    ui_set_size(table, UI_FIXED, w, UI_FIXED, h);
    table->spacing = TABLE_GAP;
    for (int i = 0; i < 4; i++)
        table->padding[i] = TABLE_PAD;
    ui_table_set_columns(table, columns, column_count);
    ui_table_set_rows(table, rows, source);
    ui_push(ui, s);
    return table;
}

// The top and the bottom of a fixture's content, inside its padding.
static float content_top(const UIElement* table) {
    return table->rect.y + TABLE_PAD;
}

static float content_bottom(const UIElement* table) {
    return table->rect.y + table->rect.h - TABLE_PAD;
}

// Row `row`'s cell rects, the whole row's first, as `which` `prefix`row and `prefix`c0 on.
static void say_cells(const char* which, const char* prefix, const UIElement* table, int row,
                      int columns) {
    for (int c = -1; c < columns; c++) {
        UIRect r;
        if (!ui_table_cell_rect(table, row, c, &r))
            continue;
        char label[32];
        if (c < 0)
            snprintf(label, sizeof(label), "%srow", prefix);
        else
            snprintf(label, sizeof(label), "%sc%d", prefix, c);
        ui_probe_rect(which, label, r);
    }
}

static int probe_table_columns(UISystem* ui, const Font* font) {
    static const float widths[] = {900.0f, 701.0f};
    for (int k = 0; k < 2; k++) {
        const UIElement* table =
            table_fixture(ui, "table-columns", widths[k], 300.0f, TABLE_COLUMNS, TABLE_COLUMN_COUNT,
                          TABLE_ROWS, &(UITableSource){.cell = table_cell});
        ui_layout(ui_top(ui), 1280.0f, 720.0f);
        char prefix[16];
        snprintf(prefix, sizeof(prefix), "w%d", (int)widths[k]);
        say_cells("table-columns", prefix, table, 0, TABLE_COLUMN_COUNT);
        ui_pop(ui);
    }
    printf("ui table-columns widest width %.6f\n",
           (double)ui_text_width(font, PROBE_SIZE, 0.0f, WIDEST));
    printf("ui table-columns title width %.6f\n",
           (double)ui_text_width(font, PROBE_SIZE, 0.0f, TABLE_COLUMNS[1].title));
    printf("ui table-columns gap points %.6f\n", (double)TABLE_GAP);
    return 0;
}

// Column 0's width as the last layout placed it.
static float first_width(const UIElement* table) {
    UIRect r = {0};
    ui_table_cell_rect(table, 0, 0, &r);
    return r.w;
}

static int probe_table_measure(UISystem* ui, const Font* font) {
    UIElement* table = table_fixture(ui, "table-measure", 900.0f, 300.0f, TABLE_COLUMNS, 4,
                                     TABLE_ROWS, &(UITableSource){.cell = table_cell});
    UIScreen* s = ui_top(ui);
    ui_layout(s, 1280.0f, 720.0f);
    printf("ui table-measure first measures %u\n", table->measures);
    unsigned was = table->measures;
    ui_layout(s, 1280.0f, 720.0f);
    printf("ui table-measure again measures %u\n", table->measures - was);
    ui_table_rows_changed(table, 5, 1);
    ui_layout(s, 1280.0f, 720.0f);
    printf("ui table-measure changed measures %u\n", table->measures - was);
    // The widest cell shrunk: column 0 narrows to the next widest.
    printf("ui table-measure widest width %.6f\n", (double)first_width(table));
    g_widest_shrunk = true;
    was = table->measures;
    ui_table_rows_changed(table, 90000, 1);
    ui_layout(s, 1280.0f, 720.0f);
    printf("ui table-measure shrunk measures %u\n", table->measures - was);
    printf("ui table-measure shrunk width %.6f\n", (double)first_width(table));
    printf("ui table-measure second width %.6f\n",
           (double)ui_text_width(font, PROBE_SIZE, 0.0f, SECOND));
    was = table->measures;
    ui_table_set_row_count(table, TABLE_ROWS + 10);
    ui_layout(s, 1280.0f, 720.0f);
    printf("ui table-measure grown measures %u\n", table->measures - was);
    // Shrunk to 5 rows, row 7 is gone at once, not only at the next layout.
    ui_table_set_row_count(table, 5);
    printf("ui table-measure shrunk row7 %d\n", ui_table_cell_rect(table, 7, -1, NULL) ? 1 : 0);
    return 0;
}

// Row 1's first cell three lines, every other cell one.
static void heights_cell(int row, int column, UITableCell* out, void* user) {
    (void)user;
    out->text = row == 1 && column == 0 ? "one\ntwo\nthree" : "one";
}

// Row `row` 50 points and one more for each row before it.
static float heights_height(int row, void* user) {
    (void)user;
    return 50.0f + (float)row;
}

static const UITableColumn HEIGHTS_COLUMNS[] = {
    {.title = "Words", .size = UI_FIT},
    {.size = UI_GROW},
};

static const UITableColumn UNTITLED_COLUMNS[] = {
    {.size = UI_FIT},
    {.size = UI_GROW},
};

// Rows 0 and 1's heights, how far row 1 starts past row 0's end, and row 0's top below the
// content's, as `prefix`.
static void say_heights(const UIElement* table, const char* prefix) {
    UIRect r0 = {0}, r1 = {0};
    ui_table_cell_rect(table, 0, -1, &r0);
    ui_table_cell_rect(table, 1, -1, &r1);
    printf("ui table-heights %s heights %.6f %.6f\n", prefix, (double)r0.h, (double)r1.h);
    printf("ui table-heights %s meet %.6f\n", prefix, (double)(r1.y - (r0.y + r0.h)));
    printf("ui table-heights %s header %.6f\n", prefix, (double)(r0.y - content_top(table)));
}

// The y of the header's rule as the table last drew it: the fill a pixel tall across its content.
static float rule_y(UISystem* ui, const UIElement* table, float pixel) {
    ui_build(ui, 1280.0f, 720.0f, 1.0f);
    const UIDrawList* dl = ui_draw_list(ui);
    const float across = table->rect.w - 2.0f * TABLE_PAD;
    for (size_t i = 0; i < ui_draw_list_quad_count(dl); i++) {
        UIRect r;
        int kind = 0;
        ui_draw_list_quad(dl, i, &r, NULL, &kind);
        if (kind == 0 && fabsf(r.h - pixel) < 1e-4f && fabsf(r.w - across) < 1e-4f)
            return r.y;
    }
    return -1.0f;
}

static int probe_table_heights(const Game* game, UISystem* ui, const Font* font) {
    UIElement* lined = table_fixture(ui, "table-heights", 600.0f, 300.0f, HEIGHTS_COLUMNS, 2, 2000,
                                     &(UITableSource){.cell = heights_cell});
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    say_heights(lined, "lined");
    // The rows scrolled 1,000 points by ten wheel units of 100: the header and its rule stay.
    const float pixel = probe_pixel(game, 1280.0f);
    printf("ui table-heights rule top %.6f\n",
           (double)(rule_y(ui, lined, pixel) - content_top(lined)));
    UITheme theme = *ui_theme(ui);
    theme.scroll_step = 100.0f;
    ui_set_theme(ui, &theme);
    pass(ui, &(UIInput){.pointer_x = lined->rect.x + 100.0f,
                        .pointer_y = lined->rect.y + 150.0f,
                        .wheel_y = -10.0f});
    printf("ui table-heights scrolled by %.6f\n", (double)lined->scroll);
    printf("ui table-heights scrolled rule %.6f\n",
           (double)(rule_y(ui, lined, pixel) - content_top(lined)));
    UIRect r = {0};
    ui_table_cell_rect(lined, 37, -1, &r);
    printf("ui table-heights scrolled row37 %.6f\n", (double)(r.y - content_top(lined)));
    printf("ui table-heights pixel points %.6f\n", (double)pixel);
    ui_pop(ui);
    const UIElement* own =
        table_fixture(ui, "table-heights-own", 600.0f, 300.0f, UNTITLED_COLUMNS, 2, 2000,
                      &(UITableSource){.cell = heights_cell, .height = heights_height});
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    say_heights(own, "own");
    printf("ui table-heights line height %.6f\n", (double)ui_line_height(font, PROBE_SIZE, 1.0f));
    printf("ui table-heights row padding %.6f\n", 2.0);
    return 0;
}

// Of 100,000 rows only those in view are drawn: the source's calls in a draw, against the rows the
// view holds; then row 50,000 scrolled to is in view.
static int probe_table_view(UISystem* ui) {
    UIElement* table =
        table_fixture(ui, "table-view", 900.0f, 300.0f, TABLE_COLUMNS, TABLE_COLUMN_COUNT,
                      TABLE_ROWS, &(UITableSource){.cell = table_cell});
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    UIRect row0 = {0};
    ui_table_cell_rect(table, 0, -1, &row0);
    g_cells_asked = 0;
    ui_build(ui, 1280.0f, 720.0f, 1.0f);
    printf("ui table-view drawn calls %d\n", g_cells_asked);
    printf("ui table-view text columns %d\n", TABLE_COLUMN_COUNT);
    printf("ui table-view row height %.6f\n", (double)row0.h);
    printf("ui table-view view height %.6f\n", (double)(content_bottom(table) - row0.y));
    ui_table_scroll_to_row(ui, table, 50000);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    UIRect far = {0};
    const bool shown = ui_table_cell_rect(table, 50000, -1, &far);
    printf("ui table-view far shown %d\n", shown ? 1 : 0);
    printf("ui table-view far bottom %.6f %.6f\n", (double)(far.y + far.h),
           (double)content_bottom(table));
    g_cells_asked = 0;
    ui_build(ui, 1280.0f, 720.0f, 1.0f);
    printf("ui table-view far calls %d\n", g_cells_asked);
    return 0;
}

// The cursor stands on no row that is a multiple of 4: not the first, 0, nor the last, 100.
static bool cursor_holds(int row, void* user) {
    (void)user;
    return row % 4 != 0;
}

static int g_cursor_calls;

static void on_cursor(UIElement* el, void* user) {
    (void)el;
    (void)user;
    g_cursor_calls++;
}

static bool take_down(UIElement* el, const InputKeyEvent* key, void* user) {
    (void)el;
    (void)user;
    return key->key == GLFW_KEY_DOWN;
}

// A table of 101 rows, focused, its cursor on none.
static UIElement* cursor_fixture(UISystem* ui) {
    UIElement* table =
        table_fixture(ui, "table-cursor", 900.0f, 300.0f, TABLE_COLUMNS, TABLE_COLUMN_COUNT, 101,
                      &(UITableSource){.cell = table_cell, .holds = cursor_holds});
    ui_table_set_on_cursor(table, on_cursor, NULL);
    ui_set_focus(ui_top(ui), table);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    return table;
}

// The cursor, whether its row is in view, and the on_cursor calls since the last say, as `label`.
static void say_cursor(const UIElement* table, const char* label) {
    const int row = ui_table_cursor(table);
    printf("ui table-cursor %s row %d\n", label, row);
    printf("ui table-cursor %s shown %d\n", label,
           ui_table_cell_rect(table, row, -1, NULL) ? 1 : 0);
    printf("ui table-cursor %s calls %d\n", label, g_cursor_calls);
    g_cursor_calls = 0;
}

// A press, and its release, at the middle of row `row`.
static void press_row(UISystem* ui, const UIElement* table, int row) {
    UIRect r = {0};
    ui_table_cell_rect(table, row, -1, &r);
    const float x = r.x + 50.0f, y = r.y + r.h * 0.5f;
    pass(ui,
         &(UIInput){.pointer_x = x, .pointer_y = y, .pointer_pressed = true, .pointer_down = true});
    pass(ui, &(UIInput){.pointer_x = x, .pointer_y = y, .pointer_released = true});
}

static int probe_table_cursor(UISystem* ui) {
    UIElement* table = cursor_fixture(ui);
    say_cursor(table, "none");
    one_key(ui, GLFW_KEY_DOWN, 0);
    say_cursor(table, "first");
    // Three Downs in one pass, over row 4, and one on_cursor for them.
    ev_key(GLFW_KEY_DOWN, 0);
    ev_key(GLFW_KEY_DOWN, 0);
    ev_key(GLFW_KEY_DOWN, 0);
    type_pass(ui);
    say_cursor(table, "down3");
    one_key(ui, GLFW_KEY_UP, 0);
    say_cursor(table, "up");
    one_key(ui, GLFW_KEY_END, 0);
    say_cursor(table, "end");
    one_key(ui, GLFW_KEY_END, 0);
    say_cursor(table, "endagain");
    one_key(ui, GLFW_KEY_HOME, 0);
    say_cursor(table, "home");
    // The view under a header a row tall, for the page Page Down moves by.
    UIRect r1 = {0};
    ui_table_cell_rect(table, 1, -1, &r1);
    printf("ui table-cursor row height %.6f\n", (double)r1.h);
    printf("ui table-cursor view height %.6f\n", (double)(table->rect.h - 2.0f * TABLE_PAD - r1.h));
    one_key(ui, GLFW_KEY_PAGE_DOWN, 0);
    say_cursor(table, "pagedown");
    ui_table_set_cursor(table, 50);
    pass(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f});
    say_cursor(table, "set");
    // Rows above the cursor's, in view: 49 holds it and 48 does not.
    press_row(ui, table, 49);
    say_cursor(table, "press");
    press_row(ui, table, 48);
    say_cursor(table, "pressno");
    // From none, Up lands on the last row that holds it.
    ui_table_set_cursor(table, -1);
    one_key(ui, GLFW_KEY_UP, 0);
    say_cursor(table, "upnone");
    // A host's move, outside a pass: told at once.
    ui_table_move_cursor(table, -2);
    say_cursor(table, "moved");
    // The hook is handed each key first: one taking Down leaves the cursor.
    ui_set_on_key(table, take_down, NULL);
    one_key(ui, GLFW_KEY_DOWN, 0);
    say_cursor(table, "hooked");
    return 0;
}

// ---- a table in a pane (spec 14.32)

#define PANE_TABLE_ROWS 40
#define PANE_TABLE_COLS 4
#define PANE_H          200.0f

// A pane PANE_H tall, none of it padding or border, over a row of a panel 100 points wide and a
// table of `rows` rows as tall as its rows, neither padded: on a screen of its own, pushed and laid
// out.
static UIElement* pane_table_fixture(UISystem* ui, const char* name, int rows, UIElement** pane,
                                     UIElement** side) {
    UIScreen* s = ui_screen(ui, name);
    ui_screen_set_modal(s, false);
    *pane = ui_scroll(ui_screen_root(s));
    ui_set_size(*pane, UI_FIXED, 600.0f, UI_FIXED, PANE_H);
    ui_set_style(*pane, &(UIStyle){.border = {1.0f, 1.0f, 1.0f, 0.0f}});
    UIElement* row = ui_panel(*pane);
    row->dir = UI_ROW;
    row->spacing = -1.0f;
    ui_set_size(row, UI_GROW, 0.0f, UI_FIT, 0.0f);
    *side = ui_panel(row);
    ui_set_size(*side, UI_FIXED, 100.0f, UI_GROW, 0.0f);
    UIElement* table = ui_table(row);
    ui_set_size(table, UI_GROW, 0.0f, UI_FIT, 0.0f);
    UIElement* bare[] = {*pane, row, *side, table};
    for (size_t e = 0; e < sizeof(bare) / sizeof(bare[0]); e++)
        for (int k = 0; k < 4; k++)
            bare[e]->padding[k] = -1.0f;
    ui_table_set_columns(table, TABLE_COLUMNS, PANE_TABLE_COLS);
    ui_table_set_rows(table, rows, &(UITableSource){.cell = table_cell});
    ui_push(ui, s);
    ui_layout(s, 1280.0f, 720.0f);
    return table;
}

// The pane scrolled to `at` at once, and laid out there.
static void pane_at(UISystem* ui, UIElement* pane, float at) {
    pane->scroll = pane->scroll_target = at;
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
}

/*
 * A table in a pane: the table as tall as its rows, the pane scrolling it beside a panel. Scrolled,
 * the header is held at the pane's top and a row is in view only below it; only the rows shown are
 * drawn; the wheel over the table and over the panel scrolls the pane; the cursor moved down past
 * the pane's foot is shown by the pane under the header, and Page Down moves by the rows the pane
 * shows. Then a pane centring a table shorter than it.
 */
static int probe_table_pane(UISystem* ui) {
    UIElement *pane, *side;
    UIElement* table = pane_table_fixture(ui, "table-pane", PANE_TABLE_ROWS, &pane, &side);
    UIRect head = {0}, row0 = {0};
    ui_table_header_rect(table, &head);
    ui_table_cell_rect(table, 0, -1, &row0);
    const float row_h = row0.h, header_h = head.h;
    printf("ui table-pane table height %.6f %.6f\n", (double)table->rect.h,
           (double)(header_h + PANE_TABLE_ROWS * row_h));
    printf("ui table-pane pane height %.6f\n", (double)PANE_H);
    // The wheel over the table, whose rows all show in it, scrolls the pane: three units of 100.
    UITheme theme = *ui_theme(ui);
    theme.scroll_step = 100.0f;
    ui_set_theme(ui, &theme);
    pass(ui, &(UIInput){.pointer_x = table->rect.x + 150.0f,
                        .pointer_y = pane->rect.y + 100.0f,
                        .wheel_y = -3.0f});
    printf("ui table-pane wheel table %.6f %.6f\n", (double)pane->scroll, (double)table->scroll);
    // Scrolled 300: the header held at the pane's top; each row in view exactly when some of it is
    // below the header's foot and above the pane's.
    pane_at(ui, pane, 300.0f);
    ui_table_header_rect(table, &head);
    printf("ui table-pane held top %.6f\n", (double)(head.y - pane->rect.y));
    const float header_foot = head.y + head.h, pane_foot = pane->rect.y + PANE_H;
    const float rows_top = table->rect.y + header_h; // where row 0 starts, scrolled
    int wrong = 0, shown = 0;
    for (int r = 0; r < PANE_TABLE_ROWS; r++) {
        const float top = rows_top + (float)r * row_h;
        const bool want = top + row_h > header_foot + 1e-3f && top < pane_foot - 1e-3f;
        const bool got = ui_table_cell_rect(table, r, -1, NULL);
        wrong += got != want;
        shown += want;
    }
    printf("ui table-pane held wrong %d\n", wrong);
    printf("ui table-pane held row0 %d\n", ui_table_cell_rect(table, 0, -1, NULL) ? 1 : 0);
    // Only the rows shown are drawn.
    g_cells_asked = 0;
    ui_build(ui, 1280.0f, 720.0f, 1.0f);
    printf("ui table-pane drawn calls %d\n", g_cells_asked);
    printf("ui table-pane drawn shown %d\n", shown);
    printf("ui table-pane drawn columns %d\n", PANE_TABLE_COLS);
    // The wheel over the panel beside it scrolls the pane too.
    pane_at(ui, pane, 0.0f);
    pass(ui, &(UIInput){.pointer_x = side->rect.x + 50.0f,
                        .pointer_y = pane->rect.y + 100.0f,
                        .wheel_y = -1.0f});
    printf("ui table-pane wheel side %.6f\n", (double)pane->scroll);
    // The cursor moved by keys from the first row to row 30: the pane is going where row 30 shows
    // whole under the header.
    pane_at(ui, pane, 0.0f);
    ui_set_focus(ui_top(ui), table);
    for (int k = 0; k < 31; k++)
        ev_key(GLFW_KEY_DOWN, 0);
    type_pass(ui);
    const float target = pane->scroll_target;
    const float top30 = header_h + 30.0f * row_h - target; // down the pane's view
    printf("ui table-pane reveal cursor %d\n", ui_table_cursor(table));
    printf("ui table-pane reveal under %.6f\n", (double)(top30 - header_h));
    printf("ui table-pane reveal foot %.6f\n", (double)(PANE_H - (top30 + row_h)));
    // Page Down moves by the rows the pane shows under the header.
    pane_at(ui, pane, target);
    one_key(ui, GLFW_KEY_PAGE_DOWN, 0);
    printf("ui table-pane page cursor %d %d\n", ui_table_cursor(table),
           30 + (int)floorf((PANE_H - header_h) / row_h));
    ui_pop(ui);
    // A pane centring a table of 3 rows, shorter than it.
    UIElement *centred_pane, *centred_side;
    pane_table_fixture(ui, "table-pane-centred", 3, &centred_pane, &centred_side);
    centred_pane->align_main = UI_ALIGN_CENTER;
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    const UIElement* row = centred_pane->children[0];
    printf("ui table-pane centred gaps %.6f %.6f\n", (double)(row->rect.y - centred_pane->rect.y),
           (double)(centred_pane->rect.y + PANE_H - (row->rect.y + row->rect.h)));
    return 0;
}

// Row 0's column rects, as the last layout placed them, into `out`.
static void table_columns_at(const UIElement* table, UIRect* out, int columns) {
    for (int c = 0; c < columns; c++)
        if (!ui_table_cell_rect(table, 0, c, &out[c]))
            out[c] = (UIRect){-1.0f, -1.0f, -1.0f, -1.0f};
}

// Laid out again, whether the table's columns still stand where `was` holds them, and none past
// them.
static double table_kept(UISystem* ui, const UIElement* table, const UIRect* was, int columns) {
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    UIRect now[UI_TABLE_COLUMNS_MOST + 1];
    table_columns_at(table, now, columns);
    const bool past = ui_table_cell_rect(table, 0, columns, NULL);
    return !past && memcmp(now, was, (size_t)columns * sizeof(UIRect)) == 0 ? 1.0 : 0.0;
}

static int probe_table_refused(UISystem* ui) {
    UIElement* table = table_fixture(ui, "table-refused", 900.0f, 300.0f, TABLE_COLUMNS,
                                     TABLE_COLUMN_COUNT, 100, &(UITableSource){.cell = table_cell});
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    UIRect was[TABLE_COLUMN_COUNT];
    table_columns_at(table, was, TABLE_COLUMN_COUNT);
    printf("ui table-refused set kept %.6f\n", table_kept(ui, table, was, TABLE_COLUMN_COUNT));
    ui_table_set_columns(table, TABLE_COLUMNS, 0);
    printf("ui table-refused none kept %.6f\n", table_kept(ui, table, was, TABLE_COLUMN_COUNT));
    UITableColumn many[UI_TABLE_COLUMNS_MOST + 1];
    for (int c = 0; c <= UI_TABLE_COLUMNS_MOST; c++)
        many[c] = (UITableColumn){.size = UI_FIXED, .length = 10.0f};
    ui_table_set_columns(table, many, UI_TABLE_COLUMNS_MOST + 1);
    printf("ui table-refused many kept %.6f\n", table_kept(ui, table, was, TABLE_COLUMN_COUNT));
    const UITableColumn drawn_fit = {.size = UI_FIT, .drawn = true};
    ui_table_set_columns(table, &drawn_fit, 1);
    printf("ui table-refused drawnfit kept %.6f\n", table_kept(ui, table, was, TABLE_COLUMN_COUNT));
    const UIElement* child = ui_panel(table);
    printf("ui table-refused child made %.6f\n", child ? 1.0 : 0.0);
    printf("ui table-refused child count %zu\n", table->child_count);
    printf("ui table-refused child kept %.6f\n", table_kept(ui, table, was, TABLE_COLUMN_COUNT));
    // A cursor out of range, or on a row that cannot hold it, leaves it where it was.
    ui_table_set_rows(table, 100, &(UITableSource){.cell = table_cell, .holds = cursor_holds});
    ui_table_set_cursor(table, 3);
    ui_table_set_cursor(table, 100);
    printf("ui table-refused past cursor %d\n", ui_table_cursor(table));
    ui_table_set_cursor(table, -2);
    printf("ui table-refused negative cursor %d\n", ui_table_cursor(table));
    ui_table_set_cursor(table, 4);
    printf("ui table-refused unheld cursor %d\n", ui_table_cursor(table));
    // The most columns are taken: a full table is not a refusal.
    ui_table_set_columns(table, many, UI_TABLE_COLUMNS_MOST);
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
    printf("ui table-refused most last %.6f\n",
           ui_table_cell_rect(table, 0, UI_TABLE_COLUMNS_MOST - 1, NULL) ? 1.0 : 0.0);
    return 0;
}

// ---- UTF-8

static int probe_utf8(void) {
    // Every length's first and last codepoint, encoded and decoded back.
    static const uint32_t edges[] = {0x7f, 0x80, 0x7ff, 0x800, 0xffff, 0x10000, 0x10ffff};
    int round = 0;
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        char buf[5] = {0};
        const size_t n = text_utf8_encode(edges[i], buf);
        const char* p = buf;
        if (n > 0 && text_utf8_next(&p) == edges[i] && p == buf + n)
            round++;
    }
    printf("ui utf8 roundtrip ok %d\n", round);
    // No character: surrogate halves and past the last plane.
    static const uint32_t none[] = {0xd800, 0xdfff, 0x110000};
    int refused = 0;
    for (size_t i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
        char buf[4];
        refused += text_utf8_encode(none[i], buf) == 0;
    }
    printf("ui utf8 encode refused %d\n", refused);
    // Nothing but malformed bytes: overlong forms, a surrogate, lead bytes past F4, a value past
    // U+10FFFF, and a sequence cut short. Each byte is one U+FFFD.
    const char* bad = "\xc0\x80"
                      "\xe0\x80\x80"
                      "\xed\xa0\x80"
                      "\xf5\x80"
                      "\xf4\x90\x80\x80"
                      "\xe2\x82";
    size_t replaced = 0;
    for (const char* p = bad; *p;)
        replaced += text_utf8_next(&p) == 0xfffd;
    printf("ui utf8 malformed fffd %zu\n", replaced);
    printf("ui utf8 malformed bytes %zu\n", strlen(bad));
    // prev mirrors next over good and bad bytes alike: from where next left off, prev returns what
    // next did and lands where next began.
    const char* mixed = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"
                        "\x80\xe2\x82"
                        "b\xf0\x9f\x98"
                        "c\xc0\x80z";
    int steps = 0, off = 0;
    for (const char* p = mixed; *p; steps++) {
        const char* q = p;
        const uint32_t cp = text_utf8_next(&q);
        const char* back = q;
        if (text_utf8_prev(&back, mixed) != cp || back != p)
            off++;
        p = q;
    }
    printf("ui utf8 mirror steps %d\n", steps);
    printf("ui utf8 mirror off %d\n", off);
    const char* at_start = mixed;
    const uint32_t none_before = text_utf8_prev(&at_start, mixed);
    printf("ui utf8 start returns %u\n", (unsigned)none_before);
    printf("ui utf8 start moved %d\n", at_start != mixed ? 1 : 0);
    return 0;
}

static int probe_match(void) {
    say_match("none", "abc", "xyz");
    say_match("starts", "rn", "route net");
    say_match("inside", "rn", "print");
    say_match("upper", "RN", "route net");
    say_match("best", "st", "fast stop");
    say_match("utf8", "sum", "r\xc3\xa9sum\xc3\xa9");
    say_match("empty", "", "anything");
    return 0;
}

int run_ui_text_probe(Game* game, UISystem* ui, Font* font, const char* which) {
    if (!strcmp(which, "wrap-label"))
        return probe_wrap_label(ui, font);
    if (!strcmp(which, "wrap-cache"))
        return probe_wrap_cache(ui);
    if (!strcmp(which, "runs-draw"))
        return probe_runs_draw(game, ui);
    if (!strcmp(which, "runs-wrap"))
        return probe_runs_wrap(game, ui, font);
    if (!strcmp(which, "runs-refused"))
        return probe_runs_refused(ui);
    if (!strcmp(which, "scroll-layout"))
        return probe_scroll_layout(ui);
    if (!strcmp(which, "scroll-wheel"))
        return probe_scroll_wheel(ui);
    if (!strcmp(which, "scroll-clip"))
        return probe_scroll_clip(ui);
    if (!strcmp(which, "scroll-into-view"))
        return probe_scroll_into_view(ui);
    if (!strcmp(which, "scroll-follow"))
        return probe_scroll_follow(ui);
    if (!strcmp(which, "scroll-bar"))
        return probe_scroll_bar(ui);
    if (!strcmp(which, "field-type"))
        return probe_field_type(ui);
    if (!strcmp(which, "field-edit"))
        return probe_field_edit(ui);
    if (!strcmp(which, "field-utf8"))
        return probe_field_utf8(ui);
    if (!strcmp(which, "field-select"))
        return probe_field_select(game, ui, font);
    if (!strcmp(which, "field-clipboard"))
        return probe_field_clipboard(game, ui);
    if (!strcmp(which, "field-undo"))
        return probe_field_undo(ui);
    if (!strcmp(which, "field-caret-x"))
        return probe_field_caret_x(ui, font);
    if (!strcmp(which, "field-scroll-x"))
        return probe_field_scroll_x(ui);
    if (!strcmp(which, "field-multiline"))
        return probe_field_multiline(ui, font);
    if (!strcmp(which, "field-placeholder"))
        return probe_field_placeholder(ui);
    if (!strcmp(which, "field-append"))
        return probe_field_append(game, ui);
    if (!strcmp(which, "field-keys"))
        return probe_field_keys(ui);
    if (!strcmp(which, "field-focus"))
        return probe_field_focus(ui);
    if (!strcmp(which, "match"))
        return probe_match();
    if (!strcmp(which, "utf8"))
        return probe_utf8();
    if (!strcmp(which, "max-size"))
        return probe_max_size(ui);
    if (!strcmp(which, "share"))
        return probe_share(ui);
    if (!strcmp(which, "padding-none"))
        return probe_padding_none(ui);
    if (!strcmp(which, "label-padding"))
        return probe_label_padding(game, ui, font);
    if (!strcmp(which, "baseline"))
        return probe_baseline(game, ui, font);
    if (!strcmp(which, "line-ascent"))
        return probe_line_ascent(game, ui, font);
    if (!strcmp(which, "control-lines"))
        return probe_control_lines(ui);
    if (!strcmp(which, "text-mesh"))
        return probe_text_mesh(font);
    if (!strcmp(which, "rows-built"))
        return probe_rows_built(ui);
    if (!strcmp(which, "rows-measure"))
        return probe_rows_measure(ui);
    if (!strcmp(which, "rows-anchor"))
        return probe_rows_anchor(ui);
    if (!strcmp(which, "rows-focus"))
        return probe_rows_focus(ui);
    if (!strcmp(which, "table-columns"))
        return probe_table_columns(ui, font);
    if (!strcmp(which, "table-measure"))
        return probe_table_measure(ui, font);
    if (!strcmp(which, "table-heights"))
        return probe_table_heights(game, ui, font);
    if (!strcmp(which, "table-view"))
        return probe_table_view(ui);
    if (!strcmp(which, "table-cursor"))
        return probe_table_cursor(ui);
    if (!strcmp(which, "table-pane"))
        return probe_table_pane(ui);
    if (!strcmp(which, "table-refused"))
        return probe_table_refused(ui);
    return -1;
}
