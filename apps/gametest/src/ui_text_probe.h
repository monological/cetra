#ifndef _UI_TEXT_PROBE_H_
#define _UI_TEXT_PROBE_H_

#include "cetra/game/game.h"
#include "cetra/ui.h"

// The --ui-probe cases spec 14.2 added -- wrapped and styled text, the scrolling pane, the text
// field, the matcher and virtual rows -- in the probe's `ui <case> <label> <key> <numbers>` lines.
// 0 when the case ran, 1 when it failed to, -1 when it is not one of these.
int run_ui_text_probe(Game* game, UISystem* ui, Font* font, const char* which);
// A probe line holding a rect: `ui <which> <label> rect x y w h`.
void ui_probe_rect(const char* which, const char* label, UIRect r);

#endif // _UI_TEXT_PROBE_H_
