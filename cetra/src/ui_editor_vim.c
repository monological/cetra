/*
 * The vim mode (spec 14.5): vim's normal and insert modes on the code editor, a key at a time.
 * Motions, operators and their ranges follow vim's own rules -- which motions are inclusive or
 * linewise, `cw` taking what `ce` would, an exclusive motion ending at a line's start stopping at
 * the line before -- since a vim user's fingers know them, and vim itself is what the tests hold
 * this to. No code is copied from vim: the rules are its documented behaviour.
 *
 * Positions are byte offsets, as the editor's. Vim's cursor sits on a character, and in normal
 * mode never past a line's last one. A text ending in a line break has one line fewer here than
 * the editor counts: vim reads that break as the last line's end, not as the start of another.
 */

#include "ui_editor_vim.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <GLFW/glfw3.h>

#include "engine.h"
#include "ext/log.h"
#include "text.h"
#include "ui_editor_internal.h"
#include "ui_typing.h"
#include "util.h"

// ---- Keys, as vim reads them: a character, or one of these past the last codepoint

enum {
    K_ESC = 0x110000,
    K_CR,
    K_BS,
    K_DEL,
    K_TAB,
    K_LEFT,
    K_RIGHT,
    K_UP,
    K_DOWN,
    K_HOME,
    K_END,
    K_PGUP,
    K_PGDN,
    K_COPY,  // Cmd+C on macOS
    K_PASTE, // Cmd+V on macOS
};
#define K_CTRL(c) (0x120000u + (uint32_t)(c))

// The key an event is to vim; 0 for one it leaves alone -- a printable key, whose character comes
// after it, and the keys other modifiers make.
static uint32_t _vkey(const InputKeyEvent* ev) {
    if (ev->codepoint)
        return ev->codepoint >= 0x20 && ev->codepoint != 0x7f ? ev->codepoint : 0;
    const int mods = ev->mods & (GLFW_MOD_SHIFT | GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER);
#ifdef __APPLE__
    if (mods == GLFW_MOD_SUPER)
        return ev->key == GLFW_KEY_C ? K_COPY : ev->key == GLFW_KEY_V ? K_PASTE : 0;
#endif
    if (mods == GLFW_MOD_CONTROL) {
        if (ev->key >= GLFW_KEY_A && ev->key <= GLFW_KEY_Z)
            return K_CTRL('a' + (ev->key - GLFW_KEY_A));
        return ev->key == GLFW_KEY_LEFT_BRACKET ? K_ESC : 0;
    }
    if (mods)
        return 0;
    switch (ev->key) {
        case GLFW_KEY_ESCAPE:
            return K_ESC;
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER:
            return K_CR;
        case GLFW_KEY_BACKSPACE:
            return K_BS;
        case GLFW_KEY_DELETE:
            return K_DEL;
        case GLFW_KEY_TAB:
            return K_TAB;
        case GLFW_KEY_LEFT:
            return K_LEFT;
        case GLFW_KEY_RIGHT:
            return K_RIGHT;
        case GLFW_KEY_UP:
            return K_UP;
        case GLFW_KEY_DOWN:
            return K_DOWN;
        case GLFW_KEY_HOME:
            return K_HOME;
        case GLFW_KEY_END:
            return K_END;
        case GLFW_KEY_PAGE_UP:
            return K_PGUP;
        case GLFW_KEY_PAGE_DOWN:
            return K_PGDN;
        default:
            return 0;
    }
}

// ---- The state

#define KEYS_MAX    4096
#define PENDING_MAX 64

// What the register holds: characters, whole lines, or a block's pieces, a line each.
typedef enum { REG_CHARS, REG_LINES, REG_BLOCK } RegKind;

// A visual area as `.` makes it again from the cursor: its mode, its lines, and its columns -- for
// one line of characters or a block its width, for more lines of characters its last column; for
// any, INT_MAX to each line's end.
typedef struct {
    UIEditorVimState mode;
    size_t lines;
    int cols;
    bool on;
} VisualArea;

struct UIEditorVim {
    UIEditorMode mode;
    UIEditorVimState state;
    // The command being read: its counts before and after an operator (0 for none), the operator
    // waiting for its motion, a key waiting for the one after it, and what was typed of it.
    int count, op_count;
    uint32_t op;
    uint32_t prefix;
    char pending[PENDING_MAX];
    // The command's keys past its counts, for `.`; the last change's, and its count; and how deep
    // in replays of them the keys being read are, which keep no change of their own.
    uint32_t keys[KEYS_MAX];
    size_t key_count;
    uint32_t change[KEYS_MAX];
    size_t change_len;
    int change_count;
    int replaying;
    // The text an undo group is held open in, while one is; and, while a command made of others
    // runs them, how deep in it they are, which keeps the group open across them.
    TextDoc* grouped;
    int held;
    // Visual block mode: its corners, the cursor's wanted column, and whether its right edge is
    // each line's end ($). The editor's selections are then the piece of each line between them.
    // While `.` makes a block again its columns are given, not found from its corners.
    size_t block_anchor, block_head;
    int block_want;
    bool block_end;
    bool box_given;
    int box_left, box_right;
    // The area the visual operator being read took, and the last change's, for `.`; and the last
    // any visual operator took, which a count before v, V or Ctrl+V selects again.
    VisualArea area, change_area, reselect;
    // An insert: the command that began it and its count, where its typing begins in `keys`, and
    // whether a line's indent was put in by an Enter or an `o` with nothing typed after it. One a
    // block began ends with the cursor at the block's top line and left column.
    uint32_t insert_cmd;
    int insert_count;
    size_t insert_from;
    bool indent_pending;
    bool block_insert;
    size_t insert_top;
    int insert_left;
    // The last f, F, t or T and its character; the last search.
    uint32_t find_kind, find_char;
    char* search;
    bool search_back;
    // What vim last put on the clipboard, what it holds, and a block's width less one.
    char* yank;
    RegKind yank_kind;
    int yank_width;
    // The host's.
    UIEditorCommandFn undo, redo;
    void* undo_user;
    UIEditorVimPromptFn prompt;
    void* prompt_user;
};

// The look an editor takes from the state.
static void _set_state(UIEditorVim* v, UIEditorVimState state) {
    static const char* const NAMES[] = {"NORMAL", "INSERT", "VISUAL", "V-LINE", "V-BLOCK"};
    v->state = state;
    v->mode.name = NAMES[state];
    v->mode.types = state == UI_VIM_INSERT;
    v->mode.block_caret = state != UI_VIM_INSERT;
    v->mode.selection = state == UI_VIM_VISUAL_LINE ? UI_SELECTION_LINES
                        : state >= UI_VIM_VISUAL    ? UI_SELECTION_INCLUSIVE
                                                    : UI_SELECTION_SPAN;
}

static void _pending_add(UIEditorVim* v, uint32_t k) {
    size_t n = strlen(v->pending);
    if (k < 0x80 && n + 1 < PENDING_MAX) {
        v->pending[n++] = (char)k;
        v->pending[n] = '\0';
    }
}

// The command read whole, or let go: nothing waits. An insert's keys are kept until it ends.
static void _reset(UIEditorVim* v) {
    v->count = v->op_count = 0;
    v->op = v->prefix = 0;
    v->pending[0] = '\0';
    if (v->state != UI_VIM_INSERT)
        v->key_count = 0;
}

static void _record(UIEditorVim* v, uint32_t k) {
    if (v->key_count < KEYS_MAX)
        v->keys[v->key_count++] = k;
}

// The count the command was given, both multiplied; 0 for none.
static int _count0(const UIEditorVim* v) {
    if (!v->count && !v->op_count)
        return 0;
    return (v->count ? v->count : 1) * (v->op_count ? v->op_count : 1);
}

// The command just read changed the text: it is what `.` repeats, with the visual area it took,
// unless it is being replayed.
static void _keep_change(UIEditorVim* v, int count) {
    if (!v->replaying) {
        memcpy(v->change, v->keys, v->key_count * sizeof(uint32_t));
        v->change_len = v->key_count;
        v->change_count = count;
        v->change_area = v->area;
    }
    v->area.on = false;
}

// ---- The text, vim's way

typedef struct {
    UIEditorVim* v;
    UIElement* el;
    TextDoc* doc;
    size_t lines; // the text's lines less an empty one after a final line break
    int tab;
    bool visual; // the cursor may stand past a line's last character, on its line break
} Ctx;

static Ctx _ctx(UIEditorVim* v, UIElement* el) {
    Ctx c = {v, el, ui_editor_doc(el), 0, 4, v->state >= UI_VIM_VISUAL};
    if (c.doc) {
        const size_t n = text_doc_line_count(c.doc);
        size_t len = 0;
        text_doc_line(c.doc, n - 1, &len);
        c.lines = n > 1 && !len ? n - 1 : n;
        c.tab = c.doc->tab_width < 1 ? 1 : c.doc->tab_width;
    }
    return c;
}

static size_t _ls(const Ctx* c, size_t line) {
    return text_doc_line_start(c->doc, line);
}

static const char* _lt(const Ctx* c, size_t line, size_t* len) {
    return text_doc_line(c->doc, line, len);
}

static size_t _ll(const Ctx* c, size_t line) {
    size_t len = 0;
    _lt(c, line, &len);
    return len;
}

// A line's end: the offset past its last character, where vim reads a NUL.
static size_t _le(const Ctx* c, size_t line) {
    return _ls(c, line) + _ll(c, line);
}

static size_t _lof(const Ctx* c, size_t off) {
    const size_t line = text_doc_line_of(c->doc, off);
    return line < c->lines ? line : c->lines - 1;
}

static bool _empty(const Ctx* c, size_t line) {
    return _ll(c, line) == 0;
}

// The character at `off`; 0 at a line's end.
static uint32_t _gc(const Ctx* c, size_t off) {
    const size_t line = _lof(c, off);
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    const size_t at = _ls(c, line);
    if (off < at || off - at >= len)
        return 0;
    size_t i = off - at;
    return ui_typing_next(s, len, &i);
}

// The offset past the character at `off`, inside its line.
static size_t _after(const Ctx* c, size_t off) {
    const size_t line = _lof(c, off);
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    const size_t at = _ls(c, line);
    size_t i = off - at;
    if (i >= len)
        return off;
    ui_typing_next(s, len, &i);
    return at + i;
}

// The character before `off` inside its line.
static size_t _before(const Ctx* c, size_t off) {
    const size_t line = _lof(c, off);
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    const size_t at = _ls(c, line);
    if (off <= at)
        return at;
    const char* p = s + (off - at < len ? off - at : len);
    text_utf8_prev(&p, s);
    return at + (size_t)(p - s);
}

// The line's last character, or its start when it is empty.
static size_t _last(const Ctx* c, size_t line) {
    const size_t e = _le(c, line);
    return e > _ls(c, line) ? _before(c, e) : e;
}

// Normal mode's cursor: on a character, never on a line's end unless the line is empty, and never
// on the line after a final line break.
static size_t _hold(const Ctx* c, size_t off) {
    const size_t line = _lof(c, off);
    if (off >= _le(c, line))
        return _last(c, line);
    return off < _ls(c, line) ? _ls(c, line) : off;
}

// One character on, crossing to the next line's start from a line's end: 0 within a line, 2 onto
// a line's end, 1 onto the next line, -1 at the text's end.
static int _inc(const Ctx* c, size_t* off) {
    const size_t line = _lof(c, *off);
    const size_t e = _le(c, line);
    if (*off < e) {
        *off = _after(c, *off);
        return *off < e ? 0 : 2;
    }
    if (line + 1 < c->lines) {
        *off = _ls(c, line + 1);
        return 1;
    }
    return -1;
}

// One character back, from a line's start onto the end of the line before: 0 within a line, 1
// onto the line before, -1 at the text's start.
static int _dec(const Ctx* c, size_t* off) {
    const size_t line = _lof(c, *off);
    if (*off > _ls(c, line)) {
        *off = _before(c, *off);
        return 0;
    }
    if (line > 0) {
        *off = _le(c, line - 1);
        return 1;
    }
    return -1;
}

// A character's class for word motions: 0 blank, 1 punctuation, 2 a word's -- every non-blank
// one class with `big`, which is W, B and E.
static int _class_of(uint32_t cp) {
    if (cp == 0 || cp == ' ' || cp == '\t' || cp == 0xa0)
        return 0;
    if (cp < 0x100)
        return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
                       cp == '_' || cp >= 0xc0 || cp == 0xaa || cp == 0xb5 || cp == 0xba
                   ? 2
                   : 1;
    if (cp >= 0x2000 && cp <= 0x206f)
        return cp <= 0x200b || cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f ? 0 : 1;
    if (cp == 0x3000)
        return 0;
    return cp > 0x3000 && cp <= 0x303f ? 1 : 2;
}

static int _cls(const Ctx* c, size_t off, bool big) {
    const int k = _class_of(_gc(c, off));
    return big && k ? 1 : k;
}

static bool _blank(uint32_t cp) {
    return cp == ' ' || cp == '\t';
}

// The column `off` stands at in its line, tabs to their stops.
static int _vcol(const Ctx* c, size_t off) {
    const size_t line = _lof(c, off);
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    const size_t to = off - _ls(c, line);
    int col = 0;
    for (size_t i = 0; i < to && i < len;) {
        const uint32_t cp = ui_typing_next(s, len, &i);
        col += cp == '\t' ? c->tab - col % c->tab : 1;
    }
    return col;
}

// The column past the last one the character at `off` covers; at a line's end, one past it.
static int _vcol_end(const Ctx* c, size_t off) {
    const int col = _vcol(c, off);
    return col + (_gc(c, off) == '\t' ? c->tab - col % c->tab : 1);
}

// The character of `line` covering column `want`: its last one past its end, or its end itself
// with `past_end`, as insert mode's cursor may stand.
static size_t _at_col(const Ctx* c, size_t line, int want, bool past_end) {
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    const size_t at = _ls(c, line);
    int col = 0;
    size_t i = 0, last = 0;
    while (i < len) {
        size_t n = i;
        const uint32_t cp = ui_typing_next(s, len, &n);
        const int w = cp == '\t' ? c->tab - col % c->tab : 1;
        if (col + w > want)
            return at + i;
        col += w;
        last = i;
        i = n;
    }
    return at + (past_end || !len ? len : last);
}

// The line's first character that is not a blank; on a line of blanks its last, or its start.
static size_t _first_nonblank(const Ctx* c, size_t line) {
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    size_t i = 0;
    while (i + 1 < len && _blank((unsigned char)s[i]))
        i++;
    return _ls(c, line) + i;
}

// Whether only blanks stand before `off` on its line.
static bool _in_indent(const Ctx* c, size_t off) {
    const size_t line = _lof(c, off);
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    for (size_t i = 0; i < off - _ls(c, line) && i < len; i++)
        if (!_blank((unsigned char)s[i]))
            return false;
    return true;
}

// The line's leading blanks, copied for the caller to free; *n their length.
static char* _indent_of(const Ctx* c, size_t line, size_t* n) {
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    *n = 0;
    while (*n < len && _blank((unsigned char)s[*n]))
        (*n)++;
    char* out = malloc(*n + 1);
    if (out) {
        memcpy(out, s, *n);
        out[*n] = '\0';
    }
    return out;
}

static const char* _nl(const Ctx* c) {
    return c->doc->newline == TEXT_NEWLINE_CRLF ? "\r\n" : "\n";
}

// Whether the text ends with a line break, which is then the last line's own.
static bool _ends_broken(const Ctx* c) {
    return text_doc_line_count(c->doc) > c->lines;
}

// ---- Word motions, vim's

static bool _skip_class(const Ctx* c, size_t* off, int cls, bool big, bool forward) {
    while (_cls(c, *off, big) == cls)
        if ((forward ? _inc(c, off) : _dec(c, off)) == -1)
            return true;
    return false;
}

// w and W: `count` words on, a blank line counting as one. With `eol` -- under an operator -- the
// last word stops at its line's end rather than going on to the next line.
static bool _fwd_word(const Ctx* c, size_t* off, int count, bool big, bool eol) {
    while (--count >= 0) {
        const bool last_line = _lof(c, *off) == c->lines - 1;
        const int sclass = _cls(c, *off, big);
        int i = _inc(c, off);
        if (i == -1 || (i >= 1 && last_line))
            return false;
        if (i >= 1 && eol && count == 0)
            return true;
        if (sclass != 0) {
            while (_cls(c, *off, big) == sclass) {
                i = _inc(c, off);
                if (i == -1 || (i >= 1 && eol && count == 0))
                    return true;
            }
        }
        while (_cls(c, *off, big) == 0) {
            const size_t line = _lof(c, *off);
            if (*off == _ls(c, line) && _empty(c, line))
                break;
            i = _inc(c, off);
            if (i == -1 || (i >= 1 && eol && count == 0))
                return true;
        }
    }
    return true;
}

// b and B: back past blanks, stopping at an empty line, then to the start of the word before. With
// `stop`, a word already at its start is not left for the one before -- the word objects.
static bool _bck_word(const Ctx* c, size_t* off, int count, bool big, bool stop) {
    for (; --count >= 0; stop = false) {
        const int sclass = _cls(c, *off, big);
        if (_dec(c, off) == -1)
            return false;
        if (stop && sclass != _cls(c, *off, big) && sclass != 0) {
            _inc(c, off);
            continue;
        }
        bool empty_line = false;
        while (_cls(c, *off, big) == 0) {
            const size_t line = _lof(c, *off);
            if (*off == _ls(c, line) && _empty(c, line)) {
                empty_line = true;
                break;
            }
            if (_dec(c, off) == -1)
                return true;
        }
        if (empty_line)
            continue;
        if (_skip_class(c, off, _cls(c, *off, big), big, false))
            return true;
        _inc(c, off);
    }
    return true;
}

// e and E; with `stop`, a word already at its end is not left -- cw; with `empty`, an empty line
// is a word's end -- the word objects.
static bool _end_word(const Ctx* c, size_t* off, int count, bool big, bool stop, bool empty) {
    while (--count >= 0) {
        const int sclass = _cls(c, *off, big);
        if (_inc(c, off) == -1)
            return false;
        bool at_empty = false;
        if (_cls(c, *off, big) == sclass && sclass != 0) {
            if (_skip_class(c, off, sclass, big, true))
                return false;
        } else if (!stop || sclass == 0) {
            while (_cls(c, *off, big) == 0) {
                const size_t line = _lof(c, *off);
                if (empty && *off == _ls(c, line) && _empty(c, line)) {
                    at_empty = true;
                    break;
                }
                if (_inc(c, off) == -1)
                    return false;
            }
            if (!at_empty && _skip_class(c, off, _cls(c, *off, big), big, true))
                return false;
        }
        if (!at_empty)
            _dec(c, off);
        stop = false;
    }
    return true;
}

// ge and gE. With `eol` -- the word objects -- crossing back onto the line before ends it.
static bool _bckend_word(const Ctx* c, size_t* off, int count, bool big, bool eol) {
    while (--count >= 0) {
        const int sclass = _cls(c, *off, big);
        int i = _dec(c, off);
        if (i == -1)
            return false;
        if (eol && i == 1)
            return true;
        if (sclass != 0)
            while (_cls(c, *off, big) == sclass)
                if ((i = _dec(c, off)) == -1 || (eol && i == 1))
                    return true;
        while (_cls(c, *off, big) == 0) {
            const size_t line = _lof(c, *off);
            if (*off == _ls(c, line) && _empty(c, line))
                break;
            if ((i = _dec(c, off)) == -1 || (eol && i == 1))
                return true;
        }
    }
    return true;
}

// ---- Other motions

// f, F, t and T: `count` times to `ch` along the line, t and T stopping before it. A repeat by ;
// or , of a t that is already before its character moves on past it.
static bool _find_char(const Ctx* c, size_t* off, uint32_t kind, uint32_t ch, int count,
                       bool repeat) {
    const bool fwd = kind == 'f' || kind == 't', till = kind == 't' || kind == 'T';
    const size_t line = _lof(c, *off);
    const size_t ls = _ls(c, line), le = _le(c, line);
    size_t at = *off;
    bool stop = !(repeat && till && count == 1);
    while (count-- > 0) {
        for (;;) {
            if (fwd) {
                at = _after(c, at);
                if (at >= le)
                    return false;
            } else {
                if (at <= ls)
                    return false;
                at = _before(c, at);
            }
            if (_gc(c, at) == ch && stop)
                break;
            stop = true;
        }
    }
    *off = till ? (fwd ? _before(c, at) : _after(c, at)) : at;
    return true;
}

