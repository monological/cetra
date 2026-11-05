/*
 * The --ui-probe cases of spec 14.4, the code editor: its text first, which needs no window, then
 * the editor that shows it; then spec 14.6's, elements taken out and moved. Each prints
 * `ui <case> <label> <key> <numbers>` lines, as the cases of ui_text_probe.c do.
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
#include "cetra/internal/ui_hooks.h"         // what a system holds
#include "cetra/internal/ui_internal.h"      // ui_system_engine, for the clipboard; the bar's sizes
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

// ---- doc-read-only

static int g_refusals;

static void count_refusal(TextDoc* doc, const UIElement* editor, void* user) {
    (void)doc;
    (void)editor;
    (void)user;
    g_refusals++;
}

// A read-only text (spec 14.7): what is done through an editor refused and told once each, the
// host's own edits, undo and redo taken; writable again, nothing differs.
static int probe_doc_read_only(void) {
    TextDoc* doc = create_text_doc("ab", 2);
    FakeView v = {.sel = {{0, 0}, {2, 2}}, .count = 2};
    fake_attach(doc, &v);
    fake_type(doc, &v, "x");
    text_doc_set_on_action(doc, count_action, NULL);
    text_doc_set_on_refused(doc, count_refusal, NULL);
    g_actions = g_refusals = 0;
    doc->read_only = true;
    const uint64_t version = doc->version;
    // Typing at two carets is one action, and one refusal.
    fake_type(doc, &v, "y");
    printf("ui doc-read-only action refused %d\n",
           doc_is_str(doc, "xabx") && doc->version == version && g_refusals == 1 && g_actions == 0);
    printf("ui doc-read-only undo refused %d\n",
           !text_doc_undo_in(doc, &v) && doc_is_str(doc, "xabx") && g_refusals == 2);
    printf("ui doc-read-only redo refused %d\n",
           !text_doc_redo_in(doc, &v) && doc_is_str(doc, "xabx") && g_refusals == 3);
    // The host's edits land, and are told to neither hook.
    const bool replaced = text_doc_replace(doc, 0, 0, "h", 1) && doc_is_str(doc, "hxabx");
    const bool set = text_doc_set_text(doc, "new", 3) && doc_is_str(doc, "new");
    printf("ui doc-read-only host taken %d\n",
           replaced && set && g_refusals == 3 && g_actions == 0);
    // The host's undo and redo take the host's steps.
    printf("ui doc-read-only host undo %d\n", text_doc_undo(doc) && doc_is_str(doc, "hxabx") &&
                                                  text_doc_redo(doc) && doc_is_str(doc, "new") &&
                                                  g_refusals == 3);
    // Writable again: typed, told, nothing refused.
    doc->read_only = false;
    g_actions = 0;
    v = (FakeView){.sel = {{3, 3}}, .count = 1};
    fake_type(doc, &v, "z");
    printf("ui doc-read-only writable same %d\n",
           doc_is_str(doc, "newz") && g_actions == 1 && g_refusals == 3);
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

// The editor's face its own at 13 points.
static void editor_theme(UISystem* ui) {
    if (!g_face)
        g_face = create_glyph_cache(EDITOR_FONT, 13.0f);
    UITheme theme = *ui_theme(ui);
    theme.editor_face = g_face;
    theme.editor_size = 13.0f;
    ui_set_theme(ui, &theme);
}

// An editor `w` by `h` in `parent`.
static UIElement* fixed_editor(UIElement* parent, TextDoc* doc, float w, float h) {
    UIElement* ed = ui_editor(parent, doc);
    ui_set_size(ed, UI_FIXED, w, UI_FIXED, h);
    return ed;
}

// An editor of a fixed size alone on a screen, its face the editor's own at 13 points.
static UIElement* editor_fixture(UISystem* ui, TextDoc* doc, float w, float h) {
    editor_theme(ui);
    UIScreen* s = ui_screen(ui, "editor");
    ui_screen_set_modal(s, false);
    UIElement* ed = fixed_editor(ui_screen_root(s), doc, w, h);
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
// The wheel turned over `el` by `rows` of its rows, down for more than 0: a notch is the editor's
// WHEEL_ROWS, 3.
static void wheel_rows(UISystem* ui, const UIElement* el, float rows) {
    ui_update(ui,
              &(UIInput){.pointer_x = el->rect.x + 50.0f,
                         .pointer_y = el->rect.y + 50.0f,
                         .wheel_y = -rows / 3.0f},
              1280.0f, 720.0f);
}

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
    float rows = 0.0f;
    const size_t line = ui_editor_top_line(ed, &rows);
    size_t want_len = 0;
    const char* top = text_doc_line(doc, line, &want_len);
    char want[128];
    memcpy(want, top, want_len);
    // Three lines put in above it.
    text_doc_replace(doc, text_doc_line_start(doc, 10), text_doc_line_start(doc, 10),
                     "new\nnew\nnew\n", 12);
    editor_pass(ui);
    float r2 = 0.0f;
    const size_t l2 = ui_editor_top_line(ed, &r2);
    size_t got_len = 0;
    const char* got = text_doc_line(doc, l2, &got_len);
    say_flag("editor-anchor", "edit", "kept",
             l2 == line + 3 && r2 == rows && got_len == want_len && !memcmp(got, want, want_len));
    // Every line above measured: clicking far above walks the rows to the top, wrapping each.
    ui_editor_offset_at(ed, 0.0f, -100000.0f);
    editor_pass(ui);
    float r3 = 0.0f;
    const size_t l3 = ui_editor_top_line(ed, &r3);
    const int c3 = ui_editor_columns(ed);
    say_flag("editor-anchor", "measured", "kept", l3 == l2 && r3 == r2);
    // A new width wraps every line again, and keeps the line.
    ui_set_size(ed, UI_FIXED, 320.0f, UI_FIXED, EDITOR_H);
    editor_pass(ui);
    float r4 = 0.0f;
    const size_t l4 = ui_editor_top_line(ed, &r4);
    say_flag("editor-anchor", "resize", "kept", l4 == l2 && ui_editor_columns(ed) != c3);
    // And the wheel moves it a row at a time.
    wheel_rows(ui, ed, 3.0f);
    float r5 = 0.0f;
    const size_t l5 = ui_editor_top_line(ed, &r5);
    say_flag("editor-anchor", "wheel", "moved", l5 > l4 || (l5 == l4 && r5 > r4));
    return 0;
}

// Whether the rows starting at `in_a` stand as far down `a` as those at `in_b` down `b`.
static bool rows_level(UIElement* a, UIElement* b, const size_t* in_a, const size_t* in_b,
                       size_t n) {
    for (size_t i = 0; i < n; i++) {
        float xa = 0.0f, ya = 0.0f, xb = 0.0f, yb = 0.0f;
        if (!ui_editor_point_of(a, in_a[i], &xa, &ya) ||
            !ui_editor_point_of(b, in_b[i], &xb, &yb) ||
            fabsf((ya - a->rect.y) - (yb - b->rect.y)) > 0.01f)
            return false;
    }
    return true;
}

// The exact scroll: the line at the top and the rows into it, read from one editor and put on
// another to line their rows up, put before a layout and taken at it, and held past the last line
// as a wheel is.
static int probe_editor_scroll_line(UISystem* ui) {
    // 120 lines, every third long enough to wrap in 300 points, the last a short one.
    static char text[120 * 128];
    size_t len = 0;
    for (int i = 0; i < 120; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "line %03d%s\n", i,
                                i % 3 ? ""
                                      : " of a text whose lines are long enough to wrap in a "
                                        "narrow editor, twice over");
    TextDoc* doc = create_text_doc(text, len - 1);
    UIElement* ed = editor_fixture(ui, doc, 300.0f, 150.0f);
    UIElement* twin = ui_editor(ui_screen_root(ui_top(ui)), doc);
    ui_set_size(twin, UI_FIXED, 300.0f, UI_FIXED, 150.0f);
    editor_pass(ui);

    wheel_rows(ui, ed, 32.5f);
    editor_pass(ui);
    float rows = 0.0f;
    const size_t line = ui_editor_top_line(ed, &rows);
    // The top row's start is on that line, at the row its wrap puts there.
    size_t line_len = 0, starts[8];
    int hang = 0;
    const char* s = text_doc_line(doc, line, &line_len);
    const size_t wrapped = ui_editor_wrap(s, line_len, ui_editor_columns(ed), 4, starts, 8, &hang);
    const size_t row = (size_t)floorf(rows);
    say_flag("editor-scroll-line", "read", "rows",
             row < wrapped && text_doc_line_start(doc, line) + starts[row] == ui_editor_top(ed) &&
                 fabsf(rows - (float)row - 0.5f) < 0.01f);
    const size_t offsets[3] = {ui_editor_top(ed), text_doc_line_start(doc, line + 1),
                               text_doc_line_start(doc, line + 2)};

    ui_editor_scroll_line(twin, line, rows);
    editor_pass(ui);
    float twin_rows = 0.0f;
    const size_t twin_line = ui_editor_top_line(twin, &twin_rows);
    say_flag("editor-scroll-line", "set", "level",
             twin_line == line && fabsf(twin_rows - rows) < 1e-4f &&
                 rows_level(ed, twin, offsets, offsets, 3));

    // A third editor, put before it was ever laid out.
    UIElement* late = ui_editor(ui_screen_root(ui_top(ui)), doc);
    ui_set_size(late, UI_FIXED, 300.0f, UI_FIXED, 150.0f);
    ui_editor_scroll_line(late, line, rows);
    editor_pass(ui);
    say_flag("editor-scroll-line", "pending", "level", rows_level(ed, late, offsets, offsets, 3));

    // Past the last line, against a wheel run to the end.
    ui_editor_scroll_line(twin, doc->line_count + 10, 5.0f);
    wheel_rows(ui, late, 100000.0f);
    editor_pass(ui);
    float held_rows = 0.0f, wheel_rows_at = 0.0f;
    const size_t held = ui_editor_top_line(twin, &held_rows);
    const size_t wheeled = ui_editor_top_line(late, &wheel_rows_at);
    say_flag("editor-scroll-line", "held", "wheel",
             held == doc->line_count - 1 && held == wheeled && held_rows == wheel_rows_at);
    free_text_doc(doc);
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
// What the pointer does in a pass: moves to the point, is pressed there, is held there, or is let
// go there.
typedef enum { POINTER_MOVE, POINTER_PRESS, POINTER_HOLD, POINTER_RELEASE } PointerAct;

// One input pass with the pointer at (x, y), a second after the last.
static void pointer_pass(UISystem* ui, float x, float y, PointerAct act, int mods) {
    ui_update(ui,
              &(UIInput){.pointer_x = x,
                         .pointer_y = y,
                         .pointer_pressed = act == POINTER_PRESS,
                         .pointer_down = act == POINTER_PRESS || act == POINTER_HOLD,
                         .pointer_released = act == POINTER_RELEASE,
                         .pointer_mods = mods,
                         .time = (g_key_time += 1.0)},
              1280.0f, 720.0f);
}

// A press at (x0, y0) held to (x1, y1) over a pass, then let go there, a second apart.
static void press_drag(UISystem* ui, float x0, float y0, float x1, float y1, int mods) {
    pointer_pass(ui, x0, y0, POINTER_PRESS, mods);
    pointer_pass(ui, x1, y1, POINTER_HOLD, 0);
    pointer_pass(ui, x1, y1, POINTER_RELEASE, 0);
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
    float top_rows = 0.0f;
    const size_t top = ui_editor_top_line(ed, &top_rows);
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

static bool same_colour(const float* a, const float* b) {
    return fabsf(a[0] - b[0]) < 1e-4f && fabsf(a[1] - b[1]) < 1e-4f && fabsf(a[2] - b[2]) < 1e-4f &&
           fabsf(a[3] - b[3]) < 1e-4f;
}

// The flat quads drawn on the row at y in colour c: how many cells wide they come to together.
static float band_cells(const UISystem* ui, float y, const float* c, float cell) {
    const UIDrawList* dl = ui_draw_list(ui);
    float w = 0.0f;
    for (size_t i = 0; i < ui_draw_list_quad_count(dl); i++) {
        UIRect r;
        vec4 colour;
        int kind = 0;
        ui_draw_list_quad(dl, i, &r, colour, &kind);
        if (kind == 0 && fabsf(r.y - y) < 0.01f && same_colour(colour, c))
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

// A command moving the focus to another element, as a split bound to `|` moves it to the pane it
// made.
static struct {
    UIScreen* screen;
    UIElement* to;
} g_focus_move;

static bool focus_move(UIElement* el, void* user) {
    (void)el;
    (void)user;
    ui_set_focus(g_focus_move.screen, g_focus_move.to);
    return true;
}

static const UIEditorCommand FOCUS_MOVE = {"focus-move", "Moves the focus", focus_move, NULL};
static const UIEditorBinding FOCUS_KEYS[] = {{GLFW_KEY_S, 0, &FOCUS_MOVE}};

// A press is a key and then its character: a binding taking the key takes the character too,
// wherever it put the focus, and a character arriving alone is typed. A text field types a letter
// from its character, so its key is no key it uses.
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
    // A used key with no character after it in its pass -- a script's -- takes none from the next.
    key(ui, GLFW_KEY_SPACE, 0);
    type(ui, "x");
    say_flag("editor-paired-char", "stale", "typed", g_seam_runs[1] == 2 && doc_is_str(doc, " ax"));
    // A key whose command moves the focus to another editor: its character types into neither.
    UIScreen* s = ui_top(ui);
    TextDoc* other = create_text_doc("", 0);
    UIElement* there = ui_editor(ui_screen_root(s), other);
    ui_set_size(there, UI_FIXED, 200.0f, UI_FIXED, 80.0f);
    g_focus_move.screen = s;
    g_focus_move.to = there;
    const UIEditorMode mover = {.name = "mover",
                                .parent = ui_editor_standard_mode(),
                                .bindings = FOCUS_KEYS,
                                .binding_count = 1,
                                .layer = true};
    ui_editor_set_mode(ed, &mover);
    ui_set_focus(s, ed);
    editor_pass(ui);
    const InputKeyEvent moving[] = {{.key = GLFW_KEY_S}, {.codepoint = 's'}};
    send_keys(ui, moving, 2);
    say_flag("editor-paired-char", "moved", "dropped",
             ui_focused(s) == there && doc_is_str(other, "") && doc_is_str(doc, " ax"));
    // A field's letter, key and character: typed.
    UIElement* field = ui_text_field(ui_screen_root(s), 0, NULL, NULL);
    ui_set_focus(s, field);
    editor_pass(ui);
    send_keys(ui, letter, 2);
    say_flag("editor-paired-char", "field", "typed", !strcmp(ui_field_text(field), "a"));
    ui_editor_set_mode(ed, NULL);
    free_text_doc(other);
    free_text_doc(doc);
    return 0;
}

// ---- Any element takes keys (spec 14.10)

static InputKeyEvent g_seen[32];
static int g_seen_count;
static UIScreen* g_keys_screen; // where the pane's hook moves the focus when it takes K
static UIElement* g_keys_field;

// A pane's hook: everything it is handed is kept, and it takes the characters h and + and the key
// K, which moves the focus to the field, as a key opening a prompt would.
static bool pane_keys(UIElement* el, const InputKeyEvent* k, void* user) {
    (void)el;
    (void)user;
    if (g_seen_count < 32)
        g_seen[g_seen_count++] = *k;
    if (k->key == GLFW_KEY_K)
        ui_set_focus(g_keys_screen, g_keys_field);
    return k->codepoint == 'h' || k->codepoint == '+' || k->key == GLFW_KEY_K;
}

static bool take_slash(UIElement* el, const InputKeyEvent* k, void* user) {
    (void)el;
    (void)user;
    return k->codepoint == '/';
}

// Whether the pane's hook was handed exactly `want`, in order.
static bool seen_are(const InputKeyEvent* want, int n) {
    bool same = g_seen_count == n;
    for (int i = 0; same && i < n; i++)
        same = g_seen[i].key == want[i].key && g_seen[i].mods == want[i].mods &&
               g_seen[i].codepoint == want[i].codepoint;
    if (!same)
        fprintf(stderr, "  the hook was handed %d events, want %d\n", g_seen_count, n);
    return same;
}

static void centre_of(const UIElement* el, float* x, float* y) {
    *x = el->rect.x + el->rect.w * 0.5f;
    *y = el->rect.y + el->rect.h * 0.5f;
}

// A press and a release at the middle of `el`.
static void click_on(UISystem* ui, const UIElement* el) {
    float x, y;
    centre_of(el, &x, &y);
    press_drag(ui, x, y, x, y, 0);
}

// The pointer passing over `el`, moving within it.
static void pass_over(UISystem* ui, const UIElement* el) {
    float x, y;
    centre_of(el, &x, &y);
    pointer_pass(ui, x, y, POINTER_MOVE, 0);
    pointer_pass(ui, x + 1.0f, y, POINTER_MOVE, 0);
}

// A panel with a key hook takes the keys as a field does, and keeps the focus as the pointer
// passes; a field's hook is asked about each character first; a panel with none is as it was; the
// four controls and an editor refuse one.
static int probe_element_keys(UISystem* ui) {
    TextDoc* doc = create_text_doc("", 0);
    UIElement* ed = editor_fixture(ui, doc, 300.0f, 100.0f);
    UIScreen* s = ui_top(ui);
    UIElement* root = ui_screen_root(s);
    UIElement* pane = ui_panel(root);
    ui_set_size(pane, UI_FIXED, 300.0f, UI_FIXED, 120.0f);
    pane->focusable = true;
    ui_set_on_key(pane, pane_keys, NULL);
    UIElement* field = ui_text_field(root, 0, NULL, NULL);
    ui_set_on_key(field, take_slash, NULL);
    g_keys_screen = s;
    g_keys_field = field;
    UIElement* controls = ui_panel(root);
    controls->dir = UI_ROW;
    static bool on;
    static float level;
    static int pick;
    static const char* const PICKS[] = {"one", "two"};
    UIElement* const refusing[] = {
        ui_button(controls, "Button", NULL, NULL),
        ui_toggle(controls, "Toggle", &on, NULL, NULL),
        ui_slider(controls, "Slider", 0.0f, 1.0f, &level, NULL, NULL),
        ui_selector(controls, "Selector", PICKS, 2, &pick, NULL, NULL),
    };
    const UIElement* button = refusing[0];
    UIElement* plain = ui_panel(root);
    ui_set_size(plain, UI_FIXED, 300.0f, UI_FIXED, 40.0f);
    plain->focusable = true;
    UIElement* unfocusable = ui_panel(root);
    ui_set_size(unfocusable, UI_FIXED, 300.0f, UI_FIXED, 40.0f);
    ui_set_on_key(unfocusable, pane_keys, NULL);
    editor_pass(ui);

    // An editor's keys are its modes': the hook is refused, and the editor types as it did.
    ui_set_on_key(ed, pane_keys, NULL);
    type(ui, "e");
    say_flag("element-keys", "editor", "refused",
             !ed->on_key && doc_is_str(doc, "e") && g_seen_count == 0);

    // A control's own keys and navigation would stop while a hook held them: refused on each.
    bool none = true;
    for (size_t i = 0; i < sizeof(refusing) / sizeof(refusing[0]); i++) {
        ui_set_on_key(refusing[i], pane_keys, NULL);
        none = none && !refusing[i]->on_key;
    }
    say_flag("element-keys", "controls", "refused", none);

    // A press on the pane focuses it, and the keyboard is the UI's.
    click_on(ui, pane);
    say_flag("element-keys", "press", "focuses", ui_focused(s) == pane);
    say_flag("element-keys", "press", "wants", ui_wants_keyboard(ui));

    // Every key and character in order. The K key taken moves the focus to the field and takes its
    // k with it, so the field types nothing.
    const InputKeyEvent typed[] = {
        {.key = GLFW_KEY_H}, {.codepoint = 'h'},  {.key = GLFW_KEY_EQUAL, .mods = GLFW_MOD_SHIFT},
        {.codepoint = '+'},  {.key = GLFW_KEY_K}, {.codepoint = 'k'}};
    send_keys(ui, typed, 6);
    say_flag("element-keys", "keys", "order", seen_are(typed, 5));
    say_flag("element-keys", "taken", "dropped",
             ui_focused(s) == field && !ui_field_text(field)[0] && doc_is_str(doc, "e"));

    // The pointer passing over the field and the button leaves the focus on the pane; a press on
    // the field moves it there.
    click_on(ui, pane);
    pass_over(ui, field);
    pass_over(ui, button);
    say_flag("element-keys", "hover", "kept", ui_focused(s) == pane);
    click_on(ui, field);
    say_flag("element-keys", "press", "moves", ui_focused(s) == field);

    // The field's hook is asked about each character before the field types it: the / it takes is
    // kept out, the letters it declines are typed.
    type(ui, "a/b");
    say_flag("element-keys", "field", "slash", !strcmp(ui_field_text(field), "ab"));

    // Tab, which the pane's hook declines, moves the focus on; Esc lets go of it.
    click_on(ui, pane);
    key(ui, GLFW_KEY_TAB, 0);
    say_flag("element-keys", "tab", "moves", ui_focused(s) == field);
    click_on(ui, pane);
    key(ui, GLFW_KEY_ESCAPE, 0);
    say_flag("element-keys", "escape", "lets", ui_focused(s) == NULL && !ui_wants_keyboard(ui));

    // A focusable panel with no hook takes no keys, and a passing pointer takes its focus.
    click_on(ui, plain);
    say_flag("element-keys", "plain", "wants", ui_focused(s) == plain && !ui_wants_keyboard(ui));
    pass_over(ui, field);
    say_flag("element-keys", "plain", "hover", ui_focused(s) == field);

    // A hooked panel that is not focusable: a press on it is a press on nothing, which lets go;
    // focused by the app, it is handed the keys of the passes after.
    click_on(ui, unfocusable);
    say_flag("element-keys", "unfocusable", "press", ui_focused(s) == NULL);
    ui_set_focus(s, unfocusable);
    editor_pass(ui);
    g_seen_count = 0;
    type(ui, "h");
    say_flag("element-keys", "unfocusable", "set",
             ui_focused(s) == unfocusable && g_seen_count == 1 && g_seen[0].codepoint == 'h');
    free_text_doc(doc);
    return 0;
}

// A draw hook's painting: one quad over the element.
static void paint_marker(UIElement* el, UIDrawList* dl, void* user) {
    (void)user;
    ui_draw_quad(dl, el->rect, (vec4){0.25f, 0.5f, 0.75f, 1.0f});
}

static bool same_rect(UIRect a, UIRect b) {
    return fabsf(a.x - b.x) < 0.01f && fabsf(a.y - b.y) < 0.01f && fabsf(a.w - b.w) < 0.01f &&
           fabsf(a.h - b.h) < 0.01f;
}

// The quads drawn exactly over an element's rect: how many, and the widest border among them, with
// its colour and the fill of the quad that carries it.
typedef struct Over {
    int quads;
    float width;
    vec4 border, fill;
} Over;

static Over over(UISystem* ui, const UIElement* el) {
    const UIDrawList* dl = ui_draw_list(ui);
    Over o = {0};
    for (size_t i = 0; i < ui_draw_list_quad_count(dl); i++) {
        UIRect r;
        vec4 fill, border;
        float width = 0.0f;
        ui_draw_list_quad(dl, i, &r, fill, NULL);
        ui_draw_list_quad_border(dl, i, border, &width);
        if (!same_rect(r, el->rect))
            continue;
        o.quads++;
        if (width > o.width) {
            o.width = width;
            glm_vec4_copy(border, o.border);
            glm_vec4_copy(fill, o.fill);
        }
    }
    return o;
}

// A panel is drawn focused while it takes the keys (spec 14.11): its border over a draw hook's
// painting, focused or pressed, its own quad's border with no hook, over its image when its style
// has one, the theme's colour when the app sets one; and no border on a focusable panel with no key
// hook, a list's row, nor on a label.
static int probe_panel_focus(UISystem* ui, const Font* font) {
    static const vec4 FOCUS_BORDER = {0.45f, 0.70f, 1.00f, 0.6f};
    UIScreen* s = ui_screen(ui, "panels");
    ui_screen_set_modal(s, false);
    UIElement* root = ui_screen_root(s);
    UIElement* drawn = ui_panel(root);
    ui_set_size(drawn, UI_FIXED, 200.0f, UI_FIXED, 80.0f);
    drawn->focusable = true;
    ui_set_on_key(drawn, pane_keys, NULL);
    ui_set_draw(drawn, paint_marker, NULL);
    UIElement* keys = ui_panel(root);
    ui_set_size(keys, UI_FIXED, 200.0f, UI_FIXED, 40.0f);
    keys->focusable = true;
    ui_set_on_key(keys, pane_keys, NULL);
    UIElement* row = ui_panel(root);
    ui_set_size(row, UI_FIXED, 200.0f, UI_FIXED, 40.0f);
    row->focusable = true;
    // A style with an image draws the image, which carries no border, so its border goes over it.
    Texture atlas = {.id = font->atlas_texture_id};
    UIElement* pictured = ui_panel(root);
    ui_set_size(pictured, UI_FIXED, 200.0f, UI_FIXED, 40.0f);
    pictured->focusable = true;
    ui_set_on_key(pictured, pane_keys, NULL);
    ui_set_style(pictured, &(UIStyle){.bg_tex = &atlas});
    UIElement* label = ui_label(root, "label");
    label->focusable = true;
    ui_set_on_key(label, pane_keys, NULL);
    ui_set_draw(label, paint_marker, NULL);
    ui_push(ui, s);
    editor_pass(ui);

    Over o = over(ui, drawn);
    say_flag("panel-focus", "drawn", "unfocused", o.quads == 1 && o.width == 0.0f);

    ui_set_focus(s, drawn);
    editor_pass(ui);
    o = over(ui, drawn);
    say_flag("panel-focus", "drawn", "focused",
             o.quads == 2 && o.width == 1.0f && o.fill[3] == 0.0f &&
                 same_colour(o.border, FOCUS_BORDER));

    // Pressed and held, the panel is ACTIVE, and keeps the border.
    const float cx = drawn->rect.x + drawn->rect.w * 0.5f,
                cy = drawn->rect.y + drawn->rect.h * 0.5f;
    pointer_pass(ui, cx, cy, POINTER_PRESS, 0);
    ui_build(ui, 1280.0f, 720.0f, 1.0f);
    o = over(ui, drawn);
    say_flag("panel-focus", "drawn", "pressed", drawn->state == UI_STATE_ACTIVE && o.width == 1.0f);
    pointer_pass(ui, cx, cy, POINTER_RELEASE, 0);

    ui_set_focus(s, keys);
    editor_pass(ui);
    o = over(ui, keys);
    say_flag("panel-focus", "keys", "focused",
             o.quads == 1 && o.width == 1.0f && o.fill[3] > 0.0f &&
                 same_colour(o.border, FOCUS_BORDER));

    ui_set_focus(s, pictured);
    editor_pass(ui);
    o = over(ui, pictured);
    say_flag("panel-focus", "image", "focused",
             o.quads == 2 && o.width == 1.0f && same_colour(o.border, FOCUS_BORDER));

    ui_set_focus(s, row);
    editor_pass(ui);
    o = over(ui, row);
    say_flag("panel-focus", "row", "none", ui_focused(s) == row && o.width == 0.0f);

    ui_set_focus(s, label);
    editor_pass(ui);
    o = over(ui, label);
    say_flag("panel-focus", "label", "none", o.quads == 1 && o.width == 0.0f);

    // The app's colour for the focused look, from the theme.
    UITheme theme = *ui_theme(ui);
    const UITheme was = theme;
    glm_vec4_copy((vec4){1.0f, 0.0f, 0.0f, 1.0f}, theme.panel[UI_STATE_FOCUS].border);
    ui_set_theme(ui, &theme);
    ui_set_focus(s, drawn);
    editor_pass(ui);
    o = over(ui, drawn);
    say_flag("panel-focus", "theme", "colour",
             o.width == 1.0f && same_colour(o.border, (vec4){1.0f, 0.0f, 0.0f, 1.0f}));
    ui_set_theme(ui, &was);
    return 0;
}

// A fading screen fades a quad's border with its fill (spec 14.12): at a draw list's alpha of one
// half, both alphas halved.
static int probe_border_fade(UISystem* ui) {
    UIDrawList* dl = ui_draw_list(ui);
    ui_draw_list_begin(dl, 1280, 720);
    ui_draw_list_set_alpha(dl, 0.5f);
    ui_draw_rect(dl, (UIRect){10.0f, 10.0f, 100.0f, 50.0f},
                 &(UIStyle){.bg = {0.2f, 0.3f, 0.4f, 0.8f},
                            .border = {0.45f, 0.70f, 1.00f, 0.6f},
                            .border_width = 2.0f});
    vec4 fill = {0}, border = {0};
    float width = 0.0f;
    const bool one = ui_draw_list_quad_count(dl) == 1;
    ui_draw_list_quad(dl, 0, NULL, fill, NULL);
    ui_draw_list_quad_border(dl, 0, border, &width);
    say_flag("border-fade", "fill", "halved", one && fabsf(fill[3] - 0.4f) < 1e-6f);
    say_flag("border-fade", "border", "halved",
             one && fabsf(border[3] - 0.3f) < 1e-6f && width == 2.0f);
    return 0;
}

// The selection {2, 5} on the first line drawn as each shape: three cells, four with the character
// at its far end, and the line whole with its line break; the block caret over the character at
// the head.
// The colour the editor draws a selection in: the theme's, or with its alpha 0 the editor's own.
static void selection_colour(const UISystem* ui, vec4 out) {
    memcpy(out, ui_theme(ui)->selection, sizeof(vec4));
    if (out[3] <= 0.0f)
        glm_vec4_copy((vec4){0.25f, 0.45f, 0.85f, 0.45f}, out);
}

static int probe_editor_shapes(UISystem* ui) {
    TextDoc* doc = create_text_doc(SEAM_TEXT, strlen(SEAM_TEXT));
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    vec4 sel = {0.0f};
    selection_colour(ui, sel);
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

// The editor's glyphs on one side of the text's first column: how many, the first one's colour,
// and whether any other differs from it.
typedef struct {
    int count;
    vec4 colour;
    bool mixed;
} GlyphSide;

// The editor's glyphs split at the text's first column x by their centres, as a click is: the line
// numbers left of it, the code right. They are bitmaps from its face, so image quads, kind 1.
static void glyph_sides(const UISystem* ui, float x, GlyphSide* numbers, GlyphSide* code) {
    *numbers = *code = (GlyphSide){0};
    const UIDrawList* dl = ui_draw_list(ui);
    for (size_t i = 0; i < ui_draw_list_quad_count(dl); i++) {
        UIRect r;
        vec4 colour;
        int kind = 0;
        ui_draw_list_quad(dl, i, &r, colour, &kind);
        if (kind != 1)
            continue;
        GlyphSide* side = r.x + r.w * 0.5f < x ? numbers : code;
        if (side->count++ == 0)
            glm_vec4_copy(colour, side->colour);
        else if (!same_colour(colour, side->colour))
            side->mixed = true;
    }
}

static bool side_in(const GlyphSide* side, const float* colour) {
    return side->count > 0 && !side->mixed && same_colour(side->colour, colour);
}

// The line numbers in the text's colour at 38% with theme.line_numbers zeroed, in its colour once
// set while the code keeps its own, and at 38% again with it zeroed once more.
static int probe_editor_line_numbers(UISystem* ui) {
    TextDoc* doc = create_text_doc(SEAM_TEXT, strlen(SEAM_TEXT));
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    const UITheme before = *ui_theme(ui);
    UITheme plain = before, red = before;
    glm_vec4_zero(plain.line_numbers);
    glm_vec4_copy((vec4){1.0f, 0.0f, 0.0f, 1.0f}, red.line_numbers);
    ui_set_theme(ui, &plain);
    editor_pass(ui);
    float x = 0.0f, y = 0.0f;
    ui_editor_point_of(ed, 0, &x, &y);
    GlyphSide numbers, code;
    glyph_sides(ui, x, &numbers, &code);
    vec4 fg;
    glm_vec4_copy(code.colour, fg);
    const vec4 dim = {fg[0], fg[1], fg[2], fg[3] * 0.38f};
    const struct {
        const char* label;
        const char* key;
        const UITheme* theme;
        const float* numbers;
    } passes[] = {
        {"default", "dim", &plain, dim},
        {"set", "red", &red, red.line_numbers},
        {"cleared", "dim", &plain, dim},
    };
    for (size_t i = 0; i < sizeof(passes) / sizeof(passes[0]); i++) {
        ui_set_theme(ui, passes[i].theme);
        editor_pass(ui);
        glyph_sides(ui, x, &numbers, &code);
        say_flag("editor-line-numbers", passes[i].label, passes[i].key,
                 numbers.count == 3 && side_in(&numbers, passes[i].numbers) && side_in(&code, fg));
    }
    ui_set_theme(ui, &before);
    free_text_doc(doc);
    return 0;
}

// A flat quad drawn, and where it is in the draw list.
typedef struct {
    UIRect r;
    size_t at;
} Fill;

// The flat quads drawn in colour c, any colour for NULL, in the order drawn, up to `max`; how many
// there were.
static size_t fills_in(const UISystem* ui, const float* c, Fill* out, size_t max) {
    const UIDrawList* dl = ui_draw_list(ui);
    size_t n = 0;
    for (size_t i = 0; i < ui_draw_list_quad_count(dl); i++) {
        UIRect r;
        vec4 colour;
        int kind = 0;
        ui_draw_list_quad(dl, i, &r, colour, &kind);
        if (kind == 0 && (!c || same_colour(colour, c))) {
            if (n < max)
                out[n] = (Fill){r, i};
            n++;
        }
    }
    return n;
}

// A whole-line range fills every row of each line it reaches from the text's left edge to the
// editor's right, an empty line and a wrapped line's every row among them; one ending at a line's
// start does not reach that line; a selection draws over it; a style without the flag still bands
// only its characters; a zeroed style draws nothing.
static int probe_editor_whole_lines(UISystem* ui) {
    // A short line, an empty one, one that wraps in 600 points, and a last.
    char text[256];
    size_t len = (size_t)snprintf(text, sizeof(text), "short\n\n");
    for (int i = 0; i < 40; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "word ");
    len += (size_t)snprintf(text + len, sizeof(text) - len, "\nlast");
    const size_t last_at = len - 4;
    TextDoc* doc = create_text_doc(text, len);
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    const UITheme before = *ui_theme(ui);
    UITheme theme = before;
    vec4 red = {1.0f, 0.0f, 0.0f, 1.0f}, green = {0.0f, 1.0f, 0.0f, 1.0f};
    theme.editor_styles[1] = (UIEditorStyle){.whole_lines = true};
    theme.editor_styles[2] = (UIEditorStyle){0};
    glm_vec4_copy(red, theme.editor_styles[1].background);
    glm_vec4_copy(green, theme.editor_styles[2].background);
    ui_set_theme(ui, &theme);
    // The first three lines whole, up to the last's start; the last's first two characters.
    text_doc_set_ranges(doc, 0, 0, &(TextRange){0, last_at, 1}, 1);
    text_doc_set_ranges(doc, 1, 0, &(TextRange){last_at, last_at + 2, 2}, 1);
    ui_editor_set_selections(ed, &(TextSelection){1, 3}, 1);
    editor_pass(ui);
    vec4 sel = {0.0f};
    selection_colour(ui, sel);

    float tx = 0.0f, y0 = 0.0f, x = 0.0f, y1 = 0.0f, y_last = 0.0f;
    ui_editor_point_of(ed, 0, &tx, &y0);
    ui_editor_point_of(ed, 1, &x, &y1);
    const float cell = x - tx;
    ui_editor_point_of(ed, 6, &x, &y1);
    ui_editor_point_of(ed, last_at, &x, &y_last);
    const float row = y1 - y0;
    const int covered = (int)lroundf((y_last - y0) / row);
    Fill reds[16];
    const size_t red_count = fills_in(ui, red, reds, 16);
    // One quad a row above the last line, each the same span from the text's left edge.
    size_t above = 0;
    bool rows = covered >= 4 && red_count <= 16;
    for (size_t i = 0; rows && i < red_count; i++) {
        if (reds[i].r.y >= y_last - 0.01f)
            continue;
        rows = fabsf(reds[i].r.x - tx) < 0.01f && fabsf(reds[i].r.w - reds[0].r.w) < 0.01f &&
               fabsf(reds[i].r.y - (y0 + (float)above * row)) < 0.01f;
        above++;
    }
    rows = rows && above == (size_t)covered && reds[0].r.w > 40.0f * cell &&
           reds[0].r.x + reds[0].r.w <= ed->rect.x + ed->rect.w + 0.01f;
    if (!rows)
        fprintf(stderr, "editor-whole-lines: %zu red quads, %zu above the last line, %d rows\n",
                red_count, above, covered);
    say_flag("editor-whole-lines", "rows", "edge", rows);
    bool ends = true;
    for (size_t i = 0; i < red_count && i < 16; i++)
        ends = ends && fabsf(reds[i].r.y - y_last) >= 0.01f;
    say_flag("editor-whole-lines", "ends", "start", ends);
    Fill sels[4];
    const size_t sel_count = fills_in(ui, sel, sels, 4);
    say_flag("editor-whole-lines", "under", "selection",
             red_count > 0 && sel_count == 1 && fabsf(sels[0].r.y - y0) < 0.01f &&
                 sels[0].at > reds[0].at);
    const float plain = band_cells(ui, y_last, green, cell);
    say_flag("editor-whole-lines", "plain", "cells", fabsf(plain - 2.0f) < 0.01f);

    // The two styles zeroed: no fill as wide as a whole line, in any colour.
    theme.editor_styles[1] = theme.editor_styles[2] = (UIEditorStyle){0};
    ui_set_theme(ui, &theme);
    editor_pass(ui);
    Fill fills[256];
    const size_t fill_count = fills_in(ui, NULL, fills, 256);
    bool zero = red_count > 0 && fill_count <= 256;
    for (size_t i = 0; zero && i < fill_count; i++)
        zero = fabsf(fills[i].r.x - tx) >= 0.01f || fabsf(fills[i].r.w - reds[0].r.w) >= 0.01f;
    say_flag("editor-whole-lines", "zero", "none", zero);
    ui_set_theme(ui, &before);
    free_text_doc(doc);
    return 0;
}

// A glyph drawn: its rect, its top measured from its row's, its colour, and where it is in the draw
// list.
typedef struct {
    UIRect r;
    vec4 colour;
    size_t at;
} Glyph;

// The glyphs drawn on the row from `y`, `row` tall, whose centres stand in [x0, x1) -- left of the
// text a line's number and in the gap after it its sign, right of it the code -- in the order
// drawn, up to `max`; how many.
static size_t row_glyphs(const UISystem* ui, float x0, float x1, float y, float row, Glyph* out,
                         size_t max) {
    const UIDrawList* dl = ui_draw_list(ui);
    size_t n = 0;
    for (size_t i = 0; i < ui_draw_list_quad_count(dl); i++) {
        UIRect r;
        vec4 colour;
        int kind = 0;
        ui_draw_list_quad(dl, i, &r, colour, &kind);
        const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        if (kind != 1 || cy < y || cy >= y + row || cx < x0 || cx >= x1)
            continue;
        if (n < max) {
            out[n] = (Glyph){{r.x, r.y - y, r.w, r.h}, {0.0f}, i};
            glm_vec4_copy(colour, out[n].colour);
        }
        n++;
    }
    return n;
}

// The folds the editor holds are `want`, line for line and open for open.
static bool folds_are(const UIElement* ed, const UIEditorFold* want, size_t n) {
    const UIEditorFold* f = NULL;
    const size_t count = ui_editor_folds(ed, &f);
    bool same = count == n;
    for (size_t i = 0; same && i < n; i++)
        same =
            f[i].first == want[i].first && f[i].last == want[i].last && f[i].open == want[i].open;
    if (!same) {
        fprintf(stderr, "  %zu folds, want %zu:", count, n);
        for (size_t i = 0; i < count; i++)
            fprintf(stderr, " %zu-%zu%s", f[i].first, f[i].last, f[i].open ? " open" : "");
        fprintf(stderr, "\n");
    }
    return same;
}

#define FOLD_LABEL "@@ -11,30 +11,30 @@"

// A closed fold is one row, in the theme's fold colour, its label in the line numbers' and no
// number, the line after it numbered as before; a row move and a wheel take it as one row, a click
// on it lands on its first line and opens it; a line put in among its lines joins it and taking
// them all out takes it away; typing in a closed fold opens it; an editor keeps its folds in a text
// it leaves, another has none, and folds that overlap are refused.
static int probe_editor_folds(UISystem* ui) {
    // 60 lines, every seventh from the fourth long enough to wrap in 600 points.
    static char text[60 * 160];
    size_t len = 0;
    for (int i = 0; i < 60; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "line %02d%s\n", i,
                                i % 7 == 3 ? " of a text whose lines are long enough to wrap in a "
                                             "editor six hundred points across, once over at least"
                                           : "");
    TextDoc* doc = create_text_doc(text, len - 1);
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 400.0f);
    const UITheme before = *ui_theme(ui);
    UITheme theme = before;
    vec4 fold_colour = {0.5f, 0.0f, 0.5f, 1.0f}, numbers = {0.2f, 0.9f, 0.9f, 1.0f};
    glm_vec4_copy(fold_colour, theme.editor_fold);
    glm_vec4_copy(numbers, theme.line_numbers);
    ui_set_theme(ui, &theme);
    UIEditorFold folds[2] = {{10, 39, true, FOLD_LABEL}, {45, 50, true, NULL}};
    ui_editor_set_folds(ed, folds, 2);
    editor_pass(ui);
    float tx = 0.0f, y8 = 0.0f, x = 0.0f, y9 = 0.0f;
    ui_editor_scroll_line(ed, 8, 0.0f);
    editor_pass(ui);
    ui_editor_point_of(ed, text_doc_line_start(doc, 8), &tx, &y8);
    ui_editor_point_of(ed, text_doc_line_start(doc, 9), &x, &y9);
    const float row = y9 - y8;
    ui_editor_point_of(ed, text_doc_line_start(doc, 8) + 1, &x, &y9);
    const float cell = x - tx;
    // Line 40's number as it is drawn with the fold open, at the top.
    ui_editor_scroll_line(ed, 40, 0.0f);
    editor_pass(ui);
    float y40 = 0.0f;
    ui_editor_point_of(ed, text_doc_line_start(doc, 40), &x, &y40);
    Glyph want[4], got[4];
    const size_t want_n = row_glyphs(ui, -INFINITY, tx, y40, row, want, 4);

    // Closed, from line 8.
    ui_editor_set_fold_open(ed, 10, false);
    ui_editor_scroll_line(ed, 8, 0.0f);
    editor_pass(ui);
    float fold_y = 0.0f;
    ui_editor_point_of(ed, text_doc_line_start(doc, 10), &x, &fold_y);
    Fill fills[4];
    const size_t fill_n = fills_in(ui, fold_colour, fills, 4);
    bool rows = fill_n == 1 && fabsf(fills[0].r.y - fold_y) < 0.01f &&
                fabsf(fills[0].r.h - row) < 0.01f && fills[0].r.x < tx - 4.0f * cell &&
                fills[0].r.x + fills[0].r.w >= ed->rect.x + ed->rect.w - 20.0f;
    Glyph label[32];
    const size_t label_n = row_glyphs(ui, tx, INFINITY, fold_y, row, label, 32);
    size_t inked = 0;
    for (const char* c = FOLD_LABEL; *c; c++)
        inked += *c != ' ';
    rows = rows && label_n == inked && row_glyphs(ui, -INFINITY, tx, fold_y, row, got, 4) == 0;
    for (size_t i = 0; rows && i < label_n; i++)
        rows = same_colour(label[i].colour, numbers);
    // Line 40 the next row, numbered 41 as before.
    ui_editor_point_of(ed, text_doc_line_start(doc, 40), &x, &y40);
    const size_t got_n = row_glyphs(ui, -INFINITY, tx, y40, row, got, 4);
    rows = rows && fabsf(y40 - (fold_y + row)) < 0.01f && got_n == want_n && want_n == 2;
    for (size_t i = 0; rows && i < got_n; i++)
        rows = same_rect(got[i].r, want[i].r);
    // The open fold's lines each on rows of their own.
    float last_y = y40;
    for (size_t line = 45; rows && line <= 50; line++) {
        float ly = 0.0f;
        rows = ui_editor_point_of(ed, text_doc_line_start(doc, line), &x, &ly) && ly > last_y;
        last_y = ly;
    }
    if (!rows)
        fprintf(stderr,
                "editor-folds: %zu fold fills, the label %zu glyphs of %zu, line 40 at %.2f after "
                "the fold at %.2f, its number %zu glyphs of %zu\n",
                fill_n, label_n, inked, (double)y40, (double)fold_y, got_n, want_n);
    say_flag("editor-folds", "rows", "one", rows);

    // A row down from line 9 onto the fold, its first line, and on to line 40; a wheel's row from
    // line 9 puts the fold at the top; the row's offset and an offset in it.
    ui_editor_set_selections(
        ed, &(TextSelection){text_doc_line_start(doc, 9), text_doc_line_start(doc, 9)}, 1);
    ui_editor_apply(ed, UI_OP_MOVE, UI_MOTION_ROW_DOWN, 1);
    const bool onto = one_selection(ed, text_doc_line_start(doc, 10), text_doc_line_start(doc, 10));
    ui_editor_apply(ed, UI_OP_MOVE, UI_MOTION_ROW_DOWN, 1);
    const bool past = one_selection(ed, text_doc_line_start(doc, 40), text_doc_line_start(doc, 40));
    ui_editor_scroll_line(ed, 9, 0.0f);
    editor_pass(ui);
    wheel_rows(ui, ed, 1.0f);
    float top_rows = 1.0f;
    const size_t top = ui_editor_top_line(ed, &top_rows);
    const bool wheel =
        top == 10 && top_rows == 0.0f && ui_editor_top(ed) == text_doc_line_start(doc, 10);
    editor_pass(ui);
    ui_editor_point_of(ed, text_doc_line_start(doc, 10), &x, &fold_y);
    float in_x = 0.0f, in_y = 0.0f;
    const bool in = ui_editor_point_of(ed, text_doc_line_start(doc, 20) + 3, &in_x, &in_y) &&
                    fabsf(in_x - tx) < 0.01f && fabsf(in_y - fold_y) < 0.01f;
    const bool hit =
        ui_editor_offset_at(ed, tx + 0.5f, fold_y + row * 0.5f) == text_doc_line_start(doc, 10);
    if (!(onto && past && wheel && in && hit))
        fprintf(stderr, "editor-folds: onto %d past %d wheel %d (top %zu, %.2f) in %d hit %d\n",
                onto, past, wheel, top, (double)top_rows, in, hit);
    say_flag("editor-folds", "walk", "one_row", onto && past && wheel && in && hit);

    // A click on its row opens it, the caret on its first line; closed again by line.
    press_drag(ui, tx + 0.5f, fold_y + row * 0.5f, tx + 0.5f, fold_y + row * 0.5f, 0);
    const UIEditorFold opened[2] = {{10, 39, true, NULL}, {45, 50, true, NULL}};
    const bool clicked = folds_are(ed, opened, 2) && one_selection(ed, text_doc_line_start(doc, 10),
                                                                   text_doc_line_start(doc, 10));
    const UIEditorFold shut[2] = {{10, 39, false, NULL}, {45, 50, true, NULL}};
    const bool closed = ui_editor_set_fold_open(ed, 20, false) &&
                        !ui_editor_set_fold_open(ed, 41, false) && folds_are(ed, shut, 2);
    const UIEditorFold* read = NULL;
    ui_editor_folds(ed, &read);
    const bool labelled = read && read[0].label && !strcmp(read[0].label, FOLD_LABEL) &&
                          read[0].label != folds[0].label && !read[1].label;
    say_flag("editor-folds", "open", "click", clicked && closed && labelled);

    // A line put in among its lines joins it; the other moves down a line. Taking all its lines out
    // takes it away. Typing at a caret in a closed fold opens it.
    text_doc_replace(doc, text_doc_line_start(doc, 20), text_doc_line_start(doc, 20), "new\n", 4);
    const UIEditorFold grown[2] = {{10, 40, false, NULL}, {46, 51, true, NULL}};
    bool edits = folds_are(ed, grown, 2);
    text_doc_replace(doc, text_doc_line_start(doc, 10), text_doc_line_start(doc, 41), "", 0);
    const UIEditorFold gone[1] = {{15, 20, true, NULL}};
    edits = edits && folds_are(ed, gone, 1);
    ui_editor_set_folds(ed, &(UIEditorFold){4, 8, false, NULL}, 1);
    const size_t at6 = text_doc_line_start(doc, 6);
    ui_editor_set_selections(ed, &(TextSelection){at6, at6}, 1);
    ui_editor_type(ed, "x");
    size_t l6_len = 0;
    const char* l6 = text_doc_line(doc, 6, &l6_len);
    edits = edits && folds_are(ed, &(UIEditorFold){4, 8, true, NULL}, 1) && l6_len && l6[0] == 'x';
    // A Delete there opens it too; typing on the text read-only opens nothing, as nothing changes.
    ui_editor_set_fold_open(ed, 4, false);
    ui_editor_set_selections(ed, &(TextSelection){at6, at6}, 1);
    ui_editor_apply(ed, UI_OP_DELETE, UI_MOTION_CHAR_RIGHT, 1);
    edits = edits && folds_are(ed, &(UIEditorFold){4, 8, true, NULL}, 1);
    ui_editor_set_fold_open(ed, 4, false);
    doc->read_only = true;
    ui_editor_type(ed, "y");
    doc->read_only = false;
    edits = edits && folds_are(ed, &(UIEditorFold){4, 8, false, NULL}, 1);
    say_flag("editor-folds", "edits", "moved", edits);

    // Home and End from a line the fold hides stay on that line.
    ui_editor_set_selections(ed, &(TextSelection){at6 + 2, at6 + 2}, 1);
    ui_editor_apply(ed, UI_OP_MOVE, UI_MOTION_ROW_START, 1);
    bool hidden = one_selection(ed, at6, at6);
    ui_editor_apply(ed, UI_OP_MOVE, UI_MOTION_ROW_END, 1);
    size_t end6 = 0;
    text_doc_line(doc, 6, &end6);
    hidden = hidden && one_selection(ed, at6 + end6, at6 + end6);
    say_flag("editor-folds", "hidden", "own_line", hidden);

    // Shown another text and back, the folds as they were; a second editor on the text has none.
    ui_editor_set_fold_open(ed, 4, false);
    TextDoc* other = create_text_doc("other\ntext\n", 11);
    ui_editor_set_doc(ed, other);
    editor_pass(ui);
    bool parked = ui_editor_folds(ed, NULL) == 0;
    ui_editor_set_doc(ed, doc);
    editor_pass(ui);
    parked = parked && folds_are(ed, &(UIEditorFold){4, 8, false, NULL}, 1);
    UIElement* second = ui_editor(ui_screen_root(ui_top(ui)), doc);
    ui_set_size(second, UI_FIXED, 300.0f, UI_FIXED, 100.0f);
    editor_pass(ui);
    parked = parked && ui_editor_folds(second, NULL) == 0;
    say_flag("editor-folds", "parked", "kept", parked);

    // Folds that overlap, refused: the fold stays.
    const UIEditorFold overlapping[2] = {{1, 5, false, NULL}, {5, 9, false, NULL}};
    ui_editor_set_folds(ed, overlapping, 2);
    say_flag("editor-folds", "refused", "kept",
             folds_are(ed, &(UIEditorFold){4, 8, false, NULL}, 1));

    // The standard mode's commands, by name: open, toggle shut, close, and none with no fold at
    // the caret.
    const size_t at5 = text_doc_line_start(doc, 5);
    ui_editor_set_selections(ed, &(TextSelection){at5, at5}, 1);
    bool commands =
        ui_editor_run(ed, "open-fold") && folds_are(ed, &(UIEditorFold){4, 8, true, NULL}, 1) &&
        ui_editor_run(ed, "toggle-fold") && folds_are(ed, &(UIEditorFold){4, 8, false, NULL}, 1) &&
        ui_editor_run(ed, "toggle-fold") && ui_editor_run(ed, "close-fold") &&
        folds_are(ed, &(UIEditorFold){4, 8, false, NULL}, 1);
    const size_t at12 = text_doc_line_start(doc, 12);
    ui_editor_set_selections(ed, &(TextSelection){at12, at12}, 1);
    commands = commands && !ui_editor_run(ed, "open-fold");
    say_flag("editor-folds", "commands", "by_name", commands);

    ui_set_theme(ui, &before);
    free_text_doc(other);
    free_text_doc(doc);
    return 0;
}

// Fillers drawn in `colour`; the theme it replaced, to put back.
static UITheme filler_theme(UISystem* ui, const float* colour) {
    const UITheme before = *ui_theme(ui);
    UITheme theme = before;
    glm_vec4_copy((float*)colour, theme.editor_filler);
    ui_set_theme(ui, &theme);
    return before;
}

// Filler rows above a line, in the theme's filler colour from the text's left edge and with no
// number, and above a closed fold's row but not above a line it hides; a row move steps over them,
// the view's rows into a line count them, a click on one lands at its line's start, and a line's
// rows as asked are its rows as drawn.
static int probe_editor_fillers(UISystem* ui) {
    // 30 lines, the 20th long enough to wrap in 600 points.
    static char text[30 * 200];
    size_t len = 0;
    for (int i = 0; i < 30; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "line %02d%s\n", i,
                                i == 20 ? " of a text long enough to wrap in an editor six "
                                          "hundred points across, once over at the least, twice"
                                        : "");
    TextDoc* doc = create_text_doc(text, len - 1);
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 400.0f);
    vec4 filler = {0.0f, 0.6f, 0.6f, 1.0f};
    const UITheme before = filler_theme(ui, filler);
    const UIEditorFiller fillers[3] = {{5, 2}, {10, 1}, {11, 1}};
    ui_editor_set_fillers(ed, fillers, 3);
    ui_editor_set_folds(ed, &(UIEditorFold){10, 12, false, NULL}, 1);
    ui_editor_scroll_line(ed, 3, 0.0f);
    editor_pass(ui);
    float tx = 0.0f, y3 = 0.0f, y4 = 0.0f, y5 = 0.0f, y10 = 0.0f, x = 0.0f;
    ui_editor_point_of(ed, text_doc_line_start(doc, 3), &tx, &y3);
    ui_editor_point_of(ed, text_doc_line_start(doc, 4), &x, &y4);
    ui_editor_point_of(ed, text_doc_line_start(doc, 5), &x, &y5);
    ui_editor_point_of(ed, text_doc_line_start(doc, 10), &x, &y10);
    const float row = y4 - y3;
    // Two filler rows above line 5, one above the fold's row and none for line 11 inside it.
    Fill fills[8];
    const size_t fill_n = fills_in(ui, filler, fills, 8);
    const float want_y[3] = {y4 + row, y4 + 2.0f * row, y10 - row};
    bool rows = fill_n == 3 && fabsf(y5 - (y4 + 3.0f * row)) < 0.01f;
    for (size_t i = 0; rows && i < 3; i++)
        rows = fabsf(fills[i].r.y - want_y[i]) < 0.01f && fabsf(fills[i].r.x - tx) < 0.01f &&
               fabsf(fills[i].r.h - row) < 0.01f &&
               fills[i].r.x + fills[i].r.w >= ed->rect.x + ed->rect.w - 20.0f;
    for (size_t i = 0; rows && i < 3; i++)
        rows = row_glyphs(ui, -INFINITY, INFINITY, want_y[i], row, NULL, 0) == 0;
    if (!rows)
        fprintf(stderr, "editor-fillers: %zu filler quads, line 5 at %.2f, line 4 at %.2f\n",
                fill_n, (double)y5, (double)y4);
    say_flag("editor-fillers", "rows", "above", rows);

    // A row down from line 4 lands on line 5 and from line 9 on the fold, up from line 5 on line 4;
    // a wheel's row from line 4 at the top puts line 5's first filler there, and its rows count
    // the fillers.
    const size_t at4 = text_doc_line_start(doc, 4), at5 = text_doc_line_start(doc, 5);
    const size_t at9 = text_doc_line_start(doc, 9), at10 = text_doc_line_start(doc, 10);
    ui_editor_set_selections(ed, &(TextSelection){at4, at4}, 1);
    ui_editor_apply(ed, UI_OP_MOVE, UI_MOTION_ROW_DOWN, 1);
    bool walk = one_selection(ed, at5, at5);
    ui_editor_apply(ed, UI_OP_MOVE, UI_MOTION_ROW_UP, 1);
    walk = walk && one_selection(ed, at4, at4);
    ui_editor_set_selections(ed, &(TextSelection){at9, at9}, 1);
    ui_editor_apply(ed, UI_OP_MOVE, UI_MOTION_ROW_DOWN, 1);
    walk = walk && one_selection(ed, at10, at10);
    ui_editor_scroll_line(ed, 4, 0.0f);
    editor_pass(ui);
    wheel_rows(ui, ed, 1.0f);
    float r1 = -1.0f;
    const size_t l1 = ui_editor_top_line(ed, &r1);
    ui_editor_scroll_line(ed, 5, 2.0f);
    editor_pass(ui);
    float r2 = -1.0f;
    const size_t l2 = ui_editor_top_line(ed, &r2);
    walk = walk && l1 == 5 && r1 == 0.0f && l2 == 5 && r2 == 2.0f && ui_editor_top(ed) == at5;
    if (!walk)
        fprintf(stderr, "editor-fillers: a wheel's row to %zu, %.2f; line 5 put at %zu, %.2f\n", l1,
                (double)r1, l2, (double)r2);
    say_flag("editor-fillers", "walk", "over", walk);

    // A click on a filler row lands at the start of the line below it.
    ui_editor_scroll_line(ed, 3, 0.0f);
    editor_pass(ui);
    press_drag(ui, tx + 40.0f, y4 + row * 1.5f, tx + 40.0f, y4 + row * 1.5f, 0);
    bool click =
        ui_editor_offset_at(ed, tx + 40.0f, y4 + row * 1.5f) == at5 && one_selection(ed, at5, at5);
    // On the filler above a closed fold's row: the fold's first line, the fold left closed.
    press_drag(ui, tx + 40.0f, y10 - row * 0.5f, tx + 40.0f, y10 - row * 0.5f, 0);
    const UIEditorFold* folds = NULL;
    click = click && one_selection(ed, at10, at10) && ui_editor_folds(ed, &folds) == 1 &&
            !folds[0].open;
    say_flag("editor-fillers", "click", "line_start", click);

    // Line 20's rows as asked, as drawn: from its first row to line 21's.
    ui_editor_scroll_line(ed, 19, 0.0f);
    editor_pass(ui);
    float y20 = 0.0f, y21 = 0.0f;
    ui_editor_point_of(ed, text_doc_line_start(doc, 20), &x, &y20);
    ui_editor_point_of(ed, text_doc_line_start(doc, 21), &x, &y21);
    const size_t asked = ui_editor_line_rows(ed, 20);
    say_flag("editor-fillers", "line_rows", "drawn",
             asked >= 2 && fabsf((y21 - y20) - (float)asked * row) < 0.01f &&
                 ui_editor_line_rows(ed, 19) == 1 && ui_editor_line_rows(ed, 30) == 0);

    ui_set_theme(ui, &before);
    free_text_doc(doc);
    return 0;
}

// Whether a flat quad ends at the text's left edge `tx` -- a gutter's fill -- on the row from `y`,
// or with `y` NAN on any.
static bool ends_at_text(const UISystem* ui, float tx, float y) {
    Fill fills[256];
    const size_t n = fills_in(ui, NULL, fills, 256);
    for (size_t i = 0; i < n && i < 256; i++)
        if (fabsf(fills[i].r.x + fills[i].r.w - tx) < 0.01f &&
            (isnan(y) || fabsf(fills[i].r.y - y) < 0.01f))
            return true;
    return false;
}

// A range's style marks the gutter of each line it reaches: a fill on every row from the gutter's
// left to the text, drawn under the number, and a sign in the gap after the number on the line's
// first row only, in the text's plain colour; a sign alone draws no fill; a zeroed style neither.
static int probe_editor_gutter(UISystem* ui) {
    char text[512];
    size_t len = (size_t)snprintf(text, sizeof(text), "short\n");
    const size_t long_at = len;
    for (int i = 0; i < 40; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "word ");
    const size_t minus_at = len + 1;
    len += (size_t)snprintf(text + len, sizeof(text) - len, "\nminus\nlast");
    TextDoc* doc = create_text_doc(text, len);
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    const UITheme before = *ui_theme(ui);
    UITheme theme = before;
    vec4 red = {1.0f, 0.0f, 0.0f, 1.0f};
    theme.editor_styles[1] = (UIEditorStyle){.sign = '+'};
    glm_vec4_copy(red, theme.editor_styles[1].gutter);
    theme.editor_styles[2] = (UIEditorStyle){.sign = '-'};
    ui_set_theme(ui, &theme);
    text_doc_set_ranges(doc, 0, 0, &(TextRange){long_at, minus_at - 1, 1}, 1);
    text_doc_set_ranges(doc, 1, 0, &(TextRange){minus_at, minus_at + 2, 2}, 1);
    editor_pass(ui);
    float tx = 0.0f, y0 = 0.0f, x = 0.0f, y_long = 0.0f, y_minus = 0.0f;
    ui_editor_point_of(ed, 0, &tx, &y0);
    ui_editor_point_of(ed, 1, &x, &y0);
    const float cell = x - tx;
    ui_editor_point_of(ed, long_at, &x, &y_long);
    ui_editor_point_of(ed, minus_at, &x, &y_minus);
    const float row = y_long - y0;
    const int long_rows = (int)lroundf((y_minus - y_long) / row);

    // One red quad a row of the long line, from the gutter's left to the text, before its number.
    Fill reds[8];
    const size_t red_count = fills_in(ui, red, reds, 8);
    bool fill = long_rows >= 2 && red_count == (size_t)long_rows;
    for (size_t i = 0; fill && i < red_count; i++)
        fill = fabsf(reds[i].r.y - (y_long + (float)i * row)) < 0.01f &&
               fabsf(reds[i].r.h - row) < 0.01f && fabsf(reds[i].r.x + reds[i].r.w - tx) < 0.01f &&
               reds[i].r.x < tx - 4.5f * cell;
    // The gutter's numbers stand left of its gap, the gap's two cells before the text.
    const float gap = tx - 2.0f * cell;
    Glyph number[4];
    fill = fill && row_glyphs(ui, -INFINITY, gap, y_long, row, number, 4) > 0 &&
           reds[0].at < number[0].at;
    if (!fill)
        fprintf(stderr, "editor-gutter: %zu red quads for %d rows\n", red_count, long_rows);
    say_flag("editor-gutter", "fill", "under", fill);

    // The + on the long line's first row only, in the text's colour; the - on its line, shorter;
    // and the - alone fills nothing.
    Glyph code[1], plus[2], minus[2];
    row_glyphs(ui, tx, INFINITY, y0, row, code, 1);
    const size_t plus_n = row_glyphs(ui, gap, tx, y_long, row, plus, 2);
    size_t later = 0;
    for (int r = 1; r < long_rows; r++)
        later += row_glyphs(ui, gap, tx, y_long + (float)r * row, row, NULL, 0);
    const size_t minus_n = row_glyphs(ui, gap, tx, y_minus, row, minus, 2);
    const size_t short_n = row_glyphs(ui, gap, tx, y0, row, NULL, 0);
    const bool sign = plus_n == 1 && later == 0 && minus_n == 1 && short_n == 0 &&
                      !ends_at_text(ui, tx, y_minus) &&
                      same_colour(plus[0].colour, code[0].colour) &&
                      same_colour(minus[0].colour, code[0].colour) && plus[0].r.h > minus[0].r.h;
    if (!sign)
        fprintf(stderr, "editor-gutter: + %zu, on later rows %zu, - %zu, on the short line %zu\n",
                plus_n, later, minus_n, short_n);
    say_flag("editor-gutter", "sign", "first_row", sign);

    // Zeroed, neither: no quad reaching the text from the left and no glyph in the gap.
    theme.editor_styles[1] = theme.editor_styles[2] = (UIEditorStyle){0};
    ui_set_theme(ui, &theme);
    editor_pass(ui);
    bool zero = !ends_at_text(ui, tx, NAN);
    for (int r = 0; zero && r < long_rows + 3; r++)
        zero = row_glyphs(ui, gap, tx, y0 + (float)r * row, row, NULL, 0) == 0;
    say_flag("editor-gutter", "zero", "none", zero);
    ui_set_theme(ui, &before);
    free_text_doc(doc);
    return 0;
}

// The fills a bar wide drawn wholly inside `rect` less the bar's insets, and those that are not;
// all of them false past 256 fills.
static bool bars_in(const UISystem* ui, UIRect rect, size_t* in, size_t* out) {
    Fill fills[256];
    const size_t n = fills_in(ui, NULL, fills, 256);
    const UIRect inner =
        ui_rect_inset(rect, UI_BAR_INSET, UI_BAR_INSET, UI_BAR_INSET, UI_BAR_INSET);
    *in = *out = 0;
    for (size_t i = 0; i < n && i < 256; i++) {
        const UIRect r = fills[i].r;
        if (fabsf(r.w - UI_BAR_W) >= 0.01f)
            continue;
        const bool inside = r.x >= inner.x - 0.01f && r.y >= inner.y - 0.01f &&
                            r.x + r.w <= inner.x + inner.w + 0.01f &&
                            r.y + r.h <= inner.y + inner.h + 0.01f;
        *(inside ? in : out) += 1;
    }
    return n <= 256;
}

// A bar is drawn only inside its element, its insets included: an editor sized to nothing draws
// none, nor does one 4 points wide, where a press on its edge pages nothing; one with room draws
// its thumb inside itself.
static int probe_editor_bar_room(UISystem* ui) {
    static char text[200 * 16];
    size_t len = 0;
    for (int i = 0; i < 200; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "line %03d\n", i);
    TextDoc* doc = create_text_doc(text, len - 1);
    // In a row as pcb's panes are, the one sized to nothing first.
    UIElement* hidden = editor_fixture(ui, doc, 0.0f, 0.0f);
    ui_screen_root(ui_top(ui))->dir = UI_ROW;
    UIElement* shown = ui_editor(ui_screen_root(ui_top(ui)), doc);
    ui_set_size(shown, UI_FIXED, 300.0f, UI_FIXED, 150.0f);
    editor_pass(ui);

    // Every bar the one with room's, its thumb inside it.
    size_t in = 0, out = 0;
    const bool counted = bars_in(ui, shown->rect, &in, &out);
    if (out)
        fprintf(stderr, "editor-bar-room: %zu bars outside the editor with room\n", out);
    say_flag("editor-bar-room", "none", "outside", counted && out == 0);
    say_flag("editor-bar-room", "room", "thumb", counted && in >= 1);

    ui_set_size(hidden, UI_FIXED, 4.0f, UI_FIXED, 150.0f);
    editor_pass(ui);
    const bool narrow = bars_in(ui, shown->rect, &in, &out);
    if (out)
        fprintf(stderr, "editor-bar-room: %zu bars outside the editor with room, one narrow\n",
                out);
    say_flag("editor-bar-room", "narrow", "none", narrow && out == 0);

    // A press inside it, below where a thumb would stand: with a bar there it would page.
    const UIRect at = hidden->rect;
    pointer_pass(ui, at.x + 1.0f, at.y + at.h - 20.0f, POINTER_PRESS, 0);
    pointer_pass(ui, at.x + 1.0f, at.y + at.h - 20.0f, POINTER_RELEASE, 0);
    editor_pass(ui);
    const size_t top = ui_editor_top(hidden);
    if (top)
        fprintf(stderr, "editor-bar-room: a press scrolled the narrow editor to %zu\n", top);
    say_flag("editor-bar-room", "press", "still", top == 0);
    free_text_doc(doc);
    return 0;
}

// Each action told with its step: typing joined into words shares one, a group holds one open over
// any actions -- typing, a Backspace, more typing -- and one undo takes it all back. The same with
// keep_undo off.
static uint64_t g_steps[64];
static int g_step_count;

static void note_step(TextDoc* doc, const TextDocAction* action, void* user) {
    (void)doc;
    (void)user;
    if (g_step_count < 64)
        g_steps[g_step_count++] = action->step;
}

// Whether every action told since the count was cleared shares one step.
static bool one_step(void) {
    for (int i = 1; i < g_step_count; i++)
        if (g_steps[i] != g_steps[0])
            return false;
    return g_step_count > 0;
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
            // Typing with keep_undo off is no step's: typing on once it is back on starts a step
            // of its own, which taking back leaves the rest as it was.
            doc->keep_undo = false;
            type(ui, "Q");
            doc->keep_undo = true;
            size_t was_len = 0;
            char* was = text_doc_text(doc, &was_len);
            type(ui, "R");
            text_doc_undo(doc);
            say_flag("editor-steps", which, "rejoined", was && doc_is(doc, was, was_len));
            free(was);
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
                if (ch == '[' || ch == ']')
                    out[n++] = (InputKeyEvent){.key = ch == '[' ? GLFW_KEY_LEFT_BRACKET
                                                                : GLFW_KEY_RIGHT_BRACKET,
                                               .mods = GLFW_MOD_CONTROL};
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

// An editor in a fresh vim mode on `text`, laid out, the caret at `at`.
static UIElement* vim_fixture(UISystem* ui, TextDoc* doc, UIEditorVim* vim, size_t at) {
    UIElement* ed = editor_fixture(ui, doc, 600.0f, 300.0f);
    ui_editor_set_mode(ed, ui_editor_vim_mode(vim));
    editor_pass(ui);
    ui_editor_set_selections(ed, &(TextSelection){at, at}, 1);
    return ed;
}

// Whether the text was emptied to one empty line on the way -- vim's empty buffer, where its
// bytes and vim's no longer agree.
static bool g_vim_emptied;

static void note_vim_emptied(TextDoc* doc, const TextDocAction* action, void* user) {
    (void)action;
    (void)user;
    char text[3] = {0};
    const size_t len = text_doc_length(doc);
    if (len <= 2)
        text_doc_copy(doc, 0, len, text);
    g_vim_emptied = g_vim_emptied || len == 0 || !strcmp(text, "\n") || !strcmp(text, "\r\n");
}

// ---- The rules of scripts/vim_layout.vim as a host's indent and format hooks (spec 14.9), which
// the layout oracle holds beside vim's indentexpr and formatexpr: change one and change the other.

static bool layout_blank(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (s[i] != ' ' && s[i] != '\t')
            return false;
    return true;
}

// The columns a line's leading blanks reach, tabs to the text's stops: vim's indent().
static int layout_columns(const TextDoc* doc, const char* s, size_t len) {
    int col = 0;
    for (size_t i = 0; i < len && (s[i] == ' ' || s[i] == '\t'); i++)
        col = s[i] == '\t' ? col + doc->tab_width - col % doc->tab_width : col + 1;
    return col;
}

static int layout_opens(const char* s, size_t len) {
    int opens = 0;
    for (size_t k = 0; k < len; k++)
        opens += (s[k] == '(') - (s[k] == ')');
    return opens;
}

// The indent of the line above that is not blank, two more for each ( it opens and two fewer for
// each ) it closes, never below 0; a line opening with ; keeps its own. Worked down the lines, each
// from the one above as it will stand, as vim asks indentexpr a line at a time.
static bool layout_indent(UIElement* el, size_t first, size_t last, int* cols, void* user) {
    (void)user;
    TextDoc* doc = ui_editor_doc(el);
    size_t len = 0;
    const char* s = NULL;
    int above = -1, opens = 0; // the indent and the opens of the line above that is not blank
    for (size_t prev = first; prev-- > 0;) {
        s = text_doc_line(doc, prev, &len);
        if (!layout_blank(s, len)) {
            above = layout_columns(doc, s, len);
            opens = layout_opens(s, len);
            break;
        }
    }
    for (size_t line = first; line <= last; line++) {
        s = text_doc_line(doc, line, &len);
        size_t i = 0;
        while (i < len && (s[i] == ' ' || s[i] == '\t'))
            i++;
        const int col = i < len && s[i] == ';'  ? -1
                        : above < 0             ? 0
                        : above + 2 * opens > 0 ? above + 2 * opens
                                                : 0;
        cols[line - first] = col;
        if (!layout_blank(s, len)) {
            above = col < 0 ? layout_columns(doc, s, len) : col;
            opens = layout_opens(s, len);
        }
    }
    return true;
}

static bool layout_put(char** buf, size_t* n, size_t* cap, const char* s, size_t k) {
    if (!grow_array((void**)buf, cap, *n + k + 1, 1, 64))
        return false;
    memcpy(*buf + *n, s, k);
    *n += k;
    return true;
}

// The lines' words, split at blanks, laid out again at the first line's indent, as many to a line
// as fit in 24 bytes with the indent; no words make one empty line.
static bool layout_format(UIElement* el, size_t first, size_t last, char** out, size_t* len,
                          void* user) {
    (void)user;
    TextDoc* doc = ui_editor_doc(el);
    size_t first_len = 0;
    const int ind = layout_columns(doc, text_doc_line(doc, first, &first_len), first_len);
    const char* nl = text_doc_newline(doc);
    char spaces[256];
    memset(spaces, ' ', sizeof(spaces));
    const size_t indent = (size_t)ind < sizeof(spaces) ? (size_t)ind : sizeof(spaces);
    char* buf = NULL;
    size_t n = 0, cap = 0, line_len = 0;
    bool ok = layout_put(&buf, &n, &cap, "", 0);
    for (size_t line = first; ok && line <= last; line++) {
        size_t ll = 0;
        const char* s = text_doc_line(doc, line, &ll);
        for (size_t i = 0; ok && i < ll;) {
            if (s[i] == ' ' || s[i] == '\t') {
                i++;
                continue;
            }
            size_t j = i;
            while (j < ll && s[j] != ' ' && s[j] != '\t')
                j++;
            const size_t w = j - i;
            if (!line_len) {
                ok = layout_put(&buf, &n, &cap, spaces, indent);
            } else if (indent + line_len + 1 + w <= 24) {
                ok = layout_put(&buf, &n, &cap, " ", 1);
                line_len++;
            } else {
                ok = layout_put(&buf, &n, &cap, nl, strlen(nl)) &&
                     layout_put(&buf, &n, &cap, spaces, indent);
                line_len = 0;
            }
            ok = ok && layout_put(&buf, &n, &cap, s + i, w);
            line_len += w;
            i = j;
        }
    }
    if (!ok) {
        free(buf);
        return false;
    }
    *out = buf;
    *len = n;
    return true;
}

// Each case of the file CETRA_VIM_CASES names -- "<text in hex> <line> <column> <keys in hex>",
// the keys in vim's notation, after any folds -- run on a fresh text, editor and vim mode, the
// cursor put where vim's cursor() would put it, the text read-only with CETRA_VIM_READ_ONLY set,
// and the layout rules installed as the host's hooks with CETRA_VIM_LAYOUT. Prints "vim <case>
// <line> <column> <the text in hex> <emptied>", as vim's line('.') and col('.') count, the last 1
// when the text was emptied on the way.
static int probe_vim_run(UISystem* ui) {
    const char* path = getenv("CETRA_VIM_CASES");
    const bool read_only = getenv("CETRA_VIM_READ_ONLY") != NULL;
    const bool layout = getenv("CETRA_VIM_LAYOUT") != NULL;
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
            if (vim && layout) {
                ui_editor_vim_set_indent(vim, layout_indent, NULL);
                ui_editor_vim_set_format(vim, layout_format, NULL);
            }
            if (doc && vim && keys) {
                g_vim_emptied = false;
                text_doc_set_on_action(doc, note_vim_emptied, NULL);
                if (engine)
                    engine_clipboard_set(engine, "");
                const size_t start = text_doc_line_start(doc, at_line - 1) + at_col - 1;
                UIElement* ed = vim_fixture(ui, doc, vim, start);
                doc->read_only = read_only;
                // The folds the keys begin with, "<fold A B>" lines A to B as vim numbers them,
                // closed, as vim's :A,Bfold makes them.
                UIEditorFold folds[8];
                size_t fold_count = 0;
                const char* rest = keys;
                int a = 0, b = 0, used = 0;
                while (fold_count < 8 && sscanf(rest, "<fold %d %d>%n", &a, &b, &used) == 2 &&
                       used > 0) {
                    folds[fold_count++] = (UIEditorFold){(size_t)a - 1, (size_t)b - 1};
                    rest += used;
                    used = 0;
                }
                ui_editor_set_folds(ed, folds, fold_count);
                // A key at a time, as typed.
                const size_t n = vim_events(rest, events, 4096);
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
                printf(" %d\n", g_vim_emptied ? 1 : 0);
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
    // A count past the most a put makes: refused, the text as it was.
    vim_keys(ui, "<Esc>gg0yl999999999p");
    say_flag("vim-state", "count", "held",
             doc_is_str(doc, "Z" VIM_TEXT) && ui_editor_vim_state(vim) == UI_VIM_NORMAL);
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
    // Two areas sharing a character are one: d takes them whole.
    vim_keys(ui, "u");
    const TextSelection touching[] = {{0, 0}, {3, 3}};
    ui_editor_set_selections(ed, touching, 2);
    vim_keys(ui, "vllld");
    say_flag("vim-carets", "visual", "touching", doc_is_str(doc, "eta\nalpha beta\nalpha beta\n"));
    // Two carets whose ranges nest out of their order -- the first inside the bracket the second
    // takes whole -- take every line of the outer once: > and, with the layout's indent, = too.
    static const char NESTED[] = "  (a\n(b c)\nd)\n";
    const TextSelection nested[] = {{6, 6}, {11, 11}};
    text_doc_set_text(doc, NESTED, strlen(NESTED));
    ui_editor_set_selections(ed, nested, 2);
    vim_keys(ui, ">i(");
    say_flag("vim-carets", "nested", "shift", doc_is_str(doc, "      (a\n    (b c)\n    d)\n"));
    text_doc_set_text(doc, NESTED, strlen(NESTED));
    ui_editor_set_selections(ed, nested, 2);
    ui_editor_vim_set_indent(vim, layout_indent, NULL);
    vim_keys(ui, "=i(");
    say_flag("vim-carets", "nested", "indent", doc_is_str(doc, "(a\n  (b c)\n  d)\n"));
    ui_editor_set_mode(ed, NULL);
    free_text_doc(doc);
    free_ui_editor_vim(vim);
    return 0;
}

static const UIElement* g_refused_by;

static void note_refusal(TextDoc* doc, const UIElement* editor, void* user) {
    count_refusal(doc, editor, user);
    g_refused_by = editor;
}

#define VIM_RO_TEXT "lpha beta\n  gamma delta\nlast line\n"

// The vim mode on a read-only text (spec 14.7), as vim with 'nomodifiable': a visual command
// letting the area go with the cursor at its start, told with its editor; a command one refusal
// however many changes it would make; a refused command kept for `.`, which refuses it again; a
// block's I into an insert that refuses what is typed; and a text made read-only during an insert
// leaving the insert open.
static int probe_vim_read_only(UISystem* ui) {
    TextDoc* doc = create_text_doc(VIM_TEXT, strlen(VIM_TEXT));
    UIEditorVim* vim = create_ui_editor_vim();
    UIElement* ed = vim_fixture(ui, doc, vim, 0);
    text_doc_set_on_refused(doc, note_refusal, NULL);
    g_refusals = 0;
    g_refused_by = NULL;
    vim_keys(ui, "x");
    doc->read_only = true;
    ui_editor_set_selections(ed, &(TextSelection){5, 5}, 1);
    vim_keys(ui, "vlld");
    say_flag("vim-read-only", "visual", "let_go",
             g_refusals == 1 && g_refused_by == ed && ui_editor_vim_state(vim) == UI_VIM_NORMAL &&
                 one_selection(ed, 5, 5) && doc_is_str(doc, VIM_RO_TEXT));
    vim_keys(ui, "3dd");
    say_flag("vim-read-only", "command", "once", g_refusals == 2 && doc_is_str(doc, VIM_RO_TEXT));
    vim_keys(ui, "5J");
    say_flag("vim-read-only", "join", "once", g_refusals == 3 && doc_is_str(doc, VIM_RO_TEXT));
    vim_keys(ui, ".");
    say_flag("vim-read-only", "repeat", "refused",
             g_refusals == 4 && one_selection(ed, 5, 5) && doc_is_str(doc, VIM_RO_TEXT));
    // Writable again, `.` makes the last refused command, a normal mode one whose caller keeps
    // nothing of its own: the lines joined.
    doc->read_only = false;
    ui_editor_set_selections(ed, &(TextSelection){0, 0}, 1);
    vim_keys(ui, ".");
    say_flag("vim-read-only", "repeat", "kept",
             doc_is_str(doc, "lpha beta gamma delta last line\n"));
    doc->read_only = true;
    vim_keys(ui, "<C-v>I");
    const bool block_in = ui_editor_vim_state(vim) == UI_VIM_INSERT;
    vim_keys(ui, "Z");
    say_flag("vim-read-only", "block", "insert",
             block_in && g_refusals == 5 && doc_is_str(doc, "lpha beta gamma delta last line\n"));
    vim_keys(ui, "<Esc>");
    doc->read_only = false;
    ui_editor_set_selections(ed, &(TextSelection){0, 0}, 1);
    vim_keys(ui, "ib");
    doc->read_only = true;
    vim_keys(ui, "cd");
    say_flag("vim-read-only", "insert", "open",
             ui_editor_vim_state(vim) == UI_VIM_INSERT && g_refusals == 7 &&
                 doc_is_str(doc, "blpha beta gamma delta last line\n"));
    vim_keys(ui, "<Esc>");
    say_flag("vim-read-only", "insert", "left", ui_editor_vim_state(vim) == UI_VIM_NORMAL);
    ui_editor_set_mode(ed, NULL);
    free_text_doc(doc);
    free_ui_editor_vim(vim);

    // What `.` makes of a refused command, the text made writable again, and what vim only beeps
    // at, each against vim 9.2's own run: a visual put's delete under it, a 3s's count, a J on the
    // last line, an r past the line's end, a refused r and a ~ on an empty line.
    static const struct {
        const char* label;
        const char* setup;
        const char* refused;
        int refusals;
        const char* again;
        const char* want;
    } KEEP[] = {
        {"put", "wyiw0", "viwp", 1, "0.", " def ghi\n\njkl mno\n"},
        {"count", "x", "3sZ<Esc>", 1, "0.", "def ghi\n\njkl mno\n"},
        {"join", "x", "GJ", 0, "gg.", "c def ghi\n\njkl mno\n"},
        {"past", "x", "$5rq", 0, "gg.", "c def ghi\n\njkl mno\n"},
        {"replace", "x", "rq", 1, "gg.", "c def ghi\n\njkl mno\n"},
        {"tilde", "x", "j~", 0, "gg.", "c def ghi\n\njkl mno\n"},
    };
    for (size_t i = 0; i < sizeof(KEEP) / sizeof(KEEP[0]); i++) {
        const char* text = "abc def ghi\n\njkl mno\n";
        TextDoc* d = create_text_doc(text, strlen(text));
        UIEditorVim* v = create_ui_editor_vim();
        UIElement* e = vim_fixture(ui, d, v, 0);
        text_doc_set_on_refused(d, note_refusal, NULL);
        vim_keys(ui, KEEP[i].setup);
        d->read_only = true;
        g_refusals = 0;
        vim_keys(ui, KEEP[i].refused);
        const int refusals = g_refusals;
        d->read_only = false;
        vim_keys(ui, KEEP[i].again);
        say_flag("vim-read-only", KEEP[i].label, "again",
                 refusals == KEEP[i].refusals && doc_is_str(d, KEEP[i].want));
        ui_editor_set_mode(e, NULL);
        ui_pop(ui);
        free_text_doc(d);
        free_ui_editor_vim(v);
    }
    return 0;
}

// Undo by vim's units: an insert from i to Esc is one step, Enters and Backspaces in it included;
// `cw` and what it typed is one; each is told to a host as one step too, with keep_undo off.
static int probe_vim_undo(UISystem* ui) {
    for (int keep = 1; keep >= 0; keep--) {
        const char* which = keep ? "kept" : "off";
        TextDoc* doc = create_text_doc(VIM_TEXT, strlen(VIM_TEXT));
        doc->keep_undo = keep;
        text_doc_set_on_action(doc, note_step, NULL);
        UIEditorVim* vim = create_ui_editor_vim();
        UIElement* ed = vim_fixture(ui, doc, vim, 0);
        g_step_count = 0;
        vim_keys(ui, "ione<CR>two<BS>o three<Esc>");
        say_flag("vim-undo", which, "insert", one_step() && g_step_count > 3);
        if (keep) {
            vim_keys(ui, "u");
            say_flag("vim-undo", which, "insert_undone", doc_is_str(doc, VIM_TEXT));
        }
        g_step_count = 0;
        vim_keys(ui, "jwcwnew<Esc>");
        say_flag("vim-undo", which, "change", one_step() && g_step_count >= 2);
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
    // An insert of more keys than any fixed record would hold: made again whole by `.`, which
    // ends in normal mode.
    TextDoc* doc = create_text_doc("", 0);
    UIEditorVim* vim = create_ui_editor_vim();
    UIElement* ed = vim_fixture(ui, doc, vim, 0);
    vim_keys(ui, "i");
    InputKeyEvent xs[50];
    for (size_t i = 0; i < 50; i++)
        xs[i] = (InputKeyEvent){.codepoint = 'x'};
    for (int pass = 0; pass < 100; pass++)
        send_keys(ui, xs, 50);
    vim_keys(ui, "<Esc>.");
    say_flag("vim-undo", "long", "repeated",
             text_doc_length(doc) == 10000 && ui_editor_vim_state(vim) == UI_VIM_NORMAL);
    ui_editor_set_mode(ed, NULL);
    free_text_doc(doc);
    free_ui_editor_vim(vim);
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
    // One vim shared by two editors: an insert left unfinished in the first is let go when keys
    // reach the second, which reads them in normal mode, so its x deletes.
    ui_editor_vim_set_prompt(g_vim_host, NULL, NULL);
    vim_keys(ui, "iq");
    TextDoc* other = create_text_doc("xyz\n", 4);
    UIElement* ed2 = vim_fixture(ui, other, g_vim_host, 0);
    vim_keys(ui, "x");
    say_flag("vim-host", "shared", "let_go",
             doc_is_str(other, "yz\n") && ui_editor_vim_state(g_vim_host) == UI_VIM_NORMAL);
#ifdef __APPLE__
    // A paste in insert mode is made again by `.` as the text it typed, whatever the clipboard
    // holds by then.
    engine_clipboard_set(ui_system_engine(ui), "pq");
    vim_keys(ui, "0i");
    const InputKeyEvent paste = {.key = GLFW_KEY_V, .mods = GLFW_MOD_SUPER};
    send_keys(ui, &paste, 1);
    vim_keys(ui, "<Esc>");
    engine_clipboard_set(ui_system_engine(ui), "zz");
    vim_keys(ui, ".");
    say_flag("vim-host", "paste", "repeated", doc_is_str(other, "ppqqyz\n"));
#else
    say_flag("vim-host", "paste", "repeated", true); // the paste key is macOS's Cmd+V
#endif
    ui_editor_set_mode(ed2, NULL);
    free_text_doc(other);
    ui_editor_set_mode(ed, NULL);
    free_text_doc(doc);
    free_ui_editor_vim(g_vim_host);
    return 0;
}

// ---- The host answering what a name is (spec 14.9)

#define LOOKUP_TEXT "alpha beta\ngamma alpha\nbeta x\n"

// The recording host: each kind asked, the selection and the state it saw, and its answer -- true
// moving the caret to the text's start, to show it went somewhere, or showing the editor another
// text when one is given.
static int g_lookups;
static UIEditorVimLookup g_lookup_kinds[16];
static bool g_lookup_answer = true;
static TextSelection g_lookup_seen;
static UIEditorVimState g_lookup_state;
static TextDoc* g_lookup_elsewhere;

static bool lookup_host(UIElement* el, UIEditorVimLookup kind, void* user) {
    const UIEditorVim* vim = user;
    if (g_lookups < 16)
        g_lookup_kinds[g_lookups] = kind;
    g_lookups++;
    const TextSelection* s = NULL;
    size_t primary = 0;
    if (ui_editor_selections(el, &s, &primary))
        g_lookup_seen = s[primary];
    g_lookup_state = ui_editor_vim_state(vim);
    if (g_lookup_elsewhere)
        ui_editor_set_doc(el, g_lookup_elsewhere);
    else if (g_lookup_answer)
        ui_editor_set_selections(el, &(TextSelection){0, 0}, 1);
    return g_lookup_answer;
}

static void caret_at(UIElement* ed, size_t at) {
    ui_editor_set_selections(ed, &(TextSelection){at, at}, 1);
}

static int probe_vim_lookup(UISystem* ui) {
    TextDoc* doc = create_text_doc(LOOKUP_TEXT, strlen(LOOKUP_TEXT));
    UIEditorVim* vim = create_ui_editor_vim();
    ui_editor_vim_set_lookup(vim, lookup_host, vim);
    UIElement* ed = vim_fixture(ui, doc, vim, 17);
    // gd, gD, Ctrl+] and K in normal mode: each asked once, of its own kind, and the host's caret
    // kept.
    static const char* const ASKS[] = {"gd", "gD", "<C-]>", "K"};
    bool went = true;
    for (int i = 0; i < 4; i++) {
        caret_at(ed, 17);
        vim_keys(ui, ASKS[i]);
        went = went && one_selection(ed, 0, 0) && !*ui_editor_vim_pending(vim);
    }
    say_flag("vim-lookup", "normal", "asked",
             went && g_lookups == 4 && g_lookup_kinds[0] == UI_VIM_LOOKUP_DECLARATION &&
                 g_lookup_kinds[1] == UI_VIM_LOOKUP_GLOBAL &&
                 g_lookup_kinds[2] == UI_VIM_LOOKUP_TAG &&
                 g_lookup_kinds[3] == UI_VIM_LOOKUP_KEYWORD && g_lookup_seen.head == 17);
    // g, Esc, then d: nothing asked, and the d waits for its motion.
    vim_keys(ui, "g<Esc>d");
    say_flag("vim-lookup", "escaped", "waiting",
             g_lookups == 4 && !strcmp(ui_editor_vim_pending(vim), "d"));
    vim_keys(ui, "<Esc>");
    // dgd and gd in visual mode: vim's motions, nothing asked. The delete ends at a line's start,
    // so it stops before that line's break, as vim's exclusive motions do.
    caret_at(ed, 23);
    vim_keys(ui, "dgd");
    const bool deleted = doc_is_str(doc, "alpha \nbeta x\n") && one_selection(ed, 5, 5);
    vim_keys(ui, "u");
    caret_at(ed, 23);
    vim_keys(ui, "vgd");
    const bool visual = ui_editor_vim_state(vim) == UI_VIM_VISUAL && one_selection(ed, 23, 6);
    vim_keys(ui, "<Esc>");
    say_flag("vim-lookup", "motions", "vim", g_lookups == 4 && deleted && visual);
    // Ctrl+] and K in visual mode: asked once each with the area in place, then out of visual
    // mode at the area's head, where the host left it.
    g_lookup_answer = false;
    caret_at(ed, 17);
    vim_keys(ui, "viw<C-]>");
    const bool tag = g_lookups == 5 && g_lookup_kinds[4] == UI_VIM_LOOKUP_TAG &&
                     g_lookup_seen.anchor == 17 && g_lookup_seen.head == 21 &&
                     g_lookup_state == UI_VIM_VISUAL && ui_editor_vim_state(vim) == UI_VIM_NORMAL &&
                     one_selection(ed, 21, 21);
    caret_at(ed, 17);
    vim_keys(ui, "viwK");
    const bool keyword = g_lookups == 6 && g_lookup_kinds[5] == UI_VIM_LOOKUP_KEYWORD &&
                         ui_editor_vim_state(vim) == UI_VIM_NORMAL;
    say_flag("vim-lookup", "visual", "asked", tag && keyword);
    // An area over two lines is no name: nothing asked, and the area kept, as vim keeps it.
    caret_at(ed, 17);
    vim_keys(ui, "vj<C-]>");
    say_flag("vim-lookup", "visual", "lines",
             g_lookups == 6 && ui_editor_vim_state(vim) == UI_VIM_VISUAL);
    vim_keys(ui, "<Esc>");
    // A host that finds nothing: gd is vim's own, and Ctrl+] and K leave the caret.
    caret_at(ed, 23);
    vim_keys(ui, "gd");
    const bool own = g_lookups == 7 && one_selection(ed, 6, 6);
    caret_at(ed, 17);
    vim_keys(ui, "<C-]>K");
    say_flag("vim-lookup", "declined", "vim", own && g_lookups == 9 && one_selection(ed, 17, 17));
    // . after a lookup makes the change before it again, not the lookup.
    g_lookup_answer = true;
    caret_at(ed, 0);
    vim_keys(ui, "x");
    caret_at(ed, 16);
    vim_keys(ui, "gd.");
    say_flag("vim-lookup", "repeat", "change",
             g_lookups == 10 && doc_is_str(doc, "pha beta\ngamma alpha\nbeta x\n"));
    // A read-only text still asks: a lookup changes nothing.
    doc->read_only = true;
    caret_at(ed, 16);
    vim_keys(ui, "gdK");
    say_flag("vim-lookup", "read_only", "asked", g_lookups == 12);
    doc->read_only = false;
    // A host going to a definition in the same text from a block: visual mode ends where it went,
    // not at the block's head.
    caret_at(ed, 16);
    vim_keys(ui, "<C-v>e<C-]>");
    say_flag("vim-lookup", "block", "went",
             g_lookups == 13 && ui_editor_vim_state(vim) == UI_VIM_NORMAL &&
                 one_selection(ed, 0, 0));
    // With no host, dgd after a j is characterwise, where vim's takes the j's lines: the one
    // difference the mode keeps.
    ui_editor_vim_set_lookup(vim, NULL, NULL);
    text_doc_set_text(doc, LOOKUP_TEXT, strlen(LOOKUP_TEXT));
    caret_at(ed, 0);
    vim_keys(ui, "jjdgd");
    say_flag("vim-lookup", "after_j", "characters",
             doc_is_str(doc, "alpha \nbeta x\n") && one_selection(ed, 5, 5));
    ui_editor_vim_set_lookup(vim, lookup_host, vim);
    // A host showing the editor another text from visual mode: the mode back in normal mode, the
    // other text's caret left where it was, and the next key read there.
    TextDoc* other = create_text_doc("xyz\n", 4);
    g_lookup_elsewhere = other;
    caret_at(ed, 17);
    vim_keys(ui, "viw<C-]>");
    g_lookup_elsewhere = NULL;
    const bool away = ui_editor_doc(ed) == other && ui_editor_vim_state(vim) == UI_VIM_NORMAL &&
                      one_selection(ed, 0, 0);
    vim_keys(ui, "x");
    say_flag("vim-lookup", "elsewhere", "normal", away && doc_is_str(other, "yz\n"));
    ui_editor_set_mode(ed, NULL);
    ui_editor_set_doc(ed, NULL);
    free_text_doc(other);
    free_text_doc(doc);
    free_ui_editor_vim(vim);
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

// ---- Elements taken out and moved (spec 14.6)

static int g_presses;

static void count_press(UIElement* el, void* user) {
    (void)el;
    (void)user;
    g_presses++;
}

// A button's action and an editor's key each taking their own element out, inside the pass: the
// button twice, the second refused, since it is out already.
static void remove_self(UIElement* el, void* user) {
    (void)user;
    ui_remove(el);
    ui_remove(el);
}

static bool remove_editor(UIElement* el, void* user) {
    (void)user;
    ui_remove(el);
    return true;
}

static const UIEditorCommand REMOVE_EDITOR = {"remove", "Takes the editor out", remove_editor,
                                              NULL};
static const UIEditorBinding REMOVE_KEYS[] = {{GLFW_KEY_F9, 0, &REMOVE_EDITOR}};

static void build_row(UIElement* row, int index, void* user) {
    (void)index;
    (void)user;
    ui_label(row, "a row");
}

static int probe_ui_remove(UISystem* ui) {
    UIScreen* s = ui_screen(ui, "remove");
    ui_screen_set_modal(s, false);
    UIElement* root = ui_screen_root(s);
    root->align_main = UI_ALIGN_START;
    UIElement* inner = ui_panel(root);
    UIElement* inner_button = ui_button(inner, "inner", count_press, NULL);
    UIElement* after = ui_button(root, "after", count_press, NULL);
    UIElement* pressed = ui_button(root, "pressed", count_press, NULL);
    const UIElement* self = ui_button(root, "self", remove_self, NULL);
    UIElement* pane = ui_scroll(root);
    ui_set_size(pane, UI_FIXED, 200.0f, UI_FIXED, 80.0f);
    for (int i = 0; i < 30; i++)
        ui_label(pane, "a line");
    UIElement* rows = ui_scroll(root);
    ui_set_size(rows, UI_FIXED, 200.0f, UI_FIXED, 80.0f);
    ui_scroll_set_rows(rows, 10, 20.0f, build_row, NULL);
    ui_push(ui, s);
    ui_layout(s, 1280.0f, 720.0f);
    const UIElement *p = NULL, *b = NULL, *seen = NULL;

    // The focus inside what goes, as an input pass last saw it: both let go, and the next layout
    // puts the element after it where it stood.
    ui_set_focus(s, inner_button);
    pointer_pass(ui, -1.0f, -1.0f, POINTER_MOVE, 0);
    const float inner_y = inner->rect.y;
    const size_t children = root->child_count;
    ui_remove(inner);
    ui_system_held(ui, &p, &b, &seen);
    say_flag("remove", "focus", "nothing", ui_focused(s) == NULL);
    say_flag("remove", "seen", "nothing", seen == NULL);
    ui_layout(s, 1280.0f, 720.0f);
    say_flag("remove", "layout", "whole",
             root->child_count == children - 1 && root->children[0] == after &&
                 fabsf(after->rect.y - inner_y) < 0.01f);

    // A press held on what goes: let go, and its release activates nothing.
    float x = 0.0f, y = 0.0f;
    centre_of(pressed, &x, &y);
    pointer_pass(ui, x, y, POINTER_PRESS, 0);
    ui_system_held(ui, &p, &b, &seen);
    const bool was_pressed = p == pressed;
    ui_remove(pressed);
    ui_system_held(ui, &p, &b, &seen);
    pointer_pass(ui, x, y, POINTER_RELEASE, 0);
    say_flag("remove", "pressed", "let_go", was_pressed && p == NULL && g_presses == 0);

    // A button whose own action takes it out, inside the pass that released it, and asks again,
    // refused.
    ui_layout(s, 1280.0f, 720.0f);
    centre_of(self, &x, &y);
    const size_t before_self = root->child_count;
    pointer_pass(ui, x, y, POINTER_PRESS, 0);
    pointer_pass(ui, x, y, POINTER_RELEASE, 0);
    ui_system_held(ui, &p, &b, &seen);
    say_flag("remove", "own_action", "freed", root->child_count == before_self - 1 && p == NULL);

    // A bar held on the pane that goes.
    ui_layout(s, 1280.0f, 720.0f);
    const float bx = pane->rect.x + pane->rect.w - 5.0f, by = pane->rect.y + 10.0f;
    pointer_pass(ui, bx, by, POINTER_PRESS, 0);
    ui_system_held(ui, &p, &b, &seen);
    const bool was_held = b == pane;
    ui_remove(pane);
    ui_system_held(ui, &p, &b, &seen);
    pointer_pass(ui, bx, by + 20.0f, POINTER_HOLD, 0);
    say_flag("remove", "bar", "let_go", was_held && b == NULL);

    // Refused: a screen's root, and a pane's row. And the focus is refused an element of another
    // screen, which is how nothing holds an element taken out.
    const size_t kept = root->child_count;
    ui_remove(root);
    say_flag("remove", "root", "refused", ui_screen_root(s) == root && root->child_count == kept);
    ui_layout(s, 1280.0f, 720.0f);
    UIElement* row = ui_scroll_find_row(rows, 0);
    ui_remove(row);
    say_flag("remove", "row", "refused", row && ui_scroll_find_row(rows, 0) == row);
    UIScreen* os = ui_screen(ui, "other");
    UIElement* stranger = ui_panel(ui_screen_root(os));
    ui_set_focus(s, after);
    ui_set_focus(s, stranger);
    say_flag("remove", "focus", "refused", ui_focused(s) == after);

    // An editor its own key takes out, inside the pass the key came in: freed as the pass ends,
    // its text forgetting it.
    TextDoc* doc = create_text_doc("text\n", 5);
    UIElement* ed = editor_fixture(ui, doc, 300.0f, 120.0f);
    const UIEditorMode mode = {.name = "remover",
                               .parent = ui_editor_standard_mode(),
                               .bindings = REMOVE_KEYS,
                               .binding_count = 1,
                               .layer = true};
    ui_editor_set_mode(ed, &mode);
    editor_pass(ui);
    const UIElement* holder = ui_screen_root(ui_top(ui));
    key(ui, GLFW_KEY_F9, 0);
    say_flag("remove", "editor_key", "freed", holder->child_count == 0 && doc->view_count == 0);

    // An editor taken out mid-insert: its text closes the insert's group.
    UIEditorVim* vim = create_ui_editor_vim();
    UIElement* ved = vim_fixture(ui, doc, vim, 0);
    vim_keys(ui, "iab");
    const bool open = doc->grouping;
    ui_remove(ved);
    say_flag("remove", "insert", "closed", open && !doc->grouping && doc->view_count == 0);
    free_ui_editor_vim(vim);
    free_text_doc(doc);
    return 0;
}

#define MOVE_LINES 40

// A row of the editor's text, for a view to be scrolled down it.
static char* move_text(size_t* len) {
    char* text = malloc(MOVE_LINES * 16);
    size_t n = 0;
    for (int i = 0; i < MOVE_LINES; i++)
        n += (size_t)snprintf(text + n, 16, "line %d\n", i);
    *len = n;
    return text;
}

static bool children_are(const UIElement* parent, UIElement* const* want, size_t n) {
    if (parent->child_count != n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (parent->children[i] != want[i])
            return false;
    return true;
}

static int probe_ui_move(UISystem* ui) {
    size_t len = 0;
    char* text = move_text(&len);
    TextDoc* doc = create_text_doc(text, len);
    free(text);
    UIElement* ed = editor_fixture(ui, doc, 300.0f, 120.0f);
    UIScreen* s = ui_top(ui);
    UIElement* root = ui_screen_root(s);
    UIElement* field = ui_text_field(root, 0, NULL, NULL);
    ui_field_set_text(field, "kept");
    editor_pass(ui);
    const TextSelection carets[2] = {{8, 8}, {16, 18}};
    ui_editor_set_selections(ed, carets, 2);
    ui_editor_scroll_place(ed, 35, 0.0f); // line 5's start
    editor_pass(ui);
    const size_t top = ui_editor_top(ed);

    // The editor under a new row, beside the field: its carets, its view and the focus kept.
    UIElement* row = ui_panel(root);
    row->dir = UI_ROW;
    ui_move(ed, row, NULL);
    ui_move(field, row, NULL);
    editor_pass(ui);
    const TextSelection* now = NULL;
    const size_t n = ui_editor_selections(ed, &now, NULL);
    say_flag("move", "carets", "kept",
             n == 2 && now[0].anchor == 8 && now[0].head == 8 && now[1].anchor == 16 &&
                 now[1].head == 18);
    say_flag("move", "scroll", "kept", top == 35 && ui_editor_top(ed) == top);
    ui_editor_set_selections(ed, &(TextSelection){0, 0}, 1);
    type(ui, "z");
    char typed[3] = {0};
    text_doc_copy(doc, 0, 2, typed);
    say_flag("move", "focus", "kept",
             ui_focused(s) == ed && text_doc_length(doc) == len + 1 && !strcmp(typed, "zl"));
    say_flag("move", "field", "kept",
             field->parent == row && row->children[1] == field &&
                 !strcmp(ui_field_text(field), "kept"));

    // Where it goes: in front of the child named, the end for none, and in front of itself where
    // it is.
    UIElement* col = ui_panel(root);
    UIElement* a = ui_panel(col);
    UIElement* b = ui_panel(col);
    UIElement* c = ui_panel(col);
    ui_move(a, col, NULL);
    const bool to_end = children_are(col, (UIElement* const[]){b, c, a}, 3);
    ui_move(a, col, b);
    const bool to_start = children_are(col, (UIElement* const[]){a, b, c}, 3);
    ui_move(c, col, b);
    const bool between = children_are(col, (UIElement* const[]){a, c, b}, 3);
    ui_move(c, col, c);
    say_flag("move", "before", "order",
             to_end && to_start && between && children_are(col, (UIElement* const[]){a, c, b}, 3));

    // Refused: a screen's root, in front of no child of the parent, into itself or under it, a
    // pane's row or into a pane with rows or one of its rows, and to another screen.
    ui_move(root, col, NULL);
    say_flag("move", "root", "refused", root->parent == NULL && col->child_count == 3);
    ui_move(a, col, ed);
    say_flag("move", "before", "refused", children_are(col, (UIElement* const[]){a, c, b}, 3));
    ui_move(row, row, NULL);
    ui_move(row, ed, NULL);
    say_flag("move", "under", "refused", row->parent == root && ed->parent == row);
    UIElement* rows = ui_scroll(root);
    ui_set_size(rows, UI_FIXED, 200.0f, UI_FIXED, 80.0f);
    ui_scroll_set_rows(rows, 10, 20.0f, build_row, NULL);
    editor_pass(ui);
    UIElement* first = ui_scroll_find_row(rows, 0);
    ui_move(a, rows, NULL);
    ui_move(a, first, NULL);
    ui_move(first, col, NULL);
    say_flag("move", "rows", "refused",
             a->parent == col && first && first->parent == rows && col->child_count == 3);
    UIScreen* elsewhere = ui_screen(ui, "elsewhere");
    ui_move(a, ui_screen_root(elsewhere), NULL);
    say_flag("move", "screen", "refused",
             a->parent == col && ui_screen_root(elsewhere)->child_count == 0);
    free_text_doc(doc);
    return 0;
}

// ---- A hidden element (spec 14.18)

// A screen of its own on top, alone, its children from the top.
static UIScreen* scene_alone(UISystem* ui, const char* name) {
    ui_pop_all(ui);
    UIScreen* s = ui_screen(ui, name);
    ui_screen_set_modal(s, false);
    ui_screen_root(s)->align_main = UI_ALIGN_START;
    ui_push(ui, s);
    return s;
}

// A panel without padding in `parent`.
static UIElement* bare_panel(UIElement* parent) {
    UIElement* p = ui_panel(parent);
    for (int i = 0; i < 4; i++)
        p->padding[i] = -1.0f;
    return p;
}

// A panel `w` by `h`, without padding, in `parent`.
static UIElement* fixed_panel(UIElement* parent, float w, float h) {
    UIElement* p = bare_panel(parent);
    ui_set_size(p, UI_FIXED, w, UI_FIXED, h);
    return p;
}

// `lines` numbered lines, each long enough to wrap in an editor 300 wide.
static TextDoc* hidden_text(int lines) {
    static char text[200 * 96];
    size_t len = 0;
    for (int i = 0; i < lines && i < 200; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len,
                                "line %03d of a text whose lines wrap in a narrow editor\n", i);
    return create_text_doc(text, len);
}

// The flow: the middle of three panels 100 wide with gaps of 10 hidden, and one of two growing in
// a row 300 wide: no room and no gap, laid out empty, and where they stood once shown.
static void hidden_flow(UISystem* ui) {
    UIElement* root = ui_screen_root(scene_alone(ui, "hidden-flow"));
    UIElement* row = bare_panel(root);
    row->dir = UI_ROW;
    row->spacing = 10.0f;
    UIElement* p[3];
    for (int i = 0; i < 3; i++)
        p[i] = fixed_panel(row, 100.0f, 40.0f);
    UIElement* inner = fixed_panel(p[1], 20.0f, 20.0f);
    UIElement* grows = fixed_panel(root, 300.0f, 40.0f);
    grows->dir = UI_ROW;
    UIElement* g[2] = {ui_panel(grows), ui_panel(grows)};
    for (int i = 0; i < 2; i++)
        ui_set_size(g[i], UI_GROW, 0.0f, UI_FIXED, 40.0f);
    UIElement* const all[] = {p[0], p[1], p[2], inner, g[0], g[1]};
    editor_pass(ui);
    UIRect was[6];
    for (int i = 0; i < 6; i++)
        was[i] = all[i]->rect;
    ui_set_hidden(p[1], true);
    ui_set_hidden(g[1], true);
    editor_pass(ui);
    say_flag("hidden", "gap", "none",
             fabsf(p[2]->rect.x - p[0]->rect.x - 110.0f) < 0.01f &&
                 fabsf(row->rect.w - 210.0f) < 0.01f);
    say_flag("hidden", "grow", "whole", fabsf(g[0]->rect.w - 300.0f) < 0.01f);
    say_flag("hidden", "empty", "rects",
             p[1]->rect.w == 0.0f && p[1]->rect.h == 0.0f && inner->rect.w == 0.0f &&
                 inner->rect.h == 0.0f);
    ui_set_hidden(p[1], false);
    ui_set_hidden(g[1], false);
    editor_pass(ui);
    bool stood = true;
    for (int i = 0; i < 6; i++)
        stood = stood && same_rect(all[i]->rect, was[i]);
    say_flag("hidden", "shown", "stood", stood);
}

// Drawn: an editor and a panel with a fill, hidden, draw what the screen draws without them.
static void hidden_drawn(UISystem* ui, TextDoc* doc) {
    UIElement* root = ui_screen_root(scene_alone(ui, "hidden-drawn"));
    ui_label(root, "kept");
    UIElement* ed = fixed_editor(root, doc, 300.0f, 150.0f);
    UIElement* filled = fixed_panel(root, 50.0f, 50.0f);
    ui_set_style(filled, &(UIStyle){.bg = {1.0f, 0.0f, 0.0f, 1.0f}});
    editor_pass(ui);
    const size_t shown_quads = ui_draw_list_quad_count(ui_draw_list(ui));
    ui_set_hidden(ed, true);
    ui_set_hidden(filled, true);
    editor_pass(ui);
    const size_t hidden_quads = ui_draw_list_quad_count(ui_draw_list(ui));
    const uint64_t hidden_sig = ui_draw_list_signature(ui_draw_list(ui));
    ui_remove(ed);
    ui_remove(filled);
    editor_pass(ui);
    say_flag("hidden", "drawn", "nothing",
             hidden_quads < shown_quads &&
                 hidden_quads == ui_draw_list_quad_count(ui_draw_list(ui)) &&
                 hidden_sig == ui_draw_list_signature(ui_draw_list(ui)));
}

// The pointer: a press and a wheel where a hidden editor stood go to the pane round it.
static void hidden_pointer(UISystem* ui, TextDoc* doc) {
    UIScreen* s = scene_alone(ui, "hidden-pointer");
    UIElement* pane = ui_scroll(ui_screen_root(s));
    ui_set_size(pane, UI_FIXED, 300.0f, UI_FIXED, 150.0f);
    UIElement* ed = fixed_editor(pane, doc, 300.0f, 150.0f);
    for (int i = 0; i < 30; i++)
        ui_label(pane, "a line");
    editor_pass(ui);
    float x = 0.0f, y = 0.0f;
    centre_of(ed, &x, &y);
    ui_set_hidden(ed, true);
    editor_pass(ui);
    ui_update(ui, &(UIInput){.pointer_x = x, .pointer_y = y, .wheel_y = -1.0f}, 1280.0f, 720.0f);
    const float wheeled = pane->scroll_target;
    pointer_pass(ui, x, y, POINTER_PRESS, 0);
    const UIElement *pressed = NULL, *bar = NULL, *seen = NULL;
    ui_system_held(ui, &pressed, &bar, &seen);
    pointer_pass(ui, x, y, POINTER_RELEASE, 0);
    say_flag("hidden", "press", "behind",
             wheeled > 0.0f && pressed != ed && ui_focused(s) != ed && ui_editor_top(ed) == 0);
}

// The focus and what is held: hiding the focused editor leaves the focus on nothing and a key
// typed goes nowhere; nothing hidden is focused again; a press and a bar held let go when hidden;
// an editor moved under a hidden panel lets go of the focus.
static void hidden_focus(UISystem* ui, TextDoc* doc) {
    UIScreen* s = scene_alone(ui, "hidden-focus");
    UIElement* root = ui_screen_root(s);
    UIElement* ed = fixed_editor(root, doc, 300.0f, 150.0f);
    UIElement* button = ui_button(root, "pressed", count_press, NULL);
    UIElement* field = ui_text_field(root, 0, NULL, NULL);
    UIElement* holder = fixed_panel(root, 100.0f, 40.0f);
    UIElement* under = ui_text_field(holder, 0, NULL, NULL);
    ui_set_focus(s, ed);
    editor_pass(ui);
    const size_t length = text_doc_length(doc);
    ui_set_hidden(ed, true);
    const bool let_go = ui_focused(s) == NULL;
    type(ui, "x");
    say_flag("hidden", "focus", "nothing", let_go && text_doc_length(doc) == length);

    // Refocused: refused on the hidden editor and on a field under a hidden panel.
    ui_set_focus(s, field);
    ui_set_focus(s, ed);
    ui_set_hidden(holder, true);
    ui_set_focus(s, under);
    say_flag("hidden", "refocus", "refused", ui_focused(s) == field);

    // Held: the press on a button, and an editor's bar, let go when each is hidden, and the
    // button's hover and focus looks put at rest.
    ui_set_hidden(ed, false);
    editor_pass(ui);
    float x = 0.0f, y = 0.0f;
    centre_of(button, &x, &y);
    pointer_pass(ui, x, y, POINTER_PRESS, 0);
    ui_build(ui, 1280.0f, 720.0f, 1.0f);
    const UIElement *pressed = NULL, *bar = NULL, *seen = NULL;
    ui_system_held(ui, &pressed, &bar, &seen);
    const bool was_pressed = pressed == button && button->t_hover > 0.0f;
    ui_set_hidden(button, true);
    ui_system_held(ui, &pressed, &bar, &seen);
    const bool press_gone = pressed == NULL && button->t_hover == 0.0f && button->t_focus == 0.0f;
    pointer_pass(ui, x, y, POINTER_RELEASE, 0);
    const float bx = ed->rect.x + ed->rect.w - 5.0f, by = ed->rect.y + 10.0f;
    pointer_pass(ui, bx, by, POINTER_PRESS, 0);
    ui_system_held(ui, &pressed, &bar, &seen);
    const bool was_held = bar == ed;
    ui_set_hidden(ed, true);
    ui_system_held(ui, &pressed, &bar, &seen);
    pointer_pass(ui, bx, by + 20.0f, POINTER_RELEASE, 0);
    say_flag("hidden", "held", "let_go",
             was_pressed && press_gone && was_held && bar == NULL && g_presses == 0);

    // Moved under a hidden panel, the focused editor lets go of the focus.
    ui_set_hidden(ed, false);
    ui_set_focus(s, ed);
    ui_move(ed, holder, NULL);
    say_flag("hidden", "move", "let_go", ed->parent == holder && ui_focused(s) == NULL);
}

// Tab steps over a hidden button.
static void hidden_tab(UISystem* ui) {
    UIScreen* s = scene_alone(ui, "hidden-tab");
    UIElement* root = ui_screen_root(s);
    UIElement* b[3];
    for (int i = 0; i < 3; i++)
        b[i] = ui_button(root, "tab", NULL, NULL);
    ui_set_hidden(b[1], true);
    editor_pass(ui);
    ui_set_focus(s, b[0]);
    key(ui, GLFW_KEY_TAB, 0);
    say_flag("hidden", "tab", "over", ui_focused(s) == b[2]);
}

// Kept: an editor scrolled to line 50 with two carets, hidden five passes, laid out empty with its
// columns kept, and shown as it was and wrapped as it was; sized to nothing instead, it has one
// column.
static void hidden_kept(UISystem* ui, TextDoc* doc) {
    UIElement* ed =
        fixed_editor(ui_screen_root(scene_alone(ui, "hidden-kept")), doc, 300.0f, 150.0f);
    for (int i = 0; i < 10 && !ui_editor_settled(ed); i++)
        editor_pass(ui);
    const size_t l50 = text_doc_line_start(doc, 50), l51 = text_doc_line_start(doc, 51);
    const TextSelection carets[2] = {{l50, l50}, {l51 + 2, l51 + 5}};
    ui_editor_set_selections(ed, carets, 2);
    ui_editor_scroll_line(ed, 50, 0.0f);
    editor_pass(ui);
    const int cols = ui_editor_columns(ed);
    ui_set_hidden(ed, true);
    bool same_cols = true;
    for (int i = 0; i < 5; i++) {
        editor_pass(ui);
        same_cols =
            same_cols && ui_editor_columns(ed) == cols && ed->rect.w == 0.0f && ed->rect.h == 0.0f;
    }
    ui_set_hidden(ed, false);
    editor_pass(ui);
    float rows = 0.0f;
    const size_t top_line = ui_editor_top_line(ed, &rows);
    const TextSelection* now = NULL;
    const size_t n = ui_editor_selections(ed, &now, NULL);
    const bool carets_kept = n == 2 && now[0].anchor == l50 && now[0].head == l50 &&
                             now[1].anchor == l51 + 2 && now[1].head == l51 + 5;
    if (!same_cols || top_line != 50 || rows != 0.0f || !carets_kept || !ui_editor_settled(ed))
        fprintf(stderr, "hidden: kept %d columns of %d, top line %zu at %.2f, settled %d\n",
                ui_editor_columns(ed), cols, top_line, (double)rows, ui_editor_settled(ed));
    say_flag("hidden", "kept", "whole",
             cols > 1 && same_cols && top_line == 50 && rows == 0.0f && carets_kept &&
                 ui_editor_settled(ed));
    ui_set_size(ed, UI_FIXED, 0.0f, UI_FIXED, 0.0f);
    editor_pass(ui);
    say_flag("hidden", "sized", "one", ui_editor_columns(ed) == 1);
}

// Still: a pane hidden while its scroll eases asks for no frame, and keeps its scroll.
static void hidden_still(UISystem* ui) {
    UIElement* pane = ui_scroll(ui_screen_root(scene_alone(ui, "hidden-still")));
    ui_set_size(pane, UI_FIXED, 200.0f, UI_FIXED, 100.0f);
    UIElement* last = NULL;
    for (int i = 0; i < 40; i++)
        last = ui_label(pane, "a line");
    editor_pass(ui);
    ui_scroll_into_view(ui, last);
    const bool easing = ui_build(ui, 1280.0f, 720.0f, 0.001f);
    const float held_at = pane->scroll;
    ui_set_hidden(pane, true);
    const bool moving = ui_build(ui, 1280.0f, 720.0f, 0.001f);
    say_flag("hidden", "still", "nothing", easing && !moving && pane->scroll == held_at);
}

// Rows: a pane of virtual rows hidden five passes, laid out empty, builds and frees none, and
// shown has the same rows; its row and the screen's root are refused.
static void hidden_rows(UISystem* ui) {
    UIElement* root = ui_screen_root(scene_alone(ui, "hidden-rows"));
    UIElement* rows_pane = ui_scroll(root);
    ui_set_size(rows_pane, UI_FIXED, 200.0f, UI_FIXED, 100.0f);
    ui_scroll_set_rows(rows_pane, 50, 20.0f, build_row, NULL);
    editor_pass(ui);
    const unsigned builds = rows_pane->row_builds;
    const size_t built = rows_pane->child_count;
    UIElement* first = ui_scroll_find_row(rows_pane, 0);
    ui_set_hidden(rows_pane, true);
    bool none = true;
    for (int i = 0; i < 5; i++) {
        editor_pass(ui);
        none = none && rows_pane->row_builds == builds && rows_pane->child_count == built &&
               rows_pane->rect.h == 0.0f;
    }
    ui_set_hidden(rows_pane, false);
    editor_pass(ui);
    if (!none || rows_pane->row_builds != builds || rows_pane->child_count != built)
        fprintf(stderr, "hidden: %zu rows built in %u builds, %zu in %u shown, hidden kept %d\n",
                built, builds, rows_pane->child_count, rows_pane->row_builds, none);
    say_flag("hidden", "rows", "kept",
             built > 0 && none && rows_pane->row_builds == builds &&
                 rows_pane->child_count == built && ui_scroll_find_row(rows_pane, 0) == first);
    ui_set_hidden(root, true);
    ui_set_hidden(first, true);
    say_flag("hidden", "refused", "root_row", !root->hidden && first && !first->hidden);
}

// A hidden element takes no room and no gap, is neither laid out, drawn nor eased, takes no
// pointer, key or focus, lets go of what the UI held inside it, keeps all it holds, and stands
// where it stood once shown; a screen's root and a pane's row are refused. Sized to nothing
// instead, an editor wraps at one column.
static int probe_ui_hidden(UISystem* ui) {
    editor_theme(ui);
    TextDoc* doc = hidden_text(200);
    hidden_flow(ui);
    hidden_drawn(ui, doc);
    hidden_pointer(ui, doc);
    hidden_focus(ui, doc);
    hidden_tab(ui);
    hidden_kept(ui, doc);
    hidden_still(ui);
    hidden_rows(ui);
    free_text_doc(doc);
    return 0;
}

// ---- Two editors row for row (spec 14.19)

#define TOP_FILL_LINE  40
#define TOP_FILL_ROWS  3
#define TOP_FOLD_FIRST 60
#define TOP_FOLD_LAST  90

// The rows above `line` in the top-row case's editor, counted from the rows each line wraps to
// with its fillers and its closed fold by hand: what ui_editor_top_row is held to.
static double rows_above(const UIElement* ed, size_t line) {
    double rows = line > TOP_FILL_LINE ? TOP_FILL_ROWS : 0;
    for (size_t i = 0; i < line; i++)
        rows += i == TOP_FOLD_FIRST                        ? 1.0
                : i > TOP_FOLD_FIRST && i <= TOP_FOLD_LAST ? 0.0
                                                           : (double)ui_editor_line_rows(ed, i);
    return rows;
}

// Two sides of a comparison: line 4 wraps on the left, and the right pads its short line 4 out with
// fillers above line 5. The right leads from half a row into its line 5's own row, inside the rows
// the pair makes, where its line and the rows into it would put the left fillers' rows off; the
// left copies its top row, and lines 6 and 7 stand level.
static void top_row_level(UISystem* ui) {
    static const char* const four = "line 4 of a text whose lines wrap in a narrow editor, some "
                                    "of them twice over at the least";
    char left_text[512], right_text[256];
    size_t ln = 0, rn = 0;
    for (int i = 0; i < 10; i++) {
        ln += (size_t)snprintf(left_text + ln, sizeof(left_text) - ln, "%s\n",
                               i == 4 ? four : "line");
        rn += (size_t)snprintf(right_text + rn, sizeof(right_text) - rn, "line\n");
    }
    TextDoc* left_doc = create_text_doc(left_text, ln);
    TextDoc* right_doc = create_text_doc(right_text, rn);
    UIElement* root = ui_screen_root(scene_alone(ui, "top-row-level"));
    root->dir = UI_ROW;
    UIElement* left = fixed_editor(root, left_doc, 300.0f, 220.0f);
    UIElement* right = fixed_editor(root, right_doc, 300.0f, 220.0f);
    editor_pass(ui);
    const size_t wrapped = ui_editor_line_rows(left, 4);
    ui_editor_set_fillers(right, &(UIEditorFiller){5, (uint32_t)(wrapped - 1)}, 1);
    const size_t in_left[2] = {text_doc_line_start(left_doc, 7), text_doc_line_start(left_doc, 8)};
    const size_t in_right[2] = {text_doc_line_start(right_doc, 7),
                                text_doc_line_start(right_doc, 8)};
    // Half a row into line 5's own row, inside the pair, and into line 6, below its fillers.
    bool level = wrapped >= 2;
    const size_t lead_line[2] = {5, 6};
    const float lead_rows[2] = {(float)(wrapped - 1) + 0.5f, 0.5f};
    for (int i = 0; level && i < 2; i++) {
        ui_editor_scroll_line(right, lead_line[i], lead_rows[i]);
        ui_editor_scroll_row(left, ui_editor_top_row(right));
        editor_pass(ui);
        level = rows_level(left, right, in_left, in_right, 2);
        if (!level)
            fprintf(stderr,
                    "editor-top-row: from line %zu the left at row %.2f, the right at %.2f\n",
                    lead_line[i], ui_editor_top_row(left), ui_editor_top_row(right));
    }
    say_flag("editor-top-row", "level", "lines", level);
    free_text_doc(left_doc);
    free_text_doc(right_doc);
}

// The view's top as rows from the text's start: 0 at the start; a line's top the rows above it,
// fillers and a closed fold's one row counted, and the part; put back as read, on a filler, on the
// fold's row, on a wrapped line's second row and with a part; held to the first row and the last as
// a wheel is; asked before a layout, taken at it; and two editors, one line wrapping to more rows
// than its counterpart and fillers on the other side making them up, put at the same row, drawing
// the lines after the pair level.
static int probe_editor_top_row(UISystem* ui) {
    static char text[200 * 96];
    size_t len = 0;
    for (int i = 0; i < 200; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "line %03d%s\n", i,
                                i % 4 == 1 ? " of a text whose lines wrap in a narrow editor, "
                                             "some of them twice over at the least"
                                           : "");
    TextDoc* doc = create_text_doc(text, len - 1);
    UIElement* ed = editor_fixture(ui, doc, 300.0f, 220.0f);
    ui_editor_set_fillers(ed, &(UIEditorFiller){TOP_FILL_LINE, TOP_FILL_ROWS}, 1);
    ui_editor_set_folds(ed, &(UIEditorFold){TOP_FOLD_FIRST, TOP_FOLD_LAST, false, NULL}, 1);
    for (int i = 0; i < 10 && !ui_editor_settled(ed); i++)
        editor_pass(ui);
    say_flag("editor-top-row", "start", "zero", ui_editor_top_row(ed) == 0.0);

    // Line 100, below the fillers and the fold, half a row in.
    ui_editor_scroll_line(ed, 100, 0.5f);
    const double at100 = ui_editor_top_row(ed), want100 = rows_above(ed, 100) + 0.5;
    if (at100 != want100)
        fprintf(stderr, "editor-top-row: line 100 at row %.2f, the rows above it %.2f\n", at100,
                want100);
    say_flag("editor-top-row", "count", "above",
             ui_editor_line_rows(ed, 45) >= 2 && at100 == want100);

    // Put and read back: a filler's row, the fold's row, line 45's second row, and that with a
    // part.
    const double trips[4] = {rows_above(ed, TOP_FILL_LINE) + 1.0, rows_above(ed, TOP_FOLD_FIRST),
                             rows_above(ed, 45) + 1.0, rows_above(ed, 45) + 1.25};
    bool trip = true;
    for (int i = 0; i < 4; i++) {
        ui_editor_scroll_row(ed, trips[i]);
        const double back = ui_editor_top_row(ed);
        if (back != trips[i])
            fprintf(stderr, "editor-top-row: put at %.2f, read back %.2f\n", trips[i], back);
        float into = -1.0f;
        trip = trip && back == trips[i] &&
               (i != 1 || (ui_editor_top_line(ed, &into) == TOP_FOLD_FIRST && into == 0.0f));
    }
    say_flag("editor-top-row", "trip", "same", trip);

    // Past the last row, where a wheel run to the end stops; below 0, at 0.
    ui_editor_scroll_row(ed, 1e9);
    const double end = ui_editor_top_row(ed);
    ui_editor_scroll_row(ed, 0.0);
    editor_pass(ui);
    wheel_rows(ui, ed, 1e6f);
    const double wheeled = ui_editor_top_row(ed);
    ui_editor_scroll_row(ed, -5.0);
    say_flag("editor-top-row", "held", "wheel",
             end == rows_above(ed, 200) - 1.0 && wheeled == end && ui_editor_top_row(ed) == 0.0);

    // Asked of a second editor before it was laid out, taken at its first layout.
    UIElement* late = fixed_editor(ui_screen_root(ui_top(ui)), doc, 300.0f, 220.0f);
    ui_editor_scroll_row(late, 10.5);
    const double before = ui_editor_top_row(late);
    editor_pass(ui);
    say_flag("editor-top-row", "pending", "taken",
             before == 0.0 && ui_editor_top_row(late) == 10.5);
    free_text_doc(doc);
    top_row_level(ui);
    return 0;
}

// The x of line 1's number from the editor's left edge, the first glyph left of the text.
static float number_x(const UISystem* ui, UIElement* ed) {
    float tx = 0.0f, y = 0.0f, x = 0.0f, y1 = 0.0f;
    ui_editor_point_of(ed, 0, &tx, &y);
    ui_editor_point_of(ed, text_doc_line_start(ui_editor_doc(ed), 1), &x, &y1);
    Glyph g[1];
    return row_glyphs(ui, ed->rect.x, tx, y, y1 - y, g, 1) ? g[0].r.x - ed->rect.x : NAN;
}

// The gutter as wide as the digits asked: a text of 9 lines asked for 4 has the columns of one of
// 1000 in the same width, its numbers right-aligned in the wider gutter; 0, or fewer than the
// text's own, gives the text's own; and a change makes the rows guesses the budget wraps again.
static int probe_editor_gutter_digits(UISystem* ui) {
    TextDoc* nine = create_text_doc("1\n2\n3\n4\n5\n6\n7\n8\n9", 17);
    static char lots[1000 * 2];
    for (int i = 0; i < 1000; i++)
        memcpy(lots + i * 2, "x\n", 2);
    TextDoc* thousand = create_text_doc(lots, sizeof(lots) - 1);
    UIElement* ed = editor_fixture(ui, nine, 300.0f, 150.0f);
    UIElement* wide = fixed_editor(ui_screen_root(ui_top(ui)), thousand, 300.0f, 150.0f);
    editor_pass(ui);
    const int own = ui_editor_columns(ed);
    const float own_x = number_x(ui, ed);
    ui_editor_set_gutter_digits(ed, 4);
    editor_pass(ui);
    say_flag("editor-gutter-digits", "columns", "wide",
             ui_editor_columns(ed) == ui_editor_columns(wide) && ui_editor_columns(ed) < own);
    const float x = number_x(ui, ed), wide_x = number_x(ui, wide);
    if (fabsf(x - wide_x) >= 0.01f)
        fprintf(stderr, "editor-gutter-digits: line 1's number at %.2f, beside 1000 lines' %.2f\n",
                (double)x, (double)wide_x);
    say_flag("editor-gutter-digits", "numbers", "right",
             fabsf(x - wide_x) < 0.01f && x > own_x + 0.01f);
    ui_editor_set_gutter_digits(ed, 0);
    editor_pass(ui);
    const bool zero = ui_editor_columns(ed) == own;
    ui_editor_set_gutter_digits(ed, 2);
    editor_pass(ui);
    say_flag("editor-gutter-digits", "own", "kept", zero && ui_editor_columns(ed) == own);

    // A text too long for one frame's budget: settled, then asked for 7 digits, guessed again.
    static char big[4000 * 100];
    size_t len = 0;
    for (int i = 0; i < 4000; i++)
        len += (size_t)snprintf(big + len, sizeof(big) - len,
                                "line %04d of a long text whose lines wrap in a narrow editor, "
                                "more than once over\n",
                                i);
    TextDoc* long_doc = create_text_doc(big, len);
    UIElement* far = fixed_editor(ui_screen_root(ui_top(ui)), long_doc, 300.0f, 150.0f);
    for (int i = 0; i < 20 && !ui_editor_settled(far); i++)
        editor_pass(ui);
    const bool settled = ui_editor_settled(far);
    const int cols = ui_editor_columns(far);
    ui_editor_set_gutter_digits(far, 7);
    editor_pass(ui);
    const bool guessed = !ui_editor_settled(far) && ui_editor_columns(far) < cols;
    for (int i = 0; i < 20 && !ui_editor_settled(far); i++)
        editor_pass(ui);
    say_flag("editor-gutter-digits", "wrapped", "again",
             settled && guessed && ui_editor_settled(far));
    free_text_doc(nine);
    free_text_doc(thousand);
    free_text_doc(long_doc);
    return 0;
}

// Fillers under the text with a closed fold ending it, which counts them: the rows summed again
// when an edit or a new set of fillers moves the line counting them; a fold closed over the top
// putting it on the fold's row, not on a filler under it; and Home on a wrapped row of the fold's
// first line staying on that line.
static bool filler_end_fold(UISystem* ui) {
    char text[1024];
    size_t len = 0;
    for (int i = 0; i < 20; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "line %02d%s\n", i,
                                i == 10 ? " of a text long enough to wrap in an editor six hundred "
                                          "points across, once over at the least, and twice more"
                                        : "");
    TextDoc* doc = create_text_doc(text, len - 1);
    UIElement* ed =
        fixed_editor(ui_screen_root(scene_alone(ui, "filler-end-fold")), doc, 600.0f, 300.0f);
    editor_pass(ui);
    // Lines 10 to 18 closed, line 19 the last, 3 under it: rows 0 to 14. Line 19 taken out, the
    // fold ends the text and counts them: rows 0 to 13.
    ui_editor_set_folds(ed, &(UIEditorFold){10, 18, false, NULL}, 1);
    ui_editor_set_fillers(ed, &(UIEditorFiller){20, 3}, 1);
    editor_pass(ui);
    ui_editor_scroll_row(ed, 1e9);
    const double before_edit = ui_editor_top_row(ed);
    text_doc_replace(doc, text_doc_line_start(doc, 19) - 1, text_doc_length(doc), "", 0);
    ui_editor_scroll_row(ed, 1e9);
    const double after_edit = ui_editor_top_row(ed);
    // Two on a line the fold hides, the sums put right from the fold by setting it again and read,
    // then those under the text put back beside them: rows 0 to 13. (A sum left too long would not
    // show here: the top held to the fold's row reads right.)
    ui_editor_set_fillers(ed, &(UIEditorFiller){15, 2}, 1);
    ui_editor_set_folds(ed, &(UIEditorFold){10, 18, false, NULL}, 1);
    ui_editor_scroll_row(ed, 1e9);
    ui_editor_set_fillers(ed, (const UIEditorFiller[]){{15, 2}, {19, 3}}, 2);
    ui_editor_scroll_row(ed, 1e9);
    const double after_set = ui_editor_top_row(ed);
    // Opened, the top on the first line's second row, closed again: the top on the fold's row.
    ui_editor_set_fillers(ed, &(UIEditorFiller){19, 3}, 1);
    ui_editor_set_fold_open(ed, 10, true);
    editor_pass(ui);
    ui_editor_scroll_line(ed, 10, 1.0f);
    ui_editor_set_fold_open(ed, 10, false);
    editor_pass(ui);
    float into = -1.0f;
    const size_t top = ui_editor_top_line(ed, &into);
    // Home from the second row line 10 wraps to, the fold closed over it.
    size_t starts[8], len10 = 0;
    int hang = 0;
    const char* s = text_doc_line(doc, 10, &len10);
    const size_t rows = ui_editor_wrap(s, len10, ui_editor_columns(ed), 4, starts, 8, &hang);
    const size_t row1 = text_doc_line_start(doc, 10) + (rows >= 2 ? starts[1] : 0);
    ui_editor_set_selections(ed, &(TextSelection){row1 + 2, row1 + 2}, 1);
    ui_editor_apply(ed, UI_OP_MOVE, UI_MOTION_ROW_START, 1);
    const bool home = rows >= 2 && one_selection(ed, row1, row1);
    const bool ok = before_edit == 14.0 && after_edit == 13.0 && after_set == 13.0 && top == 10 &&
                    into == 0.0f && home;
    if (!ok)
        fprintf(stderr,
                "editor-filler-end: the end at %.2f, after the edit %.2f, after the set %.2f; "
                "the top at line %zu %.2f in; Home %d\n",
                before_edit, after_edit, after_set, top, (double)into, home);
    free_text_doc(doc);
    return ok;
}

// Fillers under the last line, of a text with no last line ending: drawn after it with no number,
// counted in its rows to the scroll's end, where no caret stands, a click on them landing at the
// text's end; under the new last line once the last is taken out; and one past the line count left
// out.
static int probe_editor_filler_end(UISystem* ui) {
    char text[256];
    size_t len = 0;
    for (int i = 0; i < 20; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "line %02d\n", i);
    TextDoc* doc = create_text_doc(text, len - 1);
    UIEditorVim* vim = create_ui_editor_vim();
    UIElement* ed = vim_fixture(ui, doc, vim, 0);
    vec4 filler = {0.0f, 0.6f, 0.6f, 1.0f};
    const UITheme before = filler_theme(ui, filler);

    // Past the line count: left out, so the last row is line 19's.
    ui_editor_set_fillers(ed, &(UIEditorFiller){21, 1}, 1);
    ui_editor_scroll_row(ed, 1000.0);
    editor_pass(ui);
    const bool past = ui_editor_top_row(ed) == 19.0 && fills_in(ui, filler, NULL, 0) == 0;

    // Two under line 19, seen from line 10: two filler fills after it, nothing on their rows.
    ui_editor_set_fillers(ed, &(UIEditorFiller){20, 2}, 1);
    ui_editor_scroll_line(ed, 10, 0.0f);
    editor_pass(ui);
    float tx = 0.0f, y18 = 0.0f, y19 = 0.0f, x = 0.0f;
    ui_editor_point_of(ed, text_doc_line_start(doc, 18), &tx, &y18);
    ui_editor_point_of(ed, text_doc_line_start(doc, 19), &x, &y19);
    const float row = y19 - y18;
    Fill fills[4];
    const size_t n = fills_in(ui, filler, fills, 4);
    bool under = n == 2;
    for (size_t i = 0; under && i < 2; i++)
        under = fabsf(fills[i].r.y - (y19 + (float)(i + 1) * row)) < 0.01f &&
                fabsf(fills[i].r.x - tx) < 0.01f &&
                row_glyphs(ui, -INFINITY, INFINITY, fills[i].r.y, row, NULL, 0) == 0;
    if (!under)
        fprintf(stderr, "editor-filler-end: %zu filler fills, line 19 at %.2f\n", n, (double)y19);
    say_flag("editor-filler-end", "under", "drawn", under);

    // Counted to the scroll's end, by a row put past it and by a wheel run there, in an editor too
    // short to show every row.
    const float click_y = y19 + row * 1.5f;
    ui_set_size(ed, UI_FIXED, 600.0f, UI_FIXED, 120.0f);
    editor_pass(ui);
    ui_editor_scroll_row(ed, 1000.0);
    const double end = ui_editor_top_row(ed);
    ui_editor_scroll_row(ed, 0.0);
    editor_pass(ui);
    wheel_rows(ui, ed, 1000.0f);
    editor_pass(ui);
    if (end != 21.0 || ui_editor_top_row(ed) != 21.0 || fills_in(ui, filler, NULL, 0) != 1)
        fprintf(stderr, "editor-filler-end: the end at row %.2f, wheeled to %.2f, %zu fills\n", end,
                ui_editor_top_row(ed), fills_in(ui, filler, NULL, 0));
    say_flag("editor-filler-end", "rows", "counted",
             end == 21.0 && ui_editor_top_row(ed) == 21.0 && fills_in(ui, filler, NULL, 0) == 1);

    // G and a j past it stand on the last line; two rows down from line 18 stop at the text's end,
    // as with no fillers, not on line 19 short of it; a click under it is at the text's end.
    ui_set_size(ed, UI_FIXED, 600.0f, UI_FIXED, 300.0f);
    vim_keys(ui, "Gj");
    const TextSelection* sel = NULL;
    ui_editor_selections(ed, &sel, NULL);
    const bool on_last = text_doc_line_of(doc, sel[0].head) == 19;
    const size_t at18 = text_doc_line_start(doc, 18) + 2;
    ui_editor_set_selections(ed, &(TextSelection){at18, at18}, 1);
    ui_editor_apply(ed, UI_OP_MOVE, UI_MOTION_ROW_DOWN, 2);
    const bool stopped = one_selection(ed, text_doc_length(doc), text_doc_length(doc));
    ui_editor_scroll_line(ed, 10, 0.0f);
    editor_pass(ui);
    say_flag("editor-filler-end", "caret", "last_line",
             on_last && stopped &&
                 ui_editor_offset_at(ed, tx + 20.0f, click_y) == text_doc_length(doc));

    // Line 19 taken out with the line ending before it: the fillers under line 18.
    text_doc_replace(doc, text_doc_line_start(doc, 19) - 1, text_doc_length(doc), "", 0);
    ui_editor_scroll_line(ed, 10, 0.0f);
    editor_pass(ui);
    const size_t moved_n = fills_in(ui, filler, fills, 4);
    ui_editor_scroll_row(ed, 1000.0);
    say_flag("editor-filler-end", "moved", "under",
             doc->line_count == 19 && moved_n == 2 && fabsf(fills[0].r.y - (y18 + row)) < 0.01f &&
                 ui_editor_top_row(ed) == 20.0);
    say_flag("editor-filler-end", "past", "left_out", past);
    say_flag("editor-filler-end", "fold", "end", filler_end_fold(ui));
    ui_set_theme(ui, &before);
    free_ui_editor_vim(vim);
    free_text_doc(doc);
    return 0;
}

// ---- The rows kept, the fillers read back, exact rows and the scrolls counted (spec 14.20)

// One layout of the top screen and no draw, so nothing is wrapped but what the layout asks for.
static void layout_only(UISystem* ui) {
    ui_layout(ui_top(ui), 1280.0f, 720.0f);
}

// The rows `line` of the editor's text wraps to at its columns and `tab` stops, wrapped here.
static size_t wrapped_rows(const UIElement* ed, size_t line, int tab) {
    size_t len = 0, starts[64];
    int hang = 0;
    const char* s = text_doc_line(ui_editor_doc(ed), line, &len);
    return ui_editor_wrap(s, len, ui_editor_columns(ed), tab, starts, 64, &hang);
}

// 200 lines, every fifth from the second long enough to wrap in 300 points, every fifth from the
// fourth of tabs, which wrap apart at 4 stops and at 8.
static TextDoc* kept_text(void) {
    static const char TABS[] = "\tx\tx\tx\tx\tx\tx\tx\tx\tx\tx\tx\tx\tx\tx\tx\tx";
    static char text[200 * 128];
    size_t len = 0;
    for (int i = 0; i < 200; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "line %03d%s\n", i,
                                i % 5 == 1   ? " of a text whose lines wrap in a narrow editor, "
                                               "some of them twice over"
                                : i % 5 == 3 ? TABS
                                             : "");
    return create_text_doc(text, len - 1);
}

// A line's rows answered from the count the editor keeps: settled, every line asked twice wraps
// none; on a text just shown, the first ask of a line wraps it and the second does not; at new
// columns, and at new tab stops, a line is wrapped again and gives its new count.
static int probe_editor_rows_kept(UISystem* ui) {
    TextDoc* doc = kept_text();
    TextDoc* other = kept_text();
    UIElement* ed = editor_fixture(ui, doc, 300.0f, 220.0f);
    for (int i = 0; i < 10 && !ui_editor_settled(ed); i++)
        editor_pass(ui);
    const uint64_t settled = ui_editor_wraps(ed);
    size_t total = 0;
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < doc->line_count; i++)
            total += ui_editor_line_rows(ed, i);
    if (ui_editor_wraps(ed) != settled)
        fprintf(stderr, "editor-rows-kept: settled, %llu lines wrapped by asking\n",
                (unsigned long long)(ui_editor_wraps(ed) - settled));
    say_flag("editor-rows-kept", "kept", "none",
             ui_editor_settled(ed) && total > 2 * doc->line_count &&
                 ui_editor_wraps(ed) == settled);
    // Laid out on a text just shown, with no draw: line 151 still a guess.
    ui_editor_set_doc(ed, other);
    layout_only(ui);
    const uint64_t shown = ui_editor_wraps(ed);
    const size_t first = ui_editor_line_rows(ed, 151);
    const uint64_t once = ui_editor_wraps(ed);
    const size_t second = ui_editor_line_rows(ed, 151);
    say_flag("editor-rows-kept", "guess", "once",
             once == shown + 1 && ui_editor_wraps(ed) == once && first == second &&
                 first == wrapped_rows(ed, 151, 4));
    // Back on the settled text, then narrower.
    ui_editor_set_doc(ed, doc);
    layout_only(ui);
    const size_t wide = ui_editor_line_rows(ed, 146);
    ui_set_size(ed, UI_FIXED, 240.0f, UI_FIXED, 220.0f);
    layout_only(ui);
    const uint64_t narrowed = ui_editor_wraps(ed);
    const size_t narrow = ui_editor_line_rows(ed, 146);
    say_flag("editor-rows-kept", "columns", "again",
             ui_editor_wraps(ed) == narrowed + 1 && narrow == wrapped_rows(ed, 146, 4) &&
                 narrow > wide);
    // Settled there, then 8 columns between tab stops.
    for (int i = 0; i < 10 && !ui_editor_settled(ed); i++)
        editor_pass(ui);
    const bool before = ui_editor_settled(ed);
    doc->tab_width = 8;
    layout_only(ui);
    const bool guessed = !ui_editor_settled(ed);
    const size_t tabbed = ui_editor_line_rows(ed, 148);
    if (tabbed != wrapped_rows(ed, 148, 8))
        fprintf(stderr, "editor-rows-kept: line 148 at 8 stops %zu rows, wraps to %zu\n", tabbed,
                wrapped_rows(ed, 148, 8));
    say_flag("editor-rows-kept", "tab", "again",
             before && guessed && tabbed == wrapped_rows(ed, 148, 8) &&
                 tabbed != wrapped_rows(ed, 148, 4));
    free_text_doc(doc);
    free_text_doc(other);
    return 0;
}

// The fillers the editor holds are `want`, line for line; none read back as NULL.
static bool fillers_are(const UIElement* ed, const UIEditorFiller* want, size_t n) {
    const UIEditorFiller* f = NULL;
    const size_t count = ui_editor_fillers(ed, &f);
    bool same = count == n && (n || !f);
    for (size_t i = 0; same && i < n; i++)
        same = f[i].line == want[i].line && f[i].rows == want[i].rows;
    if (!same) {
        fprintf(stderr, "  %zu fillers, want %zu:", count, n);
        for (size_t i = 0; i < count; i++)
            fprintf(stderr, " %zu+%u", f[i].line, (unsigned)f[i].rows);
        fprintf(stderr, "\n");
    }
    return same;
}

// The fillers read back as they stand: sorted, the last given for a line, none of no rows; moved
// with their lines; one on a line pasted over with the same text gone; one under the text last;
// and none, on an editor without and on an element that is not one.
static int probe_editor_fillers_read(UISystem* ui) {
    static char text[40 * 16];
    size_t len = 0;
    for (int i = 0; i < 40; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "line %02d\n", i);
    TextDoc* doc = create_text_doc(text, len - 1);
    UIElement* ed = editor_fixture(ui, doc, 300.0f, 220.0f);
    editor_pass(ui);
    const UIEditorFiller given[5] = {{30, 2}, {10, 1}, {20, 0}, {30, 3}, {5, 4}};
    ui_editor_set_fillers(ed, given, 5);
    say_flag("editor-fillers-read", "given", "sorted",
             fillers_are(ed, (const UIEditorFiller[]){{5, 4}, {10, 1}, {30, 3}}, 3));
    text_doc_replace(doc, 0, 0, "new\nnew\n", 8);
    say_flag("editor-fillers-read", "moved", "lines",
             fillers_are(ed, (const UIEditorFiller[]){{7, 4}, {12, 1}, {32, 3}}, 3));
    // Line 12, its line ending with it, pasted over with the same text.
    const size_t at = text_doc_line_start(doc, 12), end = text_doc_line_start(doc, 13);
    char same[16];
    text_doc_copy(doc, at, end, same);
    text_doc_replace(doc, at, end, same, end - at);
    say_flag("editor-fillers-read", "rewritten", "gone",
             fillers_are(ed, (const UIEditorFiller[]){{7, 4}, {32, 3}}, 2));
    const size_t lines = doc->line_count;
    ui_editor_set_fillers(ed, (const UIEditorFiller[]){{lines, 2}, {3, 1}}, 2);
    say_flag("editor-fillers-read", "under", "last",
             fillers_are(ed, (const UIEditorFiller[]){{3, 1}, {lines, 2}}, 2));
    ui_editor_set_fillers(ed, NULL, 0);
    const UIEditorFiller* none = given;
    const bool panel = ui_editor_fillers(ui_panel(ui_screen_root(ui_top(ui))), &none) == 0 && !none;
    say_flag("editor-fillers-read", "none", "zero", fillers_are(ed, NULL, 0) && panel);
    free_text_doc(doc);
    return 0;
}

// 600 lines of six words of 24 letters: each word takes a row of its own in 300 points, 41
// columns, so a line's guess from its length, 4 rows, is fewer than its wrap's 6.
static TextDoc* word_rows_text(void) {
    static const char LETTERS[] = "abcdefghijklmnopqrstu";
    static char text[600 * 160];
    size_t len = 0;
    for (int i = 0; i < 600; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len,
                                "%03d%s %03d%s %03d%s %03d%s %03d%s %03d%s\n", i, LETTERS, i,
                                LETTERS, i, LETTERS, i, LETTERS, i, LETTERS, i, LETTERS);
    return create_text_doc(text, len - 1);
}

// An editor on `doc` beside the others, laid out and not drawn: its rows guesses but its top's.
static UIElement* fresh_editor(UISystem* ui, TextDoc* doc) {
    UIElement* ed = fixed_editor(ui_screen_root(ui_top(ui)), doc, 300.0f, 220.0f);
    layout_only(ui);
    return ed;
}

// Exact rows: an editor on a text just shown counts the rows above its top, and above a row it is
// put at, as one settled on the text does, wrapping what it guessed; the bar dragged through it
// wraps none; a closed fold's lines stay guesses; and a second ask wraps nothing.
static int probe_editor_exact_rows(UISystem* ui) {
    TextDoc* doc = word_rows_text();
    editor_theme(ui);
    UIElement* root = ui_screen_root(scene_alone(ui, "exact-rows"));
    root->dir = UI_ROW;
    UIElement* settled = fixed_editor(root, doc, 300.0f, 220.0f);
    for (int i = 0; i < 10 && !ui_editor_settled(settled); i++)
        editor_pass(ui);

    UIElement* fresh = fresh_editor(ui, doc);
    ui_editor_scroll_line(settled, 300, 0.0f);
    ui_editor_scroll_line(fresh, 300, 0.0f);
    const double want = ui_editor_top_row(settled), got = ui_editor_top_row(fresh);
    if (got != want || want != 1800.0)
        fprintf(stderr, "editor-exact-rows: line 300 at row %.2f just shown, %.2f settled\n", got,
                want);
    say_flag("editor-exact-rows", "top", "same",
             ui_editor_line_rows(settled, 0) == 6 && want == 1800.0 && got == want);

    UIElement* other = fresh_editor(ui, doc);
    ui_editor_scroll_line(settled, 400, 2.5f);
    const double at = ui_editor_top_row(settled);
    ui_editor_scroll_row(other, at);
    float rows = 0.0f, other_rows = 0.0f;
    const size_t line = ui_editor_top_line(settled, &rows);
    const size_t other_line = ui_editor_top_line(other, &other_rows);
    say_flag("editor-exact-rows", "scroll", "same",
             line == 400 && other_line == line && other_rows == rows);

    // The thumb pressed at the top of its track and held half way down, with no draw.
    UIElement* dragged = fresh_editor(ui, doc);
    const uint64_t before = ui_editor_wraps(dragged);
    const float bx = dragged->rect.x + dragged->rect.w - UI_BAR_INSET - UI_BAR_W * 0.5f;
    const float mid_y = dragged->rect.y + dragged->rect.h * 0.5f;
    pointer_pass(ui, bx, dragged->rect.y + UI_BAR_INSET + 1.0f, POINTER_PRESS, 0);
    pointer_pass(ui, bx, mid_y, POINTER_HOLD, 0);
    pointer_pass(ui, bx, mid_y, POINTER_RELEASE, 0);
    float into = 0.0f;
    const size_t mid = ui_editor_top_line(dragged, &into);
    const uint64_t dragged_wraps = ui_editor_wraps(dragged) - before;
    if (mid < 150 || dragged_wraps > 8)
        fprintf(stderr, "editor-exact-rows: dragged to line %zu, %llu lines wrapped\n", mid,
                (unsigned long long)dragged_wraps);
    say_flag("editor-exact-rows", "bar", "guessed", mid >= 150 && dragged_wraps <= 8);

    // Lines 100 to 199 closed: lines 0 to 99 and 200 to 299 wrapped, the fold's one row between.
    UIElement* folded = fresh_editor(ui, doc);
    ui_editor_set_folds(folded, &(UIEditorFold){100, 199, false, NULL}, 1);
    ui_editor_scroll_line(folded, 300, 0.0f);
    const uint64_t unfolded = ui_editor_wraps(folded);
    const double above = ui_editor_top_row(folded);
    const uint64_t folded_wraps = ui_editor_wraps(folded) - unfolded;
    if (above != 1201.0 || folded_wraps > 200)
        fprintf(stderr, "editor-exact-rows: folded, line 300 at row %.2f, %llu lines wrapped\n",
                above, (unsigned long long)folded_wraps);
    say_flag("editor-exact-rows", "folded", "guessed", above == 1201.0 && folded_wraps <= 200);

    const uint64_t asked = ui_editor_wraps(fresh) + ui_editor_wraps(other);
    const double again = ui_editor_top_row(fresh);
    ui_editor_scroll_row(other, at);
    say_flag("editor-exact-rows", "again", "none",
             again == want && ui_editor_wraps(fresh) + ui_editor_wraps(other) == asked);
    free_text_doc(doc);
    return 0;
}

// Whether the editor counts one scroll more than `*n`, which then counts it.
static bool one_more(const UIElement* ed, uint64_t* n) {
    return ui_editor_scrolls(ed) == ++*n;
}

// The scrolls asked of the view counted: one for each of the four calls, a wheel's step, each pass
// of the bar pressed or held and a key moving the caret; one asked before a layout counted when
// asked and not when taken; an edit above the view, a fold closed over the top and opened, a
// layout at another width and the budget settling none.
static int probe_editor_scrolls(UISystem* ui) {
    static char text[200 * 16];
    size_t len = 0;
    for (int i = 0; i < 200; i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "line %03d\n", i);
    TextDoc* doc = create_text_doc(text, len - 1);
    UIElement* ed = editor_fixture(ui, doc, 300.0f, 150.0f);
    editor_pass(ui);

    uint64_t n = ui_editor_scrolls(ed);
    ui_editor_reveal(ed, text_doc_line_start(doc, 100));
    bool asked = one_more(ed, &n);
    ui_editor_scroll_place(ed, text_doc_line_start(doc, 50), 0.5f);
    asked = one_more(ed, &n) && asked;
    ui_editor_scroll_line(ed, 20, 0.0f);
    asked = one_more(ed, &n) && asked;
    ui_editor_scroll_row(ed, 0.0);
    asked = one_more(ed, &n) && asked;
    say_flag("editor-scrolls", "asked", "one", asked);

    // From the top, so the thumb stands at the top of its track.
    editor_pass(ui);
    wheel_rows(ui, ed, 3.0f);
    bool own = one_more(ed, &n);
    ui_editor_scroll_row(ed, 0.0);
    n = ui_editor_scrolls(ed);
    editor_pass(ui);
    const float bx = ed->rect.x + ed->rect.w - UI_BAR_INSET - UI_BAR_W * 0.5f;
    pointer_pass(ui, bx, ed->rect.y + UI_BAR_INSET + 1.0f, POINTER_PRESS, 0);
    own = one_more(ed, &n) && own;
    pointer_pass(ui, bx, ed->rect.y + ed->rect.h * 0.5f, POINTER_HOLD, 0);
    own = one_more(ed, &n) && own;
    pointer_pass(ui, bx, ed->rect.y + ed->rect.h * 0.5f, POINTER_RELEASE, 0);
    own = ui_editor_scrolls(ed) == n && own;
    key(ui, GLFW_KEY_DOWN, 0);
    own = one_more(ed, &n) && own;
    say_flag("editor-scrolls", "own", "one", own);

    UIElement* late = fixed_editor(ui_screen_root(ui_top(ui)), doc, 300.0f, 150.0f);
    ui_editor_scroll_line(late, 30, 0.0f);
    const bool when_asked = ui_editor_scrolls(late) == 1;
    editor_pass(ui);
    float rows = -1.0f;
    say_flag("editor-scrolls", "pending", "once",
             when_asked && ui_editor_scrolls(late) == 1 && ui_editor_top_line(late, &rows) == 30);

    ui_editor_scroll_line(ed, 100, 0.0f);
    editor_pass(ui);
    n = ui_editor_scrolls(ed);
    text_doc_replace(doc, text_doc_line_start(doc, 10), text_doc_line_start(doc, 10), "new\n", 4);
    ui_editor_set_folds(ed, &(UIEditorFold){95, 110, false, NULL}, 1);
    editor_pass(ui);
    ui_editor_set_fold_open(ed, 95, true);
    ui_set_size(ed, UI_FIXED, 260.0f, UI_FIXED, 150.0f);
    for (int i = 0; i < 10 && !ui_editor_settled(ed); i++)
        editor_pass(ui);
    const size_t top = ui_editor_top_line(ed, &rows);
    if (ui_editor_scrolls(ed) != n)
        fprintf(stderr, "editor-scrolls: %llu scrolls counted with none asked, the top at %zu\n",
                (unsigned long long)(ui_editor_scrolls(ed) - n), top);
    say_flag("editor-scrolls", "still", "none", ui_editor_scrolls(ed) == n && top == 95);
    free_text_doc(doc);
    return 0;
}

// ---- An editor shown another text (spec 14.6)

static int probe_editor_set_doc(UISystem* ui) {
    size_t len = 0;
    char* text = move_text(&len);
    TextDoc* a = create_text_doc(text, len);
    free(text);
    TextDoc* b = create_text_doc("other\ntext\n", 11);
    UIElement* ed = editor_fixture(ui, a, 300.0f, 120.0f);
    UIElement* twin = ui_editor(ui_screen_root(ui_top(ui)), a);
    ui_set_size(twin, UI_FIXED, 300.0f, UI_FIXED, 120.0f);
    editor_pass(ui);
    ui_editor_set_selections(twin, &(TextSelection){10, 10}, 1);

    // Where the editor is in the first text, to come back to.
    const TextSelection carets[2] = {{24, 24}, {40, 44}};
    ui_editor_set_selections(ed, carets, 2);
    ui_editor_scroll_place(ed, 100, 0.0f);
    editor_pass(ui);
    const size_t top = ui_editor_top(ed);

    // A text it has not shown: one caret at its start, the view at its top.
    ui_editor_set_doc(ed, b);
    editor_pass(ui);
    say_flag("set-doc", "start", "fresh", one_selection(ed, 0, 0) && ui_editor_top(ed) == 0);
    // The first text no longer moves its carets -- one at 3 in the second stays there through an
    // edit at the first's start -- while the second editor on it keeps its own, moved.
    ui_editor_set_selections(ed, &(TextSelection){3, 3}, 1);
    text_doc_replace(a, 0, 0, "xx", 2);
    say_flag("set-doc", "old", "forgotten", one_selection(ed, 3, 3));
    say_flag("set-doc", "twin", "kept", one_selection(twin, 12, 12));
    type(ui, "q");
    say_flag("set-doc", "typing", "new",
             doc_is_str(b, "othqer\ntext\n") && text_doc_length(a) == len + 2);
    // The text it shows already changes nothing.
    ui_editor_set_doc(ed, b);
    say_flag("set-doc", "same", "kept", one_selection(ed, 4, 4));
    // Nothing, then back to the first text: where it was there, brought through the edit made
    // there while it showed another, as a vim window comes back to a buffer.
    ui_editor_set_doc(ed, NULL);
    say_flag("set-doc", "null", "nothing", ui_editor_selections(ed, NULL, NULL) == 0);
    ui_editor_set_doc(ed, a);
    editor_pass(ui);
    const TextSelection* now = NULL;
    const size_t n = ui_editor_selections(ed, &now, NULL);
    say_flag("set-doc", "back", "where",
             top == 94 && ui_editor_top(ed) == top + 2 && n == 2 && now[0].head == 26 &&
                 now[1].anchor == 42 && now[1].head == 46);
    // A new editor placed before its first layout, as a split shows the view it split: placed at
    // that layout, once its rows and its height are known -- half way down its view, as the laid
    // out editor of that size places it, where before the layout the view has no rows to go down.
    UIElement* fresh = ui_editor(ui_screen_root(ui_top(ui)), a);
    ui_set_size(fresh, UI_FIXED, 300.0f, UI_FIXED, 120.0f);
    ui_editor_set_selections(fresh, &(TextSelection){100, 100}, 1);
    ui_editor_scroll_place(fresh, top + 2, 0.5f);
    ui_editor_scroll_place(ed, top + 2, 0.5f);
    editor_pass(ui);
    const size_t placed = ui_editor_top(ed);
    say_flag("set-doc", "fresh", "placed",
             placed < top + 2 && ui_editor_top(fresh) == placed && one_selection(fresh, 100, 100));
    // A text freed while the editor is parked on it: its place let go, and the editor goes on.
    TextDoc* c = create_text_doc("third\n", 6);
    ui_editor_set_doc(ed, c);
    ui_editor_set_doc(ed, b);
    free_text_doc(c);
    ui_editor_set_doc(ed, a);
    editor_pass(ui);
    say_flag("set-doc", "parked", "freed", ui_editor_top(ed) == placed);
    free_text_doc(a);
    free_text_doc(b);
    return 0;
}

// What vim held unfinished in an editor let go when the editor is shown another text, and when it
// is freed and another made in its place.
static int probe_vim_set_doc(UISystem* ui) {
    TextDoc* a = create_text_doc(VIM_TEXT, strlen(VIM_TEXT));
    TextDoc* b = create_text_doc("one two\n", 8);
    UIEditorVim* vim = create_ui_editor_vim();
    UIElement* ed = vim_fixture(ui, a, vim, 0);
    // A half-typed operator: w after the switch only moves.
    vim_keys(ui, "d");
    ui_editor_set_doc(ed, b);
    vim_keys(ui, "w");
    say_flag("vim-set-doc", "operator", "let_go",
             doc_is_str(b, "one two\n") && !strcmp(ui_editor_vim_pending(vim), "") &&
                 one_selection(ed, 4, 4));
    // An insert: its group on the text left is closed, and x on the next is normal mode's.
    vim_keys(ui, "iab");
    const bool open = b->grouping;
    ui_editor_set_doc(ed, a);
    vim_keys(ui, "x");
    say_flag("vim-set-doc", "insert", "let_go",
             open && !b->grouping && doc_is_str(a, &VIM_TEXT[1]) &&
                 ui_editor_vim_state(vim) == UI_VIM_NORMAL);
    // A visual area: x after the switch takes one character, at the caret the editor came back to
    // in that text, where the insert left it.
    vim_keys(ui, "vll");
    ui_editor_set_doc(ed, b);
    vim_keys(ui, "x");
    say_flag("vim-set-doc", "visual", "let_go",
             doc_is_str(b, "one abwo\n") && ui_editor_vim_state(vim) == UI_VIM_NORMAL);
    // Freed with an operator half typed, and another editor made in its place, most likely at
    // its address: w there only moves. Esc first, so this case stands apart from those above.
    vim_keys(ui, "<Esc>d");
    UIElement* parent = ed->parent;
    ui_remove(ed);
    const void* was = ed;
    UIElement* again = ui_editor(parent, a);
    // Whether the case is the one it names: another editor where the freed one was.
    printf("ui vim-set-doc address reused %d\n", (const void*)again == was ? 1 : 0);
    ui_set_size(again, UI_FIXED, 600.0f, UI_FIXED, 300.0f);
    ui_editor_set_mode(again, ui_editor_vim_mode(vim));
    ui_set_focus(ui_top(ui), again);
    editor_pass(ui);
    const size_t a_len = text_doc_length(a);
    vim_keys(ui, "w");
    say_flag("vim-set-doc", "freed", "let_go",
             text_doc_length(a) == a_len && !strcmp(ui_editor_vim_pending(vim), "") &&
                 one_selection(again, 5, 5));
    ui_editor_set_mode(again, NULL);
    free_text_doc(a);
    free_text_doc(b);
    free_ui_editor_vim(vim);
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
    if (!strcmp(which, "doc-read-only"))
        return probe_doc_read_only();
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
    if (!strcmp(which, "editor-scroll-line"))
        return probe_editor_scroll_line(ui);
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
    if (!strcmp(which, "element-keys"))
        return probe_element_keys(ui);
    if (!strcmp(which, "panel-focus"))
        return probe_panel_focus(ui, font);
    if (!strcmp(which, "border-fade"))
        return probe_border_fade(ui);
    if (!strcmp(which, "editor-decline"))
        return probe_editor_decline(ui);
    if (!strcmp(which, "editor-paired-char"))
        return probe_editor_paired_char(ui);
    if (!strcmp(which, "editor-shapes"))
        return probe_editor_shapes(ui);
    if (!strcmp(which, "editor-line-numbers"))
        return probe_editor_line_numbers(ui);
    if (!strcmp(which, "editor-whole-lines"))
        return probe_editor_whole_lines(ui);
    if (!strcmp(which, "editor-folds"))
        return probe_editor_folds(ui);
    if (!strcmp(which, "editor-fillers"))
        return probe_editor_fillers(ui);
    if (!strcmp(which, "editor-gutter"))
        return probe_editor_gutter(ui);
    if (!strcmp(which, "editor-bar-room"))
        return probe_editor_bar_room(ui);
    if (!strcmp(which, "editor-top-row"))
        return probe_editor_top_row(ui);
    if (!strcmp(which, "editor-gutter-digits"))
        return probe_editor_gutter_digits(ui);
    if (!strcmp(which, "editor-filler-end"))
        return probe_editor_filler_end(ui);
    if (!strcmp(which, "editor-rows-kept"))
        return probe_editor_rows_kept(ui);
    if (!strcmp(which, "editor-fillers-read"))
        return probe_editor_fillers_read(ui);
    if (!strcmp(which, "editor-exact-rows"))
        return probe_editor_exact_rows(ui);
    if (!strcmp(which, "editor-scrolls"))
        return probe_editor_scrolls(ui);
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
    if (!strcmp(which, "vim-read-only"))
        return probe_vim_read_only(ui);
    if (!strcmp(which, "vim-carets"))
        return probe_vim_carets(ui);
    if (!strcmp(which, "remove"))
        return probe_ui_remove(ui);
    if (!strcmp(which, "move"))
        return probe_ui_move(ui);
    if (!strcmp(which, "hidden"))
        return probe_ui_hidden(ui);
    if (!strcmp(which, "set-doc"))
        return probe_editor_set_doc(ui);
    if (!strcmp(which, "vim-set-doc"))
        return probe_vim_set_doc(ui);
    if (!strcmp(which, "vim-lookup"))
        return probe_vim_lookup(ui);
    return -1;
}
