/*
 * The UI's timings:
 *   - a frame's UI work and its two layouts, on screens the size pcb's will be;
 *   - a long text walked, and its kerning alone;
 *   - one keystroke in a multi-line field of 1, 20 and 1,000 KB;
 *   - the font atlas's build;
 *   - the code editor on texts of 80 KB, 10 MB and 64 MB: opening, the first frame, a keystroke
 *     at one caret and at a hundred, a jump, a resize, and the budget's walk to every row wrapped.
 * Wall-clock milliseconds, each the median of five runs with their spread beside it. Numbers from a
 * debug build say little: run it from a release build.
 */

#include "ui_bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLFW/glfw3.h>

#include "cetra/engine.h"
#include "cetra/glyph_cache.h"
#include "cetra/internal/ui_editor_hooks.h" // whether the budget has settled every row
#include "cetra/text.h"
#include "cetra/text_doc.h"
#include "cetra/ui_editor.h"

#define BENCH_W      1280.0f
#define BENCH_H      720.0f
#define BENCH_SIZE   22.0f
#define BENCH_RUNS   5
#define BENCH_FONT   "apps/splash/assets/Roboto-Bold.ttf"
#define FRAMES_A_RUN 100

static const char* MESSAGE =
    "the ground pour keeps 0.3 mm from every pad, and the mounting hole "
    "has a clearance of its own, so the pour stops where the clearance does";

static double _now_ms(void) {
    return glfwGetTime() * 1e3;
}