static const char OPENERS[] = "([{";
static const char CLOSERS[] = ")]}";

static bool _is_bracket(uint32_t cp) {
    return cp && cp < 0x80 && (strchr(OPENERS, (int)cp) || strchr(CLOSERS, (int)cp));
}

// %: the bracket under the cursor or the first after it on the line, to the one matching it.
static bool _match_bracket(const Ctx* c, size_t* off) {
    const size_t line = _lof(c, *off);
    size_t at = *off;
    const size_t le = _le(c, line);
    while (at < le && !_is_bracket(_gc(c, at)))
        at = _after(c, at);
    const uint32_t b = at < le ? _gc(c, at) : 0;
    if (!_is_bracket(b))
        return false;
    const char* o = strchr(OPENERS, (int)b);
    const char open = o ? (char)b : OPENERS[strchr(CLOSERS, (int)b) - CLOSERS];
    const char close = o ? CLOSERS[o - OPENERS] : (char)b;
    int depth = 0;
    size_t p = at;
    for (;;) {
        const uint32_t ch = _gc(c, p);
        if (ch == (uint32_t)open)
            depth += o ? 1 : -1;
        else if (ch == (uint32_t)close)
            depth += o ? -1 : 1;
        if (!depth) {
            *off = p;
            return true;
        }
        if ((o ? _inc(c, &p) : _dec(c, &p)) == -1)
            return false;
    }
}

// { and }: `count` paragraphs on or back, a paragraph ending at an empty line. At the text's end
// the cursor goes to the last character, and the motion takes it in.
static bool _paragraph(const Ctx* c, size_t* off, int count, bool fwd, bool* inclusive) {
    size_t curr = _lof(c, *off);
    while (count--) {
        bool did_skip = false;
        for (bool first = true;; first = false) {
            if (!_empty(c, curr))
                did_skip = true;
            if (!first && did_skip && _empty(c, curr))
                break;
            if (fwd ? curr + 1 >= c->lines : curr == 0) {
                if (count)
                    return false;
                break;
            }
            curr = fwd ? curr + 1 : curr - 1;
        }
    }
    if (fwd && curr == c->lines - 1 && !_empty(c, curr)) {
        *off = _last(c, curr);
        *inclusive = true;
    } else {
        *off = _ls(c, curr);
    }
    return true;
}

// ---- Text objects

// What a text object covers: from `start` to `end`, the character at `end` in it or not, or the
// lines they are on whole.
typedef struct {
    size_t start, end;
    bool inclusive, lines, ok;
} Obj;

// One character on as vim's incl and decl step, past a non-empty line's end straight onto the
// next line rather than stopping on it.
static int _incl(const Ctx* c, size_t* off) {
    int r = _inc(c, off);
    if (r >= 1 && *off > _ls(c, _lof(c, *off)))
        r = _inc(c, off);
    return r;
}

static int _decl(const Ctx* c, size_t* off) {
    int r = _dec(c, off);
    if (r == 1 && *off > _ls(c, _lof(c, *off)))
        r = _dec(c, off);
    return r;
}

// One character back within the line; false at its start.
static bool _oneleft(const Ctx* c, size_t* off) {
    if (*off <= _ls(c, _lof(c, *off)))
        return false;
    *off = _before(c, *off);
    return true;
}

// Back to the start of the word or the blanks `off` is in, within its line.
static void _back_in_line(const Ctx* c, size_t* off, bool big) {
    const int sclass = _cls(c, *off, big);
    while (*off > _ls(c, _lof(c, *off))) {
        const size_t p = _before(c, *off);
        if (_cls(c, p, big) != sclass)
            break;
        *off = p;
    }
}

// `count` more words or runs of blanks taken on from *off, to the last character of the last --
// with `include` a word with its blanks -- or, going `back`, to the first of the first. False when
// the text ends first; *inclusive false when the end is just past a line break.
static bool _word_more(const Ctx* c, size_t* off, int count, bool include, bool big, bool back,
                       bool* inclusive) {
    for (; count > 0; count--) {
        *inclusive = true;
        if (back) {
            if (_decl(c, off) == -1)
                return false;
            if (include != (_cls(c, *off, big) != 0)) {
                if (!_bck_word(c, off, 1, big, true))
                    return false;
            } else {
                if (!_bckend_word(c, off, 1, big, true))
                    return false;
                _incl(c, off);
            }
            continue;
        }
        if (_incl(c, off) == -1)
            return false;
        if (include != (_cls(c, *off, big) == 0)) {
            if (!_fwd_word(c, off, 1, big, true) && count > 1)
                return false;
            if (!_oneleft(c, off))
                *inclusive = false;
        } else if (!_end_word(c, off, 1, big, true, true)) {
            return false;
        }
    }
    return true;
}

// iw, aw, iW and aW: the word or the blanks under the cursor, `count` of them; a word with the
// blanks after it, or before it when none follow, for aw. Refused when the text ends first, `end`
// then where the search for it stopped, which a visual area's cursor goes to.
static Obj _obj_word(const Ctx* c, size_t from, int count, bool include, bool big) {
    Obj o = {from, from, true, false, false};
    size_t off = from;
    _back_in_line(c, &off, big);
    o.start = off;
    bool include_white = false;
    if ((_cls(c, off, big) == 0) == include) {
        if (!_end_word(c, &off, 1, big, true, true)) {
            o.end = off;
            return o;
        }
    } else {
        _fwd_word(c, &off, 1, big, true);
        if (off == _ls(c, _lof(c, off)))
            _decl(c, &off);
        else
            _oneleft(c, &off);
        include_white = include;
    }
    if (!_word_more(c, &off, count - 1, include, big, false, &o.inclusive)) {
        o.end = off;
        return o;
    }
    // aw on a word with nothing after it takes the blanks before it -- but not a line's indent.
    if (include_white &&
        (_cls(c, off, big) != 0 || (off == _ls(c, _lof(c, off)) && !o.inclusive))) {
        size_t p = o.start;
        if (_oneleft(c, &p)) {
            _back_in_line(c, &p, big);
            if (_cls(c, p, big) == 0 && p > _ls(c, _lof(c, p)))
                o.start = p;
        }
    }
    o.end = off;
    o.ok = true;
    return o;
}

// Whether `off` stands in its line's indent with `extra` more of it after.
static bool _in_indent_by(const Ctx* c, size_t off, size_t extra) {
    const size_t line = _lof(c, off);
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    size_t blanks = 0;
    while (blanks < len && _blank((unsigned char)s[blanks]))
        blanks++;
    return blanks >= off - _ls(c, line) + extra;
}

// The `open` not closed before `from` -- from itself not looked at -- or SIZE_MAX.
static size_t _unclosed_before(const Ctx* c, size_t from, uint32_t open, uint32_t close) {
    int depth = 0;
    size_t p = from;
    while (_dec(c, &p) != -1) {
        const uint32_t ch = _gc(c, p);
        if (ch == close) {
            depth++;
        } else if (ch == open) {
            if (!depth)
                return p;
            depth--;
        }
    }
    return SIZE_MAX;
}

// The `close` not opened after `from`, from itself not looked at; or SIZE_MAX.
static size_t _unclosed_after(const Ctx* c, size_t from, uint32_t open, uint32_t close) {
    int depth = 0;
    size_t p = from;
    while (_inc(c, &p) != -1) {
        const uint32_t ch = _gc(c, p);
        if (ch == open) {
            depth++;
        } else if (ch == close) {
            if (!depth)
                return p;
            depth--;
        }
    }
    return SIZE_MAX;
}

// The next `open` after `from`, for a cursor in no block: vim then takes the next one.
static size_t _next_open(const Ctx* c, size_t from, uint32_t open) {
    size_t p = from;
    while (_inc(c, &p) != -1)
        if (_gc(c, p) == open)
            return p;
    return SIZE_MAX;
}

// i( a( and the like: the `count`th block of open and close round the cursor -- or the next one
// when it is in none -- brackets and all, or what is inside them: a closer on a line of its own
// leaves its line, and the opener's line break, out.
static Obj _obj_block(const Ctx* c, size_t from, int count, bool include, uint32_t open,
                      uint32_t close) {
    Obj o = {from, from, false, false, false};
    size_t p = from;
    if (open == '{')
        while (_in_indent_by(c, p, 1))
            if (_inc(c, &p) != 0)
                break;
    if (_gc(c, p) == open)
        p = _after(c, p);
    // `count` blocks out, or none at all when there are not as many.
    size_t start = SIZE_MAX;
    const bool inside = _unclosed_before(c, p, open, close) != SIZE_MAX;
    for (; count > 0; count--) {
        const size_t q = inside ? _unclosed_before(c, p, open, close) : _next_open(c, p, open);
        if (q == SIZE_MAX)
            return o;
        p = start = q;
    }
    const size_t end_at = start == SIZE_MAX ? SIZE_MAX : _unclosed_after(c, start, open, close);
    if (end_at == SIZE_MAX)
        return o;
    size_t end = end_at;
    bool sol = false;
    if (!include) {
        _incl(c, &start);
        sol = end == _ls(c, _lof(c, end));
        _decl(c, &end);
        while (_in_indent_by(c, end, 1)) {
            sol = true;
            if (_decl(c, &end) != 0)
                break;
        }
    }
    o.start = start;
    o.ok = true;
    if (sol) {
        _incl(c, &end);
        o.end = end;
    } else if (start <= end) {
        o.end = end;
        o.inclusive = true;
    } else {
        o.end = start; // nothing between the brackets: nothing to operate on
    }
    return o;
}

// The next `q` on the line from byte `col` on, a character after an escape skipped; -1 for none.
static long _next_quote(const char* s, size_t len, size_t col, char q, bool escape) {
    for (size_t i = col; i < len;) {
        if (escape && s[i] == '\\') {
            if (++i >= len)
                return -1;
        } else if (s[i] == q) {
            return (long)i;
        }
        size_t n = i;
        ui_typing_next(s, len, &n);
        i = n;
    }
    return -1;
}

// The last `q` before byte `col`, one after an odd run of escapes skipped; 0 when there is none,
// where vim's own stops.
static size_t _prev_quote(const char* s, size_t col, char q, bool escape) {
    while (col > 0) {
        col--;
        while (col > 0 && text_utf8_is_continuation(s[col]))
            col--;
        size_t n = 0;
        if (escape)
            while (col - n > 0 && s[col - n - 1] == '\\')
                n++;
        if (n & 1)
            col -= n;
        else if (s[col] == q)
            break;
    }
    return col;
}

// i" a" and the like, on the cursor's line: the quoted text the cursor is in or on, or the next
// one after it; a" with the blanks after it, or before it when none follow.
static Obj _obj_quote(const Ctx* c, size_t from, int count, bool include, char q) {
    Obj o = {from, from, false, false, false};
    const size_t line = _lof(c, from), at = _ls(c, line);
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    size_t col = from - at;
    long open, end;
    if (col < len && s[col] == q) {
        // On a quote: which one it is, opening or closing, is found from the line's start.
        const size_t first = col;
        long scan = 0;
        for (;;) {
            open = _next_quote(s, len, (size_t)scan, q, false);
            if (open < 0 || (size_t)open > first)
                return o;
            end = _next_quote(s, len, (size_t)open + 1, q, true);
            if (end < 0)
                return o;
            if ((size_t)open <= first && first <= (size_t)end)
                break;
            scan = end + 1;
        }
    } else {
        open = (long)_prev_quote(s, col, q, true);
        if ((size_t)open >= len || s[open] != q) {
            open = _next_quote(s, len, (size_t)open, q, false);
            if (open < 0)
                return o;
        }
        end = _next_quote(s, len, (size_t)open + 1, q, true);
        if (end < 0)
            return o;
    }
    if (include) {
        if ((size_t)end + 1 < len && _blank((unsigned char)s[end + 1])) {
            while ((size_t)end + 1 < len && _blank((unsigned char)s[end + 1]))
                end++;
        } else {
            while (open > 0 && _blank((unsigned char)s[open - 1]))
                open--;
        }
    }
    if (!include && count < 2)
        open++;
    o.start = at + (size_t)open;
    size_t e = at + (size_t)end;
    if ((include || count > 1) && _inc(c, &e) == 2)
        o.inclusive = true;
    o.end = e;
    o.ok = true;
    return o;
}

// A line holding nothing but blanks.
static bool _white_line(const Ctx* c, size_t line) {
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    for (size_t i = 0; i < len; i++)
        if (!_blank((unsigned char)s[i]))
            return false;
    return true;
}

// ip and ap: the paragraph, or the run of blank lines, the cursor is in, `count` of them, whole
// lines; ap with the blank lines after it, or before it when none follow.
static Obj _obj_par(const Ctx* c, size_t from, int count, bool include) {
    Obj o = {from, from, false, true, false};
    size_t start = _lof(c, from);
    const bool white_in_front = _white_line(c, start);
    while (start > 0) {
        if (white_in_front ? !_white_line(c, start - 1) : _white_line(c, start - 1))
            break;
        start--;
    }
    size_t end = start;
    while (end < c->lines && _white_line(c, end))
        end++;
    // `end` is now the line after the blank lines, or `start` itself; one back makes it the last
    // line taken so far, which may be the line before `start` when there were none.
    long last = (long)end - 1;
    int i = count;
    if (!include && white_in_front)
        i--;
    while (i-- > 0) {
        if (last == (long)c->lines - 1)
            return o;
        const bool do_white = !include && _white_line(c, (size_t)last + 1);
        if (include || !do_white) {
            last++;
            while (last < (long)c->lines - 1 && !_white_line(c, (size_t)last + 1))
                last++;
        }
        if (i == 0 && white_in_front && include)
            break;
        if (include || do_white)
            while (last < (long)c->lines - 1 && _white_line(c, (size_t)last + 1))
                last++;
    }
    if (!white_in_front && !_white_line(c, (size_t)last) && include)
        while (start > 0 && _white_line(c, start - 1))
            start--;
    o.start = _ls(c, start);
    o.end = _ls(c, (size_t)last);
    o.ok = true;
    return o;
}

// The object `k` names after i or a (`ia`) from `from`, `count` of it, or none it names.
static Obj _object(const Ctx* c, uint32_t ia, uint32_t k, size_t from, int count) {
    const bool include = ia == 'a';
    const int n = count ? count : 1;
    switch (k) {
        case 'w':
        case 'W':
            return _obj_word(c, from, n, include, k == 'W');
        case '(':
        case ')':
        case 'b':
            return _obj_block(c, from, n, include, '(', ')');
        case '[':
        case ']':
            return _obj_block(c, from, n, include, '[', ']');
        case '{':
        case '}':
        case 'B':
            return _obj_block(c, from, n, include, '{', '}');
        case '<':
        case '>':
            return _obj_block(c, from, n, include, '<', '>');
        case '"':
        case '\'':
        case '`':
            return _obj_quote(c, from, n, include, (char)k);
        case 'p':
            return _obj_par(c, from, n, include);
        default:
            return (Obj){from, from, false, false, false};
    }
}

// ---- One motion

#define WANT_KEEP INT_MIN

typedef struct {
    size_t to;
    bool ok;        // false: vim beeps, nothing moves and an operator does nothing
    bool inclusive; // the character at the far end is in the range
    bool lines;     // the range is whole lines
    int want;       // the wanted column after: WANT_KEEP, -1 for the cursor's, or INT_MAX
    bool as_is;     // the range ends where the motion does, even at a line's start: d<BS>
} Moved;

// Whether a key, after its prefix, is a motion.
static bool _is_motion(uint32_t prefix, uint32_t k) {
    if (prefix == 'g')
        return k == 'g' || k == 'e' || k == 'E' || k == 'j' || k == 'k' || k == '_';
    if (prefix)
        return false;
    switch (k) {
        case 'h':
        case 'l':
        case 'j':
        case 'k':
        case 'w':
        case 'W':
        case 'b':
        case 'B':
        case 'e':
        case 'E':
        case '0':
        case '^':
        case '$':
        case '_':
        case '|':
        case '+':
        case '-':
        case 'G':
        case ';':
        case ',':
        case '%':
        case '{':
        case '}':
        case 'H':
        case 'M':
        case 'L':
        case ' ':
        case K_CR:
        case K_BS:
        case K_LEFT:
        case K_RIGHT:
        case K_UP:
        case K_DOWN:
        case K_HOME:
        case K_END:
        case K_CTRL('h'):
        case K_CTRL('n'):
        case K_CTRL('p'):
        case K_CTRL('j'):
        case K_CTRL('m'):
            return true;
        default:
            return false;
    }
}

// A line moved to by a motion that lands on its first character that is not a blank.
static Moved _to_line(const Ctx* c, size_t line) {
    return (Moved){_first_nonblank(c, line), true, false, true, -1};
}

// `n` lines down from `line`, held to the last, into *to; false only from the last line itself,
// with somewhere to go: vim's count of lines past the end takes what there is.
static bool _down(const Ctx* c, size_t line, size_t n, size_t* to) {
    if (n && line + 1 >= c->lines)
        return false;
    *to = line + n < c->lines ? line + n : c->lines - 1;
    return true;
}

