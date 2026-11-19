#ifndef _PICTURE_H_
#define _PICTURE_H_

#include <stdbool.h>
#include <stdint.h>

// A picture read back from the GPU (spec 14.41): rows from the top, four bytes a pixel, RGBA8 as it
// is shown (display-encoded, alpha 255). Made by the reads that return one (gl_read_picture,
// canvas_read_picture, scene_view_read_picture, engine_read_window's callback), freed by
// free_picture.
typedef struct Picture {
    int width, height;
    uint8_t* rgba;
} Picture;

void free_picture(Picture* picture);
// The picture as a binary PPM, its alpha left out. False, logged, when the file cannot be written.
bool picture_write_ppm(const Picture* picture, const char* path);

#endif // _PICTURE_H_
