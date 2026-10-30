/*
 * The text field (spec 14.2): a caret, a selection, the clipboard and undo over a UTF-8 buffer,
 * and the keys and the pointer that drive them.
 *
 * The editing is stb_textedit -- ImGui's copy, vendored with cimgui, which runs it on UTF-8 bytes
 * and carries its fixes over upstream 1.14 (the redo discard, the last line, the caret at a wrap
 * point). Its rows, its widths and the drawing are the UI's own text walk (ui_text.h), so a caret
 * lands where the glyph beside it is drawn and a wrapped row ends where a wrapped label's would.
 */

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "ext/log.h"
#include "ui.h"
#include "ui_field.h"
#include "ui_text.h"
#include "util.h"

/*
 * ImGui's copy of stb_textedit compiles as C but for names of ImGui's own: it records which side of
 * a wrap point a click or a move left the caret on in a `LastMoveDirectionLR` member of the string,
 * reads the state back through its `Stb` member, spells the two sides ImGuiDir_Left and
 * ImGuiDir_Right, cast to ImS8, and asserts with IM_ASSERT. They are given here, for this file
 * only, which is also stb's implementation unit. A second, edited copy of the header was the
 * alternative.
 */
typedef signed char ImS8;
enum { ImGuiDir_Left = 0, ImGuiDir_Right = 1 };
#define IM_ASSERT(cond) assert(cond)

typedef struct UIField UIField;
#define IMSTB_TEXTEDIT_STRING   UIField
#define IMSTB_TEXTEDIT_CHARTYPE char
#include "ext/cimgui/imgui/imstb_textedit.h"

// A field's text past this is refused, so a paste cannot take the process's memory.
#define FIELD_MAX_BYTES (1 << 20)
// A multi-line field's tallest, when the element names none.
#define FIELD_ROWS_DEFAULT 6
// Two presses this close in time and place are one double press.
#define FIELD_PRESS_SECONDS 0.4
#define FIELD_PRESS_POINTS  4.0f
// The caret's blink: lit, then dark, ImGui's rhythm.
#define CARET_LIT_SECONDS  0.8
#define CARET_DARK_SECONDS 0.4
#define CARET_WIDTH        1.5f

struct UIField {
    UIElement* el; // the element it belongs to: its runs, its callbacks' first argument
    unsigned flags;
    char* buf; // UTF-8, always NUL-terminated
    int len, cap;
    STB_TexteditState state;
    // The two members ImGui's copy of stb_textedit writes by name; see the block above.
    ImS8 LastMoveDirectionLR;
    STB_TexteditState* Stb;

    // How the buffer is measured: in the field's style, as the last layout resolved it, and broken
    // into rows at `wrap`, kept current through every edit (_sync).
    UIText text;
    vec4 color; // the text's colour, which `text` points at
    float wrap; // the content's width for a multi-line field; INFINITY on one line
    float line_h;
    UILines rows;

    float scroll_x, scroll_y; // the content's offset inside the field's box
    unsigned edits;           // every insert and delete, counted: what on_change watches
    // The caret, the selection and the edits as a turn of keys began (ui_field_keys_begin).
    int was_cursor, was_start, was_end;
    unsigned was_edits;
    bool caret_moved;  // since the blink last restarted
    double caret_time; // when it did, on the draw's clock
    double last_press; // the pointer's last press, on the input's clock
    int presses;       // how many in a row, for a word or a line
    float press_x, press_y;

    UIActionFn on_change;
    void* change_user;
    UIKeyFn on_key;
    void* key_user;
};

// The rows the text breaks into, kept by ui_lines_break until the width or the style changes. A
// single-line field is one row, whatever it holds.
static void _break_rows(UIField* f) {
    if (!f->state.single_line) {
        ui_lines_break(&f->rows, &f->text, f->wrap);
        return;
    }
    if (grow_array((void**)&f->rows.starts, &f->rows.capacity, 1, sizeof(size_t), 8)) {
        f->rows.starts[0] = 0;
        f->rows.count = 1;
    }
}

// After an edit the measuring text follows the buffer, and the rows are broken again for it at
// once: stb_textedit asks for them before the next layout.
static void _sync(UIField* f) {
    f->text.text = f->buf;
    f->text.length = (size_t)f->len;
    f->text.runs = f->el->runs;
    f->text.run_count = f->el->run_count;
    f->rows.count = 0;
    _break_rows(f);
}

