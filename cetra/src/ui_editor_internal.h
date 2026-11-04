#ifndef _UI_EDITOR_INTERNAL_H_
#define _UI_EDITOR_INTERNAL_H_

/*
 * What a mode built inside cetra reaches of the editor besides its public calls (spec 14.5).
 * Private.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "text_doc_edit.h"
#include "ui_editor.h"

struct Engine;

// Which editor this is, and showing which text: new when an editor is made and each time it is
// shown another text, never reused, so a mode remembering the last editor it saw knows another at
// the same address. 0 for anything else.
uint64_t ui_editor_serial(const UIElement* el);
// The engine the editor was last laid out under, for the clipboard; NULL before.
struct Engine* ui_editor_engine(const UIElement* el);
// Edits from the carets as one action, those that overlap merged, leaving the selections `after`
// with `primary` the primary, or with `after` NULL each caret after its own edit's text.
void ui_editor_edit(UIElement* el, TextEdit* edits, size_t n, const TextSelection* after,
                    size_t after_count, size_t primary);
// The carets set, with each one's wanted column (NULL for none) and the primary; the view follows
// it, and the next typing starts a step of its own.
void ui_editor_set_carets(UIElement* el, const TextSelection* sels, const int* wants, size_t count,
                          size_t primary);
// Each caret's wanted column for moving by line, in the order of the selections: -1 for none, and
// INT_MAX for its line's end. Valid until the selections change; NULL for no editor.
const int* ui_editor_wants(UIElement* el);
// The rows the editor shows whole, at least one.
int ui_editor_rows_shown(const UIElement* el);
// The offset starting the row `down` rows below the view's top, held to the text's last row.
size_t ui_editor_row_at(UIElement* el, int down);
// The view scrolled by rows, the carets left where they are.
void ui_editor_scroll_rows(UIElement* el, float rows);
// Whether a closed fold holds `line`, and its first and last lines (either may be NULL).
bool ui_editor_closed_fold(const UIElement* el, size_t line, size_t* first, size_t* last);

#endif // _UI_EDITOR_INTERNAL_H_