// `key` (after `prefix`, with its character `arg` for f and the like) from `from`, `count0`
// times or 0 for none given, under the operator `op` or none.
static Moved _motion(Ctx* c, uint32_t prefix, uint32_t key, uint32_t arg, size_t from, int count0,
                     uint32_t op, int want) {
    const int n = count0 ? count0 : 1;
    const size_t line = _lof(c, from);
    Moved m = {from, true, false, false, -1};
    if (prefix == 'g') {
        switch (key) {
            case 'g':
                return _to_line(c, (size_t)(count0 ? count0 - 1 : 0) < c->lines
                                       ? (size_t)(count0 ? count0 - 1 : 0)
                                       : c->lines - 1);
            case 'e':
            case 'E':
                m.ok = _bckend_word(c, &m.to, n, key == 'E', false);
                m.inclusive = true;
                return m;
            case '_': {
                size_t to = line;
                if (!_down(c, line, (size_t)n - 1, &to))
                    return (Moved){from, false, false, false, -1};
                size_t at = _le(c, to);
                while (at > _ls(c, to) && _blank(_gc(c, _before(c, at))))
                    at = _before(c, at);
                m.to = at > _ls(c, to) ? _before(c, at) : at;
                m.inclusive = true;
                return m;
            }
            case 'j':
            case 'k': {
                const UIEditorMotion motion = key == 'j' ? UI_MOTION_ROW_DOWN : UI_MOTION_ROW_UP;
                m.to = _hold(c, ui_editor_motion(c->el, motion, from, n));
                return m;
            }
            default:
                return (Moved){from, false, false, false, -1};
        }
    }
    switch (key) {
        case 'h':
        case K_LEFT:
        case K_BS:
        case K_CTRL('h'): {
            int moved = 0;
            const bool wrap = key == K_BS || key == K_CTRL('h');
            for (int k = 0; k < n; k++) {
                const size_t l = _lof(c, m.to);
                if (m.to > _ls(c, l)) {
                    m.to = _before(c, m.to);
                } else if (wrap && l > 0) {
                    // Deleting back over a line break takes it: the cursor stands past the line
                    // before's last character, and the range ends where it began.
                    const bool past = (op == 'd' || op == 'c') && !_empty(c, l - 1);
                    m.to = past ? _le(c, l - 1) : _last(c, l - 1);
                    m.as_is = m.as_is || past;
                } else {
                    break;
                }
                moved++;
            }
            m.ok = moved > 0 || op;
            return m;
        }
        case 'l':
        case K_RIGHT:
        case ' ': {
            for (int k = 0; k < n; k++) {
                const size_t l = _lof(c, m.to);
                const size_t next = _after(c, m.to);
                // In visual mode the cursor may go on past the last character onto the line's end.
                if (c->visual && m.to < _le(c, l) && next >= _le(c, l)) {
                    m.to = next;
                    continue;
                }
                if (m.to >= _le(c, l) || next >= _le(c, l)) {
                    if (key == ' ' && l + 1 < c->lines) {
                        if (op && !m.inclusive && !_empty(c, l)) {
                            m.inclusive = true;
                        } else {
                            m.to = _ls(c, l + 1);
                            m.inclusive = false;
                        }
                        continue;
                    }
                    if (op) {
                        if (!_empty(c, l))
                            m.inclusive = true;
                    } else if (k == 0) {
                        m.ok = false;
                    }
                    break;
                }
                m.to = next;
            }
            return m;
        }
        case 'j':
        case 'k':
        case K_DOWN:
        case K_UP:
        case K_CTRL('n'):
        case K_CTRL('p'):
        case K_CTRL('j'): {
            const bool down =
                key == 'j' || key == K_DOWN || key == K_CTRL('n') || key == K_CTRL('j');
            if (down ? line + 1 >= c->lines : line == 0)
                return (Moved){from, false, false, true, WANT_KEEP};
            const size_t to = down ? (line + (size_t)n < c->lines ? line + (size_t)n : c->lines - 1)
                                   : (line >= (size_t)n ? line - (size_t)n : 0);
            const int w = want >= 0 || want == INT_MAX ? want : _vcol(c, from);
            return (Moved){_at_col(c, to, w, c->visual), true, false, true, w};
        }
        case '+':
        case '-':
        case K_CR:
        case K_CTRL('m'): {
            const bool down = key != '-';
            if (down ? line + 1 >= c->lines : line == 0)
                return (Moved){from, false, false, true, -1};
            return _to_line(c, down
                                   ? (line + (size_t)n < c->lines ? line + (size_t)n : c->lines - 1)
                                   : (line >= (size_t)n ? line - (size_t)n : 0));
        }
        case '_': {
            size_t to = line;
            if (!_down(c, line, (size_t)n - 1, &to))
                return (Moved){from, false, false, true, -1};
            return _to_line(c, to);
        }
        case '0':
        case K_HOME:
            m.to = _ls(c, line);
            return m;
        case '^':
            m.to = _first_nonblank(c, line);
            return m;
        case '$':
        case K_END: {
            size_t to = line;
            if (!_down(c, line, (size_t)n - 1, &to))
                return (Moved){from, false, true, false, INT_MAX};
            m.to = c->visual ? _le(c, to) : _last(c, to);
            m.inclusive = true;
            m.want = INT_MAX;
            return m;
        }
        case '|':
            m.to = _at_col(c, line, n - 1, false);
            m.want = n - 1;
            return m;
        case 'w':
        case 'W': {
            const bool big = key == 'W';
            const uint32_t at = _gc(c, from);
            if (op == 'c' && at && !_blank(at)) {
                // cw changes to the word's end, as ce does, even from its last character.
                _end_word(c, &m.to, n, big, true, false);
                m.inclusive = true;
            } else {
                _fwd_word(c, &m.to, n, big, op != 0);
            }
            // Left on a line's end: back onto its last character, which the range then takes in.
            // Not in visual mode, whose cursor may stand there.
            if (!c->visual && m.to > from && m.to > _ls(c, _lof(c, m.to)) &&
                m.to == _le(c, _lof(c, m.to))) {
                m.to = _before(c, m.to);
                m.inclusive = true;
            }
            return m;
        }
        case 'b':
        case 'B':
            m.ok = _bck_word(c, &m.to, n, key == 'B', false);
            return m;
        case 'e':
        case 'E': {
            _end_word(c, &m.to, n, key == 'E', false, false);
            m.inclusive = true;
            if (!c->visual && m.to > from && m.to > _ls(c, _lof(c, m.to)) &&
                m.to == _le(c, _lof(c, m.to)))
                m.to = _before(c, m.to);
            return m;
        }
        case 'G':
            return _to_line(c, count0 && (size_t)count0 <= c->lines ? (size_t)count0 - 1
                                                                    : c->lines - 1);
        case ';':
        case ',': {
            if (!c->v->find_kind)
                return (Moved){from, false, false, false, -1};
            uint32_t kind = c->v->find_kind;
            if (key == ',')
                kind = kind == 'f' ? 'F' : kind == 'F' ? 'f' : kind == 't' ? 'T' : 't';
            m.ok = _find_char(c, &m.to, kind, c->v->find_char, n, true);
            m.inclusive = kind == 'f' || kind == 't';
            return m;
        }
        case 'f':
        case 'F':
        case 't':
        case 'T':
            m.ok = _find_char(c, &m.to, key, arg, n, false);
            m.inclusive = key == 'f' || key == 't';
            return m;
        case '%':
            if (count0) {
                if (count0 > 100)
                    return (Moved){from, false, false, true, -1};
                return _to_line(c, ((size_t)count0 * c->lines + 99) / 100 - 1);
            }
            m.ok = _match_bracket(c, &m.to);
            m.inclusive = true;
            return m;
        case '{':
        case '}':
            m.ok = _paragraph(c, &m.to, n, key == '}', &m.inclusive);
            return m;
        case 'H':
        case 'M':
        case 'L': {
            const int rows = ui_editor_rows_shown(c->el);
            const int down = key == 'H' ? n - 1 : key == 'L' ? rows - n : (rows - 1) / 2;
            return _to_line(c, _lof(c, ui_editor_row_at(c->el, down < 0 ? 0 : down)));
        }
        default:
            return (Moved){from, false, false, false, -1};
    }
}

// ---- Edits at the carets

// The carets, sorted, and the primary's index.
typedef struct {
    TextSelection* sel;
    int* want;
    size_t count, primary;
} Carets;

static Carets _carets(UIElement* el) {
    const TextSelection* s = NULL;
    size_t primary = 0;
    const size_t n = ui_editor_selections(el, &s, &primary);
    Carets k = {calloc(n ? n : 1, sizeof(TextSelection)), calloc(n ? n : 1, sizeof(int)), n,
                primary};
    const int* w = ui_editor_wants(el);
    for (size_t i = 0; k.sel && k.want && i < n; i++) {
        k.sel[i] = s[i];
        k.want[i] = w ? w[i] : -1;
    }
    return k;
}

static void _free_carets(Carets* k) {
    free(k->sel);
    free(k->want);
}

// One caret's edit, and where its cursor goes: `within` bytes into the new text of its edit, or,
// with no edit, at `within` in the text as it stood.
typedef struct {
    TextEdit e;
    char* owned;
    bool edits;
    size_t within;
} Change;

static int _by_start(const void* a, const void* b) {
    const size_t x = (*(const Change* const*)a)->e.start, y = (*(const Change* const*)b)->e.start;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Every caret's change made as one action, each cursor where its change says, and their texts
// freed.
static void _apply(Ctx* c, Change* ch, size_t n, size_t primary) {
    Change** order = calloc(n ? n : 1, sizeof(Change*));
    TextEdit* edits = calloc(n ? n : 1, sizeof(TextEdit));
    TextSelection* after = calloc(n ? n : 1, sizeof(TextSelection));
    size_t m = 0;
    if (order && edits && after) {
        for (size_t i = 0; i < n; i++)
            if (ch[i].edits)
                order[m++] = &ch[i];
        qsort(order, m, sizeof(Change*), _by_start);
        // A change overlapping the one before it is dropped, its cursor going to that one's.
        size_t kept = 0;
        long long shift = 0;
        for (size_t i = 0; i < m; i++) {
            if (kept && order[i]->e.start < edits[kept - 1].end) {
                order[i]->edits = false;
                order[i]->within = edits[kept - 1].start;
                continue;
            }
            edits[kept++] = order[i]->e;
            const size_t start = (size_t)((long long)order[i]->e.start + shift);
            shift += (long long)order[i]->e.len - (long long)(order[i]->e.end - order[i]->e.start);
            order[i]->within += start; // now where the cursor goes in the new text
        }
        for (size_t i = 0; i < n; i++) {
            size_t at = ch[i].within;
            if (!ch[i].edits) {
                // No edit of its own: moved through those before it.
                long long d = 0;
                for (size_t k = 0; k < kept; k++)
                    if (edits[k].end <= at)
                        d += (long long)edits[k].len - (long long)(edits[k].end - edits[k].start);
                at = (size_t)((long long)at + d);
            }
            after[i] = (TextSelection){at, at};
        }
        if (kept)
            ui_editor_edit(c->el, edits, kept, after, n, primary);
        else
            ui_editor_set_carets(c->el, after, NULL, n, primary);
    }
    for (size_t i = 0; i < n; i++)
        free(ch[i].owned);
    free(order);
    free(edits);
    free(after);
}

// The carets put where `at` says, each held onto a character as normal mode's is.
static void _place(Ctx* c, const size_t* at, const int* wants, size_t n, size_t primary,
                   bool hold) {
    TextSelection* s = calloc(n ? n : 1, sizeof(TextSelection));
    if (!s)
        return;
    for (size_t i = 0; i < n; i++) {
        const size_t p = hold ? _hold(c, at[i]) : at[i];
        s[i] = (TextSelection){p, p};
    }
    ui_editor_set_carets(c->el, s, wants, n, primary);
    free(s);
}

// Where each caret now stands, after an edit put them.
static size_t* _heads(const UIElement* el, size_t* n, size_t* primary) {
    const TextSelection* s = NULL;
    *n = ui_editor_selections(el, &s, primary);
    size_t* at = calloc(*n ? *n : 1, sizeof(size_t));
    for (size_t i = 0; at && i < *n; i++)
        at[i] = s[i].head;
    return at;
}

// ---- The register

static void _put_register(Ctx* c, const char* text, size_t len, RegKind kind) {
    Engine* engine = ui_editor_engine(c->el);
    char* copy = malloc(len + 1);
    if (!copy)
        return;
    memcpy(copy, text, len);
    copy[len] = '\0';
    free(c->v->yank);
    c->v->yank = copy;
    c->v->yank_kind = kind;
    if (engine)
        engine_clipboard_set(engine, copy);
}

// What the register holds, as `p` reads it: the clipboard, and what vim put there when that is
// still its text -- from elsewhere, whole lines when it ends in a line break. An empty yank is
// held, as an empty line put whole; NULL when nothing is.
static const char* _get_register(const Ctx* c, RegKind* kind) {
    const UIEditorVim* v = c->v;
    Engine* engine = ui_editor_engine(c->el);
    const char* clip = engine ? engine_clipboard_get(engine) : v->yank;
    if (!clip || (!*clip && !(v->yank && !*v->yank)))
        return NULL;
    *kind = v->yank && !strcmp(v->yank, clip) ? v->yank_kind
            : clip[strlen(clip) - 1] == '\n'  ? REG_LINES
                                              : REG_CHARS;
    return clip;
}

// A range's text, line breaks as '\n', for the register; freed by the caller.
static char* _range_text(const Ctx* c, size_t lo, size_t hi, size_t* n) {
    char* raw = malloc(hi - lo + 1);
    if (!raw)
        return NULL;
    const size_t got = text_doc_copy(c->doc, lo, hi, raw);
    size_t k = 0;
    for (size_t i = 0; i < got; i++)
        if (!(raw[i] == '\r' && i + 1 < got && raw[i + 1] == '\n'))
            raw[k++] = raw[i];
    raw[k] = '\0';
    *n = k;
    return raw;
}

// Text whose line breaks are '\n', in the text's own: freed by the caller.
static char* _with_nl(const Ctx* c, const char* text, size_t len, size_t* out) {
    const char* nl = _nl(c);
    const size_t nl_len = strlen(nl);
    char* s = malloc(len * nl_len + 1);
    if (!s)
        return NULL;
    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            memcpy(s + k, nl, nl_len);
            k += nl_len;
        } else {
            s[k++] = text[i];
        }
    }
    s[k] = '\0';
    *out = k;
    return s;
}

// ---- Undo

// A step of undo for a command that changed nothing, as vim keeps one for an operator over an empty
// range: `x` on an empty line, then `u`, takes back nothing. Not for one part of a command made of
// others, whose later parts make its step.
static void _empty_step(Ctx* c) {
    if (!c->v->held)
        text_doc_empty_step(c->doc, c->el->editor);
}

// One step of undo held open over a command's actions, or an insert's, from its first key to its
// last: opened once, however many times asked, and closed in the text it was opened in.
static void _group(Ctx* c, bool open) {
    UIEditorVim* v = c->v;
    if (open && !v->grouped) {
        text_doc_group(c->doc, true, c->el->editor);
        v->grouped = c->doc;
    } else if (!open && v->grouped && !v->held) {
        if (v->grouped == c->doc)
            text_doc_group(c->doc, false, NULL);
        v->grouped = NULL;
    }
}

// Where vim leaves the cursor after taking a step back or making it again: where the cursor stood
// as the step began, when that is on the step's first changed line or the line above it, else
// that line's first character not a blank. `s` is the step.
static size_t _undone_at(const Ctx* c, const TextUndoStep* s) {
    const TextUndoPart* part = s->part_count ? &s->parts[0] : NULL;
    if (!part || !part->change_count || !s->before_count)
        return SIZE_MAX;
    // Changes run from the highest start down: the last is the first in the text. Where that line
    // is gone -- the last lines deleted -- the cursor goes to the line now last.
    const size_t first = part->changes[part->change_count - 1].start;
    if (first > _le(c, c->lines - 1))
        return _first_nonblank(c, c->lines - 1);
    const size_t top = _lof(c, first);
    const size_t at = s->before[s->before_count - 1].head;
    const size_t line = _lof(c, at);
    return line == top || line + 1 == top ? at : _first_nonblank(c, top);
}

static void _undo(Ctx* c, bool redo, int count) {
    UIEditorVim* v = c->v;
    bool any = false;
    for (int i = 0; i < (count ? count : 1); i++) {
        const bool done =
            redo ? (v->redo ? v->redo(c->el, v->undo_user) : ui_editor_run(c->el, "redo"))
                 : (v->undo ? v->undo(c->el, v->undo_user) : ui_editor_run(c->el, "undo"));
        if (!done)
            break;
        any = true;
    }
    size_t n = 0, primary = 0;
    size_t* at = _heads(c->el, &n, &primary);
    Ctx now = _ctx(v, c->el);
    // With the text's own undo, the step just taken back or made again says where vim's cursor
    // goes: just undone, it is the first of the steps to redo; just redone, the last to undo.
    TextDoc* doc = now.doc;
    const TextUndoStep* s = any && !(redo ? v->redo : v->undo) && doc
                                ? (redo ? (doc->undo_count ? &doc->undo[doc->undo_count - 1] : NULL)
                                        : (doc->redo_count ? &doc->undo[doc->undo_count] : NULL))
                                : NULL;
    const size_t to = s && n == 1 ? _undone_at(&now, s) : SIZE_MAX;
    if (at && to != SIZE_MAX)
        at[0] = to;
    if (at)
        _place(&now, at, NULL, n, primary, true);
    free(at);
}

// ---- Operators

// What an operator takes from a caret: a range of bytes, whole lines or not.
typedef struct {
    size_t lo, hi;
    size_t first, last; // its lines, when whole
    bool lines;
    bool ok;
} Range;

// The range a motion makes from `from`, by vim's rules: an exclusive motion ending at a line's
// start, from another line, ends at the line before's end instead -- or takes whole lines, when
// it began in its line's indent. A delete of such a range with nothing but blanks after its end
// takes whole lines too.
static Range _range(const Ctx* c, size_t from, const Moved* m, uint32_t op) {
    Range r = {0, 0, 0, 0, m->lines, m->ok};
    if (!m->ok)
        return r;
    size_t lo = from < m->to ? from : m->to, hi = from < m->to ? m->to : from;
    bool inclusive = m->inclusive;
    if (!r.lines) {
        const size_t hl = _lof(c, hi), ll = _lof(c, lo);
        if (!inclusive && !m->as_is && hl > ll && hi == _ls(c, hl)) {
            if (_in_indent(c, lo)) {
                r.lines = true;
                r.first = ll;
                r.last = hl - 1;
            } else {
                hi = _le(c, hl - 1);
                inclusive = false;
            }
        }
        if (!r.lines && op == 'd' && hl > ll) {
            const size_t past = inclusive && hi < _le(c, hl) ? _after(c, hi) : hi;
            size_t p = past;
            while (p < _le(c, hl) && _blank(_gc(c, p)))
                p = _after(c, p);
            if (p >= _le(c, hl) && _in_indent(c, lo)) {
                r.lines = true;
                r.first = ll;
                r.last = hl;
            }
        }
    } else {
        r.first = _lof(c, lo);
        r.last = _lof(c, hi);
    }
    if (r.lines) {
        r.lo = _ls(c, r.first);
        r.hi = r.last + 1 < c->lines || _ends_broken(c) ? _ls(c, r.last + 1) : _le(c, r.last);
        return r;
    }
    r.lo = lo;
    r.hi = inclusive && hi < _le(c, _lof(c, hi)) ? _after(c, hi) : hi;
    return r;
}

// Lines [first, last] as the register holds them: each with a '\n' after it.
static char* _lines_text(const Ctx* c, size_t first, size_t last, size_t* n) {
    size_t lo = _ls(c, first), hi = _le(c, last);
    char* body = _range_text(c, lo, hi, n);
    if (!body)
        return NULL;
    char* out = realloc(body, *n + 2);
    if (!out) {
        free(body);
        return NULL;
    }
    out[(*n)++] = '\n';
    out[*n] = '\0';
    return out;
}

// The bytes a linewise delete of lines [first, last] takes: their line breaks with them, or the
// one before them when they run to a text with no final break.
static void _lines_span(const Ctx* c, size_t first, size_t last, size_t* lo, size_t* hi) {
    *lo = _ls(c, first);
    if (last + 1 < c->lines || _ends_broken(c)) {
        *hi = _ls(c, last + 1);
        if (last + 1 >= c->lines && first > 0 && !_ends_broken(c)) {
            *lo = _le(c, first - 1);
            *hi = _le(c, last);
        }
    } else {
        *hi = _le(c, last);
        if (first > 0)
            *lo = _le(c, first - 1);
    }
}

static void _enter_insert(Ctx* c, uint32_t cmd, int count);

// The case of a codepoint changed: `how` '~' swaps it, 'u' lowers it, 'U' raises it. The Latin,
// Greek and Cyrillic letters that pair 32 or 80 apart; anything else is kept.
static uint32_t _case(uint32_t cp, uint32_t how) {
    uint32_t up = cp, low = cp;
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 0xc0 && cp <= 0xde && cp != 0xd7) ||
        (cp >= 0x391 && cp <= 0x3ab && cp != 0x3a2) || (cp >= 0x410 && cp <= 0x42f))
        low = cp + 32;
    else if ((cp >= 'a' && cp <= 'z') || (cp >= 0xe0 && cp <= 0xfe && cp != 0xf7) ||
             (cp >= 0x3b1 && cp <= 0x3cb && cp != 0x3c2) || (cp >= 0x430 && cp <= 0x44f))
        up = cp - 32;
    else if (cp >= 0x400 && cp <= 0x40f)
        low = cp + 80;
    else if (cp >= 0x450 && cp <= 0x45f)
        up = cp - 80;
    else if (cp == 0xdf)
        up = 0x1e9e; // ß raised is the capital sharp s, as vim raises it
    else if (cp == 0x1e9e)
        low = 0xdf;
    if (how == 'u' || (how == '~' && low != cp))
        return low;
    return how == 'U' || how == '~' ? up : cp;
}

// The bytes [lo, hi) with their case changed, for the caller to free.
static char* _cased(const Ctx* c, size_t lo, size_t hi, uint32_t how, size_t* n) {
    char* raw = malloc(hi - lo + 1);
    char* out = malloc((hi - lo) * 2 + 1);
    if (!raw || !out) {
        free(raw);
        free(out);
        return NULL;
    }
    const size_t got = text_doc_copy(c->doc, lo, hi, raw);
    size_t k = 0;
    for (const char* p = raw; p < raw + got;) {
        const char* was = p;
        char tmp[5] = {0};
        memcpy(tmp, p, (size_t)(raw + got - p) < 4 ? (size_t)(raw + got - p) : 4);
        const char* q = tmp;
        const uint32_t cp = text_utf8_next(&q);
        const size_t used = q > tmp ? (size_t)(q - tmp) : 1;
        const uint32_t to = _case(cp, how);
        if (to != cp) {
            k += text_utf8_encode(to, out + k);
        } else {
            memcpy(out + k, was, used);
            k += used;
        }
        p += used;
    }
    free(raw);
    *n = k;
    return out;
}

// A line's indent shifted `times` tab stops in, or out with `out`, built again from its column with
// spaces as vim's shift with expandtab does: the new indent, for the caller to free, and in *cut
// the bytes of the old. NULL for an empty line, which is left alone.
static char* _shifted(const Ctx* c, size_t line, bool out, int times, size_t* n, size_t* cut) {
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    size_t blanks = 0;
    int col = 0;
    while (blanks < len && _blank((unsigned char)s[blanks])) {
        col += s[blanks] == '\t' ? c->tab - col % c->tab : 1;
        blanks++;
    }
    *cut = blanks;
    if (!len)
        return NULL;
    const int by = c->tab * (times > 0 ? times : 1);
    const int to = out ? (col - by > 0 ? col - by : 0) : col + by;
    char* sp = malloc((size_t)to + 1);
    if (!sp)
        return NULL;
    memset(sp, ' ', (size_t)to);
    sp[to] = '\0';
    *n = (size_t)to;
    return sp;
}