// The row holding byte `at`: the last that starts at or before it.
static size_t _row_of(const UIField* f, int at) {
    size_t lo = 0, hi = f->rows.count;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (f->rows.starts[mid] <= (size_t)at)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

static int _next_index(const UIField* f, int i) {
    if (i >= f->len)
        return f->len + 1;
    const unsigned char* p = (const unsigned char*)f->buf + i;
    text_utf8_next(&p);
    return (int)((const char*)p - f->buf);
}

static int _prev_index(const UIField* f, int i) {
    if (i <= 0)
        return -1;
    int j = i - 1;
    while (j > 0 && ((unsigned char)f->buf[j] & 0xc0) == 0x80)
        j--;
    return j;
}

// What a byte starts, for word moves: 0 blank, 1 punctuation, 2 a word's (letters, digits, `_`
// and anything past ASCII, so a word in another script is a word).
static int _class(const UIField* f, int i) {
    const unsigned char c = (unsigned char)f->buf[i];
    if (c == ' ' || c == '\t' || c == '\n')
        return 0;
    if (c >= 0x80 || isalnum(c) || c == '_')
        return 2;
    return 1;
}

// Back past any blanks, then back over the run of one class before them.
static int _word_left(const UIField* f, int i) {
    i = _prev_index(f, i);
    while (i > 0 && _class(f, i) == 0)
        i = _prev_index(f, i);
    if (i <= 0)
        return 0;
    const int k = _class(f, i);
    for (int p = _prev_index(f, i); p >= 0 && _class(f, p) == k; p = _prev_index(f, p))
        i = p;
    return i;
}

// Forward to the end of the next word on macOS, as its fields move; to the start of the one after
// elsewhere, as Windows' do.
static int _word_right(const UIField* f, int i) {
    const int n = f->len;
#ifdef __APPLE__
    while (i < n && _class(f, i) == 0)
        i = _next_index(f, i);
    if (i >= n)
        return n;
    const int k = _class(f, i);
    while (i < n && _class(f, i) == k)
        i = _next_index(f, i);
#else
    if (i < n && _class(f, i) != 0) {
        const int k = _class(f, i);
        while (i < n && _class(f, i) == k)
            i = _next_index(f, i);
    }
    while (i < n && _class(f, i) == 0)
        i = _next_index(f, i);
#endif
    return i > n ? n : i;
}

// The visible row's start and end, a wrapped row's as well as a line's: what Home and End, and
// Cmd+arrow, move to.
static int _row_start(const UIField* f, const STB_TexteditState* s, int cursor) {
    if (s->single_line)
        return 0;
    return (int)f->rows.starts[_row_of(f, cursor)];
}

// A line's end is before its newline, its trailing spaces in it; a wrapped row's is before the
// spaces it broke at, which leaves the caret on the row rather than at the next one's start.
static int _row_end(const UIField* f, const STB_TexteditState* s, int cursor) {
    if (s->single_line)
        return f->len;
    const size_t row = _row_of(f, cursor);
    if (row + 1 >= f->rows.count)
        return f->len;
    const size_t next = f->rows.starts[row + 1];
    if (f->buf[next - 1] == '\n')
        return (int)next - 1;
    return (int)ui_lines_end(&f->rows, &f->text, row);
}

/*
 * A codepoint's width at byte `line_start + i`: from where it is drawn to where the next is -- its
 * advance and its pair with the next -- so stb_textedit's sum of widths along a row is the walk's
 * own pen. Walked from the glyph itself, since a pair is all a pen carries between two glyphs: stb
 * asks for every width along a row to place a click, and from the row's start each was a walk of
 * the row.
 */
static float _glyph_width(UIField* f, int line_start, int i) {
    const int at = line_start + i;
    if (at >= f->len)
        return 0.0f;
    if (f->buf[at] == '\n')
        return -1.0f; // IMSTB_TEXTEDIT_GETWIDTH_NEWLINE
    return ui_text_pen(&f->text, (size_t)at, (size_t)_next_index(f, at));
}

// The row starting at `start`, from the rows already broken: stb asks for each row in turn from the
// text's start, and breaking each again was a walk of the text.
static void _layout_row(StbTexteditRow* r, UIField* f, int start) {
    const size_t row = _row_of(f, start);
    const bool last = row + 1 >= f->rows.count;
    const int next = last ? f->len : (int)f->rows.starts[row + 1];
    const bool newline = next > start && f->buf[next - 1] == '\n';
    const int end = newline ? next - 1 : next;
    r->x0 = 0.0f;
    r->x1 = ui_text_span_width(&f->text, (size_t)start, (size_t)end);
    r->baseline_y_delta = f->line_h;
    r->ymin = 0.0f;
    r->ymax = f->line_h;
    r->num_chars = end - start + (newline ? 1 : 0);
}

static bool _reserve(UIField* f, int need) {
    if (need + 1 <= f->cap)
        return true;
    size_t cap = (size_t)f->cap;
    if (!grow_array((void**)&f->buf, &cap, (size_t)need + 1, 1, 32))
        return false;
    f->cap = (int)cap;
    return true;
}

static void _delete_chars(UIField* f, int at, int n) {
    memmove(f->buf + at, f->buf + at + n, (size_t)(f->len - at - n + 1));
    f->len -= n;
    f->edits++;
    _sync(f);
}

static int _insert_chars(UIField* f, int at, const char* text, int n) {
    if (n <= 0 || f->len + n > FIELD_MAX_BYTES || !_reserve(f, f->len + n))
        return 0;
    memmove(f->buf + at + n, f->buf + at, (size_t)(f->len - at + 1));
    memcpy(f->buf + at, text, (size_t)n);
    f->len += n;
    f->edits++;
    _sync(f);
    return n;
}

#define STB_TEXTEDIT_STRINGLEN(f)                ((f)->len)
#define STB_TEXTEDIT_GETCHAR(f, i)               ((f)->buf[i])
#define STB_TEXTEDIT_NEWLINE                     '\n'
#define IMSTB_TEXTEDIT_GETWIDTH_NEWLINE          (-1.0f)
#define STB_TEXTEDIT_GETWIDTH(f, start, i)       _glyph_width(f, start, i)
#define STB_TEXTEDIT_LAYOUTROW(r, f, start)      _layout_row(r, f, start)
#define STB_TEXTEDIT_DELETECHARS(f, i, n)        _delete_chars(f, i, n)
#define STB_TEXTEDIT_INSERTCHARS(f, i, c, n)     _insert_chars(f, i, c, n)
#define IMSTB_TEXTEDIT_GETNEXTCHARINDEX(f, i)    _next_index(f, i)
#define IMSTB_TEXTEDIT_GETPREVCHARINDEX(f, i)    _prev_index(f, i)
#define STB_TEXTEDIT_MOVEWORDLEFT(f, i)          _word_left(f, i)
#define STB_TEXTEDIT_MOVEWORDRIGHT(f, i)         _word_right(f, i)
#define STB_TEXTEDIT_MOVELINESTART(f, s, cursor) _row_start(f, s, cursor)
#define STB_TEXTEDIT_MOVELINEEND(f, s, cursor)   _row_end(f, s, cursor)

// ImGui's values, out of any key code's way.
#define STB_TEXTEDIT_K_LEFT      0x200000
#define STB_TEXTEDIT_K_RIGHT     0x200001
#define STB_TEXTEDIT_K_UP        0x200002
#define STB_TEXTEDIT_K_DOWN      0x200003
#define STB_TEXTEDIT_K_LINESTART 0x200004
#define STB_TEXTEDIT_K_LINEEND   0x200005
#define STB_TEXTEDIT_K_TEXTSTART 0x200006
#define STB_TEXTEDIT_K_TEXTEND   0x200007
#define STB_TEXTEDIT_K_DELETE    0x200008
#define STB_TEXTEDIT_K_BACKSPACE 0x200009
#define STB_TEXTEDIT_K_UNDO      0x20000A
#define STB_TEXTEDIT_K_REDO      0x20000B
#define STB_TEXTEDIT_K_WORDLEFT  0x20000C
#define STB_TEXTEDIT_K_WORDRIGHT 0x20000D
#define STB_TEXTEDIT_K_PGUP      0x20000E
#define STB_TEXTEDIT_K_PGDOWN    0x20000F
#define STB_TEXTEDIT_K_SHIFT     0x400000

#define IMSTB_TEXTEDIT_IMPLEMENTATION
#define IMSTB_TEXTEDIT_memmove memmove
// stb_textedit is a set of static functions, and not every one is called from here.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "ext/cimgui/imgui/imstb_textedit.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// ------------------------------------------------------------------- the field

struct UIField* ui_field_make(UIElement* el, unsigned flags) {
    UIField* f = calloc(1, sizeof(UIField));
    if (!f || !_reserve(f, 0)) {
        free(f);
        log_error("ui_text_field: out of memory");
        return NULL;
    }
    f->el = el;
    f->flags = flags;
    f->buf[0] = '\0';
    stb_textedit_initialize_state(&f->state, !(flags & UI_FIELD_MULTILINE));
    f->Stb = &f->state;
    f->wrap = INFINITY;
    f->text.color = f->color;
    _sync(f);
    return f;
}

void ui_field_free(struct UIField* f) {
    if (!f)
        return;
    free(f->buf);
    free(f->rows.starts);
    free(f);
}

unsigned ui_field_flags(const UIElement* el) {
    return (el && el->field) ? el->field->flags : 0u;
}

void ui_field_inspect(const UIElement* el, int* cursor, int* select_start, int* select_end,
                      float* scroll_x, float* scroll_y, size_t* rows) {
    const UIField* f = el ? el->field : NULL;
    if (!f)
        return;
    if (cursor)
        *cursor = f->state.cursor;
    if (select_start)
        *select_start = f->state.select_start;
    if (select_end)
        *select_end = f->state.select_end;
    if (scroll_x)
        *scroll_x = f->scroll_x;
    if (scroll_y)
        *scroll_y = f->scroll_y;
    if (rows)
        *rows = f->rows.count;
}

void ui_field_break(const UISystem* ui, UIElement* el, const UIStyle* s, float width) {
    UIField* f = el->field;
    if (!f)
        return;
    // Runs set since the last break are a new copy at a new address -- or none -- so a pointer that
    // moved is new runs, in faces the rows were not broken for.
    if (f->text.runs != el->runs || f->text.run_count != el->run_count)
        f->rows.count = 0;
    f->text = ui_text_make(ui_draw_list(ui), f->buf, (size_t)f->len, s, el->runs, el->run_count);
    glm_vec4_copy((float*)s->fg, f->color);
    f->text.color = f->color;
    f->line_h = ui_text_line_height(&f->text, s->line_spacing);
    f->wrap = (f->flags & UI_FIELD_MULTILINE) ? width : INFINITY;
    _break_rows(f);
}

float ui_field_content_height(const UIElement* el) {
    const UIField* f = el->field;
    if (!f)
        return 0.0f;
    size_t rows = 1;
    if (f->flags & UI_FIELD_MULTILINE) {
        const size_t most = el->max_rows > 0 ? (size_t)el->max_rows : FIELD_ROWS_DEFAULT;
        rows = f->rows.count < 1 ? 1 : (f->rows.count > most ? most : f->rows.count);
    }
    return f->line_h * (float)rows;
}

const char* ui_field_text(const UIElement* el) {
    return (el && el->field) ? el->field->buf : "";
}

void ui_field_set_text(UIElement* el, const char* text) {
    if (!el || !el->field) {
        log_error("ui_field_set_text: not a text field");
        return;
    }
    UIField* f = el->field;
    const size_t n = text ? strlen(text) : 0;
    if (n > FIELD_MAX_BYTES) {
        log_error("ui_field_set_text: %zu bytes is more than a field holds", n);
        return;
    }
    if (!_reserve(f, (int)n)) {
        log_error("ui_field_set_text: out of memory for %zu bytes", n);
        return;
    }
    free(el->runs);
    el->runs = NULL;
    el->run_count = 0;
    // Moved rather than copied: the text may be the field's own, or part of it, which is never
    // longer than the buffer and so never grew it.
    memmove(f->buf, text ? text : "", n + 1);
    f->len = (int)n;
    stb_textedit_initialize_state(&f->state, !(f->flags & UI_FIELD_MULTILINE));
    f->state.cursor = f->state.select_start = f->state.select_end = f->len;
    f->scroll_x = f->scroll_y = 0.0f;
    f->caret_moved = true;
    _sync(f);
}

void ui_field_set_on_change(UIElement* el, UIActionFn on_change, void* user) {
    if (!el || !el->field) {
        log_error("ui_field_set_on_change: not a text field");
        return;
    }
    el->field->on_change = on_change;
    el->field->change_user = user;
}

void ui_field_set_on_key(UIElement* el, UIKeyFn on_key, void* user) {
    if (!el || !el->field) {
        log_error("ui_field_set_on_key: not a text field");
        return;
    }
    el->field->on_key = on_key;
    el->field->key_user = user;
}

// ------------------------------------------------------------------- the keys

#ifdef __APPLE__
#define SHORTCUT_MOD GLFW_MOD_SUPER // Cmd: select all, copy, undo, and to a line's end
#define WORD_MOD     GLFW_MOD_ALT   // Option: a word at a time
#else
#define SHORTCUT_MOD GLFW_MOD_CONTROL
#define WORD_MOD     GLFW_MOD_CONTROL
#endif

static bool _has_selection(const UIField* f) {
    return f->state.select_start != f->state.select_end;
}

// The selection's two ends in order, whichever way it was made.
static void _selection(const UIField* f, int* lo, int* hi) {
    const int a = f->state.select_start, b = f->state.select_end;
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
}

static void _select(UIField* f, int a, int b) {
    f->state.select_start = a;
    f->state.select_end = b;
    f->state.cursor = b;
    f->state.has_preferred_x = 0;
}

// Copies the selection to the engine's clipboard; nothing when there is none.
static void _copy(const UISystem* ui, const UIField* f) {
    if (!_has_selection(f))
        return;
    int lo, hi;
    _selection(f, &lo, &hi);
    char* copy = malloc((size_t)(hi - lo) + 1);
    if (!copy)
        return;
    memcpy(copy, f->buf + lo, (size_t)(hi - lo));
    copy[hi - lo] = '\0';
    engine_clipboard_set(ui_system_engine(ui), copy);
    free(copy);
}

// The clipboard as this field takes it: control characters out, a single-line field's newlines
// as spaces, a tab a space.
static void _paste(const UISystem* ui, UIField* f) {
    const char* clip = engine_clipboard_get(ui_system_engine(ui));
    const size_t n = strlen(clip);
    if (n == 0)
        return;
    char* text = malloc(n);
    if (!text)
        return;
    size_t k = 0;
    const bool multi = f->flags & UI_FIELD_MULTILINE;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)clip[i];
        if (c == '\r') {
            if (clip[i + 1] == '\n')
                continue;
            c = '\n';
        }
        if (c == '\n')
            text[k++] = multi ? '\n' : ' ';
        else if (c == '\t')
            text[k++] = ' ';
        else if (c >= 0x20 && c != 0x7f)
            text[k++] = (char)c;
    }
    if (k > 0)
        stb_textedit_paste(f, &f->state, text, (int)k);
    free(text);
}

