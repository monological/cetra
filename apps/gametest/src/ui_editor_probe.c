/*
 * The --ui-probe cases of spec 14.4, the code editor: its text first, which needs no window, then
 * the editor that shows it. Each prints `ui <case> <label> <key> <numbers>` lines, as the cases of
 * ui_text_probe.c do.
 */

#include "ui_editor_probe.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cetra/glyph_cache.h"
#include "cetra/internal/ext/stb_truetype.h" // the rasteriser the glyph cache's bitmaps are held to
#include "cetra/internal/text_doc_edit.h"    // the editor's half of the text, which the arms drive
#include "cetra/text.h"
#include "cetra/text_doc.h"

// ---- A stand-in for an editor: carets the text moves, as an editor's would be.

typedef struct FakeView {
    TextSelection sel[8];
    size_t count;
} FakeView;

static void fake_map(void* view, size_t start, size_t removed, size_t inserted, bool actor) {
    FakeView* v = view;
    for (size_t i = 0; i < v->count; i++) {
        v->sel[i].anchor = text_doc_map_offset(v->sel[i].anchor, start, removed, inserted, actor);
        v->sel[i].head = text_doc_map_offset(v->sel[i].head, start, removed, inserted, actor);
    }
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
                                          .map = fake_map,
                                          .selections = fake_selections,
                                          .set_selections = fake_set});
}

// Types `text` at every caret of `v`, as one action.
static void fake_type(TextDoc* doc, FakeView* v, const char* text) {
    TextEdit edits[8];
    for (size_t i = 0; i < v->count; i++)
        edits[i] = (TextEdit){v->sel[i].head, v->sel[i].head, text, strlen(text)};
    text_doc_apply(doc, edits, v->count, v, true);
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
    return -1;
}