static void _operate_on(Ctx* c, uint32_t op, const Carets* kp, const Range* r, const size_t* start,
                        int shifts);

// `op` over each caret's range: delete, change, yank, the shifts and the case operators -- the
// range a motion makes, a text object (after i or a, `prefix`) or the operator doubled.
//
// Each caret's operator starts at the earlier of the caret and where the motion goes -- for the
// operator doubled, the last line's first character not a blank, or for a yank its column; for an
// object, its start. That is where vim leaves the cursor after a yank or a change of case, and
// where its undo and redo put it back: so the carets are moved there before the edit, which the
// text's undo step keeps.
static void _operate(Ctx* c, uint32_t op, uint32_t prefix, uint32_t key, uint32_t arg, int count0) {
    Carets k = _carets(c->el);
    Range* r = calloc(k.count ? k.count : 1, sizeof(Range));
    size_t* start = calloc(k.count ? k.count : 1, sizeof(size_t));
    if (!k.sel || !r || !start) {
        _free_carets(&k);
        free(r);
        free(start);
        return;
    }
    const bool doubled = key == op || (op == '~' && key == '~') || (op == 'u' && key == 'u') ||
                         (op == 'U' && key == 'U');
    bool any = false;
    for (size_t i = 0; i < k.count; i++) {
        const size_t head = k.sel[i].head;
        start[i] = head;
        Moved m;
        if (prefix == 'i' || prefix == 'a') {
            const Obj o = _object(c, prefix, key, head, count0);
            if (!o.ok) {
                r[i].ok = false;
                continue;
            }
            m = (Moved){o.end, true, o.inclusive, o.lines, -1};
            r[i] = _range(c, o.start, &m, op);
            start[i] = o.start;
        } else if (doubled) {
            // dd, cc, yy, >>: count lines from the caret's.
            const size_t line = _lof(c, head);
            size_t to = line;
            if (!_down(c, line, (size_t)(count0 ? count0 : 1) - 1, &to)) {
                r[i].ok = false;
                continue;
            }
            const int want = k.want[i] >= 0 ? k.want[i] : _vcol(c, head);
            m = (Moved){op == 'y' ? _at_col(c, to, want, false) : _first_nonblank(c, to), true,
                        false, true, -1};
            r[i] = _range(c, _ls(c, line), &m, op);
        } else {
            m = _motion(c, prefix, key, arg, head, count0, op, k.want[i]);
            r[i] = _range(c, head, &m, op);
        }
        if (r[i].ok && m.to < start[i])
            start[i] = m.to;
        any = any || r[i].ok;
    }
    if (any)
        _operate_on(c, op, &k, r, start, 1);
    _free_carets(&k);
    free(r);
    free(start);
}

// `op` over ranges already found, one a caret: the register takes the primary's text, the carets
// go to each operator's start, and the edit is made as one step; `shifts` times for > and <.
static void _operate_on(Ctx* c, uint32_t op, const Carets* kp, const Range* r, const size_t* start,
                        int shifts) {
    UIEditorVim* v = c->v;
    const Carets k = *kp;
    Change* ch = calloc(k.count ? k.count : 1, sizeof(Change));
    if (!ch)
        return;
    const Range* pr = &r[k.primary];
    if (pr->ok && (op == 'd' || op == 'c' || op == 'y')) {
        size_t n = 0;
        char* text = pr->lines ? _lines_text(c, pr->first, pr->last, &n)
                               : _range_text(c, pr->lo, pr->hi, &n);
        if (text)
            _put_register(c, text, n, pr->lines ? REG_LINES : REG_CHARS);
        free(text);
    }
    _place(c, start, op == 'y' ? k.want : NULL, k.count, k.primary, true);
    if (op == 'y') {
        free(ch);
        return;
    }
    _group(c, true);
    for (size_t i = 0; i < k.count; i++) {
        Change* x = &ch[i];
        x->within = k.sel[i].head;
        if (!r[i].ok)
            continue;
        if (op == 'd') {
            size_t lo = r[i].lo, hi = r[i].hi;
            if (r[i].lines)
                _lines_span(c, r[i].first, r[i].last, &lo, &hi);
            x->e = (TextEdit){lo, hi, "", 0};
            x->edits = true;
            x->within = 0;
        } else if (op == 'c') {
            if (r[i].lines) {
                // The lines become one, keeping the first one's indent, which typing follows.
                size_t ind = 0;
                x->owned = _indent_of(c, r[i].first, &ind);
                x->e = (TextEdit){_ls(c, r[i].first), _le(c, r[i].last), x->owned ? x->owned : "",
                                  x->owned ? ind : 0};
                x->within = x->owned ? ind : 0;
            } else {
                x->e = (TextEdit){r[i].lo, r[i].hi, "", 0};
                x->within = 0;
            }
            x->edits = true;
        } else if (op == '<' || op == '>') {
            // Each line its own edit: gathered in a second pass below.
            x->within = SIZE_MAX;
        } else {
            // A change of case: the cursor stays at the operator's start, inside the text.
            size_t n = 0;
            const size_t lo = r[i].lines ? _ls(c, r[i].first) : r[i].lo;
            const size_t hi = r[i].lines ? _le(c, r[i].last) : r[i].hi;
            x->owned = _cased(c, lo, hi, op, &n);
            if (x->owned) {
                x->e = (TextEdit){lo, hi, x->owned, n};
                x->edits = true;
                x->within = start[i] > lo ? start[i] - lo : 0;
            }
        }
    }
    bool changes = false;
    for (size_t i = 0; i < k.count; i++)
        changes = changes || (ch[i].edits && (ch[i].e.start != ch[i].e.end || ch[i].e.len));
    if (!changes && op != '<' && op != '>')
        _empty_step(c);
    if (op == '<' || op == '>') {
        // One action of every line's indent rebuilt, each caret to its first line's first
        // character that is not a blank.
        TextEdit* edits = NULL;
        char** texts = NULL;
        size_t n = 0, cap = 0, texts_cap = 0;
        size_t last_done = SIZE_MAX;
        bool shifted = false;
        for (size_t i = 0; i < k.count; i++) {
            if (!r[i].ok)
                continue;
            const size_t first = r[i].lines ? r[i].first : _lof(c, r[i].lo);
            const size_t last = r[i].lines ? r[i].last : _lof(c, r[i].hi);
            for (size_t line = first; line <= last; line++) {
                if (last_done != SIZE_MAX && line <= last_done)
                    continue;
                size_t len = 0, cut = 0;
                char* sp = _shifted(c, line, op == '<', shifts, &len, &cut);
                if (!sp)
                    continue;
                if (!grow_array((void**)&edits, &cap, n + 1, sizeof(TextEdit), 16) ||
                    !grow_array((void**)&texts, &texts_cap, n + 1, sizeof(char*), 16)) {
                    free(sp);
                    break;
                }
                size_t ll = 0;
                const char* s = _lt(c, line, &ll);
                shifted = shifted || len != cut || memcmp(s, sp, cut) != 0;
                texts[n] = sp;
                edits[n++] = (TextEdit){_ls(c, line), _ls(c, line) + cut, sp, len};
            }
            last_done = last;
        }
        if (shifted)
            ui_editor_edit(c->el, edits, n, NULL, 0, 0);
        else
            _empty_step(c);
        for (size_t i = 0; i < n; i++)
            free(texts[i]);
        free(texts);
        free(edits);
        Ctx now = _ctx(v, c->el);
        size_t* at = calloc(k.count, sizeof(size_t));
        for (size_t i = 0; at && i < k.count; i++)
            at[i] = r[i].ok ? _first_nonblank(&now, r[i].lines ? r[i].first : _lof(c, r[i].lo))
                            : k.sel[i].head;
        if (at)
            _place(&now, at, NULL, k.count, k.primary, true);
        free(at);
    } else {
        _apply(c, ch, k.count, k.primary);
        // A linewise delete leaves the cursor on its line's first character not a blank.
        Ctx now = _ctx(v, c->el);
        size_t n = 0, primary = 0;
        size_t* at = _heads(c->el, &n, &primary);
        if (at && op != 'c') {
            for (size_t i = 0; op == 'd' && i < n && i < k.count; i++)
                if (r[i].ok && r[i].lines)
                    at[i] = _first_nonblank(&now, _lof(&now, at[i]));
            _place(&now, at, NULL, n, primary, true);
        }
        free(at);
    }
    free(ch);
    if (op == 'c') {
        _enter_insert(c, 'c', 1);
        return;
    }
    _group(c, false);
}

// ---- Edits that are not operators

static void _put_block(Ctx* c, const char* text, int width, size_t line, int col, int count);

// `clip`, of `kind`, put after or before the cursor, `count` times: whole lines below or above the
// cursor's line; a block's pieces, `width` columns wide, down from the cursor's column.
static void _put_text(Ctx* c, const char* clip, RegKind kind, int width, bool after, int count) {
    UIEditorVim* v = c->v;
    if (kind == REG_BLOCK) {
        Carets k = _carets(c->el);
        if (k.sel) {
            const size_t h = k.sel[k.primary].head, line = _lof(c, h);
            _put_block(c, clip, width, line,
                       after && !_empty(c, line) ? _vcol_end(c, h) : _vcol(c, h), count);
        }
        _free_carets(&k);
        return;
    }
    const size_t clip_len = strlen(clip);
    if (!clip_len)
        return;
    const bool lines = kind == REG_LINES;
    const int times = count ? count : 1;
    size_t total = clip_len * (size_t)times + 1;
    char* body = malloc(total + 1);
    if (!body)
        return;
    size_t len = 0;
    for (int t = 0; t < times; t++) {
        memcpy(body + len, clip, clip_len);
        len += clip_len;
    }
    body[len] = '\0';
    Carets k = _carets(c->el);
    Change* ch = calloc(k.count ? k.count : 1, sizeof(Change));
    if (!k.sel || !ch) {
        free(body);
        free(ch);
        _free_carets(&k);
        return;
    }
    _group(c, true);
    for (size_t i = 0; i < k.count; i++) {
        Change* x = &ch[i];
        const size_t head = k.sel[i].head;
        const size_t line = _lof(c, head);
        size_t n = 0;
        if (lines) {
            // Above: at the line's start. Below: at the next line's start, or, past a last line
            // with no line break of its own, after one put in front of the lines instead of
            // behind them.
            const bool past_last = after && line + 1 >= c->lines && !_ends_broken(c);
            const size_t at = !after ? _ls(c, line) : past_last ? _le(c, line) : _ls(c, line + 1);
            char* moved = past_last ? malloc(len + 1) : NULL;
            if (moved) {
                moved[0] = '\n';
                memcpy(moved + 1, body, len - 1);
            }
            x->owned = _with_nl(c, moved ? moved : body, len, &n);
            free(moved);
            x->e = (TextEdit){at, at, x->owned ? x->owned : "", x->owned ? n : 0};
            // The cursor goes to the first line put, at its first character not a blank, which
            // is found once it is in.
            x->within = past_last ? strlen(_nl(c)) : 0;
        } else {
            const size_t at = after && !_empty(c, line) ? _after(c, head) : head;
            x->owned = _with_nl(c, body, len, &n);
            x->e = (TextEdit){at, at, x->owned ? x->owned : "", x->owned ? n : 0};
            // One line put: the cursor on its last character; more: on its first.
            x->within = 0;
            if (!memchr(body, '\n', len) && x->owned && n) {
                const char* p = x->owned + n;
                text_utf8_prev(&p, x->owned);
                x->within = (size_t)(p - x->owned);
            }
        }
        x->edits = x->owned != NULL;
    }
    _apply(c, ch, k.count, k.primary);
    Ctx now = _ctx(v, c->el);
    size_t n = 0, primary = 0;
    size_t* at = _heads(c->el, &n, &primary);
    if (at) {
        if (lines)
            for (size_t i = 0; i < n; i++)
                at[i] = _first_nonblank(&now, _lof(&now, at[i]));
        _place(&now, at, NULL, n, primary, true);
    }
    free(at);
    _group(c, false);
    free(body);
    free(ch);
    _free_carets(&k);
}

// p and P: the register after or before the cursor, `count` times.
static void _put(Ctx* c, bool after, int count) {
    RegKind kind = REG_CHARS;
    const char* clip = _get_register(c, &kind);
    if (clip)
        _put_text(c, clip, kind, kind == REG_BLOCK ? c->v->yank_width + 1 : -1, after, count);
}

// J and gJ: `count` lines joined, at least two, J putting one space where the lines meet unless
// the next begins with ')' or either side is empty, and dropping the next line's indent. A count
// past the last line joins what there is; from the last line itself, three or more join nothing
// and only take the cursor to the line's start, where two fail. `counts`, when given, is each
// caret's own.
static bool _join(Ctx* c, int count0, bool spaces, const int* counts) {
    Carets k = _carets(c->el);
    Change* ch = calloc(k.count ? k.count : 1, sizeof(Change));
    bool any = false;
    for (size_t i = 0; k.sel && ch && i < k.count; i++) {
        const int want = counts ? counts[i] : count0;
        const int n = want > 2 ? want : 2;
        const size_t first = _lof(c, k.sel[i].head);
        ch[i].within = k.sel[i].head;
        if (first + 1 >= c->lines) {
            if (n > 2) {
                ch[i].within = _ls(c, first);
                any = true;
            }
            continue;
        }
        const size_t last = first + (size_t)n - 1 < c->lines ? first + (size_t)n - 1 : c->lines - 1;
        // What follows the first line's text once joined, built whole: each line after it, its
        // indent dropped with J, behind a space unless the line put after is empty or begins
        // with ')', or what it joins is empty or ends in a blank.
        size_t cap = 64, len = 0, at_join = 0;
        char* out = malloc(cap);
        if (!out)
            continue;
        size_t total = _ll(c, first);
        size_t prev_len = 0;
        const char* prev = _lt(c, first, &prev_len);
        uint32_t end = 0;
        if (prev_len) {
            const char* p = prev + prev_len;
            text_utf8_prev(&p, prev);
            end = text_utf8_next(&p);
        }
        for (size_t line = first + 1; line <= last; line++) {
            size_t ll = 0;
            const char* s = _lt(c, line, &ll);
            size_t skip = 0;
            if (spaces)
                while (skip < ll && _blank((unsigned char)s[skip]))
                    skip++;
            const bool space =
                spaces && skip < ll && s[skip] != ')' && total > 0 && end != '\t' && end != ' ';
            if (len + (ll - skip) + 2 > cap) {
                cap = (len + (ll - skip) + 2) * 2;
                char* grown = realloc(out, cap);
                if (!grown)
                    break;
                out = grown;
            }
            at_join = len;
            if (space)
                out[len++] = ' ';
            memcpy(out + len, s + skip, ll - skip);
            len += ll - skip;
            total += (ll - skip) + (space ? 1 : 0);
            end = 0;
            if (ll > skip) {
                const char* p = s + ll;
                text_utf8_prev(&p, s + skip);
                end = text_utf8_next(&p);
            }
        }
        ch[i].owned = out;
        ch[i].e = (TextEdit){_le(c, first), _le(c, last), out, len};
        ch[i].edits = true;
        // The cursor where the last two lines met.
        ch[i].within = at_join;
        any = true;
    }
    if (any) {
        _group(c, true);
        bool joins = false;
        for (size_t i = 0; i < k.count; i++)
            joins = joins || ch[i].edits;
        if (!joins)
            _empty_step(c);
        _apply(c, ch, k.count, k.primary);
        Ctx now = _ctx(c->v, c->el);
        size_t m = 0, primary = 0;
        size_t* at = _heads(c->el, &m, &primary);
        if (at)
            _place(&now, at, NULL, m, primary, true);
        free(at);
        _group(c, false);
    } else {
        for (size_t i = 0; ch && i < k.count; i++)
            free(ch[i].owned);
    }
    free(ch);
    _free_carets(&k);
    return any;
}

// r{c}: `count` characters from the cursor each made `ch`; refused past the line's end.
static bool _replace(Ctx* c, uint32_t ch, int count0) {
    const int n = count0 ? count0 : 1;
    Carets k = _carets(c->el);
    Change* x = calloc(k.count ? k.count : 1, sizeof(Change));
    bool any = false;
    for (size_t i = 0; k.sel && x && i < k.count; i++) {
        size_t at = k.sel[i].head;
        const size_t le = _le(c, _lof(c, at));
        size_t end = at;
        int got = 0;
        while (got < n && end < le) {
            end = _after(c, end);
            got++;
        }
        x[i].within = at;
        if (got < n)
            continue;
        char utf8[4];
        const size_t clen = ch == K_CR ? 0 : text_utf8_encode(ch, utf8);
        if (ch == K_CR) {
            size_t nl_len = strlen(_nl(c));
            x[i].owned = malloc(nl_len + 1);
            if (x[i].owned)
                memcpy(x[i].owned, _nl(c), nl_len + 1);
            x[i].e = (TextEdit){at, end, x[i].owned ? x[i].owned : "", x[i].owned ? nl_len : 0};
            x[i].within = nl_len;
        } else {
            x[i].owned = malloc(clen * (size_t)n + 1);
            if (!x[i].owned)
                continue;
            for (int t = 0; t < n; t++)
                memcpy(x[i].owned + clen * (size_t)t, utf8, clen);
            x[i].e = (TextEdit){at, end, x[i].owned, clen * (size_t)n};
            x[i].within = clen * (size_t)(n - 1);
        }
        x[i].edits = true;
        any = true;
    }
    if (any) {
        _group(c, true);
        _apply(c, x, k.count, k.primary);
        _group(c, false);
    } else {
        for (size_t i = 0; x && i < k.count; i++)
            free(x[i].owned);
    }
    free(x);
    _free_carets(&k);
    return any;
}

// ~: the case of `count` characters from the cursor swapped, the cursor going on past them.
static void _tilde(Ctx* c, int count0) {
    const int n = count0 ? count0 : 1;
    Carets k = _carets(c->el);
    Change* x = calloc(k.count ? k.count : 1, sizeof(Change));
    bool any = false;
    for (size_t i = 0; k.sel && x && i < k.count; i++) {
        const size_t at = k.sel[i].head;
        const size_t le = _le(c, _lof(c, at));
        x[i].within = at;
        if (at >= le)
            continue;
        size_t end = at;
        for (int t = 0; t < n && end < le; t++)
            end = _after(c, end);
        size_t len = 0;
        x[i].owned = _cased(c, at, end, '~', &len);
        if (!x[i].owned)
            continue;
        x[i].e = (TextEdit){at, end, x[i].owned, len};
        x[i].within = len;
        x[i].edits = true;
        any = true;
    }
    if (any) {
        _group(c, true);
        _apply(c, x, k.count, k.primary);
        Ctx now = _ctx(c->v, c->el);
        size_t m = 0, primary = 0;
        size_t* at = _heads(c->el, &m, &primary);
        if (at)
            _place(&now, at, NULL, m, primary, true);
        free(at);
        _group(c, false);
    }
    free(x);
    _free_carets(&k);
}

// ---- Insert mode

