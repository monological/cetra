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

// Two presses this close in time and place are one double press.
#define UI_TYPING_PRESS_SECONDS 0.4
#define UI_TYPING_PRESS_POINTS  4.0f
// The caret: lit, then dark, ImGui's rhythm; and its width in points.
#define UI_TYPING_CARET_LIT_SECONDS  0.8
#define UI_TYPING_CARET_DARK_SECONDS 0.4
#define UI_TYPING_CARET_WIDTH        1.5f

// What a byte starts, for word moves: 0 blank, 1 punctuation, 2 a word's (letters, digits, `_`
// and anything past ASCII, so a word in another script is a word).
int ui_typing_class(char byte);
// Back past any blanks, then back over the run of one class before them.
size_t ui_typing_word_left(const char* s, size_t len, size_t at);
// Forward to the end of the next word on macOS, as its fields move; to the start of the one after
// elsewhere, as Windows' do.
size_t ui_typing_word_right(const char* s, size_t len, size_t at);
// The word round `at`, of the class of the character at it -- or before it, at a word's end.
void ui_typing_word_round(const char* s, size_t len, size_t at, size_t* from, size_t* to);
// The clipboard as an editor takes it, into `out` (which holds `len` bytes): control characters
// out, a CR or a CRLF one '\n', and '\n' kept with `lines`, else a space; a tab kept with `tabs`,
// else a space. The bytes written.
size_t ui_typing_clean(const char* in, size_t len, char* out, bool lines, bool tabs);
// Whether a press at (x, y) at `now` is another of the run that last pressed at (px, py) at
// `then`.
bool ui_typing_again(double now, double then, float x, float y, float px, float py);
// Whether the caret is lit `now`, its last move at `since`: always without `blink`. *wake is the
// seconds until it next changes, INFINITY without `blink`.
bool ui_typing_caret_lit(double now, double since, bool blink, double* wake);

#endif // _UI_TYPING_H_
