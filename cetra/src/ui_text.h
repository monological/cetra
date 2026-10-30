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
// The widest of the text's '\n'-separated lines: what a text that does not wrap is measured by,
// each line placed by its own width rather than by the longest one's.
float ui_text_widest_line(const UIText* t);
// Whether `runs` fit the first `length` bytes of `text`: in order, apart, inside it, and starting
// and ending on a character. Refused by name otherwise, `who` naming the call.
bool ui_text_runs_check(const char* who, const char* text, size_t length, const UITextRun* runs,
                        size_t count);
// The line height of the text's face at its size, times `line_spacing` (0 is 1).
float ui_text_line_height(const UIText* t, float line_spacing);
// Draws bytes [from, to) with its pen starting at `x` and its baseline at `baseline`.
void ui_text_draw_span(UIDrawList* dl, const UIText* t, size_t from, size_t to, float x,
                       float baseline);
// The baseline of a line whose top is `top`: an ascent of the text's face below it.
float ui_text_baseline(const UIText* t, float top);

/*
 * A text's lines -- an element's, or a multi-line field's rows: wrapped at a width, or at its '\n's
 * alone -- and its measures. Kept while what they were taken for is unchanged: a long text is a
 * walk of all of it, and the layout runs several times a frame. Zeroed, they hold nothing.
 */
typedef struct UIMeasure {
    float value;
    bool taken; // `value` holds the measure; zeroed, it does not
} UIMeasure;

typedef struct UILines {
    size_t* starts; // the byte each line starts at
    size_t count, capacity;
    float width; // what they were broken at; INFINITY for '\n's alone
    // The face, size and tracking the lines and the measures are for. A text in another drops both.
    const Font* font;
    float size, tracking;
    UIMeasure measures[2]; // the widest line, and the longest word: by `wrap`
} UILines;

// Sets `*width` to the text's widest line, or its longest word when `wrap`: what an element is
// measured by across. Kept until the text changes or is in another face, size or tracking; true
// when it walked the text for it.
bool ui_lines_measure(UILines* lines, const UIText* t, bool wrap, float* width);
/*
 * Breaks `t` into `lines` at `width` -- INFINITY breaks only at its '\n's -- unless they hold the
 * lines of the same width, face, size and tracking already. True when it broke them; false when it
 * kept them, or ran out of memory, which leaves `count` 0.
 */
bool ui_lines_break(UILines* lines, const UIText* t, float width);
// Breaks `t` again from line `line` on, keeping the lines before it: for a text changed at or after
// where that line starts, as an append is, or anywhere from line 0. The measures go with the old
// text. Every line is broken when they are not this width's and style's. True once broken; false
// out of memory, which leaves `count` 0.
bool ui_lines_break_from(UILines* lines, const UIText* t, float width, size_t line);
// The end of line `i`, its '\n' and any spaces it wrapped at left off.
size_t ui_lines_end(const UILines* lines, const UIText* t, size_t i);
// Draws each of `lines` a line height below the last, from `r`'s top, placed by `align` on its own
// width.
void ui_draw_text_lines(UIDrawList* dl, UIRect r, const UIText* t, const UILines* lines,
                        float line_spacing, UIAlign align);

#endif // _UI_TEXT_H_
