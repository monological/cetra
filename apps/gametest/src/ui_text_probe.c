/*
 * The --ui-probe cases of spec 14.2, kept apart from the twelve of spec 12.2 in gametest.c: what
 * they measure is text and scrolling rather than a menu. Like those, they lay out, take input as a
 * struct of values and emit into the draw list, and never draw a frame.
 */

#include "ui_text_probe.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cetra/engine.h"
#include "cetra/internal/ui_text.h" // a label's lines, which the wrap arms count
#include "cetra/text.h"

#define PROBE_SIZE 22.0f

// The probe's long text: plain words, so its breaks are a closed form of ui_text_wrap_point.
static const char* PARAGRAPH = "the quick brown fox jumps over the lazy dog and keeps on running "
                               "past the river and over the hill until the evening comes";

static void probe_rect(const char* which, const char* label, UIRect r) {
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
    probe_rect("wrap-label", "label", label->rect);
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

static int probe_wrap_cache(UISystem* ui) {
    UIElement* label = NULL;
    UIElement* panel = wrap_fixture(ui, &label, 300.0f, PARAGRAPH);
    UIScreen* s = ui_top(ui);
    ui_layout(s, 1280.0f, 720.0f);
    printf("ui wrap-cache first wraps %.6f\n", (double)label->wraps);
    ui_layout(s, 1280.0f, 720.0f);
    ui_layout(s, 1280.0f, 720.0f);
    printf("ui wrap-cache again wraps %.6f\n", (double)label->wraps);
    panel->size[0] = 420.0f;
    ui_layout(s, 1280.0f, 720.0f);
    printf("ui wrap-cache wider wraps %.6f\n", (double)label->wraps);
    ui_set_text(label, "a new text, which breaks afresh");
    ui_layout(s, 1280.0f, 720.0f);
    printf("ui wrap-cache retext wraps %.6f\n", (double)label->wraps);
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
    probe_rect("scroll-layout", "pane", pane->rect);
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
    probe_rect("scroll-bar", "pane", pane->rect);
    probe_rect("scroll-bar", "thumb", thumb);
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
    return -1;
}
