#include "pictures.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cetra/util.h"

static struct {
    const char* window_path;
    float window_rect[4];
    const char* canvas_path;
    float canvas_ppp;
    const char* view3d_path;
    const char* offscreen_path;
    SceneView* offscreen;
    bool said_window, said_canvas, said_view3d, said_offscreen;
} g;

bool pictures_parse(int argc, char** argv, int* i) {
    if (*i + 1 >= argc)
        return false;
    const char* flag = argv[*i];
    char* value = argv[*i + 1];
    if (strcmp(flag, "--read-window") == 0) {
        float* r = g.window_rect;
        int at = 0;
        if (sscanf(value, "%f,%f,%f,%f,%n", &r[0], &r[1], &r[2], &r[3], &at) == 4 && at > 0)
            g.window_path = value + at;
        else
            fprintf(stderr, "canvas: --read-window takes X,Y,W,H,PATH\n");
    } else if (strcmp(flag, "--read-canvas") == 0) {
        int at = 0;
        if (sscanf(value, "%f,%n", &g.canvas_ppp, &at) == 1 && at > 0)
            g.canvas_path = value + at;
        else
            fprintf(stderr, "canvas: --read-canvas takes PPP,PATH\n");
    } else if (strcmp(flag, "--read-view3d") == 0) {
        g.view3d_path = value;
    } else if (strcmp(flag, "--offscreen-view") == 0) {
        g.offscreen_path = value;
    } else {
        return false;
    }
    ++*i;
    return true;
}

bool pictures_start(Engine* engine, const SceneView* view3d) {
    if (!g.offscreen_path)
        return true;
    if (!view3d) {
        fprintf(stderr, "canvas: --offscreen-view needs --view3d\n");
        return false;
    }
    g.offscreen = create_scene_view(engine, &(SceneViewDesc){.offscreen = true});
    if (!g.offscreen || !engine_add_view(engine, g.offscreen)) {
        fprintf(stderr, "canvas: the offscreen view could not be made\n");
        free_scene_view(g.offscreen);
        g.offscreen = NULL;
        return false;
    }
    return true;
}

void pictures_lay_out(const SceneView* view3d) {
    if (!g.offscreen || !view3d)
        return;
    // The pane's size, rounded as the pane's is at the window's left edge: a whole pixel either
    // way, so its pixels are the pane's; and were it ever copied into the window it would land on
    // the window's left half, where a gate can see it.
    g.offscreen->x = 0.0f;
    g.offscreen->y = view3d->y;
    g.offscreen->width = view3d->width;
    g.offscreen->height = view3d->height;
    g.offscreen->camera = view3d->camera;
    g.offscreen->settings = view3d->settings;
}

static void read_window_done(Engine* engine, Picture* picture, void* user) {
    (void)engine;
    (void)user;
    if (picture)
        picture_write_ppm(picture, g.window_path);
    free_picture(picture);
}

// Where a rectangle of points stands in the framebuffer, as the engine rounds it, said once.
static void say_rect(const Engine* engine, const char* what, float x, float y, float w, float h,
                     bool* said) {
    if (*said)
        return;
    float sx, sy;
    engine_fb_per_point(engine, &sx, &sy);
    int px[4];
    rect_points_to_pixels(x, y, w, h, sx, sy, px);
    printf("canvas picture %s at %d %d size %d %d\n", what, px[0], px[1], px[2], px[3]);
    fflush(stdout);
    *said = true;
}

void pictures_read(Engine* engine, Canvas* canvas, const CanvasView* view,
                   const SceneView* view3d) {
    if (g.window_path) {
        // Said clipped to the window, as it is read.
        const float* r = g.window_rect;
        const float x0 = fmaxf(r[0], 0.0f), x1 = fminf(r[0] + r[2], (float)engine->win_width);
        const float y0 = fmaxf(r[1], 0.0f), y1 = fminf(r[1] + r[3], (float)engine->win_height);
        say_rect(engine, "window", x0, y0, x1 - x0, y1 - y0, &g.said_window);
        engine_read_window(engine, r[0], r[1], r[2], r[3], read_window_done, NULL);
    }
    if (g.canvas_path && canvas && view) {
        Picture* picture = canvas_read_picture(canvas, view, g.canvas_ppp);
        if (picture) {
            say_rect(engine, "canvas", view->x, view->y, view->width, view->height, &g.said_canvas);
            picture_write_ppm(picture, g.canvas_path);
        }
        free_picture(picture);
    }
    if (g.view3d_path && view3d) {
        Picture* picture = scene_view_read_picture(view3d);
        if (picture) {
            say_rect(engine, "view3d", view3d->x, view3d->y, view3d->width, view3d->height,
                     &g.said_view3d);
            picture_write_ppm(picture, g.view3d_path);
        }
        free_picture(picture);
    }
    if (g.offscreen) {
        Picture* picture = scene_view_read_picture(g.offscreen);
        if (picture) {
            if (!g.said_offscreen) {
                printf("canvas picture offscreen size %d %d\n", picture->width, picture->height);
                fflush(stdout);
            }
            g.said_offscreen = true;
            picture_write_ppm(picture, g.offscreen_path);
        }
        free_picture(picture);
    }
}