// Into insert mode at each caret, as `cmd` puts it: i before the cursor, a after it, I at the
// line's first character not a blank, A at its end, o and O on a new line below or above, keeping
// the line's indent; c has put the carets already. The insert's step begins where its typing
// does, or for o and O on the line the cursor left.
static void _enter_insert(Ctx* c, uint32_t cmd, int count) {
    UIEditorVim* v = c->v;
    if (cmd == 'o' || cmd == 'O')
        _group(c, true);
    v->indent_pending = false;
    if (cmd == 'o' || cmd == 'O') {
        Carets k = _carets(c->el);
        Change* x = calloc(k.count ? k.count : 1, sizeof(Change));
        for (size_t i = 0; k.sel && x && i < k.count; i++) {
            const size_t line = _lof(c, k.sel[i].head);
            size_t ind = 0;
            char* indent = _indent_of(c, line, &ind);
            const char* nl = _nl(c);
            const size_t nl_len = strlen(nl);
            x[i].owned = malloc(nl_len + ind + 1);
            if (!indent || !x[i].owned) {
                free(indent);
                continue;
            }
            if (cmd == 'o') {
                memcpy(x[i].owned, nl, nl_len);
                memcpy(x[i].owned + nl_len, indent, ind);
                x[i].e = (TextEdit){_le(c, line), _le(c, line), x[i].owned, nl_len + ind};
                x[i].within = nl_len + ind;
            } else {
                memcpy(x[i].owned, indent, ind);
                memcpy(x[i].owned + ind, nl, nl_len);
                x[i].e = (TextEdit){_ls(c, line), _ls(c, line), x[i].owned, nl_len + ind};
                x[i].within = ind;
            }
            x[i].edits = true;
            free(indent);
        }
        if (k.sel && x)
            _apply(c, x, k.count, k.primary);
        free(x);
        _free_carets(&k);
        v->indent_pending = true;
    } else if (cmd != 'c' && cmd != 'i') {
        Carets k = _carets(c->el);
        size_t* at = calloc(k.count ? k.count : 1, sizeof(size_t));
        for (size_t i = 0; k.sel && at && i < k.count; i++) {
            const size_t head = k.sel[i].head, line = _lof(c, head);
            at[i] = cmd == 'a'   ? (head < _le(c, line) ? _after(c, head) : head)
                    : cmd == 'A' ? _le(c, line)
                                 : (_empty(c, line) ? _ls(c, line) : _first_nonblank(c, line));
            if (cmd == 'I' && at[i] < _le(c, line) && _blank(_gc(c, at[i])))
                at[i] = _le(c, line);
        }
        if (k.sel && at)
            _place(c, at, NULL, k.count, k.primary, false);
        free(at);
        _free_carets(&k);
    }
    if (cmd != 'c')
        _group(c, true);
    v->insert_cmd = cmd;
    v->insert_count = count ? count : 1;
    v->insert_from = v->key_count;
    _set_state(v, UI_VIM_INSERT);
    _reset(v);
}

// A line's blanks taken out where an Enter or an `o` put them and nothing was typed after.
static void _drop_indent(Ctx* c) {
    if (!c->v->indent_pending)
        return;
    c->v->indent_pending = false;
    Carets k = _carets(c->el);
    Change* x = calloc(k.count ? k.count : 1, sizeof(Change));
    bool any = false;
    for (size_t i = 0; k.sel && x && i < k.count; i++) {
        const size_t head = k.sel[i].head, line = _lof(c, head);
        x[i].within = head;
        if (head != _le(c, line) || head == _ls(c, line) || !_in_indent(c, head))
            continue;
        x[i].e = (TextEdit){_ls(c, line), head, "", 0};
        x[i].edits = true;
        x[i].within = 0;
        any = true;
    }
    if (any)
        _apply(c, x, k.count, k.primary);
    free(x);
    _free_carets(&k);
}

static bool _feed(UIEditorVim* v, UIElement* el, uint32_t k);

// Esc: an insert given a count typed again count less one times -- an `o` or `O` opening a line
// each time -- the indent nothing followed taken out, the cursor back onto the character before
// it, and normal mode. The keys kept for `.` are the insert's own, the repeats aside.
static void _leave_insert(Ctx* c) {
    UIEditorVim* v = c->v;
    const size_t kept = v->key_count;
    if (v->insert_count > 1) {
        const size_t from = v->insert_from, to = kept ? kept - 1 : 0;
        const size_t n = to > from ? to - from : 0;
        uint32_t* typed = malloc((n ? n : 1) * sizeof(uint32_t));
        const int times = v->insert_count - 1;
        const uint32_t cmd = v->insert_cmd;
        v->insert_count = 1;
        if (typed) {
            memcpy(typed, v->keys + from, n * sizeof(uint32_t));
            v->replaying++;
            for (int t = 0; t < times; t++) {
                if (cmd == 'o' || cmd == 'O') {
                    Ctx now = _ctx(v, c->el);
                    _drop_indent(&now);
                    _enter_insert(&now, cmd, 1);
                }
                for (size_t i = 0; i < n; i++)
                    _feed(v, c->el, typed[i]);
            }
            v->replaying--;
        }
        free(typed);
    }
    v->key_count = kept;
    Ctx now = _ctx(v, c->el);
    _drop_indent(&now);
    Ctx after = _ctx(v, c->el);
    size_t n = 0, primary = 0;
    size_t* at = _heads(c->el, &n, &primary);
    for (size_t i = 0; at && i < n; i++) {
        const size_t line = _lof(&after, at[i]);
        if (at[i] > _ls(&after, line))
            at[i] = _before(&after, at[i]);
    }
    // An insert a block began leaves one cursor, at the block's top left.
    if (at && v->block_insert && v->insert_top < after.lines) {
        at[0] = _at_col(&after, v->insert_top, v->insert_left, false);
        n = 1;
        primary = 0;
    }
    v->block_insert = false;
    _set_state(v, UI_VIM_NORMAL);
    if (at)
        _place(&after, at, NULL, n, primary, true);
    free(at);
    _group(c, false);
    _keep_change(v, v->change_count);
    _reset(v);
}

// An arrow in insert mode: the cursor moves, and what is typed after it is a step and a change of
// its own, as though the insert began there.
static void _insert_move(Ctx* c, uint32_t k) {
    UIEditorVim* v = c->v;
    _group(c, false);
    // The change so far is kept as though Esc had ended it.
    if (v->key_count < KEYS_MAX)
        v->keys[v->key_count++] = K_ESC;
    _keep_change(v, v->change_count);
    v->indent_pending = false;
    v->block_insert = false;
    const UIEditorMotion m = k == K_LEFT    ? UI_MOTION_CHAR_LEFT
                             : k == K_RIGHT ? UI_MOTION_CHAR_RIGHT
                             : k == K_UP    ? UI_MOTION_LINE_UP
                             : k == K_DOWN  ? UI_MOTION_LINE_DOWN
                             : k == K_HOME  ? UI_MOTION_ROW_START
                                            : UI_MOTION_ROW_END;
    if (m == UI_MOTION_CHAR_LEFT || m == UI_MOTION_CHAR_RIGHT) {
        // Within the line: an arrow stops at its ends.
        Carets kk = _carets(c->el);
        size_t* at = calloc(kk.count ? kk.count : 1, sizeof(size_t));
        for (size_t i = 0; kk.sel && at && i < kk.count; i++) {
            const size_t h = kk.sel[i].head, line = _lof(c, h);
            at[i] = m == UI_MOTION_CHAR_LEFT ? (h > _ls(c, line) ? _before(c, h) : h)
                                             : (h < _le(c, line) ? _after(c, h) : h);
        }
        if (kk.sel && at)
            _place(c, at, NULL, kk.count, kk.primary, false);
        free(at);
        _free_carets(&kk);
    } else {
        ui_editor_apply(c->el, UI_OP_MOVE, m, 1);
    }
    _group(c, true);
    v->key_count = 0;
    _record(v, 'i');
    v->insert_cmd = 'i';
    v->insert_count = 1;
    v->insert_from = v->key_count;
    v->change_count = 0;
}

// Enter in insert mode, as vim's autoindent makes it: each caret's line broken there, the new line
// indented as the text before the caret is and the blanks after the caret dropped. A line of only
// the indent an Enter or an `o` put in, nothing typed after it, loses its blanks.
static void _insert_break(Ctx* c) {
    UIEditorVim* v = c->v;
    Carets k = _carets(c->el);
    Change* x = calloc(k.count ? k.count : 1, sizeof(Change));
    const char* nl = _nl(c);
    const size_t nl_len = strlen(nl);
    bool indented = false;
    for (size_t i = 0; k.sel && x && i < k.count; i++) {
        const size_t h = k.sel[i].head, line = _lof(c, h), at = _ls(c, line), end = _le(c, line);
        size_t len = 0;
        const char* s = _lt(c, line, &len);
        size_t ind = 0;
        while (ind < h - at && _blank((unsigned char)s[ind]))
            ind++;
        size_t skip = h;
        while (skip < end && _blank(_gc(c, skip)))
            skip = _after(c, skip);
        size_t from = h;
        if (v->indent_pending)
            while (from > at && _blank((unsigned char)s[from - at - 1]))
                from--;
        x[i].owned = malloc(nl_len + ind + 1);
        if (!x[i].owned)
            continue;
        memcpy(x[i].owned, nl, nl_len);
        memcpy(x[i].owned + nl_len, s, ind);
        x[i].e = (TextEdit){from, skip, x[i].owned, nl_len + ind};
        x[i].within = nl_len + ind;
        x[i].edits = true;
        indented = indented || ind > 0;
    }
    if (k.sel && x)
        _apply(c, x, k.count, k.primary);
    free(x);
    _free_carets(&k);
    v->indent_pending = indented;
}

static bool _insert_key(Ctx* c, uint32_t k) {
    UIEditorVim* v = c->v;
    switch (k) {
        case K_ESC:
            _record(v, k);
            _leave_insert(c);
            return true;
        case K_CR:
            _record(v, k);
            _insert_break(c);
            return true;
        case K_BS:
        case K_CTRL('h'):
            _record(v, k);
            v->indent_pending = false;
            ui_editor_run(c->el, "delete-left");
            return true;
        case K_DEL:
            _record(v, k);
            ui_editor_run(c->el, "delete-right");
            return true;
        case K_TAB:
            _record(v, k);
            v->indent_pending = false;
            ui_editor_run(c->el, "tab");
            return true;
        case K_CTRL('w'):
        case K_CTRL('u'): {
            _record(v, k);
            v->indent_pending = false;
            Carets kk = _carets(c->el);
            Change* x = calloc(kk.count ? kk.count : 1, sizeof(Change));
            for (size_t i = 0; kk.sel && x && i < kk.count; i++) {
                const size_t h = kk.sel[i].head, line = _lof(c, h);
                size_t from = h;
                if (k == K_CTRL('u')) {
                    const size_t first = _first_nonblank(c, line);
                    from = h > first ? first : _ls(c, line);
                } else if (h > _ls(c, line)) {
                    size_t len = 0;
                    const char* s = _lt(c, line, &len);
                    from = _ls(c, line) + ui_typing_word_left(s, len, h - _ls(c, line));
                } else if (line > 0) {
                    from = _le(c, line - 1);
                }
                x[i].e = (TextEdit){from, h, "", 0};
                x[i].edits = from < h;
                x[i].within = x[i].edits ? 0 : h;
            }
            if (kk.sel && x)
                _apply(c, x, kk.count, kk.primary);
            free(x);
            _free_carets(&kk);
            return true;
        }
        case K_LEFT:
        case K_RIGHT:
        case K_UP:
        case K_DOWN:
        case K_HOME:
        case K_END:
            _insert_move(c, k);
            return true;
        case K_PASTE: {
            Engine* engine = ui_editor_engine(c->el);
            const char* clip = engine ? engine_clipboard_get(engine) : NULL;
            if (clip && *clip) {
                ui_editor_type(c->el, clip);
                v->indent_pending = false;
            }
            return true;
        }
        default:
            if (k >= K_ESC)
                return false;
            _record(v, k);
            v->indent_pending = false;
            char utf8[5] = {0};
            if (text_utf8_encode(k, utf8))
                ui_editor_type(c->el, utf8);
            return true;
    }
}

// ---- Visual mode

// The block's lines and columns: from the leftmost of its corners' columns to the last column of
// the rightmost corner's character -- or to each line's end, INT_MAX, after `$`.
typedef struct {
    size_t top, bottom;
    int left, right;
} Box;

static Box _box(const Ctx* c) {
    const UIEditorVim* v = c->v;
    const size_t la = _lof(c, v->block_anchor), lh = _lof(c, v->block_head);
    Box b = {la < lh ? la : lh, la < lh ? lh : la, v->box_left, v->box_right};
    if (v->box_given)
        return b;
    const int sa = _vcol(c, v->block_anchor), sh = _vcol(c, v->block_head);
    const int ea = _vcol_end(c, v->block_anchor) - 1, eh = _vcol_end(c, v->block_head) - 1;
    b.left = sa < sh ? sa : sh;
    b.right = v->block_end ? INT_MAX : ea > eh ? ea : eh;
    return b;
}

// What the box takes of one line: the bytes [lo, hi) of the characters it touches -- nothing, at
// the line's end, when the line only reaches it -- and the columns of a tab at either edge lying
// outside it, `before` its left and `after` its right, which vim keeps as spaces when it splits the
// tab. `reach` is false for a line ending before the box's left column.
typedef struct {
    size_t lo, hi;
    int before, after;
    bool reach;
} Cut;

static Cut _cut(const Ctx* c, size_t line, const Box* b) {
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    const size_t at = _ls(c, line);
    Cut k = {at + len, at + len, 0, 0, false};
    int col = 0;
    bool in = false;
    for (size_t i = 0; i < len && col <= b->right;) {
        const size_t was = i;
        const int start = col;
        const uint32_t cp = ui_typing_next(s, len, &i);
        col += cp == '\t' ? c->tab - col % c->tab : 1;
        if (!in && col > b->left) {
            k.lo = at + was;
            k.before = b->left > start ? b->left - start : 0;
            in = true;
        }
        if (in) {
            k.hi = at + i;
            k.after = b->right != INT_MAX && col - 1 > b->right ? col - 1 - b->right : 0;
        }
    }
    k.reach = in || col >= b->left;
    return k;
}

static char* _spaces(char* out, int n) {
    if (n > 0)
        memset(out, ' ', (size_t)n);
    return out + (n > 0 ? n : 0);
}

// What a cut becomes under `op`, for the caller to free: its text for a yank, with an edge tab
// counted only for its columns in the box, as spaces; nothing for d and c, `with` for p, and `with`
// in every column for r -- each, for an edit, between the columns of edge tabs outside the box kept
// as spaces.
static char* _cut_text(const Ctx* c, const Cut* k, uint32_t op, const char* with, size_t wlen,
                       size_t* n) {
    const bool edit = op != 'y';
    const int cols = _vcol(c, k->hi) - _vcol(c, k->lo);
    char* out = malloc((size_t)(cols + 2) * 4 + (k->hi - k->lo) + wlen + 1);
    if (!out)
        return NULL;
    char* w = edit ? _spaces(out, k->before) : out;
    if (op == 'p') {
        memcpy(w, with, wlen);
        w += wlen;
    }
    for (size_t p = k->lo; p < k->hi; p = _after(c, p)) {
        const size_t next = _after(c, p);
        const int width = _vcol_end(c, p) - _vcol(c, p);
        const int inside = width - (p == k->lo ? k->before : 0) - (next == k->hi ? k->after : 0);
        if (op == 'r') {
            for (int i = 0; i < inside; i++, w += wlen)
                memcpy(w, with, wlen);
        } else if (op == 'y' && inside < width) {
            w = _spaces(w, inside);
        } else if (op == 'y') {
            w += text_doc_copy(c->doc, p, next, w);
        }
    }
    if (edit)
        w = _spaces(w, k->after);
    *w = '\0';
    *n = (size_t)(w - out);
    return out;
}

// Block mode's look: a selection over each line's piece, its head at the side of the block the
// cursor is on, and on the cursor's own line the cursor itself.
static void _show_block(Ctx* c) {
    UIEditorVim* v = c->v;
    const Box b = _box(c);
    const size_t hl = _lof(c, v->block_head);
    const bool right = _vcol(c, v->block_head) >= _vcol(c, v->block_anchor);
    TextSelection* s = calloc(b.bottom - b.top + 1, sizeof(TextSelection));
    if (!s)
        return;
    size_t n = 0, primary = 0;
    for (size_t line = b.top; line <= b.bottom; line++) {
        const Cut k = _cut(c, line, &b);
        const size_t lo = k.lo, hi = k.hi;
        const bool some = lo < hi;
        const size_t last = some ? _before(c, hi) : lo;
        if (line == hl) {
            primary = n;
            s[n++] = (TextSelection){some ? (right ? lo : last) : v->block_head, v->block_head};
        } else if (some) {
            s[n++] = right ? (TextSelection){lo, last} : (TextSelection){last, lo};
        }
    }
    ui_editor_set_carets(c->el, s, NULL, n, primary);
    free(s);
}

// Into visual mode `to`: from normal mode the area begins at each cursor; from another visual mode
// it keeps its ends, a block's being its corners.
static void _visual(Ctx* c, UIEditorVimState to) {
    UIEditorVim* v = c->v;
    const UIEditorVimState was = v->state;
    Carets k = _carets(c->el);
    if (!k.sel) {
        _free_carets(&k);
        return;
    }
    if (was == UI_VIM_VISUAL_BLOCK) {
        k.count = 1;
        k.primary = 0;
        k.sel[0] = (TextSelection){v->block_anchor, v->block_head};
        k.want[0] = v->block_want;
    } else if (was < UI_VIM_VISUAL) {
        for (size_t i = 0; i < k.count; i++)
            k.sel[i].anchor = k.sel[i].head;
    }
    _set_state(v, to);
    c->visual = true;
    v->box_given = false;
    if (to == UI_VIM_VISUAL_BLOCK) {
        v->block_anchor = k.sel[k.primary].anchor;
        v->block_head = k.sel[k.primary].head;
        v->block_want = k.want[k.primary];
        v->block_end = v->block_want == INT_MAX;
        _show_block(c);
    } else {
        ui_editor_set_carets(c->el, k.sel, k.want, k.count, k.primary);
    }
    _free_carets(&k);
}

// Out of visual mode, each cursor where its area's head was, on a character; a block's at its
// head.
static void _visual_leave(Ctx* c) {
    UIEditorVim* v = c->v;
    const bool block = v->state == UI_VIM_VISUAL_BLOCK;
    _set_state(v, UI_VIM_NORMAL);
    c->visual = false;
    v->box_given = false;
    if (block) {
        const size_t h = v->block_head;
        _place(c, &h, &v->block_want, 1, 0, true);
        return;
    }
    Carets k = _carets(c->el);
    size_t* at = calloc(k.count ? k.count : 1, sizeof(size_t));
    for (size_t i = 0; k.sel && at && i < k.count; i++)
        at[i] = k.sel[i].head;
    if (k.sel && at)
        _place(c, at, k.want, k.count, k.primary, true);
    free(at);
    _free_carets(&k);
}

// o: each area's ends swapped. O in block mode: the cursor to the block's other side on its own
// line -- its right edge, or its left when it is there already -- and the other corner across.
static void _visual_swap(Ctx* c, bool across) {
    UIEditorVim* v = c->v;
    if (v->state == UI_VIM_VISUAL_BLOCK) {
        if (across) {
            const Box b = _box(c);
            const size_t la = _lof(c, v->block_anchor), lh = _lof(c, v->block_head);
            size_t head = _at_col(c, lh, b.right, true), anchor = _at_col(c, la, b.left, true);
            if (head == v->block_head) {
                head = _at_col(c, lh, b.left, true);
                anchor = _at_col(c, la, b.right, true);
            }
            v->block_head = head;
            v->block_anchor = anchor;
        } else {
            const size_t h = v->block_head;
            v->block_head = v->block_anchor;
            v->block_anchor = h;
        }
        v->block_want = v->block_end ? INT_MAX : -1;
        _show_block(c);
        return;
    }
    Carets k = _carets(c->el);
    for (size_t i = 0; k.sel && i < k.count; i++) {
        k.sel[i] = (TextSelection){k.sel[i].head, k.sel[i].anchor};
        k.want[i] = -1;
    }
    if (k.sel)
        ui_editor_set_carets(c->el, k.sel, k.want, k.count, k.primary);
    _free_carets(&k);
}

// A motion in visual mode: each area's head moves, its other end stays; a block's cursor moves.
static bool _visual_move(Ctx* c, uint32_t prefix, uint32_t key, uint32_t arg, int count0) {
    UIEditorVim* v = c->v;
    if (v->state == UI_VIM_VISUAL_BLOCK) {
        const Moved m = _motion(c, prefix, key, arg, v->block_head, count0, 0, v->block_want);
        if (m.ok) {
            v->block_head = m.to;
            if (m.want != WANT_KEEP)
                v->block_want = m.want;
            v->block_end = v->block_want == INT_MAX;
        }
        _show_block(c);
        return m.ok;
    }
    Carets k = _carets(c->el);
    bool moved = false;
    for (size_t i = 0; k.sel && i < k.count; i++) {
        const Moved m = _motion(c, prefix, key, arg, k.sel[i].head, count0, 0, k.want[i]);
        if (!m.ok)
            continue;
        k.sel[i].head = m.to;
        k.want[i] = m.want == WANT_KEEP ? k.want[i] : m.want;
        moved = true;
    }
    if (k.sel)
        ui_editor_set_carets(c->el, k.sel, k.want, k.count, k.primary);
    _free_carets(&k);
    return moved;
}

