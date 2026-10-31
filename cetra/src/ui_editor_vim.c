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
    // The text an undo group is held open in, while one is.
    TextDoc* grouped;
    // An insert: the command that began it and its count, where its typing begins in `keys`, and
    // whether a line's indent was put in by an Enter or an `o` with nothing typed after it.
    uint32_t insert_cmd;
    int insert_count;
    size_t insert_from;
    bool indent_pending;
    // The last f, F, t or T and its character; the last search.
    uint32_t find_kind, find_char;
    char* search;
    bool search_back;
    // What vim last put on the clipboard, and whether it was whole lines.
    char* yank;
    bool yank_lines;
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
                        : state == UI_VIM_VISUAL || state == UI_VIM_VISUAL_BLOCK
                            ? UI_SELECTION_INCLUSIVE
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

// The command just read changed the text: it is what `.` repeats, unless it is being replayed.
static void _keep_change(UIEditorVim* v, int count) {
    if (v->replaying)
        return;
    memcpy(v->change, v->keys, v->key_count * sizeof(uint32_t));
    v->change_len = v->key_count;
    v->change_count = count;
}

// ---- The text, vim's way

typedef struct {
    UIEditorVim* v;
    UIElement* el;
    TextDoc* doc;
    size_t lines; // the text's lines less an empty one after a final line break
    int tab;
} Ctx;