static int _by_value(const void* a, const void* b) {
    const double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

// The median of BENCH_RUNS values, and how far they spread.
static void _say(const char* what, const char* how, double runs[BENCH_RUNS]) {
    qsort(runs, BENCH_RUNS, sizeof(double), _by_value);
    printf("ui-bench %s %s %.4f spread %.4f\n", what, how, runs[BENCH_RUNS / 2],
           runs[BENCH_RUNS - 1] - runs[0]);
}

// A frame's UI work, the input pass and the build -- each lays the screen out -- and the layouts
// alone, twice as a frame has them; milliseconds a frame.
static void _time_screen(UISystem* ui, const char* what) {
    UIScreen* s = ui_top(ui);
    const UIInput idle = {.pointer_x = -1.0f, .pointer_y = -1.0f};
    for (int i = 0; i < 5; i++) {
        ui_update(ui, &idle, BENCH_W, BENCH_H);
        ui_build(ui, BENCH_W, BENCH_H, 0.0f);
    }
    double frame[BENCH_RUNS], layouts[BENCH_RUNS];
    for (int r = 0; r < BENCH_RUNS; r++) {
        double t0 = _now_ms();
        for (int i = 0; i < FRAMES_A_RUN; i++) {
            ui_update(ui, &idle, BENCH_W, BENCH_H);
            ui_build(ui, BENCH_W, BENCH_H, 0.0f);
        }
        frame[r] = (_now_ms() - t0) / FRAMES_A_RUN;
        t0 = _now_ms();
        for (int i = 0; i < FRAMES_A_RUN; i++) {
            ui_layout(s, BENCH_W, BENCH_H);
            ui_layout(s, BENCH_W, BENCH_H);
        }
        layouts[r] = (_now_ms() - t0) / FRAMES_A_RUN;
    }
    _say(what, "frame_ms", frame);
    _say(what, "layouts_ms", layouts);
}

// ---- the screens

static char g_names[5000][24];

static void build_message(UIElement* row, int index, void* user) {
    (void)index;
    (void)user;
    UIElement* f = ui_text_field(row, UI_FIELD_MULTILINE | UI_FIELD_READ_ONLY, NULL, NULL);
    ui_set_size(f, UI_GROW, 0.0f, UI_FIT, 0.0f);
    ui_field_set_text(f, MESSAGE);
}

// The chat's column: `count` messages as virtual rows over an input line.
static UIScreen* history_screen(UISystem* ui, int count) {
    UIScreen* s = ui_screen(ui, "bench-history");
    ui_screen_set_modal(s, false);
    UIElement* column = ui_panel(ui_screen_root(s));
    ui_set_size(column, UI_FIXED, 360.0f, UI_GROW, 0.0f);
    UIElement* pane = ui_scroll(column);
    ui_set_size(pane, UI_GROW, 0.0f, UI_GROW, 0.0f);
    pane->follow_end = true;
    ui_scroll_set_rows(pane, count, 40.0f, build_message, NULL);
    UIElement* input = ui_text_field(column, UI_FIELD_MULTILINE, NULL, NULL);
    ui_set_size(input, UI_GROW, 0.0f, UI_FIT, 0.0f);
    input->max_rows = 6;
    return s;
}

static void build_name(UIElement* holder, int index, void* user) {
    (void)user;
    UIElement* row = ui_panel(holder);
    row->dir = UI_ROW;
    ui_set_size(row, UI_GROW, 0.0f, UI_FIXED, 30.0f);
    ui_set_size(ui_label(row, g_names[index]), UI_GROW, 0.0f, UI_FIT, 0.0f);
    ui_label(row, "N");
}

// The jump: a field over a list of 5,000 names, twelve shown.
static UIScreen* palette_screen(UISystem* ui) {
    for (int i = 0; i < 5000; i++)
        snprintf(g_names[i], sizeof(g_names[i]), "net_%04d", i);
    UIScreen* s = ui_screen(ui, "bench-palette");
    UIElement* box = ui_panel(ui_screen_root(s));
    ui_set_size(box, UI_FIXED, 420.0f, UI_FIT, 0.0f);
    ui_set_size(ui_text_field(box, 0, NULL, NULL), UI_GROW, 0.0f, UI_FIT, 0.0f);
    UIElement* list = ui_scroll(box);
    ui_set_size(list, UI_GROW, 0.0f, UI_FIT, 0.0f);
    list->max_size[1] = 12 * 30.0f;
    list->spacing = -1.0f;
    ui_scroll_set_rows(list, 5000, 30.0f, build_name, NULL);
    return s;
}

// A settings screen of 40 elements: labels, sliders, toggles, selectors and buttons in turn.
static UIScreen* settings_screen(UISystem* ui) {
    static float values[8];
    static bool flags[8];
    static int picks[8];
    static const char* options[] = {"Windowed", "Borderless", "Fullscreen"};
    UIScreen* s = ui_screen(ui, "bench-settings");
    UIElement* column = ui_panel(ui_screen_root(s));
    for (int i = 0; i < 8; i++) {
        char name[32];
        snprintf(name, sizeof(name), "Setting number %d", i);
        ui_label(column, name);
        ui_slider(column, name, 0.0f, 1.0f, &values[i], NULL, NULL);
        ui_toggle(column, name, &flags[i], NULL, NULL);
        ui_selector(column, name, options, 3, &picks[i], NULL, NULL);
        ui_button(column, "Apply", NULL, NULL);
    }
    return s;
}

static void _time_one(UISystem* ui, UIScreen* s, const char* what) {
    ui_push(ui, s);
    _time_screen(ui, what);
    ui_pop(ui);
}

// ---- the text

// A walk of a long text against the kerning pairs it asks for.
static void _time_kerning(const Font* font) {
    const size_t repeats = 100;
    char* text = malloc(strlen(MESSAGE) * repeats + 1);
    if (!text)
        return;
    text[0] = '\0';
    for (size_t i = 0; i < repeats; i++)
        strcat(text, MESSAGE);
    double walk[BENCH_RUNS], kern[BENCH_RUNS];
    for (int r = 0; r < BENCH_RUNS; r++) {
        double t0 = _now_ms();
        (void)ui_text_width(font, BENCH_SIZE, 0.0f, text);
        walk[r] = _now_ms() - t0;
        t0 = _now_ms();
        volatile float sum = 0.0f;
        uint32_t prev = 0;
        for (const char* p = text; *p;) {
            const uint32_t cp = text_utf8_next(&p);
            sum += font_kern_advance(font, prev, cp);
            prev = cp;
        }
        (void)sum;
        kern[r] = _now_ms() - t0;
    }
    printf("ui-bench text bytes %zu\n", strlen(text));
    _say("text", "walk_ms", walk);
    _say("text", "kern_ms", kern);
    free(text);
}

// One keystroke -- the input pass that types it and the build that draws it -- in a multi-line
// field holding `bytes` of text, the caret at its end.
static void _time_keystroke(UISystem* ui, size_t bytes) {
    char* text = malloc(bytes + 1);
    if (!text)
        return;
    const size_t m = strlen(MESSAGE);
    for (size_t i = 0; i < bytes; i++)
        text[i] = i % (m + 1) == m ? ' ' : MESSAGE[i % (m + 1)];
    text[bytes] = '\0';
    UIScreen* s = ui_screen(ui, "bench-field");
    UIElement* field = ui_text_field(ui_screen_root(s), UI_FIELD_MULTILINE, NULL, NULL);
    ui_set_size(field, UI_FIXED, 600.0f, UI_FIT, 0.0f);
    field->max_rows = 20;
    ui_field_set_text(field, text);
    free(text);
    ui_push(ui, s);
    ui_set_focus(s, field);
    const UIInput idle = {.pointer_x = -1.0f, .pointer_y = -1.0f};
    ui_update(ui, &idle, BENCH_W, BENCH_H);
    ui_build(ui, BENCH_W, BENCH_H, 0.0f);
    const InputKeyEvent typed = {.codepoint = 'x'};
    const UIInput key = {
        .pointer_x = -1.0f, .pointer_y = -1.0f, .key_events = &typed, .key_event_count = 1};
    double runs[BENCH_RUNS];
    for (int r = 0; r < BENCH_RUNS; r++) {
        const double t0 = _now_ms();
        ui_update(ui, &key, BENCH_W, BENCH_H);
        ui_build(ui, BENCH_W, BENCH_H, 0.0f);
        runs[r] = _now_ms() - t0;
    }
    char what[32];
    snprintf(what, sizeof(what), "field%zu", bytes);
    _say(what, "keystroke_ms", runs);
    ui_pop(ui);
}

// ---- The code editor (spec 14.4)

#define EDITOR_FONT "assets/fonts/JetBrainsMono-Regular.ttf"
#define EDITOR_W    900.0f
#define EDITOR_H    700.0f

static const char* CODE_LINE =
    "    (sig IO_L1N_T0L_N1_DBC_44 :io :bank 44 :byte 0 :strobe :domain VCCO_44) ; a ball\n";

// A text of about `bytes`, pcb's lines over and over.
static char* _code(size_t bytes, size_t* len) {
    const size_t line = strlen(CODE_LINE);
    const size_t lines = bytes / line + 1;
    char* text = malloc(lines * line + 1);
    if (!text)
        return NULL;
    for (size_t i = 0; i < lines; i++)
        memcpy(text + i * line, CODE_LINE, line);
    *len = lines * line;
    text[*len] = '\0';
    return text;
}

// Brackets and `:keywords`, stateless: a highlighter of the kind pcb hands the editor.
static size_t _colour(const char* s, size_t len, uint32_t* state, TextSpan* spans, size_t max,
                      void* user) {
    (void)user;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == ';') {
            if (n < max)
                spans[n] = (TextSpan){(uint32_t)i, (uint32_t)(len - i), 1};
            n++;
            break;
        }
        if (s[i] == '(' || s[i] == ')' || s[i] == ':') {
            size_t j = i + 1;
            while (s[i] == ':' && j < len && s[j] != ' ' && s[j] != ')')
                j++;
            if (n < max)
                spans[n] = (TextSpan){(uint32_t)i, (uint32_t)(j - i), 2};
            n++;
            i = j - 1;
        }
    }
    *state = 0;
    return n;
}

