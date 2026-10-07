#ifndef _CRT_H_
#define _CRT_H_

#include <GL/glew.h>
#include <stdbool.h>

/*
 * A consumer television (spec 13.28): a finished picture shown as a living-room set would show a
 * console's. After Lottes' CRTS (2018). The picture is resampled to a signal a few hundred lines
 * tall, then drawn line by line as soft beams under a slot mask, on a slightly curved tube, with
 * composite colour bleed, and dithered as it is written to the window.
 */

typedef struct Crt Crt;

// What the television shows. Each setting 0..1 but the line count.
typedef struct CrtLook {
    float lines;           // the signal's lines, top to bottom
    float scanlines;       // 0 = lines fused, 1 = thin beams with dark gaps between
    float mask;            // the slot mask's depth
    float curvature;       // the tube's bow, 0 = flat
    float bleed;           // composite colour bleed
    bool dither;           // dither the window's 8-bit write
    float dither_strength; // its peak amplitude in LSB
} CrtLook;

// Builds its programs. NULL on failure, reported with its reason.
Crt* create_crt(void);
void free_crt(Crt* crt);

// The framebuffer a picture `width` by `height` is drawn into before it is shown: 0 when it could
// not be made, which is reported and which a later frame of the same size would not change.
GLuint crt_picture_fbo(Crt* crt, int width, int height);

// Show the picture drawn into crt_picture_fbo's framebuffer in the window, at its size.
void crt_present(Crt* crt, GLuint quad_vao, const CrtLook* look);

#endif
