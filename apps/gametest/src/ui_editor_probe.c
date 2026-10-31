/*
 * The --ui-probe cases of spec 14.4, the code editor: its text first, which needs no window, then
 * the editor that shows it. Each prints `ui <case> <label> <key> <numbers>` lines, as the cases of
 * ui_text_probe.c do.
 */

#include "ui_editor_probe.h"

#include <GLFW/glfw3.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cetra/engine.h"
#include "cetra/glyph_cache.h"
#include "cetra/internal/ext/stb_truetype.h" // the rasteriser the glyph cache's bitmaps are held to
#include "cetra/internal/text_doc_edit.h"    // the editor's half of the text, which the arms drive
#include "cetra/internal/ui_editor_hooks.h"  // the editor's wrapping and where it put things
#include "cetra/internal/ui_internal.h"      // ui_system_engine, for the clipboard
#include "cetra/text.h"
#include "cetra/text_doc.h"
#include "cetra/ui_editor.h"
#include "cetra/ui_editor_vim.h"
#include "cetra/util.h"

// ---- A stand-in for an editor: carets the text moves, as an editor's would be.

typedef struct FakeView {
    TextSelection sel[8];
    size_t count;
} FakeView;

static void fake_changed(void* view, const TextViewChange* c) {
    FakeView* v = view;
    for (size_t i = 0; !c->held && i < v->count; i++)
        text_doc_map_selection(&v->sel[i], c);
}

static size_t fake_selections(void* view, const TextSelection** out) {
    const FakeView* v = view;
    *out = v->sel;
    return v->count;
}

static void fake_set(void* view, const TextSelection* s, size_t n) {
    FakeView* v = view;
    v->count = n < 8 ? n : 8;
    memcpy(v->sel, s, v->count * sizeof(*s));
}

static void fake_attach(TextDoc* doc, FakeView* v) {
    text_doc_add_view(doc, &(TextDocView){.view = v,
                                          .changed = fake_changed,
                                          .selections = fake_selections,
                                          .set_selections = fake_set});
}

// Types `text` at every caret of `v`, which are in order, as one action.
static void fake_type(TextDoc* doc, FakeView* v, const char* text) {
    TextEdit edits[8];
    for (size_t i = 0; i < v->count; i++)
        edits[i] = (TextEdit){v->sel[i].head, v->sel[i].head, text, strlen(text)};
    text_doc_apply(doc, edits, v->count, v, NULL, 0, true);
}

// Whether the text is exactly `want`, byte for byte.
static int doc_is(const TextDoc* doc, const char* want, size_t len) {
    size_t n = 0;
    char* text = text_doc_text(doc, &n);
    const int same = text && n == len && !memcmp(text, want, len);
    free(text);
    return same;
}

static int doc_is_str(const TextDoc* doc, const char* want) {
    return doc_is(doc, want, strlen(want));
}

// ---- A flat reference: the same rules, on one string.

// The lines of `s`: each start, and each content's length without its ending.
static size_t ref_lines(const char* s, size_t len, size_t* starts, size_t* lens, size_t max) {
    size_t count = 0, at = 0;
    for (;;) {
        const char* nl = memchr(s + at, '\n', len - at);
        const size_t stop = nl ? (size_t)(nl - s) : len;
        size_t content = stop - at;
        if (nl && content && s[stop - 1] == '\r')
            content--;
        if (count < max) {
            starts[count] = at;
            lens[count] = content;
        }
        count++;
        if (!nl)
            break;
        at = stop + 1;
    }
    return count;
}

static uint32_t g_rng = 0x9e3779b9u;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

// A position in `s` on a character's edge, near `at`.
static size_t char_edge(const char* s, size_t len, size_t at) {
    while (at > 0 && at < len && text_utf8_is_continuation(s[at]))
        at--;
    return at;
}

// ---- doc-roundtrip

