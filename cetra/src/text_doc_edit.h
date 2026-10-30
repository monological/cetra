#ifndef _TEXT_DOC_EDIT_H_
#define _TEXT_DOC_EDIT_H_

/*
 * The editor's half of a TextDoc (spec 14.4): how an editor showing the text registers to have its
 * carets moved, makes an action at every caret, and reads the lines' colours. Private: an app
 * reaches it only as a debug harness, as the probe does.
 */

#include "text_doc.h"

#define TEXT_ENDING_NONE 0
#define TEXT_ENDING_LF   1
#define TEXT_ENDING_CRLF 2

// What a line keeps of its colour.
#define TEXT_COLOURED_STATE 1 // state_out is what the line leaves from state_in
#define TEXT_COLOURED_SPANS 2 // spans are the line's from state_in

struct TextLine {
    char* bytes; // the content, without the line ending: into the doc's block while
                 // capacity is 0, the line's own once it has been edited
    uint32_t len;
    uint32_t capacity;
    size_t start;    // good while the line is below the doc's starts_good
    TextSpan* spans; // while TEXT_COLOURED_SPANS
    uint32_t span_count;
    uint32_t state_in;
    uint32_t state_out;
    uint8_t ending;   // TEXT_ENDING_*
    uint8_t coloured; // TEXT_COLOURED_*
};

// An editor showing the text. `map` moves its carets and scroll through one change; the editor
// whose action it is gets `actor` true, and keeps a caret at an insertion after the new bytes,
// where any other keeps it before them.
struct TextDocView {
    void* view;
    struct UIElement* element;
    void (*map)(void* view, size_t start, size_t removed, size_t inserted, bool actor);
    size_t (*selections)(void* view, const TextSelection** out);
    void (*set_selections)(void* view, const TextSelection* selections, size_t count);
};

struct TextUndoStep {
    TextDocChange* changes; // owning their bytes
    size_t change_count;
    TextSelection* before;
    size_t before_count;
    TextSelection* after;
    size_t after_count;
    void* view; // the editor that made it; NULL for the host's, or once that editor is gone
    bool typed; // typing, which the next typing at the same carets joins
};

// One replacement an action makes, in the text as the action finds it.
typedef struct TextEdit {
    size_t start, end;
    const char* text;
    size_t len;
} TextEdit;

void text_doc_add_view(TextDoc* doc, const TextDocView* view);
void text_doc_remove_view(TextDoc* doc, const void* view);

// One action by the editor `view`: the edits, which must not overlap, applied from the highest
// start down, every view's carets moved through each, then kept for undo and told to on_action.
// `typed` is typing, which joins the step before it into words. Refused by name for edits that
// overlap or reach past the text.
bool text_doc_apply(TextDoc* doc, TextEdit* edits, size_t count, void* view, bool typed);
// The next typing starts a step of its own: a caret moved.
void text_doc_seal_undo(TextDoc* doc);
// Undo or redo from an editor, which is given the step's selections whoever made it.
bool text_doc_undo_in(TextDoc* doc, void* view);
bool text_doc_redo_in(TextDoc* doc, void* view);

// `offset` moved through one change: kept before an insertion at it, or put after it with `after`.
size_t text_doc_map_offset(size_t offset, size_t start, size_t removed, size_t inserted,
                           bool after);

// A line's colour: false while there is no highlighter, or a stateful one has not reached the line.
bool text_doc_line_spans(TextDoc* doc, size_t line, const TextSpan** spans, size_t* count);
// A stateful highlighter's walk toward `up_to`, at most `max_lines` lines coloured: whether lines
// below `up_to` are still to be reached.
bool text_doc_highlight_step(TextDoc* doc, size_t up_to, size_t max_lines);

const TextRange* text_doc_ranges(const TextDoc* doc, int layer, size_t* count);

#endif // _TEXT_DOC_EDIT_H_
