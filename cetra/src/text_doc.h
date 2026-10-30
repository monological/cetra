#ifndef _TEXT_DOC_H_
#define _TEXT_DOC_H_

/*
 * A text being edited (spec 14.4): what a code editor shows, kept apart from the editors that show
 * it, as lite keeps its Doc and vis its Text. Two editors of one text each keep their own carets
 * and scroll; an edit made through either, or by the host, moves both. No GL and no UI.
 *
 * Positions are byte offsets from the text's start, counted as a file's bytes are: a line break is
 * its one or two bytes, and a byte-order mark is three. That is what a host that edits files by
 * byte ranges -- pcb's splices -- already holds.
 *
 * The text is an array of lines, lite's shape. Each line points into the bytes the text was made
 * from until it is first edited, so a 10 MB file opens with one copy and one scan for line ends,
 * and typing on one line touches that line alone. A line keeps its terminator apart from its
 * content -- LF, CRLF, or none on the last line -- so the bytes come back exactly. A CR just
 * before an LF is the line's ending; any other CR is content, as pcb reads a file.
 *
 * Every change is one replacement: the bytes [start, end) become new bytes, and `version` counts
 * the changes. An offset or a range held at an older version is brought to this one through the
 * last TEXT_DOC_HISTORY changes, which is how a host's colours from a parse a few keystrokes
 * behind still land on the text they meant.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct UIElement;
typedef struct TextDoc TextDoc;

// How far back an offset or a range can be brought forward, in changes.
#define TEXT_DOC_HISTORY 4096
// The host's range layers: errors, a selection shared with other views, and two more of its own.
#define TEXT_DOC_LAYERS 4

typedef enum TextNewline {
    TEXT_NEWLINE_LF,   // "\n"
    TEXT_NEWLINE_CRLF, // "\r\n"
} TextNewline;

// A selection, or a caret where the two are equal.
typedef struct TextSelection {
    size_t anchor; // the end that stays when the selection is extended
    size_t head;   // the caret
} TextSelection;

// One replacement, as an action reports it.
typedef struct TextDocChange {
    size_t start; // in the text as it stood when the action began
    const char* removed;
    size_t removed_len;
    const char* inserted;
    size_t inserted_len;
} TextDocChange;

// One thing a person did: a keystroke at every caret, a paste, an undo. Its changes never overlap,
// run from the highest start to the lowest, and each start counts in the text as the action found
// it, so a host can apply them in order to the text it last saw.
typedef struct TextDocAction {
    const TextDocChange* changes;
    size_t change_count;
    const TextSelection* before; // the editor's selections as the action began
    size_t before_count;
    const TextSelection* after; // and as it left them
    size_t after_count;
    const struct UIElement* editor; // the editor it was made in; NULL for an undo called directly
    uint64_t version;               // the text's version as the action began
} TextDocAction;

typedef void (*TextDocActionFn)(TextDoc* doc, const TextDocAction* action, void* user);

// A run of one style in a line, in bytes of the line's content.
typedef struct TextSpan {
    uint32_t start;
    uint32_t len;
    uint16_t style; // an index into the editor's style table
} TextSpan;

// Colours one line: its content, without its line ending, and the state the line before left.
// Writes up to `max` spans in order into `spans`, sets *state to what this line leaves for the
// next, and returns how many spans the line has -- more than `max` asks again with room for them.
typedef size_t (*TextHighlightFn)(const char* line, size_t len, uint32_t* state, TextSpan* spans,
                                  size_t max, void* user);

// A range the host marks: an error, a part selected elsewhere.
typedef struct TextRange {
    size_t start, end;
    uint16_t style; // an index into the editor's style table
} TextRange;

typedef struct TextLine TextLine;
typedef struct TextDocView TextDocView;
typedef struct TextUndoStep TextUndoStep;

struct TextDoc {
    /* ENGINE-OWNED */
    TextLine* lines;
    size_t line_count;
    size_t line_capacity;
    size_t length;      // bytes, line endings included
    size_t starts_good; // lines below this hold a good start offset
    uint64_t version;   // changes made so far
    char* block;        // the bytes the text was made from, which unedited lines point into
    struct {            // the last TEXT_DOC_HISTORY changes, by version
        size_t start, removed, inserted;
    } history[TEXT_DOC_HISTORY];
    TextDocView* views; // the editors showing the text
    size_t view_count, view_capacity;
    TextUndoStep* undo; // past steps, oldest first, then the steps undone, newest first
    size_t undo_count;  // steps that can be undone
    size_t redo_count;  // steps after them that can be redone
    size_t undo_capacity;
    bool undo_sealed; // the next typing starts a step of its own
    TextHighlightFn highlight;
    void* highlight_user;
    bool highlight_stateful;
    size_t highlight_known; // stateful: lines below this have the state they leave worked out
    size_t span_lines;      // lines holding their spans
    TextSpan* span_scratch; // what a highlighter writes into
    size_t span_scratch_capacity;
    TextRange* ranges[TEXT_DOC_LAYERS];
    size_t range_count[TEXT_DOC_LAYERS];
    size_t range_capacity[TEXT_DOC_LAYERS];
    /* BY FUNCTION: text_doc_set_on_action */
    TextDocActionFn on_action;
    void* on_action_user;
    /* SETTINGS */
    TextNewline newline; // what typed and pasted line breaks are written as; found at load
    int tab_width;       // columns between tab stops
    bool keep_undo;      // false: no undo is kept, for a host that keeps its own
};

