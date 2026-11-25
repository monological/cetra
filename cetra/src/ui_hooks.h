#ifndef _UI_HOOKS_H_
#define _UI_HOOKS_H_

/*
 * What a test reads of the UI system that nothing else should (spec 14.6), as ui_editor_hooks.h is
 * for the editor. Private.
 */

#include "ui.h"

// What the system holds between passes: the element a press captured, the element whose bar the
// pointer holds, and the focus as the last input pass left it.
void ui_system_held(const UISystem* ui, const UIElement** pressed, const UIElement** bar,
                    const UIElement** seen);

// The clock the next pass reads -- a caret's blink, a bar's fade -- set as a window's frame sets
// it; and the seconds after it the last build asked to be drawn again, INFINITY for never.
void ui_system_set_now(UISystem* ui, double now);
double ui_system_wake(const UISystem* ui);

#endif // _UI_HOOKS_H_
