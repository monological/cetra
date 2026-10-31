#ifndef _UI_EDITOR_H_
#define _UI_EDITOR_H_

/*
 * The code editor's calls (spec 14.4). The element itself is ui_editor, in ui.h, beside the other
 * kinds; the text it shows is a TextDoc (text_doc.h).
 *
 * Lines wrap at the editor's width, after the last space that fits or at the edge for a word longer
 * than a row, and a wrapped line's later rows are indented as far as the line itself is. The scroll
 * is held to a line and a row of it, so an edit above the view, or a line measured where it had
 * been guessed, never moves what is on screen.
 *
 * Selections are a list, sorted and merged where they meet, the last one set the primary: the one a
 * scroll follows and a single click keeps.
 */

#include <stdbool.h>
#include <stddef.h>

#include "text_doc.h"
#include "ui.h"

// The editor's selections, sorted by where they start, and how many; *primary their primary. Valid
// until the text or the selections change.
size_t ui_editor_selections(const UIElement* el, const TextSelection** out, size_t* primary);
// Replaces the selections; the last given is the primary. Each is held to the text and to its
// characters' edges; ones that meet are merged.
void ui_editor_set_selections(UIElement* el, const TextSelection* selections, size_t count);
// Scrolls the least that shows the row holding `offset`.
void ui_editor_reveal(UIElement* el, size_t offset);

/*
 * What an edit is made of, after vis: a MOTION says where a caret goes, a TEXT OBJECT what range
 * lies round it, and an OPERATOR what is done to a range. Each runs once per caret. A mode turns
 * keys into them: the standard mode by a table, another by reading a sequence such as "d2w" as
 * delete, twice, by word. Nothing in the editor changes for that.
 */
typedef enum UIEditorMotion {
    UI_MOTION_NONE = 0, // no motion: the operator takes each caret's selection
    UI_MOTION_CHAR_LEFT,
    UI_MOTION_CHAR_RIGHT,
    UI_MOTION_WORD_LEFT,
    UI_MOTION_WORD_RIGHT, // the platform's: to a word's end on macOS, the next word's start
                          // elsewhere
    UI_MOTION_WORD_END,
    UI_MOTION_ROW_UP, // a row on screen, keeping the column a caret wants
    UI_MOTION_ROW_DOWN,
    UI_MOTION_ROW_START,
    UI_MOTION_ROW_END,
    UI_MOTION_LINE_FIRST, // the line's first character that is not a blank
    UI_MOTION_LINE_UP,    // a line of the text, wrapped or not, keeping the column
    UI_MOTION_LINE_DOWN,
    UI_MOTION_PAGE_UP, // the rows the editor shows, less one
    UI_MOTION_PAGE_DOWN,
    UI_MOTION_DOC_START,
    UI_MOTION_DOC_END,
    UI_MOTION_BRACKET,   // the bracket matching the one at the caret, or just before it
    UI_MOTION_FIND_NEXT, // the next match of ui_editor_find's query, round the end to the start
    UI_MOTION_FIND_PREV,
    UI_MOTION_COUNT
} UIEditorMotion;

typedef enum UIEditorObject {
    UI_OBJECT_WORD,
    UI_OBJECT_LINE,           // with its line ending
    UI_OBJECT_BRACKETS,       // the innermost (), [] or {} round the caret, brackets and all
    UI_OBJECT_BRACKETS_INNER, // what is between them
    UI_OBJECT_STRING,         // the "quoted" string round the caret, on its line
    UI_OBJECT_STRING_INNER,
    UI_OBJECT_ALL,
    UI_OBJECT_COUNT
} UIEditorObject;

typedef enum UIEditorOperator {
    UI_OP_MOVE,   // the caret to where the motion goes, the selection let go
    UI_OP_EXTEND, // the caret to where the motion goes, the selection's anchor kept
    UI_OP_SELECT, // the selection made what the motion covers
    UI_OP_DELETE, // what the motion covers -- or the selection, where there is one -- taken out
    UI_OP_CUT,
    UI_OP_COPY,
    UI_OP_PASTE,   // the clipboard over each selection, a line to each caret when it has as many
    UI_OP_INDENT,  // the lines the selection touches, a tab stop in
    UI_OP_OUTDENT, // and back
    UI_OP_COUNT
} UIEditorOperator;