// A text of `len` bytes, copied. Its line ending is CRLF when its first line ends so, else LF.
// NULL with `len` 0 is an empty text.
TextDoc* create_text_doc(const char* bytes, size_t len);
// An editor still showing the text lets go of it, and shows nothing from then on.
void free_text_doc(TextDoc* doc);

size_t text_doc_length(const TextDoc* doc);
size_t text_doc_line_count(const TextDoc* doc);
// A line's content, without its line ending, and its length. Valid until the next change.
const char* text_doc_line(const TextDoc* doc, size_t line, size_t* len);
// The offset of a line's first byte; past the last line, the text's length.
size_t text_doc_line_start(TextDoc* doc, size_t line);
// The line holding `offset`, its line ending counting as its own; the text's length is on the
// last line.
size_t text_doc_line_of(TextDoc* doc, size_t offset);
// The bytes [start, end) into `out`, which holds end - start; their count, 0 when the range is
// past the text.
size_t text_doc_copy(TextDoc* doc, size_t start, size_t end, char* out);
// The whole text, NUL-terminated, malloc'd for the caller to free; *len its length. NULL out of
// memory.
char* text_doc_text(const TextDoc* doc, size_t* len);

// The host's edit: the bytes [start, end) become `text`, written as given. Refused by name past
// the text's end, with start after end, or with either end inside a UTF-8 sequence. Every editor's
// carets and scroll move with it; it is not reported to on_action, which is for what a person
// does.
bool text_doc_replace(TextDoc* doc, size_t start, size_t end, const char* text, size_t len);
// The host's whole new text, landing as one replacement of what changed between the two: a file
// reloaded after an undo of one word moves the carets as that word's change.
bool text_doc_set_text(TextDoc* doc, const char* text, size_t len);
// `offset` held at `version` brought to the text as it is now: an offset before a change stays,
// one after moves by its length, and one inside the bytes it replaced goes to the end of the new
// ones. Refused by name, *offset untouched, for a version older than TEXT_DOC_HISTORY changes or
// newer than the text's.
bool text_doc_map(const TextDoc* doc, uint64_t version, size_t* offset);

// One layer of the host's ranges, replaced whole: given as the text stood at `version`, and moved
// with every change since and after. A range is sorted in by its start; one emptied by a change
// is dropped, and one an edit cuts into is cut short so the new bytes are outside it.
bool text_doc_set_ranges(TextDoc* doc, int layer, uint64_t version, const TextRange* ranges,
                         size_t count);
// How every line is coloured; NULL for none. `stateful` is for a language whose lines can begin
// inside something an earlier line opened -- C's /* */ -- so each line's colour waits on the
// line above; with it false every line starts from state 0 and is coloured on its own.
void text_doc_set_highlighter(TextDoc* doc, TextHighlightFn fn, void* user, bool stateful);
// Told of every action once it is made, from any editor showing the text.
void text_doc_set_on_action(TextDoc* doc, TextDocActionFn fn, void* user);

// The last step taken back, or put back again: the editor that made it is given its selections
// from before, or after. False with nothing to undo or redo, and while keep_undo is false.
bool text_doc_undo(TextDoc* doc);
bool text_doc_redo(TextDoc* doc);

#endif // _TEXT_DOC_H_
