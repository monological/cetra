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

#endif // _UI_EDITOR_H_
