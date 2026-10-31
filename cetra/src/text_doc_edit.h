#ifndef _TEXT_DOC_EDIT_H_
#define _TEXT_DOC_EDIT_H_

/*
 * The editor's half of a TextDoc (spec 14.4): how an editor showing the text registers to have its
 * carets moved, makes an action at every caret, and reads the lines' colours. Private: an app
 * names it only as a debug harness.
 */

#include "text_doc.h"

#define TEXT_ENDING_NONE 0
#define TEXT_ENDING_LF   1
#define TEXT_ENDING_CRLF 2

struct TextLine {
    char* bytes; // the content, without the line ending: into the doc's block while
                 // capacity is 0, the line's own once it has been edited
    uint32_t len;
    uint32_t capacity;
    size_t start;     // good while the line is below the doc's starts_good
    uint64_t span_at; // where its spans start in the doc's span ring, while spans_known
    uint32_t span_count;
    uint32_t state_in;
    uint32_t state_out;
    uint8_t ending;   // TEXT_ENDING_*
    bool state_known; // state_out is what the line leaves from state_in
    bool spans_known; // its spans from state_in were written at span_at, unless since written over
};

// One change, as an editor showing the text is told of it.
typedef struct TextViewChange {
    size_t first_line;    // lines_removed lines from here became lines_inserted new ones: 1 and 1
    size_t lines_removed; // for an edit inside one line
    size_t lines_inserted;
    size_t start, removed, inserted; // in bytes
    bool actor; // the editor whose action it is, which keeps a caret at an insertion after the new
                // bytes where any other keeps it before them
    bool held;  // the editor's carets are not moved through it: they are set when the action ends
} TextViewChange;

// An editor showing the text.
struct TextDocView {
    void* view;
    struct UIElement* element;
    void (*changed)(void* view, const TextViewChange* change);
    size_t (*selections)(void* view, const TextSelection** out);
    void (*set_selections)(void* view, const TextSelection* selections, size_t count);
    // The text is being freed: the editor lets go of it, and shows nothing from then on.
    void (*gone)(void* view);
};

// One action a step holds: its changes, owning their bytes, from the highest start down.
typedef struct TextUndoPart {
    TextDocChange* changes;
    size_t change_count;
} TextUndoPart;

// What one undo takes back: the actions of one step, in the order they were made -- one action, a
// run of typing joined into words, or every action a group held.
struct TextUndoStep {
    TextUndoPart* parts;
    size_t part_count;
    TextSelection* before;
    size_t before_count;
    TextSelection* after;
    size_t after_count;
    void* view; // the editor that made it; NULL for the host's, or once that editor is gone
};

// One replacement an action makes, in the text as the action finds it.
typedef struct TextEdit {
    size_t start, end;
    const char* text;
    size_t len;
} TextEdit;

void text_doc_add_view(TextDoc* doc, const TextDocView* view);
void text_doc_remove_view(TextDoc* doc, const void* view);

// One action by the editor `view`: the edits, in order and not overlapping, applied from the
// highest start down, every other view's carets moved through each, then kept for undo and told to
// on_action. `after` is where the action leaves the editor's selections, which the editor sets
// itself, its carets not moved through the edits; NULL moves them through each as an actor's.
// `typed` is typing, which joins the step before it into words. Refused by name for edits out of
// order, overlapping or reaching past the text.
bool text_doc_apply(TextDoc* doc, const TextEdit* edits, size_t count, void* view,
                    const TextSelection* after, size_t after_count, bool typed);
// The next typing starts a step of its own: a caret moved.
void text_doc_seal_undo(TextDoc* doc);
// Opened, the next action starts a step and every action after it joins that step until it is
// closed, whatever it is and whoever made it -- a vim mode's insert, from i to Esc. The step starts
// at the editor `view`'s selections as the group opened, not its first action's: an insert at
// several carets is taken back to the one cursor it began from. A host's edit inside it is a step
// of its own, and the group's next action starts another.
void text_doc_group(TextDoc* doc, bool open, const void* view);
// A step of undo that changes nothing, which an undo takes back doing nothing but putting the
// editor `view`'s selections back: vim's operator over nothing. Inside a group's step, nothing.
void text_doc_empty_step(TextDoc* doc, void* view);
// Undo or redo from an editor, which is given the step's selections whoever made it.
bool text_doc_undo_in(TextDoc* doc, void* view);
bool text_doc_redo_in(TextDoc* doc, void* view);

// `offset` moved through one change: kept before an insertion at it, or put after it with `after`.
size_t text_doc_map_offset(size_t offset, size_t start, size_t removed, size_t inserted,
                           bool after);
// A selection moved through one change as `change` says a view's are.
void text_doc_map_selection(TextSelection* s, const TextViewChange* change);
// The byte at `offset`; 0 past the text.
char text_doc_byte(TextDoc* doc, size_t offset);

// A line's colour: false while there is no highlighter, or a stateful one has not reached the line.
// The spans stay good until another line is coloured.
bool text_doc_line_spans(TextDoc* doc, size_t line, const TextSpan** spans, size_t* count);
// A stateful highlighter's walk toward `up_to`, at most `max_lines` lines coloured: whether lines
// below `up_to` are still to be reached.
bool text_doc_highlight_step(TextDoc* doc, size_t up_to, size_t max_lines);

const TextRange* text_doc_ranges(const TextDoc* doc, int layer, size_t* count);

#endif // _TEXT_DOC_EDIT_H_
