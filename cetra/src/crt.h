#ifndef _CRT_H_
#define _CRT_H_

#include <GL/glew.h>
#include <stdbool.h>

struct PostFX;

/*
 * A consumer television (spec 13.28), owned by PostFX: the finished picture -- the app's overlay
 * included, the debug GUI not -- shown as a living-room set would show a console's. After
 * Lottes' CRTS (2018). The picture is first resampled to a signal a few hundred lines tall, then
 * drawn line by line as soft beams under a slot mask, on a slightly curved tube, with composite
 * colour bleed. The window it draws to is the only place the picture is dithered.
 */

typedef struct Crt Crt;

// Builds its programs. NULL on failure, reported with its reason, which leaves the frame
// without a CRT.
Crt* create_crt(void);
void free_crt(Crt* crt);

// The framebuffer the frame is drawn into before the CRT shows it, `width` by `height`: 0 when
// it could not be made, which is reported and which a later frame of the same size would not
// change.
GLuint crt_picture_fbo(Crt* crt, int width, int height);

// Show the picture on the CRT into `target_fbo`, `fx`'s crt_* settings and dither applied.
void crt_present(Crt* crt, const struct PostFX* fx, GLuint target_fbo, int width, int height);

#endif