static void _editor_frame(UISystem* ui, const UIInput* in) {
    ui_update(ui, in, BENCH_W, BENCH_H);
    ui_build(ui, BENCH_W, BENCH_H, 0.0f);
}

// The editor on a text of `bytes`: opening it, its first frame, a keystroke at one caret and at a
// hundred spread through it, a jump far down, a resize, and the budget's walk to every row settled.
static void _time_editor(UISystem* ui, GlyphCache* face, size_t bytes, const char* label) {
    size_t len = 0;
    char* text = _code(bytes, &len);
    if (!text)
        return;
    double runs[BENCH_RUNS];
    TextDoc* doc = NULL;
    for (int r = 0; r < BENCH_RUNS; r++) {
        free_text_doc(doc);
        const double t0 = _now_ms();
        doc = create_text_doc(text, len);
        runs[r] = _now_ms() - t0;
    }
    free(text);
    _say(label, "open_ms", runs);
    text_doc_set_highlighter(doc, _colour, NULL, false);
    UITheme theme = *ui_theme(ui);
    theme.editor_face = face;
    theme.editor_size = 13.0f;
    ui_set_theme(ui, &theme);
    UIScreen* s = ui_screen(ui, "bench-editor");
    UIElement* ed = ui_editor(ui_screen_root(s), doc);
    ui_set_size(ed, UI_FIXED, EDITOR_W, UI_FIXED, EDITOR_H);
    ui_push(ui, s);
    ui_set_focus(s, ed);
    const UIInput idle = {.pointer_x = -1.0f, .pointer_y = -1.0f};
    double t0 = _now_ms();
    _editor_frame(ui, &idle);
    printf("ui-bench %s first_frame_ms %.4f\n", label, _now_ms() - t0);
    // The budget's walk, a frame's share at a time, to every row wrapped: first, so the timings
    // after it are the editor's own and not the budget's share of their frames.
    int frames = 0;
    double total = 0.0, longest = 0.0;
    while (!ui_editor_settled(ed) && frames < 100000) {
        t0 = _now_ms();
        _editor_frame(ui, &idle);
        const double ms = _now_ms() - t0;
        total += ms;
        longest = ms > longest ? ms : longest;
        frames++;
    }
    printf("ui-bench %s settle_frames %d total_ms %.4f longest_ms %.4f\n", label, frames, total,
           longest);
    const InputKeyEvent typed = {.codepoint = 'x'};
    const UIInput key = {
        .pointer_x = -1.0f, .pointer_y = -1.0f, .key_events = &typed, .key_event_count = 1};
    // One caret in the middle of the text.
    const size_t lines = text_doc_line_count(doc);
    const size_t mid = text_doc_line_start(doc, lines / 2);
    ui_editor_set_selections(ed, &(TextSelection){mid, mid}, 1);
    ui_editor_reveal(ed, mid);
    _editor_frame(ui, &idle);
    for (int r = 0; r < BENCH_RUNS; r++) {
        t0 = _now_ms();
        _editor_frame(ui, &key);
        runs[r] = _now_ms() - t0;
    }
    _say(label, "keystroke_ms", runs);
    // A hundred carets, a line each, spread through the text.
    TextSelection spread[100];
    for (size_t i = 0; i < 100; i++) {
        const size_t at = text_doc_line_start(doc, (lines - 1) * i / 100);
        spread[i] = (TextSelection){at, at};
    }
    ui_editor_set_selections(ed, spread, 100);
    _editor_frame(ui, &idle);
    for (int r = 0; r < BENCH_RUNS; r++) {
        t0 = _now_ms();
        _editor_frame(ui, &key);
        runs[r] = _now_ms() - t0;
    }
    _say(label, "keystroke100_ms", runs);
    // A jump far down, or back up: the view's lines found, wrapped and drawn.
    for (int r = 0; r < BENCH_RUNS; r++) {
        const size_t to = text_doc_line_start(doc, r % 2 ? lines / 7 : lines * 6 / 7);
        t0 = _now_ms();
        ui_editor_reveal(ed, to);
        _editor_frame(ui, &idle);
        runs[r] = _now_ms() - t0;
    }
    _say(label, "jump_ms", runs);
    // A resize: every line's rows guessed again, the view's wrapped now.
    for (int r = 0; r < BENCH_RUNS; r++) {
        ui_set_size(ed, UI_FIXED, r % 2 ? EDITOR_W : EDITOR_W - 200.0f, UI_FIXED, EDITOR_H);
        t0 = _now_ms();
        _editor_frame(ui, &idle);
        runs[r] = _now_ms() - t0;
    }
    _say(label, "resize_ms", runs);
    ui_pop(ui);
    free_text_doc(doc);
}

