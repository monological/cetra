#ifndef _GUI_H_
#define _GUI_H_

#include <stdbool.h>

struct Engine;

/*
 * The Dear ImGui panels. Split from engine.c because it is presentation over
 * the engine's public API and touches no engine internals -- everything below
 * reads Engine/Scene/PostFX fields and calls exported functions.
 *
 * The ImGui CONTEXT is not owned here. Its lifecycle (create, new-frame,
 * shutdown) and the GLFW event forwarding that feeds it are interleaved with
 * the engine's own input handling, so they stay in engine.c; this file only
 * describes what to draw once a frame is open.
 */

// Dark panel with an indigo accent. Applied once at context creation.
void gui_apply_style(void);

// Draw the enabled panels into the open ImGui frame and flush them. No-ops
// unless the engine latched a GUI frame this tick.
void gui_render_frame(struct Engine* engine);

#endif // _GUI_H_
