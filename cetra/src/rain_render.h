#ifndef _RAIN_RENDER_H_
#define _RAIN_RENDER_H_

#include <GL/glew.h>
#include <cglm/cglm.h>
#include <stdbool.h>

struct Engine;
struct Scene;
struct PostFXLateDraw;

/*
 * The GL half of the rain (spec 13.9): the streaks, drawn from postfx's late draw after the
 * temporal seam. rain.h owns what the rain IS and stays free of GL; this owns how it is drawn.
 * Engine-owned, created the first frame a scene rains.
 */
typedef struct RainRenderer {
    GLuint vao; // empty: a core profile needs one bound, and the streaks read no attributes
    // The camera a frame ago, for the velocity a streak is drawn relative to.
    vec3 prev_eye;
    bool prev_valid;
    bool program_failed; // latched, so a program that will not build logs once
    // The frame as it stood before the rain drew, mipped: what a drop refracts. Its own
    // copy because the streaks draw onto the canvas they would otherwise be reading.
    GLuint behind_fbo, behind_tex;
    int behind_w, behind_h;
} RainRenderer;

RainRenderer* create_rain_renderer(void);
void free_rain_renderer(RainRenderer* renderer);

// Draw the scene's rain onto the bound canvas. Nothing when the scene has no rain falling.
void rain_render_streaks(RainRenderer* renderer, struct Engine* engine, struct Scene* scene,
                         const struct PostFXLateDraw* late);

#endif // _RAIN_RENDER_H_
