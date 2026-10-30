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
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "ext/log.h"
#include "ui.h"
#include "ui_field.h"
#include "ui_text.h"
#include "ui_typing.h"
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
#define CARET_WIDTH     UI_TYPING_CARET_WIDTH

struct UIField {
    UIElement* el; // the element it belongs to: its runs, its callbacks' first argument
    unsigned flags;
    char* buf; // UTF-8, always NUL-terminated
    int len, cap;
    char* placeholder; // the dim hint while `buf` is empty; owned, NULL for none
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

// A single-line field's one row, whatever it holds.
static void _one_row(UIField* f) {
    if (grow_array((void**)&f->rows.starts, &f->rows.capacity, 1, sizeof(size_t), 8)) {
        f->rows.starts[0] = 0;
        f->rows.count = 1;
    }
}

// The rows the text breaks into, kept by ui_lines_break until the width or the style changes.
static void _break_rows(UIField* f) {
    if (f->state.single_line)
        _one_row(f);
    else
        ui_lines_break(&f->rows, &f->text, f->wrap);
}

// After an edit the measuring text follows the buffer and its runs, and the rows are broken again
// for it at once from row `row` on, the rows before it kept: stb_textedit asks for them before the
// next layout.
static void _sync_from(UIField* f, size_t row) {
    f->text.text = f->buf;
    f->text.length = (size_t)f->len;
    f->text.runs = f->el->runs;
    f->text.run_count = f->el->run_count;
    if (f->state.single_line)
        _one_row(f);
    else
        ui_lines_break_from(&f->rows, &f->text, f->wrap, row);
}

static void _sync(UIField* f) {
    _sync_from(f, 0);
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

// The byte past the character at `i`, and the start of the one before it: stb_textedit's steps,
// which run past the ends, as its own (ImGui's) do.
static int _next_index(const UIField* f, int i) {
    if (i >= f->len)
        return f->len + 1;
    const char* p = f->buf + i;
    text_utf8_next(&p);
    return (int)(p - f->buf);
}

static int _prev_index(const UIField* f, int i) {
    if (i <= 0)
        return -1;
    const char* p = f->buf + i;
    text_utf8_prev(&p, f->buf);
    return (int)(p - f->buf);
}

// The platform's word moves (ui_typing.h), in stb_textedit's int offsets.
static int _word_left(const UIField* f, int i) {
    return (int)ui_typing_word_left(f->buf, (size_t)f->len, i > 0 ? (size_t)i : 0);
}

static int _word_right(const UIField* f, int i) {
    return (int)ui_typing_word_right(f->buf, (size_t)f->len, i > 0 ? (size_t)i : 0);
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
    free(f->placeholder);
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

void ui_field_runs_changed(UIElement* el) {
    if (el && el->field)
        _sync(el->field);
}

void ui_field_break(const UISystem* ui, UIElement* el, const UIStyle* s, float width) {
    UIField* f = el->field;
    if (!f)
        return;
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
        rows = f->rows.count < 1 ? 1 : f->rows.count;
        if (el->max_rows > 0 && rows > (size_t)el->max_rows)
            rows = (size_t)el->max_rows;
    }
    return f->line_h * (float)rows;
}

const char* ui_field_text(const UIElement* el) {
    return (el && el->field) ? el->field->buf : "";
}

void ui_field_set_placeholder(UIElement* el, const char* text) {
    if (!el || !el->field) {
        log_error("ui_field_set_placeholder: not a text field");
        return;
    }
    free(el->field->placeholder);
    el->field->placeholder = text ? safe_strdup(text) : NULL;
}

/*
 * The text so far stays as it is, so only the last row can break otherwise: every row before it
 * ended where the text ran past its width, inside the old text. The merged runs are this field's
 * own and its text follows them, so the layout's next break finds the rows already current.
 */
bool ui_field_append(UIElement* el, const char* text, const UITextRun* runs, size_t run_count) {
    if (!el || !el->field) {
        log_error("ui_field_append: not a text field");
        return false;
    }
    UIField* f = el->field;
    text = text ? text : "";
    const size_t n = strlen(text);
    if (run_count > 0 && !(f->flags & UI_FIELD_READ_ONLY)) {
        log_error("ui_field_append: an editable field stays in one style, and takes no runs");
        return false;
    }
    if (run_count > 0 && !ui_text_runs_check("ui_field_append", text, n, runs, run_count))
        return false;
    if ((size_t)f->len + n > FIELD_MAX_BYTES) {
        log_error("ui_field_append: %zu bytes more is more than a field holds", n);
        return false;
    }
    // The text may be the field's own, which growing the buffer would move: kept as an offset.
    const uintptr_t own = (uintptr_t)text - (uintptr_t)f->buf;
    const bool inside = own <= (uintptr_t)f->len;
    if (!_reserve(f, f->len + (int)n)) {
        log_error("ui_field_append: out of memory for %zu bytes more", n);
        return false;
    }
    if (inside)
        text = f->buf + own;
    if (n == 0)
        return true;
    if (run_count > 0) {
        UITextRun* merged = realloc(el->runs, (el->run_count + run_count) * sizeof(UITextRun));
        if (!merged) {
            log_error("ui_field_append: out of memory for its runs");
            return false;
        }
        for (size_t i = 0; i < run_count; i++) {
            merged[el->run_count + i] = runs[i];
            merged[el->run_count + i].start += (size_t)f->len;
        }
        el->runs = merged;
        el->run_count += run_count;
    }
    // Its own text ends at the buffer's end, so the two ranges never overlap; its NUL is the first
    // byte written over, which is why the terminator is written apart.
    memcpy(f->buf + f->len, text, n);
    f->len += (int)n;
    f->buf[f->len] = '\0';
    _sync_from(f, f->rows.count ? f->rows.count - 1 : 0);
    return true;
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
    const size_t k = ui_typing_clean(clip, n, text, (f->flags & UI_FIELD_MULTILINE) != 0, false);
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
    if (cp < 0x20 || cp == 0x7f || (f->flags & UI_FIELD_READ_ONLY))
        return;
    char utf8[4];
    const size_t n = text_utf8_encode(cp, utf8);
    if (n == 0)
        return; // no character: a surrogate half, or past the last plane
    stb_textedit_text(f, &f->state, utf8, (int)n);
    _merge_typing(f);
}

/*
 * One key, on the platform's conventions. macOS: Option moves a word, Cmd to a line's or the text's
 * end, and Option- or Cmd-Backspace deletes that far. Elsewhere: Ctrl moves a word, Home and End
 * the line, Ctrl+Home and Ctrl+End the text. Shift extends the selection throughout. False for a
 * key the field does not use.
 */
static bool _key(const UISystem* ui, UIElement* el, UIField* f, UIRect content,
                 const InputKeyEvent* ev) {
    const int mods = ev->mods;
    const int sh = (mods & GLFW_MOD_SHIFT) ? STB_TEXTEDIT_K_SHIFT : 0;
    const bool shortcut = (mods & INPUT_MOD_SHORTCUT) != 0;
    const bool word = (mods & INPUT_MOD_WORD) != 0;
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
            // By the rows it shows, whatever held it to its height. A content box a whole number of
            // rows tall comes back from the layout's padding round trip a rounding under it.
            f->state.row_count_per_page =
                f->line_h > 0.0f ? (int)glm_max(content.h / f->line_h + 1e-3f, 1.0f) : 1;
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

bool ui_field_key(const UISystem* ui, UIElement* el, UIRect content, const InputKeyEvent* ev) {
    UIField* f = el ? el->field : NULL;
    if (!f || !ev)
        return false;
    if (ev->codepoint) {
        _type(f, ev->codepoint);
        return true;
    }
    if (f->on_key && f->on_key(el, ev, f->key_user))
        return true;
    return _key(ui, el, f, content, ev);
}

// ------------------------------------------------------------------- the pointer

// The word round the caret (ui_typing.h).
static void _select_word(UIField* f) {
    size_t a = 0, b = 0;
    ui_typing_word_round(f->buf, (size_t)f->len, (size_t)f->state.cursor, &a, &b);
    _select(f, (int)a, (int)b);
}

void ui_field_press(UIElement* el, UIRect content, float x, float y, bool extend, double now) {
    UIField* f = el->field;
    if (!f)
        return;
    const float lx = x - content.x + f->scroll_x, ly = y - content.y + f->scroll_y;
    const bool again = ui_typing_again(now, f->last_press, x, y, f->press_x, f->press_y);
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
    if (f->len == 0 && f->placeholder && f->placeholder[0]) {
        UIStyle dim = *s;
        dim.fg[3] *= 0.45f;
        ui_draw_text(dl, (UIRect){ox, oy, content.w + f->scroll_x, f->line_h}, f->placeholder, &dim,
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
        if (ui_typing_caret_lit(now, f->caret_time, blink, &wake)) {
            float cx, cy;
            _caret_at(f, &cx, &cy);
            ui_draw_quad(dl, (UIRect){ox + cx, oy + cy, CARET_WIDTH, f->line_h}, f->color);
        }
    }
    ui_pop_clip(dl);
    return wake;
}