// i( a( and the like in visual mode: from an area of one character, the block round it as an
// operator takes it; from a larger one, the block round the area -- and for i(, when what is
// inside it is no larger than the area, the block round that one. A closer on a line of its own
// leaves the area ending on the line break before it.
static Obj _visual_block_obj(const Ctx* c, size_t a, size_t h, int count, bool include,
                             uint32_t open, uint32_t close) {
    Obj o = {h, h, true, false, false};
    size_t p = h, old_start = h, old_end = h;
    if (a == h) {
        if (open == '{')
            while (_in_indent_by(c, p, 1))
                if (_inc(c, &p) != 0)
                    break;
        if (_gc(c, p) == open)
            p = _after(c, p);
    } else if (a < h) {
        old_start = p = a;
    } else {
        old_end = a;
    }
    size_t start = SIZE_MAX;
    const bool inside = _unclosed_before(c, p, open, close) != SIZE_MAX;
    for (; count > 0; count--) {
        const size_t q = inside ? _unclosed_before(c, p, open, close) : _next_open(c, p, open);
        if (q == SIZE_MAX)
            return o;
        p = start = q;
    }
    size_t end = start == SIZE_MAX ? SIZE_MAX : _unclosed_after(c, start, open, close);
    if (end == SIZE_MAX)
        return o;
    bool sol = false;
    while (!include) {
        _incl(c, &start);
        sol = end == _ls(c, _lof(c, end));
        _decl(c, &end);
        while (_in_indent_by(c, end, 1)) {
            sol = true;
            if (_decl(c, &end) != 0)
                break;
        }
        if (start < old_start || old_end < end || start == end)
            break;
        size_t q = old_start;
        _decl(c, &q);
        start = _unclosed_before(c, q, open, close);
        end = start == SIZE_MAX ? SIZE_MAX : _unclosed_after(c, start, open, close);
        if (end == SIZE_MAX)
            return o;
    }
    if (sol && _gc(c, end))
        _inc(c, &end);
    o.start = start;
    o.end = end;
    o.ok = true;
    return o;
}

// ip and ap growing a visual area that spans lines: `count` more paragraphs, or runs of blank
// lines, onto the end the cursor is at -- ap a paragraph with the blank lines after it. The line
// the cursor goes to the start of; false when the text ends first.
static bool _par_more(const Ctx* c, size_t* line, size_t anchor_line, int count, bool include) {
    const int dir = *line < anchor_line ? -1 : 1;
    const size_t edge = dir < 0 ? 0 : c->lines - 1;
    while (count-- > 0) {
        if (*line == edge)
            return false;
        int prev_white = -1;
        for (int t = 0; t < 2; t++) {
            *line = (size_t)((long)*line + dir);
            const int white = _white_line(c, *line);
            if (white == prev_white) {
                *line = (size_t)((long)*line - dir);
                break;
            }
            while (*line != edge && _white_line(c, (size_t)((long)*line + dir)) == white)
                *line = (size_t)((long)*line + dir);
            if (!include || *line == edge)
                break;
            prev_white = white;
        }
    }
    return true;
}

// The opening bracket a block object's key names, or 0 for a key that names none.
static uint32_t _block_open(uint32_t key) {
    switch (key) {
        case '(':
        case ')':
        case 'b':
            return '(';
        case '[':
        case ']':
            return '[';
        case '{':
        case '}':
        case 'B':
            return '{';
        case '<':
        case '>':
            return '<';
        default:
            return 0;
    }
}

// i and a objects in visual mode: each area made the object at its cursor -- characters for words,
// blocks and quotes, lines for paragraphs -- or, when it already covers more than a character, a
// word or a paragraph grown by more and a block by the one round it. A word's cursor goes as far
// as the search got even when the text ends first; so does a paragraph's.
static void _visual_object(Ctx* c, uint32_t ia, uint32_t key, int count0) {
    UIEditorVim* v = c->v;
    if (v->state == UI_VIM_VISUAL_BLOCK)
        return;
    const bool include = ia == 'a';
    const int n = count0 ? count0 : 1;
    const bool par = key == 'p', word = key == 'w' || key == 'W';
    const bool quote = key == '"' || key == '\'' || key == '`';
    const uint32_t open = _block_open(key);
    Carets k = _carets(c->el);
    bool any = false;
    for (size_t i = 0; k.sel && i < k.count; i++) {
        const size_t a = k.sel[i].anchor, h = k.sel[i].head;
        TextSelection to = {a, h};
        bool ok = true;
        if (word && a != h) {
            bool inclusive = true;
            _word_more(c, &to.head, n, include, key == 'W', h < a, &inclusive);
        } else if (word) {
            const Obj o = _obj_word(c, h, n, include, key == 'W');
            to = (TextSelection){o.ok ? o.start : a, o.end};
        } else if (par) {
            // In V mode a paragraph found starting at the cursor's own line is grown instead,
            // or ip again on one blank line would find the same line.
            const Obj o = _obj_par(c, h, n, include);
            const bool grow = _lof(c, a) != _lof(c, h) || (v->state == UI_VIM_VISUAL_LINE && o.ok &&
                                                           _lof(c, o.start) == _lof(c, h));
            if (grow) {
                size_t line = _lof(c, h);
                _par_more(c, &line, _lof(c, a), n, include);
                to.head = _ls(c, line);
            } else if ((ok = o.ok)) {
                to = (TextSelection){_lof(c, a) == _lof(c, o.start) ? a : o.start, o.end};
            }
        } else if (quote) {
            // As an operator takes it, less one character at its end.
            const Obj o = _obj_quote(c, h, n, include, (char)key);
            const size_t hi =
                o.inclusive && o.end < _le(c, _lof(c, o.end)) ? _after(c, o.end) : o.end;
            to = (TextSelection){o.start, hi > o.start ? _before(c, hi) : hi};
            ok = o.ok;
        } else if (open) {
            const uint32_t close = open == '(' ? ')' : open == '[' ? ']' : open == '{' ? '}' : '>';
            const Obj o = _visual_block_obj(c, a, h, n, include, open, close);
            to = (TextSelection){o.start, o.end};
            ok = o.ok;
        } else {
            ok = false;
        }
        if (!ok)
            continue;
        k.sel[i] = to;
        k.want[i] = -1;
        any = true;
    }
    if (any) {
        _set_state(v, par ? UI_VIM_VISUAL_LINE : UI_VIM_VISUAL);
        ui_editor_set_carets(c->el, k.sel, k.want, k.count, k.primary);
    }
    _free_carets(&k);
}

// The area of the primary caret, kept for `.` to make again -- and for a count before v to select
// again, which a yank's is too.
static void _note_area(Ctx* c, UIEditorVimState mode, size_t a, size_t h, int want) {
    const size_t lo = a < h ? a : h, e = a < h ? h : a;
    const size_t lines = _lof(c, e) - _lof(c, lo) + 1;
    int cols = 0;
    if (mode == UI_VIM_VISUAL)
        cols = want == INT_MAX ? INT_MAX
               : lines <= 1    ? _vcol_end(c, e) - _vcol(c, lo)
                               : _vcol_end(c, e) - 1;
    c->v->area = c->v->reselect = (VisualArea){mode, lines, cols, true};
}

static void _note_box(Ctx* c, const Box* b) {
    c->v->area = c->v->reselect =
        (VisualArea){UI_VIM_VISUAL_BLOCK, b->bottom - b->top + 1,
                     b->right == INT_MAX ? INT_MAX : b->right - b->left + 1, true};
}

// The column the cursor shows at: a tab's last, as vim's normal mode shows it.
static int _shown_col(const Ctx* c, size_t off) {
    return _gc(c, off) == '\t' ? _vcol_end(c, off) - 1 : _vcol(c, off);
}

// The area `.` repeats, made again at each caret as vim makes it: as many lines down, and as many
// columns on, or to the same last column, or each line's end -- where it may reach the line's end.
static void _visual_again(Ctx* c, const VisualArea* a) {
    UIEditorVim* v = c->v;
    Carets k = _carets(c->el);
    if (!k.sel) {
        _free_carets(&k);
        return;
    }
    for (size_t i = 0; i < k.count; i++) {
        const size_t h = k.sel[i].head, line = _lof(c, h);
        const size_t last = line + a->lines - 1 < c->lines ? line + a->lines - 1 : c->lines - 1;
        int want = _vcol(c, h);
        if (a->mode == UI_VIM_VISUAL)
            want = a->cols == INT_MAX ? INT_MAX
                   : a->lines <= 1    ? _shown_col(c, h) + a->cols - 1
                                      : a->cols;
        k.sel[i] = (TextSelection){h, _at_col(c, last, want, true)};
        k.want[i] = a->mode == UI_VIM_VISUAL ? want : -1;
    }
    _set_state(v, a->mode);
    c->visual = true;
    if (a->mode == UI_VIM_VISUAL_BLOCK) {
        const TextSelection p = k.sel[k.primary];
        v->block_anchor = p.anchor;
        v->block_head = p.head;
        v->block_want = -1;
        v->block_end = a->cols == INT_MAX;
        v->box_given = true;
        v->box_left = _vcol(c, p.anchor);
        v->box_right = a->cols == INT_MAX ? INT_MAX : v->box_left + a->cols - 1;
        _show_block(c);
    } else {
        ui_editor_set_carets(c->el, k.sel, k.want, k.count, k.primary);
    }
    _free_carets(&k);
}

// v, V and Ctrl+V with a count: the area the last visual operator took, made again at the cursor in
// its own mode `count` times as large -- or, before any, `count` characters or lines.
static void _visual_count(Ctx* c, UIEditorVimState mode, int count) {
    UIEditorVim* v = c->v;
    const VisualArea r = v->reselect;
    if (!count || !r.on) {
        _visual(c, mode);
        if (count > 1)
            _visual_move(c, 0, mode == UI_VIM_VISUAL_LINE ? 'j' : 'l', 0, count - 1);
        return;
    }
    _visual(c, r.mode);
    Carets k = _carets(c->el);
    for (size_t i = 0; k.sel && i < k.count; i++) {
        const size_t h = k.sel[i].head;
        size_t line = _lof(c, h);
        if (r.mode != UI_VIM_VISUAL || r.lines > 1) {
            const size_t down = r.lines * (size_t)count - 1;
            line = line + down < c->lines ? line + down : c->lines - 1;
        }
        int want = _vcol(c, h);
        if (r.cols == INT_MAX)
            want = INT_MAX;
        else if (r.mode == UI_VIM_VISUAL_BLOCK || (r.mode == UI_VIM_VISUAL && r.lines <= 1))
            want = _shown_col(c, h) + r.cols * count - 1;
        else if (r.mode == UI_VIM_VISUAL)
            want = r.cols;
        k.sel[i].head = _at_col(c, line, want, true);
        k.want[i] = r.mode == UI_VIM_VISUAL_LINE ? -1 : want;
    }
    if (k.sel && r.mode == UI_VIM_VISUAL_BLOCK) {
        v->block_head = k.sel[k.primary].head;
        v->block_want = k.want[k.primary];
        v->block_end = v->block_want == INT_MAX;
        _show_block(c);
    } else if (k.sel) {
        ui_editor_set_carets(c->el, k.sel, k.want, k.count, k.primary);
    }
    _free_carets(&k);
}

// Each caret's visual area as an operator takes it: its characters from one end to the other with
// the far one's in -- from a line's end, its line break, unless it is the last line's -- or with
// `lines` the lines it touches. And in `start`, where vim starts the operator: the area's first
// character, or in a mode of lines (`line_mode`) the start of the line the area began on when that
// is the first, else the cursor.
static void _visual_ranges(const Ctx* c, const Carets* k, bool lines, bool line_mode, Range* r,
                           size_t* start) {
    for (size_t i = 0; i < k->count; i++) {
        const size_t a = k->sel[i].anchor, h = k->sel[i].head;
        const size_t lo = a < h ? a : h, e = a < h ? h : a;
        Range* x = &r[i];
        *x = (Range){lo, lo, _lof(c, lo), _lof(c, e), lines, true};
        start[i] = line_mode && _lof(c, a) <= _lof(c, h) ? _ls(c, _lof(c, a)) : lo;
        if (lines) {
            x->lo = _ls(c, x->first);
            x->hi =
                x->last + 1 < c->lines || _ends_broken(c) ? _ls(c, x->last + 1) : _le(c, x->last);
            continue;
        }
        x->hi = e < _le(c, x->last)      ? _after(c, e)
                : x->last + 1 < c->lines ? _ls(c, x->last + 1)
                                         : e;
    }
}

static void _block_operate(Ctx* c, uint32_t op, bool to_end, int count0, const char* with,
                           size_t wlen);

// An operator over each visual area: d c y < > and the case ones, `upper` for the uppercase
// commands, which take the area's lines whole -- in block mode, D and C to each line's end. Visual
// mode ends; c goes on into insert mode.
static void _visual_operate(Ctx* c, uint32_t op, bool upper, int count0) {
    UIEditorVim* v = c->v;
    if (v->state == UI_VIM_VISUAL_BLOCK) {
        _block_operate(c, op, upper, count0, NULL, 0);
        return;
    }
    const bool line_mode = upper || v->state == UI_VIM_VISUAL_LINE;
    const bool lines = line_mode || op == '<' || op == '>';
    Carets k = _carets(c->el);
    Range* r = calloc(k.count ? k.count : 1, sizeof(Range));
    size_t* start = calloc(k.count ? k.count : 1, sizeof(size_t));
    if (k.sel && r && start) {
        _visual_ranges(c, &k, lines, line_mode, r, start);
        const TextSelection p = k.sel[k.primary];
        _note_area(c, upper ? UI_VIM_VISUAL_LINE : v->state, p.anchor, p.head, k.want[k.primary]);
        v->area.on = op != 'y';
        for (size_t i = 0; i < k.count; i++)
            k.want[i] = -1;
        _set_state(v, UI_VIM_NORMAL);
        c->visual = false;
        _operate_on(c, op, &k, r, start, count0 ? count0 : 1);
    }
    _free_carets(&k);
    free(r);
    free(start);
}

// The block's top line at its left column, where every block operator leaves the cursor.
static size_t _top_left(const Ctx* c, const Box* b) {
    return _hold(c, _at_col(c, b->top, b->left, false));
}

// The blanks starting at `lo` on its line rebuilt as spaces, `by` columns more -- fewer when
// negative, never below none -- as vim's block shift with expandtab does. NULL when `lo` is not on
// a blank and nothing is taken away.
static char* _block_shifted(const Ctx* c, size_t lo, int by, size_t* run_end, size_t* n) {
    const size_t le = _le(c, _lof(c, lo));
    size_t e = lo;
    while (e < le && _blank(_gc(c, e)))
        e = _after(c, e);
    *run_end = e;
    const int had = _vcol(c, e) - _vcol(c, lo);
    if (by < 0 && !had)
        return NULL;
    const int now = had + by > 0 ? had + by : 0;
    char* sp = malloc((size_t)now + 1);
    if (!sp)
        return NULL;
    memset(sp, ' ', (size_t)now);
    sp[now] = '\0';
    *n = (size_t)now;
    return sp;
}

// An operator over the block, on each line the piece between its columns: d c y < > and the case
// ones, r with `with` the character, p with `with` the text that replaces each piece; with
// `to_end`, D and C, to each line's end. A yank, a delete or a put keeps the pieces as a block. A
// tab on the block's edge is split into spaces, which keeps every column where it was. The cursor
// goes to the block's top line at its left column; c goes into insert mode at each line the block
// reaches, as I does.
static void _block_operate(Ctx* c, uint32_t op, bool to_end, int count0, const char* with,
                           size_t wlen) {
    UIEditorVim* v = c->v;
    if (to_end && (op == 'd' || op == 'c'))
        v->block_end = true;
    const Box b = _box(c);
    _note_box(c, &b);
    v->area.on = op != 'y';
    const size_t n = b.bottom - b.top + 1;
    Cut* cut = calloc(n, sizeof(Cut));
    TextEdit* edits = calloc(n, sizeof(TextEdit));
    char** owned = calloc(n, sizeof(char*));
    _set_state(v, UI_VIM_NORMAL);
    c->visual = false;
    v->box_given = false;
    if (!cut || !edits || !owned)
        goto done;
    for (size_t i = 0; i < n; i++)
        cut[i] = _cut(c, b.top + i, &b);
    if (op == 'd' || op == 'c' || op == 'y' || op == 'p') {
        // The register: each line's piece as text, a line each.
        char** pieces = calloc(n, sizeof(char*));
        size_t* lens = calloc(n, sizeof(size_t));
        size_t total = 0;
        int widest = 0;
        for (size_t i = 0; pieces && lens && i < n; i++) {
            pieces[i] = _cut_text(c, &cut[i], 'y', NULL, 0, &lens[i]);
            total += lens[i] + 1;
            const int w = _vcol(c, _le(c, b.top + i)) - b.left;
            widest = w > widest ? w : widest;
        }
        char* text = pieces && lens ? malloc(total + 1) : NULL;
        size_t len = 0;
        for (size_t i = 0; text && i < n; i++) {
            if (pieces[i])
                memcpy(text + len, pieces[i], lens[i]);
            len += pieces[i] ? lens[i] : 0;
            if (i + 1 < n)
                text[len++] = '\n';
        }
        if (text) {
            text[len] = '\0';
            v->yank_width = b.right == INT_MAX ? (widest > 0 ? widest - 1 : 0) : b.right - b.left;
            _put_register(c, text, len, REG_BLOCK);
        }
        for (size_t i = 0; pieces && i < n; i++)
            free(pieces[i]);
        free(pieces);
        free(lens);
        free(text);
    }
    if (op == 'y') {
        const size_t at = _top_left(c, &b);
        _place(c, &at, NULL, 1, 0, true);
        goto done;
    }
    // One edit a line, in order; for c, where each insert goes in the text after them.
    size_t m = 0;
    size_t* carets = calloc(n, sizeof(size_t));
    size_t caret_count = 0;
    long long shift = 0;
    const int sw = c->tab * (count0 ? count0 : 1);
    for (size_t i = 0; i < n; i++) {
        const Cut* k = &cut[i];
        size_t from = k->lo, to = k->hi, len = 0;
        char* text = NULL;
        if (op == '>' && k->lo == k->hi && !_empty(c, b.top + i)) {
            // A line ending before the block takes the spaces at its end.
            text = malloc((size_t)sw + 1);
            len = text ? (size_t)sw : 0;
            if (text)
                _spaces(text, sw);
        } else if (op == '<' || op == '>') {
            if (k->lo < k->hi)
                text = _block_shifted(c, k->lo, op == '>' ? sw : -sw, &to, &len);
        } else if (k->lo < k->hi || (op == 'p' && k->reach)) {
            text = op == 'd' || op == 'c' || op == 'r' || op == 'p'
                       ? _cut_text(c, k, op, with, wlen, &len)
                       : _cased(c, k->lo, k->hi, op, &len);
        }
        if (op == 'c' && k->reach && carets)
            carets[caret_count++] = (size_t)((long long)from + shift + k->before);
        if (!text)
            continue;
        owned[m] = text;
        edits[m++] = (TextEdit){from, to, text, len};
        shift += (long long)len - (long long)(to - from);
    }
    // The cursor at the top left first, where taking the step back leaves it.
    const size_t corner = _top_left(c, &b);
    _place(c, &corner, NULL, 1, 0, true);
    _group(c, true);
    if (m)
        ui_editor_edit(c->el, edits, m, NULL, 0, 0);
    else
        _empty_step(c);
    Ctx now = _ctx(v, c->el);
    if (op == 'c' && carets && caret_count) {
        TextSelection* s = calloc(caret_count, sizeof(TextSelection));
        for (size_t i = 0; s && i < caret_count; i++)
            s[i] = (TextSelection){carets[i], carets[i]};
        if (s)
            ui_editor_set_carets(c->el, s, NULL, caret_count, 0);
        free(s);
        free(carets);
        v->block_insert = true;
        v->insert_top = b.top;
        v->insert_left = b.left;
        _enter_insert(&now, 'c', 1);
        goto done;
    }
    free(carets);
    size_t at = _top_left(&now, &b);
    if (op == 'p' && cut[0].reach && wlen) {
        // A put leaves the cursor on the last character it put on the top line.
        const char* last = with + wlen;
        text_utf8_prev(&last, with);
        at = cut[0].lo + (size_t)cut[0].before + (size_t)(last - with);
    }
    _place(&now, &at, NULL, 1, 0, true);
    _group(c, false);
done:
    for (size_t i = 0; owned && i < n; i++)
        free(owned[i]);
    free(owned);
    free(edits);
    free(cut);
}

