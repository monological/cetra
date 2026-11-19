#include "picture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ext/log.h"

void free_picture(Picture* picture) {
    if (!picture)
        return;
    free(picture->rgba);
    free(picture);
}

bool picture_write_ppm(const Picture* picture, const char* path) {
    FILE* f = picture && path ? fopen(path, "wb") : NULL;
    uint8_t* row = f ? malloc((size_t)picture->width * 3) : NULL;
    if (!row) {
        log_error("picture_write_ppm: cannot write %s", path ? path : "(no path)");
        if (f)
            fclose(f);
        return false;
    }
    fprintf(f, "P6\n%d %d\n255\n", picture->width, picture->height);
    for (int y = 0; y < picture->height; y++) {
        const uint8_t* from = picture->rgba + (size_t)y * (size_t)picture->width * 4;
        for (int x = 0; x < picture->width; x++)
            memcpy(row + (size_t)x * 3, from + (size_t)x * 4, 3);
        fwrite(row, 1, (size_t)picture->width * 3, f);
    }
    free(row);
    fclose(f);
    return true;
}
