#ifndef _UI_TEXT_H_
#define _UI_TEXT_H_

/*
 * The UI's text walked once for every use (spec 14.2): measured, broken into lines, drawn, and a
 * caret placed in it, all by the same advances, so a wrapped line, a centred string, a caret and
 * the glyph beside it cannot disagree. Private to the UI's own files.
 *
 * A text is a UTF-8 string with the element's style and its styled runs. Each glyph is measured in
 * its run's face at the element's size; between two glyphs the pen moves by the face's kerning pair
 * plus the style's tracking, and a change of face starts the pairing afresh. A glyph the atlas
 * lacks advances by the face's own width for it, and is drawn as a box.
 */

#include "ui.h"

typedef struct UIText {
    const char* text;      // UTF-8, at least `length` bytes
    size_t length;         // bytes
    const UITextRun* runs; // in order and apart; NULL for none
    size_t run_count;
    Font* font;         // the element's face: its line height and baseline are the text's
    float size;         // points
    float tracking;     // points between two glyphs
    const float* color; // the element's text colour
} UIText;

// The text of `text` in `style`, the draw list's face and size standing in where the style names
// none. `length` is bytes; the runs are borrowed for as long as the text is used.
UIText ui_text_make(const UIDrawList* dl, const char* text, size_t length, const UIStyle* style,
                    const UITextRun* runs, size_t run_count);

// The pen at byte `at` of the line starting at byte `from`: where the glyph at `at` is drawn, its
// pair with the glyph before it included. `at` is a codepoint boundary in [from, length].
float ui_text_pen(const UIText* t, size_t from, size_t at);
// The width of bytes [from, to): the pen past the last glyph's advance, with no pair after it.
float ui_text_span_width(const UIText* t, size_t from, size_t to);
// Where the line starting at byte `from` ends, within `max_width` points: at a '\n', past the last
// space it reaches (the space kept on the line), inside a word longer than the line, or at the end.
// Always past `from` unless `from` is the end or a '\n', so a caller looping on it advances.
size_t ui_text_break(const UIText* t, size_t from, float max_width);
// The widest word, so a wrapping label is never measured narrower than one.
float ui_text_longest_word(const UIText* t);
// The line height of the text's face at its size, times `line_spacing` (0 is 1).
float ui_text_line_height(const UIText* t, float line_spacing);
// Draws bytes [from, to) with its pen starting at `x` and its baseline at `baseline`.
void ui_text_draw_span(UIDrawList* dl, const UIText* t, size_t from, size_t to, float x,
                       float baseline);
// The baseline of a line whose top is `top`: an ascent of the text's face below it.
float ui_text_baseline(const UIText* t, float top);

/*
 * A label's lines, as ui.c breaks them: wrapped at its width, or at its '\n's alone when it does
 * not wrap and only carries runs. Kept while what they were broken for is unchanged.
 */
typedef struct UILines {
    size_t* starts; // the byte each line starts at
    size_t count, capacity;
    float width; // what they were broken at; INFINITY for '\n's alone
    const Font* font;
    float size, tracking;
} UILines;

// The end of line `i`, its '\n' and any spaces it wrapped at left off.
size_t ui_lines_end(const UILines* lines, const UIText* t, size_t i);
// Draws each of `lines` a line height below the last, from `r`'s top, placed by `align` on its own
// width.
void ui_draw_text_lines(UIDrawList* dl, UIRect r, const UIText* t, const UILines* lines,
                        float line_spacing, UIAlign align);

#endif // _UI_TEXT_H_
