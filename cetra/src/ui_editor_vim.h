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
// The keys of a command not yet whole -- "2d", or "d/fo" while a line is read -- for a status line;
// "" for none.
const char* ui_editor_vim_pending(const UIEditorVim* vim);

// Asked when a key wants a line typed: '/' or '?' for a search, ':' for a command. The host shows
// its line, and passes what was typed on to ui_editor_vim_search or ui_editor_vim_command -- NULL
// when it is let go. With none set, vim reads the line from the keys, shown as pending.
typedef void (*UIEditorVimPromptFn)(UIElement* el, char kind, void* user);
void ui_editor_vim_set_prompt(UIEditorVim* vim, UIEditorVimPromptFn fn, void* user);
// A search as / makes it, or ? with `backward`: kept for n and N, its matches lit, and each cursor
// moved to its next match -- or an operator waiting for it taken there. Plain text, ignoring case
// unless the query has a capital; "" searches again. False with no match.
bool ui_editor_vim_search(UIEditorVim* vim, UIElement* el, const char* query, bool backward);
// A search line's matches lit in `el` as it is typed, as ui_editor_vim_search will take them; ""
// or NULL lights the last search's again.
void ui_editor_vim_light(UIEditorVim* vim, UIElement* el, const char* query);
// A command line, without its ':', that vim knows: a line number, $, and noh. False for any other,
// which is the host's.
bool ui_editor_vim_command(UIEditorVim* vim, UIElement* el, const char* line);
// Undo and redo -- u and Ctrl+R -- through a host's own from then on; NULL for the text's own.
void ui_editor_vim_set_undo(UIEditorVim* vim, UIEditorCommandFn undo, UIEditorCommandFn redo,
                            void* user);

// What a key asks of a host that knows the language (spec 14.9), as vim asks its tagfunc and
// keywordprg: where the name at the cursor is declared, in its block (gd) or the whole text (gD),
// where it is defined (Ctrl+]), and what it is (K).
typedef enum UIEditorVimLookup {
    UI_VIM_LOOKUP_DECLARATION,
    UI_VIM_LOOKUP_GLOBAL,
    UI_VIM_LOOKUP_TAG,
    UI_VIM_LOOKUP_KEYWORD,
} UIEditorVimLookup;
// The host reads the name from the editor -- its caret, or in visual mode its one-line selection,
// still in place -- and goes where it likes, another text included. False when it found nothing:
// gd and gD are then vim's own search, and Ctrl+] and K do nothing. Asked in normal mode with no
// operator waiting, and Ctrl+] and K in visual mode too; gd and gD after an operator or in visual
// mode are vim's motions.
typedef bool (*UIEditorVimLookupFn)(UIElement* el, UIEditorVimLookup kind, void* user);
// NULL for none.
void ui_editor_vim_set_lookup(UIEditorVim* vim, UIEditorVimLookupFn fn, void* user);

// The column `line`'s indent should reach (lines from 0), or -1 to keep the indent it has: vim's
// indentexpr with indentkeys=o,O. Asked by =, a line at a time after the lines above it are
// re-indented, a line of blanks emptied without asking; and for a line just made by o, O or
// Enter, or by a change of whole lines (cc, S), in place of the indent copied from above. The
// indent is built of spaces. With none, = does nothing and a new line copies its indent.
typedef int (*UIEditorVimIndentFn)(UIElement* el, size_t line, void* user);
void ui_editor_vim_set_indent(UIEditorVim* vim, UIEditorVimIndentFn fn, void* user);
// Lines `first` to `last` laid out again: their new text in *out, malloc'd and freed by the vim
// mode, *len bytes from the first line's start to the last's end without its break, the text's own
// breaks between lines. Vim's formatexpr, asked by gq; the cursor goes to the last new line's
// first character that is not a blank. False leaves the lines; with none, gq does nothing.
typedef bool (*UIEditorVimFormatFn)(UIElement* el, size_t first, size_t last, char** out,
                                    size_t* len, void* user);
void ui_editor_vim_set_format(UIEditorVim* vim, UIEditorVimFormatFn fn, void* user);

// A key, or a sequence of them, and what it does, for a host's help.
typedef struct UIEditorVimKey {
    const char* keys;
    const char* description;
} UIEditorVimKey;
const UIEditorVimKey* ui_editor_vim_keys(size_t* count);

#endif // _UI_EDITOR_VIM_H_