// Where `motion` takes a caret from `offset`, `count` times.
size_t ui_editor_motion(UIElement* el, UIEditorMotion motion, size_t offset, int count);
// The range `object` covers round `offset`; false when there is none, as for brackets with none
// round the caret.
bool ui_editor_object(UIElement* el, UIEditorObject object, size_t offset, TextSelection* out);
// `op` at every caret, over what `motion` covers from it `count` times, as one action.
void ui_editor_apply(UIElement* el, UIEditorOperator op, UIEditorMotion motion, int count);
// `text` typed at every caret over its selection, as one action that joins the typing before it
// into words.
void ui_editor_type(UIElement* el, const char* text);

// A thing a key or a host's command table can do. The editor lists its own; a host makes its own,
// with its own `user`, and binds them in a mode of its own.
typedef bool (*UIEditorCommandFn)(UIElement* el, void* user);
typedef struct UIEditorCommand {
    const char* name;        // "move-word-left": what a command table and a palette list it as
    const char* description; // "Move each caret back a word"
    UIEditorCommandFn run;   // false when it did nothing, so its key falls to the screen
    void* user;
} UIEditorCommand;

typedef struct UIEditorBinding {
    int key;  // GLFW_KEY_*
    int mods; // exactly these of GLFW_MOD_SHIFT, _CONTROL, _ALT and _SUPER held
    const UIEditorCommand* command;
} UIEditorBinding;

// A mode's own reader of keys, which sees each one first; true takes it.
typedef bool (*UIEditorKeyFn)(UIElement* el, const InputKeyEvent* key, void* user);
// Told after a press or a drag changed the selections.
typedef void (*UIEditorPointerFn)(UIElement* el, void* user);

// How a selection's band is drawn: what the mode acting on it takes it to cover.
typedef enum UIEditorSelectionShape {
    UI_SELECTION_SPAN,      // from its start up to its end
    UI_SELECTION_INCLUSIVE, // the character at its far end too
    UI_SELECTION_LINES,     // every line it touches, whole
} UIEditorSelectionShape;

/*
 * A key goes to the mode's reader, then its table, then its parent's, and so on up: a binding
 * whose command does nothing hands the key on too. A key a mode uses takes the character its press
 * makes with it, so a binding on Space types no space. A LAYER only adds keys over its parent: what
 * the editor reads of a mode besides its keys -- `name` to `pointer` -- is the first mode up the
 * chain that is not a layer.
 */
typedef struct UIEditorMode {
    const char* name;                  // what a status line shows: "insert", "normal"
    const struct UIEditorMode* parent; // where a key it does not take goes
    const UIEditorBinding* bindings;
    size_t binding_count;
    UIEditorKeyFn read_key; // NULL for none
    void* user;
    bool layer;       // keys only, the rest its parent's: a host's commands over a mode
    bool types;       // a typed character is put in at every caret
    bool block_caret; // the caret is drawn a cell wide, over the character at the head
    UIEditorSelectionShape selection;
    UIEditorPointerFn pointer; // NULL for none; given `user`
} UIEditorMode;

// The editor's own commands, every one the standard mode binds and more.
const UIEditorCommand* ui_editor_commands(size_t* count);
// Runs one of them by name; false for a name it does not have, or a command that did nothing.
bool ui_editor_run(UIElement* el, const char* name);
// The platform's keys: Cmd on macOS and Ctrl elsewhere for the shortcuts, Option or Ctrl for a
// word, and typing that types.
const UIEditorMode* ui_editor_standard_mode(void);
// Borrowed, and outliving the editor; NULL is the standard mode.
void ui_editor_set_mode(UIElement* el, const UIEditorMode* mode);
// The mode set; a layer's look is its base's.
const UIEditorMode* ui_editor_mode(const UIElement* el);

// What find looks for: matches are lit in view, and UI_MOTION_FIND_NEXT and _PREV go to them.
// Copied; NULL or "" for none. A match lies on one line.
enum {
    UI_EDITOR_FIND_CASE = 1, // match case; ASCII letters match either otherwise
    UI_EDITOR_FIND_WORD = 2, // only whole words
};
void ui_editor_find(UIElement* el, const char* query, unsigned flags);

#endif // _UI_EDITOR_H_
