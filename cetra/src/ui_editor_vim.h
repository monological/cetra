#ifndef _UI_EDITOR_VIM_H_
#define _UI_EDITOR_VIM_H_

/*
 * A vim mode for the code editor (spec 14.5), beside the standard mode: normal, insert and visual
 * modes, counts, operators over motions and text objects, `.`, search and a command line, read as
 * vim reads them. One object serves every editor a host gives it, since vim's mode, a half-typed
 * command, its register and its last search belong to the person typing and not to a text; each
 * editor keeps its own selections.
 *
 * A host stacks its own keys on it in a layer (ui_editor.h) -- a command of its that does nothing
 * hands the key back to vim -- and may take undo, the line typed after / ? and :, and every command
 * line vim does not know. The register is the system clipboard.
 */

#include <stdbool.h>
#include <stddef.h>

#include "ui_editor.h"

typedef struct UIEditorVim UIEditorVim;

typedef enum UIEditorVimState {
    UI_VIM_NORMAL,
    UI_VIM_INSERT,
    UI_VIM_VISUAL,
    UI_VIM_VISUAL_LINE,
    UI_VIM_VISUAL_BLOCK,
} UIEditorVimState;

UIEditorVim* create_ui_editor_vim(void);
// Every editor given its mode must have let go of it first.
void free_ui_editor_vim(UIEditorVim* vim);
// The mode an editor is given, or a host's layer is put over. Its look follows the state: a block
// caret but in insert mode, and visual selections drawn as vim takes them.
const UIEditorMode* ui_editor_vim_mode(const UIEditorVim* vim);
UIEditorVimState ui_editor_vim_state(const UIEditorVim* vim);
// The keys of a command not yet whole -- "2d" -- for a status line; "" for none.
const char* ui_editor_vim_pending(const UIEditorVim* vim);

// Asked when a key wants a line typed: '/' or '?' for a search, ':' for a command. The host shows
// its line, and passes what was typed on to ui_editor_vim_search or ui_editor_vim_command -- or
// nothing, when it is let go.
typedef void (*UIEditorVimPromptFn)(UIElement* el, char kind, void* user);
void ui_editor_vim_set_prompt(UIEditorVim* vim, UIEditorVimPromptFn fn, void* user);
// A search as / makes it, or ? with `backward`: kept for n and N, and the cursor moved to the next
// match. Plain text, ignoring case unless the query has a capital. False with no match.
bool ui_editor_vim_search(UIEditorVim* vim, UIElement* el, const char* query, bool backward);
// A command line, without its ':', that vim knows: a line number, and "noh". False for any other,
// which is the host's.
bool ui_editor_vim_command(UIEditorVim* vim, UIElement* el, const char* line);
// Undo and redo -- u and Ctrl+R -- through a host's own from then on; NULL for the text's own.
void ui_editor_vim_set_undo(UIEditorVim* vim, UIEditorCommandFn undo, UIEditorCommandFn redo,
                            void* user);

// A key, or a sequence of them, and what it does, for a host's help.
typedef struct UIEditorVimKey {
    const char* keys;
    const char* description;
} UIEditorVimKey;
const UIEditorVimKey* ui_editor_vim_keys(size_t* count);

#endif // _UI_EDITOR_VIM_H_
