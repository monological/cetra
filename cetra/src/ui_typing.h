#ifndef _UI_TYPING_H_
#define _UI_TYPING_H_

/*
 * The platform's conventions for editing text (spec 14.2, shared since 14.4): how far a word move
 * goes, what a double press takes, what a paste keeps, and the caret's blink. The text field and
 * the code editor both edit by these, so the two cannot drift apart. Private.
 *
 * On a byte string `len` long, stepping whole UTF-8 characters.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Two presses this close in time and place are one double press.
#define UI_TYPING_PRESS_SECONDS 0.4
#define UI_TYPING_PRESS_POINTS  4.0f
// The caret: lit, then dark, ImGui's rhythm; and its width in points.
#define UI_TYPING_CARET_LIT_SECONDS  0.8
#define UI_TYPING_CARET_DARK_SECONDS 0.4
#define UI_TYPING_CARET_WIDTH        1.5f

// A run of presses: the last one's time and place, and how many in the run so far.
typedef struct UITypingPresses {
    double time;
    float x, y;
    int count;
} UITypingPresses;

// A caret's blink: when it last restarted, and whether it has moved since, which restarts it.
typedef struct UITypingCaret {
    double since;
    bool moved;
} UITypingCaret;

// The character at s[*i] of a run `len` long, never reading past it -- an editor's line is not
// NUL-terminated -- and *i moved past it.
uint32_t ui_typing_next(const char* s, size_t len, size_t* i);
// What a byte starts, for word moves: 0 blank, 1 punctuation, 2 a word's (letters, digits, `_`
// and anything past ASCII, so a word in another script is a word).
int ui_typing_class(char byte);
// Back past any blanks, then back over the run of one class before them.
size_t ui_typing_word_left(const char* s, size_t len, size_t at);
// Forward past any blanks, then over the run of one class after them: to a word's end.
size_t ui_typing_word_end(const char* s, size_t len, size_t at);
// Forward to the end of the next word on macOS, as its fields move; to the start of the one after
// elsewhere, as Windows' do.
size_t ui_typing_word_right(const char* s, size_t len, size_t at);
// The word round `at`, of the class of the character at it -- or before it, at a word's end.
void ui_typing_word_round(const char* s, size_t len, size_t at, size_t* from, size_t* to);
// The clipboard as an editor takes it, into `out` (which holds `len` bytes): control characters
// out, a CR or a CRLF one '\n', and '\n' kept with `lines`, else a space; a tab kept with `tabs`,
// else a space. The bytes written.
size_t ui_typing_clean(const char* in, size_t len, char* out, bool lines, bool tabs);
// A press at (x, y) at `now`, counted into the run when it is close enough to the last in time and
// place: how many the run now holds, from 1, back to 1 after `cycle` when it is not 0.
int ui_typing_press(UITypingPresses* p, double now, float x, float y, int cycle);
// Whether the caret is lit `now`, its blink restarted now if it moved: always without `blink`.
// *wake is the seconds until it next changes, INFINITY without `blink`.
bool ui_typing_caret_lit(UITypingCaret* c, double now, bool blink, double* wake);

#endif // _UI_TYPING_H_
