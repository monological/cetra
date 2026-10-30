#ifndef _UI_BENCH_H_
#define _UI_BENCH_H_

#include "cetra/game/game.h"
#include "cetra/ui.h"

// --ui-bench (spec 14.3): the UI's timings that decide items 10 to 14 of spec 14.3, printed as
// `ui-bench <what> <how> <numbers>` lines. Meaningful on a release build only. 0 when it ran.
int run_ui_bench(Game* game, UISystem* ui, const Font* font);

#endif // _UI_BENCH_H_