// I and A in block mode: a caret at each line the block reaches, before its columns, or for A
// after them on every line -- one short of them filled out with spaces first, and after `$` at its
// end -- then insert mode, where what is typed goes in at every caret. A tab the caret would stand
// inside is split into spaces first.
static void _block_insert(Ctx* c, bool append) {
    UIEditorVim* v = c->v;
    const Box b = _box(c);
    _note_box(c, &b);
    const size_t n = b.bottom - b.top + 1;
    size_t* at = calloc(n, sizeof(size_t));
    TextEdit* pads = calloc(n, sizeof(TextEdit));
    char** owned = calloc(n, sizeof(char*));
    _set_state(v, UI_VIM_NORMAL);
    c->visual = false;
    v->box_given = false;
    size_t count = 0, m = 0;
    long long shift = 0;
    for (size_t i = 0; at && pads && owned && i < n; i++) {
        const size_t line = b.top + i;
        const Cut k = _cut(c, line, &b);
        const int width = _vcol(c, _le(c, line));
        // The edit this line takes: a split tab, or spaces out to the block; where its caret goes.
        size_t from = 0, to = 0, caret = 0;
        int pad = 0;
        if (!append) {
            if (!k.reach)
                continue;
            from = to = caret = k.lo;
            if (k.before) {
                to = _after(c, k.lo);
                pad = _vcol_end(c, k.lo) - _vcol(c, k.lo);
                caret = k.lo + (size_t)k.before;
            }
        } else if (b.right == INT_MAX) {
            from = to = caret = _le(c, line);
        } else if (width > b.right) {
            from = to = caret = k.hi;
            if (k.after) {
                from = _before(c, k.hi);
                pad = _vcol_end(c, from) - _vcol(c, from);
                caret = from + (size_t)(pad - k.after);
            }
        } else {
            from = to = _le(c, line);
            pad = b.right + 1 - width;
            caret = from + (size_t)pad;
        }
        at[count++] = (size_t)((long long)caret + shift);
        if (!pad || !(owned[m] = malloc((size_t)pad + 1)))
            continue;
        _spaces(owned[m], pad);
        pads[m] = (TextEdit){from, to, owned[m], (size_t)pad};
        m++;
        shift += (long long)pad - (long long)(to - from);
    }
    // The cursor at the top left first, where taking the step back leaves it.
    const size_t corner = _top_left(c, &b);
    _place(c, &corner, NULL, 1, 0, true);
    _group(c, true);
    if (m)
        ui_editor_edit(c->el, pads, m, NULL, 0, 0);
    TextSelection* s = calloc(count ? count : 1, sizeof(TextSelection));
    for (size_t i = 0; s && at && i < count; i++)
        s[i] = (TextSelection){at[i], at[i]};
    if (s && count)
        ui_editor_set_carets(c->el, s, NULL, count, 0);
    free(s);
    for (size_t i = 0; owned && i < m; i++)
        free(owned[i]);
    free(owned);
    free(pads);
    free(at);
    v->block_insert = true;
    v->insert_top = b.top;
    v->insert_left = b.left;
    Ctx now = _ctx(v, c->el);
    _enter_insert(&now, 'c', 1);
}

// I and A over areas that are not blocks, as vim takes them -- each area as whole lines, starting
// at the start of the line it began on when that is the first, else at the cursor. I inserts at
// that start. A inserts at the area's end, held onto a character, and after that character unless
// its last column is the line's first or the area runs to each line's end ($).
static void _visual_insert(Ctx* c, bool append) {
    UIEditorVim* v = c->v;
    if (v->state == UI_VIM_VISUAL_BLOCK) {
        _block_insert(c, append);
        return;
    }
    Carets k = _carets(c->el);
    size_t* at = calloc(k.count ? k.count : 1, sizeof(size_t));
    if (!k.sel || !at) {
        free(at);
        _free_carets(&k);
        return;
    }
    const TextSelection p = k.sel[k.primary];
    _note_area(c, UI_VIM_VISUAL_LINE, p.anchor, p.head, -1);
    for (size_t i = 0; i < k.count; i++) {
        const size_t h = k.sel[i].head, a = _ls(c, _lof(c, k.sel[i].anchor));
        const size_t start = a <= h ? a : h, end = a <= h ? h : a;
        if (!append) {
            at[i] = start;
            continue;
        }
        const size_t line = _lof(c, end);
        const bool past = k.want[i] != INT_MAX && _vcol_end(c, end) > 1 && !_empty(c, line);
        at[i] = _hold(c, end);
        if (past)
            at[i] = _after(c, at[i]);
    }
    _set_state(v, UI_VIM_NORMAL);
    c->visual = false;
    _place(c, at, NULL, k.count, k.primary, false);
    free(at);
    _free_carets(&k);
    _enter_insert(c, 'i', 1);
}

// J and gJ over each area: its lines joined, two at least.
static bool _visual_join(Ctx* c, bool spaces) {
    UIEditorVim* v = c->v;
    Carets k = _carets(c->el);
    size_t* at = calloc(k.count ? k.count : 1, sizeof(size_t));
    int* counts = calloc(k.count ? k.count : 1, sizeof(int));
    bool any = false;
    if (k.sel && at && counts) {
        if (v->state == UI_VIM_VISUAL_BLOCK) {
            const Box b = _box(c);
            _note_box(c, &b);
            k.count = 1;
            k.primary = 0;
            k.sel[0] = (TextSelection){_top_left(c, &b), _at_col(c, b.bottom, b.left, true)};
        } else {
            const TextSelection p = k.sel[k.primary];
            _note_area(c, v->state, p.anchor, p.head, k.want[k.primary]);
        }
        // Each cursor at its area's start as an operator takes it, which the step keeps; a join
        // running past the last line fails, the cursor left there.
        bool fits = true;
        const bool line_mode = v->state == UI_VIM_VISUAL_LINE;
        for (size_t i = 0; i < k.count; i++) {
            const size_t a = k.sel[i].anchor, h = k.sel[i].head;
            const size_t first = _lof(c, a), last = _lof(c, h);
            const size_t top = first < last ? first : last;
            counts[i] = (int)(first < last ? last - first : first - last) + 1;
            fits = fits && top + (size_t)(counts[i] > 2 ? counts[i] : 2) <= c->lines;
            at[i] = line_mode && first <= last ? _ls(c, first) : a < h ? a : h;
        }
        _set_state(v, UI_VIM_NORMAL);
        c->visual = false;
        v->box_given = false;
        _place(c, at, NULL, k.count, k.primary, true);
        any = fits && _join(c, 0, spaces, counts);
    }
    free(at);
    free(counts);
    _free_carets(&k);
    return any;
}

// r{c} in visual mode: every character of each area made `ch`, its line breaks kept; the cursor at
// the area's start. Refused for a line break.
static bool _visual_replace(Ctx* c, uint32_t ch) {
    UIEditorVim* v = c->v;
    if (ch >= K_ESC) {
        _visual_leave(c);
        return false;
    }
    if (v->state == UI_VIM_VISUAL_BLOCK) {
        char utf8[4] = {0};
        const size_t len = text_utf8_encode(ch, utf8);
        _block_operate(c, 'r', false, 0, utf8, len);
        return true;
    }
    const bool lines = v->state == UI_VIM_VISUAL_LINE;
    Carets k = _carets(c->el);
    Range* r = calloc(k.count ? k.count : 1, sizeof(Range));
    size_t* start = calloc(k.count ? k.count : 1, sizeof(size_t));
    Change* x = calloc(k.count ? k.count : 1, sizeof(Change));
    if (!k.sel || !r || !start || !x) {
        _free_carets(&k);
        free(r);
        free(start);
        free(x);
        return false;
    }
    _visual_ranges(c, &k, lines, lines, r, start);
    const TextSelection p = k.sel[k.primary];
    _note_area(c, v->state, p.anchor, p.head, k.want[k.primary]);
    char utf8[4];
    const size_t clen = text_utf8_encode(ch, utf8);
    for (size_t i = 0; i < k.count; i++) {
        const size_t lo = r[i].lo, hi = lines ? _le(c, r[i].last) : r[i].hi;
        size_t bytes = 0;
        for (size_t q = lo; q < hi;) {
            const uint32_t at = _gc(c, q);
            const size_t next = at ? _after(c, q) : q + strlen(_nl(c));
            bytes += at ? clen : next - q;
            q = next;
        }
        x[i].owned = malloc(bytes + 1);
        x[i].within = start[i] - lo;
        if (!x[i].owned)
            continue;
        size_t len = 0;
        for (size_t q = lo; q < hi;) {
            const uint32_t at = _gc(c, q);
            const size_t next = at ? _after(c, q) : q + strlen(_nl(c));
            if (at) {
                memcpy(x[i].owned + len, utf8, clen);
                len += clen;
            } else {
                text_doc_copy(c->doc, q, next, x[i].owned + len);
                len += next - q;
            }
            q = next;
        }
        x[i].e = (TextEdit){lo, hi, x[i].owned, len};
        x[i].edits = true;
    }
    _set_state(v, UI_VIM_NORMAL);
    c->visual = false;
    _place(c, start, NULL, k.count, k.primary, true);
    _group(c, true);
    _apply(c, x, k.count, k.primary);
    Ctx now = _ctx(v, c->el);
    size_t m = 0, primary = 0;
    size_t* at = _heads(c->el, &m, &primary);
    if (at)
        _place(&now, at, NULL, m, primary, true);
    free(at);
    _group(c, false);
    free(x);
    free(r);
    free(start);
    _free_carets(&k);
    return true;
}

// The columns `len` bytes of `s` take from a line's start.
static int _columns(const Ctx* c, const char* s, size_t len) {
    int col = 0;
    for (size_t i = 0; i < len;) {
        const uint32_t cp = ui_typing_next(s, len, &i);
        col += cp == '\t' ? c->tab - col % c->tab : 1;
    }
    return col;
}

// p and P over each visual area: the area replaced by the register, `count` times -- as lines when
// the area is lines, and the area's line broken round them when the register holds lines and the
// area does not. A block takes one line of characters in every line, and anything else at its top
// left. With p the register takes what the area held; P keeps it.
static void _visual_put(Ctx* c, bool keep, int count0) {
    UIEditorVim* v = c->v;
    RegKind kind = REG_CHARS;
    const char* got = _get_register(c, &kind);
    char* reg = got ? strdup(got) : NULL;
    const size_t reg_len = reg ? strlen(reg) : 0;
    const int reg_width = v->yank_width + 1;
    const int times = count0 ? count0 : 1;
    if (v->state == UI_VIM_VISUAL_BLOCK) {
        if (reg && kind == REG_CHARS && !memchr(reg, '\n', reg_len)) {
            char* each = malloc(reg_len * (size_t)times + 1);
            for (int t = 0; each && t < times; t++)
                memcpy(each + reg_len * (size_t)t, reg, reg_len);
            if (each)
                _block_operate(c, 'p', false, 0, each, reg_len * (size_t)times);
            free(each);
        } else {
            // The block's pieces deleted, and the register put at its top left, as one step.
            const Box b = _box(c);
            const size_t corner = _top_left(c, &b);
            _place(c, &corner, NULL, 1, 0, true);
            _group(c, true);
            v->held++;
            _block_operate(c, 'd', false, 0, NULL, 0);
            _note_box(c, &b);
            Ctx now = _ctx(v, c->el);
            if (reg && kind == REG_BLOCK) {
                _put_block(&now, reg, reg_width, b.top, b.left, times);
            } else if (reg && kind == REG_LINES) {
                // Lines go below the block with p; with P above it, or below its top line when
                // the delete left that line short of the block's left column.
                const size_t gap = _at_col(&now, b.top, b.left, true);
                const bool below = !keep || _hold(&now, gap) < gap;
                const size_t line = keep ? b.top : b.bottom < now.lines ? b.bottom : now.lines - 1;
                const size_t at = _ls(&now, line);
                _place(&now, &at, NULL, 1, 0, true);
                _put_text(&now, reg, kind, -1, below, times);
            } else if (reg) {
                _put_text(&now, reg, kind, -1, false, times);
            }
            v->held--;
            _group(c, false);
        }
        if (reg && keep)
            _put_register(c, reg, reg_len, kind);
        if (keep && kind == REG_BLOCK)
            v->yank_width = reg_width - 1;
        free(reg);
        return;
    }
    const bool lines = v->state == UI_VIM_VISUAL_LINE;
    Carets k = _carets(c->el);
    Range* r = calloc(k.count ? k.count : 1, sizeof(Range));
    size_t* start = calloc(k.count ? k.count : 1, sizeof(size_t));
    Change* x = calloc(k.count ? k.count : 1, sizeof(Change));
    char* body = NULL;
    size_t body_len = 0;
    if (!k.sel || !r || !start || !x)
        goto done;
    _visual_ranges(c, &k, lines, lines, r, start);
    const TextSelection p = k.sel[k.primary];
    _note_area(c, v->state, p.anchor, p.head, k.want[k.primary]);
    // What the area held, for the register after p.
    size_t held_len = 0;
    const Range* pr = &r[k.primary];
    char* held = lines ? _lines_text(c, pr->first, pr->last, &held_len)
                       : _range_text(c, pr->lo, pr->hi, &held_len);
    // The register as it goes in: whole lines end in a line break.
    const bool put_lines = lines || kind == REG_LINES;
    const size_t one = reg_len + (put_lines && (!reg_len || reg[reg_len - 1] != '\n') ? 1 : 0);
    char* raw = malloc(one * (size_t)times + 2);
    if (raw) {
        size_t len = 0;
        if (put_lines && !lines && reg)
            raw[len++] = '\n';
        for (int t = 0; reg && t < times; t++) {
            memcpy(raw + len, reg, reg_len);
            len += reg_len;
            if (one > reg_len)
                raw[len++] = '\n';
        }
        raw[len] = '\0';
        body = _with_nl(c, raw, len, &body_len);
    }
    free(raw);
    const size_t nl_len = strlen(_nl(c));
    for (size_t i = 0; body && i < k.count; i++) {
        size_t lo = r[i].lo, hi = r[i].hi, len = body_len;
        if (lines) {
            // The last lines of a text with no final line break: put without one.
            if (r[i].last + 1 >= c->lines && !_ends_broken(c)) {
                hi = _le(c, r[i].last);
                len = body_len >= nl_len ? body_len - nl_len : 0;
            }
        } else if (put_lines) {
            x[i].within = nl_len;
        } else if (reg && !memchr(reg, '\n', reg_len) && body_len) {
            const char* q = body + body_len;
            text_utf8_prev(&q, body);
            x[i].within = (size_t)(q - body);
        }
        x[i].e = (TextEdit){lo, hi, body, len};
        x[i].edits = true;
    }
    _set_state(v, UI_VIM_NORMAL);
    c->visual = false;
    // The carets at the areas' starts first, where taking the step back leaves them.
    _place(c, start, NULL, k.count, k.primary, true);
    _group(c, true);
    if (body)
        _apply(c, x, k.count, k.primary);
    {
        // Lines put leave the cursor on the first's first character not a blank.
        Ctx now = _ctx(v, c->el);
        size_t m = 0, primary = 0;
        size_t* at = _heads(c->el, &m, &primary);
        for (size_t i = 0; at && put_lines && i < m; i++)
            at[i] = _first_nonblank(&now, _lof(&now, at[i]));
        if (at)
            _place(&now, at, NULL, m, primary, true);
        free(at);
    }
    _group(c, false);
    if (held && !keep)
        _put_register(c, held, held_len, lines ? REG_LINES : REG_CHARS);
    free(held);
done:
    free(body);
    free(reg);
    free(x);
    free(r);
    free(start);
    _free_carets(&k);
}

// A block's pieces put down from `line`, each at column `col` of its line -- a line short of it
// filled with spaces, and lines past the text's end added -- each filled out with spaces to the
// block's `width` (below 0, its widest piece's) where text follows it, `count` times along. The
// cursor goes to the first piece.
static void _put_block(Ctx* c, const char* text, int width, size_t line, int col, int count) {
    UIEditorVim* v = c->v;
    const int times = count ? count : 1;
    size_t pieces = 1;
    for (const char* q = text; *q; q++)
        pieces += *q == '\n';
    const char** starts = calloc(pieces, sizeof(char*));
    size_t* lens = calloc(pieces, sizeof(size_t));
    TextEdit* edits = calloc(pieces, sizeof(TextEdit));
    char** owned = calloc(pieces, sizeof(char*));
    if (!starts || !lens || !edits || !owned)
        goto done;
    int widest = 0;
    {
        size_t i = 0;
        const char* s = text;
        for (const char* q = text;; q++) {
            if (*q == '\n' || !*q) {
                starts[i] = s;
                lens[i] = (size_t)(q - s);
                const int w = _columns(c, s, lens[i]);
                widest = w > widest ? w : widest;
                i++;
                s = q + 1;
                if (!*q)
                    break;
            }
        }
    }
    if (width < 0)
        width = widest;
    const char* nl = _nl(c);
    const size_t nl_len = strlen(nl);
    size_t m = 0, first_at = 0, first_pad = 0;
    // Lines past the text's end are added by one edit at its end.
    char* added = NULL;
    size_t added_len = 0, added_cap = 0;
    for (size_t i = 0; i < pieces; i++) {
        const size_t target = line + i;
        const bool exists = target < c->lines;
        int at_col = 0, post = 0;
        size_t at = 0, cut_to = 0;
        bool short_line = true;
        if (exists) {
            size_t len = 0;
            const char* s = _lt(c, target, &len);
            size_t b = 0, prev = 0;
            int prev_col = 0;
            while (b < len && at_col < col) {
                prev = b;
                prev_col = at_col;
                const uint32_t cp = ui_typing_next(s, len, &b);
                at_col += cp == '\t' ? c->tab - at_col % c->tab : 1;
            }
            if (at_col > col) {
                // A tab across the column is split, its columns either side kept as spaces.
                post = at_col - col;
                at_col = prev_col;
                b = prev;
            }
            at = _ls(c, target) + b;
            cut_to = post ? at + 1 : at;
            short_line = !post && (at_col < col || b >= len);
        }
        const size_t pad = at_col < col ? (size_t)(col - at_col) : 0;
        const int w = _columns(c, starts[i], lens[i]);
        const size_t fill = width > w ? (size_t)(width - w) : 0;
        size_t need = pad + ((size_t)times) * (lens[i] + fill) + (size_t)post + nl_len + 1;
        char* buf = malloc(need);
        if (!buf)
            continue;
        size_t len = 0;
        memset(buf, ' ', pad);
        len += pad;
        for (int t = 0; t < times; t++) {
            memcpy(buf + len, starts[i], lens[i]);
            len += lens[i];
            if (t < times - 1 || !short_line) {
                memset(buf + len, ' ', fill);
                len += fill;
            }
        }
        len = (size_t)(_spaces(buf + len, post) - buf);
        if (i == 0) {
            first_at = at;
            first_pad = pad;
        }
        if (exists) {
            owned[m] = buf;
            edits[m++] = (TextEdit){at, cut_to, buf, len};
            continue;
        }
        // A new line: after a line break, or before the text's own final one.
        const size_t grow = len + nl_len;
        if (!grow_array((void**)&added, &added_cap, added_len + grow + 1, 1, 64)) {
            free(buf);
            continue;
        }
        if (!_ends_broken(c)) {
            memcpy(added + added_len, nl, nl_len);
            added_len += nl_len;
        }
        memcpy(added + added_len, buf, len);
        added_len += len;
        if (_ends_broken(c)) {
            memcpy(added + added_len, nl, nl_len);
            added_len += nl_len;
        }
        if (i == 0) {
            first_at = c->doc->length + (_ends_broken(c) ? 0 : nl_len);
            first_pad = pad;
        }
        free(buf);
    }
    // The lines added follow an insert at the text's very end, as one edit there.
    if (added && m && edits[m - 1].start == c->doc->length) {
        char* both = malloc(edits[m - 1].len + added_len + 1);
        if (both) {
            memcpy(both, edits[m - 1].text, edits[m - 1].len);
            memcpy(both + edits[m - 1].len, added, added_len);
            free(owned[m - 1]);
            owned[m - 1] = both;
            edits[m - 1].text = both;
            edits[m - 1].len += added_len;
        }
        free(added);
        added = NULL;
    }
    if (added) {
        owned[m] = added;
        edits[m++] = (TextEdit){c->doc->length, c->doc->length, added, added_len};
    }
    _group(c, true);
    if (m)
        ui_editor_edit(c->el, edits, m, NULL, 0, 0);
    Ctx now = _ctx(v, c->el);
    const size_t to = first_at + first_pad;
    _place(&now, &to, NULL, 1, 0, true);
    _group(c, false);
done:
    for (size_t i = 0; owned && i < pieces; i++)
        free(owned[i]);
    free(owned);
    free(edits);
    free(lens);
    free(starts);
}

