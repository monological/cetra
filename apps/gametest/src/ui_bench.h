#ifndef _UI_BENCH_H_
#define _UI_BENCH_H_

#include "cetra/ui.h"

// --ui-bench: the UI's timings, printed as `ui-bench <what> <how> <numbers>` lines. Meaningful on
// a release build only. 0 when it ran.
int run_ui_bench(UISystem* ui, const Font* font);

#endif // _UI_BENCH_H_