static Ctx _ctx(UIEditorVim* v, UIElement* el) {
    Ctx c = {v, el, ui_editor_doc(el), 0, 4};
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

// b and B: back past blanks, stopping at an empty line, then to the start of the word before.
static bool _bck_word(const Ctx* c, size_t* off, int count, bool big) {
    while (--count >= 0) {
        if (_dec(c, off) == -1)
            return false;
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

// e and E; with `stop`, a word already at its end is not left -- cw.
static bool _end_word(const Ctx* c, size_t* off, int count, bool big, bool stop) {
    while (--count >= 0) {
        const int sclass = _cls(c, *off, big);
        if (_inc(c, off) == -1)
            return false;
        if (_cls(c, *off, big) == sclass && sclass != 0) {
            if (_skip_class(c, off, sclass, big, true))
                return false;
        } else if (!stop || sclass == 0) {
            while (_cls(c, *off, big) == 0)
                if (_inc(c, off) == -1)
                    return false;
            if (_skip_class(c, off, _cls(c, *off, big), big, true))
                return false;
        }
        _dec(c, off);
        stop = false;
    }
    return true;
}

// ge and gE.
static bool _bckend_word(const Ctx* c, size_t* off, int count, bool big) {
    while (--count >= 0) {
        const int sclass = _cls(c, *off, big);
        if (_dec(c, off) == -1)
            return false;
        if (sclass != 0)
            while (_cls(c, *off, big) == sclass)
                if (_dec(c, off) == -1)
                    return true;
        while (_cls(c, *off, big) == 0) {
            const size_t line = _lof(c, *off);
            if (*off == _ls(c, line) && _empty(c, line))
                break;
            if (_dec(c, off) == -1)
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
                m.ok = _bckend_word(c, &m.to, n, key == 'E');
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
            return (Moved){_at_col(c, to, w, false), true, false, true, w};
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
            m.to = _last(c, to);
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
                _end_word(c, &m.to, n, big, true);
                m.inclusive = true;
            } else {
                _fwd_word(c, &m.to, n, big, op != 0);
            }
            // Left on a line's end: back onto its last character, which the range then takes in.
            if (m.to > from && m.to > _ls(c, _lof(c, m.to)) && m.to == _le(c, _lof(c, m.to))) {
                m.to = _before(c, m.to);
                m.inclusive = true;
            }
            return m;
        }
        case 'b':
        case 'B':
            m.ok = _bck_word(c, &m.to, n, key == 'B');
            return m;
        case 'e':
        case 'E': {
            _end_word(c, &m.to, n, key == 'E', false);
            m.inclusive = true;
            if (m.to > from && m.to > _ls(c, _lof(c, m.to)) && m.to == _le(c, _lof(c, m.to)))
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

static void _put_register(Ctx* c, const char* text, size_t len, bool lines) {
    Engine* engine = ui_editor_engine(c->el);
    char* copy = malloc(len + 1);
    if (!copy)
        return;
    memcpy(copy, text, len);
    copy[len] = '\0';
    free(c->v->yank);
    c->v->yank = copy;
    c->v->yank_lines = lines;
    if (engine)
        engine_clipboard_set(engine, copy);
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
// range: `x` on an empty line, then `u`, takes back nothing.
static void _empty_step(Ctx* c) {
    text_doc_empty_step(c->doc, c->el->editor);
}

// One step of undo held open over a command's actions, or an insert's, from its first key to its
// last: opened once, however many times asked, and closed in the text it was opened in.
static void _group(Ctx* c, bool open) {
    UIEditorVim* v = c->v;
    if (open && !v->grouped) {
        text_doc_group(c->doc, true);
        v->grouped = c->doc;
    } else if (!open && v->grouped) {
        if (v->grouped == c->doc)
            text_doc_group(c->doc, false);
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

// The lines [first, last] shifted a tab stop in, or out with `out`, each built again from its
// column with spaces, as vim's shift with expandtab does; an empty line is left alone going in.
static char* _shifted(const Ctx* c, size_t line, bool out, size_t* n, size_t* cut) {
    size_t len = 0;
    const char* s = _lt(c, line, &len);
    size_t blanks = 0;
    int col = 0;
    while (blanks < len && _blank((unsigned char)s[blanks])) {
        col += s[blanks] == '\t' ? c->tab - col % c->tab : 1;
        blanks++;
    }
    *cut = blanks;
    if (!out && !len)
        return NULL;
    const int to = out ? (col - c->tab > 0 ? col - c->tab : 0) : col + c->tab;
    char* sp = malloc((size_t)to + 1);
    if (!sp)
        return NULL;
    memset(sp, ' ', (size_t)to);
    sp[to] = '\0';
    *n = (size_t)to;
    return sp;
}

// `op` over each caret's range: delete, change, yank, the shifts and the case operators.
//
// Each caret's operator starts at the earlier of the caret and where the motion goes -- for the
// operator doubled, the last line's first character not a blank, or for a yank its column. That
// is where vim leaves the cursor after a yank or a change of case, and where its undo and redo put
// it back: so the carets are moved there before the edit, which the text's undo step keeps.
static void _operate(Ctx* c, uint32_t op, uint32_t prefix, uint32_t key, uint32_t arg, int count0) {
    UIEditorVim* v = c->v;
    Carets k = _carets(c->el);
    Range* r = calloc(k.count ? k.count : 1, sizeof(Range));
    Change* ch = calloc(k.count ? k.count : 1, sizeof(Change));
    size_t* start = calloc(k.count ? k.count : 1, sizeof(size_t));
    if (!k.sel || !r || !ch || !start) {
        _free_carets(&k);
        free(r);
        free(ch);
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
        if (doubled) {
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
        if (r[i].ok && m.to < head)
            start[i] = m.to;
        any = any || r[i].ok;
    }
    if (!any) {
        _free_carets(&k);
        free(r);
        free(ch);
        free(start);
        return;
    }
    // The register takes the primary's text.
    const Range* pr = &r[k.primary];
    if (pr->ok && (op == 'd' || op == 'c' || op == 'y')) {
        size_t n = 0;
        char* text = pr->lines ? _lines_text(c, pr->first, pr->last, &n)
                               : _range_text(c, pr->lo, pr->hi, &n);
        if (text)
            _put_register(c, text, n, pr->lines);
        free(text);
    }
    _place(c, start, op == 'y' ? k.want : NULL, k.count, k.primary, true);
    if (op == 'y') {
        _free_carets(&k);
        free(r);
        free(ch);
        free(start);
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
                char* sp = _shifted(c, line, op == '<', &len, &cut);
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
    _free_carets(&k);
    free(r);
    free(ch);
    free(start);
    if (op == 'c') {
        _enter_insert(c, 'c', 1);
        return;
    }
    _group(c, false);
}

// ---- Edits that are not operators

// p and P: the register after or before the cursor, `count` times; whole lines below or above the
// cursor's line.
static void _put(Ctx* c, bool after, int count) {
    UIEditorVim* v = c->v;
    Engine* engine = ui_editor_engine(c->el);
    const char* clip = engine ? engine_clipboard_get(engine) : v->yank;
    if (!clip || !*clip)
        return;
    const size_t clip_len = strlen(clip);
    // Whole lines when vim put them there so, or, from elsewhere, when they end in a line break.
    const bool lines =
        v->yank && !strcmp(v->yank, clip) ? v->yank_lines : clip[clip_len - 1] == '\n';
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

// J and gJ: `count` lines joined, at least two, J putting one space where the lines meet unless
// the next begins with ')' or either side is empty, and dropping the next line's indent. A count
// past the last line joins what there is; from the last line itself, three or more join nothing
// and only take the cursor to the line's start, where two fail.
static bool _join(Ctx* c, int count0, bool spaces) {
    const int n = count0 > 2 ? count0 : 2;
    Carets k = _carets(c->el);
    Change* ch = calloc(k.count ? k.count : 1, sizeof(Change));
    bool any = false;
    for (size_t i = 0; k.sel && ch && i < k.count; i++) {
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
// the line's indent; c has put the carets already.
static void _enter_insert(Ctx* c, uint32_t cmd, int count) {
    UIEditorVim* v = c->v;
    if (cmd != 'c')
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
        else
            _move(c, 0, p, k, count0);
        if (op && op != 'y')
            _keep_change(v, count0);
        _reset(v);
        return true;
    }
    if (p == 'r') {
        if (k < K_ESC || k == K_CR) {
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
            if (_join(c, count0, false))
                _keep_change(v, count0);
            _reset(v);
            return true;
        }
        if (_is_motion('g', k)) {
            if (op) {
                _operate(c, op, 'g', k, 0, count0);
                if (op != 'y')
                    _keep_change(v, count0);
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
            if (_join(c, count0, true))
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

// What follows an operator: the operator again for lines, a motion, or Esc.
static bool _after_op(Ctx* c, uint32_t k) {
    UIEditorVim* v = c->v;
    const int count0 = _count0(v);
    const uint32_t op = v->op;
    if (k == K_ESC) {
        _reset(v);
        return true;
    }
    if (k == 'g' || k == 'f' || k == 'F' || k == 't' || k == 'T') {
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
    return _command(c, k);
}

// `.`: the last change made again, with the count given now if there is one.
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
    v->replaying++;
    v->count = count;
    for (size_t i = 0; i < n; i++)
        _feed(v, c->el, keys[i]);
    v->replaying--;
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

// A click in normal mode leaves the cursor on a character.
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
    if (!selected && k.sel && at)
        _place(&c, at, NULL, k.count, k.primary, true);
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