// ---- Normal mode

static bool _normal_key(Ctx* c, uint32_t k);

// Every caret moved by a motion, each held onto a character.
static bool _move(Ctx* c, uint32_t prefix, uint32_t key, uint32_t arg, int count0) {
    Carets k = _carets(c->el);
    size_t* at = calloc(k.count ? k.count : 1, sizeof(size_t));
    if (!k.sel || !at) {
        free(at);
        _free_carets(&k);
        return false;
    }
    bool moved = false;
    for (size_t i = 0; i < k.count; i++) {
        const Moved m = _motion(c, prefix, key, arg, k.sel[i].head, count0, 0, k.want[i]);
        at[i] = m.ok ? m.to : k.sel[i].head;
        if (m.ok) {
            k.want[i] = m.want == WANT_KEEP ? k.want[i] : m.want;
            moved = true;
        }
    }
    _place(c, at, k.want, k.count, k.primary, true);
    free(at);
    _free_carets(&k);
    return moved;
}

// A command whose first key `p` waited for the next, `k`.
static bool _second(Ctx* c, uint32_t p, uint32_t k) {
    UIEditorVim* v = c->v;
    const int count0 = _count0(v);
    const uint32_t op = v->op;
    if (k == K_ESC) {
        _reset(v);
        return true;
    }
    if (p == 'f' || p == 'F' || p == 't' || p == 'T') {
        if (k >= K_ESC) {
            _reset(v);
            return true;
        }
        v->find_kind = p;
        v->find_char = k;
        if (op)
            _operate(c, op, 0, p, k, count0);
        else if (c->visual)
            _visual_move(c, 0, p, k, count0);
        else
            _move(c, 0, p, k, count0);
        if (op && op != 'y')
            _keep_change(v, count0);
        _reset(v);
        return true;
    }
    if ((p == 'i' || p == 'a') && (op || c->visual)) {
        // A text object after an operator -- diw, ci(, yap -- or made the visual area.
        if (k < K_ESC && c->visual) {
            _visual_object(c, p, k, count0);
        } else if (k < K_ESC) {
            _operate(c, op, p, k, 0, count0);
            if (op != 'y')
                _keep_change(v, count0);
        }
        _reset(v);
        return true;
    }
    if (p == 'r') {
        if (c->visual) {
            if (_visual_replace(c, k))
                _keep_change(v, count0);
        } else if (k < K_ESC || k == K_CR) {
            if (_replace(c, k, count0))
                _keep_change(v, count0);
        }
        _reset(v);
        return true;
    }
    if (p == 'z') {
        Carets kk = _carets(c->el);
        if (kk.sel) {
            const size_t head = kk.sel[kk.primary].head;
            if (k == 'z' || k == '.')
                ui_editor_scroll_place(c->el, head, 0.5f);
            else if (k == 't' || k == K_CR)
                ui_editor_scroll_place(c->el, head, 0.0f);
            else if (k == 'b' || k == '-')
                ui_editor_scroll_place(c->el, head, 1.0f);
        }
        _free_carets(&kk);
        _reset(v);
        return true;
    }
    if (p == 'g') {
        if (c->visual && (k == '~' || k == 'u' || k == 'U' || k == 'J')) {
            const bool changed = k != 'J' || _visual_join(c, false);
            if (k != 'J')
                _visual_operate(c, k, false, count0);
            if (changed)
                _keep_change(v, count0);
            _reset(v);
            return true;
        }
        if (k == '~' || k == 'u' || k == 'U') {
            if (op) {
                // g~g~, gugu, gUgU: the operator doubled, a line.
                if (op == k) {
                    _operate(c, op, 0, k, 0, count0);
                    _keep_change(v, count0);
                }
                _reset(v);
                return true;
            }
            v->op = k;
            _pending_add(v, k);
            return true;
        }
        if (k == 'J') {
            if (_join(c, count0, false, NULL))
                _keep_change(v, count0);
            _reset(v);
            return true;
        }
        if (_is_motion('g', k)) {
            if (op) {
                _operate(c, op, 'g', k, 0, count0);
                if (op != 'y')
                    _keep_change(v, count0);
            } else if (c->visual) {
                _visual_move(c, 'g', k, 0, count0);
            } else {
                _move(c, 'g', k, 0, count0);
            }
            _reset(v);
            return true;
        }
        _reset(v);
        return true;
    }
    _reset(v);
    return true;
}

static void _repeat(Ctx* c);

// Normal mode's commands: everything that is not an operator's motion.
static bool _command(Ctx* c, uint32_t k) {
    UIEditorVim* v = c->v;
    const int count0 = _count0(v);
    switch (k) {
        case K_ESC: {
            // Esc drops what was waiting, and several carets to the primary. It is always
            // taken, as vim takes it: pressed out of habit, it must not let go of the editor. A
            // host that wants it binds it in its layer.
            _reset(v);
            Carets kk = _carets(c->el);
            if (kk.count > 1) {
                const size_t h = kk.sel[kk.primary].head;
                _place(c, &h, NULL, 1, 0, true);
            }
            _free_carets(&kk);
            return true;
        }
        case 'd':
        case 'c':
        case 'y':
        case '<':
        case '>':
            v->op = k;
            return true;
        case 'g':
        case 'z':
        case 'f':
        case 'F':
        case 't':
        case 'T':
        case 'r':
            v->prefix = k;
            return true;
        case 'x':
        case K_DEL:
            _operate(c, 'd', 0, 'l', 0, count0);
            _keep_change(v, count0);
            _reset(v);
            return true;
        case 'X':
            _operate(c, 'd', 0, 'h', 0, count0);
            _keep_change(v, count0);
            _reset(v);
            return true;
        case 'D':
            _operate(c, 'd', 0, '$', 0, count0);
            _keep_change(v, count0);
            _reset(v);
            return true;
        case 'C':
            v->count = v->op_count = 0;
            _operate(c, 'c', 0, '$', 0, count0);
            v->change_count = count0;
            return true;
        case 's':
            v->count = v->op_count = 0;
            _operate(c, 'c', 0, 'l', 0, count0);
            v->change_count = count0;
            return true;
        case 'S':
            v->count = v->op_count = 0;
            _operate(c, 'c', 0, 'c', 0, count0);
            v->change_count = count0;
            return true;
        case 'Y':
            _operate(c, 'y', 0, 'y', 0, count0);
            _reset(v);
            return true;
        case 'p':
        case 'P':
        case K_PASTE:
            _put(c, k == 'p', count0);
            _keep_change(v, count0);
            _reset(v);
            return true;
        case 'J':
            if (_join(c, count0, true, NULL))
                _keep_change(v, count0);
            _reset(v);
            return true;
        case '~':
            _tilde(c, count0);
            _keep_change(v, count0);
            _reset(v);
            return true;
        case 'i':
        case 'a':
        case 'I':
        case 'A':
        case 'o':
        case 'O':
            v->change_count = count0;
            v->count = v->op_count = 0;
            v->pending[0] = '\0';
            _enter_insert(c, k, count0);
            return true;
        case 'u':
            _undo(c, false, count0);
            _reset(v);
            return true;
        case K_CTRL('r'):
            _undo(c, true, count0);
            _reset(v);
            return true;
        case '.':
            _repeat(c);
            return true;
        case 'v':
        case 'V':
        case K_CTRL('v'):
            _visual_count(c,
                          k == 'v'   ? UI_VIM_VISUAL
                          : k == 'V' ? UI_VIM_VISUAL_LINE
                                     : UI_VIM_VISUAL_BLOCK,
                          count0);
            _reset(v);
            return true;
        case K_CTRL('e'):
        case K_CTRL('y'):
            ui_editor_scroll_rows(c->el,
                                  (float)(count0 ? count0 : 1) * (k == K_CTRL('e') ? 1 : -1));
            _reset(v);
            return true;
        case K_CTRL('d'):
        case K_CTRL('u'):
        case K_CTRL('f'):
        case K_CTRL('b'):
        case K_PGUP:
        case K_PGDN: {
            const int shown = ui_editor_rows_shown(c->el);
            const bool down = k == K_CTRL('d') || k == K_CTRL('f') || k == K_PGDN;
            const int rows = k == K_CTRL('d') || k == K_CTRL('u') ? shown / 2 : shown - 2;
            const int n = (rows > 1 ? rows : 1) * (count0 && rows == shown - 2 ? count0 : 1);
            ui_editor_scroll_rows(c->el, (float)(down ? n : -n));
            ui_editor_apply(c->el, UI_OP_MOVE, down ? UI_MOTION_ROW_DOWN : UI_MOTION_ROW_UP, n);
            Carets kk = _carets(c->el);
            size_t* at = calloc(kk.count ? kk.count : 1, sizeof(size_t));
            for (size_t i = 0; kk.sel && at && i < kk.count; i++)
                at[i] = _first_nonblank(c, _lof(c, kk.sel[i].head));
            if (kk.sel && at)
                _place(c, at, NULL, kk.count, kk.primary, true);
            free(at);
            _free_carets(&kk);
            _reset(v);
            return true;
        }
        case K_TAB:
        case K_COPY:
            _reset(v);
            return true;
        default:
            if (_is_motion(0, k)) {
                _move(c, 0, k, 0, count0);
                _reset(v);
                return true;
            }
            // A key vim does not know is taken, as vim beeps at it, unless it is one only a host
            // could mean.
            _reset(v);
            return k < K_ESC;
    }
}

// Visual mode's commands: a motion moves the area's head, an operator acts on the area and ends
// visual mode, v V and Ctrl+V switch to another visual mode or back to normal.
static bool _visual_command(Ctx* c, uint32_t k) {
    UIEditorVim* v = c->v;
    const int count0 = _count0(v);
    const bool block = v->state == UI_VIM_VISUAL_BLOCK;
    switch (k) {
        case K_ESC:
            _visual_leave(c);
            break;
        case 'v':
        case 'V':
        case K_CTRL('v'): {
            const UIEditorVimState to = k == 'v'   ? UI_VIM_VISUAL
                                        : k == 'V' ? UI_VIM_VISUAL_LINE
                                                   : UI_VIM_VISUAL_BLOCK;
            if (v->state == to)
                _visual_leave(c);
            else
                _visual(c, to);
            break;
        }
        case 'o':
        case 'O':
            _visual_swap(c, k == 'O' && block);
            break;
        case 'g':
        case 'z':
        case 'f':
        case 'F':
        case 't':
        case 'T':
        case 'r':
        case 'i':
        case 'a':
            v->prefix = k;
            return true;
        case 'd':
        case 'x':
        case K_DEL:
        case 'X':
        case 'D':
            // X is a block's d; D takes a block to each line's end; either takes other areas'
            // lines whole.
            _visual_operate(c, 'd', k == 'D' || (k == 'X' && !block), count0);
            _keep_change(v, count0);
            break;
        case 'y':
        case 'Y':
        case K_COPY:
            _visual_operate(c, 'y', k == 'Y' && !block, count0);
            break;
        case 'S':
        case 'R':
            if (block)
                _visual(c, UI_VIM_VISUAL_LINE);
            _visual_operate(c, 'c', true, count0);
            break;
        case 'c':
        case 's':
        case 'C':
            _visual_operate(c, 'c', k == 'C', count0);
            break;
        case '<':
        case '>':
        case '~':
        case 'u':
        case 'U':
            _visual_operate(c, k, false, count0);
            _keep_change(v, count0);
            break;
        case 'J':
            if (_visual_join(c, true))
                _keep_change(v, count0);
            break;
        case 'p':
        case 'P':
        case K_PASTE:
            _visual_put(c, k == 'P', count0);
            _keep_change(v, count0);
            break;
        case 'I':
        case 'A':
            _visual_insert(c, k == 'A');
            break;
        default:
            if (!_is_motion(0, k)) {
                _reset(v);
                return k < K_ESC;
            }
            _visual_move(c, 0, k, 0, count0);
            break;
    }
    _reset(v);
    return true;
}

// What follows an operator: the operator again for lines, a motion, or Esc.
static bool _after_op(Ctx* c, uint32_t k) {
    UIEditorVim* v = c->v;
    const int count0 = _count0(v);
    const uint32_t op = v->op;
    if (k == K_ESC) {
        _reset(v);
        return true;
    }
    if (k == 'g' || k == 'f' || k == 'F' || k == 't' || k == 'T' || k == 'i' || k == 'a') {
        v->prefix = k;
        _pending_add(v, k);
        return true;
    }
    const bool doubled =
        k == op || (op == '~' && k == '~') || (op == 'u' && k == 'u') || (op == 'U' && k == 'U');
    if (!doubled && !_is_motion(0, k)) {
        _reset(v);
        return true;
    }
    _operate(c, op, 0, k, 0, count0);
    if (op != 'y')
        _keep_change(v, count0);
    _reset(v);
    return true;
}

static bool _normal_key(Ctx* c, uint32_t k) {
    UIEditorVim* v = c->v;
    if (v->prefix) {
        const uint32_t p = v->prefix;
        v->prefix = 0;
        _record(v, k);
        return _second(c, p, k);
    }
    int* n = v->op ? &v->op_count : &v->count;
    if ((k >= '1' && k <= '9') || (k == '0' && *n)) {
        if (*n < 99999999)
            *n = *n * 10 + (int)(k - '0');
        _pending_add(v, k);
        return true;
    }
    _record(v, k);
    if (v->op)
        return _after_op(c, k);
    if (k != '.')
        _pending_add(v, k);
    return c->visual ? _visual_command(c, k) : _command(c, k);
}

// `.`: the last change made again, with the count given now if there is one -- a visual one over
// an area made again at the cursor.
static void _repeat(Ctx* c) {
    UIEditorVim* v = c->v;
    const int count = v->count ? v->count : v->change_count;
    _reset(v);
    if (!v->change_len)
        return;
    uint32_t* keys = malloc(v->change_len * sizeof(uint32_t));
    if (!keys)
        return;
    const size_t n = v->change_len;
    memcpy(keys, v->change, n * sizeof(uint32_t));
    const VisualArea area = v->change_area;
    v->replaying++;
    if (area.on)
        _visual_again(c, &area);
    v->count = count;
    for (size_t i = 0; i < n; i++)
        _feed(v, c->el, keys[i]);
    v->replaying--;
    v->box_given = false;
    v->change_count = count;
    free(keys);
    _reset(v);
}

// ---- The mode

static bool _feed(UIEditorVim* v, UIElement* el, uint32_t k) {
    Ctx c = _ctx(v, el);
    if (!c.doc)
        return false;
    if (v->state == UI_VIM_INSERT)
        return _insert_key(&c, k);
    return _normal_key(&c, k);
}

static bool _read_key(UIElement* el, const InputKeyEvent* ev, void* user) {
    const uint32_t k = _vkey(ev);
    return k && _feed(user, el, k);
}

// The pointer: a click ends visual mode and leaves the cursor on a character; a drag is visual mode
// over the characters it crossed, its head on the last it reached.
static void _pointer(UIElement* el, void* user) {
    UIEditorVim* v = user;
    if (v->state == UI_VIM_INSERT)
        return;
    Ctx c = _ctx(v, el);
    if (!c.doc)
        return;
    Carets k = _carets(el);
    size_t* at = calloc(k.count ? k.count : 1, sizeof(size_t));
    bool selected = false;
    for (size_t i = 0; k.sel && at && i < k.count; i++) {
        at[i] = k.sel[i].head;
        selected = selected || k.sel[i].anchor != k.sel[i].head;
    }
    if (selected) {
        for (size_t i = 0; i < k.count; i++)
            if (k.sel[i].head > k.sel[i].anchor)
                _dec(&c, &k.sel[i].head);
        _set_state(v, UI_VIM_VISUAL);
        v->box_given = false;
        ui_editor_set_carets(el, k.sel, NULL, k.count, k.primary);
    } else if (k.sel && at) {
        _set_state(v, UI_VIM_NORMAL);
        c.visual = false;
        _place(&c, at, NULL, k.count, k.primary, true);
    }
    free(at);
    _free_carets(&k);
}

UIEditorVim* create_ui_editor_vim(void) {
    UIEditorVim* v = calloc(1, sizeof(UIEditorVim));
    if (!v) {
        log_error("create_ui_editor_vim: out of memory");
        return NULL;
    }
    v->mode.read_key = _read_key;
    v->mode.pointer = _pointer;
    v->mode.user = v;
    _set_state(v, UI_VIM_NORMAL);
    return v;
}

void free_ui_editor_vim(UIEditorVim* vim) {
    if (!vim)
        return;
    free(vim->yank);
    free(vim->search);
    free(vim);
}

const UIEditorMode* ui_editor_vim_mode(const UIEditorVim* vim) {
    return vim ? &vim->mode : NULL;
}

UIEditorVimState ui_editor_vim_state(const UIEditorVim* vim) {
    return vim ? vim->state : UI_VIM_NORMAL;
}

const char* ui_editor_vim_pending(const UIEditorVim* vim) {
    return vim ? vim->pending : "";
}

void ui_editor_vim_set_prompt(UIEditorVim* vim, UIEditorVimPromptFn fn, void* user) {
    if (!vim)
        return;
    vim->prompt = fn;
    vim->prompt_user = user;
}

void ui_editor_vim_set_undo(UIEditorVim* vim, UIEditorCommandFn undo, UIEditorCommandFn redo,
                            void* user) {
    if (!vim)
        return;
    vim->undo = undo;
    vim->redo = redo;
    vim->undo_user = user;
}

bool ui_editor_vim_search(UIEditorVim* vim, UIElement* el, const char* query, bool backward) {
    (void)vim;
    (void)el;
    (void)query;
    (void)backward;
    return false;
}

bool ui_editor_vim_command(UIEditorVim* vim, UIElement* el, const char* line) {
    (void)vim;
    (void)el;
    (void)line;
    return false;
}

static const UIEditorVimKey KEYS[] = {
    {"i a I A o O",
     "Insert before or after the cursor, at the line's start or end, or on a new line"},
    {"Esc", "Back to normal mode"},
    {"h j k l", "Left, down, up, right"},
    {"w b e ge", "On a word, back a word, to a word's end, back to one's end"},
    {"W B E gE", "The same, words split at blanks only"},
    {"0 ^ $", "The line's start, its first character, its end"},
    {"gg G", "The first line, the last, or line N with a count"},
    {"f F t T ; ,", "To a character on the line, before it, and again either way"},
    {"% { }", "The matching bracket, the paragraph before or after"},
    {"H M L", "The top, middle and bottom of the view"},
    {"d c y", "Delete, change and yank over a motion; dd cc yy a line"},
    {"> <", "Indent and outdent over a motion; >> << a line"},
    {"g~ gu gU", "Swap, lower and raise case over a motion"},
    {"iw aw ip ap", "A word or a paragraph, after an operator or in visual mode"},
    {"i( a( i[ a[ i{ a{ i\" a\"", "What a bracket or a quote holds, or it with them"},
    {"v V Ctrl+V", "Select characters, lines or a block; o to its other end"},
    {"I A in a block", "Type on every line of the block"},
    {"x X D C s S Y", "Delete or change a character, to the line's end, or the line"},
    {"p P", "Put after or before the cursor"},
    {"J gJ", "Join lines, with or without a space"},
    {"r ~", "Replace a character, swap a character's case"},
    {"u Ctrl+R", "Undo, redo"},
    {".", "Repeat the last change"},
    {"zz zt zb", "Scroll the cursor's line to the middle, top or bottom"},
    {"Ctrl+D Ctrl+U Ctrl+F Ctrl+B", "Half a page down or up, a page down or up"},
};

const UIEditorVimKey* ui_editor_vim_keys(size_t* count) {
    if (count)
        *count = sizeof(KEYS) / sizeof(KEYS[0]);
    return KEYS;
}
