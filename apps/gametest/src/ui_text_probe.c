/*
 * The --ui-probe cases of spec 14.2, kept apart from the twelve of spec 12.2 in gametest.c: what
 * they measure is text and scrolling rather than a menu. Like those, they lay out, take input as a
 * struct of values and emit into the draw list, and never draw a frame.
 */

#include "ui_text_probe.h"

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
    return -1;
}