static int probe_doc_roundtrip(void) {
    static const struct {
        const char* label;
        const char* text;
        size_t len;
        size_t lines;
    } cases[] = {
        {"lf", "one\ntwo\n", 8, 3},
        {"crlf", "one\r\ntwo\r\n", 10, 3},
        {"mixed", "one\r\ntwo\nthree", 14, 3},
        {"lone_cr", "a\rb\nc\r", 6, 2},
        {"bom", "\xef\xbb\xbfx\ny", 6, 2},
        {"no_final", "abc", 3, 1},
        {"empty", "", 0, 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TextDoc* doc = create_text_doc(cases[i].text, cases[i].len);
        printf("ui doc-roundtrip %s loaded %d\n", cases[i].label,
               doc_is(doc, cases[i].text, cases[i].len));
        printf("ui doc-roundtrip %s lines %d\n", cases[i].label,
               text_doc_line_count(doc) == cases[i].lines);
        // An edit in the middle, on a character's edge, then taken back.
        const size_t at = char_edge(cases[i].text, cases[i].len, cases[i].len / 2);
        char want[64];
        memcpy(want, cases[i].text, at);
        memcpy(want + at, "Z\nY", 3);
        memcpy(want + at + 3, cases[i].text + at, cases[i].len - at);
        text_doc_replace(doc, at, at, "Z\nY", 3);
        printf("ui doc-roundtrip %s edited %d\n", cases[i].label,
               doc_is(doc, want, cases[i].len + 3));
        text_doc_undo(doc);
        printf("ui doc-roundtrip %s undone %d\n", cases[i].label,
               doc_is(doc, cases[i].text, cases[i].len));
        free_text_doc(doc);
    }
    // The line ending a text is found to use.
    TextDoc* crlf = create_text_doc("a\r\nb\n", 5);
    TextDoc* lf = create_text_doc("a\nb\r\n", 5);
    printf("ui doc-roundtrip newline crlf %d\n", crlf->newline == TEXT_NEWLINE_CRLF);
    printf("ui doc-roundtrip newline lf %d\n", lf->newline == TEXT_NEWLINE_LF);
    free_text_doc(crlf);
    free_text_doc(lf);
    return 0;
}

// ---- doc-random

#define RANDOM_STEPS 3000
#define RANDOM_MAX   4096

static int probe_doc_random(void) {
    static const char* const PIECES[] = {"a", "bc", " ", "\n", "\r", "\r\n", "\xc3\xa9", "xyz"};
    static char ref[RANDOM_MAX * 2];
    size_t len = 0;
    for (int i = 0; i < 200; i++) {
        const char* p = PIECES[rnd() % 8];
        memcpy(ref + len, p, strlen(p));
        len += strlen(p);
    }
    static char first[RANDOM_MAX * 2], last[RANDOM_MAX * 2];
    memcpy(first, ref, len);
    const size_t first_len = len;
    TextDoc* doc = create_text_doc(ref, len);
    static size_t starts[RANDOM_MAX + 2], lens[RANDOM_MAX + 2];
    static char copy[RANDOM_MAX * 2];
    int wrong = 0, steps = 0;
    for (int step = 0; step < RANDOM_STEPS && !wrong; step++, steps++) {
        size_t a = char_edge(ref, len, len ? rnd() % (len + 1) : 0);
        size_t b = char_edge(ref, len, a + rnd() % 6);
        if (b > len)
            b = len;
        if (b < a)
            b = a;
        char text[32];
        size_t n = 0;
        for (int k = (int)(rnd() % 4); k > 0; k--) {
            const char* p = PIECES[rnd() % 8];
            memcpy(text + n, p, strlen(p));
            n += strlen(p);
        }
        if (len - (b - a) + n > RANDOM_MAX) {
            n = 0;
            b = len < a + 64 ? len : char_edge(ref, len, a + 64);
        }
        if (!text_doc_replace(doc, a, b, text, n)) {
            wrong++;
            break;
        }
        memmove(ref + a + n, ref + b, len - b);
        memcpy(ref + a, text, n);
        len = len - (b - a) + n;
        // Everything the text answers, against the reference.
        const size_t lines = ref_lines(ref, len, starts, lens, RANDOM_MAX + 2);
        if (!doc_is(doc, ref, len) || text_doc_length(doc) != len ||
            text_doc_line_count(doc) != lines) {
            fprintf(stderr,
                    "doc-random step %d: [%zu, %zu) made %zu bytes: text %d, length %zu "
                    "of %zu, lines %zu of %zu\n",
                    step, a, b, n, doc_is(doc, ref, len), text_doc_length(doc), len,
                    text_doc_line_count(doc), lines);
            wrong++;
            break;
        }
        for (size_t l = 0; l < lines && !wrong; l++) {
            size_t got_len = 0;
            const char* got = text_doc_line(doc, l, &got_len);
            if (text_doc_line_start(doc, l) != starts[l] || got_len != lens[l] ||
                memcmp(got, ref + starts[l], lens[l])) {
                fprintf(stderr,
                        "doc-random step %d: line %zu starts %zu of %zu, %zu bytes of %zu\n", step,
                        l, text_doc_line_start(doc, l), starts[l], got_len, lens[l]);
                wrong++;
            }
        }
        for (int q = 0; q < 8 && !wrong; q++) {
            const size_t at = len ? rnd() % len : 0;
            size_t line = 0;
            while (line + 1 < lines && starts[line + 1] <= at)
                line++;
            if (text_doc_line_of(doc, at) != line) {
                fprintf(stderr, "doc-random step %d: byte %zu on line %zu, not %zu\n", step, at,
                        text_doc_line_of(doc, at), line);
                wrong++;
            }
            const size_t reach = rnd() % 40;
            const size_t to = at + reach < len ? at + reach : len;
            const size_t got = text_doc_copy(doc, at, to, copy);
            if (got != to - at || memcmp(copy, ref + at, to - at)) {
                fprintf(stderr, "doc-random step %d: bytes %zu to %zu copied as %zu, on line %zu\n",
                        step, at, to, got, text_doc_line_of(doc, at));
                for (size_t k = 0; k < to - at; k++)
                    fprintf(stderr, "%02x/%02x ", (unsigned char)copy[k],
                            (unsigned char)ref[at + k]);
                fprintf(stderr, "\n");
                wrong++;
            }
        }
    }
    printf("ui doc-random checked steps %d\n", steps);
    printf("ui doc-random wrong count %d\n", wrong);
    // Every step taken back gives the first text; put back again, the last.
    memcpy(last, ref, len);
    int undone = 0;
    while (text_doc_undo(doc))
        undone++;
    printf("ui doc-random undone steps %d\n", undone);
    printf("ui doc-random undone first %d\n", doc_is(doc, first, first_len));
    while (text_doc_redo(doc))
        ;
    printf("ui doc-random redone last %d\n", doc_is(doc, last, len));
    free_text_doc(doc);
    return 0;
}

// ---- doc-map

static void say_mapped(const TextDoc* doc, const char* label, size_t offset, size_t want) {
    size_t at = offset;
    const bool ok = text_doc_map(doc, 0, &at);
    printf("ui doc-map %s offset %d\n", label, ok && at == want);
    if (!ok || at != want)
        fprintf(stderr, "doc-map %s: %zu went to %zu, want %zu\n", label, offset, at, want);
}

static int probe_doc_map(void) {
    // "abcdefghij", then XY put in at 2, "ef" taken out, and "ij" made "Q": "abXYcdghQ".
    TextDoc* doc = create_text_doc("abcdefghij", 10);
    const TextRange ranges[] = {{3, 5, 1}, {4, 6, 2}, {1, 9, 3}};
    text_doc_replace(doc, 2, 2, "XY", 2);
    text_doc_replace(doc, 6, 8, "", 0);
    text_doc_replace(doc, 8, 10, "Q", 1);
    printf("ui doc-map text final %d\n", doc_is_str(doc, "abXYcdghQ"));
    say_mapped(doc, "before", 0, 0);
    say_mapped(doc, "at_insert", 2, 2);
    say_mapped(doc, "after_insert", 3, 5);
    say_mapped(doc, "in_deleted", 5, 6);
    say_mapped(doc, "in_replaced", 9, 9);
    say_mapped(doc, "end", 10, 9);
    // Ranges given as the text stood before all three.
    text_doc_set_ranges(doc, 0, 0, ranges, 3);
    size_t count = 0;
    const TextRange* got = text_doc_ranges(doc, 0, &count);
    printf("ui doc-map ranges count %d\n", count == 2);
    printf("ui doc-map ranges cut %d\n",
           count == 2 && got[1].start == 3 + 2 && got[1].end == 6 && got[1].style == 1);
    printf("ui doc-map ranges grown %d\n",
           count == 2 && got[0].start == 1 && got[0].end == 8 && got[0].style == 3);
    // And moved by the changes after: "d" of the cut range stays put under a change before it.
    text_doc_replace(doc, 0, 1, "", 0);
    got = text_doc_ranges(doc, 0, &count);
    printf("ui doc-map ranges follow %d\n", count == 2 && got[1].start == 4 && got[1].end == 5);
    // A version the text has not reached, or one too far back, is refused.
    size_t at = 3;
    printf("ui doc-map refused future %d\n", !text_doc_map(doc, 99, &at) && at == 3);
    for (int i = 0; i < TEXT_DOC_HISTORY + 1; i++)
        text_doc_replace(doc, 0, 0, "a", 1);
    printf("ui doc-map refused old %d\n", !text_doc_map(doc, 0, &at) && at == 3);
    free_text_doc(doc);
    return 0;
}

// ---- doc-set-text

static int probe_doc_set_text(void) {
    TextDoc* doc = create_text_doc("one two three", 13);
    FakeView v = {.sel = {{9, 9}}, .count = 1};
    fake_attach(doc, &v);
    const uint64_t before = doc->version;
    text_doc_set_text(doc, "one 2 three", 11);
    const size_t h = before % TEXT_DOC_HISTORY;
    printf("ui doc-set-text trimmed one %d\n", doc->version == before + 1);
    printf("ui doc-set-text trimmed change %d\n", doc->history[h].start == 4 &&
                                                      doc->history[h].removed == 3 &&
                                                      doc->history[h].inserted == 1);
    printf("ui doc-set-text caret moved %d\n", v.sel[0].head == 7);
    printf("ui doc-set-text text matches %d\n", doc_is_str(doc, "one 2 three"));
    // Unchanged is no change.
    text_doc_set_text(doc, "one 2 three", 11);
    printf("ui doc-set-text same none %d\n", doc->version == before + 1);
    free_text_doc(doc);
    // The shared start ends on a character's edge: é to è changes the whole character.
    TextDoc* accents = create_text_doc("a\xc3\xa9", 3);
    text_doc_set_text(accents, "a\xc3\xa8", 3);
    printf("ui doc-set-text edge change %d\n", accents->history[0].start == 1 &&
                                                   accents->history[0].removed == 2 &&
                                                   accents->history[0].inserted == 2);
    // The host's own edit inside a character is refused.
    printf("ui doc-set-text refused inside %d\n", !text_doc_replace(accents, 2, 2, "x", 1));
    free_text_doc(accents);
    return 0;
}

// ---- doc-undo

static int g_actions;
static size_t g_action_changes;

static void count_action(TextDoc* doc, const TextDocAction* action, void* user) {
    (void)doc;
    (void)user;
    g_actions++;
    g_action_changes = action->change_count;
}

static bool g_inside_refused;

// Tries to change the text while an action is told, and reads the bytes the action reports.
static void edit_in_action(TextDoc* doc, const TextDocAction* action, void* user) {
    (void)user;
    g_inside_refused = !text_doc_replace(doc, 0, 0, "q", 1) && action->change_count == 1 &&
                       action->changes[0].removed_len == 2 &&
                       !memcmp(action->changes[0].removed, "ab", 2);
}

static int probe_doc_undo(void) {
    TextDoc* doc = create_text_doc("", 0);
    FakeView v = {.sel = {{0, 0}}, .count = 1};
    fake_attach(doc, &v);
    text_doc_set_on_action(doc, count_action, NULL);
    const char* keys[] = {"h", "e", "l", "l", "o", " ", "w"};
    for (size_t i = 0; i < 7; i++)
        fake_type(doc, &v, keys[i]);
    printf("ui doc-undo actions told %d\n", g_actions == 7);
    printf("ui doc-undo typed text %d\n", doc_is_str(doc, "hello w"));
    // "hello" is one step and " w" another.
    printf("ui doc-undo steps two %d\n", doc->undo_count == 2);
    text_doc_undo(doc);
    printf("ui doc-undo undo1 text %d\n", doc_is_str(doc, "hello"));
    printf("ui doc-undo undo1 caret %d\n", v.sel[0].head == 5);
    text_doc_undo(doc);
    printf("ui doc-undo undo2 text %d\n", doc_is_str(doc, ""));
    text_doc_redo(doc);
    text_doc_redo(doc);
    printf("ui doc-undo redo text %d\n", doc_is_str(doc, "hello w"));
    printf("ui doc-undo redo caret %d\n", v.sel[0].head == 7);
    // A caret moved splits typing that would otherwise join.
    fake_type(doc, &v, "a");
    text_doc_seal_undo(doc);
    fake_type(doc, &v, "b");
    printf("ui doc-undo sealed steps %d\n", doc->undo_count == 4);
    free_text_doc(doc);

    // Typing at two carets is one step of two changes, and joins as one caret's does.
    doc = create_text_doc("ab", 2);
    FakeView two = {.sel = {{0, 0}, {2, 2}}, .count = 2};
    fake_attach(doc, &two);
    text_doc_set_on_action(doc, count_action, NULL);
    fake_type(doc, &two, "x");
    printf("ui doc-undo carets changes %d\n", g_action_changes == 2);
    fake_type(doc, &two, "y");
    printf("ui doc-undo carets text %d\n", doc_is_str(doc, "xyabxy"));
    printf("ui doc-undo carets steps %d\n", doc->undo_count == 1);
    printf("ui doc-undo carets moved %d\n", two.sel[0].head == 2 && two.sel[1].head == 6);
    text_doc_undo(doc);
    printf("ui doc-undo carets undone %d\n", doc_is_str(doc, "ab"));
    printf("ui doc-undo carets restored %d\n", two.sel[0].head == 0 && two.sel[1].head == 2);
    free_text_doc(doc);

    // Kept by nobody: nothing recorded, nothing undone.
    doc = create_text_doc("", 0);
    doc->keep_undo = false;
    FakeView off = {.sel = {{0, 0}}, .count = 1};
    fake_attach(doc, &off);
    fake_type(doc, &off, "z");
    printf("ui doc-undo off steps %d\n", doc->undo_count == 0);
    printf("ui doc-undo off refused %d\n", !text_doc_undo(doc) && doc_is_str(doc, "z"));
    free_text_doc(doc);

    // A change asked for from inside on_action is refused, while the undo it reports stands.
    doc = create_text_doc("", 0);
    FakeView told = {.sel = {{0, 0}}, .count = 1};
    fake_attach(doc, &told);
    fake_type(doc, &told, "ab");
    text_doc_set_on_action(doc, edit_in_action, NULL);
    text_doc_undo(doc);
    printf("ui doc-undo inside refused %d\n", g_inside_refused && doc_is_str(doc, ""));
    printf("ui doc-undo inside redo %d\n", text_doc_redo(doc) && doc_is_str(doc, "ab"));
    free_text_doc(doc);
    return 0;
}

// ---- doc-highlight

static int g_asked[8];

// Each line one span of style 1, counted by its first letter; with `state`, a line holding "/*" and
// no "*/" opens a comment the next lines are in until one holds "*/".
static size_t count_lines(const char* line, size_t len, uint32_t* state, TextSpan* spans,
                          size_t max, void* user) {
    (void)user;
    if (len && line[0] >= 'a' && line[0] <= 'h')
        g_asked[line[0] - 'a']++;
    const bool opens = len >= 2 && memchr(line, '/', len);
    const bool closes = len >= 2 && memchr(line, '*', len) && line[0] == '*';
    if (*state == 1)
        *state = closes ? 0 : 1;
    else if (opens && line[0] != '*')
        *state = 1;
    if (max)
        spans[0] = (TextSpan){0, (uint32_t)len, (uint16_t)(*state ? 2 : 1)};
    return 1;
}

static int probe_doc_highlight(void) {
    // Lines colour on their own: one asked once, asked again only once changed.
    TextDoc* doc = create_text_doc("a\nb\nc\nd", 7);
    text_doc_set_highlighter(doc, count_lines, NULL, false);
    const TextSpan* spans = NULL;
    size_t count = 0;
    memset(g_asked, 0, sizeof(g_asked));
    text_doc_line_spans(doc, 2, &spans, &count);
    text_doc_line_spans(doc, 2, &spans, &count);
    printf("ui doc-highlight plain asked %d\n", g_asked[2] == 1);
    printf("ui doc-highlight plain only %d\n", g_asked[0] + g_asked[1] + g_asked[3] == 0);
    text_doc_replace(doc, 4, 5, "c", 1); // the same letter, the line changed
    text_doc_line_spans(doc, 2, &spans, &count);
    text_doc_line_spans(doc, 3, &spans, &count);
    printf("ui doc-highlight plain again %d\n", g_asked[2] == 2 && g_asked[3] == 1);
    free_text_doc(doc);

    // Lines that carry a state wait on the walk, and a changed line re-asks the next only while the
    // state it leaves has changed.
    doc = create_text_doc("a\nb /*\nc\n*/ d\ne", 15);
    text_doc_set_highlighter(doc, count_lines, NULL, true);
    printf("ui doc-highlight state waits %d\n", !text_doc_line_spans(doc, 2, &spans, &count));
    while (text_doc_highlight_step(doc, 5, 2))
        ;
    text_doc_line_spans(doc, 2, &spans, &count);
    printf("ui doc-highlight state inside %d\n", count == 1 && spans[0].style == 2);
    memset(g_asked, 0, sizeof(g_asked));
    text_doc_replace(doc, 4, 6, "", 0); // "b /*" loses its "/*"
    while (text_doc_highlight_step(doc, 5, 2))
        ;
    printf("ui doc-highlight state reasked %d\n", g_asked[1] == 1 && g_asked[2] == 1);
    printf("ui doc-highlight state kept %d\n", g_asked[4] == 0);
    text_doc_line_spans(doc, 2, &spans, &count);
    printf("ui doc-highlight state outside %d\n", count == 1 && spans[0].style == 1);
    free_text_doc(doc);
    return 0;
}

// ---- glyph-cache

#define EDITOR_FONT "assets/fonts/JetBrainsMono-Regular.ttf"

// Whether every glyph the cache holds is, in its atlas, the bitmap stb_truetype makes of it at the
// cache's scale, byte for byte: so no glyph was packed over another, and none moved as the atlas
// grew.
static int glyphs_wrong(const GlyphCache* c, const uint32_t* cps, size_t n, int* checked) {
    const stbtt_fontinfo* info = c->font_info;
    int wrong = 0;
    *checked = 0;
    // All asked for first, so a glyph packed early is checked after every later one, and every
    // growth of the atlas they cause, has had its chance to overwrite it.
    for (size_t i = 0; i < n; i++) {
        GlyphBitmap g;
        glyph_cache_get((GlyphCache*)c, cps[i], &g);
    }
    for (size_t i = 0; i < n; i++) {
        GlyphBitmap g;
        if (!glyph_cache_get((GlyphCache*)c, cps[i], &g) || g.missing || !g.width)
            continue;
        int w = 0, h = 0, xo = 0, yo = 0;
        unsigned char* want =
            stbtt_GetCodepointBitmap(info, c->scale, c->scale, (int)cps[i], &w, &h, &xo, &yo);
        const int x = (int)lroundf(g.uv[0] * (float)c->atlas.width);
        const int y = (int)lroundf(g.uv[1] * (float)c->atlas.height);
        bool same = want && w == g.width && h == g.height && xo == g.left && yo == g.top;
        for (int r = 0; same && r < h; r++)
            same = !memcmp(c->pixels + (size_t)(y + r) * (size_t)c->atlas.width + (size_t)x,
                           want + (size_t)r * (size_t)w, (size_t)w);
        stbtt_FreeBitmap(want, NULL);
        wrong += !same;
        (*checked)++;
    }
    return wrong;
}

static int probe_glyph_cache(void) {
    GlyphCache* c = create_glyph_cache(EDITOR_FONT, 26.0f);
    if (!c) {
        fprintf(stderr, "glyph-cache: %s did not load\n", EDITOR_FONT);
        return 1;
    }
    const stbtt_fontinfo* info = c->font_info;
    int advance, bearing;
    stbtt_GetCodepointHMetrics(info, 'M', &advance, &bearing);
    const float scale = stbtt_ScaleForPixelHeight(info, 26.0f);
    printf("ui glyph-cache cell rounded %d\n",
           c->cell_width == (int)lroundf((float)advance * scale));
    // Every codepoint the face has from ASCII through Cyrillic, enough to grow the atlas.
    static uint32_t cps[0x500];
    size_t n = 0;
    for (uint32_t cp = 0x20; cp < 0x500; cp++)
        if (stbtt_FindGlyphIndex(info, (int)cp))
            cps[n++] = cp;
    const int first_height = c->atlas.height;
    int checked = 0;
    const int wrong = glyphs_wrong(c, cps, n, &checked);
    printf("ui glyph-cache glyphs checked %d\n", checked);
    printf("ui glyph-cache glyphs wrong %d\n", wrong);
    printf("ui glyph-cache atlas grew %d\n", c->atlas.height > first_height);
    GlyphBitmap g;
    printf("ui glyph-cache missing box %d\n", glyph_cache_get(c, 0x4e00, &g) && g.missing);
    // The same size changes nothing; another empties the atlas and makes the glyphs again.
    const int pen = c->pen_x + c->pen_y * c->atlas.width;
    glyph_cache_set_size(c, 26.0f);
    printf("ui glyph-cache same kept %d\n", pen == c->pen_x + c->pen_y * c->atlas.width);
    glyph_cache_set_size(c, 39.0f);
    const float big = stbtt_ScaleForPixelHeight(info, 39.0f);
    printf("ui glyph-cache resize cell %d\n", c->cell_width == (int)lroundf((float)advance * big));
    const uint32_t some[] = {'A', 'g', '@', 0xe9, 0x3a9};
    const int resized_wrong = glyphs_wrong(c, some, 5, &checked);
    printf("ui glyph-cache resize redrawn %d\n", resized_wrong == 0 && checked == 5);
    free_glyph_cache(c);
    return 0;
}

// ---- The editor on screen

#define EDITOR_W 300.0f
#define EDITOR_H 220.0f

static GlyphCache* g_face;

// An editor of a fixed size alone on a screen, its face the editor's own at 13 points.
static UIElement* editor_fixture(UISystem* ui, TextDoc* doc, float w, float h) {
    if (!g_face)
        g_face = create_glyph_cache(EDITOR_FONT, 13.0f);
    UITheme theme = *ui_theme(ui);
    theme.editor_face = g_face;
    theme.editor_size = 13.0f;
    ui_set_theme(ui, &theme);
    UIScreen* s = ui_screen(ui, "editor");
    ui_screen_set_modal(s, false);
    UIElement* ed = ui_editor(ui_screen_root(s), doc);
    ui_set_size(ed, UI_FIXED, w, UI_FIXED, h);
    ui_push(ui, s);
    ui_set_focus(s, ed);
    return ed;
}

// One input pass of nothing and one build, so the editor is laid out and drawn.
static void editor_pass(UISystem* ui) {
    ui_update(ui, &(UIInput){.pointer_x = -1.0f, .pointer_y = -1.0f}, 1280.0f, 720.0f);
    ui_build(ui, 1280.0f, 720.0f, 1.0f);
}

static void say_flag(const char* which, const char* label, const char* key, bool ok) {
    printf("ui %s %s %s %d\n", which, label, key, ok ? 1 : 0);
}

static int probe_editor_wrap(void) {
    static const struct {
        const char* label;
        const char* text;
        int cols;
        size_t starts[4];
        size_t count;
        int hang;
    } cases[] = {
        {"space", "aaaa bbbb cccc", 10, {0, 10}, 2, 0},
        {"long", "abcdefghijklmnop", 5, {0, 5, 10, 15}, 4, 0},
        {"hang", "    word word word", 10, {0, 9, 14}, 3, 4},
        {"tab", "ab\tcd", 4, {0, 3}, 2, 0},
        {"utf8", "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", 2, {0, 4, 8}, 3, 0},
        {"wide", "        xx", 10, {0}, 1, 0},
        {"fits", "exactly ten", 11, {0}, 1, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t starts[8];
        int hang = -1;
        const size_t n = ui_editor_wrap(cases[i].text, strlen(cases[i].text), cases[i].cols, 4,
                                        starts, 8, &hang);
        bool ok = n == cases[i].count && hang == cases[i].hang;
        for (size_t k = 0; ok && k < n; k++)
            ok = starts[k] == cases[i].starts[k];
        if (!ok) {
            fprintf(stderr, "editor-wrap %s: %zu rows, hang %d:", cases[i].label, n, hang);
            for (size_t k = 0; k < n && k < 8; k++)
                fprintf(stderr, " %zu", starts[k]);
            fprintf(stderr, "\n");
        }
        say_flag("editor-wrap", cases[i].label, "rows", ok);
    }
    return 0;
}

// What a click lands on: every glyph the editor drew, clicked in the left quarter of its cell,
// lands on the offset whose cell that is; and every character's offset, clicked at its caret's
// place, lands on itself.
static int probe_editor_hit(UISystem* ui) {
    const char* text = "first line\twith a tab\n"
                       "  a long second line that wraps round the editor's narrow width more "
                       "than once, to be sure\n"
                       "short \xc3\xa9 \xc3\xbc \xce\xa9\n"
                       "\n"
                       "last";
    TextDoc* doc = create_text_doc(text, strlen(text));
    UIElement* ed = editor_fixture(ui, doc, EDITOR_W, EDITOR_H);
    editor_pass(ui);
    const UIDrawList* dl = ui_draw_list(ui);
    float tx = 0.0f, ty = 0.0f;
    ui_editor_point_of(ed, 0, &tx, &ty);
    // The cell's width in points, from two neighbouring carets.
    float x1 = 0.0f, y1 = 0.0f;
    ui_editor_point_of(ed, 1, &x1, &y1);
    const float cw = x1 - tx;
    int glyphs = 0, misplaced = 0;
    for (size_t i = 0; i < ui_draw_list_quad_count(dl); i++) {
        UIRect r;
        vec4 colour;
        int kind = 0;
        ui_draw_list_quad(dl, i, &r, colour, &kind);
        const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        if (kind != 1 || cx < tx)
            continue; // a fill, or a line number
        const float col = floorf((cx - tx) / cw);
        const float cell_x = tx + col * cw;
        const size_t at = ui_editor_offset_at(ed, cell_x + cw * 0.25f, cy);
        float px = 0.0f, py = 0.0f;
        const bool shown = ui_editor_point_of(ed, at, &px, &py);
        glyphs++;
        if (!shown || fabsf(px - cell_x) > 0.01f || cy < py) {
            misplaced++;
            fprintf(stderr, "editor-hit: a glyph at %.2f,%.2f clicked to byte %zu at %.2f,%.2f\n",
                    (double)cx, (double)cy, at, (double)px, (double)py);
        }
    }
    int round = 0, round_wrong = 0;
    for (size_t i = 0; i <= strlen(text); i++) {
        if (i < strlen(text) && text_utf8_is_continuation(text[i]))
            continue;
        float px = 0.0f, py = 0.0f;
        if (!ui_editor_point_of(ed, i, &px, &py))
            continue;
        round++;
        const size_t at = ui_editor_offset_at(ed, px, py + 2.0f);
        if (at != i) {
            round_wrong++;
            fprintf(stderr, "editor-hit: byte %zu at %.2f,%.2f clicked back to %zu\n", i,
                    (double)px, (double)py, at);
        }
    }
    printf("ui editor-hit glyphs checked %d\n", glyphs);
    printf("ui editor-hit glyphs wrong %d\n", misplaced);
    printf("ui editor-hit roundtrip checked %d\n", round);
    printf("ui editor-hit roundtrip wrong %d\n", round_wrong);
    return 0;
}

// What holds the view where it is: an edit above it, a guess put right, and a new width all keep
// the line at its top.
static int probe_editor_anchor(UISystem* ui) {
    // 300 lines, each a numbered sentence long enough to wrap in a narrow editor.
    static char text[300 * 128];
    size_t len = 0;
    for (int i = 0; i < 300; i++)
        len +=
            (size_t)snprintf(text + len, sizeof(text) - len,
                             "line %03d of a text whose lines are long enough to wrap twice\n", i);
    TextDoc* doc = create_text_doc(text, len);
    UIElement* ed = editor_fixture(ui, doc, 200.0f, EDITOR_H);
    editor_pass(ui);
    // Down to line 150, by revealing it: every line above is still a guess.
    ui_editor_reveal(ed, text_doc_line_start(doc, 150));
    editor_pass(ui);
    size_t line = 0, row = 0;
    int cols = 0;
    ui_editor_top(ed, &line, &row, &cols);
    size_t want_len = 0;
    const char* top = text_doc_line(doc, line, &want_len);
    char want[128];
    memcpy(want, top, want_len);
    // Three lines put in above it.
    text_doc_replace(doc, text_doc_line_start(doc, 10), text_doc_line_start(doc, 10),
                     "new\nnew\nnew\n", 12);
    editor_pass(ui);
    size_t l2 = 0, r2 = 0;
    int c2 = 0;
    ui_editor_top(ed, &l2, &r2, &c2);
    size_t got_len = 0;
    const char* got = text_doc_line(doc, l2, &got_len);
    say_flag("editor-anchor", "edit", "kept",
             l2 == line + 3 && r2 == row && got_len == want_len && !memcmp(got, want, want_len));
    // Every line above measured: clicking far above walks the rows to the top, wrapping each.
    ui_editor_offset_at(ed, 0.0f, -100000.0f);
    editor_pass(ui);
    size_t l3 = 0, r3 = 0;
    int c3 = 0;
    ui_editor_top(ed, &l3, &r3, &c3);
    say_flag("editor-anchor", "measured", "kept", l3 == l2 && r3 == r2);
    // A new width wraps every line again, and keeps the line.
    ui_set_size(ed, UI_FIXED, 320.0f, UI_FIXED, EDITOR_H);
    editor_pass(ui);
    size_t l4 = 0, r4 = 0;
    int c4 = 0;
    ui_editor_top(ed, &l4, &r4, &c4);
    say_flag("editor-anchor", "resize", "kept", l4 == l2 && c4 != c3);
    // And the wheel moves it a row at a time.
    ui_update(ui,
              &(UIInput){.pointer_x = ed->rect.x + 50.0f,
                         .pointer_y = ed->rect.y + 50.0f,
                         .wheel_y = -1.0f},
              1280.0f, 720.0f);
    size_t l5 = 0, r5 = 0;
    int c5 = 0;
    ui_editor_top(ed, &l5, &r5, &c5);
    say_flag("editor-anchor", "wheel", "moved", l5 > l4 || (l5 == l4 && r5 > r4));
    return 0;
}

// ---- Keys

static double g_key_time;

// One input pass carrying `events`, the pointer off the editor, and one build.
static void send_keys(UISystem* ui, const InputKeyEvent* events, int count) {
    ui_update(ui,
              &(UIInput){.pointer_x = -1.0f,
                         .pointer_y = -1.0f,
                         .key_events = events,
                         .key_event_count = count,
                         .time = (g_key_time += 1.0)},
              1280.0f, 720.0f);
    ui_build(ui, 1280.0f, 720.0f, 1.0f);
}

static void key(UISystem* ui, int k, int mods) {
    const InputKeyEvent e = {.key = k, .mods = mods};
    send_keys(ui, &e, 1);
}

static void type(UISystem* ui, const char* text) {
    InputKeyEvent e[64] = {0};
    int n = 0;
    for (const char* p = text; *p && n < 64;)
        e[n++] = (InputKeyEvent){.codepoint = text_utf8_next(&p)};
    send_keys(ui, e, n);
}

// A press at (x0, y0) held to (x1, y1) over a pass, then let go there, a second apart.
static void press_drag(UISystem* ui, float x0, float y0, float x1, float y1, int mods) {
    ui_update(ui,
              &(UIInput){.pointer_x = x0,
                         .pointer_y = y0,
                         .pointer_pressed = true,
                         .pointer_down = true,
                         .pointer_mods = mods,
                         .time = (g_key_time += 1.0)},
              1280.0f, 720.0f);
    ui_update(
        ui,
        &(UIInput){
            .pointer_x = x1, .pointer_y = y1, .pointer_down = true, .time = (g_key_time += 1.0)},
        1280.0f, 720.0f);
    ui_update(ui,
              &(UIInput){.pointer_x = x1,
                         .pointer_y = y1,
                         .pointer_released = true,
                         .time = (g_key_time += 1.0)},
              1280.0f, 720.0f);
    ui_build(ui, 1280.0f, 720.0f, 1.0f);
}

// The editor's one selection is (anchor, head).
static bool one_selection(const UIElement* ed, size_t anchor, size_t head) {
    const TextSelection* s = NULL;
    const size_t n = ui_editor_selections(ed, &s, NULL);
    if (n != 1 || s[0].anchor != anchor || s[0].head != head) {
        fprintf(stderr, "  selections %zu, the first %zu %zu, want %zu %zu\n", n,
                n ? s[0].anchor : 0, n ? s[0].head : 0, anchor, head);
        return false;
    }
    return true;
}

static void reset(TextDoc* doc, UIElement* ed, const char* text, size_t anchor, size_t head) {
    text_doc_set_text(doc, text, strlen(text));
    ui_editor_set_selections(ed, &(TextSelection){anchor, head}, 1);
}

#define KEYS_TEXT "alpha beta\n  gamma(delta)\nlast"
#define WORD      INPUT_MOD_WORD
#define SHORT     INPUT_MOD_SHORTCUT

static int probe_editor_keys(UISystem* ui) {
    TextDoc* doc = create_text_doc(KEYS_TEXT, strlen(KEYS_TEXT));
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    editor_pass(ui);
    Engine* engine = ui_system_engine(ui);
    reset(doc, ed, KEYS_TEXT, 0, 0);
    key(ui, GLFW_KEY_RIGHT, 0);
    say_flag("editor-keys", "right", "ok", one_selection(ed, 1, 1));
    key(ui, GLFW_KEY_RIGHT, GLFW_MOD_SHIFT);
    say_flag("editor-keys", "shift_right", "ok", one_selection(ed, 1, 2));
    reset(doc, ed, KEYS_TEXT, 1, 3);
    key(ui, GLFW_KEY_LEFT, 0);
    say_flag("editor-keys", "left_collapse", "ok", one_selection(ed, 1, 1));
    reset(doc, ed, KEYS_TEXT, 0, 0);
    key(ui, GLFW_KEY_RIGHT, WORD);
#ifdef __APPLE__
    say_flag("editor-keys", "word_right", "ok", one_selection(ed, 5, 5)); // the word's end
#else
    say_flag("editor-keys", "word_right", "ok", one_selection(ed, 6, 6)); // the next word's start
#endif
    reset(doc, ed, KEYS_TEXT, 9, 9);
    key(ui, GLFW_KEY_LEFT, WORD);
    say_flag("editor-keys", "word_left", "ok", one_selection(ed, 6, 6));
    reset(doc, ed, KEYS_TEXT, 3, 3);
    key(ui, GLFW_KEY_DOWN, 0);
    say_flag("editor-keys", "down", "ok", one_selection(ed, 14, 14));
    key(ui, GLFW_KEY_END, 0);
    say_flag("editor-keys", "end", "ok", one_selection(ed, 25, 25));
    reset(doc, ed, KEYS_TEXT, 20, 20);
    key(ui, GLFW_KEY_HOME, 0);
    const bool first = one_selection(ed, 13, 13);
    key(ui, GLFW_KEY_HOME, 0);
    say_flag("editor-keys", "home", "ok", first && one_selection(ed, 11, 11));
#ifdef __APPLE__
    reset(doc, ed, KEYS_TEXT, 20, 20);
    key(ui, GLFW_KEY_UP, GLFW_MOD_SUPER);
#else
    reset(doc, ed, KEYS_TEXT, 20, 20);
    key(ui, GLFW_KEY_HOME, GLFW_MOD_CONTROL);
#endif
    say_flag("editor-keys", "doc_start", "ok", one_selection(ed, 0, 0));
    reset(doc, ed, KEYS_TEXT, 5, 5);
    key(ui, GLFW_KEY_BACKSPACE, 0);
    say_flag("editor-keys", "backspace", "ok",
             doc_is_str(doc, "alph beta\n  gamma(delta)\nlast") && one_selection(ed, 4, 4));
    reset(doc, ed, KEYS_TEXT, 0, 0);
    key(ui, GLFW_KEY_DELETE, 0);
    say_flag("editor-keys", "delete", "ok",
             doc_is_str(doc, "lpha beta\n  gamma(delta)\nlast") && one_selection(ed, 0, 0));
    reset(doc, ed, KEYS_TEXT, 10, 10);
    key(ui, GLFW_KEY_BACKSPACE, WORD);
    say_flag("editor-keys", "word_delete", "ok",
             doc_is_str(doc, "alpha \n  gamma(delta)\nlast") && one_selection(ed, 6, 6));
    reset(doc, ed, KEYS_TEXT, 24, 24);
    key(ui, GLFW_KEY_ENTER, 0);
    say_flag("editor-keys", "enter_indents", "ok",
             doc_is_str(doc, "alpha beta\n  gamma(delta\n  )\nlast") && one_selection(ed, 27, 27));
    reset(doc, ed, KEYS_TEXT, 26, 26);
    key(ui, GLFW_KEY_TAB, 0);
    const bool tabbed = doc_is_str(doc, "alpha beta\n  gamma(delta)\n    last");
    key(ui, GLFW_KEY_TAB, GLFW_MOD_SHIFT);
    say_flag("editor-keys", "tab_outdent", "ok", tabbed && doc_is_str(doc, KEYS_TEXT));
    reset(doc, ed, KEYS_TEXT, 3, 3);
    key(ui, GLFW_KEY_A, SHORT);
    say_flag("editor-keys", "select_all", "ok", one_selection(ed, 0, strlen(KEYS_TEXT)));
    reset(doc, ed, KEYS_TEXT, 6, 10);
    key(ui, GLFW_KEY_C, SHORT);
    ui_editor_set_selections(ed, &(TextSelection){0, 0}, 1);
    key(ui, GLFW_KEY_V, SHORT);
    say_flag("editor-keys", "copy_paste", "ok",
             !strcmp(engine_clipboard_get(engine), "beta") &&
                 doc_is_str(doc, "betaalpha beta\n  gamma(delta)\nlast"));
    reset(doc, ed, KEYS_TEXT, 0, 5);
    key(ui, GLFW_KEY_X, SHORT);
    say_flag("editor-keys", "cut", "ok",
             !strcmp(engine_clipboard_get(engine), "alpha") &&
                 doc_is_str(doc, " beta\n  gamma(delta)\nlast"));
    reset(doc, ed, KEYS_TEXT, 0, 0);
    type(ui, "zz\xc3\xa9");
    const bool typed = doc_is_str(doc, "zz\xc3\xa9" KEYS_TEXT) && one_selection(ed, 4, 4);
    key(ui, GLFW_KEY_Z, SHORT);
    const bool undone = doc_is_str(doc, KEYS_TEXT) && one_selection(ed, 0, 0);
    key(ui, GLFW_KEY_Z, SHORT | GLFW_MOD_SHIFT);
    say_flag("editor-keys", "type_undo_redo", "ok",
             typed && undone && doc_is_str(doc, "zz\xc3\xa9" KEYS_TEXT));
    // Every command listed once, by a name and a description.
    size_t count = 0;
    const UIEditorCommand* cmds = ui_editor_commands(&count);
    bool listed = count >= 40;
    for (size_t i = 0; listed && i < count; i++) {
        listed = cmds[i].name && cmds[i].description && cmds[i].run;
        for (size_t k = i + 1; listed && k < count; k++)
            listed = strcmp(cmds[i].name, cmds[k].name) != 0;
    }
    say_flag("editor-keys", "commands", "listed", listed);
    return 0;
}

static bool selections_are(const UIElement* ed, const size_t* heads, size_t n) {
    const TextSelection* s = NULL;
    const size_t count = ui_editor_selections(ed, &s, NULL);
    bool ok = count == n;
    for (size_t i = 0; ok && i < n; i++)
        ok = s[i].head == heads[i];
    if (!ok) {
        fprintf(stderr, "  %zu selections:", count);
        for (size_t i = 0; i < count; i++)
            fprintf(stderr, " %zu-%zu", s[i].anchor, s[i].head);
        fprintf(stderr, "\n");
    }
    return ok;
}

#define CARETS_TEXT "one\ntwo\nthree\nfour\nfive"

static int probe_editor_carets(UISystem* ui) {
    TextDoc* doc = create_text_doc(CARETS_TEXT, strlen(CARETS_TEXT));
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    editor_pass(ui);
    // Typing at five carets, one at each line's start: one action.
    const TextSelection starts[] = {{0, 0}, {4, 4}, {8, 8}, {14, 14}, {19, 19}};
    ui_editor_set_selections(ed, starts, 5);
    type(ui, "x");
    const size_t after[] = {1, 6, 11, 18, 24};
    say_flag("editor-carets", "type", "five",
             doc_is_str(doc, "xone\nxtwo\nxthree\nxfour\nxfive") && selections_are(ed, after, 5));
    // Selections that overlap are one.
    text_doc_set_text(doc, CARETS_TEXT, strlen(CARETS_TEXT));
    const TextSelection overlap[] = {{0, 3}, {2, 6}};
    ui_editor_set_selections(ed, overlap, 2);
    say_flag("editor-carets", "merge", "one", one_selection(ed, 0, 6));
    // Five lines pasted at five carets: one each.
    ui_editor_set_selections(ed, starts, 5);
    engine_clipboard_set(ui_system_engine(ui), "A\nB\nC\nD\nE");
    key(ui, GLFW_KEY_V, SHORT);
    say_flag("editor-carets", "paste", "split",
             doc_is_str(doc, "Aone\nBtwo\nCthree\nDfour\nEfive"));
    // Cmd+D: the word at the caret, then its next match, then the one after.
    text_doc_set_text(doc, "two one two\ntwo", 15);
    ui_editor_set_selections(ed, &(TextSelection){1, 1}, 1);
    key(ui, GLFW_KEY_D, SHORT);
    key(ui, GLFW_KEY_D, SHORT);
    key(ui, GLFW_KEY_D, SHORT);
    const size_t matches[] = {3, 11, 15};
    say_flag("editor-carets", "next_match", "three", selections_are(ed, matches, 3));
    // Esc: back to the primary alone, the last added.
    key(ui, GLFW_KEY_ESCAPE, 0);
    say_flag("editor-carets", "escape", "one", one_selection(ed, 12, 15));
    // A caret a row below, twice, at the same column.
    text_doc_set_text(doc, CARETS_TEXT, strlen(CARETS_TEXT));
    ui_editor_set_selections(ed, &(TextSelection){1, 1}, 1);
#ifdef __APPLE__
    key(ui, GLFW_KEY_DOWN, GLFW_MOD_SUPER | GLFW_MOD_ALT);
    key(ui, GLFW_KEY_DOWN, GLFW_MOD_SUPER | GLFW_MOD_ALT);
#else
    key(ui, GLFW_KEY_DOWN, GLFW_MOD_CONTROL | GLFW_MOD_ALT);
    key(ui, GLFW_KEY_DOWN, GLFW_MOD_CONTROL | GLFW_MOD_ALT);
#endif
    const size_t column[] = {1, 5, 9};
    say_flag("editor-carets", "column", "three", selections_are(ed, column, 3));
    // Option+click adds a caret where it lands.
    ui_editor_set_selections(ed, &(TextSelection){0, 0}, 1);
    float x = 0.0f, y = 0.0f;
    ui_editor_point_of(ed, 9, &x, &y);
    ui_update(ui,
              &(UIInput){.pointer_x = x + 1.0f,
                         .pointer_y = y + 4.0f,
                         .pointer_pressed = true,
                         .pointer_down = true,
                         .pointer_mods = GLFW_MOD_ALT,
                         .time = (g_key_time += 1.0)},
              1280.0f, 720.0f);
    ui_update(ui,
              &(UIInput){.pointer_x = x + 1.0f,
                         .pointer_y = y + 4.0f,
                         .pointer_released = true,
                         .time = (g_key_time += 1.0)},
              1280.0f, 720.0f);
    const size_t clicked[] = {0, 9};
    say_flag("editor-carets", "alt_click", "two", selections_are(ed, clicked, 2));
    // The bar's thumb dragged scrolls the view, and a drag in the text after it selects: the bar
    // is the UI's, and lets go when the pointer does.
    char lines[200 * 8];
    size_t n = 0;
    for (int i = 0; i < 200; i++)
        n += (size_t)snprintf(lines + n, sizeof(lines) - n, "line%d\n", i);
    text_doc_set_text(doc, lines, n);
    ui_editor_set_selections(ed, &(TextSelection){0, 0}, 1);
    editor_pass(ui);
    press_drag(ui, ed->rect.x + ed->rect.w - 4.0f, ed->rect.y + 8.0f,
               ed->rect.x + ed->rect.w - 4.0f, ed->rect.y + 108.0f, 0);
    size_t top = 0, top_row = 0;
    int cols = 0;
    ui_editor_top(ed, &top, &top_row, &cols);
    press_drag(ui, ed->rect.x + 120.0f, ed->rect.y + 60.0f, ed->rect.x + 180.0f,
               ed->rect.y + 100.0f, 0);
    const TextSelection* s = NULL;
    ui_editor_selections(ed, &s, NULL);
    say_flag("editor-carets", "bar_then_drag", "selects", top > 0 && s[0].anchor != s[0].head);
    text_doc_set_text(doc, CARETS_TEXT, strlen(CARETS_TEXT));
    editor_pass(ui);
    // Find: the next match of the query after the caret, selected.
    ui_editor_set_selections(ed, &(TextSelection){0, 0}, 1);
    ui_editor_find(ed, "f", 0);
    const bool found = ui_editor_run(ed, "find-next") && one_selection(ed, 14, 15);
    const bool again = ui_editor_run(ed, "find-next") && one_selection(ed, 19, 20);
    say_flag("editor-carets", "find", "next", found && again);
    // Selections that touch, typed over twice: each becomes a caret after its own new text, not
    // after the next one's.
    text_doc_set_text(doc, "\nx", 2);
    const TextSelection touching[] = {{0, 1}, {1, 2}};
    ui_editor_set_selections(ed, touching, 2);
    type(ui, "z");
    type(ui, "y");
    const size_t landed[] = {2, 4};
    say_flag("editor-carets", "touching", "typed",
             doc_is_str(doc, "zyzy") && selections_are(ed, landed, 2));
    // A caret where a selection starts is one with it, whichever came first.
    text_doc_set_text(doc, "\nx", 2);
    const TextSelection at_start[] = {{0, 0}, {1, 0}};
    ui_editor_set_selections(ed, at_start, 2);
    type(ui, "z");
    say_flag("editor-carets", "caret_at_start", "merged",
             doc_is_str(doc, "zx") && one_selection(ed, 1, 1));
    return 0;
}

// ---- A vim-like mode, built through the public calls alone

// The modes and what the normal one has read so far: a count, and an operator waiting for its
// motion. Both modes carry it as their `user`, as a host's would.
typedef struct Vim {
    UIEditorMode normal, insert;
    int count, pending;
} Vim;

static bool vim_normal(UIElement* el, const InputKeyEvent* k, void* user) {
    Vim* v = user;
    if (!k->codepoint)
        return false; // a key: the standard mode's, below
    const uint32_t c = k->codepoint;
    if (c >= '1' && c <= '9') {
        v->count = v->count * 10 + (int)(c - '0');
        return true;
    }
    const int n = v->count ? v->count : 1;
    v->count = 0;
    if (v->pending == 'd') {
        v->pending = 0;
        if (c == 'w')
            ui_editor_apply(el, UI_OP_DELETE, UI_MOTION_WORD_END, n);
        return true;
    }
    switch (c) {
        case 'x':
            ui_editor_apply(el, UI_OP_DELETE, UI_MOTION_CHAR_RIGHT, n);
            break;
        case 'i':
            ui_editor_set_mode(el, &v->insert);
            break;
        case 'd':
            v->pending = 'd';
            break;
        case 'j':
            ui_editor_apply(el, UI_OP_MOVE, UI_MOTION_LINE_DOWN, n);
            break;
        default:
            break;
    }
    return true; // a normal mode takes every character, and types none
}

static bool vim_insert(UIElement* el, const InputKeyEvent* k, void* user) {
    Vim* v = user;
    if (k->key != GLFW_KEY_ESCAPE)
        return false;
    ui_editor_set_mode(el, &v->normal);
    return true;
}

#define MODE_TEXT "hello world\nsecond line\nthird line"

static int probe_editor_mode(UISystem* ui) {
    static Vim vim;
    vim.normal = (UIEditorMode){.name = "normal",
                                .parent = ui_editor_standard_mode(),
                                .read_key = vim_normal,
                                .user = &vim,
                                .block_caret = true};
    vim.insert = (UIEditorMode){.name = "insert",
                                .parent = ui_editor_standard_mode(),
                                .read_key = vim_insert,
                                .user = &vim,
                                .types = true};
    TextDoc* doc = create_text_doc(MODE_TEXT, strlen(MODE_TEXT));
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    editor_pass(ui);
    ui_editor_set_mode(ed, &vim.normal);
    type(ui, "x");
    say_flag("editor-mode", "x", "deletes", doc_is_str(doc, "ello world\nsecond line\nthird line"));
    type(ui, "dw");
    say_flag("editor-mode", "dw", "deletes", doc_is_str(doc, " world\nsecond line\nthird line"));
    type(ui, "2j");
    say_flag("editor-mode", "count", "moves", one_selection(ed, 19, 19));
    type(ui, "iZ");
    say_flag("editor-mode", "insert", "types",
             doc_is_str(doc, " world\nsecond line\nZthird line") &&
                 !strcmp(ui_editor_mode(ed)->name, "insert"));
    key(ui, GLFW_KEY_RIGHT, 0); // the standard mode's key, under insert
    say_flag("editor-mode", "inherits", "arrow", one_selection(ed, 21, 21));
    key(ui, GLFW_KEY_ESCAPE, 0);
    type(ui, "q");
    say_flag("editor-mode", "escape", "normal",
             !strcmp(ui_editor_mode(ed)->name, "normal") &&
                 doc_is_str(doc, " world\nsecond line\nZthird line"));
    return 0;
}

// ---- A host that keeps its own undo

static int g_host_undos;

static bool host_undo(UIElement* el, void* user) {
    (void)el;
    (void)user;
    g_host_undos++;
    return true;
}

static const UIEditorCommand HOST_UNDO = {"host-undo", "The host's own undo", host_undo, NULL};
static const UIEditorBinding HOST_KEYS[] = {{GLFW_KEY_Z, INPUT_MOD_SHORTCUT, &HOST_UNDO}};

static int probe_editor_host_undo(UISystem* ui) {
    TextDoc* doc = create_text_doc("", 0);
    doc->keep_undo = false;
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    editor_pass(ui);
    const UIEditorMode host = {.name = "host",
                               .parent = ui_editor_standard_mode(),
                               .bindings = HOST_KEYS,
                               .binding_count = 1,
                               .layer = true};
    ui_editor_set_mode(ed, &host);
    type(ui, "ab");
    key(ui, GLFW_KEY_Z, SHORT);
    say_flag("editor-host-undo", "host", "ran", g_host_undos == 1);
    say_flag("editor-host-undo", "text", "kept", doc_is_str(doc, "ab"));
    say_flag("editor-host-undo", "own", "refused", !ui_editor_run(ed, "undo"));
    ui_editor_set_mode(ed, NULL);
    return 0;
}

// ---- The seams a mode stands on (spec 14.5)

// How often the layer's command and the base's have run.
static int g_seam_runs[2];

static bool seam_run(UIElement* el, void* user) {
    (void)el;
    (*(int*)user)++;
    return true;
}

static bool seam_decline(UIElement* el, void* user) {
    (void)el;
    (void)user;
    return false;
}

static const UIEditorCommand SEAM_LAYER = {"seam-layer", "The layer's own", seam_run,
                                           &g_seam_runs[0]};
static const UIEditorCommand SEAM_BASE = {"seam-base", "The base's own", seam_run, &g_seam_runs[1]};
static const UIEditorCommand SEAM_DECLINE = {"seam-decline", "Does nothing", seam_decline, NULL};
static const UIEditorBinding SEAM_LAYER_KEYS[] = {{GLFW_KEY_F6, 0, &SEAM_LAYER},
                                                  {GLFW_KEY_SPACE, 0, &SEAM_DECLINE}};
static const UIEditorBinding SEAM_BASE_KEYS[] = {{GLFW_KEY_SPACE, 0, &SEAM_BASE}};

#define SEAM_TEXT "hello world\nsecond line\nthird"

// The flat quads drawn on the row at y in colour c: how many cells wide they come to together.
static float band_cells(const UISystem* ui, float y, const float* c, float cell) {
    const UIDrawList* dl = ui_draw_list(ui);
    float w = 0.0f;
    for (size_t i = 0; i < ui_draw_list_quad_count(dl); i++) {
        UIRect r;
        vec4 colour;
        int kind = 0;
        ui_draw_list_quad(dl, i, &r, colour, &kind);
        if (kind == 0 && fabsf(r.y - y) < 0.01f && fabsf(colour[0] - c[0]) < 1e-4f &&
            fabsf(colour[1] - c[1]) < 1e-4f && fabsf(colour[2] - c[2]) < 1e-4f &&
            fabsf(colour[3] - c[3]) < 1e-4f)
            w += r.w;
    }
    return w / cell;
}

// The width of the caret drawn at `offset`: the narrowest flat quad starting at its place.
static float caret_drawn(UISystem* ui, UIElement* ed, size_t offset) {
    float x = 0.0f, y = 0.0f;
    if (!ui_editor_point_of(ed, offset, &x, &y))
        return -1.0f;
    const UIDrawList* dl = ui_draw_list(ui);
    float w = -1.0f;
    for (size_t i = 0; i < ui_draw_list_quad_count(dl); i++) {
        UIRect r;
        vec4 colour;
        int kind = 0;
        ui_draw_list_quad(dl, i, &r, colour, &kind);
        if (kind == 0 && fabsf(r.x - x) < 0.01f && fabsf(r.y - y) < 0.01f && (w < 0 || r.w < w))
            w = r.w;
    }
    return w;
}

// A layer takes its base's caret, typing and selection shape, and adds its own keys.
static int probe_editor_layer(UISystem* ui) {
    TextDoc* doc = create_text_doc(SEAM_TEXT, strlen(SEAM_TEXT));
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    const UIEditorMode base = {.name = "base", .block_caret = true};
    const UIEditorMode layer = {.name = "layer",
                                .parent = &base,
                                .bindings = SEAM_LAYER_KEYS,
                                .binding_count = 1,
                                .layer = true,
                                .types = true};
    ui_editor_set_mode(ed, &layer);
    editor_pass(ui);
    float x0 = 0.0f, x1 = 0.0f, y = 0.0f;
    ui_editor_point_of(ed, 0, &x0, &y);
    ui_editor_point_of(ed, 1, &x1, &y);
    say_flag("editor-layer", "caret", "block", fabsf(caret_drawn(ui, ed, 0) - (x1 - x0)) < 0.01f);
    type(ui, "a");
    say_flag("editor-layer", "typing", "base", doc_is_str(doc, SEAM_TEXT));
    key(ui, GLFW_KEY_F6, 0);
    say_flag("editor-layer", "keys", "own", g_seam_runs[0] == 1);
    say_flag("editor-layer", "name", "set", !strcmp(ui_editor_mode(ed)->name, "layer"));
    ui_editor_set_mode(ed, NULL);
    free_text_doc(doc);
    return 0;
}

// A layer's command that does nothing hands its key to the base, whose binding then runs.
static int probe_editor_decline(UISystem* ui) {
    TextDoc* doc = create_text_doc(SEAM_TEXT, strlen(SEAM_TEXT));
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    const UIEditorMode base = {
        .name = "base", .bindings = SEAM_BASE_KEYS, .binding_count = 1, .types = true};
    const UIEditorMode layer = {.name = "layer",
                                .parent = &base,
                                .bindings = SEAM_LAYER_KEYS,
                                .binding_count = 2,
                                .layer = true};
    ui_editor_set_mode(ed, &layer);
    editor_pass(ui);
    g_seam_runs[1] = 0;
    key(ui, GLFW_KEY_SPACE, 0);
    say_flag("editor-decline", "declined", "parent", g_seam_runs[1] == 1);
    ui_editor_set_mode(ed, NULL);
    free_text_doc(doc);
    return 0;
}

// A press is a key and then its character: a binding taking the key takes the character too, and a
// character arriving alone is typed.
static int probe_editor_paired_char(UISystem* ui) {
    TextDoc* doc = create_text_doc("", 0);
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    const UIEditorMode base = {
        .name = "base", .bindings = SEAM_BASE_KEYS, .binding_count = 1, .types = true};
    ui_editor_set_mode(ed, &base);
    editor_pass(ui);
    g_seam_runs[1] = 0;
    const InputKeyEvent press[] = {{.key = GLFW_KEY_SPACE}, {.codepoint = ' '}};
    send_keys(ui, press, 2);
    say_flag("editor-paired-char", "used", "swallowed", g_seam_runs[1] == 1 && doc_is_str(doc, ""));
    type(ui, " ");
    say_flag("editor-paired-char", "alone", "typed", doc_is_str(doc, " "));
    const InputKeyEvent letter[] = {{.key = GLFW_KEY_A}, {.codepoint = 'a'}};
    send_keys(ui, letter, 2);
    say_flag("editor-paired-char", "unbound", "typed", doc_is_str(doc, " a"));
    ui_editor_set_mode(ed, NULL);
    free_text_doc(doc);
    return 0;
}

// The selection {2, 5} on the first line drawn as each shape: three cells, four with the character
// at its far end, and the line whole with its line break; the block caret over the character at
// the head.
static int probe_editor_shapes(UISystem* ui) {
    TextDoc* doc = create_text_doc(SEAM_TEXT, strlen(SEAM_TEXT));
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    const UITheme* t = ui_theme(ui);
    vec4 sel = {t->selection[0], t->selection[1], t->selection[2], t->selection[3]};
    if (sel[3] <= 0.0f)
        glm_vec4_copy((vec4){0.25f, 0.45f, 0.85f, 0.45f}, sel);
    static const struct {
        const char* label;
        UIEditorSelectionShape shape;
        float cells;
    } shapes[] = {
        {"span", UI_SELECTION_SPAN, 3.0f},
        {"inclusive", UI_SELECTION_INCLUSIVE, 4.0f},
        {"lines", UI_SELECTION_LINES, 12.0f},
    };
    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        const UIEditorMode mode = {.name = shapes[i].label,
                                   .types = true,
                                   .block_caret = true,
                                   .selection = shapes[i].shape};
        ui_editor_set_mode(ed, &mode);
        ui_editor_set_selections(ed, &(TextSelection){2, 5}, 1);
        editor_pass(ui);
        float x0 = 0.0f, x1 = 0.0f, y = 0.0f;
        ui_editor_point_of(ed, 0, &x0, &y);
        ui_editor_point_of(ed, 1, &x1, &y);
        const float cells = band_cells(ui, y, sel, x1 - x0);
        if (fabsf(cells - shapes[i].cells) > 0.01f)
            fprintf(stderr, "editor-shapes %s: %.2f cells, want %.0f\n", shapes[i].label,
                    (double)cells, (double)shapes[i].cells);
        say_flag("editor-shapes", shapes[i].label, "band", fabsf(cells - shapes[i].cells) < 0.01f);
        if (i == 1)
            say_flag("editor-shapes", "caret", "head",
                     fabsf(caret_drawn(ui, ed, 5) - (x1 - x0)) < 0.01f);
    }
    ui_editor_set_mode(ed, NULL);
    free_text_doc(doc);
    return 0;
}

// Each action told with its step: typing joined into words shares one, a group holds one open over
// any actions -- typing, a Backspace, more typing -- and one undo takes it all back. The same with
// keep_undo off.
static uint64_t g_steps[16];
static int g_step_count;

static void note_step(TextDoc* doc, const TextDocAction* action, void* user) {
    (void)doc;
    (void)user;
    if (g_step_count < 16)
        g_steps[g_step_count++] = action->step;
}

static int probe_editor_steps(UISystem* ui) {
    for (int keep = 1; keep >= 0; keep--) {
        TextDoc* doc = create_text_doc("", 0);
        doc->keep_undo = keep;
        UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
        editor_pass(ui);
        text_doc_set_on_action(doc, note_step, NULL);
        g_step_count = 0;
        type(ui, "ab c");
        const char* which = keep ? "kept" : "off";
        // "ab" one step, " c" the next.
        say_flag("editor-steps", which, "words",
                 g_step_count == 4 && g_steps[0] == g_steps[1] && g_steps[2] != g_steps[1] &&
                     g_steps[3] == g_steps[2]);
        g_step_count = 0;
        text_doc_group(doc, true, NULL);
        type(ui, "x");
        key(ui, GLFW_KEY_BACKSPACE, 0);
        type(ui, "yz");
        text_doc_group(doc, false, NULL);
        type(ui, "w");
        bool grouped = g_step_count == 5 && g_steps[0] != 0;
        for (int i = 1; grouped && i < 4; i++)
            grouped = g_steps[i] == g_steps[0];
        say_flag("editor-steps", which, "grouped", grouped && g_steps[4] != g_steps[0]);
        if (keep) {
            // "w", then the group whole: "ab" and " c" are the steps left.
            text_doc_undo(doc);
            text_doc_undo(doc);
            say_flag("editor-steps", which, "undone",
                     doc_is_str(doc, "ab c") && doc->undo_count == 2);
            text_doc_redo(doc);
            say_flag("editor-steps", which, "redone", doc_is_str(doc, "ab cyz"));
        }
        ui_editor_set_mode(ed, NULL);
        free_text_doc(doc);
    }
    return 0;
}

// ---- The vim mode against vim itself (spec 14.5)

// A hex digit's value; -1 for none.
static int hex_digit(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

// `hex` decoded into a buffer the caller frees, NUL-terminated; *n its length.
static char* unhex(const char* hex, size_t len, size_t* n) {
    char* out = malloc(len / 2 + 1);
    if (!out)
        return NULL;
    *n = 0;
    for (size_t i = 0; i + 1 < len; i += 2)
        out[(*n)++] = (char)(hex_digit(hex[i]) * 16 + hex_digit(hex[i + 1]));
    out[*n] = '\0';
    return out;
}

// Vim's notation for keys -- <Esc>, <CR>, <BS>, <Del>, <Tab>, the arrows, <C-x>, <lt> -- read
// into the events a press makes: a key for each of those, a character for anything else.
static size_t vim_events(const char* keys, InputKeyEvent* out, size_t max) {
    static const struct {
        const char* name;
        int key;
    } named[] = {{"Esc", GLFW_KEY_ESCAPE},  {"CR", GLFW_KEY_ENTER}, {"BS", GLFW_KEY_BACKSPACE},
                 {"Del", GLFW_KEY_DELETE},  {"Tab", GLFW_KEY_TAB},  {"Left", GLFW_KEY_LEFT},
                 {"Right", GLFW_KEY_RIGHT}, {"Up", GLFW_KEY_UP},    {"Down", GLFW_KEY_DOWN},
                 {"Home", GLFW_KEY_HOME},   {"End", GLFW_KEY_END}};
    size_t n = 0;
    for (const char* p = keys; *p && n < max;) {
        if (*p == '<') {
            const char* end = strchr(p, '>');
            const size_t len = end ? (size_t)(end - p - 1) : 0;
            if (end && len == 2 && !strncmp(p + 1, "lt", 2)) {
                out[n++] = (InputKeyEvent){.codepoint = '<'};
                p = end + 1;
                continue;
            }
            if (end && len == 3 && (p[1] == 'C' || p[1] == 'c') && p[2] == '-') {
                const char ch = p[3];
                if (ch == '[')
                    out[n++] =
                        (InputKeyEvent){.key = GLFW_KEY_LEFT_BRACKET, .mods = GLFW_MOD_CONTROL};
                else
                    out[n++] = (InputKeyEvent){.key = GLFW_KEY_A + (ch | 0x20) - 'a',
                                               .mods = GLFW_MOD_CONTROL};
                p = end + 1;
                continue;
            }
            bool found = false;
            for (size_t k = 0; end && k < sizeof(named) / sizeof(named[0]); k++) {
                if (strlen(named[k].name) == len && !strncmp(p + 1, named[k].name, len)) {
                    out[n++] = (InputKeyEvent){.key = named[k].key};
                    p = end + 1;
                    found = true;
                    break;
                }
            }
            if (found)
                continue;
        }
        out[n++] = (InputKeyEvent){.codepoint = text_utf8_next(&p)};
    }
    return n;
}

// Each case of the file CETRA_VIM_CASES names -- "<text in hex> <line> <column> <keys in hex>",
// the keys in vim's notation -- run on a fresh text, editor and vim mode, the cursor put where
// vim's cursor() would put it. Prints "vim <case> <line> <column> <the text in hex>", as vim's
// line('.') and col('.') count.
static int probe_vim_run(UISystem* ui) {
    const char* path = getenv("CETRA_VIM_CASES");
    long size = 0;
    char* all = path ? read_entire_file(path, &size) : NULL;
    if (!all) {
        fprintf(stderr, "vim-run: CETRA_VIM_CASES names no file to read\n");
        return 1;
    }
    Engine* engine = ui_system_engine(ui);
    InputKeyEvent* events = malloc(4096 * sizeof(InputKeyEvent));
    int index = 0;
    for (char* line = all; events && line < all + size;) {
        char* end = memchr(line, '\n', (size_t)(all + size - line));
        if (!end)
            end = all + size;
        *end = '\0';
        char text_hex[1 << 16], keys_hex[1 << 14];
        size_t at_line = 0, at_col = 0;
        if (sscanf(line, "%65535s %zu %zu %16383s", text_hex, &at_line, &at_col, keys_hex) == 4) {
            size_t text_len = 0, keys_len = 0;
            char* text = unhex(text_hex, strlen(text_hex), &text_len);
            char* keys = unhex(keys_hex, strlen(keys_hex), &keys_len);
            TextDoc* doc = text ? create_text_doc(text, text_len) : NULL;
            UIEditorVim* vim = create_ui_editor_vim();
            if (doc && vim && keys) {
                UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
                ui_editor_set_mode(ed, ui_editor_vim_mode(vim));
                if (engine)
                    engine_clipboard_set(engine, "");
                editor_pass(ui);
                const size_t start = text_doc_line_start(doc, at_line - 1) + at_col - 1;
                ui_editor_set_selections(ed, &(TextSelection){start, start}, 1);
                // A key at a time, as typed.
                const size_t n = vim_events(keys, events, 4096);
                for (size_t i = 0; i < n; i++)
                    send_keys(ui, &events[i], 1);
                const TextSelection* s = NULL;
                size_t primary = 0;
                ui_editor_selections(ed, &s, &primary);
                const size_t head = s ? s[primary].head : 0;
                const size_t l = text_doc_line_of(doc, head);
                size_t out_len = 0;
                char* out = text_doc_text(doc, &out_len);
                printf("vim %d %zu %zu ", index, l + 1, head - text_doc_line_start(doc, l) + 1);
                for (size_t i = 0; out && i < out_len; i++)
                    printf("%02x", (unsigned char)out[i]);
                printf("\n");
                free(out);
                ui_editor_set_mode(ed, NULL);
                ui_pop(ui);
            }
            free_text_doc(doc);
            free_ui_editor_vim(vim);
            free(text);
            free(keys);
        }
        index++;
        line = end + 1;
    }
    free(events);
    free(all);
    return 0;
}

// An editor in a fresh vim mode on `text`, laid out, the caret at `at`.
static UIElement* vim_fixture(UISystem* ui, TextDoc* doc, UIEditorVim* vim, size_t at) {
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    ui_editor_set_mode(ed, ui_editor_vim_mode(vim));
    editor_pass(ui);
    ui_editor_set_selections(ed, &(TextSelection){at, at}, 1);
    return ed;
}

// Keys in vim's notation, a pass each, as typed.
static void vim_keys(UISystem* ui, const char* keys) {
    InputKeyEvent events[256];
    const size_t n = vim_events(keys, events, 256);
    for (size_t i = 0; i < n; i++)
        send_keys(ui, &events[i], 1);
}

#define VIM_TEXT "alpha beta\n  gamma delta\nlast line\n"

// The state a host shows: its names, the keys of a command half typed, the caret's shape, and a
// click past a line's end leaving the cursor on its last character.
static int probe_vim_state(UISystem* ui) {
    TextDoc* doc = create_text_doc(VIM_TEXT, strlen(VIM_TEXT));
    UIEditorVim* vim = create_ui_editor_vim();
    UIElement* ed = vim_fixture(ui, doc, vim, 0);
    say_flag("vim-state", "normal", "named",
             ui_editor_vim_state(vim) == UI_VIM_NORMAL &&
                 !strcmp(ui_editor_mode(ed)->name, "NORMAL"));
    float x0 = 0.0f, x1 = 0.0f, y = 0.0f;
    ui_editor_point_of(ed, 0, &x0, &y);
    ui_editor_point_of(ed, 1, &x1, &y);
    const float cell = x1 - x0;
    say_flag("vim-state", "normal", "block", fabsf(caret_drawn(ui, ed, 0) - cell) < 0.01f);
    vim_keys(ui, "2d");
    say_flag("vim-state", "pending", "shown", !strcmp(ui_editor_vim_pending(vim), "2d"));
    vim_keys(ui, "<Esc>");
    say_flag("vim-state", "pending", "dropped",
             !strcmp(ui_editor_vim_pending(vim), "") && doc_is_str(doc, VIM_TEXT));
    vim_keys(ui, "i");
    say_flag("vim-state", "insert", "named",
             ui_editor_vim_state(vim) == UI_VIM_INSERT &&
                 !strcmp(ui_editor_mode(ed)->name, "INSERT"));
    editor_pass(ui);
    say_flag("vim-state", "insert", "bar", caret_drawn(ui, ed, 0) < cell * 0.5f);
    vim_keys(ui, "Z<Esc>");
    say_flag("vim-state", "escape", "normal",
             ui_editor_vim_state(vim) == UI_VIM_NORMAL && one_selection(ed, 0, 0) &&
                 doc_is_str(doc, "Z" VIM_TEXT));
    // A click well past the first line's end: on its last character, not past it.
    float ex = 0.0f, ey = 0.0f;
    ui_editor_point_of(ed, 3, &ex, &ey);
    press_drag(ui, ex + 400.0f, ey + 2.0f, ex + 400.0f, ey + 2.0f, 0);
    say_flag("vim-state", "click", "held", one_selection(ed, 10, 10));
    // The visual modes, each named and drawn its own way, and Esc back to normal mode.
    static const struct {
        const char* keys;
        UIEditorVimState state;
        const char* name;
        UIEditorSelectionShape shape;
    } VISUAL[] = {{"v", UI_VIM_VISUAL, "VISUAL", UI_SELECTION_INCLUSIVE},
                  {"V", UI_VIM_VISUAL_LINE, "V-LINE", UI_SELECTION_LINES},
                  {"<C-v>", UI_VIM_VISUAL_BLOCK, "V-BLOCK", UI_SELECTION_INCLUSIVE}};
    bool visual = true;
    for (size_t i = 0; i < sizeof(VISUAL) / sizeof(VISUAL[0]); i++) {
        vim_keys(ui, VISUAL[i].keys);
        visual = visual && ui_editor_vim_state(vim) == VISUAL[i].state &&
                 !strcmp(ui_editor_mode(ed)->name, VISUAL[i].name) &&
                 ui_editor_mode(ed)->selection == VISUAL[i].shape;
    }
    vim_keys(ui, "<Esc>");
    say_flag("vim-state", "visual", "named",
             visual && ui_editor_vim_state(vim) == UI_VIM_NORMAL && one_selection(ed, 10, 10));
    // A drag from the first character to the fifth: visual mode over the five, its head on the
    // last it reached.
    float x4 = 0.0f;
    ui_editor_point_of(ed, 4, &x4, &y);
    press_drag(ui, x0 + 1.0f, y + 2.0f, x4 + cell * 0.6f, y + 2.0f, 0);
    say_flag("vim-state", "drag", "visual",
             ui_editor_vim_state(vim) == UI_VIM_VISUAL && one_selection(ed, 0, 4));
    ui_editor_set_mode(ed, NULL);
    free_text_doc(doc);
    free_ui_editor_vim(vim);
    return 0;
}

#define VIM_CARETS_TEXT "alpha beta\nalpha beta\nalpha beta\n"

// The carets at the first character of each line, as a host would put them.
static void three_carets(UIElement* ed) {
    const TextSelection three[] = {{0, 0}, {11, 11}, {22, 22}};
    ui_editor_set_selections(ed, three, 3);
}

// Several carets, which vim itself has none of: a block's I and A type on every line and leave one
// cursor at its top left, which undo puts back; and each of several carets takes a count, an
// operator over a motion, and a visual area of its own.
static int probe_vim_carets(UISystem* ui) {
    TextDoc* doc = create_text_doc(VIM_CARETS_TEXT, strlen(VIM_CARETS_TEXT));
    UIEditorVim* vim = create_ui_editor_vim();
    UIElement* ed = vim_fixture(ui, doc, vim, 6);
    vim_keys(ui, "<C-v>jjI");
    say_flag("vim-carets", "block", "insert",
             ui_editor_vim_state(vim) == UI_VIM_INSERT &&
                 ui_editor_selections(ed, NULL, NULL) == 3);
    vim_keys(ui, "Z<Esc>");
    say_flag("vim-carets", "block", "typed",
             doc_is_str(doc, "alpha Zbeta\nalpha Zbeta\nalpha Zbeta\n") && one_selection(ed, 6, 6));
    vim_keys(ui, "u");
    say_flag("vim-carets", "block", "undone",
             doc_is_str(doc, VIM_CARETS_TEXT) && one_selection(ed, 6, 6));
    vim_keys(ui, "<C-v>jjeAZ<Esc>");
    say_flag("vim-carets", "block", "append",
             doc_is_str(doc, "alpha betaZ\nalpha betaZ\nalpha betaZ\n") && one_selection(ed, 6, 6));
    vim_keys(ui, "u");
    three_carets(ed);
    vim_keys(ui, "3x");
    say_flag("vim-carets", "count", "each",
             doc_is_str(doc, "ha beta\nha beta\nha beta\n") &&
                 ui_editor_selections(ed, NULL, NULL) == 3);
    vim_keys(ui, "u");
    three_carets(ed);
    vim_keys(ui, "dw");
    say_flag("vim-carets", "operator", "each", doc_is_str(doc, "beta\nbeta\nbeta\n"));
    vim_keys(ui, "u");
    three_carets(ed);
    vim_keys(ui, "vlld");
    say_flag("vim-carets", "visual", "each",
             doc_is_str(doc, "ha beta\nha beta\nha beta\n") &&
                 ui_editor_vim_state(vim) == UI_VIM_NORMAL);
    ui_editor_set_mode(ed, NULL);
    free_text_doc(doc);
    free_ui_editor_vim(vim);
    return 0;
}

static uint64_t g_vim_steps[64];
static int g_vim_step_count;

static void note_vim_step(TextDoc* doc, const TextDocAction* action, void* user) {
    (void)doc;
    (void)user;
    if (g_vim_step_count < 64)
        g_vim_steps[g_vim_step_count++] = action->step;
}

// Whether every action told since the count was cleared shares one step.
static bool one_step(void) {
    for (int i = 1; i < g_vim_step_count; i++)
        if (g_vim_steps[i] != g_vim_steps[0])
            return false;
    return g_vim_step_count > 0;
}

// Undo by vim's units: an insert from i to Esc is one step, Enters and Backspaces in it included;
// `cw` and what it typed is one; each is told to a host as one step too, with keep_undo off.
static int probe_vim_undo(UISystem* ui) {
    for (int keep = 1; keep >= 0; keep--) {
        const char* which = keep ? "kept" : "off";
        TextDoc* doc = create_text_doc(VIM_TEXT, strlen(VIM_TEXT));
        doc->keep_undo = keep;
        text_doc_set_on_action(doc, note_vim_step, NULL);
        UIEditorVim* vim = create_ui_editor_vim();
        UIElement* ed = vim_fixture(ui, doc, vim, 0);
        g_vim_step_count = 0;
        vim_keys(ui, "ione<CR>two<BS>o three<Esc>");
        say_flag("vim-undo", which, "insert", one_step() && g_vim_step_count > 3);
        if (keep) {
            vim_keys(ui, "u");
            say_flag("vim-undo", which, "insert_undone", doc_is_str(doc, VIM_TEXT));
        }
        g_vim_step_count = 0;
        vim_keys(ui, "jwcwnew<Esc>");
        say_flag("vim-undo", which, "change", one_step() && g_vim_step_count >= 2);
        if (keep) {
            vim_keys(ui, "u");
            say_flag("vim-undo", which, "change_undone", doc_is_str(doc, VIM_TEXT));
            vim_keys(ui, "<C-r>");
            say_flag("vim-undo", which, "change_redone",
                     doc_is_str(doc, "alpha beta\n  new delta\nlast line\n"));
        }
        ui_editor_set_mode(ed, NULL);
        free_text_doc(doc);
        free_ui_editor_vim(vim);
    }
    return 0;
}

// A host over the vim mode: its Space acts in normal mode only and takes the space with it, in
// insert mode handing the key back so the space is typed; its own undo takes u and Ctrl+R.
static int g_vim_host_help, g_vim_host_undos, g_vim_host_redos;
static char g_vim_host_asked;
static UIEditorVim* g_vim_host;

static void vim_host_prompt(UIElement* el, char kind, void* user) {
    (void)el;
    (void)user;
    g_vim_host_asked = kind;
}

static bool vim_host_help(UIElement* el, void* user) {
    (void)el;
    (void)user;
    if (ui_editor_vim_state(g_vim_host) != UI_VIM_NORMAL)
        return false;
    g_vim_host_help++;
    return true;
}

static bool vim_host_undo(UIElement* el, void* user) {
    (void)el;
    (void)user;
    g_vim_host_undos++;
    return true;
}

static bool vim_host_redo(UIElement* el, void* user) {
    (void)el;
    (void)user;
    g_vim_host_redos++;
    return true;
}

static const UIEditorCommand VIM_HOST_HELP = {"help", "The host's help", vim_host_help, NULL};
static const UIEditorBinding VIM_HOST_KEYS[] = {{GLFW_KEY_SPACE, 0, &VIM_HOST_HELP}};

static int probe_vim_host(UISystem* ui) {
    TextDoc* doc = create_text_doc(VIM_TEXT, strlen(VIM_TEXT));
    doc->keep_undo = false;
    g_vim_host = create_ui_editor_vim();
    ui_editor_vim_set_undo(g_vim_host, vim_host_undo, vim_host_redo, NULL);
    UIElement* ed = vim_fixture(ui, doc, g_vim_host, 0);
    const UIEditorMode host = {.name = "pcb",
                               .parent = ui_editor_vim_mode(g_vim_host),
                               .bindings = VIM_HOST_KEYS,
                               .binding_count = 1,
                               .layer = true};
    ui_editor_set_mode(ed, &host);
    // A press of Space: its key, then its character.
    const InputKeyEvent space[] = {{.key = GLFW_KEY_SPACE}, {.codepoint = ' '}};
    send_keys(ui, space, 2);
    say_flag("vim-host", "space", "normal", g_vim_host_help == 1 && one_selection(ed, 0, 0));
    vim_keys(ui, "i");
    send_keys(ui, space, 2);
    vim_keys(ui, "<Esc>");
    say_flag("vim-host", "space", "insert", g_vim_host_help == 1 && doc_is_str(doc, " " VIM_TEXT));
    say_flag("vim-host", "layer", "look",
             !strcmp(ui_editor_mode(ed)->name, "pcb") &&
                 ui_editor_vim_state(g_vim_host) == UI_VIM_NORMAL);
    vim_keys(ui, "u<C-r>");
    say_flag("vim-host", "undo", "host",
             g_vim_host_undos == 1 && g_vim_host_redos == 1 && doc_is_str(doc, " " VIM_TEXT));
    // The host's line: / and : ask for it, and what it answers is the search or the command.
    ui_editor_vim_set_prompt(g_vim_host, vim_host_prompt, NULL);
    vim_keys(ui, "gg/");
    const char asked_search = g_vim_host_asked;
    const bool found = ui_editor_vim_search(g_vim_host, ed, "gamma", false);
    say_flag("vim-host", "prompt", "search",
             asked_search == '/' && found && one_selection(ed, 14, 14));
    vim_keys(ui, ":");
    const char asked_command = g_vim_host_asked;
    const bool known = ui_editor_vim_command(g_vim_host, ed, "3");
    say_flag("vim-host", "prompt", "command",
             asked_command == ':' && known && one_selection(ed, 26, 26) &&
                 !ui_editor_vim_command(g_vim_host, ed, "w"));
    // d/ answered by the host, and . making the change again with the same search, asking nothing.
    vim_keys(ui, "gglld/");
    ui_editor_vim_search(g_vim_host, ed, "a", false);
    const bool deleted = doc_is_str(doc, " ala beta\n  gamma delta\nlast line\n");
    g_vim_host_asked = 0;
    vim_keys(ui, ".");
    say_flag("vim-host", "prompt", "repeat",
             deleted && g_vim_host_asked == 0 &&
                 doc_is_str(doc, " ala\n  gamma delta\nlast line\n"));
    ui_editor_set_mode(ed, NULL);
    free_text_doc(doc);
    free_ui_editor_vim(g_vim_host);
    return 0;
}

// ---- The host's edits: carets, selections and the view through them

#define HOST_TEXT "abc def ghi\nline two\n"

static int probe_editor_host_edit(UISystem* ui) {
    TextDoc* doc = create_text_doc(HOST_TEXT, strlen(HOST_TEXT));
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    editor_pass(ui);
    // Before a selection: it moves by the change's length.
    reset(doc, ed, HOST_TEXT, 4, 7);
    text_doc_replace(doc, 0, 3, "xxxxx", 5);
    say_flag("editor-host-edit", "before", "moved", one_selection(ed, 6, 9));
    // After it: nothing moves.
    reset(doc, ed, HOST_TEXT, 4, 7);
    text_doc_replace(doc, 8, 11, "", 0);
    say_flag("editor-host-edit", "after", "kept", one_selection(ed, 4, 7));
    // Inside it: its far end moves with the change, its near end stays.
    reset(doc, ed, HOST_TEXT, 4, 7);
    text_doc_replace(doc, 5, 6, "EE", 2);
    say_flag("editor-host-edit", "inside", "grown", one_selection(ed, 4, 8));
    // Over a caret: it goes to the end of the new bytes.
    reset(doc, ed, HOST_TEXT, 5, 5);
    text_doc_replace(doc, 4, 7, "Q", 1);
    say_flag("editor-host-edit", "over", "end", one_selection(ed, 5, 5));
    // Put in at a caret by the host: the caret stays before it, the host's text not typed.
    reset(doc, ed, HOST_TEXT, 4, 4);
    text_doc_replace(doc, 4, 4, "zz", 2);
    say_flag("editor-host-edit", "at", "before", one_selection(ed, 4, 4));
    // A whole new text that changes only past the caret: the caret stays.
    reset(doc, ed, HOST_TEXT, 2, 2);
    text_doc_set_text(doc, "abc def ghi\nline 2\n", 19);
    say_flag("editor-host-edit", "reload", "kept", one_selection(ed, 2, 2));
    // Two editors of one text: an edit in one moves the other's caret.
    UIScreen* s = ui_top(ui);
    UIElement* twin = ui_editor(ui_screen_root(s), doc);
    ui_set_size(twin, UI_FIXED, 300.0f, UI_FIXED, 200.0f);
    editor_pass(ui);
    ui_editor_set_selections(twin, &(TextSelection){12, 12}, 1);
    ui_editor_set_selections(ed, &(TextSelection){0, 0}, 1);
    type(ui, "ab");
    say_flag("editor-host-edit", "twin", "moved", one_selection(twin, 14, 14));
    // The text freed while two editors still show it: they let go of it, and a pass reaches
    // nothing freed.
    free_text_doc(doc);
    type(ui, "c");
    say_flag("editor-host-edit", "freed", "inert",
             ui_editor_selections(ed, NULL, NULL) == 0 &&
                 ui_editor_selections(twin, NULL, NULL) == 0);
    return 0;
}

int run_ui_editor_probe(Game* game, UISystem* ui, Font* font, const char* which) {
    (void)game;
    (void)ui;
    (void)font;
    if (!strcmp(which, "doc-roundtrip"))
        return probe_doc_roundtrip();
    if (!strcmp(which, "doc-random"))
        return probe_doc_random();
    if (!strcmp(which, "doc-map"))
        return probe_doc_map();
    if (!strcmp(which, "doc-set-text"))
        return probe_doc_set_text();
    if (!strcmp(which, "doc-undo"))
        return probe_doc_undo();
    if (!strcmp(which, "doc-highlight"))
        return probe_doc_highlight();
    if (!strcmp(which, "glyph-cache"))
        return probe_glyph_cache();
    if (!strcmp(which, "editor-wrap"))
        return probe_editor_wrap();
    if (!strcmp(which, "editor-hit"))
        return probe_editor_hit(ui);
    if (!strcmp(which, "editor-anchor"))
        return probe_editor_anchor(ui);
    if (!strcmp(which, "editor-keys"))
        return probe_editor_keys(ui);
    if (!strcmp(which, "editor-carets"))
        return probe_editor_carets(ui);
    if (!strcmp(which, "editor-mode"))
        return probe_editor_mode(ui);
    if (!strcmp(which, "editor-host-undo"))
        return probe_editor_host_undo(ui);
    if (!strcmp(which, "editor-host-edit"))
        return probe_editor_host_edit(ui);
    if (!strcmp(which, "editor-layer"))
        return probe_editor_layer(ui);
    if (!strcmp(which, "editor-decline"))
        return probe_editor_decline(ui);
    if (!strcmp(which, "editor-paired-char"))
        return probe_editor_paired_char(ui);
    if (!strcmp(which, "editor-shapes"))
        return probe_editor_shapes(ui);
    if (!strcmp(which, "editor-steps"))
        return probe_editor_steps(ui);
    if (!strcmp(which, "vim-run"))
        return probe_vim_run(ui);
    if (!strcmp(which, "vim-state"))
        return probe_vim_state(ui);
    if (!strcmp(which, "vim-undo"))
        return probe_vim_undo(ui);
    if (!strcmp(which, "vim-host"))
        return probe_vim_host(ui);
    if (!strcmp(which, "vim-carets"))
        return probe_vim_carets(ui);
    return -1;
}
