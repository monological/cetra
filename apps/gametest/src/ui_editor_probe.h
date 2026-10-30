#ifndef _UI_EDITOR_PROBE_H_
#define _UI_EDITOR_PROBE_H_

#include "cetra/game/game.h"
#include "cetra/ui.h"

// The --ui-probe cases spec 14.4 added -- the code editor's text, and the editor showing it -- in
// the probe's `ui <case> <label> <key> <numbers>` lines. 0 when the case ran, 1 when it failed to,
// -1 when it is not one of these.
int run_ui_editor_probe(Game* game, UISystem* ui, Font* font, const char* which);

#endif // _UI_EDITOR_PROBE_H_