// The atlas built from nothing: a pool of its own, so nothing is cached.
static void _time_atlas(void) {
    double runs[BENCH_RUNS];
    size_t glyphs = 0;
    for (int r = 0; r < BENCH_RUNS; r++) {
        FontPool* pool = create_font_pool();
        const double t0 = _now_ms();
        const Font* font = load_font(pool, BENCH_FONT, 64.0f, true);
        runs[r] = _now_ms() - t0;
        glyphs = font ? HASH_COUNT(font->glyph_cache) : 0;
        free_font_pool(pool);
    }
    printf("ui-bench atlas glyphs %zu\n", glyphs);
    _say("atlas", "load_ms", runs);
}

int run_ui_bench(UISystem* ui, const Font* font) {
    _time_one(ui, history_screen(ui, 200), "history200");
    _time_one(ui, history_screen(ui, 4000), "history4000");
    _time_one(ui, palette_screen(ui), "palette");
    _time_one(ui, settings_screen(ui), "settings");
    _time_kerning(font);
    _time_keystroke(ui, 1024);
    _time_keystroke(ui, 20 * 1024);
    _time_keystroke(ui, 1000 * 1024); // under the field's 1 MiB, which would refuse the keystroke
    _time_atlas();
    GlyphCache* face = create_glyph_cache(EDITOR_FONT, 26.0f);
    if (face) {
        _time_editor(ui, face, 80 * 1024, "editor80k");
        _time_editor(ui, face, 10 * 1024 * 1024, "editor10m");
        _time_editor(ui, face, 64 * 1024 * 1024, "editor64m");
        free_glyph_cache(face);
    }
    return 0;
}