// With nothing selected, selects from the caret to `to`, so the key that follows deletes that.
static void _select_to_unless_selected(UIField* f, int to) {
    if (!_has_selection(f))
        _select(f, f->state.cursor, to);
}

/*
 * Typing undoes a word at a time, as a Mac's fields do, where stb_textedit records every
 * character it is given. A character's insert joins the one before when it carries straight on
 * from it, unless it is a space after something that is not: that starts the next word.
 *
 * stb's records say what UNDO does, so a typed insert is a record that deletes: its length is
 * `delete_length`, and it stores no characters (`insert_length` 0).
 */
static void _merge_typing(UIField* f) {
    StbUndoState* u = &f->state.undostate;
    if (u->undo_point < 2)
        return;
    const StbUndoRecord* last = &u->undo_rec[u->undo_point - 1];
    StbUndoRecord* prev = &u->undo_rec[u->undo_point - 2];
    if (last->insert_length != 0 || prev->insert_length != 0 || prev->delete_length <= 0 ||
        prev->where + prev->delete_length != last->where)
        return;
    if (f->buf[last->where] == ' ' && last->where > 0 && f->buf[last->where - 1] != ' ')
        return;
    prev->delete_length += last->delete_length;
    u->undo_point--;
}

// One character typed.
static void _type(UIField* f, uint32_t cp) {
    if (cp < 0x20 || cp == 0x7f || cp > 0x10ffff || (f->flags & UI_FIELD_READ_ONLY))
        return;
    char utf8[4];
    int n = 0;
    if (cp < 0x80) {
        utf8[n++] = (char)cp;
    } else if (cp < 0x800) {
        utf8[n++] = (char)(0xc0 | (cp >> 6));
        utf8[n++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        utf8[n++] = (char)(0xe0 | (cp >> 12));
        utf8[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        utf8[n++] = (char)(0x80 | (cp & 0x3f));
    } else {
        utf8[n++] = (char)(0xf0 | (cp >> 18));
        utf8[n++] = (char)(0x80 | ((cp >> 12) & 0x3f));
        utf8[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        utf8[n++] = (char)(0x80 | (cp & 0x3f));
    }
    stb_textedit_text(f, &f->state, utf8, n);
    _merge_typing(f);
}

/*
 * One key, on the platform's conventions. macOS: Option moves a word, Cmd to a line's or the text's
 * end, and Option- or Cmd-Backspace deletes that far. Elsewhere: Ctrl moves a word, Home and End
 * the line, Ctrl+Home and Ctrl+End the text. Shift extends the selection throughout. False for a
 * key the field does not use.
 */
static bool _key(const UISystem* ui, UIElement* el, UIField* f, const InputKeyEvent* ev) {
    const int mods = ev->mods;
    const int sh = (mods & GLFW_MOD_SHIFT) ? STB_TEXTEDIT_K_SHIFT : 0;
    const bool shortcut = (mods & SHORTCUT_MOD) != 0;
    const bool word = (mods & WORD_MOD) != 0;
    const bool ro = (f->flags & UI_FIELD_READ_ONLY) != 0;
    const bool multi = (f->flags & UI_FIELD_MULTILINE) != 0;
#ifdef __APPLE__
    const bool to_end = shortcut; // Cmd+arrow
#else
    const bool to_end = false;
#endif
    switch (ev->key) {
        case GLFW_KEY_ESCAPE:
        case GLFW_KEY_TAB:
            return false;
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER:
            if (!multi || ((f->flags & UI_FIELD_SUBMIT_ON_ENTER) && !sh)) {
                if (el->action)
                    el->action(el, el->action_user);
            } else if (!ro) {
                stb_textedit_text(f, &f->state, "\n", 1);
            }
            break;
        case GLFW_KEY_LEFT:
            stb_textedit_key(f, &f->state,
                             (to_end ? STB_TEXTEDIT_K_LINESTART
                              : word ? STB_TEXTEDIT_K_WORDLEFT
                                     : STB_TEXTEDIT_K_LEFT) |
                                 sh);
            break;
        case GLFW_KEY_RIGHT:
            stb_textedit_key(f, &f->state,
                             (to_end ? STB_TEXTEDIT_K_LINEEND
                              : word ? STB_TEXTEDIT_K_WORDRIGHT
                                     : STB_TEXTEDIT_K_RIGHT) |
                                 sh);
            break;
        case GLFW_KEY_UP:
            // On one line, up is the line's start, as a Mac's fields have it.
            stb_textedit_key(f, &f->state,
                             (to_end || !multi ? STB_TEXTEDIT_K_TEXTSTART : STB_TEXTEDIT_K_UP) |
                                 sh);
            break;
        case GLFW_KEY_DOWN:
            stb_textedit_key(f, &f->state,
                             (to_end || !multi ? STB_TEXTEDIT_K_TEXTEND : STB_TEXTEDIT_K_DOWN) |
                                 sh);
            break;
        case GLFW_KEY_HOME:
            stb_textedit_key(
                f, &f->state,
                ((mods & GLFW_MOD_CONTROL) ? STB_TEXTEDIT_K_TEXTSTART : STB_TEXTEDIT_K_LINESTART) |
                    sh);
            break;
        case GLFW_KEY_END:
            stb_textedit_key(
                f, &f->state,
                ((mods & GLFW_MOD_CONTROL) ? STB_TEXTEDIT_K_TEXTEND : STB_TEXTEDIT_K_LINEEND) | sh);
            break;
        case GLFW_KEY_PAGE_UP:
        case GLFW_KEY_PAGE_DOWN:
            if (!multi)
                return false; // a pane round a one-line field pages instead
            f->state.row_count_per_page = el->max_rows > 0 ? el->max_rows : FIELD_ROWS_DEFAULT;
            stb_textedit_key(
                f, &f->state,
                (ev->key == GLFW_KEY_PAGE_UP ? STB_TEXTEDIT_K_PGUP : STB_TEXTEDIT_K_PGDOWN) | sh);
            break;
        case GLFW_KEY_BACKSPACE:
            if (ro)
                break;
            if (to_end)
                _select_to_unless_selected(f, _row_start(f, &f->state, f->state.cursor));
            else if (word)
                _select_to_unless_selected(f, _word_left(f, f->state.cursor));
            stb_textedit_key(f, &f->state, STB_TEXTEDIT_K_BACKSPACE);
            break;
        case GLFW_KEY_DELETE:
            if (ro)
                break;
            if (to_end)
                _select_to_unless_selected(f, _row_end(f, &f->state, f->state.cursor));
            else if (word)
                _select_to_unless_selected(f, _word_right(f, f->state.cursor));
            stb_textedit_key(f, &f->state, STB_TEXTEDIT_K_DELETE);
            break;
        case GLFW_KEY_A:
            if (shortcut)
                _select(f, 0, f->len);
            break;
        case GLFW_KEY_C:
            if (shortcut)
                _copy(ui, f);
            break;
        case GLFW_KEY_X:
            if (shortcut) {
                _copy(ui, f);
                if (!ro)
                    stb_textedit_cut(f, &f->state);
            }
            break;
        case GLFW_KEY_V:
            if (shortcut && !ro)
                _paste(ui, f);
            break;
        case GLFW_KEY_Z:
            if (shortcut && !ro)
                stb_textedit_key(f, &f->state, sh ? STB_TEXTEDIT_K_REDO : STB_TEXTEDIT_K_UNDO);
            break;
        case GLFW_KEY_Y:
#ifndef __APPLE__
            if (shortcut && !ro)
                stb_textedit_key(f, &f->state, STB_TEXTEDIT_K_REDO);
#endif
            break;
        default:
            break;
    }
    return true;
}

void ui_field_keys_begin(UIElement* el) {
    UIField* f = el ? el->field : NULL;
    if (!f)
        return;
    f->was_cursor = f->state.cursor;
    f->was_start = f->state.select_start;
    f->was_end = f->state.select_end;
    f->was_edits = f->edits;
}

// An edit is told by the count of them rather than the length, since a paste over a selection can
// leave the length as it was.
void ui_field_keys_end(UIElement* el) {
    UIField* f = el ? el->field : NULL;
    if (!f)
        return;
    const bool edited = f->edits != f->was_edits;
    if (edited || f->state.cursor != f->was_cursor || f->state.select_start != f->was_start ||
        f->state.select_end != f->was_end)
        f->caret_moved = true;
    if (edited && f->on_change)
        f->on_change(el, f->change_user);
}

bool ui_field_key(const UISystem* ui, UIElement* el, const InputKeyEvent* ev) {
    UIField* f = el ? el->field : NULL;
    if (!f || !ev)
        return false;
    if (ev->codepoint) {
        _type(f, ev->codepoint);
        return true;
    }
    if (f->on_key && f->on_key(el, ev, f->key_user))
        return true;
    return _key(ui, el, f, ev);
}

// ------------------------------------------------------------------- the pointer

// A word round the caret, of the class of the character at it -- or before it, at a word's end.
static void _select_word(UIField* f) {
    int at = f->state.cursor;
    if (at >= f->len || (at > 0 && _class(f, at) == 0 && _class(f, _prev_index(f, at)) != 0))
        at = _prev_index(f, at);
    if (at < 0) {
        _select(f, 0, 0);
        return;
    }
    const int k = _class(f, at);
    int a = at, b = _next_index(f, at);
    for (int p = _prev_index(f, a); p >= 0 && _class(f, p) == k; p = _prev_index(f, p))
        a = p;
    while (b < f->len && _class(f, b) == k)
        b = _next_index(f, b);
    _select(f, a, b > f->len ? f->len : b);
}

void ui_field_press(UIElement* el, UIRect content, float x, float y, bool extend, double now) {
    UIField* f = el->field;
    if (!f)
        return;
    const float lx = x - content.x + f->scroll_x, ly = y - content.y + f->scroll_y;
    const bool again = now - f->last_press < FIELD_PRESS_SECONDS &&
                       fabsf(x - f->press_x) < FIELD_PRESS_POINTS &&
                       fabsf(y - f->press_y) < FIELD_PRESS_POINTS;
    f->presses = again ? f->presses + 1 : 1;
    f->last_press = now;
    f->press_x = x;
    f->press_y = y;
    if (extend)
        stb_textedit_drag(f, &f->state, lx, ly);
    else
        stb_textedit_click(f, &f->state, lx, ly);
    if (f->presses == 2) {
        _select_word(f);
    } else if (f->presses >= 3) {
        const int c = f->state.cursor;
        _select(f, _row_start(f, &f->state, c), _row_end(f, &f->state, c));
    }
    f->caret_moved = true;
}

void ui_field_drag(UIElement* el, UIRect content, float x, float y) {
    UIField* f = el->field;
    if (!f || f->presses >= 2)
        return; // a word or a line held stays selected
    const int cursor = f->state.cursor;
    stb_textedit_drag(f, &f->state, x - content.x + f->scroll_x, y - content.y + f->scroll_y);
    if (f->state.cursor != cursor)
        f->caret_moved = true;
}

// ------------------------------------------------------------------- drawing

// The caret's place in content coordinates: its row's top and its pen.
static void _caret_at(const UIField* f, float* x, float* y) {
    const size_t row = _row_of(f, f->state.cursor);
    *x = ui_text_pen(&f->text, f->rows.starts[row], (size_t)f->state.cursor);
    *y = f->line_h * (float)row;
}

// Scrolls the content so the caret stays inside the field, and never past the text's end.
static void _keep_caret_in_view(UIField* f, UIRect content) {
    float cx, cy;
    _caret_at(f, &cx, &cy);
    if (cx - f->scroll_x > content.w - CARET_WIDTH)
        f->scroll_x = cx - content.w + CARET_WIDTH;
    if (cx < f->scroll_x)
        f->scroll_x = cx;
    if (cy + f->line_h - f->scroll_y > content.h)
        f->scroll_y = cy + f->line_h - content.h;
    if (cy < f->scroll_y)
        f->scroll_y = cy;
}

// Keeps the content's scroll inside the text. Only a single-line field scrolls sideways: a
// multi-line one wraps at its width.
static void _clamp_scroll(UIField* f, UIRect content) {
    const float wide =
        f->state.single_line ? ui_text_span_width(&f->text, 0, (size_t)f->len) : 0.0f;
    const float max_x = glm_max(wide + CARET_WIDTH - content.w, 0.0f);
    const float max_y = glm_max(f->line_h * (float)f->rows.count - content.h, 0.0f);
    f->scroll_x = glm_clamp(f->scroll_x, 0.0f, max_x);
    f->scroll_y = glm_clamp(f->scroll_y, 0.0f, max_y);
}

// The rows any of which show through `content` at the scroll: [*first, *last).
static void _rows_shown(const UIField* f, UIRect content, size_t* first, size_t* last) {
    *first = 0;
    *last = f->rows.count;
    if (f->line_h <= 0.0f)
        return;
    const size_t lo = (size_t)(f->scroll_y / f->line_h);
    const size_t hi = (size_t)ceilf((f->scroll_y + content.h) / f->line_h);
    *first = lo < f->rows.count ? lo : f->rows.count;
    *last = hi < f->rows.count ? hi : f->rows.count;
}

double ui_field_draw(const UISystem* ui, UIElement* el, const UIStyle* s, UIRect content,
                     bool focused, double now, bool blink) {
    UIField* f = el->field;
    UIDrawList* dl = ui_draw_list(ui);
    ui_draw_rect(dl, el->rect, s);
    if (!f)
        return INFINITY;
    glm_vec4_copy((float*)s->fg, f->color);
    _clamp_scroll(f, content);
    if (focused)
        _keep_caret_in_view(f, content);

    ui_push_clip(dl, content);
    const float ox = content.x - f->scroll_x, oy = content.y - f->scroll_y;
    size_t first, last;
    _rows_shown(f, content, &first, &last);

    // The selection, behind the text, row by row.
    if (_has_selection(f)) {
        const UITheme* theme = ui_theme(ui);
        vec4 tint = {0.25f, 0.45f, 0.85f, 0.55f};
        if (theme->selection[3] > 0.0f)
            glm_vec4_copy((float*)theme->selection, tint);
        int lo, hi;
        _selection(f, &lo, &hi);
        for (size_t i = first; i < last; i++) {
            const size_t start = f->rows.starts[i];
            const size_t next = i + 1 < f->rows.count ? f->rows.starts[i + 1] : (size_t)f->len;
            if ((size_t)lo >= next || (size_t)hi <= start)
                continue;
            const size_t a = (size_t)lo > start ? (size_t)lo : start;
            const size_t end = ui_lines_end(&f->rows, &f->text, i);
            const size_t b = (size_t)hi < end ? (size_t)hi : end;
            const float x0 = ui_text_pen(&f->text, start, a);
            // A selection running on past the row's end shows a little past it, as a newline does.
            float x1 = ui_text_pen(&f->text, start, b);
            if ((size_t)hi > end)
                x1 += f->line_h * 0.3f;
            if (x1 > x0)
                ui_draw_quad(dl, (UIRect){ox + x0, oy + f->line_h * (float)i, x1 - x0, f->line_h},
                             tint);
        }
    }

    // The text, or the placeholder while there is none.
    if (f->len == 0 && el->text && el->text[0]) {
        UIStyle dim = *s;
        dim.fg[3] *= 0.45f;
        ui_draw_text(dl, (UIRect){ox, oy, content.w + f->scroll_x, f->line_h}, el->text, &dim,
                     UI_ALIGN_START);
    } else {
        for (size_t i = first; i < last; i++)
            ui_text_draw_span(dl, &f->text, f->rows.starts[i], ui_lines_end(&f->rows, &f->text, i),
                              ox, ui_text_baseline(&f->text, oy + f->line_h * (float)i));
    }

    // The caret, while the field is focused and editable.
    double wake = INFINITY;
    if (focused && !(f->flags & UI_FIELD_READ_ONLY)) {
        if (f->caret_moved) {
            f->caret_time = now;
            f->caret_moved = false;
        }
        bool lit = true;
        if (blink) {
            const double period = CARET_LIT_SECONDS + CARET_DARK_SECONDS;
            double phase = fmod(now - f->caret_time, period);
            phase = phase < 0.0 ? 0.0 : phase;
            lit = phase < CARET_LIT_SECONDS;
            wake = (lit ? CARET_LIT_SECONDS : period) - phase;
        }
        if (lit) {
            float cx, cy;
            _caret_at(f, &cx, &cy);
            ui_draw_quad(dl, (UIRect){ox + cx, oy + cy, CARET_WIDTH, f->line_h}, f->color);
        }
    }
    ui_pop_clip(dl);
    return wake;
}
